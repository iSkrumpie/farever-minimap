// Local-Hero tracker driven by the hl_alloc_obj watcher.
//
// Each ent.Hero allocation pushes the raw pointer into a pending
// list. hero_state_tick() (called per frame from the Present hook,
// after damage_tick) iterates pending candidates, waits until the
// Haxe constructor has populated Hero.ownerPlayer, then verifies:
//   - ownerPlayer is a userland pointer
//   - ownerPlayer.hero == this Hero  (bidirectional integrity)
//   - ownerPlayer.isMe == 1          (we are the local player)
// First candidate that passes is locked. Subsequent allocs that
// arrive before the lock get retried up to a hard cap; the lock is
// re-validated every 64 ticks so dungeon transitions don't freeze
// us on a stale pointer.

#include "hero_state.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace farever {
namespace {

// ent.Hero offsets — same as the structural scanner used.
constexpr std::size_t OFF_HERO_OWNERPLAYER  = 16;
constexpr std::size_t OFF_HERO_POSX         = 144;   // f64
constexpr std::size_t OFF_HERO_POSY         = 152;
constexpr std::size_t OFF_HERO_POSZ         = 160;
constexpr std::size_t OFF_HERO_ROTZ         = 168;

constexpr std::size_t OFF_PLAYER_HERO       = 272;   // Player.hero (back-ref)
constexpr std::size_t OFF_PLAYER_ISME       = 280;   // u8

// How many ticks (~ frames) to retry a pending Hero before dropping
// it. 600 = ~10 s at 60 Hz, which is generous for constructor lag
// from a network sync burst.
constexpr int kMaxPendingRetries = 600;

struct Pending {
    std::uintptr_t hero_ptr;
    int            retries;
};

std::atomic<bool>           g_active{false};
std::atomic<std::uintptr_t> g_locked_hero{0};
std::atomic<std::uint64_t>  g_ticks{0};

std::mutex                  g_pending_mu;
std::vector<Pending>        g_pending;

HeroSnapshot                g_snapshot{};

// Plausible world-coordinate ranges (W1 spans ~6 tiles × 256 m).
// Used to reject sync-proxy / template Heroes whose isMe is set but
// whose position is still (0,0) — the structural scan in minimap-dll
// applied the same filter, and skipping it here was the cause of the
// "arrow doesn't track the player" bug.
constexpr double RX_LO = -10000.0, RX_HI = 10000.0;
constexpr double RY_LO = -10000.0, RY_HI = 10000.0;
constexpr double RZ_LO =   -500.0, RZ_HI =  1500.0;

bool position_is_plausible(std::uintptr_t hero_ptr) {
    double pos[4];
    if (!mem_read_bytes(hero_ptr + OFF_HERO_POSX, pos, sizeof(pos)))
        return false;
    double x = pos[0], y = pos[1], z = pos[2];
    if (std::isnan(x) || std::isinf(x)) return false;
    if (std::isnan(y) || std::isinf(y)) return false;
    if (std::isnan(z) || std::isinf(z)) return false;
    if (x < RX_LO || x > RX_HI) return false;
    if (y < RY_LO || y > RY_HI) return false;
    if (z < RZ_LO || z > RZ_HI) return false;
    // (0, 0) is the canonical "uninitialised" pose — reject it
    // explicitly so we don't lock on a template Hero whose isMe was
    // already set by the network deserialiser.
    if (std::fabs(x) < 0.01 && std::fabs(y) < 0.01) return false;
    return true;
}

// Returns true if this Hero pointer is currently the local player's:
//   - ownerPlayer.isMe == 1
//   - bidirectional Player.hero == this Hero
//   - position is a plausible in-world coordinate (not the world
//     origin and not the bogus values a not-yet-streamed Hero shows)
bool is_local_hero(std::uintptr_t hero_ptr) {
    std::uint64_t owner_u64 = 0;
    if (!mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &owner_u64)) return false;
    auto owner = static_cast<std::uintptr_t>(owner_u64);
    if (!mem_is_userland(owner)) return false;

    std::uint64_t player_hero = 0;
    if (!mem_read_u64(owner + OFF_PLAYER_HERO, &player_hero)) return false;
    if (player_hero != hero_ptr) return false;

    std::uint8_t is_me = 0;
    if (!mem_read_u8(owner + OFF_PLAYER_ISME, &is_me)) return false;
    if (is_me != 1) return false;

    return position_is_plausible(hero_ptr);
}

void on_hero_alloc(std::uintptr_t obj) {
    if (!obj) return;
    // Short-circuit: if we already have a lock, ignore further allocs.
    // (Re-validation in the tick handles dungeon transitions.)
    if (g_locked_hero.load(std::memory_order_acquire) != 0) return;
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.push_back({obj, 0});
}

void publish() {
    HeroSnapshot s{};
    std::uintptr_t a = g_locked_hero.load(std::memory_order_acquire);
    if (a == 0) {
        g_snapshot = s;
        return;
    }
    double pos[4]{0,0,0,0};
    if (!mem_read_bytes(a + OFF_HERO_POSX, pos, sizeof(pos))) {
        // Pointer went stale (memory unmapped). Drop the lock and let
        // the next alloc seed a fresh candidate.
        g_locked_hero.store(0);
        g_snapshot = s;
        logf("hero_state: read failed, dropping lock");
        return;
    }
    s.locked = true;
    s.x      = pos[0];
    s.y      = pos[1];
    s.z      = pos[2];
    s.rot_z  = pos[3];
    g_snapshot = s;
}

}  // namespace

void hero_state_start() {
    if (g_active.exchange(true)) return;
    g_locked_hero.store(0);
    g_ticks.store(0);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.clear();
    }
    g_snapshot = HeroSnapshot{};
    hl_hook_register(L"ent.Hero", on_hero_alloc);
    logf("hero_state: watcher registered (render-thread tick)");
}

void hero_state_stop() {
    g_active.store(false);
}

void hero_state_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;

    std::uintptr_t locked = g_locked_hero.load(std::memory_order_acquire);

    if (locked == 0) {
        // Try to settle a pending candidate.
        std::vector<Pending> work;
        {
            std::lock_guard<std::mutex> lk(g_pending_mu);
            work.swap(g_pending);
        }
        std::vector<Pending> retry;
        retry.reserve(work.size());
        std::uintptr_t found = 0;
        for (Pending p : work) {
            if (found) {
                // Already locked one this tick; drop the rest — they
                // can't be the local player.
                continue;
            }
            if (is_local_hero(p.hero_ptr)) {
                found = p.hero_ptr;
                continue;
            }
            if (++p.retries < kMaxPendingRetries) {
                retry.push_back(p);
            }
        }
        if (found) {
            g_locked_hero.store(found, std::memory_order_release);
            logf("hero_state: locked Hero @ 0x%llx (tick %llu)",
                 static_cast<unsigned long long>(found),
                 static_cast<unsigned long long>(n));
        } else if (!retry.empty()) {
            std::lock_guard<std::mutex> lk(g_pending_mu);
            g_pending.insert(g_pending.end(), retry.begin(), retry.end());
        }
    } else if ((n & 0x3F) == 0) {
        // Periodic re-validation. Dungeon transitions can keep the
        // old Hero memory mapped but flip isMe to 0; this catches it.
        if (!is_local_hero(locked)) {
            g_locked_hero.store(0);
            logf("hero_state: re-validation failed at tick %llu, "
                 "dropping lock",
                 static_cast<unsigned long long>(n));
        }
    }

    publish();

    // Heartbeat: log current position every ~10 s so we can sanity-
    // check that the locked Hero is actually moving with the player.
    if (locked && (n % 600 == 0)) {
        logf("hero_state: pos=(%.1f, %.1f, %.1f) rot=%.3f (tick %llu)",
             g_snapshot.x, g_snapshot.y, g_snapshot.z, g_snapshot.rot_z,
             static_cast<unsigned long long>(n));
    }
}

HeroSnapshot hero_state_read() { return g_snapshot; }

}  // namespace farever

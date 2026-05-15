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

// Returns true if this Hero pointer is currently the local player's
// (ownerPlayer.isMe == 1 and the bidirectional Player.hero matches).
// Used both at lock time and during periodic re-validation.
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
    return is_me == 1;
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
}

HeroSnapshot hero_state_read() { return g_snapshot; }

}  // namespace farever

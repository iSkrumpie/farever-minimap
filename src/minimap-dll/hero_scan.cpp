// In-process Hero scanner. Port of tools/find_me.py to C++ — runs on a
// background thread, walks committed RW regions in our own process,
// matches the ent.Hero signature (4 plausible doubles + 4 userland
// pointers), then validates via Hero.ownerPlayer.isMe and the
// bidirectional Player.hero == Hero check.
//
// Once a Hero is locked, hero_scan_read() is a fast deref — no IPC,
// no Python, no JSON. The whole find_me.py + live_position.json
// pipeline is replaced by this module.

#include "hero_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

namespace farevermod {
namespace {

// --- Memory layout constants (from research/hero-layout.md) ---------

constexpr std::size_t kHeroSize           = 1584;
constexpr std::size_t OFF_HLTYPE          = 0;
constexpr std::size_t OFF_REMOVED         = 8;
constexpr std::size_t OFF_OWNERPLAYER     = 16;
constexpr std::size_t OFF_HOST            = 48;
constexpr std::size_t OFF_POSX            = 144;
constexpr std::size_t OFF_POSY            = 152;
constexpr std::size_t OFF_POSZ            = 160;
constexpr std::size_t OFF_ROTZ            = 168;
constexpr std::size_t OFF_POSITION        = 176;
constexpr std::size_t OFF_HERO_IN_PLAYER  = 272;
constexpr std::size_t OFF_ISME            = 280;

// Plausible world-coordinate ranges (W1 spans ~6 tiles, each 256 m).
constexpr double      RX_LO   = -10000.0, RX_HI   = 10000.0;
constexpr double      RY_LO   = -10000.0, RY_HI   = 10000.0;
constexpr double      RZ_LO   =   -500.0, RZ_HI   =  1500.0;
constexpr double      RROT_LO =     -7.0, RROT_HI =     7.0;

// User-land pointer range: HashLink heap is above 4 GB on Windows x64;
// the upper limit is the architecturally-mandated 47-bit user-mode cap.
constexpr std::uintptr_t USERLAND_LO = 0x0000000100000000ULL;
constexpr std::uintptr_t USERLAND_HI = 0x00007FFFFFFFFFFFULL;

// --- State ---------------------------------------------------------

std::atomic<bool>           g_thread_running{false};
std::atomic<bool>           g_locked{false};
std::atomic<bool>           g_failed{false};
std::atomic<std::uintptr_t> g_hero_addr{0};
std::thread                 g_thread;

// --- SEH helpers ---------------------------------------------------

// Reads inside RW pages should normally succeed, but VirtualQuery
// races with allocations, and the bidirectional pointer chain can
// land on a stale Player whose `hero` field has been freed. Wrap
// every dereference of an untrusted address in SEH.
//
// These helpers live outside any C++ object scope on purpose so MSVC
// accepts the __try without /EHa.
int seh_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uint64_t*>(addr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int seh_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uint8_t*>(addr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int seh_read_4_doubles(std::uintptr_t addr, double out[4]) {
    __try {
        out[0] = *reinterpret_cast<const double*>(addr + 0);
        out[1] = *reinterpret_cast<const double*>(addr + 8);
        out[2] = *reinterpret_cast<const double*>(addr + 16);
        out[3] = *reinterpret_cast<const double*>(addr + 24);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// --- Candidate filter ----------------------------------------------

bool is_userland(std::uintptr_t v) {
    return v >= USERLAND_LO && v <= USERLAND_HI;
}

bool in_range(double v, double lo, double hi) {
    return v >= lo && v <= hi && !std::isnan(v) && !std::isinf(v);
}

// Cheap structural check. Filters are ordered cheapest-first so most
// offsets reject after one read (the position floats are the most
// selective single signal; everything random in memory fails their
// range test). The pointer-validation reads only run on offsets that
// already look position-like.
bool looks_like_hero(std::uintptr_t hero_addr) {
    double pos4[4];
    if (!seh_read_4_doubles(hero_addr + OFF_POSX, pos4)) return false;
    double x = pos4[0], y = pos4[1], z = pos4[2], r = pos4[3];
    if (!in_range(x, RX_LO,   RX_HI))   return false;
    if (!in_range(y, RY_LO,   RY_HI))   return false;
    if (!in_range(z, RZ_LO,   RZ_HI))   return false;
    if (!in_range(r, RROT_LO, RROT_HI)) return false;
    if (std::fabs(x) < 0.01 && std::fabs(y) < 0.01) return false;
    if (std::fabs(x) < 10.0 && std::fabs(y) < 10.0 && std::fabs(z) < 10.0) {
        const double eps = 1e-6;
        if (std::fabs(x - std::round(x)) < eps &&
            std::fabs(y - std::round(y)) < eps &&
            std::fabs(z - std::round(z)) < eps) {
            return false;
        }
    }

    std::uint64_t htype, owner, host, posp;
    std::uint8_t  removed;
    if (!seh_read_u64(hero_addr + OFF_HLTYPE,      &htype))    return false;
    if (!is_userland(htype))   return false;
    if (!seh_read_u64(hero_addr + OFF_OWNERPLAYER, &owner))    return false;
    if (!is_userland(owner))   return false;
    if (!seh_read_u64(hero_addr + OFF_HOST,        &host))     return false;
    if (!is_userland(host))    return false;
    if (!seh_read_u64(hero_addr + OFF_POSITION,    &posp))     return false;
    if (!is_userland(posp))    return false;
    if (!seh_read_u8 (hero_addr + OFF_REMOVED,     &removed))  return false;
    if (removed > 1)           return false;
    return true;
}

// Bidirectional validation: candidate must be the local player.
bool is_local_hero(std::uintptr_t hero_addr) {
    std::uint64_t owner = 0;
    if (!seh_read_u64(hero_addr + OFF_OWNERPLAYER, &owner)) return false;
    if (!is_userland(owner)) return false;
    std::uint64_t player_hero = 0;
    if (!seh_read_u64(owner + OFF_HERO_IN_PLAYER, &player_hero)) return false;
    if (player_hero != hero_addr) return false;
    std::uint8_t is_me = 0;
    if (!seh_read_u8(owner + OFF_ISME, &is_me)) return false;
    return is_me == 1;
}

// --- Scan loop ------------------------------------------------------

void scan_thread() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto cur = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    logf("hero_scan: scanning user-mode address space [%llx, %llx)",
         (unsigned long long)cur, (unsigned long long)end);

    std::size_t regions_scanned = 0;
    std::size_t bytes_scanned   = 0;
    std::uintptr_t found        = 0;

    DWORD t_start = GetTickCount();

    while (cur < end && g_thread_running.load() && found == 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)) == 0)
            break;
        auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        auto size = static_cast<std::size_t>(mbi.RegionSize);
        // Safety: a 0-sized region would deadlock the outer loop.
        if (size == 0) { cur += 4096; continue; }
        cur = base + size;

        if (mbi.State != MEM_COMMIT) continue;
        if (size < (64 * 1024)) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable) == 0) continue;
        // Skip huge mapped sections (>= 1 GB): HashLink GC chunks are at
        // most a few hundred MB; gigantic regions are usually file
        // mappings / textures and bog the scan down.
        if (size > (1ULL << 30)) continue;

        regions_scanned++;
        bytes_scanned += size;
        if ((regions_scanned & 0x1FF) == 0) {
            logf("hero_scan: ... %zu regions, %.0f MB, %lu s",
                 regions_scanned, bytes_scanned / (1024.0 * 1024.0),
                 (GetTickCount() - t_start) / 1000UL);
        }

        // 8-byte stride: HashLink heap allocations are 8-byte aligned
        // and the Hero starts at the alloc boundary.
        std::size_t max_off = (size > kHeroSize) ? (size - kHeroSize) : 0;
        for (std::size_t off = 0; off <= max_off; off += 8) {
            if (!g_thread_running.load()) break;
            std::uintptr_t cand = base + off;
            if (!looks_like_hero(cand)) continue;
            if (!is_local_hero(cand))   continue;
            found = cand;
            break;
        }
    }

    if (found) {
        g_hero_addr.store(found, std::memory_order_release);
        g_locked.store(true);
        logf("hero_scan: locked Hero @ 0x%llx (regions=%zu, %.1f MB scanned)",
             (unsigned long long)found, regions_scanned,
             bytes_scanned / (1024.0 * 1024.0));
    } else {
        g_failed.store(true);
        logf("hero_scan: no local Hero found "
             "(regions=%zu, %.1f MB scanned). User probably hasn't entered "
             "the world yet — scan will not retry automatically.",
             regions_scanned, bytes_scanned / (1024.0 * 1024.0));
    }
    g_thread_running.store(false);
}

}  // namespace

// --- Public API -----------------------------------------------------

void hero_scan_start() {
    if (g_thread_running.exchange(true)) return;
    // Reap any prior worker that already exited so we can reassign
    // g_thread cleanly. join() is a no-op on a finished thread but
    // still drains its state.
    if (g_thread.joinable()) g_thread.join();
    g_locked.store(false);
    g_failed.store(false);
    g_hero_addr.store(0);
    g_thread = std::thread(scan_thread);
}

void hero_scan_stop() {
    g_thread_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

bool hero_scan_locked() { return g_locked.load(std::memory_order_acquire); }
bool hero_scan_failed() { return g_failed.load(std::memory_order_acquire); }

LivePosition hero_scan_read() {
    LivePosition lp{};
    std::uintptr_t a = g_hero_addr.load(std::memory_order_acquire);
    if (a == 0) {
        // No lock — if the previous scan finished (success or failure)
        // but the address is now zero we got a stale-pointer kick. Spin
        // up a new scan transparently so a dungeon transition reconnects.
        if (!g_thread_running.load() && (g_failed.load() || g_locked.load())) {
            hero_scan_start();
        }
        return lp;
    }
    // Validate the lock every ~60 frames (≈1 sec at 60 Hz). After a
    // dungeon transition the game often keeps the OLD Hero memory
    // mapped but flips its `Player.isMe` to 0 — we'd happily keep
    // reading frozen coordinates without this check. is_local_hero
    // re-walks the bidirectional Hero <-> Player chain; if anything no
    // longer matches we drop the lock and kick off a fresh scan.
    static std::atomic<int> validate_tick{0};
    int vt = validate_tick.fetch_add(1, std::memory_order_relaxed);
    if ((vt & 0x3F) == 0) {
        if (!is_local_hero(a)) {
            g_hero_addr.store(0);
            g_locked.store(false);
            logf("hero_scan_read: validation failed (isMe/bidir), "
                 "restarting scan");
            hero_scan_start();
            return lp;
        }
    }

    double pos4[4];
    if (!seh_read_4_doubles(a + OFF_POSX, pos4)) {
        // Stale address — clear, then trigger a fresh scan so dungeon
        // enter/leave reconnects on its own.
        g_hero_addr.store(0);
        g_locked.store(false);
        logf("hero_scan_read: pointer went stale, restarting scan");
        hero_scan_start();
        return lp;
    }
    lp.x     = pos4[0];
    lp.y     = pos4[1];
    lp.z     = pos4[2];
    lp.rot_z = pos4[3];
    lp.valid = true;
    // Diagnostic: log a sample every ~10 s so we can see whether the
    // values are changing as the player walks.
    static std::atomic<int> ticks{0};
    int t = ticks.fetch_add(1, std::memory_order_relaxed);
    if ((t % 600) == 0) {
        logf("hero_scan_read: pos=(%.2f, %.2f, %.2f) rot=%.3f",
             lp.x, lp.y, lp.z, lp.rot_z);
    }
    return lp;
}

}  // namespace farevermod

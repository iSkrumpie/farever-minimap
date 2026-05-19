// Foe (NPC / mob / boss) state tracker. Mirrors the hero_state.cpp
// pattern but maintains a small bounded list instead of a single
// locked entity. Powers the farever.foes.* plugin API.
//
// Lifecycle:
//   1. hl_alloc_obj fires for any ent.Foe allocation -> on_foe_alloc
//      pushes the raw pointer into a pending queue.
//   2. foe_state_tick (render thread, called from Present) drains
//      pending into the active list, validating each (type tag,
//      pointer userland, position not NaN).
//   3. Same tick walks the active list, re-reads pos / hp / level /
//      target / etc into the snapshot, prunes entries whose reads
//      fail (likely freed by GC).
//
// All reads happen on the render thread, same constraint as
// hero_state. See feedback_hashlink_pump_thread.md for why.

#include "foe_state.h"
#include "hero_state.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace farever {
namespace {

// Offsets shared with the Hero layout (Foe extends Unit extends
// GameObject, so positional + combat + attr offsets are identical).
constexpr std::size_t OFF_FOE_POSX         = 144;   // f64
constexpr std::size_t OFF_FOE_POSY         = 152;
constexpr std::size_t OFF_FOE_POSZ         = 160;
constexpr std::size_t OFF_FOE_ROTZ         = 168;
constexpr std::size_t OFF_FOE_ISINCOMBAT   = 672;
constexpr std::size_t OFF_FOE_TARGET       = 640;
constexpr std::size_t OFF_FOE_LEVEL        = 976;
constexpr std::size_t OFF_FOE_ATTR         = 968;

// UnitAttributes block — chase Foe.attr then read 32 contiguous f64s
// covering health (240) through heal (320). Same indices as
// hero_state.cpp's UA_* enum but we only need a subset.
constexpr std::size_t OFF_ATTR_BLOCK_BASE  = 216;   // start at armor
constexpr std::size_t OFF_ATTR_BLOCK_BYTES = 80;    // armor .. shield (8 doubles)

enum FAIdx : int {
    FA_ARMOR = 0,        // 216
    FA_MAGIC_ARMOR,      // 224
    FA_MAGIC_REDUCTION,  // 232
    FA_HEALTH,           // 240
    FA_MAX_HEALTH,       // 248
    FA_HEALTH_REGEN,     // 256
    FA_SHIELD,           // 264
    FA_COUNT,            // upper bound for the buffer
};

std::atomic<bool>            g_active{false};
std::mutex                   g_pending_mu;
std::deque<std::uintptr_t>   g_pending;

struct TrackedFoe {
    std::uintptr_t ptr = 0;
    std::uint64_t  last_valid_tick = 0;
};

constexpr std::size_t kMaxTracked = kMaxFoes;
std::vector<TrackedFoe>      g_tracked;     // active list
std::atomic<FoesSnapshot>    g_snapshot_buf{};  // not used (atomic<struct> can be slow)
FoesSnapshot                 g_snapshot{};
std::atomic<std::uint64_t>   g_ticks{0};
std::uintptr_t               g_foe_type = 0;

void on_foe_alloc(std::uintptr_t obj) {
    if (!g_active.load()) return;
    if (!obj) return;
    constexpr std::size_t kMaxPending = 128;
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.push_back(obj);
    while (g_pending.size() > kMaxPending) g_pending.pop_front();
}

// Read fields for one tracked foe. Returns false if the pointer is
// stale (memory read failed or position is NaN).
bool read_one(std::uintptr_t ptr, FoeEntry& out, double hx, double hy,
              bool hero_locked) {
    if (!mem_is_userland(ptr)) return false;

    // Type-tag check: first u64 of any HL object is the hl_type*.
    if (g_foe_type) {
        std::uint64_t got_type = 0;
        if (!mem_read_u64(ptr, &got_type)) return false;
        if (static_cast<std::uintptr_t>(got_type) != g_foe_type) return false;
    }

    // Position + heading (4 contiguous f64s at 144..168).
    double pos[4]{0, 0, 0, 0};
    if (!mem_read_bytes(ptr + OFF_FOE_POSX, pos, sizeof(pos))) return false;
    for (int i = 0; i < 4; ++i) {
        if (std::isnan(pos[i]) || std::isinf(pos[i])) return false;
    }
    // Reject foes still at the origin (sync proxies / template
    // entities before constructor population).
    if (pos[0] == 0.0 && pos[1] == 0.0 && pos[2] == 0.0) return false;

    out.ptr   = ptr;
    out.x     = pos[0];
    out.y     = pos[1];
    out.z     = pos[2];
    out.rot_z = pos[3];

    std::uint8_t in_combat = 0;
    if (mem_read_u8(ptr + OFF_FOE_ISINCOMBAT, &in_combat))
        out.in_combat = (in_combat != 0);

    std::uint64_t tgt = 0;
    if (mem_read_u64(ptr + OFF_FOE_TARGET, &tgt))
        out.has_target = (tgt != 0);

    std::int32_t lvl = 0;
    if (mem_read_i32(ptr + OFF_FOE_LEVEL, &lvl))
        out.level = (int)lvl;

    // Chase Foe.attr -> UnitAttributes. Foes use plain
    // UnitAttributes (no Hero extension), so the read covers just
    // the inherited fields.
    std::uint64_t attr_u64 = 0;
    if (mem_read_u64(ptr + OFF_FOE_ATTR, &attr_u64)) {
        std::uintptr_t attr = static_cast<std::uintptr_t>(attr_u64);
        if (mem_is_userland(attr)) {
            double ua[FA_COUNT]{};
            if (mem_read_bytes(attr + OFF_ATTR_BLOCK_BASE, ua,
                               OFF_ATTR_BLOCK_BYTES)) {
                out.attr_ok = true;
                out.hp      = ua[FA_HEALTH];
                out.max_hp  = ua[FA_MAX_HEALTH];
                out.shield  = ua[FA_SHIELD];
                out.hp_pct  = (out.max_hp > 0.0) ? (out.hp / out.max_hp) : 0.0;
            }
        }
    }

    if (hero_locked) {
        double dx = out.x - hx, dy = out.y - hy;
        out.dist = std::sqrt(dx * dx + dy * dy);
    }
    return true;
}

}  // namespace

void foe_state_start() {
    if (g_active.exchange(true)) return;
    g_tracked.clear();
    g_tracked.reserve(kMaxTracked);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.clear();
    }
    g_snapshot = FoesSnapshot{};
    g_snapshot.target_index = -1;
    g_ticks.store(0);
    hl_hook_register(L"ent.Foe", on_foe_alloc);
    logf("foe_state: watcher registered on ent.Foe");
}

void foe_state_stop() {
    g_active.store(false);
}

void foe_state_tick() {
    if (!g_active.load()) return;
    std::uint64_t n = g_ticks.fetch_add(1) + 1;

    if (g_foe_type == 0) {
        g_foe_type = hl_hook_get_type(L"ent.Foe");
    }

    // Drain pending allocations into the tracked list. Cap insertions
    // per tick so a sudden mob spawn doesn't blow the render budget.
    constexpr std::size_t kMaxAddPerTick = 8;
    std::vector<std::uintptr_t> drained;
    drained.reserve(kMaxAddPerTick);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        while (!g_pending.empty() && drained.size() < kMaxAddPerTick) {
            drained.push_back(g_pending.front());
            g_pending.pop_front();
        }
    }
    for (std::uintptr_t p : drained) {
        // Dedupe + cap.
        bool dup = false;
        for (auto& t : g_tracked) {
            if (t.ptr == p) { dup = true; break; }
        }
        if (dup) continue;
        if (g_tracked.size() < kMaxTracked) {
            g_tracked.push_back({p, n});
        } else {
            // Replace the oldest entry to keep the cap.
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < g_tracked.size(); ++i) {
                if (g_tracked[i].last_valid_tick <
                    g_tracked[oldest].last_valid_tick) oldest = i;
            }
            g_tracked[oldest] = {p, n};
        }
    }

    // Hero pointer for the Foe.target match + distance compute.
    HeroSnapshot h = hero_state_read();
    std::uintptr_t hero_target = 0;
    if (h.locked && h.has_target) {
        std::uintptr_t hero_ptr = hero_state_locked_ptr();
        if (hero_ptr) {
            std::uint64_t t = 0;
            if (mem_read_u64(hero_ptr + OFF_FOE_TARGET, &t)) {
                hero_target = static_cast<std::uintptr_t>(t);
            }
        }
    }

    // Rebuild snapshot. Walk tracked list, attempt read for each,
    // drop stale entries.
    FoesSnapshot snap{};
    snap.target_index = -1;
    std::vector<TrackedFoe> still_alive;
    still_alive.reserve(g_tracked.size());

    for (auto& t : g_tracked) {
        if (snap.count >= kMaxFoes) {
            still_alive.push_back(t);
            continue;
        }
        FoeEntry e{};
        if (read_one(t.ptr, e, h.x, h.y, h.locked)) {
            t.last_valid_tick = n;
            if (hero_target && t.ptr == hero_target) {
                snap.target_index = (int)snap.count;
            }
            snap.foes[snap.count++] = e;
            still_alive.push_back(t);
        } else {
            // Three consecutive failed reads removes the entry.
            // Single failures can happen during GC compaction so
            // we don't prune on the first miss.
            if (n - t.last_valid_tick > 5) {
                // dropped
            } else {
                still_alive.push_back(t);
            }
        }
    }
    g_tracked = std::move(still_alive);

    // Sort by distance ascending so plugin readers get nearest-first.
    // Tracks the target_index after the sort.
    int new_target = -1;
    if (snap.target_index >= 0) {
        // Mark the target's ptr before sorting, look it up after.
        std::uintptr_t tgt_ptr = snap.foes[snap.target_index].ptr;
        // Insertion sort (tiny array, simpler than std::sort wrapper).
        for (std::size_t i = 1; i < snap.count; ++i) {
            FoeEntry tmp = snap.foes[i];
            std::size_t j = i;
            while (j > 0 && snap.foes[j - 1].dist > tmp.dist) {
                snap.foes[j] = snap.foes[j - 1];
                --j;
            }
            snap.foes[j] = tmp;
        }
        for (std::size_t i = 0; i < snap.count; ++i) {
            if (snap.foes[i].ptr == tgt_ptr) { new_target = (int)i; break; }
        }
        snap.target_index = new_target;
    } else {
        for (std::size_t i = 1; i < snap.count; ++i) {
            FoeEntry tmp = snap.foes[i];
            std::size_t j = i;
            while (j > 0 && snap.foes[j - 1].dist > tmp.dist) {
                snap.foes[j] = snap.foes[j - 1];
                --j;
            }
            snap.foes[j] = tmp;
        }
    }

    g_snapshot = snap;
}

FoesSnapshot foe_state_read() { return g_snapshot; }

}  // namespace farever

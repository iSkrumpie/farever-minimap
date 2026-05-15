#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

struct DamageEvent {
    std::uintptr_t dr_ptr;        // unique ID for dedupe
    double         damage;
    std::int32_t   hit_count;
    std::uint8_t   is_crit;
    std::uint8_t   is_kill;
    char           skill[64];     // ASCII, NUL-terminated
};

// Register the hl_alloc_obj watcher and start the pump thread. The
// watcher pushes freshly-allocated DamageResult pointers into a
// pending queue (their fields are still zero at hook time — the
// Haxe constructor runs AFTER hl_alloc_obj returns); the pump
// dequeues each entry on its next iteration, reads damage / hit /
// crit / skill, filters garbage, and pushes a DamageEvent into the
// drainable event ring.
//
// The pump thread registers itself with HashLink's GC via
// hl_register_thread on start and hl_unregister_thread on stop. An
// unregistered thread reading heap pointers can race with the
// stop-the-world GC and the Boehm collector won't scan its stack as
// roots — both of which were the most likely cause of the hxbit
// "Could not sync object" crash seen in the first phase-3a build.
void damage_start(const LibHL& libhl);
void damage_stop();

// Pull queued events for the aggregator (called from the render
// thread). Returns count written.
std::size_t damage_drain(DamageEvent* out, std::size_t max);

// Diagnostic counters.
struct DamageStats {
    std::uint64_t allocs_seen;
    std::uint64_t events_emitted;
    std::uint64_t dropped_uninit;     // pending entries that never settled
    std::uint64_t dropped_garbage;    // filtered post-init (bad hitcount/skill)
    std::uintptr_t damage_result_tag;
};
DamageStats damage_stats();

}  // namespace farever

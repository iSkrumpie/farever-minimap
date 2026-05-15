#pragma once

#include <cstddef>
#include <cstdint>

namespace farever {

struct SkillRow {
    char         skill[64];
    std::int32_t hit_count;
    double       total;
    double       max_hit;
    std::int32_t crit_count;
};

constexpr std::size_t kMaxRows = 64;

// Frame snapshot consumed by the overlay's DPS table.
struct AggSnapshot {
    bool         scanning_ready;   // damage source has cached the DR type tag
    bool         have_fight;       // at least one event has fired
    double       elapsed_sec;      // since first damage event of this pull
    double       total_damage;
    double       dps;
    std::size_t  row_count;
    SkillRow     rows[kMaxRows];   // sorted by total desc, truncated
};

// Drain new damage events, fold them into the current pull, refresh
// the snapshot. Call once per frame from the render thread (after
// damage_tick — order matters: damage_tick produces events, this
// consumes them).
void aggregator_tick();

AggSnapshot aggregator_snapshot();

// Manual reset (F9). Clears rows and the elapsed timer.
void aggregator_reset();

}  // namespace farever

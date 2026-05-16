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

// One past fight, sealed when combat ends. Kept in a small ring of the
// 10 most-recent so the user can glance at recent pulls.
struct FightLogEntry {
    int          id;            // monotonic; mostly for stable UI keys
    std::int64_t ended_unix_ms; // GetSystemTimeAsFileTime-derived
    double       duration_sec;
    double       total_damage;
    double       dps;
    std::int32_t hit_count;
    char         top_skill[64]; // highest-total skill of the fight
    std::size_t  row_count;     // per-skill breakdown, sorted total-desc
    SkillRow     rows[kMaxRows];
};

constexpr std::size_t kFightHistoryMax = 10;

// Frame snapshot consumed by the overlay's DPS table.
struct AggSnapshot {
    bool         scanning_ready;   // damage source has cached the DR type tag
    bool         have_fight;       // at least one event has fired
    bool         in_combat;        // damage seen within the idle timeout
    double       elapsed_sec;      // since first damage event of this pull
    double       idle_sec;         // since last damage event (0 while active)
    double       total_damage;
    double       dps;
    std::size_t  row_count;
    SkillRow     rows[kMaxRows];   // sorted by total desc, truncated

    std::size_t   history_count;            // newest first
    FightLogEntry history[kFightHistoryMax];
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

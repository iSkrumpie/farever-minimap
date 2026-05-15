// DamageEvent → SkillRow aggregator. Single-threaded; everything runs
// on the render thread alongside damage_tick. No combat-state tracking
// in this build — fight starts at the first damage event and runs
// until F9. (Hero pointer tracking via the alloc-hook landed in a
// follow-up commit; once we read Hero.combat_id we can auto-segment.)

#include "aggregator.h"
#include "damage.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace farever {
namespace {

struct Fight {
    bool   have_first_damage = false;
    DWORD  first_damage_tick = 0;
    std::unordered_map<std::string, SkillRow> rows;
};

Fight        g_fight;
AggSnapshot  g_snapshot{};

// Base-attack chain collapse: "DS_Base_Attack_1/2/3/4" → "DS_Base_Attack".
// Only triggers on names containing "Base_Attack" to leave genuine combo
// skills (e.g. DS_Bladeleaf_Skill1) alone.
void normalise_skill(char* skill) {
    if (!std::strstr(skill, "Base_Attack")) return;
    std::size_t len = std::strlen(skill);
    while (len > 0 && skill[len - 1] >= '0' && skill[len - 1] <= '9') {
        skill[--len] = 0;
    }
    if (len > 0 && skill[len - 1] == '_') skill[--len] = 0;
}

void record(const DamageEvent& ev) {
    char skill[64];
    std::memcpy(skill, ev.skill, sizeof(skill));
    skill[sizeof(skill) - 1] = 0;
    normalise_skill(skill);

    if (!g_fight.have_first_damage) {
        g_fight.have_first_damage = true;
        g_fight.first_damage_tick = GetTickCount();
        logf("aggregator: first damage (%s)", skill);
    }

    std::string key(skill);
    auto it = g_fight.rows.find(key);
    if (it == g_fight.rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, skill, sizeof(r.skill) - 1);
        r.hit_count  = (ev.hit_count > 0) ? ev.hit_count : 1;
        r.total      = ev.damage;
        r.max_hit    = ev.damage;
        r.crit_count = ev.is_crit ? 1 : 0;
        g_fight.rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += (ev.hit_count > 0) ? ev.hit_count : 1;
        r.total     += ev.damage;
        if (ev.damage > r.max_hit) r.max_hit = ev.damage;
        if (ev.is_crit) r.crit_count += 1;
    }
}

void publish() {
    AggSnapshot s{};
    s.have_fight      = g_fight.have_first_damage;
    s.scanning_ready  = true;   // hook source has no warm-up to gate on

    double elapsed = 0.0;
    if (g_fight.have_first_damage) {
        elapsed = (GetTickCount() - g_fight.first_damage_tick) / 1000.0;
        if (elapsed < 0.001) elapsed = 0.001;
    }
    double total = 0.0;
    for (const auto& kv : g_fight.rows) total += kv.second.total;

    s.elapsed_sec  = elapsed;
    s.total_damage = total;
    s.dps          = (elapsed > 0.001) ? (total / elapsed) : 0.0;

    SkillRow temp[kMaxRows];
    std::size_t n = 0;
    for (const auto& kv : g_fight.rows) {
        if (n >= kMaxRows) break;
        temp[n++] = kv.second;
    }
    std::sort(temp, temp + n, [](const SkillRow& a, const SkillRow& b) {
        return a.total > b.total;
    });
    s.row_count = n;
    for (std::size_t i = 0; i < n; ++i) s.rows[i] = temp[i];

    g_snapshot = s;
}

}  // namespace

void aggregator_tick() {
    constexpr std::size_t kBatch = 64;
    DamageEvent batch[kBatch];
    while (true) {
        std::size_t n = damage_drain(batch, kBatch);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) record(batch[i]);
        if (n < kBatch) break;
    }
    publish();
}

AggSnapshot aggregator_snapshot() { return g_snapshot; }

void aggregator_reset() {
    g_fight.rows.clear();
    g_fight.have_first_damage = false;
    g_fight.first_damage_tick = 0;
    publish();
    logf("aggregator: manual reset");
}

}  // namespace farever

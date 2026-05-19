#pragma once

#include <cstdint>
#include <cstddef>

namespace farever {

// One tracked Foe entry. Populated each render-tick from the foe's
// own fields via the same UnitAttributes chase used for the Hero.
struct FoeEntry {
    std::uintptr_t ptr;           // raw HL pointer (used as id by plugins.target)
    double         x, y, z;
    double         rot_z;
    bool           in_combat;
    bool           has_target;    // Foe.target != 0
    int            level;
    bool           attr_ok;       // true if HP / max_hp / etc populated
    double         hp;
    double         max_hp;
    double         hp_pct;        // 0..1, 0 if attr_ok false
    double         shield;
    double         dist;          // distance to local Hero, 0 if no lock
};

// Snapshot of currently-tracked foes. Bounded array. Sorted by
// dist ascending so plugins reading just the first N get the nearest
// ones.
constexpr std::size_t kMaxFoes = 32;

struct FoesSnapshot {
    std::size_t count;
    FoeEntry    foes[kMaxFoes];
    // The Foe entry matching Hero.target, or nullptr if no target /
    // target isn't in our tracked list. Points into foes[].
    int         target_index;   // -1 if no target
};

// Register the ent.Foe hl_alloc_obj watcher. Same lifecycle pattern
// as hero_state / damage / entity_state.
void foe_state_start();
void foe_state_stop();
void foe_state_tick();

// Lock-free snapshot read (returns a copy). Safe from any thread.
FoesSnapshot foe_state_read();

}  // namespace farever

#pragma once

#include <cstdint>

namespace farever {

// Snapshot of the local Hero used by the minimap render.
struct HeroSnapshot {
    bool   locked;          // false until isMe + bidirectional check passes
    double x, y, z;
    double rot_z;
};

// Register the ent.Hero alloc-hook watcher. The watcher pushes raw
// Hero pointers into a pending list; hero_state_tick (called per
// frame from Present) waits until each candidate's constructor has
// populated ownerPlayer, then verifies isMe + bidirectional Player.hero.
//
// Multiple Heroes get allocated per zone transition (you + remote
// players + NPCs in some cases). Exactly one of them ever satisfies
// the isMe check — that's the local player. Once locked, we
// re-validate every 64 ticks to survive dungeon transitions.
void hero_state_start();
void hero_state_stop();

// Per-frame validate + position read. Called from the Present hook
// (same thread as damage_tick / overlay).
void hero_state_tick();

HeroSnapshot hero_state_read();

}  // namespace farever

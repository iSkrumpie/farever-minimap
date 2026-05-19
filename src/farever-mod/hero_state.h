#pragma once

#include <cstdint>

namespace farever {

// Snapshot of the local Hero used by the minimap render and the plugin
// API. Numeric attributes come from Hero.attr (HeroAttributes /
// UnitAttributes). All `attr_ok`-gated values are zero when the lock
// is not held or when the attr chase fails this tick.
struct HeroSnapshot {
    bool   locked;          // false until isMe + bidirectional check passes
    double x, y, z;
    double rot_z;
    bool   in_combat;       // ent.Hero.isInCombat
    double combat_start;    // Hero.combatStartTime (game seconds)
    int    level;           // Hero._level
    bool   has_target;      // Hero.target != 0

    bool   attr_ok;         // true if attr pointer + reads succeeded

    // Primary character stats
    double vitality;
    double strength;
    double dexterity;
    double faith;
    double intellect;

    // Combat
    double crit_chance;
    double crit_damage;
    double armor_penetration;
    double spell_penetration;
    double fervor;
    double block_mitigation;
    double dodge_chance;
    double magic_mastery;
    double physical_mastery;
    double spell_cast_time_reduction;
    double knock_resistance;
    double cooldown_reduction;

    // Defense
    double armor;
    double magic_armor;
    double magic_reduction;

    // Health and energy
    double health;
    double max_health;
    double health_regen;
    double shield;
    double energy;
    double energy_regen;

    // Misc
    double move_speed_factor;
    double damage;            // base damage modifier
    double heal;              // base heal output

    // Hero-only (HeroAttributes layer)
    bool   hero_attr_ok;
    double poise;
    double poise_regen;
    double oxygen;
    double rage;
    double rage_regen;
    double spark;
    double spark_regen;
    double combo_point;
    double focus;
    double damage_modifier;
    double damage_taken_modifier;
    double heal_given_multiplier;
    double shield_power_multiplier;
    double glide_speed;
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

// Raw pointer to the currently-locked Hero, or 0 if no lock. Cheap
// atomic load — used by the damage filter to drop DamageResults whose
// serverSource isn't us (= incoming damage, bleeds etc.).
std::uintptr_t hero_state_locked_ptr();

// v0.4.15 anticrash mode. When enabled, hero_state_tick switches to
// polling Player.hero via the back-reference (instead of relying on
// the alloc-hook watcher for re-locks) once the initial lock has been
// stable for 5 seconds. At that moment hero_state asks hl_hook to
// disable the hl_alloc_obj trampoline entirely so the game's allocator
// runs with zero overhead from us. Trade-off: damage tracking stops.
// Engaged by dllmain at boot when data/anticrash.flag is present.
void hero_state_set_anticrash(bool on);
bool hero_state_anticrash_armed();
bool hero_state_anticrash_disarmed();   // true after the alloc-hook
                                        // has actually been removed

}  // namespace farever

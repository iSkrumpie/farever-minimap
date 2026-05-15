# Farever DPS Meter — Plan

Standalone companion mod to `../src/minimap-dll`. Same engine stack
(HashLink + Heaps + D3D12), same injection model, separate DLL so the
minimap release stays untouched.

## What we want to track

Per skill: hits, total damage, max single hit, crit %, DPS over the
current fight window. The game already tracks combat encounters
(`isInCombat`, `combatStartTime`, `combatEndTime`, `combatId` on
`ent.Hero`) so encounter detection is solved for us.

## Threat model

Same as the minimap mod, strictly read-only. The local client only ever
renders its own floating damage numbers (confirmed by the user), so the
damage buffer on `Hero` only contains our own damage — no risk of
reading other players' state. See `../docs/plan.md` § Threat model.

## M0 findings — the smoking gun

`ent.Hero` (extends `ent.Unit`) hat schon alles eingebaut:

| Offset | Type | Field | Notes |
| -----: | ---- | ----- | ----- |
|  +664  | HOBJ | `combat` -> `hxbit.ArrayProxyData` | networked combat events |
|  +672  | HBOOL | `isInCombat` | fight gate |
|  +688  | HF64 | `combatStartTime` | fight start (game time) |
|  +696  | HF64 | `combatEndTime` | fight end |
|  +704  | HF64 | `lastCombatSkillEnd` |
|  +712  | HI32 | `combatId` | unique fight id, resets each encounter |
|  +848  | HF64 | `lastDamageTime` | last hit timestamp |
|  +856  | HOBJ | `combatDamageHistory` -> `hl.types.ArrayObj` | full per-hit log |
|  +864  | HOBJ | `combatDamages` -> `hl.types.ArrayObj` | likely "current fight" subset |

`st.skill.DamageResult` (136 bytes) is the per-hit record:

| Offset | Type | Field |
| -----: | ---- | ----- |
|   +8   | HOBJ | `baseSkill` -> `st.skill.BaseSkill` |
|  +24   | HOBJ | `serverSource` -> `ent.GameObject` |
|  +40   | HOBJ | `target` -> `ent.GameObject` |
|  +48   | HOBJ | `affinity` -> `String` (damage type tag) |
|  +56   | HI32 | `effect` |
|  +72   | HI32 | `stepIdx` |
|  +80   | HF64 | `_amount` |
|  +88   | HI32 | `_hitCount` |
|  +96   | HF64 | `_block` |
| +104   | HBOOL | `_kill` |
| +105   | HBOOL | `_critical` |
| +112   | HF64 | `_threatMultiplier` |

`hl.types.ArrayObj` (24 bytes):

|  +8 | HI32   | `length` |
| +16 | HARRAY | `array` -> HashLink `varray*` (elements at varray+24) |

`varray` (libhl runtime, fixed): `t, at, size, _pad` then HOBJ pointers
inline starting at offset 24, 8 bytes each.

## Architecture

Same shape as the minimap DLL:

1. **Hero scan** (port of `../src/minimap-dll/hero_scan.cpp`):
   parallel heap walk, structural fingerprint (4 doubles at +144),
   filtered to local via `Player.isMe == 1`. Cooldown + periodic
   re-validation lifted directly.
2. **Damage poll**: every N frames, walk `Hero.combatDamageHistory`,
   dedupe by `DamageResult` pointer (each lives until the next fight),
   push new ones into an in-DLL ring buffer.
3. **Aggregator**: group by `baseSkill` name, accumulate hits / total /
   max / crit count over the current `combatId`. Reset when `combatId`
   changes or `combatEndTime` falls outside our window.
4. **UI**: ImGui table window. Sortable columns, live DPS at the top,
   reset button, encounter timer driven by `combatStartTime` /
   `combatEndTime`.

## Milestones

### M0 — Bytecode recon ✅

`tools/find_combat_classes.py` + `tools/dump_combat_layouts.py` —
identified `DamageResult`, the Hero's own combat history arrays, the
existing `CombatMeter` / `UnitCombatMeter` UI (used only for
visualisation, we go straight to the data).

### M1 — Live Python probe

`tools/probe_damage.py` — attaches via the minimap mod's `probe.attach`
+ `find_hero.scan_for_heroes`, picks the local Hero, walks
`combatDamageHistory`, prints each new `DamageResult`. Validation gate
before any C++ is written. **Goal**: see new entries appear when we
hit something in-game.

### M2 — In-process damage scan + ring buffer

Port the probe into the dpsmeter DLL. Reuse `hero_scan.cpp` style
(SEH-wrapped reads, periodic `is_local_hero` re-validation, multi-thread
initial scan). Add `damage_scan.cpp` that polls Hero+856 every ~5 frames,
dedupes by pointer into a fixed-size ring of ~1024 events.

### M3 — Encounter detection + aggregation

Use `combatId` as the fight key. On change: snapshot the previous fight
into history, reset the per-skill aggregator. DPS = sum(amount) /
(min(now, combatEndTime) − combatStartTime). Skill name comes from
`baseSkill.kind` (HOBJ String at offset +152 on `st.skill.Skill`).

### M4 — ImGui DPS table UI

Borderless ImGui window, draggable, can be docked or pinned next to
the minimap. WoW palette to stay consistent with the minimap aesthetic.
Columns: Skill, Hits, Total, Max, Crit %, DPS, % of total.

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| `combatDamageHistory` reset semantics unclear | Probe first, observe behaviour across encounters before coding aggregation. |
| varray layout drift (HashLink version bumps) | Pinned by `tools/version-pin.md`; element-base offset is the only thing that matters and 24 is stable in HL 1.15. |
| Skill `baseSkill` may dangle after fight end | Same Boehm-GC risk as Hero lock — read via SEH, drop and re-pick if zeroed. |
| Damage UI overlaps the minimap | Both windows are draggable; user pins them where they want. |

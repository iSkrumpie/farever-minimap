# DPS Meter — M0/M1 findings

## TL;DR
Found a working damage-event source: scan the heap for
`ui.comp.DamageDisplay` instances by canonical `hl_type*`. Each
instance is one floating damage number on screen. Since the local
client only renders the local player's damage, every match is OUR
damage.

## Why combatDamageHistory didn't work

`ent.Unit` defines three fields that *look* like damage history:
- `+664 combat : hxbit.ArrayProxyData`  (networked)
- `+856 combatDamageHistory : hl.types.ArrayObj`  (local)
- `+864 combatDamages : hl.types.ArrayObj`  (local)

Live probing showed:
- `+856` and `+864` stay length=0 even when `isInCombat=1` and damage
  is being taken (`lastDmgT` updates correctly).
- `+664` reaches an ArrayObj of length=2 with pointers ~32 bytes apart;
  walking those as DamageResults gave all-zero fields. Likely
  `combatTargets` (list of foes we have aggro on), NOT a damage log.

Conclusion: those fields are server-authoritative bookkeeping, never
populated client-side.

## What does work: DamageDisplay UI elements

`ui.comp.DamageDisplay` is the floating-damage-number UI element. The
game spawns one per damage event the local player deals. Layout:
```
@1080 lifetime    HF64
@1088 maxLifetime HF64
@1112 spread      HF64
@1136 damage      HF64   ← the value
@1144 isCrit      HBOOL
@1176 dmg         HOBJ   ← pointer to st.skill.DamageResult
```

`st.skill.DamageResult` (136 bytes):
```
@8    HOBJ baseSkill -> st.skill.BaseSkill
@24   HOBJ serverSource (the attacker GameObject)
@40   HOBJ target (the victim GameObject)
@56   HI32 effect
@80   HF64 _amount
@88   HI32 _hitCount
@104  HBOOL _kill
@105  HBOOL _critical
```

Skill name: `BaseSkill+152 -> kind:String` then standard Haxe String
(`+8 bytes`, `+16 length`).

## Type tag anchoring

We can't fingerprint-scan reliably (too many UI elements share
similar HF64 layouts — h2d.Tile, Xml, etc. all matched my v5
fingerprint). The robust path:
1. UTF-16-search the heap for the class name `ui.comp.DamageDisplay`.
2. Pointer-search for refs to it -> hl_type_obj (at offset 16 of obj).
3. Pointer-search for refs to that obj -> hl_type (at offset 8 of type).
4. The hl_type address IS the canonical type tag.

On the 2026-05-15 build the DamageDisplay type tag was
`0x00000199291c5df8`. **The tag is heap-allocated and changes
every game start** — it must be re-anchored per session.

`tools/find_type_by_name.py` automates the anchoring (~3-4 min for
the full chase). `tools/probe_dmgdisplay_v6.py --type-tag <hex>` then
does a single ptr-match scan per region for the live monitor.

## HashLink struct layouts (libhl 1.15)

`hl_type` (32 bytes):
```
+0  u32 kind         (HOBJ = 11)
+4  pad
+8  ptr obj          (hl_type_obj*)
+16 ptr vobj_proto
+24 ptr markbits
```

`hl_type_obj`:
```
+0  i32 nfields
+4  i32 nproto
+8  i32 nbindings
+12 pad
+16 ptr name         (UTF-16 string, null-terminated)
+24 ptr super
...
```

`hl.types.ArrayObj` (24):
```
+8  i32 length
+16 ptr varray (HashLink varray; elements start at varray+24, 8 bytes each)
```

## Open work

1. Speed: full heap scan ~37s. Hot-region caching after first scan
   gets us to sub-second polls (in flight).
2. Skill name decoding: walk DR -> baseSkill -> kind:String. Needs
   String pointer chase implemented.
3. Aggregator: dedupe by DR pointer, group by skill name, compute
   per-skill totals + DPS over `Hero.combatStartTime..now`.
4. Encounter detection: use `Hero.combatId` change as fight boundary.
5. Port the type-tag anchor + scan to C++ (DLL).
6. ImGui UI mirroring the minimap aesthetic.

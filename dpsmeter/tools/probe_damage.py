"""M1 live probe: attach to Farever, lock onto the local Hero, then walk
`Hero.combatDamageHistory` and print each damage event in real time.

This is the validation step before we port the same logic into the
dpsmeter DLL. If this works against the running game, the C++ port is
trivial (we already do the same kind of memory walk in the minimap mod
for the Hero position).

Layout (from hlbc_parse, 2026-05-14 build):

  ent.Hero (super ent.Unit) fields used here:
    +672  HBOOL    isInCombat
    +688  HF64     combatStartTime
    +696  HF64     combatEndTime
    +712  HI32     combatId
    +848  HF64     lastDamageTime
    +856  HOBJ     combatDamageHistory : hl.types.ArrayObj
    +864  HOBJ     combatDamages       : hl.types.ArrayObj

  hl.types.ArrayObj:
    +8    HI32     length
    +16   HARRAY   array          -> HashLink varray*

  HashLink varray (libhl, fixed):
    +0    hl_type* t
    +8    hl_type* at
    +16   i32      size
    +20   i32      _pad
    elements start at +24 (8 bytes each for HOBJ refs)

  st.skill.DamageResult:
    +8    HOBJ     baseSkill : st.skill.BaseSkill
    +24   HOBJ     serverSource : ent.GameObject
    +40   HOBJ     target : ent.GameObject
    +56   HI32     effect
    +80   HF64     _amount         <- damage number
    +88   HI32     _hitCount
    +96   HF64     _block
    +104  HBOOL    _kill
    +105  HBOOL    _critical

Usage:
    python tools/probe_damage.py [--once]
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

# Reuse find_hero / find_me machinery from the main project tools/.
PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))

from find_hero import scan_for_heroes        # type: ignore[import-not-found]
from probe import attach                     # type: ignore[import-not-found]

# Hero offsets
OFF_OWNERPLAYER             = 16
OFF_HERO_IN_PLAYER          = 272
OFF_ISME                    = 280
OFF_IS_IN_COMBAT            = 672
OFF_COMBAT_START            = 688
OFF_COMBAT_END              = 696
OFF_COMBAT_ID               = 712
OFF_LAST_DAMAGE_TIME        = 848
OFF_COMBAT_DAMAGE_HISTORY   = 856
OFF_COMBAT_DAMAGES          = 864

# ArrayObj
OFF_ARRAY_LENGTH    = 8
OFF_ARRAY_VARRAY    = 16

# varray
VARRAY_ELEMENTS_START = 24

# DamageResult
OFF_DR_BASESKILL    = 8
OFF_DR_SOURCE       = 24
OFF_DR_TARGET       = 40
OFF_DR_AMOUNT       = 80
OFF_DR_HITCOUNT     = 88
OFF_DR_BLOCK        = 96
OFF_DR_KILL         = 104
OFF_DR_CRITICAL     = 105


def read_u8(pm, addr: int) -> int:
    return pm.read_bytes(addr, 1)[0]

def read_u32(pm, addr: int) -> int:
    return struct.unpack("<I", pm.read_bytes(addr, 4))[0]

def read_i32(pm, addr: int) -> int:
    return struct.unpack("<i", pm.read_bytes(addr, 4))[0]

def read_u64(pm, addr: int) -> int:
    return struct.unpack("<Q", pm.read_bytes(addr, 8))[0]

def read_f64(pm, addr: int) -> float:
    return struct.unpack("<d", pm.read_bytes(addr, 8))[0]


def find_local_hero(pm) -> int | None:
    candidates = scan_for_heroes(pm)
    if not candidates:
        return None
    for addr, _, _ in candidates:
        try:
            owner = read_u64(pm, addr + OFF_OWNERPLAYER)
            if owner == 0:
                continue
            if read_u64(pm, owner + OFF_HERO_IN_PLAYER) != addr:
                continue
            if read_u8(pm, owner + OFF_ISME) == 1:
                return addr
        except Exception:
            continue
    return None


def walk_damage_array(pm, hero_addr: int, offset: int) -> list[tuple[int, dict]]:
    """Walk a Hero.combatDamage(History|s) array, return [(dr_addr, {fields})]."""
    arr_ptr = read_u64(pm, hero_addr + offset)
    if arr_ptr == 0:
        return []
    length = read_i32(pm, arr_ptr + OFF_ARRAY_LENGTH)
    if length <= 0 or length > 10000:
        return []
    varray_ptr = read_u64(pm, arr_ptr + OFF_ARRAY_VARRAY)
    if varray_ptr == 0:
        return []
    elements_base = varray_ptr + VARRAY_ELEMENTS_START
    out: list[tuple[int, dict]] = []
    for i in range(length):
        try:
            dr = read_u64(pm, elements_base + i * 8)
            if dr == 0:
                continue
            row = {
                "amount":    read_f64(pm, dr + OFF_DR_AMOUNT),
                "hitCount":  read_i32(pm, dr + OFF_DR_HITCOUNT),
                "block":     read_f64(pm, dr + OFF_DR_BLOCK),
                "kill":      read_u8(pm, dr + OFF_DR_KILL),
                "critical":  read_u8(pm, dr + OFF_DR_CRITICAL),
                "baseSkill": read_u64(pm, dr + OFF_DR_BASESKILL),
                "source":    read_u64(pm, dr + OFF_DR_SOURCE),
                "target":    read_u64(pm, dr + OFF_DR_TARGET),
            }
            out.append((dr, row))
        except Exception as e:
            print(f"  !! element {i} read failed: {e}")
    return out


def print_hero_combat_state(pm, hero: int) -> None:
    try:
        is_in   = read_u8(pm,  hero + OFF_IS_IN_COMBAT)
        start   = read_f64(pm, hero + OFF_COMBAT_START)
        end     = read_f64(pm, hero + OFF_COMBAT_END)
        cid     = read_i32(pm, hero + OFF_COMBAT_ID)
        ldt     = read_f64(pm, hero + OFF_LAST_DAMAGE_TIME)
        print(f"  isInCombat={is_in}  combatId={cid}  start={start:.2f}  end={end:.2f}  lastDmgT={ldt:.2f}")
    except Exception as e:
        print(f"  !! state read failed: {e}")


def main() -> int:
    once = "--once" in sys.argv
    pm, _ = attach()
    print("[probe] attached. finding local Hero...", file=sys.stderr)
    hero = find_local_hero(pm)
    if hero is None:
        print("[probe] no local Hero (run when in-world)", file=sys.stderr)
        return 1
    print(f"[probe] local Hero @ 0x{hero:016x}")

    seen: set[int] = set()
    last_len_h = -1
    last_len_c = -1
    while True:
        print_hero_combat_state(pm, hero)
        # Try both arrays; usually combatDamageHistory has the full record.
        hist = walk_damage_array(pm, hero, OFF_COMBAT_DAMAGE_HISTORY)
        cur  = walk_damage_array(pm, hero, OFF_COMBAT_DAMAGES)
        if len(hist) != last_len_h or len(cur) != last_len_c:
            print(f"  combatDamageHistory={len(hist)}  combatDamages={len(cur)}")
            last_len_h = len(hist)
            last_len_c = len(cur)
        for dr_addr, row in hist:
            if dr_addr in seen:
                continue
            seen.add(dr_addr)
            crit = "CRIT" if row["critical"] else ""
            kill = "KILL" if row["kill"]     else ""
            print(f"  [{dr_addr:016x}] amount={row['amount']:7.1f}  hits={row['hitCount']:>2}  "
                  f"block={row['block']:.1f}  {crit:4} {kill:4}  "
                  f"skill=0x{row['baseSkill']:x}  src=0x{row['source']:x}  tgt=0x{row['target']:x}")
        if once:
            return 0
        time.sleep(0.5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[probe] stopped.")

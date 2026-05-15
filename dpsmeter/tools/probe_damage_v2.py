"""Probe v2: walk combatDamageHistory on EVERY Unit-like instance in the
heap (not just our own Hero). Hypothesis: the array on a unit tracks
damage that unit RECEIVED, so the foes you hit are the ones with
populated histories — and each DamageResult's `serverSource` points
back to your Hero.

Strategy:
  1. Scan once for all candidates (4-double position fingerprint =
     every Hero+Foe+NPC subclass of ent.Unit).
  2. Find the local Hero among them (isMe check).
  3. Lock the candidate list. Every poll, walk each candidate's
     combatDamageHistory, dedupe by DamageResult pointer, attribute
     hits whose serverSource == localHero to "us".
"""
from __future__ import annotations
import struct
import sys
import time
from pathlib import Path

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))

from find_hero import scan_for_heroes           # type: ignore[import-not-found]
from probe import attach                        # type: ignore[import-not-found]

OFF_OWNERPLAYER             = 16
OFF_HERO_IN_PLAYER          = 272
OFF_ISME                    = 280
OFF_COMBAT_DAMAGE_HISTORY   = 856
OFF_COMBAT_DAMAGES          = 864
OFF_ARRAY_LENGTH            = 8
OFF_ARRAY_VARRAY            = 16
VARRAY_ELEMENTS_START       = 24
OFF_DR_BASESKILL            = 8
OFF_DR_SOURCE               = 24
OFF_DR_TARGET               = 40
OFF_DR_AMOUNT               = 80
OFF_DR_HITCOUNT             = 88
OFF_DR_CRITICAL             = 105
OFF_DR_KILL                 = 104


def u8 (pm, a): return pm.read_bytes(a, 1)[0]
def u32(pm, a): return struct.unpack("<I", pm.read_bytes(a, 4))[0]
def i32(pm, a): return struct.unpack("<i", pm.read_bytes(a, 4))[0]
def u64(pm, a): return struct.unpack("<Q", pm.read_bytes(a, 8))[0]
def f64(pm, a): return struct.unpack("<d", pm.read_bytes(a, 8))[0]


def safe(fn, *args, default=None):
    try:
        return fn(*args)
    except Exception:
        return default


def find_local_hero(pm, candidates) -> int | None:
    for addr, _, _ in candidates:
        owner = safe(u64, pm, addr + OFF_OWNERPLAYER, default=0)
        if not owner:
            continue
        if safe(u64, pm, owner + OFF_HERO_IN_PLAYER, default=0) != addr:
            continue
        if safe(u8, pm, owner + OFF_ISME, default=0) == 1:
            return addr
    return None


def walk_history(pm, unit_addr: int, field_off: int) -> list[tuple[int, dict]]:
    arr_ptr = safe(u64, pm, unit_addr + field_off, default=0)
    if not arr_ptr:
        return []
    length = safe(i32, pm, arr_ptr + OFF_ARRAY_LENGTH, default=0)
    if length <= 0 or length > 5000:
        return []
    varray = safe(u64, pm, arr_ptr + OFF_ARRAY_VARRAY, default=0)
    if not varray:
        return []
    base = varray + VARRAY_ELEMENTS_START
    out: list[tuple[int, dict]] = []
    for i in range(length):
        dr = safe(u64, pm, base + i * 8, default=0)
        if not dr:
            continue
        out.append((dr, {
            "amount": safe(f64, pm, dr + OFF_DR_AMOUNT, default=0.0) or 0.0,
            "hits":   safe(i32, pm, dr + OFF_DR_HITCOUNT, default=0) or 0,
            "crit":   safe(u8,  pm, dr + OFF_DR_CRITICAL, default=0) or 0,
            "kill":   safe(u8,  pm, dr + OFF_DR_KILL,     default=0) or 0,
            "source": safe(u64, pm, dr + OFF_DR_SOURCE,   default=0) or 0,
            "target": safe(u64, pm, dr + OFF_DR_TARGET,   default=0) or 0,
            "skill":  safe(u64, pm, dr + OFF_DR_BASESKILL,default=0) or 0,
        }))
    return out


def main() -> int:
    pm, _ = attach()
    print("[v2] scanning heap for Unit candidates...", file=sys.stderr)
    candidates = scan_for_heroes(pm)
    print(f"[v2] {len(candidates)} candidates", file=sys.stderr)

    hero = find_local_hero(pm, candidates)
    if hero is None:
        print("[v2] no local hero — exit", file=sys.stderr)
        return 1
    print(f"[v2] local Hero @ 0x{hero:016x}")

    # Strip the local hero from the candidate list — the other Units (foes
    # and remote heroes) are what we want to inspect for received damage.
    others = [c[0] for c in candidates if c[0] != hero]
    print(f"[v2] watching {len(others)} other Units")

    seen_dr: set[int] = set()
    poll_count = 0
    while True:
        poll_count += 1
        ours_this_poll = 0
        total_units_with_dmg = 0
        for unit_addr in others:
            dmg = walk_history(pm, unit_addr, OFF_COMBAT_DAMAGE_HISTORY)
            if dmg:
                total_units_with_dmg += 1
            for dr, row in dmg:
                if dr in seen_dr:
                    continue
                seen_dr.add(dr)
                src_is_me = (row["source"] == hero)
                tag = "** MINE **" if src_is_me else "  other  "
                print(f"  {tag} tgt=0x{unit_addr:012x} dr=0x{dr:012x} "
                      f"amount={row['amount']:8.1f}  hits={row['hits']:>2}  "
                      f"crit={row['crit']}  kill={row['kill']}  "
                      f"src=0x{row['source']:x}")
                if src_is_me:
                    ours_this_poll += 1
        if poll_count % 4 == 0:
            print(f"  [poll {poll_count}] {total_units_with_dmg}/{len(others)} units have history; "
                  f"seen total drs={len(seen_dr)}")
        time.sleep(0.5)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[v2] stopped.")

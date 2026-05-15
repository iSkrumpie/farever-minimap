"""Probe v3: focus only on the local Hero, walk all three plausible
damage sources, and additionally dump the network combat chain.

Sources tried:
  A) Hero +664 combat : hxbit.ArrayProxyData -> +40 array : ArrayDyn
                       -> +8 array : ArrayObj -> length/varray
  B) Hero +856 combatDamageHistory : ArrayObj
  C) Hero +864 combatDamages       : ArrayObj

For (A), we don't yet know whether the elements are DamageResult
pointers or hxbit-replicated proxies. The probe just prints the first
few raw quadwords so we can decode them by eye.
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

OFF_OWNERPLAYER = 16
OFF_HERO_IN_PLAYER = 272
OFF_ISME = 280

OFF_COMBAT_PROXY     = 664   # ArrayProxyData
OFF_COMBAT_HISTORY   = 856   # ArrayObj  ("combatDamageHistory")
OFF_COMBAT_DAMAGES   = 864   # ArrayObj  ("combatDamages")

OFF_IS_IN_COMBAT     = 672
OFF_COMBAT_ID        = 712
OFF_LAST_DAMAGE_TIME = 848
OFF_COMBAT_START     = 688

# ArrayProxyData
OFF_PROXY_ARRAY      = 40    # -> ArrayDyn
# ArrayDyn
OFF_DYN_ARRAY        = 8     # -> ArrayBase (concretely ArrayObj usually)
# ArrayObj / ArrayBase
OFF_ARR_LENGTH       = 8
OFF_ARR_VARRAY       = 16
VARRAY_ELEMENTS_START = 24

# DamageResult
OFF_DR_AMOUNT    = 80
OFF_DR_HITCOUNT  = 88
OFF_DR_KILL      = 104
OFF_DR_CRITICAL  = 105
OFF_DR_SOURCE    = 24
OFF_DR_TARGET    = 40
OFF_DR_SKILL     = 8


def u8 (pm, a): return pm.read_bytes(a, 1)[0]
def i32(pm, a): return struct.unpack("<i", pm.read_bytes(a, 4))[0]
def u64(pm, a): return struct.unpack("<Q", pm.read_bytes(a, 8))[0]
def f64(pm, a): return struct.unpack("<d", pm.read_bytes(a, 8))[0]

def safe(fn, *args, default=None):
    try: return fn(*args)
    except Exception: return default


def find_local_hero(pm):
    for addr, _, _ in scan_for_heroes(pm):
        owner = safe(u64, pm, addr + OFF_OWNERPLAYER, default=0)
        if not owner: continue
        if safe(u64, pm, owner + OFF_HERO_IN_PLAYER, default=0) != addr: continue
        if safe(u8, pm, owner + OFF_ISME, default=0) == 1:
            return addr
    return None


def walk_array_obj(pm, arr_ptr, max_elements=20):
    """Walk an ArrayObj-like struct: read length and varray, return list
    of element pointers (HOBJ array)."""
    if not arr_ptr:
        return None, []
    length = safe(i32, pm, arr_ptr + OFF_ARR_LENGTH, default=-1)
    if length < 0 or length > 100000:
        return None, []
    varray = safe(u64, pm, arr_ptr + OFF_ARR_VARRAY, default=0)
    if not varray:
        return length, []
    out = []
    for i in range(min(length, max_elements)):
        ptr = safe(u64, pm, varray + VARRAY_ELEMENTS_START + i * 8, default=0)
        out.append(ptr)
    return length, out


def dump_damage_result(pm, dr):
    if not dr:
        return
    amt = safe(f64, pm, dr + OFF_DR_AMOUNT, default=0.0)
    hits = safe(i32, pm, dr + OFF_DR_HITCOUNT, default=0)
    crit = safe(u8, pm, dr + OFF_DR_CRITICAL, default=0)
    kill = safe(u8, pm, dr + OFF_DR_KILL, default=0)
    src  = safe(u64, pm, dr + OFF_DR_SOURCE, default=0)
    tgt  = safe(u64, pm, dr + OFF_DR_TARGET, default=0)
    skl  = safe(u64, pm, dr + OFF_DR_SKILL, default=0)
    print(f"      DR @ 0x{dr:x}: amount={amt:.1f} hits={hits} crit={crit} kill={kill} "
          f"src=0x{src:x} tgt=0x{tgt:x} skill=0x{skl:x}")


def main():
    pm, _ = attach()
    print("[v3] scanning for Hero...", file=sys.stderr)
    hero = find_local_hero(pm)
    if hero is None:
        print("[v3] no Hero", file=sys.stderr)
        return 1
    print(f"[v3] Hero @ 0x{hero:016x}")

    while True:
        is_in   = safe(u8,  pm, hero + OFF_IS_IN_COMBAT, default=0)
        cid     = safe(i32, pm, hero + OFF_COMBAT_ID, default=0)
        ldt     = safe(f64, pm, hero + OFF_LAST_DAMAGE_TIME, default=0.0)
        cstart  = safe(f64, pm, hero + OFF_COMBAT_START, default=0.0)
        print(f"\n[poll] isInCombat={is_in}  combatId={cid}  lastDmgT={ldt:.2f}  cStart={cstart:.2f}")

        # ---- (A) combat : ArrayProxyData -> ArrayDyn -> ArrayBase
        proxy = safe(u64, pm, hero + OFF_COMBAT_PROXY, default=0)
        print(f"  (A) combat@664 ArrayProxyData=0x{proxy:x}")
        if proxy:
            dyn = safe(u64, pm, proxy + OFF_PROXY_ARRAY, default=0)
            print(f"      .array=0x{dyn:x}  (ArrayDyn)")
            if dyn:
                inner = safe(u64, pm, dyn + OFF_DYN_ARRAY, default=0)
                print(f"      .array=0x{inner:x}  (ArrayBase / ArrayObj)")
                if inner:
                    length, elems = walk_array_obj(pm, inner)
                    print(f"      length={length}  first elems={[hex(p) for p in elems[:8]]}")
                    for p in elems[:5]:
                        dump_damage_result(pm, p)

        # ---- (B) combatDamageHistory
        h = safe(u64, pm, hero + OFF_COMBAT_HISTORY, default=0)
        print(f"  (B) combatDamageHistory@856 ArrayObj=0x{h:x}")
        if h:
            length, elems = walk_array_obj(pm, h)
            print(f"      length={length}  first elems={[hex(p) for p in elems[:8]]}")
            for p in elems[:5]:
                dump_damage_result(pm, p)

        # ---- (C) combatDamages
        c = safe(u64, pm, hero + OFF_COMBAT_DAMAGES, default=0)
        print(f"  (C) combatDamages@864 ArrayObj=0x{c:x}")
        if c:
            length, elems = walk_array_obj(pm, c)
            print(f"      length={length}  first elems={[hex(p) for p in elems[:8]]}")
            for p in elems[:5]:
                dump_damage_result(pm, p)

        time.sleep(1.0)


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: print("\n[v3] stopped.")

"""Anchor on the class name: find the canonical hl_type for a Haxe
class by searching the heap for its UTF-16 name string, then chasing
back through hl_type_obj.name and hl_type.obj.

Steps:
  1. Find UTF-16 string "ui.comp.DamageDisplay\0" in any committed,
     readable region. Call its address NAME_ADDR.
  2. Find a u64 in memory equal to NAME_ADDR. That location is the
     `name` field of some hl_type_obj struct, so obj_ptr = addr - 16.
  3. Find a u64 in memory equal to obj_ptr. That location is the `obj`
     field (the union) of an hl_type, so type_ptr = addr - 8.
  4. Verify type_ptr's kind == 11 (OBJ).

The result, type_ptr, is THE canonical type tag for the class. Every
live instance of that class has *its first 8 bytes equal to type_ptr*.
That gives us a perfect fingerprint: a single 8-byte ptr-match.

Usage:
    python find_type_by_name.py ui.comp.DamageDisplay
    python find_type_by_name.py st.skill.DamageResult
    python find_type_by_name.py ui.comp.UnitEffectDisplay
"""
from __future__ import annotations
import struct
import sys
import time
from pathlib import Path

import numpy as np

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from probe import attach, iter_committed_regions   # type: ignore[import-not-found]


HEAP_LO = 0x0000000100000000
HEAP_HI = 0x00007FFFFFFFFFFF


def utf16_bytes(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\x00\x00"


def find_byte_sequence(pm, needle: bytes) -> list[int]:
    """Return all addresses where `needle` is found in committed regions."""
    hits = []
    for base, sz, _prot in iter_committed_regions(pm):
        if sz < len(needle) or sz > (1 << 30):
            continue
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        # Scan with bytes.find loop (fast in C).
        i = 0
        while True:
            j = data.find(needle, i)
            if j < 0:
                break
            hits.append(base + j)
            i = j + 1
    return hits


def find_u64(pm, target: int) -> list[int]:
    """Return all 8-aligned addresses whose stored u64 == target."""
    hits = []
    tgt = np.uint64(target)
    for base, sz, _prot in iter_committed_regions(pm):
        if sz < 8 or sz > (1 << 30):
            continue
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        n8 = len(data) // 8
        if n8 == 0: continue
        arr = np.frombuffer(data, dtype=np.uint64, count=n8)
        idxs = np.flatnonzero(arr == tgt).tolist()
        for j in idxs:
            hits.append(base + j * 8)
    return hits


def main():
    if len(sys.argv) < 2:
        print("usage: find_type_by_name.py <Class.Name>", file=sys.stderr)
        return 2
    name = sys.argv[1]
    print(f"[anchor] looking for '{name}'...")
    pm, _ = attach()

    # 1) UTF-16 string search
    needle = utf16_bytes(name)
    t0 = time.perf_counter()
    name_hits = find_byte_sequence(pm, needle)
    print(f"  string '{name}' found {len(name_hits)} times in {time.perf_counter()-t0:.1f}s")
    for h in name_hits[:8]:
        print(f"    @0x{h:016x}")
    if not name_hits:
        return 1

    # 2) For each candidate string, find ptr-refs to it
    type_tags = []
    for sidx, name_addr in enumerate(name_hits):
        t0 = time.perf_counter()
        ref_hits = find_u64(pm, name_addr)
        # Filter: should be at offset 16 of an hl_type_obj
        for r in ref_hits:
            obj_ptr = r - 16
            # 3) find ptr-refs to obj_ptr (those are hl_type.obj fields)
            type_refs = find_u64(pm, obj_ptr)
            for tr in type_refs:
                tp = tr - 8
                # verify kind == OBJ (11)
                try:
                    kind = struct.unpack("<I", pm.read_bytes(tp, 4))[0]
                except Exception:
                    continue
                if kind == 11:
                    type_tags.append((tp, name_addr, obj_ptr))
        if name_hits and sidx == 0:
            print(f"  string ref-walk: {time.perf_counter()-t0:.1f}s, "
                  f"{len(type_tags)} canonical type tags found so far")

    if not type_tags:
        print("  no canonical hl_type found referencing those name strings")
        return 1

    # Show all unique type tags (should be exactly one in normal builds)
    seen = set()
    print(f"\n=== canonical hl_type for '{name}' ===")
    for tp, name_addr, obj_ptr in type_tags:
        if tp in seen: continue
        seen.add(tp)
        print(f"  hl_type* = 0x{tp:016x}   (obj=0x{obj_ptr:016x}, name@0x{name_addr:016x})")


if __name__ == "__main__":
    sys.exit(main())

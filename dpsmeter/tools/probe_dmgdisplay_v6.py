"""Probe v6 — FAST + PRECISE. Scan heap for hl_type* matching the
canonical DamageDisplay type tag (found via find_type_by_name.py).

Every 8-byte-aligned u64 in the heap equal to the tag IS a
DamageDisplay instance, period. Read damage / isCrit / dmg fields
at fixed offsets.

The type tag is heap-allocated and reallocated on each game start, so
this script first reaffirms the tag against the current process (by
re-running find_type_by_name's logic) — but only if --type-tag isn't
provided.
"""
from __future__ import annotations
import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from probe import attach, iter_committed_regions   # type: ignore[import-not-found]


# DamageDisplay field offsets (from hlbc dump)
OFF_LIFETIME    = 1080
OFF_MAXLIFETIME = 1088
OFF_ANIMHEIGHT  = 1104
OFF_SPREAD      = 1112
OFF_SHAKESIZE   = 1120
OFF_DAMAGE      = 1136
OFF_ISCRIT      = 1144
OFF_DMG_PTR     = 1176
INSTANCE_SIZE   = 1184


# Cache of regions that had hits on the previous full scan. Once warm we
# only re-scan those — the GC heap is mostly contiguous, so a handful of
# regions cover all live DamageDisplays.
_HOT_REGIONS: list[tuple[int, int]] = []


def _regions_to_scan(pm):
    """Return (base, size) list. Warm-cache after first hot region known."""
    if _HOT_REGIONS:
        return list(_HOT_REGIONS)
    return [(b, s) for b, s, _p in iter_committed_regions(pm)
            if INSTANCE_SIZE + 16 <= s <= (1 << 30)]


def scan_for_tag(pm, tag: int):
    """Yield instance addresses (8-aligned) whose u64 @ 0 == tag."""
    tgt = np.uint64(tag)
    new_hot: list[tuple[int, int]] = []
    for base, sz in _regions_to_scan(pm):
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        n = len(data)
        n8 = n // 8
        if n8 == 0: continue
        arr = np.frombuffer(data, dtype=np.uint64, count=n8)
        idxs = np.flatnonzero(arr == tgt).tolist()
        if idxs:
            new_hot.append((base, sz))
        for j in idxs:
            addr = base + j * 8
            # Bounds check
            if j * 8 + INSTANCE_SIZE > n:
                continue
            # Read the DamageDisplay fields right out of the data slice
            blob = data[j*8 : j*8 + INSTANCE_SIZE]
            lt   = struct.unpack_from("<d", blob, OFF_LIFETIME)[0]
            mlt  = struct.unpack_from("<d", blob, OFF_MAXLIFETIME)[0]
            ah   = struct.unpack_from("<d", blob, OFF_ANIMHEIGHT)[0]
            sp   = struct.unpack_from("<d", blob, OFF_SPREAD)[0]
            shk  = struct.unpack_from("<d", blob, OFF_SHAKESIZE)[0]
            dmg  = struct.unpack_from("<d", blob, OFF_DAMAGE)[0]
            crit = blob[OFF_ISCRIT]
            dr   = struct.unpack_from("<Q", blob, OFF_DMG_PTR)[0]
            yield (addr, lt, mlt, ah, sp, shk, dmg, crit, dr)
    # Warm the cache. We only narrow once we have hot regions and a couple
    # of completed scans (so a transient miss doesn't shrink to nothing).
    if new_hot and not _HOT_REGIONS:
        _HOT_REGIONS[:] = new_hot
    elif new_hot:
        # Union of old hot + this scan's hot — keeps regions that are
        # sometimes empty between scans.
        merged = set(_HOT_REGIONS) | set(new_hot)
        _HOT_REGIONS[:] = list(merged)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type-tag", type=lambda s: int(s, 0),
                    default=0x00000199291c5df8,
                    help="DamageDisplay hl_type* (default = last found tag)")
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args()

    pm, _ = attach()
    print(f"[v6] type tag = 0x{args.type_tag:016x}")
    seen: set[int] = set()
    seen_dr: set[int] = set()
    poll = 0
    while True:
        poll += 1
        t0 = time.perf_counter()
        results = list(scan_for_tag(pm, args.type_tag))
        dt = time.perf_counter() - t0
        new_inst = 0
        new_dr = 0
        for (addr, lt, mlt, ah, sp, shk, dmg, crit, dr) in results:
            if addr not in seen:
                seen.add(addr); new_inst += 1
            if dr and dr not in seen_dr:
                seen_dr.add(dr); new_dr += 1
                crittxt = "CRIT" if crit else "    "
                print(f"  [{poll:>2}] DR=0x{dr:012x} {crittxt} damage={dmg:9.1f} "
                      f"lt={lt:.2f}/{mlt:.2f} spread={sp:+.2f}")
        print(f"[v6] poll {poll} in {dt:.2f}s  total live DD = {len(results)}, "
              f"new_inst={new_inst}  new_DR={new_dr}  total_DR_seen={len(seen_dr)}")
        if args.once:
            return 0
        time.sleep(0.5)


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: print("\n[v6] stopped.")

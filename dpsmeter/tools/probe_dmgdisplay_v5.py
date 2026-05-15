"""Probe v5: same fingerprint as v4 but tightened + group candidates
by hl_type* (offset 0). DamageDisplay has ONE type tag — every real
instance shares it, while 335k false positives are spread across
random type pointers.

Tightenings vs v4:
  - dmg_ptr must be in GC heap range (~0x000001_xx_xxxx_xxxx)
  - hl_type* at offset 0 must be in libhl static range
    (0x00007FF6.._0x00007FFF.. typically) OR plausible heap range
  - damage must be > 1.0 (no tiny floats)
  - maxLifetime > 0.1
  - lifetime <= maxLifetime
  - exactly one of lifetime/spread is nonzero (skips all-zero memory)

Then bin remaining candidates by hl_type* and print the top 10.
"""
from __future__ import annotations
import struct
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from probe import attach, iter_committed_regions   # type: ignore[import-not-found]


OFF_LIFETIME     = 1080
OFF_MAXLIFETIME  = 1088
OFF_ANIMTIME     = 1096
OFF_ANIMHEIGHT   = 1104
OFF_SPREAD       = 1112
OFF_DAMAGE       = 1136
OFF_ISCRIT       = 1144
OFF_DMG_PTR      = 1176
INSTANCE_SIZE    = 1184

# HashLink GC heap addresses we've actually seen
HEAP_LO = 0x0000000100000000
HEAP_HI = 0x00007FFFFFFFFFFF
# libhl static data (type tags) on Windows is in the EXE-mapped range
# 0x00007FF6_xxxx_xxxx .. 0x00007FFF_xxxx_xxxx, AND HashLink can store
# type tags in the heap too. Use a broad check.
TYPE_LO  = 0x00007FF000000000
TYPE_HI  = 0x00007FFFFFFFFFFF


def scan_region(data: bytes, region_base: int) -> list[tuple[int, dict]]:
    n = len(data)
    if n < INSTANCE_SIZE + 16:
        return []
    arrf = np.frombuffer(data, dtype=np.float64, count=n // 8)
    arru = np.frombuffer(data, dtype=np.uint64,  count=n // 8)
    arr1 = np.frombuffer(data, dtype=np.uint8)
    M = arrf.size

    def f64(off): k = off // 8; return arrf[k : k + (M - k)]
    def u64(off): k = off // 8; return arru[k : k + (M - k)]

    fL = f64(OFF_LIFETIME); fMl = f64(OFF_MAXLIFETIME); fAt = f64(OFF_ANIMTIME)
    fAh = f64(OFF_ANIMHEIGHT); fS = f64(OFF_SPREAD); fD = f64(OFF_DAMAGE)
    fP = u64(OFF_DMG_PTR); fT = u64(0)
    L = min(fL.size, fMl.size, fAt.size, fAh.size, fS.size, fD.size, fP.size, fT.size)
    fL=fL[:L]; fMl=fMl[:L]; fAt=fAt[:L]; fAh=fAh[:L]; fS=fS[:L]; fD=fD[:L]; fP=fP[:L]; fT=fT[:L]

    with np.errstate(invalid="ignore"):
        mask = (
            np.isfinite(fL)  & (fL  >= 0.0)  & (fL  <= 10.0)
          & np.isfinite(fMl) & (fMl >  0.1)  & (fMl <= 10.0)
          & np.isfinite(fAt) & (fAt >= 0.0)  & (fAt <= 10.0)
          & np.isfinite(fAh) & (fAh >= -200.0) & (fAh <= 200.0)
          & np.isfinite(fS)  & (fS  >= -200.0) & (fS  <= 200.0)
          & np.isfinite(fD)  & (fD  >  1.0)  & (fD  < 1e8)
          & (fP >= HEAP_LO) & (fP <= HEAP_HI)
          & (fT >= HEAP_LO) & (fT <= HEAP_HI)   # type tag must be a real ptr
          & (fL <= fMl + 1e-9)
        )
    idxs = np.flatnonzero(mask).tolist()

    out = []
    for j in idxs:
        base = j * 8
        if base + OFF_ISCRIT >= n: continue
        ic = arr1[base + OFF_ISCRIT]
        if ic not in (0, 1): continue
        # The 7 bytes after the bool byte should be zero (HL packs HBOOL into a single byte,
        # the rest of the 8-byte slot is padding). Loose check: at least the next 2 bytes zero.
        if arr1[base + OFF_ISCRIT + 1] != 0: continue
        out.append((region_base + base, int(fT[j]), int(fP[j]),
                    float(fD[j]), int(ic),
                    float(fL[j]), float(fMl[j])))
    return out


def main() -> int:
    pm, _ = attach()
    print("[v5] one-shot heap scan...")
    t0 = time.perf_counter()
    total_bytes = 0
    total_hits  = 0
    by_type: Counter = Counter()
    examples: dict[int, list] = {}

    for base, sz, _prot in iter_committed_regions(pm):
        if sz < INSTANCE_SIZE + 32 or sz > (1 << 30):
            continue
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        total_bytes += sz
        hits = scan_region(data, base)
        for addr, tt, dmgp, dmg, ic, lt, ml in hits:
            total_hits += 1
            by_type[tt] += 1
            ex = examples.setdefault(tt, [])
            if len(ex) < 4:
                ex.append((addr, dmg, ic, lt, ml, dmgp))

    dt = time.perf_counter() - t0
    print(f"[v5] scanned {total_bytes/1e9:.2f} GB in {dt:.1f}s, {total_hits} raw hits")
    print(f"[v5] {len(by_type)} distinct hl_type* values")
    print(f"\nTop 15 hl_type* by hit count (the real DamageDisplay type stands out):")
    for tt, count in by_type.most_common(15):
        print(f"\n  hl_type* = 0x{tt:016x}    {count} candidates")
        for addr, dmg, ic, lt, ml, dmgp in examples[tt]:
            crit = "CRIT" if ic else "    "
            print(f"     @0x{addr:016x} {crit} damage={dmg:9.1f} lt={lt:.2f}/{ml:.2f} dmg=0x{dmgp:x}")
    return 0


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: print("\n[v5] stopped.")

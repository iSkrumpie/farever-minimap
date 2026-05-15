"""Probe v4: scan the heap for ui.comp.DamageDisplay instances by
fingerprint. These UI elements are created for every floating damage
number, and the user only sees their own damage, so each match IS one
of our damage events.

Fingerprint at 8-aligned offsets within a region:
  +1080  lifetime    HF64  finite, in [0, 10]
  +1088  maxLifetime HF64  finite, in (0.05, 10]
  +1112  spread      HF64  finite, in [-50, 50]
  +1136  damage      HF64  finite, in (0, 1e7)
  +1144  isCrit      HBOOL byte in {0,1}, next 7 bytes zero (HL packs bools)
  +1176  dmg         HOBJ  userland pointer (8 bytes)
  +1184  HF64 fields (further) finite

Strategy: numpy-vectorise over a region, find candidate base offsets,
then bulk-read fields and check.
"""
from __future__ import annotations
import math
import struct
import sys
import time
from pathlib import Path

import numpy as np

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from probe import attach, iter_committed_regions   # type: ignore[import-not-found]


OFF_LIFETIME     = 1080
OFF_MAXLIFETIME  = 1088
OFF_SPREAD       = 1112
OFF_DAMAGE       = 1136
OFF_ISCRIT       = 1144
OFF_DMG_PTR      = 1176
INSTANCE_SIZE    = 1184


def is_userland_ptr(v: int) -> bool:
    return 0x0000000100000000 <= v <= 0x00007FFFFFFFFFFF


def scan_region(data: bytes) -> list[tuple[int, dict]]:
    """Return list of (offset_in_region, fields) candidates."""
    n = len(data)
    if n < INSTANCE_SIZE + 8:
        return []
    arrf = np.frombuffer(data, dtype=np.float64, count=n // 8)
    arru = np.frombuffer(data, dtype=np.uint64,  count=n // 8)
    arr1 = np.frombuffer(data, dtype=np.uint8)
    M = arrf.size

    def fld_f64(byte_off):
        k = byte_off // 8
        return arrf[k : k + (M - k)]

    def fld_u64(byte_off):
        k = byte_off // 8
        return arru[k : k + (M - k)]

    # Build slices for the 4 HF64 fields. All slices must be the same length
    # (the slice for the highest offset is shortest), so trim.
    fL  = fld_f64(OFF_LIFETIME)
    fMl = fld_f64(OFF_MAXLIFETIME)
    fS  = fld_f64(OFF_SPREAD)
    fD  = fld_f64(OFF_DAMAGE)
    fP  = fld_u64(OFF_DMG_PTR)
    fLast = fld_f64(INSTANCE_SIZE - 8)
    L = min(fL.size, fMl.size, fS.size, fD.size, fP.size, fLast.size)
    fL = fL[:L]; fMl = fMl[:L]; fS = fS[:L]; fD = fD[:L]; fP = fP[:L]

    with np.errstate(invalid="ignore"):
        mask = (
            np.isfinite(fL)  & (fL  >= 0.0)   & (fL  <= 10.0)
          & np.isfinite(fMl) & (fMl >  0.05)  & (fMl <= 10.0)
          & np.isfinite(fS)  & (fS  >= -50.0) & (fS  <=  50.0)
          & np.isfinite(fD)  & (fD  >   0.0)  & (fD  <  1e7)
          & (fP >= 0x0000000100000000) & (fP <= 0x00007FFFFFFFFFFF)
          & (fL <= fMl + 1e-9)
        )
    idxs = np.flatnonzero(mask)
    out = []
    for j in idxs.tolist():
        candidate_off_in_region = j * 8  # since slice starts at element j of arrf
        # The 'j' indexing is already shifted by OFF_LIFETIME//8.  We need to
        # map back: candidate offset of instance start = j*8.
        base = j * 8
        # isCrit byte must be in {0,1}
        if base + OFF_ISCRIT >= n: continue
        ic = arr1[base + OFF_ISCRIT]
        if ic not in (0, 1): continue
        # next 7 bytes after the bool should be padding (HL packs bools as bytes,
        # but the surrounding bytes are likely zero or another small u8). Keep it loose.
        out.append((base, {
            "lifetime":    float(fL[j]),
            "maxLifetime": float(fMl[j]),
            "spread":      float(fS[j]),
            "damage":      float(fD[j]),
            "isCrit":      int(ic),
            "dmg_ptr":     int(fP[j]),
        }))
    return out


def main() -> int:
    pm, _ = attach()
    print("[dd] starting heap scan for DamageDisplay fingerprint...")
    print("[dd] keep dealing damage; rescan every poll (slow)")
    poll = 0
    seen: dict[int, dict] = {}   # addr -> first-seen fields

    while True:
        poll += 1
        t0 = time.perf_counter()
        n_regions = 0
        bytes_scanned = 0
        n_hits_this_poll = 0
        for base, sz, _prot in iter_committed_regions(pm):
            # Skip giant regions (likely file mappings) and tiny ones
            if sz < INSTANCE_SIZE + 32 or sz > (1 << 30):
                continue
            try:
                data = pm.read_bytes(base, sz)
            except Exception:
                continue
            n_regions += 1
            bytes_scanned += sz
            for rel, fields in scan_region(data):
                addr = base + rel
                if addr in seen:
                    continue
                seen[addr] = fields
                n_hits_this_poll += 1
                crit = "CRIT" if fields["isCrit"] else "    "
                print(f"  [+{poll:>2}] 0x{addr:016x} {crit} damage={fields['damage']:8.1f} "
                      f"lt={fields['lifetime']:.2f}/{fields['maxLifetime']:.2f} "
                      f"spread={fields['spread']:+.2f} dmg=0x{fields['dmg_ptr']:x}")
        dt = time.perf_counter() - t0
        print(f"[dd] poll {poll}: scanned {bytes_scanned/1e9:.2f} GB in {n_regions} regions "
              f"in {dt:.1f}s  new={n_hits_this_poll}  total_seen={len(seen)}")
        # No sleep — scan itself is the rate limiter.


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: print("\n[dd] stopped.")

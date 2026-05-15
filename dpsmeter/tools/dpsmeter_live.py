"""DPS Meter — Python prototype. Live per-skill damage breakdown.

End-to-end:
  1. Attach to Farever.exe.
  2. Lock the local Hero (for combatStartTime / combatId).
  3. Anchor the canonical hl_type* for ui.comp.DamageDisplay by UTF-16
     name search if not provided.
  4. Hot-cached heap scan ~10Hz, find new DamageResult pointers.
  5. For each new DR, decode skill name via DR.baseSkill.kind.
  6. Aggregate by skill, reset on combatId change.
  7. Pretty-print a live table.

Run:
    python dpsmeter_live.py
Or, after we've found the tag this session, skip the slow anchor:
    python dpsmeter_live.py --type-tag 0x199291c5df8
"""
from __future__ import annotations
import argparse
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

PROJECT_TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(PROJECT_TOOLS))
from find_hero import scan_for_heroes                # type: ignore[import-not-found]
from probe import attach, iter_committed_regions     # type: ignore[import-not-found]


# ---- offsets ----
OFF_OWNERPLAYER     = 16
OFF_HERO_IN_PLAYER  = 272
OFF_ISME            = 280
OFF_COMBAT_START    = 688
OFF_COMBAT_END      = 696
OFF_COMBAT_ID       = 712
OFF_IS_IN_COMBAT    = 672

# DamageDisplay (1184)
OFF_DD_LT       = 1080
OFF_DD_MAXLT    = 1088
OFF_DD_DAMAGE   = 1136
OFF_DD_ISCRIT   = 1144
OFF_DD_DMG_PTR  = 1176
DD_SIZE         = 1184

# DamageResult (136)
OFF_DR_BASESKILL = 8
OFF_DR_TARGET    = 40
OFF_DR_AMOUNT    = 80
OFF_DR_HITCOUNT  = 88
OFF_DR_KILL      = 104
OFF_DR_CRITICAL  = 105

# Skill / BaseSkill: kind:String at +152
OFF_SKILL_KIND = 152

# Haxe String (24): bytes@+8, length@+16
OFF_STR_BYTES = 8
OFF_STR_LEN   = 16


# ---- helpers ----
def u8 (pm, a): return pm.read_bytes(a, 1)[0]
def u32(pm, a): return struct.unpack("<I", pm.read_bytes(a, 4))[0]
def i32(pm, a): return struct.unpack("<i", pm.read_bytes(a, 4))[0]
def u64(pm, a): return struct.unpack("<Q", pm.read_bytes(a, 8))[0]
def f64(pm, a): return struct.unpack("<d", pm.read_bytes(a, 8))[0]


def safe(fn, *a, default=None):
    try: return fn(*a)
    except Exception: return default


def utf16_bytes(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\x00\x00"


# ---- type tag anchoring ----
def find_byte_sequence(pm, needle: bytes):
    hits = []
    for base, sz, _p in iter_committed_regions(pm):
        if sz < len(needle) or sz > (1 << 30): continue
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        i = 0
        while True:
            j = data.find(needle, i)
            if j < 0: break
            hits.append(base + j); i = j + 1
    return hits


def find_u64(pm, target: int):
    hits = []
    tgt = np.uint64(target)
    for base, sz, _p in iter_committed_regions(pm):
        if sz < 8 or sz > (1 << 30): continue
        try: data = pm.read_bytes(base, sz)
        except Exception: continue
        n8 = len(data) // 8
        if n8 == 0: continue
        arr = np.frombuffer(data, dtype=np.uint64, count=n8)
        for j in np.flatnonzero(arr == tgt).tolist():
            hits.append(base + j * 8)
    return hits


def anchor_type_tag(pm, class_name: str) -> int | None:
    print(f"[anchor] searching for '{class_name}' (one-time, slow)...")
    name_hits = find_byte_sequence(pm, utf16_bytes(class_name))
    if not name_hits:
        return None
    print(f"  name string at {[hex(h) for h in name_hits]}")
    for name_addr in name_hits:
        for r in find_u64(pm, name_addr):
            obj_ptr = r - 16
            for tr in find_u64(pm, obj_ptr):
                tp = tr - 8
                if safe(u32, pm, tp, default=0) == 11:
                    return tp
    return None


# ---- Hero locking ----
def find_local_hero(pm) -> int | None:
    for addr, _, _ in scan_for_heroes(pm):
        owner = safe(u64, pm, addr + OFF_OWNERPLAYER, default=0)
        if not owner: continue
        if safe(u64, pm, owner + OFF_HERO_IN_PLAYER, default=0) != addr: continue
        if safe(u8, pm, owner + OFF_ISME, default=0) == 1:
            return addr
    return None


# ---- DamageDisplay scan with hot-region cache ----
_HOT: list[tuple[int, int]] = []


def scan_dd(pm, tag: int):
    """Yield (dr_ptr, damage, isCrit) for each DamageDisplay found."""
    tgt = np.uint64(tag)
    new_hot: list[tuple[int, int]] = []
    regions = list(_HOT) if _HOT else [(b, s) for b, s, _ in iter_committed_regions(pm)
                                        if DD_SIZE + 16 <= s <= (1 << 30)]
    for base, sz in regions:
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
            off = j * 8
            if off + DD_SIZE > n: continue
            blob = data[off : off + DD_SIZE]
            dr   = struct.unpack_from("<Q", blob, OFF_DD_DMG_PTR)[0]
            if dr < 0x100000000: continue   # heap pointer check
            dmg  = struct.unpack_from("<d", blob, OFF_DD_DAMAGE)[0]
            if not (0 < dmg < 1e8): continue
            crit = blob[OFF_DD_ISCRIT]
            if crit not in (0, 1): continue
            yield dr, dmg, crit
    if new_hot and not _HOT:
        _HOT[:] = new_hot
    elif new_hot:
        merged = set(_HOT) | set(new_hot)
        _HOT[:] = list(merged)


# ---- String decoding ----
def read_haxe_string(pm, str_ptr: int) -> str:
    if not str_ptr or str_ptr < 0x100000000: return ""
    bytes_ptr = safe(u64, pm, str_ptr + OFF_STR_BYTES, default=0)
    length    = safe(i32, pm, str_ptr + OFF_STR_LEN,   default=0)
    if not bytes_ptr or length <= 0 or length > 256: return ""
    try:
        raw = pm.read_bytes(bytes_ptr, length * 2)
    except Exception:
        return ""
    try:
        return raw.decode("utf-16-le", errors="replace")
    except Exception:
        return ""


def read_skill_name(pm, dr_ptr: int) -> str:
    bs = safe(u64, pm, dr_ptr + OFF_DR_BASESKILL, default=0)
    if not bs: return "?"
    skind = safe(u64, pm, bs + OFF_SKILL_KIND, default=0)
    if not skind: return "?"
    return read_haxe_string(pm, skind) or "?"


# ---- aggregator ----
class Aggregator:
    def __init__(self):
        self.fight_id = -1
        self.fight_start = 0.0
        self.seen_dr: set[int] = set()
        self.by_skill: dict[str, dict] = {}
        self.start_wall = time.time()

    def reset(self, fight_id: int, fight_start: float):
        self.fight_id = fight_id
        self.fight_start = fight_start
        self.seen_dr = set()
        self.by_skill = {}
        self.start_wall = time.time()

    def record(self, dr_ptr: int, skill: str, dmg: float, hits: int, crit: bool, kill: bool):
        if dr_ptr in self.seen_dr:
            return
        self.seen_dr.add(dr_ptr)
        row = self.by_skill.setdefault(skill, {"count":0, "total":0.0, "max":0.0, "crits":0})
        row["count"] += max(1, hits)
        row["total"] += dmg
        row["max"]    = max(row["max"], dmg)
        if crit: row["crits"] += 1

    def render(self, fight_id: int, in_combat: bool, t_now: float) -> str:
        elapsed = max(0.001, t_now - self.start_wall)
        total = sum(r["total"] for r in self.by_skill.values())
        lines = []
        flag = "IN-COMBAT" if in_combat else "out"
        lines.append(f"=== Fight #{fight_id}  elapsed={elapsed:6.1f}s  total={total:11.1f}  DPS={total/elapsed:9.1f}  ({flag}) ===")
        lines.append(f"  {'Skill':<32}  {'Hits':>5} {'Total':>10} {'Max':>8} {'Crit%':>6} {'DPS':>9} {'%':>5}")
        sorted_skills = sorted(self.by_skill.items(), key=lambda kv: -kv[1]["total"])
        for name, r in sorted_skills:
            critpct = (100.0 * r["crits"] / max(1, r["count"]))
            pct = 100.0 * r["total"] / max(1.0, total)
            lines.append(f"  {name[:32]:<32}  {r['count']:>5} {r['total']:>10.1f} {r['max']:>8.1f} "
                         f"{critpct:>5.0f}% {r['total']/elapsed:>9.1f} {pct:>4.0f}%")
        return "\n".join(lines)


# ---- main loop ----
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--type-tag", type=lambda s: int(s, 0), default=None,
                    help="DamageDisplay hl_type*; skip slow anchoring if known")
    ap.add_argument("--interval", type=float, default=0.5)
    args = ap.parse_args()

    pm, _ = attach()
    hero = find_local_hero(pm)
    if hero is None:
        print("[live] no local Hero (enter the world first)", file=sys.stderr)
        return 1
    print(f"[live] Hero @ 0x{hero:016x}")

    tag = args.type_tag or anchor_type_tag(pm, "ui.comp.DamageDisplay")
    if tag is None:
        print("[live] could not anchor DamageDisplay type tag", file=sys.stderr)
        return 1
    print(f"[live] DamageDisplay hl_type* = 0x{tag:016x}")
    print(f"[live] subsequent runs:  python dpsmeter_live.py --type-tag 0x{tag:x}")

    agg = Aggregator()
    last_render = 0.0

    while True:
        t_loop = time.perf_counter()
        # Hero state
        in_combat = bool(safe(u8,  pm, hero + OFF_IS_IN_COMBAT, default=0))
        cid       =      safe(i32, pm, hero + OFF_COMBAT_ID,    default=0)
        cstart    =      safe(f64, pm, hero + OFF_COMBAT_START, default=0.0)
        if cid != agg.fight_id:
            if agg.by_skill:
                print("\n[live] fight ended — final breakdown:")
                print(agg.render(agg.fight_id, False, time.time()))
            agg.reset(cid, cstart)
            print(f"\n[live] new fight: combatId={cid}")

        # Scan DamageDisplays, record new DRs
        for dr_ptr, dmg, crit in scan_dd(pm, tag):
            if dr_ptr in agg.seen_dr: continue
            # Read DR meta
            hits = safe(i32, pm, dr_ptr + OFF_DR_HITCOUNT, default=1) or 1
            kill = safe(u8,  pm, dr_ptr + OFF_DR_KILL,     default=0) or 0
            skill = read_skill_name(pm, dr_ptr)
            # Reject garbage: real DR has hitCount in [1, 50] and a printable
            # ASCII skill name (Haxe skill kinds are like "DS_Bladeleaf_Skill1").
            if not (1 <= hits <= 50):  continue
            if not skill or not skill.replace("_", "").replace(".", "").isalnum():
                continue
            agg.record(dr_ptr, skill, dmg, hits, bool(crit), bool(kill))

        # Render every 0.5 s
        now = time.time()
        if now - last_render >= args.interval:
            os.system("cls" if os.name == "nt" else "clear")
            print(agg.render(agg.fight_id, in_combat, now))
            print(f"\n[live] scan_dd took {(time.perf_counter()-t_loop)*1000:.0f} ms, "
                  f"hot_regions={len(_HOT)}, unique DRs={len(agg.seen_dr)}")
            last_render = now

        time.sleep(args.interval)


if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: print("\n[live] stopped.")

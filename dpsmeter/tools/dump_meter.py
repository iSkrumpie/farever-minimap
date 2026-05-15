"""Dump full ui.hud.MeterLine + UnitCombatMeter layouts and find what
feeds them — the game has its own DPS UI we can read from."""
from __future__ import annotations
import json, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

TARGETS = [
    "ui.hud.MeterLine",
    "ui.hud.UnitCombatMeter",
    "ui.hud.CombatMeter",
    "ui.hud.FoeCombatInfo",
    "ui.comp.FmtText",
]

def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    for t in TARGETS:
        cls = raw.get(t)
        if cls is None:
            print(f"!! {t}: not found\n")
            continue
        print(f"=== {t} (size={cls['instance_size']}, super={cls.get('super')}) ===")
        for f in cls.get("fields", []):
            extra = f"  [{f.get('type_desc','')}]" if f["kind_name"] in ("HOBJ", "HENUM", "HVIRTUAL") else ""
            nm = f["name"] or "<anon>"
            print(f"  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")
        print()

    # Also: any class with a HF64 field literally named dps / totalDamage / DPS
    print("\n--- classes with HF64 named like dps/damage values ---")
    for name, cls in raw.items():
        for f in cls.get("fields", []):
            nm = (f.get("name") or "").lower()
            if f["kind_name"] in ("HF64", "HI32", "HI64") and (
                re.fullmatch(r"dps|maxdps|currentdps|damage|totaldamage|dmgtotal|totaldmg|sumdamage", nm)
                or re.search(r"dpsdealt|damagedealt|damagedone|totaldamage", nm)
            ):
                print(f"  {name} @{f['offset']:<5} {f['kind_name']:<6} {f['name']}")

if __name__ == "__main__":
    main()

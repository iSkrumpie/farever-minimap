"""Quick layout dump for ArrayObj / ArrayProxyData and friends — we need
to know how to walk the Hero.combatDamageHistory array."""
from __future__ import annotations
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

TARGETS = [
    "hl.types.ArrayObj",
    "hl.types.ArrayBase",
    "hxbit.ArrayProxyData",
    "hxbit.ArrayProxy",
    "st.skill.BaseSkillAccess",
    "st.skill.BaseSkill",
    "ent.GameObject",
    "ent.Unit",
    "st.Player",
]

def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    for t in TARGETS:
        cls = raw.get(t)
        if cls is None:
            print(f"!! {t}: not found")
            continue
        print(f"=== {t} (size={cls['instance_size']}, super={cls.get('super')}) ===")
        for f in cls.get("fields", []):
            extra = f"  [{f.get('type_desc','')}]" if f["kind_name"] in ("HOBJ", "HENUM", "HVIRTUAL") else ""
            nm = f["name"] or "<anon>"
            print(f"  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")
        print()

if __name__ == "__main__":
    main()

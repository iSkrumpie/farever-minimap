"""M0 phase 2: dump FULL field layouts for the most interesting combat
classes found by find_combat_classes.py. We need the exact byte offset
of every field so the runtime scanner knows where damage values live.
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

TARGETS = [
    "st.skill.DamageResult",
    "st.skill.HitResult",
    "st.skill.Skill",
    "ui.comp.DamageDisplay",
    "ui.hud.CombatMeter",
    "ui.hud.UnitCombatMeter",
    "ui.hud.FoeCombatInfo",
    "st.player.HeroStats",
    "ent.Hero",
]

# Resolve all super-chains too.
def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))

    def dump(name: str, depth: int = 0) -> None:
        cls = raw.get(name)
        prefix = "  " * depth
        if cls is None:
            print(f"{prefix}!! {name}: NOT FOUND")
            return
        print(f"{prefix}=== {name} (size={cls.get('instance_size')}, super={cls.get('super')}) ===")
        for f in cls.get("fields", []):
            extra = ""
            if f["kind_name"] in ("HOBJ", "HENUM", "HVIRTUAL"):
                extra = f"  [{f.get('type_desc','')}]"
            nm = f["name"] or "<anon>"
            print(f"{prefix}  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")

    for t in TARGETS:
        dump(t)
        print()


if __name__ == "__main__":
    main()

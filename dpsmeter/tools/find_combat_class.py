"""Look for st.skill.Combat / st.Combat / ent.Combat — a class that
plausibly represents one ongoing fight, fielded by Hero.combat@664
(ArrayProxyData with length=2 in the live game)."""
from __future__ import annotations
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"


def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))

    print("=== ALL classes whose leaf name == 'Combat' or 'CombatData' or 'CombatTarget' ===")
    for name, cls in raw.items():
        leaf = name.rsplit(".", 1)[-1]
        if leaf in ("Combat", "CombatData", "CombatTarget", "CombatEntry", "CombatRecord",
                    "CombatInfo", "DamageRecord", "DamageTally", "DamageAccum"):
            print(f"\n[{cls['instance_size']:>5}] {name}  (super={cls.get('super')})")
            for f in cls.get("fields", []):
                td = f.get("type_desc","")
                extra = f"  [{td}]" if td and f["kind_name"] in ("HOBJ","HENUM","HVIRTUAL") else ""
                nm = f["name"] or "<anon>"
                print(f"  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")

    print("\n\n=== ANY non-UI class whose leaf name contains 'Combat' or 'Damage' (size < 800) ===")
    for name, cls in raw.items():
        leaf = name.rsplit(".", 1)[-1].lower()
        sz = cls.get("instance_size", 0)
        if sz > 800 or sz == 0:
            continue
        if leaf.startswith("$"):  # type metadata, skip
            continue
        if "combat" in leaf or "damage" in leaf:
            if name.startswith("ui.") or name.startswith("domkit."):
                continue
            print(f"  [{sz:>4}] {name} (super={cls.get('super')})")

    print("\n\n=== fields of type ArrayProxyData on st.Player or ent.Hero (size < 400 element classes hinted) ===")
    # Print all fields mentioning hxbit.ArrayProxyData on Player/Unit/Hero-related classes
    for target in ("st.Player", "ent.Hero", "ent.Unit", "ent.GameObject"):
        cls = raw.get(target)
        if cls is None: continue
        print(f"\n  {target}:")
        for f in cls.get("fields", []):
            if "ArrayProxyData" in f.get("type_desc", "") or f.get("type_desc") == "HOBJ:hxbit.ArrayProxyData":
                print(f"    @{f['offset']:<4} {f['name']} -> {f.get('type_desc')}")


if __name__ == "__main__":
    main()

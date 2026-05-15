"""Dump ent.Foe + any sub-class we find. The combat@664 array on Hero
holds 2 pointers in the live probe, ~32 bytes apart -- likely Foe
instances. Maybe damage we deal is recorded on the Foe end."""
from __future__ import annotations
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

TARGETS = [
    "ent.Foe",
    "ent.Critter",
    "ent.NPC",
    "ent.Mob",
]

def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    # Find any ent.* class that extends ent.Unit
    print("=== All ent.* classes extending ent.Unit ===")
    for name, cls in raw.items():
        if name.startswith("ent.") and cls.get("super") == "ent.Unit":
            print(f"  [{cls['instance_size']:>5}] {name}")
    print()

    # Also classes that have a 'damage'-named field with a Hero ref
    print("\n=== classes with a Hero-typed field + a damage-like HF64 ===")
    for name, cls in raw.items():
        fs = cls.get("fields", [])
        has_hero = any(f["kind_name"]=="HOBJ" and "Hero" in (f.get("type_desc","") or "") for f in fs)
        dmgf = [f for f in fs if f["kind_name"] in ("HF64","HI32") and any(kw in (f["name"] or "").lower() for kw in ("damage","dmg","amount","total","hits","done"))]
        if has_hero and dmgf:
            print(f"  [{cls['instance_size']:>4}] {name}")
            for f in dmgf:
                print(f"     @{f['offset']:<4} {f['kind_name']:<6} {f['name']}")

    for t in TARGETS:
        cls = raw.get(t)
        if cls is None:
            continue
        print(f"\n=== {t} (size={cls['instance_size']}, super={cls.get('super')}) ===")
        # Show fields > offset 1100 (Unit-specific bits are above Hero+1080)
        for f in cls.get("fields", []):
            if f["offset"] < 1080:  # skip inherited Unit fields, dumped before
                continue
            td = f.get("type_desc","")
            extra = f"  [{td}]" if td and f["kind_name"] in ("HOBJ","HENUM","HVIRTUAL") else ""
            nm = f["name"] or "<anon>"
            print(f"  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")

if __name__ == "__main__":
    main()

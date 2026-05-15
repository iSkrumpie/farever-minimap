"""Hunt for the non-UI data source that feeds ui.hud.MeterLine.

MeterLine has the fields { count:HI32, amount:HF64, skill:String,
target:GameObject, hero:Hero } at offsets 1080+. The UI is probably
fed by a smaller, non-UI 'meter row' / 'damage tally' class that lives
in a persistent data structure on the Hero or Player even when the
window is closed."""
from __future__ import annotations
import json, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"


def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))

    # Pattern 1: small classes (size <= 256) that combine
    # an int 'count' and a float 'amount' field. That's the canonical
    # damage-tally shape.
    print("=== small classes with count:HI32 + amount:HF64 ===")
    for name, cls in raw.items():
        if cls.get("instance_size", 0) > 256:
            continue
        fnames = {(f["name"] or "").lower(): f for f in cls.get("fields", [])}
        if "count" in fnames and "amount" in fnames:
            c = fnames["count"]; a = fnames["amount"]
            if c["kind_name"] in ("HI32", "HI64") and a["kind_name"] == "HF64":
                print(f"  [{cls['instance_size']:>4}] {name}")
                for f in cls["fields"]:
                    nm = f["name"] or "<anon>"
                    td = f.get("type_desc", "")
                    extra = f"  [{td}]" if td and f["kind_name"] in ("HOBJ", "HENUM", "HVIRTUAL") else ""
                    print(f"     @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")
                print()

    # Pattern 2: classes whose NAME has 'Meter' / 'Tracker' / 'Record'.
    print("\n=== classes named *Meter*/*Tracker*/*Record*/*Tally* ===")
    rx = re.compile(r"meter|tracker|record|tally|history", re.I)
    for name, cls in raw.items():
        leaf = name.rsplit(".", 1)[-1]
        if rx.search(leaf) and cls.get("instance_size", 0) < 1000:
            print(f"  [{cls['instance_size']:>4}] {name} (super={cls.get('super')})")

    # Pattern 3: find classes that reference (HOBJ field of type)
    # 'st.skill.Skill' or 'ent.GameObject' or 'ent.Hero' together
    # with a HF64 that *looks* like damage.
    print("\n=== classes with hero:Hero + amount:HF64 or similar (non-UI) ===")
    for name, cls in raw.items():
        if cls.get("instance_size", 0) > 512:
            continue
        fs = cls.get("fields", [])
        has_hero = any(f["kind_name"] == "HOBJ" and "Hero" in (f.get("type_desc","") or "") for f in fs)
        has_amount = any(f["kind_name"] == "HF64" and (f["name"] or "").lower() in ("amount","damage","total","dmg","dps") for f in fs)
        if has_hero and has_amount:
            print(f"  [{cls['instance_size']:>4}] {name}")

if __name__ == "__main__":
    main()

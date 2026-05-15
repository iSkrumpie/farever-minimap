"""Need the exact layout of hl.types.ArrayDyn — it's the heterogeneous
typed array that hxbit ArrayProxyData wraps. Hero.combat at +664 is an
ArrayProxyData whose .array field (+40) is an ArrayDyn."""
from __future__ import annotations
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

TARGETS = [
    "hl.types.ArrayDyn",
    "hl.types.ArrayBase",
    "hl.types.ArrayAccess",
    "hl.types.ArrayBytes_Int",
    "hl.types.ArrayBytes_Float",
    "hxbit.ArrayProxyData",
    "hxbit.BaseProxy",
]

def main():
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    for t in TARGETS:
        cls = raw.get(t)
        if cls is None:
            print(f"!! {t}: not found\n"); continue
        print(f"=== {t} (size={cls['instance_size']}, super={cls.get('super')}) ===")
        for f in cls.get("fields", []):
            td = f.get("type_desc","")
            extra = f"  [{td}]" if td and f["kind_name"] in ("HOBJ","HENUM","HVIRTUAL") else ""
            nm = f["name"] or "<anon>"
            print(f"  @{f['offset']:<4} {f['kind_name']:<8} {nm}{extra}")
        print()

if __name__ == "__main__":
    main()

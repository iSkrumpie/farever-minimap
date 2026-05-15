"""Find String class layout — we need to read DR.baseSkill.kind:String
to attribute each damage event to a skill."""
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
raw = json.loads((ROOT / "tools" / "classes.json").read_text(encoding="utf-8"))
for t in ("String", "hl.types.String", "hl.types.Bytes"):
    cls = raw.get(t)
    if cls is None:
        print(f"!! {t}: not found"); continue
    print(f"=== {t} (size={cls['instance_size']}, super={cls.get('super')}) ===")
    for f in cls.get("fields", []):
        td = f.get("type_desc","")
        extra = f"  [{td}]" if td and f["kind_name"] in ("HOBJ","HENUM","HVIRTUAL") else ""
        print(f"  @{f['offset']:<4} {f['kind_name']:<8} {f['name'] or '<anon>'}{extra}")
    print()

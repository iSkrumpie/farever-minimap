"""Hero.combatDamageHistory stayed empty when the player took damage,
so it likely tracks damage received and is server-authoritative.

This script grep the bytecode class dump for *any* class field that
plausibly accumulates damage dealt by the local player:

  - field names containing  damage|dmg|dps|hits|crit  (substr, ci)
  - classes named           Score|Stats|Meter|Tracker|Combat

so we can hunt for a "totalDamageDealt"-style accumulator or a list
of recent hits that the client actually populates.
"""
from __future__ import annotations
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"

FIELD_RX = re.compile(r"dmg|damage|dps|hits|crit|attack|deal", re.I)
CLASS_RX = re.compile(r"score|stats|meter|tracker|combat|damage", re.I)


def main() -> int:
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    print("--- Classes whose NAME hints at combat/dmg tracking, with damage-named fields ---")
    for name, cls in raw.items():
        if not CLASS_RX.search(name.split(".")[-1]):
            continue
        fields = cls.get("fields", [])
        hits = [f for f in fields if f.get("name") and FIELD_RX.search(f["name"])]
        if not hits:
            continue
        size = cls.get("instance_size")
        print(f"\n[{size:>5}] {name}")
        for f in hits:
            nm = f["name"]
            kind = f["kind_name"]
            td = f.get("type_desc", "")
            extra = f"  [{td}]" if td and kind in ("HOBJ", "HENUM", "HVIRTUAL") else ""
            print(f"  @{f['offset']:<4} {kind:<8} {nm}{extra}")

    print("\n\n--- ANY field name containing 'dealt' / 'dpsDealt' / 'damageDealt' / 'totalDmg' ---")
    NEEDLE = re.compile(r"dealt|totaldmg|totaldamage|damagedone|dpsdealt|dealtotal", re.I)
    for name, cls in raw.items():
        for f in cls.get("fields", []):
            if f.get("name") and NEEDLE.search(f["name"]):
                print(f"  {name} @{f['offset']} {f['kind_name']} {f['name']}")

    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())

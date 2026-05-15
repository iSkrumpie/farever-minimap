"""M0 recon: scan hlbc_parse output for damage/combat class candidates.

Reads ../../tools/classes.json (9 125 classes), groups by name keyword,
prints field layout for each interesting class. Output goes to
research/combat_class_candidates.json so we can re-grep without parsing
the 33 MB JSON every time.

Usage:
    python tools/find_combat_classes.py
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLASSES = ROOT / "tools" / "classes.json"
OUT     = Path(__file__).resolve().parents[1] / "research" / "combat_class_candidates.json"

# Buckets keyed by display label -> regex over the dotted class name.
# Case-insensitive substring matches on the leaf (last segment) name.
BUCKETS: dict[str, str] = {
    "damage":     r"damage",
    "hit":        r"\bhit\b|hitfeedback|hitinfo|hitresult|hitevent",
    "combat":     r"combat",
    "floating":   r"floatingnumber|floatingtext|floatingdamage|popuptext|dmgpopup|dmgtext",
    "skill":      r"\bskill\b|\bspell\b|ability",
    "feedback":   r"feedback",
    "log":        r"\blog\b|history",
    "stat":       r"\bstat\b|stats|stathit",
    "attack":     r"attack",
    "fight":      r"fight",
    "wound":      r"wound",
    "death":      r"death|kill",
    "event":      r"event",   # too broad; filter post-hoc
}


def leaf(name: str) -> str:
    return name.rsplit(".", 1)[-1].lower()


def field_signature(cls: dict) -> str:
    fields = cls.get("fields", [])
    parts = []
    for f in fields:
        parts.append(f"{f['name']}:{f['kind_name']}@{f['offset']}")
    return ", ".join(parts)


def main() -> int:
    raw = json.loads(CLASSES.read_text(encoding="utf-8"))
    by_bucket: dict[str, list[dict]] = {b: [] for b in BUCKETS}
    for name, cls in raw.items():
        ln = leaf(name)
        for bucket, pattern in BUCKETS.items():
            if re.search(pattern, ln):
                by_bucket[bucket].append({
                    "name": name,
                    "instance_size": cls.get("instance_size"),
                    "super": cls.get("super"),
                    "fields": cls.get("fields", []),
                })

    # Cross-bucket dedupe: a class can land in multiple buckets, keep all.
    summary = {}
    for bucket, hits in by_bucket.items():
        # Sort by instance size (larger -> probably more useful)
        hits.sort(key=lambda c: c["instance_size"] or 0, reverse=True)
        summary[bucket] = hits

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(summary, indent=2, ensure_ascii=False))

    # Pretty print to stdout.
    for bucket in BUCKETS:
        hits = summary[bucket]
        if not hits:
            continue
        print(f"\n=== bucket: {bucket} ({len(hits)} hits) ===")
        for c in hits[:30]:
            sig = field_signature(c)
            if len(sig) > 240:
                sig = sig[:240] + "..."
            print(f"  [{c['instance_size']:>5}] {c['name']} :: {sig}")
        if len(hits) > 30:
            print(f"  ... and {len(hits) - 30} more")

    print(f"\nWrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

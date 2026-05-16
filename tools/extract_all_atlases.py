"""Extract every UI/icons/atlas_* PNG from res.pak to data/atlases/.

The DLL uses the path string embedded in each skill's gfx record
(e.g. "UI/icons/atlas_class_Mage_96PX.png") to locate the right
atlas at runtime, so we mirror the in-pak directory layout under
the live data folder + the release-staging copy.

Run:  python tools/extract_all_atlases.py
"""
from __future__ import annotations

import shutil
from pathlib import Path

from pak_extract import open_pak  # type: ignore[import-not-found]

GAME_DIR     = Path(r"D:\SteamLibrary\steamapps\common\Farever")
PROJ_DIR     = Path(r"D:\farevermod")
RES_PAK      = GAME_DIR / "res.pak"

WANTED_PREFIX = "UI/icons/atlas_"
TARGETS = [
    GAME_DIR / "data" / "atlases",
    PROJ_DIR / "release-staging" / "farever-minimap-dps-0.2" / "data" / "atlases",
]


def main() -> int:
    buf, entries, data_section_start = open_pak(RES_PAK)
    matches = [(p, sz, pos) for (p, sz, pos) in entries
               if p.startswith(WANTED_PREFIX) and p.endswith(".png")]
    matches.sort()

    print(f"found {len(matches)} atlas entries")
    payloads: list[tuple[str, bytes]] = []
    for p, sz, pos in matches:
        data = buf[data_section_start + pos : data_section_start + pos + sz]
        payloads.append((p, data))
        print(f"  {p}  {sz:>10,} bytes")

    for root in TARGETS:
        if not root.parent.parent.exists():
            print(f"!! skip {root} (parent missing)")
            continue
        for p, data in payloads:
            dst = root / p
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
        print(f"-> wrote {len(payloads)} files under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Copies the Trilogy executables next to Corruption_3 PC.exe, which starts them for an R3ME01 disc."""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DESTINATION = ROOT / "build/RM3E01_00_runtime_link_probe"
TITLES = {"R3ME01_FE": "Corruption_3 Trilogy Menu.exe", "R3ME01_MP3": "Corruption_3 Trilogy MP3.exe"}


def main() -> None:
    for title, name in TITLES.items():
        source = ROOT / f"titles/{title}/work/recomp/build/{title}_runtime/Corruption_3 PC.exe"
        if not source.exists():
            raise SystemExit(f"Not built yet: {source}")
        shutil.copyfile(source, DESTINATION / name)
        print(DESTINATION / name)


if __name__ == "__main__":
    main()

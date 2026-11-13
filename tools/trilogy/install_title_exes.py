#!/usr/bin/env python3
"""Copies the Trilogy executables next to Corruption_3 PC.exe, which starts them for an R3ME01 disc.

On Linux the destination is the RM3E01 build directory (MP3_BUILD_DIR, as build_prime_runtime.sh uses
it, or /mnt/mp3port/builds/RM3E01_00_runtime_linux), and the executables have no extension.
"""
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WINDOWS = os.name == "nt"
SUFFIX = ".exe" if WINDOWS else ""
if WINDOWS:
    DESTINATION = ROOT / "build/RM3E01_00_runtime_link_probe"
else:
    DESTINATION = Path(os.environ.get("MP3_BUILD_DIR") or
                       Path(os.environ.get("MP3PORT", "/mnt/mp3port")) / "builds/RM3E01_00_runtime_linux")
TITLES = {"R3ME01_FE": "Corruption_3 Trilogy Menu", "R3ME01_MP3": "Corruption_3 Trilogy MP3"}


def main() -> None:
    if not (DESTINATION / f"Corruption_3 PC{SUFFIX}").exists():
        raise SystemExit(f"No Corruption_3 PC{SUFFIX} in {DESTINATION}")
    for title, name in TITLES.items():
        source = ROOT / f"titles/{title}/work/recomp/build/{title}_runtime/Corruption_3 PC{SUFFIX}"
        if not source.exists():
            raise SystemExit(f"Not built yet: {source}")
        shutil.copyfile(source, DESTINATION / f"{name}{SUFFIX}")
        if not WINDOWS:
            shutil.copymode(source, DESTINATION / name)
        print(DESTINATION / f"{name}{SUFFIX}")


if __name__ == "__main__":
    main()

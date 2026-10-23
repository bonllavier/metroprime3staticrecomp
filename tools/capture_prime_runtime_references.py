#!/usr/bin/env python3
"""Pin the independent IOS/DI/HCI behavior references used by the runtime probe."""
import hashlib
import json
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
COMMIT = "cf5cb7ef81c3e8e7af5c9d566334219ffda9f68b"
PATHS = (
    "Source/Core/Core/IOS/IOS.cpp",
    "Source/Core/Core/IOS/DI/DI.cpp",
    "Source/Core/Core/IOS/DI/DI.h",
    "Source/Core/Core/HW/DVD/DVDInterface.cpp",
    "Source/Core/Core/IOS/USB/Bluetooth/BTEmu.cpp",
    "Source/Core/Core/IOS/USB/Bluetooth/BTReal.cpp",
    "Source/Core/Core/HW/WII_IPC.cpp",
    "Source/Core/Core/HW/WII_IPC.h",
    "Source/Core/Core/HW/AudioInterface.cpp",
    "Source/Core/Core/HW/AudioInterface.h",
    "Source/Core/Core/HW/DSP.cpp",
    "Source/Core/Core/HW/DSPHLE/DSPHLE.cpp",
    "Source/Core/Core/HW/DSPHLE/UCodes/INIT.cpp",
    "Source/Core/Core/HW/DSP.h",
    "Source/Core/Core/HW/MemoryInterface.cpp",
    "Source/Core/Core/HW/ProcessorInterface.cpp",
    "Source/Core/Core/HW/DSPHLE/UCodes/UCodes.cpp",
    "Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp",
    "Source/Core/Core/HW/DSPHLE/UCodes/AXWii.cpp",
    "Source/Core/Core/HW/DSPHLE/MailHandler.cpp",
    "Source/Core/Core/HW/DSPHLE/UCodes/ROM.cpp",
    "Source/Core/Common/Hash.cpp",
)


def main():
    directory = ROOT / "recomp/.tools/references/dolphin" / COMMIT
    rows = []
    for name in PATHS:
        url = f"https://raw.githubusercontent.com/dolphin-emu/dolphin/{COMMIT}/{name}"
        target = directory / name
        if target.exists():
            data = target.read_bytes()
        else:
            with urlopen(url, timeout=20) as response:
                data = response.read()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        rows.append({"url": url, "file": target.relative_to(ROOT).as_posix(),
                     "sha256": hashlib.sha256(data).hexdigest()})
    report = {"project": "dolphin-emu/dolphin", "commit": COMMIT,
              "license": "GPL-2.0-or-later (SPDX in each referenced file)",
              "purpose": "Independent behavior reference; not proof of Prime compatibility.", "files": rows}
    output = ROOT / "recomp/checkpoints/20261020_prime_runtime_reference_provenance.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"commit": COMMIT, "files": len(rows), "manifest": str(output)}, indent=2))


if __name__ == "__main__":
    main()

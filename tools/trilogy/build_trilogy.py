#!/usr/bin/env python3
"""Rebuilds both Trilogy executables (menu and MP3) from the current RM3E01 sources and installs them next to
Corruption_3 PC.exe: make_address_map -> make_title_tree -> audit_native_collisions -> build_title -> build
-> check -> install_title_exes. Close the game first.

  build_trilogy.py              test build (QA tools on, like the local RM3E01 build)
  build_trilogy.py --public     without the QA tools, for the public package (make_release.ps1 runs this)
  build_trilogy.py --configure-only [--public]
                                only switch the QA tools on or off and rebuild (no regeneration)

Any change to the RM3E01 runtime, Aurora, hooks or tools reaches Trilogy only through this chain
(recomp/docs/trilogy_r3me01.md).
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLES = ("R3ME01_MP3", "R3ME01_FE")
WINDOWS = os.name == "nt"
SUFFIX = ".exe" if WINDOWS else ""
QA_MARKER = b"MKW_PRIME_QA_TOOLS_ENABLED_BUILD"


def run(*command: str, cwd: Path = ROOT, env: dict | None = None) -> str:
    print("$", " ".join(command), flush=True)
    done = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          errors="replace", env=env)
    if done.returncode:
        print(done.stdout[-4000:])
        raise SystemExit(f"FAILED: {' '.join(command)}")
    return done.stdout


def build(title: str) -> Path:
    build_dir = ROOT / f"titles/{title}/work/recomp/build/{title}_runtime"
    env = dict(os.environ)
    if WINDOWS:
        tools = ROOT / ".tools/portable-tools"
        env["PATH"] = f"{tools / 'llvm-mingw/bin'};{tools / 'Ninja'};{env['PATH']}"
        jobs = os.environ.get("TRILOGY_JOBS", "4")
        run(str(tools / "Ninja/ninja.exe"), "-C", str(build_dir), f"-j{jobs}", "WiiCompiled", env=env)
    else:
        run("cmake", "--build", str(build_dir), "--target", "WiiCompiled", "--parallel", os.environ.get("TRILOGY_JOBS", "6"), env=env)
    return build_dir / f"Corruption_3 PC{SUFFIX}"


def version() -> str:
    header = (ROOT / "third_party/WiiCompiled/runtime/include/prime_port_version.h").read_text(encoding="utf-8")
    return re.search(r'#define PRIME_PORT_VERSION "([^"]+)"', header)[1]


def check(exe: Path, public: bool) -> None:
    data = exe.read_bytes()
    if public and QA_MARKER in data:
        raise SystemExit(f"{exe} still carries the QA tools")
    if not public and QA_MARKER not in data:
        print(f"note: {exe} has no QA marker", flush=True)
    wanted = version()
    title_tree_header = exe.parents[2] / "third_party/WiiCompiled/runtime/include/prime_port_version.h"
    copied = re.search(r'#define PRIME_PORT_VERSION "([^"]+)"', title_tree_header.read_text(encoding="utf-8"))[1]
    if copied != wanted:
        raise SystemExit(f"{exe}: title tree has version {copied}, RM3E01 has {wanted}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--public", action="store_true", help="build without the QA tools")
    parser.add_argument("--configure-only", action="store_true", help="only reconfigure and rebuild")
    args = parser.parse_args()
    python = [sys.executable, "-B"]
    public = ["--public"] if args.public else []
    for title in TITLES:
        print(f"===== {title}", flush=True)
        if args.configure_only:
            run(*python, "tools/trilogy/build_title.py", title, "--from-step", "configure", *public)
        else:
            run(*python, "tools/trilogy/make_address_map.py", title)
            print(run(*python, "tools/trilogy/make_title_tree.py", title).strip().splitlines()[-1], flush=True)
            print(run(*python, "tools/trilogy/audit_native_collisions.py", title).strip().splitlines()[-1], flush=True)
            run(*python, "tools/trilogy/build_title.py", title, *public)
        exe = build(title)
        check(exe, args.public)
        print(f"{title}: {exe} ({exe.stat().st_size / 2**20:.0f} MB, {'public' if args.public else 'QA'} build)", flush=True)
    print(run(*python, "tools/trilogy/install_title_exes.py"), flush=True)


if __name__ == "__main__":
    main()

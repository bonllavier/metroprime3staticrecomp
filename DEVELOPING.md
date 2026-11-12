# Developer guide

How to build and run the port from source on **Windows** or **Linux**, starting from nothing but
this repository and your own copy of the game. Nothing in this guide downloads or distributes game
data. Every game-derived file is produced locally from your disc and stays out of Git.

> **Game revision.** The project targets *Metroid Prime 3: Corruption* USA, game ID **`RM3E01`, revision 0**.
> Its `main.dol` must have SHA-256 `6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104`
> (SHA-1 `3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2`). The scripts refuse any other revision.
> The *Metroid Prime Trilogy* (`R3ME01`) and PAL (`RM3P01`) discs have their own pipelines under
> `tools/trilogy` and `titles/`; start with RM3E01.

## 1. Overview of the pipeline

```
your disc image ──(nod)──► build inputs: sys/main.dol, sys/fst.bin, files/RSO/*.rso, selfile.sel
                                │
main.dol ──(translator: translate-recursive)──► generated/RM3E01_00_conservative  (21,712 C++ functions)
RSO modules ──(RsoRuntimeTranslator)──────────► .tools/rso-runtime                (74 relocatable modules)
                                │
            tools/prepare_prime_runtime_hooks.py   (runtime hooks applied to copies of the translation)
            Translator.Cli emit-build-shards       (C++ grouped into build shards)
                                │
            CMake + Ninja + clang ──► "Corruption_3 PC" executable (runtime + Aurora + translated code)
```

At run time the executable opens the disc image with `nod` and reads every resource from it. The
extracted files above are build inputs only.

## 2. Workspace layout

The scripts expect this repository to be checked out as a folder named **`recomp`** inside a
*workspace root* folder:

```
<workspace>/
  recomp/                 ← this repository
  tools/                  ← created by setup_workspace (two inspection scripts)
  decomp/config/RM3E01_00 ← created by setup_workspace (symbol metadata)
  decomp/orig/RM3E01_00   ← link to your disc extraction (setup_workspace)
```

```bash
git clone https://github.com/bonllavier/metroprime3staticrecomp.git <workspace>/recomp
```

`main` builds both Windows and Linux.

Keep the workspace on a disk with **~25 GB free**. The translated sources, the build tree and the
caches are large, and a full Windows build used about 13 GB.

## 3. Toolchain

The versions below are the ones the project is built and tested with. The build scripts check some
of them (nod revision, Rust version on Linux).

| Tool | Version | Windows | Linux |
| --- | --- | --- | --- |
| clang / lld / llvm-ar | 22.1.8 | llvm-mingw 20260616 (UCRT) | official LLVM release |
| CMake, Ninja | 4.3.3, 1.13.2 | portable bundle | portable bundle |
| .NET SDK | 8.0.x | `recomp/.tools/dotnet` | any location on `PATH` |
| Rust (for nod) | 1.99.1 stable | `stable-x86_64-pc-windows-gnu` | `stable-x86_64-unknown-linux-gnu` |
| Python | 3.10+ | on `PATH` as `python` | `python3` |
| Git | any recent | | |

### Windows

From PowerShell, in `<workspace>`:

```powershell
# clang/lld (llvm-mingw), CMake and Ninja, pinned and hash-checked
powershell -ExecutionPolicy Bypass -File recomp\third_party\WiiCompiled\Launcher\Prepare-PortableTools.ps1 -Destination recomp\.tools\portable-tools

# .NET 8 SDK into recomp\.tools\dotnet (https://dot.net/v1/dotnet-install.ps1)
powershell -ExecutionPolicy Bypass -File dotnet-install.ps1 -Channel 8.0 -InstallDir recomp\.tools\dotnet

# Rust for the GNU target, kept inside recomp\.tools\rust
$env:RUSTUP_HOME = "$PWD\recomp\.tools\rust\rustup-home"; $env:CARGO_HOME = "$PWD\recomp\.tools\rust\cargo-home"
rustup-init.exe -y --profile minimal --default-toolchain stable-x86_64-pc-windows-gnu --no-modify-path

# nod (disc image reader) as a static library, pinned to commit 6a54b418
powershell -ExecutionPolicy Bypass -File recomp\tools\build_nod_ffi.ps1
```

### Linux (Ubuntu 22.04 tested)

```bash
sudo apt-get install build-essential pkg-config git python3 libasound2-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev \
  libusb-1.0-0-dev libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev libstdc++-12-dev
```

- `libstdc++-12-dev` is required on Ubuntu 22.04: clang 22 picks the GCC 12 headers.
- On Ubuntu Studio do **not** install `libjack-dev`: it replaces jackd2 and removes the audio stack.

Then:

1. Run `recomp/third_party/WiiCompiled/Launcher/prepare-portable-tools.sh`. It prepares clang 22, CMake and Ninja. Put the `bin/` of its x86-64 toolchain first on `PATH`.
2. Install the .NET 8 SDK (`dotnet-install.sh --channel 8.0`) and Rust 1.99.1 (`rustup`).
3. Copy [`tools/env.example.sh`](tools/env.example.sh), adjust the paths, and `source` it in every shell. It keeps the tools, caches and temporary files on a disk of your choice.
4. Run `recomp/tools/build_nod_ffi.sh`, which builds `libnod.a` with the zstd/zlib/bzip2/lzma compressors needed for `.rvz`/`.wia`/`.gcz`.

## 4. Extract the build inputs from your disc

The build needs `sys/main.dol`, `sys/fst.bin`, the 74 RSO modules and `selfile.sel` (in
`files/RSO/Rev2 Production/`). The game itself does not need them.

**Option A: `nod_extract_build_inputs`** (this repository). It reads ISO/RVZ/WIA/WBFS/GCZ directly,
validates partition hashes and extracts only what the build needs.

```bash
# Linux (Windows: the same with llvm-mingw's clang++, the x86_64-pc-windows-gnu libnod.a and
# -lws2_32 -luserenv -lbcrypt -lntdll instead of -lpthread -ldl -lm)
NOD=<path to nod-source>
clang++ -std=c++20 -O2 -fuse-ld=lld -I$NOD/nod-ffi/include recomp/tools/nod_extract_build_inputs.cpp \
  $NOD/target/x86_64-unknown-linux-gnu/release/libnod.a -lpthread -ldl -lm -o nod_extract_build_inputs
./nod_extract_build_inputs "<your image.rvz>" <extraction-dir> "RSO/Rev2 Production/"
```

**Option B: Dolphin.** Right-click the game → *Properties* → *Filesystem* → right-click the data
partition → *Extract Entire Disc…*. Use the folder that contains `sys/` and `files/`.

Then place the support files and link the extraction:

```bash
recomp/tools/setup_workspace.sh <extraction-dir>
```

On Windows: `powershell -ExecutionPolicy Bypass -File recomp\tools\setup_workspace.ps1 -Extraction <extraction-dir>`.

This copies the symbol metadata of [`workspace/`](workspace/README.md) into the workspace root (it
never overwrites existing files). It also checks `main.dol`'s hash, copies it to
`recomp/input/RM3E01_00/main.dol`, and links the extraction as `recomp/.tools/disc-extraction-rm3e01`
and `decomp/orig/RM3E01_00`. Everything it writes is ignored by Git.

## 5. Translate `main.dol`

From the `recomp/` folder (`dotnet` is `recomp\.tools\dotnet\dotnet.exe` on Windows):

```bash
dotnet build third_party/WiiCompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release
dotnet third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll \
  translate-recursive 0x80006320 --project config/RM3E01_00/project_full_conservative.yml \
  --output-metadata generated/RM3E01_00_conservative/base_translation_output.json --threads 8
```

This takes under a minute and about 3 GB of RAM. It writes the 21,712 functions to
`generated/RM3E01_00_conservative/functions`.

> **Known difference.** The translation the maintainer validated in game was produced on 2026-10-20.
> The current translator generates 102 functions differently: it saves more registers around some
> calls. They are expected to be equivalent but have not been played through. If a crash points at
> translated code, compare against that note in [`docs/linux_port.md`](docs/linux_port.md)
> ("Base de traducción").

## 6. Build

The build scripts run every step after the base translation:
1. They build the translators and run their self-tests.
2. They convert, translate and independently verify the 74 RSO modules.
3. They apply the DOL jump-table repairs and the runtime hooks.
4. They generate the data-section blobs and the build shards.
5. They configure and build with CMake/Ninja.
6. They copy the start-up screens and the default configuration next to the executable.

```powershell
# Windows, from <workspace>: output in recomp\build\RM3E01_00_runtime_link_probe
powershell -ExecutionPolicy Bypass -File recomp\tools\build_prime_runtime.ps1 -Jobs 8
```

```bash
# Linux: output in recomp/build/RM3E01_00_runtime_linux (or --build-dir DIR / $MP3_BUILD_DIR)
recomp/tools/build_prime_runtime.sh -j 8 --qa
```

- **First build:** compiles about 13,500 objects. It takes hours on a 4-core CPU.
- **Later builds:** after changing the runtime or Aurora, rebuild only the target with `cmake --build <build-dir> --target WiiCompiled`. That takes minutes.
- **When to run the full script again:** when the translation, the RSO modules, the hooks or the default configuration change.
- **`--qa` / `-DMKW_PRIME_QA_TOOLS=ON`:** adds the test tools:
  - F5/F7 draw dumps;
  - the F8 stack profiler;
  - the F9 performance panel;
  - input recording and replay.

  Public packages must be built without it, and `tools/package_release.ps1` refuses an executable that has them.

## 7. Run

```bash
cd <build-dir>
touch portable.txt          # keep UserData/ (settings, saves, logs, caches) next to the executable
./"Corruption_3 PC" --iso "<your image.rvz>"
```

On Windows, run `Corruption_3 PC.exe --iso "<image>"`, or let it ask for the image on first start.

- **First launch:** shaders are compiled and cached (`UserData/Cache`). That takes several minutes, later launches are fast.
- **F1:** settings.
- **Logs:** written to `UserData/Logs/<run>/console.log`.

## 8. Testing and diagnostics

| Tool | Use |
| --- | --- |
| `tools/test_graphics.ps1`, `tools/test_prime_runtime_contracts.ps1` | Windows regression tests of the renderer and runtime contracts |
| `qa/qa_record.ps1`, `qa/qa_replay.ps1` (Windows), `tools/linux_diag/qa_replay.sh` (Linux) | Record a play session and replay it unattended with per-window FPS: the reference for performance A/B tests |
| F8 (QA build) + `tools/summarize_stack_profile.py` | Sampling profiler of the game, GX, decoder, frame and presenter threads; reads Windows and Linux profiles |
| `tools/linux_diag/` | `ptracer.c` (attach gdb without root), `alloctrack.c` (live allocations by call stack), `watchdog.sh` (kills the game below 1.5 GB free RAM) |
| F7 / F5 (QA build) | Dump the draw list, image and textures of one or 60 frames to `UserData/Logs/captures` |
| Runtime unit tests | `MKW_PLATFORM_TESTS_ONLY` builds and the `*_tests` targets of the runtime; run the binaries directly |

[`docs/linux_port.md`](docs/linux_port.md) lists behaviours that were expensive to get right and
must not regress (Vulkan pipeline cache persistence, Dawn toggles, thread pools and core affinity,
X11 window sync…). Read its "No romper lo que funciona" table before changing Aurora or the platform layer.

## 9. Rules of the project

- The game logic runs at 60 Hz on VI retrace, like the console. Optimizations must not speed it up or decouple it. Frame interpolation only inserts images at presentation.
- Never commit or bundle disc data, extracted resources, translated output, keys or saves. The ISO/RVZ stays read-only.
- Platform differences go behind `#if defined(_WIN32)` / `#if defined(__linux__)`. A change that alters every platform is documented.
- A performance change is measured, ideally with a QA replay, before and after.

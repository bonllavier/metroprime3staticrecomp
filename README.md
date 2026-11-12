# Metroid Prime 3: Corruption — static recompilation (source)

Source of **Corruption_3 PC**, a native PC port of *Metroid Prime 3: Corruption* (Wii, USA, `RM3E01`
revision 0) by **bonllavier**. The game's PowerPC code is translated ahead of time into C++ and
compiled for the host, so no emulator or virtual machine runs at play time. A native runtime
(high-level emulation of the Wii OS and libraries, graphics through Aurora/WebGPU) runs the
translated code.

**No game data is included or distributed.** To build or play you need your own dump of the disc.
The build reads a few files from it, and the game reads the image itself while it runs. See [Legal](#legal).

## Status

| Platform | State |
| --- | --- |
| Windows 10/11 x64 (Direct3D 12 or Vulkan) | Main platform. Public test builds are released. The campaign start (G.F.S. Olympus, Norion, Bryyo up to Rundas) plays at or near 60 FPS. |
| Linux x86-64 (Vulkan, X11 tested) | Native port, same source tree. Boots and plays at a stable 60 FPS; 120 FPS frame interpolation matches Windows. Not packaged yet. See [`docs/linux_port.md`](docs/linux_port.md). |
| *Metroid Prime Trilogy* (`R3ME01`) | Prime 3 from the Trilogy disc boots to the ship cockpit in local test builds. Not released. See [`docs/trilogy_r3me01.md`](docs/trilogy_r3me01.md). |
| Other platforms | Not started. [`PORTING.md`](PORTING.md) describes what a new platform needs. |

What the port adds on top of the original game (all switchable in the **F1** overlay):

- **Controls.**
  - Keyboard and mouse with mouse look, or a gamepad (Xbox, PlayStation, Switch Pro, generic). Every Wii Remote and Nunchuk gesture is mapped to keys or buttons, and all of them can be rebound.
  - The game's button prompts and motion instructions show the bound key or pad button, in all five game languages.
- **Display.**
  - Any resolution and window mode, ultrawide (Hor+), first-person FOV.
  - Frame interpolation above 60 FPS. Game logic stays at 60 Hz.
  - V-Sync option.
- **Graphics.**
  - Per-pixel lighting, enhanced materials, ambient occlusion, screen-space reflections.
  - Texture upscaling (Super-xBR), anisotropic filtering, MSAA.
  - Texture replacement packs (Dolphin naming), ReShade (Windows).
- **Shader handling.** Shader cache and preload, plus a hybrid ubershader that avoids stutter while new shaders compile.
- **Extras.** RetroAchievements, photo mode, F11 screenshots, F12 bug reports, quit to title.

## Branches

| Branch | Content |
| --- | --- |
| `main` | The project: Windows and Linux from one source tree. Clone and build this one. |
| `parallel-experiment`, `linux-port` | Kept for experiments. They started the Windows and Linux lines and were merged into `main` on 2026-11-11; they may fall behind it. |

Linux-specific code is guarded by `#if defined(__linux__)`, Windows-specific code by `_WIN32`, and
changes that affect every platform are listed in the docs. One source tree builds both.

## Getting started as a developer

- [`DEVELOPING.md`](DEVELOPING.md): prepare a workspace, extract the build inputs from your disc, translate, build and run on Windows or Linux, plus test and diagnostic tools.
- [`PORTING.md`](PORTING.md): how the code is organised and what a new platform (another OS, a console) has to provide.

## Repository layout

| Path | What it is |
| --- | --- |
| `third_party/WiiCompiled/translator/` | The PowerPC → C++ static translator (C#, .NET 8). |
| `third_party/WiiCompiled/runtime/` | The native runtime: guest memory, CPU context, OS/library HLE (`src/hle/`), Prime-specific code, settings overlay, platform layer. |
| `third_party/WiiCompiled/aurora-main/` | Aurora: GX graphics on WebGPU (Dawn: D3D12 / Vulkan / Metal), SDL3 window and input, frame interpolation, shader cache. |
| `config/RM3E01_00/` | Translation project: function map, RSO module list, jump-table repairs. |
| `tools/` | Build pipeline (`build_prime_runtime.ps1` / `.sh`), RSO translator, hooks (`prepare_prime_runtime_hooks.py`), packaging, QA and analysis scripts. |
| `tools/linux_diag/` | Linux profiling, memory tracking, watchdog and QA replay helpers. |
| `workspace/` | Symbol metadata the pipeline reads from outside the repository; `tools/setup_workspace.*` places it. |
| `docs/` | Technical documentation and design decisions (in Spanish). Start with [`docs/plan_y_estado.md`](docs/plan_y_estado.md) and [`docs/historial.md`](docs/historial.md). |
| `checkpoints/` | Evidence of translation and validation steps (hashes, commands, results). |
| `titles/` | Per-title projects for other discs (Trilogy, PAL); the trees are regenerated from the disc. |
| `qa/` | Private QA recording and replay scripts (the recordings themselves are not committed). |

Generated and private material is ignored by Git: `input/`, `generated/`, `build/`, `.tools/`, QA recordings and logs.

## Documentation index (Spanish)

| Topic | Document |
| --- | --- |
| Overall plan and state | [`docs/plan_y_estado.md`](docs/plan_y_estado.md), [`docs/historial.md`](docs/historial.md) |
| Linux port, build, equivalents, things that must not regress | [`docs/linux_port.md`](docs/linux_port.md) |
| Keyboard, mouse and motion prompts | [`docs/entrada_teclado_raton.md`](docs/entrada_teclado_raton.md) |
| Gamepad | [`docs/mando_gamepad.md`](docs/mando_gamepad.md) |
| Frame interpolation, high FPS and V-Sync | [`docs/fps_altos.md`](docs/fps_altos.md) |
| Aurora decoder thread, threading | [`docs/hilo_decodificador_aurora.md`](docs/hilo_decodificador_aurora.md), [`docs/parallelizacion.md`](docs/parallelizacion.md) |
| Shader stutter, ubershader | [`docs/tirones_shaders_referencias.md`](docs/tirones_shaders_referencias.md), [`docs/ubershader_diseno.md`](docs/ubershader_diseno.md) |
| Graphics improvements, lighting, SSAO | [`docs/mejoras_graficas.md`](docs/mejoras_graficas.md), [`docs/iluminacion_motor.md`](docs/iluminacion_motor.md), [`docs/ssao_aurora.md`](docs/ssao_aurora.md) |
| Audio | [`docs/prime_audio_output.md`](docs/prime_audio_output.md) |
| Trilogy and PAL discs | [`docs/trilogy_r3me01.md`](docs/trilogy_r3me01.md), [`docs/pal_rm3p01.md`](docs/pal_rm3p01.md) |
| Releases | [`docs/publicacion_github.md`](docs/publicacion_github.md) |

## Legal

This repository contains no Nintendo code or data. The translated C++ is generated locally from
your own disc and is never committed or distributed (it is derived from the game). Builds must not
bundle disc files, extracted resources, keys or saves. The ISO/RVZ is opened read-only.

The project builds on WiiCompiled and is licensed under the **GNU General Public License v3.0**
([`LICENSE`](LICENSE)). Third-party components keep their own licenses: see
[`third_party/WiiCompiled/THIRD-PARTY-NOTICES.md`](third_party/WiiCompiled/THIRD-PARTY-NOTICES.md)
and the license files next to each component.

Not affiliated with, endorsed by, or associated with Nintendo or Retro Studios. *Metroid* and
*Metroid Prime* are trademarks of Nintendo.

## Credits

- [WiiCompiled](third_party/WiiCompiled/README.md): the static recompiler and runtime this port is built on.
- [Aurora](https://github.com/encounter/aurora), [Dawn](https://dawn.googlesource.com/dawn), [SDL3](https://www.libsdl.org/): graphics, window and input.
- [nod](https://github.com/encounter/nod): reading the disc image (ISO, RVZ, WIA, WBFS, GCZ).
- [rcheevos](https://github.com/RetroAchievements/rcheevos): RetroAchievements.
- [PrimeDecomp](https://github.com/PrimeDecomp/prime) and the Metroid Prime modding community: reference for the engine's structures.
- [Dolphin](https://dolphin-emu.org/): reference for hardware behaviour and the texture replacement format.

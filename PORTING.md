# Porting to another platform

This note is for developers who want to bring the port to a new host: another desktop OS, a handheld
or a console. It explains what is already portable, what each platform has to provide, and where the
platform-specific code lives. Read [`DEVELOPING.md`](DEVELOPING.md) first and get a Windows or Linux
build running: every new platform starts from a working desktop build to compare against.

## What is portable as is

- **The translated game code.** The translator turns every PowerPC function into a plain C++
  function over a `CpuContext` (registers, CR, XER, FPRs) and guest memory accessors. That code has
  no OS calls and compiles with clang on any little-endian 64-bit host. Big-endian guest memory is
  byte-swapped on access. x86-64 builds use the `x86-64-v3` profile (AVX2); AArch64 builds compile
  for the host CPU (`runtime/cmake/RuntimeLibrary.cmake`).
- **The translator itself** (C#, .NET 8) runs on the developer's PC. Its output is source code, so
  the target never runs .NET. Translate on Windows or Linux and copy `generated/` to the target build.
- **Most of the runtime.** The OS/library HLE (`runtime/src/hle/`: OS threads, DVD, NAND, IPC, VI,
  AX audio, KPAD/WPAD, GX command stream) and the Prime-specific code are standard C++20.
- **Aurora's GX emulation** (`aurora-main/lib/gx`, `lib/gfx`): it builds WGSL shaders and WebGPU
  command streams, independent of the native graphics API.

## What a new platform has to provide

| Area | Current implementations | Notes for a new host |
| --- | --- | --- |
| **Guest memory** | `runtime/src/guest_flat_memory.cpp`: Windows `VirtualAlloc2`/`MapViewOfFile3`, Linux/macOS `mmap` of a `memfd` | The translated code addresses guest RAM through one flat host mapping: a 4 GiB reservation with MEM1/MEM2 and their mirrors mapped as aliases of the same physical pages. The host needs reserve-then-map virtual memory and aliasing (two virtual views of one memory object). Without aliasing, mirrors must be emulated in the accessors, at a cost. |
| **Guest thread contexts** | `runtime/src/host_context.cpp`: Windows Fibers; elsewhere `libco` (`runtime/third_party/libco`, System V AMD64 and AArch64 back ends) | Guest threads are cooperative stackful contexts. A host with the System V x86-64 or AArch64 ABI can reuse libco; otherwise add a `co_switch` back end. |
| **Graphics** | Aurora → WebGPU via **Dawn** (D3D12, Vulkan, Metal), shaders WGSL → Tint | This is the largest piece for a console. Two routes: (1) add a back end for the console's graphics API to Dawn (or another WebGPU implementation), with Tint emitting its shader language (Tint already writes HLSL, MSL, SPIR-V and GLSL); or (2) replace Aurora's WebGPU layer (`aurora-main/lib/webgpu`, `lib/gfx`) with a native renderer. Route 1 keeps every Aurora feature (interpolation, ubershader, post effects, caches). |
| **Window, input, audio, threads** | **SDL3** (vendored by Aurora) | SDL3 has ports for many systems, some of them private to console developers. Without SDL, replace `aurora-main/lib/window.cpp` and `input.cpp` and the runtime's audio back end (`runtime/src/audio_backend*`). |
| **Disc image reading** | `nod` (Rust static library, `tools/build_nod_ffi.*`), `runtime/src/hle/storage/dvd.cpp` | Needs a Rust target for the platform, or a replacement reader for ISO/RVZ that implements the same calls (open, read file by FST path, partition-validated reads). |
| **Files and paths** | `runtime/src/platform/host_platform.cpp`, `runtime_config.h` | Executable directory, per-user data directory, log directory. `portable.txt` keeps `UserData/` beside the executable. |
| **CPU topology, priorities** | `aurora-main/include/aurora/host_cores.h`, `runtime/include/host_threads.h` | Keep the heavy workers (GX consumer, decoder, shader compiles) off the game thread's core, and give the game, GX, decoder and frame threads priority over background work. Both mattered a lot on Linux: see `docs/linux_port.md`. |
| **Timing** | `runtime/src/hle/vi.cpp` (`SleepPreciselyUntil`), `aurora.cpp` (`wait_until_precise`) | A high-resolution sleep plus a short spin keeps the 60 Hz VI cadence. |
| **Optional features** | ReShade (Windows only), RetroAchievements HTTP (`host_https`), update check, screenshots, F12 reports, Discord presence | Disable them on a new host at first; each one is behind its own option or platform guard. |

To list every platform branch, search the runtime and Aurora for `_WIN32`, `__linux__` and
`__APPLE__`. Today there are 69 files, and most of them have only a few lines. The densest are:
- `runtime/src/main.cpp`;
- `settings_overlay.cpp`;
- `guest_flat_memory.cpp`;
- `aurora-main/lib/aurora.cpp`;
- `pipeline_cache.cpp`;
- `window.cpp`;
- `host_context.cpp`;
- `host_platform.cpp`.

## Suggested order

1. **Build the runtime for the target with no game.**
   - Configure with `MKW_PLATFORM_TESTS_ONLY` and get the platform tests passing: memory, contexts, timebase.
   - Linux did this with `MKW_LINUX_RUNTIME_COMPILE_CHECK`, which compiles every runtime source without the translated shards.
2. **Bring up guest memory and contexts.** Nothing else runs without them.
3. **Boot with a null or minimal renderer** and the disc reader.
   - The log shows the RSO modules linking, DVD reads and the game reaching its first frames.
   - Compare the log line by line with a desktop run.
4. **Graphics.** Dawn back end or native renderer, then the shader cache.
   - Check compile times on the target: Linux needed changes there (see `docs/linux_port.md`).
5. **Input, audio, presentation pacing**, then the optional features.
6. **Measure.** Use the QA recordings (`qa/`, `tools/linux_diag/qa_replay.sh`) and the F8 profiler (`runtime/src/stack_profiler.cpp`) to compare against the desktop builds.

The Linux port followed this path in a few days of work. [`docs/linux_port.md`](docs/linux_port.md)
documents each Windows → Linux equivalence and the problems found along the way. Most of them would
show up again on any new host:
- shader cache persistence;
- driver-side shader validation cost;
- shader module memory;
- thread affinity inheritance;
- thread priorities;
- window-system round trips.

## Licensing on closed platforms

The code is **GPL v3** (inherited from WiiCompiled). Distributing a build that links GPL code with a
proprietary SDK that cannot be published may not be compatible with the GPL. Check this before
starting a port to a platform whose SDK is under NDA. Game data is never part of the project on any
platform: each user supplies their own disc image.

# Port de Linux (recompilación estática RM3E01_00)

Rama `linux-port`. Objetivo: el mismo ejecutable que en Windows, compilado de forma nativa para Linux x86_64 con Aurora (Dawn/Vulkan + SDL3), leyendo la imagen de disco del jugador en tiempo de ejecución.

## Entorno de desarrollo verificado

Ubuntu 22.04, NVIDIA RTX 3050 (driver 580), todo dentro de un disco dedicado montado en `/mnt/mp3port`:

| Componente | Versión | Origen |
|---|---|---|
| clang / lld / llvm-ar | 22.1.8 | `Launcher/prepare-portable-tools.sh` (hash fijado) |
| CMake / Ninja | 4.3.3 / 1.13.2 | el mismo script |
| .NET SDK | 8.0.428 | `dotnet-install.sh --channel 8.0` |
| Rust | 1.99.1 | `rustup` (la misma versión que exige `build_nod_ffi`) |
| Dawn | v20260603.191052 | paquete precompilado que fija Aurora |
| SDL3 | 3.4.4 | Aurora (vendor) |

`source /mnt/mp3port/env.sh` coloca herramientas, cachés de NuGet/Cargo/pip y temporales en ese disco.

Paquetes del sistema (cabeceras; ~37 MB):

```bash
sudo apt-get install libfuse2 build-essential pkg-config libasound2-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev \
  libusb-1.0-0-dev libpipewire-0.3-dev libwayland-dev libdecor-0-dev liburing-dev libstdc++-12-dev
```

- `libstdc++-12-dev` es obligatorio en Ubuntu 22.04: clang 22 elige las cabeceras de GCC 12 y sin ese paquete no encuentra `<vector>`.
- No instalar `libjack-dev` en sistemas con Ubuntu Studio: sustituye jackd2 y desinstala su entorno de audio. SDL funciona sin JACK (PipeWire/ALSA).
- `libpulse-dev` es opcional; el audio sale por PipeWire o ALSA.

## Construcción

```bash
source /mnt/mp3port/env.sh && cd $MP3PORT/src
recomp/tools/build_nod_ffi.sh                                   # nod 6a54b418, x86_64-unknown-linux-gnu
clang++ -std=c++20 -O2 -fuse-ld=lld -I$MP3PORT/tools/nod-source/nod-ffi/include \
  recomp/tools/nod_extract_build_inputs.cpp \
  $MP3PORT/tools/nod-source/target/x86_64-unknown-linux-gnu/release/libnod.a -lpthread -ldl -lm \
  -o $MP3PORT/builds/tools/nod_extract_build_inputs
$MP3PORT/builds/tools/nod_extract_build_inputs "<imagen.rvz|iso>" $MP3PORT/builds/disc-extraction-rm3e01 "RSO/Rev2 Production/"
recomp/tools/build_prime_runtime.sh -j 8
```

`nod_extract_build_inputs` extrae del disco, con validación de hashes de la partición, solo las entradas privadas del build (DOL, FST, 74 RSO y `selfile.sel`). Se enlazan en `recomp/.tools/disc-extraction-rm3e01` y `decomp/orig/RM3E01_00`, las mismas rutas que usa Windows. No son un requisito del juego, que sigue leyendo la imagen.

`build_prime_runtime.sh` es el equivalente de `build_prime_runtime.ps1`, con los mismos pasos y comprobaciones: traductores .NET, los 74 RSO (conversión, traducción, verificación independiente de relocaciones y registro), las reparaciones de switch del DOL, los hooks, la semilla de pipelines, los shards y el build con CMake/Ninja. No incluye `repair_prime_clock_metadata.py`, que corrige un problema del reloj de Windows.

Dependencias externas al repo `recomp` que el pipeline lee por ruta relativa (copiar desde el árbol de Windows):

- `tools/inspect_startup_rso.py`, `tools/inspect_static_sel.py`
- `decomp/config/RM3E01_00/symbols.txt`, `recovered_leaf.json`

### Base de traducción

`generated/RM3E01_00_conservative` se usa tal como la produjo Windows el 20 de octubre de 2026, que es la base validada en juego. El traductor actual genera 101 funciones distintas (guarda más registros); esa salida está aparte, en `builds/translation_linux_current`, para revisarla. El script de Windows tampoco regenera la base.

### Verificaciones

- Pruebas de plataforma (`MKW_PLATFORM_TESTS_ONLY`): 30/30 PASS.
- Compilación del runtime con perfil Prime sin shards: `-DMKW_LINUX_RUNTIME_COMPILE_CHECK=ON`, objetivo `mkw_linux_runtime_check`.
- `nod` en Linux: el `main.dol` leído del RVZ tiene el SHA-1 fijado `3e81f9b0…224b2`; los 75 archivos extraídos son idénticos a la extracción de Windows.
- La toolchain portable no incluye `ctest`: ejecutar los binarios de prueba directamente.

## Equivalencias Windows → Linux

| Función | Windows | Linux | Estado |
|---|---|---|---|
| Vista con ratón | `SetCursorPos`/`ClipCursor`/`GetCursorPos` en cada lectura KPAD | modo relativo de SDL3 (XInput2 en X11; relative-pointer + pointer-constraints en Wayland); `xrel/yrel` → `AddMotion` | implementado, pendiente de QA en juego |
| Salir/reiniciar (`OSRestart`) | `CreateProcessW` + `WaitForSingleObject` | `posix_spawn` + `pidfd_open`/`poll` (sondeo con `kill` en kernels antiguos) | implementado |
| Menú Trilogy | `Corruption_3 Trilogy Menu.exe` | `Corruption_3 Trilogy Menu` | implementado |
| Selector de imagen de disco | `IFileOpenDialog` / `MessageBoxW` | `SDL_ShowFileDialogWithProperties` (portal XDG / zenity) / `SDL_ShowMessageBox` | implementado |
| Informe F12 | DXGI, `RtlGetVersion`, GDI+, `explorer.exe` | `/sys/class/drm`, `/etc/os-release`, `/proc/meminfo`, `__get_cpuid`, `SDL_SavePNG`, `SDL_OpenURL` | implementado |
| Capturas | JPEG por GDI+ | PNG por `SDL_SavePNG` | implementado |
| Nombre de tecla según distribución | `GetKeyNameTextW` | `SDL_GetKeyFromScancode` + `SDL_GetKeyName` | implementado |
| Búsqueda de actualizaciones | WinHTTP | `HostHttps` (libcurl por `dlopen`, solo HTTPS, también en redirecciones) | implementado |
| Borrado seguro de contraseña | `SecureZeroMemory` | `explicit_bzero` | implementado |
| ReShade | DLL para D3D12 | — | fuera del port de Linux por ahora (decisión del usuario) |
| Iconos de teclado y de mando en pantalla (Xbox, PlayStation, Nintendo, GameCube) | GDI+ | `PrimeIconCanvas`: formas por distancia con antialiasing + `stb_truetype` (ImGui) con una sans negrita del sistema (Noto Sans, DejaVu Sans, Liberation Sans); misma geometría, colores e inversión de filas. La lógica de qué icono va con cada acción es común a las dos plataformas (`IconBackend`) | implementado, pendiente de QA en juego |
| Entrada y tipo de mando | SDL3 | SDL3 (HIDAPI/hidraw, evdev, udev) | ya era común |
| Logros (RetroAchievements) | WinHTTP + DPAPI + GDI+ | `HostHttps` + Secret Service por `libsecret` (`dlopen`; sin servicio de secretos la sesión no se recuerda y nunca se guarda en claro) + `SDL_LoadPNG_IO`; rcheevos v12.5.0 fijado en `.tools/rcheevos` | implementado, pendiente de QA |
| Overlay de rendimiento F9 | `GetProcessTimes`, Toolhelp32, PDH `GPU Engine` | `CLOCK_PROCESS_CPUTIME_ID`, `/proc/self/task/*/stat`, DRM fdinfo `drm-engine-*` (amdgpu, i915/xe, nouveau; el driver propietario de NVIDIA puede no publicarlo) | implementado |
| Núcleo reservado para el juego | `GetLogicalProcessorInformationEx`, `SetThreadAffinityMask`, `SetThreadIdealProcessor` | `/sys/.../thread_siblings_list`, `pthread_setaffinity_np`; el hilo del juego se fija al núcleo (Linux no tiene "procesador ideal"). i7-4790: CPU 3+7 para el juego, 0-2 y 4-6 para el resto | implementado |
| Prioridad y nombre de hilos | `SetThreadPriority`, `SetThreadDescription` | `setpriority` por hilo (fondo +5; subir juego/GX requiere `CAP_SYS_NICE`), `pthread_setname_np` | implementado |
| Espera activa al final del retrace VI | `YieldProcessor` | `pause` (x86) / `yield` (ARM) | implementado |
| Validación SHA-256 de los RSO | BCrypt | mbedTLS (`mbedtls_sha256`) | implementado |
| Latencia DXGI | `IDXGISwapChain2` | no aplica (modos de presentación de Vulkan) | no aplica |
| Cursor leído en el instante de KPAD | `GetCursorPos` | eventos SDL (hasta un fotograma más tarde) | diferencia conocida |
| Raíz de las continuaciones de hilos Prime | `GetCurrentFiber` | `HostContext::Current()` (libco) en todos los backends | implementado; el juego arranca |
| Salida de audio | WASAPI vía SDL | SDL; si el controlador elegido no tiene salida por defecto (PipeWire activo en un sistema PulseAudio, p. ej. Ubuntu 22.04) prueba los demás (ALSA → PulseAudio) | implementado; audio real comprobado |
| MSAA 2x/8x (opción F1 y recetas de la semilla grabadas en D3D12) | D3D12 los admite | el backend Vulkan de Dawn solo 1x/4x: 2x→1x, 8x→4x y recetas no compatibles no se precompilan | implementado (antes abortaba al arrancar) |
| Guardado de la caché monolítica de pipelines Vulkan | `PerformIdleTasks` buscado en `webgpu_dawn.dll` | llamada directa a `dawn::native::PerformIdleTasks` (Dawn enlazado estáticamente) | implementado: sin esto cada arranque recompilaba ~11.000 pipelines (>9 min, ~150 ms cada uno en NVIDIA) bloqueando el dibujado; ahora la precarga desde caché tarda ~65 s |
| `SDL_SyncWindow` en cada fotograma | inmediato | en X11 es una ida y vuelta al servidor y, con los bordes de KDE, una espera de 10 ms por fotograma; solo se llama tras un cambio de ventana pedido por el juego | implementado: de ~35-50 a 60 FPS estables (preparación del fotograma 13,7 → 2-4 ms) |
| Prioridad de las compilaciones de shaders (precarga y las que cubre el ubershader) | `SetThreadPriority` por compilación: `BELOW_NORMAL` para precarga/puenteadas, juego/GX/fotograma por encima de lo normal | sin `CAP_SYS_NICE` (`RLIMIT_NICE` 0) un hilo puede subir su nice pero no volver a bajarlo, y el juego no puede pasar de nice 0. Dos grupos fijos del mismo tamaño: compilaciones urgentes a nice 0 y precarga/puenteadas a nice 10 (mismas colas y límites de `render_policy.hpp`) | implementado, pendiente de comprobar en juego: con los seis hilos a nice 0 la generación bajaba a ~40 FPS durante la partida y a ~22-29 en la precarga |
| Núcleos físicos (`in_play_compile_cap`) | `GetLogicalProcessorInformationEx` | listas distintas de `thread_siblings_list` (antes: lógicos / 2, erróneo sin SMT) | implementado |
| Icono de la ventana | recurso `prime_port.rc` (`MetroidPrime3PCPort.ico`) leído con `LoadImageW` | la entrada de 128×128 del mismo `.ico` se decodifica al compilar (`cmake/ico_to_rgba_source.py`, solo biblioteca estándar de Python) a píxeles RGBA enlazados en el ejecutable; `main.cpp` los pasa a Aurora (`SDL_SetWindowIcon`) | implementado |
| Vigilante de parones (diagnóstico F8, QA) | temporizadores de alta resolución + Toolhelp32 | — | pendiente (solo diagnóstico) |

## Primer arranque en Linux (2026-11-11)

Ubuntu 22.04, X11, RTX 3050 (Vulkan, driver 580), RVZ leído con `nod`: el juego llega a la pantalla de seguridad de la correa del Wiimote a ~55 FPS con la caché de shaders vacía (11.364 en compilación), audio por ALSA→PulseAudio, 97 entradas/salidas RSO y cambios de hilo del invitado sin errores. Ejecutable de 213 MB; necesita glibc 2.35 y `libpng16.so.16` del build (empaquetar para distribuir).

Correcciones que hizo falta aplicar: `data_sections_init_blobs.S` regenerado en el host (el de Windows es COFF con rutas `G:/`), MSAA no admitido por Vulkan, raíz de continuaciones sin fibras de Windows y selección de controlador de audio.

Las pantallas propias del port que sustituyen a las de la correa (`runtime/assets/texture_replacements/strap_screen`, 8 imágenes: RM3E01 inglés/francés/español y Trilogy R3ME01) las copia `build_prime_runtime.sh` junto al ejecutable, como hace `package_release.ps1` en Windows; el juego las instala en `UserData/texture_replacements` al arrancar (Aurora: "Indexed 8 texture replacements").

Rendimiento (RTX 3050, i7-4790, X11): 60 FPS estables tras la precarga. Durante el primer minuto (precarga de 11.371 pipelines desde la caché) baja a ~22-29 FPS; pendiente de optimizar.

Configuración por defecto: `build_prime_runtime.sh` genera `Config.default.toml` con la plantilla de `package_release.ps1` (sin la sección ReShade); el juego la copia a `UserData/Config.toml` solo en el primer arranque. Sin ella faltaban `keyboard_icons`, vista con ratón y los ajustes de la versión publicada, y el juego mostraba los botones del Wiimote.

Pendiente de QA manual: ratón, mando (iconos Xbox/PlayStation), teclado, logros, F1/F9/F12, rendimiento con la caché caliente, Wayland.

# Port de Linux (recompilación estática RM3E01_00)

Rama `main` (el port de Linux empezó en `linux-port`, que se conserva para experimentos). Objetivo: el mismo ejecutable que en Windows, compilado de forma nativa para Linux x86_64 con Aurora (Dawn/Vulkan + SDL3), leyendo la imagen de disco del jugador en tiempo de ejecución.

## Estado actual (2026-11-11, `linux-port` 3234e72b)

- El juego arranca y se juega en Linux a 60 FPS estables (Ubuntu 22.04, X11, RTX 3050 con driver 580, i7-4790), con audio y lectura del RVZ en tiempo de ejecución. Se comprobó una partida nueva desde el arranque.
- Precarga de shaders: ~15 s con la caché caliente, ~4-5 min sin caché, con un máximo de ~2,3 GB de memoria.
- Funciona: pantallas propias de la correa, configuración por defecto de la versión publicada (iconos de teclado, vista con ratón, efectos), icono de la ventana, guardado de la caché Vulkan.
- Implementado sin QA del usuario: iconos de mando Xbox/PlayStation (se generan al usar un mando por primera vez), logros, F1/F9/F12, Wayland, Wiimote por Bluetooth (no investigado).
- Desde el 11 de noviembre de 2026, Windows y Linux se desarrollan en una sola rama, `main`, y las dos copias de trabajo (NTFS y `/mnt/mp3port/src/recomp`) usan esa rama. `parallel-experiment` y `linux-port` se conservan para experimentos y pueden quedarse atrás. Los cambios de Linux van bajo `#if defined(__linux__)`, y las correcciones comunes se describen aquí. Para pasar trabajo de una copia a la otra: commit, push a `main` y `git pull --ff-only` en la otra.

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
| Núcleo reservado para el juego | `GetLogicalProcessorInformationEx`, `SetThreadAffinityMask`, `SetThreadIdealProcessor` (sugerencia, no se hereda) | `/sys/.../thread_siblings_list`, `pthread_setaffinity_np`. Los trabajadores pesados (hilo GX, decodificador, compilación de shaders, texturas) quedan fuera del núcleo del juego. Se usa la máscara del proceso capturada al arrancar, y el hilo del juego no se fija. i7-4790: CPU 3+7 libres para el juego; 0-2 y 4-6 para los trabajadores | corregido 2026-11-11: antes el hilo del juego se fijaba a 3+7, sus hilos hijos heredaban esa máscara y `keep_off_game_core` calculaba una máscara vacía. El hilo GX, el decodificador y la compilación corrían todos en ese núcleo. Repetición `hangar_1x_window`: de 44,6 FPS (70/105 ventanas < 57) a 59,4 (1/80, la del arranque) |
| Prioridad y nombre de hilos | `SetThreadPriority` (juego, GX, decodificador y fotograma por encima de lo normal; compilación y texturas por debajo), `SetThreadDescription` | el usuario no puede bajar su nice (`RLIMIT_NICE` 0), así que la subida va por `SDL_SetCurrentThreadPriority(HIGH)`, que pide nice -10 a RealtimeKit por D-Bus (`host_cores::raise_thread_priority`). El hilo del juego lo repite tras `aurora_initialize`, cuando SDL ya tiene D-Bus. Fondo: nice 5 / 10. `pthread_setname_np` | implementado 2026-11-11: juego, GX, decodificador, fotograma y presentador a -10 comprobados con `ps -L`. `hangar_1x_window`: 59,4 → 59,84 FPS (2/80 ventanas a 55) |
| Espera activa al final del retrace VI | `YieldProcessor` | `pause` (x86) / `yield` (ARM) | implementado |
| Validación SHA-256 de los RSO | BCrypt | mbedTLS (`mbedtls_sha256`) | implementado |
| V-Sync (`[video] vsync`) | Fifo; con interpolación, Mailbox si D3D12 lo ofrece | X11 + NVIDIA ofrece Fifo, FifoRelaxed e Immediate, sin Mailbox. Sin V-Sync presenta con Immediate. Con V-Sync usa Fifo, y la interpolación queda en 60 | implementado: a 60, 59,82 FPS; a 120 sin V-Sync, 116,15 (igual que Windows, 116,0-116,7). Ver `fps_altos.md` |
| Ultrawide Hor+ (`[video] aspect_ratio`, `prime_widescreen.cpp`) | igual | igual: es código común sin rama de plataforma. Incluye la corrección del 12/11 (no activar viewports del juego; puntero escalado a los laterales) | comprobado 2026-11-12, repetición `hangar_uw21` (21:9, ventana sin bordes 1920x1027): 21:9 fuera de los vídeos y 16:9 durante ellos, el viewport de rayos X 500x200 aparece una sola vez al crearse (frame 5106) y no se dibuja, 59,03 FPS de media (una ventana a 50 en una carga) |
| Latencia DXGI | `IDXGISwapChain2` | no aplica (modos de presentación de Vulkan); la opción F1 "Single-frame present queue" se oculta | no aplica |
| API gráfica (F1 > Video) | Automático (Direct3D 12) / Direct3D 12 / Vulkan | Automático (Vulkan) / Vulkan: Dawn no tiene Direct3D en Linux y `main.cpp` ignora `d3d12` (usa Vulkan). Los textos de F1 sobre Windows (emparejar el Bluetooth, contador de GPU, cursor, idioma de las teclas) tienen su versión Linux | implementado |
| Cursor leído en el instante de KPAD | `GetCursorPos` | eventos SDL (hasta un fotograma más tarde) | diferencia conocida |
| Raíz de las continuaciones de hilos Prime | `GetCurrentFiber` | `HostContext::Current()` (libco) en todos los backends | implementado; el juego arranca |
| Salida de audio | WASAPI vía SDL | SDL; si el controlador elegido no tiene salida por defecto (PipeWire activo en un sistema PulseAudio, p. ej. Ubuntu 22.04) prueba los demás (ALSA → PulseAudio) | implementado; audio real comprobado |
| MSAA 2x/8x (opción F1 y recetas de la semilla grabadas en D3D12) | D3D12 los admite | el backend Vulkan de Dawn solo 1x/4x: 2x→1x, 8x→4x y recetas no compatibles no se precompilan | implementado (antes abortaba al arrancar) |
| Guardado de la caché monolítica de pipelines Vulkan | `PerformIdleTasks` buscado en `webgpu_dawn.dll` | llamada directa a `dawn::native::PerformIdleTasks` (Dawn enlazado estáticamente) | implementado: sin esto cada arranque recompilaba ~11.000 pipelines (>9 min, ~150 ms cada uno en NVIDIA) bloqueando el dibujado; ahora la precarga desde caché tarda ~65 s |
| `SDL_SyncWindow` en cada fotograma | inmediato | en X11 es una ida y vuelta al servidor y, con los bordes de KDE, una espera de 10 ms por fotograma; solo se llama tras un cambio de ventana pedido por el juego | implementado: de ~35-50 a 60 FPS estables (preparación del fotograma 13,7 → 2-4 ms) |
| Prioridad de las compilaciones de shaders (precarga y las que cubre el ubershader) | `SetThreadPriority` por compilación: `BELOW_NORMAL` para precarga/puenteadas, juego/GX/fotograma por encima de lo normal | sin `CAP_SYS_NICE` (`RLIMIT_NICE` 0) un hilo puede subir su nice pero no volver a bajarlo, y el juego no puede pasar de nice 0. Dos grupos fijos del mismo tamaño: compilaciones urgentes a nice 0 y precarga/puenteadas a nice 10 (mismas colas y límites de `render_policy.hpp`) | implementado, pendiente de comprobar en juego: con los seis hilos a nice 0 la generación bajaba a ~40 FPS durante la partida y a ~22-29 en la precarga |
| Validación SPIR-V de Dawn (`enable_spirv_validation`) | no aplica (D3D12 no genera SPIR-V) | desactivada en Release (se mantiene con `WII_COMPILED_VALIDATE_GPU`): validaba cada SPIR-V aunque viniera de la caché y era el 70 % de la precarga (muestras gdb). Cambiar el toggle cambia las claves de la caché de Dawn: el primer arranque tras actualizar recompila todo | implementado: precarga con caché caliente de 65-131 s a ~15 s (12.931 pipelines, 1,6 GB) |
| Caché de módulos de shader de Aurora | — | Dawn conserva el programa Tint (~1 MB) de cada módulo mientras la aplicación lo referencia; la caché los guardaba todos: ~11.000 módulos (>7 GB) en un arranque sin caché y la PC se congeló. Ahora guarda los 256 más recientes (afecta a todas las plataformas) | implementado: compilación completa sin caché en 256 s con 2,3 GB como máximo |
| Núcleos físicos (`in_play_compile_cap`) | `GetLogicalProcessorInformationEx` | listas distintas de `thread_siblings_list` (antes: lógicos / 2, erróneo sin SMT) | implementado |
| Icono de la ventana | recurso `prime_port.rc` (`MetroidPrime3PCPort.ico`) leído con `LoadImageW` | la entrada de 128×128 del mismo `.ico` se decodifica al compilar (`cmake/ico_to_rgba_source.py`, solo biblioteca estándar de Python) a píxeles RGBA enlazados en el ejecutable; `main.cpp` los pasa a Aurora (`SDL_SetWindowIcon`) | implementado |
| Herramientas de QA (`MKW_PRIME_QA_TOOLS`): F5/F7 volcado de dibujos, F4 imágenes intercaladas, F9 panel, F8 perfil de pilas | build de prueba con `-DMKW_PRIME_QA_TOOLS=ON`; F8 suspende hilos (`SuspendThread`/`GetThreadContext` + `StackWalk64`) | `build_prime_runtime.sh --qa`. F8 manda una señal de tiempo real (`tgkill`) a cada hilo, que registra su RIP y se desenrolla con `backtrace()`, en el mismo formato v3; `summarize_stack_profile.py` reconoce el ELF (base de carga, `nm`/`c++filt` del sistema). El fotograma y el presentador ya informan su TID real en Linux | implementado y comprobado: F8 automático de 10 s con 35.431 muestras de 5 hilos y 4 fallos; F7 guarda BMP, lista de dibujos y texturas |
| Vigilante de parones (diagnóstico F8, QA) | temporizadores de alta resolución + Toolhelp32 | — | pendiente (solo diagnóstico) |

## Primer arranque en Linux (2026-11-11)

Ubuntu 22.04, X11, RTX 3050 (Vulkan, driver 580), RVZ leído con `nod`: el juego llega a la pantalla de seguridad de la correa del Wiimote a ~55 FPS con la caché de shaders vacía (11.364 en compilación), audio por ALSA→PulseAudio, 97 entradas/salidas RSO y cambios de hilo del invitado sin errores. Ejecutable de 213 MB; necesita glibc 2.35 y `libpng16.so.16` del build (empaquetar para distribuir).

Correcciones que hizo falta aplicar: `data_sections_init_blobs.S` regenerado en el host (el de Windows es COFF con rutas `G:/`), MSAA no admitido por Vulkan, raíz de continuaciones sin fibras de Windows y selección de controlador de audio.

Las pantallas propias del port que sustituyen a las de la correa (`runtime/assets/texture_replacements/strap_screen`, 8 imágenes: RM3E01 inglés/francés/español y Trilogy R3ME01) las copia `build_prime_runtime.sh` junto al ejecutable, como hace `package_release.ps1` en Windows; el juego las instala en `UserData/texture_replacements` al arrancar (Aurora: "Indexed 8 texture replacements").

Rendimiento (RTX 3050, i7-4790, X11): 60 FPS estables. Precarga con caché caliente ~15 s (12.931 pipelines) con 55-60 FPS; sin caché (primer arranque o tras cambiar toggles de Dawn) ~4-5 min con caídas puntuales a 30-48 FPS y ~2,3 GB de memoria.

Configuración por defecto: `build_prime_runtime.sh` genera `Config.default.toml` con la plantilla de `package_release.ps1` (sin la sección ReShade); el juego la copia a `UserData/Config.toml` solo en el primer arranque. Sin ella faltaban `keyboard_icons`, vista con ratón y los ajustes de la versión publicada, y el juego mostraba los botones del Wiimote.

## Ejecutar y probar

```bash
cd /mnt/mp3port/builds/RM3E01_00_runtime_linux        # portable.txt: UserData/ vive aquí
./"Corruption_3 PC" --iso "/mnt/mp3port/iso/Metroid Prime 3 - Corruption (USA).rvz"
```

- El build de trabajo (`/mnt/mp3port/builds/RM3E01_00_runtime_linux`) es de prueba, con las herramientas de QA (`--qa`, F4/F5/F7/F8/F9), como los builds de prueba de Windows. Un paquete público se compila con `--public` en otro directorio.
  - F7 y F5 guardan en `UserData/Logs/captures`; también se piden creando un archivo `request` vacío allí.
  - F4 guarda 3 s de imágenes intercaladas junto a las reales en `UserData/Logs/captures/interp_<fecha>/` (o con un archivo `interp_request` vacío allí). Se analizan con `tools/interp_capture_check.py`. Es la única forma de ver un fallo que solo ocurre en las intercaladas. Ver [fps_altos.md](fps_altos.md).
  - F8 guarda en `UserData/Logs/profiles` y se resume con `tools/summarize_stack_profile.py`.
  - `WII_COMPILED_STACK_PROFILE_AT=<s>` graba 10 s sin intervención.
- Recompilar tras tocar el runtime o Aurora: `source /mnt/mp3port/env.sh; cmake --build /mnt/mp3port/builds/RM3E01_00_runtime_linux --target WiiCompiled --parallel 6` (de 1 a 3 minutos). `build_prime_runtime.sh` solo hace falta si cambian traducciones, RSO, hooks, shards, el `Config.default.toml` o las pantallas de la correa.
- Logs: `UserData/Logs/base_<epoch>_pid<PID>/console.log`. Líneas útiles:
  - `[present-latency] frames=N` cada 10 s (600 = 60 FPS).
  - `Pipeline prewarm finished: N pipelines in S s (Dawn blob cache: hits/lookups, stores)`: con la caché caliente, 100 % de aciertos y 0 stores.
  - `[pipeline-compile-summary]` durante la compilación.
- Una sola instancia: lanzar con un comando simple, `cd dir; ./juego ... & pid=$!` (nunca dentro de una cadena `&&` ni de una subshell, porque `$!` capturaría la subshell). Comprobar el PID antes de usar `kill`. Si queda una instancia viva, el usuario ve dos ventanas y las medidas no valen.
- Antes de lanzar una prueba, comprobar si el usuario tiene el juego abierto (`pgrep -af "Corruption_3 PC"`). Si hace falta para corregir errores o validar una corrección, se puede cerrar su instancia sin preguntar (directiva del usuario, 2026-11-11); avisar en la respuesta.
- Pruebas pesadas (caché vacía, cambios de memoria o de hilos): lanzar junto al juego `tools/linux_diag/watchdog.sh PID LOG`, que registra la memoria cada segundo y cierra el juego si quedan menos de 1,5 GB libres. Una prueba sin él congeló la PC del usuario y obligó a reiniciar (2026-11-11).
- Nunca capturar la ventana raíz ni el escritorio para ver el juego: expuso ventanas privadas del usuario. Usar la captura del juego o pedirla al usuario.

## Metroid Prime Trilogy USA (R3ME01) en Linux (2026-11-13)

Mismo diseño que en Windows ([trilogy_r3me01.md](trilogy_r3me01.md)): un ejecutable por DOL, junto a `Corruption_3 PC`, con el mismo `UserData`. `Corruption_3 PC` con un disco R3ME01 arranca `Corruption_3 Trilogy Menu`; el menú lanza `Corruption_3 Trilogy MP3` al elegir MP3, y MP3 vuelve al menú.

```bash
./"Corruption_3 PC" --iso "<Metroid Prime Trilogy (USA).rvz>"   # o directamente ./"Corruption_3 Trilogy Menu" --iso ...
```

Construcción (sin la cadena de Windows de `dtk`/extracción: se reutilizan de `titles/<título>/` de la copia de Windows `address_map.json`, `dtk/`, `extract/`, `functions.txt` y la traducción base `generated/` sin `build_shards`/`runtime_hooks`; `titles/` está fuera de Git):

```bash
source /mnt/mp3port/env.sh && cd $MP3PORT/src/recomp      # con el build de RM3E01 hecho (reutiliza sus _deps)
for t in R3ME01_MP3 R3ME01_FE; do
  python3 -B tools/trilogy/make_address_map.py $t          # si el runtime usa direcciones nuevas
  python3 -B tools/trilogy/make_title_tree.py $t
  python3 -B tools/trilogy/audit_native_collisions.py $t
  python3 -B tools/trilogy/build_title.py $t               # RSO, reparaciones, hooks, blobs de datos, shards, configure (QA; --public sin QA)
  cmake --build titles/$t/work/recomp/build/${t}_runtime --target WiiCompiled --parallel 7
done
python3 -B tools/trilogy/install_title_exes.py             # copia a builds/RM3E01_00_runtime_linux (o MP3_BUILD_DIR)
```

- Si solo cambia el runtime: `make_title_tree.py` y `audit_native_collisions.py` de cada título, `cmake --build` e `install_title_exes.py`. **`make_title_tree.py` copia el runtime y el overlay en ese momento**: hay que volver a pasarlo tras cada cambio, o el build compila la copia vieja.
- Diferencias con Windows: `make_title_tree.py` enlaza con symlinks (en Windows, `mklink /J`); `build_title.py` usa `dotnet` del PATH con `--no-incremental`, regenera `data_sections_init_blobs.S` en el host (como `build_prime_runtime.sh`) y configura como `build_prime_runtime.sh`. Sin cortina al cambiar de ejecutable: la ventana se cierra y se abre otra. `--iso` se pasa de un ejecutable al siguiente (en Windows no; ahí lee `[paths] iso_path`).
- El mapa de direcciones se regeneró aquí: añade solo las tres direcciones del puntero en ultrawide (`prime_widescreen.cpp`, 12/11); las demás coinciden con las de Windows. Pipeline: MP3 205 switches y menú 203 sin casos perdidos; `ScriptGUIScreenFrontEndUI` de MP3 no enlaza, como en Windows.

## No romper lo que funciona

Cada punto describe un comportamiento verificado; cambiarlo exige volver a medir.

| Lugar | Qué debe seguir así | Qué pasa si se rompe |
|---|---|---|
| `aurora-main/lib/webgpu/gpu.cpp`, `serialize_pipeline_caches` | rama Linux que llama a `dawn::native::PerformIdleTasks` | la caché Vulkan no se guarda y cada arranque recompila (>9 min) |
| `gpu.cpp`, toggles del dispositivo | `vulkan_monolithic_pipeline_cache` activo; `enable_spirv_validation` desactivado en Release | cualquier toggle nuevo o retirado cambia las claves de la caché de Dawn y el siguiente arranque recompila todo (~4-5 min); la validación SPIR-V multiplica la precarga por 4-8 |
| `aurora-main/lib/gx/pipeline.cpp`, `MaxCachedShaderModules` | la caché de módulos de shader está acotada (256, LRU) | Dawn retiene ~1 MB de programa Tint por módulo vivo: más de 7 GB en un arranque sin caché y la PC se congela |
| `aurora-main/lib/gfx/pipeline_cache.cpp`, `WorkerTier` | en Linux, un grupo urgente a nice 0 y uno de precarga/puenteadas a nice 10; juntos no superan `pipeline_worker_count()` | sin bajar la prioridad, los FPS caen mientras compila; sin el tope, se duplican los hilos que compilan a la vez. Un hilo sin `CAP_SYS_NICE` no puede volver a bajar su nice: no cambiar la prioridad por compilación como en Windows |
| `aurora-main/include/aurora/host_cores.h` (Linux) | `keep_off_game_core` usa `process_affinity_mask()`, capturada antes de restringir ningún hilo; `prefer_game_core` no fija el hilo del juego | los hilos creados por el hilo del juego heredan su núcleo, y el GX, el decodificador y la compilación se amontonan en él: 30-50 FPS |
| Prioridades en Linux (`host_threads.h`, `fifo.cpp`, `aurora.cpp`, `main.cpp`) | juego, GX, decodificador y fotograma piden prioridad alta por SDL/RealtimeKit; no volver a `setpriority(-5)`, que falla en silencio | esos hilos compiten a nice 0 con el escritorio y la compilación urgente |
| `aurora-main/lib/window.cpp`, `SDL_SyncWindow` | en Linux solo tras un cambio de ventana pedido (`request_window_sync`) | en X11/KDE cuesta unos 10 ms por fotograma (~35-50 FPS) |
| `tools/build_prime_runtime.sh` | copia las 8 pantallas de la correa y genera `Config.default.toml` a partir de la plantilla de `package_release.ps1` | vuelven las pantallas del Wiimote o los botones del Wii en lugar de los iconos de teclado. La configuración por defecto se edita en `package_release.ps1`, nunca en una copia aparte |
| `runtime/cmake/PublicProducts.cmake` + `ico_to_rgba_source.py` | el icono de Linux sale del mismo `MetroidPrime3PCPort.ico` | la ventana queda sin icono |
| `UserData/Config.toml` | el juego lo crea una vez a partir de `Config.default.toml` | un `Config.toml` antiguo sin las claves de la versión publicada desactiva funciones; hacer una copia de respaldo antes de cambiarlo |
| Código común con Windows | las diferencias van bajo `#if defined(__linux__)` / `_WIN32`; los cambios comunes se documentan en esta página | se rompe el build o el comportamiento de Windows, que no se puede compilar desde Linux |

Otras reglas del entorno:
- No instalar `libjack-dev` (desinstala el audio de Ubuntu Studio).
- No hacer commits en el `.git` anidado de `third_party/WiiCompiled`.
- La imagen de disco es de solo lectura, y no se publican datos del disco, claves ni partidas.
- ReShade queda fuera de Linux por decisión del usuario.

## Mandos que el kernel no reconoce (8BitDo Ultimate 2C 2,4 GHz, 2026-11-12)

En modo X-input, el receptor del 8BitDo Ultimate 2C (`2dc8:310a`) expone el mando como interfaz Xbox 360: clase `ff`, subclase `5d`, protocolo `01`. El driver `xpad` de kernels antiguos (5.15) no conoce ese ID y deja la interfaz sin driver. Solo aparecen el "teclado" y el "ratón" del receptor, y ni SDL ni el juego ven un mando. Con el mando apagado, el receptor se presenta como `2dc8:301c` ("8BitDo IDLE").

- **Comprobar:**
  - `lsusb` muestra el receptor.
  - `ls /dev/input/js*` no muestra nada.
  - En `/sys/bus/usb/devices/<puerto>:1.0/` no hay driver.
- **Arreglo (necesita `sudo`, lo ejecuta el usuario):**

  ```bash
  sudo modprobe xpad
  echo 2dc8 310a | sudo tee /sys/bus/usb/drivers/xpad/new_id
  # permanente: regla udev que repite lo anterior al conectar el receptor
  echo 'ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="2dc8", ATTR{idProduct}=="310a", RUN+="/sbin/modprobe xpad", RUN+="/bin/sh -c '"'"'echo 2dc8 310a > /sys/bus/usb/drivers/xpad/new_id'"'"'"' | sudo tee /etc/udev/rules.d/99-8bitdo-ultimate-2c.rules
  sudo udevadm control --reload-rules
  ```

- **Resultado:** aparece como "Generic X-Box pad" (`/dev/input/js0`, con ACL `uaccess` para el usuario). SDL 3.4 le aplica el mapeo Xbox estándar, y todos los botones, gatillos y sticks llegan con su nombre.
- **Verificación:** se hizo con un programa de prueba enlazado al mismo `libSDL3.a` del build, que imprime los eventos de gamepad.
- **Configuración:** si el mando no respondía cuando se intentó mapearlo, revisar `[prime_pad]` en `Config.toml`. Ahí quedó `a = ["guide"]`, es decir, disparar y confirmar en el botón Home, y por eso parecía que el mando no hacía nada en los menús.

## Diagnóstico (`tools/linux_diag`)

- **Muestreo de pilas sin `perf`** (en Ubuntu `ptrace_scope=1`):
  - Compilar `ptracer.c` (`gcc -O2 -shared -fPIC -o ptracer.so ptracer.c`).
  - Lanzar el juego con `LD_PRELOAD=.../ptracer.so`.
  - Tomar varias muestras con `gdb -p PID -batch -ex "thread apply all bt 25"` y agruparlas por función.
  - Por este método se encontró la validación SPIR-V. Guardar `ptracer.so` fuera de `/tmp`, que se vacía al reiniciar; si falta, `LD_PRELOAD` falla sin avisar y `gdb` no puede enganchar.
- **Memoria** (no hay heaptrack ni valgrind):
  - `alloctrack.c` es una biblioteca para `LD_PRELOAD` que agrupa por pila de 8 marcos los bloques vivos de 16 KiB o más, y los `mmap` anónimos de 256 KiB o más.
  - Cada 20 s escribe en `ALLOCTRACK_OUT` las pilas principales y el mapa de módulos. Las direcciones se resuelven con `addr2line -f -C -e "Corruption_3 PC" <dirección - base>`.
  - Por este método se encontró la retención de programas Tint.
  - `MALLOC_ARENA_MAX=2` sirve para descartar que el crecimiento sea fragmentación de glibc.
- **`watchdog.sh`**: ver «Ejecutar y probar».
- **`qa_replay.sh NOMBRE [ETIQUETA] [VAR=valor…]`**: equivalente de `qa/qa_replay.ps1`.
  - Repite una grabación de QA (las privadas viven en el árbol de Windows, `recomp/qa/recordings`) con un perfil aparte (`/mnt/mp3port/builds/qa/profile`), que comparte la caché de shaders, y con el vigilante de memoria.
  - Imprime los FPS por ventana. Necesita un build `--qa`.
  - Para A/B de rendimiento: `hangar_1x_window`, unos 6,5 minutos.
  - Resultados en `/mnt/mp3port/builds/qa/runs`.
- **Hilos y núcleos en vivo**: `grep Cpus_allowed_list /proc/PID/task/*/status` y `voluntary_ctxt_switches` por hilo. Así se encontró el núcleo compartido.
- **Análisis de F8 con libc**: `nm -D` no tiene las funciones internas de libc. Para un desplazamiento concreto, `gdb -batch -ex "info symbol 0x…" /lib/x86_64-linux-gnu/libc.so.6`: por ejemplo, el retorno del `syscall` futex dentro de `pthread_cond_signal`.

## Revisión Windows → Linux de reparto de CPU/GPU (2026-11-11)

Funciones de Windows que influyen en el rendimiento y su estado en Linux:

- Núcleo reservado y afinidades: corregido (ver la tabla).
- Prioridades de hilos: equivalente por RealtimeKit.
- Prioridad por compilación: dos grupos fijos.
- Núcleos físicos: equivalente.
- Temporizadores de alta resolución (`CreateWaitableTimerExW` + `timeBeginPeriod`) en el ritmo VI y la presentación: no hace falta nada en Linux, porque `sleep_until` usa hrtimers. El kernel lowlatency tiene un margen de temporizador de 50 µs que absorbe la espera activa final.
- Opciones de compilación: iguales en las dos plataformas (`-O3`, perfil x86-64-v3).
- Sin equivalente por ahora, sin efecto en los FPS de juego:
  - observador de refresco DXGI (`display_refresh.cpp`, solo para la interpolación de fotogramas);
  - cola de presentación DXGI;
  - vigilante de parones F8 (diagnóstico).
- Sistema: Turbo Boost desactivado en este PC (`intel_pstate/no_turbo`).

## Pendiente

- QA del usuario: mando (iconos Xbox/PlayStation), ratón, F1/F9/F12, logros, Wayland.
- Caídas a 30-48 FPS durante una compilación sin caché (solo en el primer arranque). Volver a medirlas con la corrección de núcleos: los trabajadores de compilación también estaban en el núcleo del juego.
- El Turbo Boost del i7-4790 está desactivado en este sistema (`/sys/devices/system/cpu/intel_pstate/no_turbo` = 1): 3,6 GHz fijos frente a hasta 4,0 en Windows. Es un ajuste del sistema o de la BIOS, no del port.
- Wiimote por Bluetooth.
- Ocultar MSAA 2x/8x en Vulkan.
- Vigilante de parones F8.
- Empaquetado (AppImage o tar con `libpng16`, más un lanzador `.desktop` con el icono).
- Revisar las 101 funciones en que difiere el traductor actual.
- Hacer el pipeline autosuficiente (hoy necesita archivos del árbol de Windows).
- El comentario de `[paths]` en la configuración por defecto muestra una ruta de Windows.

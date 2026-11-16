# Preparación para compartir el port (GitHub, releases, reportes)

Fecha: 25 de octubre de 2026. Petición del usuario: dejar el proyecto listo para que probadores externos lo usen y reporten fallos. Todo lo descrito está en la capa host (runtime y Aurora); la ISO, el matching y `decomp/` no cambian.

## Qué cambió en el runtime

| Tema | Cambio | Archivos |
| --- | --- | --- |
| Nombre y marca | El producto se llama **Corruption_3 PC**; el ejecutable pasa a `Corruption_3 PC.exe` (el target CMake sigue siendo `WiiCompiled`). Icono y bloque de versión incrustados (`CompanyName` = bonllavier, `FileVersion` = 0.1.0). La ventana SDL usa el mismo icono, leído del recurso del ejecutable. Los diálogos fatales y el panel F1 llevan el nombre del producto; el pie del panel F1 muestra versión y autor. | `runtime/include/prime_port_version.h` (única fuente de nombre, versión, autor, URL), `runtime/assets/windows/prime_port.rc`, `MetroidPrime3PCPort.ico` (generado desde `imgs/logo.png`), `cmake/PublicProducts.cmake`, `src/product/base_product.cpp`, `src/main.cpp`, `src/host_cpu_baseline.cpp` |
| Idioma | Todo el overlay (F1 y los paneles F5/F6/F7/F8/F9/F10/F12) está en inglés, incluidas las etiquetas de acciones de la tabla de controles. Los iconos de teclas de los avisos del juego ya seguían el idioma de Windows (`GetKeyNameTextW` y `GetUserDefaultUILanguage`): en un Windows en español muestran "Clic izq." y "Mayús", en inglés "L-Click" y "Shift". | `src/settings_overlay.cpp`, `src/hle/input/kpad.cpp` (etiquetas), `src/hle/input/prime_key_icons.cpp` (sin cambios, verificado) |
| ISO sin extracción | El perfil Prime ya no necesita `[paths] dvd_root`. Si no hay carpeta extraída válida, la tabla de archivos (`fst.bin`) se lee de la partición de datos de la imagen con `nod_partition_meta`, se indexan sus archivos con rutas de disco (sin rutas host) y las lecturas siguen yendo a la imagen. Si `iso_path` está vacío, se usa la primera imagen (`.iso/.rvz/.wbfs/.gcz/.wia/.ciso`, orden alfabético) de la carpeta `game/` junto al ejecutable. Mensajes de error en inglés con la ruta esperada. | `src/hle/storage/dvd.cpp` (`PrimePublishDvdFst`, `LoadFstIndexFromBytes`, `PublishPrimeDiscFstBytes`, `Reader::FstBytes`), `include/runtime_config.h` (`PrimeGameImageDirectory`, `ResolvedPrimeIsoPath`) |
| Teclado y ratón | Deja de depender de la variable de entorno: opción `[prime] keyboard_mouse` (por defecto `true`, casilla en F1 > Controls, requiere reinicio). La variable `WII_COMPILED_PRIME_KEYBOARD_MOUSE=0/1` sigue teniendo prioridad para los lanzadores de prueba. | `include/runtime_config.h` (`PrimeKeyboardMouseMode`), `kpad.cpp`, `prime_pointer_filter.cpp` |
| Reporte de errores | **F12** (o F1 > Diagnostics > *Create bug report*) escribe `UserData/Reports/report_<fecha>/` y su `.zip` con: captura del fotograma presentado (Aurora lee la fuente de presentación con `aurora_request_frame_capture(0, ruta)`, donde 0 = siguiente fotograma; el BMP se convierte a PNG con GDI+), `console.log` de la sesión, `crash_*.txt` si los hubo, `Config.toml`, `system.txt` (Windows, idioma, CPU, RAM, GPU por DXGI, pantalla, API, modo de entrada, imagen de disco) y `HOW_TO_REPORT.txt`. Se ejecuta en un hilo propio y abre el Explorador con el zip seleccionado. El diálogo de error fatal ahora indica la carpeta `Logs/base_<ts>_pid<pid>` que hay que adjuntar. | `src/bug_report.cpp`, `include/bug_report.h`, `aurora-main/lib/aurora.cpp` (sentinela de fotograma 0 y mutex de la petición), `src/main.cpp` (`RuntimeCrash::RunLogDirectory`, texto del diálogo) |
| Build | `CMAKE_RC_COMPILER` debe apuntar al `windres.exe` de llvm-mingw (no está en el PATH); `build_prime_runtime.ps1` lo pasa en los dos `cmake -S`. Enlaza además `dxgi` y `gdiplus`. Los scripts de `recomp/tools` referencian `Corruption_3 PC.exe`. | `tools/build_prime_runtime.ps1`, 19 scripts de `tools/` |

## Empaquetado y publicación

- `recomp/tools/package_release.ps1` crea `dist/Corruption_3-PC-v<versión>-win64/` y su `.zip` + `.sha256` con: ejecutable, 8 DLL, `dsp_coef.bin`, `initial_pipeline_cache.db` (semilla fusionada con la última sesión: 3.977 recetas), `wii_bootstrap/`, `portable.txt`, `UserData/Config.toml` limpio (sin rutas ni geometría de ventana), `game/PUT_YOUR_DISC_IMAGE_HERE.txt`, README, LICENSE, THIRD-PARTY-NOTICES y `docs/`. Rechaza empaquetar si aparece cualquier imagen de disco, partida, log o caché personal.
- `publish/` es el repositorio de GitHub (contenido en inglés): `README.md` con el banner (`docs/media/banner.png`), `CHANGELOG.md` (una sección por versión), `LICENSE` (GPL-3, heredada de WiiCompiled), `THIRD-PARTY-NOTICES.md`, `docs/controls.md`, `docs/troubleshooting.md`, `docs/development.md`, plantillas de issues (bug y rendimiento, con el campo obligatorio "¿dónde estabas en el juego?") y el workflow `Release`.
- `recomp/tools/publish_release.ps1` hace la publicación versionada sin `gh`: lee la versión de `prime_port_version.h`, empaqueta, hace commit de `publish/`, crea la etiqueta `vX.Y.Z`, empuja rama y etiqueta, crea la Release por la API REST con la sección del CHANGELOG y sube el zip y el sha256. Necesita `$env:GITHUB_TOKEN` (permiso *contents: write*). El nombre del repositorio por defecto es `bonllavier/Corruption_3-PC` (parámetro `-Repository` y constante `PRIME_PORT_REPOSITORY`).
- Para cada versión nueva: subir `PRIME_PORT_VERSION` en el header, añadir la sección `## vX.Y.Z` al CHANGELOG, recompilar y ejecutar `publish_release.ps1`.

## Por qué el ejecutable no se compila en GitHub Actions

El build enlaza el código traducido del DOL y de los 74 RSO, generado a partir del disco del usuario (`recomp/generated`, 971 MB, y `recomp/.tools/disc-extraction-rm3e01`). Ese material deriva del binario de Nintendo y no puede subirse a un repositorio público ni descargarse en un runner. Por tanto la compilación es local y GitHub solo aloja la documentación, las plantillas y los binarios de cada Release. El workflow `Release` comprueba la documentación, rechaza archivos prohibidos y, si alguien empuja una etiqueta a mano, crea una Release borrador con las notas del CHANGELOG para adjuntar el zip desde la web.

Nota de licencia: WiiCompiled es GPL-3; distribuir el binario modificado obliga a ofrecer el código fuente correspondiente del host (runtime y Aurora modificados). Los shards traducidos son un caso distinto y queda como decisión del usuario qué fuentes del host publicar; el repositorio preparado solo contiene documentación y binarios.

## Validación

Ver la sección final de este documento, que se actualiza con cada prueba registrada en `recomp/checkpoints/20261025_prime_publish_preparation.json`.

## Validación del 25 de octubre (tarde)

Ejecutable `Corruption_3 PC.exe` (versión 0.1.0) con la configuración del usuario sin `dvd_root` ni `iso_path` y la ISO enlazada (hard link) en `game/`:

| Prueba | Resultado |
| --- | --- |
| isoonly (pid 5868) | `[prime-iso-open] image=game/...iso`, FST original de 235 entradas publicada desde la imagen, 219 archivos indexados; modo teclado/ratón activo por configuración; F12 genera el zip con captura PNG, `console.log`, `Config.toml` y `system.txt` (Windows 19045, i7-4790, RTX 3050, es-MX). |
| isoonly2 / dvdroot | Al pulsar A en el título aparecía "WiiConnect24 cannot be used ... Error Code: 109042" en la selección de partida, con y sin carpeta extraída. Causa: el HLE de `/dev/net/kd/request` respondía -42 a *try suspend* en las dos primeras sondas (ciclo de arranque de Mario Kart). Antes quedaba oculto porque las pulsaciones automáticas de `skip_intro` (0,75 s tras el vídeo) pasaban el título y cerraban el diálogo. |
| final / final2 (pid 20732 / 21236) | Con la corrección (respuesta 0 bajo el perfil Prime): título → Main Menu sin diálogo, 60 FPS, Extras accesible, F12 correcto, 0 errores en el log. |

Observaciones: `skip_intro` ahora deja el juego en el título (una pulsación de A); en tres ejecuciones el clic automático enviado unos 15 s después del título (con la animación de texto del Main Menu aún en curso) se perdió y el siguiente siempre entró (final3, pid 18208, llega a Extras); queda por confirmar a mano que los clics de menú son fiables. Paquete: `dist/Corruption_3-PC-v0.1.0-win64.zip` (70,9 MB, SHA-256 ef237d7c...). Evidencia: `recomp/checkpoints/20261025_prime_publish_preparation.json` y `UserData/Logs/window_pid*_{isoonly,final}*.png`.

## Cambios posteriores del 25 de octubre (noche)

- **Selección de la ISO por diálogo.** Al arrancar sin `iso_path` válido, el runtime muestra un aviso y el diálogo de archivos de Windows (`PrimeDiscPicker::Prompt`, `runtime/src/hle/storage/prime_disc_picker.cpp`, `comdlg32`); la ruta elegida se guarda en `UserData/Config.toml` (`[paths] iso_path`) y se reintenta la apertura; cancelar termina con el diálogo fatal. También en F1 > Diagnostics > *Select disc image...*. La carpeta `game/` sigue detectándose en silencio pero ya no se documenta ni se empaqueta. Validado (pid 20836): aviso, diálogo, ruta escrita, `[prime-iso-pick]`, arranque y partida de Norion a 60 FPS.
- **Excepción 0x6BA en el diálogo.** El diálogo de Explorer lanza y captura internamente `RPC_S_SERVER_UNAVAILABLE` (0x6BA); `SehLogger` la trataba como fatal en la primera oportunidad (pid 16548). Ahora solo los códigos con severidad de error (`0xC0000000`) son fatales a la primera; el resto sigue su curso hasta `UnhandledSehFilter`.
- **Actualizaciones in situ.** El zip ya no incluye `UserData/` ni `game/`: lleva `Config.default.toml` en la raíz y `EnsureConfigFile` lo copia a `UserData/Config.toml` solo si no existe. Descomprimir una versión nueva sobre la carpeta conserva partidas (`UserData/NAND`), ajustes, teclas, ruta de la ISO y reportes.
- **Reportes por Discussions y aviso de fase de pruebas.** README con bloque destacado pidiendo testers; los reportes F12 se envían a Discussions > Bug reports (también `HOW_TO_REPORT.txt` y `config.yml` de issues).
- **`skip_intro` fuera de la versión pública.** La casilla desaparece del panel F1 y la clave no va en `Config.default.toml`; para pruebas propias valía `[prime] skip_intro = true`, `WII_COMPILED_PRIME_SKIP_INTRO=1` o `--skip-intro`. El 04/11/2026 el usuario lo descartó en todas las versiones: esas tres formas ya no existen. Solo los recorridos grabados de QA conservan las pulsaciones, con `WII_COMPILED_QA_TAP_INTRO=1` en la build de pruebas.
- **Revisión de copyright del contenido público** (`publish/` y el zip): sin archivos del disco; `dsp_coef.bin` y `wii_bootstrap/shared2/wc24` son los archivos libres de Dolphin (GPL-2+, hashes en THIRD-PARTY-NOTICES); `initial_pipeline_cache.db` contiene recetas de estado GX, no activos; el banner y el logo son arte propio estilizado. El riesgo que no se puede eliminar: el ejecutable contiene el código del juego traducido y las secciones de datos del DOL (`generated/data_sections_init*`), como cualquier recompilación estática; el README lo declara y exige el disco propio.

## Nombre definitivo: Corruption_3 PC (25 de octubre, noche)

Por decisión del usuario el producto, el ejecutable y el repositorio pasan a llamarse **Corruption_3 PC**: `Corruption_3 PC.exe` (con espacio, `OUTPUT_NAME` en `PublicProducts.cmake`; requiere reconfigurar porque el build usa `CMAKE_SUPPRESS_REGENERATION`), zip `Corruption_3-PC-vX.Y.Z-win64.zip`, repositorio `bonllavier/Corruption_3-PC` (GitHub no admite espacios). `prime_port_version.h` es la única fuente; el `.ico` conserva su nombre. Los scripts de `recomp/tools` y los de prueba apuntan al nuevo ejecutable. La configuración por defecto publicada copia las teclas y opciones de control del usuario y activa todas las opciones de rendimiento; el README explica la compilación de shaders, la caché y qué tirones no hay que reportar, y pide a los testers su `pipeline_cache.db` para ampliar la semilla.

## Versión visible y aviso de nuevas versiones (25 de octubre, noche)

- **F1 > Info**: producto, versión (`PRIME_PORT_VERSION`), fecha y hora de compilación (`__DATE__`/`__TIME__` del overlay), autor, botón al repositorio, carpeta del ejecutable, `Config.toml`, imagen de disco y carpeta de reportes.
- **Comprobación de versiones** (`runtime/src/update_check.cpp`, WinHTTP): al arrancar, si `[prime] check_updates` (por defecto `true`; `WII_COMPILED_PRIME_CHECK_UPDATES=0` lo anula), un hilo hace un GET a `api.github.com/repos/<owner>/<repo>/releases/latest` (owner/repo salen de `PRIME_PORT_REPOSITORY`), compara `tag_name` con la versión y guarda el estado. Con versión nueva: aviso verde abajo a la derecha durante 30 s y línea destacada en F1 > Info con botón a la página de descarga. Sin releases publicadas responde "HTTP 404 (no release published yet)"; sin red, "offline?". El paquete incluye además `Check for updates.ps1` (PowerShell, `Invoke-RestMethod`) que compara la `FileVersion` del ejecutable con la última release.
- **Reportes**: carpeta y zip `report_<fecha>_v<versión>`; primera línea de `HOW_TO_REPORT.txt` y `system.txt` con la versión.

## Efectos de imagen de Aurora y ReShade integrado (26 de octubre)

- **Post-proceso propio** (`aurora-main/lib/gfx/post_process.cpp`, enganchado en `encode_presentation_snapshot` de `aurora.cpp` tras la pasada "snapshot" y antes de ImGui): pasadas de pantalla completa sobre la imagen presentada, en este orden: SSAO, bloom (umbral + 2 niveles de reducción + gaussiana separable + composición), CAS (FidelityFX, MIT), FXAA 3.11, color (brillo, contraste, saturación, gamma, temperatura, viñeta). Ping-pong con una textura auxiliar del tamaño de la imagen; uniformes de 64 bytes por pasada. API C `aurora_set_post_process`/`aurora_get_post_process`/`aurora_post_process_status_note` (`gfx.h`), configuración `[effects]` y pestaña **F1 > Effects** (todo en caliente). Todo desactivado por defecto en `Config.default.toml`.
- **SSAO: el diseño vigente, la cronología de problemas y las reglas están en [ssao_aurora.md](ssao_aurora.md); lo que sigue es histórico.**
- **Profundidad para SSAO.** Aurora usa Z invertida (1 cerca, 0 lejos) y la Z NDC de GX es −z, de modo que con los términos de la proyección en perspectiva A = xf[4], B = xf[5]: distancia = B / (A − z). Se usa la **primera** proyección en perspectiva de cada fotograma (la del mundo; el HUD tiene su propia perspectiva con near/far diminutos). Como Prime borra el Z varias veces por fotograma (copias de efectos, HUD, copia de pantalla), la profundidad se **acumula**: `begin_depth` la pone a 0 al empezar el fotograma y `merge_depth` hace max(acumulado, EFB) antes de cada borrado de Z y al final del fotograma (texturas R32Float en ping-pong). Sin escena en perspectiva en el fotograma (menús, mapa, vídeos) la pasada no se ejecuta; los píxeles con z > 0,9 (casco, cañón) ni reciben ni producen oclusión; z < 0,0005 (cielo/borrado) se ignora. Solo con MSAA desactivado. Vista de depuración con `WII_COMPILED_POST_DEBUG_DEPTH=1` (recuadro inferior izquierdo: R distancia lineal/60, B z bruta).
- **Historial de la calibración** (sesión con el usuario jugando en Norion): instantánea en la presentación → siempre vacía (la copia de pantalla borra Z); captura antes de cada borrado → captaba la Z del HUD; captura tras la pasada más pesada → parpadeo al disparar (copias con borrado en mitad del mundo) y suelo ausente; acumulación por fotograma → estable.
- **ReShade 6.8.0** en `recomp/third_party/reshade/` (`ReShade64.dll` = `d3d12.dll` del instalador oficial renombrado, BSD-3) con `ReShade.ini` (Inicio = menú, Fin = efectos, ImprPant = captura, rutas relativas), preajuste `ReShadePreset.ini` (ContrastAdaptiveSharpen + SMAA) y shaders MIT/BSD (SweetFX, MagicBloom, AdaptiveSharpen, Colourfulness, AmbientLight, LUT, Deband...). Carga manual opcional (`[reshade] enabled`, `runtime/src/reshade_loader.cpp`) antes de `aurora_initialize`; cargado así ReShade exige `ReShade.ini` junto al ejecutable salvo `RESHADE_BASE_PATH_OVERRIDE`, que se fija a la carpeta `reshade/` (sin esa variable fallaba con error 1114). Solo Direct3D 12 (Vulkan necesita la capa del instalador). El paquete copia la carpeta entera; los avisos de terceros recogen cada licencia.

## Efectos: correcciones del 27 de octubre

- **Fotogramas repetidos.** Aurora presenta a veces dos veces el mismo fotograma (repetición sin dibujos). `latch_frame` conserva la cámara del mundo durante hasta 30 presentaciones vacías y `render_impl` no reinicia la profundidad acumulada si no hay dibujos en el EFB. Antes: 1.316 apagados intermitentes de la oclusión en una sesión; después (pid 13960, 614 repeticiones): ningún apagado en juego, solo uno legítimo en la transición del menú.
- **Distorsión del disparo cargado.** 11 dibujos ortográficos escriben profundidad cerca de la cámara (franja 0,004-0,031). La fusión filtra por la franja del mundo y se hace en cada cambio de pasada.
- **Bloom solo de la escena 3D** (`[effects] bloom_world_only`, por defecto activado): copia del color tras la pasada con más dibujos (el mundo), antes del visor y el HUD.
- **ReShade** se registra como add-on (cabeceras `recomp/third_party/reshade/include`, API 20) y sus efectos se pausan en menús, mapa, vídeos y con cualquier panel del port abierto (`ReShadeLoader::SetEffectsAllowed`). Excluir el visor dentro del juego exigiría `render_effects` dentro de la lista de comandos de Dawn, descartado por el riesgo con los descriptor heaps.
- **Incidente:** el disco G: se llenó al guardar y vació `post_process.cpp`; se reconstruyó entero. Copias de seguridad y scripts de prueba en `recomp/.session_tmp` (no se publica).

## Credenciales: control obligatorio antes de publicar (31/10/2026)

`check_no_credentials.ps1` impide que el zip o el repositorio `publish/` lleven credenciales del desarrollador (sesión y usuario de RetroAchievements, `GITHUB_TOKEN`, `VIRUSTOTAL_API_KEY`, `Config.toml`, logs, informes). Lo llaman `package_release.ps1` (carpeta y zip) y `publish_release.ps1` (antes de VirusTotal, de git y de la subida). Detalles en `retroachievements.md`. Se invoca dentro del mismo proceso (`& script -Path @(...)`): con `powershell -File`, PowerShell 5.1 parte la lista de rutas.

## Formatos de disco, aviso de shaders y datos nuevos del informe (15/11/2026)

Origen: discusión #2 de GitHub (3johnrw, v0.1.6, portátil con RTX 5080, imagen `.wbfs`). Las dos primeras sesiones arrancaron con la caché de shaders vacía (621 s y 653 s de generación), el jugador entró a la partida a los ~140 s y la escena o los personajes quedaron oscuros durante toda la sesión, también minutos después de terminar la generación. La tercera sesión, ya con caché, se veía bien. Las lecturas del disco coincidían entre sesiones: el formato no fue la causa. La causa exacta no quedó demostrada porque el log no registraba los dibujos omitidos.

- **Formatos.** Solo `.iso` y `.rvz` (decisión del usuario; `.wbfs` y el resto no están probados). `PrimeDiscPath::IsSupportedExtension` es la única lista; `PrimeDiscPath::Check` devuelve `UnsupportedFormat` antes de abrir el archivo, así que vale para `--iso`, `CORRUPTION3_ISO`, `[paths] iso_path`, la carpeta `game`, el diálogo (ya sin la entrada "All files") y el campo de F1. Pruebas: `mkw_prime_disc_path_tests` y 6 casos nuevos en `tools/test_prime_disc_selection.ps1` (un `.wbfs` que es un enlace duro a la imagen válida, más `.gcz`, `.wia`, `.ciso`).
- **Aviso rojo.** `DrawShaderGenerationAlert` (`settings_overlay.cpp`): mientras dura la generación de arranque (`aurora_get_shader_prewarm`), un recuadro rojo que late con dos triángulos de advertencia que parpadean: "GENERATING SHADERS - N LEFT / Wait before starting a game. Do not change settings." Se dibuja en la lista de primer plano de ImGui (por encima de la pantalla de arranque, el juego y F1), es código común a Windows y Linux y va en la versión pública. Con la caché llena dura lo que la precarga (~15 s).
- **`system.txt` del informe F12.** Formato y tamaño de la imagen, duración de la sesión, ejecuciones anteriores en `Logs`, y el bloque de `aurora_describe_render_state`: adaptador, driver y backend en uso; caché de shaders fría o caliente, número de arranques sobre ese archivo de caché y build; estado y duración de la generación; pipelines creados, en cola y fallidos; dibujos omitidos (pases normales y pases de copia) y dibujos con shader sustituto (reserva por vértice, ubershader). Si el informe se hace durante la generación, lo dice en una línea `NOTE`.
- **`console.log`.**
  - `[runtime] Corruption_3 PC <versión> (test build) windows|linux` al principio.
  - `[clock] t=<s> local=<fecha hora> presents=N shaders_queued=N shader_generation=...` cada 10 s (el log no tiene marcas de tiempo por línea; los analizadores de logs no cambian).
  - `[shader-cache] launch N on this cache file: N blobs at open (cold|warm)` al abrir la caché (contador `launches` en `cache_meta`).
  - `[draw-pipeline-summary]` como mucho cada 10 s y solo si algún contador se mueve: omitidos por presupuesto de fotograma (antes invisibles), omitidos en pases de copia, reserva, ubershader, cola y pipelines fallidos.
  - `[persistent-pass-skip]` ahora nombra el pipeline, el tamaño del pase, el fotograma y la cola (primeros 24 casos), y solo cuenta dibujos realmente omitidos (antes contaba también los que luego salían con la reserva).
  - `[pipeline-failed]` cuando un pipeline o su módulo no se crea.
  - `[prime-iso-open]` añade `format=` y `size_bytes=`.
- **Pendiente.** Reproducir la escena oscura con la caché vacía entrando a la partida durante la generación, ahora con estos contadores. Aparte, al cerrar tras un error fatal muy temprano el proceso se queda colgado en ~1 de cada 5-10 arranques (`std::exit` espera el cerrojo del cargador mientras otros hilos cargan DLL); visto con `WII_COMPILED_AUTOMATED_PROBE=1`, sin corregir.

## Dos archivos por versión: principal y complemento de Trilogy (16/11/2026)

**Decisión del usuario, para Windows y Linux:** cada versión pública sale en dos archivos de la misma versión, para que la descarga habitual no crezca.

| Archivo | Contenido | Tamaño aproximado |
| --- | --- | --- |
| `Corruption_3-PC-vX.Y.Z-win64.zip` (`-linux-x86_64.tar.gz` en Linux) | todo para *Metroid Prime 3: Corruption* (USA): `Corruption_3 PC`, DLL o librerías, semilla de shaders, `wii_bootstrap`, pantallas de la correa, configuración por defecto y documentación | ~89 MB |
| `Corruption_3-PC-vX.Y.Z-Trilogy-addon-win64.zip` (`-linux-x86_64.tar.gz`) | solo `Corruption_3 Trilogy Menu` y `Corruption_3 Trilogy MP3`, más `README - Trilogy add-on.txt`, `LICENSE` y `THIRD-PARTY-NOTICES.md` | ~90 MB |

Reglas:

- **El complemento se descomprime en la carpeta principal.** No trae nada más porque usa lo de esa carpeta: DLL o librerías, semilla, `UserData` (partidas, ajustes, teclas, cachés) y la ruta del disco. `Corruption_3 PC` sigue siendo la única entrada: con un disco de Trilogy abre el menú de Trilogy.
- **Los dos archivos tienen siempre la misma versión.** Se compilan del mismo código y comparten configuración. Al arrancar el menú, `Corruption_3 PC` pasa su versión en `CORRUPTION3_BASE_VERSION`; los ejecutables de Trilogy se niegan a arrancar con otra (`[trilogy-exec] version mismatch`) y muestran qué archivo descargar. Si se abren directamente, no hay versión que comparar.
- **Complemento ausente o incompleto:** al elegir un disco de Trilogy sin los dos ejecutables, el diálogo del disco dice cuál falta, qué archivo descargar (con la página de la versión) y permite elegir otra imagen. Queda en el log como `[prime-trilogy] add-on missing`.
- **Actualizar:** el aviso de versión nueva (esquina inferior y F1 > Info) recuerda descargar también el complemento cuando está instalado. F1 > Info muestra si está instalado, y el `system.txt` de F12 lleva `trilogy add-on: installed / incomplete / not installed`.
- Toda la lógica común está en `runtime/include/hle/prime_trilogy_addon.h` (nombres de ejecutables y archivos por plataforma, textos, variable de versión).

Herramientas:

- Windows: `package_release.ps1` genera los dos zips con su `.sha256` y comprueba que el complemento contenga exactamente sus cinco archivos y que los tres ejecutables cumplan las reglas públicas (sin QA ni diagnósticos, misma versión, sin símbolos de depuración). `make_release.ps1` compila Trilogy sin QA (`build_trilogy.py --public`), verifica los dos zips como los recibe un jugador (el principal sin Trilogy, el complemento descomprimido encima) y los analiza en VirusTotal. `publish_release.ps1` sube los dos zips y sus `.sha256`, y la nota de la release explica qué es cada uno.
- Linux: `tools/package_release_linux.sh` hace lo mismo con `.tar.gz`, que conserva el permiso de ejecución, y añade `README - Linux.txt` (requisitos, arranque, `--iso`). Primera ejecución real el 16/11/2026 con v0.1.7; ver "Paquete de Linux v0.1.7" en `linux_port.md`. Todavía no hay publicación de Linux en `publish_release.ps1`.
- `-LayoutTest` (Windows) y `--layout-test` (Linux) empaquetan el build de pruebas solo para comprobar la estructura; esos archivos no son releases.
- Prueba de punta a punta (Windows): `recomp/tools/trilogy/test_trilogy_addon.ps1 -Image "<Trilogy USA .rvz>"`. Monta una carpeta de jugador con los zips y comprueba: los dos zips, complemento ausente, complemento incompleto, complemento instalado (el menú arranca) y versión distinta e igual.

Instrucciones para jugadores: en el README público (sección del complemento), en `README - Trilogy add-on.txt` dentro del zip y en la nota de cada release.

Validación del 16/11/2026 (Windows, build de pruebas 08:38-08:40, RVZ de Trilogy USA): `test_trilogy_addon.ps1` pasó dos veces seguidas los seis casos.

| Caso | Resultado |
| --- | --- |
| Zips | 107 entradas en el principal, sin ejecutables de Trilogy; el complemento con sus 5 archivos; 98,9 + 93,3 MB con el build de pruebas, algo menos en el público sin símbolos |
| Sin complemento | `[prime-trilogy] add-on missing` con los dos ejecutables y el nombre del zip |
| Complemento incompleto | el mensaje nombra solo `Corruption_3 Trilogy MP3.exe` |
| Complemento instalado | el menú de Trilogy arranca |
| Otra versión | el menú sale con código 1 y `version mismatch` |
| Misma versión | el menú sigue en marcha |

Fallos que encontró la prueba y ya están corregidos:

- En `package_release.ps1`, el bucle de ejecutables pisaba la variable del nombre del paquete: el zip principal salía como `Corruption_3 Trilogy MP3.exe.zip`.
- En la propia prueba: el menú se arrancaba sin imagen, como nunca lo arranca el juego, que la hereda en `CORRUPTION3_ISO`. Además, el código de salida se perdía en PowerShell 5.1.

Limitación conocida: al lanzar el menú, `Corruption_3 PC` termina con `ExitProcess` sin vaciar el log (a propósito, en todos los cierres), así que a veces falta su última línea, `started the Trilogy menu`. El log del menú (`[trilogy-exec] saved region`) sí confirma el arranque.

### Mensajes del disco y cierre tras un error (16/11/2026, antes de la 0.1.7)

El usuario recordaba un mensaje con más formatos de los permitidos. Eran las versiones 0.1.5 y 0.1.6, que todavía aceptaban `.iso, .rvz, .wbfs, .gcz, .wia o .ciso` en tres textos: el diálogo, la imagen no válida y la imagen no encontrada. Se comprobó leyendo las cadenas de los ejecutables publicados. La 0.1.7 ya no los tenía, pero la revisión corrigió:

- **Formato no admitido:** solo nombra `.iso` y `.rvz` y la conversión con Dolphin. Ya no menciona otros formatos de copia, y `prime_disc_path_tests` lo comprueba.
- **Sin imagen:** nombra los dos discos válidos, Corruption USA (RM3E01) y Trilogy USA (R3ME01).
- **Disco equivocado:** el encabezado es "This disc image is not one the port runs" y debajo va el texto de `WrongDiscText` (lo que encontró y los dos válidos). Los reemplazos de esos textos en `make_title_tree.py` se quitaron porque ya no hacen falta.
- **Documentación pública:** README (sin el "not accepted yet"), troubleshooting y CHANGELOG 0.1.7.
- **`FailDvd` termina con `std::_Exit(1)`** en vez de `std::exit`. Con los destructores globales y otros hilos vivos, a veces el proceso no terminaba tras el mensaje: 1 de 5 arranques con un `.wbfs`, y 3 casos de `test_prime_disc_selection.ps1` en dos pasadas. Después del cambio, 18 de 18 casos en dos pasadas seguidas y 25 de 25 arranques con `.wbfs` terminan, con código 1, en 0,1 a 1,2 s. Otras rutas fatales que siguen con `std::exit` (`abi_bridge.cpp`, `os_init.cpp`) no se tocaron.

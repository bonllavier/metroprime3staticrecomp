# RetroAchievements en el port de RM3E01

Petición del usuario (31/10/2026): poder ganar los logros de RetroAchievements para Metroid Prime 3: Corruption (juego 34689) en el port. Estado: fase 0 hecha y fase 1 (casual) implementada; QA pendiente.

## Cómo funciona

- Librería oficial `rcheevos` (MIT), con la API `rc_client`. El programa le da:
  - una función de lectura de memoria;
  - una llamada por fotograma (`rc_client_do_frame`);
  - las peticiones HTTP: la librería no trae red y la conexión la pone el programa.
- Los logros son condiciones sobre direcciones de memoria de la consola. Mapa de Wii en rcheevos (`src/rcheevos/consoleinfo.c`):
  - 0x00000000–0x017FFFFF: MEM1 (0x80000000);
  - 0x01800000–0x0FFFFFFF: sin uso;
  - 0x10000000–0x13FFFFFF: MEM2 (0x90000000).
- Nuestra memoria invitada usa esas mismas direcciones y el mismo orden de bytes (big endian), y el código traducido del juego, incluidos los RSO que carga el propio juego, trabaja sobre ella. Las definiciones de logros escritas para Dolphin deberían funcionar sin cambios. Falta comprobarlo con logros reales.
- El juego se identifica por una huella MD5 de los bytes del disco tal como están en la ISO (`rc_hash_wii` / `rc_hash_wii_disc`, `src/rhash/hash_disc.c`), sin claves ni descifrado.

## Fase 0: huella de la ISO (hecha)

Algoritmo de rcheevos v12.5.0 reimplementado en Python (script en el scratchpad de la sesión), abriendo la ISO en solo lectura. Para la ISO del usuario, `Metroid Prime 3 - Corruption (USA).iso` (RM3E01, cifrada, una partición de datos en 0xF800000), da `19a1c0da8cad36165fb6287c3ea4adde`. Coincide con «Metroid Prime 3 - Corruption (USA) redump», una de las tres huellas admitidas por el set. Las otras son Europa (`af70ac448acf6534cc9548cbf7368fa4`) y el subset «Extra Corruption» (`bde61dde2389e6ca6cea97d1135cc97b`).

Código de referencia descargado en `recomp/.tools/rcheevos`: etiqueta v12.5.0, commit `1433173220a7eaede6a9ed7a18e94117be1821e0`, licencia MIT. Compilarlo requiere el permiso del usuario: el modo automático bloquea compilar código descargado.

## Reglas de RetroAchievements

- Un cliente no aprobado desbloquea solo en modo casual (softcore), según un aviso antiguo de su foro, el tema 29231. Hay que confirmarlo.
- Requisitos para el modo hardcore (docs.retroachievements.org, «Hardcore Compliance Requirements»):
  - un identificador propio («Nombre/versión»);
  - 6 meses publicado;
  - sin cargar partidas rápidas, cámara lenta, avance por fotogramas, rebobinado, trucos, editores de memoria ni reproducción de entradas;
  - una partida reanudada pasa a casual, y pasar de casual a hardcore reinicia el juego;
  - presencia y tablas de clasificación funcionando;
  - política de privacidad y lista de licencias.
- La aprobación es caso por caso, y los requisitos no mencionan ports ni recompilaciones: hay que preguntar a RetroAchievements antes de trabajar en hardcore. Revisar también qué opinan de las mejoras que escriben en la memoria del juego (el FOV) o cambian el control (el mouse look).

## Plan

1. **Base (casual):**
   - `rcheevos` como librería estática;
   - lectura de memoria con el mapa de arriba, sobre `Memory`;
   - `rc_client_do_frame` en cada retrazado, en el hilo del juego, como `PrimeFov::Frame`; `rc_client_idle` mientras el juego está en pausa o F1 está abierto;
   - HTTPS con WinHTTP;
   - inicio de sesión en F1: solo se guarda el token, cifrado con DPAPI, nunca la contraseña;
   - identificar el juego con la ISO configurada;
   - aviso de desbloqueo en el overlay.
2. **Completo:** lista de logros con progreso, indicadores de reto, tablas de clasificación, presencia y sonidos.
3. **Hardcore:** solo si RetroAchievements lo acepta, con el bloqueo de las funciones no permitidas y la solicitud de aprobación a los 6 meses.

No se publica ninguna integración con el servicio sin la decisión del usuario.

## Fase 1 implementada (31/10/2026)

Decisiones del usuario: solo modo casual (softcore), sin hardcore. Con RetroAchievements activo, el FOV personalizado queda bloqueado para jugar como el original. Ventana propia en una tecla F y avisos en pantalla con la estética del juego.

- **Build:** `cmake/PublicProducts.cmake` compila `mkw_rcheevos` (estática, `RC_CLIENT_SUPPORTS_HASH`) desde `MKW_PRIME_RCHEEVOS_DIR` (por defecto `recomp/.tools/rcheevos`, v12.5.0) y define `MKW_PRIME_RCHEEVOS_AVAILABLE`. Si no está la carpeta, el exe se compila sin el cliente. `prime_achievements*.cpp` quedan fuera de los grupos unity (`CMakeLists.txt`) porque la cabecera de ReShade exige otra versión de ImGui.
- **Núcleo** (`runtime/src/hle/prime_achievements.cpp`):
  - `rc_client` en modo casual (`rc_client_set_hardcore_enabled(0)`).
  - Lectura de memoria con el mapa de Wii, por `memcpy` desde `Memory::GetPointer`.
  - `rc_client_do_frame` en cada retrazado (`vi.cpp`, antes de `PrimeFov::Frame`).
  - Peticiones HTTPS con WinHTTP, solo `https` (`WINHTTP_FLAG_SECURE`), en un hilo que las atiende en orden.
  - Agente de usuario `Corruption3PC/<versión> (Windows) rcheevos/…`.
  - Identificación de la ISO en un hilo aparte.
  - Insignias y avatares descargados por HTTPS, decodificados con GDI+ y subidos con `aurora_imgui_add_texture`; Dawn tiene `ImplicitDeviceSynchronization`.
- **Credenciales:**
  - La contraseña solo sale en la petición de inicio de sesión. Se borra con `SecureZeroMemory`: el búfer del campo, las copias internas de ImGui (`InputTextState`, `InputTextDeactivatedState`; el campo usa `NoUndoRedo`) y el cuerpo POST después de enviarlo.
  - El token se guarda cifrado con DPAPI (`CryptProtectData`, usuario actual, entropía propia) en `UserData/retroachievements.dat`, nunca en Config.toml (los informes F12 lo incluyen).
  - Un token rechazado se borra y aparece un aviso en pantalla. Cerrar sesión borra el archivo.
  - El log solo recibe avisos de rcheevos, y se descarta cualquier mensaje que pueda citar credenciales.
- **F2: ventana RetroAchievements** (`prime_achievements_ui.cpp`):
  - Activar/desactivar y avisos sí/no.
  - Cuenta: inicio de sesión, avatar, nombre, puntos casual y hardcore, cerrar sesión, abrir el perfil.
  - Juego: insignia, título, X de Y logros, puntos, barra, presencia y enlace a la página.
  - Lista de logros por grupos de rcheevos: insignia en color o bloqueada, puntos, descripción, fecha de desbloqueo o barra de progreso.
  - La ventana bloquea la entrada del juego como F1.
- **Avisos en pantalla:** paneles del visor de Prime en el borde derecho (cian, esquinas cortadas, líneas de escaneo, resplandor, acento naranja Phazon). Entran deslizándose y muestran la insignia enmarcada, el encabezado («LOGRO DESBLOQUEADO»), el título, la descripción y «+puntos». Hay hasta tres a la vez, 6,5 s cada uno y 9 s el de juego completado. También avisan al cargar el juego (X/Y), al perder o recuperar la conexión y ante errores del servidor. El indicador de progreso aparece abajo a la derecha unos segundos, y los retos activos como insignias con un marco naranja que pulsa. La interfaz de RetroAchievements (ventana y avisos) está solo en inglés, por decisión del usuario.
- **FOV:** `PrimeFov::SetBlocked` mientras RetroAchievements está activo. F1 desactiva la casilla y explica por qué. El ajuste se conserva para cuando se desactive.

Pendiente (QA con la cuenta de prueba del usuario):
- inicio de sesión, recuperar la sesión al reiniciar, identificar la ISO y cargar el set;
- desbloquear un logro real, comprobando que las condiciones leen bien nuestra memoria;
- presencia en la web, los avisos y la ventana F2;
- comprobar que el servidor acepta un cliente desconocido en casual.

**Rendimiento.** Con RetroAchievements desactivado, solo cuesta una lectura atómica por retrazado. Activo, el hilo del juego ejecuta `rc_client_do_frame` una vez por retrazado: evalúa las condiciones del set leyendo unos pocos cientos de bytes con `memcpy`. La red, la huella de la ISO y las imágenes van en hilos aparte. Con `WII_COMPILED_PRIME_PERFORMANCE_TRACE=1`, `[retroachievements-perf]` registra cada 600 retrazados la media y el máximo en µs de esa llamada.

**Congelación al iniciar sesión (31/10/2026, pid 16436), corregida.** Fue el primer campo de texto del overlay. ImGui pide la entrada de texto de Windows (`SDL_StartTextInput`/`SDL_SetTextInputArea`, IME) desde `ImGui::Render`, que corre en el frame worker de Aurora. Esa llamada necesita el hilo de la ventana, que es el del juego, y el hilo del juego estaba en `GxGpuThread::HostLock` esperando a ese mismo frame worker (`aurora_wait_for_frame_worker_sealed`): bloqueo mutuo. `settings_overlay.cpp` sustituye `Platform_SetImeDataFn` por `DeferredSetImeData`, que solo anota la petición; `Draw()` la aplica en el hilo de la ventana con la función del backend, después de `aurora_wait_for_frame_worker()`.

## Credenciales del desarrollador: nunca en la versión pública (31/10/2026)

Instrucción del usuario: ninguna credencial suya de RetroAchievements puede llegar al público. Dónde están y qué lo impide:

- **Token de sesión y nombre de usuario:** solo en `UserData/retroachievements.dat`, cifrado con DPAPI (usuario de Windows actual, entropía `Corruption_3 PC/RetroAchievements/token`). La contraseña nunca se guarda. `Config.toml` solo tiene `[prime] achievements` y las notificaciones.
- **Logs:** solo «logged in», el juego cargado y los logros desbloqueados. Los mensajes de rcheevos que podrían citar `t=`, `p=`, `token` o `password` se sustituyen por «message withheld». Comprobado en todos los `console.log` del 31/10: no aparece el usuario.
- **Informes F12:** captura, `console.log`, `crash_*.txt`, `Config.toml`, `system.txt` y la partida. Nunca `retroachievements.dat`.
- **Zip público:** nunca contiene `UserData`.
- **Control automático** `recomp/tools/check_no_credentials.ps1`. Descifra localmente cada `retroachievements.dat` de `recomp/build`, `dist` y `%LOCALAPPDATA%\WiiCompiled`, y busca el token (en todos los archivos) y el nombre de usuario (en los de texto, salvo que sea el nombre público del autor) en ASCII y UTF-16. También busca `GITHUB_TOKEN` y `VIRUSTOTAL_API_KEY`, y rechaza por nombre `retroachievements.dat`, `Config.toml`, `*.log` y las carpetas `Reports`, `Logs` y `UserData`. Revisa tanto carpetas como el contenido de los zip. Nunca imprime ni escribe un secreto: solo dice qué archivo y qué tipo de secreto.
  - `package_release.ps1` lo ejecuta sobre la carpeta preparada y sobre el zip (si falla, borra el zip).
  - `publish_release.ps1` lo ejecuta sobre el zip y la carpeta `publish/` **antes** de subir a VirusTotal, del `git push` y de la subida a GitHub.
  - Validado el 31/10: `publish/` y el zip 0.1.5 limpios (122 archivos, 1 sesión comprobada); con una clave falsa detecta texto ASCII y UTF-16, binarios y entradas de zip, sin mostrar la clave.
- No desactivar ni saltarse este control. Si da un falso positivo, corregir el script, nunca publicar a mano.

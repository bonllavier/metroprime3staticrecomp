# Recompilación estática de Metroid Prime 3

Recomp 11 de noviembre: port nativo de Linux (rama `linux-port`). El juego corre a 60 FPS en Ubuntu 22.04/X11 con Vulkan (RTX 3050) leyendo el RVZ, con la misma configuración por defecto, pantallas de la correa e icono que Windows. Precarga de shaders ~15 s con caché (validación SPIR-V de Dawn desactivada) y caché de módulos de shader acotada (antes un arranque sin caché retenia >7 GB). Antes de tocar Aurora o el build de Linux leer [linux_port.md](docs/linux_port.md), en especial "No romper lo que funciona".
Recomp 25 de octubre (tarde): preparación para compartir. Producto "Corruption_3 PC" (ejecutable `Corruption_3 PC.exe` con icono y versión 0.1.0, autor bonllavier), overlay F1/Fx en inglés, arranque solo con la imagen de disco en `game/` (FST leída de la ISO, sin carpeta extraída), modo teclado/ratón como opción `[prime] keyboard_mouse`, reportes de error con F12 (captura + log + ajustes + sistema en `UserData/Reports`), empaquetado `recomp/tools/package_release.ps1`, repositorio preparado en `publish/` y publicación versionada `recomp/tools/publish_release.ps1`. El ejecutable no puede compilarse en GitHub Actions (código traducido derivado del disco). Ver [publicacion_github.md](docs/publicacion_github.md).
Recomp 25 de octubre: iluminación por píxel y reescalado de texturas Super-xBR (CPU, asíncrono, sin archivos) como opciones del host en Config.toml y el overlay F1; variantes por píxel precalentadas y pipeline de respaldo para no omitir dibujos en la primera sesión. Validado a 60 FPS en la Olympus con cachés vacías y calientes. Ver [mejoras_graficas.md](docs/mejoras_graficas.md) y recomp/checkpoints/20261025_prime_per_pixel_lighting_texture_upscale.json.
Recomp 22 de octubre: version 8db728e5, PID7452 para QA manual. Puente WPAD bruto corregido y verificado en el procesador original; E/Q como sensor longitudinal manual pendiente de probar en la palanca. Build y 19 pruebas PASS. Arranque ~60 present FPS; radio, rendimiento de escena pesada, velocidad irregular y 30 minutos jugables siguen pendientes. ISO y matching intactos. Ver recomp/checkpoints/20261022_raw_wpad_live_validation.json.


Este árbol es el ensayo independiente de recompilación estática para **RM3E01_00**. La ruta de decompilación con matching vive en `../decomp/`; este ensayo no modifica sus fuentes, configuración ni binarios. La ISO no se modifica.

Checkpoint historico del 20 de octubre: [arranque IOS/HCI](docs/20261020_ios_boot_progress.md). Ya existe ejecutable Windows y una lectura de recurso contrastada; diez comandos HCI iniciales completan. No hay pantalla verificada. Usar tools/build_prime_runtime.ps1 para conservar las adaptaciones al regenerar y tools/probe_prime_runtime.ps1 para registrar cada prueba por PID.

## Estado

- Referencia del traductor: WiiCompiled, commit `4c166a7eb1ddfc79b4c832e422e6bb00ec06fb4f`, conservada en `third_party/WiiCompiled/` con su licencia y avisos.
- Entrada local: copia de `decomp/orig/RM3E01_00/sys/main.dol`, SHA-1 esperado `3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2`.
- Revisión y layout comprobados con dtk. Entry point `0x80006320`; `_SDA_BASE_` `0x806801C0`; `_SDA2_BASE_` `0x806869C0`.
- Herramientas locales verificadas: .NET SDK 8.0.426, CMake 4.3.3, Ninja 1.13.2 y LLVM-MinGW Clang 22.1.8 (x86_64-w64-windows-gnu).
- El mapa amplio produjo 21.712 funciones sin instrucciones no soportadas. Las 72 shards, 17 unidades de registro/despacho y tres unidades de soporte compilan como 92 objetos COFF x86-64. La build conservadora evita la optimizacion state-free. El ejecutable Windows ya compila y enlaza con servicios de arranque RM3E01_00; no hay pantalla del juego verificada.
- Las pruebas genéricas de plataforma Windows de WiiCompiled compilan y pasan 9/9; no validan su HLE ni los gráficos de Metroid 3. Ver `docs/plan_y_estado.md` y `checkpoints/`.
- El traductor local acepta `translation.enable_state_free_abi`; el mapa completo se genero con `false` porque el modo optimizado falla en `0x802E201C`. Ver el parche y el checkpoint en `docs/plan_y_estado.md`.
- El HLE upstream registra 582 direcciones fijas. Hay colisiones de nombre confirmadas, por ejemplo `0x80173A54`: Prime 3 `EnableButtonPressingMode`, registro WiiCompiled `GXSetGPMetric`. No enlazarlo sin auditoria por titulo.

## Carpetas

- `config/RM3E01_00/`: manifiesto del título y mapa de funciones de esta revisión.
- `input/RM3E01_00/`: binarios locales derivados de la copia propia; ignorados por Git.
- `generated/`, `build/`, `logs/`: resultados generados, ignorados por Git.
- `runtime/`: adaptaciones específicas de este port.
- `third_party/WiiCompiled/`: herramienta de referencia fijada al commit indicado.
- `docs/` y `checkpoints/`: decisiones y evidencia de traducción.

No usar `allow_unsupported_instructions` en una salida destinada a ejecutar el juego. Un fallo de traducción se registra como tal. Cada checkpoint identifica revisión, hashes, comandos, resultados y limitaciones. Generar código o abrir una ventana no cuenta como llegar a una pantalla del juego.


- La auditoria estatica del arranque registra nueve sitios directos desde `0x80006320` y dieciseis en `main` (`0x802D5150`). Tambien registra el dispatch condicional del entry a `0x804B9E9C`, la entrada ya mapeada de `InitMetroTRK` (`0x804C0000 - 24932`); Ghidra confirma el mismo destino. Evidencia y hashes en `checkpoints/20261020_entry_boot_audit.json`, reproducible con `tools/audit_entry_boot.py`.

- La medicion estatica de llamadas directas alcanza 1.906 funciones desde el entry point, cero destinos directos sin fuente y cero colisiones HLE conocidas dentro de ese cierre. 190 funciones alcanzables contienen dispatch indirecto. Dos targets virtuales del recorrido inicial estan resueltos desde vtables del DOL (`0x8001A440`, `0x802D4FD4`); el sitio de `main` hacia `0x802D5384` queda bajo un guard estaticamente falso. Otros callbacks/virtuales y RSO siguen sin validar. No implica ejecucion. Reproducir con `tools/audit_direct_call_closure.py`; checkpoint `checkpoints/20261020_direct_call_closure.json`.

- Correccion de auditoria (2026-10-20): se descarto un ensayo auxiliar basado en un calculo de direccion incorrecto. El destino real es `0x804B9E9C` (`InitMetroTRK`), ya presente en el mapa canonico. Motivo archivado en `checkpoints/20261020_interior_entry_trial_rejected.json`.

- Se localizo la ISO en la raiz: cabecera validada como ID `RM3E01`, disco 0, revision 0, 4.699.979.776 bytes, SHA-256 `3621e6522f99e8c6755353530f2d571489b805bee85c054a96ac52c7e32ee71e`; el archivo esta marcado de solo lectura. Una extraccion de analisis ignorada bajo `.tools/disc-extraction-rm3e01` confirma que `sys/main.dol` coincide con el DOL fijado; el lector ISO del runtime sigue pendiente.
- El perfil Prime siembra los bytes de identificacion de low memory desde la cabecera observada (`RM3E01\0\0`) y mantiene los valores Mario Kart fuera del perfil; compila `mkw_runtime_common`. El link estricto mantiene tres destinos incompatibles heredados del scheduler/alarma Mario Kart. Ver `checkpoints/20261020_supplied_disc_audit.json`, `checkpoints/20261020_prime_lowmem_id.json`, `checkpoints/20261020_iso_partition_dol_match.json` y `checkpoints/20261020_static_recomp_progress_0806.json`, `patches/wiicompiled_rm3e01_lowmem_id.patch` y `docs/plan_y_estado.md`.

- Revalidacion 2026-10-20: se recreo solamente la extraccion temporal ignorada `recomp/.tools/disc-extraction-rm3e01` para recuperar evidencia RSO/FST; su DOL coincide byte por byte y el hash del FrontEndUI RSO coincide con el usado por los probes. El HLE DVD sigue limitado a una carpeta extraida: esta evidencia no convierte la extraccion en requisito del producto ni demuestra lectura ISO en runtime. Repetir `python -B recomp/tools/verify_research_extraction.py`; ver `checkpoints/20261020_research_extraction_revalidation.json`.
- Prototipo de lectura ISO directa (2026-10-20): un probe C++ enlaza temporalmente el FFI de `nod` desde `.tools`, valida hashes de particion y lee DOL, FST, FrontEnd.pak, GuiDVD.pak, selfile.sel y FrontEndUI RSO byte por byte desde la ISO; aun no esta conectado a DVD HLE. `nod` trae tablas de claves comunes Wii en su codigo, por lo que no se integra ni distribuye hasta resolver esta restriccion. Evidencia: `checkpoints/20261020_nod_direct_iso_probe.json`; comando: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File recomp/tools/build_and_run_nod_iso_probe.ps1`.
Auditoria de cuerpos del prolog (2026-10-20): los cuatro callbacks de la tabla relocada son hojas en metadata y fuente: cero llamadas directas o indirectas; las fuentes verificadas emiten 12, 9, 9 y 9 operaciones de escritura de memoria, respectivamente. Esto caracteriza su c�digo inicializador, pero no asigna significado a los datos que escriben. Se reproduce con `python -B recomp/tools/verify_frontend_rso_prolog.py` y queda en `recomp/checkpoints/20261020_frontend_rso_prolog_dispatch_audit.json`.

### Direcciones literales del runtime HLE frente a RM3E01_00

La auditoria `python -B recomp/tools/audit_runtime_literal_guest_targets.py` escaneo 83 fuentes C++ de runtime y encontro 14 dispatches a direcciones invitadas literales. Las 14 estan dentro de funciones Prime 3 de otra frontera y ninguna coincide con una entrada exacta de `functions.txt`. Ejemplos: el runtime llama `0x801A20BC` como `OSInitContext`, pero RM3E01 lo sit�a dentro de `fn_801A1DE0` en +0x2DC; `0x801A8088` como `__OSUnlockAllMutex` cae en el constructor `__ct__12CRenderActorF...`; y las direcciones THP `0x80550CC0` / `0x80551658` tambien caen dentro de cuerpos Prime. El inventario no demuestra que todos esos caminos se ejecuten, pero s� que no se deben despachar por esos numeros ni mapear por el nombre Mario Kart. No se agregaron aliases ni stubs. Proximo bloqueo de runtime: implementar el contrato Prime equivalente o demostrar un limite PPC llamable y sus argumentos para los servicios requeridos por el arranque. Evidencia con sitios, lineas, hashes y contenedores: [checkpoint](../checkpoints/20261020_runtime_literal_guest_target_audit.json).

- Ghidra confirma el literal HLE `OSInitContext` `0x801A20BC` dentro de `FUN_801a1de0` +`0x2DC` (`stw r0,0x2c(r1)`), sin xrefs. El DOL abierto coincide por SHA-256. No es una entrada PPC validada y no se debe invocar ni aliasar; revisar el binding/contexto Prime. Evidencia: `checkpoints/20261020_runtime_osinitcontext_literal_rejection.json`.

Verificacion reproducible de `0x801A20BC`: `python -B recomp/tools/verify_runtime_osinitcontext_literal_rejection.py` contrasta el hash del DOL, instruccion en los bytes fuente, entry map, rango simbolico y bytes crudos del DOL, ademas de los datos Ghidra registrados.


## Alarmas del primer recorrido y registros HLE interiores (2026-10-20)

La auditoria `python -B recomp/tools/audit_hle_registration_interior_targets.py` compara cada registro de direccion fija activo con los rangos completos de funciones Prime, no solo con los entry points exactos. Las 569 direcciones quedan dentro de cuerpos Prime con otro limite y ninguna es una entrada de `functions.txt`; ninguna aparece como nodo exacto del cierre directo actual desde `main`. Esto no resuelve callbacks/dispatches indirectos ni prueba alcance en runtime. El ejemplo `0x801A9C08`, registrado como `SelectThread`, cae en `AcceptScriptMsg__12CRenderActor...` +`0xB4`; `0x80006348` cae dentro de `__start` y el `__init_hardware` Prime real empieza en `0x800065C4`. Evidencia: [auditoria por rangos](../checkpoints/20261020_hle_registration_function_range_audit.json) y [contraste Ghidra con alarma temprana](../checkpoints/20261020_hle_interior_registration_and_startup_alarm.json).

Ghidra y la salida traducida verifican el camino `main` (`0x802D5150`) -> `0x802EAB50` -> `0x802EDC9C` -> `0x802EB5A4`; este ultimo ejecuta `InvokeDirectCpu<0x804DEC60u>(ctx)` hacia el `OSSetPeriodicAlarm` real de Prime. Por eso no se puede quitar el procesamiento de alarmas para cerrar el enlace. Siguen pendientes la cola SDA de Prime, su interrupcion decrementer, contexto de callback y cambio de hilo; no se ha ejecutado el camino ni presentado pantalla.

El analisis del temporizador ahora incluye el callback watchdog `0x802EB4E4`; el helper HLE calcula la cola con `r13-0x6360`, pero Prime la guarda en `r13-0x1D50` (head `0x8067E470`). Ese desajuste, la interrupcion decrementer y el contexto del callback impiden considerar operativo el timer, aunque el call a `OSSetPeriodicAlarm` de Prime ya esta en el cierre traducido. Ver `recomp/checkpoints/20261020_hle_interior_registration_and_startup_alarm.json`.


## Ruta real de la interrupcion de alarma Prime (2026-10-20)

El DOL fija `OSInit` (`0x804DE05C`) -> inicializador de alarmas `0x804DE938`; este consulta y registra la ranura 8 de la tabla de interrupciones con el wrapper `0x804DF028`. El cuerpo `fn_804DF028` hace `bl 0x804DEDFC`, el servicio de alarma ya traducido. `python -B recomp/tools/verify_prime_alarm_interrupt_route.py` comprueba hash de revision, bytes y destinos de los saltos relativos: [checkpoint](../checkpoints/20261020_prime_alarm_interrupt_route.json). Esto corrige el resultado historico de cero xrefs: Ghidra no tenia un objeto de funcion para el wrapper y por eso omitio su branch; el scan de bytes recupera el camino estatico. La ejecucion de la interrupcion y la equivalencia entre el `OSContext` guardado por PPC, `CpuContext` y fibers Windows siguen pendientes.

El cierre directo desde `__start` (`0x80006320`) cubre 1.906 funciones/4.358 aristas, sin fuentes o hashes faltantes, y enumera 447 dispatches indirectos. Ninguno de los 569 registros fijos HLE aparece como nodo exacto de ese cierre; esto no resuelve los 447 destinos indirectos. Evidencia: `checkpoints/20261020_entry_direct_closure_audit.json` y el cruce en `checkpoints/20261020_hle_registration_function_range_audit.json`.

La decodificacion del vector matiza el puente de callback: el dispatcher entrega r3 = indice 8 y r4 = el puntero que lee de low-memory `0xD4`; el wrapper `0x804DF028` lo conserva mientras completa el guardado PPC y salta al handler. El `ProcessAlarmQueue` actual ya usa `*0x800000D4` como r4 del callback, por lo que ese argumento coincide con el DOL. Su base de cola sigue equivocada (`r13-0x6360` frente a `r13-0x1D50`); queda validar la creacion/propiedad del OSContext y ejecutar el handler traducido en un contexto host aislado.

En el perfil RM3E01_00 sustitu? la referencia HLE de Mario Kart `func_801A1ED8` por la entrada Prime `0x804E0144`. Ghidra confirma que esa rutina guarda los registros en `OSContext`, y el scheduler Prime la llama desde `0x804E6CD0`; el `mkw_runtime_common` del perfil recompila. El enlace estricto pas? de tres s?mbolos faltantes a dos (`func_801A0620`, `func_801AADE0`); todav?a no genera ejecutable. Verificador y evidencia: `recomp/tools/verify_prime_context_save_binding.py` y `recomp/checkpoints/20261020_prime_context_save_binding.json`. La equivalencia de fibers en ejecuci?n sigue sin probar.

## Estado del probe grafico en recomp/ (2026-10-20)

La traduccion de `func_804D6690` ahora contempla todos los destinos de sus dos tablas indirectas del DOL: 36/36 y 23/23. El verificador `python -B recomp/tools/verify_prime_gx_state_switches.py` comprueba esos destinos contra el DOL RM3E01_00 fijado por SHA-256; esto valida cobertura de tablas, no matching de instrucciones. Ninja vuelve a generar `WiiCompiled.exe` correctamente.

Dos ejecuciones posteriores permanecieron vivas 30 y 20 segundos sin el crash anterior. La ventana existe, pero la captura es completamente negra y no cuenta como pantalla del juego. Aurora registra `SetViewport`/`SetScissor` descartados antes del primer render pass y completa el prewarm de 1.149 pipelines; hay que rastrear el ciclo de frame y la presentacion antes de afirmar progreso visual. Persisten las adaptaciones aproximadas de MMIO y el uso de extraccion privada en el runtime. Ver [checkpoint y logs](checkpoints/20261020_prime_runtime_gx_switch_progress.json) y [verificacion de tablas](checkpoints/20261020_prime_gx_state_switches.json).


- Probe runtime 2026-10-20: 30 s, MI/DSP MMIO superado, descriptores 0-7 completados, retorno de GX y entrada a colas IOS/STM; sin pantalla y sin fatal en log antes de detener el proceso. La ejecucion contiene un bypass diagnostico no validado del campo callback +0x28; no cuenta como fix. Ver checkpoint `checkpoints/20261020_prime_vi_wait_progress.json`. WiiCompiled aporta runtime/HLE y traductor reutilizables, pero este ejecutable esta generado y configurado para RM3E01_00; no es un lanzador universal de juegos Wii.

### Estado del sondeo FST/IPC (2026-10-20)

El perfil RM3E01_00 publica en memoria invitada el FST original: 235 entradas, 7.248 bytes, SHA-256 `bf2f1187e0791d050951057976578ee43d8320ebdc26a961ed9cbce0a8924ded`. El volcado `fst-runtime-prime.bin` es byte a byte igual a `recomp/.tools/disc-extraction-rm3e01/sys/fst.bin`. El FST sintetico anterior de WiiCompiled tenia 241 entradas y cambiaba indices; no se usa como tabla invitada de Prime. La inicializacion especifica del perfil ocurre antes de `OSInit`.

El probe limpio de 22 s completa los ocho descriptores de recursos y avanza por STM, NAND/red, hora y operaciones DI. La traza registra 16 entradas al handler IPC antes del bloqueo. Al llegar a `/dev/usb/oh1/57e/305`, se encolan el open y tres IOCTLV con `outstanding=0`; los indices avanzan a head=17, tail=27, get=1, put=11. Despues de ese punto no aparece otro paquete IOS ni otra entrada al handler; Aurora termina el precalentamiento de pipelines. El proceso permanece activo hasta que el harness lo detiene, sin error fatal y sin mostrar pantalla. Esto acota el problema al despertar/reanudacion de la cola en esa fase; el FST ya no es el bloqueo y todavia falta localizar la transicion perdida.

Se probo y retiro la llamada a `OS_HLE_DispatchPendingPrimeIpcInterrupt` al final de la rutina traducida de cola: produjo reentrada distinta, rechazo de `/dev/di`, una solicitud IOS no soportada y una violacion MMIO sin contexto CPU actual. No se considera un arreglo. Los shards generados conservan trazas acotadas del handler y la cola, que pueden perderse al regenerarlos. Son diagnosticas, no comportamiento del producto. Evidencia, hashes y limitaciones: `recomp/checkpoints/20261020_prime_fst_ios_queue_probe.json`. La carga sigue dependiendo de la extraccion privada de investigacion; no se ha visto una pantalla real.

## Avance de arranque (20 de octubre de 2026)

Se corrigio el despacho perdido de respuestas IPC anidadas con un drenaje iterativo acotado a 64 finalizaciones. La compilacion pasa y un sondeo de 45 s llega a VIDimming, recursos y apertura USB sin error fatal; no muestra una pantalla. El descriptor DI y el orden de espera de las solicitudes aun no estan validados: los ioctl 0x86 siguen usando el fd previo de red. Ver `checkpoints/20261020_prime_ipc_completion_pump.json` y `docs/plan_y_estado.md`.

Validacion de este hito: build de `WiiCompiled` y `ctest -R "mkw_(platform_paths|runtime_config)_tests"` pasan; resultado 2/2.

La traza `ipc-queue-return` confirma un resultado sincronico obsoleto: `open /dev/di` devuelve 6 y se reutiliza el mismo objeto antes de que el HLE registre su apertura. Sigue pendiente demostrar el origen del despertar prematuro.

### Candidata RSO_rhbm (2026-10-20)

El offset `0xC690` del callback observado cae dentro de la seccion 1 de `RSO_rhbm.rso` (`0xFE88` bytes). Con la colocacion sintetica del probe, la direccion de traduccion es `0x806A4C70`; se generaron y compilaron seis funciones AMD64 con cero instrucciones no soportadas y cero SSA invalidas. La direccion runtime `0x0101C690` solo coincide si el loader coloca esa seccion en una base determinada, todavia sin evidencia. No se integro la candidata. Ver `checkpoints/20261020_rhbm_callback_candidate_probe.json` y `python -B tools/verify_rhbm_callback_candidate.py`.

La traza runtime confirma que las cinco solicitudes RSO vienen de `FUN_800CC804` y leen recursos del boton HOME de Wii (`RSO_rhbm.rso`, ARC de idioma, sonidos e icono). `0x0101C690` ya esta en la pila al entrar; los cinco estados previos a dispatch son cero. No es evidencia de que el titulo de Metroid se haya presentado. La ejecucion sigue sin pantalla y su cola se detiene al encolar USB. Ver checkpoint `checkpoints/20261020_rhbm_callback_candidate_probe.json`.


### LR de las solicitudes IPC tras VIDimming (2026-10-20)

Se corrigio la traza de `func_8050441C`: `r1+132` leia datos ajenos al marco y producia falsos caller. El LR entrante se guarda en `r1+36` despues de reservar 32 bytes; con esa ranura, los ultimos enqueues se atribuyen a `func_80504780` (open USB) y `func_80505384` (tres IOCTLV). El build pasa y el proceso sigue vivo 25 s sin fatal, pero `head=28` no avanza mientras `tail` sube a 33 y no aparece otro paquete IOS. Esta evidencia acota el siguiente paso al despertar/planificacion de la cola; aun no identifica la transicion perdida ni muestra una pantalla. Ver `checkpoints/20261020_prime_ipc_queue_caller_trace.json` y ejecutar `python -B recomp/tools/verify_prime_ipc_queue_caller_trace.py`.

### 2026-10-21: contexto de la interrupcion VI Prime

El HLE VI ahora pasa por `r4` un puntero OSContext guest valido al handler Prime `0x804EEB6C`: usa el contexto del hilo en `0x800000E4` y, cuando no hay hilo activo, el contexto idle propio del DOL `0x8060B830` observado en `0x804E6BD4`. Ghidra confirma que el handler `0x18` se registra en `0x804EF71C`, preserva `r4` y lo restaura mediante `0x804E00DC`. Una traza previa mostraba que dejar el `r4` comun del contexto inyectaba `0x1000` y da?aba `0x800000D4`. Tambien se rechazo explicitamente el puntero cero: `Memory::Contains(0, 0x2C8)` habia resultado verdadero.

La prueba de 25 s consume el comando IOS sincronico 7 en `head=28` y avanza la cola hasta `head=29`; el estado anterior se deten?a antes de ese consumo. El proceso permanece vivo y sin fatal, pero no hay imagen del juego y termina sondeando retraces. Ninja y CTest 9/9 pasan. Evidencia: `recomp/checkpoints/20261021_prime_vi_context_argument.json`; verificar con `python -B recomp/tools/verify_prime_vi_context_argument.py`. El DOL RM3E01_00 mantiene SHA-256 `6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104`; la ISO continua intacta y en solo lectura. Proximo: localizar PC/estado del hilo despues del comando 7; no simular respuestas IOS ni afirmar que ya se renderiza una pantalla.


## Paralelismo

El experimento de distribucion de trabajo se desarrolla en la rama `parallel-experiment`, separada de `main` y de la etiqueta `before-parallelism`. El primer candidato paraleliza condicionalmente la expansion de vertices de display lists grandes; la equivalencia de bytes esta cubierta por pruebas, pero falta medir el efecto sobre FPS reales. Alcance, limites y comandos: [experimento de paralelismo](docs/parallelizacion.md).

## QA manual por peticion del usuario, 22 de octubre

El usuario regreso y asume el QA. Se retiro la inyeccion de botones por
archivo en KPAD, sus headers/test/target, senders de comandos, clicks
Windows y job de teclado del probe. Se conserva el control manual SDL
y la evidencia historica pasiva. No ejecutar navegacion automatica.
Ejecutable recompilado y enlazado; input_expr PASS. Iniciar mediante
recomp/tools/run_prime_keyboard_mouse.ps1 para activar teclado/raton.
Enter/clic izquierdo = A; Espacio/clic derecho = B; WASD = palanca;
Shift = Z; Ctrl = C; raton = puntero.
La prueba de 40 minutos fue solo de titulo/demostracion: no acredita
30 minutos jugables. ButtonPressing sigue pendiente de verificacion
en ejecucion. Checkpoint 20261022_manual_qa_automation_removed.json.


El lanzador manual run_prime_keyboard_mouse.ps1 incorpora un observador
pasivo separado watch_prime_hang.ps1. Tras 15 segundos de ventana sin
respuesta intenta guardar una captura nativa en recomp/.tools/. No pulsa
controles ni termina/reinicia el juego. -NoHangCapture permite desactivarlo.
El muestreo pausa brevemente cada hilo y lo reanuda en finally; resuelve
simbolos con base ASLR y etiqueta palabras de pila como candidatas, sin
afirmar unwind completo. Pruebas de fixture de captura, simbolizacion y
salida del observador tras terminar su proceso PASS; captura del bloqueo
real y correccion siguen pendientes. No se recompilo el juego por este
cambio de herramientas. Checkpoint 20261022_passive_hang_observer.json.


Actualizacion 2026-10-23: las mediciones de la escena pesada muestran un hilo GX saturado y coste dominante en resolucion de texturas estaticas. El cache de snapshots ahora indexa claves por hash sin quitar la validacion exacta de bytes. Build Windows y pruebas CTest 21/21 pasan; falta medir FPS en la misma escena con recomp/tools/run_parallel_candidate.ps1 -PerformanceTrace.


La prueba en el hangar del indice hash (23 de octubre) mantuvo el coste GX de texturas estaticas en torno a 190-196 ms/s; no se observa mejora suficiente de FPS. Siguiente candidata: validacion por generacion de escritura rastreada con fallback de bytes exactos.


Actualizacion 2026-10-23: la candidata de generacion de escritura compila, supera CTest 19/19 y fue abierta por tools/run_parallel_candidate.ps1. En la misma vista del hangar marco 28.4 FPS (35.17 ms/frame, P95 46.79 ms); el trace registro ~1.72 M consultas al cache, ~1.716 M aciertos por generacion y 100 comparaciones exactas. El hilo principal sigue al 100% y no se alcanzan 60 FPS. La lectura de paletas aun registra cero aciertos por generacion, por lo que su coste queda pendiente.

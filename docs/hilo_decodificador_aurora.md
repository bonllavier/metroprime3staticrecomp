# Decodificador de Aurora en hilo propio (etapa 1)

Fecha: 25 de octubre de 2026. Opción **experimental, desactivada por defecto**: F1 → Rendimiento → "Decodificador de Aurora en hilo propio", clave `aurora_decode_thread` en `[prime]` de Config.toml, variable `WII_COMPILED_AURORA_DECODE_THREAD=1`. Requiere reiniciar el juego.

## Problema que ataca

En las zonas pesadas (visor de escaneo, salas grandes) el hilo "Prime GX consumer" llega al 80-93 % de ocupación y el juego baja a 40-50 FPS. Ese hilo hacía dos trabajos seguidos:

1. Interpretar la ráfaga de comandos de la Wii (listas de dibujo, expansión de vértices, espejo de registros BP/CP).
2. **Aurora**: decodificar esos mismos comandos (`fifo::process`), resolver texturas y preparar los dibujos para la GPU. Cada escritura BP (`GXApplyBPReg`) y cada copia o enlace de textura hacía un `drain()` síncrono, así que nunca se acumulaba nada.

Con la opción activa, el trabajo 2 corre en el hilo "Aurora GX decoder". Medición esperada: el hilo GX baja de ~90 % a ~55 % en esas zonas; el decodificador queda en ~35-40 %.

## Diseño

- **Un único escritor del stream**: el hilo que tiene el candado GX (`GxGpuThread::Lock`): el intérprete, o el host (VI/overlay) dentro de un `Lock`. Escribe en el búfer de Aurora (`fifo::write_*`).
- **Chunks** (`aurora-main/lib/gx/stream_queue.hpp`): `hand_off()` entrega el búfer completo al decodificador y toma otro del pool; `commit()` lo hace cuando el búfer pasa de 32 KiB, o de 2 KiB con el decodificador ocioso. Los cortes solo ocurren en límites de comando (GXEnd, escrituras BP, callbacks). Cola de 16 MiB / 1024 chunks con contrapresión.
- **Todo lo que lee o cambia estado del decodificador viaja por el stream**:
  - Registros BP: `GXApplyBPReg` escribe los 5 bytes `GX_LOAD_BP_REG`.
  - Listas XF/indexadas (`GXCallDisplayList`): bytes copiados al stream.
  - Dibujos directos (`submit_raw_draw`): cabecera + vértices al stream.
  - Trabajo de Prime (`runtime/include/hle/prime_gx_stream.h`, `gx_stream.cpp`): token PE, enlace de texturas, carga de TLUT, enlace de paletas, copia de textura, copia XFB. Van como `GX_LOAD_AURORA_HOST_CALLBACK` y el decodificador ejecuta el handler **fuera** del candado del renderer, en orden.
- **Lo que necesita el estado al día hace `aurora_stream_sync()`** (entregar y esperar): `GxGpuThread::Lock` (presentación, begin/end de frame, overlay), `GX_HLE_PrimeDrawDone` (hilo del juego), `GX_HLE_PrimeResetFifo`, `EnsureAuroraFrameActive` cuando no hay frame activo, y cualquier `drain()` no convertido (seguro por defecto: `drain()` = `sync()` en el escritor, no-op en el decodificador).
- **Latencia**: el token PE y la copia XFB fuerzan `hand_off()`; el hilo GX entrega lo pendiente al quedarse sin ráfagas (bajo el candado); sin hilo GX, la lectura del token PE por la CPU entrega.
- **Hilos**: el decodificador nunca ejecuta código del juego ni el servicio de temporización (`ServiceGuestTimingDuringAuroraFrameWait` lo salta; Aurora también).
- **Errores**: una excepción en el decodificador se relanza en el escritor en la siguiente operación, como hace el hilo GX.

## Invariantes que no se deben romper

1. El decodificador es el único que toca `g_gxState` y `g_primeTlutMemory` mientras la opción está activa. En el escritor, `PrimeGxStream::Async()` decide emitir en lugar de llamar. **Tampoco se lee** `g_gxState` desde el escritor: con el hilo activo va atrasado respecto al stream (véase el primer error corregido más abajo).
2. Un chunk nunca se corta dentro de `GXBegin..GXEnd`. `hand_off()` lo garantiza consultando `fifo::in_begin()`; el hilo GX solo entrega lo pendiente al vaciarse la cola si no hay un dibujo multi-ráfaga abierto (`GX_HLE_PrimeInsideDraw()`).
3. El token PE se publica en el decodificador: el juego reutiliza su memoria de staging al verlo, y Aurora lee RAM del juego al decodificar.
4. `GXSetTexCopySrc/Dst` y `GXSetDispCopySrc/Dst` escriben estado del decodificador directamente: por eso las copias llevan sus parámetros en el comando (`GXCopyTexWithParams`, `GXCopyDispTrackedWithParams`).
5. Orden de candados: `GxGpuThread::Lock` antes de `g_viMutex`; nunca al revés (el decodificador toma `g_viMutex` al marcar el XFB listo).
6. Un handler de callback (hilo decodificador) nunca toca la sombra del SDK (`__gx`) ni escribe en el FIFO. Si su trabajo depende de estado del SDK pendiente (VCD/VAT/BP sucios), el escritor llama `GXAuroraPublishDirtyState()` antes de emitir el callback.

## Errores corregidos el 25 de octubre de 2026

Síntoma: con la opción activa, error fatal `unmapped vtx attr 9` en la pantalla de ranuras de guardado. En `console.log`, antes del fatal: datos flotantes de vértices decodificados como opcodes (`unknown opcode 0x73`), dibujos absurdos (`vtxCount=15310`), `GXEnd: vertex data not evenly divisible`. Checkpoint: `recomp/checkpoints/20261025_aurora_decode_thread_fix_unmapped_vtx_attr.json`.

1. **El escritor leía estado del decodificador para decidir si reemitía registros CP.** `GXSetVtxDesc`/`GXSetVtxAttrFmt` comparaban con `g_gxState.vtxDesc`/`vtxFmts` para detectar que una lista de visualización cruda (`GXCallDisplayList`: listas solo de registros, listas aplanadas) había cargado VCD/VAT a espaldas de la sombra del SDK. Con el decodificador asíncrono la comparación veía el valor viejo, no se reemitía nada, y cuando el decodificador aplicaba la lista su tamaño de vértice ya no coincidía con el del escritor. Corrección: banderas en el escritor (`fifo::vertex_cp_unknown_vcd/vat`): `GXCallDisplayList` recorre la lista (NOP, BP, CP, XF, XF indexado, CALL_DL anidado; se detiene de forma conservadora en un dibujo o comando Aurora) y marca el estado como desconocido si carga CP 0x50..0x9F; `__GXSetVCD`/`__GXSetVAT` lo vuelven conocido al emitir fuera de una grabación. En modo síncrono se mantiene además la comparación antigua, así que ese camino no cambia.
2. **Un chunk se cortaba dentro de un dibujo.** El hilo GX llamaba `aurora_stream_flush()` al vaciarse la cola de ráfagas aunque hubiera un dibujo multi-ráfaga abierto; `hand_off()` reiniciaba el búfer entre `GXBegin` y `GXEnd`. Corrección: `hand_off()` no actúa con `in_begin()`, y el flush del hilo GX exige `!GX_HLE_PrimeInsideDraw()`. Si vuelve a ocurrir, el decodificador escribe `asynchronous decoder: chunk ends inside a draw` como error.

3. **El callback de paletas leía estado de vértices antes de que llegara.** `GX_HLE_PrimeBindDrawPalettes` emitía el callback `BindPalettes` y recién después `submit_raw_draw` publicaba el estado sucio del SDK (VCD/VAT) al stream. En el decodificador, `GXRawDrawTextureMask` construía la configuración del shader con el VCD anterior (en el primer dibujo del arranque, `POS = GX_NONE`, de ahí `unmapped vtx attr 9` en la pantalla de título sin ninguna desalineación previa). Además, esa función ejecutaba `__GXSetDirtyState()` desde el hilo decodificador: toca la sombra del SDK y escribe en el búfer del escritor. Corrección: `GXAuroraPublishDirtyState()` (escritor) se llama antes de emitir el callback, y `GXRawDrawTextureMask` solo publica estado sucio cuando corre en el escritor. Regla: un handler de callback nunca toca `__gx` ni escribe en el FIFO; si necesita estado del SDK, el escritor lo publica antes de emitir el callback.

4. **Un reinicio del FIFO (PI) descartaba comandos no decodificados.** `abort_vertex_writer()` vaciaba el búfer del escritor completo; en modo síncrono solo contiene el paquete `GXBegin` incompleto, pero con el hilo activo contiene todo lo escrito desde la última entrega (registros incluidos). Ahora, en modo asíncrono, recorta solo el paquete abierto y conserva el resto en orden; `clear_buffer()` marca el estado CP como desconocido.
5. **Clave de pipeline corrupta persistida.** La sesión desalineada del primer error generó un dibujo sin atributo de posición y su clave quedó guardada en `UserData/Cache/pipeline_cache.db` (hash `F01B5E303E9996E5`). Desde entonces, cada arranque la compilaba en el precalentamiento y disparaba el mismo fatal con la pantalla aún en negro, con la opción activada o no. Se eliminó de la caché del usuario (copia en el scratchpad de la sesión). Protección añadida: `valid_pipeline_config` exige posición y tipos de atributo 0..3; la carga de la caché borra las filas inválidas en lugar de solo saltarlas; el decodificador salta con error (`draw ... with no position attribute`) un dibujo así en vez de crear su clave; y `tools/prime_pipeline_seed.py merge` descarta esas filas (`invalid=N`) para que nunca lleguen a la semilla publicada.

La época `GXAuroraVtxStateEpoch` (memo de `gx_stream_common.h`) pertenece al escritor: el decodificador asíncrono ya no la incrementa; `GXCallDisplayList` la incrementa cuando marca el estado CP como desconocido.

Pendiente de revisión (etapa 2): escrituras directas del escritor en `g_gxState` que no pasan por el stream: `GXSetZScaleOffset`/`GXSetScissorBoxOffset` (GXTransform.cpp), política de viewport (GXAurora.cpp), filtro de copia y bounding box (GXFrameBuffer.cpp). No producen desalineación del stream, pero pueden aplicarse a un dibujo anterior al previsto.

## Cómo probar

- Activar la opción, reiniciar, y confirmar en `console.log` la línea `Asynchronous GX decoder started`.
- Jugar: visor de escaneo, transformación del personaje, sombras en el piso, cinemáticas, menú de pausa, guardar y cargar, cambio de sala, apagar y volver a abrir.
- Grabación F8 en una sala pesada: el perfil ahora incluye el hilo `decoder` (5 hilos: game, gx, decoder, frame_worker, presenter).
- Si algo falla: capturas F7/F5 y `console.log`; la opción se desactiva en F1 y todo vuelve al camino síncrono.

## Pendiente (etapas siguientes)

- Medir y ajustar los umbrales de chunk.
- ~~Revisar `GxGpuThread::Idle()` (estado CP) si algún camino del juego dependiera de que la GPU terminó de dibujar y no solo de leer el FIFO.~~ Sí dependía: resuelto el 01/11/2026 (sección de la lava de Bryyo).
- Diagnósticos del volcado F7 emitidos desde el escritor usan `current_frame()` del productor; pueden desfasarse un frame.

## Etapa 2: expansión de vértices en el decodificador (30/10/2026)

En la cinemática de Norion el hilo GX seguía al 89 % con el decodificador al 5 %: la expansión de vértices de las listas de dibujo (`ExpandMappedAttribute<2>`, 19 % propio) y el resto de `TrySubmitPrimePackedDl` corrían en el hilo GX. Ahora un dibujo empaquetado cuya expansión no puede fallar (`PrimeDlVertices::ExpandCannotFail`: todos los atributos indexados mapeados en todo su rango de índices) viaja como `GX_LOAD_AURORA_DEFERRED_DRAW`. La carga útil (`runtime/include/hle/prime_deferred_draw.h`) lleva el plan de atributos y los índices codificados. El decodificador la expande en línea dentro de `process()`, con `PrimeExpandDeferredDraw` y su propia caché de vértices, y la entrega a `handle_draw` como un dibujo normal: se fusiona y se agrupa igual. Si un lote de staging se llena, el comando se reintenta entero.

- El escritor publica antes el estado de vértices y paletas, y `GXAuroraPublishDirtyState()` (lo que vaciaría `GXBegin`).
- El decodificador lee los arrays del juego más tarde que antes. Es seguro por la misma razón que ya lo es con las texturas y las listas: los puntos de sincronización del juego (token PE, draw done, reinicio del FIFO) esperan al decodificador.
- Los dibujos con lecturas índice a índice (arrays no mapeados, grupos NBT3) siguen en el hilo GX, igual que sin decodificador asíncrono.
- `WII_COMPILED_PRIME_DEFER_DL_EXPANSION=0` desactiva solo esta etapa, para hacer A/B.
- Pruebas en `prime_dl_vertex_expansion_tests`: `ExpandCannotFail`, la codificación y decodificación de la carga útil, y que la expansión diferida es idéntica byte a byte.

Corrección del mismo día (grabaciones de las 21:10 y 21:27): diferir todos los dibujos convirtió al decodificador en el cuello de botella (78-82 % ocupado en Norion y en la cinemática de Dane, 58 % en juego normal). Norion siguió a 48 FPS y escenas que iban bien bajaron a 56-58. Ahora el hilo GX difiere un dibujo solo si el decodificador no tiene nada en cola ni en curso (`aurora_stream_decoder_idle_hint`, contador atómico `StreamQueue::m_outstanding`, sin candado). Si no, expande él mismo y la carga se reparte sola. Además, el hilo GX y el decodificador se mantienen fuera del núcleo reservado del juego (`aurora::host_cores::keep_off_game_core`): un hilo gráfico cargado no debe compartir núcleo físico con el hilo del juego (la explosión de la torreta estaba limitada por el hilo del juego, al 93 %). Nota de análisis: las grabaciones F8 antiguas se simbolizan con el exe actual, así que sus nombres de funciones no valen; los porcentajes de ocupación por hilo sí.

Ajuste (grabación de las 22:05, entrada a Norion): con la condición «nada pendiente» el decodificador solo recibió un 10 % (el hilo GX siguió al 81 % y expandiendo el 21 %), porque el escritor entrega un chunk cada 16 KiB y casi siempre hay uno en vuelo. Ahora se difiere mientras el decodificador tenga como mucho un chunk pendiente (`aurora_stream_decoder_backlog_hint() <= 1`). Si se atrasa, la cola supera ese valor y la expansión vuelve sola al hilo GX. La otra caída de esa grabación (s 74-75, 49 FPS) es lógica del juego: el hilo del juego al 96 %, con un 74 % en código del juego (`func_8027351C`) y el runtime en su 14 % habitual.

## Retrazado sin drenar el decodificador (31/10/2026)

Grabación F8 `stack_profile_20261031_000702.txt`: el hilo del juego esperaba al decodificador (`StreamQueue::sync`) un 3,3 % de su tiempo. El 2,9 % venía del primer bloqueo de `AdvanceRetrace` (`vi.cpp`) y solo el 0,4 % de la presentación por copia. Ese bloqueo procesa eventos de ventana (`UpdateAuroraAndProcessEvents`) y abre el fotograma si no lo está, pero como `GxGpuThread::Lock` drenaba toda la cola del decodificador en cada retrazado.

`GxGpuThread::HostLock` (`gx_gpu_thread.h/.cpp`) excluye al consumidor GX y espera la fase SEALED del frame worker (`aurora_wait_for_frame_worker_sealed`, nueva: ImGui recibe la entrada ahí), pero no drena el decodificador. Un `Lock` dentro de un `HostLock` asciende (consumo del dibujo partido y `aurora_stream_sync`) y, al salir, el `HostLock` vuelve a considerarse sin drenar. Lo que se procesa con él no toca estado GX ni del decodificador: SDL/ventana, ImGui IO, entrada, teclas F (volcado F7, captura F11 e informe F12 dejan peticiones con mutex o atómicos), mando, cursor del mouse look, banderas de superficie.

En `AdvanceRetrace`:
- **Primer bloque:** usa `HostLock` cuando el fotograma ya está abierto. Si hay que abrirlo (`BeginAuroraFrame`, que cambia el estado del renderizador), mantiene el `Lock`.
- **Segundo bloque:** solo toma el `Lock` si la presentación es posible (fotograma abierto y XFB listo que coincide con el actual, o negro fuera de Prime) según el estado VI ya copiado. Si no, vuelve a procesar eventos con `HostLock` cuando antes también lo hacía.

Exe del 31/10 00:21, `test_graphics.ps1` en verde. QA pendiente:
- F1 (abrir, cerrar, cambiar opciones)
- F11, F12, F8
- Alt+Enter y redimensionado de ventana
- Minimizar y restaurar
- Conectar y desconectar el mando
- Salir al título
- Grabación F8 en Norion

## Arrays que el juego reescribe: la lava de Bryyo (01/11/2026)

Síntoma (usuario, F5 `capture_20261101_041007` y `041019`): la lava salta entre dos niveles de brillo (p. ej. 227 / 210) sin patrón fijo y su superficie se mueve de forma errática, también sin tocar nada y con todos los efectos apagados; desactivar el decodificador lo mitiga. Causa: la etapa 2 aplaza la expansión de un dibujo empaquetado al decodificador, que lee los arrays del juego más tarde, y el reparto de carga decide aplazar o no dibujo a dibujo (`aurora_stream_decoder_backlog_hint() <= 1`). Los fluidos reescriben sus arrays (ondas) cada fotograma: los fotogramas aplazados leían valores de otro momento. La regla de la etapa 2 («los puntos de sincronización del juego esperan al decodificador») no cubre arrays que la CPU reescribe entre dibujos sin sincronizar con la GPU.

Cambio (`gx_dl.cpp`, `ArraysWrittenRecently`): un dibujo solo se aplaza si ningún gránulo de 64 KiB de los rangos que leen sus índices (`PrimeDlVertexCache::ReadSpans`) recibió una notificación de escritura (`GxGuestWrite::g_generations`: DC flush/store, DMA) en el último segundo, y si todos son rastreables (no espejo sin caché, sin atributos de varios grupos). Los demás se expanden en el hilo GX al enviarse, como sin decodificador. Registro: `[prime-packed-indexed-dl] kept on the GX thread: arrays written recently`. Pendiente: confirmación del usuario con el decodificador activado; si queda algo, probar `WII_COMPILED_PRIME_DEFER_DL_EXPANSION=0` para separar la etapa 2 de la 1 (texturas y listas leídas tarde).

Segunda causa (F5 `capture_20261101_043659`, decodificador activado, ya con el cambio anterior): el brillo sigue alternando (109 / 103) y el patrón de la textura de lava salta. Entre los dos estados, el dibujo de la lava es idéntico (geometría, tres etapas 512x512 + 512x512 + 32x32, mismas texturas), luego lo que cambia son las matrices de textura. El juego las carga con `GX_LOAD_INDX_A..D` (carga XF indexada): el escritor solo pasaba al flujo la base del array (`ApplyIndexedXfArrayForPacket` → `GXSetArray`) y el paquete de cinco bytes, y el decodificador leía las 4..16 palabras de matriz de memoria del juego más tarde, cuando la CPU ya había escrito el desplazamiento de la onda del fotograma siguiente.

Cambio (`gx_stream_common.h`, `SubmitIndexedXfImmediate`; llamado desde `gx_dl.cpp` `OnIndexedXf` y la ruta LOAD_INDX de `gx_fifo.cpp`): con el decodificador activo, el escritor resuelve la carga indexada en el momento en que el juego la emite. Copia las palabras desde `base + índice * stride` y envía un `GX_LOAD_XF_REG` equivalente con los datos en línea. Sin decodificador, o si el array no es válido, queda el camino anterior. Regla general: ningún dato que el decodificador lea de memoria del juego puede depender de que la CPU no lo haya reescrito antes, salvo que exista un punto de sincronización con la GPU. Pendiente: confirmación del usuario.

Causa raíz (F5 `capture_20261101_051505`, cámara quieta, decodificador activado; las dos correcciones anteriores no bastaron):
- **Síntoma:** la lava se mantiene en un brillo de ~145 y, en un solo fotograma cada ~11, sube a 185-192. En ese fotograma aparece una costura punteada entre parches de la superficie: los bordes de parches vecinos quedan a alturas distintas. Es la «geometría irregular» que vio el usuario.
- **Lo que no es:** la capa de 3 texturas tiene matrices y vértices continuos, y el cociente entre fotogramas no es suave (no es SSAO): sigue el relieve de las ondas.
- **Mecanismo:** el juego sabe que la GPU terminó un tramo por el registro de estado CP (bits de inactividad y de breakpoint alcanzado, `PrimeCpUpdateStatus` en `memory.cpp`), y entonces reutiliza sus búferes. Ese estado solo miraba `GxGpuThread::Idle()`, es decir, al consumidor GX, no al decodificador. Con el decodificador activo, el juego recibía «breakpoint alcanzado» mientras el decodificador aún leía arrays de vértices, matrices o texturas de lo ya entregado. Reescribía las alturas de las ondas y unos parches salían con las alturas viejas y otros con las nuevas. Este era el pendiente «revisar `GxGpuThread::Idle()`» de la etapa 1.

Cambio:
- `PrimeCpUpdateStatus` exige además `aurora_stream_decoder_backlog_hint() == 0`. El consumidor entrega su búfer al decodificador antes de quitar `busy`, así que el contador cubre todo lo anterior al breakpoint.
- `StreamQueue::m_outstanding` usa release/acquire. `stop()` y el fallo del decodificador lo ponen a cero (si no, el juego sondearía su breakpoint para siempre), y un bloque que termina tras `stop()` ya no lo decrementa.
- Prueba en `aurora_stream_queue_tests`.

Efecto esperado: el juego espera al decodificador en sus breakpoints, como esperaba al hardware. Si baja el FPS en salas pesadas, hacer una grabación F8; el hilo del juego mostraría la espera en el sondeo del estado CP.

`ArraysWrittenRecently` y `SubmitIndexedXfImmediate` quedan como protección, pero tienen coste en el hilo GX. Si el usuario confirma la lava, evaluar quitar `ArraysWrittenRecently` para recuperar la etapa 2 en los fluidos.

Diagnóstico añadido a F5 (`dump_draw`): `tmN=slot:...` (matrices de texgen tal como las tiene el decodificador), `ptN=` (post-matriz) y `vh=` (hash de los vértices preparados). Nota: con el disco lleno, F5 deja archivos vacíos que el proceso mantiene abiertos (`Device or resource busy` hasta cerrar el juego).

### Detector de lecturas tardías y arrays por valor (01/11/2026, tarde)

El cambio del estado CP no bastó: el usuario siguió viendo la geometría errática de la lava al dispararle (ondas), y solo con el decodificador activado.

**Detector**
- `GxGuestWrite` numera cada notificación con una época (`g_epoch`; `g_lastEpoch` por gránulo y un anillo de los 8192 rangos exactos más recientes).
- Cada ráfaga del FIFO lleva la época de cuando el juego la escribió (`Entry::epoch`, `t_consumerEpoch`).
- La carga de un dibujo aplazado también lleva esa época (`Packed::epoch`).
- `NoteLateArrayReads` (gx_dl.cpp) clasifica cada dibujo empaquetado como lectura tardía de sus mismos bytes, solo de vecinos del gránulo, o indeterminada. Lo registra cada 5 s en `[prime-late-array-read]` cuando hay algo.

**Resultado** (dos partidas disparando a la lava, decodificador activado y desactivado)
- Mismos bytes: 0 en el hilo GX y 0 en el decodificador.
- Todo lo marcado fue de vecinos del mismo gránulo: falsos positivos del primer detector, que solo miraba gránulos.
- El juego publica unas 9.000 escrituras por segundo (~150 por fotograma).
- Conclusión: la expansión de Prime está limpia; la diferencia estaba en otro camino.

**Causa**
- Las listas que no expande Prime pasan sus arrays a Aurora como puntero (`ApplyAuroraArraysForDisplayList` → `GXSetArray`).
- Aurora sube el array en el primer dibujo del fotograma de host que lo usa, en el decodificador, es decir, tarde. Para entonces el juego puede haber reescrito ya la malla de ondas.
- Los parches expandidos por Prime y los decodificados por Aurora mostraban alturas de momentos distintos.

**Cambio**
- Subcomando `GX_LOAD_AURORA_ARRAYDATA` (0x50 + atributo: tamaño, bandera LE y bytes), escrito por `GXSetArrayCopy` (GXGeometry). El decodificador guarda su propia copia por atributo.
- Con el decodificador activo, `ApplyAuroraArrayIfChanged` envía por valor los arrays escritos en el último segundo (`RangeWrittenRecently`), incluidos los de matrices indexadas de las listas.
- La copia se reenvía solo si cambia la generación del rango o un dibujo lee más allá de lo copiado. Así la malla de ondas viaja aproximadamente una vez por fotograma.
- Los arrays reempaquetados (stride > 255), que usaban un búfer de dos posiciones, también van por valor.
- `ApplyIndexedXfArrayForPacket` olvida la copia (`GxForgetCopiedArray`).
- Registro `[prime-array-copy]` cada 10 s. Prueba `ArrayData_DecoderKeepsItsOwnCopy`.
- Pendiente: confirmación del usuario y medir el coste.

### Copias de los arrays en los dibujos aplazados (01/11/2026, noche)

**Resultado del arreglo anterior:** `[prime-array-copy]` no apareció ni una vez. La lava no usa ese camino. El usuario siguió viendo grietas punteadas rectas entre parches de la lava y geometría que parpadea al disparar o cuando los animales caen en ella (capturas de pantalla del usuario).

**Hipótesis:** todas las protecciones anteriores (`ArraysWrittenRecently`, el detector de épocas, `RangeWrittenRecently`) dependen de que el juego notifique sus escrituras (DC flush/store, DMA). Si la malla de ondas se escribe sin notificación (por ejemplo, por el alias sin caché), se la cree estática, los parches se aplazan al decodificador y este la lee tarde. El detector de épocas no podía verlo. Los parches (4-12 vértices) no pasan por la caché de vértices (mínimo 48).

**Cambio, independiente de las notificaciones:**
- `SmallArraySnapshots` (gx_dl.cpp): al aplazar un dibujo cuyos índices leen como mucho 4 KB de arrays, el hilo GX copia esos bytes en la carga (`PrimeDeferredDraw::Snapshot`; `Packed::snapshotOffset/Skip/Size`, `Header::snapshotBytes`).
- `Decode` apunta `arrayData` a la copia. La expansión usa el camino vértice a vértice, que lee la copia porque todos los índices del dibujo están en el rango copiado. La caché de vértices del decodificador se omite para estos dibujos.
- El decodificador compara la copia con la memoria viva y registra cada 5 s `[prime-deferred-snapshot] draws=N game_had_changed_the_array=M` cuando el juego ya había cambiado el array. Es la prueba, por contenido, de la lectura tardía.
- Prueba en `prime_dl_vertex_expansion_tests`: tras reescribir el array, la expansión da los valores anteriores.
- Se quitó el detector de épocas del hilo GX (coste por dibujo). La infraestructura de épocas queda en `GxGuestWrite` y en las ráfagas.

### Causa aislada: la etapa 2 (01/11/2026, noche)

- **Datos:** `[prime-array-copy]` nunca aparece (ese camino no se usa en el juego). `[prime-deferred-snapshot]`: 8 cambios en 406.282 dibujos. Los datos de vértices llegaban bien.
- **Opción nueva** `[prime] defer_dl_expansion` (F1 > Rendimiento, "Expand draws on the decoder thread", predeterminada activada, aplica al reiniciar). Exporta `WII_COMPILED_PRIME_DEFER_DL_EXPANSION`.
- **Prueba A/B del usuario:** con el decodificador activado y esta opción desactivada, **la lava se ve bien** al dispararle y con los animales.
- **Hipótesis y cambio:** la caché de vértices del decodificador (`ExpandPackedDraw`, dibujos de 48 vértices o más) valida con las generaciones de escritura. El juego reescribe las ondas sin notificar, y el reparto de carga manda cada dibujo al decodificador solo algunos fotogramas, así que su entrada podía tener varios fotogramas. Ahora `PrimeExpandDeferredDraw` no usa la caché, y la copia de arrays sube a 16 KiB.
- **Pendiente:** prueba del usuario con la opción activada. Si se confirma, revisar también la caché del hilo GX para arrays que se escriben sin notificación.

### Verificación y causa: el juego reescribe la malla mientras el hilo GX la lee (01/11/2026, 7:30)

- **Modo `[prime] defer_dl_verify`** (`WII_COMPILED_PRIME_DEFER_VERIFY`): el hilo GX también expande cada dibujo aplazado y el decodificador compara su resultado (hash en `Header::verifyHash`). Es solo diagnóstico.
- **Resultado:** ~15 millones de dibujos idénticos. Solo 8 diferencias, todas al disparar a la lava (7:23): parches consecutivos de 94 vértices (`cmd=0x98`, posiciones `0xfc0aa0`, normales `0xfd9be0`), aunque llevaban copia. El juego estaba reescribiendo la malla en el mismo momento en que el hilo GX la leía.
- **Sincronización del juego:** arma un breakpoint del FIFO por fotograma (600 cada 10 s en `[prime-gpu-thread]`), lo que permite que la GPU vaya hasta un fotograma por detrás. En la Wii la GPU termina mucho antes. Aquí el hilo GX puede procesar los parches del fotograma N mientras el juego escribe las ondas del N+1 en el mismo array, sin notificación.
- **Por qué solo con la etapa 2:** sin ella, el hilo GX va tan cargado que lee después de que el juego terminó de escribir (malla completa, un fotograma de desfase invisible). Con ella va más ligero y lee durante la escritura: vértices compartidos leídos en momentos distintos y cuñas entre parches.

**Cambio (vista coherente por fotograma):**
- El decodificador marca como volátil un array cuya copia ya no coincide con la memoria (`MarkVolatileArray`, `[prime-volatile-array]`).
- Para esos arrays, el hilo GX lee cada byte una sola vez por fotograma (`FrameCoherentAttributes`, vistas que se extienden con cada parche; el fotograma lo marca la copia de pantalla, `GxPrimeNoteDisplayCopy`). Todos los parches que comparten un vértice ven el mismo valor y la superficie queda cerrada aunque la lectura coincida con la escritura del juego.
- Estos dibujos no se aplazan y se expanden sin la caché de vértices.

**Opciones nuevas:** `defer_dl_expansion`, `defer_dl_verify` y `dl_vertex_cache` (las dos últimas solo para diagnóstico).

### La malla cambia de dirección al volver a entrar (01/11/2026, 7:50)

Prueba del usuario (registro `base_1793536381`): la lava quedó cosida un rato y las cuñas volvieron al regresar a la sala desde la estación de guardado. La malla aparece en otra dirección en cada carga (`0xfd9be0`, `0x100bde0`, `0xcab820`), y al volver las entradas de la caché de vértices pasan de 209 a 583.

Cambio: `NoteSnapshotChanges` marca todos los arrays del dibujo, porque un dibujo marcado deja de aplazarse y su otro array ya no podría detectarse; los 16 huecos del registro se reciclan. No hay confirmación todavía.

Límite conocido: la detección sigue dependiendo de un cruce poco frecuente (3 en 3,7 millones de dibujos), así que puede haber cuñas hasta que salte. Siguiente prueba si persiste: `[prime] dl_vertex_cache = false`. La solución comprobada sigue siendo `defer_dl_expansion = false`.

### Decisión para la versión 0.1.6 (01/11/2026)

El decodificador queda activado por defecto y `defer_dl_expansion` desactivado: con la etapa 2 activa, las grietas de la lava siguen sin resolver. Se retiró el detector de épocas. `defer_dl_verify` y `dl_vertex_cache` solo existen en las compilaciones de prueba (`MKW_PRIME_QA_TOOLS`).

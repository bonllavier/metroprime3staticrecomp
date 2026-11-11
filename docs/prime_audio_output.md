# Salida de audio del runtime Prime

Checkpoint 2026-10-22, RM3E01 disco 0 revision 0. Salida real al mixer Windows comprobada: la version `ba8672b0ad452c320f91ddd3035cb81811450cb33a6632458445e7246396c462`, PID 5484, produce PCM no nulo y CoreAudio mide un pico de sesion de 0.4048427 (40.48%). Escucha humana, sincronizacion y estabilidad prolongada siguen pendientes. Abrir un stream SDL por si solo no demostraba esta salida.

Los registros DSP audio DMA `0xCC005030..0xCC00503A` solo almacenaban valores, sin transferir muestras ni emitir AID. La primera candidata DMA `30b05609885042e5e7e818abe0180867c6e5215f86f1814341cabb1411970879` uso erroneamente el bit mailbox DSP `0x80`. Se retiro tras reproducir una ventana negra: el handler de mensajes DSP espera un mailbox que no existe para una transferencia AID. La version `62ef3e09...` fue restaurada como ejecutable canonico durante la correccion.

Ghidra confirmo en el DOL original SHA-256 `6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104`:

- `AIInit 0x804BE930` registra `0x804BEAB0` en el slot OS 5 y desmascara `0x04000000`.
- `0x804BEAB0` reconoce AID con `control & 0xFF5F | 0x08`, y llama al callback en `0x8067E130`.
- `0x804C0990` inicializa el anillo AX, registra `0x804C0788` y programa 384 bytes de DMA.
- `0x804C0788` prepara el frame siguiente mediante `0x804C0528` y actualiza el buffer programado.
- El slot 7/handler `0x804C5514` corresponde al mailbox DSP: reconoce `0x80` y espera un mensaje. No es AID.

La fuente candidata corrige el bit AID y despacha el handler OS original del slot 5. El mailbox sigue usando su ruta separada; no se despacha IOS IPC desde el tick DMA. El modelo consume 32 bytes cada 250 microsegundos, conserva el buffer latched durante la transferencia y recarga base/longitud al terminar. Se elimina la entrega directa de cuadros AX al host para evitar duplicacion o saltarse el anillo original.

Tambien faltaba atender el reloj DMA en el spin del scheduler propio de Prime: los pumps anteriores dependian de esperas host que el titulo no siempre ejecuta. `VI_HLE_PollRetraceGuestSpin` ahora llama al servicio de audio diferido desde su frontera segura de 1 ms. Antes habia muestras en RAM pero la cola SDL recibia rafagas aisladas; con este servicio la cola permanece continua y CoreAudio mide sonido real. Los mensajes DSP repetitivos tienen un presupuesto de traza para evitar escrituras ilimitadas por frame.

La salida conserva PCM entero de 16 bits, 32 kHz, estereo; convierte endian y orden R/L a L/R sin compresion adicional. SDL adapta esa fuente al dispositivo real `Main Output (Komplete Audio 6 WDM Audio)`: WASAPI, 44.1 kHz, estereo float32, gain 1. No se modifico la configuracion del dispositivo Windows. Esto no acredita por si solo equivalencia completa del mezclador AX HLE con el DSP original.

Referencia de hardware consultada: [Dolphin DSP.h](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSP.h) y [DSP.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSP.cpp), definiciones de AID/mailbox y DMA FIFO. Las rutinas RM3E01 se comprobaron por separado en el DOL propio.

20/20 pruebas de plataforma pasan, incluidas las nuevas comprobaciones de DMA, ritmo, anillo, AID separado de mailbox y reconocimiento W1C sin borrar el otro evento. La candidata compilo y enlazo; la ventana responde y presenta el titulo original. El log `recomp/build/RM3E01_00_runtime_link_probe/UserData/Logs/base_1792713591_pid5484/console.log` confirma el callback AID original, PCM no nulo, dispositivo sin pausa y gain 1. Las mediciones CoreAudio muestran la sesion activa, sin mute, volumen 1, y el mismo pico no nulo en la sesion y en el endpoint por defecto.

Evidencia con hashes y limites: `recomp/checkpoints/20261022_prime_audio_aid_route.json` y `recomp/checkpoints/20261022_prime_audio_coreaudio_live.json`. La version se deja abierta para QA manual; no se afirma que todas las escenas o formatos de sonido esten validados.

`recomp/tools/inspect_windows_audio_sessions.py --pid PID --seconds 3` consulta CoreAudio de solo lectura: endpoint, estado, mute, volumen y pico de la sesion del proceso. No cambia ajustes ni genera sonidos. El backend registra driver/dispositivo, primera cola PCM y primer bloque no nulo. Las entradas del juego permanecen manuales.


## Radio y voz pendiente, 22 de octubre

El usuario confirma audio en intro y menus y efectos de los simbolos, pero ninguna voz del operador. FPS habilitado en Config.toml; aproximadamente 17.7 FPS en la captura de cockpit anterior al cambio IPC. No se considera una mejora de rendimiento.

Inspeccion de solo lectura del PID4740 en la radio: speech group 0x809761E0 con volumen 0.75 y lista de canales vacia. Un actor streamed-audio de grupo speech, 0x80FF7BA0, conserva muestra 0x808E6200 / sonido FMOD 0x8127C0C0 en open-state 1 (cargando), sin handle. Su identificador es 386A20FE8EAC8AD0; no se identifica aun como una linea concreta de dialogo. El hilo FMOD 0x80E7E308 esta suspendido con count 1 dentro del callback de lectura 0x80332E8C: espera DVD de Darksamus-mae-32.fsb, bloque 0x80EAE980 con estado terminado y transferencia 0x10000/0x10000. La lista de callbacks 0x8067DE94 ya esta vacia. Una lectura de musica anterior puede bloquear la cola de carga posterior; esto es una hipotesis fundamentada, no prueba de que la radio acepto el codigo.

El wrapper original 0x80332E8C somete la lectura, despues suspende el hilo; su callback 0x80333228 llama al tail-wrapper 0x80332D5C de OSResumeThread. Nuestro IPC publicaba una lectura en la misma pila de submission, permitiendo consumir el wake antes de la suspension. Candidata 4f95e13cf4d98f68bc792fb81b6d3c0b361fc6dcc984b2acc76aeeaa8b1ad2a6 introduce un deadline de lectura: 600 us + transferencia a 32 MiB/s, el limite inferior de una lectura ya bufferizada documentado por Dolphin DVDInterface.cpp. No copia implementacion GPL ni simula seek/rotacion; no es un modelo completo de tiempos del disco. No bloquea el host con sleep. Conserva FIFO y mailbox ocupado, reconoce aceptacion por separado y publica respuestas vencidas al consultar PPCCTRL/IRQ. No fuerza hilos ni completa escenas desde el host.

Referencia de medidas: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DVD/DVDInterface.cpp (READ_COMMAND_LATENCY_US y BUFFER_TRANSFER_RATE). Implementacion propia en prime_ipc_reply_queue.h / memory.cpp. Las 21 pruebas de plataforma pasan: incluye deadlines, FIFO, callback unico y ausencia de overflow. Build y enlace pasan. Ejecutable anterior respaldado; candidata abierta como PID7304. Arranque/titulo siguen avanzando; voz y radio requieren QA manual, aun no declarados corregidos.

inspect_prime_radio_audio.py ofrece un resumen de solo lectura sin suspender hilos, inyectar controles ni guardar dumps completos. Vistas PPC32 parciales contrastadas con el DOL original; las lecturas concurrentes no son atomicas. No se toca matching ni la ISO.

Validacion adicional candidata PID7304: open-ginga.fsb progresa por bloques de 64 KiB hasta offset 0x150000 y el grupo music tiene canal activo. CoreAudio mide pico de sesion 0.4526716, volumen 1 y sin mute. Los dos hilos FMOD de streaming esperan en colas (state4/count0), sin reproducir en esta observacion el anterior park state1/count1 tras lectura terminada. Aun falta validar la escena radio y la voz con el usuario.

QA humano confirmado el 22 de octubre: el usuario responde "Ahora se oye y avanza" para la voz del operador y la continuacion tras el codigo de radio en 4f95e13c. Se conserva como base actual. Esto valida la correccion funcional de esa escena, sin extenderla a fidelidad DSP completa, rendimiento ni estabilidad prolongada. Siguiente paso: continuar QA manual despues de la radio; controles sin automatizacion y FPS visible.


## Revisión de fidelidad, 23 de octubre

- El mezclador AX emulado ya reproduce el microcódigo (SRC polifásico con la ROM de coeficientes, filtros por voz, rampas): no se modifica.
- La única conversión del host era el remuestreador de SDL (32 kHz -> dispositivo): -0.36 dB y residuo -28 dB a 12 kHz, -3.6 dB a 15 kHz. Se sustituye por `audio_resampler.h` (polifásico racional Kaiser, 128 taps/fase): plano hasta 14 kHz, -0.24 dB a 15 kHz, residuo <= -104 dB. El stream se abre en la frecuencia y formato F32 del dispositivo, así SDL no convierte nada. `WII_COMPILED_AUDIO_SDL_RESAMPLER=1` vuelve al de SDL.
- Deriva de reloj AI/tarjeta (~0,02 %) vaciaba la cola en minutos. Ahora el ritmo de la DMA AI emulada sigue la cola (máx. ±0,1 %, control proporcional sobre media de 2 s); las muestras no se tocan. Prueba de 5 min: +200..+285 ppm, cola ~53 ms, 0 underruns.
- `MusicAttenuation::TickGuest` queda inactivo en Prime: seguía punteros de Mario Kart en RAM de Prime.

Evidencia: `../checkpoints/20261023_audio_fidelity_review.json`. 22/22 pruebas PASS; el usuario reporta sonido "un poco más claro".

## Audio de los vídeos THP, 30 de octubre

Síntoma (usuario): los vídeos con sonido (game over `Death_Movie.thp`, finales, `HyperDeath_*`) suenan dañados, con artefactos; la imagen va bien. Los vídeos del menú no tienen pista de audio. Todos los THP con audio son estéreo a 32 kHz, igual que la salida AI, así que no hay conversión de frecuencia ni interviene el remuestreador del host (que solo ve la salida final). La grabación F8 de las 01:17 contiene el game over (s 88-106, DVD estable a ~2 MB/s) a 59-61 FPS, y el log de esa sesión no muestra underruns: el defecto está en el PCM que produce el juego.

Ruta del juego (DOL RM3E01): `fn_803E6E58` encadena en el callback de DMA AI un envoltorio (`fn_803E6ED8`) que llama primero al callback de AX (`0x804C0788`) y luego a los mezcladores registrados; el reproductor de vídeo registra `fn_80354D34` (vía `fn_80355034`). Ese mezclador, como el THPPlayer del SDK, lee `AIGetDMAStartAddr` (el búfer de AX recién programado), reprograma la DMA a su doble búfer `lbl_805CC840`, invalida el búfer de AX (`DCInvalidateRange`) y suma en él el audio THP (`fn_80354DF4`). El audio THP se decodifica con cada fotograma (`fn_80353F70` -> `fn_804EE544`).

Causa: `__AXOutNewFrame` (`fn_804C0528`) arranca la mezcla del DSP y el callback programa en la DMA el búfer que el DSP está mezclando en ese momento (la DMA lo toma 3 ms después). En la Wii el DSP lo termina mientras la CPU hace el resto del trabajo del fotograma de sonido; con el hilo de mezcla (`Mix audio on its own thread`, activado por defecto) el reproductor podía leerlo a medio escribir. La DMA ya esperaba al hilo (`JoinMixWorkerIfWriting`); la lectura del reproductor no.

Corrección: overlay de `DCInvalidateRange` 0x804DF8C8 en `prepare_prime_runtime_hooks.py` que, al entrar, llama a `PrimeBeforeGuestDcInvalidate` (`ax_mix.cpp`): espera solo si una mezcla pendiente o en curso escribe en ese rango. No notifica a las cachés GX (regla de `copias_efb_y_cache.md`). Comprobación rápida sin recompilar: con la opción del hilo de mezcla desactivada (F1 > Audio, se aplica al momento) la mezcla es síncrona y el vídeo debería sonar limpio.

## Música atascada en bucle, 31 de octubre

Síntoma (usuario): al entrar a Norion la música se trabó y repitió un trozo en bucle hasta que empezó la pista siguiente. Grabación F8 `stack_profile_20261031_005503.txt`: el streaming de música lee 64 KiB del disco cada 1-2 s. Tras una carga de zona en s 55 (15,7 MB), no hubo ninguna lectura de disco de s 60 a s 78. En s 79 se reanudó con la pista nueva. El registro no tiene errores de disco.

Mecanismo (el mismo de la radio del 22 de octubre): el envoltorio de lectura de FMOD `0x80332E8C` somete la lectura y después suspende su hilo; el callback `0x80333228` lo reanuda con `OSResumeThread`. Si la respuesta IPC se entrega antes de la suspensión, la reanudación se pierde y el hilo queda suspendido (count 1). La protección era solo de tiempo real (200 µs desde la sumisión, `PrimeDiscReplyDelayMicros`). Si Windows quita la CPU al hilo del juego entre la sumisión y la suspensión más tiempo que eso, al volver la respuesta ya ha vencido y se entrega en el siguiente sondeo, antes de suspender. Esa noche había contención de CPU externa (Chrome, `[host-stall]` con el sistema al 100 %).

Corrección (exe del 31/10 01:04): la respuesta de una lectura `/dev/di` (ioctl 0x71) espera además a que el juego haya avanzado 256 sondeos de interrupción desde la sumisión. Se cuentan con `g_primeGuestProgress`, que incrementa `MemoryPrimeIpcPollControl` en el hilo del juego, así que no avanza mientras el hilo está desalojado. Como respaldo, a los 20 ms de tiempo real se entrega igualmente (`PrimeIpcReplyQueue::kProgressFallbackMicros`), para que un invitado que deje de sondear no se quede sin respuesta. El reposo del invitado es un bucle que sigue sondeando, por lo que no hay espera adicional cuando todos los hilos aguardan el disco. Prueba en `prime_ipc_reply_queue_tests` (puerta y respaldo).

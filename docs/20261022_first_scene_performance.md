# Primera escena: modulos y medicion de rendimiento

Revision RM3E01_00. Checkpoint: `../checkpoints/20261022_first_scene_modules_and_performance.json`.

## Fallo observado

La ejecucion manual PID 5756 llego a la vista en primera persona y fallo al llamar a `0x811D725C`, LR `0x8019F8CC`. El ultimo tramo del log carga `RSO_Friendly.rso` y `RSO_GunTurret.rso`. La direccion coincide exactamente con el prolog de Friendly: base residente `0x811CD920` + texto `0x120` + prolog `0x981C`. El caller original `0x8019F7B8`, revisado en Ghidra contra el DOL fijado, llama al puntero almacenado en modulo+0x24 despues de enlazarlo.

Se incorporan ambos modulos: Friendly, 158 funciones / 1.451 relocaciones; GunTurret, 125 / 1.245. Sus hashes originales y REL sinteticos estan fijados. El verificador independiente compara todas las relocaciones nominales; el registro comprueba hashes de fuentes y ausencia de instrucciones solapadas. La cobertura de referencias de codigo HA/LO de los doce modulos pasa con 1.293 funciones y ninguna entrada materializada ausente. Esto sigue siendo traduccion parcial, sin afirmar recuperacion completa de clases o toda llamada indirecta.

Los 16 punteros de las dos tablas switch de Friendly (datos+0x428..0x467) quedan excluidos de las raices invocables. Se comprueban los dos limites de ocho casos, referencias HA/LO y secuencias lwzx/mtctr/bctr originales. Los demas punteros de codigo de datos se contrastan con el inventario explicito. GunTurret requirio admitir stb con desplazamiento reubicado: se conserva escritura de un byte y registro fuente; cuatro pruebas del lowering real comprueban ambos signos, vecinos y registros. El build exige esa prueba antes de traducir.

## Cambios de rendimiento

El log anterior contiene 190.738 lineas / 10.226.997 bytes; 159.110 lineas son PPCMSG, PPCCTRL y cambios de hilo. Ahora esas tres trazas informativas admiten como maximo 128, 256 y 128 entradas respectivamente. Los errores siguen fuera del limite. Para un diagnostico completo se puede iniciar otro proceso con `WII_COMPILED_PRIME_VERBOSE_TRACE=1`. No se eliminan logs historicos ni se cambian operaciones IPC o planificacion.

El adaptador evitaba cargar paletas no usadas, pero calculaba informacion de shader y tomaba otra barrera incluso cuando ningun slot contenia C4/C8/C14X2. Ahora comprueba primero los ocho formatos BP y retorna si no existe ninguna textura indexada. El envio real de geometria conserva su publicacion de estado y drain. Las pruebas cubren los dos bancos BP, los tres formatos indexados y formatos normales. No se omiten dibujos ni se cambia la resolucion.

## Medicion y QA

`tools/run_prime_keyboard_mouse.ps1` activa `WII_COMPILED_PRIME_PERFORMANCE_TRACE=1` para el proceso y restaura el entorno al terminar. `-NoPerformanceTrace` permite probar sin instrumentacion. No se inyectan botones.

Cada cinco segundos de lecturas KPAD, `[prime-performance]` informa FPS y tiempos de **presentacion** existentes en Aurora. `[prime-render-cpu]` informa llamadas y milisegundos acumulados desde el reporte anterior para BP, enlace de textura, consulta de paletas, envio raw, copia de color e ISO. Son duraciones CPU de pared inclusivas: pueden contener esperas; no se deben sumar como porcentajes ni interpretar como tiempo GPU. Los valores pueden partir una operacion entre ventanas. No miden todas las funciones del renderer ni toda la ejecucion PPC.

`tools/sample_prime_performance.py --pid PID --output RUTA_PRIVADA.json` obtiene 200 muestras RIP por hilo y deltas de tiempo CPU. Suspende cada hilo brevemente y siempre lo reanuda; no escribe memoria guest ni presiona controles. La medicion perturba el proceso, por lo que no constituye un benchmark de FPS. Solo atribuye simbolos EXE dentro de secciones ejecutables; las palabras de datos ya no se etiquetan como funciones. Los simbolos cercanos y las muestras no son un stack unwind. El fixture dormido paso: muestras en ntdll y cero CPU observado. Capturas y logs permanecen en G:.

El nuevo ejecutable, SHA256 `ae4fa08677b480170e22b15b230e7d1dc1945d1e7cf64a2fba3e8ffb302aac8b`, se lanzo como PID 20496. Las primeras pantallas registraron aproximadamente 60 presentaciones/s, media 16,67 ms; esto no demuestra una mejora en la zona jugable. Falta repetir manualmente el tramo de Friendly/GunTurret, identificar el coste dominante en escena, corregir defectos graficos y validar 30 minutos de juego estable. La ISO sigue en solo lectura y el DOL original conserva su hash; no se altero el matching.


### QA: radio y velocidad irregular (22 de octubre)

El usuario confirma aceleracion de toda la escena/animaciones, no solo de la camara. Sigue sin causa probada; no se cambia el reloj ni el timestep. PID 2252 continua activo pero en radio/holograma se observan 8-13 present FPS. Las mediciones anteriores de 35-39 corresponden a otra fase.

El usuario informa bloqueo de progresion tras introducir el codigo de radio. El muestreo sigue en dibujo activo, sin fatal nuevo. Aun falta establecer si la radio acepto el codigo. La guia de Nintendo requiere despues activar un control verde/rojo a la izquierda y empujar el Wiimote hacia la pantalla. El adaptador teclado/raton actual publica aceleracion neutra y distancia fija: ese gesto posterior todavia no esta implementado. No atribuirle sin evidencia la espera de la radio.

Candidata b778ce850ee48f2e753a3c2d47f1bd5d9619cc778caf91e6e65b75c4141c097c: reutilizacion conservadora del estado de vertices entre dibujos consecutivos y cache de flags de diagnostico en rutas calientes. Build y cuatro pruebas de componentes PASS. No promovida ni ejecutada; se conserva la sesion manual. No hay mejora de FPS atribuible todavia a esta candidata. Checkpoint: recomp/checkpoints/20261022_radio_wait_and_diagnostic_cache.json.


### Entrada bruta coherente y sensor manual (22 de octubre, build pendiente)

Se confirmo en Ghidra RM3E01_00 que 0x803C2670 lee KPAD en 0x804D9138, copia una muestra procesada y, si existe el procesador de movimiento, solicita historial bruto a 0x804D9E20 antes de llamar 0x803C0A24. El lector bruto original es void: copia min(count,16) registros de 56 bytes. Una captura de solo lectura del proceso2252 en 0x805F5198 (896 bytes) encontro los 16 registros con err=-1. Esto contradice la muestra KPAD conectada del adaptador de teclado y prueba una desconexion entre las dos interfaces; no prueba aun la causa de la espera en la radio.

Se anadio un overlay reproducible para el lector bruto que, solo en modo teclado/raton y canal0, entrega la misma muestra ya publicada en KPAD. Los demas casos siguen por el cuerpo original. Conserva los registros de retorno del contrato void. El serializador WPAD ahora produce bytes PPC explicitos y se reutiliza para mandos fisicos. Se verifican dispositivo, extension, formato, botones, ejes, sticks y el paquete completo de reposo en una prueba independiente. Los objetos IR brutos siguen invalidos: no se afirma soporte completo de movimiento/apuntado fisico.

E/Q publican +/-1g en el eje longitudinal del sensor mientras el usuario mantiene la tecla; al soltar o perder foco vuelve a cero. No pulsan botones Wii ni cambian estados de la escena. El codigo original sigue reconociendo los gestos. Su suficiencia para la palanca inicial requiere QA real; no declararla validada por pasar la prueba del paquete.

Tambien se anadio [prime-vi-cadence] bajo el flag existente de rendimiento: retrazos entregados frente a tiempo real y maximos de entrega tardia/anticipada. Es observacion; reloj/timestep siguen sin parches. Las seis pruebas enfocadas pasan. La nueva candidata completa esta compilando aparte; PID2252 y su EXE siguen sin cambios. Evidencia original: recomp/checkpoints/20261022_raw_wpad_original_contract.json.


### Validacion en ejecucion de WPAD bruto (22 de octubre)

La candidata 8db728e5a541f3160f5fbaa4dc9ea48762c22cf3e3f18be997cceba543222eb7 compilo y paso 19/19 pruebas. Se cerro normalmente PID2252, se preservo su EXE en recomp/.tools/WiiCompiled_before_raw_wpad_1890.exe y se abrio PID7452. No se inyectaron botones. Los imports DLL no cambiaron.

El procesador original de movimiento ahora consume muestras reales del adaptador: dispositivo1, error0, acelerometro bruto(512,512,612); sus indices de historial pasaron de (25,26) a (260,261) en dos capturas de solo lectura. Antes permanecian en (0,0) y los16 registros SDK indicaban error-1. Esto valida el puente de entrada, no la finalizacion de la radio ni el gesto de la palanca. E/Q estan disponibles como sensor longitudinal manual, pendiente de QA del gesto.

El primer minuto del arranque registra aproximadamente60 present FPS y60 retrazos/s, sin entregas anticipadas. No extrapolar ese resultado a la cabina/holograma ni declarar solucionada la velocidad irregular. Pendientes: repetir radio, probar palanca, medir escena pesada y validar juego sostenido. Checkpoint: recomp/checkpoints/20261022_raw_wpad_live_validation.json.

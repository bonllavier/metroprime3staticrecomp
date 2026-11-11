# Salir de la partida al menú principal (RM3E01)

30/10/2026. Síntoma (usuario): al elegir Salir en una partida para volver a la pantalla principal, la imagen queda en negro y el juego no responde.

## Diagnóstico (proceso colgado pid 2200, solo lectura)

- `capture_prime_hang.py`: el hilo del juego consume el 100 % de un núcleo y todas las muestras caen en la misma instrucción de `func_802ED6C4`. El log termina en `[prime-pi-fifo-reset] frame_aborted=1`, la llamada `GXAbortFrame` de ese mismo código.
- `0x802ED6C4` es la ruta de reinicio de CMain. Desactiva la alarma, aborta el GX, guarda el bloque de *fast reset* (opciones y dos indicadores, 128 bytes en `0x811FFF80`, `OSSetSaveRegion`) y llama a `__DVDPrepareResetAsync` (`0x804CAB44`, comando DVD 0x24). Después espera con `do {} while (flag == 0)` en `0x802EDC00`: el callback `0x802E9FFC` pone 1 si el resultado del DVD es 1, y 2 en caso contrario. Con 1 llama a `OSRestart(0)` (`0x804E57C4`) y, si no, a `OSReturnToMenu` (`0x804E5850`).
- Volcado de MEM1: el bloque del comando 0x24 está en ejecución (estado 1). El comando corresponde a DI ioctl 0x7A (`DVDLowPrepareCoverRegister`), que nuestro `/dev/di` sí responde (`memory.cpp`), pero el indicador sigue a 0.
- Causa: el bucle no contiene ninguna llamada y el código traducido solo atiende las interrupciones en los límites de llamada. La respuesta IPC que dispararía el callback nunca se entrega. En la consola, esa interrupción interrumpe el bucle. No tiene relación con el cambio de sondeo de la ronda nocturna: el criterio de sondeo no cambió.
- Aunque el bucle termine, el runtime no implementaba `OSRestart` ni `OSReturnToMenu`; el reinicio intentaría arrancar de nuevo el disco mediante IOS.

## Corrección (`prepare_prime_runtime_hooks.py`, `src/hle/os/prime_host_restart.cpp`)

1. Overlay en `0x802EDC00`: cada vuelta llama a `OS_HLE_ServicePrimeIdle`, igual que los bucles de inicialización de Bluetooth y WPAD. Atiende IPC, VI y alarmas, así que el DVD completa el comando y el callback pone el indicador.
2. Overlay de entrada en `OSRestart`: el ejecutable se relanza con la misma línea de comandos y el proceso termina como al cerrar la ventana (`ExitForAuroraWindowClose`). El nuevo proceso espera a que el anterior salga, hasta 15 s, antes de abrir el disco, las cachés y los logs (`PrimeWaitForRestartedParent`, `WII_COMPILED_PRIME_RESTART_PARENT`).
3. Overlay de entrada en `OSReturnToMenu`: la consola volvería al menú Wii, así que el programa se cierra.

Pendiente: el bloque de *fast reset* de `0x811FFF80` no se conserva entre procesos; el juego vuelve a leer las opciones de la partida guardada como en un arranque en frío. Falta además la QA en el juego: salir desde la pausa, confirmar que aparece la pantalla de título y que las opciones se mantienen.

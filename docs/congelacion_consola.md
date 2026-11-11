# Congelación por consola en modo "Seleccionar" (26 de octubre de 2026)

## Síntoma

El juego se queda congelado del todo (imagen fija, sin sonido, "no responde") sin ningún error en `console.log`. Ocurre cuando se lanza desde una ventana de PowerShell o CMD y alguien hace clic dentro de esa ventana: el título pasa a "Seleccionar ..." y Windows detiene a cualquier programa que escriba en la consola hasta que se pulsa Esc.

## Mecanismo (confirmado con la captura de hilos de la sesión pid 15960)

1. El runtime redirige `stdout`/`stderr` a dos tuberías de 8 KB y un hilo por tubería las vuelca al archivo `console.log` y, como espejo, a la consola heredada.
2. Con la consola en modo selección, el hilo de volcado se queda bloqueado en `WriteFile` sobre la consola.
3. La tubería se llena. El hilo del juego, al escribir su siguiente línea de log, se bloquea en `WriteFile` sobre la tubería.
4. Los demás hilos (GX, presentador, trabajador de fotogramas) esperan al hilo del juego en sus variables de condición.

En la captura, el hilo del juego aparecía dentro de `ZwWriteFile` pasando por los módulos de enganche de Bitdefender (`bdhkm64.dll`, `atcuf64.dll`); esos módulos envuelven todas las llamadas de archivo y no eran la causa.

## Corrección

- `runtime/include/transcript_sink.h`: cada destino del log (archivo, consola) tiene su propio hilo y una cola acotada. Escribir en la cola nunca bloquea: si el destino se atasca, lo que no cabe se descarta y se anota cuántos bytes se perdieron cuando vuelve a moverse. Archivo: 16 MiB de cola; consola: 256 KiB.
- Al cerrar, el archivo tiene 5 s para vaciarse y la consola 0,5 s; si el destino sigue atascado, el proceso sale igual.
- Mientras el juego corre se desactiva QuickEdit en la consola heredada, para que el espejo siga fluyendo; se restaura al salir.
- `tools/capture_prime_hang.py` volvía a rechazar el ejecutable por su nombre antiguo; corregido. Por eso el vigilante automático no capturó nada aquella vez.

Pruebas: `mkw_transcript_sink_tests` (vaciado ordenado, escritura no bloqueante con informe de descartes, cierre acotado), dentro de `tools/test_graphics.ps1`.

## Cómo reproducirlo a mano

Lanzar el juego desde PowerShell con `recomp/tools/run_prime_keyboard_mouse.ps1`, hacer clic en la ventana de PowerShell. Antes de la corrección el juego se congelaba en pocos segundos; pulsar Esc en la consola lo liberaba.

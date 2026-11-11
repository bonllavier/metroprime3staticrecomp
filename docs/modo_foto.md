# Modo foto (RM3E01)

Petición del usuario (31/10/2026): poder detener a voluntad animaciones y cinemáticas para ajustar materiales sobre una imagen fija. Implementado; falta la QA en el juego.

## Cómo funciona

El bucle principal `0x802EDC9C` llama una vez por fotograma a la actualización `0x802EBAF4`. Esa función mide el tiempo real transcurrido, avanza los acumuladores y entrega la cola de mensajes que mueve actores, animaciones, scripts, cinemáticas y entrada. Después, el mismo bucle dibuja el mundo (`0x800D7358`). Con el modo foto activo, un overlay en la entrada de `0x802EBAF4` (`tools/prepare_prime_runtime_hooks.py`) devuelve `r3 = 1` sin ejecutarla. Un 0 haría que el bucle marcara su indicador de parada en `+0x12C` (`0x802EDFD0`). El juego vuelve a dibujar en cada fotograma el mismo instante congelado, así que los cambios de F1 (materiales, efectos) se ven en vivo.

- **F3** activa o desactiva el modo. **Flecha derecha**, solo en modo foto, deja correr una actualización: avance fotograma a fotograma.
- Mientras está activo se ocultan el contador de FPS, el aviso de mouse look y los avisos de RetroAchievements. El indicador «PHOTO MODE» se ve 4 s al activarlo o al avanzar un fotograma y después desaparece, para que **F11** saque capturas limpias.
- Al reanudar, la primera actualización ve una diferencia de tiempo grande; la propia actualización la trata como un tirón y reinicia su acumulador (rama de `FLOAT_8067dbf0`).
- **Vigilante del juego:** una alarma periódica de 1 s (`0x802EB4E4`, armada con `OSSetPeriodicAlarm` en `0x802EBA0C`) suma 1,0 al float `0x8067DC2C`, y la actualización lo pone a 0 en cada fotograma (`0x802EBB74`). Si llega a 10,0, la alarma llama a `rs_reallyfatal_printf("INFINITE LOOP")`: el manejador fatal `0x803F6160` pone la pantalla en negro, libera el event hook de STM (ioctl `0x3002`, sin soporte en el runtime) y deja el juego parado para siempre. Como el modo foto se salta la actualización, `PrimePhotoModeSkipUpdate` pone el contador a 0 en cada fotograma congelado. Fallo observado el 31/10/2026 (pid 2768): con el modo foto activo y tras «Reset to defaults» en Gandrayda, contador = 10,0, `[prime-vi-cadence] max_late_ms=10000` y después `Dropping command Draw` sin fin. Cualquier otra función del host que salte la actualización debe hacer lo mismo.
- **Rendimiento:** desactivado, cuesta una comprobación por fotograma. Activo, el juego no ejecuta su lógica y solo dibuja, así que la CPU trabaja menos que jugando.

## Límites

- La cámara queda donde estaba: no hay cámara libre.
- La música y los sonidos que ya sonaban siguen, porque el audio no depende de esa actualización.
- RetroAchievements sigue evaluando la memoria, que no cambia mientras está congelado.

Pendiente de QA: congelar una cinemática y retomarla, avanzar fotograma a fotograma, ajustar materiales con el juego congelado y salir del modo en plena partida.

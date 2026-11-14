# Copias del EFB, avisos de caché de datos y cachés de texturas (RM3E01)

Estado verificado por el usuario el 24 de octubre de 2026: sin texturas moradas, sin parpadeo de la sombra del piso y sin ruido en la transformación del personaje. Evidencia en `recomp/checkpoints/20261024_prime_dc_range_texture_staleness.json`.

## Reglas que no se deben romper

1. **Las funciones DC de Prime avisan de escrituras de la CPU.**
   - `DCFlushRange` 0x804DF8F4, `DCStoreRange` 0x804DF924, `DCFlushRangeNoSync` 0x804DF954 y `DCZeroRange` 0x804DF980 tienen overlay en `recomp/tools/prepare_prime_runtime_hooks.py`.
   - El overlay llama a `PrimeNotifyGuestDcRange` (`runtime/src/hle/os/os_cache.cpp`) al retornar, después de que `DCZeroRange` haya puesto los bloques a cero.
   - Sin esos avisos, el caché de texturas reutiliza contenido viejo en la misma dirección: texturas moradas o cambiadas, como las estrellas.
2. **`DCInvalidateRange` (0x804DF8C8) no debe avisar.**
   - Indica que terminó un DMA, incluida la copia del EFB que hace la propia GPU.
   - Los DMA reales (DVD, locked cache) ya avisan por su cuenta.
3. **Un flush no prueba que la CPU escribiera en todo su rango.**
   - `gx_copy.cpp` guarda digests XXH3 por páginas de 4 KiB bajo cada destino de copia del EFB.
   - `InvalidateEfbCopyDestinationsForRangeNow` retira la copia solo si una página afectada cambió.
   - Si se quita esta comprobación:
     - la sombra proyectada de los personajes (copias R4 de 64×64 en 0x92103a20/0x92104220/0x92104a20) alterna con RAM vieja;
     - las copias RGBA8 de 640×448 de la transformación (0x922210e0/0x92339100) se leen como ruido.
4. Las direcciones de los hooks DC de Mario Kart (0x801A1600…) no se aplican a RM3E01. No dar por hecho que un override de MKW funciona en Prime.
5. **No se llama a Dawn con `g_asyncMutex` tomado** (`efb_ram_copy.cpp`), ni se destruye un búfer con él. Dawn entrega los `MapAsync` terminados dentro de cualquiera de sus llamadas, con su bloqueo tomado, y `complete_async_slot` pide ese mutex: congelación permanente. Ver `fps_altos.md`.

## Copias de profundidad con MSAA

Con MSAA activo (F1 > Video), la v0.1.6 cerraba el juego con `Depth tex copies from multisampled EFB targets are not supported` en cuanto el juego copiaba profundidad. La sonda del visor de escaneo lo hace en cuanto se usa (A8, Z16 y Z8L de 32×32 alrededor de la retícula), así que el fallo era seguro; la sonda de destellos (Z24X8 de 4×4) pasa por el mismo código, aunque no se ha ejercitado. No se puede saltar la copia: el juego elige el objetivo del escaneo con esos bytes.

- **Corrección (2 de noviembre de 2026):** `tex_copy_conv` tiene una variante que enlaza la profundidad como `texture_depth_multisampled_2d` y lee la muestra 0, una superficie real. No se promedia: entre dos superficies daría una distancia que no existe. Los pipelines solo se crean si la sesión arranca con MSAA.
- **Comprobado:** el replay `hangar` a 2x reproducía el fallo antes del cambio y llega al final sin errores después; a 1x también llega al final sin errores, con las mismas copias de profundidad.
- **Sin comprobar:** el valor de los bytes frente a 1x y el escaneo real con MSAA. La copia A8 de IDs de objeto sale del color ya resuelto, así que en los bordes el alfa es la media de dos IDs y puede dar un ID que no existe. Puede notarse como un objetivo mal elegido o un borde de color raro en el visor. Si pasa, habría que leer el alfa de la muestra 0 del color sin resolver, como en la profundidad.

## Punto oscuro en el centro de la pantalla

El usuario lo vio el 2 de noviembre de 2026 en las vistas 3D: una marca de unos 3×2 píxeles en el centro exacto, encima de todo y en el mismo sitio aunque cambie la escena. El wiki de Dolphin describe un punto parecido en Prime 2 y 3 por encima de 1x.

- **Origen:** `0x803E9A34`, que el final de escena `0x803E9F78` llama justo antes de la copia a pantalla. Pone una proyección ortográfica de 640×448, matriz identidad, y envía 288 vértices: 12 vueltas de 8 triángulos, todos iguales, en (0,1), (0,0) y (1,0). No cambia el TEV ni la textura, así que el color sale del último dibujo del fotograma. Por eso es negro en unas escenas y otro color sobre la mira. Su función en la consola no está identificada.
- **Por qué solo se ve escalado (hipótesis):** el triángulo mide medio píxel del EFB y su hipotenusa pasa justo por el centro del píxel (320, 223), así que a 1x puede no cubrir ninguna muestra. A 3× cubre varios píxeles. No está comprobado en una Wii real.
- **Corrección:** el overlay de `0x803E9A34` en `prepare_prime_runtime_hooks.py` escribe 0.0 donde el bucle escribe 1.0. Se conservan la proyección, el estado GX, `Begin`/`End` y el número de vértices; los triángulos quedan sin área a cualquier resolución.
- **Comprobado:** replay privado `hangar_3x_window` (`recomp/qa/runs/hangar_3x_window/20261102_032129_dot_fix`), con 22 capturas de la imagen de GPU a 2816×1344 sacadas con `WII_COMPILED_GPU_CAPTURE_MARKER`. En las 11 con la mira centrada, el centro es verde puro y no hay ninguna marca; en el resto tampoco se ve. Las capturas F7 del 1 de noviembre, anteriores al cambio, sí la tienen. No hay una captura previa a 3x con la que comparar.
- **Sin comprobar:** los vídeos pregrabados (THP), donde el usuario también ve una rayita. Si sigue ahí con este cambio, la causa es otra.
- **No confundir:** el círculo verde de la mira es otra cosa. Son dibujos del propio HUD del juego, con textura de 128×128 y color constante verde, junto a las tres puntas de la retícula. No se ha tocado.

## Cómo depurar

- **F7:** un fotograma.
- **F5:** ráfaga de 8 fotogramas seguidos, quieto frente al problema.
- Cada dibujo de la captura tiene una línea `  tex` con la textura resuelta para ese dibujo: tamaño y formato GX, puntero de datos y textura del host enlazada. Si la textura del host tiene el tamaño de la copia escalada y `hf` = formato de la copia, es una copia del EFB; si mide lo mismo que la textura GX, se leyó de la RAM.
- Las líneas `[dump-copy]` del `console.log` dan la dirección de destino de cada copia durante la captura.
- Herramientas de análisis: `burst_analyze.py`, `bmp2png.py` y `bmpdiff.py` (sesión del 24 de octubre). Vale la pena moverlas a `recomp/tools` si se vuelven a usar.

## Escrituras tardías de las sondas sobre memoria reutilizada (13/11/2026)

Síntoma (usuario, Linux): dos cierres al salir de la cabina de la nave al control normal (`[prime-mouse-look]` pasa de `morph=3` a `morph=0` justo antes). El primero, `rs_reallyfatal_printf` con `Alloc failed ... Size: 134744072` (`0x08080808`); el segundo, `CTexture::Load` desde `fn_803506B0` con un puntero de textura `0x83008383` (acceso a `0x8300838D`). Ambos valores tienen forma de píxeles de sonda (Z8/A8), no de punteros ni de tamaños.

Hipótesis (no reproducida en una grabación): las sondas que se leen de forma asíncrona (destello de lente, objetivo del visor de escaneo; `kAsyncReadbackMaxBytes`) se escriben en la RAM del invitado un fotograma después de su `GXCopyTex`. Si en ese fotograma el juego libera el búfer de destino (al salir de la cabina rehace el HUD y sus sondas) y otro objeto ocupa esa memoria, la escritura tardía lo corrompe.

Cambio (`efb_ram_copy.cpp`, `harvest_mapped_slots`): cada ranura guarda los bytes que escribió la última vez (`AsyncSlot::lastWritten`). El juego solo lee estas sondas, así que si al llegar la siguiente lectura la memoria ya no contiene esos bytes, tiene otro dueño: no se escribe, se olvida el registro (una sonda viva se vuelve a escribir en el fotograma siguiente) y se anota `[efb-ram] stale async readback skipped dest=… size=… (count N)`. La primera escritura de cada ranura no se comprueba.

Validación: repetición `hangar_1x_window` completa, 59,10 FPS, 0 escrituras descartadas (sin falsos positivos en Bryyo). Pendiente: prueba del usuario saliendo de la nave varias veces; si vuelve a caer, buscar esa línea en `console.log`.

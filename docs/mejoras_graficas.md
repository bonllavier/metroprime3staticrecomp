# Mejoras gráficas opcionales del host (RM3E01)

Fecha: 25 de octubre de 2026. Las dos opciones de este documento viven en la capa host (Aurora y el runtime), no tocan la lógica del juego, la ISO ni el matching. Ambas están desactivadas por defecto en una configuración nueva y se controlan desde `Config.toml` (`[video]`) o desde el overlay F1 > Video. En la configuración del usuario quedaron activadas (`per_pixel_lighting = true`, `texture_upscale = 2`, `texture_upscale_max_size = 256`).

## Iluminación por píxel (`per_pixel_lighting`)

- **Qué hace.** La fórmula de iluminación XF de GX (luces difusas, atenuación de foco y distancia, canal especular, redondeo a 8 bits) se evalúa en el shader de fragmentos en vez del de vértices. Aurora traía el camino como constante de compilación `UsePerPixelLighting = false`; ahora es un bit de `ShaderConfig` (`perPixelLighting`, antes relleno a cero) que `populate_pipeline_config` pone a 1 solo cuando algún canal de color tiene iluminación activa. Los materiales sin luces conservan la clave por vértice, así que la semilla de pipelines publicada los sigue cubriendo.
- **Cómo se aplica.** `aurora_set_per_pixel_lighting(bool)` cambia `aurora::gx::g_perPixelLighting` en caliente; los pipelines del otro modo siguen en caché. La normal interpolada se renormaliza en el fragmento salvo cuando es el vector cero de un draw sin normales (mismo guard que el vértice).
- **Pipelines nuevos y cómo se evita la pantalla a oscuras.** Cada material con luces necesita un pipeline nuevo en este modo. La primera sesión del 25 de octubre con la opción activa mostró salas y brazos de Samus negros: con `skip_unready_pipelines = true` el draw se omite mientras su pipeline compila, y los pipelines por píxel no estaban en ninguna caché. Dos medidas lo resuelven:
  1. **Precalentado de variantes.** `load_pipeline_cache_entries` crea, para cada receta GX cacheada con iluminación, la variante por píxel cuando la opción está activa (no se persisten filas nuevas: se derivan de la semilla en cada arranque y la caché de blobs de Dawn las hace instantáneas a partir de la segunda sesión: 5.318 pipelines en 2,3 s).
  2. **Pipeline de respaldo.** `DrawData::fallbackPipeline` guarda la clave por vértice del mismo material; `gx::render` la usa si el pipeline por píxel aún no está listo, en lugar de omitir el draw. Con la caché de Dawn vacía (condición de instalación nueva) la sala y los brazos se dibujan iluminados desde el principio mientras el precalentado continúa.
- **Qué cambia en pantalla.** Luces dinámicas y focos con caída redonda en lugar de triangular, brillo especular concentrado en trajes y metales, sin bandas de Gouraud en polígonos grandes. Los lightmaps horneados, las sombras por copia EFB y el TEV no cambian.
- **Coste.** Más trabajo por fragmento (hasta 8 luces por canal). Presentación a 60 FPS en la Olympus en todas las pruebas.

## Reescalado de texturas al cargar (`texture_upscale`, `texture_upscale_max_size`)

- **Qué hace.** `lib/gfx/texture_upscale.cpp` amplía cada nivel de mip con Super-xBR (2x, o 4x aplicando 2x dos veces) en CPU y sustituye la textura original por la ampliada. No hay archivos, volcados ni IA: todo ocurre en memoria durante la carga. Implementación propia del método publicado por Hyllian (MIT), acreditada en `THIRD-PARTY-NOTICES.md`.
- **Asíncrono, sin tirones por el filtro.** La textura original se sube primero, igual que sin la opción. Un hilo propio decodifica a RGBA8 y reescala; `apply_completed()` (hilo GX, al principio de `resolve_sampled_textures`) crea la textura GPU nueva y la intercambia dentro del mismo `TextureRef`, de modo que todas las cachés que guardan el handle (instantáneas raw, memos por objeto, bind groups) ven la versión ampliada sin volver a resolver nada. El intercambio espera tres frames desde la programación para que la decodificación CMPR por GPU, que se codifica en el frame siguiente, actúe sobre la textura original.
- **Dónde engancha.** `resolve_static_texture` y `resolve_static_palette_texture` en `lib/gx/gx.cpp`: primero `find_cached` (contenido ya reescalado en esta sesión), si no, creación normal y `schedule`. Prime carga todas sus texturas por registros BP (objetos "raw", marcados `no_cache`); la caché de instantáneas raw conserva el handle mientras los bytes fuente no cambien y el módulo cachea por hash XXH64 del contenido (presupuesto 768 MB de GPU).
- **Qué no se toca.** Formatos de profundidad, texturas con mips arbitrarios (efectos con imágenes distintas por nivel), vídeo THP (YUVA), texturas menores de 8 px o mayores que `texture_upscale_max_size` (128, 256 o 512). El objeto GX (filtros, wrap, rango LOD) no cambia: con el mismo número de niveles la GPU elige un nivel más abajo a distancia y solo cambian los texels magnificados. El modo de wrap decide si el filtro envuelve o fija los bordes, para no crear costuras en texturas repetidas.
- **Coste.** Paso 1 y 2 de Super-xBR por filas en el pool de decodificación (64x80: ~2 ms; 256x256 con seis mips: decenas de ms, ya fuera del hilo GX). Al cargar la Olympus se reescalan 320 texturas de hasta 256x256. El overlay muestra reescaladas, reutilizadas, omitidas, pendientes, caché GPU y CPU acumulada.
- **Límites conocidos.** No inventa detalle: suaviza escalones y contornos, no sustituye un pack HD. Las texturas CMPR conservan la cuantización de bloques del original. Los mapas de indirección (refracción, visor) también se procesan porque su uso no se conoce al cargar; si aparecen artefactos en esos efectos, el siguiente paso es excluirlos por etapa indirecta. Cambiar la opción en caliente afecta a las texturas que se carguen desde ese momento.

## Pantalla de título y perilla de nitidez (25 de octubre, tarde)

- El título se dibuja con una sola textura CMPR de 832x456 (más texturas de 512 px) que el límite de 256 px dejaba fuera. Se añadió el valor 1024 a `texture_upscale_max_size`; con él la textura se reescala a 1664x912 (confirmado en el log), pero el aspecto apenas cambia: la imagen original es blanda y de poca resolución para 1920 px, y Super-xBR conserva bordes sin inventar detalle. Comparaciones en `compare_title_256_vs_1024.png` y `compare_title_sharpen.png`; el usuario confirmó que no aprecia diferencia.
- Nueva perilla `texture_upscale_sharpen` (0.0 a 1.0, slider en el overlay): máscara de desenfoque 3x3 sobre RGB tras el reescalado. A 0.5 el título sigue casi igual; queda a 0.0 por defecto y en la configuración del usuario.
- Los destinos de copias EFB leídos desde RAM (640x448 y 320x224 I8 del bloom) cambian cada frame y ahora se excluyen del reescalado: antes generaban trabajo continuo en el hilo (pendientes constantes).
- Para mejorar de verdad el título haría falta arte de mayor resolución: un reemplazo de textura (ya soportado, uso personal) o un reescalado por IA fuera del juego, opciones que el usuario descartó por ahora.

## Validación del 25 de octubre de 2026

Ejecutable final `recomp/build/RM3E01_00_runtime_link_probe/WiiCompiled.exe` (SHA-256 `a8f3775b51cecc84dd7b1f170c0a55b1d729339a7180150640ffc85198776fbc`). Todas las ejecuciones en modo teclado/ratón (`WII_COMPILED_PRIME_KEYBOARD_MOUSE=1`), Enter en la selección de partida a los 30 s, capturas con `tools/capture_prime_window.ps1`; las imágenes y los `console.log` están en `UserData/Logs` (`window_pid*_<etiqueta>_*.png`, `grid_*.png`).

| Prueba | Opciones | Resultado |
| --- | --- | --- |
| A (pid 10776) | píxel off, reescalado 2x | Iluminada, 60 FPS. Usuario: texturas correctas, pequeño tirón. |
| B (pid 15152) | píxel on, reescalado off | Iluminada, 60 FPS. Usuario: estable. |
| Ambas, primera sesión (pid 8592, 10232) | ambas on, antes de la corrección | Salas y brazos negros: pipelines por píxel compilando con draws omitidos. |
| D (pid 18392) | ambas on, variantes precalentadas, sin respaldo | Sala iluminada, brazos negros hasta terminar el precalentado (5.318 pipelines, 50,2 s). |
| E (pid 8100) | ambas on, segunda sesión | Correcta; precalentado 2,3 s con todos los blobs de Dawn en caché. |
| F (pid 18916) | ambas on, caché de Dawn vacía, reescalado asíncrono y respaldo | Correcta a los 60 y 80 s mientras seguían compilando (2.218 pipelines a los 80 s); 320 texturas reescaladas. Usuario: bien, tirones muy leves. |
| G (pid 18992) | ambas on, cachés calientes | 600 frames cada 10 s, sin errores, 320 texturas reescaladas, pendientes 0. |

Pendiente de QA del usuario: apreciar el cambio de iluminación en escenas con luces dinámicas (disparos, focos) y decidir el tamaño máximo de reescalado (512 cubre las paredes grandes a cambio de más CPU y memoria).

Evidencia: `recomp/checkpoints/20261025_prime_per_pixel_lighting_texture_upscale.json`.

## Límites de memoria del reescalado (25 de octubre, tarde)

- Con `texture_upscale = 4` y `texture_upscale_max_size = 1024` el proceso terminó con `std::bad_alloc` en la pantalla de título (pid 18576): la imagen de 832x456 a 4x son 6 millones de texels y el filtro necesita unos 48 bytes por texel de salida en memoria temporal, además del resultado, sobre un sistema con 16 GB y 5,6 GB de archivo de paginación.
- Ahora cada trabajo se limita a 4 millones de texels de salida (2048x2048): por encima la textura se queda en el original y se registra `exceeds the ... texel limit`. Un `std::bad_alloc` dentro del hilo de reescalado se captura y solo omite esa textura.
- Las texturas que cambian de contenido en la misma dirección en pocos frames (copias EFB releídas desde RAM: 640x448 y 320x224 I8) se dejan en el original tras el segundo cambio; antes generaban trabajo continuo en el hilo.

## Texturas magenta en Norion y decisión sobre Super-xBR (25 de octubre, noche)

- **Síntoma.** Con el reescalado activo (2x o 4x) aparecen polígonos magenta en la estructura central de Norion ("Get Generator A Back Online"); sin reescalado no aparecen (ejecuciones pid 11340 con iluminación por píxel desactivada: magenta; pid 7820 con reescalado desactivado: correcto). La iluminación por píxel queda descartada como causa.
- **Evidencia.** `WII_COMPILED_TEXTURE_UPSCALE_TRACE=1` muestra que varias texturas CMPR 512x512 sin mips de Norion ya contienen entre un 4 % y un 15 % de texels magenta en la decodificación CPU (`BuildRGBA8FromCMPR`), antes del filtro; la lógica de ese decodificador coincide con el shader de decodificación GPU que usa el camino normal. Los píxeles magenta de la captura F7 `capture_20261025_093637` son magenta oscurecido por la iluminación (56,7,49), es decir, contenido de textura. Hipótesis pendiente: esas regiones son relleno o texels transparentes (modo CMPR con alfa 0 y color intermedio) que el camino normal nunca muestra y que el reescalado hace visibles por mezcla sin premultiplicar o por un cambio de muestreo; no se ha confirmado. `WII_COMPILED_TEXTURE_UPSCALE_DUMP=<dir>` vuelca fuente y resultado en BMP para seguir la investigación.
- **Decisión del usuario.** Super-xBR se queda desactivado (`texture_upscale = 0`): el resultado en texturas 3D no le convence, es un filtro pensado para arte 2D. El código permanece como opción experimental con los límites de memoria y las exclusiones descritas arriba; no debe activarse por defecto ni recomendarse hasta resolver el magenta.


## Metal mejorado sin efecto en la piel (27 de octubre de 2026)

- **Síntoma.** El metal mejorado también hacía brillar como metal la piel de Samus sin traje (y previsiblemente la de Dane y los cazarrecompensas).
- **Causa.** El efecto se activa en todo material con reflejo de entorno por normal (texgen `GX_TG_NRM`), y el juego usa ese mismo reflejo en la piel.
- **Corrección.** Etiquetas de textura (`texture_replacement::set_texture_tag_list`, etiqueta `TagNoMetal`): cada dibujo suma las etiquetas de sus texturas, calculadas una vez por textura subida (`TextureRef::tagBits`), y en los dibujos con piel `build_uniform` envía intensidad de metal 0. No cambia ninguna clave de pipeline ni crea shaders. La misma infraestructura sirve a la máscara de SSAO de la cara (`TagNoSsao`, ver `ssao_aurora.md` #19).
- **Lista RM3E01** (`runtime/src/post_effects.cpp`, `kSkin`): cara y ojo vistos por el visor (`aa70669d17e36772`, `0dc7a7f288795c1a`), cara, atlas de piel con manos y oreja y ojo sin casco (`1420cb1e12575132`, `fabed183c7759daa`, `a908b39735a0c171`), de las capturas F7 del 27/10 a las 22:49. Pendientes: Dane, los cazarrecompensas y otros personajes; para añadirlos, F7 en la escena y copiar el hash de `<captura>.textures/`.

## Materiales mejorados por textura (27 de octubre de 2026)

- **Objetivo.** Dar sombreado PBR (relieve sacado de la textura, especular GGX con las luces del juego, reflejo procedural) a superficies identificadas, usando solo las texturas originales. Por decisión del usuario, sin reescalado ni IA.
- **Hallazgo.** Con el F7 ampliado (línea `mat`: `ppl`, `metal`, `lit`, máscaras de luces, fuentes de texgen), los cascos de la flota y del G.F.S. Olympus son materiales con luz dinámica e iluminación por píxel, pero sin texgen de normal (`texgen=4` o `4,4`). Por eso el metal mejorado no les llegaba.
- **Diseño.** Cada textura puede pertenecer a una clase de material (bits 2-4 del byte de etiquetas de `texture_replacement`). Si un dibujo con una textura de clase es por píxel e iluminado, se usa la variante `enhancedMetal = 1` de su material. Esa variante sale de una tabla propia (`material_pipeline_state`), para no reescribir la tabla de la que depende la memoria de `resolve_pipeline_state`, y usa como respaldo el material iluminado por vértice mientras compila. La intensidad y los parámetros de la clase viajan en la cabecera de uniformes de cada dibujo (`g_drawMetal`). La piel (`TagNoMetal`) tiene prioridad y fuerza intensidad 0.
- **Clases RM3E01** (`runtime/src/post_effects.cpp`, `ApplyMaterials`): 1 = casco de las naves de la Federación (25 texturas, intensidad 0,45, rugosidad 0,40, metálico 0,70, brillo de borde 0,30, relieve 1,0); 2 = pintura de la nave de Samus (1 textura, 0,35 / 0,22 / 0,35 / 0,40 / 0,60). Excluidos a propósito: cúpula verde, paneles holográficos turquesa, degradados y franjas de luz.
- **Opción.** `[video] enhanced_materials` (por defecto sí), F1 > Video > «Enhanced materials»; necesita iluminación por píxel. Las variantes nuevas no se precalientan: se compilan la primera vez que aparecen (la caché de Dawn las conserva) y mientras tanto se dibujan con luz por vértice.
- Pendiente: QA visual del usuario en la escena inicial y en el Olympus; más clases (piedra, hormigón, orgánico) con el inspector de materiales.

### Ampliación del 28 de octubre de 2026

- **Piel sin metal:** se añadieron la cara, las orejas y el cuello del comandante Dane (`031fbb0783e1baaa`) y el ojo de Gandrayda (`6629d875d7a329ba`). Su uniforme y su armadura conservan el metal.
- **Cazarrecompensas** (F7 de las 01:21, puente del Olympus). Los tres ya tenían reflejo de entorno y recibían el metal genérico del traje. Ahora cada uno tiene su clase: 3 = Ghor, exoesqueleto (`730ddbecf4cf6b33`; metal cepillado: 0,45 / 0,30 / 0,85 / 0,40 / 1,0); 4 = Rundas, armadura cristalina (`a877691538c72be2`; cristal pulido: 0,40 / 0,12 / 0,25 / 0,80 / 0,40); 5 = Gandrayda, piel azul y cristales (`2037db7fad0b0396`; brillo suave de piel: 0,18 / 0,55 / 0,05 / 0,30 / 0,30). Valores: intensidad / rugosidad / metálico / brillo de borde / relieve. Dependen de la opción «Enhanced materials».
- Se retiró la casilla de diagnóstico magenta del SSAO por petición del usuario, tras confirmar la máscara del visor. Para diagnóstico sigue `WII_COMPILED_POST_DEBUG_VIEW=mask`.

## Escarcha de Gandrayda (31/10/2026)

Petición del usuario: un material más actual para Gandrayda sin perder su diseño, con pequeños destellos en variaciones de morado que reaccionen a la luz. Captura usada: F7 del 28/10 a las 01:21:35 (puente del Olympus, primer plano). Su cuerpo es la clase 5 (`2037db7fad0b0396`, 512×512), con la UV del modelo en la primera coordenada (`texgen` TEX0) y reflejos de entorno en las demás.

Ampliación del 31/10/2026 (F7 del Aurora Unit, 16:27–16:28): el usuario vio que bajar el brillo solo oscurecía partes de la cabeza y el pecho. La piel opaca usa `2037db7fad0b0396`, pero encima se dibuja una capa púrpura translúcida (torso, brazos, cuello y cresta) con `62d55c9b72987c67` (512×512, siempre junto a `09d5afc2e9b0d633` y `6b476e19298806e1`, mezcla alfa normal). Se añadió a la clase 5. Como el polvo solo suma color y la mezcla lo multiplica por el alfa de esa capa, allí se ve algo más tenue. Quedan fuera a propósito el ojo (`6629d875d7a329ba`), las pestañas (`3f8483defffa49c7`) y las pasadas aditivas (`c085fa55dedefade`, `2444802414a52317` sola), para no duplicar el polvo ni ponerlo en los ojos. Ninguna de estas texturas aparece con otros personajes en los tres volcados revisados. Con la capa añadida, el usuario fijó como valores por defecto «Own glow» 0,60 (antes 0,30) y «Glint sharpness» 4 (antes 130): destellos amplios que se encienden a menudo.

- **Shader** (`aurora-main/lib/gx/shader.cpp`, bloque de metal mejorado). Se añade un término de escarcha a las clases con `aurora_set_material_sparkle(clase, cantidad, celdas por unidad de textura, tono)`:
  - Cada celda de la UV del modelo tiene un 40 % de probabilidad de llevar un destello redondo diminuto, con su propia inclinación de normal, su propio exponente de brillo y un tono de ±0,07 alrededor del de la clase, parcialmente desaturado.
  - Se enciende cuando el vector medio de una luz GX coincide con su normal, más un leve reflejo frontal del entorno.
  - Se desvanece donde un destello mediría menos de unos 2 píxeles, para que no titile.
  - Nunca se aplica en superficies con brillo propio (alfa del EFB ≥ 0,6, sus cristales).
  - Solo se genera si el dibujo tiene una coordenada de textura sacada de una UV de vértice, porque una generada desde la normal haría que los destellos se deslizaran.
- **Parámetros:** sin cambiar la estructura de uniformes. `pad.x` lleva la intensidad del metal en 12 bits (0–2) y la escarcha en los 20 restantes: cantidad (8 bits), celdas `32 + 8·n` (6 bits) y tono (6 bits). `DrawMetalOverride::sparkle` y `g_materialSparkle` por clase.
- **Gandrayda:** `aurora_set_material_sparkle(5, 0.6, 144, 0.78)`, del violeta al magenta. Una celda mide unos 3,5 téxeles de su textura. Depende de la opción «Enhanced materials».
- **Validación:**
  - `shader_golden_test` gana `AURORA_DUMP_SHADER_DIR=<carpeta>`, que vuelca la variante de metal mejorado de cada receta con iluminación por píxel: 1.714 shaders, 611 con escarcha.
  - `recomp/.session_tmp/test_scripts/wgsl_check.cpp` compila una carpeta de WGSL con Dawn en un dispositivo D3D12 sin ventana. Resultado: 1.714 sin errores; un error introducido a propósito se detecta en su línea.
- **Coste:** cambió el texto de los shaders de metal mejorado, así que Dawn recompila esos pipelines una vez, en el primer arranque.

Primera versión vista por el usuario en el juego: se notaban los puntos, pero pidió destellos más pequeños y un efecto animado, «como un universo».

### Versión animada, «polvo de estrellas» (31/10/2026)

- **Dos capas de partículas diminutas.** Celdas base de 224 por unidad de textura (unos 2,3 téxeles), y la segunda capa ×1,65. Radio de 0,07 a 0,15 de celda con núcleo brillante, presentes en el 45 % de las celdas. Las capas se desplazan en direcciones distintas: (3, 2) y (−4, 5) celdas por ciclo.
- **Titileo propio.** Cada partícula da de 2 a 5 ciclos por ciclo de animación, con su propia fase. Tiene un brillo propio tenue, que sube con la luz difusa, más el destello especular de antes cuando la luz incide en su inclinación.
- **Nebulosa.** Ruido de valor suave (retícula ×6) que se desplaza un paso por ciclo y tiñe el sombreado como mucho un 20 %, entre azul violáceo y magenta, mediante `metal_tint` sobre el difuso.
- **Animación.** Ciclo de 8 s con el reloj real; la fase va en 8 bits de `pad.x`. Todos los desplazamientos y titileos son números enteros por ciclo, así que el bucle no tiene saltos.
- **Bits de `pad.x`:** intensidad del metal en 8 bits (0–2), cantidad 6, celdas `32 + 16·n` 5, tono 5 y fase 8.
- **Lejanía.** Las partículas desaparecen cuando la celda baja de unos 0,7–1,6 píxeles, para que no aparezca ruido.
- **Gandrayda:** `aurora_set_material_sparkle(5, 0.7, 224, 0.78)`.
- **Validación:** `wgsl_check` sobre 1.714 shaders volcados, 611 con el efecto: sin errores.

### Pestaña F1 > Materials y parámetros completos (31/10/2026)

- **El cuerpo de Gandrayda no recibía el polvo.** Sus 32 dibujos usan la textura de clase 5 y la UV del modelo (F7 de las 14:18), pero el alfa de su cuerpo es la textura por una constante: el juego lo usa como brillo, y por eso el vestido morado resplandece. El polvo se apagaba donde el alfa pasa de 0,6, la protección pensada para cristales y energía. Ahora es el parámetro «On glowing areas» (`dustOnGlow`), a 1 en Gandrayda.
- **`AuroraMaterialExt`** (`aurora/gfx.h`), con `aurora_set_material_ext(clase, &p)`: 26 parámetros en coma flotante.
  - Color y brillos: brillo, saturación, especular, reflejo y tinte RGB.
  - Polvo de estrellas: cantidad, celdas, tamaño, densidad, escala de la segunda capa, deriva, titileo, brillo propio, destellos, nitidez, tono, variación de tono, saturación y núcleo blanco.
  - Nebulosa: intensidad, escala y velocidad.
  - Generales: aplicar en zonas brillantes y tamaño mínimo en píxeles.
- **Uniformes.** Los shaders de metal mejorado (no el ubershader) reciben `mat_ext: array<vec4f, 7>` al final del bloque de uniformes, detrás de `dst_alpha`. El float 7 lleva el tiempo real en segundos (cíclico cada hora), ya sin bucle de 8 s. `pad.x` vuelve a ser la intensidad en f32. Hay 8 ranuras: la 0 es el metal mejorado genérico (Samus y el resto de superficies con el reflejo del juego) y las demás, una por clase. Validado con `wgsl_check`: 1.714 shaders, 653 con `mat_ext` y 611 con polvo, sin errores.
- **Módulo `runtime/src/hle/prime_materials.cpp`.** Tabla de entradas con texturas, valores por defecto y grupos de parámetros visibles: Samus y metal reflectante, Ghor, Rundas, Gandrayda, cascos de la Federación y nave de Samus. Las de Samus son las de F1 > Video (`[video] enhanced_metal_*`) más su color y sus brillos. Gandrayda muestra además los grupos de polvo y nebulosa. El resto se guarda en `[prime_materials]` como `<entrada>_<parámetro>`, al soltar el control. Cada entrada tiene un botón para volver a los valores por defecto. Sustituye a `ApplyMaterials` de `post_effects.cpp`.

## Samus sin traje: Zero Suit, piel, ojos y pelo (31/10/2026)

Petición del usuario: mejorar el material del Zero Suit («textura cauchosa») en la escena inicial de la nave, y dar a la piel, los ojos y el pelo un material propio, sin poner nada muy brillante en la piel. Capturas: F7 del 31/10 a las 17:15:50–17:16:00 (cabina y primer plano de Samus en el asiento). Las texturas ya estaban identificadas desde el 27/10 para excluir el SSAO y el metal de la piel.

- **Clases nuevas** (`runtime/src/hle/prime_materials.cpp`, F1 > Materials):
  - 6 = Zero Suit (`f3e54ec244d7485d`): satinado gomoso. Intensidad 0,61, rugosidad 0,32, metálico 1,0, borde 0,46, relieve 0,58 (costuras), reflejos 0,60 (ajuste del usuario del 31/10; antes 0,60 / 0,32 / 0,12 / 0,20 / 0,60).
  - 7 = piel (`1420cb1e12575132` cara, `fabed183c7759daa` manos, oreja, párpado y boca): 0,70 / 0,29 / 0 / 0 / 0,15, destellos 1,6 (rugosidad 0,29 elegida por el usuario el 31/10; antes 0,58).
  - 8 = ojos (`a908b39735a0c171`): brillo húmedo pequeño. 0,40 / 0,15 / 0 / 0,15 / 0, destellos 0,8.
  - 9 = pelo (`314bb9bb1f511da7`, `939f8dd9bfe1fe85`, `18fe1fa63030e519`, `a6b9a48dfdaa5fc4`; el atlas incluye la goma roja de la coleta): brillo dorado. 0,50 / 0,14 / 0,25 / 0,15 / 0,70 (rugosidad 0,14 elegida por el usuario el 31/10; antes 0,38), reflejos 0,50.
- **Por qué la piel no puede quedar brillante:** con metálico 0, el shader no añade reflejo de entorno ni oscurece la base, y el especular GGX con f0 = 0,04 solo da un brillo tenue (con rugosidad 0,29, más concentrado pero igual de débil: f0 es 25 veces menor que en un metal). El borde a 0 evita reforzar el reflejo de entorno que el propio juego pone en la cara (`f8decb1c1b1791bd`). El relieve se queda bajo porque sale de la luminancia de la textura y en la piel convertiría el ruido en poros.
- **Alfa:** estos dibujos sacan el alfa del canal de luz más una constante 0, no de la textura, así que la protección de zonas que brillan (alfa > 0,6) no los apaga.
- **Clases hasta 15** (Aurora): el número de clase ocupa los bits 2-4 y 6 del byte de etiquetas (`material_class_bits`, `material_class_of`); el bit 5 sigue siendo la cobertura del SSAO y el 7 la marca de «sin calcular» de la caché. `g_materialClasses` y `g_materialExt` pasan a 16 entradas. Prueba: `TextureTags.MaterialClassBitsRoundTripWithoutTouchingOtherTags` (`gx_fifo_tests`).
- **Prioridad:** una clase de material gana ahora a la etiqueta de piel (`TagNoMetal`). La piel y el ojo de Samus siguen en esa lista (`post_effects.cpp`), así que vuelven a quedar neutros si se desactivan los materiales mejorados o la iluminación por píxel. El resto de caras listadas (Samus tras el visor, Dane, el ojo de Gandrayda) no tiene clase y no cambia.
- **Primera aparición:** el Zero Suit no tenía mapa de reflejo (`metal=0`), así que usa una variante de pipeline nueva. Mientras se compila se dibuja con el material normal de luz por vértice, sin tirón.
- Pendiente: QA visual del usuario en la escena de la nave; ajustar valores por defecto con sus capturas.

## Nave de Samus: barniz y separación pintura / metal (31/10/2026)

Petición del usuario: que la nave de Samus por fuera se vea realista y majestuosa, aprovechando todo lo que tiene el sistema de materiales y sin poner en riesgo los 60 FPS. Capturas: F7 del 31/10 a las 18:07–18:08 (espacio, Olympus, hangar) y del 30/10 a las 18:30.

- **Texturas** (agrupadas por la matriz del modelo de la nave en cada volcado):
  - Fuselaje: `84c129a3cbb758cd` (la única que había), `12521a534f726f17`, `8e4cdae8ef6424c8` y `50dfc739e8c8ea80`. Cada atlas mezcla pintura amarilla y roja con piezas de metal gris oscuro.
  - Cúpula verde: `c44b34a485dcf621` y `6e26862675eeb96c`, en una clase nueva, la 10.
  - Fuera a propósito: el mapa de luces `17368ba5c432c6e7` (emisivo, va con `12521a53`), el mapa de reflejo compartido `b2d14185d46dfbcd`, y los cascos de la Federación y las piezas del hangar que se dibujan junto a la nave (`9f35cb99…`, `e717a008…`, `2dc2e5fc…`).
  - El alfa de estos dibujos sale del canal de luz, no de la textura, así que la protección de brillo no los apaga.
- **Dos capacidades nuevas del shader** (bloque de metal mejorado, `lib/gx/shader.cpp`). Los parámetros van en `AuroraMaterialExt` (31 campos) y en `mat_ext`, que pasa de 7 a 8 vec4:
  - **Barniz** (`coat`, `coatRoughness`): una capa dieléctrica lisa con f0 = 0,04. Aporta un especular GGX propio con visibilidad de Kelemen, calculado con la normal geométrica y no con el relieve, como una laca sobre paneles, y sin teñirse del color de la pintura. Añade también el reflejo del entorno procedural con Fresnel de Schlick, y atenúa la base donde refleja. Sirve también para cristales: con metálico 0 el metal no refleja nada, y el barniz sí.
  - **Pintura / metal** (`paintSplit`, `bareMetallic`, `bareRoughness`): la saturación del color sombreado decide por píxel. Lo saturado (`smoothstep(0,22, 0,45)`) es pintura, con los valores de Surface más el barniz. Lo gris o muy oscuro es metal desnudo, con sus propios metálico y rugosidad, y sin barniz. La saturación no depende de la iluminación.
  - Las clases que no los usan reciben 0 y dan exactamente el mismo resultado. Ambos bloques están tras ramas uniformes, así que su coste solo existe en los píxeles de clases que los activan: unas decenas de operaciones por luz.
- **Valores por defecto:**
  - Fuselaje: intensidad 0,55, rugosidad de la pintura 0,30, metálico 0,20, borde 0,35, relieve 0,70; barniz 0,85 con rugosidad 0,07; metal desnudo con metálico 0,85 y rugosidad 0,38.
  - Cúpula: 0,60 / 0,10 / 0 / 0,30 / 0, destellos 1,2, barniz 1,0 con rugosidad 0,04.
  - F1 > Materials muestra los grupos nuevos «Clear coat» y «Paint and bare metal».
- **Validación:** `test_graphics.ps1` en verde (las recetas golden guardan el metal apagado y no cambian). Con `AURORA_DUMP_SHADER_DIR`, las 1.714 variantes de metal se compilaron con Dawn mediante `wgsl_check` (de `.session_tmp/test_scripts`, recompilado contra el Dawn del proyecto): 0 errores, 653 con el código nuevo. Un error metido a propósito se detecta en su línea.
- **Coste de caché:** cambió el texto de los shaders de metal mejorado, así que Dawn los recompila una vez en el primer arranque, como ya pasó con la escarcha.
- Pendiente: QA visual del usuario en el espacio y en el hangar, y ajuste de valores.

## Iluminación del mundo, experimental (31/10/2026)

Petición del usuario: que las paredes de los niveles dejen de verse «sin vida», con tres niveles que se activan y desactivan por separado. Por ahora experimentales y encendidos por defecto (F1 > Effects > «World lighting (experimental)»; `[effects] env_material`, `env_borrowed_lights`, `ssr`, `ssr_strength`).

- **Hallazgo (F7 del 31/10, 18:33-18:34, Olympus).** El 76–93 % de los dibujos del escenario no usa luces GX: `color = mapa de luz × difusa × 2`, con el mapa de luz en un registro (etapa 0: `C0 = ras + lightmap × C1`; después `PREV = C0 × difusa`). El mapa de luz usa la primera UV y la difusa la segunda; los vértices traen normales que el juego no usa. Otras salas (Aurora Unit, cabina de la nave) ya tienen luces GX y no cambian.
- **Nivel 1, material de entorno.** Bit `ShaderConfig::envMaterial` (antes relleno: las claves del seed no cambian). `env_lightmap_stage` reconoce el patrón por la estructura del TEV (registro con textura × difusa en un producto puro, sin sumar PREV); por UV se escogía la difusa, y la regla de «producto puro» descarta máscaras emisivas sumadas. El dibujo reutiliza el bloque de metal mejorado (especular, reflejo procedural, relieve, barniz, separación pintura / metal): el «color del entorno» sale del mapa de luz (`metal_lc = lightmap × 1,6`). Clase 15 «World surfaces» en F1 > Materials (intensidad 0,45, rugosidad 0,55, metálico 0,15, relieve 0,45, reflejos 0,8, barniz 0,25 / 0,35), o la clase propia de la textura si la tiene. Solo dibujos opacos con escritura de Z, normales y la vista del mundo (perspectiva, banda ≥ 0,5, ancho completo). Mientras compila la variante se usa el pipeline original: el efecto aparece un poco después, sin tirón.
- **Nivel 2, luces prestadas.** Cada fotograma se cuentan las luces GX de los dibujos iluminados de la vista del mundo (personajes); las 4 con más `luminancia × usos` pasan, ya preparadas, al uniform `env_lights` del fotograma siguiente (como mucho un fotograma de retraso de cámara). Dan especular GGX con la atenuación GX (`env_att`) y se apagan donde el mapa de luz está en sombra (`env_shadow`). Sin luces ese fotograma, máscara 0.
- **Nivel 3, reflejos en suelos (SSR).** Pase `kSsr` en el mismo punto que el SSAO (dentro del fotograma, antes del visor), sobre la profundidad acumulada y la copia de color: normal reconstruida, suelo si mira al «arriba» del mundo (columna z de la matriz normal del primer dibujo estático con mapa de luz del fotograma, `note_world_up`), 32 pasos crecientes más 5 de refinamiento, Fresnel, desvanecimiento en bordes y por distancia (40 m), menos reflejo en texturas con mucho detalle (varianza de luminancia) y ninguno en zonas que brillan (alfa > 0,6). `kSsaoApply` mezcla el reflejo (enlace 5) y funciona con o sin SSAO. Necesita MSAA desactivado, como el SSAO.
- **Coste.** Niveles 1 y 2: unas decenas de operaciones por píxel del escenario y una variante de pipeline por material del escenario (compilada en segundo plano). Nivel 3: un pase de pantalla completa que solo traza en píxeles de suelo.
- **Validación.** `test_graphics.ps1` en verde, más `EnvironmentMaterial.*` (patrón del Olympus reconocido, máscara sumada y textura directa rechazadas). Con `AURORA_DUMP_SHADER_DIR` y `wgsl_check`: 457 variantes de entorno, 1.714 de metal y los 15 shaders de post-proceso compilan sin errores. Tamaños de uniform comprobados: `mat_ext` 128 bytes, `env_lights` 4 × 80 y la máscara 16.
- **Ajuste del 31/10 (F7 de las 19:58, pasillo del Olympus con la Morph Ball).** El usuario vio el reflejo del suelo bien, pero «demasiado perfecto», como un espejo sobre las baldosas y con rayas verticales finas. Cambios en `kSsr`:
  - **Relieve** (`ssr_relief`, 0–2, por defecto 1): el gradiente de luminancia de la textura del suelo, normalizado por el brillo local, inclina la normal en el plano del suelo. El reflejo se dobla en juntas y bordes de panel.
  - **Cavidades:** lo más oscuro que su entorno (juntas, ranuras, suciedad) refleja hasta un 65 % menos.
  - **Rugosidad** (`ssr_roughness`, 0–1, por defecto 0,35): el reflejo se difumina según la distancia recorrida, con 8 muestras en un disco girado por píxel (patrón 4 × 4). Tapa también las rayas.
  - **Detalle relativo:** la suavidad usa el contraste relativo al brillo local (varianza / media²), así un suelo de roca oscuro cuenta como rugoso.
  - «Surface relief» del material «World surfaces» sube de 0,45 a 0,75.
  - **Mapas de desplazamiento (parallax), descartados por ahora:** habría que volver a muestrear las texturas del juego con la UV desplazada dentro de cada etapa del TEV, y las texturas no traen altura (usar la luminancia hunde los colores oscuros aunque sean planos). El coste y el riesgo no compensan frente al relieve de normal.
  - Validación: los 15 shaders de post-proceso compilan con Dawn; `test_graphics.ps1` en verde (272 pruebas de Aurora).
- Pendiente: QA visual del usuario (Olympus, hangar, otros planetas) y medir con F8 que se mantienen los 60 FPS.

## Más mejoras de iluminación, cada una con su casilla (31/10/2026)

Investigación previa (67 volcados F7, 8.095 dibujos iluminados): toda la luz dinámica del juego es Lambert con atenuación foco/distancia, ambiente de registro y de 1 a 5 luces; nunca usa especular GX. Las salas con luces GX en las paredes (Aurora Unit: 232–294 dibujos opacos con mapa de luz por fotograma) quedaban fuera del material de entorno. El usuario pidió todas las mejoras propuestas salvo el preset de color, cada una activable en F1 > Effects > «World lighting (experimental)».

| Mejora | Clave `[effects]` | Defecto | Dónde | Coste |
|---|---|---|---|---|
| Material en salas con luces | `lit_room_material` | sí | `command_processor.cpp`: dibujos iluminados, opacos, con mapa de luz → `material_pipeline_state` con la clase 15 y sus propias luces | bajo |
| Mapas de luz suavizados | `smooth_lightmaps` | sí | variante de entorno: la fracción de texel pasa por smoothstep antes de la muestra bilineal (misma lectura, sin sangrado del relleno del atlas) | nulo |
| Brillos suaves | `soft_highlights` | sí | bloque de material: `base + add / (1 + add / (1 − base))` en vez de recortar a blanco | nulo |
| Reflejos con el color de la zona | `area_reflections` | no (desde el 01/11/2026: aporta poco a la vista) | `kAreaProbe` (2 × 1 píxeles: mitad superior e inferior de la escena) → lectura asíncrona como `depth_peek` (`post::after_submit`) → suavizado → `lit_ext` del uniform → mezcla 70 % en el entorno procedural del metal y del barniz. Llega unos fotogramas tarde: solo afecta al tinte del reflejo | bajo |
| Sombras de personajes suaves | `soft_shadows` | sí | dibujos `dst × (1 − src)` con textura por texgen de posición: variante con PCF de 5 muestras bilineales (≈3 × 3) | bajo |
| Rayos de luz | `light_shafts`, `light_shafts_intensity` | sí, 0,5 | `kShafts` (GPU Gems 3, cap. 13) a un cuarto de resolución: hasta 4 luces prestadas proyectadas en pantalla, 24 pasos con decaimiento sobre lo brillante y lo que brilla (alfa); se suma en `kSsaoApply` (enlace 6) | **medio** |
| Oclusión en los reflejos | `specular_occlusion` | sí | `kSsaoApply`: el peso del SSR se multiplica por el AO | nulo |
| Mezcla lineal | `linear_lighting` | **no** | bloque de material: la luz añadida se suma en lineal (2,2) | nulo; cambia el aspecto de todos los materiales |

- **Uniform:** `lit_ext` (3 vec4) tras `mat_ext` en los shaders de material: indicadores y los dos colores de zona. Los indicadores son uniformes: cambiar una casilla no recompila nada.
- **Fallo corregido (`pad1`):** el primer bit de `ShaderConfig::pad1` es la marca de las claves de ubershader (`uber.hpp`). Al añadir `envMaterial` antes de él, la marca se desplazó un bit. Las claves de ubershader no se guardan, así que no se dañó ninguna; las variantes de entorno guardadas en la sesión con ese error quedan con el bit de la marca y `valid_pipeline_config` las borra al arrancar («Removing invalid cached GX pipeline configuration»). Ahora la marca vuelve a su bit (0x40), `envMaterial` usa el único libre (0x80) y la sombra suave un byte de relleno explícito (`colorChannels[0]._p1`, `shader_soft_shadow`). Prueba `ShaderConfigLayout.UberMarkerKeepsItsBit`.
- **Validación:** `test_graphics.ps1` en verde (273 pruebas de Aurora). Con `AURORA_DUMP_SHADER_DIR` y `wgsl_check` compilan 2.282 variantes (1.714 de metal, 457 de entorno y 111 de sombra suave; la PCF está en 76, el resto usa coordenadas indirectas y queda como estaba) y los 17 shaders de post-proceso.
- Pendiente: QA visual y de rendimiento del usuario (F8), sobre todo de los rayos de luz.

## Brillo del juego sin copias fantasma (01/11/2026)

Síntoma (usuario, F7 `capture_20261101_025234`): las franjas de luz, las toberas de la nave y otros brillos verticales dejan copias del brillo a los lados. Es el problema que la wiki de Dolphin describe para Metroid Prime 3 como «bloom offset» a resoluciones internas altas: «multiple copies of the bloom spread out from the source».

Causa, vista en la lista de pases de la captura: el juego guarda el cuarto superior izquierdo del EFB, copia todo el EFB a mitad de tamaño (`GXCopyTex` con `halfScale`, declarada de 320x224) y la difumina dentro de ese cuarto dibujándola 6 veces desplazada en horizontal y 6 en vertical, con desplazamientos pensados para los texels de la Wii; después la extiende a toda la pantalla y restaura el cuarto. Nuestra copia mide 960x458 (3 x 2 veces más): una luz fina queda más estrecha que la separación entre las 6 copias y estas no se funden.

Cambio: `GXCopyTex` marca la copia a mitad de tamaño de casi todo el EFB (`gfx::note_resolve_soften` con la escala copia/tamaño del juego) y, tras su resolve, `post::soften_copy` la difumina con un filtro tienda separable del radio de un texel de la Wii (shader `kCopySoften`, dos pases sobre una textura temporal). El resto de copias, entre ellas la que guarda el cuarto que se restaura, no cambia. Interruptor F1 > Effects «Fix glow ghost copies», `[effects] glow_ghost_fix` (activo por defecto). Equivale a la opción «blur to native resolution» de Dolphin. Coste: dos pases sobre una textura de ~960x458.

Corrección y confirmación (01/11/2026): la primera versión no se activaba nunca. Exigía que la copia cubriera el 90 % del alto del EFB lógico configurado (640x528), pero Prime dibuja 640x448 (85 %). Ahora `GXCopyTex` compara el rectángulo ya convertido a píxeles de render con el objetivo (1920x916 de 1920x1080): ancho completo y al menos el 60 % del alto. Las primeras 6 copias a mitad de tamaño escriben `[glow-soften] half-size copy ... : glow source | not the whole picture` en `console.log`, y `post::soften_copy` registra `[glow-soften] glow source copy ... softened by X x Y texels` (o el motivo si se salta). El usuario confirmó el arreglo en la nave (toberas al despegar y aterrizar) y en las luces verticales.

## Reflejos en paredes lisas (experimental, 01/11/2026)

Sección propia en F1 > Effects, «Wall reflections (experimental)»: `[effects] wall_reflections` (apagado por defecto) y `wall_reflections_strength` (0,35). Usa el mismo pase `kSsr` que los suelos: además de las superficies orientadas hacia arriba (suelos, `P.a.z` = fuerza del suelo, 0 si está apagado), trata las casi perpendiculares al «arriba» del mundo (paredes, `P.e.x` = fuerza de pared). Selección automática: solo donde la textura es muy lisa (contraste local relativo < 0,06, más estricto que en suelos), para dejar mates las paredes gastadas o con detalle. Se desvanece a partir del 20 % del borde de pantalla y en los rayos que vuelven hacia la cámara (`r.z`), donde la pared mostraría lo que hay detrás: ahí queda el reflejo propio del material. Comparte rugosidad y relieve con los suelos. Coste bajo a medio (más pantalla cubierta por paredes). Pendiente: valoración del usuario en el Olympus; si refleja donde no debe, restringirlo por textura (clase de material con máscara por profundidad como la exclusión de la cara de Samus; los cristales son transparentes y necesitarían la variante por color).

Cara de Samus (05/11/2026): la normal por profundidad es plana en cada triángulo, y los lados de la nariz tras el visor reflejaban la mejilla como paredes lisas. Las texturas de la máscara de exclusión del SSAO (`kNoSsao`/`kSsaoCover`) ya no reciben reflejos de suelo ni de pared; ver `ssao_aurora.md` #23.

## Parpadeo de la lava (01/11/2026, pendiente de confirmar)

F5 `capture_20261101_035817` y `040409` (Bryyo, lava; la segunda con SSAO apagado): la lava baja de brillo en fotogramas sueltos (de ~237 a ~225 de rojo, perdiendo sobre todo verde y azul) mientras la roca de alrededor no cambia nada. Los dibujos del brillo del juego son idénticos en los fotogramas claros y oscuros: lo que varía es la escena de la que el juego saca su brillo, que se copia después de nuestra aplicación de post-proceso (#16 de `ssao_aurora.md`). Candidatos nuestros que cambian por fotograma sobre la lava: los reflejos de suelo/pared (la lava es lisa, recibía el reflejo de la roca y el número de aplicaciones por lote varía de 1 a 3) y los haces de luz (la lista de luces del fotograma podía salir vacía y los haces aparecían y desaparecían).
Cambios: `kSsr` atenúa el reflejo en texels muy brillantes (`selfLit = 1 - smoothstep(0.50, 0.72, luma)`), y `latch_frame` mantiene las últimas luces hasta 8 fotogramas cuando un fotograma no registra ninguna. Si sigue parpadeando, aislar con F1: «Floor reflections», «Reflections on smooth walls» y «Light shafts», uno a uno.

## Iluminación del motor

El análisis del modelo de luz del juego (actores, escenario, límites) y las mejoras posibles a nivel de motor está en `iluminacion_motor.md` (07/11/2026).

## Operadores del G.F.S. Olympus: traje de caucho y piel (08/11/2026)

Petición del usuario: materiales nuevos para los operadores del Olympus, con el traje como de caucho y la piel con menos brillo, más parecida a la humana. Capturas: F7 del 08/11 a las 22:06-22:10 (dos hombres de pie, una mujer de verde y hombres sentados en las consolas).

- **Hallazgo.** Todos los dibujos de los operadores (traje, cabeza y piel) usan el texgen de reflejo de entorno del juego (`metal=1`). Por eso recibían el metal mejorado genérico (metálico 0,62) y la cara brillaba como una armadura. El reflejo propio del juego en la cara pasa por una máscara casi negra (`636802fd`, `c1232516`), así que el brillo excesivo venía del metal mejorado.
- **Clase 11, traje** (`c634e941368a5675` hombres; `d79d8ef7c3ad56fc` y `d549b6602b2a5275` mujer, que incluyen sus manos): caucho dieléctrico. Intensidad 0,60, rugosidad 0,50, metálico 0, borde 0,35, relieve 0,45, brillos 1,1, capa satinada 0,25 con rugosidad 0,40.
- **Clase 12, cara** (`ac0d38f2f5056e20` hombres; `d1c29b175aa1c1c2` mujer, con capucha, cara y pelo): piel mate. Intensidad 0,65, rugosidad 0,55, metálico 0, borde 0,10, relieve 0,10, brillos 0,7. También se añadieron a la lista de piel de `post_effects.cpp`, para que sin materiales mejorados no reciban el metal genérico.
- **Fuera a propósito:** los mapas de luces del traje (`7aea837c`, `2a9bd4e3`), las máscaras de reflejo, los mapas de entorno de 32×32 compartidos por traje y cara, los ojos (`1c9f40b6`) y los dientes (`59d8604c`).
- **Límites:** la capucha está en el mismo atlas que la cara, así que comparte el material mate de piel, y las manos de la mujer el de caucho del traje. Separarlas por saturación (como la pintura de la nave) no es fiable con estos colores.
- **Ajuste:** F1 > Materials > «G.F.S. Olympus operators: suit / face». Pruebas gráficas completas en verde. Pendiente: QA visual del usuario en el Olympus.

## Distorsiones sin escalones (`smooth_distortion`, 12/11/2026)

- **Síntoma:** a 1815×866 (multiplicador 0, ventana de 1920), la esfera del disparo cargado y el fondo del visor de escaneo al llamar a la nave se veían pixelados. El cañón refractado mostraba escalones de unos 3×2 píxeles, el tamaño de un texel de la EFB nativa (640×448).
- **Diagnóstico (captura F7 `capture_20261112_225023`):** el dibujo 957 (pasada 2) copia la zona del cañón, 164×218 texels GX (465×422 en el host, resolución completa), y la deforma con texturas indirectas (mapa RGBA8 de 64×64). El problema no está en la copia ni en el filtro del sampler. Aurora, como el hardware, redondea la muestra indirecta a bytes, y el desplazamiento avanza en saltos de texeles nativos enteros. A resolución nativa el salto es de un píxel; al reescalar se nota.
- **Cambio:** con `[video] smooth_distortion = true` (por defecto; F1 > Video > «Smooth distortion effects»), el shader suma al desplazamiento entero la fracción por debajo del byte de la muestra filtrada, con la misma matriz, el mismo `>>3` y el mismo exponente. Solo afecta al formato ITF_8, con matrices estáticas y dinámicas S/T. Los bytes exactos (mapas muestreados con nearest) dan fracción cero y conservan el resultado del hardware.
- **Implementación:** `t_IndTexRaw` en el shader específico y `ind_tf` en el ubershader (`gx/shader.cpp`). La opción llega en `ind_mtx[impar].w` (`gx/shader_info.cpp`, `g_smoothIndirect`), sin tocar la clave de pipeline: se cambia en caliente. API `aurora_set_smooth_indirect`.
- **Coste:** el primer arranque tras el cambio recompiló 4.122 de 31.389 shaders (precarga de 96 s en Linux); después vienen de la caché.
- **Validado:** compila, precarga sin errores de validación WGSL y 60 FPS en el menú.
- **Pendiente:** confirmación visual en el juego (disparo cargado y llamada a la nave).

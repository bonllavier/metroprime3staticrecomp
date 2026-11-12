# SSAO (oclusión ambiental) en Aurora: diseño, hallazgos y reglas

Documento vivo del efecto SSAO de la pestaña **F1 > Effects**. Registra cada problema encontrado, su
causa comprobada, la corrección y lo que **no** debe deshacerse. Juego: RM3E01 (USA, disco 0, rev. 0).
Código: `third_party/WiiCompiled/aurora-main/lib/gfx/post_process.cpp` (shaders y pasadas),
`lib/gfx/common.cpp` (`render_impl`: acumulación de profundidad y punto de aplicación),
`lib/gx/command_processor.cpp` (`post::note_projection`, `post::note_draw`).

## Arquitectura actual (27 de octubre de 2026)

1. **Cámara del mundo.** `note_draw` cuenta los dibujos por cámara en perspectiva
   (A = xf[4], B = xf[5], znear y zfar del viewport). Solo cuentan los viewports de ancho completo
   con una banda de profundidad de al menos 0,01; así quedan fuera el radar 64×64, el cañón
   (banda 0,002–0,004) y el cielo (0,9956–1). `latch_frame` (al sellar el fotograma) elige la cámara
   con más dibujos. En Bryyo es A = −0,000267, B = −0,200053, banda 0,125–1, con unos 308 dibujos.
   Cada render pass queda **etiquetado** con las cámaras que dibujaron en él
   (`gfx::note_pass_camera`, hasta cuatro claves completas A, B, znear, zfar).
2. **Profundidad acumulada.** Prime borra el Z varias veces por fotograma. En cada frontera de pass
   y al final, `merge_depth` guarda **1/distancia** de los píxeles de la banda del mundo en una
   textura **R32Float**. Lo hace sin blending: escribe `max(anterior, nuevo)` en una textura temporal
   y la copia de vuelta.
3. **Aplicación dentro del fotograma.** Al final del bloque de passes del EFB dibujados con la cámara
   del mundo (`sceneEnd` en `render_impl`, con al menos 24 dibujos del mundo), tras dibujar ese pass y
   **antes de su copia del EFB**. Si el bloque continúa en el lote siguiente, la profundidad se arrastra
   y el SSAO se aplica allí:
   - Se copia el color del EFB con `CopyTextureToTexture`, conservando el alfa.
   - Se calcula el SSAO al tamaño del EFB.
   - Se aplican dos desenfoques bilaterales.
   - Se escribe `color × ao` de vuelta en el EFB, **conservando el alfa**.
   - Se reinicia la profundidad acumulada para el bloque siguiente.

   El visor, el HUD, los mensajes y el cañón se dibujan después, así que nunca reciben oclusión.
4. **Shader.** Reconstruye posiciones en espacio de vista sobre **texels enteros** de la profundidad.
   La normal sale de los texels vecinos, eligiendo la diferencia unilateral más pequeña. Solo ocluye
   la geometría que sube sobre el plano de la superficie (`max(0, cos − sesgo)`), con caída
   cuadrática por distancia. El patrón entrelazado es de 4×4. El radio en píxeles está calibrado a
   1080 filas y se escala con la resolución interna.
5. **Presentación.** `post::apply` ya no ejecuta SSAO; solo publica la nota de estado.
   El bloom «solo mundo» usa la misma copia de color.

## Valores por defecto (desde 0.1.3, elegidos por el usuario el 27/10/2026; última revisión el 31/10/2026)

SSAO **activado** por defecto. Valores: radius 1,0 · intensity 0,70 · softness 2,0 · samples 12 ·
bias 1,2 % · power 0,99 · fade 0 (desactivado) · edge sharpness 0,52 · glow protection 1,0.

El objetivo es sombra de contacto en esquinas y uniones (paredes, nave, traje), no oscurecer zonas
amplias. El radio corto y el bias de 1,2 % lo consiguen; La intensidad 0,70 con power 0,99 (ajuste del usuario del 31/10/2026; antes 0,60 y 0,74) da una sombra algo más marcada, todavía sin bordes duros.
Se aplican en:
- `post_effects.cpp`: valores de respaldo.
- `settings_overlay.cpp`: deslizadores y botón «Reset SSAO».
- `post_process.hpp`: valores iniciales de `Config`.
- `tools/package_release.ps1`: `Config.default.toml` publicado, con `msaa = 1`, que el SSAO necesita.

## Cronología de problemas y correcciones

| # | Síntoma reportado | Causa comprobada | Corrección | Estado |
|---|---|---|---|---|
| 1 | Profundidad vacía | La copia de pantalla borra el Z antes del final del fotograma | Acumulación con fusiones en cada frontera de pass | Resuelto |
| 2 | Parpadeo general; oclusión fuera de lugar | Condición de carrera de la proyección; se elegía la cámara equivocada (la regla de far máximo escogía el cañón) | Cámara por número de dibujos y bandas, fijada al sellar | Resuelto |
| 3 | Se apaga al disparar cargado | La distorsión del disparo sobrescribía la profundidad | Fusión filtrada a la banda del mundo | Resuelto |
| 4 | Apagados intermitentes | Presentaciones repetidas sin dibujos reiniciaban el estado | Se retiene la cámara durante 30 presentaciones vacías y no se reinicia sin dibujos en el EFB (614 repeticiones, 0 apagados) | Resuelto |
| 5 | Oclusión desplazada hacia arriba | Mapeo uv imagen→EFB | Rectángulo de la copia de pantalla (`note_display_source`) | Resuelto |
| 6 | «Puntitos» | Ruido de muestreo | Patrón entrelazado, desenfoque bilateral y control Softness | Resuelto |
| 7 | Franjas horizontales en el suelo de la mitad hacia abajo, con una «separación en la mitad» | La profundidad tiene **menos filas que la imagen**: los cálculos por píxel de imagen repetían una fila cada pocas líneas y la normal salía mal en esas filas. En half float, además, el paso de precisión se duplica en cada potencia de dos | Todo se calcula sobre texels enteros de la profundidad; acumulación en R32Float | Resuelto (confirmado por el usuario: «estaba perfecto») |
| 8 | Recompilación de unos 6.000 shaders tras un cambio | Activar la feature `Float32Blendable` del dispositivo cambia la clave de la caché de pipelines | Feature retirada; fusión sin blending (max + copia) | Resuelto. **No volver a activar features del dispositivo por un efecto** |
| 9 | El SSAO oscurecía los mensajes de los NPC y el HUD | Se aplicaba sobre la imagen final, después del visor | Ahora se aplica dentro del fotograma, antes del visor (la máscara por comparación de color se probó y se descartó; ver #10) | Resuelto (confirmado por el usuario) |
| 10 | Parpadeo con la máscara de visor | La copia de color se tomaba tras un pass distinto en cada fotograma (la disposición de passes varía; el mundo puede dibujarse dos veces), y la máscara rechazaba toda la escena | Máscara eliminada; aplicación dentro del fotograma | Resuelto |
| 11 | Destello blanco enorme («bloom que parpadea») | Prime usa el **alfa del EFB** como máscara de su propio glow. El pase dentro del fotograma escribía alfa = 1, y la copia por pipeline también forzaba alfa = 1 | Copia con `CopyTextureToTexture` y alfa conservado al escribir | Resuelto |
| 12 | El SSAO parpadea, un fotograma sí y otro no | Una llamada a `render_impl` puede contener más de un bloque del mundo, y solo se aplicaba tras el último. Grabación F5 de las 01:37: brillo alterno 113,8 / 110,7 en una escena quieta; la diferencia tenía forma de bordes de SSAO | Se aplica por bloque y se reinicia la profundidad tras cada aplicación | Paso intermedio; completado por #14 a #16 |
| 13 | Línea horizontal y vertical que cruzan el centro; tras ella el SSAO se ve distinto | Ver #16: el cuadrante superior izquierdo volvía sin oclusión | Ver #16 | Resuelto |
| 14 | Parpadeo por zonas | El mundo de cada fotograma del juego se reparte entre **dos lotes** (final de uno y principio del siguiente) con un punto de corte que cambia en cada fotograma (por ejemplo 214 + 316 o 28 + 457 dibujos). Se aplicaba SSAO a cada mitad solo con su propia profundidad | La profundidad se arrastra entre lotes (`set_depth_carry`) y el SSAO se aplica una sola vez cuando el bloque del mundo termina | Resuelto |
| 15 | Una parte de la pantalla distinta del resto | El juego hace passes cortos con la misma cámara del mundo (1 a 6 dibujos, efectos dibujados en partes del EFB) y se les aplicaba SSAO con su profundidad mínima | Solo se aplica SSAO a bloques con 24 dibujos del mundo o más (`kMinWorldBlockDraws`); de los bloques pequeños solo se descarta la profundidad | Resuelto |
| 18 | En cinemáticas y en la estación de guardado aparecen siluetas oscuras de personajes en las paredes (arriba a la izquierda), que se mueven con ellos | Antes de la escena, el juego dibuja los **mapas de sombra** de los personajes en la esquina superior izquierda del EFB (viewports de 128×128 y 256×256, proyección ortográfica, rango de profundidad 0–1). La acumulación fusionaba la profundidad en **todas** las fronteras de pass, y esas profundidades caían dentro de la banda del mundo. Grabación F5 del 27/10 a las 03:30, passes 0–3 | Solo se fusiona la profundidad de los passes del EFB que dibujan con la cámara del mundo (`worldPass` en `render_impl`). Se excluyen los mapas de sombra, los renders fuera de pantalla y los efectos | Implementado; el juego normal conserva el SSAO. Pendiente de confirmar en la cinemática |
| 17 | El SSAO de la cara de Samus se ve a través del cristal del visor (primer plano) | El cristal es transparente y no escribe profundidad: el SSAO calculaba la cara de detrás y oscurecía el color final, que incluye el brillo verde | **Protección de brillo**: el alfa del EFB es la cantidad de glow que escribe Prime (medido: superficies normales 0,1–0,55; visor, tiras de luz y energía del cañón ≈ 1). `ao = mix(ao, 1, smoothstep(0,6, 0,9, alfa) × fuerza)`. Parámetro `ssao_glow_protect`, 0–1, por defecto 1. Vista `WII_COMPILED_POST_DEBUG_VIEW=glow` | Implementado; las paredes conservan su oclusión. Pendiente de confirmar en el primer plano de Samus |
| 16 | Cuadrante superior izquierdo sin SSAO y parpadeando (la «cruz») | Justo después del mundo, el juego **copia todo el EFB** (para su glow), dibuja un efecto en el cuadrante superior izquierdo y lo **restaura desde esa copia**. Como el SSAO se aplicaba después de la copia, ese cuadrante volvía sin oclusión | El SSAO se aplica después de dibujar el pass que cierra el mundo y **antes de su copia del EFB**; ese pass se fusiona en ese momento | Resuelto. Confirmado por el usuario el 27/10 («luce muy bien, ya no parpadea»). Vista de diagnóstico uniforme en toda la pantalla |
| 19 | La cara de Samus tras el visor no luce bien con SSAO ni con la protección de brillo (#17) | La piel tiene un alfa de superficie normal; solo la textura distingue la cara del casco | **Máscara por textura.** Con F7 (que ahora guarda cada textura con su hash en `<captura>.textures/`) se identificaron la piel `aa70669d17e36772` y el ojo `0dc7a7f288795c1a` (capturas del 27/10, 21:38). Cada dibujo que muestrea una textura de la lista queda marcado (`DrawData::ssaoExclude`). `render_impl` codifica ese pass en segmentos: antes de cada tramo marcado guarda la profundidad, después anota los texels que cambiaron (`post::face_before/face_after`), y al fusionar el pass marca en una máscara R8 los texels donde esa profundidad sigue siendo la final (no los tapó el casco). `kSsaoApply` deja ao = 1 ahí. Sin pipelines nuevos del juego. Opción `ssao_skip_face` (por defecto sí, F1 > Effects). Vista `WII_COMPILED_POST_DEBUG_VIEW=mask` (magenta = excluido); log `[post-ssao-exclude]` | Confirmado por el usuario (cara tras el visor). Ampliado el 27/10 (22:49) a Samus sin traje: cara, piel, ojo, traje Zero, pelo y pestañas (`kNoSsao`); sin mapas de reflejo compartidos, que marcarían otros objetos. El 28/10 (00:47) se probaron cuatro texturas de degradado dibujadas junto al ojo (`dfa7304d…`, `4bb0d3db…`, `23876070…`, `28eea079…`) para el SSAO visible a los lados del visor: no cubrían esa zona (usuario) y se retiraron. Tampoco el forro `9aca649df1ee125a`. Causa real: lo que se oscurecía era el borde interior del casco visto a través del cristal, que no escribe profundidad. Solución: **máscara por color** (`TagSsaoCover`, `aurora_set_ssao_cover_textures`): en los tramos de dibujos con el cristal del visor `a9ca4b803677683f` se copia el color del EFB antes y se marcan los texels que cambian (`kCoverDiff`). El mismo atlas pinta las franjas verdes brillantes del traje, que ya estaban protegidas por brillo. Diagnóstico en directo: F1 > Effects > «Show SSAO-free area (magenta)» (`ssao_show_mask`). Confirmado con la vista magenta el 28/10. `WII_COMPILED_SSAO_EXCLUDE_EXTRA=<hex>,<hex>` añade texturas sin recompilar |
| 20 | El SSAO se ve en la cara y el borde interior del casco con el **traje PED** | El casco del PED es otro modelo: ninguna textura de #19 aparece en sus dibujos (F7 del 31/10, 06:51) | Igual que en la Varia. **Por textura** (`kNoSsao`): la cara del PED, `a1b6bddc9cda340b` (256×128), dibujada en el grupo opaco del casco con `f8decb1c` y el mapa de reflejo `0175f385`, que no se añaden porque los mapas compartidos marcarían otros objetos; el dibujo ya queda marcado por la cara. **Por color** (`kSsaoCover`): el cristal del visor del PED, dos atlas de 128×128 dibujados en pareja tras el casco y sin escribir profundidad: `ce0cf067ed0ba1d7` (mezcla alfa) y `c26b65383eafc212` (aditivo). La cara también entra en `kSkin` (sin reflejo de metal) | Implementado; pendiente de confirmar por el usuario con la vista magenta |
| 21 | En la nave de Samus, al empezar la partida, el SSAO de los paneles parpadea justo cuando por la ventana se ven la flota de la Federación y Norion; antes de eso y en el resto del juego, estable | La flota y el planeta son un **fondo lejano** dibujado en el mismo pass con otra cámara (A = −0,00122, B = −10,01) y una banda de profundidad fina al final (0,956–1). Tiene tantos dibujos como la cabina o más (81 contra 70, 116 contra 87) y la diferencia cambia según las naves que entran en cuadro, así que la cámara elegida por número de dibujos alternaba entre cabina y fondo. Con el fondo elegido, la profundidad de los paneles quedaba fuera de la banda y perdían la oclusión. Grabaciones F5 del 31/10 a las 17:19:51 y 17:20:18 | `latch_frame` prefiere cualquier cámara cuya banda empiece antes de 0,9 (`kFarLayerNear`); una capa lejana solo se elige si no hay otra. Revisados los 627 volcados F7/F5 guardados: el mundo usa siempre la banda 0,125–1. La capa lejana ganaba en 119 de ellos, todos de la misma secuencia espacial (cabina del 27/10, nave volando del 27/10 y del 30/10); en esos fotogramas ahora gana el mundo. En el resto no cambia nada | Implementado; pendiente de confirmar por el usuario en la cabina |
| 22 | (Nueva función) Reflejos en suelos | — | El SSR comparte el punto de aplicación dentro del fotograma, la profundidad acumulada y la copia de color con el SSAO. `wants_depth` y `wants_scene_color` también lo cuentan; `kSsaoApply` (ahora `Layout::FourTextures`) aplica el AO solo si corrió (`P.f.x`) y mezcla el reflejo (`P.f.y`, enlace 5). Detalles en `mejoras_graficas.md` | Implementado; pendiente de QA |

| 22 | En la pelea con Rundas, el post-proceso llegaba a 9,5 ms de GPU por fotograma y bajaban los FPS al debilitarlo | Cada fotograma tenía 2-4 bloques completos del mundo y la profundidad del EFB persiste entre ellos, así que cada aplicación recalculaba SSAO y reflejos de toda la pantalla y oscurecía de nuevo lo ya tratado (medido con `WII_COMPILED_GPU_TIMING=1`, 01/11/2026) | Una aplicación por fotograma del juego en su último bloque completo; los anteriores solo acumulan profundidad (acción 3) y los bloques pequeños ya no vacían la acumulación salvo si una pasada de cara forzó su fusión. `[post-ssao] accumulated_blocks=` | Implementado; pendiente de confirmar por el usuario |
| 23 | Triángulos algo más oscuros en la nariz de Samus tras el visor (Varia, RM3E01 y Trilogy; F7 del 05/11/2026, 23:16) | No era el SSAO ni el material de piel: los dibujos de la cara (`aa70669d`) salen neutros. Eran los **reflejos en paredes** (`wall_reflections`, activos por defecto): `kSsr` toma la normal de la profundidad, plana en cada triángulo, y en la cara de pocos polígonos los lados de la nariz cuentan como «pared lisa» y reflejan la mejilla, faceta a faceta. La máscara de #19 solo anulaba el AO | `kSsaoApply` no mezcla el reflejo (suelo ni pared) en los píxeles de la máscara, y la máscara se construye también con SSAO apagado si hay reflejos (`g_excludeOption` = (ssao o ssr o wall_ssr) y `ssaoSkipMarked`). WGSL de `kSsaoApply`/`kSsr` validado con Dawn. Pendiente de QA del usuario |

| 24 | (Nueva función, 08/11/2026) Iluminación moderna, nivel 1: sombras de contacto y exposición automática | — | Comparten el punto de aplicación, la profundidad acumulada y la instantánea, pero **no cambian ningún pase del SSAO**: `modern_scene()` corre después de `kSsao` y sus desenfoques y escribe una copia de la instantánea (`g_litScene`, alfa conservado); `kSsr` y `kSsaoApply` leen esa copia en vez de `g_sceneColor`. Con ambas opciones apagadas se usa `g_sceneColor` como antes. `wants_depth`/`wants_scene_color` también las cuentan, para que la profundidad y la instantánea existan con el SSAO apagado. Objetos `Pass` propios (el desenfoque de contacto reutiliza solo el texto de `kSsaoBlur`). Ver `iluminacion_motor.md` | Pendiente de QA del usuario |

| 25 | El SSAO se ve encima de la niebla, las explosiones y las partículas: la profundidad de la pared de detrás «se nota» sobre el efecto (usuario, 12/11/2026, Windows y Linux) | Los efectos se dibujan dentro del bloque del mundo, con mezcla y **sin escribir profundidad**. El SSAO lee la profundidad de la superficie de detrás y oscurece el color final, que ya incluye el efecto. Es el caso #17 en general | **Máscara graduada de efectos.** Al grabar cada dibujo, `command_processor` marca `ssaoEffect` si mezcla (`GX_BM_BLEND` o `GX_BM_SUBTRACT`, salvo One/Zero) y no escribe Z. `render_impl` agrupa esos dibujos en tramos como los de cobertura (#19): copia el color antes y `kEffectDiff` escribe después cuánto cambió cada texel, `smoothstep(1,5/255, 48/255, Δ)`, como 0..0,49 con fusión por máximo en la misma máscara R8. No alcanza las marcas binarias de la cara y el visor (1,0, leídas como > 0,5). `kSsaoApply` hace `ao = mix(ao, 1, cobertura)` y reduce el reflejo de suelos y paredes en la misma proporción. Opción `[effects] ssao_under_effects` (F1 > Effects, «SSAO under fog and effects», activa por defecto). En la vista `WII_COMPILED_POST_DEBUG_VIEW=mask` los efectos salen en cian, proporcional a la cobertura. Sin pipelines nuevos del juego ni toggles de Dawn: la caché no cambia | Implementado. Repetición `hangar_1x_window` en Linux con la vista de máscara y 19 capturas: el cian cae en explosiones, chispas, fogonazo del cañón, llamas, vapor y luces (0 a 15 % de la imagen); suelos y paredes sin efectos conservan el SSAO; 59,91 FPS de media. Los dibujos luminosos sobre el suelo (plataforma de aterrizaje, estación de guardado) también quitan el SSAO del suelo de debajo. Pendiente de QA del usuario en niebla densa y en Windows |

## Reglas que no deben romperse

- La profundidad se calcula sobre **texels enteros** (`texel_of` / `texel_pos`). No volver a muestrear
  por píxel de imagen: reaparecen las franjas (#7).
- La acumulación es **R32Float sin blending**: max más copia (#7, #8). Ni half float, ni features
  nuevas del dispositivo.
- El SSAO se aplica **dentro del fotograma, una vez por fotograma del juego**, en el último bloque
  completo del mundo de ese fotograma (con la profundidad arrastrada entre lotes y acumulada desde
  los bloques completos anteriores del mismo fotograma), seguido de `reset_depth`. El fin de
  fotograma es la copia a pantalla (`GXCopyDisp`, `startsGameFrame`). No volver a aplicarlo en la
  presentación ni con máscaras de color (#9, #10, #12, #14, #22).
- Aplicarlo **antes de la copia del EFB** del pass que cierra el mundo; el juego restaura partes del
  EFB desde esa copia (#16).
- No aplicar SSAO a los bloques pequeños con la cámara del mundo (#15).
- Fusionar profundidad **solo** de los passes del EFB con la cámara del mundo; los mapas de sombra de
  las cinemáticas se dibujan en el mismo EFB (#18).
- La cara de Samus se excluye por **textura**, nunca por color ni por zona de pantalla. Para otra textura:
  F7 en la escena, buscar el hash en `<captura>.textures/` y añadirlo a `kSamusFace` en
  `runtime/src/post_effects.cpp` (#19).
- **Conservar siempre el alfa del EFB** en cualquier pase que escriba en él (#11).
- No usar el número de dibujos para identificar el mundo; usar la etiqueta de cámara completa por
  pass (#10).
- Una cámara con la banda de profundidad al final (empieza en 0,9 o más) es un fondo lejano: nunca
  gana a la cámara del mundo por número de dibujos (#21).
- Los efectos transparentes (mezcla sin escritura de Z) solo **atenúan** el SSAO con la máscara graduada (0..0,49). No escribir 1,0 por ellos: eso es la exclusión binaria de la cara y el visor (#25).
- Los efectos nuevos de la escena modifican una **copia** de la instantánea, nunca los pases del SSAO
  ni `g_sceneColor` (#24): así el SSAO sigue igual con ellos encendidos o apagados.

## Cómo medir (sin adivinar)

- **F5** graba 60 fotogramas del EFB en `UserData/Logs/captures`. Como el SSAO está dentro del
  fotograma, aparece en esas imágenes.
- La grabadora **alterna dos encuadres** entre fotogramas pares e impares (retícula y cañón en otra
  posición), también con SSAO desactivado (grabación de las 01:48: diferencia 9,7 entre consecutivos
  y 0 entre fotogramas de la misma paridad). Para detectar parpadeo hay que comparar fotogramas
  **de la misma paridad** y mirar la imagen de diferencia.
- Herramientas privadas en `recomp/.session_tmp/test_scripts/` (no se publican):
  - `seqdiff.py <prefijo>`: diferencias con el fotograma anterior (d1) y con el de dos antes (d2).
  - `bmpdiff.py a.bmp b.bmp salida.png [ganancia]`: imagen de diferencia amplificada.
  - `bmp2png.py`: convierte una captura a PNG.
- `WII_COMPILED_POST_DEBUG_VIEW=ao` muestra solo la oclusión, en gris, sobre los píxeles con
  profundidad del mundo; `ny` y `nx` muestran las componentes de la normal reconstruida. Fue la
  herramienta que reveló #14 a #16.
- `WII_COMPILED_POST_DEBUG_DEPTH=1`:
  - Recuadro de depuración: rojo = distancia/60, verde = fuera del mundo.
  - Logs cada 30 fotogramas:
    - `[post-scene]`: passes del EFB; `*` = SSAO aplicado, `-` = bloque pequeño descartado,
      `carried` = dibujos arrastrados del lote anterior.
    - `[post-ssao-efb]` y `[post-ssao-cam]`.
  - **Solo para diagnóstico**: con esta variable el juego registra más y puede ir peor.
- No ejecutar instancias de prueba mientras el usuario juega: dos instancias comparten el mando y
  `Config.toml`. El 27 de octubre coincidió con un error de entrada («Prime input callback is not
  translated»), y en ese momento el disco G: también estaba lleno.
- Vigilar el espacio en G:. Las grabaciones F5 ocupan unos 7 MB por fotograma; con el disco lleno se
  truncan las capturas y pueden fallar otras escrituras.

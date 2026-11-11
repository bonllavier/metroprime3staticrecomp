# Ubershader opcional (modo híbrido)

Diseño del 26 de octubre de 2026. Implementado y validado ese mismo día. Fuentes de contexto en `tirones_shaders_referencias.md` (Dolphin: artículo de 2017, `ShaderCache`, `UberShaderPixel`, `AsyncShaderCompiler`, `VideoConfig`; port de Prime 1).

**Estado:** funcional y experimental. Apagado por defecto. Con la opción apagada el juego no cambia: el generador especializado sigue protegido por el golden de 3.966 shaders y no se tocó ni un byte de su salida.

## Qué hace

Cuando un dibujo llega al codificador y su pipeline especializado todavía se está compilando, dibuja con un shader genérico que lee el material (etapas TEV, tabla de swaps, comparación de alfa, texturas indirectas) del buffer de uniforms en lugar de esperar. Las cachés mandan: si el pipeline especializado está listo se usa siempre, y los pases que alimentan copias EFB de una sola vez (visor de escaneo, bloom, IDs en alfa de destino) esperan al exacto aunque haya ubershader.

Activación: F1 > Video > *Ubershader while shaders compile*, clave `[video] ubershader = true`, o `WII_COMPILED_UBERSHADER=1`. Se aplica al arrancar para el precalentado y en vivo para los dibujos.

## Diseño real

| Pieza | Dónde | Notas |
|---|---|---|
| Clave | `lib/gx/uber.hpp` | Un `PipelineConfig` con el TEV borrado. Marca `pad1 = 1` y cuatro máscaras en bytes de relleno de `TevStage`. Viaja por la cola y la persistencia de pipelines sin tocar su código |
| Máscaras | `make_uber_config` | Texturas siempre las 8 (el bind group ya tiene 8 ranuras). Texcoords: los de la receta más los texgens definidos (los de tipo bump no se emiten). Canales de color exactos |
| Empaquetado | `make_uber_data` | 16 etapas x 4 palabras, swaps, comparación de alfa, etapas indirectas, konst por etapa (se calcula por dibujo) |
| Generador | `lib/gx/shader.cpp` | Misma función que el especializado con una bifurcación: la etapa de vértices es idéntica, la de fragmentos lee el TEV del uniform |
| Uniforms | `build_uber_shader_info`, `build_uniform` | Mismo prefijo que el especializado más un bloque de 896 bytes al final. Si no cabe en 3.840 bytes (53 de 1.590 claves) ese material no tiene ubershader |
| Elección | `lib/gx/pipeline.cpp::render`, `render_policy.hpp` | Política pura probada: especializado listo, luego pase persistente espera, luego ubershader, luego espera |
| Registro | `command_processor.cpp` | Solo si el pipeline no estaba listo al grabar el dibujo: se prepara el uniform del ubershader y se pide su pipeline. Con interpolación de fotogramas, la imagen intercalada dibuja ese material en su posición final (usa su propio rango de uniforms) |
| Precalentado | `pipeline_cache.cpp` | Con la opción activa, cada receta cargada pide también su ubershader (no persistente) |
| Seguridad | `build_uber_shader` | Un error de WGSL no es fatal: se captura en un ámbito de errores, se registra una vez por mensaje distinto y el material sigue esperando a su pipeline |

Bits por etapa (palabra 0: argumentos de color y alfa; 1: operaciones; 2: textura/canal/swaps/banderas; 3: indirecto). Detalles en `make_uber_data` y en `uber_tev_body`.

## Resultados medidos (2026-10-26)

| Medida | Valor |
|---|---|
| Recetas de la semilla | 3.966 |
| Claves de ubershader | 1.590 (746 módulos de shader distintos) |
| Claves que no caben en el uniform | 53 (3 %) |
| Prewarm sin / con ubershader | 5.718 pipelines en 2,5 s / 7.299 en 4,2 s |
| Caché de Dawn sin / con | 148 MiB / 245 MiB |
| Coste en GPU con todo forzado por el ubershader | 55-57 FPS frente a 59-60 |
| Equivalencia visual (dibujando todo con el ubershader, escena de la nave) | 0,77 % de píxeles distintos y solo en elementos animados (llamas, radar, líneas del visor) |

La comparación visual se hizo cargando la partida del usuario (Bryyo 05 %) con `WII_COMPILED_UBER_FORCE=1` y comparando con la captura del modo normal (`tools/drive_prime.ps1` y `tools/compare_images.ps1`).

## Cómo validarlo

1. Pruebas automáticas: `tools/test_graphics.ps1` (incluye golden del ubershader, límite de uniforms y estructura del WGSL).
2. Modo forzado: `WII_COMPILED_UBERSHADER=1` y `WII_COMPILED_UBER_FORCE=1` dibujan todo con el ubershader cuando existe. Cualquier diferencia con el juego normal es un fallo del ubershader. Registro `[uber] draws encoded...`.
3. Errores de compilación: línea `[uber] ubershader ... rejected` en el registro. `WII_COMPILED_UBER_DUMP_FAILED=<prefijo>` guarda el WGSL rechazado.
4. Cuando cambie el generador del ubershader intencionadamente: `UPDATE_GOLDEN=1 gx_fifo_tests --gtest_filter='UberShader.GoldenHashes*'` y anotar que la caché de esos shaders se invalida.

## Pendiente

- Validación del usuario con el visor de escaneo, bloom y efectos de fases (el modo forzado no los recorrió: la prueba automática solo cargó la partida y la sala inicial).
- Medir en una iGPU de gama baja antes de recomendarlo a nadie.
- Decidir si se persisten las claves de ubershader para jugadores sin semilla completa.
- Priorizar la compilación del ubershader por delante de los especializados pendientes (hoy es FIFO).
- 53 materiales sin ubershader por tamaño de uniforms: empaquetar más o pasar a un buffer de almacenamiento.

## Revisión del 26 de octubre (grabación F8 del usuario)

Causa de los bajones con la opción activa:

1. Prime mantiene el EFB entre fotogramas, así que Aurora marca todos los pases como "requieren pipeline listo". Cada shader nuevo bloqueaba el fotograma durante su compilación (94-144 ms en la grabación), con o sin "do not wait".
2. La política prohibía el ubershader en copias de una sola vez, y una copia cuenta así la primera vez que se produce, justo cuando aparecen los materiales nuevos.
3. Las compilaciones en juego corrían con prioridad superior a la normal en 6 hilos y quitaban CPU al juego.
4. Las variantes con iluminación por píxel no tenían ubershader precompilado.

Correcciones:

- El ubershader se permite en todos los pases. Nunca se salta un dibujo.
- Las compilaciones ya cubiertas por el ubershader van a prioridad baja, con dos hilos como máximo. Si un dibujo llega a esperarlas, recuperan la prioridad.
- Se precompilan los ubershaders de las variantes por píxel.

A/B con `tools/ab_ubershader.ps1` (1 de cada 8 materiales sin precompilar y caché de Dawn en frío tras el arranque, partida Bryyo, 60 s caminando):

| Pasada | FPS medio | FPS mínimo por ventana de 5 s | Peor fotograma |
|---|---|---|---|
| Sin ubershader | 58,4 | 49,6 | 218 ms |
| Con ubershader | 59,7 | 55,9 | 75 ms |

Interruptores de prueba: `WII_COMPILED_PREWARM_SKIP_ONE_IN=<n>`, `WII_COMPILED_DAWN_CACHE_COLD_AFTER_BOOT=1`, `WII_COMPILED_UBER_STATS=1` (motivo de cada dibujo sin pipeline listo) y `WII_COMPILED_UBERSHADER=0/1`. `tools/summarize_stack_profile.py` ya lee los perfiles F8 v3 y admite `--window desde,hasta`.

## Revisión de la implementación (26 de octubre, segunda pasada)

Fallo encontrado y corregido: las claves del ubershader se guardaban en `pipeline_cache.db` como recetas normales (53 filas en el perfil del usuario). Una clave así tiene el programa TEV vacío: el generador especializado la habría rechazado o, en la herramienta de semilla y en el golden, generado código fuera de rango. Ahora:

- `find_pipeline` no persiste claves de ubershader; se derivan de las recetas en cada arranque.
- `valid_pipeline_config` exige los bits reservados a cero, así que el cargador borra las filas antiguas (se confirmó: 53 eliminadas en el siguiente arranque) y el golden las ignora.
- `tools/prime_pipeline_seed.py merge` descarta esas filas por el byte de banderas.

Simplificaciones:

- Se retiró `requireExactPipelines`, `WII_COMPILED_UBER_STRICT_BAKES` y el campo `persistentPass` de la política: el ubershader vale en todos los pases y lo único prohibido en un pase persistente es saltar dibujos.
- `compile_priority` (sin uso) se sustituyó por `pick_compile_slot`, que es lo que ejecutan de verdad los hilos de compilación: urgente, luego puenteado con tope de dos hilos, luego precalentado. Con pruebas.
- El puente deja de consultar el mapa de pipelines por dibujo una vez que el especializado está listo.

Opciones revisadas: "Experimental: do not wait for new shaders" sigue teniendo efecto propio (salta dibujos en pases normales para los materiales que el ubershader no cubre), así que se conserva con una descripción corregida. El resto de interruptores son de prueba o diagnóstico (`WII_COMPILED_UBER_FORCE`, `WII_COMPILED_UBER_STATS`, `WII_COMPILED_UBERSHADER`, `WII_COMPILED_PREWARM_SKIP_ONE_IN`, `WII_COMPILED_DAWN_CACHE_COLD_AFTER_BOOT`) y no aparecen en la interfaz.

Verificación: suite completa en verde (265 GX, 19 políticas, 8 caché, 22 runtime) y una pasada A/B con la opción activa: media 59,2 FPS, peor fotograma 71 ms, sin ubershaders rechazados.

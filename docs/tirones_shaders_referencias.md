# Tirones por compilación de shaders: referencias y opciones

Registrado el 24 de octubre de 2026 a petición del usuario. Reúne lo que Dolphin documentó sobre Metroid Prime 3 (RM3E01), cómo resolvió los tirones de compilación de shaders, qué hizo el port de Metroid Prime 1 y qué tenemos ya en WiiCompiled/Aurora.

**Decisión del usuario:** el ubershader **no** es la única solución a considerar. Hay que evaluar alternativas propias, sobre todo las que aprovechan que este proyecto es específico de un juego y tiene sus datos y su código. No implementar nada de este documento sin acordarlo.

## Fuentes

| Fuente | Qué aporta |
|---|---|
| [Wiki de Dolphin: Metroid Prime 3](https://wiki.dolphin-emu.org/index.php?title=Metroid_Prime_3%3A_Corruption_%28Wii%29) | Problemas conocidos de emulación del juego y ajustes recomendados. |
| [Blog de Dolphin: "Ubershaders: A Ridiculous Solution to an Impossible Problem"](https://es.dolphin-emu.org/blog/2017/07/30/ubershaders/?cr=es) (JMC47, MayImilae; 30 de julio de 2017, actualizado en 2023) | Origen del problema, soluciones descartadas, ubershader, modo híbrido y problemas de drivers. |
| [Repo de Dolphin](https://github.com/dolphin-emu/dolphin), `Source/Core/VideoCommon/` | Código: `ShaderCache.cpp/.h`, `UberShaderPixel.cpp`, `UberShaderCommon.cpp`, `AsyncShaderCompiler.cpp` y `VideoConfig.h` (rama master leída el 24 de octubre de 2026). |
| `references/MetroidPrimePort` (Odrannnn) | Port nativo de Prime 1 sobre Aurora: caché de pipelines semilla grabada con un recorrido por todas las salas. |

## Problemas del juego en Dolphin (wiki)

1. **Tirones por compilación de shaders.** Prime 3 es "uno de los títulos más susceptibles"; en casos graves hay pausas de más de un segundo.
   - Desde 5.0-4869 se resuelve con ubershaders.
   - Desde 5.0-6461 los shaders cacheados se compilan en segundo plano al iniciar, lo que puede causar tirones breves.
   - Ajustes recomendados: *Shader Compilation = Asynchronous (Ubershaders)* y *Compile Shaders Before Starting*.
2. **Desconexiones del Wiimote**, ligadas a los datos del altavoz (*Speaker Data*). Se evitan desactivando el altavoz o con Bluetooth passthrough. **Es relevante para la etapa de Wiimote real por Bluetooth.**
3. **Fondo de color sólido en el menú de pausa** si se pausa con el visor de escaneo activo (en lugar de la captura desenfocada). Se evita con *Disable Fog* (issue 13711 de Dolphin). **Vigilar en nuestro visor de escaneo**, que depende de copias del EFB y de dst alpha.
4. **Bloom desplazado a resolución interna mayor que 1x**: aparecen copias desplazadas porque el bloom se renderiza a baja resolución. Se evita renderizando el bloom a resolución nativa (Graphics Mods) o desactivando *Scaled EFB Copy*. **Relevante para nuestra resolución interna configurable.**
5. **Punto en el centro de la pantalla por encima de 1x.** Se oculta quitando el casco y el visor en las opciones del juego, o con texturas personalizadas.
6. El online cerró el 27 de junio de 2013.

## Por qué existen los tirones (artículo)

- El TEV de Flipper/Hollywood se configura en el instante del dibujo y no guarda configuraciones. El juego cambia de estado sin aviso y espera que el dibujo salga en el mismo fotograma.
- En una GPU moderna cada configuración necesita un shader especializado compilado por el driver. Si la compilación dura más de un fotograma, el juego se para.
- Hay unos 5,64 × 10^511 configuraciones posibles solo del TEV, más el T&L de vértices, así que no se pueden compilar todas por adelantado.
- **Prime 3 fue el primer juego con el que probaron los ubershaders**, porque ninguna solución anterior le servía. Fue de los pocos juegos que perdió la calificación "jugable" por los tirones.

## Soluciones que Dolphin evaluó

| Idea | Resultado en Dolphin | Lectura para este proyecto |
|---|---|---|
| Generar todos los shaders por adelantado | Imposible por el número de combinaciones. | Para un solo juego el conjunto real es pequeño y finito (ver alternativas). |
| Predecir los shaders que vendrán | Inviable de forma genérica: un emulador no sabe qué hará el juego. | **Nosotros sí podemos:** tenemos los materiales en el disco y el código recuperado del juego. |
| Compartir cachés de UID (configuraciones antes de compilar) | Útil pero incompleto; poco solapamiento entre juegos y se invalida con cada corrección gráfica. | Es lo que hizo Prime 1 con su semilla, y aquí es viable porque es un solo juego. |
| Compilación asíncrona sin dibujar | Funciona, pero hay objetos que aparecen tarde y fallos si un dibujo alimenta una copia del EFB que se hace una sola vez (cabezas de Mii). | Es nuestra opción experimental "no esperar a los shaders nuevos". Riesgo concreto en Prime 3: visor de escaneo, bloom e IDs de objeto en dst alpha. |
| Ubershader exclusivo | Elimina los tirones, pero cuesta mucha GPU. | Coste alto; las iGPU no llegan ni a 1x. |
| **Ubershader híbrido** | Dibuja con el ubershader mientras el shader especializado compila en segundo plano, y luego lo sustituye. Es el modo recomendado. | Candidato principal si se implementa un ubershader. |

### Detalles de drivers del artículo

- **Variantes de pipeline:** cambiar el blend o el depth state crea otro pipeline, y muchos drivers no reutilizan el shader, así que hay un tirón menor la primera vez aunque se use el ubershader. Dolphin precompila las variantes comunes.
- Ubershaders compilados con NVIDIA en OpenGL/Vulkan rinden peor que en D3D. Nosotros usamos Dawn sobre D3D12, que es el camino favorable.
- Sin caché de shaders en el driver, el ubershader se recompila en cada arranque.
- Cambiar ajustes como el MSAA o la iluminación por píxel obliga a recompilar los ubershaders.

## Cómo está en el código de Dolphin

- Modos (`VideoConfig.h`, `ShaderCompilationMode`):
  - `Synchronous`
  - `SynchronousUberShaders` (exclusivo)
  - `AsynchronousUberShaders` (híbrido)
  - `AsynchronousSkipRendering`
- `bWaitForShadersBeforeStarting` corresponde a *Compile Shaders Before Starting*.
- Arranque (`ShaderCache::InitializeShaderCache`):
  1. Ajusta los hilos del compilador asíncrono a los de precompilación.
  2. Carga las cachés de shaders y la caché de UID del juego (`<GameID>.uidcache`, magia `PUID` y versión `GX_PIPELINE_UID_VERSION`).
  3. Si usa ubershaders, ejecuta `QueueUberShaderPipelines()`, que enumera todas las UID de ubershader de vértice, píxel y geometría compatibles (mismo número de texgens) con un formato de vértice ficticio y variantes de blend.
  4. `CompileMissingPipelines()` encola todo lo conocido.
  5. Si se pidió esperar, `WaitForAsyncCompiler()` antes de empezar.
  6. Vuelve a la configuración de hilos de ejecución.
- En ejecución:
  - `GetPipelineForUidAsync()` devuelve "pendiente" mientras compila el especializado, y en ese caso se usa `GetUberPipelineForUid()`.
  - Cada fotograma, `RetrieveAsyncShaders()` (en `after_frame_event`) recoge los pipelines terminados.
  - Los trabajos llevan prioridad y se reencolan si sus etapas no están listas.
- El ubershader de píxeles (`UberShaderPixel.cpp`) lee el estado del TEV (etapas, órdenes, constantes, swap tables, alpha test, fog) desde uniforms. Su UID solo depende de pocas cosas (número de texgens, early depth, iluminación por píxel, salida entera, etc.), por eso hay pocos ubershaders.

## Port de Metroid Prime 1 (Odrannnn/MetroidPrimePort)

- Incluye `assets/initial_pipeline_cache.db` (unos 9 MB) embebido en el ejecutable. Contiene las configuraciones de pipeline de un recorrido por los menús y **todas las salas** (`tools/pipeline_seed.py tour` y luego `merge`). Según su documentación solo guarda configuraciones, no recursos del juego.
- Al arrancar compila esa semilla en segundo plano mientras muestra un aviso con progreso (`DrawShaderCompilationToast`).
- Si Aurora cambia la versión del formato, la semilla se ignora y la prueba `port_pipeline_seed` falla.

## Lo que ya tenemos (WiiCompiled/Aurora, octubre de 2026)

- Caché de pipelines persistente en SQLite con versión (`GXPipelineConfigVersion` 20 con migración desde la 19) y precompilación al arrancar con pantalla de estado. Equivale a la caché de UID más *Compile Shaders Before Starting* de Dolphin, pero **solo con lo que este PC ya vio**.
- `aurora-main/lib/gfx/pipeline_cache.cpp` ya admite una **semilla** `initial_pipeline_cache.db` (solo lectura, con su esquema verificado), igual que Prime 1. Todavía no tenemos semilla grabada.
- `aurora-main/lib/webgpu/gpu_cache.cpp` es la caché de blobs de Dawn (DXIL y PSO), con poda por antigüedad.
- Compilación en segundo plano con prioridades, la espera limitada `wait_pipeline_until` y un presupuesto de 4 ms por fotograma.
- Opción experimental "no esperar a los shaders nuevos", desactivada por defecto. Tiene el mismo riesgo que el asíncrono de Dolphin.
- Las mediciones con F8 (`recomp/checkpoints/20261023_room_entry_hitches.json` y siguientes) muestran que los tirones grandes que quedan son pipelines usados por primera vez.

## Alternativas propias (abiertas; ninguna está decidida)

1. **Enumerar las configuraciones desde los datos del juego.** Los materiales de cada área están en los PAK del disco. Recorrer los materiales de un área con el código recuperado del juego, contra un grabador de GX en seco, daría sus configuraciones sin jugar. Es la "predicción" que Dolphin no puede hacer de forma genérica. Pendiente: qué estados vienen del contexto (luces, niebla, blend, formato de vértice) y cómo combinarlos.
2. **Precalentar al cargar un área.** El juego carga áreas mientras Samus espera en una puerta o un ascensor. Interceptar la carga del área (o de sus recursos) y encolar sus pipelines con prioridad aprovecha ese tiempo muerto.
3. **Semilla distribuible** grabada por el propio usuario: un modo "grabar caché" mientras juega, después fusionado, como en Prime 1, sin navegación automática. Antes de publicarla, revisar que solo contenga configuraciones y no datos del juego.
4. **Bajar el coste por pipeline.**
   - Medir dónde se va el tiempo: generar WGSL, Tint→HLSL, DXC→DXIL o crear el PSO de D3D12.
   - Compartir módulos entre variantes que solo difieren en blend o depth, mover a uniforms lo que hoy forma parte de la clave del shader, y comprobar la caché de blobs de Dawn.
5. **Ubershaders parciales o por familias.** Uno por número de etapas o texgens, con las operaciones del TEV en uniforms, usado solo como puente en modo híbrido. Son más pequeños y baratos que uno completo.
6. **Sustituto aproximado en lugar de saltar el dibujo.** Mientras compila, dibujar con un pipeline ya listo y parecido (mismas texturas, misma combinación de salida). El error visual es menor que saltarlo. Nunca aplicarlo a dibujos que alimentan copias del EFB.
7. **Lista de dibujos críticos que no se pueden saltar ni aproximar:** los que escriben dst alpha, las copias del EFB leídas por la CPU (sondas del visor), el bloom y los menús. La lección de las cabezas de Mii en Dolphin aplica a nuestro visor de escaneo.
8. **Más hilos de compilación según los núcleos** (ya hay detección de topología en `host_threads.h`) y prioridad para lo que está en pantalla.

Cada propuesta debe medirse con grabaciones F8 de 120 s antes y después. La política es no superar 60 FPS: el objetivo son 60 FPS estables sin tirones, no tasas más altas.

## Medición: tres grabaciones F8 de 120 s (24 de octubre de 2026)

Archivos: `UserData/Logs/profiles/stack_profile_20261024_193723.txt`, `_193931.txt` y `_194158.txt`. Analizados con hitch.py, hitch2.py, hitch_budget.py, hitch_leaf.py y game_overhead.py del scratchpad.

| Causa del fotograma lento (> 20 ms) | Grabación 1 | Grabación 2 | Grabación 3 |
|---|---|---|---|
| Pipelines nuevos: tiempo perdido | **6.975 ms** (71 fotogramas, 48 de ≥ 80 ms) | 623 ms (2 de ≥ 80 ms) | **1.502 ms** (7 de ≥ 80 ms; máximo 542 ms con 16 pipelines) |
| CPU, sin otra actividad (≈ 24–26 ms, una VBlank perdida) | 2.107 ms (239) | 686 ms (86) | 391 ms (52) |
| Subida de texturas (> 64 KiB) | 0 | 89 ms | 165 ms |
| Disco | 195 ms | 32 ms | 53 ms |

- **Todos** los fotogramas de 80 ms o más coinciden con pipelines nuevos. El hilo de frames de Aurora pasa el 36 % de esas ventanas en `wait_pipeline_impl`.
- Al arrancar ya se precompilan 12.473 pipelines en 3,3 s con la caché de blobs de Dawn (todo aciertos), y aun así cada sala nueva aporta entre 10 y 48. Compilador: DXC (`use_dxc`).
- **La opción experimental "no esperar a los shaders nuevos" no actúa en Prime.** `begin_frame(clearEfb=false)` activa `g_requirePersistentEfbPipelines`, y las copias a textura (`persistentCopy`) marcan `requireReadyPipelines`. Así casi todos los pases esperan sin límite (`wait_pipeline_for_persistent_pass`) y el presupuesto de 4 ms no se aplica. Es intencionado: saltar un dibujo de un pase que acaba en una copia del EFB la estropea para siempre (la lección de las cabezas de Mii). Un ubershader híbrido permitiría dibujar sin esperar y sin perder contenido.
- **Hilo del juego** (grabación 1): 69 % del tiempo ocupado, del cual solo el 57 % es código del juego. Sobrecostes medidos:
  - Lectura del reloj `PPC_Mftb`/`PPC_Mftbu` (`steady_clock::now`): ≈ 11 %.
  - Sondeo de interrupciones en cada llamada (`OS_HLE_PollPendingPrimeIpcInterrupt` + `MemoryPrimeIpcPollControl`): ≈ 9 %.
  - Escritura del FIFO de la GPU (`PrimeGpuTryCaptureCpuFifo`, `HleFifoWrite`, `SubmitBurst`/`Push`): ≈ 8 %.
  - Despacho indirecto: ≈ 3 %.
  - Los fotogramas de CPU lentos no tienen una función del juego dominante: son escenas con más trabajo que superan los 16,7 ms por poco.

## Semilla de pipelines de RM3E01 (24 de octubre de 2026)

- **Archivo canónico:** `recomp/third_party/WiiCompiled/runtime/assets/pipeline/RM3E01_00/initial_pipeline_cache.db`. El build de RM3E01 lo instala junto al ejecutable (`cmake/PublicProducts.cmake`). Antes se instalaba por error la semilla de Mario Kart (`mkw-base-pal`, 1.199 filas), que Aurora fusionaba en cada caché y compilaba al arrancar sin uso.
- **Herramienta:** `python -B recomp/tools/prime_pipeline_seed.py merge [--install <carpeta del exe>]`. Une las filas de `UserData/Cache/pipeline_cache.db` en la semilla por `(type, hash)`, sin borrar nunca filas.
  - Conserva solo la `config_version` más reciente de cada tipo.
  - Excluye las filas de Mario Kart, que la migración v19→v20 de Aurora reconoce por su contenido.
  - `build_prime_runtime.ps1` la ejecuta en cada build completo.
- **Portabilidad:**
  - Las filas son recetas: estado GX del TEV, mezcla, profundidad y formato de vértices. No contienen binarios de GPU ni bytes de recursos del juego, así que valen para cualquier GPU, driver y backend (D3D12/Vulkan).
  - El MSAA no es configurable (siempre 1) y la resolución no forma parte de la clave.
  - `dawn_cache.db` (DXIL/PSO) depende de cada GPU y driver, y no se distribuye.
- **Estado:** 12.873 filas (50,8 MiB sin comprimir) de la partida del usuario hasta el 24 de octubre. Además se eliminaron las 1.199 filas de Mario Kart de su caché local (copia previa en el scratchpad).
- **Mantenimiento:** cuando cambie `GXPipelineConfigVersion`, Aurora necesita la migración correspondiente o la semilla deja de cargarse. Tras un cambio de versión, ejecutar el juego una vez (Aurora migra la caché local) y luego `merge`. Revisar la procedencia antes de publicarla, igual que hizo el port de Prime 1 al distribuir la suya.

## Clave de pipeline sin estado de dibujo (24 de octubre de 2026)

Algunos valores que el juego cambia de un dibujo a otro formaban parte de la clave del pipeline. Cada valor nuevo compilaba otro pipeline, también al volver a zonas ya visitadas.

- **v21:** `dstAlpha` pasa a ser un uniform más la constante de mezcla (la clave solo guarda si está activo).
- **v22:** `kcSel`/`kaSel` pasan a uniforms `konst<N>` por etapa.
- Las migraciones de Aurora unen las filas duplicadas.
- **Resultado medido con v21:** 7,3 s → 3,1 s perdidos por shaders y 47 → 16 tirones de 80 ms o más en grabaciones de 120 s.
- **Pendiente:** funciones de iluminación de los canales de color (unos 1.283 pares casi duplicados) y `vtxStride` (formato de vértices). Detalles en `recomp/checkpoints/20261024_pipeline_key_dst_alpha.json`.

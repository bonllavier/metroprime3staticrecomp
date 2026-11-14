# Iluminación del motor de RM3E01 y margen de mejora

Fecha: 7 de noviembre de 2026. Análisis estático con Ghidra (puente HTTP, `main.dol` RM3E01 revisión 0) y el asm de dtk (`decomp/build/RM3E01_00/asm`). No se ha modificado código ni ejecutado el juego. Las direcciones son de RM3E01_00; no valen para R3ME01 (Trilogy) ni RM3P01.

Complementa a `mejoras_graficas.md`, que describe lo que ya añade Aurora en la capa host.

## Cómo ilumina el juego

### 1. Actores (personajes, enemigos, objetos móviles, brazo de Samus)

| Función | Dirección | Qué hace |
| --- | --- | --- |
| `CLightParameters::CLightParameters` | 0x80013B98 | Guarda el máximo de luces de área (+0x24) y dinámicas (+0x28). **Ambos se recortan a 4** (−1 o >4 → 4). |
| `CLightParameters::MakeActorLights` | 0x80013D5C | Reserva un `CActorLights` de 0x318 bytes. |
| `CActorLights::CActorLights` | 0x8000C6A4 | Dos `reserved_vector<CLight, 4>` (área en +0x0, dinámicas en +0x154); `CLight` = 0x54 bytes. Umbral de movimiento al cuadrado en +0x308. |
| `CActorLights::BuildAreaLightList` | 0x8000D09C (0x1154 bytes) | Selección de luces estáticas del área (ver abajo). |
| `CActorLights::BuildDynamicLightList` | 0x8000ED64 | Luces dinámicas: `CScriptDynamicLight` enlazadas y la lista del gestor (+0x5CC, entradas de 0x58). Prueba esfera (radio de la luz) contra la AABB del actor; entran por orden de llegada hasta 4. Las de tipo 3 (ambiente local) se suman al color de ambiente. |
| `CActorLights::ActivateLights` | 0x8000C918 | Carga las luces en GX (`0x803E8BA8` → `0x803E8BD4`), máscara `(1 << n) − 1` (`0x803E8F10`) y el ambiente, recortado a 1,0. |

`BuildAreaLightList`, paso a paso:

1. Solo recalcula si el actor se movió más de `kDefaultMinPosChange` = **0,1** unidades (0x8067EA74) o pasó su intervalo de fotogramas, escalonado con un contador 0..7 (`0x8067A13C`). Entre medias reutiliza la lista anterior.
2. Candidatas: el conjunto de luces del área (`ELightSet` 0..3, ver más abajo), filtradas por la visibilidad del PVS del área y por una esfera de radio ×2 (`0x8067EA8C`) contra la AABB del actor.
3. Ordena por intensidad en el **centro** del actor (`0x803E2D44`) y se queda con las N primeras (N ≤ 4, menos 1 si está activo el modo de «luz combinada» `0x806781E0`).
4. **Sombra de la luz principal:** lanza un único rayo estático desde el centro del actor hacia la luz más fuerte (`0x8009F0E8`). Si hay obstáculo, esa luz se apaga en 15 fotogramas (`0x8067EA80`, aplicado en `ActivateLights`). Es una sombra binaria para todo el actor.
5. Las luces que no caben se suman al ambiente como color plano o, con el modo combinado, se funden en una luz direccional (`0x8000CCF8`/`0x8000E924`).

`CGameArea` post-construido (+0xB4) guarda cuatro conjuntos de luces: `0x8000CF84` devuelve el vector de `CLight` (+0xA8, +0xCC, +0xF0, +0x114) y `0x8000CF24` la información paralela de 0x60 bytes por luz (+0x9C, +0xC0, +0xE4, +0x108; el byte +0x38 decide si participa en la prueba de sombra).

Campos de `CLight` observados (0x54 bytes): posición +0x00, dirección +0x0C, color +0x1C, tipo +0x20 (1 punto, 2 direccional, 3 ambiente local), radio cacheado +0x48 (`0x8037A534`, se recalcula si el bit 5 de +0x50 está puesto), id +0x50.

### 2. Geometría del mundo

- El color base es **lightmap × difusa × 2**, horneado (ver «Iluminación del mundo» en `mejoras_graficas.md`). Las luces estáticas del área no se evalúan en tiempo real sobre el escenario: ya están en el lightmap.
- **Las luces dinámicas sí iluminan el escenario.** `0x801ACAB8` (gestor, cada fotograma) recorre los `CGameLight` activos (disparos, explosiones, `CScriptDynamicLight` con el byte +0x134) y llena dos listas: +0x5CC (con id, para actores) y +0x5D8 (`CLight`, para el mundo); ambas se ordenan. `0x801ACA14` pasa la segunda a la ranura 0x18 de la vtable del renderizador (`0x80482BF4`, equivalente a `PrepareDynamicLights` de Prime 1).
- `0x80482BF4`: descarta luces puntuales fuera del frustum (radio ×2), calcula qué nodos del octree de cada área toca cada luz (`0x804B6CF4`) y asigna a cada trozo de modelo visible una **combinación de hasta 4 luces** más un gris de ambiente, empaquetada en 32 bits (4 índices de 6 bits + 6 bits de ambiente; `0x80482B28`). Las combinaciones se deduplican en una tabla de **96** como máximo (`0x804817D4`; si se llena, el trozo usa la combinación 0, sin luces).
- Al dibujar (`0x804833A8` y hermanas `0x804835D4`, `0x80483804`, `0x80483A94`, `0x80483CC4`, más `0x8047D6F0`), `0x804842CC` carga las luces de la combinación del trozo y su máscara. En el TEV, la luz rasterizada se suma al lightmap (`C0 = ras + lightmap × C1`).

### 3. Lo que el motor no tiene

- Ni especular GX (confirmado en 8.095 dibujos iluminados de 67 volcados F7) ni relieve.
- Ni sombras de luces dinámicas ni sombras por píxel en actores: solo el lightmap, la sombra proyectada de Samus o las de mancha (`CSimpleShadow`), y la prueba binaria de un rayo descrita arriba.
- Rango de 8 bits: colores de luz `CColor`, EFB RGBA8 con el alfa usado como máscara del brillo.
- Elección de luces por actor y por trozo, no por píxel: cuando cambia el conjunto, la luz aparece o desaparece de golpe.

## Qué ya cubre Aurora

Iluminación por píxel, materiales por clase con GGX, barniz y separación pintura/metal, material de entorno sobre el lightmap, 4 «luces prestadas» (deducidas de los dibujos del fotograma anterior), SSR en suelos y paredes, SSAO, sombras PCF, rayos de luz, lightmaps suavizados, brillos suaves y mezcla lineal opcional. Gran parte sigue pendiente del QA del usuario.

## Mejoras posibles, por relación beneficio/riesgo

Todas serían opciones del host (hooks del runtime y Aurora). No tocan la ISO, el DOL de matching ni el ritmo lógico de 60 Hz.

### Prioridad alta

**A. Luces reales del motor en Aurora (sustituye las prestadas).** Post-hook en `0x801ACAB8` para copiar la lista del mundo (+0x5D8) y lectura del conjunto de luces del área actual (`CGameArea` +0xB4 → +0xA8), más la matriz de vista. Aurora recibiría todas las luces colocadas por los artistas, en coordenadas de mundo, en el mismo fotograma. Evita el fotograma de retraso y el parpadeo (lava), y limitar a 4 luces vistas en personajes. Alimenta también los rayos de luz.
Regla clave: las luces estáticas ya están horneadas en el lightmap. Solo deben aportar especular y relieve, enmascarados por el lightmap como ahora (`env_shadow`), **nunca difusa otra vez**. Coste: copia de unas decenas de `CLight` por fotograma y un bucle por píxel con tope (16–32 luces cercanas o una rejilla por tiles).

**B. Sombras de contacto en espacio de pantalla.** Marcha de rayos corta sobre la profundidad que ya acumula el SSAO, hacia cada luz de A. Sombrearía los especulares del host y la luz dinámica sobre el mundo, y en actores sustituiría la sombra binaria de todo el cuerpo por una por píxel. Coste bajo a medio, en el mismo punto del fotograma que SSAO/SSR. Límites conocidos: no ve oclusores fuera de pantalla y supone un grosor.

### Prioridad media

**C. Más de 4 luces dinámicas por trozo de escenario.** Con A disponible, Aurora puede evaluar todas las luces dinámicas por píxel en los dibujos con lightmap. Hay dos variantes. (1) Solo añadir especular de todas, conservando la difusa del juego: sin riesgo. (2) Sustituir también la difusa: hook en `0x804842CC` para no cargar luces GX en el mundo. Eliminaría los cortes con muchos disparos o explosiones a la vez, pero exige que la variante del host esté compilada antes de apagar las del juego, porque el pipeline de respaldo se quedaría sin luz.

**D. Estabilidad de la luz en actores.** Recalcular cada fotograma (umbral 0,1 → 0 e intervalo 1, mediante overlay de datos del runtime) y fundir en el tiempo las luces que entran o salen del conjunto (post-hook en `ActivateLights`, por puntero de `CActorLights` e id de luz). Reduce los saltos de luz en enemigos y personajes al pasar junto a focos. El coste de CPU es despreciable en PC.

**E. Calidad del lightmap.** Filtrado bicúbico (más allá del smoothstep actual) y dithering con ruido azul para las bandas de 8 bits × 2 en degradados oscuros. Solo Aurora; coste casi nulo.

### Proyectos grandes, sin prioridad

**F. HDR interno.** EFB en RGBA16F con tone mapping, bloom físico de lo sobreexpuesto y salida HDR10. Hay que respetar el recorte de TEV y blending que espera el juego, las copias EFB (brillo, visores) y el alfa como máscara. Riesgo alto: cambia el aspecto de todo.

**G. Mapas de sombra reales** para la luz principal, reenviando la geometría del fotograma con un pipeline solo de profundidad. Sobre todo ganarían los actores (proyectar y recibir); el escenario estático ya tiene sus sombras horneadas. Coste de GPU y de ingeniería alto.

**H. Rehornear los lightmaps con iluminación global.** Desaconsejado: cambia la dirección artística (los lightmaps originales están retocados a mano), supone mucho trabajo y obliga a cachear datos derivados de la ISO.

## Orden recomendado

1. Cerrar el QA pendiente de las opciones experimentales actuales (F8 a 60 FPS), para no apilar efectos sin validar.
2. A, porque es la base de B, C y de mejores rayos de luz.
3. B, y después D o E según lo que se vea en las capturas.

## Nivel 1 implementado (08/11/2026)

Petición del usuario: completar el nivel 1 (puntos A–E más exposición y estabilización temporal), activable desde F1 > Video y sin dañar lo que ya funciona, en especial el SSAO. Todo vive en el host (hooks del runtime y Aurora); la ISO, el DOL de matching y la lógica a 60 Hz no cambian.

### Opciones (`[video]` de `Config.toml`, F1 > Video > «Modern lighting»)

| Opción | Clave | Defecto | Qué hace |
| --- | --- | --- | --- |
| Interruptor general | `modern_lighting` | sí | Apagado, todas las partes quedan desactivadas y la imagen es exactamente la anterior. |
| Luces reales del motor (A) | `engine_lights` | sí | Las luces del material de entorno, los rayos de luz y las sombras de contacto salen de las listas del motor. |
| Todas las luces dinámicas en paredes (C, variante 1+inyección) | `extra_dynamic_lights` | sí | Trozos iluminados con 4 luces reciben en las ranuras 4–7 las luces dinámicas que el motor dejó fuera. |
| Luz suave en personajes (D) | `actor_light_fade` | sí | Fundido de ~1/6 s cuando cambia la elección de luces de área de un actor, y del color de ambiente. |
| Sombras de contacto (B) | `contact_shadows`, `contact_shadows_strength` (0,6), `contact_shadows_length` (0,6 m) | sí | Marcha de rayos en pantalla hacia las 4 luces más fuertes. |
| Estabilización temporal | `temporal_stability` | sí | Reproyección y mezcla de las sombras de contacto (no con interpolación de fotogramas). |
| Tramado del lightmap (E) | `lightmap_dither` | sí | Ruido triangular de un paso de 8 bits en la muestra del lightmap. |
| Exposición automática | `auto_exposure`, `exposure_range` (0,6 EV), `exposure_speed` (1,5 s) | sí | Adaptación ocular y curva que conserva las luces altas. |

Las partes A, C y D dependen de direcciones de RM3E01 y solo existen en ese perfil (`MKW_GAME_PROFILE_RM3E01_00`); B, E, la exposición y la estabilización son de Aurora y genéricas.

### Datos del motor (runtime, `hle/prime_engine_lights.{h,cpp}`)

- **Post-hook de `0x801ACAB8`** (prólogo `9421fe60`; RAII como el mouse look): cuando el motor termina las listas dinámicas del fotograma, el runtime lee la del mundo (`this + 0x5D8`, `rstl::vector<CLight>`) y el conjunto de luces 0 de cada área cargada (`*(this+4)` = gestor, `+0x28 → +0x2134` = `CWorld`; `+0x30` número de áreas, `+0x38` entradas de 8 bytes con el `CGameArea` en `+4`; cargada si `+0x98 == 0x11`; `+0xB4 → +0xA8`). Envía hasta 512 `AuroraEngineLight` en espacio de mundo con `aurora_set_engine_lights`. El rango sale de `+0x48` o, si está marcado como sucio (`+0x50 & 0x20`), con la fórmula de `0x8037A580` (intensidad = byte máximo / 255; `sqrt(I / (k2/17))` o `I / (k1/17)`).
- **Hook de `0x8000C918`** `CActorLights::ActivateLights` (prólogo `9421ffb0`): antes del cuerpo, por cada bloque de luces de actor se recuerdan las luces de área elegidas (identificadas por posición) con un peso que sube o baja 1/10 por paso de 1/60 s de reloj; las que entran se escalan en color, las que salen se reponen en una ranura libre con color decreciente, y el ambiente `+0x2B8` se suaviza. Después del cuerpo se restaura el bloque byte a byte. Avanza con el reloj, no con el hook anterior, para terminar siempre el fundido (modo foto, cinemáticas).

### Aurora

- **`gx.cpp` / `gx.hpp`:** entrada protegida por cerrojo (`set_engine_lights`, `take_engine_lights`); `MaxEnvLights = 8` (las prestadas siguen siendo como máximo 4); `EngineEnvLights`.
- **`command_processor.cpp`, `engine_lights`:** en el primer dibujo estático con lightmap de cada fotograma (iluminado o no) se toma su matriz de posición como vista del mundo, se convierten las luces exactamente como el cargador `0x803E8BD4` (foco con `GXInitLightSpot(cutoff, COS2)`, direccional a 2^20 contra la dirección, personalizada con su atenuación angular), se puntúan (luminancia con el `k0` que codifica intensidad × alcance / distancia, ×1,5 dinámicas, direccionales ×4) y se eligen 8 para el material de entorno, 4 para las sombras de contacto y 4 delante de la cámara para los rayos de luz. La matriz se anota para la reproyección temporal. Se ejecuta después del inicio de fotograma de las luces prestadas para que los rayos conserven las del motor.
- **Inyección (C):** en dibujos iluminados con lightmap cuya máscara del canal 0 es exactamente `0x0F`, se buscan las 4 luces cargadas en la lista dinámica del motor (posición con tolerancia 1e-3·(1+|p|)); las dinámicas no cargadas cuya esfera toca la de alguna cargada (hasta 4) van a las ranuras 4–7 y a la máscara de los canales con esa misma máscara, solo durante `build_uniform` (también el del ubershader híbrido) y se restauran después. La máscara es uniform: ningún pipeline nuevo.
- **Shader de entorno (`shader.cpp`, `shader_info.cpp`):** `env_lights: array<Light, 8>`; `env_light_mask.y = 1` marca el modo motor, `w & 1` el tramado. `env_gate()` devuelve exactamente `env_shadow` con `y = 0` (luces prestadas, matemática idéntica a la anterior); en modo motor aplica una caída suave hasta el rango (`color.a` = +rango estática, −rango dinámica) y solo las estáticas usan la puerta del lightmap. El tramado suma dos patrones de gradiente entrelazado (ruido triangular de ±1/255) a la muestra del lightmap.
- **Post-proceso (`post_process.cpp`):** estado `Modern` aparte de `Config` (que `set_config` sustituye entero). Dentro de `apply_ssao_in_frame`, tras los pases del SSAO, `modern_scene()` produce una copia de la instantánea del mundo con las sombras de contacto y la exposición (`kSceneLight`, alfa conservado), y solo los reflejos y la escritura final (`kSsaoApply`, sin cambios en su código) leen esa copia. Con las dos partes apagadas se devuelve la instantánea original: el camino del SSAO es el de siempre. Pases nuevos con objetos `Pass` propios: `kContact` (12 pasos por luz, sesgo y grosor según la distancia, sin efecto en zonas que brillan ni en la máscara de la cara de Samus, desvanecido de 30 a 60 m), dos desenfoques con el texto de `kSsaoBlur`, `kContactTemporal` (reproyección con `vista previa × inversa(vista actual)`, rechazo por distancia, recorte 3×3, peso 0,85, giro del patrón de muestreo cada fotograma), `kLumMeasure` (log2 de la luminancia lineal en 16×9 puntos, centro con más peso, brillos con menos) y `kAdapt` (1×1, `1 − e^(−dt/velocidad)`). La exposición corrige la mitad de la diferencia con la referencia log2(0,1), hasta +rango para aclarar y −rango/2 para oscurecer; al aclarar usa Reinhard extendido con el punto blanco igual a la ganancia (lo blanco sigue blanco). Historial y adaptación solo avanzan en la imagen nativa; las intercaladas los reutilizan.

### Límites conocidos

- **No es HDR real:** el EFB sigue siendo de 8 bits (su formato forma parte de las ~5.300 claves de pipeline y de todas las copias EFB). La exposición y la curva trabajan sobre la imagen ya cuantizada.
- **Sombras de contacto en pantalla:** no ven oclusores fuera de imagen, suponen un grosor, y cerca de paredes pueden sumarse a la sombra ya horneada del lightmap (alcance corto para limitarlo).
- **Inyección (C):** sin los límites de cada trozo, la relevancia se estima por solape con las luces ya cargadas; un trozo sin luces porque se llenaron las 96 combinaciones no recibe nada.
- **Luz suave en personajes:** solo las luces de área; las dinámicas siguen como las elige el motor.

### Validación (08/11/2026)

- **Pruebas:** `test_graphics.ps1` en verde (283 de Aurora, entre ellas la nueva `ShaderGolden.EnvironmentVariantWithEightEngineLightsFitsTheUniformLimit`; 8 + 47; 29 del runtime; Python).
- **WGSL:** con `AURORA_DUMP_SHADER_DIR` y `wgsl_check` (recompilado contra el Dawn del proyecto) compilan sin errores las 3.393 variantes GX y los 23 shaders de post-proceso. Las 484 variantes de entorno llevan `env_gate` y el tramado.
- **Juego, reproducciones QA del recorrido `hangar`** (la partida de la grabación arranca ahora en Bryyo). Copias privadas de las grabaciones: `hangar_modern_on`/`hangar_modern_off` y `hangar_i120_modern`/`hangar_i120_off`, con la ISO apuntando al `.rvz` actual.

  | Pasada | Resultado |
  | --- | --- |
  | 60 FPS, iluminación moderna apagada | 59,90 FPS de media, mínimo 58,1, peor fotograma 102 ms, 0 errores |
  | 60 FPS, encendida | 59,92 de media, mínimo 57,0, peor fotograma 124 ms, 0 errores; `[prime-engine-lights] first list: 10 lights` |
  | 120 simulados (`QA_INTERP_IGNORE_MONITOR` + `QA_SKIP_PRESENT`), apagada | 119,66 de media, mínimo 115,7 |
  | 120 simulados, encendida | 119,1 de media; una ventana de 5 s a 65,4 hacia los 90 s, donde la apagada también baja; el resto a 120 |

- **Primera pasada encendida descartada:** apareció el diálogo de la ISO, la reproducción se desfasó y dio dos capturas blancas. Repetida en las mismas condiciones que la apagada, no hay ninguna.
- **Cuelgue 1 (interpolación), corregido:** a 120 con `IGNORE_MONITOR` y sin `SKIP_PRESENT`. Era una carrera en `InterpolationWorkerPool`; ver `fps_altos.md`.
- **Cuelgue 2 (sin resolver, solo en un modo de prueba artificial):** con el arreglo anterior, en el mismo modo (presentación limitada a 60 por el driver mientras se intercalan imágenes), el juego se bloqueó a los ~250 s. Captura `capture_prime_hang.py`:
  - el hilo GX esperaba un cerrojo dentro de `efb_ram::schedule` (desde `GXCopyTex`);
  - otro hilo estaba en el callback de mapeo de `efb_ram::after_submit` (`complete_async_slot`, que llama a `Unmap()` y al gancho `notify_guest_write` con `g_asyncMutex` tomado, en contra de la regla del propio archivo);
  - el hilo del juego esperaba al FIFO.

  Ese código no es de este trabajo y el bloqueo no se reprodujo con la receta documentada (`SKIP_PRESENT`).

  **Corregido el 8 de noviembre de 2026** (`efb_ram_copy.cpp`). Apareció también en replays a 120 reales con presentación de verdad (`hangar_i120`, con y sin la sincronización con el refresco, ver `fps_altos.md`), capturado con `capture_prime_hang.py`. Primero se sacaron `Unmap()` y el gancho de debajo de `g_asyncMutex`; el siguiente cuelgue mostró el callback parado dentro de `wgpuBufferUnmap` ya sin el cerrojo: con `AllowSpontaneous`, Dawn ejecuta el callback desde sus propias llamadas y con sus cerrojos tomados, y volver a llamar a Dawn desde ahí también se bloquea. Ahora el callback solo marca el slot como `Mapped`; el hilo del fotograma (`drain_async_events`, que ya corría en `encode_async_downloads` y `after_submit`) lee el búfer, escribe la RAM del invitado, avisa con `notify_guest_write` y hace `Unmap()` con el cerrojo suelto, con el slot en `Completing` para que nadie lo reutilice. `schedule()` también avisa del slot nuevo fuera del cerrojo.
- **Pendiente del usuario:** QA visual en partidas reales (cada casilla de F1 > Video > «Modern lighting» en la misma escena), sobre todo exposición y sombras de contacto, y a 120 Hz reales.

## Nivel 2: materiales con datos del juego y luz que rebota (13/11/2026)

Petición del usuario: mejoras de iluminación por código, reutilizando solo lo que el juego ya trae, eficientes en hardware medio, cada una activable en F1. Todas cuelgan del interruptor general «Modern lighting» y están encendidas por defecto. Sin toggles de Dawn nuevos ni features del dispositivo.

### Opciones (`[video]`, F1 > Video > «Modern lighting»)

| Opción | Clave | Defecto | Qué hace |
| --- | --- | --- | --- |
| Relieve desde la textura | `texture_relief` | sí | El relieve del bloque de material sale de la muestra de la textura de color de la superficie (`material_diffuse_stage`), no del color ya iluminado. Las sombras horneadas y la caída de las luces dejan de leerse como relieve. Sin muestras extra. |
| Material en todo objeto iluminado | `default_material` | sí | Los dibujos iluminados, opacos, de la vista del mundo, sin mapa de reflejo, sin clase y sin lightmap (enemigos, puertas, objetos) reciben el bloque de material con la clase 14 «Objects and creatures (default)» de F1 > Materials: dieléctrico suave (0,50 / 0,50 / 0 / 0,15 / 0,50, brillos 0,9). Usa la tabla `material_pipeline_state` y el respaldo por vértice mientras compila. |
| Máscaras del juego | `material_masks` | sí | (a) Si el material carga una textura propia en un registro que luego multiplica el mapa de entorno (`material_reflection_mask_stage`), su luminancia escala el metal por píxel. (b) Lo que suma la etapa emisiva final (`material_emissive_stage`: `PREV + K × textura`) se guarda aparte (`mat_emis_add`), el bloque sombrea la superficie sin ello y se vuelve a sumar al final. |
| Luz que rebota | `bounce_light` y `bounce_light_strength` (1,0), `_emissive` (1,0), `_surfaces` (0,6), `_radius` (3 m), `_samples` (8) | sí | Pase de pantalla, ver abajo. |

Las tres primeras son bits de `lit_ext[1].w` (uniform): cambiarlas no recompila. El texto de las variantes de material sí cambió, así que Dawn las recompila una vez.

### Luz que rebota (`post_process.cpp`)

En `modern_scene()`, junto a las sombras de contacto y la exposición, sin tocar los pases del SSAO:

1. `kGiSource`: radiancia a 1/4 de la instantánea, en lineal, por `superficies + 4 × emisivo × smoothstep(0,55, 0,9, alfa)`. El alfa del EFB es la máscara de brillo del juego y aquí representa el brillo que no cabe en 8 bits. Otra pasada la reduce a 1/8.
2. `kGi` a media resolución: 4–16 muestras en disco (patrón girado cada fotograma nativo), posición desde la profundidad acumulada, peso `coseno en el receptor × ventana lineal de distancia`; las muestras cercanas leen 1/4 y las lejanas 1/8. Una superficie plana no se ilumina a sí misma. Respeta la máscara de la cara y se atenúa bajo efectos (#25 de `ssao_aurora.md`).
3. `kGiTemporal`: reproyección con la misma matriz relativa que las sombras de contacto (ahora calculada una vez para ambas), peso 0,9, recorte 3×3 ensanchado. No avanza en imágenes intercaladas.
4. Dos desenfoques 3×3 con rechazo por profundidad.
5. `kSceneLight` (ahora `Layout::FourTextures`, enlace 5) suma en lineal `tono de la superficie × luz`, con caída suave hacia blanco y nunca sobre superficies que brillan.

Vista de diagnóstico (builds QA): `WII_COMPILED_POST_DEBUG_VIEW=gi`.

### Corrección de lo existente: el material de entorno no se aplicaba nunca

`command_processor.cpp` exigía `blendMode == GX_BM_NONE` para el material de entorno y el de salas iluminadas. Prime dibuja todo el escenario sólido con la mezcla activada y factores One/Zero (`blend=1/1/0`, en todas las capturas F7 del 05 al 13/11). Ningún dibujo del mundo lo recibía, y con él tampoco funcionaban en las paredes los brillos de las luces del motor, el suavizado de lightmaps ni el tramado. Ahora `opaque_blend()` acepta las dos formas. En la repetición `hangar_1x_window`: 232–352 dibujos por fotograma con `env=1 cls=15`, antes 0. El aspecto de «World surfaces» se ve por primera vez: era suave por diseño y en Bryyo apenas se nota; sus valores se ajustan en F1 > Materials.

La línea `mat` de F7 añade `env=` (variante de entorno) y `cls=` (clase de material; -1 ninguna, 14 por defecto, 15 mundo).

### Herramientas de QA nuevas

- `WII_COMPILED_QA_DUMP_EVERY=<n>` y `WII_COMPILED_QA_DUMP_DIR=<carpeta>`: volcado F7 en fotogramas exactos de la repetición (`frame_<n>`). El archivo `request` caía en fotogramas distintos en cada pasada.
- `WII_COMPILED_QA_DUMP_AB=1`: en cada punto, `frame_<n>_on` con la configuración y, cuatro fotogramas después, `frame_<n>_off` con todo lo de este apartado y el material de entorno apagados; luego restaura. La cámara de una repetición se desvía entre pasadas; cuatro fotogramas después apenas se mueve.

### Medidas (Linux, RTX 3050, i7-4790, `hangar_1x_window`, 1x)

| Pasada | FPS medios | Ventanas < 57 | GPU media |
| --- | --- | --- | --- |
| Todo lo nuevo apagado | 58,43 | 10/80 | 33,5 % |
| Todo encendido | 59,78 | 1/80 | 33,6 % |
| Encendido con capturas A/B | 59,10 | 5/81 | 34,8 % |

La variación entre pasadas es mayor que la diferencia: el coste no se distingue del ruido a 1x.

### Resultado visual y límites

- Comparación A/B dentro de la misma pasada (15 puntos): diferencia media de 0 a 29 niveles de 255 según la escena. Los cambios mayores están junto a disparos, explosiones y llamas (la luz que rebota) y en los objetos con el material por defecto.
- En Bryyo a pleno día la luz que rebota es débil: en la vista de diagnóstico, mediana 4–7/255 y percentil 99 ≈ 52/255. Es correcto para una técnica de pantalla (solo ilumina desde lo que está cerca y en pantalla), pero no es un cambio de aspecto grande. La primera versión (ventana cuadrática, peso 0,35) era casi nula y se corrigió.
- Pendiente del usuario: valorarlo en partidas reales y en salas con muchas superficies que brillan (Olympus, lava, interiores), y ajustar fuerza y alcance.
- Pendientes de la lista del 13/11: personajes con todas las luces del motor (5) y niebla volumétrica (6).

## Nivel 2, segunda parte: luces completas, reiluminación, HDR bloom y niebla (14/11/2026)

Petición del usuario tras ver que el nivel 2 cambiaba poco: los dos puntos pendientes (personajes con todas las luces, niebla volumétrica) más reiluminar el escenario con las luces reales y un HDR con bloom de varias escalas. Todas activables en F1 > Video > «Modern lighting», encendidas por defecto.

| Opción | Clave | Defecto | Qué hace |
| --- | --- | --- | --- |
| Personajes con todas las luces | `actor_all_lights` | sí | `engine_lights::inject_actor`: en los dibujos iluminados de la vista del mundo sin lightmap (y no a menos de 1,5 m de la cámara), cada ranura GX libre (en ninguna máscara de canal) recibe la siguiente luz del motor que alcanza el origen del modelo y no está cargada ya; las estáticas al 60 % porque el motor ya suma al ambiente las que no caben. Solo uniform. |
| Reiluminar paredes | `world_relight`, `world_relight_strength` (0,6) | sí | En el material de entorno, las luces del motor dan además luz difusa (Lambert con la normal de relieve, mismo alcance y puerta de sombra del lightmap que los brillos) sobre la textura de la superficie. `lit_ext[2].w`, solo en dibujos con `usesEnvLights`. |
| HDR bloom | `hdr_bloom`, `hdr_bloom_strength` (0,6), `hdr_bloom_threshold` (0,6) | sí | `kBloomPre` (1/2, lineal, alfa del EFB × 4 como brillo que no cabe en 8 bits, umbral), cinco reducciones hasta 1/64 y cinco ampliaciones con filtro tienda. Se suma en `kSceneLight` (enlace 6) con caída hacia blanco. Solo la imagen del mundo: el visor, el HUD y el cañón quedan fuera. |
| Niebla volumétrica | `volumetric_fog`, `volumetric_fog_density` (0,5) | sí | `kFog` a 1/4: 10 pasos por el rayo de vista hasta la superficie visible (máx. 40 m) para las 4 luces del motor de las sombras de contacto (ahora con su color, `note_engine_lights(..., colors)`), atenuación por alcance y Henyey-Greenstein g = 0,35. Se compone con el bloom en `kAddCompose`. |

Medidas (`hangar_1x_window` con parejas A/B, 1x, RTX 3050): 58,21 FPS de media, 8 de 81 ventanas por debajo de 57, GPU 35,1 % de media. Con todo encendido la imagen sube de media 7–21 niveles de 255 respecto a apagado (diferencia media 6–35 según la escena): paredes más iluminadas, halos amplios en disparos y llamas, y bruma iluminada en la cueva. Pendiente: valoración del usuario y ajuste de intensidades; la bruma junto al cañón en la cueva puede ser excesiva.

# Más de 60 FPS en el port de RM3E01

Petición del usuario (5 de noviembre de 2026): dar a quien tenga un PC mejor la posibilidad de superar los 60 FPS, sin dañar el juego ni RetroAchievements. El usuario no puede verlo en su monitor. La política de lógica a 60 Hz sigue en pie (`AGENTS.md`): nada de esto toca la lógica.

## Qué se implementó

F1 > Video, «Frame rate above 60 (interpolated)», clave `[video] frame_interpolation_fps`:

| Valor | Opción | Comportamiento |
|---|---|---|
| 0 | Off | Igual que siempre; el valor por defecto |
| 1 | Match monitor | Tantas imágenes por segundo como el refresco del monitor |
| 120, 144, 165, 180, 240, 360 | Fijo | Esa tasa |
| 2 | Uncapped | Sube mientras el PC tenga margen, hasta 480 |

La lógica, el VI y la memoria del juego no cambian. Aurora vuelve a dibujar el fotograma ya grabado con las matrices interpoladas entre el fotograma anterior y el actual.

En un monitor de 60 Hz o menos la opción queda apagada sola y F1 lo avisa, porque esas imágenes se descartarían y los intentos cuestan fotogramas reales. Se comprueba cada segundo, por si la ventana cambia de monitor.

## Por qué interpolar y no acelerar la lógica

El bucle del juego ya es de paso fijo:

- `0x802EBAF4` acumula tiempo con `OSGetTime` y ejecuta ticks de `lbl_806795F8` segundos. **Siempre ejecuta al menos uno por llamada**, así que si el bucle corre más deprisa, el juego se acelera.
- `0x802EA018` elige ese paso en cada vuelta: 0,02 s (PAL) o 1/60 s, según `lbl_8067DF01`. El motor está parametrizado entre 50 y 60 Hz, no para 120.
- `30fps.txt` activa un modo propio del motor con dos ticks por dibujo (`gpMain+0x12D`). El caso contrario, dos dibujos por tick, no existe.
- El bucle dibuja justo después de actualizar, en el mismo hilo.

Subir el paso a 1/120 s con el VI a 120 Hz es técnicamente posible y lo descartamos:

- **Contadores por tick.** La auditoría del port de Prime 1 (`references/MetroidPrimePort/docs/HIGH_FPS_AUDIT.md`), de la misma familia de motor, encontró rozamientos aplicados por tick, tiempos de IA y temporizadores que cambian el juego. Allí se corrigieron en el código fuente; aquí serían unas 18.500 funciones traducidas más 74 RSO, sin fuente.
- **Literales de tasa.** Solo en el DOL hay 54 referencias a 60,0 (46 funciones) y 38 a 30,0 (34 funciones), y en los RSO 44 y 57 valores. Algunos serán grados y no fotogramas; hay que revisarlos uno a uno.
- **RetroAchievements.** `rc_client_do_frame` cuenta fotogramas: los contadores de impactos, los temporizadores y las tablas (`RC_FORMAT_FRAMES` asume 60 por segundo) dejarían de valer.
- **CPU.** Duplicaría el trabajo del hilo del juego, que ya es el límite en escenas pesadas. La interpolación solo gasta GPU y codificación.

El port de Prime 1 llega a la misma conclusión (`FRAME_INTERPOLATION.md`): deja la simulación a otra tasa como experimento y usa interpolación. Allí puede interpolar actores porque tiene el código fuente; aquí se hace en la capa gráfica, que es lo que ya hacía Aurora.

## RetroAchievements

No necesita ningún cambio. `PrimeAchievements::Frame()` corre en `AdvanceRetrace`, a 60 Hz, y la interpolación no escribe memoria del juego. Comprobado en replays: VI a 60,005 Hz con la opción activa. No se bloquea como el FOV porque no altera nada de lo que lee el servicio. Sigue pendiente la QA general de RetroAchievements (ver `retroachievements.md`), que no depende de esto.

## Qué estaba roto

La interpolación heredada existía, pero en Prime empeoraba el juego. Medido a 120 en el replay `hangar`: 60 imágenes por segundo y **30 fotogramas reales**. Las causas, en orden:

1. **Prime no daba calendario.** Presenta desde el propio retrazado (`VI_HLE_PresentFrame(..., false)`), camino que no pasaba horario a Aurora ni avisaba de fotogramas tardíos. El control adaptativo nunca actuaba y las imágenes salían en ráfaga. `StampInterpolationSchedule` da el mismo calendario a los dos caminos.
2. **El limitador de latencia bloqueaba cada imagen.** `dxgi_frame_latency.cpp` limita la cola a 1 fotograma y espera en cada `Present`, con el bloqueo del dispositivo de Dawn tomado. Con interpolación activa la cola sube a 4, el objeto solo se vacía y la superficie pasa a `Mailbox`, que muestra la imagen más nueva en cada refresco sin rasgado. Fuera de la interpolación todo sigue igual.
3. **Solo múltiplos de 60.** 144 Hz no se podía representar. Ahora cada tick del monitor cae en un punto del fotograma y se dibuja con ese peso (`policy::next_frame_cadence`). A 144 salen pesos como 0,42, 0,83 | 0,25, 0,67 | 0,08, 0,5, 0,92, con las imágenes espaciadas por igual.
4. **Latencia innecesaria.** La imagen real se mostraba un fotograma y medio después. El grupo arranca ahora en cuanto suele acabar la codificación, de modo que solo se retiene lo que exige la cadencia: como máximo 1 − 1/N de fotograma (8,3 ms a 120).
5. **Control adaptativo.** Decide por encima de `policy::next_images_per_frame`: baja si se pierde el 10 % de los fotogramas o la codificación llena el 80 % del periodo, y sube solo si la siguiente tasa cabe en el 60 %. Reintenta con espera doble tras cada fallo, hasta 256 s.
6. **Ubershader apagado con interpolación.** Los materiales en compilación desaparecían. Ahora se dibujan en su posición final en las imágenes intercaladas.

## Parpadeo y texturas que desaparecen (corregido el 7 de noviembre de 2026)

Primera prueba real del usuario, ya con un monitor de 120 Hz, y en el PC de un amigo: la imagen parpadeaba y a veces desaparecían texturas. Tres causas, de mayor a menor peso.

### 1. Cada lote mezclaba dos fotogramas del juego

Prime sella un lote (`aurora_end_frame`) en su retrazado VI o en el primer sondeo del juego tras la copia de pantalla (`present_on_copy`). Para entonces el consumidor GX ya ha grabado el principio del fotograma siguiente: el lote es «final del fotograma k + copia de pantalla + principio del fotograma k+1», con un corte que cambia en cada fotograma (ya visto en el SSAO, problema 14 de `ssao_aurora.md`). Medido en el replay `hangar`: **todos** los lotes, 36 dibujos de media tras la copia.

La interpolación volvía a dibujar el lote completo en cada imagen intercalada, antes que la real y sobre el mismo EFB. Al empezar la imagen real, el EFB ya no contenía el principio del fotograma k dibujado por la real anterior, sino el principio de k+1 dibujado por la intercalada. Resultado: la imagen real combinaba dos fotogramas (objetos que faltan o aparecen en otra posición, uniones con profundidad cruzada). La intercalada, a su vez, mezclaba la cabeza del fotograma sin interpolar con la cola interpolada. Además el emparejamiento era por lote, así que con el corte variable un mismo dibujo podía no encontrar pareja o encontrar la de otra instancia.

Corrección (`gfx/common.cpp`, `gx/frame_interpolation.cpp`, `aurora.cpp`):

- Con la interpolación activa, al sellar se cortan los pases posteriores a la última copia de pantalla (`startsGameFrame`) y se aplazan al lote siguiente, cuyas imágenes (intercaladas y real) los dibujan primero. Cada imagen dibuja así un fotograma del juego entero, de su inicio a su copia. Las imágenes intercaladas solo se repiten con interpolación si el lote empieza en un inicio de fotograma (`sealed_frame_starts_game_frame`); si no, se muestran copias de la real.
- Los datos de los pases aplazados siguen en la GPU: hay dos juegos de búferes (vértices, uniformes, índices, almacenamiento: 80 MB más, creados la primera vez que se aplaza) que se alternan en cada subida, y cada pase guarda el suyo (`RenderPass::bufferSet`).
- Emparejamiento y cadencia por fotograma del juego, no por lote: el límite es la copia de pantalla (`note_frame_interpolation_boundary`). La cabeza de un fotograma se empareja al sellar su lote y la cola al sellar el siguiente, contra el mismo fotograma anterior, y lo que reclamó la cabeza no lo vuelve a tomar la cola. Un sello sin copia de pantalla no inserta imágenes; dos seguidos (un título que presenta sin copias) vuelven al límite por sello.
- Un lote cortado por una lectura del EFB a RAM o por falta de espacio sigue siendo «no repetible»: no se aplaza nada y la imagen siguiente se duplica.

Prueba objetiva (`WII_COMPILED_QA_INTERP_WEIGHT_ONE` + `WII_COMPILED_QA_INTERP_COMPARE_EVERY=15`): con peso 1 la imagen intercalada debe ser idéntica a la real. Código nuevo: **694 de 694 grupos idénticos píxel a píxel**. Comportamiento anterior (`WII_COMPILED_QA_INTERP_NO_CARRY`): 385 de 699, con hasta el 48 % de la imagen claramente distinta y 73 grupos por encima del 1 %.

### 2. Sin SSAO ni efectos de escena en las imágenes intercaladas

El SSAO (activado por defecto), los reflejos del suelo y los rayos de luz se aplicaban solo en la imagen real. A 120 Hz se alternaban imágenes con y sin ellos: parpadeo. Ahora, con fotogramas completos, cada imagen intercalada los calcula también; la lectura de colores de zona sigue siendo una por fotograma. Coste: el doble de SSAO; con la presentación simulada el `hangar` mantiene 119,5 imágenes/s.

### 3. Texturas recién cargadas sin decodificar

Las imágenes intercaladas se codifican antes que la real y no ejecutaban las decodificaciones CMPR ni las conversiones de paleta, que solo hacía la real: una textura cargada en ese fotograma salía vacía o antigua en la intercalada. Ahora la primera imagen del grupo decodifica, y las conversiones de paleta corren en todas (su origen puede ser una copia del EFB de esa misma imagen).

### Cortes de cámara

Interpolar entre dos planos distintos inventaba un plano intermedio. Para cada dibujo estático emparejado, `actual · anterior⁻¹` es el movimiento de la cámara; si al menos el 40 % de 16 o más pares saltan más de 35° o 5 m en 1/60 s, ese fotograma se muestra sin intercaladas. En 4 minutos de `hangar`: 7 cortes (cargas y transiciones), ninguno en juego normal.

### El limitador de FPS del driver (causa en el PC del usuario)

En el PC del usuario la opción nunca podía pasar de 60: el perfil global del driver de NVIDIA tiene **«Velocidad máxima de fotogramas» = 60** (ajuste de usuario) y «Modo de baja latencia» activado. El driver duerme dentro de `Present` para cumplirlo: ~10 ms por llamada con la GPU al 14 %, y como `Present` retiene el bloqueo del dispositivo de Dawn, frenaba la codificación y el control adaptativo bajaba a una imagen por fotograma. Era la «espera dentro de `Present`» que se atribuyó a un V-Sync forzado; la fase variaba despacio porque es un limitador, no la sincronía con el monitor. Con la presentación simulada el mismo PC da 119,5 imágenes/s.

- Al arrancar, `dxgi_frame_latency.cpp` lee (solo lectura, NvAPI DRS) el límite del perfil del ejecutable o, si no tiene, del global: `[frame-limit]` en el log.
- Un límite de 61 o menos deja la opción apagada, como un monitor de 60 Hz, y F1 > Video explica dónde cambiarlo. Uno mayor acota el objetivo.
- `recomp/tools/nvidia_game_profile.py check|create|delete` crea o quita un perfil de NVIDIA solo para los ejecutables del port con el limitador desactivado. No se ejecutó `create`: cambia la configuración del driver y lo decide el usuario.

### Modo de baja latencia (opcional)

F1 > Video, «Low latency (predicted in-between images)», clave `[video] frame_interpolation_low_latency` (apagada por defecto). La interpolación muestra primero la imagen intermedia entre k-1 y k, así que la real k llega medio fotograma después (8,3 ms a 120) más lo que tarda en codificarse la intermedia. En baja latencia la real k sale en cuanto se codifica y las imágenes siguientes la prolongan con su último movimiento (pesos 1 + w: 1,5 a 120). Cuando el movimiento para o cambia de sentido, la imagen predicha se pasa medio fotograma; por eso solo se extrapola si el par se mueve como mucho 30° y 30 unidades por fotograma, y si no se muestra el propio fotograma.

- `aurora.cpp`: con la opción, la real se codifica primero (sin finalizar los pases), se entrega al presentador al momento y luego se codifican las predichas, con `presentAt` = entrega de la real + (w − 1) periodos. La instantánea de profundidad para `GXPeekZ` es siempre la de la imagen real.
- Prueba de equivalencia con peso 1 en este orden: 681 de 681 grupos idénticos. Presentación simulada: 119,3 imágenes/s. Test `LowLatencyPredictsPastTheFrameOnlyForSmallMotion`.

## Juntas abiertas, líneas blancas y fuego clonado (corregido el 7 de noviembre de 2026)

Con 120 FPS ya en pantalla, el usuario vio líneas blancas parpadeantes en los bordes de las paredes (sobre todo al disparar) y el fuego de los candelabros de Bryyo duplicado en otro sitio. A 60 no pasaba. Las capturas F5 guardan la imagen real, que salía bien; el fallo estaba en las intercaladas. Tres causas, en `gx/frame_interpolation.cpp`:

1. **Emparejamiento sin límite de distancia.** El fuego y las chispas son sprites con matriz propia que nacen y mueren cada fotograma (en una captura, 9 sprites en un fotograma y 19 en el siguiente, con el mismo material). El sobrante se emparejaba con otro sprite a decenas de metros (la puerta admitía 1500 unidades) y la intercalada lo dibujaba a medio camino: el clon. Ahora `within_reach` exige que un dibujo de una matriz esté a menos de 2,5 m (`kMaximumObjectStep`) de su pareja según una de tres referencias: donde estaba en pantalla (pegado a la cámara), donde lo lleva el movimiento de la cámara (estático en el mundo) o donde lo lleva su propio último movimiento. El movimiento de la cámara se calcula por votación (`estimate_camera_delta`) entre los dibujos cuya identidad exacta aparece una sola vez en los dos fotogramas. Si no hay pareja dentro del límite, el dibujo se muestra donde está.
2. **Cada pieza giraba alrededor de su propio origen.** La interpolación descomponía cada matriz y movía su origen en línea recta mientras la cámara giraba, con un error distinto según el origen de cada pieza: dos piezas vecinas dejaban una rendija. Ahora, si `delta = actual × inversa(anterior)` es rígido, el par sigue el movimiento helicoidal de `delta` (`prepare_screw`/`evaluate_screw`), que depende solo de `delta`. Todas las piezas estáticas comparten el `delta` de la cámara y caen en el mismo sitio. Si cambia la escala, se usa la descomposición de antes. En baja latencia, la decisión de extrapolar también depende de `delta`, así que es la misma para toda la escena, y un dibujo con huesos (paleta) se extrapola entero o nada.
3. **Piezas sin pareja paradas.** La luz del disparo cambia el material de algunas piezas de pared, que perdían su identidad y se quedaban en su posición final mientras el resto se movía. Ahora hay una tercera identidad, solo geometría y topología de matrices (`FrameInterpolationDrawIdentity::geometry`), y un dibujo que sigue sin pareja toma la de un dibujo emparejado con exactamente la misma matriz (`borrowedPrevious`, sin reclamar la pareja ni sembrar predicción). Por eso se preparan copias de uniformes para todos los dibujos de un marco con predecesor.

Tests: `SpriteWithoutANearbyPartnerIsNotDrawnHalfwayToAnother`, `DrawWithoutPartnerMovesWithItsSameMatrixSibling`, `CameraTurnKeepsPiecesWithDifferentOriginsTogether` (falla con la descomposición: la esquina común de dos piezas a 30 m con giro de 12° no coincide). El usuario confirmó que los gráficos quedaron bien. Diagnóstico QA: `borrowed=` y `camera_estimates=` en `[prime-interp]`.

## Salas a 60 y salas a 120: el control del ritmo culpaba a las imágenes (corregido el 7 de noviembre de 2026)

El control baja las imágenes intercaladas cuando en una ventana de 60 fotogramas el 10 % o más llegan tarde (más de 25 ms entre sellados), y cada bajada doblaba la espera antes de volver a subir, hasta 256 ventanas (unos 4 minutos). En la grabación F8 del usuario (i7-4790), los fotogramas tardíos son los pesados del propio juego: en ellos la lógica (`func_801AF4B4`, `func_800E1298`) y la expansión de listas de visualización del consumidor GX duplican su peso, mientras que el trabajo de interpolación va en otros hilos. El hilo del juego espera al retrazado el 14,5 % del tiempo a 60 y el 15,6 % a 120, así que las imágenes no lo frenan. A 60 seguía habiendo unos 20 fotogramas tardíos cada 5 s en las salas pesadas. La carga de codificación era 0,30-0,40.

`policy::step_images_per_frame` (`render_policy.hpp`): una bajada causada solo por fotogramas tardíos es una prueba. Si en la ventana siguiente, ya sin imágenes, siguen llegando tarde al menos la mitad, se vuelve al ritmo anterior y durante 60 ventanas los fotogramas tardíos no cuentan. Si dejaron de llegar tarde, las imágenes eran la causa y se mantiene la bajada con la espera doblada (ahora como mucho 32 ventanas). Las bajadas por carga de codificación o presentación no cambian. Cada fotograma tardío sigue saliendo sin sus intercaladas. Tests `RenderPolicyImageRate.*`; `probe_restores=` en `[prime-interp]` cuenta las pruebas deshechas.

El usuario confirmó que los 120 FPS quedaron más estables. Los fotogramas tardíos del propio juego son coste de CPU de la lógica en ese procesador, un problema distinto de la interpolación.

## Mapa del menú de pausa a 30 FPS: emparejamiento cuadrático (corregido el 7 de noviembre de 2026)

Con la interpolación activada, el mapa del menú de pausa bajaba a unos 30 FPS. Con 60 FPS, o en otras pantallas del menú, no pasaba. En la grabación F8 del usuario (`stack_profile_20261107_214703.txt`), `[prime-interp]` daba `candidates=4947` y `presents_per_s` 20-35. `[frame-worker-slow] phase=seal_encode` tardaba 30-76 ms por fotograma. En esos segundos, el 61 % de las muestras del hilo de fotogramas estaba en el emparejamiento (`process_segment`: `transform_match_distance_squared`, `within_reach` y la ordenación de aristas). En segundos normales era menos del 1 %.

El mapa dibuja miles de piezas con la misma malla y el mismo material, y el emparejamiento comparaba cada una con todas. La rejilla de celdas de 1500 unidades metía todo el mapa en una celda. Además, el tope de 65 536 aristas solo contaba los pares aceptados. Con piezas separadas, la puerta de 2,5 m rechazaba casi todos los pares y el recorrido llegaba a N² completo. En la prueba con 3150 piezas costaba 143 ms por fotograma con piezas separadas y 17 ms con piezas juntas.

Cambios en `frame_interpolation.cpp`:

- Las piezas que conservan su matriz se emparejan primero por hash del valor exacto (distancia cero, el par que el voraz elegiría igual).
- La rejilla usa celdas de 2 × 2,5 m × 1,01. Cada entrada anterior se archiva bajo sus tres referencias de alcance: posición, predicción y movimiento de cámara. Son las de `reach_references`, que comparte con `within_reach`. Cada dibujo consulta solo el bloque de 2 × 2 × 2 celdas más cercano. Es una lista plana ordenada, sin asignaciones por celda, y no oculta ningún par que la puerta aceptaría.
- El trabajo se cuenta antes de hacerlo: 16 384 + 8 por dibujo. Si se supera, como con piezas apiñadas dentro del alcance unas de otras, se recurre al emparejamiento por orden de envío. El mismo fallback ya existía al desbordar, pero antes llegaba después de evaluar 65 536 pares en cada pasada.

Resultado en `FrameInterpolationContract.ThousandsOfLookalikeDrawsMatchCheaplyAndToThemselves`: 0 pares evaluados con las piezas juntas y 14 256 (unos 4,5 por pieza) con las piezas separadas, frente a unos 10 millones antes. Cada pieza sigue emparejada consigo misma mientras el mapa se desplaza. El tiempo restante, 5-10 ms en la prueba, es el coste fijo por dibujo, el mismo que con identidades únicas. `frame_interpolation_pair_evaluations()` expone el contador para las pruebas.

Segunda grabación (`stack_profile_20261107_222355.txt`): el bajón a 30 desapareció. Con el mapa alejado o en perspectiva quedaba a 60. Con dos imágenes, la carga de codificación era 0,53 y había 10/60 fotogramas tardíos. La prueba de ritmo bajó a una imagen y la mantuvo. Con una imagen la carga era 0,29, y para subir hace falta que 0,29 × 2 < 0,5, así que no volvía a subir. El emparejamiento seguía siendo un 19 % del hilo de fotogramas. Venía de muchos grupos medianos apiñados por la vía de todos los pares (≤ 1024 pares por grupo).

Cambios:

- El emparejamiento por matriz idéntica se aplica a todos los grupos de una matriz, no solo a los grandes, así que un mapa quieto se empareja en tiempo lineal.
- Hay un presupuesto de pares para todo el segmento, con la misma fórmula (16 384 + 8 por dibujo). Un grupo que ya no cabe se empareja por orden de envío.
- `transform_match_distance_squared` calcula en float. Cualquier valor no finito o desbordado deja la suma no finita.

En la prueba, con grupos de 30 piezas apiñadas desplazándose, se miden 41 400 pares por fotograma, el tope, frente a unos 94 500 sin presupuesto.

Tercera grabación (`stack_profile_20261107_224327.txt`): el mapa alcanza 120 a ratos y vuelve a 60. Con dos imágenes, el hilo de fotogramas estaba ocupado ~79 %. Codificar en Dawn era ~40 %, la preparación por dibujo ~28 % y el emparejamiento seguía siendo ~20 % con una imagen. Había 6-7/60 fotogramas tardíos, justo el umbral del 10 %. La prueba bajaba a una imagen, dejaban de llegar tarde y se mantenía la bajada con espera doblada.

Cambios:

- El presupuesto de pares baja a 4096 + 4 por dibujo.
- Al agotar el presupuesto, cada pasada empareja por orden de envío dentro de su propia identidad. Antes solo lo hacía la pasada estable, que mezcla mallas distintas con el mismo material.
- Las referencias de alcance de cada entrada anterior se calculan una vez por segmento, no una vez por par.
- Los vectores de pares preparados (megabytes con 5000 dibujos) se conservan entre segmentos en vez de asignarse en cada fotograma.

En la prueba de grupos de 30 piezas apiñadas se miden 16 660 pares por fotograma, frente a 41 400.

La política (umbral del 10 %) no se ha tocado: esos fotogramas tardíos son tirones reales.

Cuarta grabación (`stack_profile_20261107_225604.txt`): el mapa ya no baja de ~100 y casi todo el tiempo está con dos imágenes (101-121 por segundo, ~1 fotograma tardío por segundo). El hilo de fotogramas estaba ocupado ~94 %:

- codificación Dawn y driver ~55 %: 5000 dibujos × 2 imágenes;
- preparación de interpolación ~28 %.

Medido por fases en la prueba (3150 dibujos): emparejamiento 0,8-2,3 ms, preparación de pares 1,1 ms, detección de corte 0,25 ms e interpolación en el pool 0,3 ms.

Cambios:

- La preparación de pares de una matriz (`prepare_affine_pair`, posición y normal) se reserva en el bucle serie y se rellena en el `InterpolationWorkerPool`.
- `MatchEdge` pasa a 12 bytes y se ordena por los bits de la distancia (finita y no negativa, monótona como entero).
- La detección de corte usa una muestra de 256 pares repartidos.
- En las pasadas posteriores se comprueba primero si quedan dibujos sin pareja antes de buscar el grupo.
- La comprobación de matrices idénticas se hace primero en orden de envío con `memcmp`. El hash solo se usa si algo estaba quieto o el grupo es diminuto.
- Antes de construir la rejilla se estiman los pares por la extensión del grupo, como si estuviera repartido uniformemente: agrupado solo añade pares. Si la estimación supera 4× el presupuesto, el grupo pasa directamente a orden de envío.
- La rejilla busca celdas en un `absl::flat_hash_map` de rangos en vez de con búsqueda binaria.
- `pipeline_cache.cpp`: cada hilo recuerda las pipelines que ya encontró listas (manejadores crudos y generación que se incrementa con `g_pipelines.clear()`, la única operación que quita entradas). Así `bind_pipeline` ya no toma `g_pipelineMutex` en cada cambio de pipeline.

Prueba de mapa, mediana por fotograma (con el juego abierto, medidas ruidosas):

| Caso | Antes | Ahora |
|---|---|---|
| Juntas | ~3,0 ms | ~2,2 ms |
| Grupos de 30 | ~3,2 ms | ~2,5 ms |
| Separadas | ~4,0 ms | ~3,3 ms |

De lo que queda en el hilo de fotogramas, parte se ejecuta ahora en el pool.

Si el hilo sigue saturado, lo único grande que queda es la codificación Dawn (dos imágenes de ~5000 dibujos). Reducirla exigiría solapar la codificación de la imagen nativa con la preparación de la intercalada, que hoy debe terminar antes de desmapear el staging en `end_batch_impl`.

Quinta grabación (`stack_profile_20261107_231553.txt`, mapa de otra zona): el usuario nota que se mantiene más cerca de 120, pero un tramo de ~30 s cayó a 60.

La causa fue la política, no la carga:

- La bajada empezó en una ventana de transición (206-344 dibujos) con 6/60 fotogramas tardíos del propio juego. La prueba bajó a una imagen, los tardíos cesaron y se mantuvo.
- Ya en el mapa, con una imagen la carga era 0,29. El crecimiento pedía 0,29 × 2 < 0,50, así que no volvía a subir aunque el hilo de fotogramas estaba ocioso el 63 %. Con dos imágenes, ese mapa mide 0,43-0,59.

`next_images_per_frame`: el paso de la imagen nativa a la primera intercalada usa un margen de 0,65 (`kRoomToGrowFromNative`), todavía por debajo del umbral de sobrecarga de 0,70. Los pasos siguientes mantienen 0,50 con la predicción lineal. Tests en `ControllerClimbsOnlyWhenTheNextRateFits`.

Tras la grabación, en el mapa de 3716 dibujos hubo una bajada por carga (encode 0,72 con dos imágenes, 11/60 tardíos). No quedó en ningún F8; pendiente de otra grabación si se repite.

## Cuelgue corregido: inversión de bloqueos en `efb_ram`

Afecta también a quien juega sin interpolación, aunque con mucha menos probabilidad. Dos hilos:

- El presentador, dentro de `Present` y con el bloqueo del dispositivo de Dawn, recibe el aviso de un `MapAsync` terminado. `complete_async_slot` quiere `g_asyncMutex`.
- El hilo de sellado tiene `g_asyncMutex` en `seal_async_downloads` o `encode_async_downloads` y llama a Dawn (`CreateTexture`, `CreateBuffer`, comandos), que quiere el bloqueo del dispositivo.

Resultado: congelación permanente, con el VI y el audio vivos. Se reprodujo en el replay real a 120. La regla es que **no se llama a Dawn con `g_asyncMutex` tomado**, ni se destruye un búfer con él: `AsyncState::Encoding` reserva el slot mientras el mutex está suelto. `depth_peek` y `post_process` ya cumplían la regla.

Pista para futuras capturas: `capture_prime_hang.py` con el presentador dentro de `webgpu_dawn.dll` en una espera de mutex, con un símbolo `CallbackHelper<WGPUBufferMapCal...` en la pila, y el hilo de sellado también en Dawn.

## Imágenes sincronizadas con el refresco del monitor (8 de noviembre de 2026)

El usuario notaba que, a 120 en un monitor de 120 Hz, la imagen a veces no se sentía fluida aunque el contador marcara 120, y la mayor parte del tiempo sí. Preguntó por el V-Sync.

**Causa.** Con interpolación, la superficie está en `Mailbox`: en cada refresco el monitor muestra la imagen terminada más reciente, sin rasgado y sin bloquear al juego (el V-Sync clásico, `Fifo`, bloqueaba el hilo que codifica y por él al juego). El presentador entregaba cada imagen según el reloj del juego (rejilla VI de 16,666 ms) o, en baja latencia, en cuanto terminaba de codificarse, con unos 2 ms de variación. Cuando esas entregas caían cerca del instante de un refresco, un milisegundo decidía en cuál entraban: dos en el mismo (la primera nunca se ve) y ninguna en el siguiente (la anterior se ve dos veces). La fase entre los dos relojes cambia despacio, de ahí los ratos buenos y malos.

**Medición objetiva.** Interruptor privado `WII_COMPILED_QA_DISPLAY_CADENCE=1` (`dxgi_frame_latency.cpp`): un hilo lee cada ~1,5 ms `GetFrameStatistics` de DXGI (qué `Present` está en pantalla y desde qué refresco) con una referencia propia de la cadena de intercambio. Como cada imagen visible dura al menos un refresco, se ve cada una: `[display-cadence]` cada 10 s cuenta imágenes mostradas, pasos limpios (una nueva por refresco), refrescos repetidos e imágenes perdidas. Una primera versión leía justo después de cada `Present` y, con la sincronía, coincidía con el instante en que DXGI actualiza: inventaba parejas de pérdida y repetición. Funciona en ventana sin bordes con el modelo flip de Dawn en Windows 10.

**Qué se hizo.**

- `display_refresh.cpp` (Aurora, Windows): un hilo espera los blancos verticales del monitor de la ventana (`IDXGIOutput::WaitForVBlank`, salida localizada con `MonitorFromWindow`) solo mientras la interpolación y la opción están activas. `policy::observe_refresh` (`render_policy.hpp`) ajusta con un lazo de segundo orden la fase y el periodo, descarta despertares tardíos y exige 120 refrescos regulares, poco jitter y un periodo a menos del 1 % del modo de pantalla. En el PC del usuario fija entre 119,977 y 120,005 Hz con 13-55 µs de jitter según la sesión.
- Refresco variable (G-Sync/FreeSync): los refrescos siguen a las entregas y alinear se perseguiría a sí mismo. Si la regularidad se pierde mientras se usa, se espera 2 s, 8 s y 32 s antes de reintentar y a la cuarta se deja apagado en ese monitor (`[refresh-sync]` en el log).
- `encode_sealed_frame` da a cada imagen un tiempo estable `alignAt`: la frontera VI de su fotograma más un desfase que sigue el lado tardío de cuándo están listas (sube en pocos fotogramas, baja despacio); las intercaladas, (w − 1) o (w − sesgo) periodos después. Una imagen tardía sale tarde en su propio refresco en vez de desplazar a todas las siguientes. Con la media en lugar del lado tardío, la mitad llegaban tarde en las salas pesadas.
- El presentador (`align_to_display`) lleva ese tiempo a la rejilla del monitor con `policy::align_to_refresh`: cada imagen sale al 20 % de su intervalo, un refresco después de la anterior. El desfase medio se mantiene entre −0,2 y 1,05 periodos con histéresis; cuando la diferencia de relojes lo saca del margen se mueve un refresco (una repetición o una imagen compartida, inevitable). Nunca se retiene una imagen más de dos refrescos. Solo si las imágenes van como mucho al ritmo del refresco (paso ≥ 0,95 periodos); también con una imagen por fotograma, para que cada una ocupe dos refrescos exactos.
- **Solo con margen** (`policy::step_align_gate`, `frame_interpolation_has_headroom`): se alinea tras dos ventanas de ritmo tranquilas (tasa al máximo, como mucho 2 de 60 fotogramas tarde, carga de codificación < 0,6). Si se pierde mientras se alinea, la siguiente apertura espera el doble (hasta 32 ventanas); un minuto sin problemas lo perdona. Además, `policy::note_alignment_lateness` la aparta mientras más del 10 % de las imágenes llegan tarde a su refresco y vuelve tras dos ventanas tranquilas. Motivo, medido: en baja latencia, una imagen retenida para su refresco conserva su búfer de la cadena de intercambio y deja la cola de la GPU detrás de su `Present` (D3D12 voltea después de todo lo enviado antes); en las salas pesadas el juego llegaba tarde mucho más a menudo y el control de ritmo se quedaba en una imagen por fotograma.
- F1 > Video, «Sync images to the monitor's refresh», clave `[video] frame_interpolation_display_sync` (activada por defecto, solo actúa con la interpolación encendida). F1 dice a qué frecuencia está sincronizado, si está en pausa por falta de margen o por qué no se sincroniza. Coste: unos 4 ms de latencia media mientras alinea.

**Parámetros.** Elegidos en una simulación (`RenderPolicyRefreshAlignment.*`): 120 imágenes/s contra 119,98 Hz durante 200 s, imágenes listas ±2 ms alrededor de su tiempo estable y una GPU que tarda hasta 3 ms. Sin alinear salen ~3.900 repeticiones y pérdidas; alineado, las ~4 que obliga la diferencia de relojes. La fase 0,35 dio lo mismo que 0,2 en el juego. Interruptores privados: `WII_COMPILED_QA_ALIGN_PHASE`, `_EARLIEST`, `_LATEST`; `WII_COMPILED_QA_NO_DISPLAY_SYNC=1` vuelve a la entrega sin alinear. `[refresh-align]` cada 10 s cuenta imágenes a tiempo y tardías y el desfase medio.

**Resultados en el PC del usuario** (i7-4790, RTX 3050, monitor de 120 Hz, ventana sin bordes; replays `hangar_i120_lowlat` y `hangar_i120`, lector exacto). Las vueltas varían mucho de una a otra en este PC (VS Code cargaba la CPU en segundo plano), así que solo se comparan corridas alternas en las mismas condiciones.

| Prueba | Sin sincronía | Con sincronía |
|---|---|---|
| Tramo tranquilo (80 s), baja latencia, tres pares alternos: refrescos limpios | 85,9 / **44,0** / 98,0 % | 99,2 / 85,8 / 83,2 % |
| Ventanas de 10 s con la alineación activa | — | 98 a 99,9 % limpias |
| Vuelta completa, baja latencia, con la puerta de margen: FPS medios | 103,1 | 103,2 |
| Ventanas a una imagen por fotograma en esa vuelta | 7 de 86 | 7 de 86 |

- Sin sincronía, la calidad depende de la fase entre los dos relojes en cada sesión: de 44 % a 98 % de refrescos limpios en el mismo tramo. Es el «a veces fluido y a veces no» del usuario.
- Con la alineación activa, las ventanas de 10 s salen limpias al 98-99,9 %. Las peores corridas «con» son ventanas en que la puerta de margen la tenía cerrada (el PC iba cargado) y se volvió a la entrega de antes.
- Antes de la puerta, alinear siempre en baja latencia empeoraba las salas pesadas (83-100 FPS frente a 103, 43 de 84 ventanas a una imagen por fotograma). Con ella, esas salas quedan como sin sincronía.
- Tras el arreglo de `efb_ram` (abajo), las 12 vueltas completas a 120 reales terminaron; antes se colgaban 6 de 7.

**Cuelgues encontrados por el camino.** Las pruebas a 120 reales se colgaban a menudo antes de terminar el recorrido, con y sin la sincronía. Las capturas (`capture_prime_hang.py`) señalaron la lectura asíncrona de copias EFB a RAM (`efb_ram_copy.cpp`): corregido, ver «Cuelgue 2» en `iluminacion_motor.md`. Después, todas las vueltas completas terminaron. Aparte queda una desincronización del replay `hangar_i120*` hacia el fotograma 15.000 (`Unsupported /dev/stm/immediate ioctl=0x3002` y una escritura MMIO bloqueada en `0xcd000004`), que ya aparecía el 5 de noviembre y no volvió a salir tras el arreglo.

## Huecos negros en Samus durante cinemáticas con baja latencia (corregido el 10 de noviembre de 2026)

El usuario vio parpadeos y huecos negros en la geometría de Samus (Zero Suit y con traje) en las cinemáticas del principio, solo a 120 con interpolación y nunca a 60. Las ráfagas F5 no lo muestran: guardan la imagen real, no las intercaladas. En esas tomas Samus es un modelo con paleta de huesos (hasta 10 matrices por dibujo) partido en varios dibujos que comparten huesos, y el usuario juega en baja latencia (las intercaladas se extrapolan medio fotograma más allá del real).

Dos fallos en `gx/frame_interpolation.cpp`:

1. **Sin freno de rotación para los huesos.** `prepare_indexed_pair` solo medía la traslación; `rotationDot` quedaba en 1, así que `extrapolation_allowed` dejaba predecir cualquier hueso aunque girara 90° por fotograma. En un parpadeo o en las piezas del traje encajando, la pose predicha se pasaba de largo. Ahora mide el giro por los tres ejes de la matriz (el mismo umbral de 30° por fotograma que las piezas rígidas) y no predice si la longitud de un eje cambia más de un 20-25 %.
2. **Decisiones por dibujo, no por esqueleto.** Si una pieza no tenía pareja válida se mostraba en su pose real mientras la vecina se interpolaba, y en baja latencia una pieza podía extrapolarse y la vecina no: la malla se abría por la costura. Ahora los dibujos cuyas paletas comparten un hueso idéntico byte a byte forman un esqueleto (unión-búsqueda sobre las mismas claves que ya emparejaban los huesos) y toman una sola decisión: si una pieza no se puede interpolar, todo el esqueleto muestra el fotograma real en esa imagen; si un hueso no se puede predecir, ninguna pieza se extrapola.

El coste es que, en esas imágenes concretas, el personaje se ve a 60 en vez de partirse. Tests `LowLatencyDoesNotCarryAFastTurningBonePastTheFrame` y `PiecesOfOneSkeletonTakeTheSameDecision`; los dos fallan con el código anterior (el hueso rápido se extrapolaba y la pieza vecina se movía mientras la otra se quedaba quieta).

Sin comprobar en el juego: no hay grabación QA de la partida nueva con esas cinemáticas. Pendiente de que el usuario las vea de nuevo a 120 con baja latencia.

## Pantallas verdes de la nave que parpadean al girar (corregido el 10 de noviembre de 2026)

En la cabina de la nave de Samus, las pantallas verdes de la consola parpadeaban al girar la cámara a 120; a 60 no. Las ráfagas F5 (`capture_20261110_0449*`) muestran solo la imagen real, pero sus listas de dibujos lo explican: en los dibujos de la consola con mapa de entorno o proyectado (`ppl=1 metal=1`, texgen desde la normal) la matriz de textura `tm2` lleva la rotación de la cámara (`1,-0.00087…` en un fotograma, `0.99932,-0.0369…` en el siguiente, unos 2° de giro). La interpolación movía la geometría a medio camino pero dejaba las matrices de textura del fotograma actual, así que en cada imagen intercalada la capa brillante de las pantallas iba medio fotograma por delante del panel y volvía: parpadeo. Los bucles de animación de las pantallas (capas aditivas cuyo color konst late 0,70 → 0,55 → 0,40 → 0,25 por fotograma, y desplazamientos lentos de UV) no lo causan.

Corrección: `shader_info.cpp` anota dónde están en el bloque de uniformes las matrices de textura que el dibujo usa (las de la región de posición con ranura ≥ 10 y las post-transform usadas), `record_interpolation_draw` guarda su valor en el fotograma (`InterpolationFrame::textureMatrices`) y al rellenar cada imagen intercalada se interpolan con el mismo peso que la geometría si el paso fue suave (`texture_matrix_moves_smoothly`: giro de hasta 0,2 por coeficiente con su desplazamiento, o un desplazamiento puro de hasta 0,02, un desplazamiento lento). Un salto (una animación que cambia de celda, un giro muy rápido) conserva el valor del fotograma. Se aplican después de la geometría y solo si esta se movió: un esqueleto o una pieza que no se predice en baja latencia deja también sus mapas en el fotograma. Test `TextureMatricesMoveWithTheCameraButNotAcrossJumps`.

Mejora de paso: los reflejos de entorno de todas las superficies metálicas (cabina, armadura) acompañan ya al giro en las imágenes intercaladas. Pendiente de que el usuario lo vea en la nave.

## Lo que queda por comprobar

El usuario tiene un monitor de 120 Hz desde el 7 de noviembre y ese mismo día quitó el límite de 60 del driver (`[frame-limit] ... off`): ya juega a 120 en pantalla. Sigue pendiente:

- La fluidez real y el rasgado a 120, 144 o más. La cadencia está probada por tests y por el presentador simulado, no a ojo.
- La latencia percibida al apuntar.
- Pantalla completa exclusiva a más de 60 Hz. En ese modo, con interpolación, se quita el V-Sync forzado porque `Mailbox` ya evita el rasgado; está sin ejercitar.
- Que `Present` ya no bloquee sin el limitador (con `Mailbox` no debería esperar al refresco).
- Sincronía con el refresco (8 de noviembre): la impresión del usuario jugando a 120; con G-Sync/FreeSync activo (debería apagarse sola por irregular, sin probar); en pantalla completa exclusiva y con monitores de 144 Hz o más; el coste de latencia (~4 ms) al apuntar. La puerta de margen podría ser demasiado estricta en un PC con carga de fondo.

Límites conocidos de la técnica:

- Solo se interpolan las matrices de los dibujos en perspectiva; una textura que se desplaza o una partícula que nace avanzan a 60 Hz.
- Un dibujo sin pareja (ni hermano con la misma matriz) se muestra en su estado final.
- El apuntado sigue recibiendo la entrada a 60 Hz. Una mejora posible, no hecha: girar la cámara en las imágenes intercaladas con el último movimiento del ratón, como la fase 1 del port de Prime 1.

## Cómo probarlo sin monitor rápido

Interruptores privados, solo en el build con `MKW_PRIME_QA_TOOLS`:

| Variable | Efecto |
|---|---|
| `WII_COMPILED_QA_SKIP_PRESENT=1` | No toca la superficie y retiene las imágenes si la GPU va por detrás, como haría la cola. Simula un monitor de cualquier refresco |
| `WII_COMPILED_QA_INTERP_IGNORE_MONITOR=1` | No apaga la opción en un monitor de 60 Hz ni por el límite de FPS del driver (en la simulación no se llama a `Present`) |
| `WII_COMPILED_QA_INTERP_HOLD_RATE=1` | Mantiene la tasa configurada, sin control adaptativo |
| `WII_COMPILED_QA_INTERP_SLOTS_ONLY=1` | Muestra solo imágenes intercaladas: toda captura de pantalla es una de ellas |
| `WII_COMPILED_QA_INTERP_WEIGHT_ONE=1` | Las imágenes intercaladas usan peso 1: deben ser idénticas a la real |
| `WII_COMPILED_QA_INTERP_COMPARE_EVERY=N` | Cada N grupos lee de la GPU la primera intercalada y la real y registra `[interp-compare]` con los píxeles distintos |
| `WII_COMPILED_QA_INTERP_NO_CARRY=1` | Vuelve a repetir lotes sin aplazar la cabeza del fotograma siguiente (comportamiento anterior, solo para comparar) |
| `WII_COMPILED_QA_DISPLAY_CADENCE=1` | `[display-cadence]` cada 10 s: lo que llegó de verdad a la pantalla según DXGI (imágenes mostradas, refrescos repetidos, imágenes perdidas) |
| `WII_COMPILED_QA_NO_DISPLAY_SYNC=1` | Entrega sin alinear con el refresco del monitor, para comparar |
| `WII_COMPILED_QA_ALIGN_PHASE`, `_EARLIEST`, `_LATEST` | Fase y márgenes de la alineación (en periodos de refresco) |

El replay registra `[prime-interp]` cada 5 s y `[dxgi-present]` cada 600 llamadas. `WII_COMPILED_GPU_TIMING=1` cambia la clave de caché de Dawn y añade cientos de MB a la caché: solo para diagnóstico.

## Resultados

Replay privado `hangar` completo (23.351 fotogramas de entrada, unos 7 minutos, 1920x1080 sin bordes), RTX 3050 e i7-4790, monitor de unos 60 Hz. Todas las pruebas terminan en el mismo fotograma de entrada y con el VI a 60,00 Hz.

| Prueba | Imágenes/s (media; mín-máx) | Fotogramas reales/s (media; mín) | Notas |
|---|---|---|---|
| Opción apagada | 59,7 (46-60) | no aplica | `Immediate`, cola 1: como antes. Media del informe 59,91 |
| Antes de los cambios, 120 | 60 | 30 | La interpolación empeoraba el juego |
| 144 simulado | 141 (122-146) | 59,3; 53,8 | 0 cambios de tasa. Espaciado 6,94 ms, p95 7,40 ms |
| Sin límite simulado | 264 (92-480) | 58,8; 52,4 | 19 cambios de tasa. 126 avisos de grupos descartados por el presentador |
| Sin límite real, monitor de 60 Hz (antes de apagarla ahí) | 59,5 | 58,8; 51,0 | No pasa de 60: `Present` espera al refresco. Por eso se apaga sola |
| 120 real forzado, sin control adaptativo | 59,6 | 38,2; 35,2 | Sin cuelgues. Muestra lo que el control evita |
| Alternando apagado y encendido cada 10 s | 59,8 (56-60) | no medible | 43 cambios de modo de presentación, sin cuelgues ni errores |

Otros datos:

- Dibujos emparejados: 95-97 % de media en fotogramas de más de 100 dibujos. Ningún fotograma tuvo que duplicarse por copias del EFB a RAM.
- «Simulado» significa con `WII_COMPILED_QA_SKIP_PRESENT`: la GPU renderiza cada imagen, pero la superficie no se toca, de modo que la espera de `Present` no cuenta. En un PC real con monitor rápido el resultado estará entre esa fila y la de 120 forzado, según cuánto bloquee su `Present`.
- Sin límite cuesta más que igual al monitor: unos 0,9 fotogramas reales por segundo frente a la opción apagada en este PC, el más débil al que apunta la opción. Por eso la recomendación es «Match monitor».
- Capturas de pantalla mostrando solo imágenes intercaladas (`recomp/qa/runs/hangar_i120/20261105_014631_slots_visual2`): HUD, arma, nave y escena correctos.
- Pruebas: `test_graphics.ps1` completo (275 + 8 + 44 de Aurora, 29 del runtime, Python). Las dependencias DLL del ejecutable no cambian. Los módulos modificados compilan sin `MKW_PRIME_QA_TOOLS`, comprobado solo en sintaxis: no se generó un build público.

Evidencia: `recomp/qa/runs/` (privado) y `recomp/checkpoints/20261105_fps_altos.json`.

7 de noviembre, tras la corrección por fotogramas del juego (mismo PC, monitor principal de 120 Hz con el límite de 60 del driver, replay `hangar` de 3 a 4 minutos):

| Prueba | Imágenes/s | Notas |
|---|---|---|
| 120 real (driver a 60) | 59,95 | `Present` ~10 ms por llamada, GPU al 14 %: el control baja a 1 imagen por fotograma |
| 120 simulado | 119,5 (mín. 115) | 0 reducciones; 99,6 % de los grupos con intercaladas reales; SSAO en todas |
| 144 simulado | 141,2 (mín. 127) | Igual que antes de la corrección |
| 120 simulado, baja latencia | 119,3 (mín. 113) | |
| Opción en 120 sin interruptores (driver a 60) | 59,96 | Detecta el límite, queda apagada; F1 lo explica |
| Peso 1, comparación intercalada/real | — | 694/694 idénticas (nuevo), 385/699 (anterior) |

Pruebas: `test_graphics.ps1` completo (278 + 8 + 44 de Aurora, con tres tests nuevos de interpolación; 29 del runtime; Python). Trilogy regenerado con `make_title_tree.py` e instalado. Checkpoint: `recomp/checkpoints/20261107_interpolacion_fotogramas.json`.

## Trilogy (R3ME01)

Sus ejecutables salen de copias generadas, así que un cambio en las fuentes de RM3E01 no les llega hasta regenerar `titles/` y recompilar (`docs/trilogy_r3me01.md`). Hecho el 05/11/2026: solo hizo falta `make_title_tree.py` y Ninja, sin volver a traducir, porque desde el build anterior solo cambiaron archivos de este trabajo.

Recorrido menú → MP3 hasta la cabina de la nave, 170 s, ventana de 854x480, perfil privado (`titles/probe_runs/`):

| Prueba | Menú | MP3 | Notas |
|---|---|---|---|
| Apagada | 60 FPS | 60 FPS | `OSExec` y cortina como antes; captura de la cabina correcta |
| 144 simulado | 144 imágenes/s | 144 imágenes/s | 0 bajadas de tasa, 0 fotogramas no aptos. Modo `Mailbox` en los dos ejecutables |

Sin comprobar en Trilogy: la imagen de las intercaladas (con la simulación la ventana no se actualiza), fotogramas reales por segundo, jugar con la opción encendida y abrir F1 en estos ejecutables; el texto de la opción está en los dos binarios.

## Cuelgue del grupo de hilos de la interpolación (08/11/2026)

Síntoma: en una reproducción QA a 120 Hz (`hangar_i120` con `WII_COMPILED_QA_INTERP_IGNORE_MONITOR=1`), el juego dejó de responder a los ~80 s de partida. Ventana congelada, log detenido y CPU todavía en uso.

Captura (`capture_prime_hang.py`): el hilo del juego esperaba a `aurora_wait_for_frame_worker_sealed`; el hilo que sella daba vueltas en `std::this_thread::yield()` de `InterpolationWorkerPool::run` (`finalize_frame_interpolation+0x3525`); los hilos de trabajo estaban parados.

Causa: los contadores del grupo (`m_count`, `m_next`, `m_remaining`) eran compartidos y se reiniciaban en cada reparto. Un trabajador despertado tarde para el reparto anterior entraba en el siguiente con el `count` viejo que había leído, reclamaba un bloque de 16 del reparto nuevo y restaba más de lo que quedaba. `m_remaining` (sin signo) daba la vuelta y nunca volvía a cero.

Corrección: cada `run()` crea su propio `Job` (función, tamaño, contadores, trabajadores activos). Los trabajadores toman un `shared_ptr` del trabajo bajo el cerrojo. Un rezagado se queda con un trabajo agotado: no encuentra bloques, nunca llama a su función y no toca el reparto nuevo. `run()` espera solo a los trabajadores de su propio trabajo.

Contexto: apareció al probar la iluminación moderna (nivel 1, `iluminacion_motor.md`), cuyos pases añaden trabajo a las imágenes intercaladas y cambian los tiempos. El fallo es anterior y también podía darse sin ella.

## Bajones en el mapa y en un corredor: controlador de imágenes (08/11/2026, 20:58)

Perfil F8 `stack_profile_20261108_204003.txt` y `base_1794188045_pid18684/console.log` (pid 18684, 120 Hz, iluminación moderna activa). El mapa de esta vez tenía 5.271 dibujos (4.950 la noche anterior). Con una imagen por fotograma el trabajador de fotogramas usaba 0,26-0,32 del periodo; con dos, 0,60-0,72, con 22-44 de cada 60 fotogramas del juego tarde. El juego espera a que el trabajador selle cada fotograma (`AdvanceRetrace` → `aurora_wait_for_frame_worker_sealed`), así que ese retraso le quita fotogramas reales: 43-55 fotogramas sellados por segundo (delta de `sealed=`) mientras el contador marcaba ~80-100. (`prime-input-latency frames=` no sirve para esto: cuenta de menos.) La iluminación moderna no es la causa: 0,3 % del hilo decodificador, casi nada en el trabajador y `HookedPresent` en 2,7 % (la GPU no limita).

Tres fallos del controlador (`policy::step_images_per_frame`):

1. Una sonda que no veía desaparecer los retrasos restauraba dos imágenes aunque una sola ya costara 0,51, y quedaba inmune un minuto: ~1 minuto con 43-55 fotogramas reales (ventanas 63-69 y 108).
2. Crecer de 1 a 2 imágenes a 0,28-0,32 (predicción 0,56-0,64 < 0,65) fallaba siempre y se reintentaba cada 20-80 s, con caídas de 1-2 s a 30-45.
3. En un corredor, tras el mapa, un pico breve (0,49 con 7/60 tarde) bajó a 60 y la espera acumulada en el mapa lo dejó ~10 s a 60 con carga 0,15.

Corrección: la sonda solo restaura si la tasa anterior cabe (`carga × imágenes < 0,70`); un crecimiento que falla en sus dos primeras ventanas con carga ≥ 0,55 se recuerda con la carga de partida y no se repite en la misma escena hasta que la carga baje al 80 % o pase ~1 minuto; una espera ganada en una escena se olvida cuando la carga cae por debajo del 60 % de la medida con menos imágenes (medida en la ventana siguiente a una reducción). Pruebas: `RenderPolicyImageRate.AProbeDoesNotRestoreARateThatWouldNotFit`, `AGrowthThatFailsAtOnceIsNotRetriedInTheSameScene`, `AnOverloadRightAfterGrowingAlsoCountsAsAFailedGrowth`, `AClearlyLighterSceneForgetsTheWaitAndTheFailedGrowth`, `TheWaitAfterAReductionIsMeasuredAtTheLowerRate`; `test_graphics.ps1` completo en verde.

Efecto esperado: en ese mapa pesado, 60 estables con todos los fotogramas del juego en lugar de oscilar entre ~100 y 30-45; en mapas o zonas más ligeros sigue subiendo a 120. Queda pendiente el QA del usuario en el mapa (no hay grabación QA del mapa). Hacer caber dos imágenes en el mapa necesitaría abaratar el trabajador: Dawn `Finish`/`Submit` ~19 %, codificación de la imagen intercalada ~21 % y emparejamiento ~21 % de su tiempo.

### Segunda revisión (21:17-21:25): el juego más lento por carga externa

Con el controlador corregido el mapa ya no oscila, pero se quedaba en 52-56 fotogramas reales por segundo con una sola imagen (delta de `sealed=`). La noche anterior, con 2 imágenes, sellaba 59,4 y mostraba 120 con el mismo número de dibujos. El hilo del juego estaba ocupado ~90 % dibujando el mapa sin esperar a Aurora (esperas del trabajador 5 %, `fifo::sync` 4 %). Los hooks de iluminación moderna no aparecen en ese camino y sus pases apenas suman en el trabajador. La causa encontrada fue carga externa: tres copias de un vigilante de cuelgues de QA (`hang_watch.sh`, lanzado por una sesión de Claude a las 11:59-13:06) llevaban toda la tarde lanzando `ls`, `sed`, `tasklist`, `grep` y `date` cada 5 s, y Bitdefender analizaba cada proceso. Se veían en `[host-stall]` como `bash.exe`, `sed.exe`, `tasklist.exe`, `cygwin-console-helper.exe` y `bdservicehost.exe` junto a Chrome (ráfagas de ~0,9 s) y VS Code. Se cerraron a las 21:30. Pendiente: repetir el mapa sin ellos y comparar con la iluminación moderna activada y desactivada.

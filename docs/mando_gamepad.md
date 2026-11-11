# Soporte de mandos (SDL3) para RM3E01

Petición: issue #3 del repositorio público (soporte de mando al estilo PrimeHack). Diseño e implementación: 30/10/2026. Estado: implementado y compilado; QA en el juego pendiente. Núcleo puro `runtime/include/hle/prime_gamepad.h` (pruebas `mkw_prime_gamepad_tests`, incluida la equivalencia del modelo de teclado sin mando), runtime `runtime/src/hle/input/prime_gamepad_runtime.cpp`, integración en `kpad.cpp`, pestaña F1 > Controls > Gamepad, iconos `PrimeKeyIcons::GeneratePadIcons` y variantes de sustitución en Aurora (`aurora_set_texture_replacement_variant`).

## Qué hace PrimeHack y qué no se adopta

Referencia leída: `shiiion/dolphin` (PrimeHack), `Source/Core/Core/PrimeHack/Mods/FpsControls.cpp`, `ContextSensitiveControls.cpp`, `WiimoteEmu.cpp`. Es GPL: se estudia el diseño, no se copia código.

- Cámara: en Prime 3 lee el stick derecho (o el ratón) como velocidad (`sensibilidad * eje / 60` por fotograma) y **escribe directamente** el yaw en la transformación de CPlayer y el pitch en primera persona, cada fotograma. Durante el lock-on recalcula yaw/pitch desde la matriz de la cámara para evitar un salto al soltar.
- Lazo: parchea código del juego (`grapple_lasso`) y simula la mano con variables propias; opción de pulsar repetidamente para tirar.
- Visores y rayos: botones directos que escriben el visor seleccionado en memoria, además del menú del juego.
- Puzzles de movimiento (`ContextSensitiveControls`): recorre la lista de objetos, detecta cerraduras, escáneres de ADN y palancas por su vtable y escribe su estado o velocidad; bloquea la cámara en el centro (o a 45° en las radios de la nave). Incluye direcciones para `PRIME_3_STANDALONE` NTSC-U, que es nuestro RM3E01, pero solo como referencia.
- El cursor de los menús, la selección de visor y la caída libre (pelea con Ridley) se manejan moviendo el puntero con el stick.
- Valores por defecto: sensibilidades horizontal 45 / vertical 35, invertir ejes, "recentrar pitch" y "controlar la mira con lock-on".

Nuestro port ya resuelve lo mismo por otra vía, más conservadora: un Wii Remote + Nunchuk virtual (`prime_keyboard_mouse.h`) que el juego lee como el original, gestos de movimiento sintetizados según el analizador del propio juego (`prime_motion_model.h`, lazo y salto con resorte comprobados) y la cámara mediante post-hooks de CPlayer (`prime_mouse_look.h`), que respetan las condiciones del juego (morfosfera, lock-on, interacciones, cinemáticas, sugerencias de control). Por eso el mando **no escribe memoria del juego ni parchea código**: alimenta esa misma capa. Lo que sí tomamos de PrimeHack:

- stick derecho como velocidad con curva y sensibilidad propias (no como posición);
- el puntero se mueve con el stick allí donde el juego muestra cursor;
- invertir X/Y, y sensibilidades separadas para la cámara y el cursor;
- un modo para los puzzles de giro y empuje: en nuestro caso un modificador mantenido (`pad_motion`) que convierte el stick derecho en giro/empuje del mando virtual.

## Arquitectura

1. **Lectura**: Aurora ya inicializa `SDL_INIT_GAMEPAD`, abre los mandos al conectarse y los expone por puerto (`PADGetSDLGamepadForIndex`). Se lee el mando del puerto 0 en cada lectura KPAD del juego (`PrimeInputTryReadKpad`, el mismo punto que el teclado, sin retraso de cola de eventos).
2. **Botones**: cada acción Wii de `kPrimeActions` (las mismas del teclado) acepta hasta dos entradas del mando en `[prime_pad]` de Config.toml, con nombres posicionales de SDL3 (`south`, `east`, `left_trigger`...), válidos en Xbox, PlayStation y Nintendo. Se combinan con OR con las del teclado: el teclado no cambia en nada y ambos funcionan a la vez.
3. **Stick izquierdo**: es el stick del Nunchuk en analógico, con zona muerta radial reescalada. Si el teclado también mueve, gana la mayor magnitud.
4. **Stick derecho**:
   - con mouse look elegible (primera persona, sin lock-on ni interacción ni cinemática), velocidad de giro con curva (2 por defecto) inyectada en la misma ruta que el ratón (`PrimeMouseLook`), así que valen las mismas puertas y hooks;
   - en el resto de casos (menús, mantener − para el visor, lock-on, escáner), mueve el puntero IR virtual, que se queda donde se dejó.
5. **Movimiento**: el lazo, el salto con resorte y la sacudida reutilizan los gestos del teclado (los mismos perfiles probados). `pad_motion` (RB por defecto): mientras se mantiene, el stick derecho gira el mando virtual (X, hasta 90°, analógico) y lo empuja o tira (Y).
6. **Vibración** (opcional): reenviar el motor del Wii Remote (WPADControlMotor) a `SDL_RumbleGamepad`.

### Mapeo por defecto (posicional)

| Wii | Mando (Xbox / PlayStation / Switch) |
|---|---|
| A (disparar) | RT / R2 / ZR |
| B (saltar) | A / Cruz / B |
| Z (fijar) | LT / L2 / ZL |
| C (morfosfera) | B / Círculo / A |
| − (visor, mantener) | LB / L1 / L |
| + (pausa; mantener: Hipermodo) | Menu / Options / + |
| Cruceta abajo (misil; mantener: misiles buscadores) | X / Cuadrado / Y, y la cruceta |
| Cruceta | Cruceta |
| Lazo (gesto del Nunchuk) | Y / Triángulo / X |
| 1 | View / Create / − |
| 2 | Share / micrófono / Capture, o el touchpad |
| Salto con resorte | L3 |
| Sacudir | R3 |
| Modo movimiento (giro/empuje) | RB / R1 / R |
| HOME | sin asignar (el botón Guía lo capturan Steam y Game Bar) |

Las opciones del juego para intercambiar Disparar/Saltar y Visor/Hiper siguen funcionando: el juego cambia el comando, y los iconos siguen la textura de cada botón.

## Iconos de los botones en pantalla

El juego dibuja los botones como texturas: iconos del HUD y etiquetas `&image=0x<id>;` dentro de sus textos. El modo teclado ya las sustituye con PNG generados al arrancar (`prime_key_icons.cpp`, tabla de 36 texturas indexada por acción Wii). Para el mando se aprovecha esa misma tabla: cada textura muestra el símbolo de la entrada del mando asignada a esa acción.

- **Estilo**: se elige con `SDL_GetGamepadType`: Xbox (letras de colores), PlayStation (cruz, círculo, cuadrado y triángulo dibujados como formas), Nintendo (letras posicionales: el botón sur es B) o GameCube; si no se reconoce, un estilo genérico. Los símbolos se dibujan con GDI+, igual que las teclas: no se incluyen logotipos ni gráficos oficiales (evita problemas de marcas y licencias).
- **Cuándo cambian**: los iconos siguen al último dispositivo usado, como en los juegos de PC (`ActiveDevice`). Un botón o un movimiento claro del stick pasan al mando; una tecla, un clic o un movimiento amplio del ratón vuelven al teclado; la deriva del stick no cuenta.
- **Cambio en vivo**: Aurora indexa las sustituciones solo al arrancar y sus cachés guardan las texturas ya sustituidas. Hace falta una API nueva en Aurora para elegir la subcarpeta activa (`keyboard_icons`, `pad_xbox`, `pad_playstation`, ...) y descartar de las cachés solo las texturas de esa tabla (son 36 hashes, no una purga general que provocaría tirones). Todos los conjuntos se generan al arrancar y se regeneran al cambiar una asignación.

## F1 > Controles

Una pestaña «Mando» junto a la del teclado, con la misma captura («pulsa un botón») y la misma regla de que una entrada mueve una sola acción. También incluye la sensibilidad de cámara y de cursor, la curva, las zonas muertas, invertir X/Y y la vibración, además del nombre y el tipo del mando detectado. La captura ignora los sticks en reposo y acepta direcciones de stick y gatillos como botones.

## Riesgos y comprobaciones

- El modo virtual (`PrimeKeyboardMouseMode`) es la puerta de todo: con un Wii Remote real conectado por Bluetooth el mando no interviene.
- La latencia se mide antes y después con `[prime-input-latency]` y `[present-latency]`: el usuario es sensible al retraso de entrada.
- El estilo de los mandos GameCube y de los genéricos se revisa con mandos reales: SDL rotula los botones de forma distinta según el adaptador.
- Pendiente de QA en el juego: cerraduras, palancas, radios de la nave, la caída libre contra Ridley y el selector de visor con el stick.

## Implementación (30/10/2026)

- Opciones de `[prime]`: `gamepad` (por defecto, activado), `pad_icons` (activado), `pad_look_speed` (220 °/s), `pad_look_curve` (2), `pad_pointer_speed` (1,6 pantallas/s), `pad_deadzone` (0,15) y `pad_invert_y`. Las asignaciones van en `[prime_pad]`.
- Se lee el mando que se usó por última vez. Los Wii Remote reales que SDL expone por HIDAPI no cuentan como mando.
- La pestaña de F1 omite las filas `stick_*` (en el mando, el stick izquierdo ya es el stick) y la de mouse look.
- Iconos: `texture_replacements/variants/pad_<estilo>/`, dibujados en segundo plano la primera vez que se usa ese estilo y de nuevo tras cambiar una asignación. Aurora consulta la variante activa antes del índice principal; al cambiar descarta solo las sustituciones afectadas y las conversiones originales con el mismo hash (las cachés raw de Prime incluidas), sin purga general. Si no hay ninguna sustitución, `find_replacement` sale sin calcular el hash.
- Prompts con dos símbolos (empujar o tirar: botón del modo movimiento más el stick derecho) y diagonales del stick con flecha combinada.
- Pendiente: vibración, la QA en el juego (las cerraduras, el visor, la caída libre y los iconos de cada familia) y medir la latencia.

## Pasos

1. Núcleo puro y pruebas (hecho).
2. Lectura del mando y combinación con el teclado en `kpad.cpp`, con análisis del stick para `PrimeKeyboardMouse` (entrada analógica opcional de stick, giro y profundidad; sin mando el resultado debe ser idéntico al actual, comprobado con prueba).
3. Stick derecho → `PrimeMouseLook` (velocidad) y puntero.
4. `[prime_pad]` en Config.toml y pestaña F1.
5. Iconos por estilo y API de cambio en vivo en Aurora, con pruebas en `render_policy_tests`/`texture_replacement`.
6. Vibración.

## Capa de movimiento (31/10/2026)

Petición del usuario: su 8BitDo Ultimate 2C no deja asignar L4/R4, estrella ni compartir, y un mando con pocos botones no llega a todas las acciones de movimiento.

**Botones extra.** En los registros, el mando aparece como `XInput Controller #1` (tipo 3). El protocolo XInput solo transmite los botones de un mando de Xbox 360. Si L4/R4 no tienen asignación, no envían nada. Si la tienen (asignación por hardware del propio mando), envían el botón estándar asignado: el juego ve ese botón y no puede distinguirlo como uno nuevo. La estrella es el botón de asignación y turbo del propio mando. El controlador HIDAPI de 8BitDo de SDL 3.4.4 (`SDL_hidapi_8bitdo.c`) expone L4/R4/PL/PR en SF30 Pro, SN30 Pro, Pro 2, Pro 3 y Ultimate 2 Wireless. El Ultimate 2C no está en su lista. Nuestra tabla ya acepta las paletas de SDL (`left_paddle1`...) y `misc1` cuando un mando las envía.

**Capa.** El botón del modo movimiento (`pad_motion`, RB por defecto) hace también de mayúscula. Cada acción tiene, además de sus dos entradas, una tercera en `[prime_pad_motion]` (`lasso_pull = ["north"]`) que la activa mientras ese botón se mantiene. Las entradas sin combinación siguen con su acción normal, así que se puede mover, disparar y saltar durante el giro. La ruta de cada entrada se decide al pulsarla y se mantiene hasta soltarla (`PrimeGamepad::LayerRouting`, con pruebas). Pulsar RB con la cruceta abajo ya mantenida no convierte los misiles en inclinación, y soltar RB primero no corta una inclinación en curso. La capa solo acepta botones y gatillos que no sean el propio botón de movimiento. Las direcciones de los sticks no entran: con la mayúscula, el stick izquierdo sigue moviendo y el derecho gira y empuja. Al asignar un botón nuevo a `pad_motion`, se borra de la capa. En F1 > Controles > Mando, la tercera columna se titula «With RB held»: la captura ignora la mayúscula y los sticks. Si una acción del HUD solo tiene combinación, su icono muestra los dos símbolos.

Combinaciones por defecto (con RB mantenido):

| Combinación | Acción |
|---|---|
| RB + Y | Tirar del lazo |
| RB + cruceta arriba / abajo | Apuntar el mando arriba / abajo |
| RB + cruceta izquierda / derecha | Girar el Nunchuk |
| RB + X / B | Inclinar el Nunchuk arriba / abajo |
| RB + LB | Movimiento lento (giro y empuje) |
| RB + View / Create / − | Mouse look sí/no (sin él, el stick derecho mueve el puntero también en primera persona, como en Wii) |

Con un 8BitDo, se puede asignar por hardware L4 o R4 a RB para tener la mayúscula detrás del mando.

La fila `mouse_look_toggle` ya no se oculta en la pestaña del mando: antes solo se podía asignar editando Config.toml. Mientras la pestaña espera una pulsación, el cambio de mouse look no se activa.

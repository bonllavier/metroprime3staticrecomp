# Entrada experimental de teclado y raton para RM3E01_00

Guia de usuario actual: [controles y arranque](../../docs/controles_y_arranque_pc.md). El resto de este documento conserva evidencia historica del arranque inicial; no representa el estado actual del titulo y audio. E/Q ya proporcionan movimiento longitudinal manual, pendiente de QA de la palanca.


El puente recibe eventos SDL de la ventana y publica una muestra PPC32 de 132
bytes al recorrido original de entrada. El teclado se presenta como un mando
virtual con Nunchuk. No se ha verificado un Wiimote fisico ni un adaptador
Bluetooth. El modo se activa expresamente; el ejecutable directo conserva el
recorrido original sin mando hasta configurarlo.

Ejecutar desde el repositorio:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/run_prime_keyboard_mouse.ps1
```

Asignaciones iniciales:

| PC | Entrada del juego |
| --- | --- |
| Enter o boton izquierdo del raton | A |
| Espacio o boton derecho del raton | B |
| WASD | Stick del Nunchuk |
| Shift | Z del Nunchuk |
| Control | C del Nunchuk |
| Flechas | Cruceta |
| Escape | + |
| - | - |
| 1 / 2 / H | 1 / 2 / HOME |
| Posicion del raton dentro de la ventana | Apuntado absoluto experimental |

Perder el foco libera las teclas y el apuntado. Abrir una interfaz del host
que bloquee la entrada deja la muestra neutral. El movimiento diagonal del
stick se normaliza. Los botones con dos asignaciones conservan el estado si
queda pulsada cualquiera de las dos. Una muestra por retrace limita duplicados.

La prueba automatizada usa mensajes Windows dirigidos exclusivamente a la
ventana del PID iniciado, comprueba su propietario y foco, mueve el cursor de
forma temporal y restaura el cursor y el foco anterior en finally. Los eventos
pasan por SDL y el mismo proveedor. Las capturas de memoria verifican A,
pulsacion/liberacion, W/stick y apuntado, ademas del formato, gravedad y errores.
Esto prueba la ruta de eventos sinteticos, no latencia ni mandos fisicos.

Los callbacks originales siguen registrados mediante WPADSetConnectCallback
y WPADSetExtensionCallback. Para el dispositivo virtual se notifican desde un
punto con interrupciones habilitadas, en contexto de registros aislado. Las
ranuras +2212/+2208 del control se contrastaron con el DOL y sus traducciones.
Los dos overlays optativos son 0x804F66B8 (consulta de mando) y 0x804D9138
(lector de muestras procesadas). Sin este modo retornan al cuerpo original.
No se cambian los flags del juego para saltar la pantalla de desconexion.

Estado: el juego acepta los callbacks y consume muestras; desaparece el aviso
de desconexion y abre Video/FrontEnd/attract01.thp. La imagen posterior queda
negra y el titulo sigue sin verificar. Pendientes: presentacion/avance de THP,
movimientos y giros requeridos, parametros completos de apuntado, callbacks y
reconexion generales, configuracion persistente de controles y Wiimote fisico.
No declarar el juego jugable por esta prueba.

Evidencia: checkpoints 20261021_prime_keyboard_mouse_windows_input.json,
20261021_prime_keyboard_mouse_negative_checks.json y
20261021_prime_keyboard_mouse_boot.json. Todos viven en ../checkpoints/.

## Textos de gestos con la tecla o el botón asignado (11 de noviembre de 2026)

En los modos de teclado/ratón y de mando, las indicaciones que piden mover el Wii Remote o el Nunchuk nombran la entrada asignada. Por ejemplo, "Push the Wii Remote toward the screen to activate thrusters." pasa a "Press E to activate thrusters." o a "RB + R stick up" con mando. Se cubren 16 indicaciones en sus cinco idiomas (80 cadenas de RM3E01, 106 con las variantes de Trilogy):
- empujar, tirar y propulsores;
- girar a cada lado y girar sin más;
- inclinar arriba y abajo;
- apuntar;
- Rotosalto de la Morfosfera;
- lanzar y tirar del lazo;
- punto de enganche y toma de energía;
- sacudir el Nunchuk;
- la bomba de palanca (Metroid3.pak): "Pull back then push forward to pump." pasa a "Mantén {pull} para tirar hacia ti y luego {push} para empujar hacia delante. Repite para bombear.", y su "Point the Wii Remote at the screen.", que solo aparece en esa bomba y se muestra en el paso de tirar, a "Apunta a la pantalla con el ratón y mantén {pull} para tirar de la palanca hacia ti." (26 de noviembre de 2026; antes decía solo "Apunta con el ratón.", que no explicaba qué hacer, y el de bombear no se cambiaba).

Con el Wii Remote real (modo teclado/ratón desactivado) no se cambia nada.

- **Dónde.**
  - `CStringTable::GetString` (0x803CDB38) pasa el texto UTF-8 de la tabla a la vista prestada de 0x803FC164 y luego lo convierte a cadena ancha (0x803CC6A8).
  - El hook de `prepare_prime_runtime_hooks.py` llama a `PrimeMotionTextOverride` (`runtime/src/hle/prime_motion_text.cpp`) justo antes de construir la vista y le cambia el puntero (r4).
  - El juego convierte y libera su copia como siempre, sin límites de longitud ni recuentos de referencias que tocar.
  - Las tablas de 16 bits (la otra rama) no se tocan.
- **Cómo reconoce las indicaciones.**
  - Por el FNV-1a 64 del texto exacto, en `prime_motion_text_hashes.inc`, generado del disco con `tools/prime_icons/motion_text_hashes.py`.
  - El código no contiene texto del juego; solo los reemplazos, escritos para el port.
  - Se conservan el `&just=center;` inicial y el salto de línea final. Las etiquetas `&image=` de los iconos de fijar blanco y enganche van dentro del reemplazo.
- **Qué entrada nombra.**
  - En teclado, la tecla o el botón de ratón principal de la acción (`KeyLabel`, la misma etiqueta que los iconos). Las flechas se escriben Up/Down/Left/Right porque la fuente del juego no tiene esos glifos.
  - Con mando, la del dispositivo usado por última vez (`PrimeGamepadRuntime::CurrentDevice`):
    - empujar, tirar y girar: botón de movimiento + dirección del stick derecho;
    - resto de gestos: su botón, o la combinación de la capa de movimiento.
  - Si una acción no tiene asignación se deja el texto original.
  - Un cambio de asignación se ve la próxima vez que el juego pide el texto.
- **Memoria.**
  - Bloque de 16 KiB reservado bajo la FST del DVD (`system_bridge.cpp`, perfil RM3E01): el MEM2 arena hi baja 16 KiB.
  - La vista solo se lee durante `GetString`, así que el bloque se usa como anillo: cada copia ocupa su tamaño redondeado a 32 bytes (hasta 8 KiB, el límite de lectura).
  - El mismo hook aplica las correcciones de los textos traducidos en cualquier modo de entrada (ver [textos_traducciones.md](textos_traducciones.md)).
- **Pruebas.**
  - En builds de QA, cada reemplazo deja `[prime-motion-text] prompt=… language=… device=… text=…` en el log.
  - Las cadenas que parecen de gesto pero no están en la tabla dejan `unknown prompt fnv=…`. "Nunchuk is required." aparece así a propósito: no es un gesto.
- **Trilogy (R3ME01).**
  - `make_title_tree.py` traslada el hook por `address_map.json`: 0x803CDB38→0x803CC1C8, 0x803FC164→0x803FABDC.
  - Comprobado: el prólogo `9421ffe0` y el ancla aparecen una vez en la traducción de Trilogy.
  - Si Trilogy redacta alguna indicación de otra forma, sale como `unknown prompt` en un build de QA. Para añadirla, regenerar la tabla con las dos extracciones: `MOTION_TEXT_ROOTS="<RM3E01 files>;<Trilogy files/MP3>" python tools/prime_icons/motion_text_hashes.py` (separador `;` en Windows y `:` en Linux).

## Batería del Wiimote siempre llena (25 de octubre de 2026)

- **Síntoma.** "Warning: Wii Remote Batteries Are Low" permanente y el indicador del casco vacío.
- **Causa.** El generador de entrada (`FUN_803c2670`) llama a `WPADGetInfoAsync(0, manager+0xB8C, 0x803C4884)` cada pocos segundos (temporizador en +0xBF0). El callback solo copia la `WPADInfo` a +0xB28.. cuando el resultado es 0. Con el mando virtual de teclado/ratón la librería WPAD no tiene dispositivo, la petición falla y el gestor conserva batería 0; el HUD (`FUN_801c50c0`) enciende `battery` (0 a 4) segmentos de `model_battery1..4` y muestra el aviso en los niveles 0 y 1.
- **Solución.** Hook en `WPADGetInfoAsync` (0x804F6A2C, prólogo `9421ff70`) con `PrimeInputTryGetInfoAsync` en `kpad.cpp`: escribe `WPADInfo{dpd=1, speaker=0, attach=según extensión, lowBat=0, nearempty=0, battery=4, led=1<<canal, protocol=1, firmware=0}`, devuelve 0 y ejecuta el callback del juego con (canal, 0) como una petición completada. Aplica al mando virtual y al Bluetooth por igual.
- **Opción.** `[prime] wiimote_battery_full` (por defecto `true`, variable `WII_COMPILED_PRIME_WIIMOTE_BATTERY_FULL`), casilla en F1 > Controles. Se lee en cada llamada, sin reiniciar.
- **Validación.** Ejecución pid 16704: cuatro respuestas del hook en 85 s, sin errores, indicador de batería con cuatro segmentos confirmado por el usuario. Evidencia: `checkpoints/20261025_prime_wiimote_battery_full.json`.


## Alcance vertical de la vista con ratón (26 de noviembre de 2026)

- **Síntoma.** Con la vista con ratón activada no se podía mirar ni apuntar tan arriba ni tan abajo como con el apuntado original del Wiimote.
- **Pitch del juego (0x801522E0, CPlayer +0x780).** En el modo normal es `spline(puntero en [0, 1]) × lbl_80680570 (grados a radianes)`. La spline de subir está en `CTweakPlayerControl +0x398C` (comando 55, 0x802E8760) y la de bajar en +0x39CC (comando 56, 0x802E876C). Con `CTweakPlayer +0x258` (0x802E6BE0) el pitch se acumula y se limita a −30°/+40° (`FLOAT_80682524`/`FLOAT_80682528`). Medido en juego (log del 26/11, leyendo las splines): **55° abajo y 35° arriba**.
- **Causa.** Con el Wiimote, además del giro de la cámara, la mira llega al borde de la pantalla, lo que suma cerca de medio campo de visión vertical al apuntado. Con la vista con ratón la mira queda centrada: el alcance era solo el de la cámara. Antes era incluso menor, porque `PitchStep` aplicaba −30°/+40° (los límites del otro modo).
- **Cámara (0x8007F904).** Convierte +0x77C linealmente en su pitch (`c + (±40° − c) × p / 40°`, c = cámara +0x20C) y lo limita a ±89,5° (`lbl_8067F500`/`lbl_8067F504`). Por eso admite más rango que el de las splines. La suavización de 0x80151F64 solo limita la velocidad.
- **Solución.** La vista con ratón usa ±80° (`kMouseLookPitchLimit`, `prime_mouse_look.h`), con margen antes de los 89,5° de la cámara. Se aplica en `PitchStep`, al seguir el pitch del juego con la vista desactivada y en `SetPitch` de QA. Un primer intento que copiaba el rango de las splines dejaba la vista igual de corta y se descartó.
- **Validación.** Pasa `mkw_prime_mouse_look_tests`. Probado en juego por el usuario el 26/11/2026 con el build de Linux: el alcance vertical ya no se queda corto respecto al Wiimote. Cambio aplicado también en la copia de Windows (sin compilar allí).

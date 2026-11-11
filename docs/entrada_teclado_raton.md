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

## Batería del Wiimote siempre llena (25 de octubre de 2026)

- **Síntoma.** "Warning: Wii Remote Batteries Are Low" permanente y el indicador del casco vacío.
- **Causa.** El generador de entrada (`FUN_803c2670`) llama a `WPADGetInfoAsync(0, manager+0xB8C, 0x803C4884)` cada pocos segundos (temporizador en +0xBF0). El callback solo copia la `WPADInfo` a +0xB28.. cuando el resultado es 0. Con el mando virtual de teclado/ratón la librería WPAD no tiene dispositivo, la petición falla y el gestor conserva batería 0; el HUD (`FUN_801c50c0`) enciende `battery` (0 a 4) segmentos de `model_battery1..4` y muestra el aviso en los niveles 0 y 1.
- **Solución.** Hook en `WPADGetInfoAsync` (0x804F6A2C, prólogo `9421ff70`) con `PrimeInputTryGetInfoAsync` en `kpad.cpp`: escribe `WPADInfo{dpd=1, speaker=0, attach=según extensión, lowBat=0, nearempty=0, battery=4, led=1<<canal, protocol=1, firmware=0}`, devuelve 0 y ejecuta el callback del juego con (canal, 0) como una petición completada. Aplica al mando virtual y al Bluetooth por igual.
- **Opción.** `[prime] wiimote_battery_full` (por defecto `true`, variable `WII_COMPILED_PRIME_WIIMOTE_BATTERY_FULL`), casilla en F1 > Controles. Se lee en cada llamada, sin reiniciar.
- **Validación.** Ejecución pid 16704: cuatro respuestas del hook en 85 s, sin errores, indicador de batería con cuatro segmentos confirmado por el usuario. Evidencia: `checkpoints/20261025_prime_wiimote_battery_full.json`.


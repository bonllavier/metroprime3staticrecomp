# QA privada: grabación y reproducción de sesiones

Carpeta privada. Nada de aquí entra en `publish/` ni en el zip: `package_release.ps1` rechaza un ejecutable compilado con las herramientas de QA, y las grabaciones contienen tu partida.

## Qué hace

- **Grabar:** juegas con teclado y ratón como siempre y el runtime anota, fotograma a fotograma, lo que recibe el mando virtual (botones, stick, puntero, gestos) y el movimiento del ratón de la vista con ratón. Al empezar una grabación se copian tu partida y tu `Config.toml`, para que una reproducción parta del mismo punto.
- **Reproducir:** el juego arranca en un perfil aparte (`qa/profile`) con la partida y los ajustes de la grabación, comparte las cachés de shaders con tu perfil, y recibe las entradas grabadas en lugar del teclado. Al terminar se cierra solo y deja un informe: FPS por ventanas de 5 s, peor fotograma, compilaciones de shaders, errores y capturas periódicas.

## Compilación

El build local debe configurarse con `-DMKW_PRIME_QA_TOOLS=ON` (ya está así en `recomp/build/RM3E01_00_runtime_link_probe`). Para empaquetar una versión pública hay que compilar con `OFF`; el empaquetador lo comprueba.

## Grabar (el juego sigue corriendo entre grabaciones)

```powershell
# 1. Arranca el juego preparado para grabar y espera al título
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_record.ps1 -Launch

# 2. En el título (o en los menús), empieza a grabar
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_record.ps1 -Name hangar -Notes "del hangar a las torretas"

# 3. Juega el recorrido. Para parar la grabación sin cerrar el juego:
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_record.ps1 -Stop

# 4. Puedes volver al título y empezar otra grabación con otro -Name; o cerrar el juego.
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_record.ps1 -Status
```

Reglas para que la reproducción sea fiel:

- Una grabación solo puede empezar en el título o en los menús. Si estás dentro del juego el comando lo rechaza, porque la reproducción arranca desde la partida guardada en disco y debe pasar por los menús para cargarla igual que tú.
- Empieza a grabar poco después de ver el título y no toques los menús antes de dar la orden.
- Guarda la partida en el punto desde el que quieras que empiecen los recorridos antes de grabar.
- Tras cada puerta o carga, quédate quieto un segundo. Evita acciones que dependan de un enemigo concreto si el objetivo es medir rendimiento.

## Reproducir

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_replay.ps1 -Name hangar -Label base
powershell -NoProfile -ExecutionPolicy Bypass -File recomp\qa\qa_replay.ps1 -Name hangar -Label uber -Env "WII_COMPILED_UBERSHADER=1"
```

El juego no debe estar abierto (comparten la caché de shaders). Durante la reproducción no uses el teclado: el ratón se ignora, pero el teclado sigue llegando. La ventana del juego se fija "siempre encima" mientras dura la reproducción, para que las capturas la vean aunque el foco esté en otra aplicación. Resultados en `qa/runs/<nombre>/<fecha>_<etiqueta>/`: `report.json`, `console.log` y capturas `shot_NNNNs.png`.

## Enemigos y desvíos: anclas de posición

Cada medio segundo la grabación guarda la posición de Samus en el mundo (línea `a`). En la reproducción, en cada ancla se compara la posición real con la grabada: si se desvía más de medio metro (un enemigo empujó, un salto salió distinto), se anota `drift at frame N` y Samus se recoloca en la posición grabada. Además la vida se mantiene al máximo durante toda la reproducción, para que ningún ataque corte el recorrido. El informe resume `anchors_checked`, `drift_events`, `max_drift_m` y `corrections`.

Para desactivarlo en una pasada: `-Env "WII_COMPILED_QA_NO_CORRECT=1"` (solo detectar) o `"WII_COMPILED_QA_NO_GOD=1"` (vida normal).

Campos del juego usados (RM3E01, comprobados en vivo el 26 de octubre): posición en `CPlayer+0x48/+0x58/+0x68`, estado del jugador en `CPlayer+0x35A0`, vida en `CPlayerState+0xC` y capacidad en `+0x8`. El puntero a `CPlayer` lo aportan los hooks de la vista con ratón, así que las anclas existen desde el primer fotograma en primera persona.

## Límites

- Las entradas se aplican por fotograma de juego, contado desde que el título está activo. Si una carga tarda más o menos que al grabar, el juego puede desfasarse unos fotogramas. Las anclas corrigen la posición, no el momento: un desfase grande en una puerta puede dejar una pulsación fuera de sitio.
- La hora del juego sigue siendo la real. Si hiciera falta determinismo total habría que virtualizar el reloj; no está hecho.
- `meta.json` guarda el hash del ejecutable con el que se grabó. Un cambio que altere la lógica de entrada puede invalidar grabaciones antiguas.

## Formato y variables (solo build de QA)

Formato de texto, probado en `mkw_prime_input_recording_tests`: `f <fotograma> ...` fija la entrada desde ese fotograma, `m <fotograma> dx,dy` es el movimiento del ratón de ese fotograma, `end <fotograma>` cierra.

| Variable | Efecto |
|---|---|
| `WII_COMPILED_QA_CONTROL_DIR=<carpeta>` | Acepta `start.txt` (ruta de salida) y `stop.txt`; escribe `status.txt` |
| `WII_COMPILED_QA_RECORD=<archivo>` | Graba desde el primer fotograma del título |
| `WII_COMPILED_QA_REPLAY=<archivo>` | Reproduce desde el primer fotograma del título |
| `WII_COMPILED_QA_REPLAY_EXIT=1` | Cierra el juego al acabar la reproducción |
| `WII_COMPILED_QA_USER_DATA=<carpeta>` | Perfil de datos alternativo (partida, ajustes, logs, cachés) |

## Autocomprobación

`qa_selfcheck.ps1` graba un recorrido sintético (título, partida, unos pasos y un giro de ratón), lo reproduce y compara el último fotograma de ambos. Resultado del 26 de octubre: 2,6 % de píxeles distintos, solo elementos animados; 60 FPS de media en la reproducción. Sirve para comprobar que grabación y reproducción siguen alineadas tras cambios en la entrada.

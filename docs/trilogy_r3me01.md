# Metroid Prime 3 desde Metroid Prime Trilogy (R3ME01)

Petición del usuario (03/11/2026): ejecutar el Metroid Prime 3 que trae *Metroid Prime Trilogy* USA, además de RM3E01. Solo interesa ese juego, no MP1 ni MP2. Sin RetroAchievements en esta versión, decisión del usuario.

Estado (04/11/2026): **arranca en la build local de pruebas, sin publicar.** `Corruption_3 PC.exe` con el RVZ de Trilogy abre el menú, el menú lanza MP3 con su bloque de opciones, y MP3 pasa la cinemática inicial hasta la cabina de la nave, a 60 FPS. Aún no se ha jugado: ver [Pendiente](#pendiente).

## Qué se ha probado (perfil privado `titles/probe_profile`, entradas por guion)

- Entrada única: `[prime-trilogy] started the Trilogy menu`, y el menú abre el mismo RVZ.
- Menú: correa, intro, título, Game Select, Main Menu, Single Player Games y Select Difficulty, a 60 FPS. Recursos de `/fe/`, RSO enlazados.
- Salto: `[trilogy-exec] OSExec /rs5mp3_p.dol`, y en MP3 `saved region 0x81173820 size 0x8c7e0`.
- MP3: `/MP3/RS5.PAK`, 11 RSO enlazados, cinemática inicial y cabina de la nave con HUD, casi 4 min sin errores y sin entradas.
- Elegir MP1 o MP2 muestra el aviso y vuelve al menú (MP2 observado; MP1 solo hasta `OSExec`).
- RM3E01 tras estos cambios: recorrido `hangar_1x_window` completo, 0 errores, media 59,87 FPS, mínimo 57,6.

Tres problemas que hubo que resolver y que no conviene repetir:

- **Espera de WPAD.** El `WPADInit` de Trilogy ya no espera; el bucle `WPADGetStatus() != 3` está en el llamador (`WPAD_WAITS`) y atiende interrupciones en cada vuelta, como el overlay de RM3E01. No se escribe el estado.
- **`OSGetSavedRegion` va inlineada.** El traductor la integra en su único llamador, así que un overlay en su entrada no se ejecuta nunca. La restauración va en la entrada del llamador (`EXEC_HOOKS`), que escribe las dos palabras que lee la hoja.
- **Fin de AX.** El correo `0xCDD10002` (fin de tarea tras `AXQuit`, antes de `OSExec`) devuelve el DSP a su ROM en `PrimeDspSendMail`; antes era un error fatal.

Para repetir la prueba: `tools/trilogy/probe_title.ps1 -Script tools/trilogy/menu_to_mp3.inputs.txt` (generado por `make_menu_inputs.py`). El menú solo actualiza el hover con un puntero que se mueve, y en 16:9 la X del puntero se escala por 1,28. Las pruebas se quedan sin entradas dentro de MP3: su `ScriptGUIScreenFrontEndUI` no enlaza, así que el reloj de la repetición (`PrimeFrontEndActive`) nunca arranca.

## El disco (comprobado con `nod` sobre el RVZ del usuario)

- ID `R3ME01`, disco 1, revisión 0, doble capa (8.511.160.320 bytes). La imagen la lee `nod`; no se extrae para jugar.
- El disco trae cuatro DOL: `rs5fe_p.dol` (el menú de Trilogy, idéntico a `sys/main.dol`, SHA-1 `d55b890c…`), `rs5mp1_p.dol`, `rs5mp2_p.dol` y `rs5mp3_p.dol` (SHA-1 `e86908b9…`).
- Carpetas por juego: `fe/`, `MP1/`, `MP2/`, `MP3/`. `MP3/` tiene 74 RSO (`MP3/RSO/Production/`, no `RSO/Rev2 Production/`) y `fe/` tiene 5.
- `MP3/` no trae `FrontEnd.pak`, `precache.RMC` ni los vídeos de menú; trae `RS5.PAK` a cambio. Los menús viven en `fe/`.

## El ejecutable no es el de RM3E01

El port es una recompilación estática, así que hay que traducir este DOL. Comparación contra el `main.dol` de RM3E01:

| Medida | Resultado |
| --- | --- |
| Compilación | `2008/08/21 17:09 Build v3.593 (RS5)` |
| Texto | 0x56E7E0 frente a 0x56FF40 bytes; las funciones no conservan su dirección |
| SDA / SDA2 | `0x80672260` / `0x80678B00` (RM3E01: `0x806801C0` / `0x806869C0`) |
| Funciones de RM3E01 con el mismo código, normalizando reubicaciones | 18.442 de 21.667 |
| Sin coincidencia exacta | 3.225 (1,9 MB), pero 2.311 (1,56 MB) con similitud ≥ 95 % |
| Realmente distintas (similitud < 50 %) | 162 (76 KB): Mii, WiiConnect24, menús, Wii remote |

Los scripts de esa medición se pueden rehacer normalizando `b`/`bc`, `addi`/`addis`/`ori` y desplazamientos de carga y guardado, que son los campos que cambian al reubicar. La similitud se mide con la ventana entre las dos funciones vecinas que sí coinciden.

## Cómo pasa el control el menú (comprobado en el código)

En `rs5fe_p.dol`, `0x8026536C` recibe el nombre del DOL y un indicador. Serializa las opciones en un búfer de `0x7D0` bytes, muestra `Metroid Prime 3: Corruption Loading...`, copia el búfer a `(0x81200000 - tamaño del framebuffer) - 0x7E0`, llama a `OSSetSaveRegion` y a `OSExec` (`0x80490890`).

En `rs5mp3_p.dol`:

- `main` (`0x802D39DC`) llama a `0x802D3958`, que lee `OSGetSaveRegion`, la vacía, reserva `0x7D0` bytes y copia el bloque si existe.
- Un bloque nulo está contemplado: `r13-0x26BC` se queda a cero y `main` calcula sus indicadores sin él.
- `0x802AAFDC` vuelve a `/rs5fe_p.dol` con el mismo formato de bloque en `0x81200000 - 0x7E0`.

Las partidas: solo `rs5fe_p.dol` contiene `banner.bin`, `RS5SaveManagerTextures_DGRP` y `TXTR_RS5Save*`. `rs5mp3_p.dol` conoce `save.bin`.

**Decisión:** traducir también `rs5fe_p.dol` en vez de imitar el bloque. Es el menú real, con las ranuras, la creación de la partida y los créditos compartidos.

## Diseño

- **Un ejecutable por DOL.** Los dos DOL usan las mismas direcciones y los mismos nombres de función traducida, y no caben en un proceso. `OSExec` se resuelve como `OSRestart` (`prime_host_restart.cpp`): lanzar el otro ejecutable, esperar a que este salga y conservar el bloque en un archivo.
- **RM3E01 no se toca.** Sus fuentes del runtime, su build y sus pruebas siguen igual. Lo específico de Trilogy se genera aparte.
- **Direcciones por mapa verificado.** Cada dirección que usa el port se traduce a la de cada DOL de Trilogy con un mapa generado, y se valida contra los bytes originales. Una dirección que no se pueda demostrar es un error, no se adivina. Los offsets de campos cambiados se revisan a mano.
- **MP1 y MP2** no se traducen. El menú los seguirá mostrando; falta decidir qué hace al elegirlos.
- **RetroAchievements** queda apagado en este perfil.

## Cadena construida (`tools/trilogy/`)

Todo lo derivado queda en `titles/` (ignorado por Git). Para rehacer un título:

```
python -B tools/trilogy/make_title_inputs.py
python -B tools/trilogy/make_address_map.py R3ME01_MP3
python -B tools/trilogy/make_title_tree.py R3ME01_MP3
python -B tools/trilogy/audit_native_collisions.py R3ME01_MP3
python -B tools/trilogy/build_title.py R3ME01_MP3
```

Antes: `titles/<título>/extract` con `disc_file_tool.cpp` (DOL, `fst.bin`, RSO) y `titles/<título>/dtk/config/symbols.txt` con `dtk dol split`.

- **Mapa de direcciones:** empareja por código normalizado, por posición, por similitud en la ventana y por llamadas alineadas (esta última resolvió `CMain`, donde cambió el orden de funciones). MP3: 21.408 de 21.667 funciones; 484 de 522 literales.
- **Política de literales:** muchas direcciones del runtime son de Mario Kart y no se tocan. Solo se reescriben los archivos `prime_*`, los datos y las entradas o interiores permitidos a partir de `0x803BE000`.
- **Colisiones:** un override de Mario Kart que en RM3E01 cae dentro de otra función puede ser una entrada real aquí. `audit_native_collisions.py` las lista y `make_title_tree.py` las neutraliza (12 en MP3, 6 en el menú; entre ellas `0x80006348`, el `__init_hardware` real de Trilogy).
- **Hooks:** cada instrucción esperada se compara normalizada. Solo no coincidieron dos, en el bucle de `WPADInit`, que ya no existe en este SDK; ese hook se elimina.
- **RSO:** las raíces revisadas a mano eran un subconjunto de las descubiertas, así que aquí solo se usan las descubiertas. El RSO `ScriptGUIScreenFrontEndUI` del MP3 no enlaza contra su `selfile.sel` (`rso_unlinkable.json`), señal de que el menú real no vive en esa carpeta.
- **Puente de ejecutables:** `tools/trilogy/runtime_overlay/prime_trilogy_exec.cpp`, con hooks en `OSExec` y `OSGetSavedRegion`.

Resultados de traducción: 21.704 y 20.409 funciones, cero instrucciones no soportadas y cero SSA inválidas; 205 y 203 switches auditados sin casos perdidos.

## RVZ en RM3E01 (hecho en la build local, sin publicar)

El ejecutable anunciaba `.rvz` pero `libnod.a` se compilaba sin compresores y abrir fallaba con `Unsupported WIA/RVZ compression: Zstandard`. `tools/build_nod_ffi.ps1` lo compila con zstd, zlib, bzip2 y lzma, y `build_prime_runtime.ps1` rechaza un archivo sin ellos. RetroAchievements hashea ahora con `nod` en vez del archivo crudo: el `rcheevos` real da `19a1c0da...` en la ISO y en un RVZ de prueba creado a partir de ella; falta pasarlo por el RVZ del usuario y probar el inicio de sesión dentro del juego. La reproducción del recorrido `hangar` desde el RVZ termina sin errores. El tiempo de lectura es igual (35 ms frente a 36 ms en total). Los tirones no están bien medidos, porque esa pasada coincidió con análisis en paralelo: falta repetirla con la máquina libre.

## Vista con ratón en Trilogy (04/11/2026)

El usuario vio el apuntado mal en su partida, y la causa era `CPlayer`: el port escribía el cabeceo en `+0x780`, que en Trilogy es el objetivo de la vista libre horizontal (el `+0x778` de RM3E01), y el filtro leía las banderas de `+0x34EC`, que ahí valen `ffffffff`. Por eso capturaba el ratón también en la nave, donde el puntero debe mover el brazo.

`MOVED_FIELDS` en `make_title_tree.py` reescribe estos campos en la copia generada. Cada uno se comprueba con la misma instrucción en los dos DOL, y si deja de coincidir la generación se detiene:

| Campo | RM3E01 | Trilogy |
| --- | --- | --- |
| Subestado de órbita | `+0x730` | `+0x738` |
| Cabeceo objetivo / suavizado | `+0x780` / `+0x77C` | `+0x788` / `+0x784` |
| Velocidad del cabeceo | `+0x3C1C` | `+0x3C2C` |
| Anulación del cabeceo | `+0x3B40` | `+0x3B54` |
| Modo de apuntado / rampa de giro | `+0x3C18` / `+0x3C20` | `+0x3C28` / `+0x3C30` |
| Bloqueo de giro | `+0x3AD4` | `+0x3AE8` |
| Banderas de control | `+0x34EC` | `+0x34F4` |
| `CControlMapper` | `+0x35C4` | `+0x35D8` |

La sensibilidad también se movió: `CGameState +0x142C` ya no existe y `0x800AE3AC` la lee del objeto de opciones (`*0x8066F7D8 + 0x24`, o `*0x8066B5E8` si no hay objeto). Las tablas de velocidad por superficie y los comandos 2/3/55/56 no cambian. FOV no necesita cambios: `+0x1010`, `+0x178`, `+0x2E4` y sus funciones son iguales.

Comprobado: leyendo la memoria de la partida abierta del usuario, sin escribir, el único `CPlayer` tiene 100 comandos en `+0x35D8` y un puntero a `CPlayerState` en `+0x35A8`. Sus banderas en `+0x34F4` empiezan por `0x18`, sin el bit 5 de "a pie". En el perfil de pruebas valen `382…`/`380…`, como en RM3E01. **Falta jugarlo con el ratón:** las repeticiones no dan entradas dentro de MP3.

## Una sola ventana a la vista (04/11/2026)

Dos procesos no pueden compartir una ventana y los dos DOL no caben en uno, así que `prime_trilogy_exec.cpp` pone una cortina: el proceso nuevo copia del escritorio lo que muestra la ventana vieja a otra igual, encima de todo y sin tomar el foco. Solo entonces sale el viejo. La cortina se retira tras tres presentaciones de la ventana nueva, o a los 30 s si nunca presenta. Ambos ejecutables comparten AppUserModelID para no mover el botón de la barra de tareas.

Se descartó mantener vivo el proceso viejo hasta el primer fotograma: el nuevo abre las cachés SQLite sin tiempo de espera, y un bloqueo con el viejo aún escribiendo aborta la caché de pipelines. Con la cortina se conserva el orden de siempre.

Si la ventana vieja no está en primer plano no hay cortina, porque la copia recogería lo que tenga delante; se cierra y se abre como antes. Medido en el perfil de pruebas (muestreo cada 10 ms): siempre hay una ventana visible en el mismo rectángulo (cortina 3,2 s), el foco pasa a la nueva y la captura durante la cortina muestra la pantalla "Metroid Prime 3: Corruption Loading..." del menú. Sin probar: pantalla completa, ventana grande y vuelta de MP3 al menú.

## Pantallas de inicio (05/11/2026)

Las dos pantallas de la correa del Wiimote de Trilogy tienen texturas propias (`tex1_832x456_ad15d2f8f67a24d8_14`, "Allow adequate room…", y `tex1_832x456_daf65c9d00a79c85_14`, la del conector), vistas en dos capturas F7. Se sustituyen con las mismas dos imágenes de RM3E01, copiadas con esos nombres en `runtime/assets/texture_replacements/strap_screen/` (01/02 y 02/02, por orden de aparición) y añadidas a `package_release.ps1`. Los ejecutables de Trilogy comparten carpeta y `UserData` con `Corruption_3 PC.exe`, así que no hace falta recompilar. Los hashes de RM3E01 no cambian.

**Otros idiomas (16/11/2026).** El usuario vio las pantallas originales de la correa en Trilogy porque juega en español. Trilogy no reutiliza los recursos de RM3E01: `fe/NoARAM.pak` trae `TXTR_StrapScreenA169`/`B169` en inglés, francés, español, alemán, italiano, neerlandés y japonés, cada una con un hash propio (RM3E01 solo trae inglés, francés y español). Hashes calculados con `tools/prime_icons` (lectura de TXTR, XXH64 del nivel base, formato 14) sobre el pak leído del RVZ con `disc_file_tool.exe file`, que reproducen los dos ya conocidos:

| Recurso | Hash Trilogy (página 01 / 02) |
| --- | --- |
| A169 / B169 (inglés) | `ad15d2f8f67a24d8` / `daf65c9d00a79c85` (copias en el repositorio) |
| A169FREN / B169FREN | `fc7e927ec31f602c` / `3b7f56efd43b2e0d` |
| A169SPAN / B169SPAN | `38e98d1efe9e42e4` / `33f6b552eaade8ea` |
| A169GERM / B169GERM | `42f8505dd5b3a2cb` / `74a8868a7e5ea753` |
| A169ITAL / B169ITAL | `fca50773873d7413` / `bed9eed71eca041e` |
| A169DUTC / B169DUTC | `b63d3922b062f374` / `4a40d88f0e470807` |

Las pantallas del port son dos imágenes (01/02) iguales para todos los idiomas: los ocho PNG del repositorio son dos contenidos repetidos (comprobado con md5).

Para no meter 17 MB de PNG repetidos en el zip, `main.cpp` crea esas copias en `UserData/texture_replacements/strap_screen` al arrancar, a partir de las imágenes que ya se distribuyen (tabla `kTrilogyStrapScreens`). Comprobado el 16/11 arrancando el build local 15 s: `strap_screen` pasa de 8 a 18 archivos. El japonés no se sustituye: el port no ofrece ese idioma.

**Iconos de teclado y mando (16/11/2026).** De las 120 texturas de botones de `config/RM3E01_00/keyboard_icon_textures.json`, 119 están en los paks de Trilogy (`MP3/` y `fe/`) con el mismo identificador y el mismo nombre de reemplazo; la que falta (`6a74653492612ba7`, GuiNAND) no la usa el port. Los iconos valen igual en Trilogy sin cambios.

## Trilogy Europa (R3MP01), análisis del 06/11/2026

El usuario pidió saber si el RVZ europeo (`D:\Metroid Prime Trilogy (Europe) (En,Fr,De,Es,It).rvz`) puede ejecutarse con los mismos ejecutables que Trilogy USA y que, en la medida de lo posible, no haya que generar otro exe. Solo se ha leído el disco con `nod` (`disc_file_tool.exe`); no se ha modificado ni ejecutado nada en el port.

Disco: `R3MP01`, disco 0, revisión 0. Mismos cuatro DOL y mismas carpetas (`fe/`, `MP1/`, `MP2/`, `MP3/`, 74 RSO en `MP3/RSO/Production/`).

**Los DOL son otra compilación.** `rs5mp3_p.dol` SHA-1 `9d2e89dd…` (`2009/01/15 14:08 Build v3.629 (RS5)`, USA `v3.593`); `rs5fe_p.dol` SHA-1 `a0b36f08…`. Texto `0x572A00` (USA `0x56FF40`), SDA/SDA2 `0x80675AA0`/`0x8067C3E0` (menú `0x80627B40`/`0x8062DF60`). Emparejando con `pair_functions` de `make_address_map.py` sobre `dtk dol split` de cada DOL:

| | MP3 | Menú |
| --- | --- | --- |
| Funciones USA iguales salvo reubicación | 21.431 de 21.654 | 20.123 de 20.362 |
| …en la misma dirección | 3.239 | 1.338 |
| Diferentes o sin pareja, juego | 23 (10 KB) | 29 (10 KB) |
| Diferentes o sin pareja, SDK (WPAD/KPAD, OS) | 121 (65 KB) | 127 (67 KB) |

Como el port es una recompilación estática y los datos del DOL van incrustados (`data_sections_init`), el exe USA lleva el código y los datos del DOL USA. **No puede ejecutar el DOL europeo**: casi todas las direcciones se desplazan. Hoy además lo rechaza `dvd.cpp` por el ID.

**Los datos casi coinciden.** De los archivos de `MP3/` y `fe/` con el mismo tamaño en ambos discos, 233 de 233 tienen el mismo SHA-1, incluidos `Metroid1..8.pak`, `Worlds.pak`, `UniverseArea.pak`, `Logbook.PAK`, el audio y 72 RSO. Difieren:

- RSO: `MP3/RSO_SpacePirate.rso`, `rhbm` (en las dos carpetas), `fe/RSO_ScriptGUIScreenFrontEndUI.rso` (el menú) y las dos `selfile.sel`. Las `selfile.sel` solo traen exportaciones: mismos nombres y desplazamientos que concuerdan con el mapa de funciones, más `WPADSetCallbackByKPAD`, que importa el `rhbm` europeo y no existe en el DOL USA.
- `MP3/RS5.PAK`: la europea es un superconjunto (+93 STRG con nombre, 47 TXTR, 2 FONT), y sus 129 CSMP tienen los mismos bytes con otros ID: en la USA solo 22 de 151 nombres apuntan a un recurso que existe.
- `MP3/MiscData.pak` (11 STRG), `fe/GuiDVD.PAK` (3 STRG, 2 FRME), `fe/MiscData.pak` (3 STRG, banner), `fe/RS5FE.PAK` (1 MREA, 1 CAUD, 1 STRG, 2 CSMP) y los THP de `fe/Video`.
- Los STRG siguen en v3 con los mismos seis idiomas (`ENGL JAPN GERM FREN SPAN ITAL`, también en la USA), pero hay cadenas **reordenadas**: `STRG_Hud` 1/2 (`Command_Visor_Offline`/`CombatVisor`), `STRG_Main` 162/163, y la 181 cambia de texto. Las tablas de nombres se reordenan igual: el código que pida por nombre recibe el texto correcto y el que pida por índice no. No se ha comprobado cuál de las dos hace el código.

Caminos para un solo exe, sin decidir:

1. **Código USA con el disco europeo.** Aceptar `R3MP01` en los ejecutables de Trilogy, servir `selfile.sel` con los desplazamientos USA (tabla generada y validada byte a byte contra la `selfile.sel` USA), traducir también las cuatro variantes europeas de RSO dentro de los mismos ejecutables (el enlazador ya elige el módulo cuyos bytes verifican) y atender `WPADSetCallbackByKPAD`. No ejecuta el código europeo (v3.629); hay que vigilar los índices reordenados y los FRME/MREA cambiados.
2. **Traducir los DOL europeos** con la misma cadena (`R3MP01_FE`, `R3MP01_MP3`). Es lo fiel, pero da dos ejecutables más, salvo que se rehaga la build para meter las dos traducciones en un mismo exe con símbolos separados y el runtime `prime_*` por variante. Es una reforma grande de la build, con riesgo para RM3E01.

## Cambios en RM3E01

**Linux (13/11/2026):** la misma cadena funciona en Linux; ver la sección de Trilogy en [linux_port.md](linux_port.md). Al montarla se vio que el `address_map.json` de Windows es anterior a la corrección del puntero en ultrawide (12/11): sin `make_address_map.py`, `kGuiCursorOrthoReturn` (0x800CB4F8) queda con la dirección de RM3E01 en los ejecutables de Trilogy y esa corrección no se aplica allí. En Windows hay que volver a pasar `make_address_map.py` por cada título antes de la próxima compilación.

**Desde el 16/11/2026 la cadena entera es un solo comando:** `python -B tools/trilogy/build_trilogy.py` (build de pruebas, con QA) o `--public` (sin QA, el que usa `make_release.ps1`). Hace `make_address_map`, `make_title_tree`, `audit_native_collisions`, `build_title`, la compilación y la instalación de los dos títulos, y comprueba la versión y la marca de QA. `--configure-only` solo cambia QA y recompila. Ese día los ejecutables de Trilogy llevaban sin regenerarse desde el 12/11: les faltaban el aviso de compilación de shaders, el periodo VI de 59,94 Hz con el paso de lógica escalado (la dirección `lbl_8068258C` se traslada a `0x806746A4` en MP3 y `0x80623A2C` en el menú; en los tres DOL vale 1/60), la revisión de textos del 14/11 y lo demás de esos días. El anclaje del hook de presentación en `vi.cpp` se ajustó a la nueva forma del código (el bloque de QA que mide la presentación).

Los ejecutables de Trilogy no reciben por sí solos lo que se cambie en las fuentes de RM3E01: `make_title_tree.py` copia el runtime a `titles/`. Si no cambian los hooks, las herramientas ni el traductor, basta con `make_title_tree.py` y `audit_native_collisions.py` de cada título, `ninja WiiCompiled` en `titles/<título>/work/recomp/build/<título>_runtime` e `install_title_exes.py`, con el juego cerrado. Tardó unos 6 minutos el 05/11/2026 (FPS por encima de 60, ver `fps_altos.md`).

Si cambian los hooks, hay que pasar por la cadena completa (`make_address_map.py`, `make_title_tree.py`, `audit_native_collisions.py`, `build_title.py`, ninja e `install_title_exes.py`). El 11/11/2026, al añadir los hooks de ultrawide, se vio que el hook del idioma (`0x802F13A4`, `lbz r0,-0x22C0(r13)`) nunca había llegado a Trilogy: su desplazamiento sobre r13 es el de RM3E01. Ahora `make_title_tree.py` lo lee de la instrucción equivalente de cada DOL, igual que el `lfs` del relleno, y se detiene si esa instrucción deja de ser `lbz r0,d(r13)`. La cadena completa pasó para MP3 y para el menú. La opción de idioma en Trilogy sigue sin probar en pantalla.

## Pendiente

1. Jugar dentro de MP3: mover a Samus, disparar, escáner, gestos y la vista con ratón ya corregida. **Nada de esto se ha visto funcionar todavía con entradas reales.**
2. Revisar los campos de los gestos: las direcciones están mapeadas, pero sus campos no se han contrastado. La comparación general: 87,1 % de las funciones emparejadas sin cambios salvo direcciones, 2,3 % con offsets distintos, 10,6 % con cambios o reordenación de instrucciones.
3. Guardar en MP3, comprobar que el progreso aparece en la ranura del menú, y volver al menú desde el juego (`OSExec /rs5fe_p.dol`, sin probar).
4. Probar `Corruption_3 PC.exe` sin el ejecutable del menú al lado (error esperado en el selector) y con el perfil real del usuario.
5. Textos de gestos con la tecla o el botón asignado (ver `entrada_teclado_raton.md`).
   - El hook se traslada solo y su ancla está comprobada.
   - Hecho el 16/11/2026: se leyeron las tablas STRG de todos los paks de `MP3/` (uno a uno desde el RVZ, sin dejar copia). Trilogy redacta distinto 24 cadenas en alemán, francés, español e italiano (por ejemplo "Gira el mando hacia la derecha."), ya añadidas a `prime_motion_text_hashes.inc` (75 → 99). Su japonés es texto real, no el inglés repetido de RM3E01, y se deja fuera (`motion_text_hashes.py` también lo omite).
   - Comprobar en juego con un build de QA (`unknown prompt`).
6. Empaquetado público (16/11/2026): `package_release.ps1` incluye y comprueba `Corruption_3 Trilogy Menu.exe` y `Corruption_3 Trilogy MP3.exe` con las mismas reglas que `Corruption_3 PC.exe` (sin herramientas de QA ni diagnósticos, versión, sin símbolos de depuración). `make_release.ps1` los compila sin QA con `tools/trilogy/build_trilogy.py --public`, los verifica en el zip y después los devuelve al build de QA (`--configure-only`). `publish_release.ps1` y VirusTotal analizan también los dos ejecutables. `[update-check]` también corre en ellos.

7. **Mejoras de CPU C1 y C4 en Trilogy, para la v0.1.9** (decisión del usuario, 27/11/2026). La 0.1.8 sale con el complemento como estaba.
   - **Situación en la 0.1.8:** Trilogy tiene las tandas del FIFO de GX (C2) y la recogida en línea (C3), que están en el runtime. No tiene las pasadas del traductor:
     - `--ctx-flush-elision elide`;
     - `--proven-ram-accesses on`;
     - `--nonvolatile-reload-elision elide`.

     `build_title.py` y `PublicProducts.cmake` emiten los shards sin ellas, y solo se validaron con RM3E01.
   - **Plan:**
     1. Activarlas en la cadena de `tools/trilogy/`, para MP3 y para el menú.
     2. Compilar primero con `--nonvolatile-reload-elision verify` (`MKW_CTX_RELOAD_CHECK`).
     3. Jugar en MP3 de Trilogy, incluida la cinemática de Rundas, y pasar la repetición del hangar sin líneas `[ctx-flush-check]`.
     4. Pasar a `elide`.
   - **A vigilar:** el código de arranque que carga constantes en r1/r2/r13 se trata como ruptura de ABI; comprobarlo en los dos DOL de Trilogy. Ver `rendimiento_cpu_pendiente.md`, C1-C4.

## Auditoría RM3E01 → Trilogy (16/11/2026)

Petición del usuario: dejar el MP3 de Trilogy equiparado con RM3E01 (textos, reloj de 59,94 Hz, menús, juego, iluminación), con las adaptaciones que Trilogy necesite. Herramientas nuevas, de solo lectura:

- `tools/trilogy/audit_pair_fields.py <título> --port`: compara el código traducido de cada función que usa el port con su pareja de Trilogy (desplazamientos de memoria movidos y cambios de código). `--census` hace lo mismo con todas las funciones emparejadas (`titles/<título>/field_census.txt`).
- `tools/trilogy/audit_small_data.py <título> --port`: compara por valor las constantes de small-data (`r2`) que leen esas funciones, porque `audit_pair_fields.py` normaliza esos desplazamientos y no vería un umbral cambiado.

Referencia independiente: las direcciones de PrimeHack (`AddressDBInit.cpp`, solo consultado) para Corruption suelto y dentro de Trilogy, NTSC-U. Coinciden con el mapa del port en todo lo que usa: gestor de estado `0x805C6C68`, `CTweakGun` `0x8066F87C`, cursor `0x8066FD08`, `0x8066FCFC`, los seis puntos de FOV (`0x8007F3DC`, `0x800DBF50`, `0x800DB584`, `0x80179860`, culling `0x8031490C`, cambio de cámara `0x802B1CE4`), el lazo `0x80171AD8` y los campos de `CPlayer` (`+0x35A8`, `+0x784` movidos; `+0x174`, `+0x358`, `+0x370` iguales).

### Corregido

1. **OSRestart y OSReturnToMenu estaban mal emparejados** (similitud 0,525 y 0,507). El reinicio de CMain llama en RM3E01 a `0x804E57C4` y `0x804E5850`. En el mismo punto, Trilogy MP3 llama a `0x804E5E20` y `0x804E5FF4`, y el menú a `0x80493DCC` y `0x80493FA0`. Los ganchos `PrimeHostOsRestart`/`PrimeHostOsReturnToMenu` estaban en `__OSReturnToMenu` y en otra función que acaba en OSPanic. `make_address_map.py` ahora revisa por llamadas alineadas (de cuerpos emparejados con fiabilidad) toda pareja de similitud inferior a 0,9, y la cambia cuando las llamadas nombran otra función. Además de esas dos, cambió 10 parejas en MP3 y 5 en el menú que el port no usa (y deshizo 5 y 3 parejas débiles que ocupaban la función correcta); el mapa de direcciones del port solo cambió en esos dos ganchos.
2. **OSRestart en Trilogy MP3 abre el menú de Trilogy** (`PrimeTrilogyRestartToMenu` en `prime_trilogy_exec.cpp`; `make_title_tree.py` lo pone en `prime_host_restart.cpp` solo para MP3). En la consola, un reinicio arranca el `main.dol` del disco, que es el menú. En el menú, relanzarse a sí mismo ya es lo correcto.
3. **RetroAchievements fuera de Trilogy de forma explícita** (`-DMKW_PRIME_RCHEEVOS_DIR=OFF` en `build_title.py`). Antes quedaba fuera solo porque la carpeta de rcheevos no existe en el árbol de Trilogy. `package_release.ps1` exige `rc_client` solo en `Corruption_3 PC.exe`.
4. Pantallas de la correa en otros idiomas y 24 avisos de gestos de Trilogy (ver arriba).

### Comprobado sin cambios

- **Reloj (59,94 Hz y paso de lógica):** la función que carga el paso (`fn_802EA018` en RM3E01, `0x802E8964` en MP3, `0x8029F3C4` en el menú) elige entre NTSC y PAL igual en los tres DOL. La dirección trasladada es la NTSC (`0x806746A4`, `0x80623A2C`) y vale 1/60.
- **Textos:** regenerada la tabla de correcciones con RM3E01 y Trilogy (paks de `MP3/` y `fe/` leídos del RVZ a una carpeta temporal, ya borrada). Sale idéntica a la del repositorio, así que la revisión del español del 14/11 ya cubre Trilogy. La tabla de gestos fusionada coincide con `motion_text_hashes.py` sobre los dos discos (99 cadenas).
- **Iluminación moderna:** `0x801ACAB8` cambió (Trilogy movió la prueba de área cargada a `0x8009582C`), pero lee los mismos campos (`+0x98 == 0x11`, `+0xB4`). `CActorLights::ActivateLights`, `BuildAreaLightList` y `0x8037A580` son iguales salvo direcciones globales.
- **Vista con ratón:** los campos movidos ya estaban en `MOVED_FIELDS`. Los demás que lee (`+0x30C`, `+0x358`, `+0x370`, tweaks `+0x1D0`, `+0x1D4`, `+0x3BC`, `+0x2864`, superficies `+0xDC`/`+0xFC`/`+0x11C`) aparecen igual en las funciones de Trilogy, y sus constantes (`0x8017A4B4` y la cadena de giro) valen lo mismo.
- **Gestos:** las constantes de las 12 funciones del analizador de movimiento son idénticas.
- **Entrada:** los lectores con gancho, KPAD (`0x804D93DC`, muestras de 132 bytes) y WPAD crudo (`0x804DA188`, emparejado con similitud 0,52), tienen el mismo contrato y llamadores equivalentes. Solo cambia la zancada interna por canal (1316 → 1336), que el port no usa.
- **Ultrawide:** el gancho multiplica el aspecto que calcula el juego. Trilogy usa otras constantes para su 16:9 (1,1786 y 1,3333 frente a 0,9333 y 1,4685), pero eso no afecta al gancho. Las direcciones de retorno de las cámaras y del cursor caen en funciones idénticas.
- **Modo foto:** el contador del vigilante `0x8066FD1C` tiene 5 de 5 referencias emparejadas. Los ganchos del bucle principal pasan la comprobación de instrucciones; en el menú, `0x802A3220` llama a la actualización `0x802A0EB4` seguida de `cmpwi r3,0`.
- **Clases de material, piel e iconos:** de 10.608 texturas de RM3E01, 10.333 son idénticas en Trilogy. De las 275 que cambian o faltan, solo tres están en listas del port: dos iconos (uno idéntico en `fe/`, el otro sin uso) y una del casco de la nave, que tiene el mismo nivel base (`9748bedf52b0d2b4`).
- **Literales sin traducir de archivos genéricos** (`vi.cpp`, `dvd.cpp`, `os_*.cpp`, …): son restos de Mario Kart que caen en código traducido, igual en RM3E01 que en Trilogy.

# Metroid Prime 3: Corruption PAL (RM3P01), análisis de compatibilidad

Petición del usuario (06/11/2026): comprobar si la versión europea de Corruption (`E:\Metroid Prime 3 - Corruption (Europe) (En,Fr,De,Es,It).rvz`) puede ejecutarse con el mismo `Corruption_3 PC.exe` que RM3E01, sin un exe aparte para PAL. Solo se leyó el disco con `nod` (`titles/disc_file_tool.exe`); no se cambió ni se ejecutó nada del port.

## Resultado: el exe actual no puede ejecutarlo

- Disco `RM3P01`, disco 0, revisión 0. `nodtool verify --md5` sobre el RVZ: coincide con Redump «Metroid Prime 3 - Corruption (Europe) (En,Fr,De,Es,It)», CRC32 `ba6a0568`, MD5 `f4c734a1e191c45e9ac037d92e14485d`, SHA-1 `1fb2a7ae44f97f9a380368717d26b01e9b272159`. El MD5 del archivo `.rvz` es `b500799ada512e401bc3b60f5d802a7c`. La huella de RetroAchievements (`rc_hash_wii` de rcheevos v12.5.0, discos cifrados, leyendo con `nod`) es `af70ac448acf6534cc9548cbf7368fa4`, la que el set admite para Europa. Lo mismo da la copia de `E:\…(En,Fr,De,Es,It) vimm\`, otro RVZ de la misma imagen Redump. La herramienta reproduce `19a1c0da8cad36165fb6287c3ea4adde` con el RVZ USA. Mismos 219 archivos que RM3E01, y 57 cambian de tamaño.
- `main.dol` SHA-1 `6cdfc3bf…`, `2007/08/24 16:52 Build v3.453 (mp3)`; RM3E01 es `2007/07/27 13:13 Build v3.436 (MP3)`. Texto `0x5704E0` (USA `0x56E7E0`), SDA/SDA2 `0x806827C0`/`0x80688FE0`.
- Emparejado con `pair_functions` (`tools/trilogy/make_address_map.py`) sobre `dtk dol split`: 21.442 de 21.667 funciones de RM3E01 son iguales salvo reubicación, pero **solo 214 siguen en la misma dirección**. Hay 118 funciones del juego distintas o sin pareja (65 KB) y 107 casi iguales (127 KB); el SDK no cambia.
- `selfile.sel` añade seis exportaciones que el DOL USA no tiene: `CGameArea::GetOcclusionState`, `CStateManager::ForceDesiredPauseScreen`, `CResLoader::GetResourceNameById`, `CWorldTransManager::StopBryyoWarp`, `CGraphics::mRenderModeObj` y `WPADSetAcceptConnection`.
- RSO: 32 de 74 son idénticos y 42 cambian. 22 de ellos importan alguna de esas funciones nuevas (Space Pirate, Rundas, Dark Samus, Seed Boss 2, Grapple Point…).
- Todos los PAK cambian, también los de mundo. En `Metroid1.pak` cambian 588 STRG, 53 MREA, 37 TXTR, 14 CAUD, 13 CHAR, 7 SAND, 6 CMDL, 6 ANIM, DCLN, PATH, MLVL y SAVW, y hay recursos nuevos (61 STRG, 13 CAUD, 5 DCLN). `Worlds.pak`, `Metroid8.pak` y `MiscData.pak` siguen la misma pauta.

El exe lleva el DOL USA traducido con sus datos incrustados, así que no puede ejecutar el código PAL. A diferencia de Trilogy Europa, usar el código USA con los datos PAL tampoco sirve: los RSO PAL llaman a funciones que no existen en el DOL USA, y las áreas, personajes y colisiones son otra versión del contenido.

## Qué haría falta para un único exe

La versión PAL necesita su propia traducción, como Trilogy: mapa de direcciones RM3E01 → RM3P01, hooks verificados contra las instrucciones, campos movidos revisados en las 225 funciones cambiadas y traducción de sus 42 RSO distintos. Para que siga siendo un solo exe, las dos traducciones tendrían que convivir en el mismo binario:

- Las funciones generadas son `extern "C" func_XXXXXXXX` en los dos DOL, y sus nombres chocan. Haría falta un prefijo por región en el traductor (o renombrar símbolos de objeto) y elegir la región por el ID del disco al arrancar.
- Hacen falta datos incrustados, tabla de símbolos, hooks y RSO por región.
- En el runtime hay unas 400 direcciones literales distintas (muchas son de Mario Kart y no se usan), además de los hooks. Las que usa Prime tendrían que salir de una tabla por región, o habría que compilar el código `prime_*` dos veces en espacios de nombres separados.
- El exe actual ocupa 169 MB y pasaría a unos 330 MB; la compilación tardaría aproximadamente el doble.

Sin decidir: es una reforma de la build con riesgo para RM3E01, y conviene decidirla junto con Trilogy Europa (ver «Trilogy Europa (R3MP01)» en `trilogy_r3me01.md`).

## Decisión y plan (06/11/2026)

El usuario decidió que USA y PAL se ejecuten con el mismo `Corruption_3 PC.exe`.

Ya medido: `make_address_map.py` (lanzado sobre `titles/RM3P01_00`, que es una carpeta derivada) empareja 21.660 de 21.667 funciones (13.479 únicas, 7.963 por posición, 209 por similitud y 9 por llamadas) y resuelve 491 de 522 literales del runtime. Las 31 sin resolver son direcciones de Mario Kart (`vi.cpp`, `dvd.cpp`, `gx_init.cpp`, `task_thread.cpp`…), direcciones nominales de RSO (`0x806AB…`, `prepare_prime_rso_translation.py`) y límites de memoria. Trilogy USA tenía 484 de 522.

Fases:

1. **Traducción PAL completa**, con la cadena de `tools/trilogy/` generalizada a `RM3P01_00`: hooks verificados por instrucción, campos movidos (`MOVED_FIELDS`) revisados en las funciones cambiadas, los 74 RSO PAL y un exe de prueba privado que llegue a jugar.
2. **Un solo exe.** El enlace es `clang++`/`lld` con objetos y librerías estáticas; los DLL (SDL3, Dawn, png, zlib) son compartidos. Los símbolos que define el conjunto PAL (objetos generados y runtime con literales PAL) se renombran con `llvm-objcopy --redefine-syms`, y se enlazan junto a los USA. El exe elige la región por el ID del disco y se relanza a sí mismo con la otra región si hace falta, como ya hace la cadena de Trilogy. La memoria invitada se reserva en tiempo de ejecución (`guest_flat_memory.cpp`), así que no se duplica. Hay que revisar los constructores estáticos con efectos secundarios, que correrían para las dos regiones. Tamaño estimado: unos 330 MB en lugar de 169.
3. **SYSCONF por región.** Escribir `IPL.E60 = 1` (EuRGB60) para que el PAL vaya a 60 Hz, según la política de 60 Hz, y una opción de idioma (`IPL.LNG`: 1 inglés, 2 alemán, 3 francés, 4 español, 5 italiano).
4. RetroAchievements con la huella `af70ac44…`, pruebas de RM3E01 sin regresiones y empaquetado.

## Idiomas en RM3E01, sin el PAL (06/11/2026)

El usuario solo quería el PAL por sus idiomas. Comprobado que **el disco USA ya trae el juego completo en español, francés, alemán e italiano**, y que su código tiene el selector, bloqueado en inglés en consolas NTSC:

- **Datos.** Los 5.095 STRG de los PAK de RM3E01 tienen `ENGL JAPN GERM FREN SPAN ITAL`, sin que falte ninguno. Comparados con el PAL por ID y cadena (7.833 cadenas en STRG comunes), el español coincide en 5.524. El resto son revisiones de redacción del PAL y algunas cadenas reordenadas. Por ejemplo, «Entrada al túnel de mantenimiento denegada. Encuentra una ruta alternativa.» frente a «Túnel de mantenimiento cerrado. Encontrar una ruta alternativa.». El PAL tiene 73 STRG que la USA no tiene.
- **Código** (DOL USA, Ghidra):
  - `0x8038F65C` calcula el índice de idioma como `SCGetLanguage() - 1`, y el japonés o un valor fuera de rango dan 0. El resultado queda en `+0x18` del contexto.
  - `0x803CD4B8` fija el idioma STRG activo (`0x8067DE58`) a partir de la tabla `ENGL GERM FREN SPAN ITAL DUTC JAPN` (`0x805860B0`). La llama el arranque `0x802EDC9C` con el resultado de `0x802F1378`.
  - `0x802F1378` devuelve 0 (inglés) salvo que exista `translator.txt` en el disco o que la bandera `r13-0x22C0` (`0x8067DF00`) valga 1. En ese caso devuelve el índice de la consola, y el neerlandés pasa a inglés.
  - Esa bandera la pone `0x803E80C8` (inicio de `CGraphics`) solo cuando `VIGetTvFormat()` es `VI_PAL` (1) o `VI_EURGB60` (5).
  - `0x8007B444` (avisos del sistema: disco, Nunchuk) usa la misma función solo con esa bandera.
  - `0x802F1378` es idéntica en el PAL (`0x802F2ACC`), salvo las direcciones.
- **Cambio en el port**, sin PAL ni otro exe:
  - Opción `[prime] language` (`english`, `spanish`, `french`, `german`, `italian`) en una pestaña propia, F1 > Language > «Game language», separada de las opciones de vídeo por decisión del usuario (07/11/2026). El panel muestra el idioma en uso y avisa de que hace falta reiniciar.
  - Se escribe en `IPL.LNG` del SYSCONF (2 alemán, 3 francés, 4 español, 5 italiano).
  - Con un idioma distinto del inglés, el overlay de `0x802F1378` (`prepare_prime_runtime_hooks.py`, instrucción `0x802F13A4` `lbz r0,-0x22C0(r13)` verificada) lee la bandera como 1, y el juego sigue su propio camino. Con inglés no cambia nada.
  - Los avisos del sistema de `0x8007B444` siguen en inglés.
  - Requiere reiniciar, porque el juego lee el idioma una vez al arrancar.
- **Probado el 07/11/2026.** Build local (`build_prime_runtime.ps1`, overlay presente en `func_802F1378`) y reproducción QA `hangar_spanish`: copia privada de `hangar_1x_window` con `language = "spanish"`, ISO USA.
  - El registro muestra `[prime-sysconf] … language=spanish`.
  - Capturas con «PULSA … PARA EMPEZAR» en el título y «Misión añadida al Banco de Datos.» en el juego.
  - Reproducción completa, 0 errores, media 59,55 FPS.
  - El usuario lo vio también en español.
  - Sin probar: francés, alemán e italiano, y los ejecutables de Trilogy, que no reciben el cambio hasta regenerar sus árboles y revisar si la función equivalente tiene la misma bandera.
- **Decisión del usuario:** con esto no hace falta la versión PAL, y el plan de un exe con dos regiones queda descartado.
- **Pantalla de la correa por idioma (07/11/2026).** `NoARAM.pak` del disco USA trae `TXTR_StrapScreen{A,B}{169,43}` en inglés, `FREN` y `SPAN`; alemán e italiano usan las inglesas. Con francés o español el juego dibujaba las suyas y no las imágenes propias del port (capturas F7 del usuario, 01:32 y 01:34). Las dos imágenes se instalan también con los hashes XXH64 de esas texturas 16:9: francés `ad8eb16a5f40d2e2` (01/02) y `f6c8146f6a1af236` (02/02), español `0dd8e2ea1b2d67da` (01/02) y `9a1135bd0715a8fe` (02/02). Los nombres coinciden con los de las capturas F7, y el cálculo reproduce los hashes ingleses ya conocidos. Están en `runtime/assets/texture_replacements/strap_screen` y en `package_release.ps1`. Comprobado en el juego en francés: la página 02/02 muestra la imagen del port. Las variantes 4:3 (640x480) no tienen sustituta en ningún idioma, tampoco en inglés.

## Idioma

Corruption PAL no tiene selector de idioma propio: ningún STRG de `FrontEnd.pak`, `GuiDVD.pak`, `GuiNAND.pak` ni `MiscData.pak` contiene «Language» o «Sprache». El juego toma el idioma de la consola (`IPL.LNG` en `/shared2/sys/SYSCONF`, ítem SC `0xB`). Hoy el port lo escribe fijo en inglés (`prime_host_options.cpp`, `WriteSysconf`). Los STRG de los dos discos traen `ENGL JAPN GERM FREN SPAN ITAL`, y el disco USA tiene el texto realmente traducido (por ejemplo «Spiel beenden?»). Falta comprobar si el código USA respeta un idioma distinto del inglés, el francés o el español.

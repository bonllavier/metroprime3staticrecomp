# Correcciones de los textos traducidos (discos USA)

13 de noviembre de 2026. Corruption USA (RM3E01) y Trilogy USA (R3ME01) traen las tablas STRG en
inglés, alemán, francés, español e italiano (`ENGL JAPN GERM FREN SPAN ITAL`; la japonesa repite el
inglés). Algunas cadenas se quedaron en inglés y otras tienen erratas. El port las corrige al vuelo,
leyendo siempre el disco USA del jugador. La ISO no se modifica, no se extrae nada para jugar y el
código no lleva texto del juego.

## Qué se corrige

Con `[prime] language` en `german`, `french`, `spanish` o `italian` (F1 > Language). En inglés no
cambia nada.

| Disco | Idioma | Cadenas | Qué |
|---|---|---|---|
| Corruption | español | ~120 | Frases en inglés ("Gracias por tu ayuda, Samus. The blast doors are down…", "Toggle Bookmark", "Restore Default", "INFORMATION:" en cuatro registros de misión, "[ Bonus Gallery ]", Locked/Stop/Play/View en la galería, títulos del registro "Red Phaazoid" y "Phazite Armored Trooper", "Punto de Enganche Point", "Stick de Control"), ~60 erratas (Phazee, Bryo, Cieloab, enegía, regaloGift, arms…) y un icono de recompensa sobrante |
| Corruption | alemán | ~95 | ~80 erratas (Verteidgungssystems, Voteil, Föderarion, Wilkommen…), "Morphology:", "Extras-Menu", Hussar, el título de Aurora-Einheit 242 (llevaba "Advanced Aerotrooper") y un `&push;` que faltaba |
| Corruption | francés | ~45 | Erratas (recommendé, Puit, piel d'énergie, platefrome, sécuríté…) y "Gragnol Adult" |
| Corruption | italiano | ~50 | Textos enteros en inglés: dos entradas del registro (Scudo Raggi multiblocco / fisso), el aviso de guardar al final de la partida, "file de una versión anterior", el terminal sin energía, "Data moved to Logbook…", "Energy Cell", "Vehicle:", "Cavern Metroid", "Press", "Hold", Stop/Play; erratas (antiaree, Phazee, acciacio…) y dos iconos equivocados |
| Trilogy (menú) | los cuatro | ~110 por idioma | El menú de Trilogy USA quedó casi entero en inglés: botones ([ Play Game ], [ Back ], [ Erase ]…), galerías, los 60 nombres de pistas de la banda sonora, zonas y jefes de MP1/MP2, ranuras, Traje Fusión, control parental |
| Trilogy (MP3) | los cuatro | 17 por idioma | 13 escaneos nuevos de Trilogy (terminales, ascensores, puertas, proyector), códigos de acceso, la pista de la galería y los avisos del tablón Wii y del control parental. El disco europeo los traduce en `RS5.PAK`; el USA no |

En Trilogy, el texto de MP3 ya es el revisado por Nintendo para Europa (comparte el 99,6 % de las
cadenas españolas con Trilogy PAL), así que casi ninguna errata de Corruption USA aparece allí.

Los nombres propios de zonas y jefes siguen el nombre oficial de cada idioma. Las frases están
redactadas para el port. Los discos europeos (RM3P01, R3MP01) solo se usaron para encontrar los
errores; no se copia su texto ni el runtime los lee.

## Cómo funciona

- **Hook.** Es el de los textos de gestos: `CStringTable::GetString` (0x803CDB38 en RM3E01;
  0x803CC1C8 en Trilogy MP3 y 0x8037E470 en el menú de Trilogy, trasladados por `make_title_tree.py`).
  Llama a `PrimeMotionTextOverride` (`runtime/src/hle/prime_motion_text.cpp`) con la dirección del texto
  UTF-8.
- **Correcciones.** Con un idioma distinto del inglés, `PrimeTextFixes::Apply`
  (`runtime/src/hle/prime_text_fixes.cpp`) busca el FNV-1a 64 del texto exacto y el idioma en
  `prime_text_fixes_table.inc`. Si está, aplica sus reglas de `prime_text_fixes_rules.inc`:
  - un fragmento y su corrección, solo en límites de palabra (para que "Bryo" no toque "Bryyo");
  - o la cadena entera.
- **Copia.** La copia corregida se escribe en el bloque de 16 KiB bajo la FST (anillo de tamaño
  variable, como los textos de gestos) y el juego construye su cadena desde ahí.
- **Límite.** Se leen hasta 8 KiB por cadena; la más larga de los discos USA tiene 5,6 KB.
- **Gestos.** Los textos de gestos siguen funcionando igual en el modo teclado/ratón y de mando. Las
  dos tablas sí se solapan: desde la revisión del español (14/11), reglas como «en sentido
  antirreloj» también tocan avisos de gestos («Gira el mando en sentido antirreloj.»). Por eso el hook
  busca primero el aviso de gesto (por el hash del original) y solo aplica las correcciones si no lo es
  o si alguna acción del aviso no tiene tecla asignada. Los reemplazos en español de
  `prime_motion_text.cpp` ya usan la redacción revisada («en el sentido de las agujas del reloj»).

## Regenerar las tablas

Las dos `.inc` las genera `tools/prime_icons/text_fixes.py` a partir de
`tools/prime_icons/text_fixes_data.py`, que contiene las reglas:

- `FRAGMENTS`: fragmento → corrección, por idioma;
- `SCOPED`: lo mismo, limitado a una tabla o a una cadena (`ASSETID#índice`);
- `WHOLE`: cadena completa, identificada por `ASSETID#índice` y una guarda corta que debe aparecer en
  el original. La guarda evita tocar otra revisión del disco con otro texto en ese índice.

```
TEXT_FIX_ROOTS="<RM3E01 files>:<R3ME01 files>" python tools/prime_icons/text_fixes.py third_party/WiiCompiled/runtime/src/hle
```

- `<RM3E01 files>` es la extracción de investigación (`recomp/.tools/disc-extraction-rm3e01/files`).
- `<R3ME01 files>` es la de Trilogy USA (en Linux, `/mnt/mp3port/builds/disc-extraction-r3me01/files`,
  con `MP3/` y `fe/`).
- Se extrae de la imagen con una copia de `nod_extract_build_inputs` que acepte ese ID.
- Separador `:` en Linux y `;` en Windows.
- `TEXT_FIX_REPORT=<archivo>` escribe cada cambio con el texto antes y después, para revisarlo. Ese
  archivo contiene texto del juego: no publicarlo.
- El script se detiene si una regla global no encuentra nada, si un texto corregido supera 8 KiB o si
  una cadena recibe dos correcciones distintas. Las reglas del menú que ya estaban traducidas en un
  idioma salen como "not needed".

## Comprobado

- **C++ frente al generador.** Arnés que compila `prime_text_fixes.cpp` suelto y pasa las 28.589
  cadenas únicas de los dos discos (todos los idiomas) por `Apply`. En los cinco idiomas, 0
  diferencias con el resultado del generador; en inglés no cambia ninguna.
- **Verificación estructural de todas las traducciones de ambos discos** frente al inglés: iconos
  `&image`, balance de `&push`/`&pop`, `&if`/`&endif`, `&link`/`&endlink`, cadenas vacías y texto en
  otro idioma. Lo encontrado y corregible está en las reglas. Quedan, revisados y aceptados:
  - mismo botón con otra textura (B de menú en lugar de B del HUD);
  - frases reformuladas con un icono menos;
  - las opciones del menú de pausa en italiano, que usan otro estilo de casillas en sus cuatro cadenas.
- **Repeticiones QA** `hangar_spanish` (Linux, build `--qa`), sin errores:
  - en español, 59,93 FPS de media y ninguna ventana por debajo de 57;
  - en alemán (`'cfg:prime.language="german"'`, con comillas: sin ellas el TOML no es válido y el juego
    arranca en inglés), 59,19 y 59,23 FPS.

  En alemán una ventana marca 0 muestras (~1,7 s sin presentar al cargar `ScriptAchievement`). Pasa
  igual al arrancar en inglés, donde las correcciones no se activan: es la grabación española
  repetida en otro idioma, no este cambio.

  Esas grabaciones no pasan por los textos corregidos: la prueba en pantalla queda para una partida
  nueva.

## Revisión del español de Corruption (14 de noviembre de 2026)

El usuario notó frases mal adaptadas al jugar en español. Se leyeron los 3.349 pares inglés/español
distintos de Corruption USA y el usuario eligió qué corregir. La lista privada, con citas del juego, está
fuera del repositorio (`analysis/textos_es/revision_espanol_corruption.md`). Reglas en la sección
"Spanish review of 14/11/2026" de `text_fixes_data.py`:

- **Sentido cambiado u opuesto.** Pistas de jefes y escaneos que decían lo contrario (Omega Ridley, Cosechadora de
  Phazon, el aviso final sobre Samus Oscura, "Estación de Guardado" en lugar de Recarga), o diálogos que
  perdían el tono (Dane, el soldado del Olympus, el soldado cautivo).
- **Texto equivocado.** Escaneos con el texto de otro, fichas con otro título (Anhur, Gandrayda),
  estaciones de tránsito y números cambiados.
- **Nombres.** Se unifican con la revisión de Trilogy USA: Campo Aislante, Campeón poseído, Visor de
  Rayos X, Visor de Órdenes, Visor General, Omega Ridley, Estación de tirolina, etc. **El PED (Pila o
  Tanque de Phazon) se deja como está**, por decisión del usuario.
- **Adaptaciones** (proyecto "Dread", "Me encanta que los planes salgan bien", babor, timón...) y el
  inventario en segunda persona.
- **Erratas:** unas 150.

El 26/11 se añadió la bomba de palanca de Metroid3.pak: "Empuja repetidamente hacia atrás y hacia delante para bombear." pasa a "Tira del mando hacia atrás y luego empújalo hacia delante para bombear." (con Wiimote real; en teclado o mando se ve el aviso de gesto con la tecla). 1.116 cadenas, 882 reglas.

No se pudieron corregir cinco frases, porque su texto español es idéntico al de otra frase correcta y el
port las reconoce por el texto: el terminal de la bodega "Está desactivado" (uno está activo), los
datos del satélite "sobre Éter" (uno es de Tallon IV), el proyector "Se ve la imagen de Ghor" (uno no
recibe señal), "No se puede bombardear la zona…" (uno es "sin acceso") y "Desactiva el mecanismo de
seguridad" en un ascensor sin energía.

## No corregido (limitaciones)

- **Cadenas vacías.** En Trilogy, dos nombres de pistas del menú ("Emperor Ing Spider" y "Metroid
  Prime 2: Echoes Credits") están vacíos en los cuatro idiomas. Todas las cadenas vacías tienen el
  mismo hash, así que este sistema no puede distinguirlas.
- **Cuestionario de dificultad** en inglés de `NoARAM` (Trilogy, "Did you complete the battle in
  Tallon IV and Aether?"). También está en inglés en el disco europeo; parece un resto de la versión
  japonesa sin uso.
- **Textos de Metroid Prime 2 que sobran en `MiscData` de Corruption** ("Dark Beam", "Light World",
  "Chozo Battle Suit"…). No se usan.
- **Nombres de objetos en inglés en alemán** ("Morph Ball", "Screw Attack"): Nintendo los dejó así a
  propósito.
- **Estilo.** Revisiones de redacción que el disco europeo hizo sin que hubiera error (por ejemplo,
  imperativo frente a infinitivo en los objetivos).
- **Idioma de redacción.** Las traducciones nuevas en alemán, francés e italiano las ha escrito el
  port; conviene que alguien que hable cada idioma las vea en pantalla.

## Dónde verlas en una partida nueva (español, Corruption)

- G.F.S. Olympus / Norion:
  - el soldado de la Federación tras cerrarse las compuertas ("Gracias por tu ayuda, Samus. Las
    compuertas blindadas se han cerrado…");
  - el escaneo de los escudos de enganche ("Escudo fijo").
- Pausa > Registro:
  - pasar el puntero por el marcador ("Cambiar marcador");
  - criaturas "Phaazoide carmesí" y "Soldado de phazita";
  - registros de misión de Cielolab/Bryyo ("DATOS:").
- Pausa > Opciones, en un deslizador: "Valores predeterminados".
- Menú principal > Extras:
  - "[ Galería de extras ]";
  - en la banda sonora, "Bloqueado", "Parar", "Oír" y "Ver".
- Log de un build QA: `[prime-text-fix] fnv=… text=…` por cada sustitución.

## Coste

Unos 98 KB en el ejecutable tras la revisión del español (1.115 cadenas, 881 reglas): 95 KB de tablas
y textos de las reglas y unos 3 KB de código. Antes de la revisión eran 62 KB (787 cadenas, 581 reglas).
Medido compilando el módulo con `-O3 -march=x86-64-v3` con las tablas y sin ellas.

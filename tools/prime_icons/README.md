# Herramientas de iconos de teclado (RM3E01)

Scripts de solo lectura sobre la copia extraída del disco (`PRIME_ICONS_FILES`, por defecto
`recomp/.tools/disc-extraction-rm3e01/files`). Producen la tabla de texturas de botones que el
runtime lleva embebida en `runtime/src/hle/input/prime_key_icons.cpp` y que se guarda en
`recomp/config/RM3E01_00/keyboard_icon_textures.json`.

- `pak_probe.py`: lector de paks de corruption3 (versión 2: secciones STRG/RSHD/DATA, recursos CMPD).
- `lzo_txtr.py`: LZO1X en Python puro y la segmentación de 16 KB con tamaño `s16` por segmento
  (negativo = sin comprimir); decodifica los recursos y arma hojas de contacto PNG.
- `icon_probe.py`: cabecera TXTR (formato Retro → formato GX), decodificación I4/I8/IA4/IA8/
  RGB565/RGB5A3/RGBA8/C4/C8/CMPR y escritura PNG sin dependencias.
- `tag_scan.py`: descomprime todas las tablas STRG y extrae las etiquetas `&image=` con contexto.
- `xxh64.py`: XXH64 puro (validado con los vectores oficiales) para reproducir el nombre de
  reemplazo de Aurora: `tex1_<ancho>x<alto>_<hash del nivel base>[_<hash de paleta>]_<formato GX>.png`.
- `icon_table.py`: cruza nombres de recursos, etiquetas de texto y hashes y escribe `icon_table.json`.

Orden: `tag_scan.py` → `lzo_txtr.py` (hojas para clasificar a ojo) → `icon_table.py`. Las
texturas del disco están guardadas de abajo arriba; los PNG de reemplazo deben conservar ese orden
de filas (el generador del runtime los voltea).

## Textos traducidos

- `text_fixes_data.py`: correcciones de los textos alemanes, franceses, españoles e italianos de los
  discos USA (fragmentos, fragmentos limitados a una cadena y cadenas completas por `ASSETID#índice`).
- `text_fixes.py`: aplica esas correcciones a todas las tablas STRG de las extracciones de RM3E01 y
  Trilogy USA (`TEXT_FIX_ROOTS`) y escribe `prime_text_fixes_rules.inc` y `prime_text_fixes_table.inc`
  para `runtime/src/hle/prime_text_fixes.cpp`. Ver `docs/textos_traducciones.md`.

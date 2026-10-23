# Perfil Mii local: RM3E01_00

El objetivo inmediato es disponer de un solo Mii persistente, «Jugador», que
la biblioteca RFL original acepte y que el juego pueda asociar a una partida.
No hace falta implementar el editor completo del Canal Mii para este ensayo.

`tools/prepare_prime_mii_profile.py` crea `RFL_DB.dat` en una ruta explícita.
Lee la apariencia predeterminada de la dirección `0x8058A5A8` del DOL propio,
comprobando el hash del DOL y de los 74 bytes. Cambia nombre, creador e identidad
local; no incorpora recursos del juego en el código de la herramienta.
El identificador normal persiste dentro del archivo y no depende de volver
a generar un Mii al arrancar. Su componente de consola es local al PC; no
representa una consola Wii física ni un servicio en línea.

La base incluye 100 espacios oficiales, uno ocupado, la cabecera de 127.456
bytes, los 10.000 nodos vacíos del índice oculto y sus registros vacíos de
64 bytes. Tamaño total: 767.456 bytes. Se escribe el CRC de cabecera según el
polinomio `0x1021`; su residuo debe ser cero. Los 74 bytes de un Mii dentro
de la base no se confunden con el formato de intercambio de 76 bytes con CRC.
La herramienta rechaza una ruta ya existente, incluso si su contenido está
dañado. `--check` valida únicamente el formato de perfil que esta herramienta
genera; no es un importador general de bases con Miis del usuario.

Investigación del formato, sin copiar implementación ni recursos:
[RFL, commit db7c33a](https://github.com/koopthekoopa/RFL/tree/db7c33ae2989b6103a07e666f2f2f9bbd1d2f926),
en particular `RFLi_Database.h`, `RFLi_HiddenDatabase.h`, `RFL_Format.c`
y `RFL_Database.c`. No se encontró licencia general en ese repositorio:
se conserva como referencia privada, con hashes en
`checkpoints/20261021_rfl_format_reference.json`. Las direcciones se
contrastan por separado con nuestro DOL, no con otro juego.

## Evidencia del 21 de octubre

Las cinco pruebas de contrato pasan: CRC contrastado con `binascii`, identidad
y nombre, rechazo de truncamientos y de corrupción incluso con CRC recalculado.
El checkpoint `20261021_prime_single_mii_offline_contract.json` conserva el
primer resultado. Se respaldaron las partidas antes del ensayo en
`.tools/prime_before_single_mii_20261021/`.

El ensayo `base_1792620827_pid11156` abrió la base desde NAND. La rutina
original `0x8054A6D8` aceptó el índice cero y rechazó los otros 99 índices.
Los 74 bytes que devolvió en RAM MEM2 coinciden con el archivo, incluyendo
nombre e identidad. El observador opcional `WII_COMPILED_TRACE_PRIME_MII`
no cambia argumentos, retornos ni memoria guest; al quitar sus tres líneas
se recupera el cuerpo generado original. Evidencia:
`checkpoints/20261021_prime_single_mii_original_acceptance.json`.

Ese ensayo falló posteriormente al dibujar: WebGPU rechazó un viewport con
altura negativa y dimensiones casi cero. **La aceptación del perfil no
demuestra que su retrato, selección y guardado funcionen.** Se investiga el
recorrido de gráficos sin omitir dibujos ni declarar una partida válida por
la mera existencia de `save.bin`.

Siguiente validación: retrato y selección originales, escritura de la asociación
Mii/partida y un segundo arranque que conserve y recupere la misma identidad.
La creación aún es una utilidad de desarrollo explícita; queda pendiente su
integración en la preparación del producto final.

## Comprobaciones adicionales

La rutina CRC original traducida `0x8054AE9C` se compilo aparte con memoria
y temporizacion acotadas: ocho casos PASS, incluyendo la cabecera completa
valida (residuo cero) y un cambio de identidad rechazado. Se verificaron
los limites de su trabajo asincrono de 5.120 bytes; la cabecera requiere
25 callbacks. Evidencia: `20261021_prime_original_rfl_crc_contract.json`.

El primer modelo de grabacion FIFO detecto una semilla incorrecta de arena
baja: empezar en el final de BSS hacia que la asignacion FIFO sobrescribiera
las pilas. Se sustituyo por la constante `__ArenaLo=0x8069C3C0` que esta en
el propio DOL en `0x804DE16C`. El productor comun ahora escribe RAM en bloques
BE de 32 bytes y solo entrega al GP los bloques enlazados, conservando bytes
parciales al cambiar PI. Las pruebas cubren direccion MEM2 alta, orden de
bytes, wrap, stores de ancho mixto y cambios de puntero con bloque parcial.

El ensayo pid2356 muestra otra vez Game Select con retratos y supera el
viewport corrupto, pero falla en un listener original del popup. No equivale
a retratos perfectos. Se tradujeron ese listener y los dos wrappers restantes
de la familia observada. FrontEnd tiene ahora 240 funciones (602 entre cuatro
modulos); siguen siendo traducciones estaticas, no matching manual.

`save.bin` contiene ahora el CreateID local en offset 35156, ausente en el
respaldo anterior; se preservo una copia privada posterior con la base Mii.
La aparicion de esos ocho bytes sola no demuestra asociacion correcta de
slots ni recarga. Esas comprobaciones siguen pendientes.


## Continuidad: cache y prueba posterior

El fallo posterior de recursos se rastreo a /tmp/26226D27.pak, copia parcial
de InGameNAND.pak: 131072/335872 bytes, prefijo exacto del original. Se
reservo como evidencia privada y el juego regenero una copia completa
identica. Las otras cinco caches identificadas coinciden enteras. El
verificador integral ahora comprueba todas las identificadas presentes.

La ultima prueba pid6660 termino 100 s sin fatal y muestra Game Select al
segundo95. RFL original vuelve a aceptar el mismo CreateID local. Esto
confirma persistencia del perfil, no la validez completa de los slots.
El arranque de ScriptAchievement compila con diez funciones y 213
relocaciones verificadas, pero ese probe no ejecuta su bind. El total
actual de las cinco traducciones parciales RSO es 621 funciones.


## Mii: seleccion original y siguiente limite de ejecucion

El probe `base_1792633534_pid8496` muestra Game Select con el tercer espacio
vacio, Mii Select con siete opciones, la advertencia original sobre eliminar
el perfil desde el Canal Mii y, tras aceptarla, tres espacios Game Start.
El juego escribio otra aparicion del CreateID local en save.bin, en 50650
(la anterior esta en 35156). Se conservo una copia privada del save y la base.
Esto prueba seleccion y escritura observadas; la recarga del tercer slot y
la fidelidad grafica completa de los retratos siguen pendientes.
Evidencia: `20261021_prime_mii_original_slot_selection.json`.

El recorrido continuo por el aviso original WiiConnect24 109042, Main Menu,
Play Game y dificultad Normal. ScriptAchievement enlazo y ejecuto su fabrica.
Luego fallo al despachar su metodo virtual text+0x2FC desde DOL 0x8027E3C8.
El volcado verifica las 213 relocaciones y todas las secciones originales.
No se sustituyo el metodo por un stub: se tradujeron los tres metodos locales
de la tabla publicada por el constructor y sus dependencias directas.
La build contiene ahora 22 funciones de Achievement y 633 entre cinco RSO
parciales. Build completo y contrato optimizado de contexto PASS; ejecucion
posterior al nuevo metodo aun pendiente. Ver checkpoints
`20261021_prime_achievement_virtual_boundary.json`,
`20261021_prime_achievement_live_factory_relocations.json` y
`20261021_prime_achievement_virtual_methods_build.json`.

El harness permite opcionalmente hasta 600 segundos; su valor predeterminado
sigue siendo 15. La herramienta de entrada permite hover sin A y captura
mientras conserva el foco, con PID/hash y recibo propio. Las capturas y los
samples PPC prueban la transicion; enviar mensajes Windows solo no basta.


## Recarga del perfil local verificada

En el segundo proceso, `base_1792634066_pid4504`, Game Select conserva los tres
slots Game Start. Seleccionar el tercero vuelve al Main Menu original tras
reconocer el aviso WiiConnect24. El save conserva SHA256
`ed2998db6b84c685568e70c39249ec764e741c8c4a01f23179dafecc9e10537b`
y la base Mii conserva SHA256
`278a56a0c229c683d0a44c2f22cc06ae54dd49188c3a827b3a1150353465f5dd`.
Evidencia: `20261021_prime_mii_slot_reload.json`. El alcance es creacion,
asociacion y recarga del slot en menus; aun no guardado de progreso jugable.
Algunos retratos carecen de rasgos: fidelidad grafica pendiente, no alterar
el perfil para encubrir el problema de dibujo.

El metodo virtual nuevo de Achievement tambien se ejecuto. El bloqueo
posterior es DOL 0x8017393C, cuyo cuerpo traducido ya existia pero quedaba
excluido por una declaracion GX_FATAL_STUB de Mario Kart en la rama Prime
sin registro. Las cinco declaraciones de ese tipo se retiraron solo de la
rama Prime; las ramas de Mario Kart conservan su registro. El escaneo de
los fatal stubs restantes no encuentra colisiones con entradas reales del
DOL de Metroid. Build y prueba del indice nativo se verifican a continuacion.
Checkpoint: `20261021_prime_dol_fatal_stub_exclusion_boundary.json`.

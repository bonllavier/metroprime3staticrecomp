# Arranque de RSO: HOME, Tweaks y front end

Estado vigente: titulo, Game Select, Mii Select, Main Menu y dificultad
originales visibles. Perfil Jugador aceptado por RFL; tercer slot creado,
asociado y recargado en otro proceso hasta Main Menu, con save/base Mii
identicos. Progreso jugable y fidelidad de retratos pendientes. Achievement
enlaza, ejecuta fabrica y metodo virtual; 633 funciones entre cinco RSO
parciales. Se corrigieron cinco exclusiones de funciones DOL heredadas de
Mario Kart; build y pruebas del indice 8/8 PASS, nuevo recorrido en prueba.
El gate integral anterior (2410 lecturas ISO y GPU) no valida toda esta build.
Las secciones siguientes conservan la historia; ver los ultimos checkpoints.

Estado posterior: se corrigio el switch del recorrido FMOD y el juego ya lee
recursos de `FrontEnd.pak`. El probe de 30 segundos verifico 124 lecturas ISO
y no registro fallo fatal. El probe de 55 segundos llego al prologo de
`RSO_ScriptGUIScreenFrontEndUI`, aun sin traducir en ese ejecutable. No hay
pantalla ni audio verificados. Ver la ultima seccion para el siguiente paso.

Actualizacion de las 05:42 UTC: HOME y Tweaks ya permanecen enlazados a la vez.
Tweaks ejecuta sus dos constructores y retorna de su prologo. Tambien retorna
la primera entrada de la tabla de tres callbacks publicada por su helper de
arranque. El limite pasa a la segunda entrada (text+`0x1BB94`). Se verificaron
78 lecturas ISO en este recorrido; no hay pantalla ni audio verificados.
Evidencia: [checkpoint de callbacks](../checkpoints/20261021_prime_tweaks_first_callback.json).
Los apartados iniciales siguientes conservan el checkpoint anterior.

Checkpoint del 21 de octubre de 2026, 05:31 UTC. El ejecutable RM3E01_00
ejecuta el prologo real de `RSO_rhbm`, sus seis constructores, `HBMSize` y
`HBMInit`. Las trazas registran entradas y retornos con SP y LR restaurados.
Esto supera el bloqueo anterior en `0x9016E770`; no es la pantalla de titulo.

El nuevo limite observado es el prologo de `RSO_Tweaks`, en `0x809FB828`,
llamado desde el enlace original en `0x8019F8CC`. Esta direccion procede de
la asignacion observada en esta prueba; no es una ubicacion obligatoria.

Evidencia: [probe verificado](../checkpoints/20261021_prime_rhbm_init_tweaks_boundary.json),
[auditoria del enlace HOME](../checkpoints/20261020_rhbm_live_sdk_relocations.json) y
[auditoria del enlace Tweaks](../checkpoints/20261021_tweaks_live_sdk_relocations.json).

## Implementacion

- Traduccion estatica privada de 19 funciones de HOME, sin instrucciones no
  soportadas. No se considera reconstruccion matching ni recuperacion de clases.
- Los inmediatos afectados por relocaciones se leen de las instrucciones que
  enlazo el SDK, antes de las optimizaciones SSA del traductor. LR y llamadas
  internas se convierten a las direcciones reales de las secciones.
- Se comprueban las 4.238 relocaciones y los hashes de las secciones de HOME
  antes de ejecutarlo. BSS usa su asignacion independiente. Los 120 trampolines
  importados se validan; su despacho conserva el almacenamiento de R12 en la
  pila y el cambio de CTR del codigo original.
- El despacho de modulos es independiente del registro inmutable del DOL.
  Actualmente solo admite HOME en MEM2; ampliar a varios modulos y a MEM1 es
  trabajo pendiente, requerido para Tweaks. No se fuerza el allocator a una base.
- El puente IOS tenia WRITE y SEEK intercambiados. Se corrigieron a 4 y 5,
  respectivamente, conforme al protocolo documentado en la
  [implementacion primaria de Dolphin](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/IOS.h).
  La cache temporal resultante contiene los 332.672 bytes de `MiscData.pak`
  exactos, SHA-256 `78799b1d4161a35362a6e5c9f89157c8ba1616e1c2ead91b4357f846c6524ca6`.

## Validaciones y limites

El probe del PID 10420 verifico 27 lecturas de particion desde la ISO, sus
solapamientos con los archivos de investigacion y las comprobaciones de RAM
registradas por el runtime. La ISO sigue en solo lectura y el DOL matching
conserva su hash. Ejecutable SHA-256:
`cee5451fbbe6448bb64f989e166de9cf9ee2bb1679c07abc6b7f828d2af1dbe1`.

Las pruebas del traductor pasaron 691/691; CTest paso 9/9. Los contratos de
relocaciones comprueban LO/HA, ramas, trampolines y mapeo a tres bases,
pero no ejecutan el modulo completo en tres ubicaciones. Las traducciones
rechazan cuerpos solapados e instrucciones no soportadas; esto no constituye
una prueba general de equivalencia de todo el juego.

No hay pantalla ni audio verificados. Siguen apareciendo avisos XF y reinicios
de FIFO. FST/inventario aun requieren la extraccion privada. No se han probado
Wiimotes fisicos ni la compatibilidad Bluetooth del usuario. No se modificaron
las fuentes matching ni el informe para amigos.

## Continuacion

Generalizar el enlace y despacho comprobado para mantener HOME y Tweaks
simultaneamente, admitir las asignaciones MEM1/MEM2 y traducir el prologo real
de Tweaks con sus dos constructores. La auditoria ya confirma 5.273 relocaciones
y todos sus bytes de seccion en el dump previo a ese prologo. Despues continuar
por la siguiente llamada real y resolver el recorrido grafico hasta presentar
la primera pantalla. No sustituir estas dependencias por stubs.

## Actualizacion posterior al primer checkpoint

El runtime ahora mantiene bindings separados por modulo, acepta MEM1/MEM2 y
obtiene el desplazamiento de la seccion del prologo desde el descriptor
verificado. HOME permanece en MEM2. Tweaks se ejecuto en MEM1 con base
`0x809DB6E0`, distinta de `0x809FB720` en la auditoria anterior. Su prologo
y los dos constructores restauraron SP/LR y retornaron. La auditoria de la
segunda ubicacion vuelve a comprobar las 5.273 relocaciones y todos los bytes
de sus secciones: [auditoria](../checkpoints/20261021_tweaks_after_prolog_audit.json).

La primera entrada de la tabla, text+`0x1BA14`, tambien retorno. Se tradujeron
201 funciones para ese recorrido con cero instrucciones no soportadas. Se
encontraron las otras dos entradas en las parejas HA/LO del helper text+`0x174`;
este escribe tres punteros antes de su llamada de cola importada. La ampliacion
a las tres entradas produce 298 funciones / 28.834 instrucciones PPC y aun
debe verificarse en ejecucion. No significa que todos esos cuerpos se hayan
ejecutado ni que el RSO este completamente recuperado.

Se incorporaron tres pruebas especificas del traductor de RSO: orden de llamada
y retorno en ramas externas con/sin link, y almacenamiento condicional sin phis
de temporales indefinidos. Pasaron 3/3. El tratamiento de llamadas importadas
publica el contexto completo y permite suspension/cambio de hilo. La comprobacion
independiente de la imagen nominal confirma las 9.511 relocaciones sumadas de
ambos modulos. El wrapper de build prepara, traduce y verifica ambos.

Siguiente paso vigente: probar las otras entradas de la tabla Tweaks y continuar
por el siguiente modulo o dependencia real. La ruta grafica sigue pendiente.

## Llegada al modulo del front end

Las primeras dos entradas de la tabla Tweaks retornan y el SDK llama tambien
su epilogo. Se corrigio despues un switch en `0x80420444`, dentro del recorrido
observado de FMOD: el registro del offset de tabla se reutilizaba despues de una
comparacion anterior. El detector tomaba ese limite cero y emitia un solo caso.
Ahora sigue el productor que multiplica por cuatro hasta el selector real y su
limite diez. Los once destinos emitidos se contrastaron con la tabla original
`0x8059F1D0`. El cuerpo se retraduce completo en una copia aislada; no se parcha
el DOL ni se sustituye el switch por un resultado fijo.

Evidencia: [reparacion del switch](../checkpoints/20261021_fmod_switch_repair.json)
y [124 lecturas del arranque](../checkpoints/20261021_prime_frontend_resource_boot.json).
Pruebas del traductor: 693/693; pruebas especificas RSO: 3/3; CTest: 9/9.

El siguiente probe, PID 18576, llega al prologo real de
`RSO_ScriptGUIScreenFrontEndUI.rso`, `0x80B05F98` en esa asignacion, con LR
`0x8019F8CC`. Sus 3.421 relocaciones y todos los bytes de seccion coinciden con
el dump de memoria: [auditoria](../checkpoints/20261021_frontend_ui_live_sdk_relocations.json).
Se prepararon doce funciones estaticas, incluidos prologo y cuatro constructores,
para probar este camino. Su ejecucion aun debe comprobarse tras la compilacion.

Se comprobo ademas que la memoria de Tweaks se reutiliza tras la descarga. El
runtime ahora prepara la retirada del binding al retornar del epilogo original
y rechaza descargarlo con otro frame del modulo activo. La proxima prueba debe
comprobar el evento de retirada y el nuevo modulo. No se afirma soporte general
de descarga/cancelacion hasta ampliar esa evidencia.

El reloj de Windows retrocedio de aproximadamente 05:54 a 00:55 UTC. Se detectaron
objetos con fecha futura y se invalidaron los objetos afectados de runtime/RSO.
El wrapper recompila los traductores sin reutilizar resultados incrementales,
configura CMake explicitamente y suprime el chequeo de regeneracion automatico
en este build privado para evitar el bucle causado por fechas futuras. Usar el
wrapper para regenerar el grafo; los hashes y los probes identifican los
artefactos con independencia del orden aparente de sus horas.

Siguiente paso vigente: ejecutar el prologo y los constructores del modulo del
front end, seguir sus llamadas reales y resolver el recorrido grafico. Los
reinicios de FIFO y avisos XF siguen sin resolver; una ventana negra activa
no equivale a la primera pantalla.

## Fabricas de recursos del front end

Se ejecutaron y contrastaron los retornos del prologo y los cuatro constructores
de `ScriptGUIScreenFrontEndUI`, y del prologo y los dos constructores de
`ScriptWorldTeleporter`. En conjunto, los cuatro modulos admitidos validan
13.600 relocaciones en el enlace original. El front end y WorldTeleporter
ya se ejecutaron en dos asignaciones distintas durante las pruebas; no se
impone una base fija. Esto no es reconstruccion matching completa de los RSO.

La tabla publicada por el helper del front end contiene la fabrica local en
`+4`. La memoria enlazada y el wrapper original `0x8021D30C` confirman ese
recorrido de GSCR: [tabla en memoria](../checkpoints/20261021_live_gscr_frontend_factory_binding.json).
No se ejecuto la primera referencia como sustituto ni se invento un parche
contra un supuesto ciclo.

La fabrica, text+`0x1675C` (identidad nominal `0x806AD3BC`), y sus dependencias
locales ya estan traducidas y ejecutadas. El ultimo probe registra su retorno
con SP/LR restaurados y `r3=0x80A447C0`, tras retornar tambien su constructor
local. La verificacion contrasta 203 lecturas ISO y mantiene los hashes de
DOL/ISO: [checkpoint de fabrica](../checkpoints/20261021_prime_frontend_factory_world_factory_boundary.json).
La creacion del objeto no demuestra que se haya presentado una imagen.

La siguiente llamada es la fabrica de WorldTeleporter, text+`0x2988`, con
identidad nominal `0x80693FC8`. Su wrapper `0x80267758` lee el slot cero,
distinto del `+4` usado por el front end. Se preparo ese cuerpo y su cierre
de llamadas directas: 35 funciones del modulo, todavia pendientes de la prueba
de ejecucion de esta ampliacion. El runtime recibe ahora una tabla de modulos
generada por CMake; agregar un descriptor no requiere cambiar el registro
inmutable del DOL ni recompilar las funciones de los modulos anteriores.

La retirada de Tweaks al terminar su epilogo se observo en ejecucion, con cero
frames activos. Las fechas futuras de 431 entradas de compilacion se ajustaron
despues del rebuild, verificando sus hashes antes y despues; no se alteraron
bytes de fuentes ni recursos del juego. La comprobacion de Ninja paso de pedir
344 compilaciones a pedir unicamente el enlace. La regeneracion del grafo sigue
siendo explicita mediante el wrapper.

Siguiente paso vigente: probar la fabrica de WorldTeleporter y continuar hasta
la presentacion real de la primera pantalla. Mantener separados creacion de
objetos, ejecucion de callbacks y validacion de la imagen/audio.

## Callbacks de ambas escenas: checkpoint vigente

El probe de 55 segundos termino por limite del harness, sin fallo fatal.
Se verificaron 205 lecturas ISO y retornos con SP/LR restaurados de ambas
fabricas y de los callbacks iniciales despachados por 0x8027E378, slot +0x18.
Evidencia: [checkpoint vigente](../checkpoints/20261021_prime_scene_callbacks_boot.json).
Esto reemplaza los siguientes pasos historicos de las secciones anteriores.

El registro contiene 467 funciones traducidas de cuatro modulos: rhbm 19,
Tweaks 298, FrontEndUI 105 y WorldTeleporter 45. Las tablas virtuales observadas
son distintas: 40 metodos en el tramo del front end y siete en WorldTeleporter;
no representan APIs completas recuperadas. La traduccion de LWZU con relocacion
conserva la actualizacion de rA y rechaza formas no revisadas; nueve pruebas
del traductor RSO y 693 pruebas Core pasaron.

Siguiente paso: investigar los reinicios de FIFO y capturar la salida real.
La permanencia del proceso, los objetos creados y los callbacks ejecutados
no prueban la primera pantalla ni la salida de audio. Matching e ISO siguen
sin cambios; la publicacion del FST todavia usa extraccion privada.

## FIFO: corrupcion del primer paquete identificada y corregida

La traza del ejecutable 84cbfba9... registra el primer dibujo en seq=1061:
triangulos, 288 vertices, 24 bytes por vertice y 6.915 bytes totales. Los
registros CP son VCD_LO=0x2200, VCD_HI=1 y VAT_A0=0x41377009. El buffer
anterior de 4.096 bytes borraba el paquete incompleto al desbordarse y
confundia los floats restantes con comandos XF. [Diagnostico verificable](../checkpoints/20261021_prime_fifo_overflow_diagnosis.json).

El buffer ahora crece conservando los bytes, con limite explicito de 16 MiB
y rechazo de rangos invalidos. La prueba nativa verifica el paquete de 6.915,
un XF maximo de 262.149, compactacion y rechazo de exceso. Los dibujos de
cero vertices consumen su cabecera sin abrir un begin pendiente.

El probe posterior registra el envio completo de 288 vertices, ningun aviso
de indexed XF invalido y cero bytes descartados en los resets; conserva
los retornos de modulos/escenas y verifica 206 lecturas ISO.
[Checkpoint tras corregir almacenamiento](../checkpoints/20261021_prime_fifo_storage_boot.json).
La captura PrintWindow sigue negra; una captura GPU negra no demuestra por
si sola ausencia de render, pero tampoco prueba ninguna pantalla del juego.

Siguiente bloqueo identificado: el callback 0x803E9820 depende del breakpoint
de CP, registrado por 0x804D0A8C. El engine espera en 0x803E9F78 y a los
250 ms aborta mediante 0x804D23D0 si su contador no baja. Las tablas MMIO
previas solo almacenaban valores y nunca entregaban la interrupcion CP.
Se esta probando seguimiento de bursts de 32 bytes y entrega del ISR original
0x804D009C desde el slot 0x11, respetando MSR.EE y mascara 0x4000. Aun sin
prueba de ejecucion de esta ampliacion; no se sustituye el callback escribiendo
el estado del engine.

## Ciclo CP estable y envio EFB/XFB

Se corrigio la perdida del breakpoint cuando la CPU seguia escribiendo el
siguiente frame: el read pointer queda detenido y los bytes se encolan hasta
que el callback original quita el breakpoint. La repeticion de 55 segundos
registra 16 entregas/resumes acotados por el logger, cero abortos y 215 lecturas
ISO verificadas, incluidos los cuatro modulos y callbacks.
[Checkpoint CP](../checkpoints/20261021_prime_cp_frame_loop.json).

Los triggers BP 0x52 de display copy ahora ejecutan la copia real con destino,
rectangulo y clear originales. Para el recorrido observado: 640x448, pitch
640, escala uno y destino 0x9008C800. Aurora conserva una textura por destino
XFB, y se selecciona la publicada por el ISR/VIFlush original en 0x8067E710.
Se corrigio el stride BP: las unidades de 32 bytes equivalen a 16 pixeles,
sin sumar quince pixeles adicionales. Escalas distintas de uno, copias a
textura y TLUT todavia requieren adaptacion; se rechazan de forma explicita.
Los cambios posteriores a negro siguen pendientes de una presentacion blank
dedicada: el adaptador solo espera la primera salida VI activa.

La primera entrega al worker expuso un cambio de hilo desde un contexto
interruptor temporal. El contador de deferral de Prime ahora vive en el host,
no en los globals de scheduler de MKW; el selector original consulta ese
contador y los callbacks no entran al scheduler MKW. La prueba posterior
permanece activa 55 segundos y el contrato de continuaciones vuelve a pasar
200 transferencias, identidad CPU, stacks locales y relevo de excepciones.
El detector del probe ahora reconoce tambien excepciones C++ no controladas.

La capa opaca de arranque de WiiCompiled esperaba el evento strap de MKW.
Se desactivo en este perfil para observar el framebuffer original. Su titulo
visible en una captura previa no cuenta como pantalla de Metroid; la captura
sin esa capa sigue negra. El checkpoint vigente valida submission y seleccion,
no contenido visual: 213 lecturas ISO y los cuatro modulos/escenas.

Siguiente paso: validar el puente de direcciones de texturas BP a memoria
host acotada. Los registros image3 previos cambiaban la direccion numerica,
pero no publicaban obj.data para Aurora. La ampliacion preparada mantiene
subidas sin cache mientras faltan invalidaciones y lifetimes completos; las
texturas indexadas requieren TLUT verificado. No se ha demostrado aun que
esta ampliacion dibuje la pantalla del juego.


## Primera pantalla original visible: persistencia del EFB

Se verifico visualmente el aviso original de seguridad del Wiimote dentro de
la ventana Windows. El codigo traducido lee los recursos y emite los vertices,
textura CMPR, mezcla y copias GX originales. La pantalla de titulo sigue pendiente.

La causa de la salida negra quedo aislada con lecturas GPU: textura, vertices,
indices y matrices correctos. El aviso se dibujaba despues de la copia XFB.
El siguiente frame de PC borraba el EFB antes de la copia siguiente. Prime ahora
conserva el EFB entre frames; el primer frame y los comandos GX controlan los
borrados. Se mantiene el reset de interpolacion separado de la reanudacion de
un lote a mitad de frame.

La repeticion sin trazado detallado permanecio activa 55 segundos: 213 lecturas
ISO contrastadas, cuatro RSO/callbacks, cero resets FIFO y cero errores WebGPU
con validacion y robustez habilitadas. El probe activa la validacion por defecto.
[Verificacion automatica](../checkpoints/20261021_prime_persistent_efb_boot.json)
y [revision visual](../checkpoints/20261021_prime_first_startup_screen_visual_review.json).
La verificacion automatica conserva game_screen_verified=false: no interpreta
pixeles; la revision visual identifica por separado el aviso y sus limites.

La prueba con trazado detallado mostro el aviso, pero despues registro un aborto
FIFO con 114 bytes pendientes. No se acepto como prueba de estabilidad. Las
capturas posteriores de ambas pruebas muestran geometria corrupta; el renderer
no se declara fiel ni completo.

Las texturas raw, cargas TLUT con almacenamiento propio y el interprete
conservador de display lists ya tienen evidencia previa en el
[checkpoint de texturas](../checkpoints/20261021_prime_textures_tlut_display_lists.json).
Copias a textura raw, escalas distintas de uno, transiciones a negro, audio,
controles reales y ausencia de extraccion privada siguen sin completar.

Siguiente diagnostico: la emision de atributos directos enteros desde display
lists conservaba VAT entero pero escribia floats; los colores empaquetados
se expandian sin cambiar VAT. Se prueba conservar su ancho y representacion
originales para resolver la geometria posterior al aviso.
Las capturas con recursos permanecen privadas en recomp/.tools y UserData/Logs;
no se incorporaron al informe congelado para amigos.


## Lector FIFO unico y captura GPU del font

La CPU emitia completos los veinte floats de un glifo. La cola previa al
backend tambien conservaba el ultimo vertice, pero el buffer GPU perdia el
cero Y y acababa incluyendo 0x610F0000 del comando BP siguiente. Los paquetes
raw se retiran antes de entregar una copia estable al backend; los productores
recursivos solo anexan bytes y el consumidor original sigue el recorrido en
orden. Una espera del renderer puede bombear callbacks, por lo que conservar
un puntero al vector mutable no era suficiente.

La repeticion posterior valida 217 lecturas ISO, los cuatro modulos/callbacks
y 55 segundos sin reset FIFO, fatal o error WebGPU. La captura a 15 segundos
conserva el aviso; a 40 segundos ya no aparecen franjas, pero la salida queda
negra. [Checkpoint](../checkpoints/20261021_prime_fifo_single_consumer_boot.json).
No se declara alcanzada la pantalla de titulo. El atlas de fuente 64x80 se
ha leido de la GPU y contiene glifos correctos; se contrasta ahora la geometria
y el estado de sus draws. La prueba nativa de ownership del consumidor cubre
entrada anidada, conservacion del owner externo y liberacion tras excepcion.
La correccion de enteros/colores empaquetados de display lists es independiente;
por si sola no resolvio las franjas.


## Estado posterior: transicion aun sin equivalencia grafica

La secuencia ampliada de capturas encontro franjas cerca de 22 segundos y
negro despues. Las franjas siguen presentes en algunas pruebas; la proteccion
FIFO por si sola no demuestra resolver el renderizado. La captura conjunta
de EFB y buffers permite comparar el mismo frame, evitando conclusiones a
partir de dos ejecuciones diferentes.

Se valido el shader del font: stride 20, posicion +0, UV +12; el pipeline
F892D70D494F0487 usa el shader 5647F2E965F39515, cull desactivado y mezcla
SRCALPHA/INVSRCALPHA. Un shader compilado de nuevo tampoco resolvio la salida.
Los vertices, matrices, atlas y estado de draw son evidencia de entrada;
no equivalen a demostrar el resultado del pipeline. Los pases persistentes
ahora esperan pipelines listos para no perder dibujos durante su compilacion.

Las escrituras BP directas cruzan la barrera del worker y su mutex, y las
copias raw hacen GXDrawDone aun con FIFO nativo vacio. Los paquetes XF e
indexed-XF tambien conservan copias estables mientras el renderer puede
bombear callbacks. Las copias XFB pendientes se registran por destino y
revision: presentar uno no consume el otro, ni una copia mas nueva del mismo.
Los retraces diferidos no cambian la fuente de presentacion. El test nativo
cubre dos destinos y una actualizacion que llega durante la presentacion.

Vulkan encontro inicialmente una referencia a TLUT no cargada. El adaptador
recorria todas las texturas residentes, incluso las no usadas por el draw.
Ahora consulta las dependencias del shader y prepara solo los slots muestreados;
si uno usado no tiene TLUT, mantiene el error explicito y detalla slot/formato.
La repeticion Vulkan permanecio activa 55 segundos con 206 lecturas ISO,
cuatro modulos/callbacks y cero resets o errores WebGPU, pero conserva la
corrupcion grafica posterior. Config.toml se restauro byte por byte.

La prueba posterior en Direct3D 12 usa paquetes XF estables y continua el
recorrido original: se observa lectura del header de Video/FrontEnd/attract01.thp.
Esto no demuestra reproduccion de video ni pantalla de titulo. Proximo:
contrastar la salida efectiva del shader de glifos y el orden draw/copy por
frame. Los contratos del traductor y del transporte de hilos conservan sus
resultados previos; esta iteracion no los cuenta como reejecutados.


## Transformacion GPU y raster aislado del texto

La prueba opcional ejecuta el cuerpo del vertex shader original del font sobre
los mismos buffers GPU y el mismo rango de uniformes del primer glifo. Escribe
solo en un buffer de diagnostico. El error maximo frente al calculo CPU con
vertices/matrices originales fue 4.754496776837591e-08. Una segunda prueba
rasteriza el DrawData original en un target privado: aparecen 47 pixeles dentro
de x=176..182, y=208..218. verify_prime_gpu_glyph_diagnostic.py contrasta hashes,
revision de ejecutable, log, coordenadas transformadas y, opcionalmente, limites
del raster. Esa imagen aislada no es una pantalla del juego ni reemplaza su UI.

La captura ampliada de un mismo pase registra 50 quads de glifos y 49 strips
de ocho vertices cero emitidos por el SDK para vaciar BP. Todos los valores
numericos y las cabeceras de uniformes son validos. El prefix de comandos hasta
el primer glifo conserva la C; al incluir el strip cero siguiente desaparece.
Una secuencia de draws en pases separados mantiene el texto en su region,
aunque todavia tiene defectos. Republicar pipeline/index buffer/texturas no lo
resolvio. Bindings independientes de uniformes cambian el prefix aislado, pero
no resolvieron el frame normal. Estas opciones siguen siendo diagnosticos,
no una correccion declarada de la interfaz de uniformes.

El adaptador evita rasterizar triangulos con todos sus registros directos de
vertice identicos: incluyen los indices de matriz, por lo que esos triangulos
no pueden cubrir pixeles en GX. La publicacion del estado se conserva. El SDK
nativo de Aurora ya evita su dummy strip en __GXSendFlushPrim. El test rechaza
registros distintos, tamanos inconsistentes, null y menos de tres vertices.
No se aplica esta regla a lineas, puntos ni fuentes indexadas. La prueba normal
posterior pasa 55 segundos y 211 lecturas ISO sin resets ni errores WebGPU,
pero la salida de texto sigue negra y no demuestra fidelidad completa.

Proximo: contrastar el comportamiento consecutivo de uniformes y draws dentro
del pase, incluida la copia XFB. Mantener la UI original de conexion de mandos;
no forzar estados de progreso ni sustituirla por una imagen o texto sintetico.
Las capturas y buffers de diagnostico siguen privados en recomp/.tools.


## Correccion del transporte FIFO con CP detenido

La captura del primer cuadro no bastaba: 120 cuadros despues aparecia una
coordenada Y perdida en el cuarto vertice y datos BP dentro del siguiente
strip. La traza del productor encontro la perdida antes de ejecutar shaders.
`GX_HLE_FIFO_WriteBurst` aplicaba su walker de comandos a un burst cuyo prefijo
era cero, aunque los bytes anteriores seguian retenidos por el breakpoint CP.
Ese cero era parte de un vertice, pero el atajo lo descartaba como NOP.

Prime ahora envia todos los bursts al productor ordenado usado por las
escrituras escalares. `prime_fifo_burst.h` preserva anchuras y orden big-endian;
se probaron el prefijo cero retenido y todas las longitudes de cola 0..11.
El test nativo FIFO paso por CTest. No se modificaron ISO ni fuentes matching.

Prueba normal del ejecutable `07c1b3eb40a8d3d7a58aa313683dcbc0f9cbbf74958a8380b0ee03e64bbb5a13`:
55 segundos, 219 lecturas ISO contrastadas, GPU/robustness validas, cuatro
modulos y callbacks originales, cero resets. Los triangulos alargados ya no
aparecen; el texto queda en su region, pero sus pixeles siguen incompletos.
Otro arranque audito los 50 quads del frame 1400: posiciones, UV, uniformes
e indices correctos. Dos corrupciones intencionadas del verificador fueron
rechazadas sin cambiar los artefactos originales. Esto no verifica el titulo.

La observacion opcional usa atomicos en fragment shader; la escritura de
storage desde vertex shader fue rechazada por WebGPU. Sus bounds se acumulan
entre cuadros y no se presentan como una prueba de cobertura de un solo frame.

Evidencia: `../checkpoints/20261021_prime_fifo_burst_order_summary.json`,
`20261021_prime_fifo_burst_order_boot.json`, `20261021_prime_fifo_burst_font_input.json`
y `20261021_prime_fifo_burst_font_negative_checks.json`. Siguiente paso:
contrastar textura/paleta y muestreo de letras. Controles y titulo pendientes.


## Fuente original legible y bloqueo de entrada identificado

Se identifico una segunda causa independiente: `resolve_sampled_textures`
reutilizaba el memo de binding aunque `GXTexObj_::no_cache()` estaba activo.
La identidad del memo no incluia la paleta y el original cambia cuatro TLUT
para seleccionar planos de bits de la fuente C4. Los vertices correctos seguian
muestreando la primera paleta. La resolucion ahora respeta no_cache tambien
en ese memo. El cambio conserva los caches normales de objetos cacheables.

La ventana real ahora muestra el texto completo:
`Communications with the Wii Remote have been interrupted.`
No se introdujo una imagen de reemplazo ni se forzo el estado del juego.
El aviso es coherente con que no existe todavia un mando conectado al SDK
original de Prime. Su entrada sigue pendiente; el titulo no esta verificado.

Verificacion adicional: se capturaron privadamente los 50 quads originales,
la textura C4 de 2560 bytes y las cuatro paletas. Un calculo Python independiente
reconstruye bloques C4, indices, paletas, interpolacion bilineal y blend SRC_ALPHA.
Sobre 286720 pixeles RGB del raster GPU estable, 286537 coinciden exactamente;
los 183 restantes difieren un solo nivel de canal de 8 bits. Tolerancia espacial
cero. Las capturas antiguas con paleta reutilizada y frame negro fallan esta
misma comparacion (error maximo 255). La primera captura de presentacion podia
ser negra porque aun seleccionaba el XFB anterior; el cuadro posterior estable
se registra por separado. No ocultar ese limite del selector de captura.

Ejecutable actual SHA256: `82088d65098c1264f3199c97204316bb8a2420110e9e1d64237b38515b6b0f33`.
Arranque normal 55 segundos: 214 lecturas ISO verificadas, cuatro modulos,
fabricas/callbacks, GPU/robustness habilitadas, cero resets FIFO.
Evidencia en `../checkpoints/20261021_prime_readable_remote_warning_summary.json`,
`20261021_prime_readable_remote_warning_boot.json`, `20261021_prime_font_pixel_reference.json`
y `20261021_prime_font_pixel_reference_negative.json`.

Siguiente: WPADProbe original 0x804F66B8 y WPADRead 0x804F7014 alimentan
el anillo de muestras desde 0x804D9BAC; La candidata KPADRead 0x804D9138 usa muestras de
132 bytes. Esto es evidencia parcial del recorrido, no autoriza reutilizar
las direcciones MKW de los wrappers existentes. Confirmar ABI, callbacks,
formatos y entrada host antes de declarar control conectado. La herramienta
Ghidra MCP no estaba disponible en esta inspeccion; fuentes traducidas y DOL
siguen disponibles para cotejo independiente.

Auditoria de siete destinos de entrada: original DOL y hashes de traduccion
verificados en `../checkpoints/20261021_prime_input_boundary_research.json`.
El nombre KPADRead sigue inferido; el tamano no prueba compatibilidad de ABI.
La verificacion final de arranque incluye los hashes de gx.cpp, del productor
de bursts y de los verificadores de raster en
`../checkpoints/20261021_prime_readable_remote_warning_full_sources.json`.


## Puente experimental de entrada de PC

Se agrego un proveedor de eventos SDL que conserva foco, acordes de botones,
stick normalizado y posicion del raton. Es optativo con
WII_COMPILED_PRIME_KEYBOARD_MOUSE=1 o tools/run_prime_keyboard_mouse.ps1.
No anuncia un mando Bluetooth emparejado: es el dispositivo virtual del modo
teclado con Nunchuk. Siguen pendientes movimiento/giros y compatibilidad fisica.

Los overlays 0x804F66B8 y 0x804D9138 consultan el modo; desactivado, ejecutan
su cuerpo original. El primer ensayo solo respondia a Probe y no producia
muestras: el consumidor esperaba los callbacks originales. Se verificaron
WPADSetConnectCallback y WPADSetExtensionCallback (+2212/+2208 del control).
Su notificacion se entrega con EE habilitado en registros aislados. El juego
recibe sus callbacks 0x803C48AC/0x803C499C y comienza a leer registros de 132
bytes. No se alteraron directamente los estados de la pantalla para saltarla.

La prueba Windows dirigida al PID comprobo propietario de ventana y foco,
publico mensajes, movio el cursor temporalmente y restauro foco/cursor. Se
capturaron siete muestras PPC32: A y sus edges correctos, W como stick hacia
arriba, retorno neutral y apuntado valido. Tres fixtures corruptas (edges,
apuntado ausente y acelerometro NaN) fueron rechazadas por el verificador.
Los mensajes Windows sin foco del primer ensayo no llegaron a las muestras
y no se cuentan como exito. La prueba no acredita latencia ni mando fisico.

Ejecutable SHA256: 1e6e8414efee9b4c5ffbac8ad73cf4f0dd44759eb7a9b7cc1345bb56b96825b7.
Teclado: 40 segundos, 217 lecturas ISO verificadas, GPU/robustness y cuatro
modulos/callbacks, cero resets. Original desactivado: 55 segundos, 213 lecturas
y gates de textura/TLUT completos. El gate TLUT no se exige al recorrido del
video, que ya no dibuja el aviso de desconexion indexado; no se modifico su
exigencia para los arranques que si lo usan.

El juego abre Video/FrontEnd/attract01.thp y lee cabecera y cuatro tramos de
frames, pero no presenta video ni titulo. Cabecera privada THP 1.1, 640x448,
1285 frames a 59.94 fps, maxAudioSamples=0. Son metadata y lecturas observadas,
no video ejecutado. Siguiente paso: seguir decodificacion, sincronizacion y
presentacion originales. No sustituirlo por un reproductor o titulo inventado.

Evidencia: ../checkpoints/20261021_prime_keyboard_mouse_summary.json,
20261021_prime_keyboard_mouse_windows_input.json,
20261021_prime_keyboard_mouse_negative_checks.json,
20261021_prime_keyboard_mouse_boot.json,
20261021_prime_keyboard_final_disabled_boot.json. Instrucciones y limites de
controles en entrada_teclado_raton.md.

La captura normal a 25 s del ejecutable actual tiene el mismo SHA256 de PNG
que antes del puente optativo: 5808551d63e5c2c1d7de32e13b611ef7a5dedb4b1eb8ffbe99dc6d464f80e036.
Metadata y siete tramos THP contrastados en
../checkpoints/20261021_prime_attract_header_and_reads.json.


## Introduccion original visible: imports RSO, copia EFB y DMA

Este checkpoint sustituye la hipotesis de que attract01.thp fallaba al
iniciar el decodificador. La traza mostro que era una precarga pausada.
El front end esperaba en 0x806AB6A0 aunque los 35 objetos asociados ya
habian terminado de cargar. IsLoading devolvia cero; el ensamblado x64
conservaba el puntero anterior en lugar de leer el retorno desde ctx.
PPC_RsoImportCall obtenia ctx por TLS y el caller tenia MKW_RESTRICT.

La emision pasa ahora ctx como argumento explicito del helper, cuya API
recibe ese mismo contexto. Los cuatro modulos se regeneraron y auditaron:
467 funciones, 3.156 sitios de import con contexto explicito. El ensamblado
posterior lee ctx->gpr[3]. Una fixture nativa -O3 compila el cuerpo real de
0x806AB6A0 y el IsLoading original: siete escenarios pasan, preservando
SP/LR y registros no volatiles. El control negativo reproduce el texto
anterior (solo se retiran los tres argumentos ctx) y falla en las cargas
finalizadas. Pasaron 10 pruebas RSO y 693 del traductor Core.

En el juego, la comprobacion pasa a uno y llega PLAY a una pelicula de
901 cuadros: Mp3_Title_Sequence.thp (640x448, aproximadamente 29.97 fps).
El primer ensayo avanzo hasta una frontera explicita de copia de textura;
218 lecturas ISO y esa terminacion se verificaron como fallo esperado,
no como arranque exitoso. No se forzo el estado ni se agrego un reproductor.

Se adapto la copia de color BP hacia GXCopyTex con orden de draws y tracking
de destino. Los registros originales del SDK en el primer caso son trigger
0x10043, stride 0xA0, fuente 640x448 y formato RGB565: 573.440 bytes.
La inversion de campos se contrasto con GXSetTexCopyDst 0x804D3184.
La adapter admite RGB565/RGB5A3/RGBA8 sin half-scale, con stride minimo y
rectangulo EFB validado; rechaza modos y strides no soportados. Sus casos
positivos/negativos estan en gx_fifo_storage_tests.cpp. La primera ejecucion
sin DMA produjo ruido de memoria, no video correcto.

El decodificador original usa los SPR DMAU/DMAL (922/923), que solo se
conservaban como valores. Ahora el perfil Prime realiza copias de bytes
entre RAM y los 16 KiB reales de locked cache, valida rangos y borra el bit
de transferencia al terminar. El campo de longitud cero representa 128
bloques de 32 bytes. La finalizacion es sincrona: no reproduce tiempos ni
cola de hardware. Se comprobaron los campos con el SDK 0x804DFB30/0x804DFB54
y con las referencias de Dolphin registradas con commit, licencia y hashes
en 20261021_dolphin_locked_cache_reference.json. No se copio el decoder ni
se incluyeron archivos de Dolphin en el producto.

La ejecucion de 50 s (base_1792589663_pid20736), SHA256 del ejecutable
e99503e3b70b27c6b19125f4545eaf233f2d24e2ea5519f50f6dda1ca4241bf3,
mostro la introduccion original Nintendo Presents / Retro Studios.
Se verificaron 1.012 lecturas ISO, GPU/robustness, CP/FIFO/XFB y los cuatro
modulos/fabricas/callbacks. Sin fatal ni resets FIFO. Un segundo verificador
comprobo PLAY, fin de espera, 64 retornos cero del SDK, las primeras 32
transferencias DMA y 32 copias RGB565. Tambien comprobo que los ocho cuerpos
originales de las trazas se conservan completos al retirar la instrumentacion.
Las capturas de 25/35/45 s muestran cambios de la secuencia; una ejecucion
posterior sin trazas llega al planeta al segundo 54. Eso aun no demuestra
menu de titulo interactivo, audio, rendimiento ni igualdad grafica con Wii.

Evidencia nueva en ../checkpoints/: 20261021_prime_rso_explicit_context_contract.json,
20261021_prime_rso_context_texture_boundary.json,
20261021_prime_title_color_copy_boot.json (ruido anterior a DMA),
20261021_prime_locked_cache_title_intro_boot.json y
20261021_prime_original_title_intro_activation.json.
Siguiente: cotejar un cuadro decodificado con una referencia independiente,
observar el final de la introduccion y continuar al titulo real.
Matching e ISO siguen intactos; informe para amigos congelado.


## Titulo visible, formato normal/NBT y callbacks del menu

La ejecucion base_1792613248_pid9292 mostro el titulo original y Press A to
Start a los 70 s. No fue una reproduccion externa: siguio el front end,
SDK y decoder originales. Posteriormente detecto cached_stride=52 frente
a 28 bytes reales por vertice. La publicacion del VAT estaba aplicando el
slot NBT no usado despues del slot NRM, que comparte registro. El perfil
Prime desactiva el alias NBT y publica la normal canonica; conserva la
comprobacion estricta del tamano de paquete. No se declara soporte completo
de todas las variantes NBT. Evidencia de la frontera: 1.838 lecturas ISO
verificadas, checkpoint 20261021_prime_title_vertex_boundary.json.

Al entregar A aparecieron llamadas indirectas que la clausura de llamadas
directas no contenia. Se audito la tabla PPC de 38 slots (22 destinos
locales y 16 imports DOL), sus cabeceras y relocaciones. Tambien se auditaron
20 registros de puntero a miembro de tres palabras (0, FFFFFFFF, destino)
y siete pares HA/LO hacia el wrapper 0x8069A4E8. Se incluyeron los destinos
originales como raices y su clausura directa: FrontEnd pasa de 105 a 198 y
luego a 211 funciones; los cuatro RSO suman 573. Esto es cobertura del
traductor, no recuperacion manual matching ni clases completas. Las 3.421
relocaciones de FrontEnd permanecen verificadas. No se sustituyeron callbacks
por retornos inventados. El contrato de imports explicitos pasa con 211
funciones, siete escenarios y el control negativo del antiguo alias TLS.
Las pruebas del traductor RSO pasan 10/10; las del Core, 693/693.

Build actual SHA256:
51e43c306172e736492c46c07eb4c6c9ac34916b4147a3d416cdd471719cad97.
base_1792615522_pid8940 completo 55 s, sin fatal: verificacion de GPU,
CP/FIFO/XFB, cuatro modulos y 1.156 lecturas ISO PASS, ademas de muestras
reales A/soltar/stick/apuntado desde mensajes de ventana a SDL y PPC.
base_1792615712_pid1348 completo 110 s, sin fatal, mostrando el titulo a
65 y 70 s. A al segundo 75 llego tras la transicion del front end a su
secuencia de demostracion: las imagenes posteriores son intro/attract,
no acceso demostrado al menu ni gameplay. Su verificacion ISO completa
sigue en curso. Checkpoints: 20261021_prime_popup_callbacks_boot.json,
20261021_prime_popup_callbacks_keyboard_input.json,
20261021_prime_title75_keyboard_input.json y
20261021_prime_title110_visual_observation.json.

Una prueba previa de 120 s quedo interrumpida sin runtime_probe.json;
no se considero superada. Se encontro Config.toml con 441 bytes NUL,
se preservo la copia y se restauro configuracion valida desde el backup
con la ruta de la ISO propia. La causa de la corrupcion no esta demostrada.
El harness conserva ahora progreso separado con observation_complete=false
hasta que exista un informe terminal; reconoce tambien los fallos de
arranque suprimidos. Ver 20261021_prime_interrupted_probe_and_config_restore.json.

## Comparacion independiente del decoder: limites pendientes

El cuadro 119 se identifico por igualdad exacta de sus 34.684 bytes JPEG
con la pelicula THP privada. Tras desentramar Y/U/V de locked cache,
FFmpeg difiere como maximo dos niveles y 134 muestras superan el limite
estricto de uno. Una referencia independiente con Huffman, dequantizacion
e IDCT coseno de doble precision segun T.81, truncando al almacenar,
reduce la diferencia: Y/U maximo uno; V tiene dos muestras con diferencia
de dos. Ambos checkpoints conservan FAIL; no se amplio el umbral ni se
altero el decoder original para ajustarlo a otra implementacion. Son
comparaciones con referencias independientes, no mediciones de hardware Wii.
PSQ y paired-FMA nativos pasan sus cuatro pruebas; la aritmetica generada
conserva -fno-fast-math y -ffp-contract=off. Evidencias:
20261021_prime_title_frame119_pixel_comparison.json y
20261021_prime_title_frame119_direct_idct.json. FFmpeg y sus licencias
permanecen como herramienta privada de investigacion, sin dependencia
nueva del ejecutable publicado.

Siguiente: pulsar A mientras el titulo esta visible, revisar las pantallas
posteriores y comprobar sus callbacks sin saltar el flujo original.
Audio, Wiimote real, rendimiento y juego completo siguen pendientes.
La extraccion privada aun es requisito del ensayo, no del producto final.


## Incidente de espacio durante la siguiente prueba

La prueba base_1792616120_pid9928 alcanzo un nuevo destino indirecto
0x80B14164, LR 0x80B0B990, desde el callback original 0x80B0B934.
No se considero superada: la unidad G se lleno y los volcados MEM1/MEM2
quedaron vacios; una captura tambien fallo. Se conservaron el informe
y crash_missing_target.txt. Es necesario repetir con espacio antes de
atribuir el nuevo destino a una tabla concreta o ampliar raices.
La compresion NTFS de logs/volcados existentes recupero 7,3 GiB sin borrar
ni cambiar su contenido logico. Un documento truncado por falta de espacio
se reconstruyo a partir de las ediciones originales del historial de
herramientas; se omitio una edicion historica que habia fallado por encoding.
El proximo informe se escribe con reemplazo atomico y comprobacion de espacio.

Repeticion del listener con espacio: base_1792616411_pid15836. El nodo
se reconstruye desde r4-20 = 0x808DBDE0; r30 ya apunta al siguiente nodo.
El callback +12 contiene 0x80B14164 (nominal 0x806AC844, text+0x15BE4)
y el objeto +16 coincide con r3=0x80A53700. Sus primeros 24
bytes coinciden con el RSO original. Dos pares HA/LO en text+0x15976/1597E
y +0x15A96/15A9E instalan ese mismo destino. Payload PPC de tres palabras:
0, FFFFFFFF, 0x80B12678. Siguiente: auditar su registro estatico antes de
ampliar la traduccion. Evidencia:
../checkpoints/20261021_prime_frontend_listener_callback_boundary.json.


## Ampliacion del listener: build en validacion

Se auditaron el wrapper text+0x15BE4 y los tres registros contiguos
data+0x4F0/4FC/508, con prefijo (0, FFFFFFFF) y destinos
text+0x154C8/1419C/140F8. Se anadieron como raices originales con
comprobaciones de relocaciones. La clausura aumenta FrontEnd de 211
a 216 funciones; total de los cuatro RSO: 578. No se declaran clases
completas. El build wrapper completo termino sin errores. SHA256 actual:
23a058c6f1ad32a0a911b4f226c97e85e2d197097a71c4773cb32f7abbefae12.
El contrato optimizado conserva siete casos y el control negativo antiguo;
todos los imports emitidos usan ctx explicito. Prueba de A a 65 s en curso.

La regresion del binario previo sin teclado experimental completo 55 s,
228 lecturas ISO verificadas, GPU/FIFO/CP/XFB y texturas/TLUT. La captura
a 50 s conserva el hash del mensaje original de mando desconectado.
Checkpoint: 20261021_prime_popup_callbacks_modeoff_boot.json.
El harness comprueba ahora 1 GiB libre antes de iniciar y escribe JSON
mediante reemplazo atomico. La precondicion negativa de 16 GiB rechaza
la prueba antes de abrir el juego; errores de captura quedan separados
y una imagen vacia no se admite como captura valida.


## Siguiente frontera: tablas pequenas del front end

La ejecucion base_1792616816_pid14396 deja atras el listener pendiente y
se detiene en una llamada DOL a 0x80B10F4C (nominal 0x806A962C),
slot +12 de la tabla 0x80B16A20 del objeto 0x808DC570. Sus 12 bytes
coinciden con el RSO original. Se auditaron seis tablas contiguas de
data+0x1E0 a +0x240: prefijos nulos, once metodos locales y una ranura
original nula en +0x21C. La primera candidata del auditor suponia
equivocadamente doce relocaciones; el assert la rechazo. Se corrigio
contra el RSO comprobado, sin inventar un import ni sustituir la ranura nula.

Las once raices y su clausura llevan FrontEnd a 232 funciones y el total
de cuatro modulos a 594. El build completo termino sin errores y conserva
3.421 relocaciones de FrontEnd verificadas; contrato ctx con control
negativo y siete casos PASS. Binario en validacion:
223611598715d75cefa9b579c0af6cf4fcd7223801f9cf5b067386d48c6b3ffa.
Evidencia: 20261021_prime_frontend_small_tables_boundary.json y
20261021_prime_rso_context_small_tables_contract.json.

El informe final de pid14396 existe, pero el harness fallo al reemplazar
el sidecar: PowerShell pasaba $null como cadena vacia a File.Replace.
La version se preservo en .tools; se corrigio con NullString.Value y se
probo sobre un reemplazo real antes de lanzar otra ejecucion. La prueba
con el harness defectuoso no se declara superada. No es un fallo nuevo
del juego ni un resultado positivo del menu.


## Segunda variante del listener y colocacion real del RSO

base_1792617102_pid5352 confirmo la escritura atomica de informes y
sidecar final, sin errores de captura. La ejecucion del juego fallo
explicitamente en otro wrapper local: 0x80B117B8. El RSO se cargo esta
vez en 0x80AFBA40, por lo que no se puede reutilizar el delta de otras
pruebas. Se obtuvo el delta de las entradas registradas, se contrasto
con dos callbacks conocidos y se verificaron puntero del nodo y primeros
24 bytes originales. Identidad correcta: nominal 0x806AC8B8, text+0x15C58.
Un par HA/LO en text+0x15856/1585E instala este wrapper; su payload apunta
al miembro text+0x154C8 que ya esta traducido. No se forzo la colocacion
ni se atribuyo el cambio de base a una corrupcion.

Se anadio esa raiz, FrontEnd 233 funciones / cuatro RSO 595, sin
instrucciones no soportadas. Build wrapper y contrato optimizado PASS.
Ejecutable en la siguiente prueba:
bb8aeae0a80f7696d2ab0a95dd0347790a040b445ec1b506c57373e6271623dc.
Evidencia: 20261021_prime_frontend_second_listener_boundary.json y
20261021_prime_rso_context_second_listener_contract.json.


## A abre el aviso original de partidas: consulta NAND incompleta

La raiz original de SaveGameInterface (nominal 0x80699440, text+0x27E0)
se verifico mediante el par HA/LO +0x236E/2376 y payload de tres palabras
que apunta al miembro ya traducido text+0x3738. FrontEnd pasa a 234
funciones y cuatro RSO a 596; build y contrato ctx PASS.
base_1792617615_pid16404 completo 110 s sin fatal ni errores de captura.
La imagen a 105 s muestra el aviso original de espacio insuficiente
y las opciones Continue Without Saving / Return to the Wii Menu Screen.
A llega al codigo original en siete muestras verificadas. Todavia no
se afirma acceso al menu principal ni gameplay. Binario:
b266a0238f29795db641ba9101a3e989a72f7d4299cb0d5f80222f593853bf94.
Verificacion completa ISO/GPU en curso. Evidencias:
20261021_prime_save_dialog_visual.json y
20261021_prime_save_dialog_keyboard_input.json.

El runtime heredado devuelve ISFS_OK en IOCTLV GETUSAGE (12) sin escribir
las salidas. La prueba original registra diez llamadas a ese comando antes
del aviso InsufficientSpace. Se verifico el protocolo con la referencia
Dolphin fijada y registrada en 20261021_dolphin_fs_reference.json:
un vector de entrada de 64 bytes y dos salidas de cuatro, clusters
de 16 KiB e inodos. El contenido GPL permanece como investigacion privada,
no se copio codigo al producto. Se preparo un contador independiente
sobre la NAND de Windows que cuenta archivos y directorios reales y
redondea cada archivo a clusters. La prueba nativa verifica un arbol real,
5 clusters / 7 inodos, rutas inexistentes/invalidas y formatos de vectores.
CTest mkw_nand_usage_tests: 1/1 PASS. Aun no esta integrado en el runtime;
no se atribuye todavia el aviso exclusivamente a esa carencia.


## Integracion de GETUSAGE en validacion

El probe del aviso original paso la verificacion completa: 1.707 lecturas
ISO, GPU, FIFO/CP y cuatro RSO. Checkpoint 20261021_prime_save_dialog_boot.json.
Se integro la consulta real de directorios en IOCTLV 12: forma 1 entrada
/ 2 salidas y tamanos 64/4/4, rangos completos, ruta terminada, errores
ENOENT/EINVAL/EIO, dos palabras big endian escritas solo al tener exito.
El contador usa archivos reales, redondeo por archivo a clusters de 16 KiB,
inodos recursivos incluyendo el directorio consultado y limites Wii.
Las entradas host de mas de doce bytes se excluyen de forma coherente con
READDIR. No se siguen enlaces simbolicos; los errores no son exitos vacios.
El perfil Prime devuelve EINVAL ante otros IOCTLV de FS no implementados.
GETSTATS y otras partes heredadas de NAND siguen fuera de esta validacion.

CTest mkw_nand_usage_tests: 1/1 PASS, con arbol real y errores/formato.
Se preservo un inventario de la NAND anterior (27 entradas) para contrastar
las consultas antes de la creacion de partidas. La primera relocalizacion
fallo por un bloqueo temporal del ejecutable; sin procesos WiiCompiled
abiertos, repetir el enlace termino correctamente. No se atribuye a un
error de codigo. Binario de la nueva prueba:
88ff525d7294f372280fa4903140858806770be1a20c98d9e8ccecb8c9a42e85.
Las capturas siguientes determinaran si cambia el aviso; no se fuerza
ningun bit de espacio libre ni se salta la interfaz original.


## GETUSAGE: medicion nativa pasa, integracion aun no

Los probes base_1792618169_pid8332 (110 s) y
base_1792618689_pid5920 (95 s) permanecieron vivos, pero la pantalla
queda negra esperando consultas que devuelven EINVAL. No son progreso
de menu validado. Los descriptores tienen forma y tamanos correctos;
la cadena copiada comienza con cero: path=0x17FC0840, primera palabra
0x00746974, en lugar de /tit. No se acepto la ruta vacia ni se repuso
una barra inventada. Se agrego una observacion optativa al SDK original
0x805013D4, hash de fuente y primera instruccion fijados, para cotejar
entrada, antes y despues de memcpy 0x80004000. Los cuerpos originales
siguen completos, la traza solo lee memoria y escribe logs.
La build diagnostica y su prueba siguen pendientes. Ver
20261021_prime_nand_empty_path_failure.json.


## Causa de la ruta vacia: ES_GETTITLEDIR ausente

La traza optativa de base_1792618923_pid6420 mostro que 0x805013D4
recibe ya vacia la fuente global 0x8060FFE0. La longitud es cero y
memcpy copia correctamente un unico terminador. Los bytes tit restantes
eran memoria anterior del destino; no se perdio una barra de una fuente
valida. La copia original tambien paso 45 casos aislados con solapamiento
y longitud cero. Inicializacion 0x80508DD4 llama a 0x8050983C, que solicita
el directorio por ES ioctlv 29; el runtime lo rechazaba. Evidencia causal:
20261021_prime_nand_empty_source_cause.json.

Se implemento ES_GETTITLEDIR: un vector de titulo de ocho bytes y una
salida SDK de 30, validacion de rangos y ruta de 29 caracteres mas NUL.
El SDK original sigue almacenando la respuesta en su global; no se
reemplaza la inicializacion por un valor forzado. Tambien se corrigio
solo en el perfil Prime la categoria de titulo 00010004 heredada de MKW
a 00010000, comprobada en el ticket de la particion de juego de la ISO
original: 00010000524D3345. Checkpoint de lectura de ticket en solo lectura:
20261021_prime_disc_title_identity.json. Build completo PASS; nuevo
arranque en validacion. No se afirma todavia seleccion/creacion de partidas.


## Directorio nativo provisionado y archivos originales creados

ES_GETTITLEDIR devolvio la ruta correcta en pid14924, pero el directorio
aun no existia y GETUSAGE retornaba ENOENT; no fue un menu valido.
Se agrego al inicio de la NAND del perfil Prime la preparacion del
directorio del titulo que normalmente realiza IOS al lanzarlo.
create_directories conserva todo contenido existente; no crea saves
ficticios. Fallos reales de filesystem se reportan, no se convierten
en exito. base_1792619269_pid18308 verifico la ruta y GETUSAGE inicial
con 0 clusters / 1 inode; luego el propio juego creo banner.bin (61600)
y save.bin (65536) y leyo Mp3_logo_to_Save_slot_loop.thp. No se afirmo
visualizacion de los espacios de partida. Se copio ambos archivos
exactamente a un backup privado antes de otra ejecucion. El inventario
posterior contiene 8 clusters / 3 inodos: no sirve para comparar las
medidas iniciales previas a esas escrituras. La comparacion historica
rechazo esa falta de igualdad temporal y no se declaro PASS.

El fallo posterior fue un callback original de lista en nominal
0x806AA8AC / text+0x13C4C. Se verificaron dos pares HA/LO, puntero del
nodo, payload a text+0x13EA0 y dos registros estaticos de tres palabras.
La clausura aumenta FrontEnd a 237 y cuatro RSO a 599. Build y contrato
ctx PASS. Ejecutable de la siguiente prueba de lectura de los archivos:
dd93513b90d00e397040273a09d5b7bcc5705778c4d801470735993ad09c461d.
Evidencia: 20261021_prime_nand_progress_early_listener_boundary.json y
20261021_prime_original_initial_save_files.json.


## Game Select original visible y lectura de archivos conservada

base_1792619409_pid3240 completo 100 segundos sin fatal ni errores de
captura. A al segundo 65 lleva del titulo a Game Select; las capturas
de 80 y 95 muestran tres espacios New Game y controles Copy Slot,
Erase Slot y B Back. La traza abre el save.bin original desde la ruta
corregida. banner.bin/save.bin conservan exactamente sus hashes del
backup anterior. No se afirma validez completa del formato, guardado
de gameplay ni activacion del primer espacio. Verificacion de siete
muestras de teclado PASS; verificacion completa ISO/GPU en curso.
Evidencia: 20261021_prime_game_select_visual.json y
20261021_prime_game_select_keyboard_input.json.
Siguiente: seleccionar primer espacio con apuntado/A originales y
verificar el flujo posterior sin forzar flags ni reemplazar pantallas.


## Primer espacio activado: Mii Select y frontera de switch

La repeticion base_1792619617_pid10084 muestra Game Select a los 80 s.
El ejercicio separado apunta al primer espacio (cliente 320,190) y
entrega cinco mensajes dirigidos al PID/exe comprobados, restaurando
foco y cursor. La captura de 90 s muestra Mii Select. La imagen del Mii
contiene ruido: no se declaro esa pantalla graficamente correcta.
Mas tarde el juego construye WC24OtherFatalError y falla en un salto
local 0x80198330 dentro de 0x8019804C. La tabla original de 13 entradas
en 0x80594610 tiene seis destinos unicos; la traduccion antigua solo
emitia uno. El traductor actual recupera seis casos, incluido el faltante,
285 instrucciones sin unsupported. Se cotejaron los 52 bytes de tabla y
los 0x474 bytes del cuerpo en RAM guest con el DOL original. La reparacion
aun no esta integrada: se espera la verificacion completa pendiente del
probe Game Select antes de cambiar el ejecutable o sus fuentes.

El save.bin cambio 1652 bytes durante la interaccion, sin cambiar tamano.
Se preservo el backup anterior y otra copia privada posterior; no se
considera un guardado completado o relectura valida de ese nuevo estado.
No se parchearon returns, flags, slots ni la tabla de saltos original.
Evidencia: 20261021_prime_first_slot_mii_switch_boundary.json y
20261021_prime_first_slot_save_change.json.


La verificacion completa de Game Select termino: 2.444 lecturas ISO,
GPU/robustness y cuatro modulos/callbacks PASS, ademas de CP/FIFO/XFB.
Checkpoint 20261021_prime_game_select_boot.json. Se integro despues
la regeneracion de 0x8019804C al build reproducible: audita los trece
valores originales y seis casos unicos. El bloque anterior 0x80420444
sigue auditado de forma independiente (once casos). El DOL y sus tablas
originales permanecen intactos; solo se compila otra traduccion del cuerpo.
La nueva build esta en curso y aun no se verifico el aviso WC24 posterior.


La build con el segundo switch termino: manifest verifica trece entradas
y seis casos para 0x8019804C. Nuevo ejecutable en prueba:
ea47046d774f6a925c8c36f0d69e16f40320ef2e3e3af53dc0027a47a926749a.
Los archivos originales de Miis RFL_Res.dat se leen del disco, pero
RFL_DB.dat no existe en la NAND privada. No se ha probado que esa ausencia
explique el ruido de la imagen. Ghidra MCP no esta disponible; la auditoria
del switch usa bytes del DOL, tabla original, RAM y traductor comprobado.


## Un Mii local solicitado por el usuario

El probe pid8840 quedo interrumpido sin runtime_probe.json ni receipt del
segundo click; no prueba el switch reparado. El nuevo objetivo inmediato es
un perfil persistente Jugador, validado por RFL original y despues por
guardado/recarga. Se creo RFL_DB.dat solo donde no existia, con apariencia
leida del DOL propio, CRC contrastado y una identidad normal local estable.
Cinco pruebas offline PASS; backup privado de las partidas anterior al ensayo.
El probe pid11156 confirma que 0x8054A6D8 acepta indice 0 y rechaza 1..99;
los 74 bytes devueltos en MEM2 son iguales al archivo. El ensayo completo
falla luego en un viewport WebGPU casi cero/negativo al generar graficos.
No se declara retrato correcto, seleccion ni guardado completado.
Ver recomp/docs/miis.md y checkpoints single_mii_* del 21 de octubre.


Mii follow-up: el FIFO de CPU ahora graba bloques BE de 32 bytes en RAM y
solo entrega los enlazados al GP. La primera variante rechazo incorrectamente
un bloque parcial al restaurar PI; otra prueba de la ruta completa expuso
que la semilla de arena baja 0x806843C0 se solapaba con la pila principal.
El DOL en 0x804DE16C contiene lis/addi para __ArenaLo=0x8069C3C0; se corrigio
la semilla nativa, conservando el codigo guest. El probe pid2356 supera el
viewport corrupto y muestra Game Select con retratos, todavia sin afirmar
fidelidad del retrato. Falla mas tarde en listener FrontEnd text+0x9F2C
al dibujar un popup original. Prefijo de 32 bytes y ocho referencias HA/LO
verificados; se integra su traduccion. Seleccion/guardado de Jugador y
recarga siguen pendientes. El save actual contiene su CreateID en offset 35156; la asociacion y recarga
se deben verificar, no se deducen de esa aparicion.


Continuidad: FrontEnd 249 funciones (611 entre cuatro RSO), con nueve
funciones mas del cierre de las tres tablas de limpieza. El probe pid10920
supera esa frontera y entra en preparacion de recursos de CPlayer; falla
escribiendo 0x98000000 desde el recorrido de lectura/descompresion. El
registro del stream 0x80B43A64 muestra cursor 0x6F602 frente a tramo 0x180.
La causa todavia no esta probada; no se aumenta memoria ni se omite el
recurso. Checkpoint 20261021_prime_ingame_resource_decode_boundary.json.
Se prepara observacion sin mutar guest antes de la lectura y del decode.


Seguimiento del fallo de recursos: fd10 era /tmp/26226D27.pak, copia
derivada de InGameNAND.pak. La cache mide 131072 bytes y coincide exactamente
con el prefijo del original de 335872; las otras cinco paks en tmp coinciden
enteras con sus originales. La ultima lectura fd10 offset148736 devolvio0,
explicando que el buffer conservara datos anteriores. Se preservo copia
truncada en .tools/26226D27.pak.truncated_20261021 y se retiro solo esa
cache derivada para regeneracion por el juego. No se modifico la ISO ni saves.
La causa historica de la truncacion no se afirma. Nueva prueba en curso.


La cache InGameNAND.pak fue regenerada por el juego y coincide en sus
335872 bytes con el original propio. Probe pid17432 supera la lectura
fallida y llega al prolog de ScriptAchievement (0x80C3BD7C), aun sin
registro. Se verificaron los 5064 bytes de text en RAM enmascarando solo
los campos de relocacion enumerados; el prefijo literal no coincide
por dos llamadas reubicadas y no se trato como corrupcion. Se incorpora
modulo 5 con prolog/epilog/unresolved y constructores, hashes RSO/REL
fijados y verificacion independiente de relocaciones en build.


Build modulo ScriptAchievement PASS: 10 funciones del cierre de arranque
sin unsupported, 213 relocaciones verificadas y registro nativo integrado.
Contratos de contexto pasan para los cinco modulos, total 621 funciones.
La ejecucion nueva aun se esta observando; no se declara modulo completado
ni gameplay. Se corrigio tambien el gate auxiliar de timestamps: objetos
validos de mas de un dia ya no se rechazan por edad; la conversion Ninja
se contrasta con dos registros independientes (PCH y objeto).


El verificador de boot ahora contrasta las seis caches NAND identificadas
que esten presentes, no solo MiscData.pak. Asi un archivo parcial se
rechaza antes de aceptar evidencia de recursos. Sintaxis Python comprobada;
se espera el cierre de la nueva prueba antes del gate integral.


Probe base_1792632374_pid6660: 100 s vivo, sin fatal, Game Select original
visible al segundo95. Dos aperturas de save.bin y repetidas aceptaciones
de indice0 con CreateID 89917cbd904d2813; el perfil persiste tras reinicio.
No se encontro bind de ScriptAchievement en este ensayo, de modo que su
registro/build PASS no demuestra ejecucion. Asociacion de slots y gameplay
siguen sin validar. La verificacion integral ISO/GPU esta en curso; no
se registra como PASS antes de que termine.


Verificacion integral pid6660 terminada PASS: 2410 lecturas contra ISO,
readback guest, seis caches completas, cuatro binds/fabricas/callbacks
observados, GPU validation/robustness, CP/FIFO/XFB y persistencia EFB.
Checkpoint 20261021_prime_mii_cache_regenerated_boot.json. Visual Game Select
se registra aparte; no prueba gameplay, asociacion completa Mii/slot ni
ejecucion de ScriptAchievement en la nueva build. Siguiente: seleccion
determinista de slot/Mii y relectura de la asociacion, despues modulo5.


Probe pid14804 confirma bind/prolog ScriptAchievement, 213 relocaciones
y hashes de secciones. Luego el DOL 0x801DEC1C lee tabla en 0x80C3E200
y hace tail-call a 0x80C3C17C: fabrica original text+0xE44 instalada
por HA/LO en offsets2/10, no un switch local. Se agrego esa raiz y su
cierre: 12 funciones del modulo, build en curso. La prueba propuesta
como controlada recibio cinco pulsaciones A por entrada host antes de
nuestro click (no se emitio receipt sintetico); no se afirma una
secuencia determinista. Checkpoints achievement_factory_boundary y
slot_input_provenance registran estos limites.


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


## 22 de octubre: lote de tablas de audio

La compilacion anterior no dejo un resultado verificable tras perderse la
sesion. Se reejecuto el build completo, con log persistente en
`recomp/.tools/prime_audio_batch_build_20261022.log`, y termino con exit 0.
El ejecutable SHA256 es
`8c949c87c34947b2d9b8d5a0ba6d41393993c23fe79a586224c683e581478c4b`.
Se integraron nueve funciones mas con dieciseis tablas de once entradas,
176 destinos, tras localizar cada tabla en el DOL original y verificar la
referencia de direccion y los labels locales del candidato. Los nueve
fuentes regenerados coinciden con los auditados y los overlays compilados.
Checkpoint: `20261022_prime_audio_scaled_switch_build.json`.

La auditoria reviso 52 funciones de esa zona con saltos indirectos. Los
candidatos sin aumento de destinos no se integraron como correcciones.
La auditoria independiente y sus limites estan en
`20261021_prime_audio_scaled_switch_audit.json`; no demuestra ejecucion
nativa de todas esas ramas ni fidelidad completa de audio.
Las cinco tablas de 0x8042B530 y los dos cuerpos de tres tablas
0x80423190/0x80423EF8 tambien tienen build y tablas originales verificadas.
El siguiente ensayo comprueba el recorrido posterior a Ship Log.
El matching y la ISO no se modificaron.


## 22 de octubre: copia de color a mitad de resolucion

La build del lote de audio llego a un bloqueo grafico distinto: el probe
base_1792645530_pid3188 registra Unsupported Prime raw color texture-copy mode.
GXData original en el volcado conserva BP 0x49=0x00B000, 0x4A=0x059E7F,
0x4D=0xA0 y 0x52=0x010263: fuente 640x360 en (0,44), RGBA8 con bit de
reduccion 2:1, destino 320x180, stride 5120 y reserva 230400 bytes.
El SDK original GXSetTexCopyDst mueve su argumento mipmap al bit 9; Aurora
ya dispone de filtrado de esa copia. El adaptador ahora conserva dimensiones
de fuente y destino por separado y pasa el indicador original a esa ruta.

La prueba nativa mkw_prime_raw_bp_tests pasa 1/1: snapshot original, copias
previas sin reduccion en RGB565/RGB5A3/RGBA8, tiles pequenos, clear y rechazo
de stride incorrecto, fuentes impares/degeneradas y modos no comprobados.
Build C++ PASS, SHA256 ed80b4d0baff76d4c574eab3a4b0b4c679496028418feca99ef62bf9312e542a.
Prueba GPU en curso; no se declara paridad del filtrado con Wii.
Evidencia: 20261022_prime_half_scale_copy_contract.json.
Referencia de formato consultada: Dolphin cf5cb7ef81c3e8e7af5c9d566334219ffda9f68b,
BPMemory.h y TextureCacheBase.cpp; investigacion de formato, sin copiar su
implementacion GPL al adaptador.


## 22 de octubre: copias ejecutadas, investigacion de texturas raw

La ejecucion pid14448 confirma copias 2:1 de RGBA8 640x360 -> 320x180
y RGB565 640x448 -> 320x224. Sin embargo, muestra un rectangulo azul y
falla despues con D3D12 CreateHeap al crear GX Static Texture. Resultado
FAIL, no escena jugable verificada. La resolucion interna sigue en 1x;
no se atribuye la causa a falta de potencia sin evidencia adicional.
Checkpoint 20261022_prime_half_scale_gpu_allocation_boundary.json.

Un observador de asignaciones registra 4096 subidas (limite de la traza),
442 MiB de fuentes acumuladas y caches nativas vacias. La ruta raw marca
no_cache y recrea texturas iguales repetidamente. Ese segundo ensayo acaba
con excepcion Windows 0xC0000005; no se establece que ambos fallos tengan
una causa unica. Se implemento una reutilizacion acotada para fuentes raw
sin paleta: compara todos los bytes en cada consulta, conserva como maximo
64 entradas y 16 MiB de snapshots; un cambio en bytes o metadata reemplaza
la entrada y los dibujos pendientes conservan su propia referencia GPU.
Paletas no se cambian. Pruebas de mutacion en extremos, cambio de metadata,
reemplazo, LRU, limite de bytes y lifetime: PASS. Junto con BP: 2/2.
Build PASS; prueba GPU pendiente. Checkpoint
20261022_prime_raw_texture_snapshot_contract.json. Fidelidad y progreso
jugable permanecen pendientes.


El ensayo de la cach? de snapshots pid2840 termino 180 segundos sin fatal,
con titulo visible. No reprodujo el tramo grafico de la partida, por lo
que no prueba corregidos el OOM, el native crash ni el rectangulo azul.
Checkpoint 20261022_prime_raw_snapshot_bounded_title.json. Verificacion
integral de ISO/GPU en curso para ese ejecutable; no modificar sus fuentes
ni recompilar hasta terminar el gate. Se revalidaron SHA1 del DOL matching
3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2 y SHA256 del input original
6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104.
El helper de entrada escala desde coordenadas de referencia 854x480 al
client real y registra ambos puntos/tamanos; evita elegir otra fila cuando
la ventana cambia de tamano. Mensajes enviados no prueban seleccion.


## Peticion de autonomia y duracion, 22 de octubre

El usuario autoriza navegar menus y enviar botones/palanca/puntero de forma
autonoma mientras descansa. Hito solicitado: alcanzar una parte que requiera
acciones reales de control y mantener un escenario real al menos 30 minutos
sin errores; medir rendimiento y mejorar los costes demostrados. Un menu
quieto, ventana abierta o cinematicas por si solas no satisfacen ese hito.

La ultima ejecucion pid16240 mostro geometria 3D de la escena inicial y
copias 2:1, pero termino en el prologo sin traducir de ContextSensitiveActivator.
Su modulo original paso 199 relocaciones y secciones completas en RAM.
La traduccion privada cubre arranque, fabrica y los once metodos locales de
la tabla observada de treinta (diecinueve importados), 26 funciones sin
opcodes sin soporte. Sigue fuera del build hasta cerrar el gate previo.

Se preparo una entrada de desarrollo por comandos de mando, con secuencia
monotona, validacion de rangos y acciones de hasta cinco segundos; neutral
automatico al expirar. Prueba nativa PASS de A, palanca, expiracion y rechazo
de comandos invalidos. Aun no integrada ni verificada en el juego. El
objetivo es inyectar solo muestras de control por el puente KPAD existente,
sin escribir estados del menu, teletransportar al personaje ni omitir codigo.
Cada accion debe quedar asociada a PID, hash y recibo; confirmar aceptacion
en runtime y respuesta visible. El nuevo hito de 30 minutos sigue pendiente.


## Autonomia integrada y prueba activa, 22 de octubre

ContextSensitiveActivator ya integrado: 26 funciones, 199 relocaciones; total 659
funciones entre seis RSO parciales. Build/import contract PASS. Entrada KPAD
autonoma y cache/raw BP: 3/3 pruebas nativas PASS. Gate integral del ejecutable
anterior b59c8b25... PASS con 5.864 lecturas ISO verificadas; ese gate no se
atribuye automaticamente al nuevo ejecutable 12896330....

PID2744 (inicio UTC 05:58:46), probe de 2.400 segundos: capturas y recibos
confirman Game Select, tercer slot, aviso WC24 109042, Main Menu, Play Game,
Normal y secuencia Life-support. Es necesario hover antes del trigger A. El
helper distingue carpetas antiguas cuando Windows reutiliza un PID, usando
CreationTimeUtc y hora de inicio del proceso ademas del hash. La secuencia
se ejecuta mediante muestras de mando, sin escribir estados del juego.

La duracion jugable de 30 minutos sigue pendiente: este probe incluye menus
e introduccion. Las tasas presentadas observadas en menus/carga varian
aproximadamente entre 33 y 60 FPS; no constituyen una medicion de gameplay.
Recibos privados y capturas: recomp/.tools/prime_pid2744_*.


ScriptGrapplePoint integrado: 50 funciones, 417 relocaciones independientes
verificadas y secciones originales del volcado coherentes. Total 709 funciones
en siete RSO parciales; no son clases recuperadas manualmente. Build completo
enlazo; comprobacion incremental CMake exit 0. Tres pruebas nativas PASS;
contrato optimizado de imports incluye los siete modulos y control negativo.
PID2744 termino en su prologo sin traducir; checkpoint de navegacion autonoma
con nueve comandos aceptados y capturas hasta Life-support. PID16760 ejecuta
el build e58cbb77... y repite la ruta para verificar el siguiente limite.
No se ha alcanzado aun el hito de movimiento y treinta minutos de gameplay.


ScriptGrapplePoint paso el enlace real y hash de secciones en PID16760;
el siguiente fallo es el prologo de ScriptMultiModelActor 0x80FC9AFC.
MultiModelActor paso 488 relocaciones y secciones enlazadas del volcado;
47 cuerpos sin solapamientos integrados. La candidata inicial de 57 fue
rechazada: diez destinos pertenecen a una tabla de salto, no son funciones.
Se verificaron comparacion limite 9, referencias HA/LO data+0 y secuencia
lwzx/mtctr/bctr antes de excluirlos como raices independientes. Build exit 0,
contrato de imports PASS. Total 756 funciones en ocho modulos parciales.
PID16760 mostro escena 3D defectuosa con ultimas muestras de 9-15 FPS;
validacion GPU y trazas estaban activas, no hay causa de lentitud aislada.
El hito de control/movimiento y 30 minutos reales permanece pendiente.


ContextSensitiveAction integrado: 34 cuerpos sin solapamientos, 300
relocaciones independientes y build exit 0; total 790 funciones/nueve RSO
parciales. Su verificacion de ejecucion esta en curso con PID20012.
La auditoria de imports ya usa translation.json y hashes por fuente:
archivos archivados de candidatas rechazadas no cuentan como fuentes del
build. El control negativo y las nueve listas activas pasan en
20261022_prime_context_action_import_contract.json. Esto corrige el fallo
de conteo del primer intento de auditoria MultiModelActor, sin borrar
los archivos rechazados ni alterar su evidencia.


ContextSensitiveAction paso enlace real en PID20012 (34 funciones/300
relocaciones); el siguiente prologo faltante fue ButtonPressing 0x80FD73F8.
ButtonPressing paso 253 relocaciones y secciones del volcado, 33 cuerpos
sin solapamiento y build completo exit 0. Total 823 funciones en diez RSO
parciales. Contrato de imports de diez listas y control negativo PASS.
Ejecutable actual 14fad3cbd9e2bb82b4dec4f36efebbbec7e36151f1b927147dd6e2210ea61af9.
Proximo: verificar este enlace y llegar a escena controlable; treinta
minutos de juego real siguen pendientes. No se afirma mejora de FPS.


## QA manual por peticion del usuario, 22 de octubre

El usuario regreso y asume el QA. Se retiro la inyeccion de botones por
archivo en KPAD, sus headers/test/target, senders de comandos, clicks
Windows y job de teclado del probe. Se conserva el control manual SDL
y la evidencia historica pasiva. No ejecutar navegacion automatica.
Ejecutable recompilado y enlazado; input_expr PASS. Iniciar mediante
recomp/tools/run_prime_keyboard_mouse.ps1 para activar teclado/raton.
Enter/clic izquierdo = A; Espacio/clic derecho = B; WASD = palanca;
Shift = Z; Ctrl = C; raton = puntero.
La prueba de 40 minutos fue solo de titulo/demostracion: no acredita
30 minutos jugables. ButtonPressing sigue pendiente de verificacion
en ejecucion. Checkpoint 20261022_manual_qa_automation_removed.json.


QA manual PID20476: Windows registro AppHangB1 (eventos Application
1001/1002), sin fatal del runtime. Los diez RSO enlazaron y verificaron
secciones, incluido ButtonPressing (253 relocaciones/33 cuerpos). La
ultima pelicula leida fue Diff_Settings_To_Game; el final del log contiene
IPC IOS/WiiConnect24, sin que eso establezca la causa. El proceso ya se
cerro. Se preparo capture_prime_hang.py para registros y pilas nativas
con suspension breve y reanudacion garantizada por finally; prueba contra
proceso sintetico privado PASS. No hay inyeccion de entrada ni cambio de
estado del juego. Pendiente volcado del proceso vivo durante reproduccion
manual. Checkpoint 20261022_manual_cinematic_apphang.json.


El lanzador manual run_prime_keyboard_mouse.ps1 incorpora un observador
pasivo separado watch_prime_hang.ps1. Tras 15 segundos de ventana sin
respuesta intenta guardar una captura nativa en recomp/.tools/. No pulsa
controles ni termina/reinicia el juego. -NoHangCapture permite desactivarlo.
El muestreo pausa brevemente cada hilo y lo reanuda en finally; resuelve
simbolos con base ASLR y etiqueta palabras de pila como candidatas, sin
afirmar unwind completo. Pruebas de fixture de captura, simbolizacion y
salida del observador tras terminar su proceso PASS; captura del bloqueo
real y correccion siguen pendientes. No se recompilo el juego por este
cambio de herramientas. Checkpoint 20261022_passive_hang_observer.json.


QA PID11212 supero el enlace de los diez RSO y fallo en callback dinamico
de GrapplePoint: runtime 0x80F8E634, text+0x1DBC, nominal 0x80692C1C;
caller LR 0x801AF200. Las 417 relocaciones y secciones del modulo en RAM
coinciden. Su direccion se construye con HA/LO +0x1AAA/+0x1AB2 y se guarda
en un registro temporal de callback/contexto. Se anadio la raiz verificada:
51 cuerpos sin solapamientos para este modulo, 824 en total. Traduccion,
imports, enlace completo y comprobacion incremental CMake exit 0 PASS.
Ejecutable 33893acd7d28e7590a1f1219146be4f77ec0f83c09ad336e988cf430e789493b.
Pendiente volver a pasar por la invocacion en QA manual; no se declaran
resueltos todos los bloqueos ni 30 minutos jugables. Evidencia en
20261022_prime_grapple_callback_fix.json. Sin automatizacion de botones.


## Prioridad: estabilidad del inicio de partida (peticion del 22 de octubre)

Titulo y menus ya alcanzados. PID10244 avanzo despues del callback de
GrapplePoint y fallo en HBMShutdown, runtime 0x9016D900, nominal 0x806A7540,
text+0xEF60 del RSO rhbm. Caller LR 0x800CD444. Los 4.238 campos reubicados
y todo el texto coinciden; 34 bytes de datos difieren tras inicializacion:
no se afirma identidad de toda la imagen en ese punto.

Se tradujo la API HOME de ocho exports y se cerro el conjunto de direcciones
de codigo construidas por HA/LO dentro de cuerpos ya traducidos, con
referencias y palabras de entrada explicitas verificadas contra originales.
No se convierten tablas de salto ni palabras arbitrarias en funciones.
Home pasa de 19 a 196 cuerpos; hay tambien callbacks nuevos en Achievement,
WorldTeleporter, ContextSensitiveAction y ButtonPressing. Total 1.010
funciones en diez RSO parciales. No equivale a clases completas ni cobertura
indirecta completa: tablas de datos y codigo no ejecutado conservan limites.

Build completo exit 0; fuente seleccionada/hash, disjuncion de instrucciones,
relocaciones independientes y contrato optimizado de imports con negativo
PASS. audit_prime_rso_address_taken.py se integra en el build y detecta
entradas materializadas que faltan. ISO solo lectura y DOL matching SHA1
3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2 revalidados. Ejecutable
994bcc1eee1a2cc49a82fc5405777ac146b5d0ce4d57f119912bda55114b320e.
Se abre para QA manual con observador pasivo. No se restaura automatizacion
de botones. Pendiente verificar descarga HOME/transicion y estabilidad real;
30 minutos jugables aun NO demostrados. Evidencia:
20261022_prime_home_lifecycle_callback_fix.json,
20261022_prime_address_taken_coverage.json e import_contract.json del lote.


PID16060 descargo HOME por epilogo original y quedo sin respuesta. Se
capturaron hilos vivos: principal activo en asignacion retry 0x803F486C,
liberacion diferida 0x803F4A5C y consulta de token PE 0x803EC2C8/0x803EC0FC.
Ghidra del DOL fijado confirma emision BP48/BP47 en 0x804D2584 y lectura
PE+0x0E en 0x804D2638. El backend no publicaba ese token. Se publica ahora
el low16 de BP47 despues del drenaje productor existente de GXApplyBPReg;
no se parchea el asignador ni se fuerza su retorno. Build PASS y raw BP/
snapshot 2/2 PASS, que no prueban el token end-to-end. Sin nuevo IRQ de
token; pendiente validar reutilizacion/fidelidad y progreso en QA manual.
Checkpoint 20261022_prime_pe_token_progress.json. Nueva ejecucion abierta.


Ensayo del token PID9008: log confirma tokens 30,31,32 consumidos;
ventana responde. Captura nativa muestra hilo principal procesando
GXSetVtxDesc/write_numeric_component, no detenido en el retry anterior.
Capturas consecutivas pasan de azul a una toma 3D nueva de la camara.
Evidencia de avance de cinematica, no prueba de gameplay controlable ni
30 minutos estables. Defectos graficos siguen visibles. Se actualizo
20261022_prime_pe_token_progress.json con log/capturas del ensayo.


PID9008 avanzo hasta la toma de Samus y paro explicitamente en copia
EFB I4 no soportada: BP52=0x10A03, PE=1, source_wh=0x3FCFF, stride=0x10.
Se acepta GX_TF_I4 usando el conversor de intensidad cuantizada de Aurora
existente; tiles reales 8x8/32 bytes. Fuente 256x256 -> destino 128x128
por half-scale, 8.192 bytes. Prueba con snapshot original, tile 1x1 y
rechazo de stride duplicado PASS, build exit 0. Ejecutable
e534cdb9f129ab06883d5cecb06bb58ed9d34b1346c95e3b8d9e4e7be4437e97.
No se declara paridad visual ni estabilidad; pendiente QA de ese tramo.
Checkpoint 20261022_prime_i4_cinematic_copy.json.


Correccion de interpretacion de formato: al contrastar GXSetTexCopyDst
0x804D3184, BP bit15=0 selecciona copia por canal: nibble0 es R4 (0x20),
no I4. La primera ampliacion evito el rechazo pero eligio incorrectamente
intensidad; no se considera paridad. Se corrige el hostFormat a R4.
PID18032 avanzo y fallo despues con trigger 0x10873: A8 (0x27), fuente
72x58 desde x16/y172, stride9, 4320 bytes. Se soporta con conversores
R4/A8 existentes en Aurora; pruebas exactas de ambos snapshots PASS.
Copia R4 8x8/32B, A8 8x4/32B. Build y retest documentados en el
checkpoint 20261022_prime_channel_copy_modes.json.


## 22 de octubre: primera escena y perfil de rendimiento

El fallo manual PID 5756, 0x811D725C / LR 0x8019F8CC, corresponde al prolog original de Friendly. Se integraron Friendly (158 funciones) y GunTurret (125), con relocaciones independientes, fuentes sin solapamientos y auditoria de referencias materializadas PASS: 12 RSO parciales / 1.293 funciones. El nuevo build pasa la prueba de stb reubicado y CTest BP/snapshots/presupuesto de trazas 3/3. No equivale a recuperar clases completas ni a validar toda llamada indirecta.

Se limitaron las tres trazas que producian 159.110 de 190.738 lineas del log anterior, conservando errores y modo verbose; se evita calcular paletas si no hay texturas indexadas. PID 20496 ejecuta QA manual con tiempos CPU y presentacion. Primeras pantallas aproximadamente 60 FPS, sin demostrar aun FPS de gameplay ni enlace vivo de ambos modulos nuevos. Siguen pendientes los defectos graficos y 30 minutos jugables. Detalles y limites: recomp/docs/20261022_first_scene_performance.md; evidencia recomp/checkpoints/20261022_first_scene_modules_and_performance.json. ISO en solo lectura, DOL original con SHA256 intacto, matching sin cambios.


## 22 de octubre: presion de texturas, vertices y copia A8 del borde

El PID 20496 enlazo Friendly y GunTurret con sus relocaciones/secciones verificadas y llego al holograma de la primera zona. Fallo creando GX Static Palette Texture por falta de memoria D3D12. El perfil de cinem?tica registro 95/220 muestras en conversion CMPR/comprobacion de mips; el cache de 64 entradas expulsaba texturas con solo 4-5 MiB residentes de 16 MiB permitidos. Se aumento a 512 entradas manteniendo ese presupuesto y se sustituyo la promocion vectorial por splice LRU. La ruta raw de paletas ahora conserva snapshots de indices y TLUT con presupuesto separado de 8 MiB / 256 entradas. Ambos buffers se comparan completos; un hash de paleta solo filtra candidatos. Tambien conserva versiones A/B/A, evitando reemplazar repetidamente contenidos alternantes en la misma direccion.

El PID 12876 mejoro la reutilizacion y se mantuvo cerca de 1,55 GiB privados en el tramo observado, pero siguio a 8-12 presentaciones/s y fallo en una copia A8 de (638,152), 8x70. Su destino sigue teniendo 576 bytes. El adaptador admite esa cola horizontal de hasta siete columnas, conserva el extent de muestreo y recorta solo el rectangulo de borrado; el sampler usa clamp-to-edge para las columnas fuera del EFB. Esas columnas no tienen valores originales de hardware verificados; no se declara fidelidad exacta del bus fuera de rango. No se desactivo la comprobacion general de limites. Se consulto TextureCacheBase.cpp de Dolphin como referencia, sin copiar codigo: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureCacheBase.cpp.

El siguiente perfil encontro coste en expansion/publicacion de vertices. La nueva ruta copia los componentes originales big-endian desde arrays, incluidos indices 8/16, color empaquetado, matriz y normales NBT3; no hace conversion float de ida y vuelta. Publica el VAT directo y usa GXBegin, append binario al FIFO con comprobacion de capacidad y GXEnd. Los layouts o rangos rechazados conservan el interprete anterior. Cuatro pruebas nativas pasan: BP, snapshots/paletas, limite de trazas y expansion de vertices (incluidos NaN/negative-zero y limites). Se rechazo y sustituyo un experimento de envio directo por dibujo que produjo ~1,85 FPS: no debe usarse como mejora. Tambien se descarto antes de ejecutar una llamada directa al helper que fuerza crecimiento del FIFO; la version final usa su escritor normal con comprobacion de capacidad.

Ejecutable vigente SHA256 1890cdcdca3e501ffddeda9173fb998534fe07c4164da9ac88530e2e61eabaf2, PID 2252, log base_1792696295_pid2252. Primeras pantallas registraron ~49-60 presentaciones/s y 298.220 hits de 298.239 consultas de paletas, con 19 entradas. Falta comprobar la escena y la copia de borde con esta version; no hay aun 30 minutos jugables ni velocidad uniforme verificados. La configuracion Prime espera shaders en vez de omitir dibujos. La revision de tiempo encontro el paso original 1/60 en 0x806795F8 y el helper OSGetTime de 0x802EBEC8; no se parchearon reloj, timestep ni cinematicas. Temporales del compilador dirigidos a G:, compilacion con un proceso despues de un intento paralelo fallido por memoria. ISO y matching sin cambios, sin botones automatizados.

Evidencia: recomp/checkpoints/20261022_texture_cache_pressure_fix.json y recomp/checkpoints/20261022_packed_dl_and_a8_edge.json; perfiles privados en recomp/.tools/prime_pid*_performance*.json.

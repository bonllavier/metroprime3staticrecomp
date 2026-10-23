# Arranque IOS, DSP, hilos e ISO: checkpoints del 20 de octubre de 2026

Estado posterior: HOME ya ejecuta su prologo y HBMInit; el limite pasa a Tweaks. Ver [checkpoint del 21 de octubre](20261021_rso_runtime_progress.md). Los apartados siguientes conservan evidencia historica.

Estado vigente: el ejecutable supera WUD, recibe el primer IRQ DSP y ejecuta
dos hilos originales con pilas host separadas. Verifica 17 transferencias desde
la particion de la ISO y se detiene en el prolog aun sin traducir de
`RSO_rhbm.rso` (HOME). No hay pantalla ni audio del juego verificados.
El ultimo apartado y su checkpoint sustituyen los bloqueos historicos de abajo.

## Checkpoint inicial IOS/HCI

El ejecutable RM3E01_00 avanza desde OSInit, completa operaciones DI y lee un
recurso original. El probe de ocho segundos completa diez comandos HCI iniciales,
sin errores MMIO ni operaciones OH1 rechazadas. No hay pantalla del juego verificada.
El harness detuvo el proceso al cumplir su plazo.

## Correcciones verificadas

- La respuesta IOS debe convertir +0 en IPC_REPLY (8), conservar el resultado en
  +4 y escribir el comando original en +8. El descriptor 7 de `/dev/di` se estaba
  interpretando como IOCTLV, causando un recorrido de memoria como vectores.
  Una cola de completados conserva las respuestas hasta que se reconoce Y1;
  un evento Bluetooth ya no sobrescribe otra respuesta en ARMMSG.
- IPC usa la tabla Prime 0x8067E4D0 y recibe un OSContext valido. Los handlers
  ejecutan con EE enmascarado y registros privados; el scheduler se difiere
  hasta abandonar el callback. Los errores de memoria se propagan.
- La espera idle 0x804E6D20 atiende IPC/VI. El bucle BTA en 0x8050D558 atiende
  el decrementer cuando vence la cabecera de la cola 0x8067E470. Se ejecutan el
  servicio original 0x804DEDFC y su callback 0x8050D488, sin forzar el estado BTA.
- rfi restaura MSR para la continuacion del mismo hilo host. La transferencia
  entre pilas host de hilos distintos sigue pendiente de validar.
- DVDLowInquiry (0x12) devuelve identificacion de lector Wii. DVDLowRead (0x71)
  convierte posiciones en palabras a bytes y lee la extension FST exacta.
  Rechaza rangos sin archivo de respaldo. El recurso observado es
  `/rhbm/homeBtnIcon.tpl`: 0x18C0 bytes, offset cero, FNV-1a `4a9109fe0c8a5ba7`,
  contrastado con el archivo de investigacion. Es infraestructura HOME de Wii;
  no demuestra la pantalla de titulo de Metroid.
- OH1 mantiene pendientes los endpoints 0x81 (eventos) y 0x82 (ACL). El adaptador
  virtual implementa Reset, Read Buffer Size, Write Class of Device, Write Local
  Name, Write PIN Type, Host Buffer Size, Read Local Version, Read BD_ADDR,
  Read Local Features y Write Inquiry Mode. Las escrituras conservan su estado.
  No hay mandos emparejados ni conexiones ACL. Version/features representan un
  controlador compatible con Wii; su direccion local no es la del Bluetooth del PC.
- Los eventos se registran leyendo de vuelta el buffer guest. El verificador
  comprueba longitudes, Command Complete, opcode, estado y campos de buffers.
- GPIOB_OUT/DIR admiten los alias CD0000C0/C4 y CD8000C0/C4 en el mismo latch.
  El arranque WUD accede al bit SENSOR_BAR 0x100. Esto no conecta una barra IR fisica.
- Una direccion sin RAM o dispositivo ahora falla estrictamente en RM3E01_00;
  ya no crea bloques adicionales con ceros para absorber punteros invalidos.

## Reproduccion y pruebas

Las fuentes originales de traduccion y su metadata siguen intactas en `generated`.
`prepare_prime_runtime_hooks.py` verifica DOL, hashes y anclas de instrucciones,
y genera cinco overlays bajo `runtime_hooks`. El wrapper usa esa metadata
derivada; emitir solo desde la metadata original descarta las adaptaciones.

Desde la raiz del proyecto:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/build_prime_runtime.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/probe_prime_runtime.ps1 -Seconds 15
python -B recomp/tools/verify_prime_runtime_boot_probe.py --probe <directorio-de-esa-ejecucion>/runtime_probe.json
```

El harness asocia el log con el PID lanzado, registra hashes de ejecutable/log y
distingue proceso vivo de pantalla verificada. Bypass afecta solo esa invocacion
de PowerShell, sin cambiar la politica del sistema. El plazo maximo es 55 segundos.
Repetir el build desde el wrapper deja Ninja sin trabajo: no descarta los hooks.
CTest pasa 9/9 y el traductor paso 686/686 tras sus cambios de perfil y fronteras.
Estas pruebas no demuestran fidelidad completa del juego.

[Checkpoint](../checkpoints/20261020_prime_runtime_boot_hci_progress.json) y
[referencias fijadas por commit/hash](../checkpoints/20261020_prime_runtime_reference_provenance.json).
Dolphin `cf5cb7ef81c3e8e7af5c9d566334219ffda9f68b` aporta la referencia independiente
de protocolos; no constituye una prueba de compatibilidad de Prime.

## Pendiente

El checkpoint inicial de diez comandos queda superado por el siguiente hito.
Los tres bucles adaptados (idle, BTA y WPAD) no son todavia un sistema general
de temporizacion y preemption. Tampoco se han
demostrado los cambios entre hilos, la ejecucion de todos los RSO ni el render.
Los defaults heredados de MEM2 (128 MiB) necesitan contraste con el IOS objetivo.

El backend DVD aun depende de la extraccion privada. El producto requiere lectura
de particion directamente desde la ISO propia. El DOL de matching conserva SHA-1
`3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2`; la ISO conserva 4.699.979.776 bytes y
solo lectura. No se tocaron fuentes de `decomp/` ni el informe para amigos.

## Continuacion: WUD completado y carga DSP identificada

Prueba UTC 2026-10-21 03:12 (20 de octubre local), PID 12904:
el arranque ejecuta 28 comandos HCI, incluyendo configuracion de scans,
timeout y consulta de claves vacias. Conserva los datos de FC4C (180 bytes
en RAM del controlador virtual) y siete registros FC4F. No ejecuta el firmware
Bluetooth cargado ni conecta mandos reales. Despues del segundo reset,
el codigo original sale de la espera WUD y alcanza AI/DSP.

El quinto overlay atiende interrupciones/alarmas en la espera WPAD
`0x804D9898`; no escribe el estado WUD para saltarla. El muestreador de hilos
host permite localizar esperas sin modificar memoria guest; sus referencias
de pila son candidatas, no un stack unwind validado.

AI tiene registros de control, volumen, contador y tiempo de interrupcion
con reloj explicito. Las pruebas verifican frecuencias, parada, reset,
fracciones, wrap y W1C; no hay IRQ AI ni salida de audio demostrada.
El bootstrap DSP respeta halt/reset, flags W1C, transferencia ARAM a MEM2
y la transicion INIT (`80544348`) antes de ROM (`8071FEED`). DMA es sincrono
y el plazo INIT aproximado; no representan temporizacion exacta de hardware.

El protocolo ROM observado carga `0x2000` bytes desde `0x805A1C00`, IMEM cero,
DMEM cero y entrada `0x10`. HashEctor da `347112BA`, variante AXWii contemplada
por el mezclador de referencia. El verificador recalcula la huella desde la
seccion del DOL original y registra tambien SHA-256 del payload. No se puede
reutilizar la inicializacion AX de Mario Kart: contiene globals y callbacks
especificos de ese juego. Sigue pendiente aislar el mezclador, conectar el
buzon/IRQ DSP de Prime y verificar resultados de audio.

La ejecucion termina en una frontera explicita de backend DSP pendiente.
El harness registra `failure_logged=true`; el verificador solo acepta este
resultado con `--expect-dsp-boundary` y exige que no haya otro error.
La ventana puede seguir abierta con el dialogo fatal hasta el plazo del
harness: eso no significa que la CPU siga avanzando.

Evidencia: [checkpoint DSP](../checkpoints/20261020_prime_dsp_upload_boundary.json).
Build incremental correcto; CTest 9/9; contratos AI y DSP pasan por separado.
No hay primera pantalla del juego ni audio verificados. Siguiente paso:
conectar exclusivamente la variante AX identificada, sin accesos a globals
MKW, y mantener como fallo cualquier otra variante no verificada.

```powershell
python -B recomp/tools/verify_prime_runtime_boot_probe.py --probe <run>/runtime_probe.json --expect-dsp-boundary
```

## Estado vigente: transferencia de hilos, ISO y prolog HOME

Checkpoint UTC 2026-10-21 03:44, 20 de octubre local: PID 9928. El ejecutable
sale con codigo 1 al llamar una funcion RSO no registrada. Es una frontera
diagnosticada, no un arranque exitoso ni una pantalla presentada.

Se aislo una entrada del mezclador AX para la huella `347112BA`: omite la
inicializacion de globals/tareas y los callbacks sinteticos de Mario Kart,
desactiva el worker de mezcla para este ensayo y rechaza listas invalidas y
cambios de microcodigo aun no implementados. Un guard contrasta 100 bytes
de las dos regiones MKW monitorizadas y el puntero host de tarea antes/despues
de inicializar. La primera respuesta `DCD10000` llega al handler original
`0x804C5514`, interrupt 7, con EE enmascarado y OSContext valido. Todavia no
demuestra mezclas, DMA de audio ni salida PCM correctas.

El primer cambio real de hilo revelo que devolver desde `rfi` sobre la pila
anterior corrompia la ejecucion. Se introdujo transporte de continuaciones
Windows: cada hilo conserva pila host y CpuContext propios. El scheduler
original sigue seleccionando hilos y editando sus colas. Al volver a una pila
suspendida se carga su propio OSContext y se retoma `0x804E6CD4`, la comparacion
posterior a OSSaveContext, mediante un label del overlay. Las instrucciones
de llamada, comparacion y branch se anclan contra el DOL original.
La prueba aislada pasa 200 transferencias, canarios de pila, identidad CPU,
registros, modo NI y propagacion de excepciones al hilo principal.
El log del juego muestra dos hilos con entrada `0x8042FEC4` y retorno
`0x804E70BC`, ambos con ida/vuelta al principal. Cancelacion/reuso de hilos,
preemption general y semantica completa de FPU siguen pendientes.

Se implementaron el registro DI de tapa cerrada y los latches diagnosticos MI
`0xCC004032..58`. Estos ultimos no acumulan medidas de trafico de bus Wii.
PI FIFO reset descarta la cola y el escritor de vertices incompleto y aborta
el frame Aurora activo; no senala finalizacion. El arranque todavia agota
esperas y aborta frames, y hay avisos XF de arrays invalidos. La equivalencia
del flujo grafico y los eventos PE requieren trabajo adicional.

La peticion de `Worlds.txt` en `0xFC7C646C` era valida: 468 bytes de archivo,
480 de lectura alineada. La restriccion anterior al limite de cada archivo
FST era incorrecta para una lectura de particion. Un probe nod con hashes de
clusters habilitados contrasto el prefijo y obtuvo los doce bytes originales
restantes, sin inventar padding. [Prueba de alineacion](../checkpoints/20261020_worlds_alignment_iso_proof.json).

El runtime enlaza ahora nod de la fuente fijada
`6a54b418e541ce5eeb364399116afd6e47b817bb`, biblioteca FFI Windows GNU ya
verificada. `[paths].iso_path` selecciona la ISO; el adaptador comprueba
RM3E01/disco 0/revision 0, abre la particion de datos en solo lectura y valida
hashes durante la lectura. Se comparan tambien los bytes despues de escribir
en RAM guest. El verificador repite los rangos directamente desde la ISO y
contrasta los solapes con archivos de investigacion. Las 17 transferencias
incluyen selfile.sel, Standard.ntwk, NoARAM, Worlds.txt, MiscData y recursos HOME.
La publicacion inicial del FST y el inventario aun requieren `dvd_root`
extraido: el requisito final de funcionar sin extraccion todavia no esta cumplido.

La ultima llamada pendiente es `0x9016E770` desde `0x8019F8CC`. El RSO original
`RSO_rhbm.rso` se cargo en `0x9015E880`; seccion 1 a `+0x120`, prolog a
`+0xFDD0`: la suma reproduce exactamente el destino observado. Es el modulo
HOME, no Tweaks ni FrontEnd. No se omite el prolog ni se llama a PPC como
codigo host. La candidata historica del offset `C690` era otra entrada.

Siguiente trabajo: revisar el puente RSO/REL ya existente y traducir/registrar
el prolog y sus dependencias con relocaciones basadas en la colocacion real.
No fijar la base de esta corrida como ABI del modulo; no deducir argumentos
de registros residuales. Despues, seguir el arranque original y los eventos
graficos pendientes. No hay RSO completos reconstruidos.

Reproduccion con el build configurado y la ISO propia:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/build_prime_runtime.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/test_prime_runtime_contracts.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File recomp/tools/probe_prime_runtime.ps1 -Seconds 15
python -B recomp/tools/verify_prime_runtime_boot_probe.py --probe <run>/runtime_probe.json --expect-missing-target 0x9016e770 --verify-iso
```

El harness suprime dialogos fatales solo en su proceso hijo y registra codigo
de salida y hashes. Los lanzamientos manuales conservan los dialogos.
CTest pasa 9/9 tras recompilar la prueba de configuracion con `iso_path`.
Los tres contratos especificos pasan: [resultados](../checkpoints/20261020_prime_runtime_contract_tests.json).
Build y evidencia de ejecucion: [checkpoint vigente](../checkpoints/20261020_prime_iso_rhbm_prolog_boundary.json).
El traductor mantiene su evidencia previa de 686/686; estos cambios no son
unidades de matching ni incrementan sus recuentos. DOL e ISO conservan sus
invariantes y el informe para amigos permanece congelado.

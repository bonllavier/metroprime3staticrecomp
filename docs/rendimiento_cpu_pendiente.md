# Rendimiento de CPU: lo que falta, ordenado por impacto

Análisis del 18/11/2026 (RM3E01_00). Objetivo del usuario: que el port sea ligero y vaya a buenos FPS en hardware moderado, como el port nativo de Metroid Prime 1 (Odrannnn/MetroidPrimePort), que incluso corre en Android.

## Por qué Prime 1 es ligero y este port no

| | MetroidPrimePort (Prime 1) | Este port (Prime 3, `recomp/`) |
|---|---|---|
| Código del juego | C++ descompilado, compilado de forma nativa | PowerPC traducido a C++ (WiiCompiled) |
| Memoria | estructuras nativas | cada acceso comprueba la política MMIO y da la vuelta a los bytes (big-endian) |
| Registros | los asigna el compilador | viven en `CpuContext`; antes y después de cada llamada se copian a `ctx` y se recargan |
| Gráficos | llama a Aurora directamente | el juego escribe bytes en un FIFO emulado; el hilo GX los interpreta y expande; Aurora los vuelve a decodificar |
| Sistema operativo | no existe | hilos de la Wii, IPC, interrupciones y retrazado VI emulados |
| Plataformas | x86 y ARM (Android) | x86-64-v3; depende de la reserva plana de 4 GiB, libco y Dawn |

Medido el 18/11/2026 en la traducción base (`generated/RM3E01_00_conservative/functions`): **2,63 millones de 6,92 millones de líneas (38 %) solo mueven registros entre variables locales y `ctx`** (`ctx->gpr[`/`ctx->fpr[`). En `func_8027351C` (bucle de actualización de actores, el que más crece en las peleas) son 3.824 de 5.762 líneas, con 91 llamadas. El compilador no puede quitarlas porque `ctx` escapa a la función llamada. Por eso los perfiles F8 reparten el coste del código del juego sin un punto caliente único.

## Reparto actual de hilos (i7-4790, grabaciones F8 de octubre y noviembre)

- **Hilo del juego** (lógica traducida; un solo hilo por diseño del juego): 40-60 % ocupado en juego normal, **85-97 % en salas pesadas, peleas y partículas**. Es el límite.
  - Código del juego: 57-76 %.
  - Runtime: 17-28 %. Escrituras al FIFO de GX 9-12 %, despacho indirecto 3-6 %, sondeo de IPC/interrupciones 2-5 %.
- **Hilo GX** (intérprete del FIFO y expansión de vértices): ~89 % en la cinemática de Norion.
- **Ya en otros hilos:** decodificador de Aurora, frame worker, presentador, mezcla DSP, disco asíncrono, compilación de shaders y texturas, todos fuera del núcleo reservado del juego.

La lógica del juego (`CStateManager`, Think, animación) no se puede repartir entre hilos sin cambiar su comportamiento. Lo que queda es abaratar el código traducido y quitar del hilo del juego el trabajo que no es del juego.

## Ya hecho (no repetir)

Ver [parallelizacion.md](parallelizacion.md), [hilo_decodificador_aurora.md](hilo_decodificador_aurora.md) y [20261029_tirones_f8.md](20261029_tirones_f8.md): hilo GX propio (34 → 57 FPS en el hangar), decodificador de Aurora en hilo propio, caché de texturas por generación de escritura, memo de despacho indirecto de 2.048 entradas con vía rápida en línea, memo de llamadas RSO, sondeo de interrupciones en línea cada 8 retornos, FIFO por bloques, núcleo reservado y prioridades (también Linux), límite de compilaciones en juego, optimizaciones del SSAO.

## Pendientes de CPU al 26/11/2026 (resumen para retomar)

Lista única de sugerencias de mejora de CPU acumuladas hasta hoy, con su estado. Datos medidos en la sección "26/11/2026: F8 manual del bajón restante" y anteriores. El detalle técnico de cada punto está en las secciones numeradas siguientes y en el registro de trabajo. **No empezar ninguno sin que el usuario lo pida.**

Caso de referencia: cinemática antes de Rundas, con el arreglo de tramos de efecto (`ram5`). El hilo del juego necesita ~20 ms por fotograma lógico en el momento más cargado, frente a 16,7 ms de presupuesto; la GPU va a ~10 ms. Hay que quitar ~20 % del hilo del juego.

| # | Mejora | Hilo | Ganancia estimada | Estado | ¿Ghidra? |
|---|---|---|---|---|---|
| C1 | **Activar `--ctx-flush-elision elide` y `--proven-ram-accesses on`** en `build_prime_runtime.sh` y `.ps1` (Windows y Linux) | juego | −8 % ocupación (hangar) | **Hecho el 26/11** en los dos scripts; build de trabajo de Linux recompilado el 26/11 (hangar 59,77 FPS); falta el build de Windows | no |
| C2 | **[Implementado y validado en juego 26/11]** **FIFO de GX: entregar bloques en tandas** (256 B-1 KiB) al hilo GX en vez de cada bloque de 32 B (`PrimeGpuCaptureCpuFifoFull` > `GxGpuThread::SubmitBurst` > `Push`, ~650.000 por segundo en el bajón), vaciando en los puntos de sincronización (breakpoint de CP, lecturas PI/CP, draw sync, fin de fotograma) | juego | parte del 14,7 % del FIFO (~5-8 %) | Propuesto. Necesita modo de verificación (comparar el flujo de bytes entregado con y sin tandas) | no |
| C3 | **[Implementado 26/11 en forma de escritura en línea en los shards (`gather_inline.h`); las funciones nativas de la SDK siguen pendientes]** **FIFO de GX: funciones de la SDK nativas** que escriben vértices y matrices en la tubería palabra a palabra (`fn_804D62F8`, `804D4334`, `804D4884`, `804D2A0C`, `804D6028`, `804D61D0`, `804D624C`, `804D571C`, `fn_80371708`…; también las rutas de `Write64Slow`), con escritura en bloque (`PrimeGpuGatherWriteBulk`) | juego | con C2, ~10 de los 15 puntos del FIFO | Propuesto. Identificarlas con el C++ generado y el desensamblado de la decomp | no |
| C4 | **[Implementado (GPR) y validado en juego 26/11; `elide` por defecto]** **Traductor (B): recargas de registros no volátiles** tras cada llamada (~478.000 recargas), con su modo de verificación como `ctx-flush-check` | juego | parte del 65 % de código traducido | Propuesto (sección "Punto 1") | no |
| C5 | **ABI "state-free"** del traductor (`translation.enable_state_free_abi`), apagada desde el 20/10 por `0x802E201C` | juego | la palanca más grande (38 % de las líneas generadas) | Sin probar en juego (punto 1 de la lista siguiente) | no |
| C6 | **[Revisado 26/11: sin mejora barata]** **Despacho de llamadas indirectas** (`InvokeIndirectCpu`, 2,4 % en el bajón) y **sondeo de dispositivos** (`PollBoundaryDevices` 1,7 %, IPC ~1 %) | juego | ~3-4 % | Propuesto | no |
| C7 | **Espera activa del hilo GX**: `GxGpuThread::Main` gira 4.000 `_mm_pause` antes de dormir; es el 19 % de sus muestras en el bajón. No frena el juego en este PC, pero en CPU de 4 núcleos o portátiles quita núcleo y energía. Reducir el giro o adaptarlo | GX | libera núcleo en hardware moderado | Propuesto, medir con `thread_cpu.sh` | no |
| C8 | **Hilo GX en cinemáticas**: expansión de vértices (`ExpandPackedDraw`, `ExpandMappedAttribute<2u>`, `WalkDisplayList`), ~60 % de trabajo real en el bajón; reactivar `defer_dl_expansion` solo para arrays no volátiles (punto 4 de la lista siguiente) | GX | margen del hilo GX | Propuesto | no |
| C9 | **Código nativo recuperado de las funciones de dibujo del juego**: árbol `fn_800E1478` > `fn_801AF4B4` > `fn_801AEE3C` > `fn_8035F040` > `fn_80362B1C` > `fn_8036203C` > `fn_803EE30C` > `fn_8034F1AC` (37 % del hilo del juego en el bajón), con tiempo propio repartido en 0x8034xxxx-0x8037xxxx; también `fn_80213A84`/`fn_8035C114` y `fn_8047D6F0`. Ninguna recuperada en `decomp/` | juego | grande pero lento | Largo plazo (punto 3 de la lista siguiente) | **sí**: el usuario lo tiene en Windows; pedirle las direcciones a abrir |

Relacionado, sin ser CPU:
- **Arreglo de tramos de efecto del SSAO** (`kMaxEffectRuns = 8` en `aurora-main/lib/gfx/common.cpp`) y medidor de GPU a 2.048 pases (`gpu_timing.cpp`): hechos y probados en la copia de Linux (cinemática de 9-11 a 50+ FPS), **sin commit**. Validar también en Windows.
- **Posprocesado de GPU**: 4-5 ms por imagen en juego normal (SSAO, reflejos, máscaras). Revisarlo para GPU modestas.
- **Herramientas de QA**:
  - el F8 automático (`STACK_PROFILE_BELOW_FPS`) se disparó en tramos sin caídas; revisar su criterio;
  - para la cinemática, mejor el F8 manual (120 s);
  - repeticiones deterministas pendientes de encontrar la fuente de no determinismo de los enemigos.
- **Diagnósticos añadidos que conviene conservar**: `[staging-wait]`, `[ssao-effect-runs]` y `[gpu-pass]` con todos los pases.

## Lo que falta, por impacto

### 1. Traductor: ABI "state-free" y vida de registros entre llamadas — hilo del juego

- La palanca más grande: ataca el 38 % de las líneas generadas y el 57-76 % del hilo del juego.
- El modo existe en el traductor (`translation.enable_state_free_abi`, `CxxLinearCodeGenerator.StateFreeAbi.cs`), pero se apagó el 20/10/2026 porque falló en `0x802E201C` (quedaba `InvokeDirectCpu<0x802E2000u>(ctx)` dentro de una variante sin `CpuContext`). La build validada en juego lo registra como `Automatic state-free region ABI: 0`. **Nunca se probó en juego.**
- Plan: activarlo con esa función excluida, pruebas diferenciales, recompilación completa y A/B con la repetición `hangar_1x_window`.
- Riesgos: rutinas que no respetan la ABI (`__save_gpr`/`__restore_gpr` con r11/r12, cambios de contexto del OS, hooks del runtime que leen `ctx`).
- Estado y avances: sección "Punto 1" más abajo.

### 2. Escrituras al FIFO de GX — hilo del juego, 9-12 %

- Actualización 26/11/2026: 14,7 % en el bajón de la cinemática de Rundas. Desglose y propuestas C2/C3 en el resumen de arriba.

- Hoy cada palabra que el juego escribe en `0xCC008000` pasa por una llamada fuera de línea (`GX_HLE_FIFO_Write*` → `PrimeGatherStoreFast`).
- Opciones: append en línea dentro de los fragmentos traducidos (recompila todo), o funciones GX del SDK nativas que llamen a Aurora como en Prime 1, lo que también quitaría parte de la doble decodificación del hilo GX.

### 3. Reemplazos nativos de funciones calientes — hilo del juego

- Matemática (`CVector3f`, `CMatrix3f`, `CTransform4f`), colisión, partículas (`CElementGen`), skinning.
- El runtime ya admite funciones nativas con prioridad sobre las traducidas (`FunctionKind::Native`).
- Es el camino hacia lo que es Prime 1, pieza a pieza y guiado por F8. La descompilación con matching está al 3,5 %: camino largo.

### 4. Hilo GX en cinemáticas

- La etapa 2 (expansión de vértices en el decodificador, `defer_dl_expansion`) está apagada por las grietas de la lava de Bryyo.
- Opción: reactivarla solo para arrays que el juego no reescribe (los volátiles ya se detectan).

### 5. Hardware moderado

- Preset "ligero": SSAO, iluminación por píxel y metal mejorado apagados.
- Precarga de shaders más corta: hoy 4-5 min y 2,3 GB de pico sin caché.

### 6. Android / ARM

- No es realista a corto plazo con la recompilación estática. Steam Deck sí.

### Conflicto a decidir

Los 120 nativos (`[video] native_120fps`, prioridad de FPS altos del 17/11) duplican el trabajo del hilo del juego, en contra de "ligero en hardware moderado". Los puntos 1-3 ayudan a los dos.

## Cómo puede ayudar el usuario

- QA de los ejecutables experimentales del punto 1 (habrá crashes al principio).
- Grabar repeticiones QA de escenas pesadas (Norion, Rundas, hangar con piratas, lava de Bryyo) para una batería de rendimiento automática.
- Definir el hardware objetivo ("moderado") y, si es posible, probar en él.
- Turbo Boost desactivado en Linux (`intel_pstate/no_turbo` = 1): 3,6 GHz en vez de 4,0; cambia las mediciones.
- Elegir prioridad entre ligereza y 120 nativos.

## Punto 1: registros entre llamadas (registro de trabajo)

Afecta a Windows y Linux por igual: el cambio está en el traductor (emisión de shards) y en el runtime común. Cada plataforma solo tiene que volver a emitir los shards y compilar.

### 19/11/2026: diagnóstico

- El modo `enable_state_free_abi` del traductor es más limitado de lo que su nombre sugiere: solo convierte en funciones nativas con argumentos por valor a componentes cerrados de funciones llamadas directamente con ≤ 4 entradas y ≤ 2 salidas, todos cuyos descendientes también lo sean. No toca las llamadas a funciones grandes, que es donde está el coste.
- Medido sobre la traducción base: de 3,06 millones de líneas con `ctx`, 2,6 millones están en los puntos de llamada (81.427 llamadas, ~32 líneas por llamada: ~16 guardados antes y ~16 recargas después). Entradas y salidas de función: ~450.000.
- Por qué tantas, ejemplo de `func_8027351C` → `0x802794B8`: antes de la llamada se guardan en `ctx` todos los registros residentes aunque no hayan cambiado desde la última sincronización; después se recargan todos, incluidos r1/r2/r13 y los no volátiles r14-r31/f14-f31. El contrato de la función llamada es la unión transitiva de escrituras: un descendiente con una llamada virtual lo vuelve completo, y una función que solo guarda y restaura un no volátil cuenta como que lo escribe.
- La traducción base del 20/10 (`generated/RM3E01_00_conservative`) es la validada en juego y no se regenera (el traductor actual produce 101 funciones distintas). Por eso la mejora es una pasada sobre el C++ ya generado, al emitir los shards, que se activa con una opción.
- Los manejadores de interrupción corren en un banco de registros aislado (`GuestInterruptCallbackContext`, `os_interrupt.cpp`) y no modifican el `ctx` del código interrumpido.

Dos mejoras:

- **(A) Exacta, sin confiar en la ABI:** no guardar un registro cuyo valor local ya es el de su ranura en `ctx`. Implementada (abajo).
- **(B) Con verificación:** no recargar tras la llamada los registros que la ABI de PowerPC conserva (r1, r2, r13, r14-r31, f14-f31) cuando el contrato no lo demuestra. Riesgos conocidos: `__save_gpr`/`__restore_gpr`, cambios de contexto del OS, `OSLoadContext`. Pendiente: necesita su propio modo de verificación (comparar la ranura con el local antes de cada recarga eliminada). Potencial medido en los shards: ~478.000 recargas de esos registros de ~1,13 millones (42 %).

### 19/11/2026: (A) eliminación de volcados redundantes

- `translator/src/Translator.Core/Build/ContextFlushElision.cs`: análisis hacia delante "local igual a su ranura" sobre las sentencias emitidas, con el grafo de `goto`, `if`/`else`, `switch`/`case`/`break` de cada función. Conservador ante lo desconocido:
  - un local pasado a una función que no está en la lista verificada de parámetros por valor cuenta como escrito (`SetCRResident`, `SetCRFloatResident` y `PpcSetPairedFprInline` solo escriben su primer argumento);
  - `ctx` pasado a una función desconocida olvida todos los pares;
  - una escritura directa a una ranura rompe su par;
  - funciones con lambdas, `&ctx`, sentencias de varias líneas, `else if` o cualquier forma no reconocida no se tocan.
- `TranslatedBuildShardEmitter`: la pasada corre tras `LowerStableDirectCalls`. Con la opción activa, la identidad de cada shard incluye `ctx-flush-elision:v1:<modo>`, así que sus objetos no se mezclan con los de la build normal. Con `off` todo queda byte a byte igual.
- CLI: `emit-build-shards --ctx-flush-elision off|elide|verify` (por defecto `off`). Informa cuántos volcados quitó y por qué se saltó cada función.
- Modo `verify`: cada volcado eliminado pasa a ser `MKW_CTX_FLUSH_CHECK` (`runtime/include/guest_abi/ctx_flush_check.h`, `runtime/src/ctx_flush_check.cpp`). Compara la ranura con el local; si difieren, escribe `[ctx-flush-check] mismatch in <función> <registro>` en `console.log` (una vez por función y registro) y guarda igualmente. Es jugable aunque el análisis fallara.
- Resultado sobre la traducción base: **774.202 de 1.458.556 volcados eliminados (53 %)**, en 21.148 de 21.712 funciones. Sin tocar: 559 funciones sin registros residentes y 5 por formas no reconocidas. Comprobado comparando los shards: todas las líneas quitadas son volcados exactos `ctx->X = X;`.
- Pruebas: `ContextFlushElisionTests` (15) y la suite completa del traductor, 708/708.

Cómo probarlo (Linux; en Windows, los mismos comandos con el `Translator.Cli` y la carpeta de build de Windows):

```bash
source /mnt/mp3port/env.sh && cd $MP3PORT/src
out=recomp/generated/RM3E01_00_conservative/build_shards_ctxverify   # o _ctxelide con --ctx-flush-elision elide
mkdir -p $out && cp recomp/generated/RM3E01_00_conservative/build_shards/base_common_shard_map.json $out/
dotnet recomp/third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll emit-build-shards \
  --project recomp/config/RM3E01_00/project_full_conservative.yml \
  --base-metadata recomp/generated/RM3E01_00_conservative/runtime_hooks/base_translation_output.json \
  --base-functions-dir recomp/generated/RM3E01_00_conservative \
  --native-source-dir recomp/third_party/WiiCompiled/runtime/src \
  --ctx-flush-elision verify --out $out
cmake -S recomp/third_party/WiiCompiled/runtime -B <carpeta de build aparte> -DMKW_TRANSLATED_SHARD_MANIFEST=$PWD/$out/shards.cmake
```

- Copiar `base_common_shard_map.json` conserva el mismo reparto de funciones en shards.
- Build de prueba de verificación en Linux: `/mnt/mp3port/builds/RM3E01_00_runtime_linux_ctxverify` (copia del build de trabajo sin `Logs` ni `Screenshots`, con sus `CMakeCache.txt` reescritos a la nueva ruta).

### 19/11/2026: primera verificación en juego y contratos incompletos

- Build de verificación compilado en Linux (63 min, 14.428 pasos: la copia de la carpeta de build no aprovechó objetos). Repetición `hangar_1x_window` completa (`finished=1`, 59,66 FPS de media, 3/80 ventanas < 57), `run: /mnt/mp3port/builds/qa/runs/hangar_1x_window/20261119_111128_ctxverify`.
- Una sola discrepancia en toda la repetición, una sola vez: `func_8032F7B8 f0 slot=0x0 local=0x3ff0000000000000` (en `ctx` 0, en el local 1.0).
- Causa: `0x8042E428` es una reparación de switch escrita a mano (con una lambda que escribe `ctx->gpr[1]`, `ctx->fpr[1]`, etc.) que llama a `0x8042EF94`, y esta deja su `f0` en `ctx`. El contrato que el traductor calculó con el PowerPC original para `0x80429434` (→ `0x8042E428` → `0x8042EF94`) no incluye `f0`, así que `func_8032F7B8` ni lo guarda ni lo recarga alrededor de esa llamada. Antes, el siguiente volcado lo "curaba". `f0` es volátil y no es argumento, así que según la ABI no tenía efecto, pero no se confía en eso.
- Corrección: `ContextFlushElision.FindUntrustedCallTargets`. Son "no fiables" las funciones cuyo código escribe ranuras de registro por su cuenta (cualquier `ctx->gpr/fpr[N]...= ` que no sea el volcado exacto del generador), las sobrescrituras nativas ganadoras y todos sus llamadores transitivos. Tras una llamada a un destino no fiable, el análisis olvida todos los pares. Prueba `CallsReachingASlotWriterForgetEveryPair` (suite: 709/709).
- Coste de la protección: 1.213 destinos no fiables. Los volcados eliminados pasan de 774.202 a 774.056. El conjunto nuevo es un subconjunto del ya verificado en el hangar. Identidad de shards `ctx-flush-elision:v2`.

### 19/11/2026: medición A/B en el hangar

Base y `elide` compilados en la misma carpeta, con el mismo runtime; solo cambian los shards (`/mnt/mp3port/builds/RM3E01_00_runtime_linux_ctxverify`, ejecutables `Corruption_3 PC.base`, `.ctxelide`, `.ctxverify_v1`, `.ctxverify_v2`). Compilar solo los 72 shards tarda ~3,5 min.

- Ejecutable: 214 → 199 MB (−7 %).
- Repetición `hangar_1x_window`, 8 ejecuciones alternadas, todas completas. FPS medios: base 59,68-59,89, `elide` 59,89-59,92. Ventanas < 57: base 0-3, `elide` 0.
- El tiempo de CPU por hilo (`tools/linux_diag/thread_cpu.sh`) no mide el trabajo del hilo del juego: está al 100 % también en la base, porque el bucle ocioso del juego (`func_804E6BD4`/`0x804E6D20`) da vueltas esperando el retrazado. En los demás hilos (una ejecución por variante con datos): hilo GX 168 → 151 s, decodificador 93 → 79 s, proceso 822 → 789 s.
- Perfil F8 automático de 10 s en el segundo 150 (`WII_COMPILED_STACK_PROFILE_AT=150`), contando como espera `func_803E9F78`, `func_804E6BD4`, `func_804E6D20` y `VI_HLE_PollRetraceGuestSpin`:

| Ejecución | Hilo del juego ocupado | Código del juego | Runtime |
|---|---|---|---|
| base 1 | 36,0 % | 30,3 % | 5,7 % |
| base 2 | 50,2 % (otro momento de la escena: el dibujado pesa 28 % frente a 17 %) | 40,6 % | 9,6 % |
| `elide` 1 | 34,3 % | 28,2 % | 6,1 % |
| `elide` 2 | 34,2 % | 28,1 % | 6,1 % |

- Conclusión: código del juego −7 % (30,3 → 28,2 %), cerca del ruido de una ventana. Quitar el 53 % de los guardados rinde poco, porque los guardados son baratos en una CPU moderna (búfer de escritura).
- Dónde está el tiempo ocupado del hilo del juego (`elide`, 5.751 muestras): **82 % dentro de los cuerpos traducidos**, escrituras al FIFO de GX ~6 %, sondeo de dispositivos ~2 %, resto del runtime pequeño.

### 19/11/2026: perfil por instrucción del código traducido

Datos y scripts en `/mnt/mp3port/builds/qa/ctxflush_ab/` (`profload.py`, `hot.py`, `classify.py`; perfil `prof_ctxelide_122844` con su ejecutable). El scratchpad de la sesión se vació a mitad del trabajo: los datos de experimentos van en `/mnt/mp3port`.

- El coste está muy repartido: las 25 funciones traducidas más calientes suman el 18,8 % del tiempo ocupado del hilo del juego, con 863 funciones distintas en 10 s. Reescribir unas pocas funciones a mano no cambia el total; hace falta mejorar el traductor en general.
- Reparto por instrucción (muestra y dos instrucciones previas, por el desfase del muestreo; 1.405 muestras, 97 % de las 200 funciones más calientes):

| Categoría | Peso |
|---|---|
| Enteros y movimientos varios | 30,3 % |
| Cargas/guardados con `movbe` (latencia de memoria del juego) | 28,3 % |
| Saltos | 11,7 % |
| Coma flotante: redondeo/conversión a simple (`cvtsd2ss`/`cvtss2sd`) | 9,2 % |
| Accesos a `ctx` (`%rbx`) | 6,5 % |
| Comprobación MMIO por acceso (`and $0xfe000000; cmp $0xcc000000`) | 6,0 % |
| Aritmética de coma flotante | 2,9 % |
| Llamadas | 2,1 % |
| Puesta a cero NI con umbral thread_local (`g_mkwNiFlushThreshold`) | 1,9 % |
| Comparación de coma flotante | 1,1 % |

- Lectura:
  - La mayor parte (cargas de datos del juego, enteros, saltos) también la tendría el código nativo.
  - Lo que es sobrecoste propio de la traducción y se puede atacar suma alrededor de un 20-25 %:
    - comprobación MMIO 6 %: quitarla en accesos que el traductor demuestre que van a RAM, como la pila;
    - `ctx` 6,5 %: recargas no volátiles, mejora (B);
    - exactitud de coma flotante ~11 %: redondeo a 25 bits de `fmuls` (`PpcForce25BitInline`), corrección de empates de `fmadds`, puesta a cero previa al redondeo. Dar menos exactitud sería decisión del usuario, porque cambia resultados respecto a la consola.
  - Con la recompilación estática se puede esperar del orden de un 10-25 % menos en el hilo del juego, no el salto hasta un port nativo como el de Prime 1.

Siguiente:

1. ~~Perfil a nivel de instrucción de las funciones traducidas más calientes~~ (hecho, arriba). Original: (las muestras F8 guardan el RIP: desensamblar y repartir por tipo de operación). Objetivo: saber si pesan las comprobaciones y byteswap de los accesos a memoria, la emulación de CR (`SetCRResident`), el redondeo de coma flotante (`PpcForceSingleValueInline`, `PpcFmulsInline`) o el coste fijo de cada llamada. Decidir la siguiente mejora del traductor con esos datos.
2. ~~Partida con el build de verificación v2 en otras escenas.~~ Hecho el 19/11/2026, 20:05-20:23 (partida del usuario, comprobado que el proceso era `Corruption_3 PC.ctxverify_v2`): 0 discrepancias `[ctx-flush-check]` en tres sesiones (la primera terminó con `OSRestart` desde el juego, la segunda se cerró a los pocos segundos, la tercera ~15 min, en Bryyo según el usuario). Solo quedan verificados los sitios que se ejecutaron en las zonas jugadas. Las 400 líneas `[prime-present-slow]`/`[refresh-lock-slow]` son el tope del diagnóstico de los builds de prueba (`vi.cpp`): aparecen igual en la base.
2. Build `elide` y comparación de rendimiento con `hangar_1x_window` (`tools/linux_diag/qa_replay.sh`) frente al build de trabajo.
3. Si no hay discrepancias, opción en `build_prime_runtime.sh`/`.ps1` y después la mejora (B).

### 19/11/2026: accesos demostrados a RAM sin comprobación MMIO

Decisión del usuario: seguir con las comprobaciones MMIO.

- Asimetría encontrada: el traductor ya emite todos los guardados relativos a la pila sin comprobación (`StackStoreHelper` → `FlatWriteRam*`, 101.948 sitios, ninguno relativo a `r1` comprobado), pero las lecturas relativas a la pila (`StackLoadHelper`) seguían con la comprobación (~78.000 sitios). Tampoco se aprovechaban las bases SDA `r2` (`_SDA2_BASE_` 0x806869C0) y `r13` (`_SDA_BASE_` 0x806801C0).
- Runtime (`guest_abi/memory_access.h`): `FlatReadRam8/16/32/64/Float32/Float64` y `FlatWriteProven8/16/32/64/Float32/Float64`. Son iguales a `FlatRead*`/`FlatWrite*` sin la máscara MMIO, y conservan el camino comprobado donde la página del host es mayor que la de la Wii (en x86-64 y Windows eso es falso en tiempo de compilación). Red de seguridad: la vista plana mantiene la ventana MMIO sin acceso, así que un acceso que cayera ahí por error daría un fallo diagnosticado y no tocaría dispositivos.
- Pasada `ContextFlushElision.LowerProvenRamAccesses` (`translator/src/Translator.Core/Build/ProvenRamAccessLowering.cs`), opción `emit-build-shards --proven-ram-accesses on|off` (por defecto `off`), identidad de shards `proven-ram-accesses:v1`.
  - Reescribe `FlatRead*/FlatWrite*((rB + c))` y `(rB)` con `|c| < 0x8000`, para `rB` = `r1`, `r2`, `r13`.
  - Solo en funciones donde `rB` únicamente se carga al entrar o se recarga desde su propia ranura (`r1` también con `r1 = (r1 ± c)`), su ranura solo recibe `rB`, y nunca se pasa a una función auxiliar que no sea de parámetros por valor.
  - Corre antes que la de volcados: en modo verificación, `MKW_CTX_FLUSH_CHECK(..., r1, ...)` contaría como escritura de `r1`.
- Resultado: 113.568 comprobaciones quitadas (~30 % de los accesos comprobados):

| Base | Funciones elegibles | Lecturas | Guardados |
|---|---|---|---|
| r1 | 21.651 | 77.993 | 0 (ya iban sin comprobar) |
| r2 | 21.695 | 17.046 | 0 |
| r13 | 21.697 | 11.525 | 7.004 |

- Comprobado comparando los shards con y sin la pasada: las 113.568 líneas cambiadas solo difieren en el nombre de la función auxiliar. Pruebas `StackAndSdaAccessesLoseTheMmioCheck` y `BaseSetFromAnythingElseKeepsTheCheck` (suite 711/711).
- Build `Corruption_3 PC.ctxelide_ram` en `/mnt/mp3port/builds/RM3E01_00_runtime_linux_ctxverify`. Pendiente: A/B en el hangar frente a `.ctxelide` y `.base`, y partida del usuario.

### 19/11/2026: A/B con perfiles largos (base, `elide`, `elide` + accesos demostrados)

- Las tres variantes se compilaron en la misma carpeta con el mismo runtime: `Corruption_3 PC.base2`, `.elide2`, `.ram2`. Tamaños: 214, 199 y 190 MB.
- Opción nueva de los builds de prueba: `WII_COMPILED_STACK_PROFILE_SECONDS` (`settings_overlay.cpp`). Perfil F8 de 300 s desde el segundo 40 de la repetición: cubre casi toda la repetición y evita el ruido de las ventanas de 10 s.
- Script `/mnt/mp3port/builds/qa/ctxflush_ab/ab_long.sh` y resumen `busy_long.py`. Cada ejecución guarda la salida, el perfil y el ejecutable exacto en `run_<variante>_<hora>/`.
- Primer intento fallido: con Chrome abierto (~13,8 GB) quedaban ~1,5 GB libres, y el vigilante de memoria cerró el juego en 5 de 6 repeticiones. Esos datos están en `failed_low_mem/` y no se usan. Con Chrome cerrado, 11 GB libres y las 6 completas.
- Resultado (hilo del juego ocupado, fuera de las esperas):

| Variante | Ocupado | Código del juego | Runtime | FPS | Ventanas < 57 |
|---|---|---|---|---|---|
| base | 31,42 % / 31,49 % | 25,54 % / 25,73 % | 5,88 % / 5,77 % | 59,85 / 59,93 | 0 / 0 |
| `elide` | 34,34 % / 29,87 % | 27,71 % / 24,07 % | 6,63 % / 5,80 % | 59,80 / 59,89 | 1 / 1 |
| `elide` + accesos demostrados | 28,76 % / 28,87 % | 23,16 % / 23,19 % | 5,60 % / 5,68 % | 59,89 / 59,85 | 0 / 0 |

- Lectura:
  - Las dos mejoras juntas: **−8,4 % de tiempo ocupado del hilo del juego** (31,45 → 28,8 %) y **−9,5 % en el código del juego** (25,6 → 23,2 %). Las dos repeticiones de cada una coinciden en una décima.
  - `elide` sola no es concluyente: una repetición mejor que la base (29,87 %) y otra peor (34,34 %), sin explicación en los datos.
  - En la repetición de `ram2` del primer intento (~6 min, 75 ventanas) no hubo fallos de página MMIO ni errores.
- Siguiente:
  1. Activar las dos pasadas en `build_prime_runtime.sh`/`.ps1` (Windows y Linux) tras una partida del usuario con un build de verificación que incluya la reescritura de accesos.
  2. Mejora (B): recargas no volátiles.

### 21/11/2026: partidas del usuario con las dos mejoras y segundo contrato incompleto

- Sesiones revisadas:
  - verificación v2, 19/11 20:08-20:59, más larga de lo revisado aquel día;
  - `Corruption_3 PC.verify_ram` (verificación + accesos demostrados), 20/11 23:44 → 21/11 00:16, comprobado por el ejecutable del proceso.
- Sin fallos MMIO ni errores con la reescritura de accesos.
- Una discrepancia, la misma en las dos sesiones: `func_801556DC r4 slot=0xbfd27880 / 0x40eb6eb0 local=0x806996f8`, en el volcado de salida (`return`).
- Causa: `0x80317008` hace una llamada indirecta (`InvokeIndirectCpu`) y deja en la ranura el `r4` de la función llamada (un valor de coma flotante). El contrato que usan sus llamadores (calculado del PowerPC) no incluye `r4`, así que `func_801556DC` no lo recarga y su `r4` local (una dirección de la pila) ya no coincide con `ctx`. Según la ABI era inofensivo (`r4` es volátil tras la llamada), pero la pasada debe ser exacta sin confiar en la ABI.
- Corrección: `FindUntrustedCallTargets` toma también como semilla toda función con `InvokeIndirectCpu`, `InvokeIndirectJump` o `PRIME_RSO_TryInvoke`. Destinos no fiables: 1.213 → 11.942. Volcados eliminados: 774.056 → 769.128 (−0,6 %). Comprobado en los shards: el volcado de salida de `r4` en `func_801556DC` vuelve a emitirse. Prueba `IndirectCallersAreUntrusted` (suite 712/712).
- Pendiente: recompilar los builds de verificación y `elide` con esta corrección (shards solo, ~4 min por variante) y repetir la partida.

### 21/11/2026: bajones en la cinemática antes de Rundas (F8 del usuario)

Perfiles `stack_profile_20261121_001152.txt` y `_001453.txt` (sesión `base_1795236291_pid11273`, ejecutable `verify_ram`), copiados con el ejecutable en `/mnt/mp3port/builds/qa/ctxflush_ab/rundas_f8/`; análisis con `cadence.py`, `window.py`, `waits.py` de esa carpeta.

- Primera captura: 31 de 120 segundos por debajo de 57 FPS. Del segundo 0 al 20, **8-11 FPS**, con huecos de 200-270 ms entre imágenes; después 30-50 FPS hasta el segundo 37. Sin lectura de disco, texturas nuevas ni compilación de pipelines en esos segundos. Segunda captura: 7 segundos a 50-56 FPS.
- Hilos en el tramo 0-20 s frente a 60-120 s: juego 19,5 % ocupado (49 %), GX 15 % (39 %), decodificador 8 % (13 %), frame worker 28,8 % (13,3 %), presentador 0,3 %.
- Esperas en el tramo malo:
  - frame worker: 62 % en `aurora::gfx::begin_frame_impl`, en la espera del mapeo del búfer de staging (la GPU todavía usa el del fotograma anterior);
  - juego: 47,6 % en `aurora_wait_for_frame_worker_sealed` desde `GxGpuThread::HostLock` y 18,6 % en `aurora_wait_for_frame_worker` desde `settings_overlay::Draw` (el panel F9/F8 estaba abierto);
  - GX: 85 % ocioso.
- Log: `[present-latency]` del peor tramo con `frames=90` en 10 s y `worker_seal_encode_ms=108` (normal 2,2), `[frame-worker-slow] phase=prepare_next` 66-70 ms. `[post-ssao]` registra pases con hasta 2.076 dibujos (normal 100-350). Presupuesto de copias EFB normal (16 de 512 MiB).
- Diagnóstico: **límite de GPU**; el driver de NVIDIA bloquea el envío porque la GPU va retrasada. No es CPU ni código traducido. Opciones activas en esa partida: `per_pixel_lighting`, `enhanced_metal`, `modern_lighting`, `contact_shadows` y `ssao` (8 muestras). Hipótesis sin probar: capas grandes y transparentes de la cinemática que pagan esos sombreadores en cada píxel.
- Herramienta nueva: `tools/linux_diag/qa_record.sh` (equivalente de `qa/qa_record.ps1`: `--launch`, `NOMBRE [NOTAS]`, `--stop`, `--status`, con `MP3_BUILD_DIR`), para grabar la cinemática y repetirla con cada opción activada o desactivada (`qa_replay.sh ... cfg:sección.clave=valor`) y con `WII_COMPILED_GPU_TIMING=1`.
- Pendiente: grabación del usuario y bisección de las opciones.

### 21/11/2026: reproducciones que se pierden (anclas con orientación)

- La grabación `rundas_cine` (hecha con `qa_record.sh` sin las pulsaciones automáticas de la introducción) acababa en los menús al reproducirse: `qa_replay.sh`/`.ps1` siempre activan `WII_COMPILED_QA_TAP_INTRO=1`. Ahora `qa_record.sh` y `qa_record.ps1` graban con esas pulsaciones y lo anotan en `meta.json` (`"tap_intro": true`), y los dos `qa_replay` omiten las pulsaciones si `meta.json` dice `"tap_intro": false`.
- Problema siguiente, con `rundas_cine2`: la reproducción se desviaba poco a poco y acababa en otra sala (`drift_events=132`, `max_drift_m=355.8`). Las anclas solo corregían la posición. La vista con ratón gira a Samus con una velocidad angular sobre el tiempo medido de cada fotograma, así que el rumbo se desvía un poco en cada reproducción y el resto del recorrido va en otra dirección.
- Cambio (solo herramientas de QA):
  - las anclas guardan también la rotación 3x3 de la transformada de `CPlayer` (`+0x3C/+0x40/+0x44`, `+0x4C…`, `+0x5C…`) y la inclinación de la vista libre (`+0x780`). Línea `a <fotograma> x,y,z o r00,…,r22 p <pitch>`; las grabaciones antiguas se siguen leyendo;
  - en la reproducción, si el rumbo se desvía más de medio grado se reescribe la rotación, y si la inclinación se desvía, `+0x780`/`+0x77C` y la copia del hook (`PrimeMouseLook::SetPitch`, nueva);
  - el resumen añade `heading_drifts`, `max_heading_drift_deg`, `heading_corrections` y `pitch_corrections`;
  - prueba `mkw_prime_input_recording_tests` ampliada.
- Por comprobar en juego: que escribir la rotación de la transformada basta, es decir, que el juego no guarda el rumbo en otro sitio que lo deshaga.
- Aviso de compilación: tras la actualización del sistema del 5/11, los `.pch` antiguos fallan con `file '/usr/include/asm-generic/errno.h' has been modified since the precompiled header`. Borrar los `*.pch` de la carpeta de build los regenera (los shards se recompilan, ~5 min). Al build de trabajo le pasará lo mismo.
- Resultado con `rundas_cine3` (anclas con orientación, ejecutable `ram3`): reproducción completa pero perdida, `drift_events=113 max_drift_m=551.6 heading_drifts=130 max_heading_drift_deg=171.3 heading_corrections=130 pitch_corrections=25`. Las anclas se graban y aplican (la rotación varía sobre el eje vertical en la grabación), pero el usuario observa enemigos con acciones distintas en cada reproducción: sin un reloj determinista, corregir la posición y la orientación no basta para seguir el recorrido.
- Propuesta del usuario: save states como en Dolphin. Valoración:
  - en la recompilación estática el estado de cada hilo del juego incluye su pila nativa (libco, direcciones del ejecutable con ASLR) y mucho estado del runtime (Aurora/GPU, FIFO de GX, DSP/AX, disco asíncrono, IPC, alarmas, módulos RSO);
  - viable solo dentro de la misma sesión y en un punto tranquilo, y aun así es un proyecto grande;
  - alternativas más cortas: medir a mano con una partida guardada junto a la cinemática, o reproducciones deterministas (reloj del juego y giro de la vista con ratón por fotograma fijo en grabación y reproducción).
  - Pendiente de la decisión del usuario.

### 21/11/2026: dónde se separan las reproducciones (opción 2) y paso a la medición a mano (opción 1)

- Decisión del usuario: probar reproducciones deterministas (opción 2) y, si no funciona, medir a mano (opción 1).
- Descartado como causa del desvío:
  - el giro de la vista con ratón usa el `dt` del juego (`CFinalInput+0`, 1/60 fijo; 0,16 en los dos primeros fotogramas), no el tiempo del host;
  - RM3E01 hace un paso lógico fijo por retrazado y se ralentiza en vez de compensar; las entradas se cuentan una vez por retrazado leído (`PrimeInputTryReadKpad`).
- Traza nueva `WII_COMPILED_QA_TRACE_ANCHORS=1` (`[qa-anchor] frame= pos= drift_m= heading_deg=`, antes de corregir). Dos reproducciones de `rundas_cine3` sin corrección (`WII_COMPILED_QA_NO_CORRECT=1`), comparadas con `/mnt/mp3port/builds/qa/rundas_gpu/compare_anchors.py`:
  - **idénticas a la grabación hasta el fotograma 3.960** (0,00 m y 0,01° en todas las anclas);
  - entre 3.960 y 3.990, salto de rumbo de −78° (A) y −112° (B).
- Causa: en el fotograma 3.972 la grabación pulsa Z (`0x2000`, fijar objetivo); desde el 3.987 el puntero se centra. Al fijar, la cámara gira hacia el enemigo, y el enemigo estaba en otro sitio en cada reproducción. La IA de los enemigos no se repite (números aleatorios, cargas o reparto de hilos dependientes del tiempo real; sin identificar). El recorrido de Samus sí es determinista mientras no dependa de los enemigos.
- Para hacer deterministas los enemigos habría que localizar y controlar esa fuente (por ejemplo, la semilla del generador aleatorio del juego, o el momento en que terminan las cargas asíncronas). Queda pendiente.
- Opción 1: `tools/linux_diag/play_variant.sh ETIQUETA [sección.clave=valor ...]`. Juego a mano en un perfil desechable, con copia de la partida y del `Config.toml` del build, opciones cambiadas, cachés compartidas y traza de rendimiento. Guarda `console.log` y los perfiles F8 en `/mnt/mp3port/builds/qa/manual/<etiqueta>_<hora>/`.

### 25/11/2026: cinemática de Rundas, opciones gráficas a mano (`play_variant.sh`)

Carpetas en `/mnt/mp3port/builds/qa/manual/`. Ventanas de 5 s de `[prime-performance]` y `[present-latency]` (imágenes por 10 s / ms de sellado y codificación):

| Variante | Ventanas lentas en la cinemática (FPS) | `present-latency` en la cinemática |
|---|---|---|
| base | 16, 10, 9, 10, 18, 10, 10, 45 | 115/86, 142/68, 118/84 |
| `contact_shadows=false` | 11, 9, 10, 20, 11, 10, 37 | 156/63, 114/86, 151/65 |
| `modern_lighting=false` | 23, 11, 9, 11, 13, 11, 38 | 101/99, 167/58 |
| `effects.ssao=false` | 11, 9, 10, 32, 11, 11, 36 | 96/105, 166/59 |
| `per_pixel_lighting=false` + `enhanced_metal=false` | 11, 10, 10, 36, 11, 10, 35, 35 | 119/83, 148/66, 116/85 |

- Ninguna de las mejoras gráficas, apagada por separado, quita el bajón: ~30 s a 9-11 FPS en todas.
- Tramo lento de la base frente a juego normal, por fotograma: ~36.000 escrituras BP frente a ~7.000, ~2.300 enlaces de textura frente a ~600, ~25 copias EFB frente a ~6. Frame worker: `seal_encode` 90 ms de media y `prepare_next` (espera del staging, la GPU) 66 ms. En el mismo tramo, `[engine-lights] world view glitch ... 500 m away` (cámara de la cinemática).
- Siguiente: distinguir si la GPU va de verdad al 100 % o si GPU y CPU se esperan entre sí (lecturas de copias EFB a RAM, sincronizaciones). `play_variant.sh` registra ahora `nvidia-smi` cada 0,5 s (`gpu.csv`), toma un F8 por sí solo cuando los FPS bajan de 30 y acepta variables de entorno (`WII_COMPILED_GPU_TIMING=1` para el tiempo de GPU por pase).

### 25/11/2026: tiempo de GPU en la cinemática de Rundas (Linux)

- Variantes `diag_base` y `diag_all_off` con `WII_COMPILED_GPU_TIMING=1`. En el arranque, `play_variant.sh` tomaba `profiles/` como carpeta de la sesión y no copiaba `console.log` (corregido). `diag_base` conserva la misma salida en `stdout.log`; el F8 de `diag_all_off` se recuperó del perfil de trabajo.
- `[gpu-time]` en la cinemática: **8-10 ms de GPU por fotograma** (juego 2,5-4,7, posprocesado 4,7-5,9 incluso con todo apagado), con ≥256 pases por fotograma (el medidor solo cuenta 256, `kPairsPerFrame`), frente a 1,7 ms y 3 pases en juego normal. Los fotogramas duran ~100 ms. (Conclusión inicial «la GPU no es el límite» **errónea**: el medidor no ve los pases que siguen al 256 ni las copias de textura; ver 26/11.)
- F8 automático (`diag_all_off`, 10 s en la cinemática): ningún hilo saturado (juego 33 %, GX 36 %, decodificador 11 %, frame worker 22 %, presentador 0,6 %). Frame worker: 28,5 % esperando el mapeo del staging en `begin_frame_impl` y 42 % ocioso; el juego le espera un 22 % (`HostLock` y `settings_overlay::Draw`).
- Dato del usuario: en Windows, en el mismo PC, esta cinemática iba fluida. Apunta al camino de Linux (Vulkan/Dawn con el driver de NVIDIA).
- Hipótesis: la cinemática llena el staging varias veces por fotograma; cada `split_staging_batch` envía, espera a que la GPU termine y vuelve a mapear. Si esa vuelta es lenta en Vulkan, unas pocas divisiones explicarían los 66 ms de `prepare_next`. Diagnóstico nuevo en `aurora-main/lib/gfx/common.cpp`: `[staging-wait] frames= capacity_splits= map_waits= slow_waits= wait_ms total= max= in_splits= | high_water KiB ...`, cada 5 s si hubo divisiones o esperas de 2 ms o más. Ejecutable `Corruption_3 PC.ram4`.

### 26/11/2026: la GPU sí está al 100 %; causa probable, la máscara de SSAO bajo efectos

Variante `diag_staging` (ejecutable `ram4`, carpeta `/mnt/mp3port/builds/qa/manual/diag_staging_1125_235731/`).

- `[staging-wait]`: **`capacity_splits=0`** en toda la partida (máximo 10 de 16 MiB de vértices y 8 de 24 MiB de uniformes). La hipótesis de las divisiones queda descartada.
- La espera es la del mapeo normal al empezar cada fotograma: 47-72 esperas lentas en 5 s, 3,0-3,4 s de espera y máximos de 62-186 ms. Es decir, la GPU no termina el fotograma anterior.
- `gpu.csv`: **utilización de la GPU al 100 % de 00:01:29 a 00:02:04** (P0, 1,76-1,95 GHz, 69 W, RTX 3050 de 6 GB), justo en las ventanas de 9-11 FPS; en juego normal, 30-60 %.
- `[gpu-pass]` de `diag_base` y `diag_all_off` en la cinemática: «Post SSAO effect mask» **5,3 ms de media por fotograma**, también con `effects.ssao=false`. La opción se activa con `ssao || ssr || wall_reflections` y `[effects] ssao_under_effects` (por defecto `true`, desde el commit `6e899df6` del 12/11/2026). El resto de los pases de posprocesado suman menos de 0,3 ms.
- Coste por tramo de efecto (niebla, explosiones, partículas) en `common.cpp` y `post::face_before`/`face_after`:
  - el pase del juego se corta en dos segmentos;
  - copia completa del EFB (`CopyTextureToTexture`, sin medir);
  - un pase de pantalla completa que compara antes y después.
  En una cinemática con cientos de tramos de efecto alternados con geometría, son cientos de copias y pases completos por fotograma, la mayoría después del pase 256 y fuera del medidor.
- El recuerdo del usuario de la cinemática fluida en Windows es probablemente anterior al 12/11. El código es el mismo en Windows, así que no sería un problema solo de Linux.
- Prueba pendiente: `play_variant.sh no_under_effects effects.ssao_under_effects=false`.
- Arreglo previsto si se confirma: limitar los tramos por fotograma (fusionar tramos vecinos cuando pasan de un presupuesto) y limitar copia y comparación al rectángulo de los dibujos del tramo.

### 26/11/2026: con `ssao_under_effects=false`, y arreglo de los tramos de efecto

- Variante `no_under_effects` (`/mnt/mp3port/builds/qa/manual/no_under_effects_1126_001023/`): de ~35 s a 9-11 FPS se pasa a **dos ventanas de 5 s, de 40 y 53 FPS**. Confirma que los tramos de efecto eran la causa principal.
- En esas ventanas la GPU va al 57-78 % (no saturada) y las imágenes salen cada 45-50 ms.
  - Por ventana de 5 s: ~465.000 dibujos (~2.300 por imagen, frente a ~400 en juego normal), `bp_apply` 6,6 M llamadas / 273 ms, productor GX ~1,6 s estimados.
  - Consumidor GX ocioso un 35 %; espera de staging de 9 ms como máximo.
  - Ningún hilo aparece saturado en los contadores. Sin F8: los FPS no bajaron de 30.
- Arreglo en `common.cpp`:
  - por pase, como máximo `kMaxEffectRuns = 8` tramos de efecto; por encima se unen los tramos vecinos por los huecos más pequeños, y los dibujos del hueco se miden con el efecto;
  - contador `[ssao-effect-runs] frames= runs= after_joining= passes_joined=` cada 5 s, solo cuando se une algo.
- `gpu_timing.cpp`: `kPairsPerFrame` de 256 a 2048, para medir el fotograma entero.
- Ejecutable `Corruption_3 PC.ram5`. Prueba: opciones por defecto, `WII_COMPILED_GPU_TIMING=1` y F8 automático por debajo de 50 FPS (`PROFILE_BELOW_FPS=50`).

### 26/11/2026: resultado del arreglo de tramos de efecto (`fix_effects`, ejecutable `ram5`)

Carpeta `/mnt/mp3port/builds/qa/manual/fix_effects_1126_001938/`; opciones por defecto, con `WII_COMPILED_GPU_TIMING=1`.

- `[ssao-effect-runs]`:
  - de 2.714 tramos en 5 s quedan 1.532 en el peor tramo (~5 por imagen);
  - en juego normal, 4-9 tramos por imagen, y solo se unen en pocos pases.
- Peor ventana: **50 FPS** (p95 44 ms) y 53 FPS, frente a 9-11 FPS antes del arreglo.
- `[gpu-time]` con el fotograma entero medido (hasta 2.048 pases):
  - en la cinemática, 9,4-9,8 ms por imagen y 60-75 pases;
  - en juego normal, 6,5-8 ms y 40-57 pases, de los que el posprocesado son 4-5 ms.
  La GPU ya no es el límite del bajón restante: está en la CPU (productor GX, juego o frame worker).
- El F8 automático (00:21:07) se disparó en un tramo sin caídas (0 de 10 s por debajo de 57) y no sirve. El usuario grabará el F8 a mano en la cinemática: dura 120 s, `kF8ProfileSeconds`.
- Aparte: el posprocesado cuesta 4-5 ms de GPU por imagen en juego normal. Revisarlo si se busca margen en GPU modestas.

### 26/11/2026: F8 manual del bajón restante (50 FPS) en la cinemática de Rundas

Perfil `stack_profile_20261126_003146.txt` (120 s) en `/mnt/mp3port/builds/qa/manual/f8_manual_1126_002723/`, ejecutable `ram5` (con `elide` y `proven-RAM`). Scripts en `/mnt/mp3port/builds/qa/rundas_cpu/`:
- `tree.py`: árbol de llamadas por debajo de una función;
- `selfs.py`: tiempo propio e inclusivo;
- `classify_game.py`: reparto del hilo del juego;
- `callers.py`: quién llama a una función;
- `addrs.py`: direcciones de muestras.

Corrección en `rundas_f8/window.py`: los símbolos débiles (`W`, plantillas de C++) no se contaban y su tiempo se atribuía a la función anterior. Por ejemplo, el 17,7 % del hilo GX que aparecía en el stub vacío `func_80173df8` era `PrimeDlVertices::ExpandMappedAttribute<2u>`.

Segundos 9-11 y 15 del perfil: 42-50 imágenes por segundo. Ocupación por hilo, en el bajón (s 9-12) frente a juego normal (s 30-60):

| Hilo | Bajón | Normal |
|---|---|---|
| juego | **88 %** | 44 % |
| GX | 79 % (19 % es la espera activa con `_mm_pause` en `GxGpuThread::Main`: ~60 % de trabajo real) | 39 % |
| decodificador | 32 % | 13 % |
| frame worker | 25 % | 15 % |
| presentador | 0,5 % | 0,9 % |

Con la GPU a ~10 ms por imagen, **el límite es el hilo del juego: ~20 ms de trabajo por fotograma lógico en los momentos más cargados, frente a 16,7 ms de presupuesto** (hace falta quitar ~20 %).

Reparto del hilo del juego en el bajón (`classify_game.py`):
- 65,2 % código traducido del juego;
- **14,7 % camino de escritura del FIFO de GX del port**: `GX_HLE_FIFO_Write*`, `HleFifoWrite`, `PrimeGpuCaptureCpuFifoFull`, `GxGpuThread::SubmitBurst`/`Push`. En juego normal, 4,2 %;
- 8,3 % resto del runtime (`InvokeIndirectCpu` 2,4 %, `PollBoundaryDevices` 1,7 %, IPC 1 %);
- 11,9 % esperas.

Código traducido:
- 62 % dibujo, bajo `fn_800E1478` > `fn_801AF4B4` > `fn_801AEE3C`, y 27,5 % actualización (`fn_800D700C`);
- el dibujo baja por `fn_8035F040` > `fn_80362B1C` > `fn_8036203C` > `fn_803EE30C` > `fn_8034F1AC` (37 %), sin una función dominante: el tiempo propio está repartido en muchas funciones de 0x8034xxxx-0x8037xxxx;
- ninguna está recuperada en `decomp/`.

FIFO:
- quienes escriben son las funciones de la SDK de GX en 0x804Dxxxx (`fn_804D62F8`, `804D4334`, `804D2A0C`, `804D6028`…) y `fn_80371708`, palabra a palabra;
- `PrimeGpuCaptureCpuFifoFull` (7,9 % inclusivo) cierra cada bloque de 32 bytes y lo entrega con `SubmitBurst` > `Push`: unos 650.000 bloques por segundo en el bajón.

Lista de lo que falta para este caso, por impacto estimado:
1. **Camino del FIFO** (~15 % del hilo del juego en el bajón):
   - entregar los bloques al hilo GX en tandas de 256 B a 1 KiB en vez de uno a uno, vaciando la tanda en los puntos de sincronización (breakpoint de CP, lecturas de registros PI/CP, draw sync, fin de fotograma);
   - versiones nativas de las funciones de la SDK que escriben vértices y matrices en la tubería, con escritura en bloque (`PrimeGpuGatherWriteBulk`).
2. **Activar `elide` y `proven-RAM` en los builds normales** (`build_prime_runtime.sh` y `.ps1`): −8 % del hilo del juego medido en el hangar; ya validados en juego. Los builds publicados aún no los llevan.
3. **Recargas de registros no volátiles**, la mejora (B) de la traducción, con su modo de verificación. Afecta a todo el 65 % de código traducido.
4. **Resto del runtime**: despacho de `InvokeIndirectCpu` (llamadas indirectas y vtables) y `PollBoundaryDevices`.
5. **A largo plazo**: código nativo recuperado de las funciones de dibujo del juego (0x8034-0x8037).

### 26-27/11/2026: C1-C4 implementados (sesión autónoma, a petición del usuario)

Cada mejora tiene su interruptor, para apagarla por separado si algo falla.

- **C1, builds normales.** `tools/build_prime_runtime.sh` y `.ps1` pasan `--ctx-flush-elision elide --proven-ram-accesses on` al emitir los shards. `MP3_CTX_FLUSH_ELISION=off|elide|verify` y `MP3_PROVEN_RAM_ACCESSES=on|off` los cambian. Los builds de Trilogy (`tools/trilogy/`, `PublicProducts.cmake`) no cambian: no se han validado allí.
- **C4, recargas no volátiles** (`ContextFlushElision.ElideNonvolatileReloads`, opción del traductor `--nonvolatile-reload-elision off|elide|verify`).
  - Qué se quita: la recarga de r1, r2 o r13-r31 justo después de una llamada directa traducida (`MKW_STATIC_TRANSLATED_CALL` o `InvokeDirectCpu<>`), cuando el análisis ya demuestra que el local y su hueco en `ctx` eran iguales antes de la llamada.
  - Por qué es correcto: por la EABI, el llamado guarda y restaura esos registros. El generador los recarga porque su máscara de posibles escrituras es una unión que incluye los que el llamado restaura.
  - Exclusiones: llamadas indirectas, los ayudantes `__save/__restore_fpr/gpr` (0x803059E4-0x80305B14) y los destinos que rompen la ABI.
  - Esos destinos salen de `FindUntrustedCallTargets(..., includeIndirectCallers: false)`: escriben huecos fuera de los volcados del generador, son nativos ganadores o llaman a uno de ellos.
  - Los destinos marcados solo por llegar a una llamada indirecta (la regla del 21/11 era por un registro **volátil**, r4) conservan ahora los pares no volátiles en el análisis.
  - FPR fuera: `_savefpr` no guarda la mitad ps1 de f14-f31.
  - Recuento: 377.576 de 384.688 recargas candidatas, más 1.682 volcados más que pasan a eliminarse (770.810 de 1.458.556).
  - En modo `verify`, cada recarga quitada pasa a ser `MKW_CTX_RELOAD_CHECK` (`ctx_flush_check.h`): avisa con `[ctx-flush-check] mismatch in <func> reload rN` y recarga.
  - Pruebas del traductor: 718/718.
- **C2, bloques del FIFO en tandas** (`hle/gx/gx_gpu_thread.cpp`).
  - `SubmitBurst` prepara hasta 16 bloques en una zona propia del hilo y los publica de una vez (`PublishBatch`: un cerrojo, una escritura de `tail` y una comprobación de despertar).
  - Se vacía antes de cualquier otra entrada (`Push`), en `Lock`/`HostLock`/`Sync`, `Idle()` (estado del CP), la parada de lectura del CP y `Shutdown`.
  - También desde fuera, con `GxGpuThread::FlushPending()`: lecturas MMIO (`Read*Slow`), sondeo de fronteras (`OS_HLE_PollPendingPrimeIpcInterruptFull`, cada 8 retornos), `VI_HLE_PollRetraceGuestSpin` y `OS_HLE_ServicePrimeIdle`.
  - Se publica desde una copia, por si la decodificación en línea vuelve a entrar con el anillo casi lleno.
  - `WII_COMPILED_PRIME_GX_BATCH=0` vuelve al bloque a bloque. `[prime-gpu-thread]` informa `batches=`.
- **C3, escritura en línea en la tubería de GX** (`guest_abi/gather_inline.h`, incluido en todos los shards de funciones, y `guest_abi/gather_state.h`).
  - `GX_HLE_FIFO_Write8/16/32/Float` del código traducido pasan a macros de envoltorios en línea con las mismas comprobaciones que `PrimeGatherAppendFast`. Solo la escritura que completa un bloque llama a la función de siempre.
  - `memory.cpp` exporta `g_mkwGatherState` con punteros a la captura y a los registros PI.
  - `-DMKW_NO_INLINE_GATHER=1` lo apaga.
  - Las rutas de 64 bits (`psq_st` y `stfd` a la tubería, vía `Write64Slow`) no cambian.
- **C6, sin cambios.** `InvokeIndirectCpu` ya tiene memo y ruta directa, y `PollBoundaryDevices` ya va cada 8 retornos con tres comprobaciones. No hay una mejora barata: queda en la lista con menos prioridad.
- Compatibilidad con Windows: solo C++20 estándar e `_mm_pause` (ya usado). Sin cambios específicos de plataforma. El build de Windows lo compila el usuario.
- Build de verificación: `build_shards_reload_verify` (volcados y recargas en `verify`, accesos a RAM demostrados, `gather_inline`), en `RM3E01_00_runtime_linux_ctxverify`.

### 26/11/2026 (noche): verificación y medición de C1-C4

- **Primer build de verificación** (repetición del hangar): un solo desajuste, `func_80006320 reload r1`.
  - Pila 0x81700000 → 0x8069a3a8, después de llamar a `func_8000648C`, el código de arranque que instala r1, r2 y r13 y pone a cero r14-r31 (rompe la ABI a propósito).
  - Corrección: regla genérica en `FindUntrustedCallTargets(..., includeIndirectCallers: false)`. Una función que carga una constante en r1, r2 o r13 (`AbiBaseConstantRegex`), y todas las que la llaman, no conservan la ABI.
  - Además, `BuildTransfer` consulta el conjunto ABI aunque el destino no esté en el general (el primer intento no lo hacía y el desajuste seguía).
  - Pruebas del traductor: 720/720.
  - Quedan 377.392 de 384.688 recargas eliminadas, y 770.806 volcados.
- **Escritura en línea en la tubería de GX:**
  - El primer intento salía vacío: los shards se compilan sin las definiciones del perfil (solo `-DNOMINMAX -DTARGET_PC`, `-std=gnu++17`).
  - Ahora `gather_inline.h` no depende de `MKW_GAME_PROFILE_RM3E01_00`. Un runtime sin el perfil define `g_mkwGatherState` como "reproduciendo" para siempre, y la ruta en línea siempre llama a la función.
  - Comprobado en el ejecutable: `func_804D62F8` lee `g_mkwGatherState` y solo conserva las llamadas de reserva.
- **Repetición del hangar con el build de verificación final** (`Corruption_3 PC.c14verify3`): **0 desajustes**, sin excepciones, `full_stalls=0`, ~2,2 bloques por publicación, 59,15 FPS de media.
- **A/B en el hangar** (perfil F8 de 300 s; `ram5` es el ejecutable anterior):

| Ronda | Antes (`ram5`) | Después (C1-C4) |
|---|---|---|
| 1 | 31,47 % | 28,57 % |
| 2 | 27,65 % | 28,07 % |

  El ruido entre ejecuciones (±2-3 puntos) es mayor que el efecto en esta escena ligera (FIFO ~3 %). Las mediciones de esa noche se hicieron con un C3 todavía vacío; con C3 activo no se repitieron. La medida útil es la cinemática de Rundas, con el usuario.
- `FlushPending` sale en el perfil (~1 %), casi todo dentro de las esperas ociosas (`VI_HLE_PollRetraceGuestSpin`, espera del breakpoint de CP): tiempo libre.
- `play_variant.sh` acepta `MP3_EXE` para jugar otro ejecutable del directorio de build.
- `build_prime_runtime.sh` y `.ps1` aceptan `MP3_NONVOLATILE_RELOAD_ELISION` (por defecto `elide` desde la validación del 26/11, ver la sección siguiente).
- Ejecutables en `RM3E01_00_runtime_linux_ctxverify`:
  - `Corruption_3 PC` = `.c14verify3` (verificación, más lento);
  - `.c14elide3` (final, para medir).
- Pendiente del usuario:
  1. Partida de verificación con `Corruption_3 PC` por zonas variadas, y revisar `[ctx-flush-check]` en el log.
  2. Medición con F8 en la cinemática de Rundas con `MP3_EXE="Corruption_3 PC.c14elide3"`.
  3. Si todo va bien: `MP3_NONVOLATILE_RELOAD_ELISION` a `elide` por defecto, commit y build de Windows.
### 26/11/2026: partidas del usuario con C1-C4

Tres partidas a mano con `play_variant.sh` en `RM3E01_00_runtime_linux_ctxverify` (carpetas `verify_c14_1126_*` en `/mnt/mp3port/builds/qa/manual/`):

| Partida | Ejecutable | Duración | Cinemática de Rundas (ventanas de ~5 s) |
|---|---|---|---|
| 08:07 | verificación (`.c14verify3`; texto mapeado 0xffd6000) | ~26 min | 47,5 FPS (el build de verificación es más lento) |
| 14:22 | final (C1-C4 `elide`) | ~77 min | no llega a la cinemática (bajón de 56 en otra escena) |
| 18:18 | final (C1-C4 `elide`; texto mapeado 0xa94b000) | ~38 min | 54,9 y 58,1 FPS (p95 31 y 28 ms) |

- **Verificación:** 0 líneas `[ctx-flush-check]` en la partida de 08:07, que pasó por la cinemática. Con la repetición del hangar, son dos validaciones sin desajustes. C4 pasa a `elide` por defecto en `build_prime_runtime.sh` y `.ps1`.
- **Ojo:** a las 08:41 otra sesión de trabajo (textos traducidos) recompiló `RM3E01_00_runtime_linux_ctxverify` con el manifiesto apuntando a `build_shards_c4_elide`. Desde entonces `Corruption_3 PC` es el build final y no el de verificación. Las dos partidas siguientes lo usaron, y que no tengan `[ctx-flush-check]` no dice nada. Para saber cuál se jugó, mirar el tamaño del módulo en el `stack_profile` o las cadenas `reload r` del ejecutable.
- **Cinemática de Rundas.** Se identifica por la entrada RSO `nominal=0x8069157c lr=0x8027d460` justo antes del bajón.

| Partida | Ventanas bajas | Imágenes perdidas aprox. |
|---|---|---|
| `fix_effects` (antes de C1-C4, con `GPU_TIMING`) | 50,3 y 53,3 | ~82 |
| `f8_manual` (con F8 y `GPU_TIMING`, que la cargan más) | 44,7 y 49,5 | ~129 |
| C1-C4 (18:18, sin `GPU_TIMING`) | 54,9 y 58,1 | ~35 |

  Las condiciones no son idénticas (`GPU_TIMING` y F8 añaden coste), pero el bajón es mucho más corto y el usuario solo notó ~55 FPS.
- **La GPU no es el límite en el bajón restante:** como mucho 59-82 % en `gpu.csv` durante la cinemática. `present_entered_ms` llega a 11-14 ms y no hay compilaciones de pipelines en esas ventanas. Lo que queda es tiempo del hilo del juego (C5-C9).
- **Bajones aislados en otras escenas** (una ventana de ~55 FPS, p95 20-25 ms, ~25 imágenes): 18:18 ventanas 84, 145 y 259. Probablemente cargas de sala. El usuario no los notó.
- **Funcionalidad:** el build final se jugó ~2 h en total sin cierres ni errores, con `full_stalls=0` en el hilo GX y ~2-3 bloques por publicación en las tandas.


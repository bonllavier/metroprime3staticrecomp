# Experimento de paralelismo

La rama `parallel-experiment` parte del checkpoint publicado `before-parallelism`. `main` conserva esa base estable. El primer candidato divide en rangos la expansi?n de v?rtices indexados de display lists: un pool persistente de hasta dos workers trabaja junto con el hilo que llam? al renderer. La llamada espera a que termine la expansi?n antes de continuar con GX; no cambia el orden de comandos ni el estado del juego.

Solo se intenta en dibujos de atributos indexados que ya tienen probado el dominio completo de sus arrays, con al menos 512 v?rtices y 64 KiB de salida. Se reparten rangos disjuntos de 256 v?rtices. Dibujos peque?os, memoria no validada, equipos con dos o menos procesadores l?gicos y fallos al crear workers siguen por la ruta escalar existente. Los datos se leen de nuevo en cada dibujo; no hay cach? entre frames.

La prueba `mkw_prime_dl_vertex_expansion_tests` compara byte por byte el resultado paralelo con la ruta escalar en un dibujo grande y verifica que una llamada posterior observe cambios en la memoria fuente. El test focalizado y la suite de plataforma pasaron en la m?quina de desarrollo. El renderer tambi?n compil?; como el ejecutable habitual estaba abierto y bloqueado por Windows, el enlace se verific? con el nombre alternativo `WiiCompiled_parallel_candidate.exe`. Todav?a no hay medici?n de FPS en juego ni evidencia de que esta fase sea el cuello dominante.

Para abrir el candidato con teclado/raton, cierra primero la otra instancia del juego y ejecuta desde la raiz del proyecto `G:\gamecube_roms\metroid3_wii`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\recomp\tools\run_parallel_candidate.ps1
```

Este lanzador fija `WII_COMPILED_PRIME_KEYBOARD_MOUSE=1` solo para el proceso hijo y lo inicia desde su carpeta de build. Abrir el EXE directamente deja desactivado el puente de teclado/raton.

Para repetir las pruebas en PowerShell desde `recomp/`:

```powershell
$env:PATH = (Resolve-Path .tools\portable-tools\llvm-mingw\bin).Path + ';' + $env:PATH
cmake --build build/runtime-platform-tests --target mkw_prime_dl_vertex_expansion_tests --parallel 4
ctest --test-dir build/runtime-platform-tests -R '^mkw_prime_dl_vertex_expansion_tests$' --output-on-failure
ctest --test-dir build/runtime-platform-tests --output-on-failure
```

La siguiente validaci?n es medir, en la misma escena y resoluci?n, FPS, tiempo de expansi?n y uso por hilo con el candidato frente a la base. Si el coste no baja, ajustar el umbral o retirar esta ruta. No se debe atribuir una mejora de rendimiento al cambio antes de esa comparaci?n.

## Medicion y candidato del 2026-10-23

La captura de la escena pesada registro 23-24 FPS, un hilo del runtime alrededor del 99-101%, GPU entre 2.6-4.0% y unas 78-90 mil llamadas GX por ventana de cinco segundos. Las estimaciones del profiler son inclusivas: Draw contiene Textures, que contiene StaticTexture; no se deben sumar entre si. En estos datos el camino de texturas estaticas es el subcosto dominante observado.

Las texturas GX sin contrato de generacion se siguen validando byte por byte, pero su cache LRU buscaba la clave recorriendo linealmente hasta 512 entradas. Se agrego un indice hash para las consultas de snapshots y paletas; las colisiones todavia verifican la clave completa y los bytes, y los desalojos quitan tambien el indice. La prueba cubre una clave fria entre 262 entradas (una comparacion), colisiones y eviction. Build Windows y CTest 21/21 pasan. La version de juego aun no se ha medido; el proceso previo se mantuvo abierto para no interrumpir el avance.

El lanzador ahora abre el `WiiCompiled.exe` recompilado y conserva teclado/raton. `-PerformanceTrace` activa el registro de aciertos/fallos del cache de snapshots y los contadores de GX. Despues de guardar y cerrar la sesion activa, iniciar desde la raiz con:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\recomp\tools\run_parallel_candidate.ps1 -PerformanceTrace
```

Comparar la misma vista durante al menos 15 segundos; revisar `UserData/Logs/<sesion>/console.log` para `[prime-raw-cache]` y `[prime-raw-palette-cache]`. Esta mejora reduce busqueda de claves; no prueba por si sola que se alcancen 60 FPS.
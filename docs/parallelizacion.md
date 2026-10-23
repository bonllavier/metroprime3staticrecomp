# Experimento de paralelismo

La rama `parallel-experiment` parte del checkpoint publicado `before-parallelism`. `main` conserva esa base estable. El primer candidato divide en rangos la expansi?n de v?rtices indexados de display lists: un pool persistente de hasta dos workers trabaja junto con el hilo que llam? al renderer. La llamada espera a que termine la expansi?n antes de continuar con GX; no cambia el orden de comandos ni el estado del juego.

Solo se intenta en dibujos de atributos indexados que ya tienen probado el dominio completo de sus arrays, con al menos 512 v?rtices y 64 KiB de salida. Se reparten rangos disjuntos de 256 v?rtices. Dibujos peque?os, memoria no validada, equipos con dos o menos procesadores l?gicos y fallos al crear workers siguen por la ruta escalar existente. Los datos se leen de nuevo en cada dibujo; no hay cach? entre frames.

La prueba `mkw_prime_dl_vertex_expansion_tests` compara byte por byte el resultado paralelo con la ruta escalar en un dibujo grande y verifica que una llamada posterior observe cambios en la memoria fuente. El test focalizado y la suite de plataforma pasaron en la m?quina de desarrollo. El renderer tambi?n compil?; como el ejecutable habitual estaba abierto y bloqueado por Windows, el enlace se verific? con el nombre alternativo `WiiCompiled_parallel_candidate.exe`. Todav?a no hay medici?n de FPS en juego ni evidencia de que esta fase sea el cuello dominante.

Para repetir las pruebas en PowerShell desde `recomp/`:

```powershell
$env:PATH = (Resolve-Path .tools\portable-tools\llvm-mingw\bin).Path + ';' + $env:PATH
cmake --build build/runtime-platform-tests --target mkw_prime_dl_vertex_expansion_tests --parallel 4
ctest --test-dir build/runtime-platform-tests -R '^mkw_prime_dl_vertex_expansion_tests$' --output-on-failure
ctest --test-dir build/runtime-platform-tests --output-on-failure
```

La siguiente validaci?n es medir, en la misma escena y resoluci?n, FPS, tiempo de expansi?n y uso por hilo con el candidato frente a la base. Si el coste no baja, ajustar el umbral o retirar esta ruta. No se debe atribuir una mejora de rendimiento al cambio antes de esa comparaci?n.

# Ultrawide (Hor+) en RM3E01

Petición: issue #7 de GitHub (3440x1440, 21:9). Autorizada por el usuario el 11/11/2026 junto con los arreglos del disco de las discusiones #4 y #5.

## Qué hace

Con el modo 16:9 del juego activo, la opción `[video] aspect_ratio` (F1 > Video > *Aspect ratio*) ensancha la vista en horizontal sin tocar la vertical (Hor+):

| Valor | Efecto |
|---|---|
| `"16:9"` (por defecto) | Igual que antes: 16:9 con barras en otras formas de ventana. |
| `"window"` | Sigue la forma de la ventana o del monitor cuando es más ancha que 16:9. En un monitor 16:9 es idéntico a `"16:9"`. |
| `"21:9"` | 64:27 (2,37), con barras si la ventana es más ancha. |
| `"32:9"` | 3,56. |
| `"W:H"`, `"WxH"`, `"2.39"` | Forma libre (Config.toml), recortada a [16:9, 4,0]. |

En 4:3 no hace nada. Se aplica en vivo.

## Cómo calcula el juego su vista (direcciones RM3E01, comprobadas en el DOL)

- `0x800BE034 (w, h)` devuelve el aspecto del viewport: `tvFactor * wideFactor * w / h`. `tvFactor` sale de `0x801B1374`: 1,4685 con la bandera de 16:9 (`0x8067DF01`), 1,335 en 4:3. `wideFactor` vale 0,93333 en 16:9. La función hoja `0x801B1374` está incrustada en la traducción de `0x800BE034`.
- `0x800BF0C4` guarda ese valor en `SViewportRenderData+0x68`. A continuación, `0x800BE0DC` construye con él:
  - los planos de descarte, con `0x8036F6AC (aspect, near, far, transform, fov)`, a partir de la amplitud horizontal `fov/2 * aspect`;
  - la matriz de proyección `+0xD0` (`0x803E9384`), que usan `CGameViewport::ConvertToScreenSpace`/`ToWorldSpace`/`ToNormalisedSpace`.
  
  Solo lo hace cuando el bit 0x80 de `CGameViewport+0x58` está activo. Ese bit lo pone `0x800BDFE0`.
- Los otros llamadores de `0x800BE034` son `0x802D3850`, que pasa `CGraphics::mViewport` (`0x8059E188`: left, top, width, height), y `0x800BEE48`.
- `CGraphics::SetPerspective` es `0x803E91D4` (fov, aspect, near, far) y `SetOrtho` es `0x803E9158`. Ambas guardan el estado `lbl_805CDCE0` (byte de perspectiva; left, right, top, bottom, near, far) y llaman a `0x803EB060`, que construye la matriz con `C_MTXFrustum` (`0x80506538`) o `C_MTXOrtho` (`0x805066C0`) y llama a `GXSetProjection`.
- Las cámaras de interfaz (`CGuiCamera`, `0x80433BD8`) tienen su propio aspecto en `+0xD4` (tipo en `+0xCC`: 0 perspectiva, 1 ortográfica, l/r/t/b/n/f en `+0xD0..+0xE4`). `DrawSqueezedForWidescreen` es su variante de 16:9.
- Las cinemáticas convierten FOV horizontal a vertical con 16:9 fijo (`0x80054360`, `ConvertHorizontalFOVToVerticalFOV(…, 1.7777)`).

## Implementación

- **Modelo** `runtime/include/hle/prime_widescreen.h`, con las pruebas de `mkw_prime_widescreen_tests`. Cubre la lectura del valor, la forma objetivo, el factor (objetivo / 16:9), la prueba de «viewport de pantalla completa» y el ensanchado del aspecto y del frustum.
- **Runtime** `runtime/src/hle/prime_widescreen.cpp`, en el hilo del juego:
  - `PrimeWidescreenOnSurface`, en cada frame desde `UpdateMkwDynamicAspectSurface`:
    - calcula la forma y llama a `AuroraSetFixedAspect`;
    - si cambia el factor, marca como sucios los `CGameViewport` vistos (vtable `0x80591498`, bit 0x80 de `+0x58`), para que el juego recalcule proyección y descarte;
    - mientras el decodificador de vídeo (`0x804EB174`) produce frames y 20 frames después, presenta en 16:9 con factor 1. Así los vídeos THP, que se dibujan como un cuadro a pantalla completa, no se estiran.
  - `PrimeWidescreenViewportAspect`, gancho posterior de `0x800BE034`: multiplica el aspecto por el factor solo para el viewport que cubre la pantalla. Así coinciden proyección, descarte y conversiones a pantalla, y el FOV vertical no cambia.
  - `PrimeWidescreenGuiAspect`, a la entrada de `CGraphics::SetPerspective` (`0x803E91D4`): multiplica el aspecto si quien llama es una cámara de interfaz (retorno `0x80433C80` o `0x80433DA8`). El estado de proyección del juego guarda el valor ensanchado, de modo que las copias que el juego guarda y restaura (`0x80045DB8`, `0x80048250`) siguen coherentes.
  - `PrimeWidescreenFlush`, en `0x803EB060` antes de la matriz: en la versión publicada solo observa. Con `WII_COMPILED_PRIME_UW_FLUSH=1` (build de pruebas) reproduce la política rechazada, que ensanchaba toda perspectiva sobre un viewport de pantalla completa.
  - El tamaño de pantalla es el viewport más grande que ha puesto el juego (640x448, el EFB). Las sombras y otras texturas renderizadas usan viewports menores.
- **Overlays** en `tools/prepare_prime_runtime_hooks.py`: `0x800BE034`, `0x800BF0C4`, `0x803E91D4`, `0x803EB060` y una línea más en el de `0x804EB174`. No se parchea el DOL.
- **Aurora**: `AuroraSetFixedAspect(float)` (GXAurora) da forma a la política `AURORA_VIEWPORT_16_9`, que antes era 16:9 fijo en `window.cpp`. El framebuffer interno crece en ancho (`scale_framebuffer_to_aspect`) y la presentación deja barras si la ventana es más ancha.
- **Traza** (solo en la build de pruebas): `WII_COMPILED_PRIME_PROJECTION_TRACE=1` registra cada proyección distinta con su pila de llamadas (`[prime-uw-trace]`) y un recuento cada 10 s.

## Por qué no se ensancha en el flush (medido el 11/11/2026)

Primera versión: toda proyección en perspectiva sobre un viewport de 640x448 cuyo aspecto no viniera del viewport ensanchado se ensanchaba en `0x803EB060`. La traza del recorrido `hangar_uw21` mostró que también afectaba a:
- el render del icono Mii (`CMiiHelper::GenerateIcon`, retorno `0x800F27F4`);
- una proyección cuadrada (aspecto 1,0, `0x802C8238`, en `0x802C7DD4`) que luego dibuja a 64x64;
- las proyecciones que el juego restaura desde copias guardadas (`0x80045DB8`, `0x80048250`), que quedaban unas veces ensanchadas y otras no.

Por eso se ensancha en origen, en el viewport y en las cámaras de interfaz.

## Pruebas (11/11/2026, build local con QA)

- **Unitarias**: `mkw_prime_widescreen_tests` (aceptadas por `test_graphics.ps1`: 286 de Aurora y 32 del runtime, todas en verde).
- **Repeticiones** del recorrido `hangar` (copias `hangar_uw16`, `hangar_uw21` y `hangar_uw32` en `recomp/qa/recordings`, que solo cambian `aspect_ratio`), en pantalla 1920x1080 sin bordes:
  - 21:9: mundo Hor+ sin deformación. El HUD del casco queda exactamente donde estaría en 16:9, centrado (la munición cae en x = 240 + 0,75 × su x en 16:9). Los filtros del visor (desenfoque al aterrizar, visor gris) cubren todo el ancho. 59,9 FPS de media, sin errores.
  - 32:9: lo mismo con factor 2. 59,98 FPS de media, mínimo 59.
  - Vídeos THP: detectados solo en el título y la introducción (frames 209-5079), presentados en 16:9 y nunca durante el juego.
- **Ventana**: con `"window"`, 1680x720 da 2,333 (factor 1,3125) y 1280x720 da factor 1. Al cambiar de tamaño en vivo (1704x721 → 2,363; 2384x661 → 3,607; 984x721 → 1,0) se actualiza sin errores.
- **16:9**: con factor 1 los ganchos devuelven los valores originales (el aspecto ya es de precisión simple). La repetición `hangar_uw16` se vio como siempre; no se hizo una comparación píxel a píxel.

## Pendiente (QA del usuario)

- Menús de pausa, mapa, registro de escaneos y visores (rayos X, mando) con un monitor ultrawide real. Estas pantallas no están en el recorrido grabado.
- Cinemáticas en tiempo real: usan el viewport, así que deberían ser Hor+.
- Coste de GPU en 3440x1440 nativo (34 % más píxeles que 2560x1440).
- Trilogy: el mapa de direcciones traslada las constantes; falta verlo dentro de MP3 desde el menú de Trilogy.

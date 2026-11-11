# Campo de visión en primera persona (RM3E01)

Petición del usuario (31/10/2026): poder ajustar el FOV de la cámara de primera persona, activable y desactivable, emulando lo que hace PrimeHack. Implementado y compilado. Falta la QA en el juego.

## Referencia

PrimeHack (`shiiion/dolphin`, rama `primehack-master`), `Source/Core/Core/PrimeHack/Mods/ViewModifier.cpp` y `AddressDBInit.cpp`, entradas `PRIME_3_STANDALONE` NTSC-U. PrimeHack es GPL: se usó como referencia y no se copió código. Las direcciones se comprobaron contra nuestro DOL (dtk `decomp/build/RM3E01_00/asm` y Ghidra):

| Dirección | Original | Qué es |
|---|---|---|
| 0x8009CF90 | `stwu r1,-0x20(r1)` | Fija el FOV de la cámara: obtiene el componente 5 (`0x8009CED0`, cámara +0x178) y llama a `0x8007F0BC`, que escribe +0x18 y +0x1C. |
| 0x800DD87C, 0x800DCEB0, 0x8017D40C | `bl 0x8009CF90` | Las tres llamadas que PrimeHack anula. |
| 0x8007F504 | `bl 0x8007F0AC` | Paso de suavizado: guarda el FOV actual (+0x18) mientras se acerca al objetivo (+0x1C). Anulado en PrimeHack. En la traducción, la hoja está incrustada en `func_8007F2A4`. |
| 0x802B26C0 | `stw r0,0x14(r6)` | Dentro de `0x802B2668`: cambia el id de la cámara activa. PrimeHack aplica el FOV ahí mismo con una vmcall. |
| 0x80316A1C | `lfs f0,0xc(r3)` | Prueba de caja contra plano del descarte. PrimeHack la cambia por `li r3,1; blr` cuando el FOV pasa de 94° o lo pide el jugador. |

Cadenas de punteros (cada desplazamiento menos el último se desreferencia): lista de objetos `[[0x805C4F98]+0x1010]`; id de la cámara activa (u16) en `[[0x805C4F98+0x10]+0xC]+0x16`; cámara = `[lista + (id & 0x7FF)*8 + 4]`; si `[cámara+0x2E4]` es válido y su byte +0x38 tiene el bit 0x20, hay una cinemática y no se toca. Los parámetros `[cámara+0x178]` guardan el FOV actual en +0x18 y el objetivo en +0x1C. Brazo: `[[0x8067D78C]]+0xE0` (izquierda, adelante, arriba). Plano cercano: `[[cámara_activa+2]+0x14]+0x8C`.

## Implementación

- **Modelo** `runtime/include/hle/prime_fov.h`, con las pruebas de `mkw_prime_fov_tests`: rango 40–170 (PrimeHack recorta en 170), umbral de descarte de 94°, las tres direcciones de retorno anuladas y la colocación automática del brazo. Por debajo de 75° el brazo queda igual. Entre 75° y 125° se interpola hacia (0,22; −0,02). El plano cercano baja a 0,1 desde 75°.
- **Runtime** `runtime/src/hle/prime_fov.cpp`: `PrimeFov::Frame()` se ejecuta en cada retrazado, en el hilo del juego (`vi.cpp`, `AdvanceRetrace`). Escribe el FOV en la cámara activa y en la cámara 4, como PrimeHack. Coloca el brazo partiendo de los valores que tenía el juego, no de constantes. Al desactivar la opción, devuelve a la cámara activa, la cámara 4, el brazo y el plano cercano los valores originales guardados.
- **Overlays** (`tools/prepare_prime_runtime_hooks.py`). No se parchea el DOL, solo el C++ traducido de cada función. Todos actúan solo con la opción activa:
  - `func_8009CF90`: vuelve sin hacer nada si `ctx->lr` es una de las tres direcciones de retorno.
  - `func_8007F2A4`: el guardado incrustado de 0x8007F504 queda condicionado.
  - `func_802B2668`: llama a `PrimeFovOnCameraChange(r0)` después del guardado.
  - `func_80316A1C`: devuelve `r3 = 1` cuando el descarte está desactivado.
- **Opciones** de `[prime]` en Config.toml: `fov_enabled` (desactivada por defecto), `fov` (75 por defecto), `fov_arm_adjust` (activada), `fov_culling_off` (desactivada). Están en F1 > Vídeo, se aplican en vivo y muestran el FOV propio del juego en cuanto se lee.
- **Registro**: `[prime-fov]` registra el FOV original de la primera cámara, la posición original del brazo y cada activación o desactivación.

## Pendiente (QA en el juego)

- Que el FOV cambie en vivo y se mantenga al entrar y salir de la morfosfera, al cambiar de visor, en el modo hiper y al apuntar con Z.
- Que las cinemáticas conserven su FOV y que al desactivar la opción vuelva el del juego.
- Que el brazo se vea bien entre 75° y 125°.
- Rendimiento por encima de 94°: con el descarte desactivado se dibuja todo el escenario. Hay que medirlo con F8 en Norion. Si cuesta demasiado, la alternativa es ensanchar la región de descarte en lugar de anularla.

# Payloads de prueba — DuckyScript 3.0 sobre ESP32-S3 (Linux objetivo)

Estos payloads cubren las **Fases 1, 2 y 3** ya implementadas. Son **inocuos**:
ninguno escribe en disco, no escala privilegios, no toca red, no instala
nada. Imprimen información o abren ventanas habituales del escritorio Linux.

**Antes de probar:** abre una terminal foco (gnome-terminal / xterm / kitty)
con el cursor parpadeando, salvo en los payloads que abren su propia ventana.
Asegúrate de que el layout del macro coincide con el del sistema (`es`, `us`,
etc.); por defecto los ejemplos asumen `us`.

> Convenciones del intérprete actual:
> - `//` y `REM` son equivalentes (Fase 1).
> - `DEFAULT_DELAY n` añade `n` ms a cada comando físico (Fase 1).
> - `JITTER n` añade hasta `n%` aleatorio a cada espera (Fase 1).
> - `$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON` reflejan el estado real
>   del host enviado por el SO al endpoint OUTPUT (Fase 2).
> - `VAR $X = <expr>`, `DEFINE #X valor`, `RANDOM_INT(min,max)` y aritmética
>   `+ - * / % & | ^ && ||` con comparaciones (Fase 3).
> - `IF/ELSE/WHILE/FUNCTION` están parseados pero **no se ejecutan aún**
>   (llegan en Fase 4). Estos payloads no los usan.

---

## 1) Smoke test mínimo (alias `//`, ENTER y aliases de flechas)

Verifica el alias `//` como REM, la tecla `ENTER` y los nuevos alias de
flechas (`UP`, `DOWN`).

```ducky
// Smoke test - prints hello and moves the cursor with the new aliases
DELAY 500
STRINGLN echo "hello from ESP32-S3"
DELAY 200
UP
DOWN
```

**Esperado:** se imprime `hello from ESP32-S3` y se ven dos movimientos del
cursor (arriba/abajo) en el historial / línea actual del shell.

---

## 2) Información del sistema (DEFAULT_DELAY + comentarios mixtos)

Demuestra `DEFAULT_DELAY` (humaniza el ritmo) y comentarios con `//` y
`REM` mezclados.

```ducky
REM Show host identity
// Set a 60 ms global pacing so the tap rate looks human
DEFAULT_DELAY 60
DELAY 400
STRINGLN whoami
STRINGLN hostname
STRINGLN uname -a
STRINGLN date '+%Y-%m-%d %H:%M:%S'
```

**Esperado:** cuatro líneas de salida; cada tecla cae con ~60 ms de
separación natural.

---

## 3) Humanización con JITTER (evasión básica)

Misma idea que el anterior, pero con `JITTER` para variar cada delay
hasta un ±20 %.

```ducky
REM Same as before but with random jitter to evade simple timing IDS
DEFAULT_DELAY 80
JITTER 30
DELAY 500
STRINGLN echo "jittered typing test"
STRINGLN uptime
STRINGLN free -h
STRINGLN df -h --output=source,size,avail,target / 2>/dev/null
```

**Esperado:** el ritmo entre teclas debe variar visiblemente entre
intentos. `df` y `free` muestran info inocua.

---

## 4) Variables, DEFINE y aritmética (Fase 3)

Demuestra `DEFINE` léxico, `VAR` con expresión y `DELAY` con expresión.

```ducky
REM Phase 3: variables and math
DEFINE #BASE 25
DEFINE #SCALE 10
VAR $N = #BASE + #SCALE * 2
VAR $WAIT = $N * 20
DELAY $WAIT
STRINGLN echo "Sleep was $N x 20 ms"
DELAY ($N % 7) * 100 + 50
STRINGLN echo "Second wait was a mod-based expression"
```

**Esperado:** se calcula `$N = 45`, espera 900 ms, imprime la línea;
después espera `(45 % 7) * 100 + 50 = 350 ms` y emite la segunda línea.

---

## 5) RANDOM_INT dentro de expresión

Genera dos enteros pseudoaleatorios y los imprime; sirve también para
verificar que `esp_random` se invoca por `RANDOM_INT(min,max)`.

```ducky
REM Use RANDOM_INT to vary behavior between runs
VAR $A = RANDOM_INT(1, 9)
VAR $B = RANDOM_INT(10, 20)
VAR $SUM = $A + $B
DELAY 300
STRINGLN echo "A=$A B=$B SUM=$SUM"
DELAY RANDOM_INT(200, 800)
STRINGLN echo "Random sleep done"
```

> Nota: dentro de `STRING/STRINGLN` los `$A` literales **no** se expanden:
> los tipa el shell de Linux como variables vacías. Si quieres que se vean
> los números reales tendrás que esperar a la Fase 4 (interpolación) o
> imprimirlos vía `expr` en el shell.

**Esperado:** el segundo `STRINGLN` se imprime tras un sleep aleatorio
entre 200 y 800 ms.

---

## 6) Teclas nuevas: PRINTSCREEN, MENU, PAUSE

Comprueba los keycodes añadidos en Fase 1. En GNOME `PRINTSCREEN` abre el
recortador; `MENU` muestra el menú contextual; `PAUSE` no suele tener
acción visible (útil para registro USB).

```ducky
REM Verify the new key tokens (no permanent effect)
DELAY 600
PRINTSCREEN
DELAY 800
ESCAPE
DELAY 300
MENU
DELAY 600
ESCAPE
DELAY 200
PAUSE
```

**Esperado:** salta el atajo de captura del entorno (cancelado con
ESC), después un menú contextual (cerrado con ESC) y por último un evento
`Pause` registrado por el sistema (sin efecto).

---

## 7) Modificadores con alias `CONTROL` y `OPTION`

Verifica que `CONTROL` → CTRL y `OPTION` → ALT. Abre un nuevo prompt en la
misma terminal con `CTRL-D` cancelando si hace falta. En este ejemplo solo
selecciona toda la línea y la borra.

```ducky
DELAY 400
STRING this text will be selected and erased
DELAY 300
CONTROL a
DELAY 150
OPTION BACKSPACE
DELAY 200
STRINGLN echo "line cleared"
```

**Esperado:** la primera frase aparece, se selecciona con `CTRL-A`,
`ALT-Backspace` borra la palabra (o la línea, según el shell readline) y
luego se confirma con un mensaje limpio.

---

## 8) LED feedback — payload condicional sin IF (Fase 2)

`IF` aún no se ejecuta, pero podemos usar el evaluador de Fase 3 para
**multiplicar el delay por `$_CAPSLOCK_ON`**: si CapsLock está activado,
hay una pausa larga visible; si está apagado, la pausa es casi nula.

```ducky
REM If CapsLock is ON the macro waits 3 seconds before typing;
REM if CapsLock is OFF it types almost immediately. Reactive to host LEDs.
DELAY 500
VAR $WAIT = $_CAPSLOCK_ON * 3000 + 50
DELAY $WAIT
STRINGLN echo "Detected CapsLock=$_CAPSLOCK_ON NumLock=$_NUMLOCK_ON"
```

**Esperado:** ejecuta primero con CapsLock OFF (responde inmediato).
Activa CapsLock y vuelve a lanzar: ahora espera ~3 s antes de escribir.
El `$_CAPSLOCK_ON` literal sí se imprime como texto, pero el comportamiento
real (el delay) sí refleja el estado.

---

## 9) Combinación `NumLock + Caps` (lectura simultánea)

Demuestra que ambos estados se leen sin reentrar al host.

```ducky
DELAY 300
VAR $BITS = $_CAPSLOCK_ON * 2 + $_NUMLOCK_ON
VAR $WAIT = $BITS * 700 + 100
DELAY $WAIT
STRINGLN echo "BITS value drove a host-aware delay"
```

**Esperado:** prueba 4 combinaciones (00, 01, 10, 11) → 100, 800, 1500
y 2200 ms respectivamente.

---

## 10) Stress test del parser (todos los alias en orden)

Útil para regresión rápida tras tocar el parser.

```ducky
// Aliases regression: //, UP, DOWN, LEFT, RIGHT, ESC, CONTROL, OPTION
DEFAULT_DELAY 40
JITTER 15
DELAY 200
STRING parser-regression-
UP
DOWN
LEFT
RIGHT
ESC
DELAY 100
CONTROL a
OPTION BACKSPACE
DELAY 200
STRINGLN echo "regression OK"
```

**Esperado:** acaba con `regression OK` en una línea limpia. Si algún
alias se rechaza, el ejecutor pondrá el estado en `ERROR` y la línea
exacta saldrá en `usb_hid_exec_status_t.message`.

---

## 11) Compatibilidad con `DEFAULTDELAY` (sin guion bajo)

Un atajo en la documentación Hak5: `DEFAULTDELAY 50` debe comportarse
igual que `DEFAULT_DELAY 50`.

```ducky
DEFAULTDELAY 50
DELAY 300
STRINGLN echo "DEFAULTDELAY accepted"
```

---

## 12) Fail-safe — `STOP_PAYLOAD`

Verifica que `STOP_PAYLOAD` corta limpio (estado `DONE` y release de
modificadores). Útil como cierre defensivo en cualquier macro.

```ducky
DEFAULT_DELAY 30
DELAY 200
STRINGLN echo "about to stop the macro"
DELAY 200
STOP_PAYLOAD
REM the following line must NOT be executed
STRINGLN echo "you should NOT see this line"
```

**Esperado:** solo se imprime la primera frase; la segunda no aparece.

---

## Tabla resumen de cobertura

| Payload | Fase 1 | Fase 2 | Fase 3 |
|---------|:------:|:------:|:------:|
| 1  Smoke / aliases flecha           | ✅ | – | – |
| 2  DEFAULT_DELAY + comentarios `//` | ✅ | – | – |
| 3  JITTER (evasión)                 | ✅ | – | – |
| 4  VAR / DEFINE / DELAY $expr       | – | – | ✅ |
| 5  RANDOM_INT en expresión          | – | – | ✅ |
| 6  PRINTSCREEN / MENU / PAUSE       | ✅ | – | – |
| 7  CONTROL / OPTION aliases mod     | ✅ | – | – |
| 8  LED CapsLock condicional         | – | ✅ | ✅ |
| 9  Combo NumLock+Caps               | – | ✅ | ✅ |
| 10 Regresión parser completa        | ✅ | – | – |
| 11 DEFAULTDELAY alias               | ✅ | – | – |
| 12 STOP_PAYLOAD fail-safe           | ✅ | – | – |

---

## Cómo verificar paso a paso

1. **Modo dry-run** (sin USB conectado): los payloads deben superar el
   parser sin error y mostrar mensajes informativos por línea.
2. **Modo real** (USB-HID conectado a un PC Linux):
   - Carga el payload por la web UI.
   - Comprueba que `usb_hid_exec_status_t.state` evolucione
     `RUNNING → DONE` y que `current_line` cubra toda la macro.
   - Para los payloads de LED, alterna CapsLock/NumLock entre
     ejecuciones para ver diferencias deterministas en el `DELAY`.
3. **Verificación de jitter**: lanza el payload 3 (con `JITTER 30`) varias
   veces y mide el tiempo total con un cronómetro: debe oscilar entre
   ejecuciones aun con el mismo script.
4. **STOP de emergencia**: durante un payload largo, llama al endpoint
   `panic_stop` desde la web — la macro debe pasar a `STOPPING/DONE` y
   liberar todos los modificadores en menos de 250 ms.

Si algún payload falla, el campo `message` de la estructura de estado
contendrá la causa exacta (`Unknown command`, `Expression is not valid`,
`VAR expr error: …`, `Line N failed: <ESP_ERR>`).

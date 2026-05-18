## Commands

| Comando                 | Función                   | Ejemplo                  |
| ----------------------- | ------------------------- | ------------------------ |
| `REM`                   | Comentario de una línea   | `REM Hola`               |
| `END_REM`               | Fin comentario multilínea | `END_REM`                |
| `STRING`                | Escribe texto             | `STRING Hola`            |
| `STRINGLN`              | Texto + ENTER             | `STRINGLN Hola`          |
| `DELAY`                 | Espera en ms              | `DELAY 1000`             |
| `ENTER`                 | Pulsar Enter              | `ENTER`                  |
| `TAB`                   | Pulsar Tab                | `TAB`                    |
| `ESCAPE`                | Pulsar Escape             | `ESCAPE`                 |
| `SPACE`                 | Espacio                   | `SPACE`                  |
| `BACKSPACE`             | Retroceso                 | `BACKSPACE`              |
| `DELETE`                | Suprimir                  | `DELETE`                 |
| `INSERT`                | Insert                    | `INSERT`                 |
| `HOME`                  | Inicio línea              | `HOME`                   |
| `END`                   | Fin línea                 | `END`                    |
| `PAGEUP`                | Página arriba             | `PAGEUP`                 |
| `PAGEDOWN`              | Página abajo              | `PAGEDOWN`               |
| `UPARROW`               | Flecha arriba             | `UPARROW`                |
| `DOWNARROW`             | Flecha abajo              | `DOWNARROW`              |
| `LEFTARROW`             | Flecha izquierda          | `LEFTARROW`              |
| `RIGHTARROW`            | Flecha derecha            | `RIGHTARROW`             |
| `CTRL`                  | Mantener Ctrl             | `CTRL ALT DELETE`        |
| `ALT`                   | Mantener Alt              | `ALT F4`                 |
| `SHIFT`                 | Mantener Shift            | `SHIFT TAB`              |
| `GUI` / `WINDOWS`       | Tecla Windows/Command     | `GUI r`                  |
| `COMMAND`               | Alias macOS               | `COMMAND SPACE`          |
| `F1`–`F12`              | Teclas función            | `F5`                     |
| `HOLD`                  | Mantener tecla            | `HOLD CTRL`              |
| `RELEASE`               | Soltar tecla              | `RELEASE CTRL`           |
| `ATTACKMODE`            | Cambiar modo USB          | `ATTACKMODE HID STORAGE` |
| `SAVE_ATTACKMODE`       | Guardar modo actual       | `SAVE_ATTACKMODE`        |
| `RESTORE_ATTACKMODE`    | Restaurar modo            | `RESTORE_ATTACKMODE`     |
| `DEFINE`                | Constante                 | `DEFINE #WAIT 1000`      |
| `VAR`                   | Variable                  | `VAR $X = 5`             |
| `IF`                    | Condición                 | `IF ($X > 1) THEN`       |
| `ELSE`                  | Rama alternativa          | `ELSE`                   |
| `END_IF`                | Fin IF                    | `END_IF`                 |
| `WHILE`                 | Bucle while               | `WHILE ($X > 0)`         |
| `END_WHILE`             | Fin while                 | `END_WHILE`              |
| `LOOP`                  | Repetir payload           | `LOOP`                   |
| `BREAK`                 | Romper bucle              | `BREAK`                  |
| `CONTINUE`              | Continuar bucle           | `CONTINUE`               |
| `FUNCTION`              | Definir función           | `FUNCTION TEST()`        |
| `END_FUNCTION`          | Fin función               | `END_FUNCTION`           |
| `RETURN`                | Retornar valor            | `RETURN TRUE`            |
| `RANDOM_INT`            | Número aleatorio          | `VAR $X = RANDOM_INT()`  |
| `JITTER`                | Variación delays          | `JITTER 20`              |
| `WAIT_FOR_BUTTON_PRESS` | Esperar botón             | `WAIT_FOR_BUTTON_PRESS`  |
| `LED`                   | Control LED               | `LED R`                  |
| `CAPSLOCK`              | Toggle Caps Lock          | `CAPSLOCK`               |
| `NUMLOCK`               | Toggle Num Lock           | `NUMLOCK`                |
| `SCROLLLOCK`            | Toggle Scroll Lock        | `SCROLLLOCK`             |
| `EXFIL`                 | Exfiltración de datos     | `EXFIL /loot.txt`        |
| `INJECT_MOD`            | Modificador persistente   | `INJECT_MOD WINDOWS`     |
| `STOP_PAYLOAD`          | Detener ejecución         | `STOP_PAYLOAD`           |
| `RESTART_PAYLOAD`       | Reiniciar payload         | `RESTART_PAYLOAD`        |


## Operadores soportados.

| Tipo        | Operadores        |
| ----------- | ----------------- |
| Matemáticos | `+ - * / % ^`     |
| Comparación | `== != > < >= <=` |
| Lógicos     | `&& \|\| !`       |
| Bitwise     | `& \| << >>`      |

## Internal Variables

| Variable                             | Significado     |
| ------------------------------------ | --------------- |
| `$_CAPSLOCK_ON`                      | Estado CapsLock |
| `$_NUMLOCK_ON`                       | Estado NumLock  |
| `$_CURRENT_VID`                      | VID USB actual  |
| `$_CURRENT_PID`                      | PID USB actual  |
| `$_BUTTON_ENABLED`                   | Estado botón    |
| `$_HOST_CONFIGURATION_REQUEST_COUNT` | Enumeración USB |



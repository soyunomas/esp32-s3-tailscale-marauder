# ESP32-S3 Tailscale Marauder

Repetidor WiFi basado en **ESP32-S3** con soporte **WPA2-Enterprise**, cliente **Tailscale** integrado y capacidad de ejecución de **USB HID DuckyScript**.

[🇺🇸 English Version](README.md)

Toda la configuración se realiza a través de una interfaz web responsiva servida directamente desde el dispositivo.

<p align="center">
  <a href="img/screenshot1.png"><img src="img/screenshot1.png" width="19%" alt="Dashboard" /></a>
  <a href="img/screenshot2.png"><img src="img/screenshot2.png" width="19%" alt="WiFi Config" /></a>
  <a href="img/screenshot3.png"><img src="img/screenshot3.png" width="19%" alt="Tailscale" /></a><br>
  <a href="img/screenshot4.png"><img src="img/screenshot4.png" width="19%" alt="USB HID" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="System" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="Scheduler" /></a>
</p>

## Características Principales

### 🎓 WPA2-Enterprise y Repetidor WiFi
- **STA+AP Simultáneo** — Conexión a red WiFi corporativa y creación de AP propio.
- **Soporte eduroam** — Configuración EAP-PEAP/TTLS compatible con redes universitarias.
- **NAPT y Redirección** — NAT para clientes y apertura de puertos.

### 🛣️ Tailscale con Advertise Routes
- **Cliente Nativo** — Integración en C/FreeRTOS vía MicroLink.
- **Subnet Router** — Anuncia rutas LAN y permite acceso `Tailscale -> LAN` mediante SNAT.

### ⌨️ Ejecutor USB HID (DuckyScript)
- **Parser DuckyScript** — Soporte del conjunto de comandos listado abajo.
- **Salida HID Real** — Ejecución de pulsaciones, variables runtime, expresiones, condicionales, bucles, funciones, delay por defecto y jitter.
- **Modo Dry-Run** — Se usa automáticamente cuando TinyUSB HID no está disponible; los comandos de hardware/modo USB siguen siendo solo validación y se omiten en dry-run.
- **Almacenamiento** — Hasta 12 macros en memoria flash.
- **Pulso Keep Awake** — Envío periódico opcional de una tecla USB HID para reducir la aparición de la pantalla de bloqueo del host. El usuario elige tecla e intervalo; se pausa mientras una macro está en cola o ejecutándose.
- **Multi-layout** — Soporte para ES, US, UK, LATAM, FR, DE, IT, PT, BR, Nordic, BE, TR, PL, CZ, SK, HU, RO, HR, SR, SL, BG, RU, UA, ZH y TW.

### 📅 Programador (Scheduler)
- **Reglas Horarias** — Control automático de WiFi y VPN basado en la hora (NTP).
- **Macros USB HID programadas** — En modo `Scheduled`, cada regla puede ejecutar opcionalmente una macro USB HID guardada una vez cuando la regla pasa a estar activa.

## Comandos DuckyScript Soportados

Los siguientes comandos tienen **ejecución HID/runtime real** cuando el dispositivo USB HID está listo:

| Comando | Función | Ejemplo |
|---|---|---|
| `REM`, `//`, `END_REM` | Comentarios | `REM Comentario` |
| `STRING`, `STRINGLN` | Escribe texto, con interpolación de `$VAR` y `#DEFINE` | `STRINGLN Hola $NAME` |
| `DELAY` | Pausa en ms o resultado de expresión | `DELAY RANDOM_INT(200,800)` |
| `DEFAULT_DELAY`, `DEFAULTDELAY` | Añade pausa tras comandos físicos | `DEFAULT_DELAY 50` |
| `JITTER` | Añade variación aleatoria al delay | `JITTER 25` |
| `VAR`, `DEFINE` | Variables y defines runtime | `VAR $N = RANDOM_INT(1,5)` |
| `IF`, `ELSE`, `END_IF` | Ejecución condicional | `IF $N > 2 THEN` |
| `WHILE`, `END_WHILE`, `BREAK`, `CONTINUE` | Bucles runtime | `WHILE $N < 5` |
| `LOOP` | Reinicia el payload, opcionalmente con contador | `LOOP 3` |
| `FUNCTION`, `END_FUNCTION`, `RETURN`, `NAME()` | Llamadas simples a funciones | `OpenRun()` |
| `ENTER`, `TAB`, `ESCAPE`, `SPACE` | Teclas estándar | `ENTER` |
| `BACKSPACE`, `DELETE`, `INSERT` | Edición | `BACKSPACE` |
| `HOME`, `END`, `PAGEUP`, `PAGEDOWN`| Navegación | `HOME` |
| `UPARROW`, `DOWNARROW`, `LEFTARROW`, `RIGHTARROW` | Flechas | `UPARROW` |
| `UP`, `DOWN`, `LEFT`, `RIGHT`, `ESC`, `CONTROL`, `OPTION` | Alias runtime | `ESC` |
| `PRINTSCREEN`, `PAUSE`, `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`, `MENU` | Teclas adicionales | `SCROLLLOCK` |
| `CTRL`, `ALT`, `SHIFT`, `GUI`, `WINDOWS`, `COMMAND` | Modificadores / combos | `CTRL ALT DELETE` |
| `F1`–`F12` | Teclas de función | `F5` |
| `HOLD` / `RELEASE` | Mantener modificadores| `HOLD CTRL` |
| `STOP_PAYLOAD` | Detiene el script | `STOP_PAYLOAD` |

Los siguientes comandos se reconocen por compatibilidad del parser, pero están intencionadamente **deshabilitados en runtime / omitidos / solo validación**:
`ATTACKMODE`, `SAVE_ATTACKMODE`, `RESTORE_ATTACKMODE`, `WAIT_FOR_BUTTON_PRESS`, `LED`, `EXFIL`, `INJECT_MOD` y `RESTART_PAYLOAD`.

USB HID **Keep Awake** se configura separado de las macros DuckyScript. Puede enviar periódicamente `SCROLLLOCK`, `PAUSE`/`BREAK`, `CAPSLOCK`, `NUMLOCK`, `PRINTSCREEN`, `MENU` o `F1`-`F12` cada 5-3600 segundos. Si una macro está en cola o ejecutándose, Keep Awake se pausa automáticamente y se reanuda en el siguiente intervalo si sigue activado.

Las variables internas reconocidas son `$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON`, `$_CURRENT_VID`, `$_CURRENT_PID`, `$_BUTTON_ENABLED` y `$_HOST_CONFIGURATION_REQUEST_COUNT`. El runtime HID actual resuelve los estados de bloqueo de teclado y el placeholder del botón; VID/PID están reconocidas por el parser, pero el ejecutor aún no las resuelve.

## Uso del Scheduler

La pestaña **Scheduler** controla automatizaciones basadas en hora. Las reglas
solo se aplican cuando el modo de operación está en **Scheduled**; en
**Always on**, las reglas quedan guardadas pero no se ejecutan.

1. Abre la pestaña **Scheduler**.
2. Comprueba la zona horaria y pulsa **Sync now** si la hora del dispositivo no está sincronizada.
3. Selecciona **Scheduled** en **Operating Mode**.
4. Pulsa **Add rule** y configura:
   - días de la semana,
   - hora de inicio y fin,
   - estados AP / STA / Tailscale,
   - macro **USB HID** guardada opcional.
5. Pulsa **Apply scheduler**.

Las reglas del scheduler son semanales, no temporizadores de un solo uso. Una
regla marcada para martes se ejecutará todos los martes hasta que se cambie o se
desactive.

Cuando una regla tiene seleccionada una macro USB HID, la macro se ejecuta **una
vez cuando esa regla pasa a estar activa**. No se repite continuamente durante
la ventana activa. Si el dispositivo arranca o se aplica la configuración del
scheduler cuando ya está dentro de una ventana activa, la macro también se
encola una vez para esa ventana.

Usa **No macro** cuando la regla solo deba controlar el estado de AP / STA /
Tailscale.

## API REST

Todos los endpoints requieren **HTTP Basic Auth** (por defecto: `admin`/`admin`).

| Categoría | Método | Endpoint | Descripción |
|---|---|---|---|
| **Sistema** | `GET` | `/api/status` | Resumen del estado global |
| | `GET` | `/api/logs` | Logs del sistema (text/plain) |
| | `POST` | `/api/restart` | Reiniciar el dispositivo |
| | `POST` | `/api/ota` | Actualización de firmware |
| | `POST` | `/api/factory-reset` | Borrar config y reiniciar |
| **WiFi** | `GET` | `/api/wifi/state` | Estado detallado de la estación (STA) |
| | `POST` | `/api/wifi/pause` | Pausar reconexión del STA |
| | `POST` | `/api/wifi/resume` | Reanudar reconexión del STA |
| | `GET` | `/api/scan` | Escanear redes WiFi cercanas |
| | `GET` | `/api/clients` | Lista de clientes conectados al AP |
| | `POST` | `/api/ping` | Realizar ping a un host |
| **Tailscale** | `GET` | `/api/tailscale/status` | Estado del cliente Tailscale |
| | `GET` | `/api/tailscale/config` | Obtener config de Tailscale |
| | `POST` | `/api/tailscale/config` | Actualizar config de Tailscale |
| **USB HID** | `GET` | `/api/usb-hid/status` | Estado actual del ejecutor |
| | `GET` | `/api/usb-hid/macros` | Listar todas las macros guardadas |
| | `POST` | `/api/usb-hid/macros` | Crear una nueva macro |
| | `GET` | `/api/usb-hid/macros/{id}` | Detalles de una macro específica |
| | `PUT` | `/api/usb-hid/macros/{id}` | Actualizar una macro existente |
| | `DELETE`| `/api/usb-hid/macros/{id}` | Eliminar una macro |
| | `POST` | `/api/usb-hid/validate` | Validar sintaxis DuckyScript |
| | `POST` | `/api/usb-hid/execute` | Ejecutar script (Simulación o HID) |
| | `POST` | `/api/usb-hid/stop` | Detener ejecución de forma segura |
| | `POST` | `/api/usb-hid/panic` | Parada de emergencia y liberación |
| | `GET` | `/api/usb-hid/keepalive` | Consultar estado y contadores de Keep Awake |
| | `POST` | `/api/usb-hid/keepalive` | Configurar tecla e intervalo periódico de Keep Awake |
| **Scheduler**| `GET` | `/api/scheduler/status` | Estado y próximo evento |
| | `GET` | `/api/scheduler/config` | Obtener reglas, incluyendo `usb_hid_macro_id` opcional por regla |
| | `POST` | `/api/scheduler/config` | Actualizar reglas y validar IDs de macros programadas |
| | `POST` | `/api/scheduler/sync` | Forzar sincronización NTP |
| **Config** | `GET` | `/api/config` | Obtener configuración global |
| | `POST` | `/api/config` | Actualizar configuración global |
| | `POST` | `/api/loglevel` | Cambiar niveles de log en vivo |
| | `POST` | `/api/auth/change` | Cambiar credenciales de la web |
| | `GET` | `/api/auth/check` | Verificar credenciales actuales |

## Binarios Pre-compilados (Instalación Fácil)

Los binarios listos para usar se encuentran en la carpeta `firmware/`. Puedes subirlos directamente a tu ESP32-S3 por USB sin necesidad de compilar el código.

### Flasheo Inicial (vía USB)

1. Conecta tu ESP32-S3 al ordenador.
2. Asegúrate de tener instalado `esptool.py` (`pip install esptool`).
3. Ejecuta el siguiente comando (sustituye `/dev/ttyACM0` por tu puerto real, ej: `COM3` en Windows):

```bash
esptool.py -p /dev/ttyACM0 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 firmware/bootloader.bin \
  0x8000 firmware/partition-table.bin \
  0x89000 firmware/ota_data_initial.bin \
  0x90000 firmware/wifi_repeater.bin
```

### Actualizaciones Posteriores (Web OTA)

¡Una vez instalado, no volverás a necesitar cables!
1. Accede a la interfaz Web (IP por defecto `192.168.4.1` o la IP asignada por tu red).
2. Ve a la pestaña **Sistema**.
3. Sube el nuevo archivo `wifi_repeater.bin` en la sección de **Actualización OTA**.

## Requisitos de Compilación
- **ESP-IDF v6.1-dev** o superior.

---

## ⚠️ Aviso Legal (Disclaimer)

Esta herramienta ha sido creada exclusivamente con fines **educativos y de auditoría ética**. El uso de este software para atacar objetivos sin consentimiento previo es ilegal. Es responsabilidad del usuario final cumplir con todas las leyes locales, estatales y federales aplicables. Los desarrolladores no asumen ninguna responsabilidad por el mal uso o los daños causados por este programa.

## 📄 Licencia

Este proyecto está bajo la licencia **MIT**. Consulta el archivo [LICENSE](LICENSE) para más detalles.

Copyright (c) 2026 soyunomas

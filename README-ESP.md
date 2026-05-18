# ESP32-S3 Tailscale Marauder

Repetidor WiFi basado en **ESP32-S3** con soporte **WPA2-Enterprise**, cliente **Tailscale** integrado y capacidad de ejecución de **USB HID DuckyScript**.

[🇺🇸 English Version](README.md)

Toda la configuración se realiza a través de una interfaz web responsiva servida directamente desde el dispositivo.

<p align="center">
  <a href="img/screenshot1.png"><img src="img/screenshot1.png" width="19%" alt="Dashboard" /></a>
  <a href="img/screenshot2.png"><img src="img/screenshot2.png" width="19%" alt="WiFi Config" /></a>
  <a href="img/screenshot3.png"><img src="img/screenshot3.png" width="19%" alt="Tailscale" /></a>
  <a href="img/screenshot4.png"><img src="img/screenshot4.png" width="19%" alt="Port Forwarding" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="System" /></a>
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
- **Parser DuckyScript** — Soporte completo de sintaxis DuckyScript 3.0.
- **Salida HID Real** — Ejecución de pulsaciones de teclas reales en el equipo objetivo.
- **Modo Dry-Run** — Los comandos lógicos avanzados (bucles, variables) se validan y simulan en la UI pero no generan salida HID real todavía.
- **Almacenamiento** — Hasta 12 macros en memoria flash.
- **Multi-layout** — Soporte para más de 20 distribuciones de teclado (incluyendo ES).

### 📅 Programador (Scheduler)
- **Reglas Horarias** — Control automático de WiFi y VPN basado en la hora (NTP).

## Comandos DuckyScript Soportados

Los siguientes comandos generan **Salida HID Real** (se envían al PC):

| Comando | Función | Ejemplo |
|---|---|---|
| `REM` | Comentario | `REM Comentario` |
| `STRING` | Escribe texto | `STRING Hola` |
| `STRINGLN` | Texto + ENTER | `STRINGLN Hola` |
| `DELAY` | Pausa en ms | `DELAY 1000` |
| `ENTER`, `TAB`, `ESCAPE`, `SPACE` | Teclas estándar | `ENTER` |
| `BACKSPACE`, `DELETE`, `INSERT` | Edición | `BACKSPACE` |
| `HOME`, `END`, `PAGEUP`, `PAGEDOWN`| Navegación | `HOME` |
| `UPARROW`, `DOWNARROW`, etc. | Flechas | `UPARROW` |
| `CTRL`, `ALT`, `SHIFT`, `GUI` | Modificadores / Combos | `CTRL ALT DELETE` |
| `F1`–`F12` | Teclas de función | `F5` |
| `HOLD` / `RELEASE` | Mantener modificadores| `HOLD CTRL` |
| `STOP_PAYLOAD` | Detiene el script | `STOP_PAYLOAD` |

Los siguientes comandos se validan pero operan en **Modo Dry-Run** (simulación):
`JITTER`, `ATTACKMODE`, `VAR`, `DEFINE`, `IF/ELSE`, `WHILE`, `LOOP`, `FUNCTION`, `RANDOM_INT`, `LED`, `EXFIL`, `WAIT_FOR_BUTTON_PRESS`.

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
| **Scheduler**| `GET` | `/api/scheduler/status` | Estado y próximo evento |
| | `GET` | `/api/scheduler/config` | Obtener reglas de programación |
| | `POST` | `/api/scheduler/config` | Actualizar reglas de programación |
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
  0xf000 firmware/ota_data_initial.bin \
  0x20000 firmware/wifi_repeater.bin
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


# ESP32-S3 Tailscale Marauder

WiFi Repeater based on **ESP32-S3** with **WPA2-Enterprise** support, embedded **Tailscale** client, and **USB HID DuckyScript** execution capabilities.

[🇪🇸 Versión en Español](README-ESP.md)

All configuration is done via an embedded responsive web UI served directly from the device.

<p align="center">
  <a href="img/screenshot1.png"><img src="img/screenshot1.png" width="19%" alt="Dashboard" /></a>
  <a href="img/screenshot2.png"><img src="img/screenshot2.png" width="19%" alt="WiFi Config" /></a>
  <a href="img/screenshot3.png"><img src="img/screenshot3.png" width="19%" alt="Tailscale" /></a>
  <a href="img/screenshot4.png"><img src="img/screenshot4.png" width="19%" alt="Port Forwarding" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="System" /></a>
</p>

## Main Features

### 🎓 WPA2-Enterprise and WiFi Repeater
- **Simultaneous STA+AP** — Connects to a WiFi network (STA) and creates its own Access Point (AP).
- **WPA2-Enterprise** — Supports EAP-PEAP/TTLS (corporate/university networks like eduroam).
- **NAPT & Port Forwarding** — NAT for AP clients and port redirection.
- **Configurable Identity** — Custom Hostname and MAC addresses for STA/AP.

### 🛣️ Tailscale with Advertise Routes
- **Embedded Tailscale Client** — Native C/FreeRTOS integration using MicroLink.
- **Advertise Routes (Subnet Router)** — Advertises LAN routes and performs SNAT, allowing `Tailscale -> LAN` access.
- **Noise IK Authentication** — Secure handshake with Tailscale servers.
- **DERP Relay Support** — Connectivity even behind restrictive firewalls.

### ⌨️ USB HID Executor (DuckyScript)
- **DuckyScript Parser** — Full parser support for DuckyScript 3.0 syntax.
- **Real HID Output** — Execution of core keystroke commands on target machines.
- **Dry-Run Mode** — Advanced logic commands (logic, variables) are validated and simulated in the Web UI without sending HID reports.
- **Macro Storage** — Save up to 12 custom macros in non-volatile memory.
- **Multi-layout Support** — Support for 20+ keyboard layouts including ES, US, UK, FR, DE, etc.

### 📅 Smart Scheduler
- **Time-based Rules** — Schedule when AP, STA, or Tailscale should be active.
- **NTP Sync** — Automatic time synchronization for precise execution.

### 💻 Web UI
- **Responsive UI** — Mobile-first design.
- **OTA Updates** — Web-based firmware updates.
- **Log Viewer** — Real-time system logs.

## Supported DuckyScript Commands

The following commands are supported for **Real HID Output** (actually sent to the host):

| Command | Function | Example |
|---|---|---|
| `REM` | One-line comment | `REM Comment` |
| `STRING` | Type text | `STRING Hello World` |
| `STRINGLN` | Type text + ENTER | `STRINGLN Hello World` |
| `DELAY` | Wait in ms | `DELAY 1000` |
| `ENTER`, `TAB`, `ESCAPE`, `SPACE` | Standard keys | `ENTER` |
| `BACKSPACE`, `DELETE`, `INSERT` | Edit keys | `BACKSPACE` |
| `HOME`, `END`, `PAGEUP`, `PAGEDOWN`| Navigation keys | `HOME` |
| `UPARROW`, `DOWNARROW`, etc. | Arrow keys | `UPARROW` |
| `CTRL`, `ALT`, `SHIFT`, `GUI` | Modifiers / Combos | `CTRL ALT DELETE` |
| `F1`–`F12` | Function keys | `F5` |
| `HOLD` / `RELEASE` | Persistent modifiers | `HOLD CTRL` |
| `STOP_PAYLOAD` | Stop current script | `STOP_PAYLOAD` |

The following commands are parsed and validated but currently operate in **Dry-Run Mode** (Simulation in UI only):
`JITTER`, `ATTACKMODE`, `VAR`, `DEFINE`, `IF/ELSE`, `WHILE`, `LOOP`, `FUNCTION`, `RANDOM_INT`, `LED`, `EXFIL`, `WAIT_FOR_BUTTON_PRESS`.

## Hardware: ESP32-S3 N16R8 (Required)

## REST API

All API endpoints require **HTTP Basic Auth** (default: `admin`/`admin`).

| Category | Method | Endpoint | Description |
|---|---|---|---|
| **System** | `GET` | `/api/status` | Global system status summary |
| | `GET` | `/api/logs` | Raw system logs (text/plain) |
| | `POST` | `/api/restart` | Reboot the device |
| | `POST` | `/api/ota` | Firmware update (binary payload) |
| | `POST` | `/api/factory-reset` | Erase config and reboot |
| **WiFi** | `GET` | `/api/wifi/state` | Detailed Station (STA) state |
| | `POST` | `/api/wifi/pause` | Pause STA reconnection |
| | `POST` | `/api/wifi/resume` | Resume STA reconnection |
| | `GET` | `/api/scan` | Scan for nearby WiFi networks |
| | `GET` | `/api/clients` | List connected clients on AP |
| | `POST` | `/api/ping` | Ping a target host |
| **Tailscale** | `GET` | `/api/tailscale/status` | Tailscale client status |
| | `GET` | `/api/tailscale/config` | Get current Tailscale config |
| | `POST` | `/api/tailscale/config` | Update Tailscale config |
| **USB HID** | `GET` | `/api/usb-hid/status` | Current executor status |
| | `GET` | `/api/usb-hid/macros` | List all saved macros |
| | `POST` | `/api/usb-hid/macros` | Create a new macro |
| | `GET` | `/api/usb-hid/macros/{id}` | Get specific macro details |
| | `PUT` | `/api/usb-hid/macros/{id}` | Update existing macro |
| | `DELETE`| `/api/usb-hid/macros/{id}` | Delete a macro |
| | `POST` | `/api/usb-hid/validate` | Validate DuckyScript syntax |
| | `POST` | `/api/usb-hid/execute` | Run script (Dry-run or HID) |
| | `POST` | `/api/usb-hid/stop` | Gracefully stop execution |
| | `POST` | `/api/usb-hid/panic` | Emergency stop & release keys |
| **Scheduler**| `GET` | `/api/scheduler/status` | Scheduler state & next event |
| | `GET` | `/api/scheduler/config` | Get scheduling rules |
| | `POST` | `/api/scheduler/config` | Update scheduling rules |
| | `POST` | `/api/scheduler/sync` | Force NTP time sync |
| **Config** | `GET` | `/api/config` | Get global device config |
| | `POST` | `/api/config` | Update global device config |
| | `POST` | `/api/loglevel` | Change log levels at runtime |
| | `POST` | `/api/auth/change` | Change Web UI credentials |
| | `GET` | `/api/auth/check` | Verify current credentials |

## Pre-compiled Binaries (Easy Install)

Pre-built binaries are available in the `firmware/` directory. You can flash them directly to your ESP32-S3 via USB without building from source.

### Initial Flash (via USB)

1. Connect your ESP32-S3 to your computer.
2. Ensure you have `esptool.py` installed (`pip install esptool`).
3. Run the following command (replace `/dev/ttyACM0` with your actual port, e.g., `COM3` on Windows):

```bash
esptool.py -p /dev/ttyACM0 -b 460800 --before default_reset --after hard_reset --chip esp32s3 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 firmware/bootloader.bin \
  0x8000 firmware/partition-table.bin \
  0xf000 firmware/ota_data_initial.bin \
  0x20000 firmware/wifi_repeater.bin
```

### Subsequent Updates (Web OTA)

Once the firmware is running, you don't need cables anymore!
1. Access the Web UI (default IP `192.168.4.1` or the IP assigned by your network).
2. Go to the **System** tab.
3. Upload the new `wifi_repeater.bin` file in the **OTA Update** section.

## Build from Source

### Requirements

- **ESP-IDF v6.1-dev** or higher ([Installation guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/))

### Build

```bash
idf.py set-target esp32s3
idf.py build
```

### Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

---

## ⚠️ Disclaimer

This tool is for **educational and ethical testing purposes only**. The use of this software for attacking targets without prior mutual consent is illegal. It is the end user's responsibility to obey all applicable local, state, and federal laws. The developers assume no liability and are not responsible for any misuse or damage caused by this program.

## 📄 License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for details.

Copyright (c) 2026 soyunomas

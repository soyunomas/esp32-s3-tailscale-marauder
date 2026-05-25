# ESP32-S3 Tailscale Marauder (Advertise Routes)

WiFi Repeater based on **ESP32-S3** with **WPA2-Enterprise** support, embedded **Tailscale** client, and **USB HID DuckyScript** execution capabilities.

[🇪🇸 Versión en Español](README-ESP.md)

All configuration is done via an embedded responsive web UI served directly from the device.

<p align="center">
  <a href="img/screenshot1.png"><img src="img/screenshot1.png" width="19%" alt="Dashboard" /></a>
  <a href="img/screenshot2.png"><img src="img/screenshot2.png" width="19%" alt="WiFi Config" /></a>
  <a href="img/screenshot3.png"><img src="img/screenshot3.png" width="19%" alt="Tailscale" /></a><br>
  <a href="img/screenshot4.png"><img src="img/screenshot4.png" width="19%" alt="USB HID" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="System" /></a>
  <a href="img/screenshot5.png"><img src="img/screenshot5.png" width="19%" alt="Scheduler" /></a>
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
- **DuckyScript Parser** — Parser support for the command set listed below.
- **Real HID Output** — Execution of keystrokes plus runtime variables, expressions, branching, loops, functions, default delays, and jitter.
- **Dry-Run Mode** — Used automatically when TinyUSB HID is unavailable; hardware/USB-mode commands remain validation-only and are skipped in dry-run.
- **Macro Storage** — Saved macros are stored as variable-size files in the SPIFFS `storage` partition. The current safety limit is **512 KiB per payload** (`USB_HID_MACRO_SCRIPT_MAX_BYTES`); actual capacity is limited by free flash space.
- **Keep Awake Key Pulse** — Optional periodic USB HID key pulse to reduce host lock-screen activation. The user selects the key and interval; it pauses while any macro is queued or running.
- **Multi-layout Support** — Support for ES, US, UK, LATAM, FR, DE, IT, PT, BR, Nordic, BE, TR, PL, CZ, SK, HU, RO, HR, SR, SL, BG, RU, UA, ZH, and TW layouts.

### 📅 Smart Scheduler
- **Time-based Rules** — Schedule when AP, STA, or Tailscale should be active.
- **Dormant Scheduled mode** — Keeps AP, STA, and Tailscale/MicroLink off outside scheduled windows while the firmware and scheduler stay awake.
- **NTP acquisition for dormant operation** — Temporarily enables STA for time sync without opening the AP, then retries silently if time is still invalid.
- **Recovery paths** — Optional AP recovery window and BOOT short-press temporary AP wake for dormant operation; BOOT long-press factory reset remains unchanged.
- **Scheduled USB HID Macros** — In `Scheduled` mode, each rule can optionally run one saved USB HID macro once when the rule becomes active.
- **NTP Sync** — Automatic time synchronization for precise execution.

### 💻 Web UI
- **Responsive UI** — Mobile-first design.
- **OTA Updates** — Web-based firmware updates.
- **Log Viewer** — Real-time system logs.

## Supported DuckyScript Commands

The following commands are supported for **real HID/runtime execution** when the USB HID device is ready:

| Command | Function | Example |
|---|---|---|
| `REM`, `//`, `END_REM` | Comments | `REM Comment` |
| `STRING`, `STRINGLN` | Type text, with `$VAR` and `#DEFINE` interpolation | `STRINGLN Hello $NAME` |
| `DELAY` | Wait in ms or expression result | `DELAY RANDOM_INT(200,800)` |
| `DEFAULT_DELAY`, `DEFAULTDELAY` | Add delay after physical commands | `DEFAULT_DELAY 50` |
| `JITTER` | Add random delay variation | `JITTER 25` |
| `VAR`, `DEFINE` | Runtime variables and defines | `VAR $N = RANDOM_INT(1,5)` |
| `IF`, `ELSE`, `END_IF` | Conditional execution | `IF $N > 2 THEN` |
| `WHILE`, `END_WHILE`, `BREAK`, `CONTINUE` | Runtime loops | `WHILE $N < 5` |
| `LOOP` | Restart payload, optionally counted | `LOOP 3` |
| `FUNCTION`, `END_FUNCTION`, `RETURN`, `NAME()` | Simple function calls | `OpenRun()` |
| `ENTER`, `TAB`, `ESCAPE`, `SPACE` | Standard keys | `ENTER` |
| `BACKSPACE`, `DELETE`, `INSERT` | Edit keys | `BACKSPACE` |
| `HOME`, `END`, `PAGEUP`, `PAGEDOWN`| Navigation keys | `HOME` |
| `UPARROW`, `DOWNARROW`, `LEFTARROW`, `RIGHTARROW` | Arrow keys | `UPARROW` |
| `UP`, `DOWN`, `LEFT`, `RIGHT`, `ESC`, `CONTROL`, `OPTION` | Runtime aliases | `ESC` |
| `PRINTSCREEN`, `PAUSE`, `CAPSLOCK`, `NUMLOCK`, `SCROLLLOCK`, `MENU` | Extra keyboard keys | `SCROLLLOCK` |
| `CTRL`, `ALT`, `SHIFT`, `GUI`, `WINDOWS`, `COMMAND` | Modifiers / combos | `CTRL ALT DELETE` |
| `F1`–`F12` | Function keys | `F5` |
| `HOLD` / `RELEASE` | Persistent modifiers | `HOLD CTRL` |
| `STOP_PAYLOAD` | Stop current script | `STOP_PAYLOAD` |

The following commands are recognized for parser compatibility, but are intentionally **disabled at runtime / skipped / validation-only**:
`ATTACKMODE`, `SAVE_ATTACKMODE`, `RESTORE_ATTACKMODE`, `WAIT_FOR_BUTTON_PRESS`, `LED`, `EXFIL`, `INJECT_MOD`, and `RESTART_PAYLOAD`.

USB HID **Keep Awake** is configured separately from DuckyScript macros. It can periodically send `SCROLLLOCK`, `PAUSE`/`BREAK`, `CAPSLOCK`, `NUMLOCK`, `PRINTSCREEN`, `MENU`, or `F1`-`F12` every 5-3600 seconds. When a macro is queued or running, Keep Awake pauses automatically and resumes on the next interval if still enabled.

Recognized internal variables are `$_CAPSLOCK_ON`, `$_NUMLOCK_ON`, `$_SCROLLLOCK_ON`, `$_CURRENT_VID`, `$_CURRENT_PID`, `$_BUTTON_ENABLED`, and `$_HOST_CONFIGURATION_REQUEST_COUNT`. Current HID runtime resolves the lock-key states and button placeholder; VID/PID are parser-recognized but not resolved by the executor yet.

### USB HID payload size and storage

Saved USB HID scripts are no longer reserved as fixed NVS slots. NVS stores only macro metadata, while each payload is saved as a file in the `storage` SPIFFS partition. Small macros consume only their real file size plus filesystem overhead.

The firmware enforces a defensive **512 KiB maximum per payload**. This is a configurable safety cap in `USB_HID_MACRO_SCRIPT_MAX_BYTES`, not a preallocated buffer. The practical number of saved macros depends on available SPIFFS space and metadata capacity.

## Hardware: ESP32-S3 N16R8 (Required)

## Scheduler Usage

The **Scheduler** tab controls AP, STA, Tailscale/MicroLink, and optional USB HID
macro execution using weekly time windows. This is a live scheduler; it does not
put the ESP32 into deep sleep. In Dormant Scheduled mode the firmware, web
server task, button handler, and scheduler keep running, but the radios/services
are kept silent when they are not needed.

### Activation profiles and modes

The UI exposes **Activation Profile** shortcuts and the lower-level
**Operating Mode** selector. They map to the same scheduler engine:

| Profile / Mode | Behavior |
|---|---|
| **Normal / Always on** | AP, STA, and Tailscale scheduler gates stay enabled. Saved rules are kept but not applied. |
| **Home Stealth / Scheduled** | Rules control AP, STA, and Tailscale during active windows. When no rule is active, the existing safe behavior remains: AP/STA/Tailscale stay enabled by default. If a rule would remove the only management path, the AP safety hold keeps management available. |
| **Manual off** | AP is disabled manually, while STA and Tailscale remain available if configured. The UI rejects enabling this mode unless STA is currently connected, because otherwise management could be lost. |
| **Dormant Scheduled** | New mode. Outside active rules: `ap_desired=false`, `sta_desired=false`, and `tailscale_desired=false`. During an active rule, the rule's AP/STA/Tailscale toggles are used, except AP remains off by default when **Keep AP off during active window** is enabled. |
| **Headless Dormant** | Reserved/disabled in this UI. It would remove automatic AP recovery and is intentionally not enabled without a separate dangerous override. |

### Dormant Scheduled behavior

Dormant Scheduled is designed for operational silence, not minimum power:

- Outside a schedule window, SoftAP, STA, and Tailscale/MicroLink are turned off.
- If device time is invalid at boot, the AP stays off and STA is enabled only for
  a temporary NTP attempt.
- The NTP attempt lasts **Dormant time sync attempt** seconds. If time becomes
  valid, the scheduler evaluates immediately.
- If NTP still fails, STA is disabled again and the next attempt is delayed by
  **Dormant time sync retry** seconds.
- If **AP recovery** is enabled, a temporary AP window opens after a failed
  dormant NTP attempt.
- A short BOOT press opens a temporary AP for **Dormant button wake** seconds
  when that option is enabled. Holding BOOT for 5-10 seconds still performs the
  existing factory reset.
- Tailscale only starts after STA has an IP address, and the existing Tailscale
  boot delay still applies. When the active window ends, Tailscale is stopped
  before STA is disabled.

Dormant/headless configurations without AP recovery and without BOOT wake are
rejected with HTTP 400. This prevents saving a configuration with no recovery
path unless a future explicit dangerous override is added.

### Dormant Scheduled use cases

Use these examples as starting points. The important idea is that **Dormant
Scheduled is quiet by default**: if there is no active rule, AP, STA, and
Tailscale are off.

**1. Hidden device that checks in at night**

Goal: the ESP32 should not broadcast WiFi or keep Tailscale online during the
day, but it should become reachable every night from 03:00 to 03:15.

- Profile: **Dormant Scheduled**
- Rule: every day, `03:00` to `03:15`
- Rule toggles: **STA on**, **Tailscale on**, **AP off**
- **Keep AP off during active window**: on

Result: the device connects to the saved router during that 15-minute window,
starts Tailscale after STA gets an IP address, then stops Tailscale and turns
STA back off when the window ends. The AP never appears.

**2. Weekly maintenance window over Tailscale**

Goal: administer the device remotely every Sunday from 10:00 to 11:00 without
opening the local AP.

- Profile: **Dormant Scheduled**
- Rule: Sunday, `10:00` to `11:00`
- Rule toggles: **STA on**, **Tailscale on**, **AP off**
- **Keep AP off during active window**: on

Result: during the window, use the Tailscale IP to reach the device. Outside the
window, the device is silent again.

**3. Silent boot when time is unknown**

Goal: if the ESP32 boots with no valid clock, it should get NTP time without
making the AP visible.

- **Time sync attempt at boot**: `120`
- **Time sync retry interval**: `900`
- **Enable AP recovery window**: off

Result: STA turns on for up to 120 seconds to get NTP time. If time sync works,
the scheduler evaluates the rules immediately. If it fails, STA turns off and
the next silent attempt happens 900 seconds later.

**4. Physical recovery without leaving AP enabled**

Goal: keep the AP off normally, but still have a way to get back in if the
schedule or router settings are wrong.

- **Enable temporary AP with physical button**: on
- **Button temporary AP duration**: `300`

Result: a short BOOT press opens the AP for 5 minutes. Holding BOOT for 5-10
seconds still performs the existing factory reset.

**5. Automatic AP recovery after NTP failure**

Goal: if NTP cannot be acquired, temporarily expose AP so you can connect and
fix WiFi or scheduler settings.

- **Enable AP recovery window**: on
- **AP recovery window duration**: `300`

Result: after a failed dormant NTP attempt, the AP opens for 5 minutes, then the
device returns to dormant behavior and retries time sync later.

Recommended baseline for a quiet but recoverable setup:

- **Time sync attempt at boot**: `120`
- **Time sync retry interval**: `900`
- **Enable AP recovery window**: off
- **Enable temporary AP with physical button**: on
- **Button temporary AP duration**: `300`
- **Keep AP off during active window**: on
- Rules: **STA on**, **Tailscale on**, **AP off**

### Creating rules

1. Open **Scheduler**.
2. Confirm **Timezone** and press **Sync now** if the time is not valid.
3. Select an **Activation Profile** or **Operating Mode**.
4. Press **Add rule**.
5. Select days, start/end time, AP/STA/Tailscale states, and optionally a saved USB HID macro.
6. Press **Apply scheduler**.

Rules are weekly, not one-shot timers. A Tuesday rule runs every Tuesday until
changed or disabled. Rules do not cross midnight; use two rules for a window
that spans midnight.

When a rule has a USB HID macro selected, the macro runs once when that rule
becomes active. It does not repeat continuously during the active window. If the
device boots or scheduler config is applied while already inside an active rule
window, the macro is queued once for that active window. Use **No macro** when a
rule should only control AP / STA / Tailscale state.

### Dormant Scheduled fields

| Field | Range / default | Meaning |
|---|---:|---|
| **Time sync attempt at boot** | 30-600 s, default 120 | How long STA may stay enabled while trying to acquire NTP time in Dormant Scheduled mode. AP remains off. |
| **Time sync retry interval** | 60-86400 s, default 900 | Delay before retrying silent NTP acquisition after a failed attempt. |
| **Enable AP recovery window** | default off | Opens the SoftAP temporarily after a failed dormant NTP attempt. |
| **AP recovery window duration** | 30-1800 s, default 300 | Length of the temporary AP recovery window. |
| **Enable temporary AP with physical button** | default on | Allows a short BOOT press to open AP temporarily in Dormant Scheduled mode. |
| **Button temporary AP duration** | 30-1800 s, default 300 | Length of the AP window opened by BOOT short press. |
| **Keep AP off during active window** | default on | Forces AP off even when an active rule has AP enabled. Disable this only if the active window should expose management/AP service. |

## Web UI Field Reference

### Dashboard

| Field / control | Meaning |
|---|---|
| **Upstream** | Current STA SSID and IP address assigned by the upstream router. |
| **Signal** | STA RSSI in dBm with a visual quality bar. |
| **AP Clients** | Number of clients connected to the SoftAP and AP IP address. |
| **System** | Free heap and uptime. |
| **Tailscale** | Tailscale/MicroLink state and VPN IP when connected. |
| **Macro Storage** | SPIFFS macro storage state: available space, used space, and saved macro count. |
| **WiFi MACs** | Current STA and AP MAC addresses. |
| **Scheduler** | Current scheduler mode and compact AP/STA/Tailscale summary. |
| **Pause STA / Resume STA** | Temporarily pauses or resumes STA reconnection. Scheduler state may later reapply its desired STA gate. |
| **Connectivity Test target** | Host or IP used by the Ping button. |
| **Connected Clients** | SoftAP client list; **Refresh** reloads the list. |

### Config

**Upstream Network (STA)**

| Field | Meaning |
|---|---|
| **Scan Networks** | Scans nearby WiFi networks and can populate the STA SSID. |
| **SSID** | Upstream router/network SSID used by STA. |
| **Password** | WPA/WPA2 personal password. Blank on save keeps the existing stored password. |
| **WPA2-Enterprise (EAP)** | Enables enterprise authentication instead of personal password auth. |
| **EAP Identity** | Outer/anonymous identity for PEAP/TTLS. |
| **EAP Username** | Inner enterprise username. |
| **EAP Password** | Enterprise password. Blank on save keeps the existing stored password. |
| **Static IP (STA)** | Uses manual STA addressing instead of DHCP. |
| **IP Address** | Static STA address. Required when Static IP is enabled. |
| **Netmask** | Static subnet mask. Required when Static IP is enabled. |
| **Gateway** | Upstream router address for static routing. |
| **DNS 1 / DNS 2** | Static DNS servers for STA. |

**Access Point (AP)**

| Field | Meaning |
|---|---|
| **AP SSID** | SoftAP network name. |
| **Hide AP SSID** | Hides SSID beacon name but does not disable AP. |
| **AP Password** | SoftAP password; at least 8 characters for WPA2. Blank on save keeps the existing password. |
| **Channel** | Fixed AP channel or Auto. |
| **Max Clients** | Maximum simultaneous SoftAP clients. |

**Tailscale**

| Field | Meaning |
|---|---|
| **Enable Tailscale** | Enables the MicroLink/Tailscale runtime, subject to scheduler and STA connectivity. |
| **Auth Key** | Tailscale auth key. Blank on save keeps the existing key; **Clear Key** removes it. |
| **Device Name** | Node name registered in Tailscale. |
| **Control Host** | Optional alternate Tailscale control server; blank uses default Tailscale. |
| **Max Peers** | Peer capacity limit, 1-64. |
| **Status** | Current runtime state. |
| **DERP / DISCO / STUN** | Enables relay, peer discovery, and NAT traversal features. |
| **Expose LAN over Tailscale** | Advertises a LAN subnet over Tailscale. This forces Gateway/SNAT behavior. |
| **Hide Web UI on STA/LAN IP** | Drops inbound traffic to the web UI and configured forwarded ports when addressed to the upstream WiFi/STA IP. Tailscale and the device AP remain usable. This does not hide the device from ARP while STA is connected. |
| **Subnet CIDR** | IPv4 subnet to advertise, such as `192.168.1.0/24`. |
| **Advertised Route / Route State** | Read-only route advertisement status. |
| **Network Behavior** | Shows Repeater or Tailscale Gateway/SNAT mode. Gateway/SNAT disables AP repeater NAPT and port forwarding to AP clients. |

**Port Forwarding**

| Field / control | Meaning |
|---|---|
| **Add Rule** | Adds one TCP/UDP port forwarding rule, up to 5 rules. |
| **Enabled** | Per-rule enable switch. |
| **Protocol** | TCP or UDP NAPT rule; TCP also has a local proxy path for same-LAN access. |
| **External Port** | Port exposed on the ESP32 STA/Tailscale address. |
| **Internal IP** | Destination client IP behind the AP. |
| **Internal Port** | Destination port on the internal client. |
| **Save & Apply** | Saves Config tab values and restarts WiFi where needed. |
| **Restart** | Reboots the device without changing saved settings. |

### USB HID

| Field / control | Meaning |
|---|---|
| **Macro Name** | Name for a saved DuckyScript macro. |
| **Keyboard Layout** | Keyboard layout used to translate text and key names. |
| **Macro Script** | DuckyScript source. Payloads are capped at 512 KiB by default. |
| **Execute** | Validates and queues the current script for HID execution or dry-run. |
| **Stop** | Gracefully stops current execution. |
| **Panic** | Emergency stop and release all held keys. |
| **Save** | Stores macro metadata in NVS and the payload file in SPIFFS storage. |
| **Clear** | Clears the editor. |
| **Keep Awake Enabled** | Enables periodic HID key pulse when no macro is running. |
| **Keep Awake Key** | Key sent periodically, such as Scroll Lock, Pause/Break, or F1-F12. |
| **Every Seconds** | Keep Awake interval, 5-3600 seconds. |
| **Saved Macros** | Lightweight list of stored macros for loading, running, renaming, or deleting. Scripts are fetched only when needed. |
| **Examples** | Built-in example payloads. |

### Scheduler

| Field / control | Meaning |
|---|---|
| **Local time** | Device local time after timezone conversion. |
| **NTP** | Time sync state: waiting, valid, or synced. |
| **AP / STA / Tailscale** | Effective scheduler-controlled state. In Dormant Scheduled these can all be off outside windows. |
| **Next change** | Next detected scheduler state transition. |
| **Timezone** | POSIX timezone used for rule evaluation. |
| **Sync now** | Restarts SNTP immediately. |
| **Activation Profile** | Friendly selector for Normal, Home Stealth, Dormant Scheduled, or reserved Headless Dormant. |
| **Operating Mode** | Raw scheduler mode selector: Always on, Scheduled, Manual off, Dormant. |
| **Add rule / Clear rules** | Adds or removes weekly rules. |
| **Rule enabled** | Enables a rule. Disabled rules are ignored. |
| **Days / quick days** | Weekdays on which the rule can match. |
| **Start / End** | Local same-day active window. End must be after start. |
| **AP Enabled** | Desired AP state during the active rule. Dormant can override this with **Keep AP off during active window**. |
| **STA Enabled** | Desired STA state during the active rule. STA connects to the saved upstream router. |
| **TS Enabled** | Desired Tailscale state during the active rule; effective start still requires STA IP. |
| **USB HID Macro** | Optional saved macro to run once when the rule becomes active. |
| **Note** | Free text label for the rule. |
| **Apply scheduler** | Saves timezone, profile/mode, dormant settings, and rules. |

### Logs

| Field / control | Meaning |
|---|---|
| **Auto** | Automatically refreshes logs while the Logs tab is open. |
| **Refresh** | Reloads log output. |
| **Device Logs** | In-memory system log buffer. |

### System

| Field | Meaning |
|---|---|
| **Hostname** | Hostname applied to AP and STA netifs. |
| **STA Retry Max** | Number of failed STA attempts before entering recovery cooldown. |
| **STA Backoff Max** | Maximum exponential reconnect backoff in seconds. |
| **Custom STA MAC / STA MAC** | Optional unicast STA MAC override. |
| **Custom AP MAC / AP MAC** | Optional unicast AP MAC override. |
| **Tailscale boot delay** | Delay after boot before Tailscale may start, 0-300 seconds. |
| **WiFi recovery cooldown** | STA reconnect cooldown after repeated failures, 5-600 seconds. |
| **Scheduler poll interval** | Scheduler evaluation interval, 5-300 seconds. |
| **NTP server 1 / 2** | SNTP servers used by normal and dormant time sync. |
| **Ping count / interval / timeout / total wait** | Parameters for Dashboard ping diagnostics. |
| **Scan active min / max / timeout** | WiFi scan timing parameters. |
| **Proxy buffer / socket timeout / idle timeout / accept retry / listen backlog** | TCP proxy runtime parameters for port forwarding. |
| **System log level** | Global ESP-IDF log verbosity. `None` hides even errors. |
| **MicroLink / WireGuard log level** | Verbosity override for heavy Tailscale/WireGuard tags. |
| **Username / New Password** | Web UI Basic Auth credentials. Password minimum is 4 characters. |
| **Firmware .bin file** | OTA firmware image to upload and boot. |
| **Factory Reset** | Erases saved config and reboots. Physical BOOT hold 5-10 seconds also resets. |
| **Restart Device** | Reboots while keeping settings. |

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
| | `GET` | `/api/usb-hid/macros` | List saved macro metadata and storage stats; scripts are not included |
| | `GET` | `/api/usb-hid/storage` | Get macro storage total/used/free bytes and payload limit |
| | `POST` | `/api/usb-hid/macros` | Create a new macro, up to the configured payload limit |
| | `GET` | `/api/usb-hid/macros/{id}` | Get specific macro details, including script |
| | `PUT` | `/api/usb-hid/macros/{id}` | Update existing macro |
| | `DELETE`| `/api/usb-hid/macros/{id}` | Delete a macro |
| | `POST` | `/api/usb-hid/validate` | Validate DuckyScript syntax |
| | `POST` | `/api/usb-hid/execute` | Run script (Dry-run or HID) |
| | `POST` | `/api/usb-hid/stop` | Gracefully stop execution |
| | `POST` | `/api/usb-hid/panic` | Emergency stop & release keys |
| | `GET` | `/api/usb-hid/keepalive` | Get Keep Awake state and counters |
| | `POST` | `/api/usb-hid/keepalive` | Configure periodic Keep Awake key and interval |
| **Scheduler**| `GET` | `/api/scheduler/status` | Scheduler state, next event, and Dormant Scheduled state fields |
| | `GET` | `/api/scheduler/config` | Get scheduling rules, optional `usb_hid_macro_id`, and dormant settings |
| | `POST` | `/api/scheduler/config` | Update scheduling rules, dormant settings, and validate scheduled macro IDs |
| | `POST` | `/api/scheduler/sync` | Force NTP time sync |
| **Config** | `GET` | `/api/config` | Get global device config |
| | `POST` | `/api/config` | Update global device config |
| | `GET` | `/api/runtime/config` | Get advanced runtime tuning values |
| | `POST` | `/api/runtime/config` | Update advanced runtime tuning values |
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
  0x89000 firmware/ota_data_initial.bin \
  0x90000 firmware/wifi_repeater.bin
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

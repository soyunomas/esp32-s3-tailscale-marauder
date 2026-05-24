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
- **Almacenamiento** — Las macros guardadas usan ficheros de tamaño variable en la partición SPIFFS `storage`. El límite defensivo actual es **512 KiB por payload** (`USB_HID_MACRO_SCRIPT_MAX_BYTES`); la capacidad real depende del espacio libre en flash.
- **Pulso Keep Awake** — Envío periódico opcional de una tecla USB HID para reducir la aparición de la pantalla de bloqueo del host. El usuario elige tecla e intervalo; se pausa mientras una macro está en cola o ejecutándose.
- **Multi-layout** — Soporte para ES, US, UK, LATAM, FR, DE, IT, PT, BR, Nordic, BE, TR, PL, CZ, SK, HU, RO, HR, SR, SL, BG, RU, UA, ZH y TW.

### 📅 Programador (Scheduler)
- **Reglas Horarias** — Control automático de WiFi y VPN basado en la hora (NTP).
- **Modo Dormant Scheduled** — Mantiene AP, STA y Tailscale/MicroLink apagados fuera de las ventanas programadas, mientras el firmware y el scheduler siguen vivos.
- **Adquisición NTP en dormant** — Activa STA temporalmente para obtener hora sin abrir el AP; si falla, vuelve a silencio y reintenta más tarde.
- **Vías de recuperación** — Ventana AP de recuperación opcional y activación temporal de AP con pulsación corta de BOOT; la pulsación larga de BOOT conserva el factory reset.
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

### Tamaño de payloads y almacenamiento USB HID

Los scripts USB HID guardados ya no se reservan como slots fijos en NVS. NVS guarda solo metadatos de la macro y cada payload se guarda como fichero en la partición SPIFFS `storage`. Una macro pequeña consume solo su tamaño real de fichero más el overhead del filesystem.

El firmware aplica un máximo defensivo de **512 KiB por payload**. Es un límite configurable en `USB_HID_MACRO_SCRIPT_MAX_BYTES`, no un buffer preasignado. El número práctico de macros guardadas depende del espacio libre en SPIFFS y de la capacidad de metadatos.

## Uso del Scheduler

La pestaña **Scheduler** controla AP, STA, Tailscale/MicroLink y la ejecución
opcional de macros USB HID mediante ventanas horarias semanales. Es un
scheduler en vivo: no pone el ESP32 en deep sleep. En modo Dormant Scheduled el
firmware, el servidor web, el manejador del botón y el scheduler siguen vivos,
pero las radios y servicios quedan en silencio cuando no hacen falta.

### Perfiles de activación y modos

La UI muestra atajos en **Activation Profile** y el selector de bajo nivel
**Operating Mode**. Ambos controlan el mismo motor del scheduler:

| Perfil / modo | Comportamiento |
|---|---|
| **Normal / Always on** | AP, STA y la puerta scheduler de Tailscale quedan activados. Las reglas se guardan pero no se aplican. |
| **Home Stealth / Scheduled** | Las reglas controlan AP, STA y Tailscale durante ventanas activas. Si no hay regla activa, se mantiene el comportamiento seguro existente: AP/STA/Tailscale quedan encendidos por defecto. Si una regla quitaría la única vía de administración, el safety hold mantiene AP disponible. |
| **Manual off** | AP queda desactivado manualmente, mientras STA y Tailscale siguen disponibles si están configurados. La UI rechaza activar este modo si STA no está conectado, para evitar perder administración. |
| **Dormant Scheduled** | Modo nuevo. Fuera de reglas activas: `ap_desired=false`, `sta_desired=false` y `tailscale_desired=false`. Durante una regla activa se usan los toggles AP/STA/Tailscale de la regla, salvo que AP queda apagado por defecto si **Mantener AP apagado durante ventana activa** está activado. |
| **Headless Dormant** | Reservado/deshabilitado en esta UI. Quitarían las recuperaciones automáticas por AP y no se habilita sin un override peligroso explícito. |

### Comportamiento de Dormant Scheduled

Dormant Scheduled está diseñado para silencio operativo, no para consumo mínimo:

- Fuera de una ventana programada, SoftAP, STA y Tailscale/MicroLink se apagan.
- Si la hora no es válida al arrancar, AP permanece apagado y STA se activa solo
  para un intento temporal de NTP.
- El intento NTP dura **Intento de sincronización de hora** segundos. Si la hora
  pasa a ser válida, el scheduler evalúa inmediatamente.
- Si NTP falla, STA vuelve a apagarse y el siguiente intento se retrasa
  **Intervalo de reintento de sincronización** segundos.
- Si **recuperación AP** está activada, se abre una ventana AP temporal tras un
  fallo de NTP en dormant.
- Una pulsación corta de BOOT abre AP temporalmente durante
  **Duración del AP temporal por botón** si esa opción está activa. Mantener
  BOOT entre 5 y 10 segundos conserva el factory reset existente.
- Tailscale solo arranca después de que STA tenga IP, y respeta el boot delay
  existente de Tailscale. Cuando acaba la ventana activa, Tailscale se detiene
  antes de desactivar STA.

Las configuraciones dormant/headless sin recuperación AP y sin wake por botón se
rechazan con HTTP 400. Esto evita guardar un estado sin vía de recuperación salvo
que en el futuro se añada un override peligroso explícito.

### Casos de uso de Dormant Scheduled

Usa estos ejemplos como punto de partida. La idea importante es que **Dormant
Scheduled está apagado por defecto**: si no hay una regla activa, AP, STA y
Tailscale están apagados.

**1. Dispositivo oculto que llama a casa por la noche**

Objetivo: que el ESP32 no emita WiFi ni mantenga Tailscale activo durante el
día, pero que sea accesible cada noche de 03:00 a 03:15.

- Perfil: **Dormant Scheduled**
- Regla: todos los días, `03:00` a `03:15`
- Toggles de la regla: **STA on**, **Tailscale on**, **AP off**
- **Mantener AP apagado durante ventana activa**: activado

Resultado: el dispositivo conecta al router guardado durante esos 15 minutos,
arranca Tailscale después de obtener IP por STA, y al terminar la ventana para
Tailscale y vuelve a apagar STA. El AP no aparece nunca.

**2. Ventana semanal de mantenimiento por Tailscale**

Objetivo: administrar el dispositivo en remoto cada domingo de 10:00 a 11:00
sin abrir el AP local.

- Perfil: **Dormant Scheduled**
- Regla: domingo, `10:00` a `11:00`
- Toggles de la regla: **STA on**, **Tailscale on**, **AP off**
- **Mantener AP apagado durante ventana activa**: activado

Resultado: durante la ventana entras usando la IP de Tailscale. Fuera de la
ventana, el dispositivo vuelve a estar silencioso.

**3. Arranque silencioso cuando no hay hora**

Objetivo: si el ESP32 arranca sin reloj válido, debe obtener hora NTP sin hacer
visible el AP.

- **Intento de sincronización de hora al arrancar**: `120`
- **Intervalo de reintento de sincronización**: `900`
- **Activar ventana de recuperación AP**: desactivado

Resultado: STA se enciende hasta 120 segundos para conseguir hora NTP. Si lo
consigue, el scheduler evalúa las reglas inmediatamente. Si falla, STA se apaga
y el siguiente intento silencioso ocurre 900 segundos después.

**4. Recuperación física sin dejar el AP encendido**

Objetivo: mantener el AP apagado normalmente, pero conservar una forma de volver
a entrar si la programación o el router están mal configurados.

- **Activar AP temporal con botón físico**: activado
- **Duración del AP temporal por botón**: `300`

Resultado: una pulsación corta de BOOT abre el AP durante 5 minutos. Mantener
BOOT entre 5 y 10 segundos sigue haciendo el factory reset existente.

**5. Recuperación AP automática tras fallo NTP**

Objetivo: si no consigue hora NTP, abrir AP temporalmente para poder entrar y
corregir WiFi o la configuración del scheduler.

- **Activar ventana de recuperación AP**: activado
- **Duración de ventana de recuperación AP**: `300`

Resultado: tras un fallo NTP en dormant, el AP se abre durante 5 minutos. Luego
el dispositivo vuelve a modo dormant y reintenta sincronizar hora más tarde.

Configuración base recomendada para un modo silencioso pero recuperable:

- **Intento de sincronización de hora al arrancar**: `120`
- **Intervalo de reintento de sincronización**: `900`
- **Activar ventana de recuperación AP**: desactivado
- **Activar AP temporal con botón físico**: activado
- **Duración del AP temporal por botón**: `300`
- **Mantener AP apagado durante ventana activa**: activado
- Reglas: **STA on**, **Tailscale on**, **AP off**

### Crear reglas

1. Abre **Scheduler**.
2. Comprueba **Timezone** y pulsa **Sync now** si la hora no es válida.
3. Selecciona un **Activation Profile** o **Operating Mode**.
4. Pulsa **Add rule**.
5. Selecciona días, hora de inicio/fin, estados AP/STA/Tailscale y, opcionalmente, una macro USB HID guardada.
6. Pulsa **Apply scheduler**.

Las reglas son semanales, no temporizadores de un solo uso. Una regla marcada
para martes se ejecuta todos los martes hasta que se cambie o desactive. Las
reglas no cruzan medianoche; usa dos reglas para una ventana que cruza de día.

Cuando una regla tiene una macro USB HID seleccionada, la macro se ejecuta una
vez cuando esa regla pasa a estar activa. No se repite continuamente durante la
ventana activa. Si el dispositivo arranca o se aplica la configuración del
scheduler estando ya dentro de una ventana activa, la macro se encola una vez
para esa ventana. Usa **No macro** cuando la regla solo deba controlar AP / STA /
Tailscale.

### Campos de Dormant Scheduled

| Campo | Rango / defecto | Significado |
|---|---:|---|
| **Intento de sincronización de hora al arrancar** | 30-600 s, defecto 120 | Tiempo máximo que STA puede permanecer activo intentando obtener hora NTP en Dormant Scheduled. AP permanece apagado. |
| **Intervalo de reintento de sincronización** | 60-86400 s, defecto 900 | Espera antes de repetir la adquisición NTP silenciosa tras un fallo. |
| **Activar ventana de recuperación AP** | defecto apagado | Abre el SoftAP temporalmente después de un fallo NTP en dormant. |
| **Duración de ventana de recuperación AP** | 30-1800 s, defecto 300 | Duración de la ventana AP temporal de recuperación. |
| **Activar AP temporal con botón físico** | defecto encendido | Permite abrir AP temporalmente con una pulsación corta de BOOT en Dormant Scheduled. |
| **Duración del AP temporal por botón** | 30-1800 s, defecto 300 | Duración de la ventana AP abierta por pulsación corta de BOOT. |
| **Mantener AP apagado durante ventana activa** | defecto encendido | Fuerza AP apagado aunque una regla activa tenga AP habilitado. Desactívalo solo si la ventana activa debe exponer administración/AP. |

## Referencia de Campos de la Web UI

### Dashboard

| Campo / control | Significado |
|---|---|
| **Upstream** | SSID STA actual e IP asignada por el router upstream. |
| **Signal** | RSSI de STA en dBm con barra visual de calidad. |
| **AP Clients** | Número de clientes conectados al SoftAP e IP del AP. |
| **System** | Heap libre y uptime. |
| **Tailscale** | Estado de Tailscale/MicroLink e IP VPN cuando está conectado. |
| **Macro Storage** | Estado del almacenamiento SPIFFS de macros: espacio disponible, espacio usado y cantidad de macros guardadas. |
| **WiFi MACs** | MAC actuales de STA y AP. |
| **Scheduler** | Modo scheduler actual y resumen AP/STA/Tailscale. |
| **Pause STA / Resume STA** | Pausa o reanuda temporalmente la reconexión STA. El scheduler puede volver a aplicar después su estado deseado. |
| **Connectivity Test target** | Host o IP usada por el botón Ping. |
| **Connected Clients** | Lista de clientes del SoftAP; **Refresh** recarga la lista. |

### Config

**Upstream Network (STA)**

| Campo | Significado |
|---|---|
| **Scan Networks** | Escanea redes WiFi cercanas y puede rellenar el SSID STA. |
| **SSID** | SSID del router/red upstream usada por STA. |
| **Password** | Contraseña WPA/WPA2 personal. Si se deja en blanco al guardar, conserva la existente. |
| **WPA2-Enterprise (EAP)** | Activa autenticación enterprise en lugar de WPA personal. |
| **EAP Identity** | Identidad externa/anónima para PEAP/TTLS. |
| **EAP Username** | Usuario enterprise interno. |
| **EAP Password** | Contraseña enterprise. En blanco al guardar conserva la existente. |
| **Static IP (STA)** | Usa dirección manual para STA en lugar de DHCP. |
| **IP Address** | Dirección STA estática. Obligatoria si Static IP está activado. |
| **Netmask** | Máscara de red estática. Obligatoria si Static IP está activado. |
| **Gateway** | Router upstream para ruta por defecto. |
| **DNS 1 / DNS 2** | DNS estáticos para STA. |

**Access Point (AP)**

| Campo | Significado |
|---|---|
| **AP SSID** | Nombre de red del SoftAP. |
| **Hide AP SSID** | Oculta el nombre en beacon, pero no desactiva AP. |
| **AP Password** | Contraseña del SoftAP; mínimo 8 caracteres para WPA2. En blanco al guardar conserva la existente. |
| **Channel** | Canal AP fijo o Auto. |
| **Max Clients** | Máximo de clientes simultáneos del SoftAP. |

**Tailscale**

| Campo | Significado |
|---|---|
| **Enable Tailscale** | Activa el runtime MicroLink/Tailscale, sujeto al scheduler y a conectividad STA. |
| **Auth Key** | Auth key de Tailscale. En blanco al guardar conserva la existente; **Clear Key** la elimina. |
| **Device Name** | Nombre del nodo registrado en Tailscale. |
| **Control Host** | Control server alternativo; en blanco usa Tailscale por defecto. |
| **Max Peers** | Límite de peers, 1-64. |
| **Status** | Estado runtime actual. |
| **DERP / DISCO / STUN** | Activa relay, descubrimiento de peers y traversal NAT. |
| **Expose LAN over Tailscale** | Anuncia una subred LAN por Tailscale. Fuerza Gateway/SNAT. |
| **Subnet CIDR** | Subred IPv4 anunciada, por ejemplo `192.168.1.0/24`. |
| **Advertised Route / Route State** | Estado de anuncio de ruta, solo lectura. |
| **Network Behavior** | Muestra Repeater o Tailscale Gateway/SNAT. Gateway/SNAT desactiva NAPT del AP y port forwarding a clientes AP. |

**Port Forwarding**

| Campo / control | Significado |
|---|---|
| **Add Rule** | Añade una regla TCP/UDP, máximo 5. |
| **Enabled** | Interruptor por regla. |
| **Protocol** | Regla NAPT TCP o UDP; TCP también usa proxy local para acceso desde la misma LAN. |
| **External Port** | Puerto expuesto en la IP STA/Tailscale del ESP32. |
| **Internal IP** | IP destino del cliente detrás del AP. |
| **Internal Port** | Puerto destino en el cliente interno. |
| **Save & Apply** | Guarda valores de Config y reinicia WiFi si hace falta. |
| **Restart** | Reinicia el dispositivo sin cambiar configuración. |

### USB HID

| Campo / control | Significado |
|---|---|
| **Macro Name** | Nombre de una macro DuckyScript guardada. |
| **Keyboard Layout** | Layout usado para traducir texto y nombres de teclas. |
| **Macro Script** | Código DuckyScript. Los payloads tienen un límite por defecto de 512 KiB. |
| **Execute** | Valida y encola el script actual para HID real o dry-run. |
| **Stop** | Detiene la ejecución de forma ordenada. |
| **Panic** | Parada de emergencia y liberación de todas las teclas retenidas. |
| **Save** | Guarda metadatos de la macro en NVS y el fichero del payload en SPIFFS. |
| **Clear** | Limpia el editor. |
| **Keep Awake Enabled** | Activa pulso HID periódico cuando no hay macro ejecutándose. |
| **Keep Awake Key** | Tecla enviada periódicamente, como Scroll Lock, Pause/Break o F1-F12. |
| **Every Seconds** | Intervalo Keep Awake, 5-3600 segundos. |
| **Saved Macros** | Lista ligera de macros guardadas para cargar, ejecutar, renombrar o borrar. Los scripts se descargan solo cuando hacen falta. |
| **Examples** | Payloads de ejemplo incluidos. |

### Scheduler

| Campo / control | Significado |
|---|---|
| **Local time** | Hora local tras aplicar la zona horaria. |
| **NTP** | Estado de sincronización: esperando, hora válida o sincronizada. |
| **AP / STA / Tailscale** | Estado efectivo controlado por scheduler. En Dormant Scheduled pueden estar los tres apagados fuera de ventanas. |
| **Next change** | Próxima transición detectada del scheduler. |
| **Timezone** | Zona POSIX usada para evaluar reglas. |
| **Sync now** | Reinicia SNTP inmediatamente. |
| **Activation Profile** | Selector amigable: Normal, Home Stealth, Dormant Scheduled o Headless Dormant reservado. |
| **Operating Mode** | Selector crudo: Always on, Scheduled, Manual off, Dormant. |
| **Add rule / Clear rules** | Añade o elimina reglas semanales. |
| **Rule enabled** | Activa una regla. Las reglas desactivadas se ignoran. |
| **Days / quick days** | Días de la semana en los que la regla puede coincidir. |
| **Start / End** | Ventana local del mismo día. End debe ser posterior a Start. |
| **AP Enabled** | Estado AP deseado durante la regla. Dormant puede sobreescribirlo con **Mantener AP apagado durante ventana activa**. |
| **STA Enabled** | Estado STA deseado durante la regla. STA conecta al router upstream guardado. |
| **TS Enabled** | Estado Tailscale deseado durante la regla; el arranque efectivo requiere IP de STA. |
| **USB HID Macro** | Macro guardada opcional que se ejecuta una vez al activar la regla. |
| **Note** | Etiqueta libre de la regla. |
| **Apply scheduler** | Guarda zona horaria, perfil/modo, ajustes dormant y reglas. |

### Logs

| Campo / control | Significado |
|---|---|
| **Auto** | Refresca logs automáticamente mientras la pestaña Logs está abierta. |
| **Refresh** | Recarga la salida de logs. |
| **Device Logs** | Buffer de logs del sistema en memoria. |

### System

| Campo | Significado |
|---|---|
| **Hostname** | Hostname aplicado a las interfaces AP y STA. |
| **STA Retry Max** | Intentos STA fallidos antes de entrar en cooldown de recuperación. |
| **STA Backoff Max** | Backoff máximo de reconexión exponencial, en segundos. |
| **Custom STA MAC / STA MAC** | MAC unicast opcional para STA. |
| **Custom AP MAC / AP MAC** | MAC unicast opcional para AP. |
| **Tailscale boot delay** | Espera tras boot antes de permitir arranque de Tailscale, 0-300 segundos. |
| **WiFi recovery cooldown** | Cooldown de reconexión STA tras fallos repetidos, 5-600 segundos. |
| **Scheduler poll interval** | Intervalo de evaluación del scheduler, 5-300 segundos. |
| **NTP server 1 / 2** | Servidores SNTP usados por sincronización normal y dormant. |
| **Ping count / interval / timeout / total wait** | Parámetros del ping de diagnóstico del Dashboard. |
| **Scan active min / max / timeout** | Parámetros temporales del escaneo WiFi. |
| **Proxy buffer / socket timeout / idle timeout / accept retry / listen backlog** | Parámetros runtime del proxy TCP de port forwarding. |
| **System log level** | Verbosidad global de logs ESP-IDF. `None` oculta incluso errores. |
| **MicroLink / WireGuard log level** | Override de verbosidad para tags pesados de Tailscale/WireGuard. |
| **Username / New Password** | Credenciales Basic Auth de la web. Password mínimo 4 caracteres. |
| **Firmware .bin file** | Imagen OTA que se sube y se arranca. |
| **Factory Reset** | Borra configuración y reinicia. Mantener BOOT 5-10 segundos también resetea. |
| **Restart Device** | Reinicia conservando configuración. |

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
| | `GET` | `/api/usb-hid/macros` | Listar metadatos y estado de storage; no incluye scripts |
| | `GET` | `/api/usb-hid/storage` | Consultar bytes totales/usados/libres y límite de payload |
| | `POST` | `/api/usb-hid/macros` | Crear una nueva macro hasta el límite configurado |
| | `GET` | `/api/usb-hid/macros/{id}` | Detalles de una macro específica, incluido el script |
| | `PUT` | `/api/usb-hid/macros/{id}` | Actualizar una macro existente |
| | `DELETE`| `/api/usb-hid/macros/{id}` | Eliminar una macro |
| | `POST` | `/api/usb-hid/validate` | Validar sintaxis DuckyScript |
| | `POST` | `/api/usb-hid/execute` | Ejecutar script (Simulación o HID) |
| | `POST` | `/api/usb-hid/stop` | Detener ejecución de forma segura |
| | `POST` | `/api/usb-hid/panic` | Parada de emergencia y liberación |
| | `GET` | `/api/usb-hid/keepalive` | Consultar estado y contadores de Keep Awake |
| | `POST` | `/api/usb-hid/keepalive` | Configurar tecla e intervalo periódico de Keep Awake |
| **Scheduler**| `GET` | `/api/scheduler/status` | Estado, próximo evento y campos de Dormant Scheduled |
| | `GET` | `/api/scheduler/config` | Obtener reglas, `usb_hid_macro_id` opcional y ajustes dormant |
| | `POST` | `/api/scheduler/config` | Actualizar reglas, ajustes dormant y validar IDs de macros programadas |
| | `POST` | `/api/scheduler/sync` | Forzar sincronización NTP |
| **Config** | `GET` | `/api/config` | Obtener configuración global |
| | `POST` | `/api/config` | Actualizar configuración global |
| | `GET` | `/api/runtime/config` | Obtener ajustes runtime avanzados |
| | `POST` | `/api/runtime/config` | Actualizar ajustes runtime avanzados |
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

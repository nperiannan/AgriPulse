# AgriPulse — ESP32-S3 Controller Firmware

Irrigation, pump and tank-level controller built on the **Kinetic Dynamics Nebula S3** (ESP32-S3) board.

## Features

- **Dual-tank monitoring** — Overhead (OH) and Underground (UG) tanks via float switches and LoRa radio
- **Motor control** — Automatic and manual relay control for OH and UG pumps with safety interlocks
- **WiFi AP+STA** — Always-on access point with background STA reconnection (priority-ordered, async scan, 3 attempts/SSID, 15-min cooldown)
- **Web UI** — Built-in HTTP server with real-time status, WiFi management (scan-to-add), relay control, MQTT settings, MQTT broker config, and history graph
- **MQTT** — Publishes status to broker; subscribes to control commands; supports remote broker via domain name
- **MQTT Broker Config UI** — Change MQTT broker host/port from the built-in web page without USB; preset buttons for FQDN and LAN IP; live connection status indicator
- **NTP time sync** — Auto-sync with fallback to DS3231 RTC and NVS-persisted epoch
- **OTA updates** — ArduinoOTA over WiFi (hostname `agripulse`, port 3232); HTTP-based OTA via web app poll
- **LoRa radio** — RFM95 on HSPI for remote tank float switch data
- **LCD display** — 16×2 I2C display showing tank states and system status
- **Buzzer alerts** — Audible notifications for tank fill completion and fault conditions
- **History logging** — Ring-buffer event log stored in NVS, viewable in web UI
- **Transmitter lost detection** — Alerts when no LoRa packet received from OH transmitter for 90+ seconds

## Board Version — read this first

Two PCB revisions exist and **their pinouts differ**. This project targets the
**v1.x board**, which is the default build. The revision is selected at compile
time by the `BOARD_V2` macro, supplied by the PlatformIO environment — never
edit `Config.h` to switch boards.

| Board | Environment | Notes |
| --- | --- | --- |
| **v1.x (current target)** | `nebulas3` / `nebulas3_serial` | ESP32-S3-WROOM-1 **N16R8**, carries the ADE7758 energy meter and 3 relays |
| v2.0 | `nebulas3_v2` / `nebulas3_v2_serial` | Adds `-D BOARD_V2`; I2C, touch and float pins all move |

> **Do not enable PSRAM.** The N16R8 module's octal PSRAM occupies GPIO35–37, and
> **RLY3 (the changeover contactor) is on GPIO35**. `platformio.ini` sets
> `board_build.psram = disabled` for this reason — re-enabling it silently kills
> the changeover relay while a 3-phase motor is connected.

> **Flash:** the module has 16 MB, but `partitions/ota_dual_app.csv` currently maps
> only 4 MB (a leftover from the earlier 4 MB board). ~12 MB is unused.

## Hardware

Pin columns differ per board revision — use the column matching your build environment.

| Component | v1.x (default) | v2.0 (`BOARD_V2`) |
| --- | --- | --- |
| MCU | ESP32-S3-WROOM-1 N16R8 — 240 MHz dual-core, 320 KB RAM, 16 MB flash, PSRAM disabled | same |
| Board | Kinetic Dynamics Nebula S3 | — |
| I2C | SDA=18, SCL=17 | SDA=8, SCL=9 |
| RTC | DS3231 on I2C (0x68) | same |
| EEPROM | AT24C512 on I2C (0x50–0x57, auto-detected) | same |
| LCD | 16×2 I2C, auto-detected (0x27 / 0x3F) | same |
| LoRa | RFM95 on HSPI (MISO=12, MOSI=11, SCLK=13, CS=10, IRQ=14, RST=21) | DIO1 tied to GND |
| Relay 1 | GPIO1 — motor | same |
| Relay 2 | GPIO2 — motor | same |
| Relay 3 | GPIO35 — changeover contactor *(not yet driven by firmware)* | same |
| Energy meter | ADE7758 (3-phase) on FSPI: SCLK=4, MISO=5, MOSI=6, CS=48; IRQ→GPIO46 via optocoupler; isolated by ISO6741 | same |
| Buzzer | GPIO3 | same |
| Float switch | GPIO42 (UG tank) | GPIO47 |
| Touch buttons | OH=GPIO41, UG=GPIO40 | OH=GPIO17, UG=GPIO18 |

> Relay logic is **active-HIGH** (`RELAY_ON = HIGH`) — BC847C low-side drivers.
> The ADE7758 sits in a **mains-referenced ground domain** (`ADE_GND ≠ GND`);
> the ISO6741 and optocoupler maintain that isolation. Never bridge the grounds.

Board sources (schematic, PDF, photos) live in
[harwaredetails/esp32_4g_gateway](harwaredetails/esp32_4g_gateway). The pin
table above was extracted from that EasyEDA netlist, not read off the drawing.

## Open Items

Tracked so they are not lost between sessions.

### Blocking commissioning

| # | Item | Impact |
| --- | --- | --- |
| 1 | **CT ratio unknown** — not yet purchased | Driver stays `calibrated = false`, so **overload and dry-run trips are disarmed**. Enter the ratio to arm them. |
| 2 | **Voltage scaling defaults come from the previous board** | The 1 MΩ divider tolerance is board-specific; readings are approximate until recalibrated per unit. |
| 3 | **Local web UI has no authentication** — `isAuthenticated()` returns `true` | Anyone on the LAN or the AP can command the motor. Must be fixed before the hub can throw a 3-phase contactor from a web request. |

### Nice to have

| # | Item |
| --- | --- |
| 4 | Obtain the original `ade7758/` Arduino library to cross-check the register map, phase-sequence logic and interrupt handling. |
| 5 | Folder is spelled `harwaredetails` (missing a `d`). Rename to `hardwaredetails` when convenient. |
| 6 | Partition table maps only 4 MB of the module's 16 MB flash. |
| 7 | `IO39–42` are the JTAG lines (`TCK/TDO/TDI/TMS`) yet v1 uses `40/41` for touch and `42` for float. Consistent with the board revision notes, but the first place to look if touch or float misbehave. |
| 8 | Credentials committed in the deploy scripts and the `tank1234` AP password should be rotated and moved out of git. |

## Build & Flash

**Prerequisites:** PlatformIO Core or PlatformIO IDE extension.

### v1.x board (default target)

```bash
pio run -e nebulas3        --target upload   # OTA over WiFi
pio run -e nebulas3_serial --target upload   # USB serial
```

### v2.0 board

```bash
pio run -e nebulas3_v2        --target upload   # OTA over WiFi
pio run -e nebulas3_v2_serial --target upload   # USB serial
```

Hold **BOOT** button during "Connecting..." if auto-reset doesn't trigger.

## Network Configuration

| Service | Default |
| --- | --- |
| AP SSID | `AgriPulse` |
| AP Password | `tank1234` |
| AP IP | `192.168.4.1` |
| Web UI | `http://agripulse.local` or `http://192.168.4.1` |
| OTA hostname | `agripulse` |
| OTA password | `tank1234` |
| MQTT broker | `nperiannan-nas.freemyip.com:1883` |
| MQTT topic pub | `tm/{mac}/status` |
| MQTT topic sub | `tm/{mac}/control` |

## WiFi State Machine

Networks are tried in priority order (top = highest priority in web UI):

- 3 attempts per SSID, 10 s timeout each
- After all SSIDs fail: 15-minute cooldown before retrying
- Async WiFi scan before each round (AP stays alive during scan)

## Project Structure

```text
pulse-hub/
├── include/          # Header files + Config.h (all pin/config constants)
├── src/              # Source files
│   ├── main.cpp
│   ├── WiFiManager.cpp   # AP+STA state machine, NTP, OTA
│   ├── HttpServer.cpp    # Web UI + REST API + MQTT broker config
│   ├── MQTTManager.cpp   # MQTT pub/sub with command queue + runtime broker reload
│   ├── MotorControl.cpp  # Relay logic + safety interlocks
│   ├── Scheduler.cpp     # Timed task runner
│   ├── Sensors.cpp       # Float switch reading
│   ├── LoRaManager.cpp   # RFM95 radio
│   ├── Display.cpp       # LCD driver
│   ├── Buzzer.cpp        # Alert tones
│   ├── History.cpp       # NVS event ring buffer
│   ├── RTCManager.cpp    # DS3231 RTC
│   └── Logger.cpp        # Serial + NVS log
├── platformio.ini    # Build environments (nebulas3, nebulas3_serial)
└── CHANGELOG.md
```

## REST API Endpoints

| Method | Path | Description |
| --- | --- | --- |
| GET | `/mqttconfig` | Returns current MQTT broker, port, and connection status |
| POST | `/setmqttbroker` | Sets broker host and port, saves to NVS, reconnects |
| GET | `/status` | Returns full system status JSON |
| POST | `/control` | Motor ON/OFF, settings changes |
| GET | `/wifiscan` | Triggers async WiFi scan |
| POST | `/wifiadd` | Adds a WiFi network |

## Related

- **Web App**: Go backend + React/Ant Design frontend, Docker on Oracle Cloud VM
  - URL: `http://nperiannan-nas.freemyip.com:1880` (IP: `150.230.129.215`)
- **MQTT broker**: Mosquitto on Oracle Cloud VM port 1883, externally reachable via same domain

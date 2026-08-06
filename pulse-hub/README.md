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

## Hardware

| Component | Details |
| --- | --- |
| MCU | ESP32-S3, 240 MHz dual-core, 320 KB RAM, 4 MB Flash |
| Board | Kinetic Dynamics Nebula S3 |
| RTC | DS3231 on I2C (SDA=8, SCL=9) |
| EEPROM | AT24C512 on I2C |
| LoRa | RFM95 on HSPI (MISO=12, MOSI=11, SCLK=13, CS=10, IRQ=14, RST=21) |
| Relays | OH motor GPIO1, UG motor GPIO2 |
| Buzzer | GPIO3 |
| Float switch | GPIO47 (UG tank) |
| Touch buttons | OH GPIO17, UG GPIO18 |
| LCD | 16×2 I2C, auto-detected (0x3F on v2.0 board) |

## Build & Flash

**Prerequisites:** PlatformIO Core or PlatformIO IDE extension.

### OTA (over WiFi)

```bash
pio run -e nebulas3 --target upload
```

### Serial (USB, COM7)

```bash
pio run -e nebulas3_serial --target upload
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

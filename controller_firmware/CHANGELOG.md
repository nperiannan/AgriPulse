# Changelog

All notable changes to the Tank Monitor ESP32-S3 firmware are documented here.

---

## [2.10.0] — 2026-08-04

### Added
- **MQTT watchdog reboot**: if the MQTT connection stays down for a configurable number of
  minutes (`mqtt_watchdog_min`, range 10–60, default 15), the controller now calls
  `esp_restart()` to recover on its own instead of staying silently unreachable. Checked
  regardless of WiFi state (covers both "WiFi up but broker/internet unreachable" and a fully
  dropped WiFi link) and skipped while in AP setup mode so initial WiFi configuration isn't
  interrupted. Configurable from the mobile app and the web app's Device Settings (new
  `set_setting` key `mqtt_watchdog_min`), persisted to NVS like the other motor settings, and
  reported in both the MQTT and direct-mode HTTP status JSON.

---

## [2.9.0] — 2026-07-30

### Added
- **Motor start rejection codes** (`oh_rej` / `ug_rej` in the status JSON, `MOTOR_REJ_*` in
  `Config.h`). A manual start into an already-full tank changes neither the relay nor the
  buzzer, so the app had nothing to acknowledge and fell back to a misleading
  *"command not delivered — no ack from controller"* after ~12 s. The controller now reports
  **why** the request was a no-op, and the app shows *"Not started — the overhead tank is
  already full."* instead. The code is cleared on the next manual command for that motor.

### Changed
- **Manual starts into a FULL tank are now refused up-front** in `turnOnOHMotor()` /
  `turnOnUGMotor()`. Previously the request was accepted and then undone moments later —
  either cancelled after the full 30 s buzzer delay, or energised for a single loop before
  the FULL safety stop fired. Both wasted the user's time and produced confusing UI. Only
  applies when `manualAutoStop` is enabled; with it off, manual override still works as
  before. The existing post-buzzer-delay cancel is kept as a safety net for the case where
  the tank fills *during* the countdown.

---

## [2.8.1] — 2026-07-30

### Changed
- **Removed periodic log-snapshot publish** — `tm/{mac}/logs` was being republished
  unconditionally every 60 s regardless of whether anything new was logged, making it the
  single biggest contributor to controller MQTT data usage (est. ~0.75 GB/yr on its own).
  Logs are now published **on-demand only**: the `get_logs` command (already used by the
  app/web log viewer) and the existing OTA-start/OTA-poll log acks. No functional change to
  the logs UI — it already requests a fresh snapshot explicitly.

---

## [2.8.0] — 2026-07-29

### Changed
- **MQTT traffic streamlining** — cut mobile-data usage for controllers on metered SIM/router
  connections (~12.2 GB/yr → ~1.7 GB/yr estimated). `MQTT_PUBLISH_MS` relaxed from 5 s to
  **60 s**: the periodic `status` publish is now mainly a keep-alive (so the backend's
  `last_seen`/online indicator stays fresh) rather than the primary update path.
  `MQTT_HEARTBEAT_MS` relaxed from 2 min to **5 min**.

### Added
- **Event-driven status publish on tank-state change** — OH tank-state changes (LoRa float
  packets, `LoRaManager.cpp`) and UG tank-state changes (`Sensors.cpp`) now trigger an
  immediate `publishMQTTStatus()`, same pattern already used for motor on/off — so real
  changes are never delayed by the new 60 s keep-alive interval. Transmitter-lost transitions
  also publish immediately.
- **New `"sync"` MQTT command** — manual "pull to refresh" from the app (sync button, app
  open, or RF icon tap) forces an immediate full status publish on demand
  (`MQTTManager.cpp processPendingMQTT()`).

---

## [2.7.1] — 2026-07-19

### Fixed
- **Scheduled motor runs were ~30 s shorter than configured** — a scheduled start (e.g. 10:00 PM) called `turnOnOHMotor()`/`turnOnUGMotor()`, which (when the pre-motor buzzer is enabled) queues a 30 s buzzer-delay countdown *before* energising the relay. The scheduler stamped `startTime` at the moment the schedule fired (10:00:00), not when the relay actually energised (10:00:30), so the stop check — measured from `startTime` — cut the run 30 s early (e.g. a 2 min schedule only pumped for 1:30). Fixed by moving the buzzer warning to run *before* the scheduled time instead of after: `checkSchedules()` now polls every 1 s (was 10 s) and sounds the buzzer `MOTOR_START_BUZZER_DELAY_MS` (30 s) ahead of the scheduled time (`Schedule.preBuzzing`), then energises the relay immediately and bypasses the buzzer-delay queue at the exact scheduled second via new `startScheduledOHMotor()`/`startScheduledUGMotor()` (`MotorControl.cpp`). The motor now starts exactly on schedule and runs the full configured duration; the buzzer still gives the same 30 s heads-up, just earlier.

---

## [2.7.0] — 2026-07-19

### Added
- **MQTT heartbeat round-trip health check** — the controller now publishes a small request on `tm/{mac}/hb` every `MQTT_HEARTBEAT_MS` (2 min). The backend replies with a `ping` control command, which the controller acks straight back on the same topic. A completed round trip proves the full backend↔broker↔device command path is alive — not just that this device is publishing status, which is the passive signal that failed to catch a ~11 min backend-side MQTT outage on 2026-07-19 (see `web/backend` CHANGELOG / incident notes). If no `ping` arrives within `MQTT_HEARTBEAT_ACK_TIMEOUT_MS` (20 s) of a request, a `[HB]` WARN is logged locally (visible via `get_logs`).

### Changed
- **WiFi network deletion is now refused for the currently-connected network and for the last remaining saved network** — previously `wifi_delete` (mobile/web/BLE) could remove the SSID currently providing internet/remote access, forcibly disconnecting the device with no way to fix it remotely. `handleRemoveNetwork()` in `WiFiManager.cpp` now logs a WARN and no-ops instead.

---

## [2.6.2] — 2026-07-16

### Fixed
- **Slow motor-command acknowledgement** — oh_on/oh_off/ug_on/ug_off (and the buzzer-delay countdown finishing) previously only appeared in the next periodic MQTT status publish, up to `MQTT_PUBLISH_MS` (5s) later. Combined with cloud round-trip latency this could push the app's ack detection past its timeout, producing false "Not delivered" errors. Motor commands and buzzer-countdown completions now trigger an immediate status publish, matching the existing OTA/reboot pattern.
- **Direct-mode (local WiFi) buzzer countdown bar showed as already empty** — the local `/status` HTTP endpoint (used when connected directly to the controller, bypassing the cloud) never included `ohCd`/`ugCd` (buzzer seconds remaining) or `ohRsn`/`ugRsn`, added only to the MQTT status in v2.5.0/2.6.0. Added to `/status` so the app's countdown bar and reason data work in direct mode too.

---

## [2.6.1] — 2026-07-16

### Fixed
- **OH motor kept running when the tank was already FULL** — the overhead auto-stop was gated behind the 60 s min-run hysteresis, so a manual (or auto) OH run at FULL kept pumping for up to a minute (the UG float switch stops immediately, which is why UG stopped but OH didn't). FULL is now treated as an immediate overflow-safety stop for OH, bypassing the hysteresis and mirroring UG. A manual OH start into an already-FULL tank is cancelled outright (when auto-stop is enabled) instead of briefly energising the relay. Scheduled runs keep their existing immunity.

---

## [2.6.0] — 2026-07-16

### Added
- **Motor-change reason in MQTT status** — the status payload now includes `oh_rsn` / `ug_rsn` (the `HistReason` code of the most recent OH/UG relay change: Auto level, Manual app/web/touch, Scheduled, Tank full, Max runtime, LoRa lost, Power cut/restore). The cloud backend uses these to label derived history events accurately instead of guessing "Manual" vs "Auto/Scheduled" from timing.

---

## [2.5.0] — 2026-07-20

### Added
- **Buzzer countdown in MQTT status** — the status payload now includes `oh_cd` / `ug_cd` (buzzer-delay seconds remaining, `0` when idle). Lets the mobile app render an accurate shrinking "starting in Ns" progress bar during the pre-start buzzer, and confirms the controller acknowledged a motor command.

---

## [2.4.0] — 2026-07-13

### Added
- **v2.0 PCB support (`BOARD_V2`)** — build-time board select via PlatformIO envs `nebulas3_v2` / `nebulas3_v2_serial` (or `#define BOARD_V2`). v2.0 pin map: I2C SDA=8/SCL=9, LoRa DIO1 not wired (`RADIOLIB_NC`), touch buttons GPIO17/18, UG float GPIO47. v1.x pins are unchanged when the guard is off.
- **LCD I2C auto-detect** — scans the bus at boot and rebuilds the LCD object with the found address (handles 0x27 vs 0x3F backpacks); logs all I2C devices found.
- **Event-history reasons** — every motor ON/OFF now records *why* (Auto/level, Manual app/web/touch, Scheduled, Tank-full, Max runtime, LoRa lost, Power cut, Power restore) plus a boot reason (power-on/OTA/brownout). Packed into the record's spare flags nibble — no EEPROM migration.
- **Power-cut inference** — on boot, if a power reset occurred while a motor was intended ON, a synthetic OFF is logged, backdated to the last heartbeat (persisted only while pumping).
- **About / Device Info** on the ESP32 web UI — controller & transmitter FW, LCD/EEPROM/RTC I2C addresses, and RAM/flash usage.
- **Run-pairing** in the web Event History — motor ON→OFF merged into one row with duration and start/stop reasons.
- **LCD motor-start countdown** — a priority screen with a shrinking bar shows the pending motor and seconds remaining during the buzzer delay.

### Fixed
- **Web UI unreachable once the RFM95 was fitted** — `pollLoRa()` used a blocking `radio.receive()` (~410 ms/iteration) that starved the synchronous web server. Switched to interrupt-driven RX (`setPacketReceivedAction` + `readData`) so the loop stays responsive.
- **Buzzer silenced when cancelling one of two pending motors** — the pre-start buzzer now only stops when no motor is still counting down.
- **Push-button couldn't cancel during the countdown** — a second tap while pending now cancels the start (stops buzzer, no motor, no phantom OFF record).

---

## [2.2.0] — 2026-06-10

### Fixed
- **MQTT status payload missing RF fields** — Added `loraRSSI`, `loraSNR`, and `lastLoraReceived` to the MQTT status JSON payload. Previously these fields were only available via the HTTP API, causing the mobile app (which uses WebSocket/MQTT) to show 0 for all RF signal data.

---

## [1.3.6] — 2026-04-24

### Added
- **`buzzer_active` in MQTT and HTTP status** — Boolean field indicating whether the buzzer is currently sounding. Consumed by mobile app to show an animated buzzer icon.

---

## [1.3.5] — 2026-04-24

### Changed
- **Log timestamp** — Replaced raw milliseconds (`[3219719]`) with human-readable `[HH:MM:SS]` format. Wall-clock time is used when NTP is synced; uptime `[+HH:MM:SS]` is shown before first sync.

---

## [1.3.4] — 2026-04-24

### Added
- **Configurable log level** — Log level can be set to `Info` (Info + Warn + Error, default) or `Debug` (all messages). Configurable from the ESP32 built-in web page (Logs card), mobile app (System tab), and web app. Setting is applied instantly via HTTP POST `/setloglevel` or MQTT `set_log_level` command.
- **`log_level` in MQTT status** — Every status publish now includes the current log level so all clients can reflect it.

### Changed
- **MQTT status publish log** — The periodic `[MQTT] Published status` message is now a `DEBUG`-level log (suppressed in `Info` mode) to reduce noise.

---

## [1.3.3] — 2026-04-24

### Fixed
- **LCD splash screen** — Startup message now shows the actual firmware version (`FW: 1.3.3`) instead of hardcoded `Float v1.0`.

---

## [1.3.2] — 2026-04-24

### Added
- **LCD backlight mode selector** — Web UI setting to choose `Auto`, `Always On`, or `Always Off`. Persisted in NVS, applied immediately.
- **MQTT password change from web UI** — Settings card with password input and confirm dialog. Sends `set_mqtt_creds` command over MQTT to update NVS credentials on the ESP32 — no reflash needed.

### Removed
- **BLE (Bluetooth Low Energy) toggle removed from web UI** — BLE was already removed from firmware in v1.1.0; the toggle in the settings page has now been removed to match.

---

## [1.1.0] — 2026-04-19

### Fixed
- **ESP32-S3 crash/reboot loop** — BLE library registered with the WiFi coexistence module even without calling `BLEManager::begin()`, causing an abort when `WiFi.mode(WIFI_AP_STA)` was set with `PS_NONE`. Fixed by excluding `BLEManager.cpp` from the build via `build_src_filter`.
- **MQTT callback stack overflow** — `onMessage()` was directly invoking NTP sync, BLE send and `ESP.restart()` from within the PubSubClient callback (small stack). Fixed by queuing the raw payload and processing it from the main loop via `processPendingMQTT()`.
- **Adding a new WiFi SSID disconnected the active connection** — `handleAddNetwork()` called `WiFi.disconnect()` unconditionally. Fixed to skip disconnect if the device is already connected.
- **MQTT broker hardcoded to LAN IP** — Broker was hardcoded to `192.168.0.102` and overwritten in NVS on every boot. Fixed by using `nperiannan-nas.freemyip.com` as default and adding NVS migration (`seedDefaultsIfEmpty`) that rewrites the old IP only once.
- **AP mode unstable during STA reconnect** — `WiFi.scanNetworks(false)` (blocking scan, ~3 s) was disrupting AP beaconing, causing clients to drop. Changed to `WiFi.scanNetworks(true)` (async) with polling via `WiFi.scanComplete()` so the AP stays fully alive during scanning.

### Added
- **WiFi state machine** — Priority-ordered STA reconnection with 3 attempts per SSID (10 s timeout each) and 15-minute cooldown after all SSIDs fail. State tracked via `smTryIdx`, `smTryAttempts`, `smInCooldown`, `smCooldownUntilMs`.
- **WiFi scan diagnostic logging** — Before each reconnect round the firmware logs all visible SSIDs with channel and RSSI.
- **India WiFi country code** — `esp_wifi_set_country_code("IN", true)` enables channels 1–13, preventing some APs from being missed.
- **WiFi scan-to-add UI** — "Scan" button in the web UI calls `/wifiscan` REST endpoint, shows a dropdown sorted by RSSI with signal-strength bars, lock icon and channel/RSSI info. Clicking a network pre-fills the SSID field.
- **MQTT NVS migration** — `seedDefaultsIfEmpty()` migrates devices that still have the old hardcoded LAN IP stored in NVS to the new domain name without requiring a full factory reset.
- **NVS epoch persistence** — Current time is written to NVS every 5 minutes so the clock survives a power cut even if the DS3231 backup battery is dead.

### Removed
- **BLE (Bluetooth Low Energy) completely removed** — `BLEManager.cpp` and all references removed from `main.cpp`, `MQTTManager.cpp`, `HttpServer.cpp` and the web UI. The `handleSetBleEnabled()` HTTP handler is stubbed to return OK for backward compatibility.

### Changed
- **MQTT default broker** — Changed from `192.168.0.102` to `nperiannan-nas.freemyip.com` in `Config.h`.
- **WiFi add-network toast** — Changed from "Network added – connecting..." to "Network saved" since the connection attempt is no longer immediate.
- **WiFi remove-network** — `handleRemoveNetwork()` now only disconnects if the removed SSID is the currently connected SSID.

### Infrastructure
- Port forwarding configured through double NAT (Syrotech BSNL ONT → TP-Link ER605):
  - TCP 1883 → NAS Mosquitto (MQTT, externally reachable)
  - TCP 1880 → NAS Web App (dashboard, externally reachable)
- External URL: `http://nperiannan-nas.freemyip.com:1880` (web UI), `nperiannan-nas.freemyip.com:1883` (MQTT)

---

## [1.0.0] — Initial Release

- Dual-tank monitoring via float switches and LoRa (RFM95)
- Motor relay control with safety interlocks
- WiFi AP+STA, web UI, MQTT, NTP, OTA, DS3231 RTC, LCD, Buzzer, History log

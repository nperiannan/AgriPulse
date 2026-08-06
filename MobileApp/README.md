# Tank Monitor — Mobile App

Flutter Android app for remote monitoring and control of the TankMonitor ESP32 system.

## Features

- **Tank status** — Animated arc gauges for OH and UG tanks (FULL / HALF / LOW / EMPTY)
- **Motor control** — ON/OFF buttons with status pills and buzzer indicator
- **Transmitter lost warning** — Banner when OH transmitter loses LoRa signal
- **Motor scheduler** — Add/edit/delete timed schedules per motor with time picker
- **Settings** — Display-only mode, motor thresholds (start/stop level, max runtime), LCD backlight, MQTT password
- **OTA firmware update** — Two-step: upload `.bin` → flash with phase tracking and 150s countdown
- **Firmware rollback** — Revert to previous OTA partition
- **Device logs** — Fetch/display with level filter, color-coded, clipboard copy
- **System info** — WiFi RSSI, LoRa, TX status, uptime, firmware versions, web app version
- **Multi-device support** — Claim devices by MAC, switch between devices
- **Admin panel** — View all users and their claimed devices (admin role only)
- **Dual URL config** — WiFi URL (LAN) + Mobile Data URL (internet), auto-switches based on network
- **Auto-update** — Checks GitHub releases for newer APK, downloads and opens for install
- **Auth** — JWT login/register with persistent token

## Screens

| Screen | Description |
| --- | --- |
| Login | Username/password sign-in |
| Register | Create new account |
| Setup | Configure WiFi + Mobile Data server URLs |
| Device List | List claimed devices, tap to open dashboard |
| Claim Device | Claim device by MAC address |
| Dashboard | 4-tab layout: Dashboard, Settings, System, Logs |
| Admin | All users and their devices (admin only) |

## Project Structure

```text
MobileApp/
├── lib/
│   ├── main.dart              # App entry, routing, Provider setup
│   ├── login_screen.dart      # Login
│   ├── register_screen.dart   # Registration
│   ├── setup_screen.dart      # Server URL config
│   ├── device_list_screen.dart # Multi-device list
│   ├── dashboard_screen.dart  # Main 4-tab dashboard
│   ├── claim_screen.dart      # Claim device by MAC
│   ├── admin_screen.dart      # Admin panel
│   ├── schedule_sheet.dart    # Motor schedule bottom sheet
│   ├── tank_service.dart      # Auth, WebSocket, REST API service
│   └── models.dart            # Data models (Status, Schedule, Device)
├── pubspec.yaml
└── android/
```

## Build

```bash
flutter build apk --release
```

The release APK is at `build/app/outputs/flutter-apk/app-release.apk`.

## Install

Download the latest APK from [GitHub Releases](https://github.com/nperiannan/TankMonitor/releases) (tag prefix `MobileApp/`).

On first launch:

1. Enter server URL: `http://nperiannan-nas.freemyip.com:1880`
2. Log in with your credentials

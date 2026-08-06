# AgriPulse — Pulse Cloud

Go backend + React/Ant Design frontend, deployed as a Docker container on a cloud VM.

- **Backend version**: 2.5.0
- **Frontend version**: 2.5.0
- **Docker image**: `agripulse-cloud:2.5.0`

## Features

### Backend (Go)

- **Auth** — JWT-based login/register with SQLite user store
- **MQTT bridge** — Connects to Mosquitto broker, relays status → WebSocket and control commands → MQTT
- **WebSocket** — Real-time push of device status to connected web/mobile clients
- **Device registry** — Claim/unclaim devices by MAC address, per-user device list
- **OTA firmware staging** — Upload `.bin`, serve to ESP32 via `GET /api/ota/check/{mac}`
- **Version endpoint** — `GET /api/version` returns `{"web_version":"2.1.0"}`
- **Transparent pass-through** — No field parsing; forwards MQTT JSON payloads as-is to clients

### Frontend (React SPA)

- **Tank status** — Animated arc gauges for OH and UG tanks (FULL / HALF / LOW / EMPTY / UNKNOWN)
- **Motor control** — ON/OFF buttons with status pills and buzzer indicator
- **Transmitter lost warning** — Banner when no LoRa signal for 90+ seconds, shows last known OH state
- **Motor scheduler** — Add/edit/delete timed schedules per motor
- **Settings** — Display-only mode, motor start/stop level thresholds, max runtime, LCD backlight, MQTT password
- **OTA firmware update** — Upload `.bin` with progress bar, flash with phase tracking and 150s countdown
- **Firmware rollback** — Revert to previous OTA partition
- **Device logs** — Fetch and display logs with level filter and clipboard copy
- **System info** — WiFi RSSI, LoRa status, TX status, uptime, firmware versions
- **Dark/light theme** — Toggle with localStorage persistence
- **Auth** — Login/logout with JWT token

## Project Structure

```text
web/
├── backend/
│   └── main.go           # Go server (auth, MQTT bridge, WebSocket, OTA, REST API)
├── frontend/
│   ├── src/
│   │   ├── App.tsx        # Main React SPA (all UI in single file)
│   │   └── types.ts       # TypeScript interfaces (Status, ControlCmd, OtaStatus)
│   ├── package.json
│   └── vite.config.ts
├── Dockerfile             # Multi-stage: Node build → Go build → scratch
├── build.sh                # Build + deploy script for the cloud VM
├── build.ps1              # Windows build script
└── .dockerignore
```

## API Endpoints

| Method | Path | Description |
| --- | --- | --- |
| POST | `/api/auth/register` | Create account |
| POST | `/api/auth/login` | Login, returns JWT |
| GET | `/api/version` | Web app version |
| GET | `/api/devices` | List user's claimed devices |
| POST | `/api/devices/claim` | Claim device by MAC |
| GET | `/api/ota/check/{mac}` | ESP32 polls for staged firmware |
| POST | `/api/ota/upload` | Upload firmware `.bin` |
| WS | `/ws` | WebSocket for real-time status/control |

## Build & Deploy

### GitHub Actions build (recommended — fast)

`.github/workflows/build-web.yml` builds the Docker image on every `web/vX.Y.Z`
tag push (or manually via workflow_dispatch) and publishes it to
`ghcr.io/nperiannan/agripulse-cloud:<version>`. On the Oracle VM this collapses
the ~20 min local `docker build --no-cache` down to just a `docker pull`:

```bash
ssh -i "~/.ssh/Oracle VMs/rocky/ssh-key-2026-06-06.key" hainatraj@150.230.129.215
# scp deploy_oracle_ghcr.sh to /tmp first, then:
bash /tmp/deploy_oracle_ghcr.sh 2.5.0
```

One-time setup: make the `agripulse-cloud` GHCR package Public (GitHub →
profile → Packages → agripulse-cloud → Package settings) so the VM can pull
without authenticating, or `docker login ghcr.io` on the VM with a
`read:packages` PAT if it must stay private.

### On Oracle Cloud VM (manual local build — slower, no GitHub Actions needed)

```bash
ssh -i "~/.ssh/Oracle VMs/rocky/ssh-key-2026-06-06.key" hainatraj@150.230.129.215
cd /opt/AgriPulse/pulse-cloud
git pull origin main
VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
sudo docker build --no-cache -t agripulse-cloud:${VERSION} .
sudo docker stop agripulse-cloud && sudo docker rm agripulse-cloud
sudo docker run -d --name agripulse-cloud --network agripulse --restart always \
  -p 1880:8080 -v /opt/agripulse/data:/data \
  -e MQTT_BROKER=agripulse-mosquitto -e MQTT_PORT=1883 \
  -e MQTT_USER='<secret>' -e MQTT_PASS='<secret>' \
  -e AUTH_USER=admin -e AUTH_PASS='<secret>' \
  -e AUTH_SECRET='<secret>' \
  -e OTA_BASE_URL=http://150.230.129.215:1880 \
  agripulse-cloud:${VERSION}
```

> **Note:** Oracle Cloud VM IP is static (150.230.129.215). DNS `nperiannan-nas.freemyip.com` points to it. No DDNS cron needed.

### Local development

```bash
# Frontend
cd frontend && npm install && npm run dev

# Backend
cd backend && go run main.go
```

## Environment Variables

| Variable | Description | Default |
| --- | --- | --- |
| `MQTT_BROKER` | Mosquitto broker address | `mosquitto` |
| `MQTT_PORT` | Broker port | `1883` |
| `MQTT_USER` | MQTT username | *(see private config)* |
| `MQTT_PASS` | MQTT password | — |
| `AUTH_SECRET` | JWT signing key | — |
| `OTA_BASE_URL` | Base URL for OTA firmware downloads | — |
| `PORT` | HTTP server port | `8080` |

## Access

| | URL |
| --- | --- |
| Oracle Cloud (primary) | <http://150.230.129.215:1880> |
| Public (DNS) | <http://nperiannan-nas.freemyip.com:1880> |

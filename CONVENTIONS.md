# AgriPulse Repo Conventions

## Repository

- Monorepo: `pulse-hub/` (controller firmware), `pulse-node/` (remote node firmware), `pulse-cloud/` (Go+React server), `pulse-app/` (Flutter app)
- Remote: <https://github.com/nperiannan/AgriPulse.git>
- Branch: `main`
- **Never push this repo's work to `nperiannan/TankMonitor`** — it is a separate upstream project this code was ported from

## CRITICAL — Commit & Version Discipline

- **Every successful build must be immediately committed to git** — never leave built/tested changes uncommitted
- **Version strings must be updated BEFORE building**, not after — the binary must embed the correct version
- **AI agents must ALWAYS perform the full checklist below** when committing changes to any component, without waiting to be reminded
- After a successful build + test, the full checklist is:
  1. Update ALL version strings for the changed component (see list below)
  2. Update `README.md` versions table
  3. `git add` + `git commit` with descriptive message
  4. `git tag -a <component>/vX.Y.Z -m "..."` (annotated tag)
  5. `git push origin main && git push origin <tag>`
  6. Delete old GitHub release for that component, create new one via `gh release create`
  7. Rebuild binary/image for the changed component and deploy/install
- **Never let version strings drift** — the binary version, git tag, README table, and GitHub release must all agree
- **Do not skip the release checklist** — even for "small" changes like refactors or race condition fixes that affect user-facing behaviour

## Versioning & Tags

- Tag format: `pulse-hub/vX.Y.Z`, `pulse-node/vX.Y.Z`, `pulse-cloud/vX.Y.Z`, `pulse-app/vX.Y.Z`
- **One release per component** — no combined multi-component releases
- Tags must be **annotated** (not lightweight): `git tag -a pulse-cloud/vX.Y.Z -m "..."`
- Version strings live in:
  - Hub firmware: `pulse-hub/include/Config.h` → `#define FW_VERSION`
  - Node firmware: `pulse-node/include/Config.h` → `#define FW_VERSION`
  - Cloud backend: `pulse-cloud/backend/main.go` → `const webVersion`
  - Cloud frontend: `pulse-cloud/frontend/src/App.tsx` → `const WEB_APP_VERSION` ← must ALSO be bumped (hardcoded, not fetched from server)
  - App: Flutter `pulse-app/pubspec.yaml` → `version:` AND `pulse-app/lib/tank_service.dart` → `const mobileAppVersion` (both must be updated together)
- `README.md` versions table must also be updated on every bump

## GitHub Releases — IMPORTANT

- **Exactly one release per component** on the releases page at all times (4 total: hub, node, cloud, app)
- When creating a new release for a component, **always delete the previous release first**:

  ```bash
  gh release delete pulse-hub/vX.Y.Z --cleanup-tag --yes
  gh release create pulse-hub/vX.Y.Z firmware.bin --title "Pulse Hub Firmware vX.Y.Z" --notes "..."
  ```

- Release titles: `Pulse Hub Firmware vX.Y.Z`, `Pulse Node Firmware vX.Y.Z`, `Pulse Cloud vX.Y.Z`, `Pulse App vX.Y.Z`
- **Required assets per release** (critical for OTA and auto-update to work):
  - `pulse-hub/vX.Y.Z` → must include `firmware.bin`
  - `pulse-node/vX.Y.Z` → must include `firmware.hex`
  - `pulse-app/vX.Y.Z` → must include `app-release.apk`
  - `pulse-cloud/vX.Y.Z` → source code only (deployed via Docker)
- **Tag prefixes are load-bearing** — the app auto-update scans for the `pulse-app/` prefix and
  `pulse-cloud/backend/github_poller.go` scans for `pulse-hub/`
  - Do NOT use `app/` or `MobileApp/` — retired prefixes, no longer detected
  - Hub OTA uses the cloud API (`/api/ota/check/{mac}`), not GitHub releases

## NAS Deployment (TNAS @ 192.168.0.102) — Decommissioned

> **Superseded by Oracle Cloud VM** (see below). Kept for historical reference only.

- Was at `/Volume1/docker/TankMonitor` with sparse-checkout (`web/` only) — legacy upstream project
- git binary: `/home/nperiannan/miniconda3/bin/git`
- docker binary: `/Volume1/@apps/DockerEngine/dockerd/bin/docker`

## OrangePi (192.168.0.105) — Decommissioned

> **Superseded by Oracle Cloud VM**. Tailscale still active for other uses.

## Oracle Cloud VM — Primary Host (since 2026-06-22)

- IP: `150.230.129.215` (static, Oracle Always Free tier)
- OS: Rocky Linux 9.8, x86_64, 764 MB RAM + 5 GB swap
- SSH: `ssh -i "~/.ssh/Oracle VMs/rocky/ssh-key-2026-06-06.key" hainatraj@150.230.129.215`
- Docker: `agripulse-cloud` + `eclipse-mosquitto:2` on `agripulse` network
- Data: `/opt/agripulse/data` (SQLite), `/opt/agripulse/mosquitto` (config/data/log)
- Repo: `/opt/AgriPulse` (sparse checkout, `pulse-cloud/` only)
- DNS: `nperiannan-nas.freemyip.com` → `150.230.129.215` (static A record, no DDNS needed)
- VCN Security List: ports 22, 1880, 1883 open (0.0.0.0/0)
- `OTA_BASE_URL=http://150.230.129.215:1880`
- **Reason for move**: BSNL ISP blocks ALL inbound ports; Oracle Cloud has public static IP

## OTA Flash Flow

1. Build firmware locally: `pio run -e nebulas3_serial` → binary at `pulse-hub/.pio/build/nebulas3_serial/firmware.bin`
2. Upload via Pulse Cloud **Firmware Update (OTA)** → **Upload firmware.bin**
3. Click **Flash to ESP32**
4. Backend publishes `ota_start` MQTT command with `OTA_BASE_URL`-based download URL
5. ESP32 fetches binary over HTTP and flashes itself, then reboots
6. Success detected when firmware version string changes in the next MQTT status message

> If OTA shows "failed" after 120 s — check that `FW_VERSION` in `Config.h` was actually bumped.
> The backend detects success by watching for a version change; if the version didn't change, it times out.

## Keeping This File in Sync

- This file (`CONVENTIONS.md`) mirrors the AI session memory for this repo
- **Whenever a new rule or convention is established, update this file and push immediately**:

  ```bash
  git add CONVENTIONS.md
  git commit -m "docs: update CONVENTIONS.md — <what changed>"
  git push origin main
  ```

## Security — NEVER commit credentials without approval

- **NEVER commit passwords, tokens, secrets, or credentials to git without explicit user approval**
- Before committing any file that contains a password/secret (README, CONVENTIONS, docker-compose, shell scripts, `.env.example`), ask the user first
- Prefer placeholders: `YOUR_PASSWORD_HERE`, `<secret>`, etc.

## Git Notes

- `credential-winced` warning on push is harmless (typo in git credential helper config)
- Flutter auto-generated files (`pulse-app/linux/`, `pulse-app/macos/`, `pulse-app/windows/`) are not committed unless intentionally changed

## Terminal Hygiene

- Keep at most **1–2 terminal tabs** open at a time — close finished terminals immediately after use
- AI agents must not leave stray async terminals running after a task completes; call `kill_terminal` when done
- Before starting a new long-running command, check for and close idle terminals first

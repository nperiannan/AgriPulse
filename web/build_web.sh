#!/usr/bin/env bash
# build_web.sh — rebuild and redeploy the tankmonitor-web Docker container on the Oracle Cloud VM.
# Run from web/: bash build_web.sh
set -e

cd /opt/TankMonitor/web
git -C .. pull origin master

# Derive the version from the Go source so the image tag always matches the binary.
VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
echo "==> Building tankmonitor-web:${VERSION}"

docker build --no-cache -t tankmonitor-web:${VERSION} .

docker stop tankmonitor-web 2>/dev/null || true
docker rm   tankmonitor-web 2>/dev/null || true

docker run -d \
  --name tankmonitor-web \
  --restart always \
  --network tankmonitor \
  -p 1880:8080 \
  -v /opt/tankmonitor/data:/data \
  -e MQTT_BROKER=tankmonitor-mosquitto \\
  -e MQTT_PORT=1883 \
  -e MQTT_USER=tankmonitor \
  -e MQTT_PASS='Tank32!' \
  -e AUTH_USER=admin \
  -e AUTH_PASS='Tank32!' \
  -e AUTH_SECRET='1ee5cd0b3032e3d2d3613d23aa6b33d08890337cd7df504a9393dfa4f3e42a45' \
  -e OTA_BASE_URL=http://150.230.129.215:1880 \
  tankmonitor-web:${VERSION}

echo "--- Last 10 log lines ---"
docker logs tankmonitor-web --tail 10

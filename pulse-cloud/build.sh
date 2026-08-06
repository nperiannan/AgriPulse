#!/usr/bin/env bash
# build.sh — rebuild and redeploy the agripulse-cloud Docker container on the cloud VM.
# Run from pulse-cloud/: bash build.sh
set -e

cd /opt/AgriPulse/pulse-cloud
git -C .. pull origin main

# Derive the version from the Go source so the image tag always matches the binary.
VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
echo "==> Building agripulse-cloud:${VERSION}"

docker build --no-cache -t agripulse-cloud:${VERSION} .

docker stop agripulse-cloud 2>/dev/null || true
docker rm   agripulse-cloud 2>/dev/null || true

docker run -d \
  --name agripulse-cloud \
  --restart always \
  --network agripulse \
  -p 1880:8080 \
  -v /opt/agripulse/data:/data \
  -e MQTT_BROKER=agripulse-mosquitto \
  -e MQTT_PORT=1883 \
  -e MQTT_USER=tankmonitor \
  -e MQTT_PASS='Tank32!' \
  -e AUTH_USER=admin \
  -e AUTH_PASS='Tank32!' \
  -e AUTH_SECRET='1ee5cd0b3032e3d2d3613d23aa6b33d08890337cd7df504a9393dfa4f3e42a45' \
  -e OTA_BASE_URL=http://150.230.129.215:1880 \
  agripulse-cloud:${VERSION}

echo "--- Last 10 log lines ---"
docker logs agripulse-cloud --tail 10

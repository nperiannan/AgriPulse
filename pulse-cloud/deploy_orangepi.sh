#!/usr/bin/env bash
# deploy_orangepi.sh — rebuild and redeploy the agripulse-cloud stack on the OrangePi backup host.
# Run from anywhere; the script resolves its own location.
set -e

REPO_DIR=/opt/AgriPulse
DATA_DIR=/opt/agripulse/data
MOSQ_DIR=/opt/agripulse/mosquitto
NETWORK=agripulse

cd "$REPO_DIR/web"
git -C "$REPO_DIR" sparse-checkout reapply 2>/dev/null || true
git -C "$REPO_DIR" pull origin main

# Derive the version from the Go source so the image tag always matches the binary.
VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
echo "==> Building agripulse-cloud:${VERSION}"

# Ensure persistent data directories exist and are owned by the mosquitto container user (UID 1883)
mkdir -p "$DATA_DIR"
mkdir -p "$MOSQ_DIR/config" "$MOSQ_DIR/data" "$MOSQ_DIR/log"
chown -R 1883:1883 "$MOSQ_DIR/"

# Refresh mosquitto config from repo
cp mosquitto/mosquitto.conf "$MOSQ_DIR/config/mosquitto.conf"
chown 1883:1883 "$MOSQ_DIR/config/mosquitto.conf"

# Generate mosquitto password file (remove any stale copy first so -c can always create fresh)
rm -f "$MOSQ_DIR/config/passwd"
docker run --rm \
  -v "$MOSQ_DIR/config:/mosquitto/config" \
  eclipse-mosquitto:2 \
  sh -c "mosquitto_passwd -b -c /mosquitto/config/passwd tankmonitor 'Tank32!'"
chmod 640 "$MOSQ_DIR/config/passwd"

# Create isolated Docker network if it doesn't exist
docker network create $NETWORK 2>/dev/null || true

# Build web image (no cache to pick up any code changes)
docker build --no-cache -t agripulse-cloud:${VERSION} .

# Stop and remove existing containers
docker stop agripulse-mosquitto 2>/dev/null || true
docker rm   agripulse-mosquitto 2>/dev/null || true
docker stop agripulse-cloud 2>/dev/null || true
docker rm   agripulse-cloud 2>/dev/null || true

# Start Mosquitto broker
docker run -d \
  --name agripulse-mosquitto \
  --network $NETWORK \
  --restart always \
  -p 1883:1883 \
  -v "$MOSQ_DIR/config:/mosquitto/config:ro" \
  -v "$MOSQ_DIR/data:/mosquitto/data" \
  -v "$MOSQ_DIR/log:/mosquitto/log" \
  eclipse-mosquitto:2

# Detect this machine's LAN IP so OTA URLs are always correct regardless of network
LOCAL_IP=$(ip route get 8.8.8.8 2>/dev/null | grep -oP 'src \K\S+' || hostname -I | awk '{print $1}')
echo "==> OTA_BASE_URL will be http://${LOCAL_IP}:1880"

# Start web app — OTA_BASE_URL uses the auto-detected LAN IP of this OrangePi host
docker run -d \
  --name agripulse-cloud \
  --network $NETWORK \
  --restart always \
  -p 1880:8080 \
  -v "$DATA_DIR:/data" \
  -e MQTT_BROKER=agripulse-mosquitto \
  -e MQTT_PORT=1883 \
  -e MQTT_USER=tankmonitor \
  -e MQTT_PASS='Tank32!' \
  -e AUTH_USER=admin \
  -e AUTH_PASS='Tank32!' \
  -e AUTH_SECRET='1ee5cd0b3032e3d2d3613d23aa6b33d08890337cd7df504a9393dfa4f3e42a45' \
  -e OTA_BASE_URL="http://${LOCAL_IP}:1880" \
  agripulse-cloud:${VERSION}

echo "--- Mosquitto logs ---"
docker logs agripulse-mosquitto --tail 5
echo "--- Web app logs ---"
docker logs agripulse-cloud --tail 10

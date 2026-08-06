#!/usr/bin/env bash
# Deploy AgriPulse Cloud on the VM.
# Rebuilds the Docker image from the latest main and restarts the container,
# reusing the running container's env (secrets never leave the VM).
set -euo pipefail

cd /opt/AgriPulse/pulse-cloud

echo "--- git status (before) ---"
git status --porcelain || true

echo "--- git pull ---"
git pull --ff-only origin main

VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
echo "VERSION=${VERSION}"

echo "--- capture current container env (server-side only) ---"
docker inspect agripulse-cloud --format '{{range .Config.Env}}{{println .}}{{end}}' > /tmp/ap.env
chmod 600 /tmp/ap.env

echo "--- docker build (no-cache) ---"
docker build --no-cache -t "agripulse-cloud:${VERSION}" .

echo "--- swap container ---"
docker stop agripulse-cloud
docker rm agripulse-cloud
docker run -d --name agripulse-cloud --network agripulse --restart always \
  -p 1880:8080 -v /opt/agripulse/data:/data \
  --env-file /tmp/ap.env \
  "agripulse-cloud:${VERSION}"

rm -f /tmp/ap.env

echo "--- wait & verify ---"
sleep 4
docker ps --filter name=agripulse-cloud --format '{{.Names}} {{.Image}} {{.Status}}'
echo -n "api/version: "
curl -s http://localhost:1880/api/version || echo "(version endpoint not reachable yet)"
echo
echo "--- done ---"

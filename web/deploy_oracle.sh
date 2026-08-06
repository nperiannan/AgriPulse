#!/usr/bin/env bash
# Deploy the Tank Monitor web backend on the Oracle Cloud VM.
# Rebuilds the Docker image from the latest master and restarts the container,
# reusing the running container's env (secrets never leave the VM).
set -euo pipefail

cd /opt/TankMonitor/web

echo "--- git status (before) ---"
git status --porcelain || true

echo "--- git pull ---"
git pull --ff-only origin master

VERSION=$(sed -n 's/.*webVersion = "\([^"]*\)".*/\1/p' backend/main.go)
echo "VERSION=${VERSION}"

echo "--- capture current container env (server-side only) ---"
docker inspect tankmonitor-web --format '{{range .Config.Env}}{{println .}}{{end}}' > /tmp/tm.env
chmod 600 /tmp/tm.env

echo "--- docker build (no-cache) ---"
docker build --no-cache -t "tankmonitor-web:${VERSION}" .

echo "--- swap container ---"
docker stop tankmonitor-web
docker rm tankmonitor-web
docker run -d --name tankmonitor-web --network tankmonitor --restart always \
  -p 1880:8080 -v /opt/tankmonitor/data:/data \
  --env-file /tmp/tm.env \
  "tankmonitor-web:${VERSION}"

rm -f /tmp/tm.env

echo "--- wait & verify ---"
sleep 4
docker ps --filter name=tankmonitor-web --format '{{.Names}} {{.Image}} {{.Status}}'
echo -n "api/version: "
curl -s http://localhost:1880/api/version || echo "(version endpoint not reachable yet)"
echo
echo "--- done ---"

#!/usr/bin/env bash
# Deploy a GitHub-Actions-built web backend image to the Oracle Cloud VM.
#
# Unlike deploy_oracle.sh (which does a slow `docker build --no-cache` on the
# VM itself — npm install ~3.5m, vite build ~8.5m, go build ~15m), this pulls
# the already-built image from GitHub Container Registry (published by
# .github/workflows/build-web.yml on every `web/vX.Y.Z` tag push), so the VM
# only has to `docker pull` + swap the container — seconds, not ~20 minutes.
#
# Usage: bash deploy_oracle_ghcr.sh <version>   e.g. bash deploy_oracle_ghcr.sh 2.4.0
#
# One-time prerequisite: the ghcr.io/<owner>/agripulse-cloud package must be
# reachable from the VM. Simplest: make the package Public (GitHub → your
# profile → Packages → agripulse-cloud → Package settings → Change visibility).
# If it must stay private, `docker login ghcr.io -u <user>` on the VM first
# with a PAT that has `read:packages` scope.
set -euo pipefail

VERSION="${1:?Usage: bash deploy_oracle_ghcr.sh <version>}"
OWNER="nperiannan"
IMAGE="ghcr.io/${OWNER}/agripulse-cloud:${VERSION}"

echo "--- pull ${IMAGE} ---"
docker pull "${IMAGE}"

echo "--- capture current container env (server-side only) ---"
docker inspect agripulse-cloud --format '{{range .Config.Env}}{{println .}}{{end}}' > /tmp/ap.env
chmod 600 /tmp/ap.env

echo "--- swap container ---"
docker stop agripulse-cloud
docker rm agripulse-cloud
docker run -d --name agripulse-cloud --network agripulse --restart always \
  -p 1880:8080 -v /opt/agripulse/data:/data \
  --env-file /tmp/ap.env \
  "${IMAGE}"

rm -f /tmp/ap.env

echo "--- wait & verify ---"
sleep 4
docker ps --filter name=agripulse-cloud --format '{{.Names}} {{.Image}} {{.Status}}'
echo -n "api/version: "
curl -s http://localhost:1880/api/version || echo "(version endpoint not reachable yet)"
echo
echo "--- done ---"

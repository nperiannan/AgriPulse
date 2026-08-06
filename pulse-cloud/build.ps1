# build.ps1 — Build & deploy the Pulse Cloud Docker image to the VM
# Usage: .\build.ps1 [-Version <tag>] [-VmHost <host>] [-VmUser <user>]
param(
    [string]$Version = "2.2.3",
    [string]$VmHost = "150.230.129.215",
    [string]$VmUser = "hainatraj",
    [string]$VmRepoPath = "/opt/AgriPulse",
    [string]$SshKey = "$env:USERPROFILE\.ssh\Oracle VMs\rocky\ssh-key-2026-06-06.key",
    [string]$AuthSecret = "1ee5cd0b3032e3d2d3613d23aa6b33d08890337cd7df504a9393dfa4f3e42a45"
)

$ErrorActionPreference = 'Stop'
$ImageTag = "agripulse-cloud:$Version"

Write-Host "==> Pushing latest code to GitHub..." -ForegroundColor Cyan
Push-Location "$PSScriptRoot\.."
git push origin HEAD
Pop-Location

Write-Host ""
Write-Host "==> Deploying $ImageTag to VM ($VmUser@$VmHost)..." -ForegroundColor Cyan
Write-Host ""

$remote = @"
cd $VmRepoPath
git pull origin \$(git rev-parse --abbrev-ref HEAD)
docker build -t $ImageTag pulse-cloud/ && \
docker stop agripulse-cloud 2>/dev/null || true
docker rm   agripulse-cloud 2>/dev/null || true
docker run -d --name agripulse-cloud --restart always \
  --network agripulse \
  -p 1880:8080 \
  -v /opt/agripulse/data:/data \
  -e MQTT_BROKER=agripulse-mosquitto \
  -e MQTT_PORT=1883 \
  -e MQTT_USER=tankmonitor \
  -e MQTT_PASS='Tank32!' \
  -e AUTH_USER=admin \
  -e AUTH_PASS='Tank32!' \
  -e AUTH_SECRET='$AuthSecret' \
  -e OTA_BASE_URL=http://150.230.129.215:1880 \
  $ImageTag
docker logs agripulse-cloud --tail 5
"@

ssh -i "$SshKey" "$VmUser@$VmHost" $remote

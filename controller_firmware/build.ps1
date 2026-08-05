# build_controller.ps1 — Build controller firmware and deploy OTA via Oracle Cloud VM
# Usage:
#   .\build_controller.ps1                     # build only
#   .\build_controller.ps1 -Upload             # build + SCP firmware.bin to VM + trigger OTA
#   .\build_controller.ps1 -Upload -Mac AA:BB:CC:DD:EE:FF  # specify device MAC
#   .\build_controller.ps1 -DirectOta          # build + upload directly via espota (must be on LAN)
param(
    [switch]$Upload,     # Upload firmware.bin to VM via SCP and trigger OTA
    [switch]$DirectOta,  # Upload directly via espota to tankmonitor.local (LAN only)
    [string]$Mac  = '',  # Device MAC address (required for -Upload)
    [switch]$Monitor     # Open serial monitor after direct OTA
)

$ErrorActionPreference = 'Stop'
$FwDir    = "$PSScriptRoot"
$pio      = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) { $pio = 'pio' }  # fallback if in PATH

# Oracle Cloud VM config
$VmUser   = 'hainatraj'
$VmHost   = '150.230.129.215'
$SshKey   = "$env:USERPROFILE\.ssh\Oracle VMs\rocky\ssh-key-2026-06-06.key"
$OtaDir   = '/opt/tankmonitor/data/ota'  # Docker volume mount → /data/ota inside container
$ServerUrl = 'http://nperiannan-nas.freemyip.com:1880'

Write-Host "==> Building controller firmware (env: nebulas3)..." -ForegroundColor Cyan
Push-Location $FwDir
try {
    if ($DirectOta) {
        Write-Host "==> Building + uploading directly via espota to tankmonitor.local..." -ForegroundColor Cyan
        & $pio run --target upload --environment nebulas3
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO espota upload failed" }
        Write-Host "==> Direct OTA upload complete." -ForegroundColor Green
        if ($Monitor) {
            Write-Host "==> Opening serial monitor..." -ForegroundColor Cyan
            & $pio device monitor
        }
    } elseif ($Upload) {
        # Step 1: Build
        & $pio run --environment nebulas3
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed" }

        $fwPath = ".pio\build\nebulas3\firmware.bin"
        $fw = Get-Item $fwPath -ErrorAction Stop
        Write-Host "==> Built: $($fw.FullName)  ($([math]::Round($fw.Length/1KB, 1)) KB)" -ForegroundColor Green

        # Step 2: Resolve MAC — ask if not supplied
        if ($Mac -eq '') {
            $Mac = Read-Host "Enter device MAC address (e.g. AA:BB:CC:DD:EE:FF)"
        }
        $Mac = $Mac.Trim().ToUpper()

        # Step 3: Upload firmware.bin — try SCP first, fall back to HTTP API
        $remoteFile = "$OtaDir/$Mac.bin"
        $uploaded = $false

        if (Test-Path "$SshKey") {
            Write-Host "==> Uploading firmware.bin via SCP to VM ($VmHost`:$remoteFile)..." -ForegroundColor Cyan
            scp -i "$SshKey" "$fwPath" "${VmUser}@${VmHost}:${remoteFile}" 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "==> Firmware uploaded via SCP." -ForegroundColor Green
                $uploaded = $true
            } else {
                Write-Host "    SCP failed — falling back to HTTP API upload." -ForegroundColor Yellow
            }
        } else {
            Write-Host "    SSH key not found — using HTTP API upload." -ForegroundColor Yellow
        }

        # Get auth token (needed for HTTP upload fallback or OTA trigger)
        $ApiToken = $env:TANKMONITOR_TOKEN
        if (-not $ApiToken) {
            $cred = Get-Credential -Message "TankMonitor server login" -UserName "admin"
            $loginBody = @{ username = $cred.UserName; password = $cred.GetNetworkCredential().Password } | ConvertTo-Json
            $loginRes = Invoke-RestMethod -Uri "$ServerUrl/api/login" -Method Post -Body $loginBody -ContentType 'application/json'
            $ApiToken = $loginRes.token
        }
        $headers = @{ Authorization = "Bearer $ApiToken" }

        # Fallback: upload via HTTP API if SCP was unavailable/failed
        if (-not $uploaded) {
            Write-Host "==> Uploading firmware.bin via HTTP API ($ServerUrl)..." -ForegroundColor Cyan
            $form = @{ firmware = Get-Item $fwPath }
            Invoke-RestMethod -Uri "$ServerUrl/api/devices/$Mac/ota/upload" `
                -Method Post -Headers $headers -Form $form | Out-Null
            Write-Host "==> Firmware uploaded via HTTP API." -ForegroundColor Green
        }

        # Step 4: Trigger OTA on device
        Write-Host "==> Triggering OTA on device $Mac..." -ForegroundColor Cyan
        $triggerBody = @{ mac = $Mac } | ConvertTo-Json
        $result = Invoke-RestMethod -Uri "$ServerUrl/api/devices/$Mac/ota/trigger" `
            -Method Post -Headers $headers -Body $triggerBody -ContentType 'application/json'
        Write-Host "==> OTA triggered. Device will reboot after flashing." -ForegroundColor Green
        Write-Host "    Response: $($result | ConvertTo-Json -Compress)" -ForegroundColor DarkGray
    } else {
        # Build only
        & $pio run --environment nebulas3
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed" }

        $fw = Get-ChildItem ".pio\build\nebulas3\firmware.bin" -ErrorAction SilentlyContinue
        if ($fw) {
            Write-Host "==> Built: $($fw.FullName)  ($([math]::Round($fw.Length/1KB, 1)) KB)" -ForegroundColor Green
            Write-Host "    To deploy: .\build_controller.ps1 -Upload -Mac AA:BB:CC:DD:EE:FF" -ForegroundColor DarkGray
        } else {
            Write-Host "==> Build complete." -ForegroundColor Green
        }
    }
} finally {
    Pop-Location
}

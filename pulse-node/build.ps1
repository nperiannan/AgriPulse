# build_transmitter.ps1 — Build & upload transmitter firmware (ATmega328P via UART/Serial)
# Usage: .\build_transmitter.ps1 [-Upload] [-Env <env>] [-Port <COMx>]
param(
    [switch]$Upload,                    # Upload after build
    [string]$Env    = "upload_uart",    # PlatformIO env: upload_uart | upload_serial
    [string]$Port   = ""               # Serial port e.g. COM3 (leave empty to auto-detect)
)

$ErrorActionPreference = 'Stop'
$FwDir = "$PSScriptRoot"
$pio  = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) { $pio = 'pio' }  # fallback if in PATH

Write-Host "==> Building transmitter firmware (env: $Env)..." -ForegroundColor Cyan
Push-Location $FwDir
try {
    if ($Upload) {
        $uploadArgs = @("run", "--target", "upload", "--environment", $Env)
        if ($Port -ne "") { $uploadArgs += @("--upload-port", $Port) }

        Write-Host "==> Building + uploading..." -ForegroundColor Cyan
        & $pio @uploadArgs
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO upload failed" }
        Write-Host "==> Upload complete." -ForegroundColor Green
    } else {
        & $pio run --environment $Env
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed" }

        $fw = Get-ChildItem ".pio\build\$Env\firmware.hex" -ErrorAction SilentlyContinue
        if ($fw) {
            Write-Host "==> Built: $($fw.FullName)  ($([math]::Round($fw.Length/1KB, 1)) KB)" -ForegroundColor Green
        } else {
            Write-Host "==> Build complete." -ForegroundColor Green
        }
    }
} finally {
    Pop-Location
}

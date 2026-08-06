# build_MobileApp.ps1 — Build Flutter mobile app APK (release)
# Usage: .\build_app.ps1 [-Install]
param(
    [switch]$Install  # Pass -Install to also install on connected USB device
)

$ErrorActionPreference = 'Stop'
$AppDir = "$PSScriptRoot"

Write-Host "==> Building Flutter app..." -ForegroundColor Cyan
Push-Location $AppDir
try {
    flutter build apk --release
    if ($LASTEXITCODE -ne 0) { throw "Flutter build failed" }

    $apk = "$AppDir\build\app\outputs\flutter-apk\app-release.apk"
    Write-Host "==> Built: $apk" -ForegroundColor Green

    if ($Install) {
        Write-Host "==> Installing on connected USB device..." -ForegroundColor Cyan
        adb install -r $apk
        if ($LASTEXITCODE -ne 0) { throw "ADB install failed" }
        Write-Host "==> Installed successfully." -ForegroundColor Green
    }
} finally {
    Pop-Location
}

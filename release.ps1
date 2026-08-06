# release.ps1 — Create a GitHub release for a TankMonitor component
# Enforces: one release per component, annotated tags, correct assets, proper titles.
#
# Usage:
#   .\release.ps1 -Component web -Version 2.1.0 -Notes "Fixed X, Added Y"
#   .\release.ps1 -Component controller_firmware -Version 2.1.0 -Asset .\build\firmware.bin -Notes "Fixed Z"
#   .\release.ps1 -Component MobileApp -Version 2.0.1 -Asset .\build\app-release.apk -Notes "Bug fix"
#   .\release.ps1 -Component transmitter_firmware -Version 2.0.1 -Asset .\build\firmware.hex -Notes "Calibration fix"
param(
    [Parameter(Mandatory)]
    [ValidateSet("controller_firmware", "transmitter_firmware", "web", "MobileApp")]
    [string]$Component,

    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory)]
    [string]$Notes,

    [string]$Asset
)

$ErrorActionPreference = 'Stop'

# ── Component metadata ──────────────────────────────────────────────
$meta = @{
    controller_firmware  = @{ Title = "Controller Firmware";  RequiredAsset = "firmware.bin" }
    transmitter_firmware = @{ Title = "Transmitter Firmware"; RequiredAsset = "firmware.hex" }
    web                  = @{ Title = "Web App";              RequiredAsset = $null }
    MobileApp            = @{ Title = "Mobile App";           RequiredAsset = "app-release.apk" }
}

$info          = $meta[$Component]
$TagName       = "$Component/v$Version"
$ReleaseTitle  = "$($info.Title) v$Version"
$RequiredAsset = $info.RequiredAsset

# ── Validate asset ──────────────────────────────────────────────────
if ($RequiredAsset) {
    if (-not $Asset) {
        Write-Host "ERROR: $Component releases require a '$RequiredAsset' asset." -ForegroundColor Red
        Write-Host "       Use: -Asset <path-to-$RequiredAsset>" -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path $Asset)) {
        Write-Host "ERROR: Asset file not found: $Asset" -ForegroundColor Red
        exit 1
    }
    $assetName = Split-Path $Asset -Leaf
    if ($assetName -ne $RequiredAsset) {
        Write-Host "WARNING: Expected asset named '$RequiredAsset', got '$assetName'." -ForegroundColor Yellow
        $confirm = Read-Host "Continue anyway? (y/N)"
        if ($confirm -ne 'y') { exit 1 }
    }
} elseif ($Asset) {
    Write-Host "NOTE: $Component releases don't require an asset, but one was provided. It will be attached." -ForegroundColor Yellow
}

# ── Format release notes ────────────────────────────────────────────
$FormattedNotes = "## What's new`n`n"
$Notes -split ';' | ForEach-Object {
    $bullet = $_.Trim()
    if ($bullet) { $FormattedNotes += "- $bullet`n" }
}

# ── Show summary and confirm ────────────────────────────────────────
Write-Host ""
Write-Host "┌─────────────────────────────────────────────" -ForegroundColor Cyan
Write-Host "│ Component : $Component"                       -ForegroundColor Cyan
Write-Host "│ Tag       : $TagName (annotated)"             -ForegroundColor Cyan
Write-Host "│ Title     : $ReleaseTitle"                    -ForegroundColor Cyan
if ($Asset) {
Write-Host "│ Asset     : $Asset"                           -ForegroundColor Cyan
}
Write-Host "│ Notes     :"                                  -ForegroundColor Cyan
$FormattedNotes -split "`n" | ForEach-Object {
Write-Host "│   $_"                                         -ForegroundColor Cyan
}
Write-Host "└─────────────────────────────────────────────" -ForegroundColor Cyan
Write-Host ""

# ── Find and delete previous release for this component ─────────────
Write-Host "==> Checking for existing $Component release..." -ForegroundColor Yellow
$existing = gh release list --json tagName,name --jq ".[].tagName" 2>$null |
    Where-Object { $_ -like "$Component/*" }

if ($existing) {
    Write-Host "    Deleting old release: $existing" -ForegroundColor Yellow
    gh release delete $existing --cleanup-tag --yes
    # Also delete local tag if it exists
    git tag -d $existing 2>$null | Out-Null
    Write-Host "    Done." -ForegroundColor Green
} else {
    Write-Host "    No existing release found." -ForegroundColor Green
}

# ── Create annotated tag ────────────────────────────────────────────
Write-Host "==> Creating annotated tag $TagName ..." -ForegroundColor Yellow
git tag -a $TagName -m "$ReleaseTitle"
git push origin $TagName
Write-Host "    Tag pushed." -ForegroundColor Green

# ── Create GitHub release ───────────────────────────────────────────
Write-Host "==> Creating GitHub release '$ReleaseTitle' ..." -ForegroundColor Yellow
$ghArgs = @("release", "create", $TagName, "--title", $ReleaseTitle, "--notes", $FormattedNotes)
if ($Asset) {
    $ghArgs += $Asset
}
& gh @ghArgs

Write-Host ""
Write-Host "==> Release created successfully!" -ForegroundColor Green
Write-Host "    https://github.com/nperiannan/TankMonitor/releases/tag/$TagName" -ForegroundColor Green

# ── Verify: exactly one release per component ───────────────────────
Write-Host ""
Write-Host "==> Current releases:" -ForegroundColor Yellow
gh release list

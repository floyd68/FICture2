# Sync canonical IniStore.h (FICture2) -> NIFDiff/app/IniStore.h
# Usage (from either repo, or with explicit paths):
#   .\scripts\sync-ini-store.ps1
#   .\scripts\sync-ini-store.ps1 -CheckOnly

param(
    [switch]$CheckOnly,
    [string]$Source = "",
    [string]$Dest = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ficRoot = Split-Path -Parent $scriptDir

if (-not $Source) {
    $Source = Join-Path $ficRoot "IniStore.h"
}
if (-not $Dest) {
    $Dest = Join-Path (Split-Path -Parent $ficRoot) "NifDiff\app\IniStore.h"
}

if (-not (Test-Path -LiteralPath $Source)) {
    throw "Canonical IniStore.h not found: $Source"
}

$srcHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
$destExists = Test-Path -LiteralPath $Dest

if ($destExists) {
    $dstHash = (Get-FileHash -LiteralPath $Dest -Algorithm SHA256).Hash
    if ($srcHash -eq $dstHash) {
        Write-Host "IniStore.h already in sync."
        exit 0
    }
}

if ($CheckOnly) {
    if (-not $destExists) {
        Write-Host "MISSING: $Dest"
        exit 1
    }
    Write-Host "DRIFT: $Source != $Dest"
    exit 1
}

$destDir = Split-Path -Parent $Dest
if (-not (Test-Path -LiteralPath $destDir)) {
    throw "Destination directory missing: $destDir"
}

Copy-Item -LiteralPath $Source -Destination $Dest -Force
Write-Host "Synced IniStore.h -> $Dest"

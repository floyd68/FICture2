<#
.SYNOPSIS
    Registers FICture2 debug build file associations (HKCU, no admin required).

.DESCRIPTION
    Replicates the logic of RegisterPerUserFileAssociations() in AppSetup.cpp.
    All writes go to HKEY_CURRENT_USER, so no administrator privileges are needed.
    Run this script after building the debug configuration to test file-open
    associations without going through the installer.

.PARAMETER ExePath
    Full path to FICture2.exe. Defaults to the standard CMake Debug output:
    <repo_root>\build_nexus\bin\Debug\FICture2.exe

.PARAMETER Unregister
    When specified, removes all per-user registrations written by this script.

.EXAMPLE
    # Register using the default debug build path:
    .\scripts\register_debug.ps1

    # Register a custom path:
    .\scripts\register_debug.ps1 -ExePath "D:\MyBuild\FICture2.exe"

    # Remove all per-user associations registered by this script:
    .\scripts\register_debug.ps1 -Unregister
#>

param(
    [string]$ExePath   = "",
    [switch]$Unregister
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Resolve ExePath
# ---------------------------------------------------------------------------
if (-not $ExePath)
{
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $ExePath  = Join-Path $repoRoot "build_nexus\bin\Debug\FICture2.exe"
}

if (-not $Unregister -and -not (Test-Path $ExePath))
{
    Write-Error "FICture2.exe not found at: $ExePath`nBuild the Debug configuration first, or pass -ExePath."
    exit 1
}

# ---------------------------------------------------------------------------
# Constants — must match ImageDecodeDispatcher.cpp SupportedExtensions()
# ---------------------------------------------------------------------------
$Extensions = @(
    # WIC decoder
    ".jpg", ".jpeg", ".jfif",
    ".png",
    ".bmp",
    ".gif",
    ".tif", ".tiff",
    ".ico",
    ".heif", ".heic",
    ".webp",
    # DirectXTex decoder
    ".dds",
    ".hdr",
    ".tga",
    ".exr"
)

$AppName   = "FICture2"
$ProgId    = "FICture2.Image"
$CapKey    = "HKCU:\Software\$AppName\Capabilities"
$RegApps   = "HKCU:\Software\RegisteredApplications"

# ---------------------------------------------------------------------------
# Unregister
# ---------------------------------------------------------------------------
if ($Unregister)
{
    Write-Host "Removing per-user FICture2 file associations..."

    # ProgID
    Remove-Item -Path "HKCU:\Software\Classes\$ProgId" -Recurse -ErrorAction SilentlyContinue

    # Capabilities
    Remove-Item -Path "HKCU:\Software\$AppName"        -Recurse -ErrorAction SilentlyContinue

    # RegisteredApplications value
    if (Test-Path $RegApps)
    {
        Remove-ItemProperty -Path $RegApps -Name $AppName -ErrorAction SilentlyContinue
    }

    # Per-extension OpenWithProgids
    foreach ($ext in $Extensions)
    {
        $owpKey = "HKCU:\Software\Classes\$ext\OpenWithProgids"
        if (Test-Path $owpKey)
        {
            Remove-ItemProperty -Path $owpKey -Name $ProgId -ErrorAction SilentlyContinue
        }
        # Standalone-style direct mapping (remove only if it points to us)
        $extKey = "HKCU:\Software\Classes\$ext"
        if (Test-Path $extKey)
        {
            $cur = (Get-ItemProperty -Path $extKey -ErrorAction SilentlyContinue).'(default)'
            if ($cur -eq $ProgId)
            {
                Remove-ItemProperty -Path $extKey -Name '(default)' -ErrorAction SilentlyContinue
            }
        }
    }

    # Applications\FICture2.exe
    $exeName   = [System.IO.Path]::GetFileName($ExePath)
    $appsKey   = "HKCU:\Software\Classes\Applications\$exeName"
    Remove-Item -Path $appsKey -Recurse -ErrorAction SilentlyContinue

    # Notify Explorer
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Shell32 {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, int flags, IntPtr item1, IntPtr item2);
}
"@ -ErrorAction SilentlyContinue
    [Shell32]::SHChangeNotify(0x08000000, 0x00000000, [IntPtr]::Zero, [IntPtr]::Zero)

    Write-Host "Done. Per-user FICture2 associations removed."
    exit 0
}

# ---------------------------------------------------------------------------
# Register
# ---------------------------------------------------------------------------
$exeName = [System.IO.Path]::GetFileName($ExePath)
$cmd     = "`"$ExePath`" `"%1`""
$icon    = "`"$ExePath`",0"

Write-Host "Registering FICture2 debug build..."
Write-Host "  Executable : $ExePath"
Write-Host "  Extensions : $($Extensions -join ' ')"

function Ensure-Key([string]$path)
{
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
}

function Set-Default([string]$path, [string]$value)
{
    Ensure-Key $path
    Set-ItemProperty -Path $path -Name '(default)' -Value $value
}

# ProgID
Set-Default "HKCU:\Software\Classes\$ProgId"                                    "FICture2 Image"
Set-Default "HKCU:\Software\Classes\$ProgId\DefaultIcon"                        $icon
Set-Default "HKCU:\Software\Classes\$ProgId\shell\open\command"                 $cmd

# Capabilities (Windows Default Apps integration)
Ensure-Key $CapKey
Set-ItemProperty -Path $CapKey -Name "ApplicationName"        -Value $AppName
Set-ItemProperty -Path $CapKey -Name "ApplicationDescription" -Value "FICture2 Image Viewer"
Ensure-Key $RegApps
Set-ItemProperty -Path $RegApps -Name $AppName -Value "Software\$AppName\Capabilities"

foreach ($ext in $Extensions)
{
    Ensure-Key "$CapKey\FileAssociations"
    Set-ItemProperty -Path "$CapKey\FileAssociations" -Name $ext -Value $ProgId
}

# Applications\<exeName> (OpenWith and shell metadata)
$appsBase = "HKCU:\Software\Classes\Applications\$exeName"
Set-Default "$appsBase\shell\open\command" $cmd
Set-Default "$appsBase\DefaultIcon"        $icon
Ensure-Key  $appsBase
Set-ItemProperty -Path $appsBase -Name "FriendlyAppName" -Value $AppName

foreach ($ext in $Extensions)
{
    Ensure-Key "$appsBase\SupportedTypes\$ext"
    Ensure-Key "HKCU:\Software\Classes\$ext\OpenWithProgids"
    Set-ItemProperty -Path "HKCU:\Software\Classes\$ext\OpenWithProgids" -Name $ProgId -Value ""
    # Direct default mapping (Standalone-style; ignored by Windows when UserChoice policy is active)
    Set-Default "HKCU:\Software\Classes\$ext" $ProgId
}

# Notify Explorer
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Shell32Reg {
    [DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, int flags, IntPtr item1, IntPtr item2);
}
"@ -ErrorAction SilentlyContinue
[Shell32Reg]::SHChangeNotify(0x08000000, 0x00000000, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ""
Write-Host "Done. FICture2 debug associations registered."
Write-Host "If extensions are still handled by another app, open:"
Write-Host "  Settings > Apps > Default apps > FICture2"
Write-Host "and set it as default for each file type."

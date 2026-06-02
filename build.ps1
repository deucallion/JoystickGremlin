<#
.SYNOPSIS
    Builds the Mumble Voice Overlay plugin and copies the DLL to your Desktop.

.DESCRIPTION
    One-shot build for Windows. Configures and builds plugin/ with CMake +
    MSVC (Release, x64), then copies the resulting mumble_voice_overlay.dll to
    your Desktop so it's easy to find when installing it in Mumble
    (Settings -> Plugins -> Install plugin...).

    Prerequisites (one-time, see README/docs/INSTALL.md):
        winget install -e --id Kitware.CMake
        winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
          --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

.PARAMETER Config
    CMake build configuration. Defaults to Release.

.PARAMETER Destination
    Where to copy the built DLL. Defaults to your Desktop.

.EXAMPLE
    .\build.ps1

.EXAMPLE
    .\build.ps1 -Destination C:\Mumble\plugins
#>
[CmdletBinding()]
param(
    [string] $Config = "Release",
    [string] $Destination = [Environment]::GetFolderPath("Desktop")
)

# Stop on the first real error so failures are obvious instead of cascading.
$ErrorActionPreference = "Stop"

# Always operate relative to this script's location, regardless of where it's run from.
$root      = Split-Path -Parent $MyInvocation.MyCommand.Path
$pluginDir = Join-Path $root "plugin"
$buildDir  = Join-Path $pluginDir "build"

Write-Host "Mumble Voice Overlay - plugin build" -ForegroundColor Cyan
Write-Host "  source : $pluginDir"
Write-Host "  config : $Config"
Write-Host ""

# --- Prerequisite check ---------------------------------------------------
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error @"
CMake was not found on your PATH.
Install it (then reopen PowerShell) with:
    winget install -e --id Kitware.CMake
"@
    exit 1
}

# --- Configure ------------------------------------------------------------
Write-Host "[1/3] Configuring..." -ForegroundColor Yellow
cmake -S $pluginDir -B $buildDir -A x64
if ($LASTEXITCODE -ne 0) {
    Write-Error @"
CMake configuration failed. This usually means the Visual Studio C++ compiler
is missing. Install it (then reopen PowerShell) with:
    winget install -e --id Microsoft.VisualStudio.2022.BuildTools ``
      --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
or add "Desktop development with C++" via the Visual Studio Installer.
"@
    exit 1
}

# --- Build ----------------------------------------------------------------
Write-Host "[2/3] Building ($Config)..." -ForegroundColor Yellow
cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed. See the compiler output above."
    exit 1
}

# --- Locate + copy the DLL ------------------------------------------------
Write-Host "[3/3] Locating output..." -ForegroundColor Yellow
$dll = Get-ChildItem -Path $buildDir -Recurse -Filter "mumble_voice_overlay.dll" |
    Select-Object -First 1
if (-not $dll) {
    Write-Error "Build reported success but mumble_voice_overlay.dll was not found under $buildDir."
    exit 1
}

if (-not (Test-Path $Destination)) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
}
$target = Join-Path $Destination "mumble_voice_overlay.dll"
Copy-Item -Path $dll.FullName -Destination $target -Force

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  built : $($dll.FullName)"
Write-Host "  copied: $target" -ForegroundColor Green
Write-Host ""
Write-Host "Next: Mumble -> Settings -> Plugins -> Install plugin... and pick the copied DLL."
Write-Host "      (Leave Mumble's own 'Overlay' feature disabled.)"

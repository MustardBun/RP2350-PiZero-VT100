# Build convenience wrapper for the RP2350 PiZero VT100 terminal.
#
# Usage:
#   .\build.ps1                       configure + build with defaults
#   .\build.ps1 -Clean                delete the build directory first
#   .\build.ps1 -PicoSdkPath C:\sdk   point at a specific SDK checkout
#   .\build.ps1 -ConfigureOnly        configure without building
#
# PICO_SDK_PATH is taken from the parameter if given, otherwise from the
# environment. Nothing machine-specific is hardcoded.

[CmdletBinding()]
param(
    [string]$PicoSdkPath = $env:PICO_SDK_PATH,
    [string]$BuildDir    = "build",
    [string]$Generator   = "Ninja",
    [switch]$Clean,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root $BuildDir

# Host compiler settings inherited by the SDK's nested picotool build, which is
# configured with a fresh CMake project and would otherwise not know which host
# toolchain to use.
$savedCC = $env:CC
$savedCXX = $env:CXX
$savedPath = $env:PATH
$savedCmakeArgs = $env:CMAKE_ARGS

function Find-Tool {
    param([string]$Name, [string]$Override)
    if ($Override) { return $Override }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Restore-Env {
    $env:CC = $savedCC
    $env:CXX = $savedCXX
    $env:PATH = $savedPath
    $env:CMAKE_ARGS = $savedCmakeArgs
}

if (-not $PicoSdkPath) {
    Write-Host ""
    Write-Host "PICO_SDK_PATH is not set." -ForegroundColor Red
    Write-Host "Install the Raspberry Pi Pico SDK (2.x) and either set the"
    Write-Host "environment variable, or pass it explicitly:"
    Write-Host ""
    Write-Host "    .\build.ps1 -PicoSdkPath C:\path\to\pico-sdk"
    Write-Host ""
    exit 1
}

if (-not (Test-Path (Join-Path $PicoSdkPath "pico_sdk_init.cmake"))) {
    Write-Host "PICO_SDK_PATH '$PicoSdkPath' does not look like a Pico SDK checkout." -ForegroundColor Red
    exit 1
}

$cmake = Find-Tool -Name "cmake"
if (-not $cmake) {
    Write-Host "cmake was not found on PATH. Install CMake and re-run." -ForegroundColor Red
    exit 1
}

$genArgs = @()
if ($Generator -eq "Ninja") {
    $ninja = Find-Tool -Name "ninja"
    if (-not $ninja) {
        Write-Host "ninja was not found on PATH. Install Ninja, or use -Generator 'Unix Makefiles'." -ForegroundColor Red
        exit 1
    }
    $genArgs += @("-DCMAKE_MAKE_PROGRAM=$ninja")
}

# The SDK builds picotool from source when no installed copy is found, and that
# nested project needs a host compiler. Prefer an MSVC developer environment if
# the caller has already set one up; otherwise fall back to whatever cl.exe,
# gcc or clang is on PATH, and pass it to CMake explicitly.
if (-not $env:CC) {
    $hostCC = Get-Command cl.exe, gcc.exe, clang.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hostCC) {
        $env:CC = $hostCC.Source
        $env:CXX = if ($hostCC.Name -eq 'cl.exe') { 'cl.exe' } else { $hostCC.Source }
        Write-Host "Using host compiler: $($hostCC.Source)"
    } else {
        Write-Host "No host C compiler found on PATH." -ForegroundColor Yellow
        Write-Host "The SDK builds picotool from source and needs one. Either run this" -ForegroundColor Yellow
        Write-Host "from a Visual Studio Developer prompt, or install the SDK's picotool" -ForegroundColor Yellow
        Write-Host "separately and set PICOTOOL_FETCH_FROM_GIT_PATH." -ForegroundColor Yellow
    }
}

if ($Clean -and (Test-Path $build)) {
    Write-Host "Removing $build"
    Remove-Item $build -Recurse -Force
}

Write-Host "Configuring with $Generator ..." -ForegroundColor Cyan
& $cmake -S $root -B $build -G $Generator @genArgs -DPICO_SDK_PATH="$PicoSdkPath"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($ConfigureOnly) { exit 0 }

Write-Host "Building ..." -ForegroundColor Cyan
& $cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$uf2 = Join-Path $build "vt100_terminal.uf2"
if (Test-Path $uf2) {
    Write-Host ""
    Write-Host "Built $uf2" -ForegroundColor Green
    Write-Host "Hold BOOTSEL, plug the board in, then copy the UF2 to the RPI-RP2 drive."
} else {
    Write-Host "Build finished but vt100_terminal.uf2 was not produced." -ForegroundColor Yellow
}

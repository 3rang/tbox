# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel
#
# scripts/clean-configure.ps1 - wipe build/ and do a clean configure + build
# with MSVC (NMake), then run the ctest gate. Run from repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\clean-configure.ps1 `
#       -TG_API_ID <id> -TG_API_HASH <hash>
# The credential flags are optional; without them qr-login.exe prints a
# usage hint at startup instead of authenticating.
#
# -NoTests configures with -DTBOX_BUILD_TESTS=OFF (builds only tbox.exe +
# qr-login.exe, skips the ctest gate) - handy while iterating on the CLI.
#
# Produces (single-config NMake -> directly in build/):
#   tbox.exe  qr-login.exe (demo)  tests\test_*.exe
# Any failure (configure / build / ctest) throws and exits non-zero.

param(
    [string]$TG_API_ID = "",
    [string]$TG_API_HASH = "",
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

if (Test-Path -LiteralPath $build) {
    Remove-Item -LiteralPath $build -Recurse -Force
    Write-Host "removed stale build/: $build"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found - install Visual Studio Build Tools."
}
$vsroot = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
if (-not $vsroot) {
    throw "no Visual Studio installation found (vswhere returned nothing)."
}
$vcvars = Join-Path $vsroot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

$gen = "NMake Makefiles"
$cfg = "Release"

$creds = ""
if ($TG_API_ID -ne "" -and $TG_API_HASH -ne "") {
    $creds = " -DTG_API_ID=$TG_API_ID -DTG_API_HASH=$TG_API_HASH"
}
$testsFlag = ""
if ($NoTests) {
    $testsFlag = " -DTBOX_BUILD_TESTS=OFF"
}

& cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$build`" -G `"$gen`" -DCMAKE_BUILD_TYPE=`"$cfg`"$creds$testsFlag"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
Write-Host "configure ok"

& cmd /c "`"$vcvars`" >nul 2>&1 && cmake --build `"$build`" --config `"$cfg`""
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
Write-Host "build ok"

if ($NoTests) {
    Write-Host "tests disabled (-NoTests) - skipping ctest gate"
} else {
    Write-Host "running test gate (ctest)..."
    & cmd /c "`"$vcvars`" >nul 2>&1 && ctest --test-dir `"$build`" -C `"$cfg`" --output-on-failure"
    if ($LASTEXITCODE -ne 0) { throw "ctest failed ($LASTEXITCODE)" }
}

Write-Host ""
Write-Host "built binaries:"
Get-ChildItem -LiteralPath $build -Filter "*.exe" -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName, Length
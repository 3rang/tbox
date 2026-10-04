# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel
#
# scripts/clean-configure.ps1 - wipe build/ and do a clean configure + build
# with MSVC (NMake), then run the ctest gate. Run from repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\clean-configure.ps1
#
# Telegram credentials are NOT part of the build. To use `tbox auth` or
# qr-login.exe, export them before running:
#   set TG_API_ID=<id> && set TG_API_HASH=<hash>
#
# -NoTests configures with -DTBOX_BUILD_TESTS=OFF (builds only tbox.exe +
# qr-login.exe, skips the ctest gate) - handy while iterating on the CLI.
#
# -CMakeArgs passes extra flags straight to the configure step, e.g.
#   -CMakeArgs "-DTBOX_TDJSON_LIBRARY=OFF"   # build the core without TDLib
#   -CMakeArgs "-DTBOX_TDJSON_LIBRARY=C:\tdjson.dll"
#
# Produces (single-config NMake -> directly in build/):
#   tbox.exe  qr-login.exe (demo)  tests\test_*.exe
# Any failure (configure / build / ctest) throws and exits non-zero.

param(
    [switch]$NoTests,
    [string[]]$CMakeArgs = @()
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

$testsFlag = ""
if ($NoTests) {
    $testsFlag = " -DTBOX_BUILD_TESTS=OFF"
}

$extraFlag = ""
if ($CMakeArgs.Count -gt 0) {
    $extraFlag = " " + ($CMakeArgs -join " ")
}

& cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$build`" -G `"$gen`" -DCMAKE_BUILD_TYPE=`"$cfg`"$testsFlag$extraFlag"
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
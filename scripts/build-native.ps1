# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# scripts/build-native.ps1 - clean configure + build + test of the C core.
# Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
#
# Uses MSVC (NMake) through vcvars64, builds into build/ (gitignored),
# and runs the ctest gate. Any failure throws with a non-zero exit.

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

& cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$build`" -G `"$gen`" -DCMAKE_BUILD_TYPE=`"$cfg`""
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
Write-Host "configure ok"

& cmd /c "`"$vcvars`" >nul 2>&1 && cmake --build `"$build`" --config `"$cfg`""
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
Write-Host "build ok"

Write-Host "running test gate (ctest)..."
& cmd /c "`"$vcvars`" >nul 2>&1 && ctest --test-dir `"$build`" -C `"$cfg`" --output-on-failure"
if ($LASTEXITCODE -ne 0) { throw "ctest failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "built binaries:"
Get-ChildItem -LiteralPath $build -Filter "*.exe" -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName, Length

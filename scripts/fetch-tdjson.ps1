# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# scripts/fetch-tdjson.ps1 - download the TDLib runtime (tdjson + companion
# DLLs) into vendor/. Source: tdlib.native nupkg (prebuilt official TDLib
# releases by ForNeVeR). Re-run any time; stale files are replaced.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-tdjson.ps1
#   powershell ... -Version 1.8.67
param(
    [string]$Version = "1.8.67"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root "vendor"
New-Item -ItemType Directory -Force -Path $vendor | Out-Null

$base = "https://github.com/ForNeVeR/tdlib.native/releases/download/v$Version"
$asset = "tdlib.native.win-x64.$Version.nupkg"
$tmp = Join-Path $env:TEMP $asset

Write-Host "Downloading $base/$asset ..."
Invoke-WebRequest -Uri "$base/$asset" -OutFile $tmp

$stage = Join-Path $env:TEMP "tdlib-native-win"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($tmp, $stage)

Copy-Item (Join-Path $stage "runtimes\win-x64\native\*") $vendor -Force
Remove-Item $tmp, $stage -Recurse -Force

Write-Host "Vendored into $vendor :"
Get-ChildItem $vendor | Select-Object Name, @{n="MB";e={[math]::Round($_.Length/1MB,2)}}

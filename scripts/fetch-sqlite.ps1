# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# scripts/fetch-sqlite.ps1 - download the SQLite amalgamation into
# third_party/sqlite/. SQLite is public domain; the amalgamation is the two
# files sqlite3.c + sqlite3.h, compiled straight into the engine (no DLL).
# Re-run any time; stale files are replaced.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-sqlite.ps1
#   powershell ... -Version 3540100
param(
    [string]$Version = "3540100"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\sqlite"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$asset = "sqlite-amalgamation-$Version.zip"
$url = "https://www.sqlite.org/2026/$asset"
$tmp = Join-Path $env:TEMP $asset

Write-Host "Downloading $url ..."
Invoke-WebRequest -Uri $url -OutFile $tmp

$stage = Join-Path $env:TEMP "sqlite-amalgamation"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($tmp, $stage)

# The zip contains one directory (sqlite-amalgamation-<version>/); lift the
# two source files out of it.
$inner = Get-ChildItem $stage -Directory | Select-Object -First 1
Copy-Item (Join-Path $inner.FullName "sqlite3.c") $dest -Force
Copy-Item (Join-Path $inner.FullName "sqlite3.h") $dest -Force
Remove-Item $tmp, $stage -Recurse -Force

Write-Host "Vendored into $dest :"
Get-ChildItem $dest | Select-Object Name, @{n="MB";e={[math]::Round($_.Length/1MB,2)}}

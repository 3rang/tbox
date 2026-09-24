# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel
#
# scripts/fetch-qrcodegen.ps1 — vendor Nayuki's QR Code generator (MIT),
# single-file C implementation, into third_party/qrcodegen/. See the
# "Vendoring" section of CMakeLists.txt for how it is built.
#
# Run from the project root:
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-qrcodegen.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\qrcodegen"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# Nayuki QR-Code-generator — https://github.com/nayuki/QR-Code-generator
# Canonical C port, MIT license (same license family as cJSON/tdlib).
$base = "https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c"

$pairs = @(
    @("qrcodegen.c", "qrcodegen.c"),
    @("qrcodegen.h", "qrcodegen.h")
)

# Match the user-agent ~rest ~ of the other fetch scripts.
$userAgent = "tbox-v0.5 fetch-qrcodegen (CMake vendoring)"

foreach ($p in $pairs) {
    $src = "$base/$($p[0])"
    $out = Join-Path $dest $p[1]
    Write-Host "fetch: $src"
    Invoke-WebRequest -UseBasicParsing -Uri $src -OutFile $out -UserAgent $userAgent
}

Get-ChildItem $dest | Select-Object Name, Length

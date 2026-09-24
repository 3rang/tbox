# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel

# scripts/vendor-qrcodegen.ps1 — vendor Nayuki QR-Code-generator C port (MIT)
# into third_party/qrcodegen/. Qr-demo renders the authorization QR link here.
#
# Run from the project root:
#   powershell -ExecutionPolicy Bypass -File scripts\vendor-qrcodegen.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\qrcodegen"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$base = "https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c"
$files = @(
    @{ remote = "$base/qrcodegen.c"; local = "qrcodegen.c" },
    @{ remote = "$base/qrcodegen.h"; local = "qrcodegen.h" }
)

foreach ($f in $files) {
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $f.remote -OutFile (Join-Path $dest $f.local)
        Write-Host ("vendor qrcodegen: {0}" -f $f.local)
    }
    catch {
        Write-Error ("failed to fetch qrcodegen {0}: {1}" -f $f.local, $_.Exception.Message)
        exit 1
    }
}

Get-ChildItem $dest | Select-Object Name, Length

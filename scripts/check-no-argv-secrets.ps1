# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# scripts/check-no-argv-secrets.ps1 - the P1 guard (spec 5.1: secrets never
# in argv). Registered as a ctest test so it runs on every build.
#
# Rule: the engine must never read api_hash, a password or the TDLib db key
# from the command line. Credentials arrive over the protocol (serve) or as
# a stdin JSON line (the dev login command). This greps the C sources for
# argv access next to any secret-ish name.
#
# Exit 0 = clean, 1 = a violation was found.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "src"

$secretNames = 'api_hash', 'api-id', 'api_id', 'password', 'db-key', 'db_key', 'passwd'

# A violation is argv ACCESS next to a secret name on the same line. The
# word list alone is not enough (help text and comments mention "api-hash"
# legitimately); only a subscript into the argv array counts.
$argvAccess = 'argv\s*\['

$violations = @()
Get-ChildItem -Path $src -Recurse -Include *.c, *.h | ForEach-Object {
    $file = $_
    $n = 0
    Get-Content -LiteralPath $file.FullName | ForEach-Object {
        $n++
        $line = $_
        if (($line -match $argvAccess)) {
            foreach ($s in $secretNames) {
                if ($line -match $s) {
                    $violations += "$($file.FullName):$($n): $($line.Trim())"
                    break
                }
            }
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Output "argv secret leak(s) found:"
    $violations | ForEach-Object { Write-Output "  $_" }
    exit 1
}

Write-Output "no secrets in argv"
exit 0

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# build.ps1 - one command that produces the app.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# In order:
#   1. build + test the C engine              (scripts/build-native.ps1)
#   2. copy the engine and its DLLs into the Python package (tbox/bin/)
#   3. install the front into .venv
#   4. run the front's tests
#   5. write a wheel into dist/
#   6. freeze the desktop app into a single dist\tbox.exe, then run its
#      self-check so a broken bundle fails the build rather than the user
#
# Step 2 is what turns two half-apps into one: the wheel carries the compiled
# engine, so installing it needs no compiler and no repository. Step 6 is the
# same idea as a single double-clickable file.

$ErrorActionPreference = "Stop"

$root   = $PSScriptRoot
$front  = Join-Path $root "front"
$bundle = Join-Path $front "src\tbox\bin"
$build  = Join-Path $root "build"
$venv   = Join-Path $root ".venv"
$dist   = Join-Path $root "dist"
$py     = Join-Path $venv "Scripts\python.exe"

function Step([string]$text) {
    Write-Host ""
    Write-Host "== $text" -ForegroundColor Cyan
}

Step "1/6  C engine: configure, build, test"
& (Join-Path $root "scripts\build-native.ps1")

Step "2/6  bundle the engine into the package (tbox/bin)"
if (-not (Test-Path -LiteralPath (Join-Path $build "tbox-engine.exe"))) {
    throw "build\tbox-engine.exe is missing - step 1 did not produce it"
}
if (Test-Path -LiteralPath $bundle) {
    Remove-Item -LiteralPath $bundle -Recurse -Force
}
New-Item -ItemType Directory -Path $bundle -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build "tbox-engine.exe") -Destination $bundle
Get-ChildItem -LiteralPath $build -Filter "*.dll" | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $bundle
}
$bundled = Get-ChildItem -LiteralPath $bundle
$sizeMB = [math]::Round(($bundled | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host "     $($bundled.Count) files, $sizeMB MB (engine + runtime dlls)"

Step "3/6  install the front into .venv"
if (-not (Test-Path -LiteralPath $py)) {
    python -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "creating the virtual env failed ($LASTEXITCODE)" }
}
# the gui extra brings the window toolkit; the pack extra brings the freezer
& $py -m pip install -q -e "$front[gui,pack]"
if ($LASTEXITCODE -ne 0) { throw "pip install failed ($LASTEXITCODE)" }

Step "4/6  front tests"
& $py -m unittest discover -s (Join-Path $front "tests")
if ($LASTEXITCODE -ne 0) { throw "front tests failed ($LASTEXITCODE)" }

Step "5/6  package the app as a wheel"
if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}
New-Item -ItemType Directory -Path $dist -Force | Out-Null
& $py -m pip wheel --no-deps --wheel-dir $dist $front
if ($LASTEXITCODE -ne 0) { throw "building the wheel failed ($LASTEXITCODE)" }

Step "6/6  freeze the desktop app into one file"
& $py (Join-Path $front "packaging\make_icon.py")
if ($LASTEXITCODE -ne 0) { throw "drawing the icon failed ($LASTEXITCODE)" }
& $py -m PyInstaller --noconfirm --clean `
    --workpath (Join-Path $root "build\pyinstaller") `
    --distpath $dist `
    (Join-Path $front "packaging\tbox-gui.spec")
if ($LASTEXITCODE -ne 0) { throw "freezing the app failed ($LASTEXITCODE)" }

# a frozen windowed app has no console, so it reports through a file and an
# exit code; running it here means a bundle that cannot start fails the build
$selftest = Join-Path $root "build\selftest-report.txt"
if (Test-Path -LiteralPath $selftest) { Remove-Item -LiteralPath $selftest -Force }
$proc = Start-Process -FilePath (Join-Path $dist "tbox.exe") `
    -ArgumentList @("--selftest", "--report", $selftest) -Wait -PassThru
if ($proc.ExitCode -ne 0) { throw "the frozen app failed its self-check (exit $($proc.ExitCode))" }
Write-Host "     self-check passed:"
Get-Content -LiteralPath $selftest | ForEach-Object { Write-Host "     $_" }

Write-Host ""
Write-Host "done." -ForegroundColor Green
Get-ChildItem -LiteralPath $dist -File |
    Where-Object { $_.Extension -in ".whl", ".exe" } |
    Select-Object Name, @{ n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } } |
    Format-Table -AutoSize
Write-Host "run it:        $(Join-Path $venv 'Scripts\tbox.exe') login"
Write-Host "install it:    pip install $(Join-Path $dist '<the wheel above>')"
Write-Host "or just run:   $(Join-Path $dist 'tbox.exe')"
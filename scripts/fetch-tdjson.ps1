param(
    [string]$Version = "1.8.67",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root "vendor"
New-Item -ItemType Directory -Force -Path $vendor | Out-Null

if ($Arch -ne "x64") {
    Write-Error "Windows fetch only supports x64 for now (arm64 asset is a nupkg too, adapt if needed)"
}

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
Write-Host "Vendored into $vendor :"
Get-ChildItem $vendor | Select-Object -ExpandProperty Name
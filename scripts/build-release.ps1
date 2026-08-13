# build-release.ps1 - configure + build CHEBURNET.exe (Release, x64).
[CmdletBinding()]
param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\vsenv.ps1"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir

Import-VcVars

Write-Host "== Configure ==" -ForegroundColor Cyan
& $CMakeExe -S $root -B $build -G Ninja -DCMAKE_MAKE_PROGRAM="$NinjaExe" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host "== Build ==" -ForegroundColor Cyan
& $CMakeExe --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $build "CHEBURNET.exe"
if (Test-Path $exe) {
    Write-Host "OK: $exe ($([math]::Round((Get-Item $exe).Length/1MB,2)) MB)" -ForegroundColor Green
} else {
    throw "CHEBURNET.exe not produced"
}

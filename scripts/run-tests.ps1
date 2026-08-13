# run-tests.ps1 - run unit tests (ctest) + independent strategy cross-check.
[CmdletBinding()]
param([string]$BuildDir = "build")
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\vsenv.ps1"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    Write-Host "No build found; building first..." -ForegroundColor Yellow
    & "$PSScriptRoot\build-release.ps1" -BuildDir $BuildDir
}

Write-Host "== ctest ==" -ForegroundColor Cyan
& $CTestExe --test-dir $build --output-on-failure
$ctestRc = $LASTEXITCODE

Write-Host "== independent strategy cross-check ==" -ForegroundColor Cyan
$testExe = Join-Path $build "tests\cheburnet_tests.exe"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\cmake\verify_strategies.ps1" `
    -BatDir "$root\resources\strategies_src" -TestExe $testExe
$verifyRc = $LASTEXITCODE

if ($ctestRc -ne 0 -or $verifyRc -ne 0) {
    Write-Host "TESTS FAILED (ctest=$ctestRc verify=$verifyRc)" -ForegroundColor Red
    exit 1
}
Write-Host "ALL TESTS PASSED" -ForegroundColor Green

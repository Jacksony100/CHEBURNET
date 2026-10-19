# A separate fixed namespace; never accepted by the production EXE at runtime.
[CmdletBinding()]
param(
    [string]$BuildDir = 'build-host-e2e',
    [ValidatePattern('^[0-9a-f]{16}$')][string]$Instance = ([guid]::NewGuid().ToString('N').Substring(0, 16))
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'scripts\vsenv.ps1')
Import-VcVars
$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDir))
& $CMakeExe -S $root -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    -DCMAKE_BUILD_TYPE=Release -DCHEBURNET_ENABLE_ANALYZE=ON "-DCHEBURNET_E2E_INSTANCE=$Instance"
if ($LASTEXITCODE -ne 0) { throw 'host E2E configuration failed' }
& $CMakeExe --build $build --target CHEBURNET host_driver_smoke
if ($LASTEXITCODE -ne 0) { throw 'host E2E build failed' }
$launcher = Join-Path $build 'CHEBURNET-E2E.exe'
$helper = Join-Path $build 'tests\host_driver_smoke.exe'
$manifest = [ordered]@{
    schema=1; instance=$Instance; launcher=$launcher; driver_probe=$helper
    launcher_sha256=(Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash.ToLowerInvariant()
    driver_probe_sha256=(Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash.ToLowerInvariant()
    source_revision=[string](& git -C $root rev-parse HEAD)
}
[IO.File]::WriteAllText((Join-Path $build 'host-e2e-build.json'),
    ($manifest | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Write-Output "HOST_E2E_BUILD: $build instance=$Instance"

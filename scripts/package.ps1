# Assemble a clean public release folder. End-user UX remains one CHEBURNET.exe.
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$OutDir = 'dist', [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
. "$PSScriptRoot\package-path.ps1"
$out = Assert-SafePackageOutDir $root $OutDir
$build = Get-NormalizedFullPath $BuildDir $root

if (-not $SkipBuild) {
    & "$PSScriptRoot\build-release.ps1" -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

$exe = Join-Path $build 'CHEBURNET.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "required artifact missing: $exe" }
foreach ($required in @('README.md','CHANGELOG.md','SECURITY.md','THIRD_PARTY_NOTICES.md',
                         'DEPENDENCIES.md','LICENSE')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
        throw "required release file missing: $required"
    }
}
$licenseDir = Join-Path $root 'LICENSES'
if (-not (Test-Path -LiteralPath $licenseDir -PathType Container) -or
    @(Get-ChildItem -LiteralPath $licenseDir -File).Count -eq 0) {
    throw 'required third-party LICENSES are missing'
}

Remove-SafePackageTree $root $out
New-Item -ItemType Directory -Path $out | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $out 'CHEBURNET.exe')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'SECURITY.md') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'DEPENDENCIES.md') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $out
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $out
Copy-Item -LiteralPath $licenseDir -Destination (Join-Path $out 'LICENSES') -Recurse

$distExe = Join-Path $out 'CHEBURNET.exe'
$hash = (Get-FileHash -LiteralPath $distExe -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $out 'CHEBURNET.exe.sha256'),
    "$hash  CHEBURNET.exe`n", [Text.UTF8Encoding]::new($false))

$pdb = Join-Path $build 'CHEBURNET.pdb'
if (Test-Path -LiteralPath $pdb -PathType Leaf) {
    $symbols = Join-Path $out 'symbols'
    New-Item -ItemType Directory -Path $symbols | Out-Null
    Copy-Item -LiteralPath $pdb -Destination (Join-Path $symbols 'CHEBURNET.pdb')
}

Write-Host "Release assembled at: $out" -ForegroundColor Green
Get-ChildItem -LiteralPath $out -Recurse | Select-Object FullName, Length

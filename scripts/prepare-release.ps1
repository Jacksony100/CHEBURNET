[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Repository,
    [string]$BuildDir = 'build-release',
    [string]$OutDir = 'dist'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
. "$PSScriptRoot\version.ps1"
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'release repository has unsupported syntax'
}

# The tag must exactly match the authoritative version model, channel
# included: a stable tag cannot publish RC semantics, and vice versa.
$version = Get-CheburnetVersion -Root $root
$tagVersion = Assert-CheburnetReleaseTag -Tag $Tag -Version $version
$sourceVersion = $version.Semantic

$provenancePath = Join-Path $root 'resources\upstream\provenance.json'
$payloadVersion = [string](Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 |
    ConvertFrom-Json).version
if ($payloadVersion -notmatch '^[0-9]+(?:\.[0-9]+){1,7}[A-Za-z0-9+.-]*$') {
    throw 'invalid payload version in provenance'
}

$out = if ([IO.Path]::IsPathRooted($OutDir)) {
    [IO.Path]::GetFullPath($OutDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $root $OutDir))
}
$payload = Join-Path $out "cheburnet-payload-$payloadVersion.cbpkg"
$manifest = Join-Path $out 'update-manifest.json'
$signature = Join-Path $out 'update-manifest.json.sig'
$baseUrl = "https://github.com/$Repository/releases/download/$Tag"

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') `
    -BuildDir $BuildDir -OutDir $OutDir -SkipBuild
if ($LASTEXITCODE -ne 0) { throw 'release packaging failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'build-update-package.ps1') -OutFile $payload
if ($LASTEXITCODE -ne 0) { throw 'update package generation failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'generate-update-manifest.ps1') `
    -Launcher (Join-Path $out 'CHEBURNET.exe') -Payload $payload `
    -LauncherVersion $sourceVersion -BaseUrl $baseUrl -OutFile $manifest
if ($LASTEXITCODE -ne 0) { throw 'update manifest generation failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'sign-update-manifest.ps1') `
    -Manifest $manifest -SignatureOut $signature
if ($LASTEXITCODE -ne 0) { throw 'update manifest signing failed' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'verify-update-manifest-signature.ps1') `
    -Manifest $manifest -Signature $signature
if ($LASTEXITCODE -ne 0) { throw 'update manifest signature verification failed' }

$peVersion = Assert-CheburnetLauncherPeVersion -Launcher (Join-Path $out 'CHEBURNET.exe') `
    -Version $version

$payloadName = Split-Path -Leaf $payload
$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($payload + '.sha256', "$payloadHash  $payloadName`n",
    [Text.Encoding]::ASCII)
Copy-Item -LiteralPath (Join-Path $root 'DEPENDENCIES.md') -Destination (Join-Path $out 'DEPENDENCIES.md') -Force
Write-Output ("RELEASE_PREPARATION: PASS tag=$Tag launcher=$sourceVersion pe=$peVersion " +
              "channel=$($version.Channel) payload=$payloadVersion")

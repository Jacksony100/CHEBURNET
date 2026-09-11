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
. "$PSScriptRoot\authenticode.ps1"
. "$PSScriptRoot\package-path.ps1"
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

# ---------------------------------------------------------------------------
# Authenticode gate. Signing must already have happened: this script only
# generates hashes and signed update metadata, and both must describe the
# published bytes. A stable tag is refused outright unless the built launcher
# carries a valid, timestamped signature.
# ---------------------------------------------------------------------------
$buildLauncher = Join-Path (Get-NormalizedFullPath $BuildDir $root) 'CHEBURNET.exe'
if (-not (Test-Path -LiteralPath $buildLauncher -PathType Leaf)) {
    throw "built launcher not found: $buildLauncher"
}
$requireSignature = -not $version.IsPrerelease
if ($requireSignature) {
    $signedReport = Assert-CheburnetSignedLauncher -Path $buildLauncher -RequireTimestamp
    Write-Output ("RELEASE_SIGNING: REQUIRED status=$($signedReport.Status) " +
                  "timestamped=$($signedReport.Timestamped) sha256=$($signedReport.Sha256)")
} else {
    $signedReport = Get-CheburnetAuthenticodeReport -Path $buildLauncher
    Write-Output ("RELEASE_SIGNING: OPTIONAL (prerelease) status=$($signedReport.Status) " +
                  "timestamped=$($signedReport.Timestamped) sha256=$($signedReport.Sha256)")
}
$signedLauncherSha256 = $signedReport.Sha256

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') `
    -BuildDir $BuildDir -OutDir $OutDir -SkipBuild
if ($LASTEXITCODE -ne 0) { throw 'release packaging failed' }

# The packaged launcher must be exactly the bytes that were inspected above.
# This is what catches an executable rebuilt or replaced after signing.
$distLauncher = Join-Path $out 'CHEBURNET.exe'
if ($requireSignature) {
    $publishedReport = Assert-CheburnetSignedLauncher -Path $distLauncher -RequireTimestamp `
        -ExpectedSha256 $signedLauncherSha256
} else {
    $publishedReport = Get-CheburnetAuthenticodeReport -Path $distLauncher
    if ($publishedReport.Sha256 -cne $signedLauncherSha256) {
        throw ("packaged launcher differs from the built launcher: expected " +
               "$signedLauncherSha256, found $($publishedReport.Sha256)")
    }
}
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

# Final gate immediately before publication: the launcher that is about to be
# uploaded must still be the signed artifact, and the published SHA-256 and the
# signed update manifest must both describe exactly those bytes.
$publishedSha = (Get-FileHash -LiteralPath $distLauncher -Algorithm SHA256).Hash.ToLowerInvariant()
if ($publishedSha -cne $signedLauncherSha256) {
    throw "launcher changed after release metadata generation: $publishedSha"
}
$declaredSha = [string](Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 |
    ConvertFrom-Json).launcher.sha256
if ($declaredSha -cne $publishedSha) {
    throw "signed manifest declares launcher SHA-256 $declaredSha, published file is $publishedSha"
}
$sidecar = Get-Content -LiteralPath (Join-Path $out 'CHEBURNET.exe.sha256') -Raw -Encoding UTF8
if ($sidecar -cnotmatch ('^' + [regex]::Escape($publishedSha) + '\s')) {
    throw 'published CHEBURNET.exe.sha256 does not describe the published launcher'
}
if ($requireSignature) {
    Assert-CheburnetSignedLauncher -Path $distLauncher -RequireTimestamp `
        -ExpectedSha256 $publishedSha | Out-Null
}

Write-Output ("RELEASE_PREPARATION: PASS tag=$Tag launcher=$sourceVersion pe=$peVersion " +
              "channel=$($version.Channel) payload=$payloadVersion " +
              "signed=$($publishedReport.Ok) launcher_sha256=$publishedSha")

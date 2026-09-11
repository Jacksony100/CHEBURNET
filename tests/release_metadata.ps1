[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$Payload
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
. (Join-Path $root 'scripts\version.ps1')
$launcherPath = [IO.Path]::GetFullPath($Launcher)
$payloadPath = [IO.Path]::GetFullPath($Payload)

# The built launcher's PE version must follow the model exactly and stay
# numeric: a prerelease is expressed only by the semantic version.
$version = Get-CheburnetVersion -Root $root
Assert-CheburnetLauncherPeVersion -Launcher $launcherPath -Version $version | Out-Null

$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($launcherPath)
$expectedDescription = [regex]::Unescape('CHEBURNET \u2014 \u0443\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435 \u0441\u043e\u0435\u0434\u0438\u043d\u0435\u043d\u0438\u0435\u043c')
if ([string]$versionInfo.FileDescription -cne $expectedDescription) {
    throw 'launcher PE description is not correctly encoded Russian text'
}

$testRoot = Join-Path (Split-Path -Parent $launcherPath) ('release-metadata-' + [guid]::NewGuid().ToString('N'))
$manifest = Join-Path $testRoot 'update-manifest.json'
$truncated = Join-Path $testRoot 'truncated.cbpkg'
$generator = Join-Path $root 'scripts\generate-update-manifest.ps1'

function Invoke-ManifestGenerator {
    # Returns the generator exit code without aborting the test on failure.
    param([string[]]$Arguments)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $generator @Arguments 2>$null |
            Out-Null
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
}

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null

    # --- positive: the manifest must carry the semantic version -------------
    $exitCode = Invoke-ManifestGenerator @(
        '-Launcher', $launcherPath, '-Payload', $payloadPath,
        '-LauncherVersion', $version.Semantic,
        '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)
    if ($exitCode -ne 0 -or -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw 'valid release metadata generation failed'
    }
    $generated = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$generated.launcher.version -cne $version.Semantic) {
        throw ("generated launcher metadata must carry the semantic version " +
               "$($version.Semantic), got $($generated.launcher.version)")
    }
    if ([long]$generated.launcher.size -ne (Get-Item -LiteralPath $launcherPath).Length) {
        throw 'generated launcher size mismatch'
    }
    if ([string]$generated.channel -cne 'stable') { throw 'manifest channel must be stable' }
    # The compatibility floor must admit already published RCs of the same
    # version, otherwise an installed 1.0.0-rc.N cannot move to stable 1.0.0.
    foreach ($floor in @([string]$generated.launcher.minimum_supported_version,
                         [string]$generated.payload.minimum_launcher_version)) {
        if (-not (Test-CheburnetSemanticVersion $floor)) {
            throw "manifest compatibility floor is not a canonical version: $floor"
        }
        if ((Compare-CheburnetVersion $floor $version.Semantic) -gt 0) {
            throw "manifest compatibility floor $floor is newer than the launcher"
        }
    }

    # --- negative: the argument version disagrees with the model ------------
    if ((Invoke-ManifestGenerator @(
            '-Launcher', $launcherPath, '-Payload', $payloadPath,
            '-LauncherVersion', '9.9.9',
            '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)) -eq 0) {
        throw 'mismatched source/manifest semantic version was accepted'
    }

    # --- negative: the PE version cannot stand in for the semantic one ------
    if ((Invoke-ManifestGenerator @(
            '-Launcher', $launcherPath, '-Payload', $payloadPath,
            '-LauncherVersion', $version.Pe,
            '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)) -eq 0) {
        throw 'numeric PE version was accepted as the semantic launcher version'
    }

    # --- negative: compatibility floor newer than the published release -----
    if ((Invoke-ManifestGenerator @(
            '-Launcher', $launcherPath, '-Payload', $payloadPath,
            '-LauncherVersion', $version.Semantic,
            '-MinimumSupportedLauncherVersion', '99.0.0',
            '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)) -eq 0) {
        throw 'compatibility floor newer than the launcher was accepted'
    }

    # --- negative: malformed compatibility floor ----------------------------
    if ((Invoke-ManifestGenerator @(
            '-Launcher', $launcherPath, '-Payload', $payloadPath,
            '-LauncherVersion', $version.Semantic,
            '-MinimumSupportedLauncherVersion', '1.0.0-rc.',
            '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)) -eq 0) {
        throw 'malformed compatibility floor was accepted'
    }

    # --- negative: truncated payload package --------------------------------
    [IO.File]::WriteAllBytes($truncated, [Text.Encoding]::ASCII.GetBytes("CBPKG1`r`n"))
    if ((Invoke-ManifestGenerator @(
            '-Launcher', $launcherPath, '-Payload', $truncated,
            '-LauncherVersion', $version.Semantic,
            '-BaseUrl', 'https://example.com/releases/vtest', '-OutFile', $manifest)) -eq 0) {
        throw 'truncated payload package was accepted'
    }
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
Write-Output ("RELEASE_METADATA: PASS semantic=$($version.Semantic) pe=$($version.Pe) " +
              '(version model / PE / package provenance gates)')

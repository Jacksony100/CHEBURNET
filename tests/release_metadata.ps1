[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$Payload
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$launcherPath = [IO.Path]::GetFullPath($Launcher)
$payloadPath = [IO.Path]::GetFullPath($Payload)
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($launcherPath)
$version = ([string]$versionInfo.ProductVersion) -replace '\.0$', ''
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'test launcher has no canonical PE version' }
$expectedDescription = [regex]::Unescape('CHEBURNET \u2014 \u0443\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u0435 \u0441\u043e\u0435\u0434\u0438\u043d\u0435\u043d\u0438\u0435\u043c')
if ([string]$versionInfo.FileDescription -cne $expectedDescription) {
    throw 'launcher PE description is not correctly encoded Russian text'
}

$testRoot = Join-Path (Split-Path -Parent $launcherPath) ('release-metadata-' + [guid]::NewGuid().ToString('N'))
$manifest = Join-Path $testRoot 'update-manifest.json'
$truncated = Join-Path $testRoot 'truncated.cbpkg'
try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\generate-update-manifest.ps1') `
        -Launcher $launcherPath -Payload $payloadPath -LauncherVersion $version `
        -BaseUrl 'https://example.com/releases/vtest' -OutFile $manifest
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw 'valid release metadata generation failed'
    }
    $generated = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$generated.launcher.version -cne $version -or
        [long]$generated.launcher.size -ne (Get-Item -LiteralPath $launcherPath).Length) {
        throw 'generated launcher metadata mismatch'
    }

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\generate-update-manifest.ps1') `
        -Launcher $launcherPath -Payload $payloadPath -LauncherVersion '9.9.9' `
        -BaseUrl 'https://example.com/releases/vtest' -OutFile $manifest 2>$null | Out-Null
    $mismatchExit = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($mismatchExit -eq 0) { throw 'mismatched PE/manifest version was accepted' }

    [IO.File]::WriteAllBytes($truncated, [Text.Encoding]::ASCII.GetBytes("CBPKG1`r`n"))
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\generate-update-manifest.ps1') `
        -Launcher $launcherPath -Payload $truncated -LauncherVersion $version `
        -BaseUrl 'https://example.com/releases/vtest' -OutFile $manifest 2>$null | Out-Null
    $truncatedExit = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference
    if ($truncatedExit -eq 0) { throw 'truncated payload package was accepted' }
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
Write-Output 'RELEASE_METADATA: PASS (PE version/package provenance gates)'

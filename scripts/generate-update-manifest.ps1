[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Launcher,
    [Parameter(Mandatory=$true)][string]$Payload,
    [Parameter(Mandatory=$true)][string]$BaseUrl,
    [string]$OutFile = 'dist\update-manifest.json',
    [string]$LauncherVersion,
    # Oldest launcher allowed to move to this release. The value includes RCs
    # of the same core version, otherwise an installed 1.0.0-rc.N would get
    # LauncherTooOld when moving to stable 1.0.0 (an rc orders below stable).
    [string]$MinimumSupportedLauncherVersion = '1.0.0-rc.1',
    [string]$KeyId = 'cheburnet-release-2026'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot\version.ps1"
$baseUri = $null
if (-not [Uri]::TryCreate($BaseUrl, [UriKind]::Absolute, [ref]$baseUri) -or
    $baseUri.Scheme -cne 'https' -or [string]::IsNullOrWhiteSpace($baseUri.Host) -or
    -not [string]::IsNullOrEmpty($baseUri.UserInfo) -or
    -not [string]::IsNullOrEmpty($baseUri.Query) -or
    -not [string]::IsNullOrEmpty($baseUri.Fragment)) {
    throw 'BaseUrl must be an absolute HTTPS URL without credentials/query/fragment'
}
if ($KeyId -notmatch '^[a-z0-9-]{1,64}$') { throw 'invalid signing key id' }
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$trust = Get-Content -LiteralPath (Join-Path $root 'resources\update\release-public-key.json') `
    -Raw -Encoding UTF8 | ConvertFrom-Json
$keysProperty = $trust.PSObject.Properties['keys']
$trustedKeys = @(if ($null -ne $keysProperty) { @($keysProperty.Value) } else { @($trust) })
$selectedKeys = @($trustedKeys | Where-Object { [string]$_.key_id -ceq $KeyId })
if ($selectedKeys.Count -ne 1 -or
    [string]$selectedKeys[0].algorithm -cne 'ECDSA_P256_SHA256') {
    throw 'manifest key_id is not present exactly once in the embedded trust source'
}
$launcherPath = [IO.Path]::GetFullPath($Launcher)
$payloadPath = [IO.Path]::GetFullPath($Payload)
$out = if ([IO.Path]::IsPathRooted($OutFile)) { [IO.Path]::GetFullPath($OutFile) }
       else { [IO.Path]::GetFullPath((Join-Path $root $OutFile)) }
foreach ($path in @($launcherPath,$payloadPath)) {
    if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "artifact must be a repository file: $path"
    }
    if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "reparse artifact rejected: $path"
    }
}
if (-not $out.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'manifest output must be inside repository'
}
# The manifest's semantic version comes from the authoritative version model
# rather than from a free-form argument, so signed metadata cannot drift from
# the build. The numeric PE version is checked separately because the PE
# format cannot express a prerelease (1.0.0-rc.3 -> 1.0.0.3).
$versionModel = Get-CheburnetVersion -Root $root
if ([string]::IsNullOrEmpty($LauncherVersion)) { $LauncherVersion = $versionModel.Semantic }
if (-not (Test-CheburnetSemanticVersion $LauncherVersion)) { throw 'invalid launcher version' }
if ($LauncherVersion -cne $versionModel.Semantic) {
    throw ("launcher version $LauncherVersion does not match source semantic version " +
           "$($versionModel.Semantic)")
}
Assert-CheburnetLauncherPeVersion -Launcher $launcherPath -Version $versionModel | Out-Null
if (-not (Test-CheburnetSemanticVersion $MinimumSupportedLauncherVersion)) {
    throw 'invalid minimum supported launcher version'
}
if ((Compare-CheburnetVersion $MinimumSupportedLauncherVersion $LauncherVersion) -gt 0) {
    throw ("minimum supported launcher version $MinimumSupportedLauncherVersion is newer " +
           "than the published launcher $LauncherVersion")
}
$provenance = Get-Content -LiteralPath (Join-Path $root 'resources\upstream\provenance.json') `
    -Raw -Encoding UTF8 | ConvertFrom-Json
if (@($provenance.PSObject.Properties).Count -ne 10 -or
    [string]$provenance.provider -cne 'Flowseal/zapret-discord-youtube' -or
    [string]$provenance.version -notmatch '^[0-9]+(?:\.[0-9]+){1,7}[a-z]?$' -or
    [string]$provenance.tag -cne [string]$provenance.version -or
    [long]$provenance.release_id -le 0 -or $provenance.immutable -ne $true -or
    [string]$provenance.archive_sha256 -notmatch '^[a-f0-9]{64}$' -or
    [string]$provenance.upstream_commit -notmatch '^[a-f0-9]{40}$') {
    throw 'upstream provenance contract rejected'
}

# The manifest may only describe a package whose own strict header agrees with
# the selected provenance and compatibility schemas. This prevents release
# automation from signing a version label that is not present in the artifact.
$stream = [IO.File]::Open($payloadPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                          [IO.FileShare]::Read)
try {
    $prefix = [byte[]]::new(12)
    $read = $stream.Read($prefix, 0, $prefix.Length)
    if ($read -ne $prefix.Length -or
        [Text.Encoding]::ASCII.GetString($prefix, 0, 8) -cne "CBPKG1`r`n") {
        throw 'payload package magic/header rejected'
    }
    $headerLength = [BitConverter]::ToUInt32($prefix, 8)
    if ($headerLength -eq 0 -or $headerLength -gt 1MB -or
        12L + $headerLength -gt $stream.Length) {
        throw 'payload package header length rejected'
    }
    $headerBytes = [byte[]]::new($headerLength)
    $offset = 0
    while ($offset -lt $headerBytes.Length) {
        $count = $stream.Read($headerBytes, $offset, $headerBytes.Length - $offset)
        if ($count -le 0) { throw 'payload package header truncated' }
        $offset += $count
    }
    $headerText = [Text.UTF8Encoding]::new($false, $true).GetString($headerBytes)
    $packageHeader = $headerText | ConvertFrom-Json
} finally {
    $stream.Dispose()
}
if (@($packageHeader.PSObject.Properties).Count -ne 6 -or
    [int]$packageHeader.schema -ne 1 -or [int]$packageHeader.payload_schema -ne 1 -or
    [int]$packageHeader.strategy_schema -ne 1 -or
    [string]$packageHeader.payload_version -cne [string]$provenance.version -or
    [string]$packageHeader.provider -cne [string]$provenance.provider -or
    $null -eq $packageHeader.files) {
    throw 'payload package metadata does not match provenance/schema'
}
$manifest = [ordered]@{
    schema = 1
    channel = 'stable'
    generated_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    launcher = [ordered]@{
        version=$LauncherVersion; url=($BaseUrl.TrimEnd('/') + '/CHEBURNET.exe')
        sha256=(Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash.ToLowerInvariant()
        size=[long](Get-Item -LiteralPath $launcherPath).Length
        minimum_supported_version=$MinimumSupportedLauncherVersion
    }
    payload = [ordered]@{
        provider=[string]$provenance.provider; version=[string]$provenance.version
        url=($BaseUrl.TrimEnd('/') + '/' + [IO.Path]::GetFileName($payloadPath))
        sha256=(Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
        size=[long](Get-Item -LiteralPath $payloadPath).Length
        minimum_launcher_version=$MinimumSupportedLauncherVersion; payload_schema=1; strategy_schema=1
        upstream_release_url=[string]$provenance.release_url
    }
    key_id=$KeyId
}
$parent = Split-Path -Parent $out
if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
[IO.File]::WriteAllText($out, ($manifest | ConvertTo-Json -Depth 8 -Compress) + "`n",
                        [Text.UTF8Encoding]::new($false))
Write-Output "UPDATE_MANIFEST: PASS $out"

[CmdletBinding()]
param(
    [string]$PayloadDir = 'resources\payload',
    [string]$StrategyCatalog = 'resources\generated\strategy-catalog.json',
    [string]$Provenance = 'resources\upstream\provenance.json',
    [string]$OutFile = 'dist\cheburnet-payload.cbpkg',
    [int]$PayloadSchema = 1,
    [int]$StrategySchema = 1
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
function Resolve-RepoPath([string]$value) {
    $full = if ([IO.Path]::IsPathRooted($value)) { [IO.Path]::GetFullPath($value) }
            else { [IO.Path]::GetFullPath((Join-Path $root $value)) }
    if (-not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "path escapes repository: $value"
    }
    return $full
}
function Assert-NoReparseComponents([string]$value) {
    $full = [IO.Path]::GetFullPath($value)
    $cursor = $full
    while (-not [string]::IsNullOrWhiteSpace($cursor)) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "path contains a reparse point: $cursor"
            }
        }
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}
$payload = Resolve-RepoPath $PayloadDir
$catalog = Resolve-RepoPath $StrategyCatalog
$provenancePath = Resolve-RepoPath $Provenance
$output = Resolve-RepoPath $OutFile
foreach ($required in @($payload,$catalog,$provenancePath)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "required input missing: $required" }
    Assert-NoReparseComponents $required
}
Assert-NoReparseComponents (Split-Path -Parent $output)
if (Test-Path -LiteralPath $output) {
    $existingOutput = Get-Item -LiteralPath $output -Force
    if (($existingOutput.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'reparse output file rejected'
    }
}
$provenanceJson = Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 | ConvertFrom-Json
$version = [string]$provenanceJson.version
$provider = [string]$provenanceJson.provider
if ($version -notmatch '^[0-9]+(?:\.[0-9]+){1,7}[A-Za-z0-9+.-]*$') {
    throw 'invalid payload version in provenance'
}
if ($provider -cne 'Flowseal/zapret-discord-youtube') { throw 'unexpected payload provider' }
if ($PayloadSchema -le 0 -or $StrategySchema -le 0 -or
    $PayloadSchema -gt 1000 -or $StrategySchema -gt 1000) {
    throw 'unsupported package schema value'
}

$sources = New-Object System.Collections.Generic.List[object]
Get-ChildItem -LiteralPath $payload -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($payload.TrimEnd('\').Length + 1).Replace('\','/')
    $sources.Add([pscustomobject]@{ Path=$relative; FullName=$_.FullName })
}
$sources.Add([pscustomobject]@{ Path='strategies/catalog.json'; FullName=$catalog })
$sources.Add([pscustomobject]@{ Path='provenance.json'; FullName=$provenancePath })
if ($sources.Count -gt 255) { throw 'package file-count limit exceeded' }

$runtimeFiles = @()
foreach ($source in $sources) {
    Assert-NoReparseComponents $source.FullName
    $item = Get-Item -LiteralPath $source.FullName
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "reparse input rejected: $($source.Path)" }
    $runtimeFiles += [ordered]@{
        path = $source.Path
        size = [long]$item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$runtimeManifest = [ordered]@{ schema=1; payload_version=$version; files=$runtimeFiles }
$runtimeBytes = [Text.UTF8Encoding]::new($false).GetBytes(
    ($runtimeManifest | ConvertTo-Json -Depth 6 -Compress) + "`n")
$sha256 = [Security.Cryptography.SHA256]::Create()
try { $runtimeDigest = $sha256.ComputeHash($runtimeBytes) } finally { $sha256.Dispose() }
$runtimeSha = [BitConverter]::ToString($runtimeDigest).Replace('-','').ToLowerInvariant()
$sources.Add([pscustomobject]@{
    Path='runtime-manifest.json'; FullName=$null; Bytes=$runtimeBytes
})

$offset = [long]0
$entries = @()
foreach ($source in $sources) {
    if ($source.Path -eq 'runtime-manifest.json') {
        $size = [long]$runtimeBytes.Length
        $sha = $runtimeSha
    } else {
        $item = Get-Item -LiteralPath $source.FullName
        $size = [long]$item.Length
        $sha = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($size -le 0 -or $size -gt 32MB) { throw "file size rejected: $($source.Path)" }
    $entries += [ordered]@{ path=$source.Path; type='file'; size=$size; sha256=$sha; offset=$offset }
    $offset += $size
}
if ($offset -gt 128MB) { throw 'unpacked package limit exceeded' }
$header = [ordered]@{
    schema=1; payload_version=$version; provider=$provider
    payload_schema=$PayloadSchema; strategy_schema=$StrategySchema; files=$entries
}
$headerBytes = [Text.UTF8Encoding]::new($false).GetBytes(
    ($header | ConvertTo-Json -Depth 8 -Compress))
if ($headerBytes.Length -gt 1MB) { throw 'package header limit exceeded' }
$parent = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
$temporary = Join-Path $parent ('.cbpkg-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
                          [IO.FileShare]::None)
try {
    $magic = [Text.Encoding]::ASCII.GetBytes("CBPKG1`r`n")
    $stream.Write($magic, 0, $magic.Length)
    $length = [BitConverter]::GetBytes([uint32]$headerBytes.Length)
    $stream.Write($length, 0, $length.Length)
    $stream.Write($headerBytes, 0, $headerBytes.Length)
    foreach ($source in $sources) {
        if ($source.Path -eq 'runtime-manifest.json') {
            $stream.Write($runtimeBytes, 0, $runtimeBytes.Length)
        } else {
            $input = [IO.File]::OpenRead($source.FullName)
            try { $input.CopyTo($stream) } finally { $input.Dispose() }
        }
    }
    $stream.Flush($true)
} finally { $stream.Dispose() }
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Force }
Move-Item -LiteralPath $temporary -Destination $output

# Re-open the final object and verify the serialized package itself, not just
# the input paths used above. This catches input replacement/read races and any
# offset/header drift before release automation signs the package SHA-256.
$verify = [IO.File]::Open($output, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                          [IO.FileShare]::Read)
try {
    $prefix = [byte[]]::new(12)
    if ($verify.Read($prefix, 0, $prefix.Length) -ne $prefix.Length -or
        [Text.Encoding]::ASCII.GetString($prefix, 0, 8) -cne "CBPKG1`r`n" -or
        [BitConverter]::ToUInt32($prefix, 8) -ne $headerBytes.Length) {
        throw 'final package prefix/header length verification failed'
    }
    $serializedHeader = [byte[]]::new($headerBytes.Length)
    $at = 0
    while ($at -lt $serializedHeader.Length) {
        $read = $verify.Read($serializedHeader, $at, $serializedHeader.Length - $at)
        if ($read -le 0) { throw 'final package header is truncated' }
        $at += $read
    }
    if ([Convert]::ToBase64String($headerBytes) -cne
        [Convert]::ToBase64String($serializedHeader)) {
        throw 'final package header bytes differ from generated header'
    }
    foreach ($entry in $entries) {
        $verify.Position = 12L + $headerBytes.Length + [long]$entry.offset
        $remaining = [long]$entry.size
        $digest = [Security.Cryptography.SHA256]::Create()
        try {
            $buffer = [byte[]]::new([Math]::Min(1MB, [int][Math]::Max(1, $remaining)))
            while ($remaining -gt 0) {
                $request = [int][Math]::Min([long]$buffer.Length, $remaining)
                $read = $verify.Read($buffer, 0, $request)
                if ($read -ne $request) { throw "final package entry truncated: $($entry.path)" }
                $null = $digest.TransformBlock($buffer, 0, $read, $null, 0)
                $remaining -= $read
            }
            $null = $digest.TransformFinalBlock([byte[]]::new(0), 0, 0)
            $actual = [BitConverter]::ToString($digest.Hash).Replace('-','').ToLowerInvariant()
            if ($actual -cne [string]$entry.sha256) {
                throw "final package entry SHA-256 mismatch: $($entry.path)"
            }
        } finally {
            $digest.Dispose()
        }
    }
    if ($verify.Position -ne $verify.Length) { throw 'final package has trailing data' }
} finally {
    $verify.Dispose()
}
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "UPDATE_PACKAGE: PASS version=$version files=$($sources.Count) bytes=$((Get-Item $output).Length) sha256=$hash"

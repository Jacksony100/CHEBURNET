<#
Verify that every imported payload/strategy byte is exactly the selected subset
of the immutable Flowseal release archive recorded in provenance.json.
#>
[CmdletBinding()]
param([string]$ArchivePath)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$provenancePath = Join-Path $root 'resources\upstream\provenance.json'
$provenance = Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($field in @('provider','version','tag','release_id','source_url','release_url',
                      'imported_at_utc','archive_sha256','upstream_commit','immutable')) {
    if ($null -eq $provenance.$field -or
        [string]::IsNullOrWhiteSpace([string]$provenance.$field)) {
        throw "provenance missing $field"
    }
}
$version = [string]$provenance.version
$expectedUrl = "https://github.com/Flowseal/zapret-discord-youtube/releases/download/$version/zapret-discord-youtube-$version.zip"
if ([string]$provenance.provider -cne 'Flowseal/zapret-discord-youtube' -or
    [string]$provenance.tag -cne $version -or $provenance.immutable -ne $true -or
    [string]$provenance.source_url -cne $expectedUrl -or
    [string]$provenance.archive_sha256 -notmatch '^[a-f0-9]{64}$') {
    throw 'upstream provenance contract rejected'
}

function Test-ReservedComponent([string]$component) {
    $base = $component.Split('.')[0].ToLowerInvariant()
    return $base -in @('con','prn','aux','nul') -or
        ($base -match '^(com|lpt)[1-9]$')
}

function Get-StreamSha256([IO.Stream]$stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-upstream-verify-' + [guid]::NewGuid().ToString('N'))
$archive = if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
    Join-Path $tempRoot 'upstream.zip'
} else {
    [IO.Path]::GetFullPath($ArchivePath)
}
try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
        $headers = @{ 'User-Agent' = 'CHEBURNET-upstream-fidelity/1.0' }
        Invoke-WebRequest -Uri $expectedUrl -Headers $headers -OutFile $archive -UseBasicParsing
    } elseif (-not (Test-Path -LiteralPath $archive -PathType Leaf) -or
              ((Get-Item -LiteralPath $archive -Force).Attributes -band
               [IO.FileAttributes]::ReparsePoint)) {
        throw 'supplied upstream archive is missing or a reparse object'
    }
    $actualArchiveSha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualArchiveSha -cne [string]$provenance.archive_sha256) {
        throw 'upstream archive SHA-256 differs from provenance'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        if ($zip.Entries.Count -eq 0 -or $zip.Entries.Count -gt 4096) {
            throw 'upstream archive entry-count limit exceeded'
        }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $top = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        [long]$unpacked = 0
        foreach ($entry in $zip.Entries) {
            $name = [string]$entry.FullName
            if ([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt 500 -or
                $name.Contains('\') -or $name.StartsWith('/') -or $name.Contains(':') -or
                $name.IndexOfAny([char[]]"`0`r`n") -ge 0) {
                throw "unsafe upstream ZIP path: $name"
            }
            $trimmed = $name.TrimEnd('/')
            if ([string]::IsNullOrWhiteSpace($trimmed)) { throw 'empty upstream ZIP path' }
            $parts = @($trimmed.Split('/'))
            foreach ($part in $parts) {
                if ([string]::IsNullOrWhiteSpace($part) -or $part -eq '.' -or $part -eq '..' -or
                    $part.Length -gt 255 -or $part.EndsWith(' ') -or $part.EndsWith('.') -or
                    $part -match '[<>"|?*]' -or (Test-ReservedComponent $part)) {
                    throw "unsafe upstream ZIP component: $name"
                }
            }
            if (-not $seen.Add($trimmed)) { throw "duplicate upstream ZIP path: $name" }
            $null = $top.Add($parts[0])
            $unixType = ([int64]$entry.ExternalAttributes -shr 16) -band 0xF000
            $windowsAttributes = [int64]$entry.ExternalAttributes -band 0xFFFF
            $isDirectory = $name.EndsWith('/')
            if (($windowsAttributes -band [int][IO.FileAttributes]::ReparsePoint) -ne 0 -or
                ($isDirectory -and $unixType -notin @(0, 0x4000)) -or
                (-not $isDirectory -and $unixType -notin @(0, 0x8000))) {
                throw "unsupported object type in upstream ZIP: $name"
            }
            if ($entry.Length -lt 0 -or $entry.Length -gt 128MB -or
                $unpacked -gt 512MB - $entry.Length) {
                throw "upstream ZIP unpacked-size limit exceeded: $name"
            }
            $unpacked += $entry.Length
        }
        if ($top.Count -ne 1) { throw 'archive must have exactly one top-level directory' }
        $prefix = @($top)[0]

        $archiveFiles = [Collections.Generic.Dictionary[string,object]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $trimmed = ([string]$entry.FullName).TrimEnd('/')
            if ($entry.FullName.EndsWith('/') -or $trimmed -eq $prefix) { continue }
            if (-not $trimmed.StartsWith($prefix + '/', [StringComparison]::OrdinalIgnoreCase)) {
                throw 'archive entry escaped its top-level directory'
            }
            $relative = $trimmed.Substring($prefix.Length + 1)
            $localRelative = $null
            if ($relative -match '^(bin|lists)/(.+)$') {
                if ($Matches[2].Contains('/')) {
                    throw "nested embedded payload layout is unsupported: $relative"
                }
                if ($Matches[1] -ieq 'lists' -and $Matches[2].EndsWith('.backup', [StringComparison]::OrdinalIgnoreCase)) {
                    continue
                }
                $localRelative = 'resources/payload/' + $relative
            } elseif (-not $relative.Contains('/') -and $relative -match '^general.*\.bat$') {
                $localRelative = 'resources/strategies_src/' + $relative
            } else {
                continue
            }
            if ($archiveFiles.ContainsKey($localRelative)) {
                throw "duplicate selected upstream file: $relative"
            }
            $archiveFiles.Add($localRelative, $entry)
        }

        $localFiles = [Collections.Generic.Dictionary[string,object]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        foreach ($directory in @('resources\payload\bin','resources\payload\lists',
                                  'resources\strategies_src')) {
            $fullDir = Join-Path $root $directory
            foreach ($item in @(Get-ChildItem -LiteralPath $fullDir -Force)) {
                if ($item.PSIsContainer -or
                    (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                    throw "local upstream input is a directory/reparse object: $($item.FullName)"
                }
                $localRelative = $item.FullName.Substring($root.Length + 1).Replace('\','/')
                if ($localFiles.ContainsKey($localRelative)) {
                    throw "duplicate local upstream input: $localRelative"
                }
                $localFiles.Add($localRelative, $item)
            }
        }
        if ($archiveFiles.Count -ne $localFiles.Count -or $archiveFiles.Count -eq 0) {
            throw "selected upstream/local file-count mismatch: archive=$($archiveFiles.Count) local=$($localFiles.Count)"
        }
        $strategyCount = 0
        foreach ($pair in $archiveFiles.GetEnumerator()) {
            if (-not $localFiles.ContainsKey($pair.Key)) { throw "local import missing $($pair.Key)" }
            $local = $localFiles[$pair.Key]
            $entry = $pair.Value
            if ([long]$local.Length -ne [long]$entry.Length) {
                throw "upstream size mismatch: $($pair.Key)"
            }
            $stream = $entry.Open()
            try { $archiveSha = Get-StreamSha256 $stream } finally { $stream.Dispose() }
            $localSha = (Get-FileHash -LiteralPath $local.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($archiveSha -cne $localSha) { throw "upstream byte mismatch: $($pair.Key)" }
            if ($pair.Key.StartsWith('resources/strategies_src/', [StringComparison]::OrdinalIgnoreCase)) {
                $strategyCount++
            }
        }
        Write-Output "UPSTREAM_FIDELITY: PASS version=$version files=$($archiveFiles.Count) strategies=$strategyCount archive_sha256=$actualArchiveSha"
    } finally {
        $zip.Dispose()
    }
} finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

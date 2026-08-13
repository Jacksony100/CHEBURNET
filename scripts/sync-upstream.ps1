<#
Transactional importer for immutable Flowseal stable releases. Network and
parsing happen in an isolated temp tree; production files are swapped only
after archive digest, layout, strategy generation and resource generation pass.
#>
[CmdletBinding()]
param([string]$Version, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$headers = @{
    'User-Agent' = 'CHEBURNET-upstream-sync/1.0'
    'Accept' = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
$githubToken = [Environment]::GetEnvironmentVariable('GITHUB_TOKEN')
if (-not [string]::IsNullOrWhiteSpace($githubToken)) {
    $headers['Authorization'] = "Bearer $githubToken"
}
$api = if ([string]::IsNullOrWhiteSpace($Version)) {
    'https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/latest'
} else {
    'https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/tags/' +
        [Uri]::EscapeDataString($Version)
}
$release = Invoke-RestMethod -Uri $api -Headers $headers
if ($release.draft -or $release.prerelease) { throw 'stable import rejects draft/prerelease' }
if ($release.immutable -ne $true) { throw 'release is not immutable' }
$versionString = [string]$release.tag_name
if ($versionString -notmatch '^[0-9]+\.[0-9]+\.[0-9]+[a-z]?$') {
    throw "unsupported upstream version syntax: $versionString"
}
$asset = @($release.assets | Where-Object name -eq "zapret-discord-youtube-$versionString.zip")
if ($asset.Count -ne 1) { throw 'exact upstream ZIP release asset not found' }
if ([string]$asset[0].digest -notmatch '^sha256:([a-f0-9]{64})$') {
    throw 'GitHub release asset has no SHA-256 digest'
}
$expectedSha = $Matches[1]
if ([long]$asset[0].size -le 0 -or [long]$asset[0].size -gt 256MB) {
    throw 'upstream release asset size is outside import limits'
}

$provenancePath = Join-Path $root 'resources\upstream\provenance.json'
if (Test-Path -LiteralPath $provenancePath) {
    $current = Get-Content -LiteralPath $provenancePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($current.version -eq $versionString -and $current.archive_sha256 -eq $expectedSha) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify-upstream.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'current embedded upstream fidelity failed' }
        Write-Output "UPSTREAM_IMPORT: already current ($versionString)"
        exit 0
    }
}
if ($CheckOnly) {
    Write-Output "UPSTREAM_UPDATE_AVAILABLE: $versionString sha256:$expectedSha"
    exit 0
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-sync-' + [guid]::NewGuid().ToString('N'))
$archive = Join-Path $tempRoot 'upstream.zip'
$extract = Join-Path $tempRoot 'extract'
$candidate = Join-Path $tempRoot 'candidate'
try {
    New-Item -ItemType Directory -Path $extract,$candidate | Out-Null
    Invoke-WebRequest -Uri $asset[0].browser_download_url -Headers $headers -OutFile $archive -UseBasicParsing
    $actualSha = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha -cne $expectedSha) { throw 'release archive SHA-256 mismatch' }

    # Expand-Archive is never the first parser of an untrusted ZIP. Validate
    # every central-directory entry, normalized path, object type and unpacked
    # size before any filesystem materialization.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        if ($zip.Entries.Count -eq 0 -or $zip.Entries.Count -gt 4096) {
            throw 'upstream archive entry-count limit exceeded'
        }
        $seen = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        [long]$unpacked = 0
        foreach ($entry in $zip.Entries) {
            $name = [string]$entry.FullName
            if ([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt 500 -or
                $name.Contains('\') -or $name.StartsWith('/') -or $name.StartsWith('//') -or
                $name -match '^[A-Za-z]:' -or $name.Contains(':') -or
                $name.IndexOfAny([char[]]"`0`r`n") -ge 0) {
                throw "unsafe upstream ZIP path: $name"
            }
            $trimmed = $name.TrimEnd('/')
            if ([string]::IsNullOrWhiteSpace($trimmed)) { throw 'empty upstream ZIP path' }
            $parts = @($trimmed.Split('/'))
            if ($parts | Where-Object {
                    $base = $_.Split('.')[0].ToLowerInvariant()
                    $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' -or $_.Length -gt 255 -or
                    $_.EndsWith(' ') -or $_.EndsWith('.') -or $_ -match '[<>"|?*]' -or
                    $base -in @('con','prn','aux','nul') -or $base -match '^(com|lpt)[1-9]$'
                }) {
                throw "unsafe upstream ZIP component: $name"
            }
            if (-not $seen.Add($trimmed)) { throw "duplicate upstream ZIP path: $name" }
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
    } finally {
        $zip.Dispose()
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $extract
    $reparse = @(Get-ChildItem -LiteralPath $extract -Force -Recurse | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    })
    if ($reparse.Count -ne 0) { throw 'reparse object appeared after ZIP extraction' }
    $topLevel = @(Get-ChildItem -LiteralPath $extract -Force)
    if ($topLevel.Count -ne 1 -or -not $topLevel[0].PSIsContainer) {
        throw 'archive must have exactly one top-level directory'
    }
    $source = $topLevel[0].FullName

    foreach ($required in @('bin\winws.exe','bin\WinDivert.dll','bin\WinDivert64.sys',
                             'bin\cygwin1.dll','lists\list-general.txt','general.bat')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
            throw "upstream archive missing $required"
        }
    }
    $payloadCandidate = Join-Path $candidate 'payload'
    $strategyCandidate = Join-Path $candidate 'strategies_src'
    New-Item -ItemType Directory -Path $payloadCandidate,$strategyCandidate | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'bin') -Destination (Join-Path $payloadCandidate 'bin') -Recurse
    Copy-Item -LiteralPath (Join-Path $source 'lists') -Destination (Join-Path $payloadCandidate 'lists') -Recurse
    # .backup is upstream maintenance state, not active runtime data.
    Get-ChildItem -LiteralPath (Join-Path $payloadCandidate 'lists') -Filter '*.backup' |
        Remove-Item -Force
    Get-ChildItem -LiteralPath $source -File -Filter 'general*.bat' |
        Copy-Item -Destination $strategyCandidate

    $generated = Join-Path $candidate 'generated'
    New-Item -ItemType Directory -Path $generated | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'cmake\gen_strategies.ps1') `
        -BatDir $strategyCandidate -HeaderOut (Join-Path $generated 'GeneratedStrategies.h') `
        -CatalogOut (Join-Path $generated 'strategy-catalog.json')
    if ($LASTEXITCODE -ne 0) { throw 'candidate strategy generation failed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'cmake\gen_resources.ps1') `
        -PayloadDir $payloadCandidate -RcOut (Join-Path $generated 'payload.rc') `
        -HeaderOut (Join-Path $generated 'GeneratedManifest.h')
    if ($LASTEXITCODE -ne 0) { throw 'candidate resource generation failed' }

    $tagRef = Invoke-RestMethod -Uri (
        'https://api.github.com/repos/Flowseal/zapret-discord-youtube/git/ref/tags/' +
        [Uri]::EscapeDataString($versionString)) -Headers $headers
    $tagObject = $tagRef.object
    if ($tagObject.type -eq 'tag') { $tagObject = (Invoke-RestMethod -Uri $tagObject.url -Headers $headers).object }
    if ($tagObject.type -ne 'commit' -or [string]$tagObject.sha -notmatch '^[a-f0-9]{40}$') {
        throw 'tag does not resolve to an immutable commit'
    }
    $provenance = [ordered]@{
        provider = 'Flowseal/zapret-discord-youtube'
        version = $versionString
        tag = $versionString
        release_id = [long]$release.id
        source_url = [string]$asset[0].browser_download_url
        release_url = [string]$release.html_url
        imported_at_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        archive_sha256 = $expectedSha
        upstream_commit = [string]$tagObject.sha
        immutable = $true
    }
    $candidateProvenance = Join-Path $candidate 'provenance.json'
    [IO.File]::WriteAllText($candidateProvenance, ($provenance | ConvertTo-Json) + "`n",
                            [Text.UTF8Encoding]::new($false))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'cmake\gen_provenance.ps1') `
        -Provenance $candidateProvenance -HeaderOut (Join-Path $generated 'GeneratedProvenance.h')
    if ($LASTEXITCODE -ne 0) { throw 'candidate provenance validation failed' }

    # Compile and independently compare all candidate strategies in every
    # GameFilter mode before production files can be replaced.
    $validationRoot = Join-Path $candidate 'validation-repo'
    New-Item -ItemType Directory -Path $validationRoot | Out-Null
    foreach ($name in @('CMakeLists.txt','cmake','src','tests','scripts','resources','LICENSES',
                         'LICENSE','THIRD_PARTY_NOTICES.md','README.md')) {
        $sourceItem = Join-Path $root $name
        if (Test-Path -LiteralPath $sourceItem) {
            Copy-Item -LiteralPath $sourceItem -Destination (Join-Path $validationRoot $name) -Recurse
        }
    }
    Remove-Item -LiteralPath (Join-Path $validationRoot 'resources\payload') -Recurse -Force
    Remove-Item -LiteralPath (Join-Path $validationRoot 'resources\strategies_src') -Recurse -Force
    Move-Item -LiteralPath $payloadCandidate -Destination (Join-Path $validationRoot 'resources\payload')
    Move-Item -LiteralPath $strategyCandidate -Destination (Join-Path $validationRoot 'resources\strategies_src')
    Copy-Item -LiteralPath $candidateProvenance -Destination (Join-Path $validationRoot 'resources\upstream\provenance.json') -Force
    & (Join-Path $validationRoot 'scripts\build-release.ps1') -BuildDir 'build-import-check'
    if ($LASTEXITCODE -ne 0) { throw 'candidate clean build failed' }
    & (Join-Path $validationRoot 'scripts\run-tests.ps1') -BuildDir 'build-import-check'
    if ($LASTEXITCODE -ne 0) { throw 'candidate tests/fidelity failed' }
    Move-Item -LiteralPath (Join-Path $validationRoot 'resources\payload') -Destination $payloadCandidate
    Move-Item -LiteralPath (Join-Path $validationRoot 'resources\strategies_src') -Destination $strategyCandidate

    # Production swap only after every isolated validation succeeded.
    $payloadTarget = Join-Path $root 'resources\payload'
    $strategyTarget = Join-Path $root 'resources\strategies_src'
    $backupPayload = Join-Path $tempRoot 'old-payload'
    $backupStrategies = Join-Path $tempRoot 'old-strategies'
    $backupProvenance = Join-Path $tempRoot 'old-provenance.json'
    Copy-Item -LiteralPath $provenancePath -Destination $backupProvenance
    $payloadBackedUp = $false
    $strategiesBackedUp = $false
    $payloadInstalled = $false
    $strategiesInstalled = $false
    try {
        Move-Item -LiteralPath $payloadTarget -Destination $backupPayload
        $payloadBackedUp = $true
        Move-Item -LiteralPath $strategyTarget -Destination $backupStrategies
        $strategiesBackedUp = $true
        Move-Item -LiteralPath $payloadCandidate -Destination $payloadTarget
        $payloadInstalled = $true
        Move-Item -LiteralPath $strategyCandidate -Destination $strategyTarget
        $strategiesInstalled = $true
        Copy-Item -LiteralPath $candidateProvenance -Destination $provenancePath -Force
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $PSScriptRoot 'verify-upstream.ps1') -ArchivePath $archive
        if ($LASTEXITCODE -ne 0) { throw 'installed upstream byte fidelity failed' }
    } catch {
        if ($payloadInstalled -and (Test-Path -LiteralPath $payloadTarget)) {
            Remove-Item -LiteralPath $payloadTarget -Recurse -Force
        }
        if ($strategiesInstalled -and (Test-Path -LiteralPath $strategyTarget)) {
            Remove-Item -LiteralPath $strategyTarget -Recurse -Force
        }
        if ($payloadBackedUp -and (Test-Path -LiteralPath $backupPayload)) {
            Move-Item -LiteralPath $backupPayload -Destination $payloadTarget
        }
        if ($strategiesBackedUp -and (Test-Path -LiteralPath $backupStrategies)) {
            Move-Item -LiteralPath $backupStrategies -Destination $strategyTarget
        }
        Copy-Item -LiteralPath $backupProvenance -Destination $provenancePath -Force
        throw
    }
    Write-Output "UPSTREAM_IMPORT: PASS version=$versionString sha256=$expectedSha"
} finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

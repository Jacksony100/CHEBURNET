<#
Machine-readable report about the embedded upstream payload versus the latest
immutable stable Flowseal release.

Read-only with respect to the repository: it queries the GitHub API and reads
resources/upstream/provenance.json. Importing is a separate, explicit step
(scripts/sync-upstream.ps1).

Emits JSON for automation and Markdown for the durable task body. Exit code is
0 whether or not an update exists; the caller branches on report.status:

  current    the embedded payload already is the latest stable release
  available  a newer immutable stable release exists
  error      the upstream state could not be determined

-ImportedRoot points at a checkout where scripts/sync-upstream.ps1 has already
run, so the report can also carry the resulting strategy/payload delta and the
validation outcome.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [string]$Root,
    [string]$Version,
    [string]$JsonOut,
    [string]$MarkdownOut,
    [string]$ImportedRoot,
    [ValidateSet('unknown', 'passed', 'failed', 'skipped')][string]$Validation = 'unknown',
    [string]$ValidationDetail = '',
    [string]$RunUrl = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot\upstream-delta.ps1"

if ([string]::IsNullOrWhiteSpace($Root)) { $Root = Split-Path -Parent $PSScriptRoot }
$root = [IO.Path]::GetFullPath($Root).TrimEnd('\')

$headers = @{
    'User-Agent'           = 'CHEBURNET-upstream-report/1.0'
    'Accept'               = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
$githubToken = [Environment]::GetEnvironmentVariable('GITHUB_TOKEN')
if (-not [string]::IsNullOrWhiteSpace($githubToken)) {
    $headers['Authorization'] = "Bearer $githubToken"
}

$provenance = Get-Content -LiteralPath (Join-Path $root 'resources\upstream\provenance.json') `
    -Raw -Encoding UTF8 | ConvertFrom-Json
$currentVersion = [string]$provenance.version

$report = [ordered]@{
    schema           = 1
    generated_at     = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    provider         = 'Flowseal/zapret-discord-youtube'
    status           = 'error'
    current_version  = $currentVersion
    current_archive_sha256 = [string]$provenance.archive_sha256
    current_commit   = [string]$provenance.upstream_commit
    latest_version   = ''
    latest_release_url = ''
    latest_archive_url = ''
    latest_archive_sha256 = ''
    latest_archive_size = 0
    latest_commit    = ''
    strategies_added = @()
    strategies_removed = @()
    strategies_modified = @()
    payload_added    = @()
    payload_removed  = @()
    payload_modified = @()
    strategy_count_before = 0
    strategy_count_after  = 0
    validation       = $Validation
    validation_detail = $ValidationDetail
    run_url          = $RunUrl
    error            = ''
}

try {
    $api = if ([string]::IsNullOrWhiteSpace($Version)) {
        'https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/latest'
    } else {
        'https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/tags/' +
            [Uri]::EscapeDataString($Version)
    }
    $release = Invoke-RestMethod -Uri $api -Headers $headers
    if ($release.draft -or $release.prerelease) {
        throw 'latest release is a draft or prerelease and is not an import candidate'
    }
    if ($release.immutable -ne $true) { throw 'latest release is not immutable' }
    $latestVersion = [string]$release.tag_name
    if ($latestVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+[a-z]?$') {
        throw "unsupported upstream version syntax: $latestVersion"
    }
    $asset = @($release.assets | Where-Object name -eq "zapret-discord-youtube-$latestVersion.zip")
    if ($asset.Count -ne 1) { throw 'exact upstream ZIP release asset not found' }
    if ([string]$asset[0].digest -cnotmatch '^sha256:([a-f0-9]{64})$') {
        throw 'release asset has no SHA-256 digest'
    }
    $report.latest_version = $latestVersion
    $report.latest_release_url = [string]$release.html_url
    $report.latest_archive_url = [string]$asset[0].browser_download_url
    $report.latest_archive_sha256 = $Matches[1]
    $report.latest_archive_size = [long]$asset[0].size

    $tagRef = Invoke-RestMethod -Uri (
        'https://api.github.com/repos/Flowseal/zapret-discord-youtube/git/ref/tags/' +
        [Uri]::EscapeDataString($latestVersion)) -Headers $headers
    $tagObject = $tagRef.object
    if ($tagObject.type -eq 'tag') {
        $tagObject = (Invoke-RestMethod -Uri $tagObject.url -Headers $headers).object
    }
    if ($tagObject.type -ceq 'commit' -and [string]$tagObject.sha -cmatch '^[a-f0-9]{40}$') {
        $report.latest_commit = [string]$tagObject.sha
    }

    $report.status = if ($latestVersion -ceq $currentVersion -and
                         $report.latest_archive_sha256 -ceq [string]$provenance.archive_sha256) {
        'current'
    } else {
        'available'
    }
} catch {
    $report.status = 'error'
    $report.error = [string]$_.Exception.Message
}

# Delta after an actual import, taken from git rather than guessed.
if (-not [string]::IsNullOrWhiteSpace($ImportedRoot)) {
    $importedRootFull = [IO.Path]::GetFullPath($ImportedRoot).TrimEnd('\\')
    Push-Location $importedRootFull
    try {
        $porcelain = @(& git status --porcelain -- 'resources/strategies_src' 'resources/payload')
        $after = @(Get-ChildItem -LiteralPath `
            (Join-Path $importedRootFull 'resources\strategies_src') -File -Filter '*.bat').Count
        $delta = Get-CheburnetPayloadDelta -PorcelainLines $porcelain -StrategyCountAfter $after
        $report.strategies_added = $delta.StrategiesAdded
        $report.strategies_removed = $delta.StrategiesRemoved
        $report.strategies_modified = $delta.StrategiesModified
        $report.payload_added = $delta.PayloadAdded
        $report.payload_removed = $delta.PayloadRemoved
        $report.payload_modified = $delta.PayloadModified
        $report.strategy_count_after = $delta.StrategyCountAfter
        $report.strategy_count_before = $delta.StrategyCountBefore
    } finally {
        Pop-Location
    }
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($JsonOut))
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($JsonOut),
        ($report | ConvertTo-Json -Depth 6) + "`n", [Text.UTF8Encoding]::new($false))
}

function Format-NameList($Values) {
    $items = @($Values)
    if ($items.Count -eq 0) { return '-' }
    return ($items | ForEach-Object { '`' + $_ + '`' }) -join ', '
}

$lines = New-Object Collections.Generic.List[string]
if ($report.status -eq 'available') {
    $lines.Add("## Upstream Flowseal $($report.latest_version) is available")
} elseif ($report.status -eq 'current') {
    $lines.Add('## Upstream payload is current')
} else {
    $lines.Add('## Upstream check failed')
}
$lines.Add('')
$lines.Add('| Field | Value |')
$lines.Add('| --- | --- |')
$lines.Add("| Provider | ``$($report.provider)`` |")
$lines.Add("| Embedded version | ``$($report.current_version)`` |")
$lines.Add("| Latest stable version | ``$($report.latest_version)`` |")
$lines.Add("| Release | $($report.latest_release_url) |")
$lines.Add("| Archive SHA-256 | ``$($report.latest_archive_sha256)`` |")
$lines.Add("| Archive size | $($report.latest_archive_size) bytes |")
$lines.Add("| Upstream commit | ``$($report.latest_commit)`` |")
$lines.Add("| Strategies | $($report.strategy_count_before) -> $($report.strategy_count_after) |")
$lines.Add("| Validation | **$($report.validation)** |")
if (-not [string]::IsNullOrWhiteSpace($report.validation_detail)) {
    $lines.Add("| Validation detail | $($report.validation_detail) |")
}
if (-not [string]::IsNullOrWhiteSpace($report.run_url)) {
    $lines.Add("| Workflow run | $($report.run_url) |")
}
if (-not [string]::IsNullOrWhiteSpace($report.error)) {
    $lines.Add("| Error | $($report.error) |")
}
$lines.Add('')
if ($report.status -eq 'available') {
    $lines.Add('### Payload delta')
    $lines.Add('')
    $lines.Add('| Change | Strategies | Payload files |')
    $lines.Add('| --- | --- | --- |')
    $lines.Add("| Added | $(Format-NameList $report.strategies_added) | $(Format-NameList $report.payload_added) |")
    $lines.Add("| Removed | $(Format-NameList $report.strategies_removed) | $(Format-NameList $report.payload_removed) |")
    $lines.Add("| Modified | $(Format-NameList $report.strategies_modified) | $(Format-NameList $report.payload_modified) |")
    $lines.Add('')
    $lines.Add('### How to import')
    $lines.Add('')
    $lines.Add('The importer is deterministic and verifies the archive digest against the')
    $lines.Add('immutable release asset, so a local run reproduces exactly what CI validated:')
    $lines.Add('')
    $lines.Add('```powershell')
    $lines.Add("scripts\sync-upstream.ps1 -Version $($report.latest_version)")
    $lines.Add('scripts\build-release.ps1 -BuildDir build-upstream')
    $lines.Add('scripts\run-tests.ps1 -BuildDir build-upstream')
    $lines.Add('```')
    $lines.Add('')
    $lines.Add('Nothing is merged automatically. Review the payload delta before committing.')
}
$markdown = ($lines -join "`n") + "`n"

if (-not [string]::IsNullOrWhiteSpace($MarkdownOut)) {
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($MarkdownOut))
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($MarkdownOut), $markdown,
        [Text.UTF8Encoding]::new($false))
}

Write-Output ("UPSTREAM_REPORT: status=$($report.status) current=$($report.current_version) " +
              "latest=$($report.latest_version) validation=$($report.validation)")

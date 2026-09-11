<#
Upstream automation regression test.

Covers the parts that can be verified without touching the network:
  * the payload-delta parser (quoted paths, renames, deletions, additions);
  * the workflow contract that turns a new upstream release into one durable,
    deduplicated task and never merges anything automatically;
  * least-privilege permissions on the scheduled workflow.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
. (Join-Path $root 'scripts\upstream-delta.ps1')

$failures = New-Object Collections.Generic.List[string]
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $script:failures.Add($Message) }
}
function Assert-Set($Actual, [string[]]$Expected, [string]$Message) {
    $left = @($Actual) | Sort-Object
    $right = @($Expected) | Sort-Object
    if (($left -join '|') -cne ($right -join '|')) {
        $script:failures.Add("$Message (got: $($left -join ', '))")
    }
}

# ---- delta parser ----------------------------------------------------------
$porcelain = @(
    ' D resources/payload/bin/quic_initial_dbankcloud_ru.bin',
    '?? resources/payload/bin/tls_clienthello_sochi_park.bin',
    ' M resources/payload/lists/list-general.txt',
    ' M resources/payload/lists/list-exclude.txt',
    '?? "resources/strategies_src/general (ALT13).bat"',
    ' M "resources/strategies_src/general (EXP).bat"',
    'R  resources/payload/bin/old_name.bin -> resources/payload/bin/new_name.bin',
    ' M docs/ARCHITECTURE.md',
    '',
    'xx'
)
$delta = Get-CheburnetPayloadDelta -PorcelainLines $porcelain -StrategyCountAfter 22
Assert-Set $delta.StrategiesAdded @('general (ALT13).bat') 'ALT13 must be reported as an added strategy'
Assert-Set $delta.StrategiesModified @('general (EXP).bat') 'EXP must be reported as a modified strategy'
Assert-Set $delta.StrategiesRemoved @() 'no strategy was removed in the fixture'
Assert-Set $delta.PayloadRemoved @('quic_initial_dbankcloud_ru.bin') 'removed payload file must be reported'
Assert-Set $delta.PayloadAdded @('tls_clienthello_sochi_park.bin', 'new_name.bin') `
    'added and renamed payload files must be reported as added'
Assert-Set $delta.PayloadModified @('list-general.txt', 'list-exclude.txt') `
    'modified list files must be reported'
Assert-True ($delta.StrategyCountAfter -eq 22) 'strategy count after must be preserved'
Assert-True ($delta.StrategyCountBefore -eq 21) 'strategy count before must be derived from the delta'

# Paths outside the payload must be ignored entirely.
Assert-True (@($delta.PayloadModified) -notcontains 'ARCHITECTURE.md') `
    'unrelated repository files must not appear in the payload delta'

$empty = Get-CheburnetPayloadDelta -PorcelainLines @() -StrategyCountAfter 22
Assert-True (@($empty.StrategiesAdded).Count -eq 0 -and $empty.StrategyCountBefore -eq 22) `
    'an empty delta must report no change'

# A removal must raise the "before" count above the "after" count.
$removal = Get-CheburnetPayloadDelta -PorcelainLines @(
    ' D "resources/strategies_src/general (ALT12).bat"') -StrategyCountAfter 21
Assert-True ($removal.StrategyCountBefore -eq 22) 'a removed strategy must raise the before count'

# ---- report script contract ------------------------------------------------
$report = Get-Content -LiteralPath (Join-Path $root 'scripts\upstream-report.ps1') `
    -Raw -Encoding UTF8
foreach ($field in @('current_version', 'latest_version', 'latest_release_url',
                     'latest_archive_sha256', 'latest_commit', 'strategy_count_before',
                     'strategy_count_after', 'validation', 'run_url')) {
    Assert-True ($report -match [regex]::Escape($field)) `
        "the machine-readable report must carry the field '$field'"
}
Assert-True ($report -match "if \(\`$release\.draft -or \`$release\.prerelease\)") `
    'the report must refuse draft and prerelease upstream releases'
Assert-True ($report -match "immutable -ne \`$true") `
    'the report must require an immutable upstream release'

# ---- scheduled workflow contract -------------------------------------------
$workflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\upstream-check.yml') `
    -Raw -Encoding UTF8
Assert-True ($workflow -match '(?m)^permissions:\s*$') 'the workflow must declare permissions'
Assert-True ($workflow -match '(?m)^\s*contents:\s*read\s*$') `
    'the workflow default permission must be contents: read'
Assert-True ($workflow -notmatch '(?m)^\s*contents:\s*write\s*$') `
    'the scheduled upstream workflow must never take write access to repository contents'
Assert-True ($workflow -match 'issues:\s*write') `
    'the workflow must request issues: write to create the durable task'
Assert-True ($workflow -notmatch 'pull-requests:\s*write') `
    'the workflow must not request pull-request write access it does not use'
Assert-True ($workflow -match 'sync-upstream\.ps1 -Version') `
    'the workflow must run the secure importer for the candidate version'
Assert-True ($workflow -match 'upstream-report\.ps1') `
    'the workflow must produce the machine-readable report'
Assert-True ($workflow -match 'cheburnet-upstream:') `
    'the workflow must use a marker to deduplicate the durable task'
Assert-True ($workflow -notmatch '(?i)(gh pr merge|--auto|--admin|--merge)') `
    'the workflow must never merge anything automatically'

# Every third-party action must stay pinned to an immutable commit SHA.
foreach ($use in @([regex]::Matches($workflow, '(?m)uses:\s*(\S+)'))) {
    $reference = $use.Groups[1].Value
    if ($reference.StartsWith('./')) { continue }
    Assert-True ($reference -match '@[0-9a-f]{40}$') `
        "third-party action must be pinned to a commit SHA: $reference"
}

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    throw "UPSTREAM_AUTOMATION: FAIL ($($failures.Count))"
}
Write-Output 'UPSTREAM_AUTOMATION: PASS (delta parser, report contract, workflow policy)'

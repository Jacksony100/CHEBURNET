# Validate the four real guest reports against one candidate. Never synthesizes evidence.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReportsDir,
    [Parameter(Mandatory = $true)][string]$BundleManifest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$manifest = Get-Content -LiteralPath $BundleManifest -Raw -Encoding UTF8 | ConvertFrom-Json
$reports = @(Get-ChildItem -LiteralPath $ReportsDir -Filter 'e2e-report.json' -File -Recurse)
if ($reports.Count -ne 4) { throw "exactly four reports are required (two roles per OS); found $($reports.Count)" }
$seen = @{}
foreach ($file in $reports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    $build = [int]$report.machine.build
    if ($report.schema -ne 1 -or $report.environment_kind -cne 'disposable-vm' -or
        $report.machine.product_type -ne 1 -or $build -lt 10240 -or
        $report.machine.architecture -cne 'AMD64' -or
        $report.launcher_sha256 -cne $manifest.launcher_sha256) {
        throw "report OS, architecture or candidate mismatch: $($file.FullName)"
    }
    $edition = if ($build -ge 22000) { 'win11' } else { 'win10' }
    $role = [string]$report.token_role
    $required = @(switch -CaseSensitive ($role) {
        'elevated' { @('fresh-install', 'existing-install', 'interrupted-pending', 'startup-rollback', 'filesystem-guards') }
        'standard-user' { @('refuses-unelevated') }
        default { throw "unexpected token role: $role" }
    })
    $key = "$edition/$role"
    if ($seen.ContainsKey($key)) { throw "duplicate OS/role report: $key" }
    $seen[$key] = $true
    if ($report.failed -ne 0 -or $report.skipped -ne 0 -or $report.passed -ne $required.Count -or
        @($report.results).Count -ne $required.Count -or
        @($report.required_scenarios).Count -ne $required.Count -or
        @(Compare-Object @($report.required_scenarios | Sort-Object) @($required | Sort-Object)).Count -ne 0) {
        throw "incomplete or failed gate: $key"
    }
    foreach ($scenario in $required) {
        $record = @($report.results | Where-Object scenario -ceq $scenario)
        if ($record.Count -ne 1 -or $record[0].outcome -cne 'passed') {
            throw "required scenario did not pass: $key/$scenario"
        }
    }
}
foreach ($key in @('win10/elevated', 'win10/standard-user', 'win11/elevated', 'win11/standard-user')) {
    if (-not $seen.ContainsKey($key)) { throw "missing report: $key" }
}
Write-Output 'E2E_MATRIX: PASS (Windows 10/11 x64, both token roles, 12 scenario results, same candidate)'

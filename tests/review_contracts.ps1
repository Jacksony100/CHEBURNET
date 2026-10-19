# Run real script entry points against an isolated fixture and a harmless probe.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$Probe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-contracts-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    # A disposable Git index also works inside the importer's non-Git validation tree.
    $gitFixture = Join-Path $fixture 'repository'
    New-Item -ItemType Directory -Path (Join-Path $gitFixture 'resources') -Force | Out-Null
    & git -C $gitFixture init -q
    if ($LASTEXITCODE -ne 0) { throw 'cannot create fixture repository' }
    $payload = Join-Path $gitFixture 'resources\fixture.txt'
    [IO.File]::WriteAllText($payload, 'before')
    & git -C $gitFixture add -f -- resources/fixture.txt
    if ($LASTEXITCODE -ne 0) { throw 'cannot stage fixture' }
    [IO.File]::WriteAllText($payload, 'after')
    # The writer creates its own previously absent parent before its first write.
    $diffDir = Join-Path $fixture 'absent\upstream'
    & (Join-Path $RepositoryRoot 'scripts\write-upstream-diff.ps1') -Root $gitFixture -OutDir $diffDir
    foreach ($name in @('payload-diff.txt', 'payload-status.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $diffDir $name))) { throw "missing $name" }
    }
    if ((Get-Content -LiteralPath (Join-Path $diffDir 'payload-diff.txt') -Raw) -notmatch 'fixture.txt') {
        throw 'diff writer must report the actual changed resource'
    }
    $e2e = Join-Path $RepositoryRoot 'scripts\e2e\Invoke-CheburnetE2E.ps1'
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($e2e, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw 'E2E script must parse' }
    $functions = @('Start-CheburnetAndSettle', 'Test-InstallProbeSuccess', 'Test-InstallProbeRefusal',
                   'Read-CheburnetRuntimeState', 'Get-E2EGateExitCode')
    foreach ($function in $ast.FindAll({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst]
    }, $true)) {
        if ($functions -contains $function.Name) { Invoke-Expression $function.Extent.Text }
    }
    $script:RuntimeRoot = Join-Path $fixture 'owned-root'
    $results = Join-Path $fixture 'evidence'
    New-Item -ItemType Directory -Path $script:RuntimeRoot,$results | Out-Null
    $state = [ordered]@{schema=1; current='1.10.2'; previous_known_good=$null; pending=$null;
                       package_sha256=$null; manifest_key_id=$null; last_result='fixture'}
    [IO.File]::WriteAllText((Join-Path $script:RuntimeRoot 'active-runtime.json'),
        ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    if ((Read-CheburnetRuntimeState).current -cne '1.10.2') { throw 'wrong runtime state path' }
    $run = Start-CheburnetAndSettle -Path $Probe -TimeoutSeconds 5
    if (-not $run.Settled -or $run.NewEngine -or $run.TimedOut) {
        throw "install probe failed or started engine: $($run | ConvertTo-Json)"
    }

    # The actual CLI, not a regex: selected but skipped is a failed gate.
    $reportDir = Join-Path $fixture 'skipped-report'
    $marker = Join-Path $fixture 'absent-marker'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $e2e -Launcher $Probe -ResultsDir $reportDir -MarkerPath $marker -Scenario fresh-install
    if ($LASTEXITCODE -eq 0) { throw 'all-skipped CLI must return nonzero' }
    $reportFile = @(Get-ChildItem -LiteralPath $reportDir -Filter e2e-report.json -Recurse)
    if ($reportFile.Count -ne 1) { throw 'gate must emit its evidence even on skipped failure' }
    $report = Get-Content -LiteralPath $reportFile[0].FullName -Raw | ConvertFrom-Json
    if ($report.passed -ne 0 -or $report.failed -ne 0 -or $report.skipped -ne 1) {
        throw 'skipped scenario must be reported accurately'
    }
    Write-Output 'REVIEW_CONTRACTS: PASS (diff writer, real install probe, state path, skipped CLI)'
} finally {
    $full = [IO.Path]::GetFullPath($fixture)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'fixture cleanup escaped temporary directory'
    }
    Remove-Item -LiteralPath $full -Recurse -Force
}

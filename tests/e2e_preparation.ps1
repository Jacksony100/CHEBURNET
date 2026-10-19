[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$Probe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-e2e-prep-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    $bundle = Join-Path $fixture 'bundle'
    & (Join-Path $RepositoryRoot 'scripts\e2e\New-CheburnetE2EBundle.ps1') -Launcher $Probe -OutDir $bundle | Out-Null
    $manifestPath = Join-Path $bundle 'bundle-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.launcher_sha256 -cne (Get-FileHash -LiteralPath $Probe -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'bundle must carry the actual candidate hash'
    }
    foreach ($entry in $manifest.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $bundle $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw "staged file hash mismatch: $($entry.path)"
        }
    }
    foreach ($name in @('New-CheburnetTestVm.ps1', 'Run-InGuest.ps1', 'Test-CheburnetE2EMatrix.ps1')) {
        if (-not (Test-Path -LiteralPath (Join-Path $bundle "scripts\e2e\$name"))) { throw "missing $name" }
    }
    # Exercise the very same copy routine used for the mounted provisioning disk.
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $RepositoryRoot 'scripts\e2e\New-CheburnetTestVm.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) { throw 'VM script must parse' }
    $copy = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Copy-CheburnetE2EBundle'
    }, $true)
    if ($null -eq $copy) { throw 'missing VM payload copy routine' }
    Invoke-Expression $copy.Extent.Text
    $guestStage = Join-Path $fixture 'guest-volume\cheburnet-e2e'
    Copy-CheburnetE2EBundle -Source $bundle -Destination $guestStage
    $bootstrap = Join-Path $guestStage 'scripts\e2e\Run-InGuest.ps1'
    $computedRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $bootstrap))
    if (-not (Test-Path -LiteralPath (Join-Path $computedRoot 'release\CHEBURNET.exe'))) {
        throw 'guest bootstrap must resolve the actual launcher path'
    }
    [IO.File]::AppendAllText((Join-Path $bundle 'release\CHEBURNET.exe'), 'corruption')
    $rejected = $false
    try { Copy-CheburnetE2EBundle -Source $bundle -Destination (Join-Path $fixture 'invalid-stage') } catch {
        $rejected = $_.Exception.Message -match 'SHA-256 mismatch'
    }
    if (-not $rejected -or (Test-Path -LiteralPath (Join-Path $fixture 'invalid-stage'))) {
        throw 'tampered bundle must be refused before writing the guest volume'
    }
    # Synthetic reports exist only in this temporary regression fixture.
    $reportRoot = Join-Path $fixture 'synthetic-reports'
    $reportPaths = @()
    foreach ($edition in @('win10', 'win11')) {
        foreach ($role in @('elevated', 'standard-user')) {
            $scenarios = if ($role -ceq 'elevated') {
                @('fresh-install', 'existing-install', 'interrupted-pending', 'startup-rollback', 'filesystem-guards')
            } else { @('refuses-unelevated') }
            $folder = Join-Path $reportRoot "$edition\$role"
            New-Item -ItemType Directory -Path $folder -Force | Out-Null
            $path = Join-Path $folder 'e2e-report.json'
            $report = [ordered]@{
                schema=1; environment_kind='disposable-vm'; launcher_sha256=$manifest.launcher_sha256; token_role=$role
                machine=@{product_type=1; architecture='AMD64'; build=$(if ($edition -ceq 'win10') {19045} else {26100})}
                required_scenarios=@($scenarios)
                results=@($scenarios | ForEach-Object { @{scenario=$_; outcome='passed'} })
                passed=@($scenarios).Count; failed=0; skipped=0
            }
            [IO.File]::WriteAllText($path, ($report | ConvertTo-Json -Depth 6))
            $reportPaths += $path
        }
    }
    $validator = Join-Path $RepositoryRoot 'scripts\e2e\Test-CheburnetE2EMatrix.ps1'
    & $validator -ReportsDir $reportRoot -BundleManifest $manifestPath | Out-Null
    $original = Get-Content -LiteralPath $reportPaths[0] -Raw
    foreach ($mutation in @('skip', 'hash', 'server', 'host', 'duplicate-scenario', 'duplicate-role', 'missing-report')) {
        $bad = $original | ConvertFrom-Json
        switch ($mutation) {
            'skip' { $bad.skipped=1; $bad.passed=4; $bad.results[0].outcome='skipped' }
            'hash' { $bad.launcher_sha256=('0' * 64) }
            'server' { $bad.machine.product_type=3 }
            'host' { $bad.environment_kind='host-isolated-build' }
            'duplicate-scenario' { $bad.results[0].scenario=$bad.results[1].scenario }
            'duplicate-role' { $bad.machine.build=26100 }
            'missing-report' { Remove-Item -LiteralPath $reportPaths[0] }
        }
        if ($mutation -cne 'missing-report') {
            [IO.File]::WriteAllText($reportPaths[0], ($bad | ConvertTo-Json -Depth 6))
        }
        $rejected = $false
        try { & $validator -ReportsDir $reportRoot -BundleManifest $manifestPath | Out-Null } catch { $rejected=$true }
        if (-not $rejected) { throw "matrix validator accepted $mutation" }
        [IO.File]::WriteAllText($reportPaths[0], $original)
    }
    Write-Output 'E2E_PREPARATION: PASS (bundle hashes, guest copy/path, tampering refusal, matrix validation)'
} finally {
    $full = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'cleanup escaped temporary directory' }
    Remove-Item -LiteralPath $full -Recurse -Force
}

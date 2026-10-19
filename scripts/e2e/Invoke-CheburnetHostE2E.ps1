# Run from standard Windows PowerShell. UAC authorizes the isolated second phase.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildManifest,
    [Parameter(Mandatory = $true)][string]$ResultsDir,
    [switch]$ElevatedPhase
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$manifestPath = (Resolve-Path -LiteralPath $BuildManifest).Path
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.instance -cnotmatch '^[0-9a-f]{16}$') { throw 'invalid host build manifest' }
foreach ($pair in @(@('launcher', 'launcher_sha256'), @('driver_probe', 'driver_probe_sha256'))) {
    if ((Get-FileHash -LiteralPath $manifest.($pair[0]) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.($pair[1])) {
        throw "host build hash mismatch: $($pair[0])"
    }
}
$results = [IO.Path]::GetFullPath($ResultsDir)
$hostRoot = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) "CHEBURNET-E2E-$($manifest.instance)"
$windowsPS = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$harness = Join-Path $PSScriptRoot 'Invoke-CheburnetE2E.ps1'
$admin = ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($admin -ne [bool]$ElevatedPhase) { throw 'start the coordinator without elevation; only -ElevatedPhase uses an administrator token' }
New-Item -ItemType Directory -Path $results -Force | Out-Null
if (-not $ElevatedPhase) {
    if (Test-Path -LiteralPath (Join-Path $results 'host-report.json')) { throw 'use a fresh results directory' }
    & $windowsPS -NoProfile -ExecutionPolicy Bypass -File $harness -Launcher $manifest.launcher `
        -ResultsDir (Join-Path $results 'standard-user') -HostInstance $manifest.instance -Scenario all `
        *> (Join-Path $results 'standard-user.log')
    if ($LASTEXITCODE -ne 0) { throw 'standard-user scenario failed; see standard-user.log' }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath +
        '" -BuildManifest "' + $manifestPath + '" -ResultsDir "' + $results + '" -ElevatedPhase'
    $worker = Start-Process -FilePath $windowsPS -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -PassThru
    $deadline = [DateTime]::UtcNow.AddMinutes(6)
    $reportPath = Join-Path $results 'host-report.json'
    while (-not (Test-Path -LiteralPath $reportPath)) {
        if ($worker.HasExited) { throw 'elevated worker exited without a report' }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'host E2E did not finish within six minutes' }
        Start-Sleep -Milliseconds 300
    }
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Write-Output ("HOST_E2E: passed=$($report.passed) driver=$($report.driver_exit_code) " +
        "original_unchanged=$($report.production_unchanged) cleanup=$($report.cleanup_exit_code) report=$reportPath")
    exit $(if ($report.passed) { 0 } else { 1 })
}

function Get-ProductionFingerprint {
    $production = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'CHEBURNET'
    if (-not (Test-Path -LiteralPath $production)) { return 'absent' }
    $stack = [Collections.Generic.Stack[string]]::new()
    $stack.Push($production)
    $entries = [Collections.Generic.List[string]]::new()
    while ($stack.Count -gt 0) {
        $path = $stack.Pop()
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'production snapshot refuses reparse points' }
        $relative = $path.Substring($production.Length)
        $entries.Add($relative + '|' + (Get-Acl -LiteralPath $path).Sddl)
        if ($item.PSIsContainer) {
            foreach ($child in Get-ChildItem -LiteralPath $path -Force) { $stack.Push($child.FullName) }
        } else { $entries.Add($relative + '|' + (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash) }
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($entries.ToArray() | Sort-Object) -join "`n")
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

$receipt = [ordered]@{
    schema=1; environment_kind='host-isolated-build'; instance=$manifest.instance
    started_at=[DateTime]::UtcNow.ToString('o'); finished_at=$null
    os=(Get-CimInstance Win32_OperatingSystem | Select-Object Caption,BuildNumber,ProductType)
    launcher_sha256=$manifest.launcher_sha256; passed=$false; driver_exit_code=$null
    cleanup_exit_code=$null; production_before=$null; production_after=$null
    production_unchanged=$false; drivers_after=@(); error=$null
}
$cleanupAuthorized = $false
try {
    foreach ($executable in @($manifest.launcher, $manifest.driver_probe)) {
        $identity = @(& $executable --e2e-identity)
        if ($LASTEXITCODE -ne 0 -or ($identity -join '').Trim() -cne "CHEBURNET_E2E_INSTANCE:$($manifest.instance)") {
            throw 'executable is not the matching isolated host build'
        }
    }
    if (Test-Path -LiteralPath $hostRoot) { throw 'test namespace already exists; use a new build instance' }
    if (@(Get-Process -Name winws -ErrorAction SilentlyContinue).Count -ne 0 -or
        @(Get-Service -Name 'WinDivert*' -ErrorAction SilentlyContinue).Count -ne 0 -or
        @(Get-CimInstance Win32_SystemDriver -Filter "Name LIKE 'WinDivert%'").Count -ne 0) {
        throw 'existing winws/WinDivert detected; host E2E refuses to disturb it'
    }
    $receipt.production_before = Get-ProductionFingerprint
    $cleanupAuthorized = $true
    & $windowsPS -NoProfile -ExecutionPolicy Bypass -File $harness -Launcher $manifest.launcher `
        -ResultsDir (Join-Path $results 'elevated') -HostInstance $manifest.instance -Scenario all `
        *> (Join-Path $results 'elevated.log')
    if ($LASTEXITCODE -ne 0) { throw 'elevated scenarios failed; see elevated.log' }
    & $manifest.driver_probe --driver-smoke *> (Join-Path $results 'driver-smoke.log')
    $receipt.driver_exit_code = $LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { throw 'driver smoke failed; see driver-smoke.log' }
    $reports = @(Get-ChildItem -LiteralPath $results -Filter e2e-report.json -Recurse -File)
    if ($reports.Count -ne 2) { throw 'both token reports are required' }
    $total = 0
    foreach ($file in $reports) {
        $report = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($report.environment_kind -cne 'host-isolated-build' -or $report.launcher_sha256 -cne $manifest.launcher_sha256 -or
            $report.failed -ne 0 -or $report.skipped -ne 0) { throw 'invalid host scenario report' }
        $total += $report.passed
    }
    if ($total -ne 6) { throw 'six host scenarios must pass' }
    $receipt.passed = $true
} catch { $receipt.error = $_.Exception.Message }
finally {
    if ($cleanupAuthorized) {
        & $manifest.driver_probe --cleanup *> (Join-Path $results 'cleanup.log')
        $receipt.cleanup_exit_code = $LASTEXITCODE
        $receipt.production_after = Get-ProductionFingerprint
        $receipt.production_unchanged = $receipt.production_before -ceq $receipt.production_after
    }
    $receipt.drivers_after = @(Get-CimInstance Win32_SystemDriver -Filter "Name LIKE 'WinDivert%'" | Select-Object Name,State,PathName)
    $receipt.passed = $receipt.passed -and $receipt.production_unchanged -and
        $receipt.cleanup_exit_code -eq 0 -and $receipt.drivers_after.Count -eq 0 -and
        -not (Test-Path -LiteralPath $hostRoot)
    $receipt.finished_at = [DateTime]::UtcNow.ToString('o')
    $reportPath = Join-Path $results 'host-report.json'
    [IO.File]::WriteAllText("$reportPath.tmp", ($receipt | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath "$reportPath.tmp" -Destination $reportPath
}
exit $(if ($receipt.passed) { 0 } else { 1 })

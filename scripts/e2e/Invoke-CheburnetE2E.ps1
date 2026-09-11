<#
CHEBURNET release validation suite for a clean Windows environment.

WHAT IT DOES

The launcher performs protected-tree bootstrap, runtime extraction, integrity
verification, interrupted-pending recovery and startup rollback in App's
constructor -- before any user interface exists. The suite therefore drives real
scenarios by starting the real executable and asserting on observable state
(%ProgramData%\CHEBURNET contents, ACLs, active-runtime.json, config.json,
exit codes) instead of scripting a text user interface.

It never starts the packet engine and never loads the WinDivert driver: connect,
disconnect and relaunch remain the manual part of the gate, described in
docs/production/E2E_RELEASE_GATE.md.

SAFETY

CHEBURNET's runtime root is a fixed protected location, so this suite modifies
the machine it runs on. It refuses to run unless the machine is explicitly
marked as a disposable test environment:

    New-Item -ItemType File -Path C:\cheburnet-e2e-vm.marker

Use -SelfTest to validate the harness itself. Self-test needs no privileges,
touches nothing outside its own temporary directory, and is what CI runs.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [string]$Launcher,
    [string]$ResultsDir,
    [ValidateSet('all', 'refuses-unelevated', 'fresh-install', 'existing-install',
                 'interrupted-pending', 'startup-rollback', 'filesystem-guards')]
    [string[]]$Scenario = @('all'),
    [string]$MarkerPath = 'C:\cheburnet-e2e-vm.marker',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:RuntimeRoot = Join-Path $env:ProgramData 'CHEBURNET'

# ---------------------------------------------------------------------------
# Scenario registry. Declared as data so the self-test can verify coverage of
# the acceptance criteria without executing anything privileged.
# ---------------------------------------------------------------------------
$script:Scenarios = @(
    [pscustomobject]@{
        Name = 'refuses-unelevated'
        Requires = 'none'
        Criterion = 'AC-08 fresh install'
        Description = 'An unelevated launch is refused by Windows and changes nothing.'
    },
    [pscustomobject]@{
        Name = 'fresh-install'
        Requires = 'elevation'
        Criterion = 'AC-08 fresh install'
        Description = 'No existing runtime root: extraction, config creation, strategy enumeration and protected ACLs.'
    },
    [pscustomobject]@{
        Name = 'existing-install'
        Requires = 'elevation'
        Criterion = 'AC-08 relaunch'
        Description = 'Existing config and runtime survive a relaunch unchanged.'
    },
    [pscustomobject]@{
        Name = 'interrupted-pending'
        Requires = 'elevation'
        Criterion = 'AC-04 / AC-08 interrupted pending recovery'
        Description = 'A pending runtime left by an interrupted update is never promoted automatically.'
    },
    [pscustomobject]@{
        Name = 'startup-rollback'
        Requires = 'elevation'
        Criterion = 'AC-04 / AC-08 rollback'
        Description = 'A corrupted active runtime is rolled back to the previous known-good version.'
    },
    [pscustomobject]@{
        Name = 'filesystem-guards'
        Requires = 'elevation'
        Criterion = 'AC-07 filesystem safety'
        Description = 'A reparse point planted in the protected tree makes startup fail closed.'
    }
)

function Get-CheburnetE2EScenarios { return $script:Scenarios }

function Test-CheburnetE2EEnvironment {
    <#
    Returns the reasons this machine must not run the privileged suite.
    An empty list means the suite may run.
    #>
    param([string]$Marker = $script:MarkerPathValue)
    $reasons = New-Object Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $Marker -PathType Leaf)) {
        $reasons.Add("machine is not marked as a disposable test environment ($Marker is missing)")
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $reasons.Add('the privileged scenarios require an elevated session')
    }
    $foreign = @(Get-Process -Name 'winws' -ErrorAction SilentlyContinue)
    if ($foreign.Count -ne 0) {
        $reasons.Add("a winws.exe process is already running (pid $($foreign[0].Id)); refusing to disturb it")
    }
    return $reasons.ToArray()
}

function New-CheburnetE2EResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet('passed', 'failed', 'skipped')][string]$Outcome,
        [string]$Detail = '',
        [string[]]$Evidence = @()
    )
    # .ToArray()/explicit array rather than @(...): PowerShell 5.1 throws
    # 'Argument types do not match' when @() wraps an EMPTY generic List.
    return [pscustomobject]@{
        scenario = $Name
        outcome  = $Outcome
        detail   = $Detail
        evidence = [string[]]$Evidence
        at       = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
}

function Start-CheburnetAndSettle {
    <#
    Starts the launcher, waits until its startup work is observable, then stops
    it. Returns the observed exit behaviour. The launcher is a text UI that
    waits for input, so a settled startup is detected by the state it writes.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [int]$TimeoutSeconds = 60
    )
    $process = Start-Process -FilePath $Path -ArgumentList '--no-animation', '--ascii-only' `
        -PassThru -WindowStyle Minimized
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $settled = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { break }
        if ((Test-Path -LiteralPath (Join-Path $script:RuntimeRoot 'config.json')) -and
            (Test-Path -LiteralPath (Join-Path $script:RuntimeRoot 'runtime'))) {
            $settled = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    $exitCode = $null
    if ($process.HasExited) {
        $exitCode = $process.ExitCode
    } else {
        # Stopping the UI is not part of any scenario's assertion: the startup
        # work under test has already completed by this point.
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(10000) | Out-Null
    }
    return [pscustomobject]@{ Settled = $settled; ExitCode = $exitCode }
}

function Read-CheburnetRuntimeState {
    $path = Join-Path $script:RuntimeRoot 'runtime\active-runtime.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}

# ---------------------------------------------------------------------------
# Self-test: validates the harness, not the product. No privileges, no writes
# outside a temporary directory.
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $failures = New-Object Collections.Generic.List[string]
    function Assert-True([bool]$Condition, [string]$Message) {
        if (-not $Condition) { $script:failures.Add($Message) }
    }

    $scenarios = Get-CheburnetE2EScenarios
    Assert-True ($scenarios.Count -ge 6) 'the suite must declare at least six scenarios'
    foreach ($required in @('refuses-unelevated', 'fresh-install', 'existing-install',
                            'interrupted-pending', 'startup-rollback', 'filesystem-guards')) {
        Assert-True (@($scenarios.Name) -ccontains $required) "scenario '$required' must be declared"
    }
    foreach ($item in $scenarios) {
        Assert-True (-not [string]::IsNullOrWhiteSpace($item.Criterion)) `
            "scenario '$($item.Name)' must name the acceptance criterion it covers"
        Assert-True (-not [string]::IsNullOrWhiteSpace($item.Description)) `
            "scenario '$($item.Name)' must carry a description"
    }

    # The environment guard must refuse a machine that is not marked disposable.
    $absentMarker = Join-Path ([IO.Path]::GetTempPath()) ('absent-' + [guid]::NewGuid().ToString('N'))
    $reasons = @(Test-CheburnetE2EEnvironment -Marker $absentMarker)
    Assert-True ($reasons.Count -gt 0) 'an unmarked machine must be refused'
    Assert-True (($reasons -join ' ') -match 'disposable test environment') `
        'the refusal must name the missing disposable-environment marker'

    # The result records must be shaped the way the evidence report expects.
    $sample = New-CheburnetE2EResult -Name 'fresh-install' -Outcome 'passed' -Detail 'x' `
        -Evidence @('a', 'b')
    foreach ($field in @('scenario', 'outcome', 'detail', 'evidence', 'at')) {
        Assert-True ($null -ne $sample.PSObject.Properties[$field]) `
            "the result record must carry the field '$field'"
    }
    Assert-True (@($sample.evidence).Count -eq 2) 'evidence must survive as a collection'
    $threw = $false
    try { New-CheburnetE2EResult -Name 'x' -Outcome 'maybe' | Out-Null } catch { $threw = $true }
    Assert-True $threw 'an unknown outcome must be rejected'

    if ($failures.Count -ne 0) {
        foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
        throw "E2E_HARNESS: FAIL ($($failures.Count))"
    }
    Write-Output ("E2E_HARNESS: PASS ($($scenarios.Count) scenarios declared, environment guard " +
                  'and evidence schema verified)')
    exit 0
}

# ---------------------------------------------------------------------------
# Privileged run.
# ---------------------------------------------------------------------------
$script:MarkerPathValue = $MarkerPath
if ([string]::IsNullOrWhiteSpace($Launcher)) {
    throw 'the privileged suite requires -Launcher pointing at the CHEBURNET.exe under test'
}
$launcherPath = [IO.Path]::GetFullPath($Launcher)
if (-not (Test-Path -LiteralPath $launcherPath -PathType Leaf)) {
    throw "launcher not found: $launcherPath"
}
if ([string]::IsNullOrWhiteSpace($ResultsDir)) {
    $ResultsDir = Join-Path ([IO.Path]::GetTempPath()) 'cheburnet-e2e'
}
$results = Join-Path ([IO.Path]::GetFullPath($ResultsDir)) ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $results -Force | Out-Null

$selected = if (@($Scenario) -ccontains 'all') { @($script:Scenarios.Name) } else { @($Scenario) }
$records = New-Object Collections.Generic.List[object]

# The unelevated refusal is the only scenario that must run WITHOUT elevation,
# so it is evaluated against the environment guard rather than gated by it.
$environmentReasons = @(Test-CheburnetE2EEnvironment -Marker $MarkerPath)
$elevated = ($environmentReasons -join ' ') -notmatch 'elevated session'

foreach ($name in $selected) {
    $definition = @($script:Scenarios | Where-Object Name -ceq $name)
    if ($definition.Count -ne 1) { throw "unknown scenario: $name" }
    if ($definition[0].Requires -ceq 'elevation' -and $environmentReasons.Count -ne 0) {
        $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
            -Detail ("environment refused: " + ($environmentReasons -join '; '))))
        continue
    }

    switch ($name) {
        'refuses-unelevated' {
            if ($elevated) {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail 'session is elevated; run this scenario from a standard user session'))
                break
            }
            $before = if (Test-Path -LiteralPath $script:RuntimeRoot) {
                (Get-Item -LiteralPath $script:RuntimeRoot).LastWriteTimeUtc
            } else { $null }
            $refused = $false
            $detail = ''
            try {
                $process = New-Object Diagnostics.Process
                $process.StartInfo.FileName = $launcherPath
                $process.StartInfo.Arguments = '--version'
                $process.StartInfo.UseShellExecute = $false
                $process.Start() | Out-Null
                $process.WaitForExit(15000) | Out-Null
                $detail = "launcher started without elevation (exit $($process.ExitCode))"
            } catch {
                # ERROR_ELEVATION_REQUIRED (740): the embedded manifest asks for
                # requireAdministrator, so CreateProcess refuses outright.
                $refused = $true
                $detail = [string]$_.Exception.Message
            }
            $after = if (Test-Path -LiteralPath $script:RuntimeRoot) {
                (Get-Item -LiteralPath $script:RuntimeRoot).LastWriteTimeUtc
            } else { $null }
            $unchanged = ($before -eq $after)
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($refused -and $unchanged) { 'passed' } else { 'failed' }) `
                -Detail $detail -Evidence @("runtime root unchanged: $unchanged")))
        }

        'fresh-install' {
            if (Test-Path -LiteralPath $script:RuntimeRoot) {
                Remove-Item -LiteralPath $script:RuntimeRoot -Recurse -Force
            }
            $run = Start-CheburnetAndSettle -Path $launcherPath
            $state = Read-CheburnetRuntimeState
            $strategyCatalogue = Join-Path $script:RuntimeRoot 'runtime'
            $evidence = @(
                "settled: $($run.Settled)",
                "config.json: $(Test-Path -LiteralPath (Join-Path $script:RuntimeRoot 'config.json'))",
                "runtime dir: $(Test-Path -LiteralPath $strategyCatalogue)",
                "active-runtime.json current: $(if ($state) { $state.current } else { '(absent)' })"
            )
            $ok = $run.Settled -and $null -ne $state -and
                  -not [string]::IsNullOrWhiteSpace([string]$state.current)
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($ok) { 'passed' } else { 'failed' }) `
                -Detail 'first launch on an empty runtime root' -Evidence $evidence))
        }

        'existing-install' {
            $configPath = Join-Path $script:RuntimeRoot 'config.json'
            if (-not (Test-Path -LiteralPath $configPath)) {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail 'run fresh-install first'))
                break
            }
            $configBefore = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
            $stateBefore = Read-CheburnetRuntimeState
            $run = Start-CheburnetAndSettle -Path $launcherPath
            $configAfter = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
            $stateAfter = Read-CheburnetRuntimeState
            $ok = $run.Settled -and ($configBefore -ceq $configAfter) -and
                  ($null -ne $stateAfter) -and
                  ([string]$stateBefore.current -ceq [string]$stateAfter.current)
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($ok) { 'passed' } else { 'failed' }) `
                -Detail 'relaunch with an existing config and runtime' `
                -Evidence @("config unchanged: $($configBefore -ceq $configAfter)",
                            "current runtime unchanged: $([string]$stateBefore.current -ceq [string]$stateAfter.current)")))
        }

        'interrupted-pending' {
            $statePath = Join-Path $script:RuntimeRoot 'runtime\active-runtime.json'
            if (-not (Test-Path -LiteralPath $statePath)) {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail 'run fresh-install first'))
                break
            }
            $original = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8
            $state = $original | ConvertFrom-Json
            $pendingVersion = '99.99.99'
            $seeded = [ordered]@{
                schema = 1
                current = [string]$state.current
                previous_known_good = ''
                pending = $pendingVersion
                last_result = 'e2e-seeded-interruption'
            }
            [IO.File]::WriteAllText($statePath, ($seeded | ConvertTo-Json) + "`n",
                                    [Text.UTF8Encoding]::new($false))
            $run = Start-CheburnetAndSettle -Path $launcherPath
            $after = Read-CheburnetRuntimeState
            $promoted = ($null -ne $after) -and ([string]$after.current -ceq $pendingVersion)
            $cleared = ($null -ne $after) -and [string]::IsNullOrEmpty([string]$after.pending)
            $ok = (-not $promoted) -and ($cleared -or -not $run.Settled)
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($ok) { 'passed' } else { 'failed' }) `
                -Detail 'pending runtime left by an interrupted update' `
                -Evidence @("pending promoted to current: $promoted",
                            "pending cleared: $cleared",
                            "last_result: $(if ($after) { $after.last_result } else { '(absent)' })")))
        }

        'startup-rollback' {
            $state = Read-CheburnetRuntimeState
            if ($null -eq $state) {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail 'run fresh-install first'))
                break
            }
            # A corrupted active runtime must not be used. Without a previous
            # known-good version the launcher must refuse to start at all.
            $versionDir = Join-Path $script:RuntimeRoot ('runtime\' + [string]$state.current)
            $victim = @(Get-ChildItem -LiteralPath $versionDir -Recurse -File -Filter '*.txt' |
                Select-Object -First 1)
            if ($victim.Count -ne 1) {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail "no list file found under $versionDir to corrupt"))
                break
            }
            Add-Content -LiteralPath $victim[0].FullName -Value 'e2e-corruption'
            $run = Start-CheburnetAndSettle -Path $launcherPath -TimeoutSeconds 30
            $after = Read-CheburnetRuntimeState
            $refusedOrRolledBack = (-not $run.Settled) -or
                (($null -ne $after) -and ([string]$after.last_result -cmatch 'rollback'))
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($refusedOrRolledBack) { 'passed' } else { 'failed' }) `
                -Detail 'corrupted active runtime' `
                -Evidence @("settled: $($run.Settled)",
                            "last_result: $(if ($after) { $after.last_result } else { '(absent)' })",
                            "corrupted file: $($victim[0].FullName)")))
        }

        'filesystem-guards' {
            $logsDir = Join-Path $script:RuntimeRoot 'logs'
            $decoy = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-e2e-decoy-' +
                [guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $decoy -Force | Out-Null
            $planted = $false
            try {
                if (Test-Path -LiteralPath $logsDir) {
                    Remove-Item -LiteralPath $logsDir -Recurse -Force
                }
                New-Item -ItemType Junction -Path $logsDir -Target $decoy | Out-Null
                $planted = $true
            } catch {
                $records.Add((New-CheburnetE2EResult -Name $name -Outcome 'skipped' `
                    -Detail "could not plant a reparse point: $($_.Exception.Message)"))
                break
            }
            $run = Start-CheburnetAndSettle -Path $launcherPath -TimeoutSeconds 30
            $escaped = @(Get-ChildItem -LiteralPath $decoy -Recurse -File -ErrorAction SilentlyContinue).Count
            $ok = (-not $run.Settled) -and ($escaped -eq 0)
            $records.Add((New-CheburnetE2EResult -Name $name `
                -Outcome $(if ($ok) { 'passed' } else { 'failed' }) `
                -Detail 'junction planted at the protected logs directory' `
                -Evidence @("planted: $planted", "startup settled: $($run.Settled)",
                            "files written through the junction: $escaped")))
            if (Test-Path -LiteralPath $logsDir) {
                (Get-Item -LiteralPath $logsDir -Force).Delete()
            }
            Remove-Item -LiteralPath $decoy -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

$summary = [ordered]@{
    schema      = 1
    generated_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    launcher    = $launcherPath
    launcher_version = [string]([Diagnostics.FileVersionInfo]::GetVersionInfo($launcherPath).FileVersion)
    machine     = [ordered]@{
        os = [string](Get-CimInstance Win32_OperatingSystem).Caption
        build = [string](Get-CimInstance Win32_OperatingSystem).BuildNumber
        architecture = [string]$env:PROCESSOR_ARCHITECTURE
    }
    environment_refusals = [string[]]$environmentReasons
    results     = $records.ToArray()
    passed      = @($records | Where-Object outcome -ceq 'passed').Count
    failed      = @($records | Where-Object outcome -ceq 'failed').Count
    skipped     = @($records | Where-Object outcome -ceq 'skipped').Count
}
$summaryPath = Join-Path $results 'e2e-report.json'
[IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8) + "`n",
                        [Text.UTF8Encoding]::new($false))

foreach ($record in $records) {
    Write-Output ("[{0,-7}] {1} -- {2}" -f $record.outcome, $record.scenario, $record.detail)
}
Write-Output ("E2E: passed=$($summary.passed) failed=$($summary.failed) " +
              "skipped=$($summary.skipped) report=$summaryPath")
if ($summary.failed -ne 0) { exit 1 }

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$Probe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$harness = Join-Path $RepositoryRoot 'scripts\e2e\Invoke-CheburnetE2E.ps1'
$tokens=$null; $errors=$null
$ast = [Management.Automation.Language.Parser]::ParseFile($harness, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'host-capable harness must parse' }
$function = $ast.Find({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Test-CheburnetHostIdentity'
}, $true)
Invoke-Expression $function.Extent.Text
$id = [guid]::NewGuid().ToString('N').Substring(0,16)
$valid = [pscustomobject]@{TimedOut=$false;NewEngine=$false;ExitCode=0;Output="CHEBURNET_E2E_INSTANCE:$id`r`n"}
if (-not (Test-CheburnetHostIdentity $valid $id)) { throw 'matching isolated identity must pass' }
foreach ($case in @('timeout','engine','nonzero','production','other-instance','trailing-output')) {
    $run = [pscustomobject]@{TimedOut=$false;NewEngine=$false;ExitCode=0;Output=$valid.Output}
    switch ($case) {
        'timeout' { $run.TimedOut=$true }
        'engine' { $run.NewEngine=$true }
        'nonzero' { $run.ExitCode=2 }
        'production' { $run.Output='CHEBURNET_INSTALL_VERIFIED' }
        'other-instance' { $run.Output='CHEBURNET_E2E_INSTANCE:ffffffffffffffff' }
        'trailing-output' { $run.Output += 'extra' }
    }
    if (Test-CheburnetHostIdentity $run $id) { throw "unsafe identity accepted: $case" }
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-host-contract-' + [guid]::NewGuid().ToString('N'))
$data = [Environment]::GetFolderPath('CommonApplicationData')
$namespace = Join-Path $data "CHEBURNET-E2E-$id"
$elevated = ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$owned = $false
try {
    if (Test-Path -LiteralPath $namespace) { throw 'test namespace collision' }
    if ($elevated) {
        New-Item -ItemType Directory -Path $namespace | Out-Null
        $owned = $true
        [IO.File]::WriteAllText((Join-Path $namespace 'sentinel'), 'must survive refused probe')
    }
    # The real destructive harness must refuse a probe without a matching
    # compile-time identity BEFORE it can execute fresh-install's cleanup.
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue' # expected native stderr from a refused build
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $harness -Launcher $Probe `
            -ResultsDir $fixture -HostInstance $id -Scenario fresh-install *> ($fixture + '.log')
        $refusalCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    if ($refusalCode -eq 0) { throw 'unmatched host build must fail' }
    if ($owned -and (Get-Content -LiteralPath (Join-Path $namespace 'sentinel') -Raw) -cne 'must survive refused probe') {
        throw 'refused host probe modified existing namespace'
    }
    Write-Output 'HOST_E2E_CONTRACTS: PASS (identity guards, actual harness refusal before deletion)'
} finally {
    if ($owned) {
        $full = [IO.Path]::GetFullPath($namespace)
        if ($full -cne (Join-Path $data "CHEBURNET-E2E-$id")) { throw 'namespace cleanup escaped fixture' }
        Remove-Item -LiteralPath $full -Recurse -Force
    }
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not [IO.Path]::GetFullPath($fixture).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'cleanup escaped temporary fixture'
    }
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
    if (Test-Path -LiteralPath ($fixture + '.log')) { Remove-Item -LiteralPath ($fixture + '.log') }
}

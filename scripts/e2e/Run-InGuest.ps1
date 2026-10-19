# Execute only inside a disposable Windows client VM. Run both token roles.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('win10', 'win11')][string]$Edition,
    [ValidateSet('elevated', 'standard-user')][string]$Role = 'elevated',
    [string]$ResultsDir = 'C:\cheburnet-e2e-results',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$stage = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$manifest = Get-Content -LiteralPath (Join-Path $stage 'bundle-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $stage $entry.path))
    if (-not $path.StartsWith($stage.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'bundle file path escaped its root'
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
        throw "bundle SHA-256 mismatch: $($entry.path)"
    }
}
$launcher = Join-Path $stage 'release\CHEBURNET.exe'
if ((Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.launcher_sha256) {
    throw 'launcher SHA-256 mismatch'
}
$os = Get-CimInstance Win32_OperatingSystem
$machine = Get-CimInstance Win32_ComputerSystem
if ($machine.Model -notmatch 'Virtual Machine|VMware|VirtualBox|KVM|QEMU') {
    throw 'Run-InGuest is restricted to a disposable VM; physical host refused'
}
$build = [int]$os.BuildNumber
if ($os.ProductType -ne 1 -or $env:PROCESSOR_ARCHITECTURE -cne 'AMD64' -or -not [Environment]::Is64BitOperatingSystem -or
    -not [Environment]::Is64BitProcess -or $build -lt 10240 -or
    ($Edition -ceq 'win10' -and $build -ge 22000) -or
    ($Edition -ceq 'win11' -and $build -lt 22000)) {
    throw "wrong target OS for $Edition : $($os.Caption) build $build; x64 Windows client required"
}
$elevated = ([Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($PrepareOnly) {
    if (-not $elevated) { throw '-PrepareOnly needs an elevated guest PowerShell' }
    New-Item -ItemType File -Path 'C:\cheburnet-e2e-vm.marker' -Force | Out-Null
    New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null
    # Evidence is shared between token roles, only inside the disposable guest.
    $acl = Get-Acl -LiteralPath $ResultsDir
    $users = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-545')
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $users, 'Modify', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    Set-Acl -LiteralPath $ResultsDir -AclObject $acl
    Write-Output 'Guest prepared. Run standard-user first, then elevated, from separate PowerShell sessions.'
    exit 0
}
if ($elevated -ne ($Role -ceq 'elevated')) { throw "wrong token role: expected $Role" }
if (-not (Test-Path -LiteralPath 'C:\cheburnet-e2e-vm.marker' -PathType Leaf)) {
    throw 'prepare this disposable guest with -PrepareOnly first'
}
$roleResults = Join-Path $ResultsDir "$Edition\$Role"
& (Join-Path $PSScriptRoot 'Invoke-CheburnetE2E.ps1') -Launcher $launcher `
    -ResultsDir $roleResults -Scenario all
exit $LASTEXITCODE

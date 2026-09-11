<#
Reproducible Hyper-V test environment for the CHEBURNET release gate.

GitHub-hosted runners provide Windows Server images, not Windows 10 / Windows 11
client, and the release gate needs a clean client environment where the
WinDivert driver can actually load. This script provisions that environment
reproducibly so the manual gate is a checklist, not improvisation.

What it automates:
  * a Generation 2 VM with Secure Boot, TPM and a fixed, documented spec;
  * a provisioning VHDX carrying the release artifacts, the repository test
    scripts and the disposable-environment marker;
  * a checkpoint taken immediately after Windows setup, so every scenario can
    start from a genuinely clean state.

What stays manual, and why: choosing the Windows edition and image index inside
the ISO, and the out-of-box setup itself. An unattend.xml that picks an edition
and a product key cannot be committed generically, and guessing it would
silently produce the wrong environment.

Run elevated on a Hyper-V host.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('win10', 'win11')][string]$Edition,
    [Parameter(Mandatory = $true)][string]$IsoPath,
    [Parameter(Mandatory = $true)][string]$ReleaseDir,
    [string]$VmName,
    [string]$VmRoot = 'C:\CheburnetE2E',
    [string]$SwitchName = 'Default Switch',
    [int64]$MemoryBytes = 6GB,
    [int]$ProcessorCount = 4,
    [int64]$DiskBytes = 80GB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'provisioning a Hyper-V virtual machine requires an elevated session'
}
if (-not (Get-Command Get-VM -ErrorAction SilentlyContinue)) {
    throw 'the Hyper-V PowerShell module is not available on this host'
}
$iso = [IO.Path]::GetFullPath($IsoPath)
if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) { throw "ISO not found: $iso" }
$release = [IO.Path]::GetFullPath($ReleaseDir)
foreach ($required in @('CHEBURNET.exe', 'CHEBURNET.exe.sha256')) {
    if (-not (Test-Path -LiteralPath (Join-Path $release $required) -PathType Leaf)) {
        throw "release directory is missing $required : $release"
    }
}
if ([string]::IsNullOrWhiteSpace($VmName)) {
    $VmName = "CHEBURNET-E2E-$Edition"
}
if (Get-VM -Name $VmName -ErrorAction SilentlyContinue) {
    throw "virtual machine already exists: $VmName (remove it or pass -VmName)"
}

$vmDir = Join-Path $VmRoot $VmName
New-Item -ItemType Directory -Path $vmDir -Force | Out-Null
$systemDisk = Join-Path $vmDir 'system.vhdx'
$provisioningDisk = Join-Path $vmDir 'provisioning.vhdx'

Write-Host "== Provisioning disk ==" -ForegroundColor Cyan
New-VHD -Path $provisioningDisk -SizeBytes 2GB -Dynamic | Out-Null
$mounted = Mount-VHD -Path $provisioningDisk -PassThru | Initialize-Disk -PassThru |
    New-Partition -AssignDriveLetter -UseMaximumSize
$volume = $mounted | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'CHEBURNET-E2E' -Confirm:$false
$stage = "$($volume.DriveLetter):\"
try {
    New-Item -ItemType Directory -Path (Join-Path $stage 'release') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $release '*') -Destination (Join-Path $stage 'release') -Recurse
    New-Item -ItemType Directory -Path (Join-Path $stage 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath $PSScriptRoot -Destination (Join-Path $stage 'scripts\e2e') -Recurse

    # Bootstrap run inside the guest. It marks the guest as disposable and runs
    # the suite; it never reaches back out to the host.
    $bootstrap = @'
# Run elevated inside the guest.
$ErrorActionPreference = 'Stop'
$stage = Split-Path -Parent $PSScriptRoot
New-Item -ItemType File -Path 'C:\cheburnet-e2e-vm.marker' -Force | Out-Null
$launcher = Join-Path $stage 'release\CHEBURNET.exe'
$expected = (Get-Content (Join-Path $stage 'release\CHEBURNET.exe.sha256') -Raw).Split(' ')[0].Trim()
$actual = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -cne $expected) { throw "launcher SHA-256 mismatch: $actual" }
Write-Host "launcher SHA-256 verified: $actual" -ForegroundColor Green
& (Join-Path $PSScriptRoot 'Invoke-CheburnetE2E.ps1') -Launcher $launcher `
    -ResultsDir 'C:\cheburnet-e2e-results' -Scenario all
'@
    [IO.File]::WriteAllText((Join-Path $stage 'scripts\e2e\Run-InGuest.ps1'), $bootstrap,
                            [Text.UTF8Encoding]::new($false))
} finally {
    Dismount-VHD -Path $provisioningDisk
}

Write-Host "== Virtual machine ==" -ForegroundColor Cyan
New-VHD -Path $systemDisk -SizeBytes $DiskBytes -Dynamic | Out-Null
$vm = New-VM -Name $VmName -Generation 2 -MemoryStartupBytes $MemoryBytes `
    -VHDPath $systemDisk -SwitchName $SwitchName -Path $vmDir
Set-VM -VM $vm -ProcessorCount $ProcessorCount -AutomaticCheckpointsEnabled $false
Add-VMDvdDrive -VM $vm -Path $iso
Add-VMHardDiskDrive -VM $vm -Path $provisioningDisk
# Secure Boot and a virtual TPM are required for a representative Windows 11
# environment and are harmless for Windows 10.
Set-VMFirmware -VM $vm -EnableSecureBoot On `
    -FirstBootDevice (Get-VMDvdDrive -VM $vm)
try {
    Set-VMKeyProtector -VM $vm -NewLocalKeyProtector
    Enable-VMTPM -VM $vm
} catch {
    Write-Warning "could not enable the virtual TPM: $($_.Exception.Message)"
}

Write-Host ''
Write-Host "Virtual machine created: $VmName" -ForegroundColor Green
Write-Host "  system disk       : $systemDisk"
Write-Host "  provisioning disk : $provisioningDisk (label CHEBURNET-E2E)"
Write-Host ''
Write-Host 'Remaining manual steps:' -ForegroundColor Yellow
Write-Host "  1. Start-VM -Name $VmName and complete Windows setup from the ISO."
Write-Host '     Choose the edition deliberately: the gate requires Windows 10 x64'
Write-Host '     or Windows 11 x64 client, not Windows Server.'
Write-Host "  2. Checkpoint-VM -Name $VmName -SnapshotName 'clean-after-setup'"
Write-Host '  3. Inside the guest, open an elevated PowerShell on the CHEBURNET-E2E'
Write-Host '     volume and run: .\scripts\e2e\Run-InGuest.ps1'
Write-Host '  4. Copy C:\cheburnet-e2e-results\*\e2e-report.json out of the guest and'
Write-Host '     attach it to docs/production/FINAL_RELEASE_CERTIFICATION.md.'
Write-Host "  5. Restore-VMSnapshot -Name 'clean-after-setup' -VMName $VmName before"
Write-Host '     re-running the suite, so every run starts from a clean state.'

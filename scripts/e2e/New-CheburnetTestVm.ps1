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
  * commands for a checkpoint after manual Windows setup, so every run can
    start from a genuinely clean state.

What stays manual, and why: choosing the Windows edition and image index inside
the ISO, and the out-of-box setup itself. An unattend.xml that picks an edition
and a product key cannot be committed generically, and guessing it would
silently produce the wrong environment.

Run elevated on a Hyper-V host.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding(DefaultParameterSetName = 'Release')]
param(
    [Parameter(Mandatory = $true)][ValidateSet('win10', 'win11')][string]$Edition,
    [Parameter(Mandatory = $true)][string]$IsoPath,
    [Parameter(Mandatory = $true, ParameterSetName = 'Release')][string]$ReleaseDir,
    [Parameter(Mandatory = $true, ParameterSetName = 'Bundle')][string]$BundleDir,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$')][string]$VmName,
    [string]$VmRoot = 'C:\CheburnetE2E',
    [string]$SwitchName = 'Default Switch',
    [int64]$MemoryBytes = 6GB,
    [int]$ProcessorCount = 4,
    [int64]$DiskBytes = 80GB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Copy-CheburnetE2EBundle([string]$Source, [string]$Destination) {
    $sourceRoot = [IO.Path]::GetFullPath($Source).TrimEnd('\') + '\'
    $manifestPath = Join-Path $sourceRoot 'bundle-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schema -ne 1) { throw 'unsupported bundle schema' }
    $copies = @()
    foreach ($entry in $manifest.files) {
        $inputPath = [IO.Path]::GetFullPath((Join-Path $sourceRoot $entry.path))
        $outputRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
        $outputPath = [IO.Path]::GetFullPath((Join-Path $outputRoot $entry.path))
        if (-not $inputPath.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not $outputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'bundle path escaped its root'
        }
        if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw "bundle SHA-256 mismatch: $($entry.path)"
        }
        $copies += [pscustomobject]@{Source=$inputPath; Target=$outputPath}
    }
    foreach ($copy in $copies) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $copy.Target) -Force | Out-Null
        Copy-Item -LiteralPath $copy.Source -Destination $copy.Target
    }
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $Destination 'bundle-manifest.json')
}

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
$candidate = if ($PSCmdlet.ParameterSetName -ceq 'Bundle') {
    [IO.Path]::GetFullPath($BundleDir)
} else { [IO.Path]::GetFullPath($ReleaseDir) }
$launcherRelative = if ($BundleDir) { 'release\CHEBURNET.exe' } else { 'CHEBURNET.exe' }
foreach ($required in @($launcherRelative, "$launcherRelative.sha256")) {
    if (-not (Test-Path -LiteralPath (Join-Path $candidate $required) -PathType Leaf)) {
        throw "candidate directory is missing $required : $candidate"
    }
}
$expected = ((Get-Content -LiteralPath (Join-Path $candidate "$launcherRelative.sha256") -Raw).Trim() -split '\s+')[0]
if ($expected -notmatch '^[a-fA-F0-9]{64}$' -or
    (Get-FileHash -LiteralPath (Join-Path $candidate $launcherRelative) -Algorithm SHA256).Hash -ine $expected) {
    throw 'candidate SHA-256 mismatch'
}
if ($BundleDir -and -not (Test-Path -LiteralPath (Join-Path $candidate 'bundle-manifest.json'))) {
    throw 'prepared bundle is missing bundle-manifest.json'
}
if (-not (Get-VMSwitch -Name $SwitchName -ErrorAction SilentlyContinue)) {
    throw "Hyper-V switch not found: $SwitchName"
}
if ([string]::IsNullOrWhiteSpace($VmName)) {
    $VmName = "CHEBURNET-E2E-$Edition"
}
if (Get-VM -Name $VmName -ErrorAction SilentlyContinue) {
    throw "virtual machine already exists: $VmName (remove it or pass -VmName)"
}

$vmRootPath = [IO.Path]::GetFullPath($VmRoot).TrimEnd('\') + '\'
$vmDir = [IO.Path]::GetFullPath((Join-Path $vmRootPath $VmName))
if (-not $vmDir.StartsWith($vmRootPath, [StringComparison]::OrdinalIgnoreCase)) { throw 'VM path escaped its root' }
if (Test-Path -LiteralPath $vmDir) { throw "VM directory already exists: $vmDir" }
New-Item -ItemType Directory -Path $vmDir | Out-Null
$systemDisk = Join-Path $vmDir 'system.vhdx'
$provisioningDisk = Join-Path $vmDir 'provisioning.vhdx'

Write-Host "== Provisioning disk ==" -ForegroundColor Cyan
New-VHD -Path $provisioningDisk -SizeBytes 2GB -Dynamic | Out-Null
$mounted = $false
try {
    $disk = Mount-VHD -Path $provisioningDisk -PassThru
    $mounted = $true
    $partition = $disk | Initialize-Disk -PassThru | New-Partition -AssignDriveLetter -UseMaximumSize
    $volume = $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'CHEBURNET-E2E' -Confirm:$false
    $stage = Join-Path "$($volume.DriveLetter):\" 'cheburnet-e2e'
    if ($BundleDir) {
        Copy-CheburnetE2EBundle -Source $candidate -Destination $stage
    } else {
        & (Join-Path $PSScriptRoot 'New-CheburnetE2EBundle.ps1') `
            -Launcher (Join-Path $candidate $launcherRelative) -ExpectedSha256 $expected -OutDir $stage
    }
} finally {
    if ($mounted) { Dismount-VHD -Path $provisioningDisk }
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
Set-VMKeyProtector -VM $vm -NewLocalKeyProtector
Enable-VMTPM -VM $vm

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
Write-Host '  3. Inside the guest, open PowerShell on CHEBURNET-E2E:\cheburnet-e2e.'
Write-Host "     Elevated: .\scripts\e2e\Run-InGuest.ps1 -Edition $Edition -PrepareOnly"
Write-Host "     Standard: .\scripts\e2e\Run-InGuest.ps1 -Edition $Edition -Role standard-user"
Write-Host "     Elevated: .\scripts\e2e\Run-InGuest.ps1 -Edition $Edition -Role elevated"
Write-Host '  4. Copy C:\cheburnet-e2e-results recursively out of the guest and'
Write-Host '     attach it to docs/production/FINAL_RELEASE_CERTIFICATION.md.'
Write-Host "  5. Restore-VMSnapshot -Name 'clean-after-setup' -VMName $VmName before"
Write-Host '     re-running the suite, so every run starts from a clean state.'

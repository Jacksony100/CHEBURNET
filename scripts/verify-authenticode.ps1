<#
Independent Authenticode verification gate.

Run before publishing a stable release. Exits non-zero unless the file carries
a valid, trusted, in-date Authenticode signature (and, when required, a
countersignature), and -- when -ExpectedSha256 is given -- unless the file is
byte for byte the artifact that was signed.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$File,
    [switch]$RequireTimestamp,
    [string]$ExpectedSha256
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot\authenticode.ps1"

$report = Assert-CheburnetSignedLauncher -Path $File -RequireTimestamp:$RequireTimestamp `
    -ExpectedSha256 $ExpectedSha256
Write-Output ("AUTHENTICODE_VERIFY: PASS status=$($report.Status) " +
              "timestamped=$($report.Timestamped) sha256=$($report.Sha256) " +
              "subject=$($report.Subject)")

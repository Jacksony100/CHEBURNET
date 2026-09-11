<#
Authenticode signing for CHEBURNET release artifacts.

Channel policy:
  -Require  (stable vX.Y.Z): a missing certificate, a failed signing operation,
            a failed timestamp or a signature that does not verify are all hard
            failures. A stable release is never published unsigned.
  default   (prerelease vX.Y.Z-rc.N): signing is attempted when a certificate is
            configured and skipped with a warning when it is not. This
            prerelease exemption is deliberate and documented in
            docs/production/RELEASE_SIGNING.md; it never applies to a stable tag.

The signed file's SHA-256 is printed so the caller can bind later artifacts to
exactly these bytes. Update metadata must be generated after this step, never
before: signing changes the file, so a hash taken earlier would not describe
the published executable.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$File,
    [AllowEmptyString()][string]$CertificateBase64 = $env:AUTHENTICODE_PFX_B64,
    [AllowEmptyString()][string]$CertificatePassword = $env:AUTHENTICODE_PFX_PASSWORD,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$Require
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot\authenticode.ps1"

$target = [IO.Path]::GetFullPath($File)
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    throw "file to sign not found: $target"
}

if ([string]::IsNullOrWhiteSpace($CertificateBase64)) {
    if ($Require) {
        throw ('AUTHENTICODE_SIGNING_REQUIRED: no signing certificate is configured. ' +
               'A stable release must not be published unsigned. Provide ' +
               'AUTHENTICODE_PFX_B64 and AUTHENTICODE_PFX_PASSWORD.')
    }
    Write-Warning ('Authenticode certificate is not configured: the prerelease artifact ' +
                   'will be published unsigned (see docs/production/RELEASE_SIGNING.md).')
    Write-Output 'AUTHENTICODE_SIGN: SKIPPED (prerelease, no certificate configured)'
    exit 0
}
if ($Require -and [string]::IsNullOrWhiteSpace($CertificatePassword)) {
    throw 'AUTHENTICODE_SIGNING_REQUIRED: signing certificate password is not configured.'
}

$programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$signtool = (Get-ChildItem (Join-Path $programFilesX86 'Windows Kits\10\bin') -Filter signtool.exe -Recurse |
    Sort-Object FullName -Descending | Select-Object -First 1).FullName
if ([string]::IsNullOrWhiteSpace($signtool)) { throw 'signtool.exe was not found' }

$useTimestamp = -not [string]::IsNullOrWhiteSpace($TimestampUrl)
$temporaryRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
} else {
    [IO.Path]::GetFullPath($env:RUNNER_TEMP)
}
$pfx = Join-Path $temporaryRoot ('cheburnet-release-' + [guid]::NewGuid().ToString('N') + '.pfx')
try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($CertificateBase64))
    if ($useTimestamp) {
        & $signtool sign /fd SHA256 /td SHA256 /tr $TimestampUrl /f $pfx /p $CertificatePassword $target
    } else {
        & $signtool sign /fd SHA256 /f $pfx /p $CertificatePassword $target
    }
    if ($LASTEXITCODE -ne 0) { throw 'signtool failed to sign the file' }
    & $signtool verify /pa /all /v $target
    if ($LASTEXITCODE -ne 0) { throw 'signtool could not verify the signature it just created' }
} finally {
    if (Test-Path -LiteralPath $pfx) { Remove-Item -LiteralPath $pfx -Force }
}

# Independent verification through the Windows signature API, not only through
# signtool's own report. A timestamp is required whenever one was configured.
$report = Assert-CheburnetSignedLauncher -Path $target -RequireTimestamp:$useTimestamp
Write-Output ("AUTHENTICODE_SIGN: PASS status=$($report.Status) " +
              "timestamped=$($report.Timestamped) sha256=$($report.Sha256) " +
              "subject=$($report.Subject)")

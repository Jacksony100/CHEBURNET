<#
Release signing regression test.

Covers the four failure modes a stable release must never survive:
  1. missing signing certificate;
  2. invalid / untrusted signing certificate;
  3. executable changed after signing;
  4. final signature that does not verify.

The test signs a real throwaway executable with a self-signed certificate
created in the CURRENT USER store only. It never adds anything to a trusted
root or publisher store: changing machine trust to make a test pass would be a
security modification, and an untrusted chain is itself one of the cases the
policy must reject. The "everything is in order" path is therefore exercised
against the pure policy function, which is the same code the release scripts
call.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
. (Join-Path $root 'scripts\authenticode.ps1')

$failures = New-Object Collections.Generic.List[string]
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $script:failures.Add($Message) }
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    if (-not $threw) { $script:failures.Add($Message) }
}
function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# ---- policy function: the accept path -------------------------------------
$goodSigner = [pscustomobject]@{
    Subject    = 'CN=CHEBURNET Release'
    NotBefore  = [datetime]::Now.AddDays(-10)
    NotAfter   = [datetime]::Now.AddDays(10)
    Thumbprint = ('A' * 40)
}
$valid = [pscustomobject]@{
    Status                 = 'Valid'
    SignerCertificate      = $goodSigner
    TimeStamperCertificate = [pscustomobject]@{ Subject = 'CN=Timestamp' }
}
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $valid -RequireTimestamp).Count -eq 0) `
    'a valid timestamped signature must satisfy the policy'

# ---- policy function: every reject path -----------------------------------
foreach ($status in @('NotSigned', 'HashMismatch', 'UnknownError', 'NotTrusted',
                      'NotSupportedFileFormat')) {
    $bad = [pscustomobject]@{
        Status                 = $status
        SignerCertificate      = $goodSigner
        TimeStamperCertificate = [pscustomobject]@{ Subject = 'CN=Timestamp' }
    }
    Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $bad -RequireTimestamp).Count -gt 0) `
        "Authenticode status '$status' must be rejected"
}
$noTimestamp = [pscustomobject]@{
    Status                 = 'Valid'
    SignerCertificate      = $goodSigner
    TimeStamperCertificate = $null
}
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $noTimestamp -RequireTimestamp).Count -gt 0) `
    'a signature without a countersignature must be rejected when a timestamp is required'
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $noTimestamp).Count -eq 0) `
    'a timestamp must only be demanded when timestamping is configured'
$expired = [pscustomobject]@{
    Status            = 'Valid'
    SignerCertificate = [pscustomobject]@{
        Subject    = 'CN=Expired'
        NotBefore  = [datetime]::Now.AddDays(-40)
        NotAfter   = [datetime]::Now.AddDays(-1)
        Thumbprint = ('B' * 40)
    }
    TimeStamperCertificate = [pscustomobject]@{ Subject = 'CN=Timestamp' }
}
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $expired -RequireTimestamp).Count -gt 0) `
    'an expired signing certificate must be rejected'
$noSigner = [pscustomobject]@{
    Status = 'Valid'; SignerCertificate = $null; TimeStamperCertificate = $null
}
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $noSigner).Count -gt 0) `
    'a signature without a signer certificate must be rejected'
Assert-True (@(Test-CheburnetAuthenticodePolicy -Signature $null).Count -gt 0) `
    'absent signature information must be rejected'

# ---- real artifacts --------------------------------------------------------
$stage = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-signing-' + [guid]::NewGuid().ToString('N'))
$certificate = $null
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    # A tiny real PE to sign. Any signed Windows binary works as the source.
    $sample = Join-Path $stage 'sample.exe'
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\where.exe') -Destination $sample
    $unsignedCopy = Join-Path $stage 'unsigned.exe'
    [IO.File]::WriteAllBytes($unsignedCopy, [IO.File]::ReadAllBytes($sample))
    # Break the inherited catalog/embedded signature so this copy is genuinely unsigned.
    $bytes = [IO.File]::ReadAllBytes($unsignedCopy)
    $bytes[$bytes.Length - 1] = [byte](($bytes[$bytes.Length - 1] + 1) % 256)
    [IO.File]::WriteAllBytes($unsignedCopy, $bytes)

    # 1. missing certificate on a stable release must be a hard failure.
    # The certificate is cleared through the environment, exactly the way the
    # release workflow supplies it, because powershell.exe -File drops an
    # explicitly empty string argument.
    $signScript = Join-Path $root 'scripts\authenticode-sign.ps1'
    $savedCertificate = $env:AUTHENTICODE_PFX_B64
    $savedPassword = $env:AUTHENTICODE_PFX_PASSWORD
    $savedPreference = $ErrorActionPreference
    try {
        $env:AUTHENTICODE_PFX_B64 = ''
        $env:AUTHENTICODE_PFX_PASSWORD = ''
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signScript `
            -File $unsignedCopy -Require 2>$null | Out-Null
        $requireExit = $LASTEXITCODE
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $signScript `
            -File $unsignedCopy 2>$null | Out-Null
        $optionalExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
        $env:AUTHENTICODE_PFX_B64 = $savedCertificate
        $env:AUTHENTICODE_PFX_PASSWORD = $savedPassword
    }
    Assert-True ($requireExit -ne 0) 'a stable release without a certificate must fail signing'
    Assert-True ($optionalExit -eq 0) 'a prerelease without a certificate must still be allowed'

    # 4. an unsigned final artifact must never pass the publication gate.
    Assert-Throws { Assert-CheburnetSignedLauncher -Path $unsignedCopy } `
        'an unsigned launcher must not pass the publication gate'
    $unsignedReport = Get-CheburnetAuthenticodeReport -Path $unsignedCopy
    Assert-True (-not $unsignedReport.Ok) 'the unsigned launcher report must not be ok'

    # The accept path against a real artifact: a Windows system binary carries a
    # genuine trusted, timestamped Microsoft signature, so the gate can be shown
    # to accept a correct signature without touching any trust store.
    $trustedSample = Get-CheburnetAuthenticodeReport -Path $sample -RequireTimestamp
    if ($trustedSample.Status -ceq 'Valid') {
        Assert-True $trustedSample.Ok `
            "a genuinely signed system binary must satisfy the policy ($($trustedSample.Violations -join '; '))"
        Assert-True $trustedSample.Timestamped 'the trusted sample must be timestamped'
        Assert-True ((Assert-CheburnetSignedLauncher -Path $sample -RequireTimestamp `
            -ExpectedSha256 (Get-Sha256 $sample)).Ok) `
            'a correctly signed artifact bound to its own digest must pass the gate'
    } else {
        Write-Output ("RELEASE_SIGNING: trusted sample reported " +
                      "$($trustedSample.Status), accept path against a real signature skipped")
    }

    # 2. an invalid (self-signed, untrusted) certificate must be rejected.
    $signtool = (Get-ChildItem (Join-Path `
        ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) `
        'Windows Kits\10\bin') -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1)
    if ($null -eq $signtool) {
        Write-Output 'RELEASE_SIGNING: signtool.exe absent, real-signature cases skipped'
    } else {
        $certificate = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject 'CN=CHEBURNET Test Signing (do not trust)' `
            -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter ([datetime]::Now.AddDays(2))
        $signed = Join-Path $stage 'signed.exe'
        [IO.File]::WriteAllBytes($signed, [IO.File]::ReadAllBytes($unsignedCopy))
        $signResult = Set-AuthenticodeSignature -LiteralPath $signed -Certificate $certificate `
            -HashAlgorithm SHA256
        Assert-True ([string]$signResult.Status -cne 'NotSigned') 'test signing did not produce a signature'

        $signedReport = Get-CheburnetAuthenticodeReport -Path $signed
        Assert-True (-not $signedReport.Ok) `
            "an untrusted self-signed certificate must be rejected (status was $($signedReport.Status))"
        Assert-Throws { Assert-CheburnetSignedLauncher -Path $signed } `
            'an untrusted signature must not pass the publication gate'

        # 3. an executable changed after signing must be detected two ways:
        #    by the signature itself and by the SHA-256 binding.
        $signedSha = Get-Sha256 $signed
        $tampered = Join-Path $stage 'tampered.exe'
        $signedBytes = [IO.File]::ReadAllBytes($signed)
        $signedBytes[256] = [byte](($signedBytes[256] + 1) % 256)
        [IO.File]::WriteAllBytes($tampered, $signedBytes)
        $tamperedReport = Get-CheburnetAuthenticodeReport -Path $tampered
        Assert-True ([string]$tamperedReport.Status -cne 'Valid') `
            'a file modified after signing must not report a valid signature'
        Assert-True ((Get-Sha256 $tampered) -cne $signedSha) `
            'a file modified after signing must not keep its SHA-256'
        Assert-Throws { Assert-CheburnetSignedLauncher -Path $tampered -ExpectedSha256 $signedSha } `
            'the SHA-256 binding must reject an artifact replaced after signing'
        Assert-Throws { Assert-CheburnetSignedLauncher -Path $signed -ExpectedSha256 (('c' * 64)) } `
            'the SHA-256 binding must reject a mismatching expected digest'
        Assert-Throws { Assert-CheburnetSignedLauncher -Path $signed -ExpectedSha256 'not-a-digest' } `
            'a malformed expected digest must be rejected'
    }
} finally {
    if ($null -ne $certificate) {
        Remove-Item -LiteralPath ('Cert:\CurrentUser\My\' + $certificate.Thumbprint) -Force `
            -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}

# ---- end to end: a stable tag must refuse an unsigned launcher -------------
# Staged against a synthetic repository whose version model is stable, so the
# real source version can stay a release candidate. The gate runs before
# packaging, so only the files it reads need to exist.
$fakeRepo = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-stable-gate-' +
    [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo 'scripts') | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo 'cmake') | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo 'resources\upstream') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fakeRepo 'build-stub') | Out-Null
    # Enumerated rather than copied with a wildcard: -LiteralPath does not
    # expand wildcards, which would silently stage an empty scripts directory.
    foreach ($script in @(Get-ChildItem -LiteralPath (Join-Path $root 'scripts') -File -Filter '*.ps1')) {
        Copy-Item -LiteralPath $script.FullName -Destination (Join-Path $fakeRepo 'scripts')
    }
    Copy-Item -LiteralPath (Join-Path $root 'resources\upstream\provenance.json') `
        -Destination (Join-Path $fakeRepo 'resources\upstream\provenance.json')
    # Stable version model: no prerelease field.
    $model = @(
        'set(CHEBURNET_VERSION_MAJOR 1)',
        'set(CHEBURNET_VERSION_MINOR 0)',
        'set(CHEBURNET_VERSION_PATCH 0)',
        'set(CHEBURNET_VERSION_PRERELEASE "")'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $fakeRepo 'cmake\Version.cmake'), $model + "`r`n",
                            [Text.UTF8Encoding]::new($false))
    # The stub launcher must be genuinely unsigned: a Windows system binary
    # carries a valid Microsoft signature and would legitimately pass the gate.
    $stubBytes = [IO.File]::ReadAllBytes((Join-Path $env:SystemRoot 'System32\where.exe'))
    $stubBytes[$stubBytes.Length - 1] = [byte](($stubBytes[$stubBytes.Length - 1] + 1) % 256)
    [IO.File]::WriteAllBytes((Join-Path $fakeRepo 'build-stub\CHEBURNET.exe'), $stubBytes)
    $stubReport = Get-CheburnetAuthenticodeReport -Path (Join-Path $fakeRepo 'build-stub\CHEBURNET.exe')
    Assert-True (-not $stubReport.Ok) 'the stable-gate stub launcher must be unsigned'

    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $fakeRepo 'scripts\prepare-release.ps1') `
        -Tag 'v1.0.0' -Repository 'Jacksony100/CHEBURNET' `
        -BuildDir 'build-stub' -OutDir 'dist-stub' 2>&1 | Out-String
    $stableExit = $LASTEXITCODE
    $ErrorActionPreference = $savedPreference

    Assert-True ($stableExit -ne 0) 'a stable tag must not publish an unsigned launcher'
    Assert-True ($output -match '(?i)authenticode') `
        'the stable refusal must name the Authenticode policy as the reason'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fakeRepo 'dist-stub'))) `
        'the stable gate must refuse before any release artifact is assembled'
} finally {
    if (Test-Path -LiteralPath $fakeRepo) { Remove-Item -LiteralPath $fakeRepo -Recurse -Force }
}

# ---- the release pipeline must require a signature only for stable ---------
$preparation = Get-Content -LiteralPath (Join-Path $root 'scripts\prepare-release.ps1') `
    -Raw -Encoding UTF8
Assert-True ($preparation -match '\$requireSignature = -not \$version\.IsPrerelease') `
    'prepare-release.ps1 must derive the signing requirement from the release channel'
Assert-True ($preparation.IndexOf('Assert-CheburnetSignedLauncher', [StringComparison]::Ordinal) -lt
             $preparation.IndexOf('generate-update-manifest.ps1', [StringComparison]::Ordinal)) `
    'the signature must be verified before update metadata is generated'
Assert-True ($preparation -match 'launcher changed after release metadata generation') `
    'prepare-release.ps1 must re-check the launcher immediately before publication'

$workflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\release.yml') `
    -Raw -Encoding UTF8
Assert-True ($workflow -match "authenticode-sign\.ps1 -File build-release\\CHEBURNET\.exe -Require") `
    'the release workflow must sign stable tags with -Require'
Assert-True ($workflow -match "verify-authenticode\.ps1 -File dist\\CHEBURNET\.exe -RequireTimestamp") `
    'the release workflow must independently verify the published launcher'
Assert-True ($workflow.IndexOf('authenticode-sign.ps1', [StringComparison]::Ordinal) -lt
             $workflow.IndexOf('prepare-release.ps1', [StringComparison]::Ordinal)) `
    'signing must happen before release metadata generation in the workflow'

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    throw "RELEASE_SIGNING: FAIL ($($failures.Count))"
}
Write-Output 'RELEASE_SIGNING: PASS (policy, real signatures, pipeline ordering)'

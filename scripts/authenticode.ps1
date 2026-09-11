<#
Shared Authenticode policy for CHEBURNET release automation.

The policy decision is a pure function over a signature object so it can be
tested exhaustively without changing machine trust stores: a locally created
self-signed certificate never chains to a trusted root, so a real end-to-end
"Valid" result cannot be produced on a developer machine without modifying
system trust, which release tooling must never do.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.

Dot-source it:  . "$PSScriptRoot\authenticode.ps1"
#>
Set-StrictMode -Version Latest

function Test-CheburnetAuthenticodePolicy {
    <#
    Returns the list of policy violations for one signature object. An empty
    list means the artifact satisfies the stable-release signing policy.

    $Signature is what Get-AuthenticodeSignature returns (or an equivalent
    object carrying Status, SignerCertificate and TimeStamperCertificate).
    #>
    param(
        # AllowNull: "no signature information at all" is itself a case the
        # policy must reject, not a parameter-binding error.
        [Parameter(Mandatory = $true)][AllowNull()]$Signature,
        [switch]$RequireTimestamp
    )
    $violations = New-Object Collections.Generic.List[string]
    if ($null -eq $Signature) {
        $violations.Add('no Authenticode signature information was returned')
        return $violations
    }
    $status = [string]$Signature.Status
    if ($status -cne 'Valid') {
        # NotSigned, HashMismatch (the file changed after signing),
        # UnknownError (untrusted chain), NotTrusted, and every other status
        # are all rejections. There is deliberately no "warn and continue".
        $violations.Add("Authenticode status is '$status', expected 'Valid'")
    }
    $signer = $Signature.SignerCertificate
    if ($null -eq $signer) {
        $violations.Add('signature carries no signer certificate')
    } else {
        $notBefore = [datetime]$signer.NotBefore
        $notAfter = [datetime]$signer.NotAfter
        $now = [datetime]::Now
        if ($now -lt $notBefore -or $now -gt $notAfter) {
            $violations.Add("signer certificate is outside its validity window ($notBefore .. $notAfter)")
        }
        if ([string]::IsNullOrWhiteSpace([string]$signer.Subject)) {
            $violations.Add('signer certificate has no subject')
        }
    }
    if ($RequireTimestamp) {
        # A countersignature is what keeps the artifact verifiable after the
        # signing certificate expires. If timestamping is configured it must
        # have actually happened.
        if ($null -eq $Signature.TimeStamperCertificate) {
            $violations.Add('signature is not timestamped')
        }
    }
    return $violations
}

function Get-CheburnetAuthenticodeReport {
    <#
    Applies the policy to a file and returns a report object. Never throws on a
    policy failure -- the caller decides whether the channel requires a
    signature. Throws only when the file itself cannot be inspected.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$RequireTimestamp
    )
    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "file not found for Authenticode verification: $full"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $full
    $violations = @(Test-CheburnetAuthenticodePolicy -Signature $signature `
        -RequireTimestamp:$RequireTimestamp)
    $subject = if ($null -ne $signature.SignerCertificate) {
        [string]$signature.SignerCertificate.Subject
    } else { '' }
    $thumbprint = if ($null -ne $signature.SignerCertificate) {
        [string]$signature.SignerCertificate.Thumbprint
    } else { '' }
    return [pscustomobject]@{
        Path        = $full
        Status      = [string]$signature.Status
        Subject     = $subject
        Thumbprint  = $thumbprint
        Timestamped = ($null -ne $signature.TimeStamperCertificate)
        Sha256      = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
        Violations  = $violations
        Ok          = ($violations.Count -eq 0)
    }
}

function Assert-CheburnetSignedLauncher {
    <#
    Stable-release gate. Throws unless the file satisfies the signing policy.
    When -ExpectedSha256 is supplied, also proves the inspected file is byte
    for byte the artifact that was signed -- this is what catches an executable
    that was rebuilt, re-copied or modified after signing.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$RequireTimestamp,
        [string]$ExpectedSha256
    )
    $report = Get-CheburnetAuthenticodeReport -Path $Path -RequireTimestamp:$RequireTimestamp
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
        if ($ExpectedSha256 -cnotmatch '^[a-f0-9]{64}$') {
            throw 'ExpectedSha256 must be a lowercase hexadecimal SHA-256 digest'
        }
        if ($report.Sha256 -cne $ExpectedSha256) {
            throw ("signed artifact mismatch for $($report.Path): expected SHA-256 " +
                   "$ExpectedSha256, found $($report.Sha256)")
        }
    }
    if (-not $report.Ok) {
        throw ("Authenticode policy rejected $($report.Path): " +
               ($report.Violations -join '; '))
    }
    return $report
}

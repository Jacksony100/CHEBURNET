[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$SignatureOut,
    [string]$KeyId = 'cheburnet-release-2026'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$manifestPath = [IO.Path]::GetFullPath($Manifest)
$signaturePath = [IO.Path]::GetFullPath($SignatureOut)
foreach ($candidate in @($manifestPath,$signaturePath)) {
    if (-not $candidate.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'manifest/signature paths must be inside repository'
    }
}
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
    ((Get-Item -LiteralPath $manifestPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'manifest is missing or a reparse point'
}
$bytes = [IO.File]::ReadAllBytes($manifestPath)
$manifestDocument = [Text.UTF8Encoding]::new($false, $true).GetString($bytes) | ConvertFrom-Json
if ([string]$manifestDocument.key_id -cne $KeyId) {
    throw 'manifest key_id does not match selected signing key'
}
$trust = Get-Content -LiteralPath (Join-Path $root 'resources\update\release-public-key.json') `
    -Raw -Encoding UTF8 | ConvertFrom-Json
$keysProperty = $trust.PSObject.Properties['keys']
$trustedKeys = @(if ($null -ne $keysProperty) { @($keysProperty.Value) } else { @($trust) })
$trusted = @($trustedKeys | Where-Object { [string]$_.key_id -ceq $KeyId })
if ($trusted.Count -ne 1 -or [string]$trusted[0].algorithm -cne 'ECDSA_P256_SHA256') {
    throw 'selected signing key is not present exactly once in the embedded trust source'
}
$private = $null
if (-not [string]::IsNullOrWhiteSpace($env:CHEBURNET_SIGNING_KEY_CNG_BLOB_B64)) {
    $private = [Convert]::FromBase64String($env:CHEBURNET_SIGNING_KEY_CNG_BLOB_B64)
} else {
    $path = Join-Path $env:LOCALAPPDATA "CHEBURNET\release-keys\$KeyId.pkcs8.dpapi"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'signing key unavailable; configure CHEBURNET_SIGNING_KEY_CNG_BLOB_B64 secret'
    }
    $protected = [IO.File]::ReadAllBytes($path)
    $private = [Security.Cryptography.ProtectedData]::Unprotect(
        $protected, [Text.Encoding]::UTF8.GetBytes('CHEBURNET release key'),
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
}
$key = $null
try {
    $cng = [Security.Cryptography.CngKey]::Import(
        $private, [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
    $key = [Security.Cryptography.ECDsaCng]::new($cng)
    $publicBlob = $cng.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    if ($publicBlob.Length -ne 72) { throw 'unexpected ECDSA P-256 public blob' }
    $actualX = [Convert]::ToBase64String($publicBlob[8..39])
    $actualY = [Convert]::ToBase64String($publicBlob[40..71])
    if ($actualX -cne [string]$trusted[0].x -or
        $actualY -cne [string]$trusted[0].y) {
        throw 'private signing key does not match the embedded public key'
    }
    $signature = $key.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256)
    if ($signature.Length -ne 64) { throw 'unexpected ECDSA signature format' }
    if (-not $key.VerifyData($bytes, $signature,
                             [Security.Cryptography.HashAlgorithmName]::SHA256)) {
        throw 'post-signature verification failed'
    }
    [IO.File]::WriteAllText($signaturePath,
        [Convert]::ToBase64String($signature) + "`n", [Text.Encoding]::ASCII)
} finally {
    if ($null -ne $private) { [Array]::Clear($private, 0, $private.Length) }
    if ($null -ne $key) { $key.Dispose() }
}
Write-Output "Signed exact manifest bytes -> $SignatureOut"

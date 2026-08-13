[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$Signature,
    [string]$KeyId = 'cheburnet-release-2026'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Security

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
function Resolve-RepositoryFile([string]$Value) {
    $path = [IO.Path]::GetFullPath($Value)
    if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'verification path must be inside repository'
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "verification input is missing: $path"
    }
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "verification input is a reparse point: $path"
    }
    return $path
}

$manifestPath = Resolve-RepositoryFile $Manifest
$signaturePath = Resolve-RepositoryFile $Signature
$trustPath = Join-Path $root 'resources\update\release-public-key.json'
$trust = Get-Content -LiteralPath $trustPath -Raw -Encoding UTF8 | ConvertFrom-Json
$keysProperty = $trust.PSObject.Properties['keys']
$trustedKeys = @(if ($null -ne $keysProperty) { @($keysProperty.Value) } else { @($trust) })
$selected = @($trustedKeys | Where-Object { [string]$_.key_id -ceq $KeyId })
if ($selected.Count -ne 1 -or [string]$selected[0].algorithm -cne 'ECDSA_P256_SHA256') {
    throw 'selected verification key is not present exactly once'
}

$x = [Convert]::FromBase64String([string]$selected[0].x)
$y = [Convert]::FromBase64String([string]$selected[0].y)
if ($x.Length -ne 32 -or $y.Length -ne 32) { throw 'invalid ECDSA P-256 public coordinates' }
$blob = New-Object byte[] 72
[Array]::Copy([BitConverter]::GetBytes([uint32]0x31534345), 0, $blob, 0, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]32), 0, $blob, 4, 4)
[Array]::Copy($x, 0, $blob, 8, 32)
[Array]::Copy($y, 0, $blob, 40, 32)

$manifestBytes = [IO.File]::ReadAllBytes($manifestPath)
$signatureText = [IO.File]::ReadAllText($signaturePath, [Text.Encoding]::ASCII).Trim()
$signatureBytes = [Convert]::FromBase64String($signatureText)
if ($signatureBytes.Length -ne 64) { throw 'invalid ECDSA signature size' }

$key = $null
try {
    $cng = [Security.Cryptography.CngKey]::Import(
        $blob, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    $key = [Security.Cryptography.ECDsaCng]::new($cng)
    if (-not $key.VerifyData($manifestBytes, $signatureBytes,
                             [Security.Cryptography.HashAlgorithmName]::SHA256)) {
        throw 'manifest signature verification failed'
    }
} finally {
    if ($null -ne $key) { $key.Dispose() }
    [Array]::Clear($blob, 0, $blob.Length)
    [Array]::Clear($x, 0, $x.Length)
    [Array]::Clear($y, 0, $y.Length)
}
Write-Output "UPDATE_SIGNATURE: PASS key_id=$KeyId"

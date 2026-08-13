[CmdletBinding()]
param(
    [string]$KeyId = 'cheburnet-release-2026',
    [string]$PublicKeyOut = 'resources\update\release-public-key.json'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$root = Split-Path -Parent $PSScriptRoot
$publicPath = if ([IO.Path]::IsPathRooted($PublicKeyOut)) { $PublicKeyOut }
              else { Join-Path $root $PublicKeyOut }
$privateDir = Join-Path $env:LOCALAPPDATA 'CHEBURNET\release-keys'
$privatePath = Join-Path $privateDir ($KeyId + '.pkcs8.dpapi')
if (Test-Path -LiteralPath $privatePath) { throw "private key already exists: $privatePath" }

$key = [Security.Cryptography.ECDsaCng]::new(256)
try {
    $parameters = $key.ExportParameters($true)
    # CNG private blob is DPAPI-protected at rest and never committed.
    $pkcs8 = $key.Key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
    $protected = [Security.Cryptography.ProtectedData]::Protect(
        $pkcs8, [Text.Encoding]::UTF8.GetBytes('CHEBURNET release key'),
        [Security.Cryptography.DataProtectionScope]::CurrentUser)
    New-Item -ItemType Directory -Path $privateDir -Force | Out-Null
    [IO.File]::WriteAllBytes($privatePath, $protected)
    # Current user + SYSTEM only. No inherited ACL.
    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $user, [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.AccessControlType]::Allow))
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $system, [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $privatePath -AclObject $acl

    $public = [ordered]@{
        key_id = $KeyId
        algorithm = 'ECDSA_P256_SHA256'
        x = [Convert]::ToBase64String($parameters.Q.X)
        y = [Convert]::ToBase64String($parameters.Q.Y)
        created_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $publicPath) -Force | Out-Null
    [IO.File]::WriteAllText($publicPath, ($public | ConvertTo-Json) + "`n",
                            [Text.UTF8Encoding]::new($false))
} finally {
    if ($null -ne $pkcs8) { [Array]::Clear($pkcs8, 0, $pkcs8.Length) }
    $key.Dispose()
}
Write-Output "Generated public release key: $publicPath"
Write-Output "DPAPI-protected private key (outside repository): $privatePath"

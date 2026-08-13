[CmdletBinding()]
param([string]$Root)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Root)) { $Root = Split-Path -Parent $PSScriptRoot }
$required = @(
    'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'SECURITY.md',
    'docs\PRIVACY.md', 'docs\UPDATE_SECURITY.md',
    'LICENSES\Flowseal-MIT.txt', 'LICENSES\zapret-MIT.txt',
    'LICENSES\LGPL-3.0.txt', 'LICENSES\GPL-2.0.txt'
)
foreach ($relative in $required) {
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing license: $relative" }
    if ((Get-Item -LiteralPath $path).Length -lt 100) { throw "truncated license: $relative" }
}
$notice = Get-Content -LiteralPath (Join-Path $Root 'THIRD_PARTY_NOTICES.md') -Raw -Encoding UTF8
foreach ($name in @('Flowseal','bol-van','WinDivert','Cygwin')) {
    if ($notice -notmatch [regex]::Escape($name)) { throw "notice missing component: $name" }
}
$forbidden = Get-ChildItem -LiteralPath $Root -File -Recurse | Where-Object {
    $_.FullName -notmatch '\\build[^\\]*\\' -and $_.FullName -notmatch '\\dist\\' -and
    $_.Name -match '(?i)(private.*key|signing.*key).*(pem|pfx|p12|key)$'
}
if ($forbidden) { throw "private signing material found in repository: $($forbidden.FullName)" }
Write-Output 'LICENSE_VALIDATION: PASS'

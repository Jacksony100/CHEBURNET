[CmdletBinding()]
param([string]$OutputDirectory = 'LICENSES')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory }
          else { Join-Path $root $OutputDirectory }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$headers = @{ 'User-Agent' = 'CHEBURNET-license-import' }
$version = [string](Get-Content -LiteralPath (Join-Path $root 'resources\upstream\provenance.json') `
    -Raw -Encoding UTF8 | ConvertFrom-Json).version
if ($version -notmatch '^[0-9]+(?:\.[0-9]+){1,7}[a-z]?$') { throw 'invalid upstream provenance version' }
$sources = [ordered]@{
    'Flowseal-MIT.txt' = "https://raw.githubusercontent.com/Flowseal/zapret-discord-youtube/$version/LICENSE.txt"
    'zapret-MIT.txt' = 'https://raw.githubusercontent.com/bol-van/zapret/master/docs/LICENSE.txt'
    'LGPL-3.0.txt' = 'https://www.gnu.org/licenses/lgpl-3.0.txt'
    'GPL-2.0.txt' = 'https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt'
}
foreach ($name in $sources.Keys) {
    $response = Invoke-WebRequest -Uri $sources[$name] -Headers $headers -UseBasicParsing
    if ($response.StatusCode -ne 200 -or $response.RawContentLength -lt 500) {
        throw "license download failed: $name"
    }
    [IO.File]::WriteAllText((Join-Path $output $name), $response.Content,
                            [Text.UTF8Encoding]::new($false))
}
Write-Output "Imported $($sources.Count) authoritative license texts into $output"

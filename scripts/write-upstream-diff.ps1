# File-system preparation belongs to the writer, not the caller.
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$diff = & git -C $Root diff --stat -- resources
if ($LASTEXITCODE -ne 0) { throw 'cannot read upstream diff' }
$status = & git -C $Root status --porcelain -- resources
if ($LASTEXITCODE -ne 0) { throw 'cannot read upstream status' }
[IO.File]::WriteAllText((Join-Path ([IO.Path]::GetFullPath($OutDir)) 'payload-diff.txt'),
    ($diff -join [Environment]::NewLine) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path ([IO.Path]::GetFullPath($OutDir)) 'payload-status.txt'),
    ($status -join [Environment]::NewLine) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))

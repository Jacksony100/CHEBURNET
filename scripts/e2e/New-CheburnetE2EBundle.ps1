# Stage a fixed candidate for both clean Windows client guests. No elevation needed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$ExpectedSha256
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$launcherPath = (Resolve-Path -LiteralPath $Launcher).Path
$destination = [IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $destination) { throw "bundle destination already exists: $destination" }
$hash = (Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedSha256 -and $hash -cne $ExpectedSha256.ToLowerInvariant()) {
    throw 'candidate SHA-256 does not match the requested release'
}
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$revision = (& git -C $root rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0) { throw 'cannot determine source revision' }
$status = @(& git -C $root status --porcelain --untracked-files=normal)
if ($LASTEXITCODE -ne 0) { throw 'cannot determine source worktree status' }
New-Item -ItemType Directory -Path (Join-Path $destination 'release') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $destination 'scripts\e2e') -Force | Out-Null
Copy-Item -LiteralPath $launcherPath -Destination (Join-Path $destination 'release\CHEBURNET.exe')
[IO.File]::WriteAllText((Join-Path $destination 'release\CHEBURNET.exe.sha256'),
    "$hash  CHEBURNET.exe`r`n", [Text.Encoding]::ASCII)
foreach ($script in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File) {
    Copy-Item -LiteralPath $script.FullName -Destination (Join-Path $destination 'scripts\e2e')
}
Copy-Item -LiteralPath (Join-Path $root 'docs\production\E2E_RELEASE_GATE.md') `
    -Destination (Join-Path $destination 'E2E_RELEASE_GATE.md')
$files = @(Get-ChildItem -LiteralPath $destination -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($destination.TrimEnd('\').Length + 1).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifest = [ordered]@{
    schema = 1
    prepared_at = [DateTime]::UtcNow.ToString('o')
    source_revision = [string]$revision
    source_dirty = ($status.Count -ne 0)
    launcher_sha256 = $hash
    target_editions = @('win10', 'win11')
    files = $files
}
[IO.File]::WriteAllText((Join-Path $destination 'bundle-manifest.json'),
    ($manifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Output "E2E_BUNDLE: $destination SHA256=$hash"

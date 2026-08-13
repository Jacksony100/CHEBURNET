[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $RepositoryRoot 'scripts\package-path.ps1')

$bad = @(
    '', '.', '..', $RepositoryRoot, (Split-Path -Parent $RepositoryRoot), 'C:\',
    '\\server\share\', (Join-Path $RepositoryRoot '..\outside'),
    (Join-Path $RepositoryRoot 'dist\..\..\outside')
)
$failures = 0
foreach ($candidate in $bad) {
    try {
        $null = Assert-SafePackageOutDir $RepositoryRoot $candidate
        Write-Output "FAIL accepted unsafe OutDir: '$candidate'"
        $failures++
    } catch {
        Write-Output "PASS rejected OutDir: '$candidate'"
    }
}
foreach ($candidate in @('dist', 'artifacts\release', (Join-Path $RepositoryRoot 'out\rc'))) {
    try {
        $resolved = Assert-SafePackageOutDir $RepositoryRoot $candidate
        $normalizedRoot = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
        if (-not $resolved.StartsWith($normalizedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'not inside root'
        }
        Write-Output "PASS accepted OutDir: '$candidate'"
    } catch {
        Write-Output "FAIL rejected safe OutDir: '$candidate' -- $($_.Exception.Message)"
        $failures++
    }
}

# Existing reparse-point path components are never eligible for recursive
# cleanup. Prefer a symlink, then use an NTFS junction when Developer Mode does
# not permit unprivileged symlink creation.
$nonce = "package-safety-$PID-$([DateTime]::UtcNow.Ticks)"
$outside = Join-Path ([IO.Path]::GetTempPath()) ($nonce + '-outside')
$linkParent = Join-Path $RepositoryRoot 'artifacts'
$link = Join-Path $linkParent $nonce
New-Item -ItemType Directory -Path $outside -Force | Out-Null
New-Item -ItemType Directory -Path $linkParent -Force | Out-Null
try {
    $created = $false
    $reparseType = $null
    try {
        New-Item -ItemType SymbolicLink -Path $link -Target $outside -ErrorAction Stop | Out-Null
        $created = $true
        $reparseType = 'symbolic link'
    } catch {
        try {
            New-Item -ItemType Junction -Path $link -Target $outside -ErrorAction Stop | Out-Null
            $created = $true
            $reparseType = 'junction'
        } catch {
            Write-Output 'PASS reparse OutDir test skipped: neither symlink nor junction is available'
        }
    }
    if ($created) {
        try {
            $null = Assert-SafePackageOutDir $RepositoryRoot (Join-Path $link 'release')
            Write-Output 'FAIL accepted reparse-point OutDir component'
            $failures++
        } catch {
            Write-Output "PASS rejected reparse-point OutDir component ($reparseType)"
        }

        $tree = Join-Path $RepositoryRoot ("artifacts\$nonce-tree")
        New-Item -ItemType Directory -Path $tree | Out-Null
        $nested = Join-Path $tree 'nested-link'
        New-Item -ItemType $(if ($reparseType -eq 'junction') { 'Junction' } else { 'SymbolicLink' }) `
            -Path $nested -Target $outside -ErrorAction Stop | Out-Null
        try {
            Remove-SafePackageTree $RepositoryRoot $tree
            Write-Output 'FAIL deleted OutDir containing a nested reparse object'
            $failures++
        } catch {
            Write-Output 'PASS rejected nested reparse object before package cleanup'
        } finally {
            if (Test-Path -LiteralPath $nested) { Remove-Item -LiteralPath $nested -Force }
            if (Test-Path -LiteralPath $tree) { Remove-Item -LiteralPath $tree -Force }
        }
    }
} finally {
    if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
    if (Test-Path -LiteralPath $outside) { Remove-Item -LiteralPath $outside -Force }
}
if ($failures -ne 0) { exit 1 }
Write-Output 'PACKAGE_SAFETY: PASS'

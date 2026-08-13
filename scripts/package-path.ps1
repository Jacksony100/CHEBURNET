Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-NormalizedFullPath([string]$Path, [string]$BasePath) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Path must not be empty.' }
    $combined = if ([IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $BasePath $Path }
    $full = [IO.Path]::GetFullPath($combined).TrimEnd('\', '/')
    if ($full -match '^[A-Za-z]:$') { $full += '\' }
    return $full
}

function Assert-SafePackageOutDir([string]$RepositoryRoot, [string]$OutDir) {
    $root = Get-NormalizedFullPath $RepositoryRoot $RepositoryRoot
    $candidate = Get-NormalizedFullPath $OutDir $root
    $comparison = [StringComparison]::OrdinalIgnoreCase
    if ($candidate.Equals($root, $comparison)) { throw 'OutDir must not be the repository root.' }
    $prefix = $root.TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($prefix, $comparison)) {
        throw "OutDir must be a strict descendant of the repository root: $candidate"
    }
    $driveRoot = [IO.Path]::GetPathRoot($candidate).TrimEnd('\')
    if ($candidate.TrimEnd('\').Equals($driveRoot, $comparison)) {
        throw 'OutDir must not be a drive root.'
    }

    # Existing path components must never be junctions/symlinks.
    $cursor = $candidate
    while ($cursor.StartsWith($prefix, $comparison) -or $cursor.Equals($root, $comparison)) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "OutDir path contains a reparse point: $cursor"
            }
        }
        if ($cursor.Equals($root, $comparison)) { break }
        $cursor = Split-Path -Parent $cursor
    }
    return $candidate
}

function Remove-SafePackageTree([string]$RepositoryRoot, [string]$Target) {
    $safe = Assert-SafePackageOutDir $RepositoryRoot $Target
    if (-not (Test-Path -LiteralPath $safe)) { return }
    $item = Get-Item -LiteralPath $safe -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to delete a reparse-point OutDir: $safe"
    }
    # Validate descendants iteratively and never recurse through a reparse
    # object. Once this pass succeeds, Remove-Item cannot escape through a
    # junction/symlink already present in the release output tree.
    $pending = New-Object System.Collections.Generic.Stack[string]
    $pending.Push($safe)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($child in @(Get-ChildItem -LiteralPath $directory -Force)) {
            if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to delete OutDir containing reparse object: $($child.FullName)"
            }
            if ($child.PSIsContainer) { $pending.Push($child.FullName) }
        }
    }
    # LiteralPath prevents wildcard expansion. The verified target is an exact
    # strict descendant of the repository and the complete tree has no reparse.
    Remove-Item -LiteralPath $safe -Recurse -Force
}

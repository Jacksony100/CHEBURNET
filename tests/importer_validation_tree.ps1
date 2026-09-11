<#
Regression test for the upstream importer's isolated validation tree.

scripts/sync-upstream.ps1 validates a candidate payload inside a copy of the
repository that it assembles from an explicit file list. If that list drifts
away from what the release gates read, the import fails on a missing repository
file rather than on the imported payload -- which is exactly what happened when
docs/ and SECURITY.md were absent from the list.

This test rebuilds the same tree from the importer's own list (no network, no
payload download) and runs the release gates against it, so the list and the
gates cannot silently diverge again.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$importer = Get-Content -LiteralPath (Join-Path $root 'scripts\sync-upstream.ps1') `
    -Raw -Encoding UTF8

# Extract the importer's copy list from its own source, so the test can never
# pass against a list it does not actually use.
# Single-quoted on purpose: the pattern contains literal '$' from the importer.
$listMatch = [regex]::Match(
    $importer,
    'foreach \(\$name in @\((?<list>[^)]*)\)\) \{\s*\r?\n\s*\$sourceItem = Join-Path \$root \$name')
if (-not $listMatch.Success) {
    throw 'cannot locate the validation-tree copy list in scripts/sync-upstream.ps1'
}
$copyList = @([regex]::Matches($listMatch.Groups['list'].Value, "'([^']+)'") |
    ForEach-Object { $_.Groups[1].Value })
if ($copyList.Count -lt 5) { throw 'validation-tree copy list looks truncated' }

foreach ($required in @('CMakeLists.txt', 'cmake', 'src', 'tests', 'scripts', 'resources')) {
    if ($copyList -cnotcontains $required) {
        throw "validation-tree copy list is missing a build input: $required"
    }
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ('cheburnet-importer-tree-' +
    [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    $copied = 0
    foreach ($name in $copyList) {
        $source = Join-Path $root $name
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name) -Recurse
            $copied++
        }
    }
    if ($copied -lt 5) { throw 'validation tree staged too few entries' }

    # The gates the importer runs against the candidate tree must all be
    # satisfiable by the tree the importer builds.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $root 'scripts\validate-licenses.ps1') -Root $stage | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'importer validation tree cannot satisfy scripts/validate-licenses.ps1'
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $root 'tests\security_regression.ps1') -RepositoryRoot $stage | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'importer validation tree cannot satisfy tests/security_regression.ps1'
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $root 'tests\version_model.ps1') -RepositoryRoot $stage | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'importer validation tree cannot satisfy tests/version_model.ps1'
    }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
Write-Output ("IMPORTER_VALIDATION_TREE: PASS (" + $copyList.Count + ' entries, release gates satisfied)')

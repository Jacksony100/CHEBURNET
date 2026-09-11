<#
Regression test for inline-script encoding in GitHub Actions workflows.

GitHub writes every `run:` block to a temporary script file on the runner. For a
step with `shell: powershell`, that file is executed by Windows PowerShell 5.1,
which reads a file WITHOUT a byte-order mark as ANSI, not UTF-8. A single
non-ASCII character inside such a block therefore becomes mojibake and breaks
parsing -- the whole step fails before running anything.

This already happened twice in this repository (a Russian `throw` message in the
static-analysis and upstream-check workflows), so it is checked rather than
remembered.

Scope of the rule:
  * `run:` blocks of steps using powershell/pwsh -- ASCII only;
  * `run:` blocks of steps using bash/sh -- non-ASCII allowed (the runner reads
    those as UTF-8);
  * YAML `name:`, comments and other keys -- non-ASCII allowed, they are parsed
    as UTF-8 by the Actions runner, not by PowerShell.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$workflowDir = Join-Path $root '.github\workflows'
if (-not (Test-Path -LiteralPath $workflowDir -PathType Container)) {
    throw "workflow directory not found: $workflowDir"
}

$failures = New-Object Collections.Generic.List[string]
$checked = 0
$powershellBlocks = 0

foreach ($file in @(Get-ChildItem -LiteralPath $workflowDir -File -Filter '*.yml')) {
    $checked++
    $lines = @(Get-Content -LiteralPath $file.FullName -Encoding UTF8)
    # Default shell for a step, as declared by `shell:` anywhere above the
    # `run:` within the same step. Tracked line by line because a workflow mixes
    # powershell and bash steps.
    $currentShell = ''
    $inRun = $false
    $runIndent = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $trimmed = $line.Trim()
        $indent = $line.Length - $line.TrimStart().Length

        # A new step resets the remembered shell.
        if ($trimmed -match '^-\s+(name|uses):') {
            $currentShell = ''
            $inRun = $false
        }
        if ($trimmed -match '^shell:\s*(\S+)\s*$') {
            $currentShell = $Matches[1]
            continue
        }
        if ($trimmed -match '^run:\s*\|' -or $trimmed -match '^run:\s*>') {
            $inRun = $true
            $runIndent = $indent
            continue
        }
        if ($trimmed -match '^run:\s*\S') {
            # Single-line run.
            $isPowerShell = $currentShell -match '(?i)^(powershell|pwsh)'
            if ($isPowerShell -and $line -match '[^\x00-\x7F]') {
                $failures.Add("$($file.Name):$($i + 1) non-ASCII in a single-line powershell run")
            }
            continue
        }
        if ($inRun) {
            if ($trimmed -ne '' -and $indent -le $runIndent) {
                $inRun = $false
            } else {
                if ($currentShell -match '(?i)^(powershell|pwsh)') {
                    if ($line -match '[^\x00-\x7F]') {
                        $failures.Add("$($file.Name):$($i + 1) non-ASCII inside a powershell run block")
                    }
                }
            }
        }
    }
    # Count powershell run blocks for a sanity check on the parser itself.
    $powershellBlocks += @([regex]::Matches(
        (Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8),
        '(?m)^\s*shell:\s*(powershell|pwsh)\s*$')).Count
}

if ($checked -eq 0) { $failures.Add('no workflow files were inspected') }
# If this drops to zero the parser stopped recognising powershell steps and the
# test would pass vacuously.
if ($powershellBlocks -eq 0) {
    $failures.Add('no powershell steps were recognised; the workflow parser is broken')
}

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    throw "WORKFLOW_ENCODING: FAIL ($($failures.Count))"
}
Write-Output ("WORKFLOW_ENCODING: PASS ($checked workflows, $powershellBlocks powershell steps, " +
              'inline PowerShell is ASCII-only)')

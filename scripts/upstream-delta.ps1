<#
Payload delta derived from `git status --porcelain` after an upstream import.

Kept separate from scripts/upstream-report.ps1 so the parsing can be tested
offline against fixtures: this is the part that silently goes wrong when git
quotes a path or reports a rename.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.

Dot-source it:  . "$PSScriptRoot\upstream-delta.ps1"
#>
Set-StrictMode -Version Latest

function Get-CheburnetPayloadDelta {
    <#
    $PorcelainLines is the raw output of
        git status --porcelain -- resources/strategies_src resources/payload
    Returns the six name lists plus the resulting strategy counts.
    #>
    param(
        # git emits blank lines in some shells; they are data to skip, not a
        # parameter-binding error.
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()][AllowEmptyString()][string[]]$PorcelainLines,
        [int]$StrategyCountAfter = 0
    )
    $result = [ordered]@{
        StrategiesAdded    = New-Object Collections.Generic.List[string]
        StrategiesRemoved  = New-Object Collections.Generic.List[string]
        StrategiesModified = New-Object Collections.Generic.List[string]
        PayloadAdded       = New-Object Collections.Generic.List[string]
        PayloadRemoved     = New-Object Collections.Generic.List[string]
        PayloadModified    = New-Object Collections.Generic.List[string]
    }
    foreach ($line in $PorcelainLines) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) { continue }
        $code = $line.Substring(0, 2)
        $rest = $line.Substring(3)
        # A rename is reported as "R  old -> new"; the new path is what matters.
        $arrow = $rest.IndexOf(' -> ', [StringComparison]::Ordinal)
        if ($arrow -ge 0) { $rest = $rest.Substring($arrow + 4) }
        # git quotes paths containing spaces or non-ASCII bytes.
        $path = $rest.Trim()
        if ($path.StartsWith('"') -and $path.EndsWith('"') -and $path.Length -ge 2) {
            $path = $path.Substring(1, $path.Length - 2)
        }
        $path = $path.Replace('\\', '/')
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        $name = $path.Substring($path.LastIndexOf('/') + 1)

        $isStrategy = $path.StartsWith('resources/strategies_src/', [StringComparison]::OrdinalIgnoreCase)
        $isPayload = $path.StartsWith('resources/payload/', [StringComparison]::OrdinalIgnoreCase)
        if (-not $isStrategy -and -not $isPayload) { continue }

        $trimmed = $code.Trim()
        $bucket = if ($trimmed -ceq 'D') { 'Removed' }
                  elseif ($trimmed -ceq '??' -or $trimmed -ceq 'A' -or $trimmed -ceq 'R') { 'Added' }
                  else { 'Modified' }
        $key = if ($isStrategy) { "Strategies$bucket" } else { "Payload$bucket" }
        if (-not $result[$key].Contains($name)) { $result[$key].Add($name) }
    }
    $after = $StrategyCountAfter
    $before = $after - $result['StrategiesAdded'].Count + $result['StrategiesRemoved'].Count
    return [pscustomobject]@{
        StrategiesAdded     = $result['StrategiesAdded'].ToArray()
        StrategiesRemoved   = $result['StrategiesRemoved'].ToArray()
        StrategiesModified  = $result['StrategiesModified'].ToArray()
        PayloadAdded        = $result['PayloadAdded'].ToArray()
        PayloadRemoved      = $result['PayloadRemoved'].ToArray()
        PayloadModified     = $result['PayloadModified'].ToArray()
        StrategyCountAfter  = $after
        StrategyCountBefore = $before
    }
}

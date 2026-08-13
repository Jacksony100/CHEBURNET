# Independent raw-BAT to compiled-argv fidelity check. Strategy count and the
# full four-mode GameFilter matrix are discovered at runtime.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BatDir,
    [Parameter(Mandatory = $true)][string]$TestExe
)
$ErrorActionPreference = 'Stop'

function Get-StrategyId([string]$fileName) {
    if ($fileName -ieq 'general.bat') { return 'general' }
    if ($fileName -notmatch '^general \((.+)\)\.bat$') { throw "unsupported name: $fileName" }
    $label = $Matches[1]
    if ($label -match '^FAKE TLS AUTO( ALT\d*)?$') {
        $suffix = if ($Matches[1]) { '_' + $Matches[1].Trim().ToLowerInvariant() } else { '' }
        return 'faketls_auto' + $suffix
    }
    return (($label.ToLowerInvariant() -replace '[^a-z0-9]+', '_').Trim('_'))
}

function Split-Tokens([string]$command) {
    $out = New-Object System.Collections.Generic.List[string]
    $cur = New-Object System.Text.StringBuilder
    $quoted = $false
    foreach ($c in $command.ToCharArray()) {
        if ($c -eq '"') { $quoted = -not $quoted; continue }
        if (-not $quoted -and [char]::IsWhiteSpace($c)) {
            if ($cur.Length -gt 0) { $out.Add($cur.ToString()); [void]$cur.Clear() }
        } else { [void]$cur.Append($c) }
    }
    if ($quoted) { throw 'unterminated quote' }
    if ($cur.Length -gt 0) { $out.Add($cur.ToString()) }
    return $out.ToArray()
}

function Parse-Expected([string]$batPath, [string]$mode) {
    $ports = switch ($mode) {
        'off' { @('12', '12') }
        'all' { @('1024-65535', '1024-65535') }
        'tcp' { @('1024-65535', '12') }
        'udp' { @('12', '1024-65535') }
        default { throw "bad mode $mode" }
    }
    $lines = @(Get-Content -LiteralPath $batPath -Encoding UTF8)
    $cmd = ''
    $collecting = $false
    foreach ($line in $lines) {
        if (-not $collecting) {
            if ($line -notmatch '(?i)winws\.exe') { continue }
            $collecting = $true
        }
        $part = $line.TrimEnd()
        $continued = $part.EndsWith('^')
        if ($continued) { $part = $part.Substring(0, $part.Length - 1) }
        $cmd += ' ' + $part
        if (-not $continued) { break }
    }
    $m = [regex]::Match($cmd, '(?i)"%BIN%winws\.exe"')
    if (-not $m.Success) { throw "winws marker missing: $batPath" }
    $rest = $cmd.Substring($m.Index + $m.Length)
    $expected = New-Object System.Collections.Generic.List[string]
    foreach ($raw in (Split-Tokens $rest)) {
        $x = $raw.Replace('^!', '!')
        $x = $x.Replace('%GameFilterTCP%', $ports[0])
        $x = $x.Replace('%GameFilterUDP%', $ports[1])
        $legacy = if ($mode -eq 'off') { '12' } else { '1024-65535' }
        $x = $x.Replace('%GameFilter%', $legacy)
        $x = $x.Replace('%BIN%', 'BIN\')
        $x = $x.Replace('%LISTS%', 'LISTS\')
        if ($x.Length -gt 0) { $expected.Add($x) }
    }
    return $expected.ToArray()
}

$bats = @(Get-ChildItem -LiteralPath $BatDir -File -Filter 'general*.bat')
if ($bats.Count -eq 0) { throw "no strategies in $BatDir" }
$total = 0
$failed = 0
foreach ($bat in ($bats | Sort-Object Name)) {
    $id = Get-StrategyId $bat.Name
    foreach ($mode in @('off', 'all', 'tcp', 'udp')) {
        $total++
        $expected = @(Parse-Expected $bat.FullName $mode)
        $actual = @(& $TestExe --dump $id $mode | Where-Object { $_ -ne '' })
        $ok = $LASTEXITCODE -eq 0 -and $actual.Count -eq $expected.Count
        if ($ok) {
            for ($i = 0; $i -lt $expected.Count; $i++) {
                if ($actual[$i] -cne $expected[$i]) { $ok = $false; break }
            }
        }
        if (-not $ok) {
            $failed++
            Write-Output "MISMATCH: $id mode=$mode expected=$($expected.Count) actual=$($actual.Count)"
            $max = [Math]::Max($expected.Count, $actual.Count)
            for ($i = 0; $i -lt $max; $i++) {
                $e = if ($i -lt $expected.Count) { $expected[$i] } else { '<none>' }
                $a = if ($i -lt $actual.Count) { $actual[$i] } else { '<none>' }
                if ($e -cne $a) { Write-Output ("  [{0}] BAT='{1}' EXE='{2}'" -f $i, $e, $a) }
            }
        }
    }
}

Write-Output '-------------------------------------------'
if ($failed -eq 0) {
    Write-Output "STRATEGY_FIDELITY: $total/$total PASS ($($bats.Count) strategies x 4 modes)"
    exit 0
}
Write-Output "STRATEGY_FIDELITY: $($total - $failed)/$total FAIL"
exit 1

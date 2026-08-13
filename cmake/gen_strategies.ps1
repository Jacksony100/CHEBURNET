<#
    Discover every supported upstream `general*.bat` strategy and generate the
    typed C++ registry. The count and filenames are deliberately not hardcoded.

    Supported BAT surface:
      * one `start ... "%BIN%winws.exe"` command;
      * caret line continuation;
      * ordinary whitespace-delimited winws tokens, optionally quoted;
      * %BIN%, %LISTS%, %GameFilterTCP%, %GameFilterUDP%;
      * legacy %GameFilter% (mapped to both-protocol compatibility mode);
      * ^! escaping used by winws.

    Any other variable, shell operator, unterminated quote, or extra executable
    command is rejected instead of being partially imported.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BatDir,
    [Parameter(Mandatory = $true)][string]$HeaderOut,
    [string]$CatalogOut
)
$ErrorActionPreference = "Stop"

function Get-StrategyId([string]$fileName) {
    if ($fileName -ieq 'general.bat') { return 'general' }
    if ($fileName -notmatch '^general \((.+)\)\.bat$') {
        throw "unsupported strategy filename: $fileName"
    }
    $label = $Matches[1]
    # Preserve IDs used by existing public configs.
    if ($label -match '^FAKE TLS AUTO( ALT\d*)?$') {
        $suffix = if ($Matches[1]) { '_' + $Matches[1].Trim().ToLowerInvariant() } else { '' }
        return 'faketls_auto' + $suffix
    }
    $id = $label.ToLowerInvariant() -replace '[^a-z0-9]+', '_'
    $id = $id.Trim('_')
    if ([string]::IsNullOrWhiteSpace($id)) { throw "empty strategy id for $fileName" }
    return $id
}

function Get-SortKey([string]$fileName) {
    if ($fileName -ieq 'general.bat') { return '0000-general' }
    if ($fileName -match '^general \(ALT\)\.bat$') { return '0101-alt' }
    if ($fileName -match '^general \(ALT(\d+)\)\.bat$') {
        return ('01{0:D2}-{1}' -f [int]$Matches[1], $fileName)
    }
    if ($fileName -match '^general \(EXP\)\.bat$') { return '0200-exp' }
    if ($fileName -match '^general \(FAKE TLS AUTO(?: ALT(\d*))?\)\.bat$') {
        $n = if ($Matches[1] -eq '') { 0 } else { [int]$Matches[1] }
        if ($fileName -match ' AUTO ALT\)') { $n = 1 }
        return ('03{0:D2}-{1}' -f $n, $fileName)
    }
    if ($fileName -match '^general \(SIMPLE FAKE(?: ALT(\d*))?\)\.bat$') {
        $n = if ($Matches[1] -eq '') { 0 } else { [int]$Matches[1] }
        if ($fileName -match ' FAKE ALT\)') { $n = 1 }
        return ('04{0:D2}-{1}' -f $n, $fileName)
    }
    return '9000-' + $fileName.ToLowerInvariant()
}

function Split-CommandTokens([string]$command, [string]$path) {
    $tokens = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $quoted = $false
    for ($i = 0; $i -lt $command.Length; $i++) {
        $c = $command[$i]
        if ($c -eq '"') { $quoted = -not $quoted; continue }
        if (-not $quoted -and [char]::IsWhiteSpace($c)) {
            if ($current.Length -gt 0) {
                $tokens.Add($current.ToString())
                [void]$current.Clear()
            }
            continue
        }
        [void]$current.Append($c)
    }
    if ($quoted) { throw "unterminated quote in $path" }
    if ($current.Length -gt 0) { $tokens.Add($current.ToString()) }
    return $tokens.ToArray()
}

function Parse-Bat([string]$path) {
    $lines = @(Get-Content -LiteralPath $path -Encoding UTF8)
    $allText = $lines -join "`n"
    $recommended = -not ($allText -match '(?i)NOT RECOMMENDED')
    $starts = @($lines | Select-String -Pattern '(?i)winws\.exe')
    if ($starts.Count -ne 1) {
        throw "expected exactly one winws.exe command in $path; found $($starts.Count)"
    }

    $cmd = ''
    $collecting = $false
    foreach ($line in $lines) {
        if (-not $collecting) {
            if ($line -notmatch '(?i)winws\.exe') { continue }
            $collecting = $true
        }
        $part = $line.TrimEnd()
        $continues = $part.EndsWith('^')
        if ($continues) { $part = $part.Substring(0, $part.Length - 1) }
        $cmd += ' ' + $part
        if (-not $continues) { break }
    }

    $match = [regex]::Match($cmd, '(?i)"%BIN%winws\.exe"')
    if (-not $match.Success) { throw "unsupported winws.exe prefix in $path" }
    $rest = $cmd.Substring($match.Index + $match.Length).Trim()
    if ($rest -match '[&|<>`]') { throw "unsupported BAT shell operator in $path" }

    $tokens = New-Object System.Collections.Generic.List[string]
    foreach ($raw in (Split-CommandTokens $rest $path)) {
        $token = $raw.Replace('^!', '!')
        $token = $token.Replace('%GameFilterTCP%', '%GAME_TCP%')
        $token = $token.Replace('%GameFilterUDP%', '%GAME_UDP%')
        $token = $token.Replace('%GameFilter%', '%GAME_BOTH%')
        $token = $token.Replace('%LISTS%list-general-user.txt', '%USER_LISTS%list-general-user.txt')
        $token = $token.Replace('%LISTS%list-exclude-user.txt', '%USER_LISTS%list-exclude-user.txt')
        $token = $token.Replace('%LISTS%ipset-exclude-user.txt', '%USER_LISTS%ipset-exclude-user.txt')
        if ($token -match '%(?!BIN%|LISTS%|USER_LISTS%|GAME_TCP%|GAME_UDP%|GAME_BOTH%)[^%]+%') {
            throw "unsupported BAT variable in token '$token' ($path)"
        }
        if ($token.Contains('^')) { throw "unsupported caret escape in token '$token' ($path)" }
        if ($token.Length -eq 0) { continue }
        $tokens.Add($token)
    }
    if ($tokens.Count -eq 0) { throw "empty winws argument vector in $path" }
    return [pscustomobject]@{ Tokens = $tokens.ToArray(); Recommended = $recommended }
}

function Cpp-Escape([string]$value) {
    return $value.Replace('\', '\\').Replace('"', '\"')
}

function Json-Escape([string]$value) {
    return ($value | ConvertTo-Json -Compress)
}

$files = @(Get-ChildItem -LiteralPath $BatDir -File -Filter 'general*.bat' |
    Sort-Object @{ Expression = { Get-SortKey $_.Name } }, Name)
if ($files.Count -eq 0) { throw "no general*.bat strategies found in $BatDir" }

$ids = @{}
$strategies = foreach ($file in $files) {
    $id = Get-StrategyId $file.Name
    if ($ids.ContainsKey($id)) { throw "duplicate generated strategy id '$id'" }
    $ids[$id] = $true
    $display = if ($file.Name -ieq 'general.bat') { 'GENERAL' }
               elseif ($file.Name -match '^general \((.+)\)\.bat$') { $Matches[1] }
               else { $file.BaseName }
    $parsed = Parse-Bat $file.FullName
    [pscustomobject]@{
        File = $file.Name
        Id = $id
        Name = $display
        Desc = if ($id -eq 'general') { 'Faithful upstream general.bat default' }
               else { "Upstream strategy: $display" }
        Tokens = $parsed.Tokens
        Recommended = $parsed.Recommended
    }
}
if (-not ($strategies | Where-Object Id -eq 'general')) { throw 'general.bat is mandatory' }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// AUTO-GENERATED by cmake/gen_strategies.ps1 - DO NOT EDIT.')
[void]$sb.AppendLine('// Dynamically discovered, fail-closed port of upstream BAT argument vectors.')
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('namespace cheburnet {')
[void]$sb.AppendLine('struct GeneratedStrategy {')
[void]$sb.AppendLine('    const char* id;')
[void]$sb.AppendLine('    const wchar_t* sourceFile;')
[void]$sb.AppendLine('    const wchar_t* displayName;')
[void]$sb.AppendLine('    const wchar_t* description;')
[void]$sb.AppendLine('    bool recommended;')
[void]$sb.AppendLine('    const wchar_t* const* argv;')
[void]$sb.AppendLine('    int argc;')
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')

$rows = New-Object System.Collections.Generic.List[string]
$catalog = New-Object System.Collections.Generic.List[string]
foreach ($s in $strategies) {
    $arr = 'kArgs_' + $s.Id
    [void]$sb.AppendLine("inline constexpr const wchar_t* $arr[] = {")
    foreach ($token in $s.Tokens) {
        [void]$sb.AppendLine("    L`"$(Cpp-Escape $token)`",")
    }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine('')
    $rec = if ($s.Recommended) { 'true' } else { 'false' }
    $rows.Add("    { `"$($s.Id)`", L`"$(Cpp-Escape $s.File)`", L`"$(Cpp-Escape $s.Name)`", L`"$(Cpp-Escape $s.Desc)`", $rec, $arr, static_cast<int>(sizeof($arr) / sizeof($arr[0])) },")
    $tokenJson = ($s.Tokens | ForEach-Object { Json-Escape $_ }) -join ','
    $catalog.Add(('{{"id":{0},"source":{1},"name":{2},"recommended":{3},"argv":[{4}]}}' -f
        (Json-Escape $s.Id),(Json-Escape $s.File),(Json-Escape $s.Name),
        ($(if($s.Recommended){'true'}else{'false'})),$tokenJson))
}
[void]$sb.AppendLine('inline constexpr GeneratedStrategy kGeneratedStrategies[] = {')
foreach ($row in $rows) { [void]$sb.AppendLine($row) }
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('inline constexpr int kGeneratedStrategyCount =')
[void]$sb.AppendLine('    static_cast<int>(sizeof(kGeneratedStrategies) / sizeof(kGeneratedStrategies[0]));')
[void]$sb.AppendLine('inline constexpr char kGeneratedStrategyCatalogJson[] =')
$catalogJsonForCpp = '{"schema":1,"strategies":[' + ($catalog -join ',') + ']}'
for ($offset = 0; $offset -lt $catalogJsonForCpp.Length; $offset += 8000) {
    $length = [Math]::Min(8000, $catalogJsonForCpp.Length - $offset)
    $chunk = $catalogJsonForCpp.Substring($offset, $length)
    [void]$sb.AppendLine(('R"JSON({0})JSON"' -f $chunk))
}
[void]$sb.AppendLine(';')
[void]$sb.AppendLine('} // namespace cheburnet')

$outParent = Split-Path -Parent $HeaderOut
if (-not (Test-Path -LiteralPath $outParent)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}
[System.IO.File]::WriteAllText($HeaderOut, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
if (-not [string]::IsNullOrWhiteSpace($CatalogOut)) {
    $catalogParent = Split-Path -Parent $CatalogOut
    if (-not (Test-Path -LiteralPath $catalogParent)) {
        New-Item -ItemType Directory -Path $catalogParent -Force | Out-Null
    }
    $catalogJson = '{"schema":1,"strategies":[' + ($catalog -join ',') + "]}`n"
    [System.IO.File]::WriteAllText($CatalogOut, $catalogJson, [System.Text.UTF8Encoding]::new($false))
}

Write-Output "gen_strategies: $($strategies.Count) dynamically discovered strategies -> $HeaderOut"
foreach ($s in $strategies) {
    Write-Output ('  {0,-22} {1,4} tokens  source={2}' -f $s.Id, $s.Tokens.Count, $s.File)
}

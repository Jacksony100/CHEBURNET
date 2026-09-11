<#
Shared version library for release automation and tests.

cmake/Version.cmake is the single authoritative source. This file only reads it,
so no script duplicates a version number and no script can publish release
metadata that disagrees with the build.

This file is deliberately ASCII-only: Windows PowerShell 5.1 reads .ps1 as ANSI
unless the file carries a BOM, so non-ASCII text here would break parsing.

Dot-source it:  . "$PSScriptRoot\version.ps1"
#>
Set-StrictMode -Version Latest

# Canonical semantic-version grammar. It matches the C++ parser in
# src/update/Version.cpp: core without leading zeros, optional single-letter
# upstream revision, SemVer 2.0.0 prerelease and build metadata.
$script:CheburnetSemanticVersionPattern =
    '^(?<core>(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){0,7})(?<revision>[a-z]?)' +
    '(?:-(?<prerelease>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?' +
    '(?:\+(?<build>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'

# Release tag: stable vX.Y.Z or prerelease vX.Y.Z-rc.N (N >= 1).
$script:CheburnetReleaseTagPattern = '^v([0-9]+\.[0-9]+\.[0-9]+(?:-rc\.[1-9][0-9]*)?)$'

function Test-CheburnetSemanticVersion {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
    # -cmatch, not -match: PowerShell regex is case-insensitive by default,
    # which would let '1.0.0A' pass the lowercase upstream-revision class.
    return [bool]($Value -cmatch $script:CheburnetSemanticVersionPattern)
}

function Split-CheburnetVersion {
    <#
    Splits a semantic version into ordering components. Deliberately mirrors the
    grammar of src/update/Version.cpp; the version_model test cross-checks both
    implementations against one shared table.
    #>
    param([Parameter(Mandatory = $true)][string]$Value)
    $text = $Value
    if ($text.StartsWith('v') -or $text.StartsWith('V')) { $text = $text.Substring(1) }
    $matched = [regex]::Match($text, $script:CheburnetSemanticVersionPattern)
    if (-not $matched.Success) { throw "version is not canonical: $Value" }
    $core = @($matched.Groups['core'].Value.Split('.') | ForEach-Object { [long]$_ })
    # Assigned in two statements on purpose: `$x = if (...) { @() }` yields $null
    # in PowerShell, which would make a stable version's Prerelease unusable.
    $prerelease = @()
    if ($matched.Groups['prerelease'].Success) {
        $prerelease = @($matched.Groups['prerelease'].Value.Split('.'))
    }
    return [pscustomobject]@{
        Core          = $core
        Revision      = $matched.Groups['revision'].Value
        HasPrerelease = $matched.Groups['prerelease'].Success
        Prerelease    = $prerelease
    }
}

function Compare-CheburnetVersion {
    <#
    Returns -1/0/1 using exactly the rules of CompareVersions() in C++: core
    numerically, absent revision < present revision, prerelease < no prerelease,
    then SemVer 2.0.0 precedence; build metadata is ignored.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )
    $l = Split-CheburnetVersion $Left
    $r = Split-CheburnetVersion $Right
    $count = [Math]::Max($l.Core.Count, $r.Core.Count)
    for ($i = 0; $i -lt $count; $i++) {
        $lv = if ($i -lt $l.Core.Count) { $l.Core[$i] } else { [long]0 }
        $rv = if ($i -lt $r.Core.Count) { $r.Core[$i] } else { [long]0 }
        if ($lv -lt $rv) { return -1 }
        if ($lv -gt $rv) { return 1 }
    }
    if ($l.Revision -cne $r.Revision) {
        if ($l.Revision -eq '') { return -1 }
        if ($r.Revision -eq '') { return 1 }
        if ($l.Revision -clt $r.Revision) { return -1 }
        return 1
    }
    if ($l.HasPrerelease -ne $r.HasPrerelease) {
        if ($l.HasPrerelease) { return -1 }
        return 1
    }
    if (-not $l.HasPrerelease) { return 0 }
    $leftIds = @($l.Prerelease)
    $rightIds = @($r.Prerelease)
    $shared = [Math]::Min($leftIds.Count, $rightIds.Count)
    for ($i = 0; $i -lt $shared; $i++) {
        $li = [string]$leftIds[$i]
        $ri = [string]$rightIds[$i]
        $lNumeric = [bool]($li -cmatch '^(0|[1-9][0-9]*)$')
        $rNumeric = [bool]($ri -cmatch '^(0|[1-9][0-9]*)$')
        if ($lNumeric -ne $rNumeric) {
            if ($lNumeric) { return -1 }
            return 1
        }
        if ($lNumeric) {
            if ([long]$li -lt [long]$ri) { return -1 }
            if ([long]$li -gt [long]$ri) { return 1 }
            continue
        }
        if ($li -cne $ri) {
            if ($li -clt $ri) { return -1 }
            return 1
        }
    }
    if ($leftIds.Count -lt $rightIds.Count) { return -1 }
    if ($leftIds.Count -gt $rightIds.Count) { return 1 }
    return 0
}

function New-CheburnetVersion {
    <#
    Builds the version model from the four authored fields. Used both when
    reading cmake/Version.cmake and by tests for synthetic models.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Major,
        [Parameter(Mandatory = $true)][string]$Minor,
        [Parameter(Mandatory = $true)][string]$Patch,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Prerelease
    )
    foreach ($component in @($Major, $Minor, $Patch)) {
        if ($component -cnotmatch '^(0|[1-9][0-9]*)$') {
            throw "version component must be numeric without leading zeros: '$component'"
        }
    }
    $core = "$Major.$Minor.$Patch"
    if ($Prerelease -eq '') {
        $semantic = $core
        $tweak = '0'
        $channel = 'stable'
        $isPrerelease = $false
    } elseif ($Prerelease -cmatch '^rc\.([1-9][0-9]*)$') {
        $semantic = "$core-$Prerelease"
        $tweak = $Matches[1]
        $channel = 'prerelease'
        $isPrerelease = $true
    } else {
        throw "CHEBURNET_VERSION_PRERELEASE must be empty or rc.N, got: '$Prerelease'"
    }
    if ([long]$tweak -gt 65534) { throw 'RC number does not fit a PE version component' }
    if (-not (Test-CheburnetSemanticVersion $semantic)) {
        throw "built semantic version fails the canonical check: $semantic"
    }
    return [pscustomobject]@{
        Major        = $Major
        Minor        = $Minor
        Patch        = $Patch
        Prerelease   = $Prerelease
        Core         = $core
        Semantic     = $semantic
        Tweak        = $tweak
        Pe           = "$core.$tweak"
        Channel      = $channel
        IsPrerelease = $isPrerelease
    }
}

function Get-CheburnetVersion {
    <#
    Reads the authoritative version model from cmake/Version.cmake.
    #>
    param([Parameter(Mandatory = $true)][string]$Root)
    $path = Join-Path ([IO.Path]::GetFullPath($Root).TrimEnd('\')) 'cmake\Version.cmake'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "version model not found: $path"
    }
    $text = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    $fields = @{}
    foreach ($name in @('MAJOR', 'MINOR', 'PATCH')) {
        $pattern = '(?m)^\s*set\(CHEBURNET_VERSION_' + $name + '\s+([0-9]+)\s*\)'
        $matched = [regex]::Match($text, $pattern)
        if (-not $matched.Success) {
            throw "cmake/Version.cmake has no CHEBURNET_VERSION_$name"
        }
        $fields[$name] = $matched.Groups[1].Value
    }
    $prereleaseMatch = [regex]::Match(
        $text, '(?m)^\s*set\(CHEBURNET_VERSION_PRERELEASE\s+"([^"]*)"\s*\)')
    if (-not $prereleaseMatch.Success) {
        throw 'cmake/Version.cmake has no CHEBURNET_VERSION_PRERELEASE'
    }
    return New-CheburnetVersion -Major $fields['MAJOR'] -Minor $fields['MINOR'] `
        -Patch $fields['PATCH'] -Prerelease $prereleaseMatch.Groups[1].Value
}

function Assert-CheburnetReleaseTag {
    <#
    The tag/source gate. It closes three publication mistakes at once:
      * an arbitrary tag while the source carries a different version;
      * a stable tag vX.Y.Z while the source is a release candidate;
      * an RC tag while the source is stable.
    Returns the tag's semantic version.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)]$Version
    )
    if ($Tag -cnotmatch $script:CheburnetReleaseTagPattern) {
        throw "release tag has unsupported syntax: $Tag"
    }
    $tagVersion = $Matches[1]
    if ($tagVersion -cne $Version.Semantic) {
        throw ("tag version $tagVersion does not match source semantic version " +
               "$($Version.Semantic)")
    }
    $tagIsPrerelease = $tagVersion.Contains('-rc.')
    if ($tagIsPrerelease -ne $Version.IsPrerelease) {
        throw "tag channel and source release channel disagree: $Tag vs $($Version.Channel)"
    }
    return $tagVersion
}

function Assert-CheburnetLauncherPeVersion {
    <#
    Checks that the built launcher carries exactly the PE version the model
    prescribes and that the PE version stays numeric.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Launcher,
        [Parameter(Mandatory = $true)]$Version
    )
    $path = [IO.Path]::GetFullPath($Launcher)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "launcher not found for PE version check: $path"
    }
    if ($Version.Pe -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "PE version must be numeric: $($Version.Pe)"
    }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    if ([string]$info.FileVersion -cne $Version.Pe -or
        [string]$info.ProductVersion -cne $Version.Pe) {
        throw ("launcher PE version mismatch: expected $($Version.Pe), " +
               "file=$($info.FileVersion), product=$($info.ProductVersion)")
    }
    return $Version.Pe
}

<#
Validates the authoritative version model and the script-side comparator.

Covers three classes of release mistake:
  1. the version model (cmake/Version.cmake), the generated header and the
     embedded resources drifting apart;
  2. version ordering disagreeing between PowerShell and C++ (both read the
     shared table tests/version-order-cases.json; the C++ side is the
     updateversion test);
  3. publishing a tag that does not match the source version or its channel.

ASCII-only on purpose: PowerShell 5.1 reads .ps1 as ANSI without a BOM.
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
. (Join-Path $root 'scripts\version.ps1')

$failures = New-Object Collections.Generic.List[string]
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $script:failures.Add($Message) }
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    if (-not $threw) { $script:failures.Add($Message) }
}

# ---- 1. version model ------------------------------------------------------
$version = Get-CheburnetVersion -Root $root
Assert-True ($version.Pe -match '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') `
    "PE version must be numeric: $($version.Pe)"
Assert-True ($version.Pe -eq "$($version.Core).$($version.Tweak)") `
    'PE version must be core plus tweak'
Assert-True (Test-CheburnetSemanticVersion $version.Semantic) `
    "semantic version is not canonical: $($version.Semantic)"
if ($version.IsPrerelease) {
    Assert-True ($version.Semantic -cne $version.Core) `
        'a release candidate version must differ from the stable core version'
    Assert-True ($version.Channel -ceq 'prerelease') 'channel must be prerelease'
    Assert-True ((Compare-CheburnetVersion $version.Semantic $version.Core) -lt 0) `
        'an RC must order below the stable release of the same core version'
    Assert-True ([string]$version.Tweak -cne '0') 'a prerelease tweak cannot be zero'
} else {
    Assert-True ($version.Semantic -ceq $version.Core) `
        'a stable version must equal its core version'
    Assert-True ($version.Channel -ceq 'stable') 'channel must be stable'
    Assert-True ([string]$version.Tweak -ceq '0') 'a stable tweak must be zero'
}

# The generated header must match the model exactly.
$generated = Join-Path $root 'resources\generated\GeneratedVersion.h'
if (Test-Path -LiteralPath $generated -PathType Leaf) {
    $header = Get-Content -LiteralPath $generated -Raw -Encoding UTF8
    $channelFlag = if ($version.IsPrerelease) { '0' } else { '1' }
    # Each element is parenthesised on purpose: in PowerShell the comma operator
    # binds tighter than '+', so unparenthesised concatenations would nest.
    $expected = @(
        ('#define CHEBURNET_VERSION_STR "' + $version.Semantic + '"'),
        ('#define CHEBURNET_VERSION_PE_STR "' + $version.Pe + '"'),
        ('#define CHEBURNET_VERSION_CORE_STR "' + $version.Core + '"'),
        ('#define CHEBURNET_STABLE_CHANNEL ' + $channelFlag),
        ('#define CHEBURNET_RELEASE_CHANNEL "' + $version.Channel + '"'),
        ('#define CHEBURNET_VERSION_QUAD ' + ($version.Core -replace '\.', ',') +
         ',' + $version.Tweak)
    )
    foreach ($line in $expected) {
        Assert-True ($header.Contains($line)) "GeneratedVersion.h is missing: $line"
    }
} else {
    $failures.Add("generated version header not found: $generated")
}

# The PE version resource must consume the numeric string, never the semantic one.
$rc = Get-Content -LiteralPath (Join-Path $root 'resources\cheburnet.rc') -Raw -Encoding UTF8
Assert-True ($rc -match 'VALUE "FileVersion",\s+CHEBURNET_VERSION_PE_STR') `
    'cheburnet.rc must publish CHEBURNET_VERSION_PE_STR as FileVersion'
Assert-True ($rc -match 'VALUE "ProductVersion",\s+CHEBURNET_VERSION_PE_STR') `
    'cheburnet.rc must publish CHEBURNET_VERSION_PE_STR as ProductVersion'
Assert-True (-not ($rc -match 'CHEBURNET_VERSION_STR|CHEBURNET_VERSION_WSTR')) `
    'the PE version resource must not embed the semantic version'

# ---- 2. ordering: PowerShell against the canonical table --------------------
$casesPath = Join-Path $root 'tests\version-order-cases.json'
$cases = Get-Content -LiteralPath $casesPath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-True (@($cases.cases).Count -ge 20) 'the version ordering table is suspiciously small'
foreach ($case in $cases.cases) {
    $forward = Compare-CheburnetVersion $case.left $case.right
    $reverse = Compare-CheburnetVersion $case.right $case.left
    Assert-True ($forward -eq [int]$case.expected) `
        "order $($case.left) vs $($case.right): got $forward, expected $($case.expected)"
    Assert-True ($reverse -eq (-1 * $forward)) `
        "comparison is not antisymmetric: $($case.left) vs $($case.right)"
}
Assert-True (@($cases.invalid).Count -ge 10) 'the invalid-version list is suspiciously small'
foreach ($invalid in $cases.invalid) {
    Assert-True (-not (Test-CheburnetSemanticVersion ([string]$invalid))) `
        "accepted an invalid version: '$invalid'"
}

# ---- 3. release tag gate ---------------------------------------------------
$stable = New-CheburnetVersion -Major '1' -Minor '0' -Patch '0' -Prerelease ''
$candidate = New-CheburnetVersion -Major '1' -Minor '0' -Patch '0' -Prerelease 'rc.3'
Assert-True ((Assert-CheburnetReleaseTag -Tag 'v1.0.0' -Version $stable) -ceq '1.0.0') `
    'a stable tag on a stable source version must be accepted'
Assert-True ((Assert-CheburnetReleaseTag -Tag 'v1.0.0-rc.3' -Version $candidate) -ceq '1.0.0-rc.3') `
    'a matching RC tag must be accepted'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v1.0.0' -Version $candidate } `
    'a stable tag must not publish RC semantics'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v1.0.0-rc.3' -Version $stable } `
    'an RC tag must not publish stable semantics'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v1.0.0-rc.2' -Version $candidate } `
    'a mismatched RC number must be rejected'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v9.9.9' -Version $stable } `
    'a foreign version in the tag must be rejected'
Assert-Throws { Assert-CheburnetReleaseTag -Tag '1.0.0' -Version $stable } `
    'a tag without the v prefix must be rejected'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v1.0.0-beta.1' -Version $stable } `
    'an unsupported prerelease form in the tag must be rejected'
Assert-Throws { Assert-CheburnetReleaseTag -Tag 'v1.0.0-rc.0' -Version $stable } `
    'rc.0 must be rejected'
Assert-Throws { New-CheburnetVersion -Major '1' -Minor '0' -Patch '0' -Prerelease 'beta' } `
    'the version model must accept only rc.N'
Assert-Throws { New-CheburnetVersion -Major '01' -Minor '0' -Patch '0' -Prerelease '' } `
    'leading zeros in version components must be rejected'

# The real tag for the current model must pass the gate.
$currentTag = 'v' + $version.Semantic
Assert-True ((Assert-CheburnetReleaseTag -Tag $currentTag -Version $version) -ceq $version.Semantic) `
    "tag $currentTag must match the current version model"

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    throw "VERSION_MODEL: FAIL ($($failures.Count))"
}
Write-Output ("VERSION_MODEL: PASS semantic=$($version.Semantic) pe=$($version.Pe) " +
              "channel=$($version.Channel)")

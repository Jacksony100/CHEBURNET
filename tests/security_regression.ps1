[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$failures = 0
function Read-Source([string]$Relative) {
    $path = Join-Path $root $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing source: $Relative" }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8
}
function Require-Match([string]$Name, [string]$Text, [string]$Pattern) {
    if ($Text -match $Pattern) { Write-Output "PASS $Name"; return }
    Write-Output "FAIL $Name"
    $script:failures++
}
function Reject-Match([string]$Name, [string]$Text, [string]$Pattern) {
    if ($Text -notmatch $Pattern) { Write-Output "PASS $Name"; return }
    Write-Output "FAIL $Name"
    $script:failures++
}
function Require-Order([string]$Name, [string]$Text, [string[]]$Markers) {
    $at = -1
    foreach ($marker in $Markers) {
        $next = $Text.IndexOf($marker, $at + 1, [StringComparison]::Ordinal)
        if ($next -lt 0) {
            Write-Output "FAIL $Name (missing/out-of-order marker: $marker)"
            $script:failures++
            return
        }
        $at = $next
    }
    Write-Output "PASS $Name"
}

$package = Read-Source 'scripts\package.ps1'
$packagePath = Read-Source 'scripts\package-path.ps1'
$main = Read-Source 'src\main.cpp'
$logger = Read-Source 'src\util\Logger.cpp'
$secureFs = Read-Source 'src\core\SecureFs.cpp'
$process = Read-Source 'src\core\ProcessManager.cpp'
$launcher = Read-Source 'src\core\Launcher.cpp'
$screens = Read-Source 'src\app\UiScreens.cpp'
$ui = Read-Source 'src\ui\UiContext.cpp'
$app = Read-Source 'src\app\App.cpp'
$operationState = Read-Source 'src\app\OperationState.h'
$input = Read-Source 'src\ui\Input.cpp'
$config = Read-Source 'src\config\Config.cpp'
$json = Read-Source 'src\util\Json.cpp'
$manager = Read-Source 'src\update\UpdateManager.cpp'
$managerHeader = Read-Source 'src\update\UpdateManager.h'
$http = Read-Source 'src\update\WinHttpClient.cpp'
$packageCpp = Read-Source 'src\update\UpdatePackage.cpp'
$strategyGen = Read-Source 'cmake\gen_strategies.ps1'
$strategyRuntime = Read-Source 'src\config\Strategies.cpp'
$upstreamVerify = Read-Source 'scripts\verify-upstream.ps1'
$releaseManifest = Read-Source 'scripts\generate-update-manifest.ps1'
$packageBuilder = Read-Source 'scripts\build-update-package.ps1'
$manifestSigner = Read-Source 'scripts\sign-update-manifest.ps1'
$manifestVerifier = Read-Source 'scripts\verify-update-manifest-signature.ps1'
$releasePreparation = Read-Source 'scripts\prepare-release.ps1'
$versionModel = Read-Source 'scripts\version.ps1'
$authenticode = Read-Source 'scripts\authenticode.ps1'
$checkService = Read-Source 'src\update\UpdateCheckService.cpp'
$checkServiceHeader = Read-Source 'src\update\UpdateCheckService.h'
$diagnostics = Read-Source 'src\app\Diagnostics.cpp'
$diagnosticsHeader = Read-Source 'src\app\Diagnostics.h'
$recovery = Read-Source 'src\app\RuntimeRecovery.cpp'
$authenticodeSign = Read-Source 'scripts\authenticode-sign.ps1'
$releaseWorkflow = Read-Source '.github\workflows\release.yml'

Require-Match 'P1-01 strict descendant package cleanup' ($package + $packagePath) `
    'Assert-SafePackageOutDir[\s\S]*strict descendant'
Require-Order 'P1-02 bootstrap precedes logger' $main @(
    'PrivilegeManager::IsElevated()', 'BootstrapProtectedTree(', 'Logger::Init(')
Require-Match 'P1-02 bootstrap failure is fatal' $main `
    'if \(!bootstrap\.ok\)[\s\S]*return 3;'
Require-Match 'P1-02 logger hardens before opening and validates its live handle' $logger `
    'PrepareProtectedLogPath_NoLock[\s\S]*HardenObject\(g_path[\s\S]*OpenProtectedLog_NoLock[\s\S]*ValidateProtectedHandle'
Reject-Match 'P1-02 logger does not harden by name after taking append handle' $logger `
    'CreateFileW\([\s\S]{0,400}FILE_APPEND_DATA[\s\S]{0,800}HardenObject\(g_path'
Require-Match 'P1-02 early Unicode errors bypass CRT locale conversion' $main `
    'WriteConsoleW[\s\S]*WriteStderr\(message\)'
Require-Match 'P1-03 CNG random 128-bit temp' $secureFs `
    'array<unsigned char, 16>[\s\S]*BCryptGenRandom'
Require-Match 'P1-03 CREATE_NEW no-follow temp' $secureFs `
    'CREATE_NEW[\s\S]*FILE_FLAG_OPEN_REPARSE_POINT'
Reject-Match 'P1-03 no fixed target.tmp or privileged CREATE_ALWAYS' `
    ($secureFs + $process + (Read-Source 'src\core\ResourceExtractor.cpp') +
     (Read-Source 'src\update\RuntimeStateStore.cpp')) `
    '(?i)CREATE_ALWAYS|target\s*\+\s*L?"\.tmp"'
Require-Match 'P1-04 exact owner/DACL verification' $secureFs `
    'VerifyProtectedSecurity[\s\S]*SE_DACL_PROTECTED[\s\S]*AceCount == 3'
Require-Match 'P1-04 file and directory ACE inheritance are verified separately' $secureFs `
    'kDirectoryAclSddl[\s\S]*kFileAclSddl[\s\S]*expectedAceFlags[\s\S]*ObjectKind::Directory'
Require-Match 'P1-04 activated target ACL is mandatory' $secureFs `
    'targetAcl = HardenObject[\s\S]*if \(!targetAcl\.ok\) return targetAcl'
Require-Order 'P1-05 suspended transactional start' $process @(
    'CREATE_SUSPENDED', 'AssignProcessToJobObject', 'CreationTimeOf(pi.hProcess)',
    'ImagePathOf(pi.hProcess)', 'ResumeThread(pi.hThread)',
    'WaitForSingleObject(pi.hProcess, stabilizeMs)', 'IdentityMatches(record_)',
    'SaveRecord(recordPath_, record_)')
Require-Match 'P1-05 rollback terminates child' $process `
    'auto rollback[\s\S]*TerminateProcess[\s\S]*CloseHandle\(job\)[\s\S]*WaitForSingleObject[\s\S]*DeleteRecord'
Require-Match 'P1-05 kernel job rollback guard' $process `
    'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE[\s\S]*CreateProcessW[\s\S]*SaveRecord'
Require-Match 'P1-05 stop requires confirmed termination' $process `
    'TerminateProcess\(h, 0\)[\s\S]*stopped != WAIT_OBJECT_0[\s\S]*Logger::Error'
Require-Match 'P1-05 redirected logs validate ACL through their live handle' $process `
    'OpenProtectedRedirect[\s\S]*READ_CONTROL[\s\S]*ValidateProtectedHandle'
Require-Match 'P2-06 verified PID exclusion' ($process + $launcher + $screens) `
    'TrustedPidForExclusion[\s\S]*IdentityMatches'
Require-Match 'P2-07 output console state restore' $ui `
    'savedOutputMode_[\s\S]*savedCursor_[\s\S]*savedAttributes_[\s\S]*SetConsoleOutputCP\(savedOutputCp_\)'
Require-Match 'P2-07 input console state restore' $input `
    'savedMode_[\s\S]*SetConsoleMode'
Require-Match 'P2-08 scoped strict JSON config' ($config + $json) `
    'Find\(key\)[\s\S]*json::Parse[\s\S]*duplicate object key'
Require-Match 'UPDATE signature before trust' $manager `
    'VerifyManifestSignature[\s\S]*EvaluateLauncher[\s\S]*g_verifiedManifestCache = result'
Require-Match 'UPDATE apply remains bound to authenticated manifest' $manager `
    'most recently authenticated[\s\S]*g_verifiedManifestCache[\s\S]*SameArtifact'
Require-Match 'UPDATE downloads remain bound to authenticated manifest' $manager `
    'SameArtifact[\s\S]*DownloadVerified[\s\S]*g_verifiedManifestCache'
Require-Match 'UPDATE interrupted pending process is identity-verified and stopped' $app `
    'pendingPaths[\s\S]*recovery\.Record\(\)[\s\S]*pendingPaths\.WinwsExePath\(\)[\s\S]*recovery\.Stop\(\)[\s\S]*interrupted-pending-rolled-back'
Require-Match 'UPDATE startup recovery decides before it acts' ($app + $recovery) `
    'DecidePendingRecovery\([\s\S]*RefuseUnknownRuntime[\s\S]*StopPendingRuntimeThenClear'
Require-Match 'UPDATE a pending runtime is never promoted to current' $recovery `
    'IEqualsAscii\(runningImagePath, pendingWinwsPath\)[\s\S]*StopPendingRuntimeThenClear[\s\S]*RefuseUnknownRuntime'
Require-Match 'UPDATE a broken runtime rolls back only to a recorded known-good version' $recovery `
    'haveTrustedState \|\| !havePreviousKnownGood[\s\S]*IntegrityAction::Refuse[\s\S]*RollbackToPrevious'
Require-Match 'UPDATE/connect/cleanup declare explicit serialized states' $operationState `
    'Disconnected[\s\S]*Connecting[\s\S]*Connected[\s\S]*Disconnecting[\s\S]*Updating[\s\S]*RollingBack[\s\S]*Error'
Require-Match 'UPDATE/connect/cleanup enforce atomic transitions' ($app + $screens) `
    'TryTransition[\s\S]*AppOperationState::Disconnecting[\s\S]*AppOperationState::Updating'
Require-Match 'UPDATE background check joins its worker before destruction' $checkService `
    'UpdateCheckService::~UpdateCheckService\(\) \{ Cancel\(\); \}[\s\S]*worker_\.joinable\(\)[\s\S]*worker_\.join\(\)'
Reject-Match 'UPDATE background check never detaches its worker' ($checkService + $checkServiceHeader) `
    'detach\(\)'
Reject-Match 'UPDATE background check never touches the interface' ($checkService + $checkServiceHeader) `
    '(?i)(ui_|FrameBuffer|ShowMessage|UiContext|Present\(\)|BeginFrame)'
Require-Match 'UPDATE background check cancellation reaches the HTTP client' ($checkService + $manager) `
    'CheckNow\(true, &cancel\)[\s\S]*manifestOptions\.cancel = cancel[\s\S]*signatureOptions\.cancel = cancel'
Require-Order 'UPDATE startup starts the check before connecting and stops it after' $app @(
    'StartUpdateCheck();', 'DoConnectFlow();', 'updateCheck_->Cancel();')
Reject-Match 'UPDATE startup never waits on network input/output' $app `
    'CheckUpdatesOnStart'
Require-Match 'UPDATE HTTPS-only redirects' $http `
    'WINHTTP_OPTION_REDIRECT_POLICY_NEVER[\s\S]*IsAllowedRedirect'
Require-Match 'UPDATE stable channel follows only the latest stable GitHub release' $managerHeader `
    'https://github\.com/Jacksony100/CHEBURNET/releases/latest/download/update-manifest\.json[\s\S]*releases/latest/download/update-manifest\.json\.sig'
Reject-Match 'UPDATE production endpoint is never pinned to a release candidate' $managerHeader `
    'releases/download/v[^"\s]*-rc\.'
Require-Match 'UPDATE exact download size and hash' ($http + $packageCpp) `
    'expectedSize[\s\S]*expectedSha256[\s\S]*AtomicWriteStream'
Require-Match 'UPDATE package path allowlist' $packageCpp `
    'NormalizePackagePath[\s\S]*duplicate normalized package path[\s\S]*mandatory package content missing'
Require-Match 'STRATEGY importer fails unknown variables' $strategyGen `
    'unsupported BAT variable[\s\S]*unsupported caret escape'
Reject-Match 'STRATEGY runtime has no fixed strategy count' $strategyRuntime `
    '(?i)(strategyCount|All\(\)\.size\(\))\s*(?:==|=)\s*\d+'
Require-Match 'UPSTREAM embedded bytes checked against immutable archive' $upstreamVerify `
    'archive_sha256[\s\S]*Get-StreamSha256[\s\S]*upstream byte mismatch[\s\S]*UPSTREAM_FIDELITY: PASS'
Require-Match 'RELEASE manifest version matches actual PE and package' ($releaseManifest + $versionModel) `
    'payload package metadata does not match provenance/schema[\s\S]*GetVersionInfo[\s\S]*launcher PE version mismatch'
Require-Match 'RELEASE semantic version comes from the authoritative version model' $releaseManifest `
    'Get-CheburnetVersion[\s\S]*does not match source semantic version[\s\S]*Assert-CheburnetLauncherPeVersion'
Require-Match 'RELEASE tag must match the source version and channel' ($releasePreparation + $versionModel) `
    'Assert-CheburnetReleaseTag[\s\S]*does not match source semantic version[\s\S]*tag channel and source release channel disagree'
Reject-Match 'RELEASE never derives the semantic version from the numeric PE version' $releaseManifest `
    '\$LauncherVersion\.0'
Require-Match 'RELEASE package builder re-hashes final serialized entries' $packageBuilder `
    'Re-open the final object[\s\S]*TransformFinalBlock[\s\S]*final package entry SHA-256 mismatch'
Require-Match 'RELEASE signing key must match embedded trust key' $manifestSigner `
    'manifest key_id does not match[\s\S]*EccPublicBlob[\s\S]*private signing key does not match the embedded public key[\s\S]*VerifyData'
Require-Match 'RELEASE independently verifies signed manifest before publish' ($releasePreparation + $manifestVerifier) `
    'sign-update-manifest\.ps1[\s\S]*verify-update-manifest-signature\.ps1[\s\S]*EccPublicBlob[\s\S]*VerifyData'
Require-Match 'RELEASE stable tag cannot be published unsigned' ($authenticodeSign + $releasePreparation) `
    'AUTHENTICODE_SIGNING_REQUIRED[\s\S]*\$requireSignature = -not \$version\.IsPrerelease'
Require-Match 'RELEASE signature policy is fail-closed on every status' $authenticode `
    "Get-AuthenticodeSignature[\s\S]*expected 'Valid'[\s\S]*signature is not timestamped"
Require-Match 'RELEASE published launcher is bound to the signed bytes' $releasePreparation `
    'ExpectedSha256 \$signedLauncherSha256[\s\S]*launcher changed after release metadata generation[\s\S]*signed manifest declares launcher SHA-256'
Require-Order 'RELEASE hashes are generated only after signing' $releaseWorkflow @(
    'authenticode-sign.ps1', 'prepare-release.ps1', 'verify-authenticode.ps1',
    'softprops/action-gh-release')
Require-Order 'RELEASE stable signing step is gated by release channel' $releaseWorkflow @(
    "!contains(github.ref_name, '-rc.')",
    'authenticode-sign.ps1 -File build-release\CHEBURNET.exe -Require')
# Network APIs by name, not the English word "upload": the bundle manifest
# legitimately contains the sentence that it is never uploaded.
Reject-Match 'PRIVACY diagnostics export never uses a network API' ($diagnostics + $diagnosticsHeader) `
    '(?i)(WinHttpOpen|WinHttpConnect|WinHttpSendRequest|InternetOpen|InternetConnect|HttpSendRequest|HttpOpenRequest|WSAStartup|WSASend|WSAConnect|::socket|::connect|::send|curl_easy|URLDownloadToFile|WinHttpClient)'
Require-Match 'PRIVACY diagnostics export redacts identity, paths, addresses and secrets' $diagnostics `
    'kRedactedUser[\s\S]*kRedactedMachine[\s\S]*kRedactedPath[\s\S]*kRedactedAddress[\s\S]*kRedactedSecret'
Require-Match 'PRIVACY diagnostics export redacts every users directory, not only the current profile' $diagnostics `
    'userProfile[\s\S]*userName[\s\S]*machineName[\s\S]*\\users\\[\s\S]*RedactAddresses[\s\S]*RedactSecrets'
Reject-Match 'PRIVACY diagnostics export never reads environment blocks or browser state' $diagnostics `
    '(?i)(GetEnvironmentStrings|CookieContainer|InternetGetCookie|Chrome|Firefox|Edge..User Data)'
Require-Match 'PRIVACY diagnostics export is bounded and describes itself' ($diagnostics + $diagnosticsHeader) `
    'maxLogBytes[\s\S]*never_collected[\s\S]*manifest\.json'
Require-Match 'PRIVACY diagnostics export never silently overwrites' $diagnostics `
    'CREATE_NEW[\s\S]*ERROR_FILE_EXISTS'
Require-Match 'PRIVACY diagnostics export requires an explicit user action' $screens `
    'ScreenDiagnosticsExport[\s\S]*bundle\.preview[\s\S]*Confirm\([\s\S]*WriteZipArchive'
Require-Match 'LAUNCHER DLL search hardening is fail-closed' $main `
    'if \(!::SetDefaultDllDirectories\(LOAD_LIBRARY_SEARCH_SYSTEM32\)\)[\s\S]*return 6;'
Require-Match 'LAUNCHER instance mutex has protected admin/system security' $main `
    'O:BAD:P\(A;;GA;;;SY\)\(A;;GA;;;BA\)[\s\S]*CreateMutexW\(&mutexSecurity'

if ($failures -ne 0) {
    Write-Output "SECURITY_HARDENING_V2: FAIL ($failures regression checks)"
    exit 1
}
Write-Output 'SECURITY_HARDENING_V2: PASS'
Write-Output 'UPDATE_SECURITY: PASS'

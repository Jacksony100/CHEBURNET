[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$File,
    [string]$CertificateBase64 = $env:AUTHENTICODE_PFX_B64,
    [string]$CertificatePassword = $env:AUTHENTICODE_PFX_PASSWORD
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Convert-UnicodeMessage([string]$Value) {
    return [regex]::Unescape($Value)
}

if ([string]::IsNullOrWhiteSpace($CertificateBase64)) {
    Write-Warning (Convert-UnicodeMessage '\u0421\u0435\u0440\u0442\u0438\u0444\u0438\u043a\u0430\u0442 Authenticode \u043d\u0435 \u043d\u0430\u0441\u0442\u0440\u043e\u0435\u043d: \u0430\u0440\u0442\u0435\u0444\u0430\u043a\u0442 \u0431\u0443\u0434\u0435\u0442 \u043e\u043f\u0443\u0431\u043b\u0438\u043a\u043e\u0432\u0430\u043d \u0431\u0435\u0437 \u043f\u043e\u0434\u043f\u0438\u0441\u0438.')
    exit 0
}

$target = [IO.Path]::GetFullPath($File)
if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
    throw ((Convert-UnicodeMessage '\u0424\u0430\u0439\u043b \u0434\u043b\u044f \u043f\u043e\u0434\u043f\u0438\u0441\u0438 \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d: ') + $target)
}

$programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
$signtool = (Get-ChildItem (Join-Path $programFilesX86 'Windows Kits\10\bin') -Filter signtool.exe -Recurse |
    Sort-Object FullName -Descending | Select-Object -First 1).FullName
if ([string]::IsNullOrWhiteSpace($signtool)) {
    throw (Convert-UnicodeMessage '\u041d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d signtool.exe')
}

$temporaryRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
} else {
    [IO.Path]::GetFullPath($env:RUNNER_TEMP)
}
$pfx = Join-Path $temporaryRoot ('cheburnet-release-' + [guid]::NewGuid().ToString('N') + '.pfx')
try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($CertificateBase64))
    & $signtool sign /fd SHA256 /td SHA256 /tr https://timestamp.digicert.com /f $pfx /p $CertificatePassword $target
    if ($LASTEXITCODE -ne 0) {
        throw (Convert-UnicodeMessage '\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043f\u043e\u0434\u043f\u0438\u0441\u0430\u0442\u044c \u0444\u0430\u0439\u043b Authenticode')
    }
    & $signtool verify /pa /all /v $target
    if ($LASTEXITCODE -ne 0) {
        throw (Convert-UnicodeMessage '\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430 Authenticode \u0437\u0430\u0432\u0435\u0440\u0448\u0438\u043b\u0430\u0441\u044c \u043e\u0448\u0438\u0431\u043a\u043e\u0439')
    }
} finally {
    if (Test-Path -LiteralPath $pfx) {
        Remove-Item -LiteralPath $pfx -Force
    }
}

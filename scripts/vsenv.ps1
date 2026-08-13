# vsenv.ps1 - locate Visual Studio (MSVC x64) and expose paths + a dev-env importer.
# Dot-source this from the other scripts:  . "$PSScriptRoot\vsenv.ps1"
$ErrorActionPreference = "Stop"

function Get-VsInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio Build Tools." }
    $inst = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $inst) { throw "No VS installation with the C++ x64 toolset was found." }
    return $inst.Trim()
}

$script:VsInstall = Get-VsInstall
$script:VcVars    = Join-Path $VsInstall "VC\Auxiliary\Build\vcvars64.bat"
$script:CMakeExe  = Join-Path $VsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$script:CTestExe  = Join-Path $VsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$script:NinjaExe  = Join-Path $VsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# Fall back to CMake/CTest on PATH if the bundled ones are absent.
if (-not (Test-Path $CMakeExe)) { $c = Get-Command cmake -ErrorAction SilentlyContinue; if ($c) { $script:CMakeExe = $c.Source } }
if (-not (Test-Path $CTestExe)) { $c = Get-Command ctest -ErrorAction SilentlyContinue; if ($c) { $script:CTestExe = $c.Source } }

function Import-VcVars {
    if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found at $VcVars" }
    cmd /c "`"$VcVars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([A-Za-z_][A-Za-z0-9_()]*)=(.*)$') {
            Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
        }
    }
}

Write-Host "VS:    $VsInstall"
Write-Host "CMake: $CMakeExe"

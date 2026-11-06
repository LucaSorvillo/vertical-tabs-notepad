<#
.SYNOPSIS
    Compila VTabsNotepad (launcher + mod) con Visual Studio (MSVC x64), CMake e Ninja.
.EXAMPLE
    .\build.ps1                  # Release in build\bin
    .\build.ps1 -Config Debug
    .\build.ps1 -Clean           # ricompila da zero (rigenera anche le proiezioni)
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release',
    [switch] $Clean,
    [switch] $RegenerateProjections
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio non trovato (manca vswhere.exe).' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio con gli strumenti C++ x64 (MSVC) non trovato.' }

# Importa l'ambiente del "x64 Native Tools Command Prompt" (cl, link, cmake, ninja).
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -LiteralPath $buildDir -Recurse -Force }

& (Join-Path $root 'tools\Generate-Projections.ps1') -OutDir (Join-Path $buildDir 'generated\cppwinrt') -Force:$RegenerateProjections

cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw 'Configurazione CMake fallita.' }
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'Compilazione fallita.' }

Write-Host ''
Write-Host "Compilazione completata: $(Join-Path $buildDir 'bin')" -ForegroundColor Green

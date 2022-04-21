#requires -Version 5.1
<#
.SYNOPSIS
    Build-and-run helper for ovrwin.

.DESCRIPTION
    dd test    Build the CLI, then run the behavioural test suite.
    dd run     Build the GUI app, then launch it.
    dd cli     Build the CLI, then run it with whatever arguments follow.

    Both commands build the code first. Output binaries land in bin\ using the
    naming convention <name><64|32><r|d>, e.g. ovrwincli64d.exe, ovrwinapp64r.exe.

.PARAMETER Command
    'test', 'run' or 'cli'.

.PARAMETER Release
    Force the Release configuration. By default 'run' builds Release and
    'test' and 'cli' build Debug.

.PARAMETER DebugBuild
    Force the Debug configuration (overrides the 'run' default of Release).

.PARAMETER Win32
    Build the 32-bit (Win32) platform instead of x64.

.PARAMETER Arguments
    Everything after the command is passed through to the binary.

.EXAMPLE
    .\dd.ps1 test
    .\dd.ps1 run
    .\dd.ps1 cli --info .\samples\game\GAME.EXE
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('test', 'run', 'cli')]
    [string]$Command,

    [switch]$Release,

    [switch]$DebugBuild,

    [switch]$Win32,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not $Command) {
    Write-Host 'Usage: .\dd.ps1 COMMAND [options] [arguments]'
    Write-Host ''
    Write-Host 'Commands:'
    Write-Host '  test    Build and run the test suite (Debug by default)'
    Write-Host '  run     Build and launch the GUI app (Release by default)'
    Write-Host '  cli     Build and run the CLI with the remaining arguments'
    Write-Host ''
    Write-Host 'Options: -Release, -DebugBuild, -Win32'
    exit 0
}

# Resolve configuration: explicit -Release/-DebugBuild win; otherwise 'run'
# defaults to Release and everything else to Debug.
if ($Release) { $configuration = 'Release' }
elseif ($DebugBuild) { $configuration = 'Debug' }
elseif ($Command -eq 'run') { $configuration = 'Release' }
else { $configuration = 'Debug' }

# Resolve platform and the matching binary-name suffix.
$platform   = if ($Win32) { 'Win32' } else { 'x64' }
$archSuffix = if ($Win32) { '32'    } else { '64' }
$cfgSuffix  = if ($configuration -eq 'Release') { 'r' } else { 'd' }
$suffix     = "$archSuffix$cfgSuffix"

switch ($Command) {
    'run' {
        $project = Join-Path $root 'app\ovrwin-app.vcxproj'
        $exeName = "ovrwinapp$suffix.exe"
        $exeArgs = $Arguments
    }
    'test' {
        $project = Join-Path $root 'cli\ovrwin-cli.vcxproj'
        $exeName = "ovrwincli$suffix.exe"
        $exeArgs = @('--test')
    }
    'cli' {
        $project = Join-Path $root 'cli\ovrwin-cli.vcxproj'
        $exeName = "ovrwincli$suffix.exe"
        $exeArgs = $Arguments
    }
}

# Locate MSBuild via vswhere (works from any shell, no dev prompt required).
function Get-MSBuildPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere'. Is Visual Studio installed?"
    }

    $installPath = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.Component.MSBuild `
        -property installationPath
    if (-not $installPath) {
        throw 'Could not locate a Visual Studio installation with MSBuild.'
    }

    $candidates = @(
        (Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'),
        (Join-Path $installPath 'MSBuild\Current\Bin\amd64\MSBuild.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    throw "MSBuild.exe not found under '$installPath'."
}

$msbuild = Get-MSBuildPath

Write-Host "Building $configuration|$platform ..." -ForegroundColor Cyan
& $msbuild $project /m /nologo `
    /p:Configuration=$configuration `
    /p:Platform=$platform `
    /p:SolutionDir="$root\"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$exePath = Join-Path $root "bin\$exeName"
if (-not (Test-Path $exePath)) {
    throw "Expected binary not found: $exePath"
}

Write-Host "Launching $exeName ..." -ForegroundColor Green
if ($exeArgs) { & $exePath @exeArgs } else { & $exePath }
exit $LASTEXITCODE

<#
.SYNOPSIS
    ovrwin build helper.

.DESCRIPTION
    dd run   - build and launch the app
    dd build - build everything
    dd test  - build and run the behavioural test suite
    dd cli   - build and run the CLI with the remaining arguments
    dd bench - build, compile cc\bench\bench.c for this machine with the
               compiler that was just built, and run it here. With -Exe, write
               it out as a Windows program in exe\ and run that instead.
    dd clean - remove build/ and exe/

    Pass -Config Debug to build a command's target as Debug instead. Binaries
    land in exe\ as ovrwin.exe and ovrwincli.exe, with a -d suffix for Debug.

.EXAMPLE
    .\dd.ps1 test
    .\dd.ps1 run .\GAME
    .\dd.ps1 cli --info .\GAME\GAME.EXE
    .\dd.ps1 bench
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('run', 'build', 'test', 'cli', 'bench', 'clean')]
    [string] $Command = 'run',

    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',

    # For 'bench': write a standalone Windows executable and run that rather
    # than calling the compiled code in process.
    [switch] $Exe,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$suffix = if ($Config -eq 'Debug') { '-d' } else { '' }
$appExe = Join-Path $root "exe\ovrwin$suffix.exe"
$cliExe = Join-Path $root "exe\ovrwincli$suffix.exe"

function Get-VisualStudioPath {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe was not found; install Visual Studio with the C++ desktop workload.'
    }

    $path = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1

    if (-not $path) {
        throw 'No Visual Studio installation with the C++ desktop workload was found.'
    }

    return $path
}

# cl and rc only work inside the MSVC environment, and Ninja invokes cl directly.
function Enter-MsvcEnvironment {
    param([string] $VisualStudio)

    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
        return
    }

    $vcvars = Join-Path $VisualStudio 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat was not found at $vcvars."
    }

    & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

# Prefer whatever is on PATH, then the copy Visual Studio ships, so neither tool
# has to be installed separately.
function Resolve-Tool {
    param([string] $Name, [string] $VisualStudio, [string] $BundledRelativePath)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $bundled = Join-Path $VisualStudio $BundledRelativePath
    if (Test-Path $bundled) {
        return $bundled
    }

    throw "$Name was not found on PATH or under $VisualStudio."
}

function Invoke-Build {
    $vs = Get-VisualStudioPath
    Enter-MsvcEnvironment -VisualStudio $vs

    $cmake = Resolve-Tool -Name 'cmake' -VisualStudio $vs `
        -BundledRelativePath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Resolve-Tool -Name 'ninja' -VisualStudio $vs `
        -BundledRelativePath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

    $env:PATH = "$(Split-Path $ninja);$env:PATH"

    # The linker fails with LNK1168 if the previous build is still running.
    Get-Process ovrwin, ovrwin-d, ovrwincli, ovrwincli-d -ErrorAction SilentlyContinue | Stop-Process -Force

    $preset = $Config.ToLowerInvariant()

    Push-Location $root
    try {
        & $cmake --preset $preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & $cmake --build --preset $preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Pop-Location
    }
}

switch ($Command) {
    'build' { Invoke-Build }

    'run' {
        Invoke-Build
        Start-Process -FilePath $appExe -WorkingDirectory $root -ArgumentList $Rest
    }

    'test' {
        Invoke-Build
        & $cliExe --test
        $code = $LASTEXITCODE

        # GUI subsystem app: Out-String makes PowerShell wait for it to finish.
        & $appExe '-test' | Out-String | Write-Host
        if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE }

        exit $code
    }

    'cli' {
        Invoke-Build
        & $cliExe @Rest
        exit $LASTEXITCODE
    }

    'bench' {
        Invoke-Build

        $source = if ($Rest) { $Rest[0] } else { Join-Path $root 'cc\bench\bench.c' }

        if (-not $Exe) {
            & $cliExe --ccnative $source
            exit $LASTEXITCODE
        }

        $program = Join-Path $root ('exe\' + [System.IO.Path]::GetFileNameWithoutExtension($source) + '.exe')
        & $cliExe --ccexe $source -o $program
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & $program
        $answer = $LASTEXITCODE
        Write-Host "exit code $answer"

        # What the program returned is its answer, not this script's verdict;
        # only a fault, which comes back as a status code rather than a small
        # number, is a failure here.
        exit $(if ($answer -lt 0 -or $answer -gt 255) { 1 } else { 0 })
    }

    'clean' {
        Get-Process ovrwin, ovrwin-d, ovrwincli, ovrwincli-d -ErrorAction SilentlyContinue | Stop-Process -Force
        foreach ($path in @('build', 'exe')) {
            $full = Join-Path $root $path
            if (Test-Path $full) { Remove-Item -LiteralPath $full -Recurse -Force }
        }
    }
}

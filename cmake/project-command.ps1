#requires -Version 7.4
# dd project command backing script for ovrwin.
#
# Ported from the pre-dd bespoke driver. Compiles a C source with ovrwin's own C
# compiler and runs it, either natively in-process (--ccnative) or by emitting an
# executable first (--ccexe). Build the CLI with `dd build` before invoking this.
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$request = [Console]::In.ReadLine() | ConvertFrom-Json -AsHashtable
$root = Split-Path $PSScriptRoot
if ($request.schema -ne 1 -or $request.projectRoot -ne $root -or $request.parameters -isnot [hashtable] -or
    $request.dryRun -isnot [bool] -or $request.command -notin @('bench')) {
    throw 'Invalid project-command request.'
}
Set-Location $root

$p = $request.parameters
$config = if ($p.config) { [string]$p.config } else { 'release' }
$suffix = if ($config -eq 'debug') { '-d' } else { '' }
$ext = if ($IsWindows) { '.exe' } else { '' }
$cli = Join-Path $root "exe/ovrwincli$suffix$ext"

$source = if ($p.source) { [string]$p.source } else { 'cc/bench/bench.c' }
$sourcePath = [IO.Path]::GetFullPath((Join-Path $root $source))
if (-not $sourcePath.StartsWith([IO.Path]::GetFullPath($root), $(if ($IsWindows) { 'OrdinalIgnoreCase' } else { 'Ordinal' }))) {
    throw 'Benchmark source must live inside the project.'
}

$emitExe = [bool]$p.exe
# dd wants the files a command wrote as project-relative paths.
$programRelative = 'exe/' + [IO.Path]::GetFileNameWithoutExtension($sourcePath) + $ext
$program = Join-Path $root $programRelative

$plan = @{ command = 'bench'; cli = $cli; source = $source; mode = $(if ($emitExe) { 'ccexe' } else { 'ccnative' }) }
if ($emitExe) { $plan.program = $program }

if ($request.dryRun) {
    @{ schema = 1; data = @{ status = 'planned'; plan = $plan }; files = @() } |
        ConvertTo-Json -Depth 10 -Compress | Write-Output
    exit 0
}

if (-not (Test-Path -LiteralPath $cli)) { throw "CLI not built: $cli. Run dd build $config first." }
if (-not (Test-Path -LiteralPath $sourcePath)) { throw "Benchmark source not found: $source" }

if (-not $emitExe) {
    $output = & $cli --ccnative $sourcePath 2>&1 | Out-String
    $code = $LASTEXITCODE
    @{ schema = 1; data = @{ status = 'complete'; plan = $plan; exitCode = $code; output = $output }; files = @() } |
        ConvertTo-Json -Depth 10 -Compress | Write-Output
    exit $code
}

$compile = & $cli --ccexe $sourcePath -o $program 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    [Console]::Error.WriteLine($compile)
    exit $LASTEXITCODE
}
$run = & $program 2>&1 | Out-String
$answer = $LASTEXITCODE

# The benchmark's return value is its answer, not a verdict. Only a fault, which
# surfaces as a status code rather than a small number, counts as a failure here.
$failed = $answer -lt 0 -or $answer -gt 255
@{ schema = 1; data = @{ status = 'complete'; plan = $plan; answer = $answer; compile = $compile; output = $run }
    files = @($programRelative) } | ConvertTo-Json -Depth 10 -Compress | Write-Output
exit $(if ($failed) { 1 } else { 0 })

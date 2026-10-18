# libdos by Zac Walker
#
# What the compiler produces today, so a rewrite of the compiler can be shown
# not to have changed what it means. The port from C++ to C is meant to be
# behaviour-preserving; this is the only thing that can say whether it was.
#
#   .\tools\cc-fingerprint.ps1 -Save     record the current output as expected
#   .\tools\cc-fingerprint.ps1           compare against what was recorded
#
# A difference is not automatically a failure - a better instruction sequence
# is still a correct one - but it has to be looked at and re-recorded on
# purpose rather than noticed later.

param(
	[switch]$Save,
	[switch]$Release
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# dd builds the CLI into exe\, with a -d suffix for Debug.
$name = 'demu' + $(if ($Release) { '' } else { '-d' }) + '.exe'
$cli = Join-Path $root "exe\$name"
if (-not (Test-Path $cli)) { throw "$cli is not built. Run .\dd.ps1 build first." }

# Each entry is compiled to a .COM; the bytes are what is being pinned.
$corpus = @(
	@{ Name = 'bench'; Source = 'cc\bench\bench.c'; Args = @() },
	@{ Name = 'bench-386'; Source = 'cc\bench\bench.c'; Args = @('--cpu', '386') },
	@{ Name = 'bench-no-inline'; Source = 'cc\bench\bench.c'; Args = @('--no-inline') },
	@{ Name = 'roundtrip'; Source = 'cc\bench\roundtrip.c'; Args = @() },
	@{ Name = 'shell'; Source = 'shell\shell.c'; Args = @() },
	@{ Name = 'hello'; Source = 'shell\hello.c'; Args = @() }
)

$temp = Join-Path ([System.IO.Path]::GetTempPath()) 'libdos-fingerprint'
New-Item -ItemType Directory -Force -Path $temp | Out-Null

$lines = @()
foreach ($entry in $corpus)
{
	$source = Join-Path $root $entry.Source
	if (-not (Test-Path $source)) { continue }

	$out = Join-Path $temp ($entry.Name + '.com')
	$output = & $cli --cc $source -o $out @($entry.Args) 2>&1
	if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out))
	{
		$lines += "{0,-16} FAILED: {1}" -f $entry.Name, ($output -join ' ')
		continue
	}

	$bytes = (Get-Item $out).Length
	$hash = (Get-FileHash $out -Algorithm SHA256).Hash.Substring(0, 16)
	$lines += "{0,-16} {1,6} bytes  {2}" -f $entry.Name, $bytes, $hash
}

# What it costs to run, which is the half a hash cannot see, and what the same
# program does when it is compiled for this machine instead of for the emulator.
$bench = Join-Path $root 'cc\bench\bench.c'
if (Test-Path $bench)
{
	$report = & $cli --ccbench $bench 2>&1
	foreach ($line in $report)
	{
		if ($line -match '^exit code') { $lines += "{0,-16} {1}" -f 'bench-run', $line }
	}

	$native = & $cli --ccnative $bench 2>&1
	foreach ($line in $native)
	{
		# The time it took is not a fingerprint; what it answered is.
		if ($line -match '^checksum') { $lines += "{0,-16} {1}" -f 'bench-native', $line }
		if ($line -match '^returned (\d+)') { $lines += "{0,-16} returned {1}" -f 'bench-native', $Matches[1] }
	}
}

$expected = Join-Path $PSScriptRoot 'cc-fingerprint.txt'
$text = ($lines -join "`n") + "`n"

if ($Save)
{
	Set-Content -Path $expected -Value $text -NoNewline -Encoding utf8
	Write-Host $text
	Write-Host "Recorded $expected"
	exit 0
}

Write-Host $text
if (-not (Test-Path $expected))
{
	Write-Host 'Nothing recorded yet. Run with -Save.'
	exit 1
}

$before = (Get-Content $expected -Raw) -replace "`r`n", "`n"
if ($before -eq $text)
{
	Write-Host 'Unchanged.'
	exit 0
}

Write-Host 'CHANGED:'
Compare-Object ($before -split "`n") ($text -split "`n") |
	ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
exit 1

# Regenerates src/rom-font.cpp, the 8x8 character generator the text modes
# render from.
#
# Codes 0-127 are the IBM PC BIOS font, taken from the VGA text renderer posted
# at https://github.com/HaxeFoundation/code-cookbook/issues/55. The upper half
# of code page 437 is not in that listing, so the shading blocks and the
# line-drawing set are generated here instead - they are geometric, so the
# exact shapes fall out of which edges each character reaches.
#
#   .\tools\make-font.ps1              # writes src\rom-font.cpp
#   .\tools\make-font.ps1 -Preview 65  # prints one glyph as ASCII art

[CmdletBinding()]
param(
    [int]$Preview = -1,
    [string]$Source = 'https://api.github.com/repos/HaxeFoundation/code-cookbook/issues/55'
)

$ErrorActionPreference = 'Stop'

# Box-drawing characters described as which edges they reach and whether that
# edge is a single or a double line: left, right, up, down; 0 none, 1 single,
# 2 double.
$lineArt = @{
    179 = '0011'; 180 = '1011'; 181 = '2011'; 182 = '1022'; 183 = '1002'; 184 = '2001'
    185 = '2022'; 186 = '0022'; 187 = '2002'; 188 = '2020'; 189 = '1020'; 190 = '2010'
    191 = '1001'; 192 = '0110'; 193 = '1110'; 194 = '1101'; 195 = '0111'; 196 = '1100'
    197 = '1111'; 198 = '0211'; 199 = '0122'; 200 = '0220'; 201 = '0202'; 202 = '2220'
    203 = '2202'; 204 = '0222'; 205 = '2200'; 206 = '2222'; 207 = '2210'; 208 = '1120'
    209 = '2201'; 210 = '1102'; 211 = '0120'; 212 = '0210'; 213 = '0201'; 214 = '0102'
    215 = '1122'; 216 = '2211'; 217 = '1010'; 218 = '0101'
}

function Get-BiosGlyphs
{
    param([string]$Url)

    $issue = Invoke-RestMethod -Uri $Url -Headers @{ 'User-Agent' = 'libdos' }
    $start = $issue.body.IndexOf('static var Font')
    $end = $issue.body.IndexOf('];', $start)
    if ($start -lt 0 -or $end -lt 0) { throw 'Could not find the font array in the issue body.' }

    $matched = [regex]::Matches($issue.body.Substring($start, $end - $start), '0x[0-9A-Fa-f]{2}')
    if ($matched.Count -ne 1024) { throw "Expected 1024 font bytes, found $($matched.Count)." }

    $rows = New-Object 'System.Byte[]' 2048
    for ($i = 0; $i -lt 1024; $i++) { $rows[$i] = [Convert]::ToByte($matched[$i].Value, 16) }
    return , $rows
}

function Get-LineGlyph
{
    param([string]$Edges)

    $rows = New-Object 'System.Byte[]' 8
    $middle = 3
    $left = [int]::Parse($Edges[0])
    $right = [int]::Parse($Edges[1])
    $up = [int]::Parse($Edges[2])
    $down = [int]::Parse($Edges[3])

    $singleRow = @($middle)
    $doubleRow = @(($middle - 1), ($middle + 1))
    $singleColumn = @(3)
    $doubleColumn = @(2, 4)

    if ($left -ne 0)
    {
        $bars = $singleRow
        if ($left -eq 2) { $bars = $doubleRow }
        foreach ($y in $bars)
        {
            for ($x = 0; $x -le 4; $x++) { $rows[$y] = [byte]($rows[$y] -bor (128 -shr $x)) }
        }
    }
    if ($right -ne 0)
    {
        $bars = $singleRow
        if ($right -eq 2) { $bars = $doubleRow }
        foreach ($y in $bars)
        {
            for ($x = 3; $x -lt 8; $x++) { $rows[$y] = [byte]($rows[$y] -bor (128 -shr $x)) }
        }
    }
    if ($up -ne 0)
    {
        $stems = $singleColumn
        if ($up -eq 2) { $stems = $doubleColumn }
        foreach ($x in $stems)
        {
            for ($y = 0; $y -le ($middle + 1); $y++) { $rows[$y] = [byte]($rows[$y] -bor (128 -shr $x)) }
        }
    }
    if ($down -ne 0)
    {
        $stems = $singleColumn
        if ($down -eq 2) { $stems = $doubleColumn }
        foreach ($x in $stems)
        {
            for ($y = ($middle - 1); $y -lt 8; $y++) { $rows[$y] = [byte]($rows[$y] -bor (128 -shr $x)) }
        }
    }
    return , $rows
}

function Set-DrawingGlyphs
{
    param([byte[]]$Rows)

    for ($y = 0; $y -lt 8; $y++)
    {
        $odd = ($y % 2) -eq 1

        $shade25 = 0x88; if ($odd) { $shade25 = 0x22 }
        $shade50 = 0xAA; if ($odd) { $shade50 = 0x55 }
        $shade75 = 0x77; if ($odd) { $shade75 = 0xDD }
        $lower = 0x00; if ($y -ge 4) { $lower = 0xFF }
        $upper = 0x00; if ($y -lt 4) { $upper = 0xFF }
        $centre = 0x00; if ($y -ge 2 -and $y -lt 6) { $centre = 0x3C }

        $Rows[176 * 8 + $y] = [byte]$shade25
        $Rows[177 * 8 + $y] = [byte]$shade50
        $Rows[178 * 8 + $y] = [byte]$shade75
        $Rows[219 * 8 + $y] = [byte]0xFF
        $Rows[220 * 8 + $y] = [byte]$lower
        $Rows[221 * 8 + $y] = [byte]0xF0
        $Rows[222 * 8 + $y] = [byte]0x0F
        $Rows[223 * 8 + $y] = [byte]$upper
        $Rows[254 * 8 + $y] = [byte]$centre
    }

    foreach ($code in $lineArt.Keys)
    {
        $glyph = Get-LineGlyph -Edges $lineArt[$code]
        for ($y = 0; $y -lt 8; $y++) { $Rows[$code * 8 + $y] = $glyph[$y] }
    }

    # A few of the upper half that programs lean on for plain text.
    $copies = @{ 0xA0 = 0x61; 0xA1 = 0x69; 0xA2 = 0x6F; 0xA3 = 0x75; 0xA4 = 0x6E; 0xA5 = 0x4E }
    foreach ($code in $copies.Keys)
    {
        for ($y = 0; $y -lt 8; $y++) { $Rows[$code * 8 + $y] = $Rows[$copies[$code] * 8 + $y] }
    }
    return , $Rows
}

$font = Set-DrawingGlyphs -Rows (Get-BiosGlyphs -Url $Source)

if ($Preview -ge 0)
{
    for ($y = 0; $y -lt 8; $y++)
    {
        $bits = $font[$Preview * 8 + $y]
        $line = ''
        for ($x = 0; $x -lt 8; $x++)
        {
            if ($bits -band (128 -shr $x)) { $line += '#' } else { $line += '.' }
        }
        Write-Host $line
    }
    return
}

$lines = New-Object System.Collections.Generic.List[string]
for ($code = 0; $code -lt 256; $code++)
{
    $values = @()
    for ($y = 0; $y -lt 8; $y++) { $values += ('0x{0:x2}' -f $font[$code * 8 + $y]) }
    $lines.Add("`t" + ($values -join ', ') + ", // $code")
}

$body = @"
// libdos by Zac Walker
//
// The 8x8 character generator the text modes render from. Codes 0-127 are the
// IBM PC BIOS font; the shading blocks and the line-drawing set above 127 are
// generated from the edges each character reaches. Produced by
// tools/make-font.ps1 - do not hand-edit.

#include <cstdint>

extern const uint8_t rom_font_8x8[256 * 8] = {
$($lines -join "`r`n")
};
"@

$output = Join-Path $PSScriptRoot '..\src\rom-font.cpp'
Set-Content -Path $output -Value $body -Encoding ASCII
Write-Host "Wrote $output"

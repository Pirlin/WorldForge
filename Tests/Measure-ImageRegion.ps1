<#
.SYNOPSIS
Compares a rectangle of two images, or prints a column of brightness values, for debugging rendering differences.

.DESCRIPTION
Region mode (default): prints the mean per-pixel difference in 8-bit levels (largest channel difference, as the
editor audit's difference() measures it) and the signed mean brightness difference A minus B. A negative signed
value means A is darker.

Column mode (-Column): prints the brightness (mean of R, G, B) of image A down column X from row Y for H rows, to see
how an edge is shaped (for example how far motion blur spreads it).

Run it from a PowerShell session (not powershell.exe from Bash; the execution policy blocks that).

.EXAMPLE
& .\Tests\Measure-ImageRegion.ps1 -A build\audit\rendering-x\rendering-reflections.png -B build\audit\rendering-x\rendering-player-reflections.png -X 50 -Y 420 -W 300 -H 100
& .\Tests\Measure-ImageRegion.ps1 -A build\audit\rendering-x\rendering-falling-box-moving.png -Column -X 500 -Y 160 -H 20
#>
param(
    [Parameter(Mandatory)][string]$A,
    [string]$B,
    [int]$X = 0,
    [int]$Y = 0,
    [int]$W = 0,
    [int]$H = 0,
    [switch]$Column
)

Add-Type -AssemblyName System.Drawing
$imageA = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
try {
    if ($W -le 0) { $W = $imageA.Width-$X }
    if ($H -le 0) { $H = $imageA.Height-$Y }
    if ($Column) {
        $values = for ($row = $Y; $row -lt [Math]::Min($Y+$H,$imageA.Height); ++$row) {
            $p = $imageA.GetPixel($X,$row)
            [int](($p.R+$p.G+$p.B)/3)
        }
        "column $X rows $Y..$($Y+$H-1): $($values -join ' ')"
        return
    }
    if (-not $B) { throw "Region mode needs -B." }
    $imageB = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))
    try {
        $total = 0.0; $signed = 0.0; $count = 0
        for ($row = $Y; $row -lt [Math]::Min($Y+$H,$imageA.Height); $row += 2) {
            for ($col = $X; $col -lt [Math]::Min($X+$W,$imageA.Width); $col += 2) {
                $p = $imageA.GetPixel($col,$row); $q = $imageB.GetPixel($col,$row)
                $total += [Math]::Max([Math]::Max([Math]::Abs($p.R-$q.R),[Math]::Abs($p.G-$q.G)),[Math]::Abs($p.B-$q.B))
                $signed += (($p.R+$p.G+$p.B)-($q.R+$q.G+$q.B))/3.0
                ++$count
            }
        }
        "region x $X y $Y w $W h $H (every second pixel): mean {0:N2} levels, A minus B {1:N2}" -f ($total/$count),($signed/$count)
    } finally { $imageB.Dispose() }
} finally { $imageA.Dispose() }

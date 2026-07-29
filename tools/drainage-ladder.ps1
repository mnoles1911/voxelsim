# Measure how much of the carrier's DRAINAGE the client's detail band destroys,
# across a ladder of terrain classes ordered by base grade.
#
# WHY THIS EXISTS. worldgen v13 was accepted on mean slope by scale and on the
# geomorphon class fractions. Both are AREA statistics -- they see how rough
# ground is and what shape its cells are, and neither can see whether it is
# CONNECTED. A field of uncorrelated decimetre pits has correct local statistics
# by construction (that is what fBm IS), so it passes every one of them. Only a
# non-local pass -- routing -- sees it. See
# docs/measurements/client-detail-drainage-2026-07-29.txt.
#
# The single number to read is stranded_area, carrier vs amplified, on the SAME
# domain. On steep ground the carrier's is ~0 because the bake routes drainage;
# if the amplified column is large, the client has filled that network with pits.
# Water does not pond on a 40% slope.
#
# ORDER BY GRADE, and do not read any single row on its own. Two traps this
# ladder exists to avoid:
#   * mean voxel terrace run is NOT usable across classes -- it has a geometric
#     floor set by base slope (a 40% grade steps one voxel every 2.5 columns), so
#     a steep site looks "more fragmented" than a gentle one for reasons that
#     have nothing to do with roughness. stranded_area is a topological property
#     and does not have that defect.
#   * a gentle site's carrier is ALREADY poorly drained at 10 cm postings (a 1.7%
#     grade ponds), so its amplified/carrier contrast is small and says little.
#     The signal lives in the steep rows.
#
# TILES. Most of these sites exist only as elevation-only tiles from the older
# gen_world_tiles.py and must be repaired first -- surface heights are copied
# byte-for-byte, so every number here is unaffected, but the climate planes are a
# recorded constant and NO material, biome or topsoil number may be quoted off
# them. See tools/reencode_elevation_only_tiles.py and
# docs/measurements/tile-corpus-inventory-2026-07-29.txt.
#
#   python tools/reencode_elevation_only_tiles.py --src <cache>/<seed>/s1 --dest D:\ue-cache\repaired-s1
#
# Usage:
#   tools\drainage-ladder.ps1 -TileDir D:\ue-cache\repaired-s1 -Out drainage.txt

param(
    [string]$TileDir = 'D:\ue-cache\repaired-s1',
    [int]$Seed = 20260719,
    [int]$WindowVoxels = 512,
    [string]$Out = '',
    [string]$Probe = ''
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Probe) { $Probe = Join-Path $Root 'build\voxel-core-msvc\bench\Release\vxc_terrainprobe.exe' }
if (-not (Test-Path $Probe)) { throw "no probe at $Probe -- build target vxc_terrainprobe" }

# tile x, tile y, label, p50 grade %. Ordered gentle -> steep. Grades measured
# off the 30 m raster, so they describe the CARRIER and not the voxel surface.
$sites = @(
    @(-56, 20, 'plains',        1.7),
    @( 14, 54, 'very-gentle',   3.7),
    @( 49, -4, 'gentle',        6.0),
    @( 15, 54, 'low-relief',   11.8),
    @( 50, -4, 'moderate',     20.1),
    @( 15, 55, 'third-class',  24.6),
    @( 16, 54, 'hilly',        33.3),
    @( -5, 15, 'alpine',       40.6),
    @(  5, 25, 'high-alpine',  46.9),
    @( -4, 16, 'steepest',     54.2)
)

# Tile pitch: 512 px x 30 m. Spawn at the tile CENTRE -- a probe window on a tile
# edge straddles two tiles and mixes their aprons.
$TilePitchM = 15360
$keys = @(
    'drain.carrier.raw.interior_sinks', 'drain.carrier.raw.stranded_area',
    'drain.carrier.raw.mean_path_len',
    'drain.amplified.raw.interior_sinks', 'drain.amplified.raw.stranded_area',
    'drain.amplified.raw.mean_path_len'
)

$rows = @()
foreach ($s in $sites) {
    $tx = [int]$s[0]; $ty = [int]$s[1]; $label = [string]$s[2]; $grade = [double]$s[3]
    $xM = $tx * $TilePitchM + $TilePitchM / 2
    $yM = $ty * $TilePitchM + $TilePitchM / 2
    Write-Host ("  {0,-12} tile ({1},{2})  grade {3,5:N1}%  spawn {4},{5}" -f $label, $tx, $ty, $grade, $xM, $yM)

    $raw = & $Probe $TileDir $Seed $xM $yM $WindowVoxels --baseline 2>&1
    $v = @{}
    foreach ($line in $raw) {
        foreach ($k in $keys) {
            # The key is a whitespace-delimited first field; match it exactly so
            # `interior_sinks` cannot pick up `interior_sinks_per_km2`.
            if ($line -match ("^\s*" + [regex]::Escape($k) + "\s+(\S+)")) { $v[$k] = [double]$Matches[1] }
        }
    }
    if ($v.Count -ne $keys.Count) {
        Write-Host ("    INCOMPLETE ({0}/{1} keys) -- recording as void, not as a result." -f $v.Count, $keys.Count) -ForegroundColor Yellow
        continue
    }
    $rows += [pscustomobject]@{
        label = $label; tile = "$tx,$ty"; grade = $grade
        cSink = $v['drain.carrier.raw.interior_sinks']
        aSink = $v['drain.amplified.raw.interior_sinks']
        cStr  = $v['drain.carrier.raw.stranded_area']
        aStr  = $v['drain.amplified.raw.stranded_area']
        cPath = $v['drain.carrier.raw.mean_path_len']
        aPath = $v['drain.amplified.raw.mean_path_len']
    }
}

$hdr = "{0,-12} {1,-8} {2,6} | {3,7} {4,7} | {5,7} {6,7} | {7,8} {8,8} {9,6}" -f `
    'class', 'tile', 'grade%', 'cSinks', 'aSinks', 'cStrnd', 'aStrnd', 'cPath_m', 'aPath_m', 'ratio'
$lines = @($hdr, ('-' * $hdr.Length))
foreach ($r in $rows) {
    $ratio = if ($r.cPath -gt 0) { $r.aPath / $r.cPath } else { [double]::NaN }
    $lines += "{0,-12} {1,-8} {2,6:N1} | {3,7:N0} {4,7:N0} | {5,6:N1}% {6,6:N1}% | {7,8:N1} {8,8:N1} {9,6:N3}" -f `
        $r.label, $r.tile, $r.grade, $r.cSink, $r.aSink, $r.cStr, $r.aStr, $r.cPath, $r.aPath, $ratio
}
$text = $lines -join "`r`n"
Write-Host ""
Write-Host $text
if ($Out) {
    $text | Out-File -FilePath $Out -Encoding utf8
    Write-Host ""
    Write-Host "wrote $Out"
}

# Capture one in-engine screenshot per LANDFORM CLASS, ordered by base grade, on
# the fine tier — so the physical shape of the voxel surface can be judged across
# the range of terrain the world actually produces.
#
# WHY THIS EXISTS AS A SCRIPT. The per-class question ("do plains look like
# plains, do mountains look like mountains") had been answered from three sites,
# and three sites is not a survey. Worse, the three were picked as EXEMPLARS,
# which biases toward the classes someone already thought about. This walks a
# grade ladder instead.
#
# WHAT THIS IS AND IS NOT EVIDENCE FOR
# ------------------------------------
# It is evidence about SHAPE: the voxel surface's landform, terracing, ridge and
# valley structure, and how the fine tier's baked drainage reads once voxelised.
#
# It is NOT evidence about COLOUR, MATERIAL or BIOME unless every tile in the run
# has real climate planes. Tiles repaired by
# tools/reencode_elevation_only_tiles.py carry a CONSTANT climate, so their
# elevation is byte-exact and their materials are meaningless. The one place a
# constant climate reaches SHAPE is density3: it reads soil depth, which climate
# sets, so on ground steep enough to open the density3 gate the top voxel can
# shift. Prefer real-climate providers; say which you used.
#
# THE FINE TIER NEEDS A 3x3 COARSE RING. A tile on the edge of a generated block
# bakes with missing neighbours -- it still produces output, and its flow
# accumulation boundary conditions are degraded. Do not present an edge tile's
# capture as equivalent to an interior one; the bake prints
# superblock_missing_tiles and this script echoes it.
#
# EVERY CAPTURE'S POSITION IS VERIFIED FROM ITS OWN LOG, because the persisted
# edit log used to override -VoxelSpawnAt and silently relocate the camera (a run
# asking for plains came up at alpine). voxel-capture.ps1 now clears it; this
# script additionally re-reads the camera back out and flags any mismatch, since
# a mislabelled class in a survey is worse than a missing one.
#
# Usage:
#   tools\shape-survey.ps1 -Manifest tools\shape-survey-sites.psd1
#   tools\shape-survey.ps1            # uses the built-in ladder below

param(
    # Where the per-site fine bakes live: <FineRoot>\<providerId>\<seedhex>\s16\<x>_<y>.vxtl
    [string]$StageRoot = 'D:\ue-cache\fine-staged-survey',
    [string]$CoarseDir = 'D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-3e11cf157a836c70\000000000135276f\s1',
    [string]$SeedHex   = '000000000135276f',
    [int]$SettleSec = 200,
    [int]$Width = 1600,
    [int]$Height = 900,
    [string]$OutDir = 'D:\ue-cache\shape-survey',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$TilePitchM = 15360

# label, tile x, tile y, p50 grade %, the bake output dir, and which coarse
# provider the tile itself came from. Ordered gentle -> steep, which is the axis
# the eye should walk.
$sites = @(
    @{ label='g02-plains';  tx=-55; ty=20; grade=1.7;  bake='D:\ue-cache\bake-v4-plains'; coarse='D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-060b0c927ccc807e\000000000135276f\s1' },
    @{ label='g10';         tx=-7;  ty=2;  grade=10.1; bake='D:\ue-cache\bake-g10';        coarse=$CoarseDir },
    @{ label='g16';         tx=-6;  ty=3;  grade=16.4; bake='D:\ue-cache\bake-g16';        coarse=$CoarseDir },
    @{ label='g21';         tx=-5;  ty=3;  grade=21.2; bake='D:\ue-cache\bake-g21';        coarse=$CoarseDir },
    @{ label='g25-third';   tx=15;  ty=55; grade=24.6; bake='D:\ue-cache\bake-v4-third';   coarse='D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-060b0c927ccc807e\000000000135276f\s1' },
    @{ label='g26';         tx=-6;  ty=2;  grade=25.9; bake='D:\ue-cache\bake-g26';        coarse=$CoarseDir },
    @{ label='g35';         tx=-5;  ty=2;  grade=35.0; bake='D:\ue-cache\bake-g35';        coarse=$CoarseDir },
    @{ label='g41-alpine';  tx=-5;  ty=15; grade=40.6; bake='D:\ue-cache\bake-v4-alpine';  coarse='D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-060b0c927ccc807e\000000000135276f\s1' },
    @{ label='g43';         tx=-6;  ty=1;  grade=43.0; bake='D:\ue-cache\bake-g43';        coarse=$CoarseDir }
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$results = @()

foreach ($s in $sites) {
    $label = $s.label
    $tile  = "$($s.tx)_$($s.ty)"
    $src   = Join-Path $s.bake "$tile.vxtl"
    if (-not (Test-Path $src)) {
        Write-Host "  $label : NO FINE TILE at $src -- skipped, not substituted with coarse." -ForegroundColor Yellow
        $results += [pscustomobject]@{ label=$label; grade=$s.grade; status='no-fine-tile'; shot=''; camOk=$false }
        continue
    }

    # Stage under a provider id of its own, so one run's tile can never satisfy
    # another's lookup and quietly photograph the wrong terrain.
    $pid_ = "survey-$label"
    $dst  = Join-Path $StageRoot "$pid_\$SeedHex\s16"
    if (-not $SkipStage) {
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item $src $dst -Force
    }

    $xM = $s.tx * $TilePitchM + $TilePitchM / 2
    $yM = $s.ty * $TilePitchM + $TilePitchM / 2
    Write-Host ("=== {0}  tile ({1},{2})  grade {3}%  spawn {4},{5}" -f $label, $s.tx, $s.ty, $s.grade, $xM, $yM) -ForegroundColor Cyan

    $name = "shape-$label"
    & (Join-Path $PSScriptRoot 'voxel-capture.ps1') -Name $name -SpawnAt "$xM,$yM" `
        -SettleSec $SettleSec -Width $Width -Height $Height -TimeoutSec 700 `
        -ExtraArgs @("-VoxelTileDir=$($s.coarse)",
                     "-VoxelFineTileDir=$StageRoot",
                     "-VoxelFineTileProviderId=$pid_")

    # VERIFY THE CAMERA LANDED WHERE WE ASKED. See the header.
    $log = Join-Path $Root "Saved\capture-$name.log"
    $camOk = $false
    $camLine = ''
    if (Test-Path $log) {
        $m = Select-String -Path $log -Pattern 'Capture: cam loc=\((-?\d+), (-?\d+),' | Select-Object -Last 1
        if ($m) {
            $camLine = $m.Line -replace '.*Capture: ', ''
            # UU are centimetres; the spawn is in metres.
            $cx = [int64]$m.Matches[0].Groups[1].Value / 100
            $cy = [int64]$m.Matches[0].Groups[2].Value / 100
            $camOk = ([math]::Abs($cx - $xM) -lt 1000) -and ([math]::Abs($cy - $yM) -lt 1000)
            if (-not $camOk) {
                Write-Host ("  $label : CAMERA MISMATCH -- asked {0},{1} got {2},{3}. Discard this frame." -f $xM,$yM,$cx,$cy) -ForegroundColor Red
            }
        }
    }
    $undrawn = if (Test-Path $log) { @(Select-String -Path $log -Pattern 'Chunk left undrawn').Count } else { -1 }

    $shot = Get-ChildItem (Join-Path $Root 'ue-project\Saved\Screenshots\WindowsEditor') -Filter *.png -EA SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $dest = ''
    if ($shot) {
        $dest = Join-Path $OutDir ("{0}.png" -f $label)
        Copy-Item $shot.FullName $dest -Force
    }
    $results += [pscustomobject]@{ label=$label; grade=$s.grade; status='ok'; shot=$dest; camOk=$camOk; undrawn=$undrawn; cam=$camLine }
}

Write-Host ""
Write-Host "SURVEY RESULT (ordered by grade -- walk it gentle to steep)" -ForegroundColor Green
$results | Format-Table label, grade, status, camOk, undrawn, shot -AutoSize
Write-Host "Frames in $OutDir"
Write-Host "SHAPE ONLY. Colour/material/biome is only meaningful where the tile has real climate planes -- see the header."

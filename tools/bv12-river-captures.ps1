# Capture the bv12 river: one altitude ladder plus four named reaches, each
# with a -VoxelRiverRibbons=0 control at the byte-identical shutter pose.
#
# Every pose here was verified against the bake BEFORE shooting, with
# vxc_riverribbonprobe over the same four bv12 tiles, and every target is a
# node on the walked thalweg of docs/measurements/river-long-profile-2026-08-04.txt
# -- not a hand-picked tile centre. Three of nine vista sites in this project
# were once wrong, including a "beach" in open water; a site that is only
# plausible is not a site.
#
# THE CAMERA IS PLACED ON THE CHANNEL, UPSTREAM, LOOKING DOWNSTREAM. The offset
# is alt/tan(|pitch|), i.e. the distance at which the frame CENTRE lands on the
# target reach. Looking down the valley rather than across it is what puts
# kilometres of river in the frame instead of one crossing.
#
# THE LADDER SHARES ONE COLUMN ON PURPOSE. All four altitudes sit at
# (-160632,-85613) with the same yaw, so the ribbon's 4 km scan radius gathers
# the SAME reach set at every altitude. That is what makes the reach/quad counts
# comparable across the ladder at all, and it is the only way the near/far
# handover reads: at 10 m the implicit disc punches a hole in the ribbon and the
# quad count must go UP by the segments the disc boundary splits.

param(
    [string[]]$Only = @(),
    [int]$SettleSec = 300
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Out  = Join-Path $Root 'docs\water-map\captures'
New-Item -ItemType Directory -Force $Out | Out-Null

# The bv12 corridor. bake_ver VERIFIED FROM THE TILE BYTES (offset 29, u16),
# not from a directory date -- `-bd3d0ddc7` is newer by mtime and is bake_ver 8
# with flags=3, i.e. no water plane at all, and was nearly used as a baseline.
$Extra = @(
    '-VoxelTileDir=D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4\000000000135276f\s1',
    '-VoxelFineTileDir=D:\voxelsim\tile-cache',
    '-VoxelFineTileProviderId=terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb',
    # Precaution only, exactly as the bv10 run used it: coverage is four tiles,
    # so a high camera can ask for a tile nobody baked. The logs are checked for
    # gateLeaks afterwards and the count is reported either way.
    '-VoxelFineTileGateFatal=0')

# name          spawn                 alt    pitch  yaw     km    what
$Poses = @(
    @{ n='ladder-10m';    at='-160632,-85613'; alt=10;    p=-20; y=-6.8;  km='22.0'; d='lower trunk, near field / handover' },
    @{ n='ladder-1km';    at='-160632,-85613'; alt=1000;  p=-35; y=-6.8;  km='22.0'; d='lower trunk, mid' },
    @{ n='ladder-5km';    at='-160632,-85613'; alt=5000;  p=-55; y=-6.8;  km='22.0'; d='lower trunk, far' },
    @{ n='ladder-20km';   at='-160632,-85613'; alt=20000; p=-75; y=-6.8;  km='22.0'; d='lower trunk, vista' },
    @{ n='head-1km';      at='-165965,-72907'; alt=1000;  p=-35; y=-68.7; km='1.5';  d='steep head, no widening' },
    @{ n='seam-1km';      at='-164870,-75310'; alt=1000;  p=-35; y=-69.0; km='5.0';  d='THE SEAM PINCH, Q -14.6%' },
    @{ n='knick-1km';     at='-162440,-85145'; alt=1000;  p=-35; y=-19.6; km='19.6'; d='knickpoint, Q doubling' },
    @{ n='mouth-1km';     at='-158746,-85733'; alt=1000;  p=-35; y=-28.4; km='24.3'; d='shoreline / coastal lake' }
)

function Evidence($logPath) {
    $g = @{}
    $t = Get-Content $logPath
    $g.settle   = ($t | Select-String 'jobsInFlight=(\d+).*?pendingJobs=(\d+)' | Select-Object -Last 1).Line
    $g.unloaded = ($t | Select-String 'unloaded=(\d+)' | Select-Object -Last 1).Line
    $g.ribbon   = ($t | Select-String 'River ribbons: DRAINED build' | Select-Object -Last 1)
    $g.window   = ($t | Select-String 'River ribbons: window filled' | Select-Object -Last 1)
    $g.disabled = ($t | Select-String 'River ribbons: DISABLED' | Select-Object -Last 1)
    $g.implicit = @($t | Select-String 'RefreshImplicitWater: DRAINED refresh').Count
    $g.draining = @($t | Select-String 'RefreshImplicitWater: STILL DRAINING').Count
    $g.sheet    = @($t | Select-String 'Lake sheets: DRAINED build').Count
    $g.refused  = @($t | Select-String 'was REFUSED').Count
    $g.fine     = ($t | Select-String 'Fine tier \(([^)]*window)\)' | Select-Object -Last 1)
    $g.shutter  = ($t | Select-String 'Capture: cam loc=' | Select-Object -First 1)
    $g.undrawn  = @($t | Select-String 'Chunk left undrawn').Count
    return $g
}

foreach ($P in $Poses) {
    if ($Only.Count -gt 0 -and $Only -notcontains $P.n) { continue }
    foreach ($arm in @('on', 'off')) {
        $name = "bv12-$($P.n)-$arm"
        $dest = Join-Path $Out "$name.png"
        if (Test-Path $dest) { Write-Host "SKIP $name (exists)" -ForegroundColor DarkGray; continue }

        $args = @($Extra)
        # The control suppresses the far-field ribbon ONLY. Near-field implicit
        # water and the lake sheets are untouched, so the pair isolates the one
        # variable and the log says WHICH way it was suppressed -- a
        # suppressed-by-switch control is distinguishable from a
        # suppressed-by-absence one without opening the image.
        if ($arm -eq 'off') { $args += '-VoxelRiverRibbons=0' }

        Write-Host "`n===== $name  ($($P.d), km $($P.km), alt $($P.alt) m, pitch $($P.p), yaw $($P.y))" -ForegroundColor Cyan
        $shot = & "$Root\tools\voxel-capture.ps1" -Name $name -SpawnAt $P.at `
                    -SpawnAltM $P.alt -SpawnPitch $P.p -SpawnYaw $P.y `
                    -SettleSec $SettleSec -TimeoutSec 1200 -ExtraArgs $args
        Copy-Item $shot $dest -Force
        Write-Host "  -> $dest" -ForegroundColor Green

        $e = Evidence (Join-Path $Root "Saved\capture-$name.log")
        Write-Host "  settle   : $($e.settle)"
        Write-Host "  unloaded : $($e.unloaded)"
        Write-Host "  ribbon   : $(if ($e.ribbon) { $e.ribbon.Line -replace '^.*River ribbons: ','' } else { '(none)' })"
        Write-Host "  disabled : $(if ($e.disabled) { 'YES -- control armed' } else { 'no' })"
        Write-Host "  water    : implicit DRAINED x$($e.implicit), STILL DRAINING x$($e.draining), sheets x$($e.sheet)"
        Write-Host "  tiles    : refused $($e.refused), undrawn $($e.undrawn)"
        Write-Host "  fine     : $(if ($e.fine) { $e.fine.Line -replace '^.*Fine tier ','' } else { '(none)' })"
        Write-Host "  shutter  : $(if ($e.shutter) { $e.shutter.Line -replace '^.*Capture: ','' } else { '(none)' })"
    }

    $on  = Join-Path $Out "bv12-$($P.n)-on.png"
    $off = Join-Path $Out "bv12-$($P.n)-off.png"
    if ((Test-Path $on) -and (Test-Path $off)) {
        Write-Host "`n--- pixel diff $($P.n) ---" -ForegroundColor Yellow
        python "$Root\tools\capture-pixdiff.py" $on $off --out (Join-Path $Out "bv12-$($P.n)-diff.png")
    }
}

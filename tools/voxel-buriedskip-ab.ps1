# THE BAND'S A/B. Does the buried-chunk skip earn the GPU it costs?
#
# WHY THIS EXISTS. The GPU tail split (docs/gpu-tail-split-2026-08-27.md) named
# every term in the +5.8 ms rise from a typical frame to p95. Three of them are
# the band and nothing else:
#
#   VoxelStreamRgBand     (BandReduceMain)                   +1.46 ms   25%
#   VoxelStreamRgColumn   (classic ColumnMain, kept graphs)  +0.64 ms   11%
#   VoxelStreamRgVoxelize (classic VoxelizeMain, same)       +0.41 ms    7%
#                                                            -------   ----
#                                                             +2.51 ms   43%
#
# The mesh-region graphs are kept for the band and for NOTHING else -- the leg
# reads `kept because: quads 0, band 31671, noPack 0`. And the band itself is
# computed only when something will consult it: `bComputeBand = BuriedSkipEnabled
# || VerifyBuriedSkipEnabled` (VoxelWorldSubsystem.cpp:21730). So one flag
# removes all three terms at once.
#
# WHAT THE BAND BUYS, which is why this is an A/B and not a deletion. It proves
# a footprint's chunks are all-air or all-solid so they never get meshed. Turning
# it off means MORE chunks are meshed and packed. The question is whether the
# work it avoids is larger than the 2.51 ms it costs, and that can come out
# either way -- which is the point.
#
# IT CANNOT DELETE TERRAIN, and that asymmetry is why this is safe to measure
# before it is safe to ship. The skip REMOVES chunks; disabling it can only ADD
# them. A band-off arm that renders LESS would mean the skip was never the thing
# dropping them. Coverage is still read on both arms rather than assumed.
#
# READ, in this order:
#   1. substituted / coverage  -- the image, before any timing (see the
#      `uncovered is not an arc detector` rule: 0.02pp is visible black arcs).
#   2. gate= / p95 / p99       -- the frame.
#   3. the GPU/ split          -- did the three band terms actually vanish?
#      `[gpu-lean] kept=` must fall to ~0 on the off arm. If it does not, the
#      arm did not engage and every timing below it is void.
#   4. chunks/s                -- what the skip was buying.
#
# ALTERNATED A,B,A,B rather than A,A,B,B: this box has thermal and cache drift
# across a 20-minute sweep, and a block design confounds the arm with the clock.
param(
    [string]$Prefix = 'BSK',
    [int]$RunSec = 120
)
$ErrorActionPreference = 'Stop'
$leg = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$cv  = 'voxel.Stream.CoverageVerify 1,voxel.Stream.FrameAttribution 2'

# arm name -> extra args. Empty = stock control.
$arms = [ordered]@{
    'ctl' = @()
    'off' = @('-VoxelBuriedSkip=0')
}

foreach ($rep in 'a','b') {
    foreach ($arm in $arms.Keys) {
        $name = "$Prefix-$arm-$rep"
        Write-Host "=== $name ===" -ForegroundColor Cyan
        & $leg -LogName $name -Cvars $cv -ExtraArgs $arms[$arm] `
               -SpawnAt '-61440,-61440' -Width 2560 -Height 1440 `
               -Flight line -RunSec $RunSec -PreflightSec 90 -LingerSec 60 `
               -LogIntervalSec 2
        if ($LASTEXITCODE -ne 0) { Write-Host "$name FAILED exit $LASTEXITCODE" -ForegroundColor Red }
    }
}
Write-Host "sweep done" -ForegroundColor Green

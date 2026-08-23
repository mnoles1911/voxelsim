# Diagnose the owner's "below a certain z level from the camera, voxel terrain
# stops rendering".
#
# WHY A LADDER AND NOT ONE SHOT. The symptom is a CUTOFF, so the thing to
# measure is where it sits and what it is proportional to. Three altitudes over
# the same ground answer that in one run:
#
#   * if the cutoff is a fixed DEPTH below the camera, it tracks the camera up
#     the ladder and the amount of visible terrain stays roughly constant;
#   * if it is a fixed WORLD Z, it stays put and more terrain disappears as you
#     climb;
#   * if it is a RADIUS (the marched rings ending at ~1 km while the clipmap
#     carries the rest), the visible voxel patch grows as sqrt of nothing --
#     it stays a disc of fixed ground radius under the camera and the cutoff is
#     a circle, not a plane.
#
# Those three are distinguishable from the captures alone, which is the point:
# no theory needed, and no editor clicks.
#
# HoleStats 2 is armed on every rung. Level 2 is the level+reason breakdown
# (which ring wanted the chunk, and whether it was never admitted / still
# pending / evicted), so the log says WHY the band is empty rather than just
# that it is. Level 1 only gives the uncovered percentage, which this project
# has already recorded as untrustworthy on its own.
#
# THE FINE TIER IS FULLY BAKED HERE -- do not repeat the error this comment
# used to contain. It claimed the fine patch was ~370 m x 250 m and that these
# shots would therefore look coarse. That came from reading -VoxelSpawnAt as
# Unreal units; it is METRES (VoxelEarthGameMode.cpp:61). A fine tile is
# 8192 px x 1875 mm/px = 15.36 km, so the baked block is 46 km x 31 km and the
# whole 4 km cascade sits inside four resident tiles. Surface detail in these
# captures is real, and a coarse-looking band IS a finding, not a bake gap.
#
# SPAWN IS NOT OPTIONAL and not the capture script's default. -84480,53760 is
# outside the baked fine tiles and the gate is fatal there; -61440,-61440 sits
# at the junction of four baked tiles.
param(
    [string]$SpawnAt = '-61440,-61440',
    [double[]]$AltitudesM = @(120, 400, 1200),
    # Pitched down 35 degrees: enough to put the cutoff band in frame at every
    # rung without pointing at the nadir, where there is no horizon to judge
    # the cutoff against.
    [double]$Pitch = -35,
    [string]$Tag = 'zcut'
)

$ErrorActionPreference = 'Stop'
$Capture = Join-Path $PSScriptRoot 'voxel-capture.ps1'

# Level 2 costs shader permutations, so it is passed as a cvar rather than
# baked: the same binary serves the normal legs.
$Cvars = 'voxel.March.HoleStats 2'

foreach ($Alt in $AltitudesM) {
    $Name = "$Tag-alt$([int]$Alt)"
    Write-Host ""
    Write-Host "=== $Name : altitude $Alt m, pitch $Pitch deg ===" -ForegroundColor Cyan
    # NO $LASTEXITCODE CHECK HERE. It is set by NATIVE executables, not by a
    # PowerShell script invoked with '&' -- so it stays empty after a perfectly
    # good capture and "-ne 0" fires on the empty string. That is exactly what
    # happened on the first run of this ladder: rung 1 shot cleanly, wrote its
    # PNG, and the ladder then threw and skipped rungs 2 and 3. voxel-capture.ps1
    # sets $ErrorActionPreference = 'Stop' and throws on real failure, which
    # propagates out of '&' on its own; a second check here can only be wrong.
    & $Capture -Name $Name -SpawnAt $SpawnAt -SpawnAltM $Alt -SpawnPitch $Pitch -Cvars $Cvars
}

Write-Host ""
Write-Host "Captures in ue-project\Saved\Screenshots\WindowsEditor. Read the hole" -ForegroundColor Yellow
Write-Host "breakdown with: grep 'HoleStats' on each run's log -- byLevel says which" -ForegroundColor Yellow
Write-Host "ring wanted the missing chunk, byReason says why it was not there." -ForegroundColor Yellow

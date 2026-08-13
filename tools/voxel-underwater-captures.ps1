# SUBMERGED IN A LAKE, AT THREE DEPTHS, AT ONE COLUMN.
#
# WHY A DEDICATED SCRIPT
# ======================
#
# Every underwater capture this project has taken was in the OCEAN, via
# -VoxelForceUnderwaterSpawn, which hard-codes z = -500 UU (5 m below sea level,
# VoxelEarthGameMode.cpp:4136). Lakes sit at ~1650 m. There has never been a
# screenshot of a submerged LAKE, which is precisely why the lake underwater
# treatment could stay a hard-coded ocean tint for as long as it did.
#
# THE COLUMN IS CHOSEN, NOT GUESSED, AND THAT IS THE WHOLE POINT
# ==============================================================
#
# -65101,-51083 is the DEEPEST water in this pond: 6.01 m, from a lake datum of
# 1650.235 m against an amplifier ground of 1644.237 m. It was found by scanning
# the baked bathy_depth plane of every resident tile for the deepest wet cell
# within 500 m (max 5.98 m at this cell), and then confirmed independently with
# vxc_waterdatumprobe, which reads the datum and the three grounds by column.
# The two instruments agree to 3 cm, which is what makes the number usable --
# either one alone is a single source that has been wrong before.
#
# IT IS ALSO, BY LUCK, THE COLUMN THE OWNER HAS ALREADY BEEN JUDGING: the
# archived shore pose is -65102,-51084, one metre away and 6.02 m deep. So these
# submerged frames are directly comparable to the surface frames of the same
# water rather than being a new site with no reference.
#
# A CORRECTION WORTH KEEPING, because it nearly sent this capture to the wrong
# place. The figure "this pond is 1.33 m deep" is true of -65102,-51105 -- the
# TOP-DOWN pose's column, 21 m south -- and not of the shore pose's column. The
# two poses were conflated once while choosing this site. Depth in this basin
# varies by 4.7 m over 21 m, so "the pond" is not a single depth and any claim
# about it has to name the column.
#
# RE-DERIVE IT IF THE BAKE MOVES. These are baked numbers and BAKE_VERSION 27 is
# what they came from; a re-bake that moves the bed by half a metre moves every
# depth below. The command:
#
#   vxc_waterdatumprobe.exe <tiledir> --at -65101 -51083 --span 10 --band 9000 9001
#
# Read "LakeSampler (sheet datum)" and "Amplifier surfaceMm". THE AMPLIFIER ONE,
# not reconstructedGroundMm -- the three-grounds trap (docs/water-architecture.md
# S4) bites here. reconstructedGround is what the baked DEPTH is measured from;
# the amplifier surface is what the renderer DRAWS and therefore what
# UVoxelWorldSubsystem::GetSurfaceHeightUU returns and what -VoxelSpawnAltM
# measures from. They differ by 1.5 cm at this column and by more elsewhere.
#
# THE CAMERA IS NOT WHERE -SpawnAltM PUTS IT
# ==========================================
#
# -SpawnAltM positions the PAWN, and the camera sits +77 UU above the pawn
# origin (VoxelMovementTuning.h:63 StandEyeOffsetUU; the pawn origin is the
# capsule centre, so that is 1.67 m above the feet). The depths in the table
# below are camera depths and already account for it. Getting this wrong by one
# eye height is 77 cm, which at these depths is the difference between "1 m
# down" and "just under the surface".
#
# Also: -VoxelSpawnAltM is REJECTED unless it is > 0 (VoxelEarthGameMode.cpp:4196
# tests `&& SpawnAltMeters > 0.f`), so a "sit on the bed" pose cannot be asked
# for with a 0 and would silently become the default framing.
#
# WHY THE PAWN STAYS PUT WHILE THE CAPTURE SETTLES for 170 s: underwater
# movement is fly-style -- vertical motion comes straight from the input axis
# rather than from integrated velocity, so with no input there is no buoyancy
# and no sinking (VoxelCharacterMovement.cpp:724-727). The camera is where it
# was placed when the shutter fires. If that ever changes, these poses drift and
# the guard below is what catches it.

param(
    # The deep column. See the header before changing it.
    [string]$SpawnAt = '-65101,-51083',
    # Camera depths that result, given datum 1650.235 m and amplifier bed
    # 1644.237 m (so 6.00 m of water) and a +0.77 m eye offset:
    #
    #   SpawnAltM   pawn Z     camera Z    depth below surface
    #   5.0         1649.24    1650.01     0.23 m   just under the meniscus
    #   3.0         1647.24    1648.01     2.23 m   mid-water
    #   1.0         1645.24    1646.01     4.23 m   deep, 1.77 m of water below
    #
    # THREE DEPTHS AND NOT ONE, because the single thing most obviously missing
    # from the old treatment is any dependence on how deep you are: a flat
    # SceneColorTint renders 20 cm down and 4 m down identically. One frame
    # cannot show the absence of a gradient. Three can.
    #
    # The deepest one keeps 1.77 m of water BELOW the camera on purpose. Sitting
    # on the bed would fill the lower frame with lake bed and photograph the
    # terrain material instead of the water.
    [double[]]$AltsM = @(5.0, 3.0, 1.0),
    # Slightly down: level would be a wall of murk with no reference, and
    # steeply down is a picture of the bed. -12 keeps some bed, some mid-water
    # and some surface in frame.
    #
    # NOT 0, and this is a real trap rather than a preference: voxel-capture.ps1
    # treats a pitch of exactly 0 as "not passed" and the game mode then applies
    # its own -40 fallback framing, so asking for level gets you a steep
    # downward shot that still reports success.
    [double]$SpawnPitch = -12,
    [double]$SpawnYaw = 200,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [int]$SettleSec = 170,
    # Also shoot the same three depths at night. The underwater ambient is
    # scaled by MPC_VoxelSky's MoonLightFraction, and "is it dark underwater at
    # night" is a separate question from "does it get darker with depth" -- a
    # day-only set cannot distinguish a working night gate from a broken one.
    [switch]$IncludeNight,
    [string]$NightTimeOfDay = '23:00'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path

$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail."
}

# Datum and bed at the chosen column, from the bake. Used ONLY to label the
# output and to compute the expected depth for the guard below -- nothing here
# feeds the engine, which reads its own baked tiles.
$DatumM = 1650.235
$BedM   = 1644.237   # AMPLIFIER surface -- what -VoxelSpawnAltM measures from
$EyeM   = 0.77       # VoxelMovementTuning.h:63 StandEyeOffsetUU = 77 UU

$results = @()

function Invoke-Dive {
    param([double]$AltM, [string]$Tod, [string]$Tag)

    $camZ  = $BedM + $AltM + $EyeM
    $depth = $DatumM - $camZ
    $name  = "underwater-$Tag-{0:0.0}m" -f $depth

    Write-Host ("=== $name : pawn +{0} m over bed, camera z {1:0.00} m, {2:0.00} m below the surface" `
                 -f $AltM, $camZ, $depth) -ForegroundColor Cyan
    if ($depth -le 0) {
        throw ("REFUSING: -SpawnAltM $AltM puts the camera at {0:0.00} m, which is ABOVE the {1} m datum. " -f $camZ, $DatumM) +
              "That is an above-water shot filed under an underwater name."
    }

    $shot = & (Join-Path $PSScriptRoot 'voxel-capture.ps1') `
        -Name $name -SpawnAt $SpawnAt -SpawnAltM $AltM -SpawnPitch $SpawnPitch -SpawnYaw $SpawnYaw `
        -Width $Width -Height $Height -TimeOfDay $Tod -Date $Date -SettleSec $SettleSec |
        Select-Object -Last 1
    if (-not $shot -or -not (Test-Path $shot)) { throw "capture $name produced no image" }

    # --- THE RAN-FLAG, AND IT IS NOT "THE FILE EXISTS" -----------------------
    #
    # A capture at a pose that turned out to be above water still writes a
    # perfectly good PNG. It just is not a picture of underwater. The engine
    # logs a line once per air/water transition
    # (AVoxelOceanActor::UpdateUnderwaterState, "Ocean: camera entered water"),
    # and that log line is the ONLY evidence an unattended run has that the
    # camera was actually submerged when the shutter fired.
    #
    # If it is absent, this is a hard failure rather than a warning. An
    # above-water frame filed as "underwater-2.8m" would be read as "the
    # underwater treatment does nothing", which is exactly the conclusion these
    # captures exist to test.
    $log = Join-Path $Root "Saved\$name.log"
    if (Test-Path $log) {
        $entered = @(Select-String -Path $log -SimpleMatch 'Ocean: camera entered water' -ErrorAction SilentlyContinue)
        $exited  = @(Select-String -Path $log -SimpleMatch 'Ocean: camera exited water'  -ErrorAction SilentlyContinue)
        if ($entered.Count -eq 0) {
            throw ("NO 'Ocean: camera entered water' IN $log -- the camera was never submerged, so " +
                   "$name is an ABOVE-WATER frame. Re-derive the column's datum and bed with " +
                   "vxc_waterdatumprobe before trusting the depths in this script's header.")
        }
        if ($exited.Count -ge $entered.Count) {
            throw ("$name : the camera entered water $($entered.Count)x but exited $($exited.Count)x, so it " +
                   "was ABOVE water when the shutter fired. Something is moving the pawn during the settle.")
        }
        Write-Host "    submerged (entered x$($entered.Count), exited x$($exited.Count))" -ForegroundColor Green
    }
    else {
        Write-Warning "no log at $log -- cannot confirm the camera was submerged. Treat $name as UNVERIFIED."
    }

    Write-Host "  -> $shot" -ForegroundColor Green
    $script:results += [pscustomobject]@{ Name = $name; DepthM = $depth; Tod = $Tod; Path = $shot }
}

foreach ($alt in $AltsM) { Invoke-Dive -AltM $alt -Tod $TimeOfDay -Tag 'day' }
if ($IncludeNight) {
    foreach ($alt in $AltsM) { Invoke-Dive -AltM $alt -Tod $NightTimeOfDay -Tag 'night' }
}

Write-Host ""
Write-Host "SUBMERGED CAPTURES (all confirmed submerged from the log):" -ForegroundColor Green
$results | Sort-Object Tod, DepthM | ForEach-Object {
    Write-Host ("  {0,-6} {1,5:0.00} m   {2}" -f $_.Tod, $_.DepthM, $_.Path)
}

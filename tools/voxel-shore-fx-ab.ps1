# SHORELINE EFFECTS, ON versus OFF, at the same two poses.
#
# WHAT IS BEING COMPARED
# ======================
#
# Two features shipped during the bathymetry work and have never been confirmed
# in a screenshot:
#
#   * SHORELINE FOAM on the water (create_water_voxel_material.py's
#     BathyFoamGain, 0.55), driven by the baked signed distance to shore and
#     gated by bed slope so a beach foams and a cliff face does not.
#   * WET-SHORE DARKENING on the land (terrain_material_common.py's
#     WetShoreDarken 0.55 + WetShoreRoughness 0.22), a 2.5 m band of ground
#     inland of the waterline that goes darker AND glossier together.
#
# Both read the same baked field from opposite sides of the waterline. They are
# armed by ONE environment variable, VOXEL_SHORE_FX, precisely so a pair cannot
# be taken with one half on and the other off -- which would be a picture of
# neither feature and would look like a picture of both.
#
# ONE THING THE PAIR ALSO SHOWS, and it is not in either feature's name: the
# water material's Opacity is saturate(foam), so the shore foam is the only
# thing making water opaque in the shore strip -- up to a 55% dim of the volume
# there. The OFF arm loses the foam and that dimming together. They are one
# mechanism, not two, so this is honest; it is stated because "foam off" does
# not sound like it should change the water's transparency, and it does.
#
# WHY BOTH ARMS ARE REGENERATED RATHER THAN ONE
# =============================================
#
# The ON arm is what is currently on disk, so capturing it without rebuilding
# would save two editor launches. It is not worth it. This project has twice
# shipped a water material that was silently not the material anyone thought it
# was -- the 2026-08-10 run where every water surface drew with UE's DEFAULT
# material while the log said success, and the material-parameter-collection
# orphaning behind it. "It should still be the ON arm, nothing has changed it"
# is exactly the assumption both of those failures were made of. Two builds,
# each logging and each read back off the saved package, cost twenty minutes and
# remove the question.
#
# THE ORDER IS OFF FIRST, THEN ON, and that is deliberate rather than
# alphabetical: it means the run ENDS with the shipped arm on disk. A run that
# ends by leaving the measurement arm installed is how a measurement arm becomes
# the shipped material by accident.

param(
    # The shoreline being photographed. Default is the pond the murkiness and
    # shoreline-gap work were both judged at (Saved/capture-murkier-shore.log),
    # so this pair is comparable to those captures rather than being a new site
    # nobody has a reference for.
    [string]$SpawnAt = '-65102,-51084',
    [double]$SpawnYaw = 200,
    # TWO POSES, because the two features have different natural ranges and one
    # frame cannot show both honestly.
    #
    #   WIDE (12 m up, -30 deg) is the archived shore pose. It shows whether the
    #   shoreline reads as a JOIN at the distance the owner actually flies at.
    #   Neither effect is more than a few metres wide, so this is the pose where
    #   "we built it and it is invisible" would show up as invisible.
    #
    #   CLOSE is the pose the owner asked for, and it stands somewhere else
    #   entirely -- see the correction below. A 2.5 m wet band and a ~1.6 m foam
    #   band are a handful of pixels from 12 m up; from a low bank 5 m away they
    #   are the subject of the frame.
    [double]$WideAltM = 12,
    [double]$WidePitch = -30,

    # THE CLOSE POSE STANDS ON THE BANK, NOT OVER THE MIDDLE OF THE POND, AND
    # THE FIRST VERSION OF THIS SCRIPT GOT THAT WRONG IN A WAY WORTH RECORDING.
    #
    # It reused $SpawnAt with a lower altitude, on the assumption that the
    # archived "shore" pose stands near a shore. It does not: -65102,-51084 is
    # the DEEPEST water in this pond (6.02 m; datum 1650.235 m over an amplifier
    # bed of 1644.237 m) and the pose is a look ACROSS the water toward a bank
    # 27 m away. -SpawnAltM measures from the bed, so "4 m up" put the camera at
    # 1648.94 m -- 1.3 m UNDER the surface. The capture succeeded, framed
    # correctly, and photographed the inside of a lake. It was caught by
    # `Ocean: camera entered water` in the log, not by looking at the image.
    #
    # The replacement is read out of the baked signed distance-to-shore rather
    # than reasoned about: -65114,-51108 is dry ground 5.3 m inland of the
    # waterline, on a 0.96 m bank (ground 1651.196 m against the 1650.235 m
    # datum), and yaw 63 deg points from there back at the deep water. Both
    # halves confirmed with vxc_waterdatumprobe -- "DRY" and the amplifier
    # surface -- because the shore field and the datum are different products
    # and agreeing is the check.
    #
    # At 1 m up the camera sits ~2.67 m above the water with the waterline 5.3 m
    # ahead; at -20 deg the wet band (0-2.5 m inland) lands in the lower third
    # and the foam band just beyond it, which is the framing the whole pair is
    # for.
    [string]$CloseSpawnAt = '-65114,-51108',
    [double]$CloseYaw = 63,
    [double]$CloseAltM = 1,
    [double]$ClosePitch = -20,
    [int]$Width = 2560,
    [int]$Height = 1440,
    # Noon, equinox, clock stopped. Same three pins as every other capture on
    # this project: a pair shot under a drifting sun is a picture of the sun.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [int]$SettleSec = 170,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [int]$RegenTimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Tools   = Join-Path $Root 'ue-project\Tools'
$LogDir  = Join-Path $Root 'Saved'
$OutDir  = Join-Path $LogDir 'shore-fx-ab'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ONE EDITOR PER BOX. Same rule and same reason as every other runner here: two
# editors on this machine have previously destroyed each other's frames.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail."
}

# THE THREE MATERIALS THE ARM TOUCHES.
#
# The water one is obvious. BOTH terrain materials are here because the wet band
# straddles the boundary between them: M_VoxelTerrain shades the near-field
# voxel chunks and M_VoxelClipmap shades the heightmap rings behind them, they
# share the WetShoreDarken/WetShoreRoughness parameter names by design, and
# rebuilding only one would put a wet band on one side of the ring seam and not
# the other -- a difference that looks exactly like a bug in the feature.
#
# The SKY is deliberately NOT rebuilt. create_sky_material.py deletes and
# recreates MPC_VoxelSky, every sky-bound material holds a binding to it, and
# rebuilding it here would risk the 2026-08-10 default-material failure for no
# reason: nothing in this arm touches the sky. Same reasoning as
# voxel-water-star-regen.ps1's -WaterOnly switch.
$scripts = @(
    'create_water_voxel_material.py',
    'create_voxel_material.py',
    'create_clipmap_material.py'
)

function Invoke-Regen {
    param([string]$Arm)

    $env:VOXEL_SHORE_FX = $(if ($Arm -eq 'on') { '1' } else { '0' })
    Write-Host "=== REGEN arm '$Arm' (VOXEL_SHORE_FX=$($env:VOXEL_SHORE_FX))" -ForegroundColor Cyan

    foreach ($s in $scripts) {
        $stem = $s -replace '\.py$',''
        $log  = Join-Path $OutDir "regen-$Arm-$stem.log"
        Write-Host "  $s -> $log" -ForegroundColor Gray
        $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList @(
            "`"$Project`"", '-run=pythonscript', "-script=`"$Tools\$s`"",
            '-unattended', '-nop4', '-nosplash', "-abslog=`"$log`""
        )
        $p.WaitForExit($RegenTimeoutSec * 1000) | Out-Null
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "$s TIMED OUT after ${RegenTimeoutSec}s" }

        # THE EXIT CODE IS NOT THE TEST, IN EITHER DIRECTION. UE's end-of-run
        # summary counts three PRE-EXISTING project errors unrelated to any
        # script, so a perfect run still exits 1; and an exception inside a
        # -run=pythonscript can still leave a 0. The commandlet's own markers
        # are the test. (Measured on this project 2026-08-11; see
        # voxel-water-star-regen.ps1 for the full account.)
        $ok = Select-String -Path $log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
        $py = @(Select-String -Path $log -SimpleMatch 'LogPython: Error' -ErrorAction SilentlyContinue)
        if (-not $ok -or $py.Count -gt 0) {
            if ($py.Count -gt 0) { $py | Select-Object -First 12 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red } }
            throw "$s FAILED (successMarker=$([bool]$ok), pythonErrors=$($py.Count), exit=$($p.ExitCode)) -- see $log"
        }

        # A material that fails to COMPILE is replaced by the engine default,
        # which renders, and renders fast. An arm built from a failed compile
        # would photograph as a dramatic difference and be read as the feature.
        $failed = @(Select-String -Path $log -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
        if ($failed.Count -gt 0) {
            $failed | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
            throw "$s produced $($failed.Count) material compile failures -- see $log"
        }

        # --- READ THE ARM BACK OUT OF THE LOG -----------------------------
        #
        # This is the ran-flag, and "the script exited 0" is not one: it is
        # equally true of a run the environment variable never reached. An
        # env var that fails to arrive does not error, it silently builds the
        # other arm, and an A/B whose two halves are the same shader shows a
        # delta of zero -- which is exactly what "the feature does nothing"
        # looks like. That is the conclusion this run exists to test, so it is
        # the one conclusion it must not be able to fake.
        $pattern = if ($s -like '*water*') { 'SHORE FX ARM: (\w+)' } else { 'TERRAIN SHORE FX ARM: (\w+)' }
        $line = Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue | Select-Object -Last 1
        if (-not $line) { throw "no arm line in $log -- the generator that prints it did not run, so which shader is on disk is UNKNOWN." }
        $built = $line.Matches[0].Groups[1].Value
        if ($built -ne $Arm.ToUpper()) { throw "ASKED FOR '$($Arm.ToUpper())' BUT '$s' BUILT '$built' -- $($line.Line)" }
        Write-Host "    $($line.Line -replace '^.*?(TERRAIN SHORE FX|SHORE FX)','$1')" -ForegroundColor Green

        # AND THE SAME CLAIM OFF THE ARTEFACT, NOT OFF THE LOG.
        #
        # Only for the terrain materials, and the asymmetry is the point. The
        # terrain arm changes the graph's SHAPE -- the wet band's parameters
        # are created or they are not -- so the saved package's name table
        # names WetShoreDarken iff the arm is on. The WATER arm changes one
        # scalar's DEFAULT VALUE and creates the same node either way, so the
        # name table says BathyFoamGain in BOTH arms and this check cannot see
        # it. Claiming otherwise would be worse than not checking: see the
        # 'Time nodes=UNKNOWN' note in voxel-water-star-regen.ps1 for why
        # reporting a blind spot as a pass is the failure that costs days.
        if ($s -notlike '*water*') {
            $asset = if ($s -like '*clipmap*') { 'M_VoxelClipmap' } else { 'M_VoxelTerrain' }
            $uasset = Join-Path $Root "ue-project\Content\Voxel\$asset.uasset"
            if (Test-Path $uasset) {
                $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($uasset))
                $has = $text.Contains('WetShoreDarken')
                $diskArm = if ($has) { 'ON' } else { 'OFF' }
                if ($diskArm -ne $Arm.ToUpper()) {
                    throw ("THE PACKAGE ON DISK DISAGREES: asked for '$($Arm.ToUpper())' but $asset.uasset " +
                           "reads '$diskArm' (WetShoreDarken present=$has).")
                }
                Write-Host "    package read-back: $asset.uasset = $diskArm" -ForegroundColor Green
            }
        }
    }
}

function Invoke-Shot {
    # The column and yaw are per-POSE and not per-run: the wide pose looks
    # across the deep water from above, the close pose stands on a bank 27 m
    # away and looks back at it. They were one column until the close pose
    # turned out to be underwater; see the $CloseSpawnAt note above.
    param([string]$Arm, [string]$Pose, [string]$At, [double]$AltM, [double]$Pitch, [double]$Yaw)

    $name = "shorefx-$Arm-$Pose"
    Write-Host "=== CAPTURE $name (alt ${AltM} m, pitch ${Pitch} deg)" -ForegroundColor Cyan
    # voxel-capture.ps1 signals failure by THROWING and signals success by
    # emitting the image path -- it is not an exe, so $LASTEXITCODE says nothing
    # about it and testing that would pass on every failure. Take the path.
    $shot = & (Join-Path $PSScriptRoot 'voxel-capture.ps1') `
        -Name $name -SpawnAt $At -SpawnAltM $AltM -SpawnPitch $Pitch -SpawnYaw $Yaw `
        -Width $Width -Height $Height -TimeOfDay $TimeOfDay -Date $Date -SettleSec $SettleSec |
        Select-Object -Last 1
    if (-not $shot -or -not (Test-Path $shot)) { throw "capture $name produced no image" }

    # --- NEITHER POSE MAY BE UNDERWATER, AND THIS IS NOT A THEORETICAL GUARD --
    #
    # The first version of this script shot its "close" pose 1.3 m BELOW the
    # surface. -SpawnAltM measures from the terrain surface, which inside a lake
    # is the BED, and the column it reused is the pond's deep end -- so "4 m up"
    # was 2 m down. The capture succeeded, framed exactly as requested, reported
    # success, and photographed the inside of a lake. Nothing in the pipeline
    # objected. It was noticed only because someone read
    # `Ocean: camera entered water` in the log for an unrelated reason.
    #
    # A submerged frame in a shoreline A/B is worse than a failed one: foam and
    # wet-shore are both above-water effects, so the pair would show no
    # difference and be read as "we built two features and neither does
    # anything". Hard failure.
    $log = Join-Path $Root "Saved\capture-$name.log"
    if (Test-Path $log) {
        $sub = @(Select-String -Path $log -SimpleMatch 'Ocean: camera entered water' -ErrorAction SilentlyContinue)
        if ($sub.Count -gt 0) {
            throw ("$name IS AN UNDERWATER FRAME -- the log says 'Ocean: camera entered water'. " +
                   "$($sub[0].Line.Trim())`n" +
                   "Shoreline effects are above-water effects, so this pair would show no difference " +
                   "for a reason that has nothing to do with the feature. Re-derive the pose: " +
                   "-SpawnAltM measures from the BED inside a lake, not from the water surface.")
        }
    }
    else { Write-Warning "no log at $log -- cannot confirm $name is an above-water frame." }

    $script:Shots[$name] = $shot
    Write-Host "  -> $shot" -ForegroundColor Green
}

$script:Shots = @{}

# OFF FIRST so the run ends with the shipped arm installed -- see the header.
foreach ($arm in @('off','on')) {
    Invoke-Regen -Arm $arm
    Invoke-Shot -Arm $arm -Pose 'wide'  -At $SpawnAt      -AltM $WideAltM  -Pitch $WidePitch  -Yaw $SpawnYaw
    Invoke-Shot -Arm $arm -Pose 'close' -At $CloseSpawnAt -AltM $CloseAltM -Pitch $ClosePitch -Yaw $CloseYaw
}

Write-Host ""
Write-Host "PAIRS READY -- compare WITHIN a row, never across one:" -ForegroundColor Green
foreach ($pose in @('wide','close')) {
    Write-Host "  ${pose}:"
    foreach ($arm in @('off','on')) {
        $k = "shorefx-$arm-$pose"
        Write-Host ("    {0,-4} {1}" -f $arm.ToUpper(), $script:Shots[$k])
    }
}
Write-Host "Shipped arm (ON) is what is on disk now." -ForegroundColor Green

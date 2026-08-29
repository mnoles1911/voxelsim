# Run ONE headless flight leg that takes SCREENSHOTS WHILE MOVING, and say where
# they landed.
#
# ============================================================================
# WHY A MOVING CAPTURE EXISTS AT ALL
# ============================================================================
#
# Every capture this project takes settles first: park the camera, let the
# cascade converge for ~120 s, then fire. That protocol is what makes two arms
# comparable, and it is also why two live decisions cannot be settled at all:
#
#   * docs/outer-ring-stagger-2026-08-28.md, section "WHAT NO INSTRUMENT HERE
#     CAN SHOW". The stagger arm trades a transient far-field lag WHILE MOVING
#     for a better worst frame. A settled capture lets both arms converge on the
#     same world long before the shutter, so the parked A/B is "structurally
#     incapable of showing the thing this change trades away". That arm is
#     parked on exactly this.
#   * The half-res marcher was rejected for being "grainy at distance while
#     moving" -- a verdict nobody could photograph, so nobody could check it,
#     argue with it, or measure a cure against it.
#
# The project's own limitation note reads "no moving-capture capability exists".
# This script and -VoxelPerfShotEveryM= are that capability.
#
# ============================================================================
# THE RULE THIS SCRIPT EXISTS TO ENFORCE: AN IMAGE LEG IS NOT A TIMING LEG
# ============================================================================
#
# A shutter STALLS the frame it is serviced on -- the viewport reads back the
# render target and the PNG encode runs off it. So a moving capture perturbs the
# very timing it is flying through. Worse for an A/B, the perturbation is not
# symmetric: shots fire at fixed DISTANCES, so the slower arm takes the same
# number of stalls over fewer frames and wears a larger fraction of them. A
# timing comparison drawn from two image legs is therefore biased against the
# slower arm BY THE INSTRUMENT.
#
# So: shoot images on one leg, take timings on another, and never quote the two
# out of the same run. The engine says so at arm time and again at the end of
# the run, and the summary JSON carries frameTimingAdmissible: 0. This script
# says it once more, on the console, because the console is what gets pasted.
#
# ============================================================================
# WHAT THIS SCRIPT DOES *NOT* DO, ON PURPOSE
# ============================================================================
#
# IT DOES NOT REIMPLEMENT THE LEG. tools\voxel-run-flight-leg.ps1 owns the
# one-editor-and-no-build guard, the frozen-sun pins, the -ExecCmds quoting fix,
# the resolution ini overrides, the exit-code contract and the three-witness
# void test ("VoxelPerfRun complete" in the log AND a fresh perf_*.json). All of
# that is DELEGATED by invoking that script -- not copied. Copying it would
# fork the guard, and a guard that exists in two versions is a guard that is one
# edit away from not existing in the version you happened to run.
#
# What is added here is only what is specific to a CAPTURE leg:
#   * refuse to launch on top of an existing set of shots for the same tag,
#     because UE's own filename suffix would interleave two runs' images in one
#     directory and a comparer cannot tell them apart;
#   * refuse a leg whose traverse is too short to reach the first shot boundary,
#     BEFORE spending seven minutes finding that out;
#   * a default LingerSec that exists to let the last PNG finish writing;
#   * and a post-run report that reconciles the log's shot list against the
#     files actually on disk, since a lost async write is silent otherwise.
#
# IT DOES NOT JUDGE THE PICTURES. That is the owner's call, always
# (memory: "The owner judges screenshots, not me"). This prints conditions and
# paths.
#
# Usage:
#   tools\voxel-moving-capture.ps1 -Name stagger_on -ShotEveryM 512 `
#       -Cvars "voxel.Stream.RingStagger 1"
#   tools\voxel-moving-capture.ps1 -Name stagger_off -ShotEveryM 512 `
#       -Cvars "voxel.Stream.RingStagger 0"
#
# Then pair them WITHOUT looking at pixels first:
#   python tools\voxel-pair-moving-shots.py Saved\moveshot-stagger_on.log `
#                                           Saved\moveshot-stagger_off.log

param(
    # The arm tag. Goes into the log name AND into every filename, so two arms
    # can share one screenshot directory and still be pairable.
    #
    # WHITELISTED TO [A-Za-z0-9_] TO MATCH THE ENGINE. The C++ side sanitises
    # this the same way (UVoxelPerfRunSubsystem::Initialize), so "-Name arm-A"
    # produces files called arm_A and a script that did not mirror the rule
    # would go looking for arm-A and report "0 shots" on a perfectly good leg.
    # Mirrored here rather than left to the engine specifically so the mismatch
    # is impossible, and the substitution is announced when it happens.
    [Parameter(Mandatory=$true)][string]$Name,

    # THE SHUTTER STEP, IN METRES ALONG THE FLIGHT PATH -- not seconds. The
    # whole design rests on this: two arms are only comparable if their frames
    # are of the SAME GROUND, and on a deterministic line flight the ground is a
    # function of distance travelled. Trigger on the clock instead and the
    # slower arm -- the one under suspicion -- gets photographed over different
    # hillsides, which is the one confound that would make an armed change look
    # worse for being slower. The argument in full is at the trigger, in
    # UVoxelPerfRunSubsystem::MaybeFireMovingShot.
    #
    # 512 m default: at the standard 20 m/s that is a shot every ~26 s, so a
    # 300 s traverse yields ~12 frames -- enough to see a far-field lag build
    # and recover, few enough to look at all of them.
    [int]$ShotEveryM = 512,

    # Distance of the FIRST shot. Default 0 (shoot from the origin). Raise it to
    # skip the opening stretch, where the pawn is still flying out of the bubble
    # the preflight warmed around the spawn.
    [int]$ShotStartM = 0,

    # Disk guard. 32 x a 2560x1440 PNG is a few hundred MB.
    [int]$ShotMaxCount = 32,

    [int]$RunSec = 300,
    [int]$PreflightSec = 90,

    # LINGER IS NOT OBSERVATION TIME HERE -- IT IS THE PNG WRITE DRAINING.
    #
    # Screenshot writes are ASYNC. FinishRun requests exit the instant the
    # flight clock ends, and quitting on top of a pending write is how a ladder
    # loses its last rung (VoxelSkyLadderFixture.cpp's terminal path waits 5 s
    # for exactly this reason, and the -VoxelScreenshotAfter block waits ~3 s).
    # A capture leg's LAST shot can fire in the final metres, so this defaults
    # to 10 s of pinned-pose linger purely as write margin. It is not free --
    # the inner script's void test expects the leg to run PreflightSec + RunSec
    # + LingerSec -- but a leg that silently drops its furthest shot is the one
    # whose furthest shot you wanted.
    [int]$LingerSec = 10,

    [int]$SpeedMPerSec = 20,
    [int]$HeadingDeg = 0,
    [string]$SpawnAt = '-84480,53760',

    # 2560x1440 BY DEFAULT, unlike the timing harness's 1600x900, because this
    # leg's product is an IMAGE and the owner judges images at the resolution he
    # plays at. The inner script applies the GameUserSettings ini overrides that
    # actually move the render size (-ResX alone is inert) and then VERIFIES the
    # result by reading the engine's own view= line -- so if the engine renders
    # something else, its banner says so rather than echoing this parameter.
    [int]$Width = 2560,
    [int]$Height = 1440,

    [string]$Cvars = 'voxel.Stream.CoverageVerify 1',
    [string[]]$ExtraArgs = @(),

    # Longer than the timing harness's 480 s: preflight + a 300 s traverse +
    # linger + startup already exceeds that, and a capture leg additionally
    # stalls on every shutter.
    [int]$TimeoutSec = 900,

    # The sky pins, passed straight through. See the inner script's header for
    # why a frozen equinox noon is the default and why TimeScale 0 is the one
    # deliberate departure from the engine default. For a capture A/B the sun
    # must be pinned or the two arms are lit differently and every pixel differs.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0,

    # Launch even though shots for this tag already exist. See the check below
    # for why that is normally refused.
    [switch]$AllowExistingShots,

    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$ShotDirs = @(
    (Join-Path $Root 'ue-project\Saved\Screenshots\WindowsEditor'),
    (Join-Path $Root 'ue-project\Saved\Screenshots\Windows')
)

# --- Mirror the engine's tag whitelist, and SAY when it bites -----------------
$SafeName = ($Name.ToCharArray() | ForEach-Object {
    if ($_ -match '[A-Za-z0-9]') { $_ } else { '_' }
}) -join ''
if ($SafeName -ne $Name) {
    Write-Host ("  NOTE: -Name '$Name' contains characters the engine replaces; the files and the log " +
                "will use '$SafeName'. Quote that name, not the one you typed.") -ForegroundColor Yellow
}
if ([string]::IsNullOrWhiteSpace($SafeName)) {
    throw "REFUSING TO START: -Name sanitises to an empty string. Use [A-Za-z0-9_]."
}

$LogName = "moveshot-$SafeName"
$LogPath = Join-Path $Root "Saved\$LogName.log"
$FilePrefix = "VoxelMove_$SafeName"

# --- REFUSE A LEG THAT CANNOT REACH ITS FIRST BOUNDARY ------------------------
#
# Checked HERE, before ~7 minutes of wall clock, rather than discovered from the
# engine's end-of-run "ARMED EVERY Nm AND FIRED 0" line. That line exists and is
# loud (an armed-but-inert image leg must never be silent), but a check that can
# be made in advance should be.
#
# The traverse is a straight line at a constant speed, so its length is exactly
# RunSec x SpeedMPerSec. Preflight does not move the pawn and linger happens
# after the flight, so neither adds distance.
$TraverseM = $RunSec * $SpeedMPerSec
if ($ShotStartM -ge $TraverseM) {
    throw ("REFUSING TO START: -ShotStartM $ShotStartM is at or beyond the end of the traverse " +
           "($RunSec s x $SpeedMPerSec m/s = $TraverseM m). This leg would fly for " +
           "$($PreflightSec + $RunSec + $LingerSec)s and produce NO IMAGES.")
}
$ExpectedShots = [Math]::Floor(($TraverseM - $ShotStartM) / $ShotEveryM) + 1
if ($ExpectedShots -gt $ShotMaxCount) {
    Write-Host ("  NOTE: this traverse crosses $ExpectedShots boundaries but -ShotMaxCount is $ShotMaxCount, " +
                "so the flight past ~$($ShotStartM + ($ShotMaxCount - 1) * $ShotEveryM)m will be UNSHOT. " +
                "Raise the cap or widen -ShotEveryM if the far end is what you meant to look at.") -ForegroundColor Yellow
}

# --- REFUSE TO INTERLEAVE TWO RUNS' IMAGES IN ONE DIRECTORY -------------------
#
# FScreenshotRequest with bAddFilenameSuffix=true appends its own %05i and picks
# the next FREE index, which is a good thing -- it never silently overwrites. The
# consequence is that re-running the same arm leaves d00512_00000.png from the
# old leg beside d00512_00001.png from the new one, in the same folder, looking
# identical to a directory listing and to a glob. A comparer would then be
# pairing one arm's old world against the other arm's new one and nothing about
# the result would look wrong.
#
# So: refuse, and name the files. Move or delete them, or use a different -Name.
$existing = @()
foreach ($d in $ShotDirs) {
    if (Test-Path $d) {
        $existing += @(Get-ChildItem $d -Filter "$FilePrefix-d*.png" -ErrorAction SilentlyContinue)
    }
}
if ($existing.Count -gt 0 -and -not $AllowExistingShots) {
    $sample = ($existing | Select-Object -First 5 | ForEach-Object { $_.Name }) -join ', '
    throw ("REFUSING TO START: $($existing.Count) shot(s) for tag '$SafeName' already exist " +
           "($sample$(if ($existing.Count -gt 5) { ', ...' })). UE appends its own uniqueness suffix rather " +
           "than overwriting, so a re-run would interleave two legs' images under one prefix and a comparer " +
           "could not tell them apart. Move or delete them, or pass a different -Name. " +
           "-AllowExistingShots overrides this if you know what you are doing.")
}

# --- Hand off to the real leg script -----------------------------------------
#
# EVERYTHING BELOW THE SHOT SWITCHES IS ITS JOB, NOT THIS SCRIPT'S: the
# one-editor-and-no-build guard, clearing the .vxlog so the leg is cold, the
# resolution ini overrides and their read-back verification, MCP off, the exit
# watchdog, and the three-witness void test. Delegated by invocation so there is
# exactly one copy of each of those rules in the tree.
$flightLeg = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
if (-not (Test-Path $flightLeg)) {
    throw "REFUSING TO START: cannot find $flightLeg -- this script delegates the guard and the void test to it."
}

$shotArgs = @(
    "-VoxelPerfShotEveryM=$ShotEveryM",
    "-VoxelPerfShotStartM=$ShotStartM",
    "-VoxelPerfShotMaxCount=$ShotMaxCount",
    "-VoxelPerfShotName=$SafeName",
    "-VoxelPerfSpeed=$SpeedMPerSec",
    "-VoxelPerfHeading=$HeadingDeg"
) + $ExtraArgs

Write-Host ""
Write-Host "  MOVING CAPTURE: tag '$SafeName', a shot every ${ShotEveryM}m from ${ShotStartM}m along a " -NoNewline
Write-Host "${TraverseM}m line traverse." -ForegroundColor Cyan
Write-Host "  THIS IS AN IMAGE LEG. Its frame times are NOT admissible as timing -- every shutter stalls a" -ForegroundColor Yellow
Write-Host "  frame, and the stalls land in p95/max. Take timings on a leg with no shots." -ForegroundColor Yellow
Write-Host ""

$started = Get-Date
& $flightLeg `
    -LogName $LogName `
    -Cvars $Cvars `
    -ExtraArgs $shotArgs `
    -RunSec $RunSec `
    -PreflightSec $PreflightSec `
    -LingerSec $LingerSec `
    -Flight 'line' `
    -SpawnAt $SpawnAt `
    -Width $Width -Height $Height `
    -TimeoutSec $TimeoutSec `
    -TimeOfDay $TimeOfDay -Date $Date -TimeScale $TimeScale `
    -Editor $Editor
$legExit = $LASTEXITCODE

# --- RECONCILE THE LOG'S SHOT LIST AGAINST THE FILES ON DISK ------------------
#
# Two independent records of the same event, compared, because they fail
# independently. The log line is written by the game thread at the moment the
# request is made; the PNG is written asynchronously afterwards and can be lost
# to an exit that beats it. "12 shots requested, 11 files" is the signature of a
# LingerSec too short for the write to drain, and it is invisible from either
# record alone.
#
# Run even when the leg was declared VOID, because a void leg's images are still
# the fastest way to see what it did before it died -- but the void verdict is
# repeated at the end so nothing here reads as a pass.
$logShots = @()
if (Test-Path $LogPath) {
    $logShots = @(Select-String -Path $LogPath -Pattern 'VoxelPerfShot n=' -ErrorAction SilentlyContinue)
}
$files = @()
$foundDir = $null
foreach ($d in $ShotDirs) {
    if (Test-Path $d) {
        $new = @(Get-ChildItem $d -Filter "$FilePrefix-d*.png" -ErrorAction SilentlyContinue |
                 Where-Object { $_.LastWriteTime -ge $started })
        if ($new.Count -gt 0) { $files += $new; if (-not $foundDir) { $foundDir = $d } }
    }
}

Write-Host ""
if ($logShots.Count -eq 0) {
    # The engine logs its own loud Error for this case; echoed here because the
    # console is what gets pasted into a report and the log is 40 MB.
    Write-Host "  ${LogName}: NO SHOTS FIRED. This leg produced no images." -ForegroundColor Red
    $inert = Select-String -Path $LogPath -Pattern 'ARMED EVERY .* AND FIRED 0' -ErrorAction SilentlyContinue
    if ($inert) { Write-Host "    $($inert[0].Line.Trim())" -ForegroundColor Red }
    Write-Host "  DO NOT read this leg as 'no visible difference'." -ForegroundColor Red
}
else {
    Write-Host "  ${LogName}: $($logShots.Count) shutter(s) requested, $($files.Count) PNG(s) on disk." -ForegroundColor Green
    if ($files.Count -lt $logShots.Count) {
        Write-Host ("    MISSING $($logShots.Count - $files.Count) FILE(S). Screenshot writes are async and the " +
                    "process exits at the end of the linger window -- raise -LingerSec. The MISSING shots are " +
                    "the LAST ones, i.e. the far end of the traverse.") -ForegroundColor Yellow
    }
    if ($foundDir) { Write-Host "    in $foundDir" }
    foreach ($s in $logShots) {
        # Echo the distance/pose fields only -- the full line carries frame
        # indices that mean nothing without the log around them.
        if ($s.Line -match '(nominalM=\S+ actualM=\S+ residualM=\S+ pos=\([^)]*\) yaw=\S+ pitch=\S+)') {
            Write-Host "    $($Matches[1])"
        }
    }
    $skips = Select-String -Path $LogPath -Pattern 'stepped over \d+ boundary' -ErrorAction SilentlyContinue
    if ($skips) {
        Write-Host ("    $($skips.Count) BOUNDARY SKIP LINE(S) in this leg -- some nominal distances are missing " +
                    "from this arm's shot list and cannot be paired. See the log.") -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "  Pair against the other arm BEFORE looking at pixels:" -ForegroundColor Cyan
    Write-Host "    python tools\voxel-pair-moving-shots.py $LogPath <other-arm.log>"
    Write-Host "  The pixel judgement is the owner's; neither script prints a verdict."
}
Write-Host ""

if ($legExit -ne 0) {
    Write-Host "  ${LogName}: THE LEG ITSELF WAS DECLARED VOID above. Whatever images exist are from an " -ForegroundColor Yellow -NoNewline
    Write-Host "incomplete flight." -ForegroundColor Yellow
    exit 1
}
if ($logShots.Count -eq 0) { exit 1 }
exit 0

# The height pyramid's gates, in the order they must be run.
#
# ============================================================================
# WHY THE ORDER IS THE POINT
# ============================================================================
#
# This project shipped a "-7.6% win" that was really the marcher deleting a
# mountain, and the timing INVERTED to +3.1% once the image was made honest. So
# the cheap falsifiers run first and the expensive confirmation runs last, and
# every gate here can KILL the arm on its own:
#
#   0. CONTROL vs BASELINE. Outranks even the arm. The arm's traversal is
#      wrapped in an unconditional loop so that the control and the armed path
#      share ONE inlined instantiation of VoxelMarchTraverseWithCover -- two
#      would tax the control as well and re-base it. This gate proves that
#      restructure did not move the control. If it moved by more than the ~2%
#      horizon noise floor, STOP: every per-direction number after it would be
#      measured against a moved reference, which silently invalidates
#      comparisons instead of failing loudly.
#
#   1. THE RED ARM CAN FIRE. voxel.March.HeightPyramid.BiasM 50 lowers every
#      height bound by 50 m -- i.e. deliberately claims air where there is
#      ground. heightViolations MUST go non-zero. A confirmation that cannot
#      come out the other way is not one, and this is the leg that earns the
#      right to believe gate 2.
#
#   2. THE RED ARM PASSES. Same leg, BiasM 0. heightViolations MUST be 0.
#
#   3. ENGAGEMENT. heightConsulted > 0 (or the arm is ARMED AND INERT, this
#      project's signature failure), heightAdvanced > 0, and heightReentries > 0
#      -- without reentries only the tStart half ran and the mid-ray skips the
#      HORIZON depends on were never exercised, which is a PARTIAL result.
#
#   4. THE IMAGE. Pinned-pose A/B against a control-vs-control noise floor
#      established FIRST. Pair captures by EXPLICIT FILENAME, never `ls -t`.
#
#   5. TIMING, LAST, and only on an arm whose image is clean. Not run here --
#      tools/voxel-march-direction-sweep.ps1 owns it, because HoleStats is a
#      PERMUTATION OF THE TIMED KERNEL (4.448 ms at 0 against 4.548/4.463 at 1)
#      and so an engagement leg's milliseconds are not quotable.
#
# ============================================================================
# THE FIRST LEG AFTER A SHADER EDIT IS VOID, AND THAT IS NOT A FAILURE
# ============================================================================
#
# Editing VoxelMarch.usf invalidates the global shader map, and the editor
# recompiles it AT BOOT before the game starts. Measured on this feature's own
# first leg: 7m26s from launch to "Starting Game", against 18 SECONDS on the
# very next leg with a warm cache -- which blew the 480 s leg timeout and was
# reported as KILLED/VOID. It is a cache fill, not a hang and not a crash.
#
# So this script fires a short WARM-UP leg first whenever -Warm is passed, and
# the rule for reading any sweep is: a VOID first leg immediately after a shader
# change is expected; re-run that pose rather than treating it as a result.

param(
    [string]$Prefix = 'HPG',
    [string]$SpawnAt = '-61440,-61440',
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$PreflightSec = 75,
    [int]$RunSec = 45,
    [int]$LingerSec = 10,
    [int]$TimeoutSec = 900,
    # The pose the gates are judged at. 0,0 is the HORIZON -- the pose the frame
    # is actually spent in (4.45 ms of it) and the one a Z slab provably cannot
    # help (0.00% of 3.3e9 decisions skipped at pitch -10). A gate passed only
    # at the sky is a partial result.
    [string]$Pitch = '0',
    [string]$Yaw = '0',
    [switch]$Warm,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$RepoRoot  = (Resolve-Path "$PSScriptRoot\..").Path
$LegScript = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
if (-not (Test-Path $LegScript)) { throw "REFUSING TO START: $LegScript is missing." }

$BaseCvars = 'voxel.Stream.CoverageVerify 1, voxel.March.HoleStats 1, voxel.HeightPyramid.Build 1'

function Run-Gate([string]$Name, [string]$Cvars, [int]$Run, [int]$Pre) {
    $log = "$Prefix-$Name"
    $legArgs = @{
        LogName        = $log
        Cvars          = $Cvars
        Flight         = 'static'
        SpawnAt        = $SpawnAt
        Width          = $Width
        Height         = $Height
        PreflightSec   = $Pre
        RunSec         = $Run
        LingerSec      = $LingerSec
        LogIntervalSec = 5
        TimeoutSec     = $TimeoutSec
        TimeOfDay      = '12:00'
        Date           = '03-20'
        TimeScale      = 0
        # voxel.March.Stats is what prints the heightPyramid block. It runs at
        # PreflightSec + 30 so it lands inside the run window and never in the
        # linger, which is the same failure the sweep's capture check exists for.
        ExtraArgs      = @("-VoxelPerfPitch=$Pitch", "-VoxelPerfYaw=$Yaw",
                           '-VoxelExecCmds=voxel.March.Stats',
                           "-VoxelExecAfter=$($Pre + 30)")
    }
    Write-Host ""
    Write-Host "=== GATE $Name ===" -ForegroundColor Cyan
    Write-Host "    cvars: $Cvars" -ForegroundColor DarkGray
    if ($DryRun) { Write-Host "    (dry run)"; return }
    & $LegScript @legArgs
    Write-Host "    log: Saved\$log.log"
}

if ($Warm) {
    # SHORT AND DISCARDED. Its only job is to pay the global-shader recompile so
    # the first REAL leg is not killed by it. Nothing it measures is read.
    Run-Gate 'warm' $BaseCvars 5 15
}

# GATE 1 -- must produce heightViolations > 0.
Run-Gate 'red-bias50' "$BaseCvars, voxel.March.HeightPyramid 1, voxel.March.HeightPyramid.Verify 1, voxel.March.HeightPyramid.BiasM 50" $RunSec $PreflightSec

# GATE 2/3 -- must produce heightViolations == 0, and non-zero engagement.
Run-Gate 'red-clean' "$BaseCvars, voxel.March.HeightPyramid 1, voxel.March.HeightPyramid.Verify 1, voxel.March.HeightPyramid.BiasM 0" $RunSec $PreflightSec

Write-Host ""
Write-Host "Read both with:" -ForegroundColor Yellow
Write-Host "  grep -A14 'heightPyramid:' Saved/$Prefix-red-bias50.log"
Write-Host "  grep -A14 'heightPyramid:' Saved/$Prefix-red-clean.log"
Write-Host "GATE 1 PASSES ONLY IF bias50 SHOWS heightViolations NON-ZERO." -ForegroundColor Yellow
Write-Host "GATE 2 PASSES ONLY IF red-clean SHOWS 0 violations AND heightConsulted > 0." -ForegroundColor Yellow

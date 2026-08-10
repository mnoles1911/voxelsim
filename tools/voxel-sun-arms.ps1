# Frozen sun vs moving sun, as an A/B: the ONE measurement the day/night clock
# owes, run so that it cannot be misread.
#
# ============================================================================
# WHY THIS SCRIPT EXISTS
# ============================================================================
#
# Every perf number this project has on record was taken with a sun that had
# not moved since spawn, because until the clock landed there was no other kind
# of sun. That includes docs/backlog.md §0's shadowGather=0 / ~1.03
# gathers-per-frame figure -- the number that gets quoted when anyone asks what
# shadows cost here.
#
# A static movable directional light lets the renderer cache its whole-scene
# shadow setup. A sun that rotates every frame busts that cache and re-renders
# the resident geometry -- a 62,657-run pool -- into every cascade it touches.
# So §0 does not transfer to a day/night run, and nobody knows by how much.
# That is the largest unmeasured GPU risk in the feature, and it is the reason
# VoxelSkySubsystem.cpp's shadow-cadence cap (voxel.Sky.ShadowUpdateHz) is
# currently justified by "should be" rather than by a number -- see its own
# comment, which says so out loud.
#
# ============================================================================
# WHY THE ARMS ALTERNATE
# ============================================================================
#
# Copied wholesale from tools/voxel-t41-lead-sweep.ps1, including the reason:
# legs drift. Thermals, background work and whatever Windows decided to do at
# 04:00 all move the number, and a run that does arm A twice then arm B twice
# attributes that drift to the arm. Alternating A,B,A,B means a slow patch of
# wall clock hits both arms roughly equally.
#
# Grouping is especially dangerous for THIS pair, because the plausible result
# ("the moving arm is slower") is also exactly what a warming GPU produces for
# free if the moving arm happens to run second. Do not add a -Grouped switch.
#
# ============================================================================
# WHAT IS HELD FIXED, AND WHY THAT LIST IS NOT NEGOTIABLE
# ============================================================================
#
# The arms differ in -TimeScale and in NOTHING ELSE. Spawn, seed, route,
# heading, duration, resolution, cvars, date and START HOUR are all pinned and
# shared. The start hour matters as much as the date: a moving arm that begins
# at 12:00 sweeps a high sun through a shallow arc, and one that begins at
# 05:30 walks through a sunrise where the cascade geometry, the exposure and
# the light's own intensity are all changing at once. Those are two different
# experiments and only one of them is "what does moving the sun cost".
#
# THE SPAWN IS MANDATORY AND IS NOT A STYLE CHOICE. -84480,53760 is inside the
# generated tile set. Off it the world is a flat fallback plane, which renders
# fast, streams nothing, casts almost no shadows, and would make both arms look
# equally cheap -- i.e. it produces a clean, confident, worthless null result.
# Check the log for real tile coverage before believing anything here.
#
# Each leg goes through tools/voxel-run-flight-leg.ps1, so the one-editor guard
# and the edit-log clearing are inherited rather than reimplemented (two
# concurrent legs once corrupted a measurement; a stale .vxlog once made a leg
# not-cold). A VOID leg is reported and the pass CONTINUES: a missing arm is
# visible in the summary, a silently retried one is not.
#
# Usage:
#   tools\voxel-sun-arms.ps1 -Passes 2
#   tools\voxel-sun-arms.ps1 -Passes 3 -TimeOfDay 05:30 -Prefix w7-sunrise

param(
    # Two passes = two legs per arm, which is the minimum ground rule 1 accepts.
    # Three is better and costs 4.5 minutes each; this is an overnight-able run.
    [int]$Passes = 2,
    [string]$Prefix = 'w7-sun',

    # --- shared between the arms; changing any of these changes BOTH ---------
    # The mandatory real-terrain spawn. Single-quoted so PowerShell does not
    # treat the comma as an array separator.
    [string]$SpawnAt = '-84480,53760',
    # Written out rather than left implicit even though it IS
    # UVoxelWorldSubsystem::DefaultSeed (VoxelWorldSubsystem.h:66): an implicit
    # default is one that a later build can change underneath a half-finished
    # comparison, and this pair may well be run across a rebuild.
    [uint64]$Seed = 20260719,
    # 'line' traverses virgin terrain continuously. Chosen over 'surface'
    # because the surface circle re-treads ground it has already loaded, so its
    # shadow-casting geometry is largely static after one lap -- which is the
    # very thing the moving arm is supposed to be stressing.
    [ValidateSet('line','surface','static')][string]$Flight = 'line',
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [int]$RunSec = 120,
    [int]$PreflightSec = 90,
    [int]$LingerSec = 60,
    [int]$LogIntervalSec = 2,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [string]$Cvars = 'voxel.Stream.CoverageVerify 1',

    # --- THE ONLY THING THAT DIFFERS ----------------------------------------
    # 1.0 = the shipped default: one game day per voxel.Sky.DayLengthSeconds
    # (2400 s since 2026-08-09, was 1200), so a 270 s leg covers ~40 degrees of
    # day -- half what it covered before the day length doubled. Raise it to force a
    # bigger sweep, but say what you raised it to -- at high scales the arms
    # stop differing only in "does the sun move" and start differing in "how
    # much sky did each one traverse".
    [double]$MovingTimeScale = 1.0
)

$ErrorActionPreference = 'Stop'
$runner  = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$summary = Join-Path $PSScriptRoot 'voxel-leg-summary.ps1'

# THE GUARD, UP FRONT. voxel-run-flight-leg.ps1 enforces this per leg and would
# catch it anyway -- but it would catch it four and a half minutes into the
# first leg, after the edit log had been cleared. Failing here costs nothing and
# leaves the box exactly as it was found.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw ("REFUSING TO START: $($running.Count) editor process(es) already running -- $detail. " +
           "Two legs sharing the box produce contended numbers that look exactly like a slow " +
           "arm, and nothing in the log says so. See tools/voxel-run-flight-leg.ps1's header.")
}

if ($MovingTimeScale -eq 0) {
    throw ("-MovingTimeScale 0 makes the moving arm a second frozen arm. Both arms would then " +
           "measure the same thing and any difference between them would be pure run-to-run " +
           "drift -- which is precisely the reading this whole script is built to prevent.")
}

$expectedWindows = [int](($PreflightSec + $RunSec + $LingerSec) / $LogIntervalSec) - 4
Write-Host ""
Write-Host ("Sun A/B: $Passes pass(es), alternating frozen/moving. Shared: spawn $SpawnAt, seed $Seed, " +
            "flight $Flight, ${Width}x${Height}, start $TimeOfDay on $Date, $PreflightSec/$RunSec/$LingerSec s.") -ForegroundColor Cyan
Write-Host ("Differs: -TimeScale 0 (frozen) vs $MovingTimeScale (moving). Nothing else.") -ForegroundColor Cyan
Write-Host ""

# ORDER IS FROZEN FIRST WITHIN EACH PASS, and the pairs interleave: F,M,F,M.
# Every arm therefore sits in both an "early" and a "late" slot as passes
# accumulate, which is what makes the alternation worth anything.
$arms = @(
    [pscustomobject]@{ Tag = 'frozen'; TimeScale = 0.0 }
    [pscustomobject]@{ Tag = 'moving'; TimeScale = $MovingTimeScale }
)

$legs = @()
for ($pass = 1; $pass -le $Passes; $pass++) {
    foreach ($arm in $arms) {
        $name = "$Prefix-$($arm.Tag)-$pass"
        Write-Host ("[$([datetime]::Now.ToString('HH:mm:ss'))] leg $name " +
                    "(timeScale $($arm.TimeScale), pass $pass)") -ForegroundColor Cyan
        $ok = $false
        try {
            # Everything except -TimeScale is passed identically to both arms,
            # from the same variables, in one call site -- so the two arms
            # cannot drift apart through an edit that updates only one of them.
            $ok = & $runner `
                -LogName $name `
                -Cvars $Cvars `
                -Flight $Flight `
                -SpawnAt $SpawnAt `
                -RunSec $RunSec `
                -PreflightSec $PreflightSec `
                -LingerSec $LingerSec `
                -LogIntervalSec $LogIntervalSec `
                -Width $Width `
                -Height $Height `
                -TimeOfDay $TimeOfDay `
                -Date $Date `
                -TimeScale $arm.TimeScale `
                -ExtraArgs @("-VoxelSeed=$Seed")
        } catch {
            # Almost always the one-editor guard (a previous leg that has not
            # exited yet). Report it and keep going: the summary will show the
            # arm missing, which is the honest outcome.
            Write-Host "  $name FAILED TO START: $($_.Exception.Message)" -ForegroundColor Red
        }
        $legs += [pscustomobject]@{ Name = $name; Arm = $arm.Tag; TimeScale = $arm.TimeScale; Pass = $pass; Ok = [bool]$ok }
    }
}

Write-Host ''
Write-Host 'Sun A/B complete. Legs:' -ForegroundColor Green
$legs | Format-Table -AutoSize | Out-String -Width 160 | Write-Host

$good = @($legs | Where-Object { $_.Ok } | ForEach-Object { $_.Name })
$perArm = $legs | Where-Object { $_.Ok } | Group-Object Arm
if ($perArm.Count -lt 2 -or ($perArm | Where-Object { $_.Count -lt 2 }).Count -gt 0) {
    Write-Host ("  WARNING: fewer than two completed legs on at least one arm. Ground rule 1 -- never " +
                "conclude from a single run -- applies here whether or not the numbers look clean. " +
                "Re-run the missing legs before writing anything down.") -ForegroundColor Yellow
}

if ($good.Count -gt 0) {
    # NO -AllowPartial, EVER, from this script. voxel-leg-summary.ps1 refuses to
    # summarise a leg with too few windows because a partial flight reads like a
    # slow configuration -- and "the moving arm is slower" is exactly the
    # conclusion this comparison is fishing for, so a partial moving leg would
    # confirm it for free. A leg that is excluded here is a leg to re-run.
    Write-Host ("Summarising completed legs (expecting ~$expectedWindows windows each; " +
                "partial/void legs are excluded by the summariser):") -ForegroundColor Green
    & $summary -LogName $good -MinWindows $expectedWindows

    Write-Host ''
    Write-Host 'READING THIS RESULT:' -ForegroundColor Green
    Write-Host '  * Check the Sun column FIRST. Every frozen leg must read FROZEN at the same tod, and'
    Write-Host '    every moving leg must read MOVED with a similar drift. A "moving" leg that reads'
    Write-Host '    FROZEN did not move its sun -- voxel.Sky.Enabled may be 0, or ShadowUpdateHz may'
    Write-Host '    have capped the light updates away -- and its frame times are a frozen measurement'
    Write-Host '    wearing the moving arm''s label.'
    Write-Host '  * The decision numbers are p95FrameMs and the shadow-gather count, not chunksPerSec:'
    Write-Host '    a moving sun costs RENDER-thread time, and this draw path is render-thread bound'
    Write-Host '    with the game thread idle ~75% of the frame. Streaming throughput can be identical'
    Write-Host '    across the arms while the frame is 20% dearer.'
    Write-Host '  * Read p95 out of Saved/PerfRuns/perf_*.json, which now records timeOfDay, dateMMDD,'
    Write-Host '    timeScale, sunAltitudeDeg and sunAzimuthDeg alongside it. Quote those fields in'
    Write-Host '    whatever you write; a frame-time number without its sun is not a result.'
}

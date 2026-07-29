# Summarise finished flight legs, and REFUSE to summarise unfinished ones.
#
# WHY THIS EXISTS. tools/voxel-run-flight-leg.ps1 stops two legs sharing the box.
# This stops the other half of the same mistake: reading a log while it is still
# being written. A partial leg is not obviously partial -- it has a plausible
# chunks/s, a plausible hole count, and a plausible everything else, because the
# flight profile front-loads the cheap phase. Three times in one session a
# mid-flight log was compared against finished ones:
#
#   * a "confirmation" leg read 508.6 chunks/s against a finished 797.0
#   * a parking leg read 694.4 with 354 holes and a 37% hit rate against a
#     finished 855.3 / 2 holes / 84% -- and nearly became "the revert did not
#     restore parking"
#
# Both look exactly like a real regression. Neither was.
#
# A flight leg emits one "Voxel apply stages" line per -VoxelPerfLogInterval, so
# the window count is the completeness test. -MinWindows defaults to 130, which
# is the 133-134 a 90/120/60 leg at 2s intervals produces, with slack for the
# last window landing after the final log flush.
#
# Ground rule 1 says never conclude from a single run. Its siblings, both learned
# the hard way today: never conclude from a run that was sharing the box, and
# never conclude from a run that had not finished.

param(
    [Parameter(Mandatory=$true)][string[]]$LogName,
    [int]$MinWindows = 130,
    [switch]$AllowPartial
)

$ErrorActionPreference = 'Stop'
$SavedDir = Join-Path (Resolve-Path "$PSScriptRoot\..").Path 'Saved'

$rows = @()
foreach ($name in $LogName) {
    $path = Join-Path $SavedDir "$name.log"
    if (-not (Test-Path $path)) {
        Write-Host "  ${name}: NO LOG" -ForegroundColor Red
        continue
    }

    $lines = Get-Content $path
    $windows = @($lines | Select-String -SimpleMatch 'Voxel apply stages').Count
    if ($windows -lt $MinWindows -and -not $AllowPartial) {
        Write-Host ("  {0}: INCOMPLETE ({1}/{2} windows) -- NOT SUMMARISED. It is still running, or it died. " -f $name, $windows, $MinWindows) -ForegroundColor Yellow
        Write-Host "     A partial flight leg reads like a slow configuration. Wait for it, or pass -AllowPartial and say so in whatever you write." -ForegroundColor Yellow
        continue
    }

    # chunks/s: mean of the per-window figure, which is what the 5s line reports.
    $cps = @($lines | Select-String -Pattern 'chunksPerSec=([0-9.]+)' -AllMatches |
             ForEach-Object { $_.Matches } | ForEach-Object { [double]$_.Groups[1].Value })
    $meanCps = if ($cps.Count) { [math]::Round(($cps | Measure-Object -Average).Average, 1) } else { $null }

    $holeSeries = @($lines | Select-String -Pattern 'holes=([0-9]+)' -AllMatches |
                    ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value })
    $holes = $holeSeries | Select-Object -Last 1

    # FLIGHT-PHASE holes, which is a different question from the final converged
    # count and the more important one for T4-1.
    #
    # holes(final) asks "did the world end up complete" -- it is 0 on every
    # healthy arm, including arms with severe pop-in, because the linger phase
    # fills everything in. The transient count DURING the flight is what a player
    # would actually see as terrain arriving late, and it is where speculation
    # shows up: 840 median with it off against 64 with lead 4.0s on the first
    # pair, at identical chunks/s. A summary reporting only holes(final) would
    # have called that pair a tie.
    #
    # Windows 46-105 of a 90/120/60 leg at 2s intervals: the 90s preflight is the
    # first 45, the 120s flight the next 60. Phase boundaries are approximate by
    # one window, which does not matter for a median over 60 samples.
    $flight = @($holeSeries | Select-Object -Skip 45 -First 60 | Sort-Object)
    $flightHoles = if ($flight.Count -ge 30) {
        "med=$($flight[[int]($flight.Count/2)]) p90=$($flight[[int]($flight.Count*0.9)]) max=$($flight[-1])"
    } else { 'n/a' }

    $poolLine = @($lines | Select-String -SimpleMatch 'Voxel GPU pool:') | Select-Object -Last 1
    $allocFail = if ($poolLine -and $poolLine.Line -match 'allocFail=([0-9]+)') { $Matches[1] } else { 'n/a' }

    $parkLine = @($lines | Select-String -SimpleMatch 'Voxel park (5s window)') | Select-Object -Last 1
    $park = if ($parkLine -and $parkLine.Line -match 'cumulative parked=([0-9]+) adopted=([0-9]+) \(hit ([0-9]+)%\)') {
        "parked=$($Matches[1]) adopted=$($Matches[2]) hit=$($Matches[3])%"
    } else { 'parking off' }

    # Pool pressure. T4-1 parks geometry nobody has asked for yet, so this is the
    # column that says whether the feature is affordable: S2-3's abort condition
    # is capacityPct crossing ~90, because UpdateChunk's realloc path DELETES
    # resident terrain on a full pool.
    $poolPct = if ($poolLine -and $poolLine.Line -match 'capacityPct=([0-9.]+)') { $Matches[1] } else { 'n/a' }

    # T4-1. Reported against DISPATCHED, not against parked.
    #
    # The in-log "hit %" is adopted/parked, which answers "of the chunks we kept,
    # how many got used" -- 96% on the first T4-1 leg. The cost question is
    # adopted/DISPATCHED, because a dispatch that never parked still spent GPU:
    # the same leg was 46% on that denominator. Four denominator mistakes in this
    # programme have each been arithmetically true and directionally wrong
    # (docs/lessons-2026-07-27-s0-s1.md, appendix), so both are printed.
    $specLine = @($lines | Select-String -SimpleMatch 'Voxel speculation (5s window)') | Select-Object -Last 1
    $spec = 'spec off'
    if ($specLine -and $specLine.Line -match 'cumulative dispatched=([0-9]+) adopted=([0-9]+)') {
        $d = [double]$Matches[1]; $a = [double]$Matches[2]
        $pct = if ($d -gt 0) { [math]::Round(100 * $a / $d) } else { 0 }
        $spec = "disp=$($Matches[1]) adopted=$($Matches[2]) (${pct}% of dispatched)"
    }

    # THE SUN, AS A FIRST->LAST SWEEP RATHER THAN A SINGLE POSE.
    #
    # UVoxelPerfRunSubsystem emits one "Voxel sky (Ns window)" line per
    # -VoxelPerfLogInterval, on the same cadence as the chunksPerSec= line
    # above. The FIRST and LAST of them is what belongs in a summary, because
    # the question this column answers is not "what was the sun" but "DID THE
    # SUN MOVE" -- and a single sample cannot answer that. A leg that swept 40
    # degrees of altitude and a leg that never moved can end at the same angle.
    #
    # Why it is here at all: a movable directional light that has not rotated
    # since spawn lets the renderer keep its cached whole-scene shadow setup, so
    # a moving sun is a real and unmeasured cost on this draw path. Every perf
    # baseline in docs/status.md and docs/backlog.md §0 was taken before the
    # day/night clock existed, i.e. frozen. Comparing a moving-sun leg against
    # any of them, or against a frozen leg at a different hour, is the same
    # class of mistake as comparing two static legs at different camera poses --
    # which is exactly why VoxelPerfRunSubsystem.cpp records staticYawDeg. This
    # column is that check, in the place people actually read.
    #
    # 'no sky log' means the leg predates the instrument, NOT that the sun was
    # frozen. Those are different states and this must never conflate them.
    $skyLines = @($lines | Select-String -SimpleMatch 'Voxel sky (')
    $sun = 'no sky log'
    if ($skyLines.Count -gt 0) {
        $first = $skyLines[0].Line
        $last  = $skyLines[-1].Line
        $todA  = if ($first -match 'tod=([0-9]{2}:[0-9]{2})')    { $Matches[1] } else { '??:??' }
        $todB  = if ($last  -match 'tod=([0-9]{2}:[0-9]{2})')    { $Matches[1] } else { '??:??' }
        $altA  = if ($first -match 'sunAlt=(-?[0-9.]+)')         { [double]$Matches[1] } else { $null }
        $altB  = if ($last  -match 'sunAlt=(-?[0-9.]+)')         { [double]$Matches[1] } else { $null }
        $ts    = if ($last  -match 'timeScale=(-?[0-9.]+)')      { [double]$Matches[1] } else { $null }
        $drift = if ($null -ne $altA -and $null -ne $altB) { [math]::Round([math]::Abs($altB - $altA), 2) } else { 'n/a' }
        # Flagged on the MEASURED drift, not on timeScale alone: a nonzero scale
        # whose light updates never landed, and a zero scale, are both a still
        # sun, and the frame times only know about the sun that moved.
        if ($drift -is [double] -and $drift -lt 0.01) {
            # Frozen: one pose says everything, and the pose still has to be
            # printed -- two FROZEN legs at different hours are no more
            # comparable than a frozen and a moving one.
            $sun = "FROZEN tod=$todA alt=$altA ts=$ts"
        } else {
            $sun = "MOVED tod=$todA->$todB alt=$altA->$altB d=$drift ts=$ts"
        }
    }

    $rows += [pscustomobject]@{
        Leg = $name; Windows = $windows; ChunksPerSec = $meanCps
        Holes = $holes; FlightHoles = $flightHoles; AllocFail = $allocFail
        PoolPct = $poolPct; Park = $park; Spec = $spec; Sun = $sun
    }
}

# Width raised from 200 with the Sun column: Out-String TRUNCATES at the width
# it is given, and the column it would have dropped is the rightmost one --
# i.e. the sun, i.e. precisely the qualifier that decides whether the row to its
# left may be compared with anything.
if ($rows.Count) { $rows | Format-Table -AutoSize | Out-String -Width 260 | Write-Host }

# ---------------------------------------------------------------------------
# A NOTE ON READING THESE LOGS, learned expensively on 2026-07-27/28.
#
# This script deliberately reports LEG-WIDE aggregates, never a sampled window.
# Every phase-sampling shortcut tried during that session produced a confident
# wrong reading:
#
#   * `sed -n '30p;50p'` on the GPU fork census landed in preflight/linger and
#     read "L0=0 ... L5=0" -- which looks exactly like the GPU mesher being off.
#     It had dispatched 18,308 jobs on that leg.
#   * `tail -1` on the tick budget compared one leg's idle linger against
#     another leg's mid-flight churn and made them look wildly different.
#   * Reading any log before it reached ~130 windows read a partial flight as a
#     slow configuration, three separate times.
#
# The flight profile has three phases with completely different characteristics
# (90 s preflight fill, 120 s flight, 60 s linger). Any single window is a
# statement about a phase, not about a configuration. If a per-phase number is
# genuinely needed, say which phase it came from in whatever you write.

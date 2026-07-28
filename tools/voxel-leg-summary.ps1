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

    $rows += [pscustomobject]@{
        Leg = $name; Windows = $windows; ChunksPerSec = $meanCps
        Holes = $holes; FlightHoles = $flightHoles; AllocFail = $allocFail
        PoolPct = $poolPct; Park = $park; Spec = $spec
    }
}

if ($rows.Count) { $rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host }

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

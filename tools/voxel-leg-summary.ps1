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
# the window count is the completeness test. The bar is DERIVED FROM EACH LEG'S
# OWN COMMAND LINE (preflight+run+linger over the interval, less 5% slack for the
# last window landing after the final log flush) -- see the block at the check
# itself for why a constant was wrong. -MinWindows survives only as the fallback
# for a log whose command line cannot be read, and the fallback says so out loud.
#
# Ground rule 1 says never conclude from a single run. Its siblings, both learned
# the hard way today: never conclude from a run that was sharing the box, and
# never conclude from a run that had not finished.

param(
    [Parameter(Mandatory=$true)][string[]]$LogName,
    [int]$MinWindows = 130,
    [switch]$AllowPartial,
    # See the integrity scan below. Suppresses the refusal, never the reason.
    [switch]$AllowInvalid
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

    # -----------------------------------------------------------------------
    # INTEGRITY SCAN: guards that INVALIDATE a leg, checked before any metric.
    #
    # WHY THIS EXISTS, and the first version of it was itself the lesson.
    # On 2026-08-21 the march chunk index printed, in EVERY leg, that its
    # observed chunk span was "at or past HALF the grid" -- a condition whose
    # own text says chunks silently shadow each other as holes. I swept 780
    # logs, found it in 100% of legs that attach the index, and concluded the
    # whole archive was invalid.
    #
    # IT WAS NOT. That span is never reset and never shrinks on eviction, so it
    # is the union of every coordinate ever offered -- a TRAVEL LOG, not a
    # residency claim. Aliasing requires two chunks resident AT THE SAME TIME,
    # and a union over time cannot distinguish that. A monotonically growing
    # statistic against a fixed threshold MUST eventually fire in every leg
    # that runs long enough: the 100% rate was structurally guaranteed and
    # therefore carried no information at all.
    #
    # So this scan keys on the COUNTER THAT OBSERVES COLLISIONS DIRECTLY -- an
    # add landing on a cell another chunk already holds -- and not on the
    # proxy. The general rule, which cost a retraction to learn: never gate on
    # a statistic that cannot come out the other way.
    #
    # The failure it guards against is real and is the dangerous direction: a
    # shadowed chunk reads as a hole, a hole is empty space, and empty space is
    # exactly what a hierarchy skips fastest. Invented holes INFLATE the skip
    # ratio, so this defect makes the headline number look better, not worse.
    #
    # Failing the LEG rather than printing a note is deliberate. A caveat in a
    # column gets dropped when the number is copied into a doc; a missing row
    # cannot be. -AllowInvalid exists for deliberately measuring a known-broken
    # configuration, and makes you say so.
    $invalidators = @(
        @{ Pattern = 'CHUNKS WERE SHADOWED'
           Why     = 'march chunk index ALIASED -- shadowed chunks become invented holes, which INFLATE the skip ratio' }
        # The reference walk hitting the step budget. Same reason as above: it
        # makes the headline number BETTER, not worse. A truncated reference
        # stops early, the hierarchy runs on, and every such ray is scored as
        # "the hierarchy found content the reference missed" -- which inverts
        # the comparison gate and flatters the ratio in one move. It went
        # undetected because the two walks report exhaustion through DIFFERENT
        # FIELDS and the counter read only one, printing 0.0% while the
        # reference was clipped on nearly every ray.
        @{ Pattern = 'THE REFERENCE IS THE ONE BEING CLIPPED'
           Why     = 'the FLAT REFERENCE hit the step budget -- ratio is a property of voxel.March.StepBudget, not of the walk' }
        # P7-c. A DIFFERENT SHAPE FROM THE TWO ABOVE, and worth saying why it
        # belongs in the same list. Those two are legs whose numbers are WRONG.
        # This is a leg whose numbers are RIGHT and describe something other
        # than what a reader will assume they describe.
        #
        # voxel.GI.SourceBricks 1 retires the CPU quad ingest, so the light
        # field is fed by NOTHING. That is the arm's purpose: it bounds the
        # streaming refund (cold fill, chunks/s, quadsRetainedForGI -> 0) before
        # the brick-sourced march exists. Its streaming numbers are real and are
        # the point. Its FRAME TIMES and its CAPTURES are of a run with no GI
        # content -- indistinguishable from GI being off, because for lighting
        # purposes it is off.
        #
        # The danger is precise: this arm makes GI look FREE. Anyone who reads a
        # p95 or a screenshot from it as "GI with brick sourcing" has read the
        # cost of a feature that was not running. That is the same mistake the
        # retired +14.9%/x3.2 embodied from the other direction -- a number
        # taken on a configuration nobody ships.
        #
        # KEYED ON THE OBSERVED STRING the subsystem prints every window, not on
        # an inferred condition and not on the cvar. A cvar can be set and fail
        # to engage; this line is emitted by the code path that actually
        # declined the quads, and it carries its own arm counters beside it.
        @{ Pattern = 'QUAD INGEST RETIRED'
           Why     = 'voxel.GI.SourceBricks 1 -- the light field was FED BY NOTHING. Streaming numbers are valid; frame times and captures describe a run with NO GI CONTENT and must not be read as a lighting result' }
    )
    $violations = @()
    foreach ($inv in $invalidators) {
        $hit = @($lines | Select-String -SimpleMatch $inv.Pattern) | Select-Object -First 1
        if ($hit) { $violations += $inv.Why }
    }
    if ($violations.Count -and -not $AllowInvalid) {
        Write-Host ("  {0}: INVALID -- NOT SUMMARISED." -f $name) -ForegroundColor Red
        foreach ($v in $violations) { Write-Host "     $v" -ForegroundColor Red }
        Write-Host "     The leg ran; its numbers describe a broken configuration. Fix the cause, or pass" -ForegroundColor Red
        Write-Host "     -AllowInvalid and state the violation in whatever you write." -ForegroundColor Red
        continue
    }
    $windows = @($lines | Select-String -SimpleMatch 'Voxel apply stages').Count

    # COMPLETENESS. -MinWindows 130 is the count a 2-SECOND interval produces,
    # and it was the only test here until 2026-08-21. But voxel-run-gpu-arm.ps1
    # HARDWIRES -VoxelPerfLogInterval=5 (:206), so every gpu-arm leg ever run
    # reads "INCOMPLETE (111/130)" while having exited cleanly. A refusal that
    # fires on every leg of a whole family teaches you to pass -AllowPartial by
    # reflex, and then a genuinely dead leg gets waved through with it. So the
    # expectation is DERIVED FROM THE LEG'S OWN COMMAND LINE instead of assumed.
    #
    # The derivation must still be able to FAIL, or it is decorative: a leg that
    # produced far fewer windows than its own duration calls for is refused just
    # as before. And if the command line cannot be parsed we fall back to the
    # constant AND SAY SO, rather than passing quietly.
    $expected = $null; $basis = $null
    $cmdLine = ($lines | Select-String -Pattern '-VoxelPerfRun=' | Select-Object -First 1)
    if ($cmdLine) {
        $t = $cmdLine.Line
        $iv = [regex]::Match($t, '-VoxelPerfLogInterval=(\d+)')
        $rn = [regex]::Match($t, '-VoxelPerfRun=(\d+)')
        $pf = [regex]::Match($t, '-VoxelPerfPreflightSec=(\d+)')
        $lg = [regex]::Match($t, '-VoxelPerfLingerSec=(\d+)')
        if ($iv.Success -and $rn.Success) {
            $ivS = [int]$iv.Groups[1].Value
            $secs = [int]$rn.Groups[1].Value
            if ($pf.Success) { $secs += [int]$pf.Groups[1].Value }
            if ($lg.Success) { $secs += [int]$lg.Groups[1].Value }
            if ($ivS -gt 0) {
                # 5% slack: the last window can land after the final log flush,
                # and the preflight phase does not always emit from second zero.
                $expected = [int][math]::Floor(($secs / $ivS) * 0.95)
                $basis = "derived from this leg's own command line: ${secs}s at ${ivS}s intervals"
            }
        }
    }
    if ($null -eq $expected) {
        $expected = $MinWindows
        $basis = "FALLBACK to the -MinWindows constant -- the command line could not be parsed out of this log, so the bar is an assumption about a 2s interval, not a fact about this leg"
    }
    # A clean shutdown is DIRECT evidence the leg finished; window count is only
    # a proxy for it. Both are reported, and neither alone excuses the other.
    $exited = [bool]($lines | Select-String -SimpleMatch 'LogExit: Exiting' | Select-Object -First 1)
    if ($windows -lt $expected -and -not $AllowPartial) {
        Write-Host ("  {0}: INCOMPLETE ({1}/{2} windows) -- NOT SUMMARISED. It is still running, or it died. " -f $name, $windows, $expected) -ForegroundColor Yellow
        Write-Host ("     bar: {0}" -f $basis) -ForegroundColor Yellow
        if ($exited) {
            Write-Host "     NOTE: this log DOES contain 'LogExit: Exiting', so the process shut down cleanly." -ForegroundColor Yellow
            Write-Host "     A clean exit with too few windows means the leg was CUT SHORT, not that it is still running." -ForegroundColor Yellow
        }
        Write-Host "     A partial flight leg reads like a slow configuration. Wait for it, or pass -AllowPartial and say so in whatever you write." -ForegroundColor Yellow
        continue
    }
    if (-not $exited) {
        Write-Host ("  {0}: window count is sufficient ({1}/{2}) but the log has NO 'LogExit: Exiting' line." -f $name, $windows, $expected) -ForegroundColor Yellow
        Write-Host "     The leg met its window bar without shutting down cleanly. Summarising, but treat the tail as suspect." -ForegroundColor Yellow
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

    $parkLine = @($lines | Select-String -SimpleMatch 'Voxel park ([^)]*window)') | Select-Object -Last 1
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
    $specLine = @($lines | Select-String -SimpleMatch 'Voxel speculation ([^)]*window)') | Select-Object -Last 1
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

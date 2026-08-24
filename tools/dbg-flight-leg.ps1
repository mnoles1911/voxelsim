# Run ONE headless FLIGHT leg, and refuse to start if anything else is running.
#
# WHY THIS EXISTS SEPARATELY FROM voxel-run-leg.ps1. That script stops the editor
# when the world has been settled for N samples, which is right for a COLD-FILL
# leg and silently wrong for a flight leg: the world settles during the preflight,
# its settle rule fires, and it kills the editor BEFORE THE FLIGHT STARTS while
# returning success (found in Wave S0 --
# docs/measurements/s0-apply-census-2026-07-27.txt). It now refuses
# -VoxelPerfFlight= outright and points here.
#
# A flight leg does not need a settle rule. UVoxelPerfRunSubsystem calls
# RequestExit itself at PreflightSec + VoxelPerfRun + LingerSec, so the run's own
# clock decides when it is done and there is no lull to mistake for a finish.
#
# ============================================================================
# THE ONE-EDITOR RULE, AND WHY IT IS ENFORCED RATHER THAN REMEMBERED
# ============================================================================
#
# 2026-07-27: two legs ran CONCURRENTLY for 86 seconds because a new sweep was
# launched while the previous sweep's loop was still going. Both were real runs
# writing real logs; nothing errored; the numbers were simply contended. The
# contaminated leg then went into a measurements file as a REJECTED result with
# a confident mechanical explanation attached to it.
#
# That is worse than a crash. A crashed leg is obviously void; a contended leg
# looks exactly like a slow configuration, and "this setting made it worse" is
# precisely the shape of conclusion that a second process on the GPU produces
# for free.
#
# It was caught by a human noticing two game windows on screen, which is not a
# control. So: this script refuses to start while any UnrealEditor process is
# alive, and prints what it found.
#
# Ground rule 1 says never conclude from a single run. This is its sibling:
# never conclude from a run that was sharing the box.
#
# ============================================================================
# THE SKY PINS, AND WHY THEY DEFAULT TO A FROZEN NOON
# ============================================================================
#
# A day/night clock now exists (UVoxelSkySubsystem). Left to itself it advances
# every frame, which means a leg run today and the same leg run twenty minutes
# ago are measured under different sun angles -- and the sun is not a cosmetic
# variable on this draw path. A movable directional light that has NOT rotated
# since spawn lets the renderer keep its cached whole-scene shadow setup; a sun
# that rotates re-renders the resident geometry into the cascades it touches.
#
# Every perf baseline this project has on record was taken before the clock
# existed, i.e. with a sun frozen since spawn -- including docs/backlog.md Â§0's
# shadowGather=0 / ~1.03 gathers-per-frame figure, which is the number people
# quote. So the defaults here are chosen to REPRODUCE THAT, not to be
# interesting:
#
#   -TimeOfDay 12:00 and -Date 03-20 are the engine's own defaults
#     (VoxelSkySubsystem.cpp:254-269 -- equinox noon at 52 N, ~38 deg altitude,
#     picked so a no-switch frame matches the pre-clock static rig). Passing
#     them explicitly changes nothing about what a leg measures; it only puts
#     the value in the log and in the run's JSON, where it can be checked.
#
#   -TimeScale 0 FREEZES the sun, which the engine default (1.0) does not.
#     This is the one place the harness deliberately departs from the engine
#     default, and it is the whole point: with TimeScale 1 every leg is a
#     different measurement of a moving target, no leg is reproducible, and no
#     historical baseline is comparable to anything run after the clock landed.
#
# A leg that WANTS a moving sun passes -TimeScale 1 and gets a Warning in its
# own log saying its numbers are not comparable across the leg. That is the
# frozen-vs-moving comparison, and tools/voxel-sun-arms.ps1 is how to run it --
# alternated, both arms otherwise identical. Do not hand-roll it here.
#
# Usage:
#   tools\voxel-run-flight-leg.ps1 -LogName s1close-1 `
#       -Cvars "voxel.Stream.PoolBatchPublish 1, voxel.Stream.CoverageVerify 1"
#
# Note -ExecCmds is assembled and quoted HERE. Start-Process -ArgumentList does
# not re-quote an argument containing spaces, so a hand-built '-ExecCmds=a 1, b 1'
# reaches UE as '-ExecCmds=a' and every cvar after the first space is dropped --
# which in Wave S0 presented as every gated timing reading 0.00ms, i.e. "the
# apply path costs nothing", in an otherwise healthy-looking log.

param(
    [Parameter(Mandatory=$true)][string]$LogName,
    [string]$Cvars = 'voxel.Stream.CoverageVerify 1',
    [string[]]$ExtraArgs = @(),
    [int]$RunSec = 120,
    [int]$PreflightSec = 90,
    [int]$LingerSec = 60,
    [int]$LogIntervalSec = 2,
    # 'line' traverses virgin terrain and never revisits; 'surface' is the 100 m
    # circle, which revisits continuously. They are opposite extremes for
    # anything that CACHES geometry -- parking scored 84% hit / -18% chunks/s on
    # line, where there is nothing to revisit. Quote which one a result came from.
    [ValidateSet('line','surface','underground','static')][string]$Flight = 'line',
    [string]$SpawnAt = '-84480,53760',
    # RENDER RESOLUTION, and it is not cosmetic. The harness defaulted to
    # 1600x900 (systemresolution.resx in the log), while the owner's frame-rate
    # target is 2560x1440 -- 3.686M pixels against 1.44M, a 2.56x difference in
    # anything pixel-bound. A frame-rate result that does not state its
    # resolution is not a result. Pass -Width/-Height and say which in whatever
    # you write.
    [int]$Width = 1600,
    [int]$Height = 900,
    [int]$TimeoutSec = 480,
    # THE SKY PINS. See "THE SKY PINS, AND WHY THEY DEFAULT TO A FROZEN NOON"
    # in the header for why these three defaults are what they are. Short
    # version: 12:00 / 03-20 is the engine's own default pose, so every leg ever
    # taken stays comparable, and TimeScale 0 is what makes a leg reproducible
    # at all.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0,
    # SILENCE THE CORRECTNESS INSTRUMENT. Deliberate, stated, and rare -- see
    # the block below the param() for why arming it is the default and what it
    # cost when it was not.
    [switch]$NoCoverageVerify,
    # Same reasoning as -NoCoverageVerify: silencing the Goal 3 instrument is a
    # decision someone has to type.
    [switch]$NoFramePhase,
    [switch]$KeepEditLog,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    # Keep Epic's MCP server up for this leg (default: off, see below).
    [switch]$Mcp
)

$ErrorActionPreference = 'Stop'

# ============================================================================
# THE CORRECTNESS INSTRUMENT IS ARMED ON EVERY LEG, WHATEVER -Cvars SAYS
# ============================================================================
#
# voxel.Stream.CoverageVerify counts XY footprints in a ring's core annulus
# that no level is visibly covering -- the see-through holes, as a number. It
# defaults to 0 in VoxelDebug.cpp, so it exists only if a leg asks for it.
#
# IT USED TO BE ARMED ONLY BY THE -Cvars DEFAULT, WHICH IS NOT THE SAME THING.
# -Cvars is a single string: any caller passing -Cvars for an unrelated reason
# REPLACED the default and silently disarmed the hole counter. That is exactly
# what happened -- every leg of the 50k campaign (gp-ctl2, ahead-on, and ~20
# legs after them) passed -Cvars 'voxel.Debug 0' to pin the HUD, and so ran
# with the only instrument that can see a silent-correctness failure switched
# off. Note voxel.Debug never gated it; the string replacement did.
#
# The cost of that: a leg carrying a DARK GPU claim stage -- 909,717 records
# the host staged and the GPU claimed none of -- was read as the session's best
# result, and nothing in the log could have contradicted it. When the counter
# was finally armed on the same configuration it read holes 26,364 -> 0 and the
# P1 xcheck read 0 unwritten, which is what actually cleared that leg.
#
# So it is appended here, after the caller's string, rather than being a
# default the caller can displace without noticing. Silencing it now takes
# -NoCoverageVerify, which is a decision someone has to type and which says so
# in the log line below.
#
# COST, SETTLED 2026-08-23 against an apparent contradiction in the source.
# The cvar help says it "re-walks every ring's annulus"; the call site's comment
# says "one cvar read when off". Both are true and they describe different
# states: LogCoverageVerify() is called from MaybeLogCounters, which runs ONCE
# PER LOG WINDOW (-VoxelPerfLogInterval, 2 s on every leg here) and NOT per
# tick. So the armed cost is one annulus walk per ~100 ticks, and the disarmed
# cost is a single cvar read. There is no contradiction to resolve in the code;
# the imprecision was in an earlier revision of THIS comment.
#
# The closest matched observation: the
# closest matched observation is q-cache128k (verify OFF) against q-repro-main
# (verify ON), same arm: both settle 22.3 s at 7,387 chunks/s, identical to the
# digit. Those two legs differ in BINARY as well as in this switch, so strictly
# they bound the PAIR of changes at ~0 rather than this switch alone -- two
# variables, one equation. Read it as "no cost has ever been observed", not as
# a measured cost. If a leg ever does find it perturbing a result, state the
# number and disarm deliberately -- do not quietly drop it by passing -Cvars.
#
# (An earlier revision of this comment claimed "21.3 s -> 21.5 s, under 1%".
# That was never measured; the legs it named were different configurations.
# Corrected 2026-08-23 rather than deleted, because an invented number inside a
# comment that justifies a default is exactly the shape this file exists to
# stop.)
if (-not $NoCoverageVerify) {
    if ($Cvars -notmatch 'CoverageVerify') {
        $Cvars = if ([string]::IsNullOrWhiteSpace($Cvars)) { 'voxel.Stream.CoverageVerify 1' }
                 else { "$Cvars, voxel.Stream.CoverageVerify 1" }
    }
    Write-Host "  coverage verify ARMED (holes= on the streaming line)"
} else {
    Write-Host "  coverage verify DISARMED by -NoCoverageVerify -- this leg cannot report a hole" -ForegroundColor Yellow
}

# ============================================================================
# THE GOAL 3 INSTRUMENT IS ARMED ON EVERY LEG
# ============================================================================
#
# The owner's third goal is ">100 FPS after cold start settles". Until
# -VoxelFramePhase existed, every frame figure this campaign produced blended
# two regimes: the log's `post-warmup (t>=10s)` bucket opens at t=10 s while a
# leg settles at ~21 s, so ELEVEN SECONDS of cold-fill storm -- the regime where
# hitches are explicitly authorised -- were averaged into the same p95 as ~270 s
# of settled play. That is why the baseline reads p95=30.62 ms against
# p50=9.84 ms. No leg taken before this switch can support a >100 FPS claim.
#
# -VoxelFramePhase=1 segments the frame histogram at the ACTUAL cold-settle
# boundary, told by the settle line itself rather than guessed from apply
# volume. It is a histogram (4.4 KB at any leg length, so a long leg cannot
# silently start dropping samples) and it prints its bin width beside every
# percentile, because p95 is judged against a 10.00 ms gate and a reader has to
# see that 0.10 ms is 1% of that.
#
# Armed by default for the reason CoverageVerify is: a gated instrument nobody
# remembers to arm is this project's eleven-inert-features failure, and Goal 3
# requires EVERY leg to report its settled segment.
#
# FAILING READINGS: no `Voxel frame dist` line at all = the hooks are not in
# the build. `SETTLED total: n=0` = the leg never settled, or hook 2 is missing
# -- and NO >100 FPS CLAIM MAY BE MADE FROM SUCH A LEG in either case. Never
# report fill-segment numbers under a settled heading.
if (-not $NoFramePhase) {
    # -notmatch on an ARRAY returns the FILTERED ARRAY, not a boolean, and a
    # non-empty array is truthy -- so this fired even when the caller had
    # already passed -VoxelFramePhase=3, putting BOTH =3 and =1 on the command
    # line. FParse::Value takes the first, so the caller's value happened to
    # win and M20 was valid, but that was luck. -join makes it a string test.
    if (($ExtraArgs -join ' ') -notmatch 'VoxelFramePhase') {
        $ExtraArgs = @($ExtraArgs) + @('-VoxelFramePhase=1')
    }
    Write-Host "  frame phase ARMED (settled-segment p50/p95/p99 -- Goal 3)"
} else {
    Write-Host "  frame phase DISARMED by -NoFramePhase -- this leg cannot support a >100 FPS claim" -ForegroundColor Yellow
}
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path (Resolve-Path "$PSScriptRoot\..").Path "Saved\$LogName.log"

# THE GUARD. See the header.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw ("REFUSING TO START: $($running.Count) editor process(es) already running -- $detail. " +
           "Two legs sharing the box produce contended numbers that look exactly like a slow " +
           "configuration, and nothing in the log says so. Wait for the other run, or stop it. " +
           "See the header of this script and docs/measurements/s1-close-2026-07-27.txt.")
}

# The edit log persists across runs and replays on load, so a leg measured
# without clearing it is not cold (ground rule 11).
if (-not $KeepEditLog) {
    $dir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
    if (Test-Path $dir) {
        Get-ChildItem $dir -Filter *.vxlog -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',

    "-abslog=`"$LogPath`"",
    "-ResX=$Width", "-ResY=$Height", '-WinX=0', '-WinY=0',
    "-VoxelSpawnAt=$SpawnAt",
    # Beside the spawn, because they are the same kind of thing: state that
    # decides what the run measures and that nothing downstream can recover if
    # it is left implicit. InvariantCulture on the scale deliberately --
    # PowerShell interpolates a [double] with the CURRENT culture, so on a
    # comma-decimal machine "-VoxelTimeScale=0,5" reaches FParse::Value, which
    # stops at the comma, and the run silently freezes at 0 while the script
    # says 0.5. Same class of silent truncation the -ExecCmds note below
    # describes.
    "-VoxelTimeOfDay=$TimeOfDay",
    "-VoxelDate=$Date",
    "-VoxelTimeScale=$($TimeScale.ToString([cultureinfo]::InvariantCulture))",
    "-VoxelPerfRun=$RunSec",
    "-VoxelPerfFlight=$Flight",
    "-VoxelPerfPreflightSec=$PreflightSec",
    "-VoxelPerfLingerSec=$LingerSec",
    "-VoxelPerfLogInterval=$LogIntervalSec",
    "-ExecCmds=`"$Cvars`""
) + $ExtraArgs

$started = Get-Date
# MCP OFF FOR MEASUREMENT LEGS.
#
# ue-project/Config/DefaultEditorPerProjectUserSettings.ini sets
# bAutoStartServer=True, so EVERY editor launch binds 127.0.0.1:8000 and builds
# Epic's MCP tool registry -- including a timing leg, where nothing asked for it
# and nobody is connected. The cost is probably small; the point is that it is
# unmeasured, and a leg is supposed to differ from its pair in exactly one thing.
#
# ShouldAutoStartServer() has NO command-line opt-out -- the param can only
# force the server ON -- so this uses UE's generic ini override, which beats the
# ini value for this process only and leaves the file alone. Pass -Mcp to keep
# the server up for a leg you actually intend to drive.
#
# Appended rather than placed in the array literal on purpose: a `$(if ...)`
# that yields nothing inserts a $null into the array, and Start-Process passes
# that to the editor as an empty argument.
if (-not $Mcp) {
    $argList += '-ini:EditorPerProjectUserSettings:[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]:bAutoStartServer=False'
}

$argList | ForEach-Object { Write-Host ("ARG>[" + $_ + "]") }
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
$p.WaitForExit($TimeoutSec * 1000) | Out-Null
Write-Host ("CHILD exited=" + $p.HasExited + " code=" + $(if ($p.HasExited) { $p.ExitCode } else { "n/a" }))

if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    Write-Host "  ${LogName}: KILLED at ${TimeoutSec}s -- treat as VOID, not as a slow result." -ForegroundColor Yellow
    return $false
}

# A flight leg's own clock is ~PreflightSec + RunSec + LingerSec. Much shorter
# than that means it died rather than finished, and a short log is not a fast run.
$elapsed = [int]((Get-Date) - $started).TotalSeconds
$expected = $PreflightSec + $RunSec + $LingerSec
if ($elapsed -lt ($expected * 0.9)) {
    Write-Host ("  ${LogName}: exited after ${elapsed}s but the run's own clock is ~${expected}s -- " +
                "VOID (it did not complete its flight).") -ForegroundColor Yellow
    return $false
}

# THE WALL-CLOCK TEST ALONE IS NOT AN ACCEPTANCE TEST, and it has now let a bad
# leg through. `$elapsed` is measured from Start-Process, so it includes ~20 s of
# editor startup before the run's own clock begins; at a 0.9 threshold a leg can
# lose ~45 s of the phase being measured and still print "ok". One leg exited 6 s
# short of completing its flight and was caught only because it also happened to
# fall under 0.9 -- luck, not a check.
#
# So ask the RUN whether it finished, not the wall clock. Two witnesses, both
# produced by UVoxelPerfRunSubsystem before it calls RequestExit:
#   * "VoxelPerfRun complete" in the log, and
#   * Saved/PerfRuns/perf_*.json, written BEFORE the exit request -- so its
#     absence proves FinishRun never ran, which no timing heuristic can.
# Verified against the archive: v10-flight-1 (the leg that died early) has no
# completion line; v10-flight-2 and -3 both do. This discriminates the real cases.
#
# (Two sessions arrived at this same check independently and it merged as a
# conflict. This is main's wording, kept because it names the archive legs the
# discrimination was verified against.)
$logSaysComplete = (Test-Path $LogPath) -and
    (Select-String -Path $LogPath -Pattern 'VoxelPerfRun complete' -Quiet)
$perfDir = Join-Path (Split-Path $Project) 'Saved\PerfRuns'
$perfJson = if (Test-Path $perfDir) {
    Get-ChildItem $perfDir -Filter 'perf_*.json' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $started } | Select-Object -First 1
} else { $null }

if (-not $logSaysComplete -or -not $perfJson) {
    $why = @()
    if (-not $logSaysComplete) { $why += "no 'VoxelPerfRun complete' in the log" }
    if (-not $perfJson) { $why += "no perf_*.json written this run" }
    Write-Host ("  ${LogName}: ran ${elapsed}s but " + ($why -join ' and ') +
                " -- VOID. FinishRun did not complete, so the flight phase this leg " +
                "claims to measure did not finish. Do not read numbers out of it.") -ForegroundColor Yellow
    return $false
}

# The sun goes in the console line for the same reason the resolution does: a
# result pasted out of a terminal without it is not a result.
$sun = if ($TimeScale -eq 0) { "frozen $TimeOfDay $Date" } else { "MOVING x$TimeScale from $TimeOfDay $Date" }
Write-Host "  ${LogName}: ok (${elapsed}s) at ${Width}x${Height}, sun $sun" -ForegroundColor Green
return $true

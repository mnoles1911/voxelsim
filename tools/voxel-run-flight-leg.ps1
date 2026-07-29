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
# existed, i.e. with a sun frozen since spawn -- including docs/backlog.md §0's
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
    [switch]$KeepEditLog,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
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
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
$p.WaitForExit($TimeoutSec * 1000) | Out-Null

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

# THE WALL-CLOCK TEST ALONE IS NOT AN ACCEPTANCE TEST, and this is the second
# thing it let through. `$elapsed` is measured from Start-Process, so it includes
# ~20 s of editor startup before the run's own clock begins; at a 0.9 threshold a
# leg can lose ~45 s of the phase being measured and still print "ok". One leg
# did exactly that -- it exited 6 s short of completing its flight and was caught
# only because it happened to fall under 0.9 as well.
#
# So ask the RUN whether it finished, not the wall clock. Two independent
# witnesses, both produced by UVoxelPerfRunSubsystem before it calls RequestExit:
#   * "VoxelPerfRun complete" in the log, and
#   * Saved/PerfRuns/perf_*.json, written BEFORE the exit request -- so its
#     absence proves FinishRun never ran, which no timing heuristic can.
# Either one missing means the leg did not finish, whatever the clock says.
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

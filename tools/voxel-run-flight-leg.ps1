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
    [int]$Width = 2560,
    [int]$Height = 1440,
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
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    # Keep Epic's MCP server up for this leg (default: off, see below).
    [switch]$Mcp
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path (Resolve-Path "$PSScriptRoot\..").Path "Saved\$LogName.log"

# THE GUARD. See the header.
# THE GUARD ALSO HAS TO SEE A BUILD, NOT JUST ANOTHER EDITOR.
#
# It used to check for editors only. A leg started into a live build is exactly
# the contended-numbers case this guard exists to prevent -- five cl.exe eating
# the cores produce a leg that reads like a slow configuration, and nothing in
# the log says so. Worse, the reverse order corrupts the binary rather than the
# numbers: a build that starts while an editor holds
# UnrealEditor-VoxelEarthShaders.dll fails its link, leaves one DLL of the pair
# relinked and the other stale, and everything that runs afterwards measures an
# incoherent build. Both of those happened on 2026-08-24/25.
#
# dotnet IS ON THE LIST DELIBERATELY: UnrealBuildTool runs as dotnet.exe, so a
# build in its UBT phase shows up under neither cl nor UnrealBuildTool and the
# obvious three-name check misses exactly the window where the link happens.
# (Credit: the front-end session, which was checking this by hand.)
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor, cl, link, UnrealBuildTool, MSBuild, dotnet -ErrorAction SilentlyContinue)

# AND HOW OLD THE HOLDER IS, because "an editor is running" is not the same
# question as "the box is in use".
#
# On 2026-08-25 a capture held the box for FOUR HOURS and two queued jobs of mine
# expired waiting politely behind it -- about two and a half hours of measurement
# time, and I reported to the owner that the other lane was busy. It was not busy
# in any sense that mattered.
#
# THE PART THAT DEFEATS THE OBVIOUS HEURISTICS: it was NOT wedged. It was working
# perfectly and had simply never been told to stop -- both its shots had fired,
# its log was still growing at 45 MB, it had rendered 1.58 MILLION settled frames,
# CPU ~2.5 cores, Responding=True. A process doing exactly its job forever is
# indistinguishable from a spinning one by CPU, and from a blocked one by
# responsiveness. Do not try to classify it.
#
# So this does NOT say "wedged" -- that would have been wrong about the real case.
# It says the only thing that is both true and actionable: this has held the box
# far longer than any leg should, so a human (or the session that owns it) needs
# to look, rather than the next job queuing behind it in silence.
#
# The cause there was a real defect worth knowing: -VoxelLoadingShotAt does not
# quit after its last shot if that shot lands AFTER hand-off -- the front end
# tears down and takes whatever quits the run with it. The image is produced and
# the process never exits.
if ($running.Count -gt 0) {
    $oldestMin = ($running | ForEach-Object { ((Get-Date) - $_.StartTime).TotalMinutes } | Measure-Object -Maximum).Maximum
    if ($oldestMin -gt 15) {
        Write-Host ("  NOTE: the holding process is {0:N0} minutes old. No leg runs that long -- it has " -f $oldestMin) -ForegroundColor Yellow
        Write-Host "  most likely finished its work and never exited. Check it rather than waiting for it." -ForegroundColor Yellow
    }
}
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
    # RESOLUTION: -ResX/-ResY ALONE IS INERT. Proven 2026-08-25 -- a leg run
    # with -ResX=2560 -ResY=1440 and a leg with -ResX=800 -ResY=450 BOTH
    # rendered view=1552x873, byte-identical to every -ResX=1600 leg on disk.
    # The size came from GameUserSettings (resolution.resx="1280" in the log)
    # scaled by the 1.25 desktop DPI; the command line never touched it. So
    # every frame-rate number this project held was taken at 1552x873 while
    # its own banner claimed whatever -Width said.
    #
    # The ini overrides are what actually move it. FullscreenMode 2 = windowed.
    # VERIFY, DO NOT ASSUME: the post-run check below reads the marcher's own
    # view= and refuses a clean 'ok' when it disagrees with -Width.
    "-ResX=$Width", "-ResY=$Height", '-WinX=0', '-WinY=0',
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:ResolutionSizeX=$Width",
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:ResolutionSizeY=$Height",
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:LastUserConfirmedResolutionSizeX=$Width",
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:LastUserConfirmedResolutionSizeY=$Height",
    # FULLSCREEN MODE 1 = WINDOWED-FULLSCREEN (borderless), NOT 2 = windowed.
    # THE OWNER PLAYS FULLSCREEN AT 2560x1440 (stated 2026-08-25), so a windowed
    # leg measures a different game: 1.36 Mpx against 3.69, a 2.7x difference in
    # anything pixel-bound. Mode 2 was the first thing tried here and it is why
    # -ResX/-ResY looked inert -- ResX/ResY are honoured on the FULLSCREEN path.
    # Borderless rather than exclusive (mode 0) because an -unattended leg that
    # takes exclusive display mode can wedge the box.
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:FullscreenMode=1",
    "-ini:GameUserSettings:[/Script/Engine.GameUserSettings]:LastConfirmedFullscreenMode=1",
    # -ForceRes REMOVED 2026-09-03: it pins the command-line size AND blocks
    # runtime resolution changes -- the -VoxelForceRes/ApplySettings call it
    # was swallowing is the one channel that works. (OWNERRES-1440B: force
    # res APPLIED in the log, viewport still 1280x720, with this flag on.)
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
    # -VoxelForceRes IS THE RESOLUTION AUTHORITY NOW (2026-09-03): applied
    # in AVoxelEarthGameMode::BeginPlay through UGameUserSettings (not
    # persisted), because every external channel is proven dead. The -ini
    # GameUserSettings overrides above were proven to move the size on
    # 2026-08-25 and were observed DEAD by 2026-09-02: legs asking 1600x900
    # AND 2560x1440 both rendered 1280x720 -- UE's factory-default
    # GameUserSettings size, i.e. the overrides stopped latching entirely
    # (and no Saved/Config/Windows/ exists for -game, so nothing persisted
    # either). Rather than archaeology on the ini path, the resolution rides
    # -ExecCmds as a console command, which executes after boot and cannot
    # be ignored ('r.SetRes' via -ExecCmds was ALSO observed inert --
    # echoed empty, no effect, leg OWNERRES-1440): 'wf' = windowed-
    # fullscreen, matching the owner's
    # PreferredFullscreenMode=1 at panel-native size -- which also sidesteps
    # the 1.25 desktop-DPI scaling that shrank windowed sizes. The ini
    # overrides above are kept as a harmless belt. VERIFIED, not assumed:
    # the post-run check reads the engine's own TSR/view lines.
    "-VoxelForceRes=${Width}x${Height}wf",
    # The self-correcting half: whatever window the engine actually creates,
    # screen percentage is set at runtime to reproduce the owner's INTERNAL
    # resolution (1552x873, from his flown ProfileGPU) -- the ray count is
    # the frame's dominant term and the number that must match his config.
    '-VoxelForceInternal=1552x873',
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

# EXIT CODES, NOT JUST A RETURN VALUE.
#
# Every VOID path below used `return $false`. At script scope that sets no
# process exit code, so `powershell -File voxel-run-flight-leg.ps1 ...` exits 0
# on a leg this script has just correctly diagnosed as void -- the careful
# three-witness acceptance test below reaches nothing that automates it. A leg
# that died 15 s into a 270 s flight reported success to its caller, which is
# the same failure family as a piped Build.bat reporting the pager's exit code.
# No caller consumes the boolean (checked across tools/ and docs/ -- every
# other reference is a comment), so these are `exit 1` / `exit 0` now.

$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
$p.WaitForExit($TimeoutSec * 1000) | Out-Null

if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
    Write-Host "  ${LogName}: KILLED at ${TimeoutSec}s -- treat as VOID, not as a slow result." -ForegroundColor Yellow
    exit 1
}

# A flight leg's own clock is ~PreflightSec + RunSec + LingerSec. Much shorter
# than that means it died rather than finished, and a short log is not a fast run.
$elapsed = [int]((Get-Date) - $started).TotalSeconds
$expected = $PreflightSec + $RunSec + $LingerSec
if ($elapsed -lt ($expected * 0.9)) {
    Write-Host ("  ${LogName}: exited after ${elapsed}s but the run's own clock is ~${expected}s -- " +
                "VOID (it did not complete its flight).") -ForegroundColor Yellow
    exit 1
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
    exit 1
}

# The sun goes in the console line for the same reason the resolution does: a
# result pasted out of a terminal without it is not a result.
$sun = if ($TimeScale -eq 0) { "frozen $TimeOfDay $Date" } else { "MOVING x$TimeScale from $TimeOfDay $Date" }
# THE BANNER USED TO ECHO ITS OWN PARAMETER, WHICH IS NOT A MEASUREMENT.
#
# It printed "at ${Width}x${Height}" straight from the argument it was handed,
# so a leg that asked for 2560x1440 and rendered 1552x873 reported success at
# 2560x1440 and was quoted that way. Read the ENGINE'S size instead: the
# marcher prints `view=WxH px` and the renderer prints `SceneColor WxH`. This
# is the same rule as every other arm here -- prove it engaged, never assume.
# TWO SIZES, BOTH READ FROM THE ENGINE (2026-09-03). Under the owner's config
# (sg.ResolutionQuality=0 -> TSR at ~60.6%), the marcher's view= is the
# INTERNAL resolution (~1552x873 at 1440p output) and is SUPPOSED to differ
# from -Width: comparing view= against the window made every correctly-
# configured leg read as a warning. The OUTPUT size is what the request
# governs; TSR's own upscale line names both sides, so parse it first and
# fall back to view= when TSR is off (then internal IS output).
$internalRes = $null
$outputRes = $null
try {
    # LAST match, not first: the first view= prints before the 2 s
    # -VoxelForceInternal apply and reads the boot default (OWNERRES-1440D:
    # first 1280x720, last 1552x873 -- the last is the leg's real size).
    $m = Select-String -Path $LogPath -Pattern 'view=(\d+)x(\d+) px' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($m) { $internalRes = "$($m.Matches[0].Groups[1].Value)x$($m.Matches[0].Groups[2].Value)" }
} catch { }
try {
    # The engine's own report of the window it actually created (the TSR
    # upscale line only exists in ProfileGPU dumps, never plain logs).
    $t = Select-String -Path $LogPath -Pattern 'Voxel force internal: window (\d+)x(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($t) { $outputRes = "$($t.Matches[0].Groups[1].Value)x$($t.Matches[0].Groups[2].Value)" }
} catch { }
if (-not $outputRes) { $outputRes = $internalRes }
if (-not $internalRes) {
    Write-Host "  ${LogName}: ok (${elapsed}s) at RESOLUTION UNVERIFIED (no view= line in the log -- do not quote an fps number from this leg until you know its size), sun $sun" -ForegroundColor Yellow
}
elseif ($outputRes -ne "${Width}x${Height}") {
    Write-Host "  ${LogName}: ok (${elapsed}s) but OUTPUT ${outputRes} (internal ${internalRes}), NOT the ${Width}x${Height} requested -- r.SetRes did not latch. The leg is VALID, but quote internal ${internalRes} -> output ${outputRes}. sun $sun" -ForegroundColor Yellow
}
else {
    Write-Host "  ${LogName}: ok (${elapsed}s) OUTPUT ${outputRes}, internal ${internalRes} (both read from the engine; the gap is the owner's TSR screen percentage), sun $sun" -ForegroundColor Green
}
exit 0

# Capture ONE settled screenshot of the world, headless and unattended.
#
# WHY THIS EXISTS AS A FILE. Worldgen changes are judged on instruments here --
# vxc_terrainprobe, vxc_bench digests, the leg harness -- and that is right,
# because screenshots have twice failed to catch what the probe caught. But the
# converse also happened this session: three of the v10 detail terms measured as
# "no effect" for hours because the METRIC was binned on the wrong quantity. An
# instrument you have not sanity-checked against a picture is not obviously
# better than a picture. So: both, and this is the picture half, as a committed
# file rather than a pasted command line.
#
# THE ONE THING THAT MAKES CAPTURES LIE. A blank or half-empty frame is almost
# never a rendering bug -- it is terrain that has not streamed in yet. Capture
# too early and you photograph an empty world and then go debugging the mesher.
# So this script does two things about it:
#   * waits -SettleSec before capturing (the engine-side timer is
#     -VoxelScreenshotAfter, which UE runs off the world timer, so it is real
#     game time and not wall clock), and
#   * AFTERWARDS greps the log for the streaming counters and prints them next
#     to the file it wrote, so the capture can be believed or thrown away on
#     evidence rather than on how it looks.
#
# It also inherits the one-editor rule from voxel-run-flight-leg.ps1, for the
# same reason: a second editor on the box does not corrupt a screenshot the way
# it corrupts a timing, but it WILL make this script's own settle wait
# meaningless, and it can trip the other run.
#
# THE OTHER THING THAT MAKES CAPTURES LIE, AS OF THE DAY/NIGHT CLOCK. The sun
# now MOVES unless told not to, and a capture whose sun drifted between the
# settle wait and the shutter is not reproducible: re-run it and the shadows,
# the exposure and the colour temperature are all somewhere else. Worse, at
# epoch 0 the clock is at midnight, and a black frame and an unstreamed frame
# are the same picture -- which is the failure this script's whole header is
# about, arriving by a second route.
#
# So -TimeOfDay/-Date/-TimeScale default to a FROZEN 12:00 on 03-20, which is
# the engine's own default pose (VoxelSkySubsystem.cpp:254-269, equinox noon at
# 52 N) and therefore byte-comparable with every capture in the archive taken
# before the clock existed. -TimeScale 0 is the departure from the engine
# default (1.0) and is what makes a capture repeatable at all. Step the hour
# deliberately -- -TimeOfDay 06:30 -- to photograph a sunrise; do not get one by
# accident from a long settle.
#
# THE THIRD THING THAT MAKES CAPTURES LIE, AND IT LIED FOR EIGHT SHOTS. Until
# -SpawnAltM/-SpawnPitch existed there was no altitude or pitch control anywhere
# on this path: every capture put a pawn ON the terrain surface at the spawn
# column, looking dead level, which frames the slope the pawn is standing on.
# Eight such captures were reviewed together and the world was reasonably called
# FLAT -- a world that spans 12.5 km vertically and contains a 6,125 m massif.
# It took a stitched heightmap to establish that the terrain was fine and the
# CAMERA was the problem. A ground-level shot is evidence about ground cover and
# material, and it is NOT evidence about landform; if the question is landform,
# pass -SpawnAltM and a negative -SpawnPitch, and say which in the writeup.
#
# Usage:
#   tools\voxel-capture.ps1 -Name v10-alpine -SpawnAt '-65000,60000'
#   tools\voxel-capture.ps1 -Name v10-wide  -SpawnAt '-84480,53760' -SettleSec 150
#   tools\voxel-capture.ps1 -Name sunrise   -TimeOfDay 06:30
#   tools\voxel-capture.ps1 -Name vista     -SpawnAt '7680,7680' -SpawnAltM 2000 -SpawnPitch -25 -SettleSec 150

param(
    [Parameter(Mandatory=$true)][string]$Name,
    # Defaults to the tile-coverage centre. OFF the generated tiles you are
    # photographing the flat fallback plane, which looks like a bug and is not.
    [string]$SpawnAt = '-84480,53760',
    # Long by default. The flight legs settle during a 90 s preflight, and this
    # is the one parameter where being wrong is silent.
    [int]$SettleSec = 120,
    # THE VISTA CONTROLS (see the header). Metres ABOVE the terrain surface at
    # the spawn column, and camera pitch in degrees with NEGATIVE looking down.
    # Both default to 0, which passes NOTHING on the command line and therefore
    # reproduces every pre-existing capture exactly: ground spawn, level camera.
    [double]$SpawnAltM = 0,
    [double]$SpawnPitch = 0,
    # Yaw in degrees, 0 = +X (the historical fixed value), 90 = +Y. Added for
    # the v25 entrance captures: a hillside cave mouth faces downhill, so with
    # yaw pinned at 0 a site whose mouth opens toward -Y can only be
    # photographed from behind the hill. Default 0 keeps every existing command
    # line byte-identical.
    [double]$SpawnYaw = 0,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$TimeoutSec = 420,
    # THE SKY PINS. Defaults documented in the header; they reproduce the
    # pre-clock static rig's pose, and TimeScale 0 is what stops the sun
    # drifting between -SettleSec and the shutter. Same three switches, same
    # defaults, as tools/voxel-run-flight-leg.ps1 -- a capture and a leg that
    # disagreed about the sun would be the two halves of one investigation
    # looking at different worlds.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0,
    [string]$Cvars = '',
    # Keep the persisted edit log instead of clearing it. Only for deliberately
    # photographing an EDITED world; see the note above the clear below.
    [switch]$KeepEditLog,
    [string[]]$ExtraArgs = @(),
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path $Root "Saved\capture-$Name.log"
$ShotDir = Join-Path $Root 'ue-project\Saved\Screenshots\WindowsEditor'

# THE EDIT LOG PERSISTS ACROSS RUNS AND OVERRIDES -VoxelSpawnAt. This script did
# not clear it, and voxel-run-flight-leg.ps1 always has (its ground rule 11: a leg
# measured without clearing it is not cold). The consequence here is worse than a
# stale measurement, because it moves the CAMERA:
#
#   2026-07-29: a capture launched with -VoxelSpawnAt=-837120,314880 (the plains
#   exemplar) came up at the ALPINE exemplar instead -- the position left behind by
#   the previous capture. It was caught only because the fine tier happened to be
#   pinned to the plains tile, so the run tripped the residency gate and died
#   loudly. With the coarse tier, or with both tiles resident, it would have
#   written a screenshot named "plains" showing a mountain, and nothing in the log
#   or the filename would have said so.
#
# So: clear it, and clear it BEFORE the one-editor check so a refused start does
# not leave a half-prepared state.
if (-not $KeepEditLog) {
    $worldDir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
    if (Test-Path $worldDir) {
        Get-ChildItem $worldDir -Filter *.vxlog -ErrorAction SilentlyContinue | Remove-Item -Force
    }
}

$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw "REFUSING TO START: $($running.Count) editor process(es) already running -- $detail."
}

# Note which shots already exist, so the one this run produces can be named
# exactly rather than guessed at by timestamp.
$before = @{}
if (Test-Path $ShotDir) {
    Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        ForEach-Object { $before[$_.Name] = $true }
}

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',
    "-abslog=`"$LogPath`"",
    "-ResX=$Width", "-ResY=$Height", '-WinX=0', '-WinY=0',
    "-VoxelSpawnAt=$SpawnAt",
    # InvariantCulture on the scale: PowerShell interpolates a [double] with the
    # CURRENT culture, so on a comma-decimal machine "-VoxelTimeScale=0,5"
    # reaches FParse::Value, which stops at the comma, and the capture silently
    # runs frozen while the script says 0.5.
    "-VoxelTimeOfDay=$TimeOfDay",
    "-VoxelDate=$Date",
    "-VoxelTimeScale=$($TimeScale.ToString([cultureinfo]::InvariantCulture))",
    "-VoxelScreenshotAfter=$SettleSec"
)
# SAME InvariantCulture ARGUMENT AS -VoxelTimeScale ABOVE, and it bites harder
# here: on a comma-decimal machine "-VoxelSpawnPitch=-22,5" reaches FParse::Value,
# which stops at the comma, and the capture is taken at -22 deg while the script
# reports -22.5. Appended only when non-zero so a default run's command line is
# byte-identical to what it was before these switches existed -- the engine side
# treats an absent switch and a zero one the same, but an unchanged command line
# is the thing that makes that checkable.
if ($SpawnAltM -ne 0) {
    $argList += "-VoxelSpawnAltM=$($SpawnAltM.ToString([cultureinfo]::InvariantCulture))"
}
if ($SpawnPitch -ne 0) {
    $argList += "-VoxelSpawnPitch=$($SpawnPitch.ToString([cultureinfo]::InvariantCulture))"
}
if ($SpawnYaw -ne 0) {
    $argList += "-VoxelSpawnYaw=$($SpawnYaw.ToString([cultureinfo]::InvariantCulture))"
}
if ($Cvars) { $argList += "-ExecCmds=`"$Cvars`"" }   # embed the quotes; see the leg script
$argList += $ExtraArgs

$sun = if ($TimeScale -eq 0) { "sun frozen $TimeOfDay $Date" } else { "sun MOVING x$TimeScale from $TimeOfDay $Date -- NOT reproducible" }
$pose = if ($SpawnAltM -ne 0 -or $SpawnPitch -ne 0 -or $SpawnYaw -ne 0) { "alt +${SpawnAltM}m pitch ${SpawnPitch}deg yaw ${SpawnYaw}deg" } else { "GROUND spawn, level camera facing +X (landform will NOT read)" }
Write-Host "capture '$Name' at $SpawnAt, $pose, settling ${SettleSec}s, ${Width}x${Height}, $sun" -ForegroundColor Cyan
$started = Get-Date
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Warning "editor still alive after ${TimeoutSec}s -- killing. Capture may still be valid."
    try { $p.Kill() } catch {}
}
$elapsed = [int]((Get-Date) - $started).TotalSeconds

$shot = $null
if (Test-Path $ShotDir) {
    $shot = Get-ChildItem $ShotDir -Filter *.png -ErrorAction SilentlyContinue |
        Where-Object { -not $before.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# THE EVIDENCE, not the vibe. A capture with a small loaded-chunk count is a
# photograph of an empty world; print the numbers next to the file so that is
# decidable without opening the image.
if (Test-Path $LogPath) {
    # ANCHOR THIS TO THE STREAMING LINE. A bare 'loaded=(\d+)' also matches
    # `unloaded=` (the substring is right there) and the fine-tier line's own
    # `loaded=1`, so on the first fine-tier capture ever run it reported
    # "peak loaded=195548, final=1" -- the peak was the UNLOADED count and the
    # final was resident fine TILES. The world had 139,532 chunks and 64.4M
    # resident quads at the time, i.e. the two numbers that were supposed to
    # decide "did this stream in" both came from somewhere else. A harness that
    # reports a result it did not obtain is the failure this file exists to
    # prevent, so match the counter by its own line.
    $rx = 'Voxel streaming: loaded=(\d+)'
    $loaded = Select-String -Path $LogPath -Pattern $rx -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value }
    $peak = if ($loaded) { ($loaded | Measure-Object -Maximum).Maximum } else { 0 }
    $final = if ($loaded) { $loaded[-1] } else { 0 }
    Write-Host ("  streaming: peak loaded={0}, final={1}, samples={2}" -f
                $peak, $final, $loaded.Count)
    if (-not $loaded) {
        Write-Warning ("no 'Voxel streaming: loaded=' line in the log -- the streaming counters " +
                       "could not be read at all, so this capture has NO evidence either way " +
                       "about whether terrain was resident. Do not treat it as settled.")
    }
    elseif ($peak -lt 1000) {
        Write-Warning ("peak loaded=$peak is LOW -- this is very likely a photograph of " +
                       "terrain that had not streamed in. Raise -SettleSec, or check the " +
                       "spawn is inside the generated tile set, before believing the image.")
    }

    # A HIGH `peak loaded` IS NOT EVIDENCE OF SETTLING, AND IT PASSED A CAPTURE
    # THAT HAD NOT SETTLED. 2026-08-02, first desert vista: peak loaded=195,915
    # against a healthy band of ~40-60k for these shots. Read as a settle check
    # it looked like the BEST capture of the nine -- five times more terrain
    # than any other. It was the opposite. At the shutter the world was at
    # jobsInFlight=336, pendingJobs=941, unloaded=448,127: pure load/unload
    # churn, and `peak` was measuring the churn. Re-shot with a 420 s settle it
    # lands at 44,081, i.e. the honest number is the SMALLER one.
    #
    # `peak` and `final` cannot tell those two states apart, because a thrashing
    # world and a large settled world both report big numbers. The queue depths
    # can: a settled world has nothing in flight and nothing pending. So read
    # them, and say so out loud rather than leaving it to whoever opens the log.
    $q = @(Select-String -Path $LogPath -Pattern 'jobsInFlight=(\d+).*?pendingJobs=(\d+)') | Select-Object -Last 1
    $un = @(Select-String -Path $LogPath -Pattern 'unloaded=(\d+)') | Select-Object -Last 1
    if ($q) {
        $inFlight = [int]$q.Matches[0].Groups[1].Value
        $pending  = [int]$q.Matches[0].Groups[2].Value
        $unloaded = if ($un) { [int]$un.Matches[0].Groups[1].Value } else { -1 }
        Write-Host ("  settle: jobsInFlight={0} pendingJobs={1} unloaded={2}" -f
                    $inFlight, $pending, $unloaded)
        if ($inFlight -gt 0 -or $pending -gt 0) {
            Write-Warning ("NOT SETTLED at the shutter: jobsInFlight=$inFlight pendingJobs=$pending. " +
                           "The world was still streaming when the frame was taken, however large " +
                           "'peak loaded' looks. Raise -SettleSec and re-shoot; do not judge this image.")
        }
    } else {
        Write-Warning ("no jobsInFlight/pendingJobs line in the log -- 'peak loaded' ALONE cannot " +
                       "distinguish a settled capture from a thrashing one. Treat this capture's " +
                       "settle state as unknown.")
    }
    # CHUNKS THE GPU POOL REFUSED, which is a different failure from "not
    # streamed yet" and looks identical in the image: black gaps in otherwise
    # finished terrain. The fine tier is where this first showed up, because its
    # quad density is far higher -- the allocator reported 1,385,893 quads free
    # but a largest contiguous run of 642 against a 1,633-quad chunk, so the
    # pool was fragmented rather than full. Surfaced here because reading it out
    # of the log by hand is exactly what does not happen.
    $undrawn = @(Select-String -Path $LogPath -Pattern 'Chunk left undrawn' -AllMatches).Count
    if ($undrawn -gt 0) {
        Write-Warning ("$undrawn chunk(s) were left UNDRAWN by the GPU pool (search the log for " +
                       "'no room for'). Black gaps in this image are that, not missing terrain " +
                       "and not a mesher bug -- check the reported largest contiguous run before " +
                       "blaming pool size.")
    }
    # THE SUN THE FRAME WAS ACTUALLY TAKEN AT, read back rather than echoed from
    # the switches: the calendar quantises -Date to reachable days (7.6 real days
    # apart at the default DaysPerYear -- VoxelSkySubsystem.cpp:628-653), so
    # "-Date 06-21" and "the capture is at 21 June" are different claims. This is
    # the same argument as printing loaded= above instead of trusting -SettleSec.
    $sky = @(Select-String -Path $LogPath -SimpleMatch 'VoxelSky clock RESOLVED:') | Select-Object -First 1
    if ($sky) {
        Write-Host ("  sky: " + ($sky.Line -replace '^.*VoxelSky clock RESOLVED: ', ''))
    } else {
        Write-Warning ("no 'VoxelSky clock RESOLVED' line in the log -- either this build predates the " +
                       "day/night clock or the sky subsystem did not initialise. The sun in this image is " +
                       "whatever the static rig was left at; do not compare it against a pinned capture.")
    }

    # THE POSE THE FRAME WAS ACTUALLY TAKEN FROM, read back rather than echoed
    # from the switches -- same argument as the sky line above. -SpawnAltM is
    # relative to a surface height the engine has to probe for, so "I asked for
    # 2000 m" and "the camera was 2000 m above the terrain" are different claims,
    # and the pitch only reaches the camera through the controller. The engine
    # logs what it ACHIEVED; surface it here so nobody has to open the log.
    $pose = @(Select-String -Path $LogPath -SimpleMatch 'Spawn pose APPLIED:') | Select-Object -First 1
    if ($pose) {
        Write-Host ("  pose: " + ($pose.Line -replace '^.*Spawn pose APPLIED: ', ''))
    } elseif ($SpawnAltM -ne 0 -or $SpawnPitch -ne 0) {
        Write-Warning ("-SpawnAltM/-SpawnPitch were passed but no 'Spawn pose APPLIED' line is in the log -- " +
                       "the switches did NOT reach the ordinary spawn path (an editor built before they existed, " +
                       "or a fixture switch that poses its own camera took the spawn). This image is a GROUND " +
                       "shot; do not read landform from it.")
    }

    $errs = Select-String -Path $LogPath -Pattern 'Fatal|Assertion failed' | Select-Object -First 3
    if ($errs) { Write-Warning "log contains fatal lines:"; $errs | ForEach-Object { Write-Host "    $_" } }
}

if ($shot) {
    Write-Host "  wrote $($shot.FullName) after ${elapsed}s" -ForegroundColor Green
    $shot.FullName
} else {
    throw "no new screenshot appeared in $ShotDir after ${elapsed}s -- see $LogPath"
}

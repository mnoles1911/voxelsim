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
    # Yaw in degrees. The capture path's historical framing yaw is 45 and that
    # is what an unpassed -SpawnYaw still produces, so leaving this alone keeps
    # a run byte-identical to the archive. Passed, it is the only way to swing
    # the camera around a fixed column; see the pitch note below for why the
    # engine treats this switch and -SpawnPitch as a pair.
    [double]$SpawnYaw = 45,
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
    # THE TILE SET, AS SWITCHES RATHER THAN AS -ExtraArgs STRINGS. Which tiles a
    # capture loaded is the single most consequential thing about it and the
    # easiest to get silently wrong: the checked-in DefaultGame.ini points at
    # namespace 71e2b362e3241e71, and photographing a river fix with the wrong
    # namespace resident produces a picture of the OLD bake under the new
    # binary, which is indistinguishable from "the fix did nothing".
    #
    # Empty means "leave the ini alone", so an unpassed switch reproduces every
    # capture in the archive exactly. Passed, they are echoed in the banner and
    # checked against the engine's own resolved lines afterwards.
    #
    # -FineTileDir is the cache ROOT (the streamer appends
    # <provider_id>/<seed:016x>/s16), -CoarseTileDir is an s1 LEAF. They are not
    # the same shape of path and swapping them fails in different ways.
    [string]$FineTileDir = '',
    [string]$FineProviderId = '',
    [string]$CoarseTileDir = '',
    # THE NO-WATER CONTROL. -VoxelRiverRibbons=0 removes the far-field river
    # ribbon actor, which is what draws river water at any altitude above the
    # near-field implicit disc. It is a command-line switch and not a cvar
    # deliberately (VoxelRiverRibbonActor.h:221) because -ExecCmds lands after
    # BeginPlay, so passing it through -Cvars would silently do nothing.
    #
    # WHAT IT DOES NOT CONTROL, which matters for reading a near-field diff:
    # baked river water inside the implicit disc -- 65x65x33 bricks, +-25.6 m in
    # xy and +-12.8 m in z about the camera brick (VoxelWaterSubsystem.cpp:3005)
    # -- is drawn by RefreshImplicitWater and there is NO terrain-identical
    # switch that removes it. A control at a pose whose disc contains river
    # surface is therefore a PARTIAL control, and the diff under-reports.
    [switch]$NoRiverRibbons,
    # Keep the persisted edit log instead of clearing it. Only for deliberately
    # photographing an EDITED world; see the note above the clear below.
    [switch]$KeepEditLog,
    [string[]]$ExtraArgs = @(),
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    # THE PROJECT TO LAUNCH, WHICH IS NOT ALWAYS THIS SCRIPT'S OWN CHECKOUT.
    #
    # Captures get taken from a throwaway worktree so the capture branch does
    # not sit on top of whatever else is in flight -- but a worktree has SOURCE
    # and no Binaries, and UnrealEditor-Cmd against a project with no built
    # module silently falls back to a stock engine world: terrain everywhere,
    # no voxels, no water, and a screenshot that is a photograph of nothing to
    # do with this project. Point this at the checkout that was actually built,
    # and say in the writeup which commit that build came from.
    [string]$ProjectPath = ''
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = if ($ProjectPath) { (Resolve-Path $ProjectPath).Path }
           else { (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path }
$ProjRoot = Split-Path $Project
$LogPath = Join-Path $Root "Saved\capture-$Name.log"
$ShotDir = Join-Path $ProjRoot 'Saved\Screenshots\WindowsEditor'
if (-not (Test-Path (Join-Path $ProjRoot 'Binaries\Win64\UnrealEditor-VoxelEarth.dll'))) {
    throw ("REFUSING TO START: $ProjRoot has no built UnrealEditor-VoxelEarth.dll. " +
           "A capture from an unbuilt project photographs a stock engine world. " +
           "Pass -ProjectPath at the checkout that was built.")
}

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
#
# AND THE WATER BLOB, WHICH THIS BLOCK DID NOT CLEAR AND WHICH IS THE SAME BUG.
# Persisted world state is TWO files, not one (tools/water-playtest.ps1 has
# always cleared both, and says why): the `.vxlog` edit log replayed on load,
# and the ADR-0005 `.vxwater` CA fill blob, which is irreducible simulation
# state and therefore NOT re-derivable from seed + edit log. Only the first was
# cleared here.
#
# Measured on 2026-08-04, on the first water capture ever taken through this
# script: a breach run left 4.1 MB of `.vxwater` behind, and the NEXT run --
# nominally a fresh baseline -- logged `LoadWaterState: restored 886,179,570
# fill units across 38,712 stored brick(s), 17,235 mobilized brick(s)` before
# its own dig had happened. It came up already flooded.
#
# That is worse here than a stale measurement, for the reason the edit-log note
# above gives about the camera: it silently destroys an A/B. The
# `voxel.Water.ImplicitOcean 0` control for a breach capture is a run whose
# whole content is "no water appears" -- and run for run after the ocean-on
# arm, it would have inherited the ocean-on arm's water and photographed it.
# The pair would have looked like a null result and would have been evidence of
# nothing.
if (-not $KeepEditLog) {
    $worldDir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
    if (Test-Path $worldDir) {
        $stale = @(Get-ChildItem $worldDir -Include *.vxlog, *.vxwater -File -Recurse -ErrorAction SilentlyContinue)
        if ($stale.Count -gt 0) {
            # Say what was discarded. "Discarded 4.1 MB of water blob" is the line
            # that tells you the previous run actually poured something, and it is
            # the only warning that a comparison you were about to make had a
            # contaminated baseline.
            $what = ($stale | ForEach-Object { "{0} ({1:N0} B)" -f $_.Name, $_.Length }) -join ', '
            Write-Host "  cleared persisted world state: $what" -ForegroundColor DarkGray
            $stale | Remove-Item -Force
        }
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
# 45 is the engine-side default for the capture framing, so passing it would be
# a no-op that changes the command line for nothing -- same "an unchanged
# command line is what makes it checkable" argument as the block above.
if ($SpawnYaw -ne 45) {
    $argList += "-VoxelSpawnYaw=$($SpawnYaw.ToString([cultureinfo]::InvariantCulture))"
}
if ($Cvars) { $argList += "-ExecCmds=`"$Cvars`"" }   # embed the quotes; see the leg script
if ($FineTileDir)    { $argList += "-VoxelFineTileDir=$FineTileDir" }
if ($FineProviderId) { $argList += "-VoxelFineTileProviderId=$FineProviderId" }
if ($CoarseTileDir)  { $argList += "-VoxelTileDir=$CoarseTileDir" }
if ($NoRiverRibbons) { $argList += '-VoxelRiverRibbons=0' }
$argList += $ExtraArgs

$sun = if ($TimeScale -eq 0) { "sun frozen $TimeOfDay $Date" } else { "sun MOVING x$TimeScale from $TimeOfDay $Date -- NOT reproducible" }
$pose = if ($SpawnAltM -ne 0 -or $SpawnPitch -ne 0) { "alt +${SpawnAltM}m pitch ${SpawnPitch}deg" } else { "GROUND spawn, level camera (landform will NOT read)" }
$arm  = if ($NoRiverRibbons) { 'CONTROL (-VoxelRiverRibbons=0)' } else { 'river ribbons ON' }
Write-Host "capture '$Name' at $SpawnAt, $pose, yaw $SpawnYaw, settling ${SettleSec}s, ${Width}x${Height}, $sun, $arm" -ForegroundColor Cyan
if ($FineProviderId) { Write-Host "  fine tier: $FineTileDir  provider=$FineProviderId" -ForegroundColor DarkGray }
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
    # THE WATER QUEUE, WHICH THE SETTLE CHECK ABOVE CANNOT SEE.
    #
    # jobsInFlight/pendingJobs are TERRAIN counters. On 2026-08-03 two lake
    # captures were shot 8 and 36 ticks into a 199-tick water refresh with
    # `jobsInFlight=0 pendingJobs=0 unloaded=0` -- genuinely terrain-settled and
    # genuinely water-unsettled. The frames showed a lake as disjoint
    # brick-shaped patches, which reads as a meshing bug and sent a whole
    # investigation after "data or mesh?" when the answer was neither: the gaps
    # were wherever the water queue had not reached yet.
    #
    # So a capture CONTAINING WATER is not settled until the implicit-water
    # rebuild has drained, and that has its own line. Expect to add another
    # block here the next time a subsystem starts producing geometry: this
    # check only knows about the queues it has been taught.
    $drained = @(Select-String -Path $LogPath -Pattern 'RefreshImplicitWater: DRAINED refresh')
    $draining = @(Select-String -Path $LogPath -Pattern 'RefreshImplicitWater: STILL DRAINING') |
                Select-Object -Last 1
    $sheet = @(Select-String -Path $LogPath -Pattern 'Lake sheets: DRAINED build')
    Write-Host ("  water: implicit DRAINED x{0}, sheet DRAINED x{1}" -f
                $drained.Count, $sheet.Count)
    if ($drained.Count -eq 0) {
        if ($draining) {
            Write-Warning ("WATER NOT SETTLED at the shutter: the implicit-water refresh was " +
                           "STILL DRAINING and never reported DRAINED. Water in this frame is " +
                           "PARTIAL -- missing patches are unreached queue, not missing data " +
                           "and not a mesher bug. Raise -SettleSec and re-shoot.")
        }
        else {
            Write-Warning ("no 'RefreshImplicitWater: DRAINED refresh' line in the log. Either " +
                           "there is no water near this spawn (fine), or the water subsystem " +
                           "never ran (not fine). This capture is NOT evidence that water is " +
                           "absent -- check the 'Baked water tier ENABLED' line and the tile " +
                           "provider id before concluding anything from an empty valley.")
        }
    }
    # A REFUSED TILE HAS NO WATER AND LOOKS EXACTLY LIKE A DRY VALLEY. Most of
    # the production cache is CODEC_ZSTD, and without the injected decompressor
    # every one of those tiles is refused whole -- silently, as far as the image
    # is concerned.
    $refused = @(Select-String -Path $LogPath -Pattern 'was REFUSED')
    if ($refused.Count -gt 0) {
        Write-Warning ("$($refused.Count) fine tile(s) were REFUSED (search the log for " +
                       "'was REFUSED'). Their lakes and rivers are ABSENT from this frame. " +
                       "kNoDecompressor here means the runtime zstd DLL is missing -- see " +
                       "tools/fetch-zstd.ps1.")
    }

    # THE RIBBON QUEUE, WHICH IS A THIRD WATER QUEUE AND THE ONLY ONE THAT
    # MATTERS ABOVE ~13 m.
    #
    # The implicit-water check above reads RefreshImplicitWater, which meshes a
    # disc of +-25.6 m in xy and +-12.8 m in z about the CAMERA BRICK
    # (VoxelWaterSubsystem.cpp:3005-3009). Every river in this world is below
    # its own valley floor, so from any altitude worth photographing landform
    # from, that disc is empty air and the implicit line reports `0 candidate
    # brick(s)` -- correctly. Its ABSENCE at altitude is therefore expected and
    # is NOT evidence that water failed to settle. Treating it as a settle check
    # everywhere would condemn every altitude capture ever taken.
    #
    # What draws the river from altitude is the far-field ribbon actor, and it
    # has its own drain line, its own failure modes, and no altitude gate. It
    # meshes AT MOST ONE REACH PER TICK (VoxelRiverRibbonActor.cpp:598-621), so
    # a scene with many reaches needs many ticks after the window fill -- which
    # is exactly the shape of the lake-sheet bug that produced disjoint
    # brick-shaped patches and sent an investigation after a mesher that was
    # fine. A capture whose river is in frame is not settled until this drains.
    $ribDrained = @(Select-String -Path $LogPath -Pattern 'River ribbons: DRAINED build') |
                  Select-Object -Last 1
    $ribOff  = @(Select-String -Path $LogPath -SimpleMatch 'DISABLED (-VoxelRiverRibbons=0)')
    $ribNoTier = @(Select-String -Path $LogPath -SimpleMatch 'River ribbons: no fine water tier')
    if ($ribDrained) {
        Write-Host ("  ribbons: " + ($ribDrained.Line -replace '^.*River ribbons: ', ''))
    }
    elseif ($ribOff.Count -gt 0) {
        Write-Host "  ribbons: DISABLED -- this is the -VoxelRiverRibbons=0 CONTROL arm" -ForegroundColor DarkGray
    }
    elseif ($ribNoTier.Count -gt 0) {
        Write-Warning ("River ribbons found NO FINE WATER TIER. There is no baked river in this " +
                       "frame at all, and an empty valley here is the tile source being wrong, " +
                       "not the bake. Check -FineTileDir / -FineProviderId against the " +
                       "'Baked water tier ENABLED' line.")
    }
    else {
        Write-Warning ("no 'River ribbons: DRAINED build' line and the actor was not disabled -- " +
                       "the far-field river had NOT finished meshing at the shutter, or the actor " +
                       "never ran. River geometry in this frame is PARTIAL. Raise -SettleSec.")
    }
    # A ribbon build that drained with zero reaches is a legitimately dry frame
    # OR a pose with no river in range; either way it is not evidence of water.
    if ($ribDrained -and $ribDrained.Line -match '(\d+) reach\(es\), (\d+) quad\(s\)') {
        if ([int]$Matches[1] -eq 0 -or [int]$Matches[2] -eq 0) {
            Write-Warning ("the ribbon build DRAINED with $($Matches[1]) reach(es) and " +
                           "$($Matches[2]) quad(s) -- no far-field river was drawn here. This " +
                           "capture is not evidence about river extent.")
        }
    }

    # FABRICATED GROUND. A fine-tier elevation query that lands in a
    # non-resident tile is answered with SEA LEVEL -- terrain no other client
    # computes, in frame, in a capture that looks entirely normal. Measured:
    # a pose 4,073 m from the edge of a four-tile set produced 2,898 of these,
    # and the capture was discarded. Under -unattended the engine makes the
    # first one fatal (VoxelFineTileStreamer.cpp:543), which this script passes
    # on line 174 -- so in the normal case the run dies rather than lying. Grep
    # for it anyway: the policy is overridable, the non-fatal and aggregate
    # variants exist, and a capture that reached the shutter with any of these
    # in its log is not evidence about anything.
    $leaks = @(Select-String -Path $LogPath -SimpleMatch 'FINE TIER GATE LEAK')
    if ($leaks.Count -gt 0) {
        Write-Warning ("$($leaks.Count) FINE TIER GATE LEAK line(s) -- elevation queries landed " +
                       "outside baked coverage and were answered at SEA LEVEL. There is " +
                       "FABRICATED GROUND in this frame. Discard it and move the pose further " +
                       "inside coverage (the working margin is 4.5 km).")
    }
    # THE TIER THE FRAME WAS ACTUALLY DRAWN FROM, read back rather than echoed
    # from the switches -- same argument as the sky and pose lines below. The
    # water subsystem resolves the fine root and provider id INDEPENDENTLY of
    # the world subsystem (VoxelWaterSubsystem.cpp:460-492), so the two can
    # disagree, and a frame with the right terrain and the wrong water is the
    # exact failure this whole capture exists to rule out.
    foreach ($pat in @('Fine tier ENABLED:', 'Baked water tier ENABLED:')) {
        $ln = @(Select-String -Path $LogPath -SimpleMatch $pat) | Select-Object -First 1
        if ($ln) { Write-Host ("  " + ($ln.Line -replace '^.*?(?=' + [regex]::Escape($pat) + ')', '')) }
        else     { Write-Warning "no '$pat' line -- that tier did not initialise." }
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

    # THE POSE AT THE SHUTTER, WHICH IS THE ONLY POSE THAT FRAMED ANYTHING, AND
    # WHICH DISAGREED WITH THE LINE ABOVE FOR THE WHOLE LIFE OF -SpawnPitch.
    #
    # "Spawn pose APPLIED" is written at spawn and reads the control rotation
    # back off the controller, so it is honest about that instant -- and that
    # instant is ~150 s before the frame. The game mode's screenshot timer then
    # re-posed the camera to a hard-coded pitch -40 yaw 45 immediately before
    # requesting the shot, discarding -VoxelSpawnPitch entirely. Every capture
    # taken through this script without a fixture switch was framed at -40/45
    # while its log stated the requested pitch, and a -89 request and a -42
    # request produced the same picture. Three settled cave frames were binned
    # over it before anyone read the second line.
    #
    # The engine now honours the switch, but the lesson generalises past the one
    # bug: a pose that is accepted at spawn is not a pose that survives to the
    # shutter, and only the engine can say which one framed the image. So print
    # what the CAMERA MANAGER reported at the moment of the screenshot request,
    # and compare it against what was asked for rather than leaving that to
    # whoever opens the log.
    $shot1 = @(Select-String -Path $LogPath -Pattern 'Capture: cam loc=.*rot=\(pitch (-?[\d.]+) yaw (-?[\d.]+)\)') |
             Select-Object -First 1
    if ($shot1) {
        $gotPitch = [double]$shot1.Matches[0].Groups[1].Value
        $gotYaw   = [double]$shot1.Matches[0].Groups[2].Value
        Write-Host ("  SHUTTER pose: " + ($shot1.Line -replace '^.*Capture: ', ''))
        $wantPitch = if ($SpawnPitch -ne 0) { $SpawnPitch } else { -40 }
        # YAW WRAPS AND THIS CHECK DID NOT, so it cried wolf on the first capture
        # that used it: -SpawnYaw 270 reaches the shutter as -90, the engine's
        # normalised form of the same direction, and the comparison called a
        # correctly framed image "NOT AS REQUESTED". A false alarm on the one
        # warning that exists to make a real framing failure loud is worse than
        # no warning at all -- the next real one gets ignored. Compare the
        # signed shortest angular distance instead.
        $yawErr = [Math]::Abs((($gotYaw - $SpawnYaw) % 360 + 540) % 360 - 180)
        if ([Math]::Abs($gotPitch - $wantPitch) -gt 0.5 -or $yawErr -gt 0.5) {
            Write-Warning ("FRAMING NOT AS REQUESTED: asked for pitch $wantPitch yaw $SpawnYaw, the shutter " +
                           "fired at pitch $gotPitch yaw $gotYaw. This image is NOT the framing you asked for -- " +
                           "report it as unusable rather than as the requested pose. (pitch -40 yaw 45 exactly " +
                           "means the game mode's fallback framing overrode the switches; an editor built before " +
                           "2026-08-03 always does this.)")
        }
    } else {
        Write-Warning ("no 'Capture: cam loc=' line in the log -- the pose that actually framed this image " +
                       "could not be read. 'Spawn pose APPLIED' is NOT a substitute: it is written ~150 s " +
                       "earlier and has already been observed to disagree with the shutter.")
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

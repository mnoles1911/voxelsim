# Run one flight leg that captures BOTH the frame distribution and a mid-flight
# GPU pass breakdown, for A/B-ing render settings.
#
# WHY BOTH IN ONE LEG. A ProfileGPU capture is a single frame -- it gives the
# pass split but says nothing about the distribution, and its own frame may be
# atypical. The post-warmup p50/p95 line is the distribution but says nothing
# about which pass moved. Comparing a pass split from one leg against a p50 from
# another is how you conclude that a change helped when the two legs simply had
# different weather.
#
# The capture is deferred via voxel.DeferExec because -ExecCmds fires at startup
# and would profile frame 1 of an empty world -- which it reports as a
# successful capture. That has now cost three separate runs on this project.
#
# THE SKY PIN, AND WHY A SPLIT LEG NEEDS IT MORE THAN MOST. This leg compares
# TWO things taken at different instants of the SAME run -- the post-warmup
# p50/p95 distribution and a single deferred ProfileGPU frame at -CaptureAt --
# and the point above already warns against comparing a pass split from one
# leg against a distribution from another because they can be "different
# weather". A moving sun is exactly that failure arriving WITHIN one leg: by
# the time the deferred capture fires, the sun has rotated past wherever it
# was while the distribution was accumulating, so the two halves of this
# leg's own output would be describing different lighting.
#
# -TimeOfDay/-Date default to 12:00 / 03-20, the engine's own default pose
# (VoxelSkySubsystem.cpp:254-269), so this stays comparable to every leg taken
# before the clock existed. -TimeScale defaults to 0, departing from the
# engine's 1.0, because that is what keeps the distribution and the capture
# looking at the same sun.

param(
    [Parameter(Mandatory=$true)][string]$LogName,
    [string]$Cvars = '',
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$RunSec = 150,
    [int]$PreflightSec = 90,
    [int]$LingerSec = 10,
    # Seconds from start to the capture. Must land INSIDE the flight: preflight
    # plus roughly half the run.
    [int]$CaptureAt = 150,
    # THE SKY PINS. See the header. Same three defaults as
    # tools/voxel-run-flight-leg.ps1.
    [string]$TimeOfDay = '12:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0,
    # SPAWN AND FLIGHT ARE PARAMETERS NOW, AND THE DEFAULTS ARE THE OLD
    # HARDCODED VALUES so every leg taken before this change still reproduces
    # by name. They had to open up because the fine-tier tile cache no longer
    # covers -84480,53760: that spawn needs tile (-6,3) and the current seed's
    # namespace holds 15 tiles, all at negative Y. A leg there does not degrade,
    # it dies -- VoxelFineTileStreamer.cpp:891 is fatal on an unattended run,
    # by design, because answering sea level would make the run irreproducible.
    #
    # PICK A SPAWN FROM A BAKED TILE. -SpawnAt is in METRES (scaled by 100 into
    # UU: -61440 logs as -6144000). Fine pixels are 1.875 m and a tile is
    # 8192 px = 15.36 km, so ONE covered tile is far more than any leg needs --
    # a 120 s line flight at 20 m/s covers 2.4 km. Coverage is about picking a
    # baked tile, not about flight length.
    #
    # Prefer 'static' for A/B work on the DRAW path: it pins position AND
    # rotation and logs the pose (VoxelPerfRunSubsystem), which removes
    # streaming variance instead of averaging over it. That mode exists because
    # unpinned "settled" legs produced 43 fps and 103 fps on identical scenes.
    [string]$SpawnAt = '-84480,53760',
    [ValidateSet('line','surface','underground','static')][string]$Flight = 'line',
    # EXTRA COMMAND-LINE ARGS, for the settings that are NOT cvars.
    # -VoxelMaxRingLevel=, -VoxelRingInnerMeters=, -VoxelRingOuterMeters= and
    # -VoxelPoolCapacityQuads= are read once, before the first
    # RecomputeDesiredSet, so they cannot be cvars and cannot be passed through
    # -Cvars. Ring composition in particular is the thing an ablation most wants
    # to vary, and until this existed it could not be swept by this driver at all.
    [string[]]$ExtraArgs = @(),
    # See THE MARCHER HOOKUP GUARD below. Suppresses the refusal, never the reason.
    [switch]$NoMarcherHookupGuard
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path (Resolve-Path "$PSScriptRoot\..").Path "Saved\$LogName.log"

# ---------------------------------------------------------------------------
# THE SHADER TREE MUST NOT BE NEWER THAN THE BINARY THAT READS IT.
#
# Global shaders compile at BOOT from whatever .usf/.ush is on disk, while the
# parameter structures they bind against live in the built DLL. So an agent
# editing a shader while a leg is queued does not produce a slow leg or a wrong
# leg -- it produces a leg running a DIFFERENT BUILD OF THE RENDERER than the
# one that was intended, and there is nothing in the log that says so.
#
# MEASURED, 2026-08-21: three legs of a four-leg budget sweep died at boot with
#   VoxelBrickTraverse.ush(1090): Shader parameter MarchRing0OuterUU could not
#   be bound to FVoxelMarchCS's shader parameter structure
# because six-ring work landed new shader parameters after the DLL was built.
# Cost was small ONLY because the mismatch happened to be a binding error that
# fails fast. THE DANGEROUS CASE IS THE ONE THAT BINDS: a shader edit whose
# parameters still resolve, running against C++ semantics that have moved,
# produces a complete, plausible, silently-wrong leg. That is the case this
# guard exists for; the crash was luck, not detection.
#
# Compares the newest mtime across BOTH shader trees against the module DLL.
$repoRoot    = (Resolve-Path "$PSScriptRoot\..").Path
$shaderDirs  = @((Join-Path $repoRoot 'ue-project\Shaders'),
                 (Join-Path $repoRoot 'voxel-core\shaders')) | Where-Object { Test-Path $_ }
$shaderDll   = Join-Path $repoRoot 'ue-project\Binaries\Win64\UnrealEditor-VoxelEarthShaders.dll'
# THE STAMP EXISTS BECAUSE THE DLL IS NOT ALWAYS TOUCHED BY A BUILD.
# A shader-only change -- a NEW .usf that no C++ references yet, for instance --
# leaves the linker with nothing to do, so the DLL keeps its old mtime and this
# guard would refuse every leg forever, no matter how many times you build.
# tools/voxel-stamp-build.ps1 writes the stamp after a build that SUCCEEDED, so
# the reference time is "when the tree was last known good", which is what the
# guard actually means. Whichever of the two is newer wins.
$buildStamp  = Join-Path $repoRoot 'Saved\.shader-build-stamp'
if ((Test-Path $shaderDll) -and $shaderDirs.Count) {
    $newestShader = Get-ChildItem -Path $shaderDirs -Recurse -Include *.usf,*.ush -ErrorAction SilentlyContinue |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $dllTime = (Get-Item $shaderDll).LastWriteTime
    if (Test-Path $buildStamp) {
        $stampTime = (Get-Item $buildStamp).LastWriteTime
        if ($stampTime -gt $dllTime) { $dllTime = $stampTime }
    }
    if ($newestShader -and $newestShader.LastWriteTime -gt $dllTime) {
        $skew = [int]($newestShader.LastWriteTime - $dllTime).TotalSeconds
        Write-Host "  ${LogName}: REFUSED -- SHADER TREE IS NEWER THAN THE BINARY" -ForegroundColor Red
        Write-Host "     $($newestShader.Name) is ${skew}s newer than the last good build." -ForegroundColor Red
        Write-Host "     Shaders compile at boot from disk; parameter structures come from the DLL." -ForegroundColor Red
        Write-Host "     This leg would measure a renderer that was never built. Build first." -ForegroundColor Red
        return $false
    }
}


# ---------------------------------------------------------------------------
# THE THIRD ARTIFACT: voxelcore.lib, WHICH UBT DOES NOT TRACK.
#
# Ported from voxel-capture.ps1, which already had it. This harness knew about
# two artifacts -- the shader tree and the module DLL -- and there are three.
# `Build.bat` reports `Result: Succeeded` while linking a voxelcore.lib from
# hours earlier, because UBT sees no changed input under voxel-core/.
#
# COST, 2026-08-21: an hour of legs and two wrong conclusions, including my own
# claim that a header-only change had been missing from the engine. Note the
# trap has TWO halves and both are checked below -- a stale lib, and a lib that
# was rebuilt and never linked, which is what you create by fixing the first
# half in the wrong order.
$VoxelCoreRoot = Join-Path $repoRoot 'voxel-core'
$VoxelCoreLib = @(
    (Join-Path $repoRoot 'build\voxel-core-msvc\Release\voxelcore.lib'),
    (Join-Path $repoRoot 'build\voxel-core-msvc\voxelcore.lib')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($VoxelCoreLib -and (Test-Path $VoxelCoreRoot)) {
    $libTime = (Get-Item $VoxelCoreLib).LastWriteTimeUtc
    # src AND include ONLY, matching VoxelEarth.Build.cs's own staleness scan.
    # tests/ and bench/ do NOT build into voxelcore.lib -- they are their own
    # targets -- so scanning the whole tree refuses a capture whenever a test
    # file was touched, which is a false alarm that trains people to ignore the
    # guard. Measured the day this was written: editing test_amplifier.cpp
    # refused a capture whose generator was perfectly current.
    $scanDirs = @((Join-Path $VoxelCoreRoot 'src'), (Join-Path $VoxelCoreRoot 'include')) |
                Where-Object { Test-Path $_ }
    $newest = Get-ChildItem $scanDirs -Recurse -Include *.cpp, *.h, *.hpp, *.inl -File `
              -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending |
              Select-Object -First 1
    if ($newest -and $newest.LastWriteTimeUtc -gt $libTime) {
        throw ("REFUSING TO START: voxelcore.lib is STALE (lib $libTime UTC, newest source " +
               "$($newest.LastWriteTimeUtc) UTC -- $($newest.Name)). This capture would " +
               "photograph an OUT-OF-DATE GENERATOR while every log line looks healthy, and " +
               "Build.bat will still say 'Result: Succeeded' because UBT does not track " +
               "voxel-core's sources. Rebuild it:`n" +
               "  cmake --build build/voxel-core-msvc --config Release`n" +
               "Then force a RELINK -- rebuilding the lib alone does not trigger one:`n" +
               "  touch a file under ue-project/Source/VoxelEarth, then re-run Build.bat")
    }
    $dll = Join-Path (Join-Path $repoRoot 'ue-project') 'Binaries\Win64\UnrealEditor-VoxelEarth.dll'
    if ((Get-Item $dll).LastWriteTimeUtc -lt $libTime) {
        throw ("REFUSING TO START: UnrealEditor-VoxelEarth.dll is OLDER than voxelcore.lib " +
               "(dll $((Get-Item $dll).LastWriteTimeUtc) UTC, lib $libTime UTC), so the lib was " +
               "rebuilt and never linked in. UBT sees no changed input and will not relink on " +
               "its own. Touch a file under ue-project/Source/VoxelEarth and re-run Build.bat.")
    }
}


$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail. See voxel-run-flight-leg.ps1."
}

# ---------------------------------------------------------------------------
# THE MARCHER HOOKUP GUARD -- RETIRED 2026-08-22, AND THE REASON IT EXISTED IS GONE
#
# This used to REFUSE any leg that set voxel.March without also spawning fluid,
# because the marcher's game-thread hookup rode the fluid subsystem's tick:
# voxel.Fluid.Enable alone left the extension NEVER CREATED, and the leg reported
# frames=0 with every decline counter at zero -- which reads like an idle marcher
# and is not one. Three legs were lost to that in one night, the third after the
# cause was diagnosed, which is why it became a guard rather than a note.
#
# THE COUPLING IS NOW FIXED, not merely worked around. VoxelMarchEnsureExtension
# is called by UVoxelWorldSubsystem -- the owner of the brick pool the marcher
# reads -- and the occupancy volume is only required by source 0.
#
# PROVEN, not assumed (capture-marcher-nofluid, 2026-08-22): voxel.Fluid.Enable 0
# with no spawn renders terrain, frames=23145 == emitFrames, every decline counter
# zero, indexEntries=98442, ZERO "Fluid perf [run]" lines in the whole log, and an
# image indistinguishable from the fluids-on arm at the same pose (mean
# 148.5/144.1/138.8 against 148.5/144.2/138.9).
#
# Keeping the guard would now REFUSE the correct configuration, which is worse
# than the failure it was written for. -NoMarcherHookupGuard is retained as an
# accepted no-op so existing call sites do not break.


$dir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
if (Test-Path $dir) { Get-ChildItem $dir -Filter *.vxlog -EA SilentlyContinue | Remove-Item -Force }

$exec = "voxel.Stream.CoverageVerify 1, r.ProfileGPU.ShowUI 0"
if ($Cvars) { $exec = "$exec, $Cvars" }
$exec = "$exec, voxel.DeferExec $CaptureAt ProfileGPU"

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',
    "-abslog=`"$LogPath`"", "-ResX=$Width", "-ResY=$Height",
    "-VoxelSpawnAt=$SpawnAt",
    # InvariantCulture on the scale -- see tools/voxel-run-flight-leg.ps1's note
    # on why a comma-decimal machine silently truncates this otherwise.
    "-VoxelTimeOfDay=$TimeOfDay",
    "-VoxelDate=$Date",
    "-VoxelTimeScale=$($TimeScale.ToString([cultureinfo]::InvariantCulture))",
    "-VoxelPerfRun=$RunSec", "-VoxelPerfFlight=$Flight",
    "-VoxelPerfPreflightSec=$PreflightSec", "-VoxelPerfLingerSec=$LingerSec",
    '-VoxelPerfLogInterval=5',
    "-ExecCmds=`"$exec`""
)
if ($ExtraArgs.Count) { $argList += $ExtraArgs }

$started = Get-Date
$p = Start-Process -FilePath 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' -PassThru -WindowStyle Hidden -ArgumentList $argList
$p.WaitForExit(600000) | Out-Null
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "  ${LogName}: KILLED -- VOID" -ForegroundColor Yellow; return $false }

$elapsed = [int]((Get-Date) - $started).TotalSeconds
$expected = $PreflightSec + $RunSec + $LingerSec
if ($elapsed -lt ($expected * 0.9)) {
    Write-Host "  ${LogName}: exited after ${elapsed}s against ~${expected}s -- VOID" -ForegroundColor Yellow
    return $false
}
if (-not (Select-String -Path $LogPath -SimpleMatch 'DeferExec: running now' -Quiet)) {
    Write-Host "  ${LogName}: the deferred capture never fired -- no pass breakdown" -ForegroundColor Yellow
}
Write-Host "  ${LogName}: ok (${elapsed}s) at ${Width}x${Height}" -ForegroundColor Green
return $true

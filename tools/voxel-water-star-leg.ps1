# One PINNED NIGHT leg over water, for the reflected-stars A/B.
#
# WHAT MAKES THIS DIFFERENT FROM tools/voxel-run-gpu-arm.ps1, which is the
# established GPU-arm driver: that one flies a LINE. A line flight admits virgin
# terrain for the whole run, which is the right shape for a streaming or meshing
# question and the wrong shape for a SHADER question -- the amount of water on
# screen would change every second, and the cost of a water shader is very nearly
# proportional to the screen area the water covers. Two line legs would differ by
# however much water each happened to fly over.
#
# -VoxelPerfFlight=static pins position AND rotation and re-asserts them every
# tick (an unattended pawn really does drift: one run in this project travelled
# 254 m unprompted). So the water coverage is fixed, the sun is frozen, and the
# shader is the only thing that differs between the two arms.
#
# THE SUN IS PINNED AT NIGHT AND THAT IS THE POINT. -VoxelTimeScale=0 with
# -VoxelTimeOfDay=23:00. The star branch is gated by StarBrightness, which C++
# drives from solar altitude, so a leg at noon would measure the ON arm with its
# star term multiplied by zero -- still executing every instruction, but invisible
# and impossible to sanity-check from a screenshot.
#
# NO ProfileGPU HERE, unlike voxel-run-gpu-arm.ps1. A deferred capture costs one
# very long frame; against a few hundred post-warmup frames that frame lands in
# the p95 bucket, which is the exact statistic this A/B is being read on.

param(
    [Parameter(Mandatory=$true)][string]$LogName,
    # X,Y in UU -- the column the world streams around.
    [Parameter(Mandatory=$true)][string]$SpawnAt,
    # X,Y,Z in UU -- where the camera is actually pinned. Passed to
    # -VoxelPerfStaticAt. Without it the fixture pins wherever the pawn spawned,
    # which is always grounded on the surface column.
    [Parameter(Mandatory=$true)][string]$StaticAt,
    [Parameter(Mandatory=$true)][double]$Yaw,
    [Parameter(Mandatory=$true)][double]$Pitch,
    # METRES above the walkable ground at the spawn column, for the PREFLIGHT
    # pose only. Set it to match the -StaticAt height.
    #
    # WHY IT MATTERS AT ALL, given -StaticAt pins the camera anyway: the static
    # pin does not engage until preflight ENDS (UVoxelPerfRunSubsystem returns
    # from the preflight gate before it reaches the pinning branch). So the whole
    # settle period happens at the SPAWN pose, and the spawn pose is grounded on
    # the highest solid voxel plus 5 m -- which at this lake is the BED, putting
    # the camera about a metre UNDER the water surface for two minutes and then
    # popping it out at t=preflight. The world would have spent its settle
    # streaming for one place and then measured another.
    #
    # Matching them means the pin is a few centimetres of correction rather than
    # a teleport, so the resident set the run measures is the one it settled.
    [double]$SpawnAltM = 0,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$RunSec = 150,
    # Long enough for the lake sheets and the near-field water to be resident
    # before a single measured frame is taken.
    [int]$PreflightSec = 120,
    [int]$LingerSec = 10,
    [string]$TimeOfDay = '23:00',
    [string]$Date = '03-20',
    [double]$TimeScale = 0,
    [string]$Cvars = '',
    # SCREENSHOT MODE. Non-zero turns this into a picture run instead of a
    # measurement run: -VoxelScreenshotAfter fires at this many seconds and then
    # QUITS, so no perf summary is produced and none is demanded below.
    #
    # It reuses the same static pin as the measurement legs ON PURPOSE. The owner
    # judges this feature from screenshots, and a screenshot taken at a pose that
    # merely resembles the measured one invites the two to be read together when
    # they are not the same view. Same switch, same numbers, same frame.
    #
    # This is also why the picture is a SEPARATE RUN rather than a screenshot
    # folded into a measurement leg: a capture costs one very long frame, and in
    # a measurement leg that frame lands in the p95 bucket being reported.
    [int]$ScreenshotAfterSec = 0,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path $Root "Saved\$LogName.log"

$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail."
}

$inv = [cultureinfo]::InvariantCulture

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',
    "-abslog=`"$LogPath`"", "-ResX=$Width", "-ResY=$Height",
    "-VoxelSpawnAt=$SpawnAt",
    # InvariantCulture on every double: on a comma-decimal machine PowerShell
    # would interpolate "0,5" and FParse::Value stops at the comma, silently
    # running a different value than the one printed here.
    "-VoxelTimeOfDay=$TimeOfDay",
    "-VoxelDate=$Date",
    "-VoxelTimeScale=$($TimeScale.ToString($inv))",
    "-VoxelPerfRun=$RunSec", '-VoxelPerfFlight=static',
    "-VoxelPerfStaticAt=$StaticAt",
    "-VoxelPerfYaw=$($Yaw.ToString($inv))",
    "-VoxelPerfPitch=$($Pitch.ToString($inv))",
    "-VoxelPerfPreflightSec=$PreflightSec", "-VoxelPerfLingerSec=$LingerSec",
    '-VoxelPerfLogInterval=5',
    # MCP off for measurement legs -- see tools/voxel-run-leg.ps1 for why.
    '-ini:EditorPerProjectUserSettings:[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]:bAutoStartServer=False'
)
if ($SpawnAltM -gt 0) { $argList += "-VoxelSpawnAltM=$($SpawnAltM.ToString($inv))" }
if ($Cvars) { $argList += "-ExecCmds=`"$Cvars`"" }

$ShotDir = Join-Path (Split-Path $Project) 'Saved\Screenshots\WindowsEditor'
$shotsBefore = @()
if ($ScreenshotAfterSec -gt 0) {
    $argList += "-VoxelScreenshotAfter=$ScreenshotAfterSec"
    if (Test-Path $ShotDir) { $shotsBefore = @(Get-ChildItem $ShotDir -Filter *.png -File | ForEach-Object { $_.FullName }) }
}

$expected = $PreflightSec + $RunSec + $LingerSec
$started = Get-Date
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList $argList
# Generous: this box takes minutes to reach the first frame.
$p.WaitForExit(1800000) | Out-Null
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "${LogName}: KILLED at the deadline -- VOID" }
$elapsed = [int]((Get-Date) - $started).TotalSeconds

# --- THE RAN-FLAG -------------------------------------------------------------
#
# Project rule: a stage must write something that distinguishes "ran and found
# nothing" from "did not run". For a perf leg the distinguishing artefact is the
# completion line. NO perf lines means NOT RUN. It does NOT mean the arm was free.
$complete = Select-String -Path $LogPath -Pattern 'VoxelPerfRun complete:' -ErrorAction SilentlyContinue | Select-Object -Last 1
$post     = Select-String -Path $LogPath -Pattern 'VoxelPerfRun post-warmup' -ErrorAction SilentlyContinue | Select-Object -Last 1
$pinned   = Select-String -Path $LogPath -Pattern 'STATIC pose pinned' -ErrorAction SilentlyContinue | Select-Object -Last 1
$sky      = Select-String -Path $LogPath -Pattern 'VoxelPerfRun sky:' -ErrorAction SilentlyContinue | Select-Object -Last 1

if ($ScreenshotAfterSec -gt 0) {
    # A screenshot leg QUITS at the shutter, so there is no perf summary to
    # demand. Its ran-flag is the PNG, and it is checked by comparing the
    # directory against the listing taken before launch -- "a file exists" would
    # pass on the previous leg's picture, which is the same class of mistake as
    # reading a stale log.
    $after = @()
    if (Test-Path $ShotDir) { $after = @(Get-ChildItem $ShotDir -Filter *.png -File | ForEach-Object { $_.FullName }) }
    $new = @($after | Where-Object { $shotsBefore -notcontains $_ })
    if ($new.Count -eq 0) {
        throw ("${LogName}: NO NEW SCREENSHOT in $ShotDir after ${elapsed}s. The shutter never fired. " +
               "This is a hard failure, not an empty result. Log: $LogPath")
    }
    foreach ($f in $new) {
        $dest = Join-Path (Split-Path $LogPath) "$LogName.png"
        Copy-Item $f $dest -Force
        Write-Host "  screenshot: $dest  (engine wrote $(Split-Path $f -Leaf))" -ForegroundColor Green
    }
}
elseif (-not $complete -or -not $post) {
    throw ("${LogName}: NO PERF LINES after ${elapsed}s (expected ~${expected}s). This leg DID NOT RUN. " +
           "Treat it as a hard failure -- it is not evidence that anything was cheap. Log: $LogPath")
}

# --- THE THING THAT WOULD MAKE THE WHOLE A/B MEANINGLESS ----------------------
$compileFail = @(Select-String -Path $LogPath -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
if ($compileFail.Count -gt 0) {
    $compileFail | Select-Object -First 6 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
    throw ("${LogName}: $($compileFail.Count) 'Failed to compile Material' lines. A material that fails to " +
           "compile is replaced by the engine DEFAULT, which renders and is FAST -- this leg's numbers are void.")
}

# The water material specifically: if it fell back to the default, the leg was
# not measuring the water shader at all.
$defaultMat = @(Select-String -Path $LogPath -SimpleMatch 'M_WaterVoxel not found' -ErrorAction SilentlyContinue)
if ($defaultMat.Count -gt 0) { throw "${LogName}: M_WaterVoxel not found -- water drew with the engine default. VOID." }

Write-Host "  $LogName  (${elapsed}s wall, expected ~${expected}s)" -ForegroundColor Green
foreach ($l in @($pinned, $sky, $complete, $post)) {
    if ($l) { Write-Host "    $($l.Line -replace '^.*?LogVoxelPerf: ','')" }
}

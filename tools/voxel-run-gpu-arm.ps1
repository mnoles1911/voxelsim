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
    [double]$TimeScale = 0
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path
$LogPath = Join-Path (Resolve-Path "$PSScriptRoot\..").Path "Saved\$LogName.log"

$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail. See voxel-run-flight-leg.ps1."
}

$dir = Join-Path (Split-Path $Project) 'Saved\VoxelWorlds'
if (Test-Path $dir) { Get-ChildItem $dir -Filter *.vxlog -EA SilentlyContinue | Remove-Item -Force }

$exec = "voxel.Stream.CoverageVerify 1, r.ProfileGPU.ShowUI 0"
if ($Cvars) { $exec = "$exec, $Cvars" }
$exec = "$exec, voxel.DeferExec $CaptureAt ProfileGPU"

$argList = @(
    "`"$Project`"", '-game', '-nosplash', '-unattended', '-sm6', '-dx12',
    "-abslog=`"$LogPath`"", "-ResX=$Width", "-ResY=$Height",
    '-VoxelSpawnAt=-84480,53760',
    # InvariantCulture on the scale -- see tools/voxel-run-flight-leg.ps1's note
    # on why a comma-decimal machine silently truncates this otherwise.
    "-VoxelTimeOfDay=$TimeOfDay",
    "-VoxelDate=$Date",
    "-VoxelTimeScale=$($TimeScale.ToString([cultureinfo]::InvariantCulture))",
    "-VoxelPerfRun=$RunSec", '-VoxelPerfFlight=line',
    "-VoxelPerfPreflightSec=$PreflightSec", "-VoxelPerfLingerSec=$LingerSec",
    '-VoxelPerfLogInterval=5',
    "-ExecCmds=`"$exec`""
)

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

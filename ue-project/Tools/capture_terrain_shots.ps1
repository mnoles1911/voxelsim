# Repeatable before/after terrain captures from fixed cameras.
#
# The whole point of this file is that a "before" and an "after" shot are taken
# with byte-identical arguments -- an appearance change judged from two
# differently-framed screenshots is worth nothing. It also echoes the tile-load
# line, because a wrong -VoxelSeed/-VoxelTileScale silently falls back to the
# synthetic sampler and produces a plausible-looking screenshot of the wrong
# world.
#
#   ./capture_terrain_shots.ps1 -Tag before-ground -Camera ground
#   ./capture_terrain_shots.ps1 -Tag after-vista   -Camera vista
#   ./capture_terrain_shots.ps1 -Tag after-under   -Camera underground
#
# Output lands in ue-project/Saved/Captures/<tag>-<n>.png. The harness fires
# twice (a framing pass then the real shot), so -1 is the one to look at.

param(
  [Parameter(Mandatory = $true)][string]$Tag,
  [ValidateSet("ground", "vista", "underground")][string]$Camera = "ground",
  [int]$ScreenshotAfter = 50,
  [int]$TimeoutSec = 360,
  [string]$Engine = "D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
)

$ErrorActionPreference = "Stop"

# NOT D:\UE5\UE_5.7 -- that engine cannot load this project's 5.8 assets and
# silently falls back to DEFAULT materials, which looks exactly like a material
# bug and has cost hours. If a run comes back grey, check this first.
if (-not (Test-Path $Engine)) { throw "engine not found: $Engine (expected UE 5.8)" }

$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Proj = Join-Path $Root "ue-project\VoxelEarth.uproject"
$ShotDir = Join-Path $Root "ue-project\Saved\Screenshots\WindowsEditor"
$OutDir = Join-Path $Root "ue-project\Saved\Captures"
$LogFile = Join-Path $Root "ue-project\Saved\Logs\VoxelEarth.log"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (Test-Path $ShotDir) { Remove-Item "$ShotDir\*.png" -Force -ErrorAction SilentlyContinue }

# The reproduce recipe from the task. x=[-8,-4] y=[1,5]; outside that box the
# synthetic sampler takes over, so "loaded=25 rejected=0" below is load-bearing.
$Common = @(
  "-game", "-windowed", "-resx=1920", "-resy=1080", "-nosplash", "-unattended",
  "-VoxelSeed=20260719",
  "-VoxelTileDir=`"D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-3e11cf157a836c70\000000000135276f\s1`"",
  "-VoxelSpawnAt=-66240,67200",
  "-VoxelScreenshotAfter=$ScreenshotAfter"
)

$PerCamera = switch ($Camera) {
  "ground"      { @() }
  "vista"       { @("-VoxelVistaShot=1500", "-VoxelVistaPitch=-14") }
  "underground" { @("-VoxelUndergroundTest") }
}

$Args = @("`"$Proj`"") + $Common + $PerCamera
Write-Output "RUN $Tag [$Camera]"
Write-Output "  $($Args -join ' ')"

$p = Start-Process -FilePath $Engine -ArgumentList $Args -PassThru `
  -RedirectStandardOutput (Join-Path $OutDir "$Tag.out.log") `
  -RedirectStandardError (Join-Path $OutDir "$Tag.err.log")
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
  Write-Output "  TIMEOUT after ${TimeoutSec}s -- killing"
  $p.Kill(); Start-Sleep -Seconds 3
}

$pngs = @(Get-ChildItem "$ShotDir\*.png" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
Write-Output "  captured $($pngs.Count) png(s)"
for ($i = 0; $i -lt $pngs.Count; $i++) {
  $dest = Join-Path $OutDir "$Tag-$i.png"
  Copy-Item $pngs[$i].FullName $dest -Force
  Write-Output "  -> $dest"
}

if (Test-Path $LogFile) {
  Select-String -Path $LogFile -Pattern "Voxel tile grid: dir=|bounding box|VoxelClimateProbe:|version too new" |
    Select-Object -First 5 | ForEach-Object { "  LOG " + $_.Line }
}

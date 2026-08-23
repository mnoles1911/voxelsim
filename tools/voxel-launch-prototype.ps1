# Launch the editor for a hands-on PIE test drive of the streaming prototype.
#
# WHY A SCRIPT AND NOT A REMEMBERED COMMAND LINE. Three of these arguments are
# not optional and each one has cost someone an hour:
#
#   * -VoxelSpawnAt is NOT optional. It defaults to (0,0), which is outside the
#     baked tiles, and the fine-tier gate is FATAL there: the run dies with
#     "FINE TIER GATE LEAK ... tile (-6,3) is not resident". The default below
#     sits at the junction of four baked tiles (-4,-4)/(-5,-4)/(-4,-5)/(-5,-5),
#     which is why it is that number and not a round one.
#   * THIS PROJECT HAS NO LEVEL ASSET. Zero .umap files. The world is built in
#     code by AVoxelEarthGameMode::BeginPlay, so the editor opens an empty
#     "Untitled" world with default UE landscape and THAT IS CORRECT -- BeginPlay
#     has not run. Terrain exists only once you press Play.
#   * -dx12 -sm6 are required by the marcher.
#
# ONE EDITOR PER BOX. This refuses to start if one is already running, for the
# same reason tools/voxel-run-flight-leg.ps1 does: two editors sharing the box
# produce contended numbers that look exactly like a slow configuration, and
# nothing in the log says so.
param(
    [string]$SpawnAt = '-61440,-61440',
    # Arm the GPU streaming path. Off by default because the CPU arm is the
    # measured-better producer TODAY -- see docs/gpu-streaming-architecture.md
    # for why that is a temporary state and what replaces it.
    [switch]$GpuStream,
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe',
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path "$PSScriptRoot\..\ue-project\VoxelEarth.uproject").Path

$running = @(Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id) ($($_.ProcessName))" }) -join ', '
    throw ("REFUSING TO START: $($running.Count) editor process(es) already running -- $detail. " +
           "One editor per box. Close it first.")
}

$argList = @("`"$Project`"", '-dx12', '-sm6', "-VoxelSpawnAt=$SpawnAt", '-VoxelNoClipmap')
if ($GpuStream) { $argList += '-VoxelGpuMesh' }
$argList += $ExtraArgs

Write-Host ""
Write-Host "  Launching editor. The empty 'Untitled' world is CORRECT -- press Play (PIE) to get terrain." -ForegroundColor Cyan
Write-Host "  Spawn: $SpawnAt   GPU streaming: $(if ($GpuStream) { 'ARMED' } else { 'off (CPU producer)' })" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Once in PIE, in the console (~):" -ForegroundColor Yellow
Write-Host "    voxel.GpuStream.Prototype 1   -- the streaming panel + hole stats, one switch"
Write-Host "    voxel.Debug 1                 -- the existing perf panel (FPS, 1% low, worst frame)"
Write-Host "    voxel.March.HoleStats 1       -- uncovered %, THE number that tracks visible holes"
Write-Host "    voxel.Shadow.March 2          -- sun shadows back on (costs ~13 ms/frame, backlog 0.0a)"
Write-Host ""
Write-Host "  What good looks like, from tonight's headless legs at 30 m/s:" -ForegroundColor Yellow
Write-Host "    chunks/s   6,565 mean / 9,098 peak   (owner floor: 6,200)"
Write-Host "    frame      p50 8.48 ms, p95 27.65 ms"
Write-Host "    uncovered  ~0.03% standing still, ~4.5% flying -- lower is better, this is the hole metric"
Write-Host ""

Start-Process -FilePath $Editor -ArgumentList $argList

param([switch]$Capture)
$ErrorActionPreference = 'Stop'
$busy = Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue
if ($busy) { throw 'An editor or compiler is already active; leave that session alone.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
$launchArgs = @(
    ('"' + (Join-Path $projectRoot 'ue-project/VoxelEarth.uproject') + '"'),
    '-game', '-nosplash', '-sm6', '-dx12', '-windowed', '-VoxelNoMenu',
    '-VoxelEnvironmentLOD', '-VoxelSpawnAt=-61440,-61440',
    '-VoxelTimeOfDay=12:00', '-VoxelDate=03-20', '-VoxelTimeScale=0',
    '-VoxelForceRes=1600x900', '-ResX=1600', '-ResY=900', '-VoxelFineTileGateFatal=0',
    ('-abslog="' + (Join-Path $projectRoot 'Saved/vegetation-game.log') + '"')
)
if ($Capture) { $launchArgs += '-ExecCmds="voxel.Debug.PlayerBox 0,voxel.Vegetation.Capture"' }
else { $launchArgs += '-ExecCmds="voxel.Debug.PlayerBox 0"' }
$gameProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe' -ArgumentList $launchArgs -WindowStyle Normal -PassThru
Write-Output "Vegetation game launched: PID $($gameProcess.Id)"

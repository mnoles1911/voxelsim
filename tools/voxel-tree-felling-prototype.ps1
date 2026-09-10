param([switch]$Capture, [switch]$KeepOpen, [switch]$Validate, [switch]$CleanupProbe, [switch]$PersistenceProbe)
$ErrorActionPreference = 'Stop'
$busy = Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue
if ($busy) { throw 'An editor or compiler is already active. Leave that session alone and retry after it closes.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
$logPath = Join-Path $projectRoot 'Saved/tree-felling-game.log'
$consoleCommands = 'voxel.Debug.PlayerBox 0'
if ($CleanupProbe) { $consoleCommands += ',voxel.DebrisCleanup.Probe' }
if ($PersistenceProbe) { $consoleCommands += ',voxel.DetachedPersistence.Probe' }
$launchArgs = @(
    ('"' + (Join-Path $projectRoot 'ue-project/VoxelEarth.uproject') + '"'),
    '-game', '-nosplash', '-sm6', '-dx12', '-windowed', '-VoxelNoMenu',
    '-VoxelEnvironmentLOD', '-VoxelTreeFelling', '-VoxelSpawnAt=-61472,-61504',
    '-VoxelTimeOfDay=12:00', '-VoxelDate=03-20', '-VoxelTimeScale=0',
    '-VoxelForceRes=1600x900', '-ResX=1600', '-ResY=900', '-VoxelFineTileGateFatal=0',
    ('-ExecCmds="' + $consoleCommands + '"'), ('-abslog="' + $logPath + '"')
)
if ($Capture) { $launchArgs += '-VoxelTreeFellingCapture' }
if ($KeepOpen) { $launchArgs += '-VoxelEnvironmentLODKeepOpen' }
if ($Validate) { $launchArgs += '-VoxelEnvironmentLODValidate' }
$gameProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe' -ArgumentList $launchArgs -WindowStyle Normal -PassThru
Write-Output "Visible game launched: PID $($gameProcess.Id), log $logPath"

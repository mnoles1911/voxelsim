param([ValidateSet('restore','async','async-exit')][string]$Mode = 'restore')
$ErrorActionPreference = 'Stop'
$busy = Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue
if ($busy) { throw 'An editor or compiler is active. Leave that session alone.' }
$projectRoot = Split-Path $PSScriptRoot -Parent
$save = Join-Path $projectRoot 'ue-project/Saved/Tests/detached-roundtrip.vxlog'
if (-not (Test-Path -LiteralPath $save)) { throw 'Run the tree-felling -PersistenceProbe first.' }
$probeCommands = @{
    'restore' = 'voxel.DetachedPersistence.CheckRestored'
    'async' = 'voxel.SaveAsync.Probe'
    'async-exit' = 'voxel.SaveAsync.Probe exit'
}
$probeLogs = @{
    'restore' = 'Saved/detached-restart-game.log'
    'async' = 'Saved/async-save-game.log'
    'async-exit' = 'Saved/async-save-exit-game.log'
}
$launchArgs = @(
    ('"' + (Join-Path $projectRoot 'ue-project/VoxelEarth.uproject') + '"'),
    '-game', '-nosplash', '-sm6', '-dx12', '-windowed', '-VoxelNoMenu',
    '-VoxelEnvironmentLOD', '-VoxelTreeFelling', '-VoxelDetachedRestoreProbe', '-VoxelSpawnAt=-61472,-61504',
    '-VoxelTimeOfDay=12:00', '-VoxelDate=03-20', '-VoxelTimeScale=0',
    '-VoxelForceRes=1600x900', '-ResX=1600', '-ResY=900', '-VoxelFineTileGateFatal=0',
    ('-ExecCmds="voxel.Debug.PlayerBox 0,' + $probeCommands[$Mode] + '"'),
    ('-abslog="' + (Join-Path $projectRoot $probeLogs[$Mode]) + '"')
)
if ($Mode -eq 'async-exit') {
    $asyncSave = Join-Path $projectRoot 'ue-project/Saved/SaveGames/async_save_verification/world.vxlog'
    if (-not (Test-Path -LiteralPath $asyncSave)) { throw 'Run -Mode async first.' }
    $launchArgs += '-VoxelAsyncRestoreProbe'
}
$gameProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe' -ArgumentList $launchArgs -WindowStyle Normal -PassThru
Write-Output "Restore verification game launched: PID $($gameProcess.Id)"

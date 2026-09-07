param([string]$LogPath = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $LogPath) { $LogPath = Join-Path $projectRoot 'Saved/object-persistence-tests.log' }
$busy = @(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue)
if ($busy.Count) { throw 'An editor or compiler is active. Leave that session alone.' }
$testArgs = @(
    ('"' + (Join-Path $projectRoot 'ue-project/VoxelEarth.uproject') + '"'),
    '-unattended', '-nop4', '-nosplash', '-dx12',
    '-ExecCmds="Automation RunTests Voxel.Objects"', '-TestExit="Automation Test Queue Empty"',
    ('-abslog="' + $LogPath + '"')
)
$testProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $testArgs -WindowStyle Hidden -PassThru
Write-Output "Object registry/persistence verification launched: PID $($testProcess.Id), log $LogPath"

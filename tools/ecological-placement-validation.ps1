param([string]$Output='D:\voxelsim\asset-forge\out\ecological-placement\ue-tests2', [string]$Preview='')
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'An Unreal session is already running.'}
$resolvedOutput=[IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
if(Test-Path -LiteralPath (Join-Path $resolvedOutput 'index.json')){throw 'Choose a fresh output directory to avoid stale test evidence.'}
$arguments=@('D:\voxelsim\ue-project\VoxelEarth.uproject','-unattended','-nop4','-nosplash','-nullrhi',
    '-ExecCmds="Automation RunTests Voxel.Ecology.PublishedLibraryIntegration"',
    '-TestExit="Automation Test Queue Empty"',
    ('-ReportExportPath="'+$resolvedOutput+'"'),('-abslog="'+(Join-Path $resolvedOutput 'unreal.log')+'"'))
if($Preview){$arguments+=('-VoxelEcologyTestPreview="'+[IO.Path]::GetFullPath($Preview)+'"')}
$process=Start-Process -FilePath D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
Write-Output "Ecology integration PID $($process.Id)"
$process.WaitForExit()
$report=Get-Content -LiteralPath (Join-Path $resolvedOutput 'index.json') -Raw | ConvertFrom-Json
if($process.ExitCode -ne 0 -or $report.failed -ne 0 -or $report.notRun -ne 0 -or $report.inProcess -ne 0 -or
   ($report.succeeded+$report.succeededWithWarnings) -ne 1 -or
   @($report.tests | Where-Object fullTestPath -eq 'Voxel.Ecology.PublishedLibraryIntegration').Count -ne 1){
    throw "Ecology integration failed; inspect $resolvedOutput\index.json"
}
Write-Output "Ecology published-library integration passed: $resolvedOutput"

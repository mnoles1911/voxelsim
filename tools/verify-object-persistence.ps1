param([string]$LogPath = '', [switch]$Wait, [ValidateRange(30,3600)][int]$TimeoutSeconds = 300, [switch]$AllowOtherProjectEditors)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $LogPath) { $LogPath = Join-Path $projectRoot 'Saved/object-persistence-tests.log' }
$busy = @(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue)
if ($AllowOtherProjectEditors) {
    $projectPath = [IO.Path]::GetFullPath((Join-Path $projectRoot 'ue-project/VoxelEarth.uproject'))
    $busy = @($busy | Where-Object {
        if ($_.ProcessName -notin @('UnrealEditor','UnrealEditor-Cmd')) { return $true }
        $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId = $($_.Id)").CommandLine
        if ($commandLine -match '(?i)(?:"([^\"]+\.uproject)"|([^\s"]+\.uproject))') {
            $otherProject = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
            return [IO.Path]::GetFullPath($otherProject) -eq $projectPath
        }
        return $true
    })
}
if ($busy.Count) { throw 'An editor or compiler is active. Leave that session alone.' }
$buildTools = @(Get-CimInstance Win32_Process -Filter "Name = 'dotnet.exe'" | Where-Object { $_.CommandLine -match 'UnrealBuildTool' })
if ($buildTools.Count) { throw 'UnrealBuildTool is active. Leave that build untouched and retry when idle.' }
$testArgs = @(
    ('"' + (Join-Path $projectRoot 'ue-project/VoxelEarth.uproject') + '"'),
    '-unattended', '-nop4', '-nosplash', '-dx12', '-Multiprocess', '-forcelogflush',
    '-LogCmds="global warning,LogAutomationController display,LogAutomationCommandLine display"',
    '-ExecCmds="Automation RunTests Voxel.Objects"', '-TestExit="Automation Test Queue Empty"',
    ('-abslog="' + $LogPath + '"')
)
$testProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $testArgs -WindowStyle Hidden -PassThru
Write-Output "Object registry/persistence verification launched: PID $($testProcess.Id), log $LogPath"
if ($Wait) {
    try {
        if (!$testProcess.WaitForExit($TimeoutSeconds * 1000)) { throw "Object automation timed out; log: $LogPath" }
        if ($testProcess.ExitCode -ne 0) { throw "Object automation exited $($testProcess.ExitCode); log: $LogPath" }
        $testText = Get-Content -LiteralPath $LogPath -Raw
        if ($testText -notmatch 'Automation Test Queue Empty ([1-9][0-9]*) tests performed\.') { throw "Object automation exited without a completed test queue; log: $LogPath" }
        $testCount = [int]$Matches[1]
        if ($testText -match 'Test Completed\. Result=\{Fail\}') { throw "Object automation reported a failed test; log: $LogPath" }
        $passedCount = [regex]::Matches($testText, 'Test Completed\. Result=\{Success\}').Count
        if ($passedCount -ne $testCount) { throw "Completed queue count differs from successful tests; log: $LogPath" }
        Write-Output "PASS: $passedCount object tests completed; process exit 0."
    }
    finally {
        # The watchdog owns only the process launched by this invocation.
        $testProcess.Refresh()
        if (!$testProcess.HasExited) { Stop-Process -Id $testProcess.Id -Force }
    }
}

param(
    [Parameter(Mandatory)][string]$TileDir,
    [Parameter(Mandatory)][string]$FineTileDir,
    [Parameter(Mandatory)][string]$FineProviderId,
    [Parameter(Mandatory)][string]$AssetDir,
    [Parameter(Mandatory)][long]$Seed,
    [Parameter(Mandatory)][double]$SpawnX,
    [Parameter(Mandatory)][double]$SpawnY,
    [ValidateSet('Prepare','Rehearse')][string]$Mode = 'Rehearse',
    [ValidateRange(1,120)][int]$StartAfterSeconds = 45,
    [ValidateRange(35,120)][int]$ExitAfterSeconds = 60,
    [ValidateRange(60,900)][int]$TimeoutSeconds = 300,
    [string]$LogPath = '',
    [switch]$AllowOtherProjectEditors
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$projectPath = [IO.Path]::GetFullPath((Join-Path $projectRoot 'ue-project/VoxelEarth.uproject'))
if (!$LogPath) { $LogPath = Join-Path $projectRoot ('Saved/environment-' + $Mode.ToLowerInvariant() + '-' + [Guid]::NewGuid().ToString('N') + '.log') }
$LogPath = [IO.Path]::GetFullPath($LogPath)
foreach ($requiredPath in @($projectPath,$TileDir,$FineTileDir,$AssetDir)) {
    if (!(Test-Path -LiteralPath $requiredPath)) { throw "Required local input is missing: $requiredPath" }
}
if ($FineProviderId -match '[/\\]' -or !(Test-Path -LiteralPath (Join-Path $FineTileDir $FineProviderId) -PathType Container)) {
    throw 'The requested fine provider has no existing cache directory. Verify its exact identity before launching.'
}
# Reject argument delimiters before handing paths/provider values to Unreal.
foreach ($argumentValue in @($projectPath,$TileDir,$FineTileDir,$AssetDir,$FineProviderId,$LogPath)) {
    if ($argumentValue -match '["\r\n]') { throw 'Input contains an unsupported quote or newline.' }
}
$busy = @(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue)
if ($AllowOtherProjectEditors) {
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
if ($buildTools.Count) { throw 'UnrealBuildTool is active. Retry after the build completes.' }
$command = if ($Mode -eq 'Rehearse') { 'voxel.Environment.RehearseHandoff' } else { 'voxel.Environment.PrepareCandidate' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$testArgs = @(
    ('"' + $projectPath + '"'), '-game','-dx12','-sm6','-Multiprocess','-unattended','-nosplash','-nosound','-VoxelNoMenu',
    '-windowed','-ResX=960','-ResY=540', ('-VoxelSeed=' + $Seed.ToString($culture)),
    ('-VoxelTileDir="' + [IO.Path]::GetFullPath($TileDir) + '"'),
    ('-VoxelFineTileDir="' + [IO.Path]::GetFullPath($FineTileDir) + '"'),
    ('-VoxelFineTileProviderId="' + $FineProviderId + '"'),
    '-VoxelFineTileRingRadius=0','-VoxelFineTileCacheBudgetGB=12',
    ('-VoxelAssetDir="' + [IO.Path]::GetFullPath($AssetDir) + '"'),
    ('-VoxelSpawnAt=' + $SpawnX.ToString($culture) + ',' + $SpawnY.ToString($culture)), '-VoxelSpawnAltM=10',
    ('-VoxelExecAfter=' + $StartAfterSeconds),
    ('-VoxelExecCmds="' + $command + ',voxel.DeferExec ' + $ExitAfterSeconds + ' quit"'),
    ('-abslog="' + $LogPath + '"')
)
$testProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $testArgs -WindowStyle Hidden -PassThru
Write-Output "Owned environment verification PID $($testProcess.Id); log $LogPath"
try {
    if (!$testProcess.WaitForExit($TimeoutSeconds * 1000)) { throw "Environment verification timed out: $LogPath" }
    if ($testProcess.ExitCode -ne 0) { throw "Environment verification exited $($testProcess.ExitCode): $LogPath" }
    $testText = Get-Content -LiteralPath $LogPath -Raw
    if ($testText -notmatch 'ProductionCandidate PREPARED HIDDEN .*publicationReady=0') { throw 'Hidden preparation evidence is missing.' }
    if ($testText -match 'assets PROBE:') { throw 'Unexpected automatic composition diagnostic.' }
    if ($Mode -eq 'Rehearse') {
        if ($testText -notmatch 'ProductionHandoff REHEARSAL PASSED .*allocatorPinned=0 publicationReady=0' -or
            $testText -notmatch 'REHEARSAL RELEASED reason=successful observation-only rehearsal') { throw 'Successful rehearsal/release evidence is missing.' }
        if ($testText -match 'REHEARSAL REFUSED') { throw 'Rehearsal reported a refusal.' }
    }
    Write-Output "PASS: $Mode completed with normal process exit0."
}
finally {
    $testProcess.Refresh()
    if (!$testProcess.HasExited) { Stop-Process -Id $testProcess.Id -Force }
}

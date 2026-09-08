param(
    [Parameter(Mandatory)][string]$AssetDir,
    [Parameter(Mandatory)][string]$TileDir,
    [Parameter(Mandatory)][string]$FineTileDir,
    [Parameter(Mandatory)][string]$FineProviderId,
    [long]$Seed=20260719,
    [long]$SpawnX=-113145,
    [long]$SpawnY=-162885,
    [ValidateRange(1,200)][int]$SpawnAltM=35,
    [ValidateRange(-90,90)][int]$SpawnPitch=-25,
    [string]$LogPath='',
    [ValidateRange(120,900)][int]$TimeoutSeconds=360,
    [ValidateRange(105,600)][int]$ObservationSeconds=105,
    [switch]$AssetResolveCacheOnly,
    [switch]$AllowOtherProjectEditors
)
$ErrorActionPreference='Stop'
if($TimeoutSeconds -le $ObservationSeconds){throw 'Timeout must exceed the observation interval.'}
$root=Split-Path $PSScriptRoot -Parent
$project=[IO.Path]::GetFullPath((Join-Path $root 'ue-project/VoxelEarth.uproject'))
$runDir=Join-Path $root ('ue-project/Saved/EnvironmentPopulation/'+[Guid]::NewGuid().ToString('N'))
if(!$LogPath){$LogPath=Join-Path $runDir 'population.log'}
$LogPath=[IO.Path]::GetFullPath($LogPath)
foreach($path in @($project,$AssetDir,$TileDir,$FineTileDir)) {
    if(!(Test-Path -LiteralPath $path)){throw "Missing input: $path"}
}
if($FineProviderId -match '[/\\]' -or !(Test-Path -LiteralPath (Join-Path $FineTileDir $FineProviderId))){throw 'Invalid or unavailable fine provider.'}
if(!(Test-Path -LiteralPath (Join-Path $AssetDir 'species.vxm'))){throw 'Export the environment manifest before launching.'}
foreach($texture in @('T_SkyStarmap','T_MoonColor','T_MoonDisplacement')) {
    if(!(Test-Path -LiteralPath (Join-Path $root ('ue-project/Content/Voxel/'+$texture+'.uasset')))){throw "Missing generated sky texture $texture; follow SKY_ASSET_CREDITS.md."}
}
foreach($value in @($project,$AssetDir,$TileDir,$FineTileDir,$FineProviderId,$runDir,$LogPath)) {
    if($value -match '["\r\n]'){throw 'Unsupported argument delimiter.'}
}
$busy=@(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild -ErrorAction SilentlyContinue)
if($AllowOtherProjectEditors) {
    $busy=@($busy | Where-Object {
        if($_.ProcessName -notin @('UnrealEditor','UnrealEditor-Cmd')){return $true}
        $commandLine=(Get-CimInstance Win32_Process -Filter "ProcessId = $($_.Id)").CommandLine
        if($commandLine -match '(?i)(?:"([^\"]+\.uproject)"|([^\s"]+\.uproject))') {
            $other=if($Matches[1]){$Matches[1]}else{$Matches[2]}
            return [IO.Path]::GetFullPath($other) -eq $project
        }
        return $true
    })
}
if($busy.Count){throw 'An editor or compiler is active; leave that session alone.'}
if(@(Get-CimInstance Win32_Process -Filter "Name = 'dotnet.exe'" | Where-Object {$_.CommandLine -match 'UnrealBuildTool'}).Count){throw 'UnrealBuildTool is active.'}
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
# Fresh private user data: changing population must not reinterpret an old save.
# This exercises the existing biome placer, not manually spawned prototype actors.
$firstShot=$ObservationSeconds-45
$secondShot=$ObservationSeconds-15
$commands="voxel.Stream.FrameAttribution 1,voxel.DeferExec $firstShot voxel.Debug.Screenshot,voxel.DeferExec $secondShot voxel.Debug.Screenshot,voxel.DeferExec $ObservationSeconds quit"
$argsList=@(('"'+$project+'"'),'-game','-dx12','-sm6','-Multiprocess','-unattended','-nosplash','-nosound',
    '-VoxelNoMenu','-VoxelNoLoad','-VoxelGpuPoolAlloc=1','-windowed','-ResX=960','-ResY=540',
    ('-VoxelSeed='+$Seed),('-VoxelSpawnAt='+$SpawnX+','+$SpawnY),('-VoxelSpawnAltM='+$SpawnAltM),('-VoxelSpawnPitch='+$SpawnPitch),
    ('-VoxelAssetDir="'+[IO.Path]::GetFullPath($AssetDir)+'"'),
    ('-VoxelTileDir="'+[IO.Path]::GetFullPath($TileDir)+'"'),
    ('-VoxelFineTileDir="'+[IO.Path]::GetFullPath($FineTileDir)+'"'),('-VoxelFineTileProviderId="'+$FineProviderId+'"'),
    '-VoxelFineTileRingRadius=0','-VoxelFineTileCacheBudgetGB=12',
    ('-UserDir="'+$runDir+'/"'),('-ExecCmds="'+$commands+'"'),('-abslog="'+$LogPath+'"'))
if($AssetResolveCacheOnly){$argsList+='-VoxelAssetResolveCacheOnly'}
$owned=Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $argsList -WindowStyle Hidden -PassThru
Write-Output "Owned population PID $($owned.Id); log $LogPath; private directory $runDir"
try {
    if(!$owned.WaitForExit($TimeoutSeconds*1000)){throw 'Population run timed out.'}
    if($owned.ExitCode -ne 0){throw "Population run exited $($owned.ExitCode)."}
    $log=Get-Content -LiteralPath $LogPath -Raw
    if($log -notmatch 'VoxelAssets: INSTALLED from .* -- [1-9][0-9]* layers, [1-9][0-9]* species placeable'){throw 'Procedural catalog did not install.'}
    if($log -notmatch 'VoxelDetailAssets: first group applied -- [1-9][0-9]* detail instances' -and $log -notmatch 'VoxelDetailAssets: [1-9][0-9]* groups live \([^\r\n]*\), [1-9][0-9]* instances'){throw 'Detail vegetation did not reach rendering.'}
    if($log -match 'Fatal error:|ALLOCATOR CROSS-CHECK FAILED|CLAIM SET MISMATCH|CLAIM VERIFY FAIL|Error:.*CLAIM STAGE DARK|doubleGrant [1-9][0-9]*|badFree [1-9][0-9]*'){throw 'Runtime or allocator failure.'}
    if($log -notmatch 'Engine exit requested|Exiting\.') {throw 'Normal exit receipt missing.'}
    $shots=@(Get-ChildItem -LiteralPath (Join-Path $runDir 'Saved/Screenshots') -Recurse -Filter '*.png' -ErrorAction SilentlyContinue)
    if($shots.Count -ne 2){throw 'Expected two population captures.'}
    Write-Output 'PASS: catalog installed, procedural detail rendered, two captures and normal exit. Species coverage, appearance and frame times require review.'
    $shots.FullName | Write-Output
} finally {
    $owned.Refresh()
    if(!$owned.HasExited){Stop-Process -Id $owned.Id -Force}
}

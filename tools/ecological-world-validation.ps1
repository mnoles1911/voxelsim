param([Parameter(Mandatory=$true)][string]$AssetDirectory,[Parameter(Mandatory=$true)][string]$Output,[string]$SpawnAt='-156260,-82356',[ValidateRange(60,7200)][int]$TimeoutSeconds=900,[switch]$VisualOnly,
    [string]$FineTileDirectory='', [string]$FineTileProviderId='')
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'An Unreal session is already running'}
$outPath=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $outPath){throw 'Use a fresh output directory'}
$assetPath=[IO.Path]::GetFullPath($AssetDirectory)
$config=Join-Path $assetPath 'placement.json'
$speciesManifest=Join-Path $assetPath 'species.vxm'
if(-not(Test-Path -LiteralPath $config)){throw 'Missing ecological configuration'}
# Isolated terrain captures must specify both parts of the fine cache identity.
# This overrides only this launch; project defaults and the game cache stay intact.
if([bool]$FineTileDirectory -ne [bool]$FineTileProviderId){throw 'Specify both FineTileDirectory and FineTileProviderId'}
if($FineTileDirectory){
    if($FineTileProviderId -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid fine tile provider id'}
    $finePath=[IO.Path]::GetFullPath($FineTileDirectory)
    if($finePath.Contains('"')){throw 'Invalid fine tile cache path'}
    if(-not(Test-Path -LiteralPath (Join-Path $finePath $FineTileProviderId) -PathType Container)){throw 'Missing fine tile provider directory'}
}
New-Item -ItemType Directory -Path $outPath | Out-Null
$argsRun=@('D:\voxelsim\ue-project\VoxelEarth.uproject','/Engine/Maps/Entry','-game','-dx12','-RenderOffscreen','-unattended','-nosplash','-nop4',
    '-ResX=1280','-ResY=720','-csvGpuStats','-csvCompression=0',"-VoxelAssetDir=$assetPath","-VoxelEcologyConfig=$config",
    '-VoxelEcologyWorldCapture',"-VoxelEcologyWorldOutput=$outPath",'-VoxelDetailRingMeters=48',"-VoxelSpawnAt=$SpawnAt",'-VoxelSpawnAltM=2',
    '-VoxelTimeOfDay=10:00','-VoxelDate=2026-05-15','-VoxelTimeScale=0',"-UserDir=$outPath/session","-abslog=$outPath/game.log")
if($FineTileDirectory){
    $argsRun += @("-VoxelFineTileDir=`"$finePath`"", "-VoxelFineTileProviderId=$FineTileProviderId")
}
$record=@{arguments=$argsRun;configurationSha256=(Get-FileHash -LiteralPath $config).Hash;startedUtc=[DateTime]::UtcNow.ToString('o');scope='Actual terrain and ecological field with instanced understory'}
$record.speciesManifestSha256=(Get-FileHash -LiteralPath $speciesManifest).Hash
$record.visualOnly=[bool]$VisualOnly
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$outPath/run-manifest.json" -Encoding utf8
$proc=Start-Process D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe -ArgumentList $argsRun -WindowStyle Hidden -PassThru
Write-Output "Ecological world capture PID $($proc.Id)"
$deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while(-not $proc.WaitForExit(1000)){
    if([DateTime]::UtcNow -ge $deadline){
        $proc.Kill()
        $proc.WaitForExit()
        "Capture exceeded $TimeoutSeconds seconds; no performance acceptance." | Set-Content -LiteralPath "$outPath/timeout.txt"
        throw "World capture timed out; logs preserved in $outPath"
    }
}
if($proc.ExitCode -ne 0){throw "World capture exited $($proc.ExitCode)"}
$log=Get-Content -LiteralPath "$outPath/game.log" -Raw
if($log -notmatch 'EcologyWorld COMPLETE'){throw 'World capture did not complete'}
if($log -match 'EcologyWorld STREAMING_BUSY|FINE TIER GATE LEAK|Fatal error:|Assertion failed:|Terrain appearance upload refused:'){throw 'World capture failed stability/appearance checks'}
if((Get-FileHash -LiteralPath $config).Hash -ne $record.configurationSha256){throw 'Ecological rules changed during capture'}
if((Get-FileHash -LiteralPath $speciesManifest).Hash -ne $record.speciesManifestSha256){throw 'Species lattice changed during capture'}
$csvMatches=[regex]::Matches($log,'Writing CSV to file : (.+\.csv)')
if($csvMatches.Count -ne 1){throw 'Missing completed frame capture'}
Copy-Item -LiteralPath $csvMatches[0].Groups[1].Value.Trim() -Destination "$outPath/frames.csv"
foreach($name in @('world-player.png','world-overview.png')){if(-not(Test-Path -LiteralPath "$outPath/$name")){throw "Missing $name"}}
Write-Output "World capture completed: $outPath; inspect habitat, appearance and performance evidence"

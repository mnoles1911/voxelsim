param(
    [Parameter(Mandatory=$true)][string]$AssetDirectory,
    [Parameter(Mandatory=$true)][string]$RouteFile,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(60,7200)][int]$TimeoutSeconds=3900,
    [switch]$ProfileFrames,[switch]$DiagnoseStalls,[switch]$DetailMeshLOD,[switch]$DetailSizeCull,[string]$DetailMeshCache='',
    [switch]$AllowPreviewDetailCache,[ValidateRange(16,512)][double]$DetailRingMeters=48
)
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild,dotnet -ErrorAction SilentlyContinue){throw 'UE or build process already running'}
$assetPath=(Resolve-Path -LiteralPath $AssetDirectory).Path
$routePath=(Resolve-Path -LiteralPath $RouteFile).Path
$outPath=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $outPath){throw 'Use a fresh output directory'}
$config=Join-Path $assetPath 'placement.json'
$species=Join-Path $assetPath 'species.vxm'
$route=Get-Content -LiteralPath $routePath -Raw | ConvertFrom-Json
$configHash=(Get-FileHash -LiteralPath $config).Hash
$speciesHash=(Get-FileHash -LiteralPath $species).Hash
$routeHash=(Get-FileHash -LiteralPath $routePath).Hash
if($route.configuration_sha256 -ne $configHash -or $route.species_manifest_sha256 -ne $speciesHash){throw 'Route and inventory pins differ'}
if($null -eq $route.spawn_x_m -or $null -eq $route.spawn_y_m -or @($route.waypoints).Count -lt 1){throw 'Missing route spawn or waypoints'}
$culture=[Globalization.CultureInfo]::InvariantCulture
$spawn=([double]$route.spawn_x_m).ToString($culture)+','+([double]$route.spawn_y_m).ToString($culture)
if($AllowPreviewDetailCache -and -not $DetailMeshCache){throw 'Preview cache flag requires a manifest'}
$cachePath=if($DetailMeshCache){(Resolve-Path -LiteralPath $DetailMeshCache).Path}else{$null}
$runArgs=@('D:\voxelsim\ue-project\VoxelEarth.uproject','/Engine/Maps/Entry','-game','-dx12','-RenderOffscreen','-unattended','-nosplash','-nop4',
    '-ResX=1280','-ResY=720','-csvGpuStats','-csvCompression=0',"-VoxelAssetDir=$assetPath","-VoxelEcologyConfig=$config",
    "-VoxelDetailRingMeters=$($DetailRingMeters.ToString($culture))","-VoxelSpawnAt=$spawn",'-VoxelSpawnAltM=5',
    '-VoxelTimeOfDay=10:00','-VoxelDate=2026-05-15','-VoxelTimeScale=0',
    "-VoxelEcologyRoute=$routePath","-VoxelEcologyRouteSha256=$routeHash","-VoxelEcologyRouteOutput=$outPath/results",
    "-UserDir=$outPath/session","-abslog=$outPath/unreal.log")
if($ProfileFrames){$runArgs+='-VoxelEcologyRouteProfile'}
if($DiagnoseStalls){$runArgs+='-VoxelEcologyRouteDiagnoseStalls'}
if($DetailMeshLOD){$runArgs+='-VoxelDetailMeshLOD'}
if($DetailSizeCull){$runArgs+='-VoxelDetailSizeCull'}
if($cachePath){$runArgs+="-VoxelDetailMeshCache=$cachePath"}
if($AllowPreviewDetailCache){$runArgs+='-VoxelDetailMeshCachePreview'}
foreach($arg in $runArgs){if($arg -match '["\r\n]'){throw 'Unsupported quote/newline in command argument'}}
$record=@{arguments=$runArgs;configurationSha256=$configHash;speciesManifestSha256=$speciesHash;routeSha256=$routeHash;
    startedUtc=[DateTime]::UtcNow.ToString('o');scope='Authored actual-pawn route, with optional endpoint evidence; no broad navigation or building acceptance'}
if($cachePath){$record.detailCacheManifestSha256=(Get-FileHash -LiteralPath $cachePath).Hash}
$record.runtimeModuleHashes=@{}
foreach($module in @('UnrealEditor-VoxelEarth.dll','UnrealEditor-VoxelEarthShaders.dll')){
    $record.runtimeModuleHashes[$module]=(Get-FileHash -LiteralPath (Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module)).Hash
}
New-Item -ItemType Directory -Path $outPath | Out-Null
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$outPath/run-manifest.json" -Encoding utf8
$quotedArgs=($runArgs | ForEach-Object {'"'+$_+'"'}) -join ' '
$proc=Start-Process D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe -ArgumentList $quotedArgs -WindowStyle Hidden -PassThru
Write-Output "Ecological route PID $($proc.Id)"
$deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while(-not $proc.WaitForExit(1000)){
    if([DateTime]::UtcNow -ge $deadline){
        $proc.Kill();$proc.WaitForExit()
        'Route timed out; no acceptance.' | Set-Content -LiteralPath "$outPath/timeout.txt"
        throw "Route timed out: $outPath"
    }
}
if($proc.ExitCode -ne 0){throw "Route process exited $($proc.ExitCode)"}
$log=Get-Content -LiteralPath "$outPath/unreal.log" -Raw
if($DetailSizeCull -and $log -notmatch 'DetailSizeCull key=') {throw 'Requested size culling was not exercised'}
if($log -notmatch 'VoxelRoute COMPLETE PASS' -or $log -match 'FINE TIER GATE LEAK|Fatal error:|Assertion failed:|Terrain appearance upload refused:'){throw 'Route completion/stability check failed'}
if($ProfileFrames){
    $frames=Join-Path $outPath 'results/route-frames.csv'
    if($log -notmatch 'VoxelRoute PROFILE_BEGIN mono=' -or $log -notmatch 'VoxelRoute PROFILE_END mono=' -or
       $log -notmatch 'VoxelRoute PROFILE_SAVED ok=1 path=' -or -not (Test-Path -LiteralPath $frames)){
        throw 'Requested route frame capture did not finish and export'
    }
    $header=Get-Content -LiteralPath $frames -TotalCount 1
    foreach($field in @('FrameTime','VoxelRoute/Walking','VoxelRoute/Point','VoxelRoute/ElapsedSeconds','VoxelRoute/CaptureFrame')){
        if($field -notin ($header -split ',')){throw "Route frame CSV missing $field"}
    }
}
foreach($pair in @(@($config,$configHash),@($species,$speciesHash),@($routePath,$routeHash))){
    if((Get-FileHash -LiteralPath $pair[0]).Hash -ne $pair[1]){throw 'Route inputs changed during capture'}
}
if($cachePath){
    if((Get-FileHash -LiteralPath $cachePath).Hash -ne $record.detailCacheManifestSha256){throw 'Cache changed during route'}
    if($log -notmatch 'DetailCache index accepted resources=\d+' -or $log -match 'DetailCache index refused'){throw 'Requested cache not accepted'}
}
foreach($module in $record.runtimeModuleHashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module)).Hash -ne $record.runtimeModuleHashes[$module]){throw 'Runtime module changed during route'}
}
Write-Output "Route completed: $outPath; run analyze_ecological_route.py and inspect endpoint evidence and images"

param(
    [Parameter(Mandatory=$true)][string]$AssetDirectory,
    [Parameter(Mandatory=$true)][string]$RouteFile,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(60,7200)][int]$TimeoutSeconds=3900,
    [switch]$ProfileFrames,[switch]$DiagnoseStalls,[switch]$DetailMeshLOD,[switch]$DetailSizeCull,[switch]$NoDetailSizeCull,[switch]$DetailRetireUnused,[string]$DetailMeshCache='',
    [switch]$AllowPreviewDetailCache,[ValidateRange(16,512)][double]$DetailRingMeters=48,
    # Opt-in flags the route needs but has no switch for -- e.g. -VoxelDetailNanite,
    # which MUST ride alongside a nanite=1 cache or the identity guard refuses the
    # run. Same validation as the walk harness, and recorded in the manifest's
    # argument list like every other flag, so a receipt binds them.
    [string[]]$ExtraArgs=@()
)
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild,dotnet -ErrorAction SilentlyContinue){throw 'UE or build process already running'}
# FOREIGN GPU LOAD. Added 2026-09-11 after a capture was launched while a game
# was running on the same GPU. The process guards below catch UE and build
# tools; they cannot see an unrelated 3D application, and a GPU timing capture
# taken next to one measures contention, not the renderer. The signature is
# recognisable after the fact -- the run sat at 62 CPU seconds for 25 minutes --
# but nothing refused it at the start, which is the half that matters.
#
# Returns $null if the counter is unavailable (then we warn rather than refuse,
# because failing closed on a missing instrument would block every capture on a
# machine that simply does not expose it).
function Get-ForeignGpuLoad {
    # Three samples, and judge on the MINIMUM per process. A single sample cannot
    # tell a browser compositing a frame from a game holding the card: both read
    # ~20% for an instant. Sustained load shows up in every sample; a spike does
    # not. Measured 2026-09-11: a game sat at 77% and a video tab at a steady
    # 16.5-20.7%, while an idle desktop reads nothing above 1%.
    param([int[]]$OwnPids = @(), [int]$Samples = 3, [int]$GapMs = 700)
    $mins = @{}
    for ($i = 0; $i -lt $Samples; $i++) {
        try { $set = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop).CounterSamples }
        catch { return $null }
        $byProc = @{}
        foreach ($c in $set) {
            if ($c.CookedValue -le 0.5) { continue }
            if ($c.InstanceName -match 'pid_(\d+)') {
                $procId = [int]$Matches[1]
                if ($OwnPids -contains $procId) { continue }
                $byProc[$procId] = $byProc[$procId] + $c.CookedValue
            }
        }
        foreach ($k in @($mins.Keys)) { if (-not $byProc.ContainsKey($k)) { $mins[$k] = 0 } }
        foreach ($k in $byProc.Keys) {
            if ($i -eq 0) { $mins[$k] = $byProc[$k] }
            elseif ($byProc[$k] -lt $mins[$k]) { $mins[$k] = $byProc[$k] }
        }
        if ($i -lt $Samples - 1) { Start-Sleep -Milliseconds $GapMs }
    }
    $busy = @()
    foreach ($k in $mins.Keys) {
        if ($mins[$k] -ge 10) {
            $p = Get-Process -Id $k -ErrorAction SilentlyContinue
            $busy += [pscustomobject]@{ ProcessId = $k; Name = $(if ($p) { $p.ProcessName } else { '(exited)' }); Percent = [math]::Round($mins[$k], 1) }
        }
    }
    return , $busy
}
$foreignGpu = Get-ForeignGpuLoad
if ($null -eq $foreignGpu) { Write-Output 'WARNING: GPU utilisation counter unavailable; foreign GPU load was NOT checked' }
elseif ($foreignGpu.Count) {
    throw ('Another process is using the GPU (' + (($foreignGpu | ForEach-Object { "$($_.Name) $($_.Percent)%" }) -join ', ') + '); a timing capture taken beside it measures contention, not this renderer')
}

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
# Size culling is DEFAULT ON since 2026-09-11 (owner's verdict on the route 16/17
# pictures). -DetailSizeCull is now a no-op kept so older call sites still parse;
# -NoDetailSizeCull is the control arm.
if($NoDetailSizeCull){$runArgs+='-VoxelNoDetailSizeCull'}
if($DetailRetireUnused){$runArgs+='-VoxelDetailRetireUnused'}
if($cachePath){$runArgs+="-VoxelDetailMeshCache=$cachePath"}
if($AllowPreviewDetailCache){$runArgs+='-VoxelDetailMeshCachePreview'}
foreach($extra in $ExtraArgs){if($extra -notmatch '^-[A-Za-z0-9=.:,_-]+$'){throw 'Unsupported extra argument'};$runArgs+=$extra}
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
$gpuTick=0
while(-not $proc.WaitForExit(1000)){
    # Foreign GPU load DURING the run; the start guard alone cannot tell a clean capture
    # from a lucky one. Sampled every ~10th tick because reading the counter costs real
    # time, and on the minimum of three reads so a spike does not void a good run.
    $gpuTick++
    if($gpuTick % 10 -eq 0){
        $foreignNow=Get-ForeignGpuLoad -OwnPids @($proc.Id)
        if($null -ne $foreignNow -and $foreignNow.Count){
            $proc.Kill();$proc.WaitForExit()
            throw ('Another process began using the GPU mid-route (' + (($foreignNow | ForEach-Object { "$($_.Name) $($_.Percent)%" }) -join ', ') + '); this capture is invalid')
        }
    }
    if([DateTime]::UtcNow -ge $deadline){
        $proc.Kill();$proc.WaitForExit()
        'Route timed out; no acceptance.' | Set-Content -LiteralPath "$outPath/timeout.txt"
        throw "Route timed out: $outPath"
    }
}
if($proc.ExitCode -ne 0){throw "Route process exited $($proc.ExitCode)"}
$log=Get-Content -LiteralPath "$outPath/unreal.log" -Raw
if(-not $NoDetailSizeCull -and $log -notmatch 'DetailSizeCull key=') {throw 'Size culling is on by default but was not exercised'}
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

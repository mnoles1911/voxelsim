param([Parameter(Mandatory=$true)][string]$AssetDirectory,[Parameter(Mandatory=$true)][string]$Output,
    [string]$SpawnAt='-156260,-82356',[ValidateRange(60,7200)][int]$TimeoutSeconds=900,
    [switch]$DetailMeshLOD,[switch]$DetailSizeCull,[switch]$DetailRetireUnused,[string]$DetailMeshCache='',
    [switch]$PredictiveAssetResolve,[switch]$NoPredictiveAssetResolve,[switch]$MarchDispatchIdentity,
    [switch]$AllowPreviewDetailCache,[ValidateRange(16,512)][double]$DetailRingMeters=48,
    [string[]]$ExtraArgs=@(),
    # Output resolution. The engine renders at a screen percentage of this and TSR
    # upscales; the log's "px of a WxH view" line is the real internal size. Default
    # 1280x720 keeps every historical capture comparable -- change it only to measure
    # resolution scaling, and never between the arms of an A/B.
    [ValidateRange(640,3840)][int]$RenderWidth=1280,[ValidateRange(360,2160)][int]$RenderHeight=720,
    # Screen percentage. THIS is the resolution knob that works: -ResX/-ResY are inert here
    # (2560x1440 requested still rendered 832x468, 2026-09-10), and the project default is
    # r.ScreenPercentage=65 in DefaultEngine.ini. Passed via -dpcvars because it is
    # Init-latched. Use it to measure pixel scaling; never vary it between A/B arms.
    [ValidateRange(10,100)][int]$ScreenPercentage=0)
$ErrorActionPreference='Stop'
if(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild,UnrealBuildTool,dotnet -ErrorAction SilentlyContinue){throw 'UE or a build process is already running'}
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

$outPath=[IO.Path]::GetFullPath($Output)
if(Test-Path -LiteralPath $outPath){throw 'Use a fresh output directory'}
$assetPath=[IO.Path]::GetFullPath($AssetDirectory)
$config=Join-Path $assetPath 'placement.json'
$speciesManifest=Join-Path $assetPath 'species.vxm'
if(-not(Test-Path -LiteralPath $config)){throw 'Missing ecological configuration'}
if($AllowPreviewDetailCache -and -not $DetailMeshCache){throw 'Preview cache flag requires a cache manifest'}
$cachePath=$null
if($DetailMeshCache){$cachePath=(Resolve-Path -LiteralPath $DetailMeshCache).Path}
New-Item -ItemType Directory -Path $outPath | Out-Null
$runArgs=@('D:\voxelsim\ue-project\VoxelEarth.uproject','/Engine/Maps/Entry','-game','-dx12','-RenderOffscreen','-unattended','-nosplash','-nop4',
    "-ResX=$RenderWidth","-ResY=$RenderHeight",'-csvGpuStats','-csvCompression=0',"-VoxelAssetDir=$assetPath","-VoxelEcologyConfig=$config",
    '-VoxelWalkTest=1','-VoxelWalkWaitForEcology',"-VoxelDetailRingMeters=$($DetailRingMeters.ToString([Globalization.CultureInfo]::InvariantCulture))","-VoxelSpawnAt=$SpawnAt",'-VoxelSpawnAltM=5',
    '-VoxelTimeOfDay=10:00','-VoxelDate=2026-05-15','-VoxelTimeScale=0',"-UserDir=$outPath/session","-abslog=$outPath/game.log")
if($DetailMeshLOD){$runArgs+='-VoxelDetailMeshLOD'}
if($DetailSizeCull){$runArgs+='-VoxelDetailSizeCull'}
if($DetailRetireUnused){$runArgs+='-VoxelDetailRetireUnused'}
if($PredictiveAssetResolve){$runArgs+='-VoxelPredictiveAssetResolve'}
if($NoPredictiveAssetResolve){$runArgs+='-VoxelNoPredictiveAssetResolve'}
if($MarchDispatchIdentity){$runArgs+='-VoxelMarchDispatchIdentity'}
if($cachePath){$runArgs+="-VoxelDetailMeshCache=$cachePath"}
if($AllowPreviewDetailCache){$runArgs+='-VoxelDetailMeshCachePreview'}
# Opt-in diagnostics (e.g. -VoxelR0EntryProfile -VoxelRecomputeCensus). Recorded in the manifest's
# argument list like every other flag, so a receipt binds them; never used for A/B arms.
if($ScreenPercentage){$runArgs+="-dpcvars=r.ScreenPercentage=$ScreenPercentage"}
foreach($extra in $ExtraArgs){if($extra -notmatch '^-[A-Za-z0-9=.:,_-]+$'){throw 'Unsupported extra argument'};$runArgs+=$extra}
$record=@{arguments=$runArgs;configurationSha256=(Get-FileHash -LiteralPath $config).Hash;startedUtc=[DateTime]::UtcNow.ToString('o');
    scope='Actual movement controller in ecological forest after streaming settles; scripted straight route, not general navigation acceptance'}
$record.speciesManifestSha256=(Get-FileHash -LiteralPath $speciesManifest).Hash
$record.runtimeModuleHashes=@{}
foreach($module in @('UnrealEditor-VoxelEarth.dll','UnrealEditor-VoxelEarthShaders.dll','UnrealEditor-VoxelEarthUI.dll')){
    $modulePath=Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module
    $record.runtimeModuleHashes[$module]=(Get-FileHash -LiteralPath $modulePath).Hash
}
if($cachePath){$record.detailCacheManifestSha256=(Get-FileHash -LiteralPath $cachePath).Hash}
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$outPath/run-manifest.json" -Encoding utf8
foreach($arg in $runArgs){if($arg -match '["\r\n]'){throw 'Unsupported quote/newline in command argument'}}
$quotedArgs=($runArgs | ForEach-Object { '"'+$_+'"' }) -join ' '
$validation=@{schema=1;status='running';manifestSha256=(Get-FileHash -LiteralPath "$outPath/run-manifest.json").Hash;
    startedUtc=[DateTime]::UtcNow.ToString('o');competingProcesses=@();failure=$null}
$validation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$outPath/run-validation.json" -Encoding utf8
$moduleMetadata=@{}
foreach($module in $record.runtimeModuleHashes.Keys){
    $info=Get-Item -LiteralPath (Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module)
    $moduleMetadata[$module]="$($info.Length):$($info.LastWriteTimeUtc.Ticks)"
}
$proc=$null
try {
$proc=Start-Process D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe -ArgumentList $quotedArgs -WindowStyle Hidden -PassThru
Write-Output "Ecological walking test PID $($proc.Id)"
$deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while(-not $proc.WaitForExit(1000)){
    # Fail this owned capture if another renderer/compiler competes. Do not
    # terminate somebody else's process. ShaderCompileWorker and the engine's
    # startup ValidatePlatforms dotnet child are intentionally not in this set.
    $competing=@(Get-Process UnrealEditor,UnrealEditor-Cmd,cl,link,MSBuild,UnrealBuildTool -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $proc.Id })
    if($competing.Count){
        $validation.competingProcesses=@($competing | ForEach-Object {@{id=$_.Id;name=$_.ProcessName;observedUtc=[DateTime]::UtcNow.ToString('o')}})
        throw 'Competing UE/compiler process detected; capture is invalid'
    }
    foreach($module in $moduleMetadata.Keys){
        $info=Get-Item -LiteralPath (Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module)
        if("$($info.Length):$($info.LastWriteTimeUtc.Ticks)" -ne $moduleMetadata[$module]){throw 'Runtime module file changed during capture'}
    }
    if([DateTime]::UtcNow -ge $deadline){
        $proc.Kill();$proc.WaitForExit()
        'Walking test timed out; no acceptance.' | Set-Content -LiteralPath "$outPath/timeout.txt"
        throw "Walking test timed out; inspect $outPath"
    }
}
$log=Get-Content -LiteralPath "$outPath/game.log" -Raw
if($DetailSizeCull -and $log -notmatch 'DetailSizeCull key=') {throw 'Requested size culling was not exercised'}
$csvMatches=[regex]::Matches($log,'Writing CSV to file : (.+\.csv)')
if($csvMatches.Count -eq 1){Copy-Item -LiteralPath $csvMatches[0].Groups[1].Value.Trim() -Destination "$outPath/frames.csv"}
if($proc.ExitCode -ne 0){throw "Walking test exited $($proc.ExitCode)"}
if($log -notmatch 'VoxelWalkTest COMPLETE: (\d+) passed, 0 FAILED\.' -or [int]$Matches[1] -lt 1){throw 'Walking checks failed or did not complete'}
if($log -notmatch 'VoxelWalkTest ECOLOGY_MEASURE_BEGIN' -or $log -notmatch 'VoxelWalkTest ECOLOGY_MEASURE_END' -or $csvMatches.Count -ne 1){throw 'Missing settled walking measurement'}
if($log -match 'FINE TIER GATE LEAK|Fatal error:|Assertion failed:|Terrain appearance upload refused:'){throw 'Walking test failed stability/appearance checks'}
if((Get-FileHash -LiteralPath $config).Hash -ne $record.configurationSha256){throw 'Ecological rules changed during walking test'}
if((Get-FileHash -LiteralPath $speciesManifest).Hash -ne $record.speciesManifestSha256){throw 'Species lattice changed during walking test'}
foreach($module in $record.runtimeModuleHashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module)).Hash -ne $record.runtimeModuleHashes[$module]){throw 'Runtime module changed during walking test'}
}
if($cachePath){
    if((Get-FileHash -LiteralPath $cachePath).Hash -ne $record.detailCacheManifestSha256){throw 'Detail cache manifest changed during walking test'}
    if($log -notmatch 'DetailCache index accepted resources=\d+' -or $log -match 'DetailCache index refused'){throw 'Requested cache was not accepted; this is not a cache measurement'}
}
$validation.status='passed'
Write-Output "Walking checks completed: $outPath; inspect frame costs and route limitations"
} catch {
    $validation.status='failed';$validation.failure=$_.Exception.Message
    throw
} finally {
    if($proc){
        if(-not $proc.HasExited){$proc.Kill();$proc.WaitForExit()}
        $validation.processExitCode=$proc.ExitCode
    }
    $validation.finishedUtc=[DateTime]::UtcNow.ToString('o')
    $validation.endRuntimeModuleHashes=@{}
    foreach($module in $record.runtimeModuleHashes.Keys){
        $path=Join-Path 'D:\voxelsim\ue-project\Binaries\Win64' $module
        if(Test-Path -LiteralPath $path){$validation.endRuntimeModuleHashes[$module]=(Get-FileHash -LiteralPath $path).Hash}
    }
    $validation.endInputHashes=@{}
    foreach($pair in @(@('configurationSha256',$config),@('speciesManifestSha256',$speciesManifest),@('detailCacheManifestSha256',$cachePath))){
        if($pair[1] -and (Test-Path -LiteralPath $pair[1])){$validation.endInputHashes[$pair[0]]=(Get-FileHash -LiteralPath $pair[1]).Hash}
    }
    $validation.artifactHashes=@{}
    foreach($name in @('game.log','frames.csv')){
        if(Test-Path -LiteralPath "$outPath/$name"){$validation.artifactHashes[$name]=(Get-FileHash -LiteralPath "$outPath/$name").Hash}
    }
    $validation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$outPath/run-validation.json" -Encoding utf8
}

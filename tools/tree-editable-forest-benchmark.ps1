param([ValidateRange(1,128)][int]$Count=16,[string]$Label='forest-16',[switch]$HiddenControl,[string]$SpawnAt='-160980,-82020',[string]$AssetDirectory='D:\voxelsim\asset-forge\out\engine',[switch]$CpuTrace,[switch]$Sequence,[ValidateRange(0,2)][int]$RayQueryMode=1,[ValidateRange(900,7200)][int]$TimeoutSeconds=3600,[string]$EcologyLayout='')
$ErrorActionPreference='Stop'
if($EcologyLayout){
    if($Sequence){throw 'Ecological layout does not support the edit sequence'}
    $layout=Get-Content -LiteralPath $EcologyLayout -Raw | ConvertFrom-Json
    if(-not $layout.preview_only -or $layout.models.Count -lt 1 -or $layout.models.Count -gt 128){throw 'Invalid ecological preview layout'}
    $Count=$layout.models.Count
}elseif($Count -gt 36){throw 'Legacy arrangement supports at most36trees'}
if($Label -notmatch '^[a-z0-9-]+$'){throw 'Use a simple lowercase benchmark label'}
if(Get-Process UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue){throw 'Another Unreal session is open'}
$forestOut=Join-Path 'D:\voxelsim\asset-forge\out\tree-runtime-appearance-v1' $Label
New-Item -ItemType Directory -Path $forestOut -Force | Out-Null
$forestLog=Join-Path $forestOut 'game.log'
$forestArgs=@('D:\voxelsim\ue-project\VoxelEarth.uproject','/Engine/Maps/Entry','-game','-dx12','-RenderOffscreen','-unattended','-nosplash','-nop4','-ResX=1280','-ResY=720','-csvGpuStats','-csvCompression=0',"-VoxelAssetDir=$AssetDirectory",'-VoxelAppearanceForest','-VoxelAppearanceForestExit',"-VoxelAppearanceForestCount=$Count","-VoxelAppearanceForestOutput=$forestOut","-abslog=$forestLog")
$forestStarted=Get-Date
if($EcologyLayout){$forestArgs+=@("-VoxelEcologyForestLayout=$EcologyLayout",'-VoxelAssetScatterOff')}
if($HiddenControl){$forestArgs+='-VoxelAppearanceForestHidden'}
if($Sequence){if($HiddenControl){throw 'Sequence controls its own visible/hidden order'};$forestArgs+='-VoxelAppearanceForestSequence'}
if($CpuTrace){$forestArgs+='-VoxelAppearanceForestCpuTrace'}
$forestArgs+="-VoxelRayQuery=$RayQueryMode"
$forestArgs+=@("-VoxelSpawnAt=$SpawnAt",'-VoxelSpawnAltM=2','-VoxelTimeOfDay=10:00','-VoxelDate=2026-05-15','-VoxelTimeScale=0',"-UserDir=$forestOut/session-$([guid]::NewGuid().ToString('N'))")
$forestInventory=Join-Path $AssetDirectory 'appearance/published.json'
function Get-ForestInputsHash {
    $forestInputFiles=@(Get-Item -LiteralPath (Join-Path $AssetDirectory 'species.vxm'))
    foreach($forestInputFolder in @('banks','appearance')){
        $forestInputFiles+=@(Get-ChildItem -LiteralPath (Join-Path $AssetDirectory $forestInputFolder) -Recurse -File)
    }
    $forestHashRows=foreach($forestInputFile in ($forestInputFiles | Sort-Object FullName)){
        $forestRelative=[IO.Path]::GetRelativePath($AssetDirectory,$forestInputFile.FullName)
        $forestContentHash=(Get-FileHash -LiteralPath $forestInputFile.FullName -Algorithm SHA256).Hash
        "$forestRelative $forestContentHash"
    }
    $forestHashBytes=[Text.Encoding]::UTF8.GetBytes(($forestHashRows -join "`n"))
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($forestHashBytes)).ToLowerInvariant()
}
$forestManifest=[ordered]@{
    schema=1
    startedUtc=$forestStarted.ToUniversalTime().ToString('o')
    count=$Count
    hiddenControl=[bool]$HiddenControl
    cpuTrace=[bool]$CpuTrace
    rayQueryMode=$RayQueryMode
    spawnAt=$SpawnAt
    outputResolution=@(1280,720)
    internalResolution='Read actual runtime evidence; output resolution is not internal resolution'
    publicationSha256=(Get-FileHash -LiteralPath $forestInventory -Algorithm SHA256).Hash.ToLowerInvariant()
    forestInputsSha256=Get-ForestInputsHash
    paletteSha256=(Get-FileHash -LiteralPath 'D:\voxelsim\asset-forge\rules\tree-appearance-spring-v2.json' -Algorithm SHA256).Hash.ToLowerInvariant()
    gameModuleSha256=(Get-FileHash -LiteralPath 'D:\voxelsim\ue-project\Binaries\Win64\UnrealEditor-VoxelEarth.dll' -Algorithm SHA256).Hash.ToLowerInvariant()
    arguments=$forestArgs
    ecologicalLayout=$EcologyLayout
    ecologicalLayoutSha256=$(if($EcologyLayout){(Get-FileHash -LiteralPath $EcologyLayout -Algorithm SHA256).Hash.ToLowerInvariant()}else{$null})
}
$forestManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $forestOut 'run-manifest.json') -Encoding utf8
$forestProc=Start-Process 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' -ArgumentList $forestArgs -WindowStyle Hidden -PassThru
Write-Output "Owned forest benchmark PID $($forestProc.Id)"
# Cold shader compilation plus measured terrain settlement can exceed15minutes.
$forestTimeout=$TimeoutSeconds*1000
if(-not $forestProc.WaitForExit($forestTimeout)){
    $forestProc.Refresh()
    if(-not $forestProc.HasExited){
        Stop-Process -Id $forestProc.Id -ErrorAction SilentlyContinue
        throw 'Owned forest benchmark timed out'
    }
}
if($forestProc.ExitCode -ne 0){throw "Benchmark exited $($forestProc.ExitCode)"}
if((Get-FileHash -LiteralPath $forestInventory -Algorithm SHA256).Hash.ToLowerInvariant() -ne $forestManifest.publicationSha256){throw 'Published inventory changed during capture'}
if((Get-ForestInputsHash) -ne $forestManifest.forestInputsSha256){throw 'Forest banks or catalog changed during capture'}
if(-not (Select-String -LiteralPath $forestLog -Pattern "AppearanceForest COMPLETE count=$Count" -Quiet)){throw 'Forest did not complete'}
if($EcologyLayout){
    if((Get-FileHash -LiteralPath $EcologyLayout -Algorithm SHA256).Hash.ToLowerInvariant() -ne $forestManifest.ecologicalLayoutSha256){throw 'Ecological layout changed during capture'}
    if(-not(Select-String -LiteralPath $forestLog -Pattern 'AppearanceForest ECOLOGY_PLAYER_VIEW eyeHeightCm=170' -Quiet)){throw 'Ecological player view not captured'}
}elseif(-not (Select-String -LiteralPath $forestLog -Pattern 'AppearanceForest EDIT success=1' -Quiet)){throw 'Forest edit did not succeed'}
if(Select-String -LiteralPath $forestLog -Pattern 'Failed to compile Material|FINE TIER GATE LEAK|Assertion failed:|Fatal error:|sequence source reset failed|VoxelRayQuery MISMATCH|LogVoxelGpuWorklist: Error: \[gpu-worklist\] CLAIM (STAGE DARK|SET MISMATCH|VERIFY FAIL)|Approved GPU appearance preparation declined|Approved appearance CPU upload refused|Approved edited (?:coarse |recursive )?appearance preparation failed' -Quiet){throw 'Forest run contains a material, terrain, or runtime failure'}
if($RayQueryMode -eq 2 -and -not (Select-String -LiteralPath $forestLog -Pattern 'VoxelRayQuery AUDIT rays=\d+ samples=[1-9]\d* mismatches=0' -Quiet)){throw 'Ray audit did not exercise material comparisons'}
$forestCsv=@(Select-String -LiteralPath $forestLog -Pattern 'Writing CSV to file : (.+\.csv)')
if($Sequence){
    if($forestCsv.Count -ne 4 -or -not (Select-String -LiteralPath $forestLog -Pattern 'AppearanceForest SEQUENCE_COMPLETE passes=4' -Quiet)){throw 'Four-pass forest sequence incomplete'}
    $forestLines=Get-Content -LiteralPath $forestLog
    $forestCommon=@($forestLines | Where-Object { $_ -match 'Metadata set : gpu=|px of a \d+x\d+ view|AppearanceForest (SPAWN|PLACEMENT)' })
    foreach($forestPass in @('visible1','hidden1','hidden2','visible2')){
        $forestBegin=@(Select-String -LiteralPath $forestLog -Pattern "AppearanceForest PASS_BEGIN name=$forestPass ")
        $forestEnd=@(Select-String -LiteralPath $forestLog -Pattern "AppearanceForest PASS_COMPLETE name=$forestPass$")
        if($forestBegin.Count -ne 1 -or $forestEnd.Count -ne 1){throw "Missing pass boundaries: $forestPass"}
        $forestPassCsv=@($forestCsv | Where-Object { $_.LineNumber -ge $forestBegin[0].LineNumber -and $_.LineNumber -le $forestEnd[0].LineNumber })
        if($forestPassCsv.Count -ne 1){throw "Missing CSV inside pass: $forestPass"}
        $forestPassOut=Join-Path $forestOut $forestPass
        Copy-Item -LiteralPath $forestPassCsv[0].Matches[0].Groups[1].Value.Trim() -Destination (Join-Path $forestPassOut 'frames.csv')
        $forestPassLines=$forestLines[($forestBegin[0].LineNumber-1)..($forestEnd[0].LineNumber-1)]
        @($forestCommon)+@($forestPassLines) | Set-Content -LiteralPath (Join-Path $forestPassOut 'game.log') -Encoding utf8
        $forestPassManifest=[ordered]@{}
        foreach($forestKey in $forestManifest.Keys){$forestPassManifest[$forestKey]=$forestManifest[$forestKey]}
        $forestPassManifest['sequencePass']=$forestPass
        $forestPassManifest['hiddenControl']=$forestPass.StartsWith('hidden')
        $forestPassManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $forestPassOut 'run-manifest.json') -Encoding utf8
        foreach($forestImage in @('forest-before.png','forest-after.png')){
            if((Get-Item -LiteralPath (Join-Path $forestPassOut $forestImage)).LastWriteTime -lt $forestStarted){throw "Stale screenshot in $forestPass"}
        }
    }
    Write-Output "Four-pass forest sequence captured at $forestOut; inspect and analyze each pass before accepting."
    exit 0
}
if($forestCsv.Count -ne 1){throw 'Expected one completed forest CSV'}
$forestCsvPath=$forestCsv[0].Matches[0].Groups[1].Value.Trim()
Copy-Item -LiteralPath $forestCsvPath -Destination (Join-Path $forestOut 'frames.csv')
foreach($name in @('forest-before.png','forest-after.png')){
    $shot=Get-Item -LiteralPath (Join-Path $forestOut $name)
    if($shot.LastWriteTime -lt $forestStarted){throw "Stale screenshot $name"}
}
Write-Output "Forest run captured at $forestOut; inspect screenshots and analyze frames before accepting."

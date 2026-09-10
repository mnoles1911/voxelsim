# Finalize an already completed run without launching or replaying the game.
param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference='Stop'
$forestOut=(Resolve-Path -LiteralPath $RunDirectory).Path
$forestLog=Join-Path $forestOut 'game.log'
$forestManifest=Get-Content -LiteralPath (Join-Path $forestOut 'run-manifest.json') -Raw | ConvertFrom-Json -AsHashtable
$forestStartedUtc=if($forestManifest.startedUtc -is [DateTime]){$forestManifest.startedUtc.ToUniversalTime()}else{[DateTimeOffset]::Parse($forestManifest.startedUtc).UtcDateTime}
$Count=[int]$forestManifest.count
$RayQueryMode=[int]$forestManifest.rayQueryMode
$Sequence=@($forestManifest.arguments) -contains '-VoxelAppearanceForestSequence'
$assetArgs=@($forestManifest.arguments | Where-Object {$_ -like '-VoxelAssetDir=*'})
if($assetArgs.Count -ne 1){throw 'Capture manifest must name exactly one asset directory'}
$AssetDirectory=$assetArgs[0].Substring('-VoxelAssetDir='.Length)
$forestInventory=Join-Path $AssetDirectory 'appearance/published.json'
$ownedOutput='-VoxelAppearanceForestOutput='+$forestOut
$live=@(Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor-Cmd.exe'" | Where-Object {$_.CommandLine -and $_.CommandLine.Contains($ownedOutput)})
if($live.Count){throw 'Capture process is still live; do not finalize yet'}
if((Get-FileHash -LiteralPath 'D:\voxelsim\asset-forge\rules\tree-appearance-spring-v2.json' -Algorithm SHA256).Hash.ToLowerInvariant() -ne $forestManifest.paletteSha256){throw 'Palette changed after capture'}
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
if((Get-FileHash -LiteralPath $forestInventory -Algorithm SHA256).Hash.ToLowerInvariant() -ne $forestManifest.publicationSha256){throw 'Published inventory changed during capture'}
if((Get-ForestInputsHash) -ne $forestManifest.forestInputsSha256){throw 'Forest banks or catalog changed during capture'}
if(-not (Select-String -LiteralPath $forestLog -Pattern "AppearanceForest COMPLETE count=$Count" -Quiet)){throw 'Forest did not complete'}
if(-not (Select-String -LiteralPath $forestLog -Pattern 'AppearanceForest EDIT success=1' -Quiet)){throw 'Forest edit did not succeed'}
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
            if((Get-Item -LiteralPath (Join-Path $forestPassOut $forestImage)).LastWriteTimeUtc -lt $forestStartedUtc){throw "Stale screenshot in $forestPass"}
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
    if($shot.LastWriteTimeUtc -lt $forestStartedUtc){throw "Stale screenshot $name"}
}
Write-Output "Forest run captured at $forestOut; inspect screenshots and analyze frames before accepting."

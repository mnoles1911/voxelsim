param([string]$LogPath='', [ValidateRange(20,60)][int]$WorldSeconds=55,
    [ValidateRange(120,600)][int]$TimeoutSeconds=300, [switch]$AllowOtherProjectEditors)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$projectPath=[IO.Path]::GetFullPath((Join-Path $projectRoot 'ue-project/VoxelEarth.uproject'))
foreach($texture in @('T_SkyStarmap','T_MoonColor','T_MoonDisplacement')) {
    if(!(Test-Path -LiteralPath (Join-Path $projectRoot ('ue-project/Content/Voxel/'+$texture+'.uasset')))) {
        throw "Missing generated sky texture $texture. Restore/import the dependencies documented in ue-project/Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md before visual travel verification."
    }
}
$runDir=Join-Path $projectRoot ('ue-project/Saved/GpuTravel/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
if (!$LogPath) { $LogPath=Join-Path $runDir 'runtime.log' }
$LogPath=[IO.Path]::GetFullPath($LogPath)
foreach($value in @($projectPath,$runDir,$LogPath)) { if($value -match '["\r\n]') { throw 'Unsupported argument delimiter.' } }
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

# Core-ticker commands survive OpenLevel. Private UserDir prevents fixture saves
# and settings from replacing the developer's normal Saved directory.
$commands=@()
for($worldIndex=0;$worldIndex -lt 3;$worldIndex++) {
    $commands+=('voxel.DeferExec ' + ($worldIndex*$WorldSeconds+$WorldSeconds-7) + ' voxel.Debug.Screenshot')
    $commands+=('voxel.DeferExec ' + (($worldIndex+1)*$WorldSeconds) + $(if($worldIndex -lt 2){' open /Engine/Maps/Entry'}else{' quit'}))
}
$testArgs=@(('"'+$projectPath+'"'),'-game','-dx12','-sm6','-Multiprocess','-unattended','-nosplash','-nosound',
    '-VoxelNoMenu','-VoxelNoLoad','-VoxelGpuPoolAlloc=1','-VoxelSeed=20260719','-VoxelSpawnAt=-39661,-57292','-VoxelSpawnAltM=10',
    '-windowed','-ResX=960','-ResY=540',('-UserDir="'+$runDir+'/"'),
    ('-ExecCmds="'+($commands -join ',')+'"'),('-abslog="'+$LogPath+'"'))
$testProcess=Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $testArgs -WindowStyle Hidden -PassThru
Write-Output "Owned GPU travel PID $($testProcess.Id); log $LogPath; private user directory $runDir"
try {
    if(!$testProcess.WaitForExit($TimeoutSeconds*1000)){throw 'GPU travel timed out.'}
    if($testProcess.ExitCode -ne 0){throw "GPU travel exited $($testProcess.ExitCode)."}
    $content=Get-Content -LiteralPath $LogPath -Raw
    if($content -notmatch 'Engine exit requested|Exiting\.' -or $content -match 'ALLOCATOR CROSS-CHECK FAILED|CLAIM SET MISMATCH|CLAIM VERIFY FAIL|\[brick-gpualloc\].*(?:DOUBLE GRANT|BAD FREE)|doubleGrant [1-9][0-9]*|badFree [1-9][0-9]*|Error:.*CLAIM STAGE DARK|Fatal error:') {throw 'GPU travel lacks clean exit or reported an allocator failure.'}
    $worlds=[regex]::Split($content,'LogLoad: LoadMap: /Engine/Maps/Entry[^\r\n]*')
    if($worlds.Count -ne 4){throw "Expected3 loaded worlds, found $($worlds.Count-1)."}
    $previousChecks=0L
    for($i=1;$i -lt $worlds.Count;$i++) {
        if($worlds[$i] -notmatch 'GPU \(prev window\): claims [1-9][0-9]*' -or $worlds[$i] -notmatch 'xcheck [1-9][0-9]* ok / 0 FAIL') {throw "World$i lacks landed claims and successful allocator cross-check evidence."}
        $checks=@([regex]::Matches($worlds[$i],'xcheck ([0-9]+) ok / 0 FAIL') | ForEach-Object {[long]$_.Groups[1].Value})
        $latestChecks=($checks | Measure-Object -Maximum).Maximum
        if($latestChecks -le [Math]::Max($previousChecks,$checks[0])){throw "World$i has no new successful allocator cross-checks."}
        $previousChecks=$latestChecks
        if([regex]::Matches($worlds[$i],'voxel.DeferExec: running now: voxel.Debug.Screenshot').Count -ne 1){throw "World$i lacks exactly one screenshot request."}
    }
    $teardowns=[regex]::Matches($content,'Voxel GPU teardown:.*pool now 0, index now 0 -- both must be 0')
    if($teardowns.Count -ne 3){throw 'Not every world emptied pool/index ownership exactly once.'}
    $captures=[regex]::Matches($content,'Tracing Screenshot "(ScreenShot[0-9]+)" taken with size: 960 x 540')
    if($captures.Count -ne 3){throw 'Expected three completed viewport captures.'}
    for($i=0;$i -lt 3;$i++) {
        if($captures[$i].Index -ge $teardowns[$i].Index -or ($i -lt 2 -and $teardowns[$i].Index -ge $captures[$i+1].Index)){throw 'Screenshot/teardown order does not prove three separate worlds.'}
        $shutdown=$content.Substring($captures[$i].Index,$teardowns[$i].Index-$captures[$i].Index)
        if($shutdown -notmatch 'CPU quiescence COMPLETE tasks=[0-9]+ pool=[0-9]+ elapsedMs=[0-9.]+ admissionClosed=1'){throw "World$($i+1) lacks completed CPU quiescence before GPU ownership teardown."}
    }
    $shots=@(Get-ChildItem -LiteralPath (Join-Path $runDir 'Saved/Screenshots') -Filter '*.png' -Recurse -ErrorAction SilentlyContinue)
    if($shots.Count -ne 3){throw "Expected3 reviewable screenshots, found $($shots.Count)."}
    Write-Output 'PASS:3 worlds,2 real map travels, landed GPU claims/cross-checks, empty teardown and normal exit0. Images require review.'
    $shots.FullName | Write-Output
} finally {
    $testProcess.Refresh()
    if(!$testProcess.HasExited){Stop-Process -Id $testProcess.Id -Force}
}

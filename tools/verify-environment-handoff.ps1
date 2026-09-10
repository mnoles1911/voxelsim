param(
    [Parameter(Mandatory)][string]$TileDir,
    [Parameter(Mandatory)][string]$FineTileDir,
    [Parameter(Mandatory)][string]$FineProviderId,
    [Parameter(Mandatory)][string]$AssetDir,
    [Parameter(Mandatory)][long]$Seed,
    [Parameter(Mandatory)][double]$SpawnX,
    [Parameter(Mandatory)][double]$SpawnY,
    [ValidateSet('Prepare','Rehearse','HeldCpu','HeldGpu','VisualPilot')][string]$Mode = 'Rehearse',
    [ValidateRange(1,120)][int]$StartAfterSeconds = 45,
    [ValidateRange(35,120)][int]$ExitAfterSeconds = 60,
    [ValidateRange(60,900)][int]$TimeoutSeconds = 300,
    [string]$LogPath = '',
    [switch]$SurfaceLightingDiagnostic,
    [switch]$CaptureBuffers,
    [switch]$AssetResolveCacheOnly,
    [switch]$GpuAllocatorVisualPilot,
    [switch]$AllowOtherProjectEditors
)
$ErrorActionPreference = 'Stop'
if ($SurfaceLightingDiagnostic -and $Mode -ne 'VisualPilot') { throw '-SurfaceLightingDiagnostic requires -Mode VisualPilot.' }
if ($CaptureBuffers -and $Mode -ne 'VisualPilot') { throw '-CaptureBuffers requires -Mode VisualPilot.' }
if ($GpuAllocatorVisualPilot -and $Mode -ne 'VisualPilot') { throw '-GpuAllocatorVisualPilot requires -Mode VisualPilot.' }
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
$command = switch ($Mode) {
    'Rehearse' { 'voxel.Environment.RehearseHandoff' }
    'HeldCpu' { 'voxel.Environment.PrepareHeldCpuPages' }
    'HeldGpu' { 'voxel.Environment.PrepareHeldGpuPages' }
    'VisualPilot' { 'voxel.Environment.PublishVisualPilot' }
    default { 'voxel.Environment.PrepareCandidate' }
}
$delayedCommands = $command
if ($SurfaceLightingDiagnostic) {
    $delayedCommands = 'voxel.GI.Volume 0,voxel.Light.Propagated 0,voxel.Environment.SurfaceLightingDiagnostic 1,' + $command
}
if ($CaptureBuffers) { $delayedCommands = 'voxel.Environment.CaptureBuffers 1,' + $delayedCommands }
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
    ('-VoxelExecCmds="' + $delayedCommands + ',voxel.DeferExec ' + $ExitAfterSeconds + ' quit"'),
    ('-abslog="' + $LogPath + '"')
)
if ($Mode -eq 'VisualPilot') { $testArgs += $(if ($GpuAllocatorVisualPilot) { '-VoxelGpuPoolAlloc=1' } else { '-VoxelGpuPoolAlloc=0' }) }
if ($AssetResolveCacheOnly) { $testArgs += @('-VoxelAsyncAssetResolve','-VoxelAsyncAssetResolveWarm=0') }
$testProcess = Start-Process 'D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' -ArgumentList $testArgs -WindowStyle Hidden -PassThru
Write-Output "Owned environment verification PID $($testProcess.Id); log $LogPath"
try {
    if (!$testProcess.WaitForExit($TimeoutSeconds * 1000)) { throw "Environment verification timed out: $LogPath" }
    if ($testProcess.ExitCode -ne 0) { throw "Environment verification exited $($testProcess.ExitCode): $LogPath" }
    $testText = Get-Content -LiteralPath $LogPath -Raw
    if ($testText -notmatch 'Authority identity READY providerBound=1' -or $testText -match 'Authority identity unavailable:') {
        throw 'Canonical catalog and active provider identity binding was not confirmed.'
    }
    if ($GpuAllocatorVisualPilot -and $testText -notmatch 'ProductionVisualPilot ALLOCATOR gpu=1 privateProof=1') { throw 'Default GPU allocator publication proof is missing.' }
    if ($testText -match 'ALLOCATOR CROSS-CHECK FAILED|doubleGrant [1-9][0-9]*|badFree [1-9][0-9]*|Error:.*CLAIM STAGE DARK') {
        throw 'GPU allocator or claim proof reported a failure; transaction success alone is insufficient.'
    }
    if ($AssetResolveCacheOnly) {
        $cacheReceipts = [regex]::Matches($testText,'assets RESOLVE \(B\.3\): hits=([0-9]+) inline=([0-9]+) uncached=([0-9]+) \| warm launched=([0-9]+)')
        if (!$cacheReceipts.Count) { throw 'Cache-only resolve instrumentation did not run.' }
        foreach ($receipt in $cacheReceipts) {
            if ($receipt.Groups[4].Value -ne '0') { throw 'Cache-only test unexpectedly launched warm workers.' }
        }
    }
    if ($SurfaceLightingDiagnostic -and ($testText -notmatch 'SurfaceLightingDiagnostic APPLIED enabled=1 volumesOff=1' -or $testText -match 'SurfaceLightingDiagnostic REFUSED')) {
        throw 'Surface lighting diagnostic application with both volume paths disabled was not confirmed.'
    }
    if ($testText -notmatch 'ProductionCandidate PREPARED HIDDEN .*publicationReady=0') { throw 'Hidden preparation evidence is missing.' }
    if ($testText -match 'assets PROBE:') { throw 'Unexpected automatic composition diagnostic.' }
    if ($Mode -eq 'Rehearse') {
        if ($testText -notmatch 'ProductionHandoff REHEARSAL PASSED .*allocatorPinned=0 publicationReady=0' -or
            $testText -notmatch 'REHEARSAL RELEASED reason=successful observation-only rehearsal') { throw 'Successful rehearsal/release evidence is missing.' }
        if ($testText -match 'REHEARSAL REFUSED') { throw 'Rehearsal reported a refusal.' }
    }
    if ($Mode -eq 'HeldCpu') {
        if ($testText -notmatch 'ProductionHeldCpu PASSED .*cpuOnly=1 backendReady=0 publicationReady=0' -or
            $testText -notmatch 'REHEARSAL RELEASED reason=CPU-only private packs validated and discarded') { throw 'Successful CPU preparation/release evidence is missing.' }
        if ($testText -match 'ProductionHeldCpu REFUSED|REHEARSAL REFUSED') { throw 'CPU preparation reported a refusal.' }
    }
    if ($Mode -eq 'HeldGpu') {
        if ($testText -notmatch 'ProductionHeldGpu PASSED .*parity=all-pages .*backendReady=0 publicationReady=0' -or
            $testText -notmatch 'REHEARSAL RELEASED reason=private CPU/GPU packs matched and discarded') { throw 'Successful GPU preparation/parity/release evidence is missing.' }
        if ($testText -match 'ProductionHeldGpu REFUSED|ProductionHeldCpu REFUSED|REHEARSAL REFUSED') { throw 'GPU preparation reported a refusal.' }
    }
    if ($Mode -eq 'VisualPilot') {
        if ($testText -notmatch 'Environment material preparation READY materials=[1-3] .*intendedProxy=1') {
            throw 'Intended material readiness evidence is missing.'
        }
        if ($testText -notmatch 'ProductionVisualPilot PUBLISHED .*registry=isolated gameplay=0' -or
            $testText -match 'ProductionVisualPilot REFUSED|ProductionHeldGpu REFUSED|ProductionHeldCpu REFUSED|REHEARSAL REFUSED') {
            throw 'Successful visual-only publication evidence is missing or a refusal occurred.'
        }
        if ($testText -notmatch 'ProductionVisualPilot CAPTURED before="([^"]+)" after="([^"]+)"; colorOnly=1') {
            throw 'Before/after color capture evidence is missing.'
        }
        $capturePaths = @($Matches[1],$Matches[2])
        if ($testText -notmatch 'ProductionVisualPilot STEADY_CAPTURED path="([^"]+)" frame=([0-9]+) elapsed=([0-9.]+)') {
            throw 'Delayed steady-state color capture evidence is missing.'
        }
        if ([double]::Parse($Matches[3],[Globalization.CultureInfo]::InvariantCulture) -lt 10) {
            throw 'Steady-state capture occurred before the required settling interval.'
        }
        $capturePaths += $Matches[1]
        if ($testText -notmatch 'ProductionVisualPilot UNLIT_CAPTURED path="([^"]+)"; actualViewMode=Unlit gameplay=0') {
            throw 'Unlit diagnostic capture evidence is missing.'
        }
        $capturePaths += $Matches[1]
        if ($CaptureBuffers) {
            $buffers = [regex]::Matches($testText,'ProductionVisualPilot BUFFER_CAPTURED stage=(before|after|steady) target=(BaseColor|WorldNormal|SceneDepthWorldUnits) path="([^"]+)"; format=EXR viewportWidth=960 viewportHeight=540')
            if ($buffers.Count -ne 9) { throw 'Expected exactly nine verified EXR buffer sidecars.' }
            $seenBuffers = @{}
            foreach ($buffer in $buffers) {
                $identity = $buffer.Groups[1].Value + '/' + $buffer.Groups[2].Value
                if ($seenBuffers.ContainsKey($identity)) { throw "Duplicate buffer: $identity" }
                $seenBuffers[$identity] = $true
                $capturePaths += $buffer.Groups[3].Value
            }
        }
        foreach ($capturePath in $capturePaths) {
            if (!(Test-Path -LiteralPath $capturePath -PathType Leaf) -or (Get-Item -LiteralPath $capturePath).Length -eq 0) {
                throw "Capture file is missing or empty: $capturePath"
            }
        }
        Write-Output ('Color captures: ' + ($capturePaths -join ', '))
        Write-Output 'Capture files require visual review; continuous-frame depth/shadow and gameplay acceptance remain outstanding.'
    }
    if ($Mode -eq 'VisualPilot') { Write-Output 'PASS: transaction and capture-file checks, normal exit0. Visual acceptance requires image review.' }
    else { Write-Output "PASS: $Mode completed with normal process exit0." }
}
finally {
    $testProcess.Refresh()
    if (!$testProcess.HasExited) { Stop-Process -Id $testProcess.Id -Force }
}

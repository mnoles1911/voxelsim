<#
.SYNOPSIS
  Records pipeline-state (PSO) data from a scripted flight leg and, for a
  packaged client, builds the bundled shader pipeline cache from it.

.DESCRIPTION
  PHASE 4, 2026-09-07. Written to serve the owner's requirement that the
  loading screen never feel chunky: part of the first minute's hitching is
  pipeline-state creation on the render thread the first time each
  material/vertex-factory pair is drawn.

  READ THIS BEFORE RUNNING IT. There are TWO engine mechanisms here and they
  do not have the same reach:

    -Mode Precache   RUNS TODAY, on the editor binary, uncooked, exactly the
                     way every leg on this box already runs. Flies a leg with
                     PSO precaching armed and reports the engine's own
                     precache counters. This is the arm that can actually be
                     measured on this project as it stands.

    -Mode Bundle     REQUIRES A COOKED, PACKAGED WIN64 CLIENT, and refuses
                     without one. The shader pipeline cache is unreachable
                     from UnrealEditor(-Cmd).exe for two independent reasons,
                     both of them engine source, neither of them fixable from
                     an ini:
                       * r.ShaderPipelineCache.Enabled defaults to
                         PIPELINE_CACHE_DEFAULT_ENABLED = (!WITH_EDITOR)
                         -- D:/UE_5.8/.../RHI/Public/PipelineFileCache.h:19
                       * FShaderPipelineCache::Initialize -- the ONLY caller of
                         FPipelineFileCacheManager::Initialize
                         (RenderCore/Private/ShaderPipelineCache.cpp:772) -- is
                         compiled behind #if !UE_EDITOR and additionally
                         requires FPlatformProperties::RequiresCookedData()
                         -- Launch/Private/LaunchEngineLoop.cpp:3196-3205.
                     So there is no "just set the cvar" path. A leg run out of
                     the editor binary with -logpso logs nothing and produces
                     an empty cache, which looks exactly like a working cache
                     that found nothing to do. This script refuses instead.

  AND ONE THING NEITHER MODE FIXES, said plainly so it is not claimed: the
  ~50 s renderWaitMs on frame 1 of a cold launch, with "Preparing Shaders" on
  screen, is the EDITOR SHADER COMPILER filling the DDC. It is not PSO
  creation and no pipeline cache touches it. A warm DDC or a cooked build is
  the answer to that one.

.NOTES
  DO NOT RUN THIS WHILE ANOTHER EDITOR OR A BUILD IS LIVE. It shells out to
  tools/voxel-run-flight-leg.ps1, which refuses on its own if the box is busy
  (that refusal is the house rule, not a courtesy).

.EXAMPLE
  # The arm that runs today.
  tools\voxel-pso-cache-record.ps1 -Mode Precache -LogName pso-precache-1

.EXAMPLE
  # The packaged arm, once a cooked client exists.
  tools\voxel-pso-cache-record.ps1 -Mode Bundle `
      -PackagedDir D:\voxelsim\Packaged\Windows -LogName pso-record-1
#>
[CmdletBinding()]
param(
    [ValidateSet('Precache', 'Bundle')]
    [string]$Mode = 'Precache',

    [string]$LogName = 'pso-cache-record',

    # The flight the recording leg flies. The cache is only as good as the
    # material/vertex-factory coverage of the run that recorded it, so this
    # should be the route a player actually takes -- the kept vista spawn, at
    # speed, through terrain, water and sky.
    [string]$SpawnAt = '-56940,-56610',
    [string]$Flight = 'line',
    [int]$RunSec = 180,
    [int]$PreflightSec = 90,
    [int]$LingerSec = 30,

    # -Mode Bundle only: the staged build directory that contains
    # <Project>.exe (i.e. the output of BuildCookRun -stage).
    [string]$PackagedDir = '',

    [string]$Engine = 'D:\UE_5.8',
    [switch]$KeepIntermediates
)

$ErrorActionPreference = 'Stop'
$RepoRoot   = Split-Path -Parent $PSScriptRoot
$Project    = Join-Path $RepoRoot 'ue-project\VoxelEarth.uproject'
$EditorCmd  = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$LegRunner  = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$SavedRoot  = Join-Path $RepoRoot 'ue-project\Saved'

if (-not (Test-Path $Project))   { throw "No project at $Project" }
if (-not (Test-Path $LegRunner)) { throw "No leg runner at $LegRunner" }

# ---------------------------------------------------------------------------
# MODE: Precache -- the arm that runs on this box today.
# ---------------------------------------------------------------------------
if ($Mode -eq 'Precache') {
    Write-Host '=== PSO PRECACHE LEG (editor binary, uncooked) ===' -ForegroundColor Cyan
    Write-Host 'This measures r.PSOPrecache.* -- the runtime mechanism. It does NOT'
    Write-Host 'produce a .upipelinecache; see the header for why that needs a cooked build.'

    # r.PSOPrecache.Resources and .ProxyCreationStrategy are ECVF_ReadOnly and
    # are set from DefaultEngine.ini [SystemSettings]; passing them here would
    # be ignored. What -ExecCmds CAN do is turn the reporting on.
    $cvars = @(
        'r.PSOPrecache.DumpMaterialStats 1'
        'r.PSOPrecaching 1'
    ) -join ','

    & powershell -NoProfile -ExecutionPolicy Bypass -File $LegRunner `
        -LogName $LogName -Flight $Flight -SpawnAt $SpawnAt `
        -RunSec $RunSec -PreflightSec $PreflightSec -LingerSec $LingerSec `
        -Cvars $cvars `
        -ExtraArgs @('-VoxelFramePhase=1')
    $legExit = $LASTEXITCODE
    if ($legExit -ne 0) {
        Write-Host "LEG FAILED (exit $legExit) -- VOID, read $RepoRoot\Saved\$LogName.log" -ForegroundColor Red
        exit $legExit
    }

    $log = Join-Path $RepoRoot "Saved\$LogName.log"
    Write-Host ''
    Write-Host '--- PSO precache lines from the leg ---' -ForegroundColor Cyan
    Select-String -Path $log -Pattern 'PSOPrecache|PSO precache|PipelineStateCache' |
        Select-Object -First 40 | ForEach-Object { $_.Line }
    Write-Host ''
    Write-Host 'READING RULE: zero PSOPrecache lines does NOT mean zero precaching --'
    Write-Host 'it means the reporting cvar did not take. Check the log for the'
    Write-Host 'r.PSOPrecache.DumpMaterialStats echo before drawing any conclusion.'
    exit 0
}

# ---------------------------------------------------------------------------
# MODE: Bundle -- the packaged-client workflow.
# ---------------------------------------------------------------------------
Write-Host '=== BUNDLED SHADER PIPELINE CACHE (cooked client required) ===' -ForegroundColor Cyan

if (-not $PackagedDir -or -not (Test-Path $PackagedDir)) {
    Write-Host ''
    Write-Host 'REFUSING: -Mode Bundle needs a cooked, packaged Win64 client and -PackagedDir does not name one.' -ForegroundColor Red
    Write-Host ''
    Write-Host 'This is not a missing flag, it is an engine gate. The pipeline file cache is'
    Write-Host 'never initialised in the editor binary:'
    Write-Host '  * r.ShaderPipelineCache.Enabled defaults to (!WITH_EDITOR)  -- PipelineFileCache.h:19'
    Write-Host '  * FShaderPipelineCache::Initialize is behind #if !UE_EDITOR and'
    Write-Host '    FPlatformProperties::RequiresCookedData()               -- LaunchEngineLoop.cpp:3196'
    Write-Host 'so a recording leg out of UnrealEditor-Cmd.exe logs nothing at all and would'
    Write-Host 'hand you an empty cache that is indistinguishable from a working one.'
    Write-Host ''
    Write-Host 'To make one:'
    Write-Host "  $Engine\Engine\Build\BatchFiles\RunUAT.bat BuildCookRun ``"
    Write-Host "     -project=$Project -platform=Win64 -clientconfig=Development ``"
    Write-Host '     -cook -stage -pak -build -utf8output'
    Write-Host '  (Config/DefaultGame.ini already sets bShareMaterialShaderCode=True, which'
    Write-Host '   the cache requires -- ShaderPipelineCache.h:76.)'
    Write-Host ''
    Write-Host 'Then re-run with -PackagedDir <staged dir containing VoxelEarth.exe>.'
    exit 2
}

$clientExe = Get-ChildItem -Path $PackagedDir -Filter 'VoxelEarth*.exe' -Recurse -File |
             Select-Object -First 1
if (-not $clientExe) { throw "No VoxelEarth*.exe under $PackagedDir" }
Write-Host "Client: $($clientExe.FullName)"

# --- 1. Record ------------------------------------------------------------
# The recorded log lands in the client's own Saved/CollectedPSOs. LogPSO=1 is
# what makes the runtime record new PSOs; SaveAfterPSOsLogged forces periodic
# saves so a crash does not lose the run (ShaderPipelineCache.h:57-59).
$recordLog = Join-Path $SavedRoot "$LogName-record.log"
$recordArgs = @(
    '-game', '-nosplash', '-unattended', '-sm6', '-dx12'
    "-abslog=$recordLog"
    '-ResX=2560', '-ResY=1440', '-WinX=0', '-WinY=0'
    "-VoxelSpawnAt=$SpawnAt"
    "-VoxelPerfRun=$RunSec"
    "-VoxelPerfFlight=$Flight"
    "-VoxelPerfPreflightSec=$PreflightSec"
    "-VoxelPerfLingerSec=$LingerSec"
    '-logPSO'
    '-ExecCmds="r.ShaderPipelineCache.Enabled 1,r.ShaderPipelineCache.LogPSO 1,r.ShaderPipelineCache.SaveAfterPSOsLogged 500"'
)
Write-Host ''
Write-Host '--- 1/3 recording leg ---' -ForegroundColor Cyan
Write-Host "  $($clientExe.FullName) $($recordArgs -join ' ')"
$p = Start-Process -FilePath $clientExe.FullName -ArgumentList $recordArgs -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit((($PreflightSec + $RunSec + $LingerSec) + 240) * 1000)) {
    Stop-Process -Id $p.Id -Force
    throw 'Recording leg timed out -- VOID.'
}

# The runtime writes <ClientSaved>/CollectedPSOs/*.rec.upipelinecache; the
# staged tree's Saved is under the packaged directory, so search the whole of
# it rather than guessing the depth of the exe.
$collected = @(Get-ChildItem -Path $PackagedDir -Filter '*.rec.upipelinecache' -Recurse -File -ErrorAction SilentlyContinue)
if ($collected.Count -eq 0) {
    Write-Host 'NO *.rec.upipelinecache WAS PRODUCED.' -ForegroundColor Red
    Write-Host 'That is a FAILED record, not an empty world: check the record log for'
    Write-Host '"ShaderPipelineCache" lines and confirm the build was cooked with'
    Write-Host 'Share Material Shader Code enabled.'
    exit 3
}
Write-Host "Recorded $($collected.Count) file(s):"
$collected | ForEach-Object { Write-Host "  $($_.FullName) ($([int]($_.Length/1KB)) KB)" }

# --- 2. Expand + Build ----------------------------------------------------
# The two-pass ShaderPipelineCacheTools workflow
# (Editor/UnrealEd/Private/Commandlets/ShaderPipelineCacheToolsCommandlet.cpp:2899, :2904):
#   Expand <recorded...> <.shk stable keys...> <out.stablepc.csv>
#   Build  <in.stablepc.csv> <.shk stable keys...> <out.upipelinecache>
# The .shk stable-key files are a cook output; they live under the cooked
# metadata directory of the same cook that produced the client.
$shk = @(Get-ChildItem -Path $RepoRoot -Filter '*.shk' -Recurse -File -ErrorAction SilentlyContinue)
if ($shk.Count -eq 0) {
    Write-Host 'NO *.shk STABLE-KEY FILES FOUND.' -ForegroundColor Red
    Write-Host 'The commandlet cannot expand a recording without them; they are produced by'
    Write-Host 'the same cook as the client. Re-cook with Share Material Shader Code on.'
    exit 4
}

$work = Join-Path $SavedRoot 'PSOCache'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$stablePc = Join-Path $work "$LogName.stablepc.csv"
# NAME IT THE WAY THE RUNTIME LOOKS IT UP: <LastOpened>_<ShaderPlatform>.
# LastOpened=VoxelEarth is set in Config/DefaultGame.ini
# [ShaderPipelineCache.CacheFile]; the shader platform for a Win64 DX12 SM6
# client is PCD3D_SM6. A cache under any other name is loaded by nobody and
# reports no error.
$outCache = Join-Path $work 'VoxelEarth_PCD3D_SM6.upipelinecache' 

Write-Host ''
Write-Host '--- 2/3 Expand ---' -ForegroundColor Cyan
& $EditorCmd $Project -run=ShaderPipelineCacheTools Expand `
    @($collected.FullName) @($shk.FullName) $stablePc
if ($LASTEXITCODE -ne 0) { throw "Expand failed ($LASTEXITCODE)" }

Write-Host ''
Write-Host '--- 3/3 Build ---' -ForegroundColor Cyan
& $EditorCmd $Project -run=ShaderPipelineCacheTools Build `
    $stablePc @($shk.FullName) $outCache
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

# --- 3. Put it where the runtime and the stager look ----------------------
# Content/PipelineCaches/<PlatformDir>/*.upipelinecache is what
# CopyBuildToStagingDirectory.Automation.cs:2058 stages. NOT
# Build/<Platform>/PipelineCaches -- that is the UE4 location and files left
# there are silently not staged.
$dest = Join-Path $RepoRoot 'ue-project\Content\PipelineCaches\Windows'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Path $outCache -Destination $dest -Force
Write-Host ''
Write-Host "BUNDLED CACHE: $(Join-Path $dest (Split-Path -Leaf $outCache))" -ForegroundColor Green
Write-Host 'It is only picked up by a build cooked AFTER this point; the next cook stages it.'

if (-not $KeepIntermediates) {
    Remove-Item -Path $stablePc -Force -ErrorAction SilentlyContinue
}
exit 0

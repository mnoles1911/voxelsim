# voxel-check-worklist-shader.ps1 -- do the P3 worklist kernels compile?
#
# WHY THIS EXISTS: voxel-check-brickpack-shader.ps1's reason verbatim. The
# editor compiles global shaders at boot, so a half-finished .usf is live input
# to the next leg in a way a half-finished .cpp is not -- and the worklist adds
# two boot-compiled kernels (VoxelWorklistArgs.usf, VoxelWorklistConsume.usf)
# plus the shared record header (VoxelWorklist.ush) that any converted
# generation kernel will include. A boot-time error in any of them costs a
# whole leg to discover; this costs ~2 seconds and no editor.
#
# LIMIT, STATED (same as the sibling scripts): this proves the HLSL compiles
# under DXC with Platform.ush stubbed. It is "the tree is not mid-edit", not a
# substitute for the editor's own compile.
#
# No SPIR-V arm: these kernels are engine-side only (dispatched through RDG,
# never by the Vulkan bench), so DXIL is the one target that exists for them.

param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'
if (-not $Root) {
    $Probe = $PSScriptRoot
    while ($Probe -and -not (Test-Path (Join-Path $Probe 'ue-project'))) {
        $Parent = Split-Path $Probe -Parent
        if ($Parent -eq $Probe) { break }
        $Probe = $Parent
    }
    if ($Probe -and (Test-Path (Join-Path $Probe 'ue-project'))) { $Root = $Probe }
    else { $Root = 'D:\voxelsim' }
}
$Root = (Resolve-Path $Root).Path
$Dxc = Join-Path $Root 'tools\dxc\bin\x64\dxc.exe'
# A worktree checkout has no fetched dxc; fall back to the main checkout's.
if (-not (Test-Path $Dxc)) { $Dxc = 'D:\voxelsim\tools\dxc\bin\x64\dxc.exe' }
$Ush = Join-Path $Root 'ue-project\Shaders\VoxelWorklist.ush'
$ArgsUsf = Join-Path $Root 'ue-project\Shaders\VoxelWorklistArgs.usf'
$ConsumeUsf = Join-Path $Root 'ue-project\Shaders\VoxelWorklistConsume.usf'
$ColumnUsf = Join-Path $Root 'ue-project\Shaders\VoxelWorklistColumn.usf'
$VoxelizeUsf = Join-Path $Root 'ue-project\Shaders\VoxelWorklistVoxelize.usf'
$WorldGen = Join-Path $Root 'voxel-core\shaders\worldgen.ush'
$Stage = Join-Path $env:TEMP 'voxel-worklist-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc (run tools/fetch-dxc.ps1)" }
if (-not (Test-Path $Ush)) { throw "VoxelWorklist.ush not found at $Ush" }
if (-not (Test-Path $ArgsUsf)) { throw "VoxelWorklistArgs.usf not found at $ArgsUsf" }
if (-not (Test-Path $ConsumeUsf)) { throw "VoxelWorklistConsume.usf not found at $ConsumeUsf" }
if (-not (Test-Path $ColumnUsf)) { throw "VoxelWorklistColumn.usf not found at $ColumnUsf" }
if (-not (Test-Path $VoxelizeUsf)) { throw "VoxelWorklistVoxelize.usf not found at $VoxelizeUsf" }
if (-not (Test-Path $WorldGen)) { throw "worldgen.ush not found at $WorldGen" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The version lock, read from the file itself so this script cannot drift from
# it (the converted column kernel compiles worldgen.ush, which #errors under
# VXC_UE without a matching VXC_WORLDGEN_VERSION_CPP).
$VersionLine = Select-String -Path $WorldGen -Pattern 'define VXC_WORLDGEN_VERSION_USH (\d+)'
if (-not $VersionLine) { throw "VXC_WORLDGEN_VERSION_USH not found in $WorldGen" }
$WorldGenVersion = $VersionLine.Matches[0].Groups[1].Value

# The virtual include paths only resolve inside the engine; stage flat copies
# with the includes rewritten. Nothing in the source tree is modified.
Copy-Item $Ush (Join-Path $Stage 'VoxelWorklist.ush')
Copy-Item $WorldGen (Join-Path $Stage 'worldgen.ush')
(Get-Content $ArgsUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub') |
    Out-File (Join-Path $Stage 'VoxelWorklistArgs.hlsl') -Encoding utf8
(Get-Content $ConsumeUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelWorklist.ush"', '"VoxelWorklist.ush"') |
    Out-File (Join-Path $Stage 'VoxelWorklistConsume.hlsl') -Encoding utf8
(Get-Content $ColumnUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelWorklist.ush"', '"VoxelWorklist.ush"').
    Replace('"/VoxelCore/worldgen.ush"', '"worldgen.ush"') |
    Out-File (Join-Path $Stage 'VoxelWorklistColumn.hlsl') -Encoding utf8
(Get-Content $VoxelizeUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelWorklist.ush"', '"VoxelWorklist.ush"').
    Replace('"/VoxelCore/worldgen.ush"', '"worldgen.ush"') |
    Out-File (Join-Path $Stage 'VoxelWorklistVoxelize.hlsl') -Encoding utf8

$Out = Join-Path $Stage 'out.bin'
$Fail = 0
$Total = 0

function Try-Compile($Src, $EntryPoint, $Label, [string[]]$Defines = @()) {
    $ArgList = @('-T', 'cs_6_0', '-E', $EntryPoint, '-HV', '2021', '-O3', '-Fo', $script:Out, $Src)
    foreach ($D in $Defines) { $ArgList += @('-D', $D) }
    # stderr to a file, not 2>&1: PowerShell 5.1 wraps native stderr lines in
    # NativeCommandError and aborts the run on the first failing kernel.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }
    if ($Failed) {
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistArgs.hlsl')    'WorklistArgsMain'    'args    DXIL  WorklistArgsMain'
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistConsume.hlsl') 'WorklistConsumeMain' 'consume DXIL  WorklistConsumeMain'

# The converted Column stage (VoxelWorklistColumn.usf): compiles worldgen.ush
# under VXC_UE + VXC_RASTER_ATLAS + the stage-shape defines the host class
# sets. The defines mirror FVoxelWorklistColumnCS::ModifyCompilationEnvironment
# (16 groups / 1,024 columns per record); the kernel #errors on disagreement,
# so a shape drift fails HERE, not at editor boot.
$ColumnDefines = @('VXC_UE=1', "VXC_WORLDGEN_VERSION_CPP=$WorldGenVersion",
                   'VXC_RASTER_ATLAS=1',
                   'VXC_WORKLIST_COLUMN_GROUPS=16', 'VXC_WORKLIST_COLS_PER_RECORD=1024')
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistColumn.hlsl') 'ColumnWorklistMain'       'column  DXIL  ColumnWorklistMain'       $ColumnDefines
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistColumn.hlsl') 'ColumnWorklistVerifyMain' 'colvfy  DXIL  ColumnWorklistVerifyMain' $ColumnDefines

# The converted Voxelize stage (VoxelWorklistVoxelize.usf; P3 stage 2). Same
# mechanism: the defines mirror FVoxelWorklistVoxelizeCS's (16 groups per
# record -- one thread per COLUMN, the classic mapping -- over 1,024 columns /
# 32,768 cells), and the kernel #errors on disagreement.
$VoxelizeDefines = @('VXC_UE=1', "VXC_WORLDGEN_VERSION_CPP=$WorldGenVersion",
                     'VXC_RASTER_ATLAS=1',
                     'VXC_WORKLIST_VOXELIZE_GROUPS=16', 'VXC_WORKLIST_COLS_PER_RECORD=1024',
                     'VXC_WORKLIST_CELLS_PER_RECORD=32768')
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistVoxelize.hlsl') 'VoxelizeWorklistMain'       'voxlize DXIL  VoxelizeWorklistMain'       $VoxelizeDefines
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'VoxelWorklistVoxelize.hlsl') 'VoxelizeWorklistVerifyMain' 'voxvfy  DXIL  VoxelizeWorklistVerifyMain' $VoxelizeDefines

# The factored classic kernels, both atlas permutations -- the factoring must
# not have broken the shipped forms (the digest gate proves bytes; this proves
# the compile before a leg is spent on it).
$UeDefines = @('VXC_UE=1', "VXC_WORLDGEN_VERSION_CPP=$WorldGenVersion")
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'ColumnMain'   'wg      DXIL  ColumnMain (VXC_UE)'            $UeDefines
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'ColumnMain'   'wg      DXIL  ColumnMain (VXC_UE, atlas)'     ($UeDefines + 'VXC_RASTER_ATLAS=1')
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'VoxelizeMain' 'wg      DXIL  VoxelizeMain (VXC_UE)'          $UeDefines
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'VoxelizeMain' 'wg      DXIL  VoxelizeMain (VXC_UE, atlas)'   ($UeDefines + 'VXC_RASTER_ATLAS=1')
# And the bench form (explicit cbuffer, no VXC_UE) -- worldgen.ush is shared
# with the standalone Vulkan bench, and an edit that only compiles under
# VXC_UE would break it silently.
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'ColumnMain'   'wg      DXIL  ColumnMain (bench form)'        @("VXC_WORLDGEN_VERSION_CPP=$WorldGenVersion")
$Total += 1; $Fail += Try-Compile (Join-Path $Stage 'worldgen.ush') 'VoxelizeMain' 'wg      DXIL  VoxelizeMain (bench form)'      @("VXC_WORLDGEN_VERSION_CPP=$WorldGenVersion")

Write-Host ''
if ($Fail -gt 0) {
    Write-Host "$Fail of $Total worklist kernels FAILED -- do not start a leg on this tree."
    exit 1
}
Write-Host "All $Total worklist kernels compile."
exit 0

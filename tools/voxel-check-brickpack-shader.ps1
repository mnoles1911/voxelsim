# voxel-check-brickpack-shader.ps1 -- does the BrickPack chain compile, in EVERY
# permutation?
#
# WHY THIS EXISTS. ue-project/Shaders is shared, and the editor compiles global
# shaders at boot -- so a half-finished .usf is live input to the next leg, in a
# way a half-finished .cpp is not. A boot-time global-shader error costs a whole
# leg to discover.
#
# AND WHY IT CHECKS ALL OF THEM. The error that voided two legs ("conditional
# operator only supports results with numeric scalar, vector, or matrix types")
# fired on PERMUTATION 2 ONLY. Compiling one permutation passes it happily.
# This compiles all eight:
#   * brickpack.ush standalone (explicit register slots)  -> DXIL and SPIR-V,
#     both entry points. SPIR-V is compiled with the same -fvk-*-shift mapping
#     tools/compile-shaders.ps1 uses, which is what proves the register slots in
#     the file land on DISTINCT Vulkan bindings rather than colliding.
#   * brickpack.ush under VXC_UE (loose globals, no register slots) -> DXIL,
#     both entry points. This is the declaration form Unreal actually compiles.
#   * VoxelBrickPack.usf, the thin Unreal entry file, under VXC_UE -> DXIL, both
#     entry points -- so a broken #include is caught here and not at boot.
#   * VoxelBrickPoolWrite.usf (P1-C / P2), all FIVE entry points -> DXIL. Not a
#     brickpack.ush consumer -- it interprets no format field but the 28-bit
#     offset and the 2-bit kind -- but it is compiled at boot alongside them and
#     a half-finished edit to it kills a leg in exactly the same way.
#
# BrickPack is NOT added to tools/compile-shaders.ps1 on purpose: that script's
# outputs are staleness-compared against voxel-core/shaders/prebuilt, which is
# the bytecode vxc_gpu loads, and a never-dispatched kernel does not belong in
# among the worldgen bytecode the determinism digest was recorded against. Same
# reasoning that keeps the material-palette check out of $OutDir there.
#
# No editor, no engine, ~2 seconds. It uses tools/dxc, the same compiler the
# determinism gate uses.
#
# LIMIT, STATED: this proves the HLSL compiles. It does not prove UE's own
# preprocessor environment agrees, and it stubs /Engine/Public/Platform.ush
# (brickpack.ush deliberately uses nothing from it). A clean run here is "the
# tree is not mid-edit", which is exactly the question a leg needs answered --
# it is not a substitute for the editor's own compile.

param(
    # Repo root. Defaults to walking up from this script until ue-project is
    # found, so the script works from tools/, from the repo root, or from a
    # scratch directory -- it does not care where it is kept.
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
$Kernel = Join-Path $Root 'voxel-core\Shaders\brickpack.ush'
if (-not (Test-Path $Kernel)) { $Kernel = Join-Path $Root 'voxel-core\shaders\brickpack.ush' }
$Entry = Join-Path $Root 'ue-project\Shaders\VoxelBrickPack.usf'
$PoolWrite = Join-Path $Root 'ue-project\Shaders\VoxelBrickPoolWrite.usf'
$Stage = Join-Path $env:TEMP 'voxel-brickpack-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc" }
if (-not (Test-Path $Kernel)) { throw "brickpack.ush not found at $Kernel" }
if (-not (Test-Path $Entry)) { throw "VoxelBrickPack.usf not found at $Entry" }
if (-not (Test-Path $PoolWrite)) { throw "VoxelBrickPoolWrite.usf not found at $PoolWrite" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine, so stage a flat copy
# of the entry file with the includes rewritten. Nothing is modified in the
# source tree; the kernel itself is compiled where it lives.
Copy-Item $Kernel (Join-Path $Stage 'brickpack.ush')
(Get-Content $Entry -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelCore/brickpack.ush"', '"brickpack.ush"') |
    Out-File (Join-Path $Stage 'VoxelBrickPack.hlsl') -Encoding utf8
(Get-Content $PoolWrite -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub') |
    Out-File (Join-Path $Stage 'VoxelBrickPoolWrite.hlsl') -Encoding utf8

$Out = Join-Path $Stage 'out.bin'
$Fail = 0
$Total = 0

function Try-Compile($Src, $EntryPoint, $Defines, $Spirv, $Label) {
    $ArgList = @('-T', 'cs_6_0', '-E', $EntryPoint, '-HV', '2021', '-O3', '-Fo', $script:Out)
    if ($Spirv) {
        # Same mapping tools/compile-shaders.ps1 uses: HLSL register classes are
        # separate namespaces but Vulkan bindings are one flat space per set, so
        # without these shifts b0/t0/u0 would all collide at binding 0. Running
        # it here is what checks brickpack.ush's slot spacing.
        $ArgList += @('-spirv', '-fspv-target-env=vulkan1.1',
                      '-fvk-b-shift', '0', '0', '-fvk-t-shift', '1', '0',
                      '-fvk-u-shift', '3', '0')
    }
    foreach ($d in $Defines) { $ArgList += @('-D', $d) }
    $ArgList += $Src
    # NOT `2>&1` on a native exe: Windows PowerShell 5.1 wraps each stderr line
    # in a NativeCommandError, which with $ErrorActionPreference='Stop' aborts
    # the whole run on the FIRST failing permutation -- so a tree with three
    # broken permutations would report one. stderr goes to a file instead, and
    # the preference is dropped to Continue across the call for the same reason.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }
    if ($Failed) {
        # Write-Host, not Write-Output: in PowerShell a function returns
        # everything on the output stream, so a chatty Write-Output here turns
        # the return value into an array and `$Fail += ...` fails with
        # op_Addition. Messages go to the host; only the tally is returned.
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 10 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

$StagedKernel = Join-Path $Stage 'brickpack.ush'
$StagedEntry = Join-Path $Stage 'VoxelBrickPack.hlsl'

$StagedPoolWrite = Join-Path $Stage 'VoxelBrickPoolWrite.hlsl'

foreach ($E in 'BrickClassifyMain', 'BrickPackMain') {
    $Total += 1; $Fail += Try-Compile $StagedKernel $E @()          $false "kernel  standalone DXIL   $E"
    $Total += 1; $Fail += Try-Compile $StagedKernel $E @()          $true  "kernel  standalone SPIRV  $E"
    $Total += 1; $Fail += Try-Compile $StagedKernel $E @('VXC_UE=1') $false "kernel  VXC_UE     DXIL   $E"
    $Total += 1; $Fail += Try-Compile $StagedEntry  $E @('VXC_UE=1') $false "usf     VXC_UE     DXIL   $E"
}

# The pool-write kernels. One permutation each and no VXC_UE: they take no
# defines at all, which is the point -- they sit outside every version lock.
foreach ($E in 'BrickTotalMain', 'BrickWordCopyMain', 'BrickDescPoolWriteMain',
               'BrickChunkRecordMain', 'BrickChunkClearMain') {
    $Total += 1; $Fail += Try-Compile $StagedPoolWrite $E @() $false "poolwrite          DXIL   $E"
}

if ($Fail -gt 0) {
    Write-Output ""
    Write-Output "BRICKPACK SHADER TREE IS MID-EDIT: $Fail of $Total permutations do not compile."
    Write-Output "Do not start a leg -- it will die at boot on a global-shader error."
    exit 1
}
Write-Output ""
Write-Output "all $Total permutations compile -- the BrickPack shader tree is safe to boot."
exit 0

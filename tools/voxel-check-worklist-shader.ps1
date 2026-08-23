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
$Stage = Join-Path $env:TEMP 'voxel-worklist-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc (run tools/fetch-dxc.ps1)" }
if (-not (Test-Path $Ush)) { throw "VoxelWorklist.ush not found at $Ush" }
if (-not (Test-Path $ArgsUsf)) { throw "VoxelWorklistArgs.usf not found at $ArgsUsf" }
if (-not (Test-Path $ConsumeUsf)) { throw "VoxelWorklistConsume.usf not found at $ConsumeUsf" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine; stage flat copies
# with the includes rewritten. Nothing in the source tree is modified.
Copy-Item $Ush (Join-Path $Stage 'VoxelWorklist.ush')
(Get-Content $ArgsUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub') |
    Out-File (Join-Path $Stage 'VoxelWorklistArgs.hlsl') -Encoding utf8
(Get-Content $ConsumeUsf -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub').
    Replace('"/VoxelEarth/VoxelWorklist.ush"', '"VoxelWorklist.ush"') |
    Out-File (Join-Path $Stage 'VoxelWorklistConsume.hlsl') -Encoding utf8

$Out = Join-Path $Stage 'out.bin'
$Fail = 0
$Total = 0

function Try-Compile($Src, $EntryPoint, $Label) {
    $ArgList = @('-T', 'cs_6_0', '-E', $EntryPoint, '-HV', '2021', '-O3', '-Fo', $script:Out, $Src)
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

Write-Host ''
if ($Fail -gt 0) {
    Write-Host "$Fail of $Total worklist kernels FAILED -- do not start a leg on this tree."
    exit 1
}
Write-Host "All $Total worklist kernels compile."
exit 0

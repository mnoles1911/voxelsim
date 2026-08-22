# voxel-check-shadowmarch-shader.ps1 -- does VoxelShadowMarch.usf compile, in
# every permutation?
#
# THE SAME REASON tools\voxel-check-marchdraw-shader.ps1 EXISTS, and the same
# rule: ue-project\Shaders is shared, the editor compiles global shaders AT
# BOOT, and a half-finished .usf there does not fail a build -- it fails an
# editor launch and costs a whole leg. Run this before handing the tree to a
# build slot.
#
# UNLIKE the emit kernels this file checks FOR REAL: VoxelShadowMarch.usf has
# NO ENGINE INCLUDES by design (it decodes the GBuffer normal itself and
# restates ConvertFromDeviceZ over its own uniform), so what dxc compiles here
# is byte-for-byte the code UE's compiler will see, minus only UE's
# preprocessor environment. Three compiles:
#
#     march  CS   x1   VOXEL_MARCH_SOURCE=1
#     verify CS   x1   VOXEL_MARCH_SOURCE=1
#     source 0 guard   must FAIL on the #error in VoxelShadowMarch.usf -- a
#                      shadow ray cannot start from an arbitrary depth-buffer
#                      surface inside the 51.2 m fluid box, and the script
#                      fails if that refusal ever stops firing.
#
# No editor, no engine, ~2 seconds. Uses tools\dxc, the same compiler the
# determinism gate uses.

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
$Shaders = Join-Path $Root 'ue-project\Shaders'
$Stage = Join-Path $env:TEMP 'voxel-shadowmarch-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine, so stage flat
# copies with the includes rewritten. NOTHING IN THE SOURCE TREE IS MODIFIED.
Copy-Item (Join-Path $Shaders 'VoxelFluidContract.ush') $Stage
Copy-Item (Join-Path $Shaders 'VoxelMaterialPalette.ush') $Stage
(Get-Content (Join-Path $Shaders 'VoxelFluidCollision.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidContract.ush"', '"VoxelFluidContract.ush"') |
    Out-File (Join-Path $Stage 'VoxelFluidCollision.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelBrickTraverse.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidCollision.ush"', '"VoxelFluidCollision.ush"').
    Replace('"/VoxelEarth/VoxelMaterialPalette.ush"', '"VoxelMaterialPalette.ush"') |
    Out-File (Join-Path $Stage 'VoxelBrickTraverse.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelShadowMarch.usf') -Raw).
    Replace('#include "/Engine/Public/Platform.ush"', '// platform stub (same as voxel-check-brickpack-shader.ps1)').
    Replace('"/VoxelEarth/VoxelBrickTraverse.ush"', '"VoxelBrickTraverse.ush"') |
    Out-File (Join-Path $Stage 'shadowmarch.hlsl') -Encoding utf8

$Src = Join-Path $Stage 'shadowmarch.hlsl'
$Out = Join-Path $Stage 'out.dxil'
$Fail = 0

function Try-Compile($Profile, $Entry, $Defines, $Label, $ExpectFailure = $false) {
    $ArgList = @('-T', $Profile, '-E', $Entry, '-HV', '2021', '-Fo', $Out)
    foreach ($d in $Defines) { $ArgList += @('-D', $d) }
    $ArgList += $Src
    # NOT `2>&1` on a native exe: Windows PowerShell 5.1 wraps each stderr line
    # in a NativeCommandError, which with $ErrorActionPreference='Stop' aborts
    # on the FIRST failing permutation. stderr goes to a file instead.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }

    if ($ExpectFailure) {
        if ($Failed) { Write-Host "ok    $Label (refused, as designed)"; return 0 }
        Write-Host "FAIL  $Label -- this MUST NOT compile. The source guard in"
        Write-Host "      VoxelShadowMarch.usf is gone, so a source-0 shadow permutation can"
        Write-Host "      now exist by accident and march a volume that cannot contain its rays."
        return 1
    }
    if ($Failed) {
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 8 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

$Fail += Try-Compile 'cs_6_0' 'VoxelShadowMarchMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_SHADOW_TILE=8') 'shadow march  source=1'
$Fail += Try-Compile 'cs_6_0' 'VoxelShadowVerifyMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_SHADOW_TILE=8') 'shadow verify source=1'
$Fail += Try-Compile 'cs_6_0' 'VoxelShadowMarchMain' `
    @('VOXEL_MARCH_SOURCE=0', 'VOXEL_SHADOW_TILE=8') 'source-0 guard' $true

if ($Fail -gt 0) {
    Write-Host ''
    Write-Host "$Fail permutation(s) failed. The shader tree is MID-EDIT: do not boot an"
    Write-Host 'editor or hand this tree to a build slot until this script passes.'
    exit 1
}
Write-Host ''
Write-Host 'All shadow-march permutations compile. The tree is not mid-edit (this proves'
Write-Host "nothing about UE's own preprocessor environment -- the first real compile is"
Write-Host 'the first editor boot after the paired build).'
exit 0

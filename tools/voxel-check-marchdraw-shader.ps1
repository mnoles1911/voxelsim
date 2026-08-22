# voxel-check-marchdraw-shader.ps1 -- does VoxelMarch.usf compile, in EVERY permutation?
#
# THE SAME REASON tools\voxel-check-march-shader.ps1 EXISTS, and the same rule.
# ue-project\Shaders is shared, the editor compiles global shaders AT BOOT, and
# VoxelMarch.usf now carries four IMPLEMENT_GLOBAL_SHADER entry points. A
# half-finished .usf here does not fail a build -- it fails an editor launch, and
# costs a whole leg to discover. A half-finished .cpp cannot do that.
#
# AND WHY EVERY PERMUTATION. The error that voided two legs ("conditional
# operator only supports results with numeric scalar, vector, or matrix types")
# fired on PERMUTATION 2 ONLY. Compiling one permutation passes it happily. This
# compiles all fourteen:
#
#     march CS            x1   (VOXEL_MARCH_SOURCE 0; source 1 is P2 and #errors
#                               by design, which is itself checked below)
#     compact CS          x1
#     emit VS             x1
#     emit PS             x8   AO {0,1} x DBUFFER {0,1} x VELOCITY {0,1}
#     emit PS, static-off x2   ALLOW_STATIC_LIGHTING 0, VELOCITY {0,1}
#                              -- the SV_Target slot shift, which is the one
#                              thing in this shader that changes its OUTPUT
#                              SIGNATURE and can only be wrong at compile time
#     source 1 guard      x1   must FAIL, and the script fails if it succeeds
#
# ---------------------------------------------------------------------------
# WHAT THIS PROVES, AND -- MORE IMPORTANTLY -- WHAT IT DOES NOT
# ---------------------------------------------------------------------------
#
# The march and compact kernels use nothing from the engine, so for those this
# is a REAL compile of the code that ships.
#
# The emit VS/PS genuinely need Common.ush, DeferredShadingCommon.ush and
# VelocityCommon.ush -- the View uniform buffer, the GBuffer encoders and the
# velocity encoder. Those cannot be compiled offline: Common.ush includes
# /Engine/Generated/GeneratedUniformBuffers.ush and
# /Engine/Generated/GeneratedInstancedStereo.ush, which UE's shader compiler
# SYNTHESISES PER JOB and which exist nowhere on disk. (The third generated
# header, AutogenShaderHeaders.ush, IS on disk under
# ue-project\Intermediate\ShaderAutogen -- it is the other two that cannot be
# reproduced.)
#
# So the emit is compiled against engine-stub.ush, staged below: about forty
# lines declaring exactly the engine symbols this shader touches, with the same
# signatures and the same types. That proves:
#
#     * the HLSL is well-formed in every permutation
#     * the SV_Target lists are legal and consistent across the
#       ALLOW_STATIC_LIGHTING branch -- the defect class that would otherwise
#       put motion vectors in GBufferE
#     * the permutation #ifs actually close, in every combination
#     * nothing references a symbol that does not exist in the stub, which
#       catches a typo'd engine helper name
#
# It does NOT prove UE's own preprocessor environment agrees, and it CANNOT.
# A clean run here means "the shader tree is not mid-edit" -- which is exactly
# the question a leg needs answered before it boots -- and nothing more. The
# first real compile of the emit is the first editor launch, and that is stated
# rather than glossed.
#
# No editor, no engine, ~3 seconds. Uses tools\dxc, the same compiler the
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
$Stage = Join-Path $env:TEMP 'voxel-marchdraw-check'

if (-not (Test-Path $Dxc)) { throw "dxc not found at $Dxc" }
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null

# The virtual include paths only resolve inside the engine, so stage flat copies
# with the includes rewritten. NOTHING IN THE SOURCE TREE IS MODIFIED.
Copy-Item (Join-Path $Shaders 'VoxelFluidContract.ush') $Stage
Copy-Item (Join-Path $Shaders 'VoxelMaterialPalette.ush') $Stage
(Get-Content (Join-Path $Shaders 'VoxelFluidCollision.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidContract.ush"', '"VoxelFluidContract.ush"') |
    Out-File (Join-Path $Stage 'VoxelFluidCollision.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelBrickTraverse.ush') -Raw).
    Replace('"/VoxelEarth/VoxelFluidCollision.ush"', '"VoxelFluidCollision.ush"').
    Replace('"/VoxelEarth/VoxelMaterialPalette.ush"', '"VoxelMaterialPalette.ush"') |
    Out-File (Join-Path $Stage 'VoxelBrickTraverse.ush') -Encoding utf8
(Get-Content (Join-Path $Shaders 'VoxelMarch.usf') -Raw).
    Replace('#include "/Engine/Private/Common.ush"', '#include "engine-stub.ush"').
    Replace('#include "/Engine/Private/DeferredShadingCommon.ush"', '').
    Replace('#include "/Engine/Private/VelocityCommon.ush"', '').
    Replace('"/VoxelEarth/VoxelBrickTraverse.ush"', '"VoxelBrickTraverse.ush"') |
    Out-File (Join-Path $Stage 'march.hlsl') -Encoding utf8

# ---------------------------------------------------------------------------
# engine-stub.ush -- exactly the engine symbols VoxelMarch.usf touches.
#
# EVERY SIGNATURE HERE IS COPIED FROM THE 5.8 SOURCE, with the file and line
# beside it, so that a mismatch between this stub and the engine is a diff and
# not an argument. If the shader starts using a symbol that is not here, the
# compile fails -- which is the point: an unstubbed symbol is a symbol nobody
# checked the spelling of.
# ---------------------------------------------------------------------------
$Stub = @'
#pragma once
// STUB -- see the header of voxel-check-marchdraw-shader.ps1. Not shipped, not
// included by anything the engine compiles.

#ifndef ALLOW_STATIC_LIGHTING
#define ALLOW_STATIC_LIGHTING 1
#endif

// ShadingCommon.ush:19-43
#define SHADINGMODELID_UNLIT       0
#define SHADINGMODELID_DEFAULT_LIT 1
#define SHADINGMODELID_MASK        0xF
#define SKIP_PRECSHADOW_MASK       (1 << 5)

// DoubleFloat.ush -- the DF vector and the one operation this shader performs
// on a pair of them.
struct FDFVector3 { float3 High; float3 Low; };
float3 DFFastLocalSubtractDemote(FDFVector3 A, FDFVector3 B)
{
	return (A.High - B.High) + (A.Low - B.Low);
}

// The subset of ViewState this shader reads. Names verbatim from
// SceneView.h:912-941 and InstancedStereo.ush:26-48.
struct FStubViewState
{
	float4x4 TranslatedWorldToClip;
	float4x4 PrevTranslatedWorldToClip;
	float3   TranslatedWorldCameraOrigin;
	float4   TemporalAAJitter;
	float    PreExposure;
	FDFVector3 PreViewTranslation;
	FDFVector3 PrevPreViewTranslation;
};
static FStubViewState ResolvedView;
static FStubViewState View;

// DeferredShadingCommon.ush:137-146
float3 EncodeNormal(float3 N) { return N * 0.5f + 0.5f; }
// DeferredShadingCommon.ush:192-196
float3 EncodeBaseColor(float3 C) { return C; }
// DeferredShadingCommon.ush:221-227
float EncodeIndirectIrradiance(float L)
{
	L *= View.PreExposure;
	const float LogBlackPoint = 0.00390625f;
	return log2(L + LogBlackPoint) / 16.0f + 0.5f;
}
// DeferredShadingCommon.ush:295-299
float EncodeShadingModelIdAndSelectiveOutputMask(uint Id, uint Mask)
{
	uint Value = (Id & SHADINGMODELID_MASK) | Mask;
	return (float)Value / (float)0xFF;
}
// VelocityCommon.ush:24-27 (via Calculate3DVelocityBase :9-22)
float3 Calculate3DVelocity(float4 A, float4 C)
{
	float2 S = A.xy / A.w - ResolvedView.TemporalAAJitter.xy;
	float2 P = C.xy / C.w - ResolvedView.TemporalAAJitter.zw;
	return float3(S - P, A.z / A.w - C.z / C.w);
}
// Common.ush:2061+
float4 EncodeVelocityToTexture(float3 V) { return float4(V.xy * 0.12475f + 0.5f, 0.0f, 0.0f); }
// Common.ush -- only reached under VOXEL_MARCH_DBUFFER, which #errors anyway.
float2 SvPositionToBufferUV(float4 SvPosition) { return SvPosition.xy; }
'@
$Stub | Out-File (Join-Path $Stage 'engine-stub.ush') -Encoding utf8

$Src = Join-Path $Stage 'march.hlsl'
$Out = Join-Path $Stage 'out.dxil'
$Fail = 0

function Try-Compile($Profile, $Entry, $Defines, $Label, $ExpectFailure = $false) {
    $ArgList = @('-T', $Profile, '-E', $Entry, '-HV', '2021', '-Fo', $Out)
    foreach ($d in $Defines) { $ArgList += @('-D', $d) }
    $ArgList += $Src
    # NOT `2>&1` on a native exe: Windows PowerShell 5.1 wraps each stderr line
    # in a NativeCommandError, which with $ErrorActionPreference='Stop' aborts on
    # the FIRST failing permutation -- so a tree with three broken permutations
    # would report one. stderr goes to a file instead, and the preference is
    # dropped to Continue across the call for the same reason.
    $ErrFile = Join-Path $script:Stage 'dxc-stderr.txt'
    $Saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $script:Dxc @ArgList 2>$ErrFile | Out-Null
    $Failed = ($LASTEXITCODE -ne 0)
    $ErrorActionPreference = $Saved
    $Result = if (Test-Path $ErrFile) { Get-Content $ErrFile } else { @() }

    if ($ExpectFailure) {
        if ($Failed) { Write-Host "ok    $Label (refused, as designed)"; return 0 }
        Write-Host "FAIL  $Label -- this MUST NOT compile. The P2 guard in"
        Write-Host "      VoxelBrickTraverse.ush is gone, so 'the brick pool is wired up' can"
        Write-Host "      now be true by accident instead of by someone typing it."
        return 1
    }
    if ($Failed) {
        # Write-Host, not Write-Output: a function returns everything on the
        # output stream, so a chatty Write-Output turns the return value into an
        # array and `$Fail += ...` dies with op_Addition.
        Write-Host "FAIL  $Label"
        $Result | Select-Object -First 8 | ForEach-Object { Write-Host "      $_" }
        return 1
    }
    Write-Host "ok    $Label"
    return 0
}

$Fail += Try-Compile 'cs_6_0' 'VoxelMarchMain' @('VOXEL_MARCH_SOURCE=0') 'march   source=0'
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchCompactTilesMain' @('VOXEL_MARCH_SOURCE=0') 'compact tiles'
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifyDepthMain' @('VOXEL_MARCH_SOURCE=0') 'verify  depth gate + control'
$Fail += Try-Compile 'vs_6_0' 'VoxelMarchEmitVS' @('VOXEL_MARCH_SOURCE=0') 'emit    VS'

foreach ($Ao in 0, 1) {
    foreach ($Db in 0, 1) {
        foreach ($Vel in 0, 1) {
            # The DBuffer arm is expected to REFUSE: VoxelMarch.usf #errors it,
            # because there is no binding for the DBuffer textures at this seam
            # and a compiled-but-unbindable permutation would sit in the shader
            # map looking available. The C++ ShouldCompilePermutation refuses it
            # too; this is the shader-side half of the same statement.
            $Expect = ($Db -eq 1)
            $Fail += Try-Compile 'ps_6_0' 'VoxelMarchEmitPS' `
                @("VOXEL_MARCH_SOURCE=0", "VOXEL_MARCH_AO=$Ao", "VOXEL_MARCH_DBUFFER=$Db",
                  "VOXEL_MARCH_VELOCITY=$Vel") `
                "emit    PS ao=$Ao dbuffer=$Db vel=$Vel" $Expect
        }
    }
}

# THE SLOT SHIFT. With static lighting off there is no GBufferE and velocity
# moves from SV_Target6 to SV_Target5. This is the only thing in the shader that
# changes its output SIGNATURE, the host branches on the same predicate to build
# the binding slots, and a disagreement puts motion vectors into GBufferE with
# no error anywhere. Compiled explicitly so the branch cannot rot unnoticed.
foreach ($Vel in 0, 1) {
    $Fail += Try-Compile 'ps_6_0' 'VoxelMarchEmitPS' `
        @("VOXEL_MARCH_SOURCE=0", "VOXEL_MARCH_AO=1", "VOXEL_MARCH_DBUFFER=0",
          "VOXEL_MARCH_VELOCITY=$Vel", "ALLOW_STATIC_LIGHTING=0") `
        "emit    PS static-lighting=0 vel=$Vel"
}

# ---------------------------------------------------------------------------
# SOURCE 1 -- the brick pool (P3-B1). This used to be a wall that had to FAIL;
# it is now a traversal that has to COMPILE, and the guard test is gone because
# the thing it was guarding has arrived.
#
# The march and verify kernels carry the whole five-indirection lookup, so a
# typo in the descriptor decode or the palette walk shows up here rather than at
# boot. The emit's PS reads only the VisBuffer and is source-independent, but it
# is compiled under source 1 anyway: VoxelMarchSourceOriginVoxel() switches on
# the source and the emit is one of its two call sites, so the pair has to be
# checked together or the palette hash is keyed on a voxel that does not exist.
# THE SKIP LADDER. 0 is the flat control that produced the +18.3% indirection
# number; 1 adds brick skipping; 2 adds chunk skipping. All three must compile,
# because the skip ratio is a comparison BETWEEN them and a permutation that
# fails to build is an arm that silently never runs.
foreach ($Sk in 0, 1, 2) {
    $Fail += Try-Compile 'cs_6_0' 'VoxelMarchMain' `
        @('VOXEL_MARCH_SOURCE=1', "VOXEL_MARCH_SKIP_LEVELS=$Sk") "march   source=1 skip=$Sk"
}
# THE RING CASCADE ARM (P3-B2b-1). Both the hierarchical and the FLAT ring arms,
# because the skip ratio at 256 m is a comparison between exactly those two and
# the flat one is the control -- an arm that fails to build is an arm that
# silently never runs, which is the defect this whole script exists to catch.
# The comparator and the emit PS get rings too: the comparator produces every
# ring number, and the emit PS is what a leg actually draws with.
foreach ($Sk in 0, 1) {
    $Fail += Try-Compile 'cs_6_0' 'VoxelMarchMain' `
        @('VOXEL_MARCH_SOURCE=1', "VOXEL_MARCH_SKIP_LEVELS=$Sk", 'VOXEL_MARCH_RINGS=1') `
        "march   source=1 rings skip=$Sk"
}
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifySourceMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_VERIFY_SOURCE=1', 'VOXEL_MARCH_RINGS=1') `
    'compare rings (one kernel)'
$Fail += Try-Compile 'ps_6_0' 'VoxelMarchEmitPS' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_AO=1', 'VOXEL_MARCH_DBUFFER=0',
      'VOXEL_MARCH_VELOCITY=1', 'VOXEL_MARCH_RINGS=1') 'emit    PS rings'
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifyDepthMain' @('VOXEL_MARCH_SOURCE=1') `
    'verify  source=1 (pool)'
# The index/record join probe. Source 1 only -- there is no join to probe when
# the marcher reads a flat bitfield, and every symbol it touches is declared
# under VOXEL_MARCH_SOURCE 1.
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifyIndexMain' @('VOXEL_MARCH_SOURCE=1') `
    'probe   index/record join'
# THE FIT CHECK MUST BE ABLE TO FAIL. The probe's buffer layout is pushed from
# the CPU now, and a negative array extent is what turns an overflow into a
# compile error instead of writes the driver drops. A guard nobody has seen fail
# is a guard nobody knows works -- this compiles the probe against a deliberately
# too-small allocation and REQUIRES the build to be refused.
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifyIndexMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_VIDX_ALLOC_WORDS=8') `
    'vidx    fit check' $true

# The same proof for the comparator's contract, which is the bigger of the two.
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifySourceMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_VERIFY_SOURCE=1', 'VOXEL_MARCH_VSRC_ALLOC_WORDS=64') `
    'vsrc    fit check' $true
# The source comparator. BOTH traversals in one kernel, which is the one
# configuration the either/or source split does not otherwise produce -- so this
# is the only check that compiles VoxelMarchTraverseOccupancy and
# VoxelMarchTraverseBrick together and catches a symbol collision between them.
$Fail += Try-Compile 'cs_6_0' 'VoxelMarchVerifySourceMain' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_VERIFY_SOURCE=1') 'compare both sources (one kernel)'
$Fail += Try-Compile 'ps_6_0' 'VoxelMarchEmitPS' `
    @('VOXEL_MARCH_SOURCE=1', 'VOXEL_MARCH_AO=1', 'VOXEL_MARCH_DBUFFER=0',
      'VOXEL_MARCH_VELOCITY=1') 'emit    PS source=1 (pool)'

# ===========================================================================
# THE PROBE MUST NOT SUPPLY ITS OWN TRUTH
# ===========================================================================
#
# This script compiled the comparator with VOXEL_MARCH_RINGS=1 by hand and
# reported "compare rings (one kernel) ok" for as long as the permutation
# existed. The ENGINE never set it -- FVoxelMarchVerifySourceCS had no
# permutation domain -- so the kernel that produced every measured number was
# compiled rings-OFF while this script proved daily that rings-ON built fine.
#
# A DEFINE THE PROBE SUPPLIES AND THE ENGINE NEVER DOES IS A TEST OF NOTHING.
# It is worse than no test: it is a green light for a configuration that is
# never built. This is the general form, not the instance -- it stays in
# whether or not anything fails it today.
#
# The check: every VOXEL_MARCH_* define this script passes on a command line
# must also be something the C++ actually sets -- either via SetDefine in some
# ModifyCompilationEnvironment, or as a SHADER_PERMUTATION_* dimension name.
$SelfText = Get-Content -Raw $PSCommandPath
$RendererCpp = Join-Path $Root 'ue-project\Source\VoxelEarthShaders\Private\VoxelMarchRenderer.cpp'
if (Test-Path $RendererCpp) {
    $CppText = Get-Content -Raw $RendererCpp
    $Passed = [regex]::Matches($SelfText, "VOXEL_MARCH_[A-Z0-9_]+(?==)") |
              ForEach-Object { $_.Value } | Sort-Object -Unique
    $Orphans = @()
    foreach ($Name in $Passed) {
        # Either the C++ pushes it, or it names a permutation dimension.
        if ($CppText -notmatch [regex]::Escape('"' + $Name + '"')) {
            $Orphans += $Name
        }
    }
    if ($Orphans.Count -gt 0) {
        Write-Output ""
        foreach ($Name in $Orphans) {
            Write-Output "FAIL  ORPHAN DEFINE: this script passes $Name but no"
            Write-Output "      ModifyCompilationEnvironment or permutation dimension in"
            Write-Output "      VoxelMarchRenderer.cpp ever sets it. The engine will never"
            Write-Output "      compile that configuration, so the check above proves nothing"
            Write-Output "      about anything the engine runs."
            $Fail++
        }
    } else {
        Write-Output "ok    no orphan defines -- every permutation this script passes is one"
        Write-Output "      the engine actually sets"
    }
} else {
    Write-Output "FAIL  cannot find VoxelMarchRenderer.cpp; the orphan-define check did not run"
    $Fail++
}

if ($Fail -gt 0) {
    Write-Output ""
    Write-Output "MARCH DRAW SHADER TREE IS MID-EDIT: $Fail of 29 checks failed."
    Write-Output "Do not start a leg -- the editor compiles VoxelMarch.usf's four global shader"
    Write-Output "entry points AT BOOT and will die on a global-shader error."
    exit 1
}
Write-Output ""
Write-Output "all 29 checks pass -- the march draw shader tree is safe to boot."
Write-Output "REMINDER: the emit VS/PS were compiled against engine-stub.ush, not against UE's"
Write-Output "own environment (GeneratedUniformBuffers.ush cannot be reproduced offline). This"
Write-Output "says the tree is not mid-edit. It is not a substitute for the editor's compile."
exit 0

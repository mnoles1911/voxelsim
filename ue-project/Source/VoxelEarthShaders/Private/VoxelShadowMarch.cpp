// VoxelShadowMarch.cpp -- RDG plumbing for the shadow march.
//
// VoxelShadowMarch.h owns the seam argument (which hook, why not inside the
// primary march, how the two extensions order); VoxelShadowMarch.usf owns the
// verdict rule and the stat table. This file is the wiring: cvars, the
// parameter struct, the dispatch, the timing ring, the readback ring and the
// census line. Shapes are lifted from VoxelMarchRenderer.cpp deliberately --
// same timing-pair pattern, same retire-before-the-gate rule, same
// decline-with-a-named-reason discipline -- so that anyone who has read one
// file has read both.

#include "VoxelShadowMarch.h"

#include "VoxelBrickPool.h"       // the traversal source (public seam)
#include "VoxelMarchChunkIndex.h" // and the GPU lookup that makes it walkable

#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TextureResource.h" // FTextureRenderTargetResource
#include "FXRenderingUtils.h"
#include "GBufferInfo.h" // FGBufferBindings / GBL_Default -- which slot holds which GBuffer
#include "GlobalShader.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIUtilities.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneRenderTargetParameters.h"
#include "SceneTexturesConfig.h" // FSceneTextureUniformParameters (ENGINE_API, public)
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED / RDG_EVENT_SCOPE_STAT

DEFINE_LOG_CATEGORY_STATIC(LogVoxelShadowMarch, Log, All);

DECLARE_GPU_STAT_NAMED(VoxelShadowMarch, TEXT("VoxelShadowMarch"));

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (same note as VoxelMarchRenderer.cpp:47).
#define VOXEL_SHADOW_USF "/VoxelEarth/VoxelShadowMarch.usf"

static constexpr int32 kVoxelShadowTileSize = 8;

// ---------------------------------------------------------------------------
// The stat table -- MIRRORS VoxelShadowMarch.usf (grep: VOXEL_SHADOW_STAT).
// A renumbering there without here is a misattributed census, not a crash.
// ---------------------------------------------------------------------------
namespace VoxelShadowStat
{
	enum : int32
	{
		Considered = 0,
		Sky = 1,
		Far = 2,
		Backface = 3,
		OffBox = 4,
		Marched = 5,
		Hit = 6,
		StartedInside = 7,
		Exhausted = 8,
		StepsTotal = 9,
		MaxSteps = 10,
		Hist0 = 12,
		HistBuckets = 14,
		InsideNear = 26,
		InsideMid = 27,
		InsideFar = 28,
		MarchedNear = 29,
		MarchedMid = 30,
		MarchedFar = 31,
		Buried1 = 32,
		Buried2 = 33,
		Buried3 = 34,
		Buried4Plus = 35,
		InsideMatDeep = 36,
		InsideMatSurf = 37,
		NonAxisNormal = 38,
		VCompared = 48,
		VDisagree = 49,
		VDisShadowed = 50,
		VDisLit = 51,
		VRefSelf = 52,
		VSkipped = 53,
	};
}

namespace
{
	// ---- the arm ----------------------------------------------------------

	TAutoConsoleVariable<int32> CVarVoxelShadowMarch(
		TEXT("voxel.Shadow.March"), 2,
		TEXT("S1 of docs/shadow-march-design-2026-08-20.md: march one sun ray per shaded ")
		TEXT("pixel through the resident brick pyramid, from the depth buffer. ")
		TEXT("0 = off (default; the extension declines every hook and the frame is ")
		TEXT("byte-identical to a build without this file). ")
		TEXT("1 = march to a scratch mask + stats. NOTHING VISIBLE CHANGES -- this mode ")
		TEXT("exists to be timed against mode 0 on the same pose. ")
		TEXT("2 = S2, light-function injection. BUILT AND LIVE: the mask is copied into a ")
		TEXT("persistent render target and wired onto the sun as a light function, so this ")
		TEXT("mode CHANGES THE PICTURE. Requires the checked-in material at ")
		TEXT("/Game/Voxel/M_VoxelSunShadowLF; if it is missing the injection is DECLINED and ")
		TEXT("said (shadowinject: declinedNoTarget counts every frame) rather than silently ")
		TEXT("running as mode 1.\n")
		TEXT("THIS TEXT SAID 'NOT BUILT' UNTIL 2026-08-22, LONG AFTER MODE 2 SHIPPED. It was ")
		TEXT("written when mode 2 was a stub and never revisited when the injection landed, so ")
		TEXT("the one place an operator looks to find out what this cvar does was telling them ")
		TEXT("the feature did not exist. Stale help is not a cosmetic defect on a switch whose ")
		TEXT("whole purpose is to be set by someone reading it."),
		ECVF_RenderThreadSafe);

	// THE CASCADE, mirrored from the camera marcher's cvars so the shadow walk and
	// the camera walk cannot disagree about where a ring ends. Defaults match
	// voxel.March.Rings / voxel.March.RingOuterM.
	TAutoConsoleVariable<int32> CVarVoxelShadowRings(
		TEXT("voxel.Shadow.MarchRings"), 1,
		TEXT("Let shadow rays walk the ring cascade instead of level 0 only. 1 = on (default). "
		     "0 pins the walk to level 0, which caps useful reach at R0 (128 m) because "
		     "residency is camera-radial annuli -- beyond it a level-0 ray finds no bricks and "
		     "reads LIT. Off is the bit-exact pre-cascade control."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelShadowRingOuterM(
		TEXT("voxel.Shadow.MarchRing0OuterM"), 128.0f,
		TEXT("R0's outer radius in metres for the shadow walk's level selection. Ring L covers "
		     "[R0*2^(L-1), R0*2^L). MUST match voxel.March.RingOuterM -- a disagreement makes "
		     "shadow rays ask for levels at radii the pool does not populate, which is a hole "
		     "rather than an error."),
		ECVF_RenderThreadSafe);

	// Derived once, read by the pass. Clamped to what the index actually carries:
	// asking for a level the grid has no sub-grid for is a silent miss.
	inline uint32 VoxelShadowGetRingCount()
	{
		const int32 Max = int32(FVoxelMarchChunkIndex::kLevels);
		return uint32(FMath::Clamp(CVarVoxelShadowRings.GetValueOnRenderThread() != 0 ? Max : 1, 1, Max));
	}
	inline float VoxelShadowGetRing0OuterUU()
	{
		return FMath::Max(CVarVoxelShadowRingOuterM.GetValueOnRenderThread(), 1.0f) * 100.0f;
	}

	// RAY REACH, SPLIT FROM SURFACE REACH -- and the split is the cost control.
	//
	// They were one number, so raising reach to kill the 62% of pixels beyond it
	// ALSO made every shadow ray four times longer. Measured: reach 64 -> 512 m
	// took far= from 840,288 to 70,305 (the win) and gpuMs from 0.409 to 10.511
	// (the price). Only SURFACE reach has to grow -- that is what decides whether
	// a pixel is shadowed at all. How far its ray then travels toward the sun is
	// a separate question, and occluders hundreds of metres away contribute
	// almost nothing at a high sun.
	TAutoConsoleVariable<float> CVarVoxelShadowRayReachM(
		TEXT("voxel.Shadow.MarchRayReachM"), 96.0f,
		TEXT("How far a shadow ray travels toward the sun, metres. SEPARATE from "
		     "voxel.Shadow.MarchReachM, which decides how far away a pixel may be and still be "
		     "shadow-tested at all. This is the COST knob: ray length drives steps per ray, and "
		     "the two were coupled until 2026-08-22, so raising surface reach to 512 m also made "
		     "every ray 8x longer and took the pass from 0.4 ms to 10.5 ms. 0 = follow surface "
		     "reach (the old coupled behaviour)."),
		ECVF_RenderThreadSafe);

	// 0 means "follow surface reach", i.e. the pre-split behaviour.
	inline float VoxelShadowGetRayReachUU(float SurfaceReachM)
	{
		const float RayM = CVarVoxelShadowRayReachM.GetValueOnRenderThread();
		return (RayM > 0.0f ? RayM : SurfaceReachM) * 100.0f;
	}

	TAutoConsoleVariable<float> CVarVoxelShadowMarchReachM(
		TEXT("voxel.Shadow.MarchReachM"), 512.0f,
		TEXT("Shadow march reach, metres. Surfaces farther than this from the camera are ")
		TEXT("skipped (counted 'far', read LIT), and a shadow ray travels at most this far ")
		TEXT("toward the sun. The march box is camera +/- 2x this, chunk-snapped; the ")
		TEXT("default 64 m keeps the box inside R0's level-0 residency (128 m radius), ")
		TEXT("because this walk is level-0-only until the hierarchy lands. Casters farther ")
		TEXT("away than the reach cast nothing -- v0's stated limitation, on every capture."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelShadowMarchBudget(
		TEXT("voxel.Shadow.MarchBudget"), 2048,
		TEXT("Step budget per shadow ray (clamped 1..4096). A ray that exhausts it reads ")
		TEXT("LIT and inflates the 'exhausted' counter -- fail-lit, never quietly dark. ")
		TEXT("The default is set to NOT bind at 64 m: the budget is a safety net here, ")
		TEXT("not a knob, because the step count is the quantity S1 exists to measure."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelShadowMarchNormalOffsetVoxels(
		TEXT("voxel.Shadow.MarchNormalOffsetVoxels"), 0.5f,
		TEXT("Ray-origin offset along the GBuffer normal, in level-0 voxels. THE ACNE ")
		TEXT("KNOB, in world units -- exact, not shadow-map-resolution dependent. ")
		TEXT("DEFAULT 0.5, AND THE HALF-VOXEL IS LOAD-BEARING: the voxel adjacent to a ")
		TEXT("visible face is guaranteed air BY VISIBILITY (the camera saw the face ")
		TEXT("through it), and 0.5 puts the origin at its CENTER. An exact 1.0 lands on ")
		TEXT("that air voxel's far boundary -- the surface of whatever is next -- which ")
		TEXT("is how the offset-sweep legs measured inside rates RISING with the offset ")
		TEXT("(53.9%% at 1.0, 64.3%% far-band at 1.5, over the D4-era diagonal normals). ")
		TEXT("Never set this to an integer number of voxels."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelShadowMarchPullbackPx(
		TEXT("voxel.Shadow.MarchPullbackPx"), 0.0f,
		TEXT("Pull the ray origin back toward the CAMERA by this many pixel footprints ")
		TEXT("(footprint = TSurf x cone slope) before the normal offset. THE RANGE ")
		TEXT("DE-BIAS for startedInside: beyond ~30 m one pixel's footprint exceeds a ")
		TEXT("10 cm voxel, the depth sample is one point on sub-pixel staircase relief, ")
		TEXT("and a reconstructed origin lands inside that relief about half the time ")
		TEXT("-- the inside/f=53%% signature of s1-shadow-m1b. Any point on the camera ")
		TEXT("ray IN FRONT of the depth sample is air by visibility, so ~1.5 footprints ")
		TEXT("of pullback restores an air origin at exactly the ranges where the screen ")
		TEXT("cannot resolve the contact-shadow shift it introduces (sub-pixel by ")
		TEXT("construction). 0 = off -- the configuration S1's cost number was ")
		TEXT("measured at."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelShadowMarchVerify(
		TEXT("voxel.Shadow.MarchVerify"), 0,
		TEXT("G-S1's replay gate: 0 = off (THE DEFAULT, and timing legs must keep it ")
		TEXT("off -- the verify walks every sampled ray up to twice more). N > 0 = a ")
		TEXT("second dispatch with its own bindings re-derives every Nth pixel's ray ")
		TEXT("from the depth buffer, re-walks it, and compares its verdict against the ")
		TEXT("mask the main pass wrote, plus an entry-nudged self-pair that measures ")
		TEXT("the reference's own noise floor every frame. In S1 the two dispatches run ")
		TEXT("the same traversal, so replay agreement proves the BINDING AND RECORDING ")
		TEXT("path, not the walk -- stated here so a green S1 gate is not over-read."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelShadowMarchVerifySunNudgeDeg(
		TEXT("voxel.Shadow.MarchVerifySunNudgeDeg"), 0.0f,
		TEXT("THE MUTATION ARM: rotate the VERIFY kernel's sun by this many degrees. ")
		TEXT("With a wrong sun the replay must disagree along every shadow edge; a gate ")
		TEXT("that stays green under this is a check that cannot fail and must not be ")
		TEXT("trusted. Run once per configuration change and record the result beside ")
		TEXT("the gate's. 0 = off (default)."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelShadowMarchDiag(
		TEXT("voxel.Shadow.MarchDiag"), 0,
		TEXT("startedInside diagnosis instruments: 0 = off (default -- the timing ")
		TEXT("configuration). 1 = for every inside ray, probe straight up for the first ")
		TEXT("air voxel (burial-depth histogram), classify the inside voxel's material ")
		TEXT("(BEDROCK/ROCK = deep interior vs surface shell), and dump the first 64 ")
		TEXT("example records per frame -- pixel, TSurf, normal, material, burial and ")
		TEXT("the WORLD VOXEL, so records can be cross-checked offline against the CPU ")
		TEXT("reference world, which is the arbiter of 'bricks wrong vs reconstruction ")
		TEXT("wrong'. Costs extra solid-tests per inside ray: NEVER on in a timing leg."),
		ECVF_RenderThreadSafe);

	// THE INJECTION CONTRACT with Tools/create_sunshadow_lf_material.py. The
	// parameter name is a silent no-op if the two sides disagree
	// (SetTextureParameterValue on a missing name does nothing and the material
	// samples its white fail-lit default forever), which is why both sides
	// carry this exact string and a grep for it must always find BOTH files.
	static const TCHAR* kSunShadowLFMaterialPath =
		TEXT("/Game/Voxel/M_VoxelSunShadowLF.M_VoxelSunShadowLF");
	static const FName kSunShadowMaskParamName(TEXT("VoxelSunShadowMask"));

	TAutoConsoleVariable<int32> CVarVoxelShadowMarchMutateInjection(
		TEXT("voxel.Shadow.MarchMutateInjection"), 0,
		TEXT("THE INJECTION MUTATION ARM: under mode 2, fill the light-function target ")
		TEXT("with 0.5 instead of copying the marched mask. The whole screen's DIRECT ")
		TEXT("SUN must visibly dim to half -- a normal-looking frame under this cvar ")
		TEXT("means the injection chain (RT -> MID -> SetLightFunctionMaterial -> ")
		TEXT("RenderLightFunction) is NOT live, whatever the counters say. Run once per ")
		TEXT("configuration change and record the capture beside the gate. 0 = off."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelShadowMarchStatsPeriod(
		TEXT("voxel.Shadow.MarchStatsPeriod"), 600,
		TEXT("Frames of retired GPU stats per census line. Totals are printed as ")
		TEXT("total/count over the window, never as sums of per-frame means."),
		ECVF_RenderThreadSafe);

	struct FVoxelShadowArm
	{
		int32 Mode = 0;
		float ReachM = 64.0f;
		int32 Budget = 2048;
		float NormalOffsetVoxels = 1.0f;
		float PullbackPx = 0.0f;
		bool bDiag = false;
		bool bMutateInjection = false;
		int32 VerifyStride = 0;
		float VerifySunNudgeDeg = 0.0f;
		int32 StatsPeriod = 600;
	};

	FVoxelShadowArm VoxelShadowGetArm()
	{
		FVoxelShadowArm Arm;
		Arm.Mode = FMath::Clamp(CVarVoxelShadowMarch.GetValueOnAnyThread(), 0, 2);
		Arm.ReachM = FMath::Clamp(CVarVoxelShadowMarchReachM.GetValueOnAnyThread(), 1.0f, 512.0f);
		Arm.Budget = FMath::Clamp(CVarVoxelShadowMarchBudget.GetValueOnAnyThread(), 1, 4096);
		Arm.NormalOffsetVoxels =
			FMath::Clamp(CVarVoxelShadowMarchNormalOffsetVoxels.GetValueOnAnyThread(), 0.0f, 8.0f);
		Arm.PullbackPx = FMath::Clamp(CVarVoxelShadowMarchPullbackPx.GetValueOnAnyThread(), 0.0f, 8.0f);
		Arm.bDiag = CVarVoxelShadowMarchDiag.GetValueOnAnyThread() != 0;
		Arm.bMutateInjection = CVarVoxelShadowMarchMutateInjection.GetValueOnAnyThread() != 0;
		Arm.VerifyStride = FMath::Max(CVarVoxelShadowMarchVerify.GetValueOnAnyThread(), 0);
		Arm.VerifySunNudgeDeg = CVarVoxelShadowMarchVerifySunNudgeDeg.GetValueOnAnyThread();
		Arm.StatsPeriod = FMath::Clamp(CVarVoxelShadowMarchStatsPeriod.GetValueOnAnyThread(), 1, 100000);
		return Arm;
	}
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
//
// ONE parameter struct for both kernels. RDG only binds (and only validates)
// the parameters a given shader's map actually contains, so the main dispatch
// leaves ShadowMaskTexture null and the verify dispatch leaves ShadowOutMask
// null, and neither is an error. The pool arena names and the chunk-index
// names come from the same macros/loose globals the primary marcher binds --
// VoxelBrickTraverse.ush is the single definition of what this struct must
// carry, and a drift there is a compile error here, not a wrong picture.

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelShadowMarchParameters, )
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER(int32, MarchStepBudget)
	SHADER_PARAMETER(FMatrix44f, ShadowViewToTranslatedWorld)
	SHADER_PARAMETER(FVector3f, ShadowRayOriginLocalUU)
	SHADER_PARAMETER(FVector3f, ShadowDirToSun)
	SHADER_PARAMETER(FVector3f, ShadowVerifyDirToSun)
	SHADER_PARAMETER(float, ShadowVolumeExtentUU)
	// The cascade. MarchIndexCellsPerLevel is REQUIRED the moment this kernel
	// calls VoxelMarchBeginLevel: the index cell address is
	// GVoxelMarchIndexGrid * MarchIndexCellsPerLevel + ..., and leaving it out
	// is a loud boot-time bind failure rather than a wrong picture. Do not
	// 'fix' that by hardcoding a literal.
	SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
	SHADER_PARAMETER(uint32, ShadowRingCount)
	SHADER_PARAMETER(float, ShadowRing0OuterUU)
	SHADER_PARAMETER(FVector2f, ShadowViewRectMin)
	SHADER_PARAMETER(FVector2f, ShadowViewRectSize)
	SHADER_PARAMETER(FVector2f, ShadowInvProjDiag)
	SHADER_PARAMETER(FVector4f, ShadowInvDeviceZToWorldZ)
	SHADER_PARAMETER(float, ShadowSurfaceReachUU)
	SHADER_PARAMETER(float, ShadowRayReachUU)
	SHADER_PARAMETER(float, ShadowNormalOffsetUU)
	SHADER_PARAMETER(float, ShadowPixelConeSlope)
	SHADER_PARAMETER(float, ShadowPullbackPx)
	SHADER_PARAMETER(uint32, ShadowVerifyStride)
	SHADER_PARAMETER(uint32, ShadowDiagEnabled)
	SHADER_PARAMETER(uint32, ShadowInsideDumpCap)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, ShadowSceneDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, ShadowGBufferATexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ShadowOutMask)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, ShadowMaskTexture)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ShadowOutStats)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ShadowOutInsideDump)
END_SHADER_PARAMETER_STRUCT()

// File scope, not an anonymous namespace: the marcher's shader classes are
// declared this way and the DECLARE/IMPLEMENT_GLOBAL_SHADER machinery is known
// to be happy with it there -- same pattern, no new linkage question.
class FVoxelShadowMarchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelShadowMarchCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelShadowMarchCS, FGlobalShader);
	using FParameters = FVoxelShadowMarchParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// The brick pool is the ONLY source here -- see the #error in the .usf.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_SHADOW_TILE"), kVoxelShadowTileSize);
	}
};

class FVoxelShadowVerifyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelShadowVerifyCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelShadowVerifyCS, FGlobalShader);
	using FParameters = FVoxelShadowMarchParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
		OutEnvironment.SetDefine(TEXT("VOXEL_SHADOW_TILE"), kVoxelShadowTileSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelShadowMarchCS, VOXEL_SHADOW_USF, "VoxelShadowMarchMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelShadowVerifyCS, VOXEL_SHADOW_USF, "VoxelShadowVerifyMain", SF_Compute);

// ---------------------------------------------------------------------------
// Timing brackets -- the marcher's OpenBracket/CloseBracket, shape preserved
// ---------------------------------------------------------------------------
namespace
{
	FVoxelShadowMarchState::FTimingPair* VoxelShadowOpenBracket(
		FRDGBuilder& GraphBuilder, FVoxelShadowMarchState::FTimingPair* Ring)
	{
		if (!GSupportsTimestampRenderQueries)
		{
			return nullptr;
		}
		FVoxelShadowMarchState::FTimingPair* Timing = nullptr;
		for (int32 i = 0; i < FVoxelShadowMarchState::kNumTimingPairs; ++i)
		{
			if (!Ring[i].bInFlight)
			{
				Timing = &Ring[i];
				break;
			}
		}
		if (Timing == nullptr)
		{
			return nullptr; // no pair free; the pass still runs, unmeasured this frame
		}
		if (!Timing->Begin.IsValid())
		{
			Timing->Begin = RHICreateRenderQuery(RQT_AbsoluteTime);
			Timing->End = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		FRHIRenderQuery* Query = Timing->Begin.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelShadowMarch.TimeBegin"), ERDGPassFlags::NeverCull,
		                     [Query](FRHICommandListImmediate& RHICmdList)
		                     {
			                     RHICmdList.EndRenderQuery(Query);
		                     });
		return Timing;
	}

	void VoxelShadowCloseBracket(FRDGBuilder& GraphBuilder,
	                             FVoxelShadowMarchState::FTimingPair* Timing)
	{
		if (Timing == nullptr)
		{
			return;
		}
		FRHIRenderQuery* Query = Timing->End.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelShadowMarch.TimeEnd"), ERDGPassFlags::NeverCull,
		                     [Query](FRHICommandListImmediate& RHICmdList)
		                     {
			                     RHICmdList.EndRenderQuery(Query);
		                     });
		Timing->bInFlight = true;
	}
}

// ===========================================================================
// The extension
// ===========================================================================

FVoxelShadowMarchExtension::FVoxelShadowMarchExtension(
	const FAutoRegister& AutoRegister, UWorld* InWorld,
	TSharedPtr<FVoxelShadowMarchState, ESPMode::ThreadSafe> InState)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
	, State(MoveTemp(InState))
{
}

bool FVoxelShadowMarchExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
{
	// The base class gates to this extension's world; losing that gate would
	// march one world's bricks against another world's depth in PIE.
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}
	// Mode 0 must be byte-identical to a build without this file: declining
	// here means not one hook is called. The sun gate is NOT applied here --
	// the hooks must still run so the retire path drains rings and the census
	// can say "declined: no sun" instead of silently reporting nothing.
	return State.IsValid() && VoxelShadowGetArm().Mode != 0;
}

void FVoxelShadowMarchExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder,
                                                            FSceneView& InView)
{
	// Retired BEFORE any gate -- the marcher's rule: an un-polled ring after
	// the cvar drops to 0 reports "pending" forever, which reads exactly like
	// "the pass never ran".
	RetireGpuWork();

	const uint32 FrameNumber = GFrameNumberRenderThread;
	Views.RemoveAll([FrameNumber](const FViewStash& E) { return E.FrameNumber != FrameNumber; });

	const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
	if (ViewRect.Area() <= 0)
	{
		return;
	}

	const FViewMatrices& VM = InView.ViewMatrices;
	const FMatrix44f ViewToClip = FMatrix44f(VM.GetViewToClip());
	const float ProjXX = ViewToClip.M[0][0];
	const float ProjYY = ViewToClip.M[1][1];
	if (ProjXX == 0.0f || ProjYY == 0.0f)
	{
		return; // degenerate projection; no ray to build
	}

	FViewStash Entry;
	Entry.ViewKey = &InView;
	Entry.FrameNumber = FrameNumber;
	Entry.ViewRect = ViewRect;
	Entry.ViewToTranslatedWorld = FMatrix44f(VM.GetInvTranslatedViewMatrix());
	Entry.InvProjDiag = FVector2f(1.0f / ProjXX, 1.0f / ProjYY);
	{
		// ALREADY FLOAT IN 5.8. FSceneView::InvDeviceZToWorldZTransform is an
		// FVector4f, so widening it into a double FVector4 and narrowing back
		// does not compile -- and would be a pointless round trip if it did.
		const FVector4f T = InView.InvDeviceZToWorldZTransform;
		Entry.InvDeviceZToWorldZ = FVector4f(T.X, T.Y, T.Z, T.W);
	}
	// THE CONE SLOPE, the marcher's own derivation (VoxelMarchRenderer.cpp's
	// stash): one pixel's half-width in UU per UU of distance along the ray,
	// the larger of the two axes -- conservative where pixels are not square.
	{
		const float SlopeX = 2.0f / (ProjXX * float(ViewRect.Width()));
		const float SlopeY = 2.0f / (ProjYY * float(ViewRect.Height()));
		Entry.PixelConeSlope = FMath::Max(SlopeX, SlopeY);
	}
	// THE PRECISION SEAM, same as the marcher's stash: the camera is at planet
	// scale, so the frame-origin subtraction happens in DOUBLE downstream and
	// only the bounded difference is narrowed to float. Here we keep the double
	// origin and the floored voxel.
	{
		const FVector CamUU = VM.GetViewOrigin();
		Entry.ViewOriginUU = CamUU;
		Entry.CameraVoxel = FIntVector(int32(FMath::FloorToDouble(CamUU.X / 10.0)),
		                               int32(FMath::FloorToDouble(CamUU.Y / 10.0)),
		                               int32(FMath::FloorToDouble(CamUU.Z / 10.0)));
	}
	Views.Add(Entry);
}

void FVoxelShadowMarchExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	const FVoxelShadowArm Arm = VoxelShadowGetArm();
	if (Arm.Mode == 0 || !State.IsValid())
	{
		return;
	}

	// The sun, snapshotted once for the whole pass so the march and the verify
	// cannot see two different suns in one frame.
	FVector3f DirToSun = FVector3f(0.0f, 0.0f, 1.0f);
	bool bSunValid = false;
	{
		FScopeLock Guard(&State->Lock);
		DirToSun = State->SunDirToSunWorld;
		bSunValid = State->bSunValid;
	}
	if (!bSunValid)
	{
		State->DeclinedNoSun++;
		return;
	}

	const FViewStash* Stash = nullptr;
	for (const FViewStash& E : Views)
	{
		if (E.ViewKey == &InView && E.FrameNumber == GFrameNumberRenderThread)
		{
			Stash = &E;
			break;
		}
	}
	if (Stash == nullptr)
	{
		State->DeclinedNoView++;
		return;
	}

	// Scene textures, handed to us by the hook. GBufferA carries the world
	// normal this pass aims its one-voxel offset along.
	if (SceneTextures == nullptr)
	{
		State->DeclinedNoTextures++;
		return;
	}
	FRDGTextureRef SceneDepth = SceneTextures->GetContents()->SceneDepthTexture;
	// GBufferA comes from the hook's RENDER TARGET BINDINGS, matched BY NAME --
	// never from the scene-texture uniform buffer and never by slot index. The
	// D4 incident, recorded so it cannot be re-simplified away: the UB handed to
	// this hook was built with SceneDepth only (DeferredShadingRenderer.cpp:2499)
	// and its unset GBuffer slots fall back to SystemTextures.Black
	// (SceneTextures.cpp:1108) -- NON-NULL, so a null-check passes while every
	// sample decodes to the corner diagonal (-0.577)^3. And the tempting repair,
	// FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer(..., GBufferA),
	// IGNORES the requested setup mode whenever the scene already has a UB
	// (FXRenderingUtils.cpp:109-118) -- it returns the same depth-only buffer.
	// The MRT bindings are what the base pass actually wrote; the name is the
	// engine's own (SceneTextures.cpp:738 creates TEXT("GBufferA")); slot
	// indices are data-driven and move under r.VelocityOutputPass, which is why
	// the match is by name (the marcher's emit documents the same hazard from
	// the write side).
	// The slot comes from the same table the renderer built the bindings from
	// -- FSceneTexturesConfig::Get().GBufferBindings[GBL_Default] -- which is
	// the marcher emit's own fix for the identical defect (see the WHERE THE
	// TARGETS COME FROM block in VoxelMarchRenderer.cpp). The name is then
	// CROSS-CHECKED against the engine's own debug name (SceneTextures.cpp:738
	// creates TEXT("GBufferA")) and the extent against scene depth, so a config
	// /binding disagreement or a 1x1 system dummy declines loudly instead of
	// laundering into a plausible normal.
	const FGBufferBindings& Bindings =
		FSceneTexturesConfig::Get().GBufferBindings[GBL_Default];
	const int32 GBufferASlot = Bindings.GBufferA.Index;
	FRDGTextureRef GBufferA =
		(GBufferASlot > 0) ? RenderTargets[uint32(GBufferASlot)].GetTexture() : nullptr;

	const bool bNameOk =
		GBufferA != nullptr && FCString::Strcmp(GBufferA->Name, TEXT("GBufferA")) == 0;
	const bool bExtentOk = GBufferA != nullptr && SceneDepth != nullptr &&
	                       GBufferA->Desc.Extent == SceneDepth->Desc.Extent;
	if (SceneDepth == nullptr || GBufferA == nullptr || !bNameOk || !bExtentOk)
	{
		State->DeclinedNoTextures++;
		static bool bLoggedBadGBufferA = false;
		if (!bLoggedBadGBufferA)
		{
			bLoggedBadGBufferA = true;
			FString Slots;
			for (int32 i = 0; i < MaxSimultaneousRenderTargets; ++i)
			{
				FRDGTextureRef Tex = RenderTargets.Output[i].GetTexture();
				if (Tex == nullptr)
				{
					break;
				}
				Slots += FString::Printf(TEXT("[%d]=%s "), i, Tex->Name);
			}
			UE_LOG(LogVoxelShadowMarch, Warning,
			       TEXT("shadow march: GBufferA binding FAILED verification at ")
			            TEXT("PostRenderBasePassDeferred -- declining every frame (counted ")
			            TEXT("noTextures). config slot=%d name='%s' extent=%dx%d against ")
			            TEXT("depth %dx%d. Slots present: %s. A changed GBuffer layout must ")
			            TEXT("be re-matched here ON PURPOSE, not defaulted."),
			       GBufferASlot,
			       GBufferA ? GBufferA->Name : TEXT("<null>"),
			       GBufferA ? GBufferA->Desc.Extent.X : 0,
			       GBufferA ? GBufferA->Desc.Extent.Y : 0,
			       SceneDepth ? SceneDepth->Desc.Extent.X : 0,
			       SceneDepth ? SceneDepth->Desc.Extent.Y : 0, *Slots);
		}
		return;
	}
	{
		// The one-time binding attestation -- name, slot, extent, format. The
		// instrument names what it is actually reading, per
		// [[voxelsim-instrument-must-run-the-engine-binding]]; the per-window
		// half of the same check is the nonAxisN census canary.
		static bool bLoggedFoundGBufferA = false;
		if (!bLoggedFoundGBufferA)
		{
			bLoggedFoundGBufferA = true;
			UE_LOG(LogVoxelShadowMarch, Display,
			       TEXT("shadow march: GBufferA bound from MRT slot %d, name='%s', ")
			            TEXT("extent=%dx%d, format=%s."),
			       GBufferASlot, GBufferA->Name, GBufferA->Desc.Extent.X,
			       GBufferA->Desc.Extent.Y, GPixelFormats[GBufferA->Desc.Format].Name);
		}
	}

	// ---- the march frame: camera-anchored, chunk-snapped ------------------
	//
	// The marcher's source-1 frame construction, verbatim in structure: a pure
	// function of the pose (no recentre hysteresis, no history), snapped DOWN
	// to a 32-voxel chunk boundary so chunk-coordinate arithmetic stays exact
	// and sub-chunk camera jitter cannot move the origin. Sized to hold both
	// populations this pass touches: surfaces up to ReachM from the camera,
	// plus a ray of up to ReachM from any of them -- so half-extent 2x reach.
	// THE BOX IS CENTRED ON THE CAMERA, NOT CORNERED, AND THAT IS A PRECISION
	// PREREQUISITE FOR RAISING REACH -- not a tidiness change.
	//
	// Cornered at 2 x reach, |RayOriginLocalUU| is about 2*reach, and float32's
	// ulp there reaches the traversal's 0.01 UU advance nudge
	// (VOXEL_MARCH_HIER_NUDGE_UU) at roughly 419 m of reach. Above that the DDA
	// stalls or skips silently. The cvar already permits 512 m. So the geometry
	// had to move before the reach could, independently of anything else.
	//
	// This is the camera marcher's own arithmetic (VoxelMarchRenderer.cpp's ring
	// frame): snap the CAMERA down and size the box symmetrically about it, so
	// local coordinates stay near zero where float32 has headroom.
	const int32 ReachVoxels = FMath::CeilToInt(Arm.ReachM * 10.0f);
	const auto SnapDown = [](int32 V) { return (V >> 5) << 5; };
	const FIntVector FrameOriginVoxel(SnapDown(Stash->CameraVoxel.X - ReachVoxels),
	                                  SnapDown(Stash->CameraVoxel.Y - ReachVoxels),
	                                  SnapDown(Stash->CameraVoxel.Z - ReachVoxels));
	// Differenced in DOUBLE against the true camera position -- the precision
	// seam recorded at the stash.
	const FVector FrameOriginUU(double(FrameOriginVoxel.X) * 10.0,
	                            double(FrameOriginVoxel.Y) * 10.0,
	                            double(FrameOriginVoxel.Z) * 10.0);
	const FVector3f RayOriginLocalUU = FVector3f(Stash->ViewOriginUU - FrameOriginUU);
	// 2 x half-extent, plus one chunk of slack for the snap -- the marcher's
	// own sizing arithmetic.
	const float FrameExtentUU = float(2 * ReachVoxels + 64) * 10.0f;

	// The verify kernel's sun: identical unless the mutation arm asks for a
	// rotation, in which case the replay MUST disagree -- see the cvar text.
	FVector3f VerifyDirToSun = DirToSun;
	if (Arm.VerifySunNudgeDeg != 0.0f)
	{
		const FVector D(DirToSun);
		const FVector Up = (FMath::Abs(D.Z) < 0.99) ? FVector::UpVector : FVector::ForwardVector;
		const FVector Axis = FVector::CrossProduct(D, Up).GetSafeNormal();
		VerifyDirToSun = FVector3f(
			FQuat(Axis, FMath::DegreesToRadians(Arm.VerifySunNudgeDeg)).RotateVector(D));
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelShadowMarch, "VoxelShadowMarch");

	const FIntPoint Size = Stash->ViewRect.Size();

	// The mask, at the SCENE extent and indexed by absolute pixel, because that
	// is the addressing S2's light function will sample it with (buffer UV over
	// the scene extent) -- the shader does not change between S1 and S2.
	// TRANSIENT in S1; S2 adds a copy into a persistent render target. Cleared
	// to LIT: pixels nothing writes (outside every view rect) must read "no
	// shadow", the fail-lit rule again.
	FRDGTextureRef Mask = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(SceneDepth->Desc.Extent, PF_G8, FClearValueBinding::White,
		                          TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("VoxelShadow.Mask"));
	FRDGTextureUAVRef MaskUAV = GraphBuilder.CreateUAV(Mask);

	FRDGBufferRef Stats = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
		                                 uint32(FVoxelShadowMarchState::kStatsWords)),
		TEXT("VoxelShadow.Stats"));
	FRDGBufferUAVRef StatsUAV = GraphBuilder.CreateUAV(Stats, PF_R32_UINT);

	// The example-dump buffer exists every frame (the shader's parameter map
	// names it unconditionally; 513 words is nothing) but is cleared and read
	// back only under voxel.Shadow.MarchDiag.
	FRDGBufferRef InsideDump = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32),
		                                 uint32(FVoxelShadowMarchState::kDumpWords)),
		TEXT("VoxelShadow.InsideDump"));
	FRDGBufferUAVRef InsideDumpUAV = GraphBuilder.CreateUAV(InsideDump, PF_R32_UINT);

	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelShadowMarchParameters>();
		// BINDS BEFORE THE BRACKET, and the order is load-bearing for the
		// census: declines happen for whole stretches of every cold start (the
		// pool has not flushed, the index has not uploaded), and a bracket
		// opened before the binds would retire clear-only samples into the
		// GPU-ms window and drag the mean toward zero exactly when nothing was
		// measured. A declined frame must contribute a DECLINE COUNT and no
		// timing sample.
		if (!GetGlobalVoxelBrickPool().BindShaderParameters(GraphBuilder, *Params))
		{
			State->DeclinedNoPool++;
			return;
		}
		// THE INDEX RETURNING NULL IS NOT A DETAIL (VoxelMarchRenderer.cpp's
		// words): an unbound SRV reads as zeros, zero has kResidentBit clear,
		// every lookup would miss, and the whole screen would read LIT with no
		// error anywhere. Declining the frame is the only safe response.
		FRDGBufferRef IndexBuffer = GetGlobalVoxelMarchChunkIndex().Register(GraphBuilder);
		if (IndexBuffer == nullptr)
		{
			State->DeclinedNoIndex++;
			return;
		}

		// The bracket covers exactly what a shipping frame would pay: both
		// clears plus the march. The verify dispatch is recorded AFTER the
		// close and is therefore outside the number -- and timing legs keep it
		// off anyway.
		FVoxelShadowMarchState::FTimingPair* Timing =
			VoxelShadowOpenBracket(GraphBuilder, State->Timing);
		AddClearUAVPass(GraphBuilder, MaskUAV, 1.0f);
		AddClearUAVPass(GraphBuilder, StatsUAV, 0u);
		if (Arm.bDiag)
		{
			AddClearUAVPass(GraphBuilder, InsideDumpUAV, 0u);
		}

		Params->MarchChunkIndex = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
		Params->MarchIndexDimChunks = FUintVector(FVoxelMarchChunkIndex::kDimXY,
		                                          FVoxelMarchChunkIndex::kDimXY,
		                                          FVoxelMarchChunkIndex::kDimZ);
		Params->MarchBrickOriginVoxel = FrameOriginVoxel;
		Params->MarchStepBudget = Arm.Budget;

		Params->ShadowViewToTranslatedWorld = Stash->ViewToTranslatedWorld;
		Params->ShadowRayOriginLocalUU = RayOriginLocalUU;
		Params->ShadowDirToSun = DirToSun;
		Params->ShadowVerifyDirToSun = VerifyDirToSun;
		Params->ShadowVolumeExtentUU = FrameExtentUU;
		Params->MarchIndexCellsPerLevel = FVoxelMarchChunkIndex::kCellsPerLevel;
		// Ring geometry, from the same presets the streaming cascade is built
		// from. R0's outer radius is the ONE number; ring L is
		// [R0*2^(L-1), R0*2^L), which is kDefaultRingPresets exactly.
		// READ FROM THE MARCHER'S OWN CVARS, not from UVoxelWorldSubsystem:
		// VoxelEarthShaders may not depend on VoxelEarth. These are the same two
		// values VoxelMarchRenderer binds as MarchRingCount / MarchRing0OuterUU,
		// so both passes walk one cascade rather than two derivations of it.
		Params->ShadowRingCount = VoxelShadowGetRingCount();
		Params->ShadowRing0OuterUU = VoxelShadowGetRing0OuterUU();
		Params->ShadowViewRectMin = FVector2f(Stash->ViewRect.Min);
		Params->ShadowViewRectSize = FVector2f(Size);
		Params->ShadowInvProjDiag = Stash->InvProjDiag;
		Params->ShadowInvDeviceZToWorldZ = Stash->InvDeviceZToWorldZ;
		Params->ShadowSurfaceReachUU = Arm.ReachM * 100.0f;
		Params->ShadowRayReachUU = VoxelShadowGetRayReachUU(Arm.ReachM);
		Params->ShadowNormalOffsetUU = Arm.NormalOffsetVoxels * 10.0f;
		Params->ShadowPixelConeSlope = Stash->PixelConeSlope;
		Params->ShadowPullbackPx = Arm.PullbackPx;
		Params->ShadowVerifyStride = uint32(Arm.VerifyStride);
		Params->ShadowDiagEnabled = Arm.bDiag ? 1u : 0u;
		Params->ShadowInsideDumpCap = uint32(FVoxelShadowMarchState::kDumpRecords);
		Params->ShadowOutInsideDump = InsideDumpUAV;
		Params->ShadowSceneDepthTexture = SceneDepth;
		Params->ShadowGBufferATexture = GBufferA;
		Params->ShadowOutMask = MaskUAV;
		Params->ShadowOutStats = StatsUAV;

		TShaderMapRef<FVoxelShadowMarchCS> Shader(ShaderMap);
		const FIntVector Groups(FMath::DivideAndRoundUp(Size.X, kVoxelShadowTileSize),
		                        FMath::DivideAndRoundUp(Size.Y, kVoxelShadowTileSize), 1);
		FComputeShaderUtils::AddPass(GraphBuilder,
		                             RDG_EVENT_NAME("VoxelShadowMarch.March(%dx%d reach=%.0fm)",
		                                            Size.X, Size.Y, Arm.ReachM),
		                             ERDGPassFlags::Compute, Shader, Params, Groups);

		// ---- S2: hand the mask to the light function -----------------------
		//
		// Inside the timing bracket on purpose: the copy is part of what a
		// shipping mode-2 frame pays for this pass. What the bracket CANNOT
		// see is the light function's own evaluation, which happens inside the
		// engine's RenderLights -- the frame-level A/B is the number that
		// includes it.
		if (Arm.Mode == 2)
		{
			FTextureRenderTargetResource* TargetResource = nullptr;
			FIntPoint TargetExtent = FIntPoint::ZeroValue;
			{
				FScopeLock Guard(&State->Lock);
				// Tell the game thread what size the target must be -- this is
				// how the subsystem learns (and re-learns, on a resolution
				// change) the buffer extent.
				State->DesiredMaskExtent = SceneDepth->Desc.Extent;
				TargetResource = State->MaskTargetResource;
				TargetExtent = State->MaskTargetExtent;
			}
			FRHITexture* TargetRHI =
				(TargetResource != nullptr) ? TargetResource->GetRenderTargetTexture() : nullptr;
			if (TargetRHI != nullptr && TargetExtent == SceneDepth->Desc.Extent)
			{
				FRDGTextureRef Dest =
					RegisterExternalTexture(GraphBuilder, TargetRHI, TEXT("VoxelShadow.MaskTarget"));
				if (Arm.bMutateInjection)
				{
					// The mutation arm: a uniform 0.5. The sun must read
					// half-dark everywhere or the injection is not live.
					AddClearRenderTargetPass(GraphBuilder, Dest,
					                         FLinearColor(0.5f, 0.5f, 0.5f, 1.0f));
					State->InjectionMutated++;
				}
				else if (Dest->Desc.Format == Mask->Desc.Format &&
				         Dest->Desc.Extent == Mask->Desc.Extent)
				{
					AddCopyTexturePass(GraphBuilder, Mask, Dest);
					State->InjectionCopies++;
				}
				else
				{
					State->DeclinedNoTarget++;
					static bool bLoggedFormatMismatch = false;
					if (!bLoggedFormatMismatch)
					{
						bLoggedFormatMismatch = true;
						UE_LOG(LogVoxelShadowMarch, Warning,
						       TEXT("shadow march injection: target format/extent mismatch ")
						            TEXT("(mask %s %dx%d vs target %s %dx%d) -- copies decline, ")
						            TEXT("counted. The subsystem creates the target with ")
						            TEXT("InitCustomFormat(PF_G8); a change there must be ")
						            TEXT("mirrored in the mask's CreateTexture."),
						       GPixelFormats[Mask->Desc.Format].Name, Mask->Desc.Extent.X,
						       Mask->Desc.Extent.Y, GPixelFormats[Dest->Desc.Format].Name,
						       Dest->Desc.Extent.X, Dest->Desc.Extent.Y);
					}
				}
			}
			else
			{
				// Target not published (yet): first frames, a resize in flight,
				// or the material/subsystem declined. Counted, never silent.
				State->DeclinedNoTarget++;
			}
		}

		VoxelShadowCloseBracket(GraphBuilder, Timing);

		// ---- G-S1's replay, floor pair and mutation arm --------------------
		if (Arm.VerifyStride > 0)
		{
			auto* VParams = GraphBuilder.AllocParameters<FVoxelShadowMarchParameters>();
			// A SEPARATE set of bindings, deliberately: the replay's whole value
			// in S1 is that both sides bound the same world and the mask landed
			// where the census thinks ([[voxelsim-instrument-must-run-the-
			// engine-binding]] is the recorded lesson, applied from the other
			// side).
			if (GetGlobalVoxelBrickPool().BindShaderParameters(GraphBuilder, *VParams))
			{
				VParams->MarchChunkIndex = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
				VParams->MarchIndexDimChunks = Params->MarchIndexDimChunks;
				VParams->MarchBrickOriginVoxel = FrameOriginVoxel;
				VParams->MarchStepBudget = Arm.Budget;
				VParams->ShadowViewToTranslatedWorld = Stash->ViewToTranslatedWorld;
				VParams->ShadowRayOriginLocalUU = RayOriginLocalUU;
				VParams->ShadowDirToSun = DirToSun;
				VParams->ShadowVerifyDirToSun = VerifyDirToSun;
				VParams->ShadowVolumeExtentUU = FrameExtentUU;
				VParams->ShadowViewRectMin = FVector2f(Stash->ViewRect.Min);
				VParams->ShadowViewRectSize = FVector2f(Size);
				VParams->ShadowInvProjDiag = Stash->InvProjDiag;
				VParams->ShadowInvDeviceZToWorldZ = Stash->InvDeviceZToWorldZ;
				VParams->ShadowSurfaceReachUU = Arm.ReachM * 100.0f;
				VParams->ShadowRayReachUU = VoxelShadowGetRayReachUU(Arm.ReachM);
				VParams->ShadowNormalOffsetUU = Arm.NormalOffsetVoxels * 10.0f;
				VParams->ShadowPixelConeSlope = Stash->PixelConeSlope;
				VParams->ShadowPullbackPx = Arm.PullbackPx;
				VParams->ShadowVerifyStride = uint32(Arm.VerifyStride);
				VParams->ShadowDiagEnabled = 0u;
				VParams->ShadowInsideDumpCap = 0u;
				VParams->ShadowOutInsideDump = InsideDumpUAV;
				VParams->ShadowSceneDepthTexture = SceneDepth;
				VParams->ShadowGBufferATexture = GBufferA;
				VParams->ShadowMaskTexture = Mask;
				VParams->ShadowOutStats = StatsUAV;

				TShaderMapRef<FVoxelShadowVerifyCS> VShader(ShaderMap);
				const FIntVector VGroups(FMath::DivideAndRoundUp(Size.X, kVoxelShadowTileSize),
				                         FMath::DivideAndRoundUp(Size.Y, kVoxelShadowTileSize), 1);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("VoxelShadowMarch.Verify(stride=%d nudge=%.2fdeg)",
					               Arm.VerifyStride, Arm.VerifySunNudgeDeg),
					ERDGPassFlags::Compute, VShader, VParams, VGroups);
			}
			else
			{
				State->MaskUnavailableForVerify++;
			}
		}

		// ---- the readback ---------------------------------------------------
		FVoxelShadowMarchState::FReadbackSlot* Slot = nullptr;
		for (int32 i = 0; i < FVoxelShadowMarchState::kNumReadbacks; ++i)
		{
			if (!State->Readbacks[i].bInFlight)
			{
				Slot = &State->Readbacks[i];
				break;
			}
		}
		if (Slot != nullptr)
		{
			if (!Slot->Readback.IsValid())
			{
				Slot->Readback =
					MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelShadow.StatsReadback"));
			}
			AddEnqueueCopyPass(GraphBuilder, Slot->Readback.Get(), Stats,
			                   uint32(FVoxelShadowMarchState::kStatsWords) * sizeof(uint32));
			Slot->bInFlight = true;
		}
		// A frame with no free slot simply goes uncounted -- the census divides
		// by frames RETIRED, so an uncounted frame biases nothing.

		if (Arm.bDiag)
		{
			for (int32 i = 0; i < 2; ++i)
			{
				FVoxelShadowMarchState::FReadbackSlot& DSlot = State->DumpReadbacks[i];
				if (DSlot.bInFlight)
				{
					continue;
				}
				if (!DSlot.Readback.IsValid())
				{
					DSlot.Readback =
						MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelShadow.DumpReadback"));
				}
				AddEnqueueCopyPass(GraphBuilder, DSlot.Readback.Get(), InsideDump,
				                   uint32(FVoxelShadowMarchState::kDumpWords) * sizeof(uint32));
				DSlot.bInFlight = true;
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Retirement and the census
// ---------------------------------------------------------------------------

void FVoxelShadowMarchExtension::RetireGpuWork()
{
	if (!State.IsValid())
	{
		return;
	}

	// Timing.
	{
		float NewMs = -1.0f;
		for (int32 i = 0; i < FVoxelShadowMarchState::kNumTimingPairs; ++i)
		{
			FVoxelShadowMarchState::FTimingPair& Pair = State->Timing[i];
			if (!Pair.bInFlight)
			{
				continue;
			}
			uint64 BeginMicros = 0;
			uint64 EndMicros = 0;
			if (RHIGetRenderQueryResult(Pair.End.GetReference(), EndMicros, false) &&
			    RHIGetRenderQueryResult(Pair.Begin.GetReference(), BeginMicros, false))
			{
				NewMs = float(double(EndMicros - BeginMicros) / 1000.0);
				Pair.bInFlight = false;
			}
		}
		if (NewMs >= 0.0f)
		{
			State->LastGpuMs = NewMs;
			State->WindowGpuMsSum += NewMs;
			State->WindowGpuMsMax = FMath::Max(State->WindowGpuMsMax, NewMs);
			State->WindowGpuMsCount++;
		}
	}

	// Stats readbacks.
	for (int32 i = 0; i < FVoxelShadowMarchState::kNumReadbacks; ++i)
	{
		FVoxelShadowMarchState::FReadbackSlot& Slot = State->Readbacks[i];
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		const uint32 Bytes = uint32(FVoxelShadowMarchState::kStatsWords) * sizeof(uint32);
		const uint32* Data = static_cast<const uint32*>(Slot.Readback->Lock(Bytes));
		if (Data != nullptr)
		{
			for (int32 w = 0; w < FVoxelShadowMarchState::kStatsWords; ++w)
			{
				if (w == VoxelShadowStat::MaxSteps)
				{
					State->WindowMaxSteps = FMath::Max(State->WindowMaxSteps, Data[w]);
				}
				else
				{
					State->WindowStats[w] += uint64(Data[w]);
				}
			}
			State->WindowFrames++;
		}
		Slot.Readback->Unlock();
		Slot.bInFlight = false;
	}

	// The dump ring: keep only the latest landed payload; the census prints it.
	for (int32 i = 0; i < 2; ++i)
	{
		FVoxelShadowMarchState::FReadbackSlot& DSlot = State->DumpReadbacks[i];
		if (!DSlot.bInFlight || !DSlot.Readback.IsValid() || !DSlot.Readback->IsReady())
		{
			continue;
		}
		const uint32 Bytes = uint32(FVoxelShadowMarchState::kDumpWords) * sizeof(uint32);
		const uint32* Data = static_cast<const uint32*>(DSlot.Readback->Lock(Bytes));
		if (Data != nullptr)
		{
			State->LastDumpWords.SetNumUninitialized(FVoxelShadowMarchState::kDumpWords);
			FMemory::Memcpy(State->LastDumpWords.GetData(), Data, Bytes);
		}
		DSlot.Readback->Unlock();
		DSlot.bInFlight = false;
	}

	EmitCensusIfDue();
}

void FVoxelShadowMarchExtension::EmitCensusIfDue()
{
	const FVoxelShadowArm Arm = VoxelShadowGetArm();
	if (State->WindowFrames < uint64(Arm.StatsPeriod))
	{
		return;
	}

	const uint64* W = State->WindowStats;
	const double Frames = double(State->WindowFrames);
	const uint64 Marched = W[VoxelShadowStat::Marched];

	if (Marched == 0)
	{
		// The refusal path: zeros that look healthy are how tonight's counters
		// lied. Name every reason the rays did not run.
		UE_LOG(LogVoxelShadowMarch, Warning,
		       TEXT("shadowmarch: NO RAYS MARCHED over %llu retired frames -- refusing a ")
		            TEXT("healthy-looking census. declines: noView=%llu noSun=%llu noPool=%llu ")
		            TEXT("noIndex=%llu noTextures=%llu | per-frame considered=%.0f sky=%.0f ")
		            TEXT("far=%.0f backface=%.0f offbox=%.0f | idxEntries=%d"),
		       State->WindowFrames, State->DeclinedNoView, State->DeclinedNoSun,
		       State->DeclinedNoPool, State->DeclinedNoIndex, State->DeclinedNoTextures,
		       double(W[VoxelShadowStat::Considered]) / Frames,
		       double(W[VoxelShadowStat::Sky]) / Frames, double(W[VoxelShadowStat::Far]) / Frames,
		       double(W[VoxelShadowStat::Backface]) / Frames,
		       double(W[VoxelShadowStat::OffBox]) / Frames,
		       GetGlobalVoxelMarchChunkIndex().GetNumEntries());
	}
	else
	{
		// Totals as total/count over the window -- never sums of per-frame
		// means (the recorded 870x-denominator lesson).
		const double MeanSteps = double(W[VoxelShadowStat::StepsTotal]) / double(Marched);
		// p95 from the power-of-two histogram: the value reported is the
		// BUCKET'S UPPER EDGE, i.e. "95% of marched rays took <= this many
		// steps", quantised to the bucket boundaries. An approximation, and
		// says so in the field name.
		uint32 P95Edge = 1;
		{
			const uint64 Threshold = (uint64)(0.95 * double(Marched));
			uint64 Cum = 0;
			for (int32 b = 0; b < VoxelShadowStat::HistBuckets; ++b)
			{
				Cum += W[VoxelShadowStat::Hist0 + b];
				P95Edge = 1u << b;
				if (Cum >= Threshold)
				{
					break;
				}
			}
		}
		const double GpuMsMean = (State->WindowGpuMsCount > 0)
		                             ? State->WindowGpuMsSum / double(State->WindowGpuMsCount)
		                             : -1.0;
		UE_LOG(LogVoxelShadowMarch, Display,
		       TEXT("shadowmarch: mode=%d reach=%.1fm budget=%d offsetVox=%.2f pullbackPx=%.2f ")
		            TEXT("frames=%llu | ")
		            TEXT("gpuMs mean=%.3f max=%.3f n=%u | per-frame: considered=%.0f sky=%.0f ")
		            TEXT("far=%.0f backface=%.0f offbox=%.0f marched=%.0f nonAxisN/f=%.1f | hit%%=%.1f ")
		            TEXT("inside/f=%.2f (near=%.0f mid=%.0f far=%.0f) exhausted/f=%.2f | ")
		            TEXT("steps/marched mean=%.1f p95edge<=%u ")
		            TEXT("max=%u | idxEntries=%d declines nv=%llu ns=%llu np=%llu ni=%llu nt=%llu"),
		       Arm.Mode, Arm.ReachM, Arm.Budget, Arm.NormalOffsetVoxels, Arm.PullbackPx,
		       State->WindowFrames,
		       GpuMsMean, State->WindowGpuMsMax, State->WindowGpuMsCount,
		       double(W[VoxelShadowStat::Considered]) / Frames,
		       double(W[VoxelShadowStat::Sky]) / Frames, double(W[VoxelShadowStat::Far]) / Frames,
		       double(W[VoxelShadowStat::Backface]) / Frames,
		       double(W[VoxelShadowStat::OffBox]) / Frames, double(Marched) / Frames,
		       double(W[VoxelShadowStat::NonAxisNormal]) / Frames,
		       100.0 * double(W[VoxelShadowStat::Hit]) / double(Marched),
		       double(W[VoxelShadowStat::StartedInside]) / Frames,
		       double(W[VoxelShadowStat::InsideNear]) / Frames,
		       double(W[VoxelShadowStat::InsideMid]) / Frames,
		       double(W[VoxelShadowStat::InsideFar]) / Frames,
		       double(W[VoxelShadowStat::Exhausted]) / Frames, MeanSteps, P95Edge,
		       State->WindowMaxSteps, GetGlobalVoxelMarchChunkIndex().GetNumEntries(),
		       State->DeclinedNoView, State->DeclinedNoSun, State->DeclinedNoPool,
		       State->DeclinedNoIndex, State->DeclinedNoTextures);
	}

	// The per-band RATES -- the field leg D1 could not answer: a band with more
	// rays and more insides is indistinguishable from a band that is worse
	// until the denominator exists. Printed as inside/marched per band.
	if (Marched > 0)
	{
		const auto Rate = [&](int32 Num, int32 Den) -> double
		{
			return (W[Den] > 0) ? 100.0 * double(W[Num]) / double(W[Den]) : -1.0;
		};
		UE_LOG(LogVoxelShadowMarch, Display,
		       TEXT("shadowbands: marched/f near=%.0f mid=%.0f far=%.0f | inside RATE ")
		            TEXT("near=%.1f%% mid=%.1f%% far=%.1f%%"),
		       double(W[VoxelShadowStat::MarchedNear]) / Frames,
		       double(W[VoxelShadowStat::MarchedMid]) / Frames,
		       double(W[VoxelShadowStat::MarchedFar]) / Frames,
		       Rate(VoxelShadowStat::InsideNear, VoxelShadowStat::MarchedNear),
		       Rate(VoxelShadowStat::InsideMid, VoxelShadowStat::MarchedMid),
		       Rate(VoxelShadowStat::InsideFar, VoxelShadowStat::MarchedFar));
	}

	// The diagnosis block, present only when its counters were populated.
	{
		const uint64 BuriedTotal = W[VoxelShadowStat::Buried1] + W[VoxelShadowStat::Buried2] +
		                           W[VoxelShadowStat::Buried3] + W[VoxelShadowStat::Buried4Plus];
		if (BuriedTotal > 0)
		{
			UE_LOG(LogVoxelShadowMarch, Display,
			       TEXT("shadowdiag: burial of inside origins (first air ABOVE, voxels): ")
			            TEXT("1=%.1f%% 2=%.1f%% 3=%.1f%% 4plus=%.1f%% | inside voxel material: ")
			            TEXT("bedrock/rock=%.1f%% surface=%.1f%%"),
			       100.0 * double(W[VoxelShadowStat::Buried1]) / double(BuriedTotal),
			       100.0 * double(W[VoxelShadowStat::Buried2]) / double(BuriedTotal),
			       100.0 * double(W[VoxelShadowStat::Buried3]) / double(BuriedTotal),
			       100.0 * double(W[VoxelShadowStat::Buried4Plus]) / double(BuriedTotal),
			       100.0 * double(W[VoxelShadowStat::InsideMatDeep]) / double(BuriedTotal),
			       100.0 * double(W[VoxelShadowStat::InsideMatSurf]) / double(BuriedTotal));

			// Example records -- ACTUAL failing rays. World voxel included so
			// each line can be checked offline against the CPU reference world.
			if (State->LastDumpWords.Num() == FVoxelShadowMarchState::kDumpWords)
			{
				static const TCHAR* MatNames[16] = {
					TEXT("AIR"),     TEXT("BEDROCK"),  TEXT("ROCK"),        TEXT("GRAVEL"),
					TEXT("SAND"),    TEXT("SUBSOIL"),  TEXT("TOPSOIL"),     TEXT("SNOW"),
					TEXT("GRASS"),   TEXT("JUNGLE"),   TEXT("SAVANNA"),     TEXT("PODZOL"),
					TEXT("PERMAFROST"), TEXT("MUD"),   TEXT("CLAY"),        TEXT("WATERMARK")};
				const uint32* D = State->LastDumpWords.GetData();
				const int32 NumRecords = FMath::Min(int32(D[0]),
				                                    FVoxelShadowMarchState::kDumpRecords);
				for (int32 r = 0; r < FMath::Min(NumRecords, 8); ++r)
				{
					const uint32* R = D + 1 + r * 8;
					const float TSurf = *reinterpret_cast<const float*>(&R[1]);
					const float Nx = float((R[2] >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
					const float Ny = float((R[2] >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
					const float Nz = float(R[2] & 0x3FF) / 1023.0f * 2.0f - 1.0f;
					const uint32 Mat = R[3] & 0xFF;
					const uint32 Burial = (R[3] >> 8) & 0xF;
					const uint32 Band = (R[3] >> 12) & 0x3;
					const int32 Vx = int32(R[4]);
					const int32 Vy = int32(R[5]);
					const int32 Vz = int32(R[6]);
					const float NdotL = *reinterpret_cast<const float*>(&R[7]);
					UE_LOG(LogVoxelShadowMarch, Display,
					       TEXT("shadowdiag:  ex%d px=(%u,%u) tSurf=%.1fm N=(%.2f,%.2f,%.2f) ")
					            TEXT("NdotL=%.2f mat=%s burial=%u band=%u worldVoxel=(%d,%d,%d) ")
					            TEXT("= (%.1f, %.1f, %.1f) m"),
					       r, R[0] >> 16, R[0] & 0xFFFF, TSurf / 100.0f, Nx, Ny, Nz, NdotL,
					       (Mat < 16) ? MatNames[Mat] : TEXT("?"), Burial, Band, Vx, Vy, Vz,
					       double(Vx) * 0.1, double(Vy) * 0.1, double(Vz) * 0.1);
				}
			}
		}
	}

	// The verify verdict, graded against the floor measured in the SAME window
	// on the SAME rays -- calibration against a control drawn from the same
	// population, never a loosened threshold.
	if (Arm.VerifyStride > 0)
	{
		const uint64 Compared = W[VoxelShadowStat::VCompared];
		const uint64 Disagree = W[VoxelShadowStat::VDisagree];
		const uint64 RefSelf = W[VoxelShadowStat::VRefSelf];
		if (Compared == 0)
		{
			UE_LOG(LogVoxelShadowMarch, Warning,
			       TEXT("shadowverify: NO SAMPLES over %llu frames (stride=%d, skipped=%llu, ")
			            TEXT("maskUnavailable=%llu) -- REFUSING A VERDICT. A gate with no ")
			            TEXT("denominator is a check that cannot fail."),
			       State->WindowFrames, Arm.VerifyStride, W[VoxelShadowStat::VSkipped],
			       State->MaskUnavailableForVerify);
		}
		else
		{
			const TCHAR* Verdict =
				(Disagree == 0)
					? TEXT("AGREES")
					: ((Disagree <= RefSelf) ? TEXT("AT REFERENCE NOISE FLOOR (not proof)")
					                         : TEXT("DISAGREES"));
			UE_LOG(LogVoxelShadowMarch, Display,
			       TEXT("shadowverify: %s -- compared=%llu disagree=%llu (walkShadowed/maskLit=%llu ")
			            TEXT("walkLit/maskShadowed=%llu) refSelfDisagree=%llu skipped=%llu ")
			            TEXT("sunNudgeDeg=%.2f%s"),
			       Verdict, Compared, Disagree, W[VoxelShadowStat::VDisShadowed],
			       W[VoxelShadowStat::VDisLit], RefSelf, W[VoxelShadowStat::VSkipped],
			       Arm.VerifySunNudgeDeg,
			       (Arm.VerifySunNudgeDeg != 0.0f)
			           ? TEXT("  [MUTATION ARM ACTIVE -- this run MUST read DISAGREES; ")
			                 TEXT("record it beside the gate]")
			           : TEXT(""));
		}
	}

	// The injection census -- only printed when mode 2 did anything, and loud
	// about the mutation arm being active.
	if (State->InjectionCopies + State->InjectionMutated + State->DeclinedNoTarget > 0)
	{
		UE_LOG(LogVoxelShadowMarch, Display,
		       TEXT("shadowinject: copies=%llu mutated=%llu declinedNoTarget=%llu%s"),
		       State->InjectionCopies, State->InjectionMutated, State->DeclinedNoTarget,
		       (State->InjectionMutated > 0)
		           ? TEXT("  [MUTATION ARM ACTIVE -- direct sun must read HALF-DARK ")
		                 TEXT("everywhere; a normal-looking frame means the injection is ")
		                 TEXT("NOT live]")
		           : TEXT(""));
	}

	// Reset the window.
	FMemory::Memzero(State->WindowStats, sizeof(State->WindowStats));
	State->WindowMaxSteps = 0;
	State->WindowFrames = 0;
	State->WindowGpuMsSum = 0.0;
	State->WindowGpuMsMax = 0.0f;
	State->WindowGpuMsCount = 0;
	State->DeclinedNoView = 0;
	State->DeclinedNoSun = 0;
	State->DeclinedNoPool = 0;
	State->DeclinedNoIndex = 0;
	State->DeclinedNoTextures = 0;
	State->MaskUnavailableForVerify = 0;
	State->InjectionCopies = 0;
	State->InjectionMutated = 0;
	State->DeclinedNoTarget = 0;
}

// ===========================================================================
// The subsystem
// ===========================================================================

bool UVoxelShadowMarchSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr &&
	       (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UVoxelShadowMarchSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	State = MakeShared<FVoxelShadowMarchState, ESPMode::ThreadSafe>();
	Extension = FSceneViewExtensions::NewExtension<FVoxelShadowMarchExtension>(&InWorld, State);

	// Idempotent, game thread; the marcher's publish path also calls this. Done
	// here as well so the shadow march does not depend on the fluid publisher
	// having run -- the index seeding itself late is the NORMAL case, per
	// FVoxelMarchChunkIndex::AttachToGlobalPool's own comment.
	GetGlobalVoxelMarchChunkIndex().AttachToGlobalPool();

	UE_LOG(LogVoxelShadowMarch, Display,
	       TEXT("Voxel shadow march extension registered (march at PostRenderBasePassDeferred, ")
	            TEXT("priority -10 = after the primary marcher's emit). voxel.Shadow.March 0 = off ")
	            TEXT("(default); 1 = march to scratch + census, nothing visible changes."));
}

void UVoxelShadowMarchSubsystem::Deinitialize()
{
	if (bInjectionWired)
	{
		if (UDirectionalLightComponent* Sun = SunComponent.Get())
		{
			Sun->SetLightFunctionMaterial(nullptr);
		}
		bInjectionWired = false;
	}
	Extension.Reset();
	State.Reset();
	Super::Deinitialize();
}

bool UVoxelShadowMarchSubsystem::IsTickable() const
{
	return State.IsValid();
}

TStatId UVoxelShadowMarchSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelShadowMarchSubsystem, STATGROUP_Tickables);
}

void UVoxelShadowMarchSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!State.IsValid())
	{
		return;
	}
	const int32 Mode = CVarVoxelShadowMarch.GetValueOnGameThread();
	if (Mode == 0)
	{
		// STILL unwire before returning: a session that drops 2 -> 0 must not
		// leave the light function on the sun -- an early return here was the
		// one path that could strand it, wired forever with a stale mask.
		UpdateInjection(0);
		return;
	}

	// Find the sun: the directional light flagged as atmosphere sun index 0 --
	// what VoxelSkySubsystem::SpawnRig configures -- else the first directional
	// light. Re-found automatically if the actor is respawned (weak ptr goes
	// stale); the choice is logged once so two suns cannot be a silent
	// ambiguity.
	if (!SunComponent.IsValid())
	{
		// A stale sun (respawned actor) also invalidates the light-function
		// wiring -- the NEW component has no light function until
		// UpdateInjection re-applies it.
		bInjectionWired = false;
		UDirectionalLightComponent* Chosen = nullptr;
		UDirectionalLightComponent* Fallback = nullptr;
		int32 Candidates = 0;
		for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
		{
			UDirectionalLightComponent* C =
				Cast<UDirectionalLightComponent>(It->GetLightComponent());
			if (C == nullptr)
			{
				continue;
			}
			Candidates++;
			if (Fallback == nullptr)
			{
				Fallback = C;
			}
			if (C->IsUsedAsAtmosphereSunLight() && C->GetAtmosphereSunLightIndex() == 0)
			{
				Chosen = C;
				break;
			}
		}
		if (Chosen == nullptr)
		{
			Chosen = Fallback;
		}
		if (Chosen != nullptr)
		{
			SunComponent = Chosen;
			if (!bLoggedSunChoice)
			{
				bLoggedSunChoice = true;
				UE_LOG(LogVoxelShadowMarch, Display,
				       TEXT("shadow march sun: '%s' (%d directional light(s) present, ")
				            TEXT("atmosphere-sun preferred)"),
				       *Chosen->GetOwner()->GetName(), Candidates);
			}
			bLoggedNoSun = false;
		}
		else if (!bLoggedNoSun)
		{
			bLoggedNoSun = true;
			UE_LOG(LogVoxelShadowMarch, Warning,
			       TEXT("voxel.Shadow.March %d requested but NO DIRECTIONAL LIGHT exists yet. ")
			            TEXT("Every frame declines with reason noSun until one appears -- the ")
			            TEXT("census names it; this is not silent."),
			       Mode);
		}
	}

	// Feed the direction. A DERIVED copy of what the light shades with -- the
	// registered trap and its check are documented at the state field.
	if (SunComponent.IsValid())
	{
		const FVector DirToSun = -SunComponent->GetDirection();
		FScopeLock Guard(&State->Lock);
		State->SunDirToSunWorld = FVector3f(DirToSun.GetSafeNormal());
		State->bSunValid = true;
	}
	else
	{
		FScopeLock Guard(&State->Lock);
		State->bSunValid = false;
	}

	UpdateInjection(Mode);
}

// ---------------------------------------------------------------------------
// S2 -- the light-function wiring
// ---------------------------------------------------------------------------
void UVoxelShadowMarchSubsystem::UpdateInjection(int32 Mode)
{
	if (Mode != 2)
	{
		if (bInjectionWired)
		{
			if (UDirectionalLightComponent* Sun = SunComponent.Get())
			{
				Sun->SetLightFunctionMaterial(nullptr);
			}
			bInjectionWired = false;
			{
				FScopeLock Guard(&State->Lock);
				State->MaskTargetResource = nullptr;
				State->MaskTargetExtent = FIntPoint::ZeroValue;
			}
			UE_LOG(LogVoxelShadowMarch, Display,
			       TEXT("shadow march injection UNWIRED (mode %d): sun light function ")
			            TEXT("cleared, conventional shadows unchanged."),
			       Mode);
		}
		return;
	}

	// 1. The checked-in parent material. Missing is DECLINED AND SAID, never
	// silently run-as-mode-1: the march still runs and is censused, and every
	// frame counts declinedNoTarget, so a leg configured for S2 that measured
	// S1 says so in its own log.
	if (LightFunctionParent == nullptr)
	{
		LightFunctionParent = LoadObject<UMaterialInterface>(nullptr, kSunShadowLFMaterialPath);
		if (LightFunctionParent == nullptr)
		{
			if (!bLoggedNoMaterial)
			{
				bLoggedNoMaterial = true;
				UE_LOG(LogVoxelShadowMarch, Warning,
				       TEXT("voxel.Shadow.March 2: light-function material missing at %s. ")
				            TEXT("Run ue-project/Tools/create_sunshadow_lf_material.py (editor ")
				            TEXT("commandlet, one-time) and CHECK THE ASSET IN. The march runs ")
				            TEXT("and is censused; INJECTION IS DECLINED -- the visible sun is ")
				            TEXT("unchanged and shadowinject counts declinedNoTarget."),
				       kSunShadowLFMaterialPath);
			}
			return;
		}
	}

	// 2. The render target, at the extent the render thread asked for.
	FIntPoint Desired = FIntPoint::ZeroValue;
	{
		FScopeLock Guard(&State->Lock);
		Desired = State->DesiredMaskExtent;
	}
	if (Desired.X <= 0 || Desired.Y <= 0)
	{
		return; // no mode-2 frame has rendered yet; the extent arrives with it
	}
	if (MaskTarget != nullptr &&
	    (MaskTarget->SizeX != Desired.X || MaskTarget->SizeY != Desired.Y))
	{
		// Resize: UNPUBLISH FIRST, republish on a later tick -- see the state
		// field's comment. The render thread declines (counted) meanwhile.
		{
			FScopeLock Guard(&State->Lock);
			State->MaskTargetResource = nullptr;
			State->MaskTargetExtent = FIntPoint::ZeroValue;
		}
		MaskTarget->InitCustomFormat(Desired.X, Desired.Y, PF_G8, /*bForceLinearGamma*/ true);
		MaskTarget->UpdateResourceImmediate(false);
		return;
	}
	if (MaskTarget == nullptr)
	{
		MaskTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("VoxelSunShadowMaskRT"));
		MaskTarget->ClearColor = FLinearColor::White; // fail-lit before the first copy
		MaskTarget->bAutoGenerateMips = false;
		// FORMAT NOTE, PAID FOR BY A DEAD LEG: the first attempt used PF_R8 on
		// both the RDG mask and this target -- reasonable, since the copy needs
		// identical formats. But PF_R8 IS NOT A LEGAL RENDER TARGET OVERRIDE
		// FORMAT: it is absent from FTextureRenderTargetResource::IsSupportedFormat
		// (TextureRenderTarget.cpp:518-540), and InitCustomFormat ASSERTS rather
		// than declining, so the editor died 21 s into the first injection leg.
		// PF_G8 is on that list, is 8-bit single-channel like PF_R8, and both
		// sides moved together so the copy still matches.
		// PF_R8 EXPLICITLY, not RTF_R8: the enum route maps RTF_R8 to PF_G8
		// (TextureRenderTarget2D.h:50) and the RDG copy from the PF_R8 mask
		// would then decline on format mismatch every frame.
		MaskTarget->InitCustomFormat(Desired.X, Desired.Y, PF_G8, /*bForceLinearGamma*/ true);
		MaskTarget->UpdateResourceImmediate(false);
		return; // publish next tick, once the resource exists
	}

	// 3. Wire the sun, once.
	if (!bInjectionWired)
	{
		UDirectionalLightComponent* Sun = SunComponent.Get();
		if (Sun == nullptr)
		{
			return; // the sun block above will re-find it; wiring retries next tick
		}
		LightFunctionMID = UMaterialInstanceDynamic::Create(LightFunctionParent, this);
		LightFunctionMID->SetTextureParameterValue(kSunShadowMaskParamName, MaskTarget);
		Sun->SetLightFunctionMaterial(LightFunctionMID);
		// FAIL-LIT: if the engine fades or disables the light function (quality
		// scalability, distance fade), the sun must return to UNSHADOWED-by-us,
		// never to black. DisabledBrightness defaults to 0.5, which would read
		// as a permanent half-shadow -- exactly the mutation arm's signature,
		// arriving as a bug.
		Sun->SetLightFunctionDisabledBrightness(1.0f);
		Sun->SetLightFunctionFadeDistance(1.0e8f);
		bInjectionWired = true;
		UE_LOG(LogVoxelShadowMarch, Display,
		       TEXT("shadow march INJECTION LIVE: sun light function = MID of %s, param ")
		            TEXT("'%s', target %dx%d PF_R8. Prove it with ")
		            TEXT("voxel.Shadow.MarchMutateInjection 1 before trusting any capture."),
		       kSunShadowLFMaterialPath, *kSunShadowMaskParamName.ToString(),
		       MaskTarget->SizeX, MaskTarget->SizeY);
	}

	// 4. Publish the resource (idempotent; also the republish after a resize).
	FTextureRenderTargetResource* Res = MaskTarget->GameThread_GetRenderTargetResource();
	if (Res != nullptr)
	{
		FScopeLock Guard(&State->Lock);
		State->MaskTargetResource = Res;
		State->MaskTargetExtent = FIntPoint(MaskTarget->SizeX, MaskTarget->SizeY);
	}
}

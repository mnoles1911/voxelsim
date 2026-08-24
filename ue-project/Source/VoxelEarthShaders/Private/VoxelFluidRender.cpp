// VoxelFluidRender.cpp -- RDG pass builder for the screen-space fluid
// renderer. See VoxelFluidRender.h for the compositing-hook decision and
// VoxelFluidRender.usf for the pass chain and the mirrored material law; this
// file is the plumbing between them, following VoxelFluidSim.cpp's shape
// (FGlobalShader classes, parameter structs, IMPLEMENT_GLOBAL_SHADER off the
// virtual path) plus the project's FIRST raster passes outside a mesh pass.

#include "VoxelFluidRender.h"

#include "VoxelFluidSim.h" // FVoxelFluidSimState (particle buffer + slot bound), contract mirrors
#include "VoxelFluidOccupancy.h" // AddRebaseParticlesPass + the volume's live origin

#include "CommonRenderResources.h" // GEmptyVertexDeclaration
#include "DataDrivenShaderPlatformInfo.h"
#include "FXRenderingUtils.h" // UE::FXRenderingUtils::GetRawViewRectUnsafe
#include "GlobalShader.h"
#include "PipelineStateCache.h"
// TStatic{Blend,Rasterizer,DepthStencil,Sampler}State. Reached transitively
// through the unity blob until this file was first compiled on its own (the
// adaptive non-unity path every modified file takes), which is a compile error
// that has nothing to do with the change that triggers it -- so it is included
// explicitly here rather than left to whoever edits this file next.
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h" // the ray census ring
#include "SceneRenderTargetParameters.h" // ESceneTextureSetupMode
#include "SceneView.h"
#include "SceneTexturesConfig.h" // FSceneTextureUniformParameters (ENGINE_API, public)
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "VoxelRenderFrame.h" // the render-frame split: water is a named bucket, not residual

// `stat GPU` line for the whole screen-space fluid pass chain (splat ->
// smoothX -> smoothY -> shadeComposite). Same note as VoxelFluidSim.cpp's:
// RDG_GPU_STAT_SCOPE is a deprecated no-op in UE 5.8, so the scope is the
// _STAT variant of the RDG event scope, and its name matches the event name.
// This is complementary to the file's own RQT_AbsoluteTime bracket, not a
// duplicate: that one feeds the 1 Hz perf line's renderMs, this one feeds the
// profiler.
DECLARE_GPU_STAT_NAMED(VoxelFluidRender, TEXT("VoxelFluidRender"));

// The march spike's own `stat GPU` line, separate from the fluid renderer's:
// the two passes share this hook and this file, and a shared scope would make
// the one number the G1 gate exists for uncombinable from the water's.
DECLARE_GPU_STAT_NAMED(VoxelMarchSpike, TEXT("VoxelMarchSpike"));

// DELIBERATELY NOT INCLUDED: Runtime/Renderer/Internal/PostProcess/
// PostProcessInputs.h. FPostProcessingInputs is Renderer-Internal (UBT exposes
// Internal/ only to engine-scope modules, and its sibling includes do not
// resolve from a game module's include set). The hook's own signature only
// needs the forward declaration SceneViewExtension.h provides, and the ONE
// thing this pass wants from Inputs -- the scene-texture uniform buffer -- has
// a sanctioned public accessor instead:
// UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer (FXRenderingUtils.h,
// RENDERER_API, "utilities for external module authors" by design). So Inputs
// goes unused and the internal header stays internal.

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (same note as VoxelFluidSim.cpp:21).
#define VOXEL_FLUID_RENDER_USF "/VoxelEarth/VoxelFluidRender.usf"
#define VOXEL_MARCH_SPIKE_USF  "/VoxelEarth/VoxelMarchSpike.usf"

namespace
{
	// ---- the Phase 0 G1 march spike's two cvars ---------------------------
	//
	// Two, and only two, because every arm of the measurement has to be
	// reachable without relaunching the editor: the plan's arm table is one
	// budget value per row plus one no-fetch flag, and a shipped default of 0
	// means the first build carrying this code cannot regress anything.

	TAutoConsoleVariable<int32> CVarVoxelMarcherSpike(
		TEXT("voxel.Marcher.Spike"), 0,
		TEXT("Phase 0 gate G1 (docs/ray-marching-plan-2026-08-19.md #3): ray-march the 512^3 ")
		TEXT("occupancy volume once per screen pixel and composite the result OVER the terrain, ")
		TEXT("to measure what a per-pixel march actually costs. THE VALUE IS THE DDA STEP ")
		TEXT("BUDGET, clamped to [1, 4096] -- so one cvar value is one arm: 1 = pass overhead ")
		TEXT("alone, 64, 256, 886 = 512*sqrt(3), the full box diagonal. 0 = off, and off costs ")
		TEXT("nothing: no pass is added and no query is issued. NEEDS voxel.Fluid.Enable 1, ")
		TEXT("which is what builds and fills the volume being marched -- with the fluid off ")
		TEXT("there is no volume and the spike reports marchGpuMs=pending forever. Read the ")
		TEXT("result off the 1 Hz 'Fluid perf' line's marchGpuMs= field. This ADDS a pass; it ")
		TEXT("never replaces the terrain draw, so the delta against 0 is the march and nothing ")
		TEXT("else. BEFORE QUOTING A NUMBER, wait for that same line's occupancy= to stop ")
		TEXT("growing: unbuilt volume reads as SOLID by contract, so a half-filled volume stops ")
		TEXT("every ray at the fill frontier and reports a march that is cheap for entirely the ")
		TEXT("wrong reason. The volume only spans 51.2 m and the camera is at its centre, so the ")
		TEXT("march reaches ~25.6 m -- that is the traversal the 886-step arm names, not the ")
		TEXT("horizon."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarcherSpikeNoFetch(
		TEXT("voxel.Marcher.SpikeNoFetch"), 0,
		TEXT("Phase 0 gate G1, the arm that separates MEMORY cost from ALU cost -- and the most ")
		TEXT("important one. 1 = the solidity test is stubbed to always return false at COMPILE ")
		TEXT("time (a shader permutation, not a branch), so the walk runs the same loop over the ")
		TEXT("same box-clipped segment with the same trip count and ZERO memory traffic. ")
		TEXT("march(budget) - march(budget, NoFetch 1) is the cost of asking the volume a ")
		TEXT("question, addressing arithmetic included. Nothing hits in this arm, so the screen ")
		TEXT("shows no marched surface: that is correct, not a failure. Ignored when ")
		TEXT("voxel.Marcher.Spike is 0."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarcherSpikeCount(
		TEXT("voxel.Marcher.SpikeCount"), 0,
		TEXT("Phase 0 gate G1 ray census: per frame, count every ray by OUTCOME and by how many ")
		TEXT("voxels it actually advanced, and report the distribution on the log line ")
		TEXT("\"Marcher census:\". Four total-time arms give the SHAPE of the cost but not the ")
		TEXT("POPULATION, and \"3.18 ms to march 25.6 m\" cannot be extrapolated to 4 km without ")
		TEXT("knowing how many rays were still alive at each depth -- inferring that from totals ")
		TEXT("is the derived-not-measured reasoning this project keeps being burned by. Outcomes ")
		TEXT("are hit / miss / budget-exhausted / started-inside / never-entered-the-box, and ")
		TEXT("exhausted is deliberately NOT folded into miss: it is the population a mip pyramid ")
		TEXT("would rescue. THE CENSUS MARCHES EVERY RAY A SECOND TIME, in its own compute pass ")
		TEXT("added after the timing bracket closes, so it cannot move marchGpuMs -- but the GPU ")
		TEXT("is doing twice the work, so NEVER QUOTE A TIMING FROM A COUNTING RUN. Counting arms ")
		TEXT("and timing arms are different runs, by construction. Ignored when voxel.Marcher.Spike ")
		TEXT("is 0."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarcherSpikeSkip(
		TEXT("voxel.Marcher.SpikeSkip"), 0,
		TEXT("Phase 0 gate G1, the empty-space skip experiment. 0 = the flat DDA control (the arm ")
		TEXT("the four timing points were taken on), 1 = one mip level (8^3 blocks, 64^3 bits, ")
		TEXT("32 KB), 2 = two levels (adds 64^3 blocks, 256 bytes). THE DELIVERABLE IS THE RATIO ")
		TEXT("steps(skip=0)/steps(skip=2) on identical poses, read off the \"Marcher census:\" ")
		TEXT("line -- NOT the time. The 51.2 m box is far too small for its timing to extrapolate ")
		TEXT("to 4 km; the step ratio is the multiplier that extrapolation needs. The ratio is a ")
		TEXT("LOWER bound: this volume is uniformly 10 cm, while a real cascade coarsens with ")
		TEXT("distance and would skip in much bigger strides. The mip is rebuilt every frame from ")
		TEXT("the occupancy volume, outside the timing bracket, so it cannot enter marchGpuMs. ")
		TEXT("DO NOT COMBINE WITH voxel.Marcher.SpikeNoFetch: the mip reads are not stubbed, so ")
		TEXT("that combination is neither a clean no-fetch arm nor a clean skip arm."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelMarcherSpikeSkipVerify(
		TEXT("voxel.Marcher.SpikeSkipVerify"), 0,
		TEXT("Phase 0 gate G1: prove the skip walk against the flat one. 1 = the census pass runs ")
		TEXT("BOTH walks on every ray and counts disagreements, reported as skipCompared/ ")
		TEXT("skipMismatch on the \"Marcher census:\" line. THE SKIP WALK IS THE ONLY PART OF THIS ")
		TEXT("SPIKE NOT BACKED BY A UNIT TEST, and its characteristic failure -- skipping one cell ")
		TEXT("too many -- produces a LOWER step count and a picture that still looks like terrain, ")
		TEXT("i.e. it reads as a better result rather than as a bug. Any non-zero mismatch ")
		TEXT("invalidates the step ratio. Needs voxel.Marcher.SpikeCount 1 and SpikeSkip > 0; it ")
		TEXT("costs a second march per ray in the census pass only, never in the timed one."),
		ECVF_RenderThreadSafe);

	// VoxelFluidRender.usf VOXEL_FLUID_DEPTH_SENTINEL -- the splat depth
	// target's clear value ("no fluid"). Mirrored like the sim's kernel
	// coefficients: change both or the empty test breaks.
	constexpr float kDepthSentinel = 1.0e30f;

	// ---- shader classes ----------------------------------------------------
	// Same SM5 gate and reasoning as FVoxelFluidShader (VoxelFluidSim.cpp:52):
	// these kernels are float/uint math and StructuredBuffer SRVs, all
	// SM5-legal, and the solver whose buffer they read gates on SM5.
	class FVoxelFluidRenderShader : public FGlobalShader
	{
	public:
		FVoxelFluidRenderShader() = default;
		FVoxelFluidRenderShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	// -- pass 1: splat (VS + PS share one parameter struct; each stage binds
	// the subset its reflection kept, which is how loose uniforms shared
	// across stages work everywhere else in the engine) --
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelFluidSplatParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVoxelFluidParticle>, Particles)
		SHADER_PARAMETER(uint32, SplatSlotCount)
		SHADER_PARAMETER(float, ParticleRadiusUU)
		SHADER_PARAMETER(float, ThicknessScale)
		SHADER_PARAMETER(FVector3f, FluidOriginTranslatedWorld)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToView)
		SHADER_PARAMETER(FMatrix44f, ViewToClip)
		SHADER_PARAMETER(FVector2f, FullViewRectMin)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZ)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FVoxelFluidSplatVS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelFluidSplatParameters;
		DECLARE_GLOBAL_SHADER(FVoxelFluidSplatVS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidSplatVS, FVoxelFluidRenderShader);
	};

	class FVoxelFluidSplatPS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelFluidSplatParameters;
		DECLARE_GLOBAL_SHADER(FVoxelFluidSplatPS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidSplatPS, FVoxelFluidRenderShader);
	};

	// -- passes 2/3: separable bilateral smooth --
	class FVoxelFluidSmoothCS : public FVoxelFluidRenderShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidSmoothCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidSmoothCS, FVoxelFluidRenderShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SmoothInDepth)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SmoothOutDepth)
			SHADER_PARAMETER(FIntPoint, SmoothTextureSize)
			SHADER_PARAMETER(FIntPoint, SmoothStepDir)
			SHADER_PARAMETER(int32, SmoothRadiusPx)
			SHADER_PARAMETER(float, SmoothDepthSigmaUU)
		END_SHADER_PARAMETER_STRUCT()

		static constexpr int32 kGroupSize = 8; // [numthreads(8,8,1)] in the .usf
	};

	// -- pass 4: shade + composite --
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelFluidShadeParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FluidDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FluidThicknessTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearClampSampler)
		SHADER_PARAMETER(FVector2f, ShadeHalfTexSize)
		SHADER_PARAMETER(FVector2f, ShadeViewRectMin)
		SHADER_PARAMETER(FVector2f, ShadeViewRectSize)
		SHADER_PARAMETER(FVector2f, InvProjDiag)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZ)
		SHADER_PARAMETER(FVector3f, SunDirView)
		SHADER_PARAMETER(float, SunDayGate)
		SHADER_PARAMETER(float, SmoothDepthSigmaUU)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FVoxelFluidScreenVS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelFluidShadeParameters;
		DECLARE_GLOBAL_SHADER(FVoxelFluidScreenVS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidScreenVS, FVoxelFluidRenderShader);
	};

	class FVoxelFluidShadePS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelFluidShadeParameters;
		DECLARE_GLOBAL_SHADER(FVoxelFluidShadePS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidShadePS, FVoxelFluidRenderShader);
	};

	// ---- the Phase 0 G1 march spike ---------------------------------------
	//
	// THE RAY BLOCK, SHARED BY EVERY SPIKE PASS, as a macro for exactly the
	// reason VOXEL_FLUID_OCCUPANCY_PARAMETERS is one: the picture pass and the
	// census pass must trace the SAME ray, and the only way two parameter
	// structs can be guaranteed to describe the same ray is for there to be one
	// copy of the list. A field that drifts between them would not fail to
	// compile -- it would produce a census of a slightly different frame.
	//
	// Deliberately a STRUCT and not loose per-element FShaderParameters:
	// docs/gpu-g2-draw-path.md records that the loose form measurably does not
	// bind in this project and that ShaderBindings.Add on an unbound parameter
	// is a silent no-op -- which in a measurement pass would mean marching
	// against a zeroed origin and reporting the cost of a march that hit
	// nothing.
	// The occupancy block is bound by ITS macro so the four names are spelled
	// exactly as VoxelFluidCollision.ush declares them (VoxelFluidOccupancy.h:177
	// -- that macro exists because a misspelling is a silently unbound resource,
	// not a compile error). Comments stay outside the macro body: a block comment
	// spliced into a continued #define is legal but reads as a trap.
#define VOXEL_MARCH_SPIKE_RAY_PARAMETERS() \
		VOXEL_FLUID_OCCUPANCY_PARAMETERS() \
		SHADER_PARAMETER(FMatrix44f, MarchViewToTranslatedWorld) \
		SHADER_PARAMETER(FVector3f, MarchRayOriginLocalUU) \
		SHADER_PARAMETER(float, MarchVolumeExtentUU) \
		SHADER_PARAMETER(FVector2f, MarchViewRectMin) \
		SHADER_PARAMETER(FVector2f, MarchViewRectSize) \
		SHADER_PARAMETER(FVector2f, MarchInvProjDiag) \
		SHADER_PARAMETER(int32, MarchStepBudget) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchMipL1) \
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchMipL2)

	// -- the picture, and the pass the timing bracket encloses --
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchSpikeParameters, )
		VOXEL_MARCH_SPIKE_RAY_PARAMETERS()
		SHADER_PARAMETER(FVector4f, MarchInvDeviceZToWorldZ)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, MarchSceneDepthTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	// -- the census (voxel.Marcher.SpikeCount 1) --
	//
	// No scene depth and no render target: the census does not care what is in
	// front of the marched surface, only how far each ray got. Fewer bindings
	// is also fewer ways for this pass to disagree with the one it is meant to
	// describe.
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchSpikeCountParameters, )
		VOXEL_MARCH_SPIKE_RAY_PARAMETERS()
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, MarchHistogram)
		SHADER_PARAMETER(int32, MarchHistBins)
		SHADER_PARAMETER(int32, MarchSkipVerify)
		SHADER_PARAMETER(FUintVector2, MarchCountViewSize)
	END_SHADER_PARAMETER_STRUCT()

	// The full-screen VS is FluidRenderScreenVS, REUSED: same entry point, same
	// .usf, no second copy. This class exists only so the entry point can be
	// paired with the spike's parameter struct instead of the shade pass's --
	// the VS reads none of either, which is exactly why sharing the entry point
	// is safe (FVoxelFluidScreenVS already binds a struct it never reads).
	class FVoxelMarchSpikeVS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelMarchSpikeParameters;
		DECLARE_GLOBAL_SHADER(FVoxelMarchSpikeVS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchSpikeVS, FVoxelFluidRenderShader);
	};

	class FVoxelMarchSpikePS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelMarchSpikeParameters;
		DECLARE_GLOBAL_SHADER(FVoxelMarchSpikePS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchSpikePS, FVoxelFluidRenderShader);

		// THE NO-FETCH ARM, AND WHY IT IS A PERMUTATION. A uniform branch
		// (`if (bNoFetch) return false;`) leaves the buffer load in the binary
		// for the other side of the branch, and the memory system is the entire
		// thing this arm is trying to remove -- the measurement would read the
		// same either way and would look like "memory is free". Two binaries,
		// both compiled up front, one cvar flip between them. The define name
		// is the one VoxelFluidCollision.ush guards on, so no aliasing sits
		// between the cvar and the code it removes.
		class FNoFetchDim : SHADER_PERMUTATION_BOOL("VOXEL_FLUID_SOLID_NO_FETCH");
		// THE SKIP LEVEL IS A PERMUTATION FOR A MEASURED REASON, not a stylistic
		// one: as a runtime branch the mip loads stayed in the binary on the
		// untaken path, and the no-fetch arm -- whose whole claim is ZERO memory
		// traffic -- went from 0 to 3 dx.op.bufferLoad the moment the skip code
		// landed. Offline disassembly caught that; nothing at runtime would have.
		class FSkipLevelsDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_SKIP_LEVELS", 3);
		using FPermutationDomain = TShaderPermutationDomain<FNoFetchDim, FSkipLevelsDim>;
	};

	// The census marcher. NO no-fetch permutation, deliberately: with the
	// solidity stub nothing ever hits, so a no-fetch census would be a
	// distribution of a population that does not exist. The census always
	// describes the real, fetching march.
	class FVoxelMarchSpikeCountCS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelMarchSpikeCountParameters;
		DECLARE_GLOBAL_SHADER(FVoxelMarchSpikeCountCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchSpikeCountCS, FVoxelFluidRenderShader);

		// The census MUST trace with the same skip level as the picture, or it
		// would be a distribution of a different march.
		class FSkipLevelsDim : SHADER_PERMUTATION_INT("VOXEL_MARCH_SKIP_LEVELS", 3);
		using FPermutationDomain = TShaderPermutationDomain<FSkipLevelsDim>;

		static constexpr int32 kGroupSize = 8; // [numthreads(8,8,1)] in the .usf
	};

	// ---- the pyramid's two reduce passes ---------------------------------
	//
	// Rebuilt every frame the spike runs, from the live occupancy volume, and
	// added BEFORE the timing bracket opens. L1 touches the 16 MiB volume
	// exactly once (one thread per output word, 512 input words each); L2 reads
	// only L1's 32 KB.
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchSpikeMipL1Parameters, )
		VOXEL_FLUID_OCCUPANCY_PARAMETERS()
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchMipOutL1)
		SHADER_PARAMETER(uint32, MarchMipL1WordCount)
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchSpikeMipL2Parameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchMipL1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchMipOutL2)
		SHADER_PARAMETER(uint32, MarchMipL2WordCount)
	END_SHADER_PARAMETER_STRUCT()

	class FVoxelMarchSpikeMipL1CS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelMarchSpikeMipL1Parameters;
		DECLARE_GLOBAL_SHADER(FVoxelMarchSpikeMipL1CS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchSpikeMipL1CS, FVoxelFluidRenderShader);
		static constexpr int32 kGroupSize = 64;
	};

	class FVoxelMarchSpikeMipL2CS : public FVoxelFluidRenderShader
	{
	public:
		using FParameters = FVoxelMarchSpikeMipL2Parameters;
		DECLARE_GLOBAL_SHADER(FVoxelMarchSpikeMipL2CS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchSpikeMipL2CS, FVoxelFluidRenderShader);
		static constexpr int32 kGroupSize = 64;
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSplatVS,  VOXEL_FLUID_RENDER_USF, "FluidRenderSplatVS",  SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSplatPS,  VOXEL_FLUID_RENDER_USF, "FluidRenderSplatPS",  SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSmoothCS, VOXEL_FLUID_RENDER_USF, "FluidRenderSmoothCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScreenVS, VOXEL_FLUID_RENDER_USF, "FluidRenderScreenVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidShadePS,  VOXEL_FLUID_RENDER_USF, "FluidRenderShadePS",  SF_Pixel);

// The spike's VS is the fluid composite's VS -- same file, same entry point.
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchSpikeVS,  VOXEL_FLUID_RENDER_USF, "FluidRenderScreenVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchSpikePS,  VOXEL_MARCH_SPIKE_USF,  "VoxelMarchSpikePS",   SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchSpikeCountCS, VOXEL_MARCH_SPIKE_USF, "VoxelMarchSpikeCountCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchSpikeMipL1CS, VOXEL_MARCH_SPIKE_USF, "VoxelMarchSpikeMipL1CS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchSpikeMipL2CS, VOXEL_MARCH_SPIKE_USF, "VoxelMarchSpikeMipL2CS", SF_Compute);

// ---------------------------------------------------------------------------
// The march spike's arm, asked of the cvars rather than of a pass
// ---------------------------------------------------------------------------
//
// ONE clamp, in ONE place, used by both the printer and the pass. The header
// carries the argument for why this is not a stat; what it buys mechanically is
// that `marchGpuMs=off` can only ever mean voxel.Marcher.Spike 0, because the
// answer no longer travels through anything that can decline to run.
//
// GetValueOnAnyThread: the perf line reads this on the GAME thread and the pass
// reads it on the RENDER thread, and both must agree about which arm they are
// describing. Both cvars are ECVF_RenderThreadSafe.
FVoxelMarchSpikeArm VoxelMarchSpikeGetArm()
{
	FVoxelMarchSpikeArm Arm;
	const int32 Raw = CVarVoxelMarcherSpike.GetValueOnAnyThread();
	// Clamped, not rejected -- a mistyped launch argument must still measure
	// something explicable rather than turning the spike off. 4096 is ~4.6x the
	// 886-step box diagonal the plan's widest arm asks for.
	Arm.StepBudget = Raw > 0 ? FMath::Clamp(Raw, 1, 4096) : 0;
	Arm.bNoFetch = CVarVoxelMarcherSpikeNoFetch.GetValueOnAnyThread() != 0;
	Arm.bCount = CVarVoxelMarcherSpikeCount.GetValueOnAnyThread() != 0;
	Arm.SkipLevels = FMath::Clamp(CVarVoxelMarcherSpikeSkip.GetValueOnAnyThread(), 0, 2);
	Arm.bSkipVerify = CVarVoxelMarcherSpikeSkipVerify.GetValueOnAnyThread() != 0;
	return Arm;
}

// ---------------------------------------------------------------------------

FVoxelFluidRenderExtension::FVoxelFluidRenderExtension(
	const FAutoRegister& AutoRegister, UWorld* InWorld,
	TSharedPtr<FVoxelFluidRenderState, ESPMode::ThreadSafe> InState)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
	, State(MoveTemp(InState))
{
}

bool FVoxelFluidRenderExtension::IsActiveThisFrame_Internal(
	const FSceneViewExtensionContext& Context) const
{
	// The world gate first (base class): only this subsystem's world.
	if (!FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context))
	{
		return false;
	}
	if (!State.IsValid())
	{
		return false;
	}
	FScopeLock Guard(&State->Lock);

	// THE MARCH SPIKE KEEPS THE EXTENSION ACTIVE ON ITS OWN. Read on the game
	// thread (this hook runs during view-family setup), hence GetValueOnAnyThread
	// rather than the render-thread accessor the pass itself uses.
	//
	// It needs its own clause because the fluid's clause is not a superset: that
	// one wants a sim tick pending, and PendingSimArgs is consumed once per
	// frame by PreRenderViewFamily. A render frame that outran the game tick
	// would decline the whole extension and the spike would silently skip
	// frames -- not corrupting any individual measurement, but quietly changing
	// how many frames the percentiles are built from, which is the kind of
	// thing that is invisible until two arms disagree for no reason.
	if (VoxelMarchSpikeGetArm().StepBudget > 0 && State->OccupancyKeepAlive.IsValid())
	{
		return true;
	}

	// Render OR sim: the extension now carries the solver's passes too
	// (PreRenderViewFamily), so a pending sim tick keeps the extension active
	// even while voxel.Fluid.Render is 0. Each pass site early-outs on its
	// own gate.
	return State->SimState.IsValid()
	       && (State->Settings.bEnabled || State->PendingSimArgs.IsSet());
}

void FVoxelFluidRenderExtension::PreRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	// THE FRAME ANCHOR IS TAKEN HERE TOO, and this is the reason it is a shared
	// idempotent call rather than something the marcher owns: on a leg with
	// voxel.March 0 the marcher's extension declines IsActiveThisFrame and not
	// one of its hooks is called, so an anchor that lived only there would emit
	// nothing at all for the quad control arm -- the exact configuration a
	// render-frame comparison would want to measure.
	VoxelRenderFrame::Touch(GraphBuilder);
	VOXEL_RENDER_FRAME_SCOPE(Fluid);

	// Consume the mailbox exactly once; a second view family this frame sims
	// nothing (and pays nothing).
	TOptional<FVoxelFluidSimTickArgs> Args;
	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> Sim;
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> OccAnchor;
	FIntVector RebaseDeltaVoxels = FIntVector::ZeroValue;
	FIntVector ArgsOriginVoxel = FIntVector::ZeroValue;
	bool bStaleOrigin = false;
	{
		FScopeLock Guard(&State->Lock);
		Sim = State->SimState;
		if (State->PendingSimArgs.IsSet())
		{
			// THE STALE-ORIGIN GATE (contract items 1/4). These args carry
			// origin-relative boxes and name particles that are origin-relative
			// too; both are only meaningful against the origin they were built
			// for. The game thread is a frame ahead and may already have
			// recentred, so ask the volume where it is NOW -- it takes its own
			// lock, and the game thread never nests these two in the other
			// order (it takes the rebase delta BEFORE it takes this lock), so
			// the nesting here cannot close a cycle.
			ArgsOriginVoxel = State->PendingSimArgsOriginVoxel;
			const FVoxelFluidOccupancyVolume* Volume = State->PendingSimArgs->Occupancy;
			bStaleOrigin = Volume != nullptr && Volume->GetOriginVoxel() != ArgsOriginVoxel;

			if (bStaleOrigin)
			{
				// Drop the tick, KEEP the delta owed: it belongs to the move
				// that invalidated these args, and the next tick's args will be
				// built against the origin it lands on.
				//
				// WHY DROPPING IS THE ONLY HONEST ANSWER HERE, since it costs a
				// frame of occupancy fills too: the delta for the move that made
				// these args stale is still on the VOLUME (the game thread takes
				// it when it posts), so running anyway would solve un-rebased
				// particles against a volume bound one step away -- up to 12.8 m
				// of displacement, which the density constraint resolves by
				// ejecting the water out of the rock it now sits in. That is the
				// blast artifact, once per recentre. A frozen frame is the same
				// "blocked, never guessed" trade the whole occupancy design makes.
				//
				// STATED LIMIT: a camera fast enough to recentre EVERY tick
				// (>6.4 m per tick, ~384 m/s at 60 Hz) can lose a run of ticks
				// this way. It is already far past the ~48 m/s at which the
				// refill cannot keep up, so the water there is frozen for the
				// documented reason regardless, and the counter says so.
				State->Stats.SimFramesStaleOrigin++;
			}
			else
			{
				Args = State->PendingSimArgs;
				OccAnchor = State->OccupancyKeepAlive;
				RebaseDeltaVoxels = State->PendingRebaseDeltaVoxels;
				State->PendingRebaseDeltaVoxels = FIntVector::ZeroValue;
			}
			State->PendingSimArgs.Reset();
		}
	}
	if (bStaleOrigin || !Args.IsSet() || !Sim.IsValid())
	{
		return;
	}

	// ---- the particle rebase, BEFORE the solver's passes --------------------
	//
	// WHERE IT GOES IN THE FRAME (VoxelFluidOccupancy.h, contract item 8):
	// BETWEEN solver ticks -- after the previous tick's finalize (last frame's
	// graph) and before this tick's integrate (added just below, in this one).
	// The solver's other position buffers are per-tick transients rebuilt from
	// ParticlesRW, so they need no rebase and must not get one; a pass landing
	// mid-tick would shift the stored positions out from under a sorted domain
	// built from the old ones.
	//
	// Re-registering the same pooled buffer AddSimPasses registers is not a
	// second registration: FRDGBuilder::RegisterExternalBuffer returns the
	// handle it already made for that pooled buffer (RenderGraphBuilder.cpp
	// FindExternalBuffer), so both sites see one resource and RDG orders this
	// pass ahead of the solver's on its own dependency tracking.
	if (RebaseDeltaVoxels != FIntVector::ZeroValue)
	{
		const uint32 Slots = FMath::Max(Args->SimSlotBound, Sim->RenderSlotBound);
		bool bRebased = false;
		if (Slots > 0 && Sim->bBuffersInitialized && Sim->Particles.IsValid())
		{
			FRDGBufferRef ParticlesRDG =
				GraphBuilder.RegisterExternalBuffer(Sim->Particles, TEXT("VoxelFluid.Particles"));
			bRebased = FVoxelFluidOccupancyVolume::AddRebaseParticlesPass(
				GraphBuilder, GraphBuilder.CreateUAV(ParticlesRDG), Slots, RebaseDeltaVoxels);
		}
		FScopeLock Guard(&State->Lock);
		if (bRebased)
		{
			State->Stats.RebasePasses++;
			State->Stats.RebaseSlots += uint64(Slots);
		}
		else if (Slots > 0)
		{
			// Particles exist and did not move with the window. Counted loudly
			// rather than absorbed -- this is the failure the contract calls
			// "water teleported sideways relative to the terrain".
			State->Stats.RebaseMissed++;
		}
		// Slots == 0: nothing has ever spawned, so there is nothing to move and
		// nothing to report. Deliberately not counted as a miss.
	}

	// The particle buffer now speaks the origin these args were built against,
	// whether it already did or the pass above moved it. The splat reads this,
	// not Settings.FluidOriginWorld, so a frame that skipped the tick keeps
	// drawing the water where the water actually is.
	State->ParticleOriginWorld = FVector(double(ArgsOriginVoxel.X), double(ArgsOriginVoxel.Y),
	                                     double(ArgsOriginVoxel.Z)) *
	                             double(vxc::kFluidVoxelUU);
	State->bParticleOriginValid = true;

	// OccAnchor keeps Args->Occupancy alive across this scope; the raw
	// pointer inside Args is the one the passes bind (contract: one origin,
	// one volume).
	VoxelFluidSim::AddSimPasses(GraphBuilder, *Sim, Args.GetValue());
}

void FVoxelFluidRenderExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	VoxelRenderFrame::NoteSetupEnd();
}

void FVoxelFluidRenderExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
	VOXEL_RENDER_FRAME_SCOPE(Fluid);
	check(IsInRenderingThread());
	if (!State.IsValid())
	{
		return;
	}

	// ---- poll last frames' GPU timings first, so this frame's stats carry
	// the newest completed number either way --------------------------------
	{
		float NewMs = -1.0f;
		for (FVoxelFluidRenderState::FTimingPair& Pair : State->TimingRing)
		{
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
			FScopeLock Guard(&State->Lock);
			State->Stats.RenderGpuMs = NewMs;
		}
	}

	// Scene captures / reflection captures never draw the fluid: they have
	// their own depth but the fluid is a first-person presentation layer, and
	// a capture re-running these passes would double the cost invisibly.
	if (InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection)
	{
		return;
	}

	// ---- the Phase 0 G1 march spike ---------------------------------------
	// FIRST, and ahead of every fluid gate below: it needs the occupancy volume
	// and nothing else -- not the particles, not the settings, not
	// voxel.Fluid.Render. Self-gating and self-timing; returns immediately when
	// voxel.Marcher.Spike is 0.
	AddMarchSpikePass(GraphBuilder, InView);

	// ---- copy settings + sim state out under the lock ----------------------
	FVoxelFluidRenderSettings Settings;
	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> Sim;
	{
		FScopeLock Guard(&State->Lock);
		Settings = State->Settings;
		Sim = State->SimState;
	}
	if (!Settings.bEnabled || !Sim.IsValid())
	{
		return;
	}
	// Buffers are render-thread members of the sim state; this IS the render
	// thread. A sim that has not allocated yet (or was just released by an
	// Enable-cycle -- ReleaseRenderThread nulls the pooled refs) renders
	// nothing, silently and safely: the subsystem's perf line still says
	// renderMs=pending, which is the visible symptom.
	if (!Sim->bBuffersInitialized || !Sim->Particles.IsValid() || Sim->RenderSlotBound == 0)
	{
		return;
	}

	// The scene textures, via the public accessor (see the include note at the
	// top of this file for why not Inputs.SceneTextures).
	const TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
		UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer(
			GraphBuilder, MakeStridedView(int32(sizeof(FSceneView)), &InView, 1),
			InView.GetFeatureLevel(), ESceneTextureSetupMode::SceneColor | ESceneTextureSetupMode::SceneDepth);
	if (SceneTexturesUB == nullptr)
	{
		return;
	}
	const FSceneTextureUniformParameters* SceneTextures = SceneTexturesUB->GetContents();
	FRDGTextureRef SceneColorTexture = SceneTextures->SceneColorTexture;
	FRDGTextureRef SceneDepthTexture = SceneTextures->SceneDepthTexture;
	if (SceneColorTexture == nullptr || SceneDepthTexture == nullptr)
	{
		return;
	}

	const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
	if (ViewRect.Area() <= 0)
	{
		return;
	}
	// Half resolution from the start -- the plan's stated cost control. At
	// 2560x1440 the working set is: depth raw + 2 smooth ping-pongs, R32F
	// 1280x720 (3 x 3.69 MB) + thickness R16F 1280x720 (1.84 MB) ~= 12.9 MB
	// of pooled transients.
	const FIntPoint HalfSize(FMath::DivideAndRoundUp(ViewRect.Width(), 2),
	                         FMath::DivideAndRoundUp(ViewRect.Height(), 2));

	const FViewMatrices& VM = InView.ViewMatrices;
	const FMatrix44f TranslatedWorldToView = FMatrix44f(VM.GetTranslatedViewMatrix());
	const FMatrix44f ViewToClip = FMatrix44f(VM.GetViewToClip());
	// The precision seam (contract "units" note): fluid origin is a world-UU
	// double; folding it with PreViewTranslation FIRST keeps the float3 the
	// shader adds camera-relative, exactly like the solver keeps positions
	// origin-relative.
	//
	// AND IT IS THE PARTICLE BUFFER'S OWN ORIGIN, not the settings' -- the two
	// differ for exactly as long as a recentre is owed a rebase (see
	// FVoxelFluidRenderState::ParticleOriginWorld). Using the settings there
	// would draw the water one step away from where it is being simulated, once
	// per recentre. Settings.FluidOriginWorld is the fallback for the frames
	// before any sim tick has been consumed, where the two are equal anyway.
	const FVector ParticleOriginWorld =
		State->bParticleOriginValid ? State->ParticleOriginWorld : Settings.FluidOriginWorld;
	const FVector3f FluidOriginTranslatedWorld =
		FVector3f(ParticleOriginWorld + VM.GetPreViewTranslation());
	const float ProjXX = ViewToClip.M[0][0];
	const float ProjYY = ViewToClip.M[1][1];
	if (ProjXX == 0.0f || ProjYY == 0.0f)
	{
		return; // degenerate projection (ortho shadow-ish view); nothing sane to draw
	}
	const FVector2f InvProjDiag(1.0f / ProjXX, 1.0f / ProjYY);
	// Rotation-only transform of the toward-the-sun direction into view space
	// (double matrix, then narrowed -- directions have no precision seam).
	const FVector SunDirViewD =
		FVector(VM.GetTranslatedViewMatrix().TransformVector(FVector(Settings.SunDirWorld)));
	const FVector3f SunDirView = FVector3f(SunDirViewD.GetSafeNormal());

	const float RadiusUU = FMath::Clamp(Settings.ParticleRadiusUU, 1.0f, 200.0f);
	// Thickness normalisation (VoxelFluidRender.usf "THICKNESS NORMALISATION"):
	// one particle == one 10 cm voxel of water (contract :27-29), so the
	// impostor sphere's chord integral is rescaled to the true particle volume.
	const float ThicknessScale =
		FMath::Pow(VoxelFluidSim::kRestSpacingUU, 3.0f) /
		((4.0f / 3.0f) * UE_PI * RadiusUU * RadiusUU * RadiusUU);
	// Bilateral range sigma tied to the sprite size: bumps up to ~1.5 radii
	// are "the same surface" and get rounded; bigger steps are silhouettes.
	const float DepthSigmaUU = 1.5f * RadiusUU;
	const int32 SmoothRadiusPx = FMath::Clamp(Settings.SmoothRadiusPx, 1, 32);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelFluidRender, "VoxelFluidRender");

	// ---- GPU timing bracket (begin) -- same RQT_AbsoluteTime shape as the
	// solver's (VoxelFluidSim.cpp), so renderMs and simGpuMs are comparable --
	FVoxelFluidRenderState::FTimingPair* Timing = nullptr;
	if (GSupportsTimestampRenderQueries)
	{
		for (FVoxelFluidRenderState::FTimingPair& Pair : State->TimingRing)
		{
			if (!Pair.bInFlight)
			{
				Timing = &Pair;
				break;
			}
		}
	}
	if (Timing != nullptr)
	{
		if (!Timing->Begin.IsValid())
		{
			Timing->Begin = RHICreateRenderQuery(RQT_AbsoluteTime);
			Timing->End = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		FRHIRenderQuery* Query = Timing->Begin.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelFluidRender.TimeBegin"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Query);
			});
	}

	// ---- transient targets -------------------------------------------------
	FRDGTextureRef DepthRaw = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(HalfSize, PF_R32_FLOAT,
		                          FClearValueBinding(FLinearColor(kDepthSentinel, 0, 0, 0)),
		                          TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VoxelFluid.RenderDepthRaw"));
	FRDGTextureRef Thickness = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(HalfSize, PF_R16F, FClearValueBinding::Black,
		                          TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("VoxelFluid.RenderThickness"));
	FRDGTextureRef DepthSmoothA = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(HalfSize, PF_R32_FLOAT, FClearValueBinding::None,
		                          TexCreate_UAV | TexCreate_ShaderResource),
		TEXT("VoxelFluid.RenderDepthSmoothA"));
	FRDGTextureRef DepthSmoothB = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(HalfSize, PF_R32_FLOAT, FClearValueBinding::None,
		                          TexCreate_UAV | TexCreate_ShaderResource),
		TEXT("VoxelFluid.RenderDepthSmoothB"));

	FRDGBufferRef ParticlesRDG =
		GraphBuilder.RegisterExternalBuffer(Sim->Particles, TEXT("VoxelFluid.Particles"));

	const uint32 SlotBound = Sim->RenderSlotBound;
	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	// ---- pass 1: depth + thickness splat (one geometry pass, MRT with
	// per-target blends: RT0 MIN keeps the nearest impostor surface, RT1 ADD
	// accumulates thickness -- the atomic-min/atomic-add the rasteriser gives
	// for free, per the .usf's raster-vs-scatter argument) -------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidSplatParameters>();
		Params->Particles = GraphBuilder.CreateSRV(ParticlesRDG);
		Params->SplatSlotCount = SlotBound;
		Params->ParticleRadiusUU = RadiusUU;
		Params->ThicknessScale = ThicknessScale;
		Params->FluidOriginTranslatedWorld = FluidOriginTranslatedWorld;
		Params->TranslatedWorldToView = TranslatedWorldToView;
		Params->ViewToClip = ViewToClip;
		Params->FullViewRectMin = FVector2f(ViewRect.Min);
		Params->InvDeviceZToWorldZ = InView.InvDeviceZToWorldZTransform;
		Params->SceneDepthTexture = SceneDepthTexture;
		Params->RenderTargets[0] = FRenderTargetBinding(DepthRaw, ERenderTargetLoadAction::EClear);
		Params->RenderTargets[1] = FRenderTargetBinding(Thickness, ERenderTargetLoadAction::EClear);

		TShaderMapRef<FVoxelFluidSplatVS> VertexShader(ShaderMap);
		TShaderMapRef<FVoxelFluidSplatPS> PixelShader(ShaderMap);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VoxelFluidRender.Splat(%u slots)", SlotBound), Params,
			ERDGPassFlags::Raster,
			[Params, VertexShader, PixelShader, HalfSize, SlotBound](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				// RT0 R32F: MIN blend == nearest surface wins, order-free.
				// RT1 R16F: ADD == thickness accumulates, order-free. Both are
				// commutative, which is what lets an unsorted particle draw be
				// correct -- the same order-independence argument the water
				// material's docstring makes for reading depth, applied to
				// writing it.
				PSOInit.BlendState =
					TStaticBlendState<CW_RED, BO_Min, BF_One, BF_One, BO_Add, BF_One, BF_Zero,
					                  CW_RED, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_Zero>::GetRHI();
				PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI =
					GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *Params);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Params);
				RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(HalfSize.X), float(HalfSize.Y), 1.0f);
				// 2 triangles per instance, vertex-pulled: SV_VertexID 0..5,
				// SV_InstanceID = particle slot.
				RHICmdList.DrawPrimitive(0, 2, SlotBound);
			});
	}

	// ---- passes 2/3: separable bilateral smooth (X then Y) ----------------
	const auto AddSmoothPass = [&](FRDGTextureRef In, FRDGTextureRef Out, FIntPoint StepDir,
	                               const TCHAR* Name)
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidSmoothCS::FParameters>();
		Params->SmoothInDepth = In;
		Params->SmoothOutDepth = GraphBuilder.CreateUAV(Out);
		Params->SmoothTextureSize = HalfSize;
		Params->SmoothStepDir = StepDir;
		Params->SmoothRadiusPx = SmoothRadiusPx;
		Params->SmoothDepthSigmaUU = DepthSigmaUU;
		TShaderMapRef<FVoxelFluidSmoothCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("VoxelFluidRender.%s", Name), Shader, Params,
			FIntVector(FMath::DivideAndRoundUp(HalfSize.X, FVoxelFluidSmoothCS::kGroupSize),
			           FMath::DivideAndRoundUp(HalfSize.Y, FVoxelFluidSmoothCS::kGroupSize), 1));
	};
	AddSmoothPass(DepthRaw, DepthSmoothA, FIntPoint(1, 0), TEXT("SmoothX"));
	AddSmoothPass(DepthSmoothA, DepthSmoothB, FIntPoint(0, 1), TEXT("SmoothY"));

	// ---- pass 4: shade + composite into scene colour ----------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidShadeParameters>();
		Params->FluidDepthTexture = DepthSmoothB;
		Params->FluidThicknessTexture = Thickness;
		Params->SceneDepthTexture = SceneDepthTexture;
		Params->BilinearClampSampler =
			TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Params->ShadeHalfTexSize = FVector2f(HalfSize);
		Params->ShadeViewRectMin = FVector2f(ViewRect.Min);
		Params->ShadeViewRectSize = FVector2f(ViewRect.Size());
		Params->InvProjDiag = InvProjDiag;
		Params->InvDeviceZToWorldZ = InView.InvDeviceZToWorldZTransform;
		Params->SunDirView = SunDirView;
		Params->SunDayGate = Settings.SunDayGate;
		Params->SmoothDepthSigmaUU = DepthSigmaUU;
		Params->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);

		TShaderMapRef<FVoxelFluidScreenVS> VertexShader(ShaderMap);
		TShaderMapRef<FVoxelFluidShadePS> PixelShader(ShaderMap);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VoxelFluidRender.ShadeComposite"), Params, ERDGPassFlags::Raster,
			[Params, VertexShader, PixelShader, ViewRect](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				// SrcAlpha/InvSrcAlpha over: the same `over` the shipped
				// translucent water composites with. The shader never reads
				// scene colour (material ban 1); the blend unit does the
				// compositing.
				PSOInit.BlendState =
					TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
				PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI =
					GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *Params);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Params);
				RHICmdList.SetViewport(float(ViewRect.Min.X), float(ViewRect.Min.Y), 0.0f,
				                       float(ViewRect.Max.X), float(ViewRect.Max.Y), 1.0f);
				RHICmdList.DrawPrimitive(0, 1, 1); // the fullscreen triangle
			});
	}

	// ---- GPU timing bracket (end) + frame accounting -----------------------
	if (Timing != nullptr)
	{
		FRHIRenderQuery* Query = Timing->End.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelFluidRender.TimeEnd"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Query);
			});
		Timing->bInFlight = true;
	}
	{
		FScopeLock Guard(&State->Lock);
		State->Stats.FramesRendered++;
	}
}

// ---------------------------------------------------------------------------
// The Phase 0 gate G1 march spike
// ---------------------------------------------------------------------------
//
// docs/ray-marching-plan-2026-08-19.md section 3. VoxelMarchSpike.usf carries
// the argument for what is being measured, what is deliberately excluded, why
// the composite is an addition rather than a substitution, and why the shader
// must never contain a `discard`. This function is the plumbing and the gate;
// it restates none of that.
//
// WHAT IT COSTS WHEN OFF: one cvar read and one early return. No pass, no
// query, no allocation, no shader bound. The shipped default is 0 and the first
// build carrying this code renders exactly what the previous one did.
void FVoxelFluidRenderExtension::AddMarchSpikePass(FRDGBuilder& GraphBuilder,
                                                   const FSceneView& InView)
{
	// ---- retire finished queries FIRST -------------------------------------
	//
	// Before the gate, not after. If the cvar goes to 0 while pairs are still
	// in flight, an un-polled ring stays permanently full and the NEXT arm --
	// the one somebody is about to measure -- finds no free pair and reports
	// marchGpuMs=pending forever. That failure looks exactly like "the spike
	// never ran", which is the one reading this file must never produce by
	// accident.
	{
		float NewMs = -1.0f;
		for (FVoxelFluidRenderState::FTimingPair& Pair : State->MarchTimingRing)
		{
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
			FScopeLock Guard(&State->Lock);
			State->Stats.MarchGpuMs = NewMs;
		}
	}

	// ---- decode any landed ray census --------------------------------------
	//
	// Polled here, beside the timing poll and before the gate, for the same
	// reason: a ring left unpolled after the cvar goes to 0 stays full, and the
	// next counting run then reports nothing while looking exactly like a
	// census that ran and found nothing.
	//
	// The whole histogram comes back and the CPU does the arithmetic. That is
	// the point -- mean and p95 are computed from the distribution itself, so
	// there is no bucketing error and no accumulator that can overflow. A GPU
	// reduction would have been one more shader to be wrong in a way that still
	// prints entirely plausible numbers.
	for (FVoxelFluidRenderState::FCensusReadback& Slot : State->CensusRing)
	{
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		const int32 Bins = Slot.Bins;
		const uint32 Bytes = (uint32(Bins) * uint32(VoxelMarchSpike::kHistRows) +
		                      uint32(VoxelMarchSpike::kDiagSampleWords)) * sizeof(uint32);
		const uint32* Src = static_cast<const uint32*>(Slot.Readback->Lock(Bytes));
		if (Src != nullptr)
		{
			FVoxelMarchSpikeCensus Census;
			Census.bValid = true;
			Census.Generation = Slot.Generation;
			Census.StepBudget = Slot.StepBudget;
			Census.RaysDispatched = Slot.RaysDispatched;

			// Per-outcome totals and step sums, straight off the bins. uint64
			// throughout: at 1552x873 a full-screen arm at budget 4096 tops out
			// near 5.5e9 step-rays, comfortably past uint32 -- exactly the kind
			// of quiet wrap that would have produced a believable, wrong mean.
			uint64 OutcomeRays[VoxelMarchSpike::kOutcomeCount] = {};
			uint64 OutcomeSteps[VoxelMarchSpike::kOutcomeCount] = {};
			for (int32 Outcome = 0; Outcome < VoxelMarchSpike::kOutcomeCount; ++Outcome)
			{
				const uint32* Row = Src + int64(Outcome) * Bins;
				for (int32 Bin = 0; Bin < Bins; ++Bin)
				{
					const uint64 N = Row[Bin];
					OutcomeRays[Outcome] += N;
					OutcomeSteps[Outcome] += N * uint64(Bin);
				}
			}

			Census.Hit = OutcomeRays[VoxelMarchSpike::kOutcomeHit];
			Census.Miss = OutcomeRays[VoxelMarchSpike::kOutcomeMiss];
			Census.Exhausted = OutcomeRays[VoxelMarchSpike::kOutcomeExhausted];
			Census.Inside = OutcomeRays[VoxelMarchSpike::kOutcomeInside];
			Census.NoBox = OutcomeRays[VoxelMarchSpike::kOutcomeNoBox];

			uint64 TotalRays = 0;
			uint64 TotalSteps = 0;
			for (int32 Outcome = 0; Outcome < VoxelMarchSpike::kOutcomeCount; ++Outcome)
			{
				TotalRays += OutcomeRays[Outcome];
				TotalSteps += OutcomeSteps[Outcome];
			}
			Census.RaysCounted = TotalRays;

			const auto MeanOf = [](uint64 Steps, uint64 Rays) -> float
			{
				return Rays > 0 ? float(double(Steps) / double(Rays)) : 0.0f;
			};
			Census.MeanSteps = MeanOf(TotalSteps, TotalRays);
			Census.MeanStepsHit = MeanOf(OutcomeSteps[VoxelMarchSpike::kOutcomeHit], Census.Hit);
			Census.MeanStepsMiss = MeanOf(OutcomeSteps[VoxelMarchSpike::kOutcomeMiss], Census.Miss);
			Census.MeanStepsExhausted =
				MeanOf(OutcomeSteps[VoxelMarchSpike::kOutcomeExhausted], Census.Exhausted);

			// p95 over ALL rays, by walking the summed distribution.
			// Nearest-rank: the smallest step count at or below which at least
			// 95% of rays fall. No interpolation -- the quantity is an integer
			// count of voxel advances, and inventing a fractional one would be
			// reporting a number the GPU never produced.
			if (TotalRays > 0)
			{
				const uint64 Target = (TotalRays * 95 + 99) / 100;
				uint64 Running = 0;
				for (int32 Bin = 0; Bin < Bins; ++Bin)
				{
					for (int32 Outcome = 0; Outcome < VoxelMarchSpike::kOutcomeCount; ++Outcome)
					{
						Running += Src[int64(Outcome) * Bins + Bin];
					}
					if (Running >= Target)
					{
						Census.P95Steps = float(Bin);
						break;
					}
				}
			}

			// The diagnostics row -- not an outcome, not in the conservation sum.
			Census.SkipLevels = Slot.SkipLevels;
			Census.SkipMismatch =
				Src[int64(VoxelMarchSpike::kDiagRow) * Bins + VoxelMarchSpike::kDiagMismatchBin];
			Census.SkipCompared =
				Src[int64(VoxelMarchSpike::kDiagRow) * Bins + VoxelMarchSpike::kDiagComparedBin];

			// The sample tail. SampleCount is the number of rays that CLAIMED a
			// slot, which is capped at kDiagSampleMax for the records themselves
			// -- the log prints both so a truncated sample set says so.
			{
				const int64 SampleBase = int64(Bins) * VoxelMarchSpike::kHistRows;
				Census.SkipTies = Src[SampleBase + VoxelMarchSpike::kDiagSampleTies];
				Census.SkipRefNoise = Src[SampleBase + VoxelMarchSpike::kDiagSampleRefNoise];
				Census.SkipRestartNoise = Src[SampleBase + VoxelMarchSpike::kDiagSampleRestart];
				const int32 Claimed = int32(FMath::Min<uint32>(
					Src[SampleBase + VoxelMarchSpike::kDiagSampleClaimed], INT32_MAX));
				Census.SampleCount = FMath::Min(Claimed, VoxelMarchSpike::kDiagSampleMax);
				for (int32 i = 0; i < Census.SampleCount; ++i)
				{
					const uint32* R = Src + SampleBase + VoxelMarchSpike::kDiagSampleHeader +
					                  int64(i) * VoxelMarchSpike::kDiagSampleStride;
					FVoxelMarchSpikeCensus::FSample& Sample = Census.Samples[i];
					Sample.PixelX = R[0];
					Sample.PixelY = R[1];
					Sample.bFlatFound = (R[2] & 1u) != 0u;
					Sample.bSkipFound = (R[2] & 2u) != 0u;
					Sample.FlatVoxel = FIntVector(int32(R[3]), int32(R[4]), int32(R[5]));
					Sample.SkipVoxel = FIntVector(int32(R[6]), int32(R[7]), int32(R[8]));
					Sample.SkipSteps = R[9];
				}
			}

			// EVERY read of Src is above this line. The unlock invalidates the
			// mapping, and a p95 loop that ran after it would be reading freed
			// memory while producing numbers that look ordinary.
			Slot.Readback->Unlock();

			FScopeLock Guard(&State->Lock);
			// Late arrivals must never regress a newer census -- the same rule
			// the solver's counts ring applies to its own snapshots.
			if (Census.Generation >= State->Stats.MarchCensus.Generation)
			{
				State->Stats.MarchCensus = Census;
			}
		}
		Slot.bInFlight = false;
	}

	// ---- the gate ----------------------------------------------------------
	//
	// THE BUDGET IS THE ARM. Clamped, not rejected: an out-of-range value from
	// a mistyped launch argument must still measure something explicable rather
	// than turning the spike off, and 4096 is already ~4.6x the 886-step box
	// diagonal the plan's widest arm asks for.
	// The SAME accessor the perf line prints from, so the number and the arm
	// beside it can never describe different budgets.
	const FVoxelMarchSpikeArm Arm = VoxelMarchSpikeGetArm();
	const int32 StepBudget = Arm.StepBudget;
	const bool bNoFetch = Arm.bNoFetch;
	const bool bCount = Arm.bCount;
	const int32 SkipLevels = Arm.SkipLevels;
	const bool bSkipVerify = Arm.bSkipVerify;
	if (StepBudget <= 0)
	{
		return;
	}
	{
		// The arm THIS FRAME'S PASS actually used, recorded next to the time it
		// will produce. NOT the source of the "off"/"armed" distinction -- that
		// question is answered from the cvars, upstream of every gate that can
		// stop this function being called at all (see FVoxelMarchSpikeArm).
		FScopeLock Guard(&State->Lock);
		State->Stats.MarchStepBudget = StepBudget;
		State->Stats.bMarchNoFetch = bNoFetch;
	}

	// The volume, via the anchor the sim-args post keeps alive. This is the
	// SAME 512^3 bit volume the fluid collides against -- built from
	// vxc::packBrickSolidBits over vxc::World INCLUDING the edit overlay,
	// GPU-resident, never read back -- which is the whole reason the spike can
	// exist without a producer (plan section 3: "cheaper than expected, because
	// it is already written").
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> Volume;
	{
		FScopeLock Guard(&State->Lock);
		Volume = State->OccupancyKeepAlive;
	}
	if (!Volume.IsValid())
	{
		// voxel.Fluid.Enable is 0, or no sim tick has been posted yet. Nothing
		// to march. Counted as "pending" by the perf line, never as 0.00 ms.
		return;
	}

	// REGISTRATION WITHOUT CLEAR OR FILL (VoxelFluidOccupancy.h:484): the
	// volume's own AddPasses already ran this frame, at PreRenderViewFamily,
	// inside the solver's graph build. This is a reader in a later pass of the
	// same graph, and it must not touch the bits. Null means AddPasses has
	// never run -- the buffer does not exist yet, and binding a null SRV is an
	// RDG assert rather than a black frame.
	if (Volume->Register(GraphBuilder) == nullptr)
	{
		return;
	}

	// Scene textures, via the public accessor -- see the include note at the
	// top of this file for why not Inputs.SceneTextures.
	const TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUB =
		UE::FXRenderingUtils::GetOrCreateSceneTextureUniformBuffer(
			GraphBuilder, MakeStridedView(int32(sizeof(FSceneView)), &InView, 1),
			InView.GetFeatureLevel(),
			ESceneTextureSetupMode::SceneColor | ESceneTextureSetupMode::SceneDepth);
	if (SceneTexturesUB == nullptr)
	{
		return;
	}
	const FSceneTextureUniformParameters* SceneTextures = SceneTexturesUB->GetContents();
	FRDGTextureRef SceneColorTexture = SceneTextures->SceneColorTexture;
	FRDGTextureRef SceneDepthTexture = SceneTextures->SceneDepthTexture;
	if (SceneColorTexture == nullptr || SceneDepthTexture == nullptr)
	{
		return;
	}

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

	// THE PRECISION SEAM, and the one place this pass can be quietly wrong.
	// Both operands are world UU at planet scale (tens of km, comfortably past
	// where a float32 UU loses sub-voxel precision); their DIFFERENCE is at
	// most the volume's 5,120 UU. So the subtraction happens in double and the
	// narrowing happens after it. GetOriginUU is the ONE place the volume's UU
	// origin is computed (VoxelFluidOccupancy.h:250) and the collision walk's
	// hard requirement -- FluidOriginUU == FluidVolumeOriginVoxel * 10 UU
	// (VoxelFluidCollision.ush:35-44) -- is exactly what makes this difference
	// the frame the walk expects. Deriving it any other way puts the marched
	// terrain a few metres from the drawn terrain, which reads as a worldgen
	// bug.
	const FVector3f RayOriginLocalUU = FVector3f(VM.GetViewOrigin() - Volume->GetOriginUU());
	const float VolumeExtentUU =
		float(FVoxelFluidOccupancyVolume::DimVoxels) * vxc::kFluidVoxelUU;

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelMarchSpike, "VoxelMarchSpike");

	// ---- the empty-space skip pyramid (voxel.Marcher.SpikeSkip) ------------
	//
	// BEFORE the timing bracket opens. The reduce reads the whole 16 MiB volume
	// and must not be able to enter marchGpuMs -- the number this gate exists
	// for. In a real marcher the pyramid would be maintained incrementally as
	// the occupancy fill advances; rebuilding it wholesale every frame is the
	// measurement-build shortcut, and it is affordable only because it sits
	// outside the thing being measured.
	//
	// ALWAYS ALLOCATED, EVEN IN THE CONTROL ARM, and cleared rather than
	// reduced there. The buffers are bound by every permutation (the control
	// permutation simply never references them, so DXC strips the loads), and
	// RDG requires a bound SRV to have been written by something. A 32 KB clear
	// outside the bracket is the price of the control arm binding the same
	// parameter struct as the skip arms -- which is what keeps the two arms
	// otherwise identical.
	const int32 kMipL1Words =
		(FVoxelFluidOccupancyVolume::DimVoxels / 8) *        // cells along x
		(FVoxelFluidOccupancyVolume::DimVoxels / 8) *        // rows (y)
		(FVoxelFluidOccupancyVolume::DimVoxels / 8) / 32;    // 32 cells per word
	const int32 kMipL2Words = 64; // 8 rows of 8, one word per (y2,z2) row

	FRDGBufferRef MipL1 = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(kMipL1Words)),
		TEXT("VoxelMarchSpike.MipL1"));
	FRDGBufferRef MipL2 = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(kMipL2Words)),
		TEXT("VoxelMarchSpike.MipL2"));
	FRDGBufferUAVRef MipL1UAV = GraphBuilder.CreateUAV(MipL1, PF_R32_UINT);
	FRDGBufferUAVRef MipL2UAV = GraphBuilder.CreateUAV(MipL2, PF_R32_UINT);

	if (SkipLevels <= 0)
	{
		// Control arm: written, never read. Zero rather than left undefined so
		// that a permutation mix-up shows up as "nothing is solid" -- a black
		// screen with every ray missing -- instead of as plausible terrain
		// assembled from uninitialised memory.
		AddClearUAVPass(GraphBuilder, MipL1UAV, 0u);
		AddClearUAVPass(GraphBuilder, MipL2UAV, 0u);
	}
	else
	{
		{
			auto* MipParams = GraphBuilder.AllocParameters<FVoxelMarchSpikeMipL1Parameters>();
			Volume->BindShaderParameters(GraphBuilder, *MipParams);
			MipParams->MarchMipOutL1 = MipL1UAV;
			MipParams->MarchMipL1WordCount = uint32(kMipL1Words);
			TShaderMapRef<FVoxelMarchSpikeMipL1CS> MipShader(ShaderMap);
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("VoxelMarchSpike.MipL1"), MipShader, MipParams,
				FIntVector(FMath::DivideAndRoundUp(kMipL1Words,
				                                   FVoxelMarchSpikeMipL1CS::kGroupSize), 1, 1));
		}
		if (SkipLevels >= 2)
		{
			auto* MipParams = GraphBuilder.AllocParameters<FVoxelMarchSpikeMipL2Parameters>();
			MipParams->MarchMipL1 = GraphBuilder.CreateSRV(MipL1, PF_R32_UINT);
			MipParams->MarchMipOutL2 = MipL2UAV;
			MipParams->MarchMipL2WordCount = uint32(kMipL2Words);
			TShaderMapRef<FVoxelMarchSpikeMipL2CS> MipShader(ShaderMap);
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("VoxelMarchSpike.MipL2"), MipShader, MipParams,
				FIntVector(FMath::DivideAndRoundUp(kMipL2Words,
				                                   FVoxelMarchSpikeMipL2CS::kGroupSize), 1, 1));
		}
		else
		{
			// SkipLevels == 1 never reads L2, but it is still bound. Same
			// argument as the control arm's clear.
			AddClearUAVPass(GraphBuilder, MipL2UAV, 0u);
		}
	}

	FRDGBufferSRVRef MipL1SRV = GraphBuilder.CreateSRV(MipL1, PF_R32_UINT);
	FRDGBufferSRVRef MipL2SRV = GraphBuilder.CreateSRV(MipL2, PF_R32_UINT);


	// ---- GPU timing bracket (begin) ----------------------------------------
	// Same RQT_AbsoluteTime shape as the solver's and the fluid renderer's, so
	// marchGpuMs, simGpuMs and renderMs are directly comparable -- and against
	// the same clock the Arm A capture's PrePass/BasePass numbers came from.
	FVoxelFluidRenderState::FTimingPair* Timing = nullptr;
	if (GSupportsTimestampRenderQueries)
	{
		for (FVoxelFluidRenderState::FTimingPair& Pair : State->MarchTimingRing)
		{
			if (!Pair.bInFlight)
			{
				Timing = &Pair;
				break;
			}
		}
	}
	if (Timing != nullptr)
	{
		if (!Timing->Begin.IsValid())
		{
			Timing->Begin = RHICreateRenderQuery(RQT_AbsoluteTime);
			Timing->End = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		FRHIRenderQuery* Query = Timing->Begin.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelMarchSpike.TimeBegin"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Query);
			});
	}

	// ---- the march ---------------------------------------------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelMarchSpikeParameters>();
		Volume->BindShaderParameters(GraphBuilder, *Params);
		// Rotation of a view-space direction into world space. The TRANSLATED
		// inverse view matrix, not the world one: its translation row is the
		// camera in translated world (~0) rather than a planet-scale offset, so
		// narrowing it to float is harmless. The shader multiplies with w = 0
		// and uses only the upper 3x3 regardless.
		Params->MarchViewToTranslatedWorld = FMatrix44f(VM.GetInvTranslatedViewMatrix());
		Params->MarchRayOriginLocalUU = RayOriginLocalUU;
		Params->MarchVolumeExtentUU = VolumeExtentUU;
		Params->MarchViewRectMin = FVector2f(ViewRect.Min);
		Params->MarchViewRectSize = FVector2f(ViewRect.Size());
		Params->MarchInvProjDiag = FVector2f(1.0f / ProjXX, 1.0f / ProjYY);
		Params->MarchInvDeviceZToWorldZ = InView.InvDeviceZToWorldZTransform;
		Params->MarchStepBudget = StepBudget;
		Params->MarchMipL1 = MipL1SRV;
		Params->MarchMipL2 = MipL2SRV;
		Params->MarchSceneDepthTexture = SceneDepthTexture;
		// ELoad, and RGB-only writes below: the terrain is already in this
		// texture and stays there. The spike is an addition (VoxelMarchSpike.usf,
		// "IT IS AN ADDITION, NEVER A SUBSTITUTION").
		Params->RenderTargets[0] =
			FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);

		FVoxelMarchSpikePS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVoxelMarchSpikePS::FNoFetchDim>(bNoFetch);
		PermutationVector.Set<FVoxelMarchSpikePS::FSkipLevelsDim>(SkipLevels);

		TShaderMapRef<FVoxelMarchSpikeVS> VertexShader(ShaderMap);
		TShaderMapRef<FVoxelMarchSpikePS> PixelShader(ShaderMap, PermutationVector);
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VoxelMarchSpike.March(budget %d, nofetch %d)", StepBudget,
			               bNoFetch ? 1 : 0),
			Params, ERDGPassFlags::Raster,
			[Params, VertexShader, PixelShader, ViewRect](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				// SrcAlpha/InvSrcAlpha over, RGB only -- the same `over` the
				// fluid composite and the shipped translucent water use. The
				// shader never reads scene colour; the blend unit composites.
				PSOInit.BlendState =
					TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
				PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				// NO depth test and NO depth write. The occlusion decision is
				// made in the shader against the opaque scene depth, AFTER the
				// march, on purpose: a hardware depth test would reject pixels
				// before the pixel shader ran, and the arm would then be timing
				// however much of the screen happened to be sky at that pose
				// rather than a full screen of marching.
				PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI =
					GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *Params);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Params);
				RHICmdList.SetViewport(float(ViewRect.Min.X), float(ViewRect.Min.Y), 0.0f,
				                       float(ViewRect.Max.X), float(ViewRect.Max.Y), 1.0f);
				RHICmdList.DrawPrimitive(0, 1, 1); // the fullscreen triangle
			});
	}

	// ---- GPU timing bracket (end) + frame accounting -----------------------
	if (Timing != nullptr)
	{
		FRHIRenderQuery* Query = Timing->End.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelMarchSpike.TimeEnd"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Query);
			});
		Timing->bInFlight = true;
	}

	// ---- the ray census (voxel.Marcher.SpikeCount 1) -----------------------
	//
	// AFTER the timing bracket closed, deliberately and non-negotiably: this
	// dispatch marches every ray a SECOND time, and if it landed inside the
	// bracket it would roughly double the number the whole gate exists to
	// produce. The bracket ends above; everything below is outside it.
	//
	// It still makes the GPU busier, which is why counting is its own cvar and
	// its own run -- see the cvar's text. Nothing here runs when it is off.
	if (bCount)
	{
		FVoxelFluidRenderState::FCensusReadback* Free = nullptr;
		for (FVoxelFluidRenderState::FCensusReadback& Slot : State->CensusRing)
		{
			if (!Slot.bInFlight)
			{
				Free = &Slot;
				break;
			}
		}
		if (Free == nullptr)
		{
			// Counted, not silent: if this climbs, the GPU is more than
			// kNumCensusReadbacks frames behind and the census is sampling
			// rather than reporting every frame.
			State->CensusSkips++;
		}
		else
		{
			// One bin per reachable step count, so the histogram is exact.
			// Steps are provably in [0, StepBudget], hence budget + 1 bins.
			const int32 Bins = StepBudget + 1;
			// kHistRows, not kOutcomeCount: one row above the five outcomes holds
			// the skip-vs-flat verification tallies and is excluded from the
			// conservation sum.
			// The histogram rows plus the sample tail (see the layout note in the
			// .usf). One buffer and one readback rather than two, so the samples
			// can never land a frame apart from the count they belong to.
			const uint32 Elements = uint32(Bins) * uint32(VoxelMarchSpike::kHistRows) +
			                        uint32(VoxelMarchSpike::kDiagSampleWords);

			FRDGBufferRef HistRDG = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Elements),
				TEXT("VoxelMarchSpike.Histogram"));
			FRDGBufferUAVRef HistUAV = GraphBuilder.CreateUAV(HistRDG);
			// Cleared every frame: the histogram describes ONE frame's rays.
			// Accumulating across frames would silently average over a moving
			// camera and read as a much smoother distribution than any frame
			// actually had.
			AddClearUAVPass(GraphBuilder, HistUAV, 0u);

			{
				auto* CountParams = GraphBuilder.AllocParameters<FVoxelMarchSpikeCountParameters>();
				Volume->BindShaderParameters(GraphBuilder, *CountParams);
				CountParams->MarchViewToTranslatedWorld =
					FMatrix44f(VM.GetInvTranslatedViewMatrix());
				CountParams->MarchRayOriginLocalUU = RayOriginLocalUU;
				CountParams->MarchVolumeExtentUU = VolumeExtentUU;
				CountParams->MarchViewRectMin = FVector2f(ViewRect.Min);
				CountParams->MarchViewRectSize = FVector2f(ViewRect.Size());
				CountParams->MarchInvProjDiag = FVector2f(1.0f / ProjXX, 1.0f / ProjYY);
				CountParams->MarchStepBudget = StepBudget;
				CountParams->MarchMipL1 = MipL1SRV;
				CountParams->MarchMipL2 = MipL2SRV;
				CountParams->MarchSkipVerify = (bSkipVerify && SkipLevels > 0) ? 1 : 0;
				CountParams->MarchHistogram = HistUAV;
				CountParams->MarchHistBins = Bins;
				CountParams->MarchCountViewSize =
					FUintVector2(uint32(ViewRect.Width()), uint32(ViewRect.Height()));

				FVoxelMarchSpikeCountCS::FPermutationDomain CountPermutation;
				CountPermutation.Set<FVoxelMarchSpikeCountCS::FSkipLevelsDim>(SkipLevels);
				TShaderMapRef<FVoxelMarchSpikeCountCS> CountShader(ShaderMap, CountPermutation);
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelMarchSpike.Census(budget %d)", StepBudget),
					CountShader, CountParams,
					FIntVector(FMath::DivideAndRoundUp(ViewRect.Width(),
					                                   FVoxelMarchSpikeCountCS::kGroupSize),
					           FMath::DivideAndRoundUp(ViewRect.Height(),
					                                   FVoxelMarchSpikeCountCS::kGroupSize),
					           1));
			}

			if (!Free->Readback.IsValid())
			{
				Free->Readback =
					MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarchSpike.CensusReadback"));
			}
			AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), HistRDG,
			                   Elements * sizeof(uint32));
			// The slot remembers the SHAPE it was enqueued with. The budget is a
			// live cvar, so a slot enqueued at 886 and decoded after a flip to
			// 64 would otherwise be read against the wrong row stride and
			// produce a census that looks fine and describes nothing.
			Free->Bins = Bins;
			Free->StepBudget = StepBudget;
			Free->SkipLevels = SkipLevels;
			Free->RaysDispatched = uint64(ViewRect.Width()) * uint64(ViewRect.Height());
			Free->Generation = ++State->CensusGeneration;
			Free->bInFlight = true;
		}
	}

	{
		FScopeLock Guard(&State->Lock);
		State->Stats.MarchFrames++;
	}
}

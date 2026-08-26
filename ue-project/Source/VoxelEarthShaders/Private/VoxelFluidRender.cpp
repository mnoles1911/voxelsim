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

namespace
{
	// CARRY THE ENGINE'S TAA/TSR SUB-PIXEL JITTER ON THE WATER SURFACE
	// RECONSTRUCTION. Same defect, same shape, as voxel.March.TAAJitter: the
	// shade pass rebuilds a view-space position from InvProjDiag, the projection
	// DIAGONAL, while UE puts the jitter in the third row (M[2][0]/M[2][1]). The
	// old comment on VoxelFluidRender.usf:135 stated the premise correctly and
	// drew the wrong conclusion from it -- "TAA jitter is sub-pixel and ignored".
	// It is sub-pixel, and being sub-pixel is precisely why it matters: the SCENE
	// DEPTH this pass reads was rasterised WITH the jitter, so reconstructing the
	// ray without it puts the surface normal and the refraction offset a fraction
	// of a pixel away from the geometry they are shading, every frame, by a
	// different amount.
	//
	// DEFAULT 0, DELIBERATELY. The owner has approved the MARCHER arm, not this
	// one; this ships dark until it has been measured at a pose with near-field
	// water in frame.
	//
	// THE HOOK IS SAFE, AND THAT IS NOT LUCK -- IT IS THE ONE THING THAT SANK THE
	// MARCHER FOR A DAY. Reading GetTemporalAAJitter() in
	// PreRenderView_RenderThread returns exactly (0,0), because that hook runs
	// from OnRenderBegin, BEFORE BeginInitViews applies the jitter. This pass
	// runs from PrePostProcessPass_RenderThread, which is after InitViews and
	// before TSR, so the value here is the real per-frame offset.
	TAutoConsoleVariable<int32> CVarVoxelFluidRenderTAAJitter(
		TEXT("voxel.Fluid.Render.TAAJitter"), 0,
		TEXT("Subtract the frame's TemporalAAJitter from NDC before the water shade pass "
		     "rebuilds a view-space position. 0 = the shipping behaviour (the ray ignores the "
		     "jitter entirely, so it disagrees with the jittered SceneDepth it is shading "
		     "against). 1 = carry it, which is what voxel.March.TAAJitter does for the marched "
		     "ray.\nNOT MEASURED YET. Needs a pose with near-field water actually in frame, and "
		     "voxel.Fluid.Render is itself default 0, so BOTH must be on before an A/B here "
		     "means anything -- an A/B with no water on screen compares the arm against "
		     "itself.\nNOT A PERMUTATION: a float2 uniform every permutation already binds."),
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
		// The frame's TAA/TSR jitter in NDC (voxel.Fluid.Render.TAAJitter), or
		// (0,0) when the arm is off. Subtracted from NDC before the
		// reconstruction -- the inverse of what UE did to the projection row 2.
		SHADER_PARAMETER(FVector2f, FluidTemporalAAJitter)
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
}

IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSplatVS,  VOXEL_FLUID_RENDER_USF, "FluidRenderSplatVS",  SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSplatPS,  VOXEL_FLUID_RENDER_USF, "FluidRenderSplatPS",  SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSmoothCS, VOXEL_FLUID_RENDER_USF, "FluidRenderSmoothCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScreenVS, VOXEL_FLUID_RENDER_USF, "FluidRenderScreenVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidShadePS,  VOXEL_FLUID_RENDER_USF, "FluidRenderShadePS",  SF_Pixel);

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

	// AND THE VIEW ORIGIN, for exactly the reason the anchor above is shared.
	//
	// NoteView() lived ONLY in the marcher's extension, which declines
	// IsActiveThisFrame on a stock leg -- so on the quad path the split's
	// camera speed stayed 0.0 for every frame, `bMoving` was never true, and
	// every settled frame was filed as SETTLED-PARKED. That is worse than the
	// empty population the settle latch produced: measured on M-split, a leg
	// flying at 23 m/s reported 199 PARKED frames and 0 MOVING ones, so the
	// PARKED bucket was not missing data, it was MISLABELLED flight.
	//
	// Goal 3 is a MOVING-frame goal, so a split that cannot see moving frames
	// answers a question nobody asked. Publishing here costs one FVector copy
	// on a hook the quad path already runs.
	if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0] != nullptr)
	{
		VoxelRenderFrame::NoteView(InViewFamily.Views[0]->ViewMatrices.GetViewOrigin());
	}
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
	// Resolved HERE, in the same hook that uses it. PrePostProcessPass runs
	// after InitViews, so this is the real offset and not the (0,0) that the
	// earlier PreRenderView hook would hand back.
	const FVector2D FluidJitterD = VM.GetTemporalAAJitter();
	const FVector2f FluidTemporalAAJitter =
	    (CVarVoxelFluidRenderTAAJitter.GetValueOnRenderThread() != 0)
	        ? FVector2f(float(FluidJitterD.X), float(FluidJitterD.Y))
	        : FVector2f::ZeroVector;
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
		Params->FluidTemporalAAJitter = FluidTemporalAAJitter;
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

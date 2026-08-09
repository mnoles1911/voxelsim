// VoxelFluidRender.cpp -- RDG pass builder for the screen-space fluid
// renderer. See VoxelFluidRender.h for the compositing-hook decision and
// VoxelFluidRender.usf for the pass chain and the mirrored material law; this
// file is the plumbing between them, following VoxelFluidSim.cpp's shape
// (FGlobalShader classes, parameter structs, IMPLEMENT_GLOBAL_SHADER off the
// virtual path) plus the project's FIRST raster passes outside a mesh pass.

#include "VoxelFluidRender.h"

#include "VoxelFluidSim.h" // FVoxelFluidSimState (particle buffer + slot bound), contract mirrors

#include "CommonRenderResources.h" // GEmptyVertexDeclaration
#include "DataDrivenShaderPlatformInfo.h"
#include "FXRenderingUtils.h" // UE::FXRenderingUtils::GetRawViewRectUnsafe
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneRenderTargetParameters.h" // ESceneTextureSetupMode
#include "SceneView.h"
#include "SceneTexturesConfig.h" // FSceneTextureUniformParameters (ENGINE_API, public)
#include "ShaderParameterStruct.h"

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
	// Consume the mailbox exactly once; a second view family this frame sims
	// nothing (and pays nothing).
	TOptional<FVoxelFluidSimTickArgs> Args;
	TSharedPtr<FVoxelFluidSimState, ESPMode::ThreadSafe> Sim;
	TSharedPtr<FVoxelFluidOccupancyVolume, ESPMode::ThreadSafe> OccAnchor;
	{
		FScopeLock Guard(&State->Lock);
		Sim = State->SimState;
		if (State->PendingSimArgs.IsSet())
		{
			Args = State->PendingSimArgs;
			State->PendingSimArgs.Reset();
			OccAnchor = State->OccupancyKeepAlive;
		}
	}
	if (!Args.IsSet() || !Sim.IsValid())
	{
		return;
	}
	// OccAnchor keeps Args->Occupancy alive across this scope; the raw
	// pointer inside Args is the one the passes bind (contract: one origin,
	// one volume).
	VoxelFluidSim::AddSimPasses(GraphBuilder, *Sim, Args.GetValue());
}

void FVoxelFluidRenderExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
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
	const FVector3f FluidOriginTranslatedWorld =
		FVector3f(Settings.FluidOriginWorld + VM.GetPreViewTranslation());
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

	RDG_EVENT_SCOPE(GraphBuilder, "VoxelFluidRender");

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

// VoxelMarchBound.cpp -- THE PER-RAY RESIDENT-EXTENT BOUND'S PRODUCER
// (voxel.March.Bound, Stage 0b).
//
// The two passes this file adds, why they read what they read, and why a
// decline returns null instead of a stand-in, are in VoxelMarchBound.h. The
// shader-side doctrine -- the soundness claim, the encoding, the refused
// permutations -- is in VoxelMarchBound.ush, and the consumer clamp is in
// VoxelBrickTraverse.ush's ZCut socket. This file is deliberately the SMALL
// half: enumerate the pool's own chunk table, rasterise its cubes, hand back
// one texture.

#include "VoxelMarchBound.h"

#include "VoxelBrickPool.h"

#include "CommonRenderResources.h" // GEmptyVertexDeclaration -- the DepthPreEmit pattern
#include "GlobalShader.h"
#include "RHIGPUReadback.h" // the cull engagement window's ring
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelMarchBound, Log, All);

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (the VOXEL_MARCH_USF note, same rule).
#define VOXEL_MARCH_BOUND_USF "/VoxelEarth/VoxelMarchBound.usf"

// Mirrors kVoxelMarchMaxRings (VoxelMarchRenderer.cpp) and
// VOXEL_MARCH_MAX_RINGS (VoxelBrickTraverse.ush): the consumer's loader is a
// fixed-trip loop over that many slices, so a producer that rendered more
// could never be read, and one that claimed more would make the loader load
// out of array bounds. Clamped against, never trusted.
static constexpr int32 kVoxelMarchBoundMaxSlices = 7;

// The BO_Min identity, and what an unwritten pixel decodes as. MUST MATCH
// VOXEL_MARCH_BOUND_EMPTY in VoxelMarchBound.ush -- both are the number
// "beyond any reachable t, still finite in float". The shader's #define is
// the authority; this is the CLEAR VALUE handed to RHI, which cannot read a
// shader define, so the constant is restated here with its provenance,
// exactly as kVoxelMarchStencilReceiveDecalMask restates an engine-private
// constant. 1e30 in BOTH channels: Near decodes +1e30, Far decodes -1e30 --
// the EMPTY interval, which is exactly what "no resident chunk touches this
// ray at this level" must decode as (the empty-interval collapse is the arm's
// primary payoff, per the Stage 0a census).
static constexpr float kVoxelMarchBoundEmpty = 1e30f;

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchBoundListParameters, )
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER(uint32, MarchBoundSlices)
	// The frame origin, needed since the cull: the list pass now derives
	// each record's cube (the shared VoxelMarchBoundChunkBox helper) to test
	// it against the frustum.
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	// The padded, jittered ray frustum -- five planes through the camera in
	// local UU, xyz inward normal, w = -dot(n, cam). Derived below from the
	// SAME view quantities the march binds; see VoxelMarchBoundBuildFrustum.
	SHADER_PARAMETER_ARRAY(FVector4f, MarchBoundCullPlane, [5])
	// 0 = the CPU found a degenerate plane and disabled the cull (fail-open:
	// keep everything). Mirrored to the readback by the shader so the log
	// can word culled=0 correctly.
	SHADER_PARAMETER(uint32, MarchBoundCullEnable)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchBoundOutCullStats)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchBoundOutList)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchBoundOutArgs)
END_SHADER_PARAMETER_STRUCT()

// One struct for VS and PS, the FVoxelMarchDepthOnlyParameters pattern: two
// structs for one draw is two chances for the halves to disagree about what
// is bound.
//
// The seven view fields carry THE NAMES OF THE LOOSE GLOBALS VoxelMarch.usf
// declares (the producer's .usf includes it for VoxelMarchBuildRay), so the
// PS provably reconstructs the march's own ray from the march's own uniforms.
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelMarchBoundDrawParameters, )
	VOXEL_BRICK_POOL_PARAMETERS()
	SHADER_PARAMETER(FMatrix44f, MarchViewToTranslatedWorld)
	SHADER_PARAMETER(FVector3f, MarchRayOriginLocalUU)
	SHADER_PARAMETER(float, MarchVolumeExtentUU)
	SHADER_PARAMETER(FVector2f, MarchViewRectMin)
	SHADER_PARAMETER(FVector2f, MarchViewRectSize)
	SHADER_PARAMETER(FVector2f, MarchInvProjDiag)
	SHADER_PARAMETER(FVector2f, MarchTemporalAAJitter)
	SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
	SHADER_PARAMETER(uint32, MarchBoundLevel)
	// The half-res dilation slope, 4x the full-res pixel half-width cone --
	// the VS carries the conservatism argument.
	SHADER_PARAMETER(float, MarchBoundDilateSlope)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchBoundList)
	RDG_BUFFER_ACCESS(MarchBoundDrawArgs, ERHIAccess::IndirectArgs)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

// The compilation environment all three shaders share. VOXEL_MARCH_SOURCE 1
// so VoxelBrickTraverse.ush declares the pool globals the list pass and the
// VS read (VOXEL_MARCH_HAS_BRICKPOOL), and VOXEL_MARCH_RINGS 1 so
// VoxelMarchBuildRay compiles the CENTRED box clip the ring marcher runs --
// the cornered variant is a DIFFERENT ray clip, and a bound built through it
// would describe rays the march never traces. VOXEL_MARCH_BOUND itself stays
// 0 here: the consumer half of VoxelMarchBound.ush (texture global, statics,
// #errors) must not compile into the producer.
static void VoxelMarchBoundModifyEnvironment(FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
	OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_RINGS"), 1);
}

class FVoxelMarchBoundListCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchBoundListCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchBoundListCS, FGlobalShader);
	using FParameters = FVoxelMarchBoundListParameters;

	static constexpr int32 kGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VoxelMarchBoundModifyEnvironment(OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_BOUND_LIST_GROUP"), kGroupSize);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchBoundListCS, VOXEL_MARCH_BOUND_USF, "VoxelMarchBoundListMain",
                        SF_Compute);

class FVoxelMarchBoundVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchBoundVS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchBoundVS, FGlobalShader);
	using FParameters = FVoxelMarchBoundDrawParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VoxelMarchBoundModifyEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchBoundVS, VOXEL_MARCH_BOUND_USF, "VoxelMarchBoundVS", SF_Vertex);

class FVoxelMarchBoundPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelMarchBoundPS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelMarchBoundPS, FGlobalShader);
	using FParameters = FVoxelMarchBoundDrawParameters;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
	                                         FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		VoxelMarchBoundModifyEnvironment(OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchBoundPS, VOXEL_MARCH_BOUND_USF, "VoxelMarchBoundPS", SF_Pixel);

// ---------------------------------------------------------------------------
// The cull engagement window (see FVoxelMarchBoundCullStats in the header)
// ---------------------------------------------------------------------------
//
// A three-slot ring, the house readback shape: the GPU runs 1-3 frames
// behind, the render thread must not stall, and a frame with no free slot
// goes unmeasured and biases nothing because the window divides by frames
// LANDED. Ring is render-thread-only; the accumulated window is shared with
// the game-thread drain under its own lock.
namespace
{
	struct FBoundCullReadbackSlot
	{
		TUniquePtr<FRHIGPUBufferReadback> Stats;
		TUniquePtr<FRHIGPUBufferReadback> Args;
		bool bInFlight = false;
		int32 SliceCount = 0;
	};
	static constexpr int32 kBoundCullReadbacks = 3;
	FBoundCullReadbackSlot GBoundCullRing[kBoundCullReadbacks];
	FCriticalSection GBoundCullWindowLock;
	FVoxelMarchBoundCullStats GBoundCullWindow;

	// Render thread, called once per produce. BOTH readbacks of a slot must
	// be ready before either is read: they describe one dispatch, and the
	// considered == culled + drawn identity the log prints is only exact if
	// the two halves come from the same frame.
	void VoxelMarchBoundPollCullReadbacks()
	{
		for (FBoundCullReadbackSlot& Slot : GBoundCullRing)
		{
			if (!Slot.bInFlight || !Slot.Stats.IsValid() || !Slot.Args.IsValid() ||
			    !Slot.Stats->IsReady() || !Slot.Args->IsReady())
			{
				continue;
			}
			const uint32* S = static_cast<const uint32*>(Slot.Stats->Lock(3 * sizeof(uint32)));
			const uint32* A = static_cast<const uint32*>(
				Slot.Args->Lock(uint32(Slot.SliceCount) * 4u * sizeof(uint32)));
			if (S != nullptr && A != nullptr)
			{
				FScopeLock Guard(&GBoundCullWindowLock);
				GBoundCullWindow.Considered += S[0];
				GBoundCullWindow.Culled += S[1];
				GBoundCullWindow.LastEnable = S[2];
				for (int32 L = 0; L < Slot.SliceCount && L < 7; ++L)
				{
					// Element 1 of FRHIDrawIndirectParameters is
					// InstanceCount -- the same word the tile readback
					// reads, for the same reason: four uints copied, the
					// second read, because a wrong offset returns a
					// plausible number instead of an error.
					GBoundCullWindow.DrawnPerLevel[L] += A[L * 4 + 1];
				}
				GBoundCullWindow.Frames++;
				GBoundCullWindow.bEverLanded = true;
			}
			if (S != nullptr) { Slot.Stats->Unlock(); }
			if (A != nullptr) { Slot.Args->Unlock(); }
			Slot.bInFlight = false;
		}
	}

	// The padded, jittered ray frustum, in the LOCAL UU frame. ONE AUTHORITY:
	// the corner directions run the exact BuildRay pipeline -- pixel -> NDC
	// (y flipped) -> (Ndc - TemporalAAJitter) * InvProjDiag -> the row-vector
	// transform through ViewToTranslatedWorld -- for the four view-rect
	// corners PADDED ONE FULL PIXEL OUTWARD, plus the centre for orientation
	// and the forward plane. Returns false (cull disabled, fail-open) on any
	// degenerate or non-finite normal rather than culling with garbage.
	bool VoxelMarchBoundBuildFrustum(const FVoxelMarchBoundInputs& Inputs,
	                                 FVector4f OutPlanes[5])
	{
		const FVector2f Size = Inputs.ViewRectSize;
		if (Size.X <= 0.0f || Size.Y <= 0.0f)
		{
			return false;
		}
		const auto RayDir = [&Inputs, Size](float PxX, float PxY) -> FVector3f
		{
			FVector2f Ndc(PxX / Size.X * 2.0f - 1.0f, PxY / Size.Y * 2.0f - 1.0f);
			Ndc.Y = -Ndc.Y; // clip y is up, pixel y is down -- BuildRay's flip
			const FVector2f D2 = (Ndc - Inputs.TemporalAAJitter) * Inputs.InvProjDiag;
			// Row-vector transform with w = 0: TransformVector is V * M,
			// which is what mul(float4(v, 0), M) computes in BuildRay.
			const FVector4f W4 =
				Inputs.ViewToTranslatedWorld.TransformVector(FVector3f(D2.X, D2.Y, 1.0f));
			return FVector3f(W4.X, W4.Y, W4.Z);
		};
		const float Pad = 1.0f; // one full pixel outward, on top of the jitter
		const FVector3f D[4] = {
			RayDir(-Pad, -Pad),                     // top-left
			RayDir(Size.X + Pad, -Pad),             // top-right
			RayDir(Size.X + Pad, Size.Y + Pad),     // bottom-right
			RayDir(-Pad, Size.Y + Pad),             // bottom-left
		};
		const FVector3f Centre = RayDir(Size.X * 0.5f, Size.Y * 0.5f);
		const FVector3f Cam = Inputs.RayOriginLocalUU;

		FVector3f Normals[5];
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			Normals[Edge] = FVector3f::CrossProduct(D[Edge], D[(Edge + 1) % 4]);
		}
		Normals[4] = Centre; // the forward plane: everything behind writes
		                     // nothing anyway, but not listing it was most of
		                     // BND-eng2's boundMs

		for (int32 PlaneIdx = 0; PlaneIdx < 5; ++PlaneIdx)
		{
			FVector3f N = Normals[PlaneIdx];
			// Orient INWARD by the centre ray rather than by a handedness
			// argument: the sign of a cross product here depends on corner
			// winding and axis conventions, and a flipped plane would cull
			// the visible half of the world -- checked, not reasoned about.
			if (FVector3f::DotProduct(N, Centre) < 0.0f)
			{
				N = -N;
			}
			N = N.GetSafeNormal();
			if (!N.ContainsNaN() && N.IsUnit(1.0e-3f))
			{
				OutPlanes[PlaneIdx] = FVector4f(N, -FVector3f::DotProduct(N, Cam));
				if (OutPlanes[PlaneIdx].ContainsNaN())
				{
					return false;
				}
			}
			else
			{
				// Degenerate (zero-area edge, NaN matrix): FAIL-OPEN.
				return false;
			}
		}
		return true;
	}
} // namespace

FVoxelMarchBoundCullStats VoxelMarchBoundGetAndResetCullStats()
{
	FScopeLock Guard(&GBoundCullWindowLock);
	const FVoxelMarchBoundCullStats Out = GBoundCullWindow;
	GBoundCullWindow = FVoxelMarchBoundCullStats();
	// LastEnable survives the reset: it describes the shader's most recent
	// state, not the window's events, and a drain that zeroed it would print
	// one spurious DISABLED per quiet window.
	GBoundCullWindow.LastEnable = Out.LastEnable;
	return Out;
}

FRDGTextureRef VoxelMarchBoundProduce(FRDGBuilder& GraphBuilder, const FGlobalShaderMap* ShaderMap,
                                      const FVoxelMarchBoundInputs& Inputs)
{
	// Land whatever earlier frames finished before adding this frame's work
	// -- polled here, on the render thread, every armed frame, so a slot
	// never sits ready-but-unread while the ring looks full.
	VoxelMarchBoundPollCullReadbacks();

	FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
	const uint32 ChunkSlots = Pool.GetConfig().ChunkCapacity;
	const int32 SliceCount = FMath::Clamp(Inputs.SliceCount, 1, kVoxelMarchBoundMaxSlices);
	// PixelConeSlope <= 0 declines: the half-res target's conservatism IS
	// the cone dilation, and an undilated half-res bound can miss a cube a
	// block ray hits -- a hole. No real projection produces a non-positive
	// slope, so this is a corruption guard, not a mode.
	if (ChunkSlots == 0 || Inputs.TargetSize.X <= 0 || Inputs.TargetSize.Y <= 0 ||
	    Inputs.PixelConeSlope <= 0.0f)
	{
		// No pool config yet (never flushed) or a degenerate view. Declining
		// hands the caller null, which switches the march to the CONTROL
		// permutation -- see the header for why that and never a stand-in.
		// LOGGED ONCE: a silent decline plus an armed cvar is exactly the
		// "armed and inert" state the engagement line warns about, and this
		// is the line its warning tells the reader to look for.
		static bool bDeclineLogged = false;
		if (!bDeclineLogged)
		{
			bDeclineLogged = true;
			UE_LOG(LogVoxelMarchBound, Display,
			       TEXT("voxel.March.Bound producer DECLINED (chunk capacity %u, target "
			            "%dx%d, coneSlope %g). The march runs the CONTROL permutation "
			            "until this resolves; bndConsulted will read 0 and must be worded "
			            "as inert, not as 'cut nothing'."),
			       ChunkSlots, Inputs.TargetSize.X, Inputs.TargetSize.Y,
			       Inputs.PixelConeSlope);
		}
		return nullptr;
	}

	// ---- the list pass ----------------------------------------------------
	//
	// The list is SLICE-STRIDED at the pool's full chunk capacity: a slot
	// lives in at most one level, so Slices x capacity always fits, and the
	// flat layout buys a one-pass build (no prefix sum) for a few transient
	// megabytes (7 x ~131K x 4 B ~= 3.7 MB at current defaults) -- the Stage
	// 0b trade, stated rather than hidden.
	FRDGBufferRef List = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), int32(ChunkSlots) * SliceCount),
		TEXT("VoxelMarch.BoundList"));
	FRDGBufferRef Args = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndirectParameters>(SliceCount),
		TEXT("VoxelMarch.BoundArgs"));
	FRDGBufferUAVRef ArgsUAV = GraphBuilder.CreateUAV(Args, PF_R32_UINT);
	// Cleared so every level's InstanceCount starts at zero; the list pass's
	// own first threads write the non-atomic fields (vertex count 36 etc.).
	AddClearUAVPass(GraphBuilder, ArgsUAV, 0u);

	// The frustum, and the cull's engagement words (considered / culled /
	// enable-mirror -- drawn lives in the args). Cleared every frame: the
	// words describe ONE dispatch, and the readback below copies them the
	// same frame they are written.
	FVector4f CullPlanes[5] = {FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0),
	                           FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0),
	                           FVector4f(0, 0, 0, 0)};
	const bool bCullEnabled = VoxelMarchBoundBuildFrustum(Inputs, CullPlanes);
	FRDGBufferRef CullStats = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 3), TEXT("VoxelMarch.BoundCullStats"));
	FRDGBufferUAVRef CullStatsUAV = GraphBuilder.CreateUAV(CullStats, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, CullStatsUAV, 0u);

	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelMarchBoundListParameters>();
		if (!Pool.BindShaderParameters(GraphBuilder, *Params))
		{
			// Arenas not created yet. Same decline, same reason.
			return nullptr;
		}
		Params->MarchBoundSlices = uint32(SliceCount);
		Params->MarchBrickOriginVoxel = Inputs.FrameOriginVoxel;
		for (int32 PlaneIdx = 0; PlaneIdx < 5; ++PlaneIdx)
		{
			Params->MarchBoundCullPlane[PlaneIdx] = CullPlanes[PlaneIdx];
		}
		Params->MarchBoundCullEnable = bCullEnabled ? 1u : 0u;
		Params->MarchBoundOutCullStats = CullStatsUAV;
		Params->MarchBoundOutList = GraphBuilder.CreateUAV(List, PF_R32_UINT);
		Params->MarchBoundOutArgs = ArgsUAV;

		TShaderMapRef<FVoxelMarchBoundListCS> Shader(ShaderMap);
		// NeverCull for the march passes' reason: in mode 2 the consumer
		// chain can end at scratch targets RDG would otherwise remove, and a
		// producer that silently vanished would leave this arm reporting a
		// near-zero boundMs for work that never happened -- the exact
		// instrument failure the march pass's own NeverCull note records.
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VoxelMarch.BoundList(%u slots -> %d levels)", ChunkSlots, SliceCount),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, Shader, Params,
			FIntVector(FMath::DivideAndRoundUp(int32(ChunkSlots), FVoxelMarchBoundListCS::kGroupSize),
			           1, 1));
	}

	// ---- the raster passes, one per ring level -----------------------------
	//
	// RG32F, min-blended, cleared to (+EMPTY, +EMPTY) so an unwritten pixel IS
	// the empty interval -- the encoding contract in VoxelMarchBound.ush.
	// TexCreate_TargetArraySlicesIndependently IS WHAT MAKES SLICE > 0
	// BINDABLE AS A RENDER TARGET ON D3D12, and its absence was a CRASH, not
	// a caveat: D3D12 creates per-slice RTVs only under this flag
	// (D3D12Texture.cpp:1733, bCreateRTVsPerSlice =
	// EnumHasAnyFlags(Desc.Flags, TexCreate_TargetArraySlicesIndependently)),
	// and without them GetRenderTargetView checks
	// `ArraySliceIndex == -1 || ArraySliceIndex == 0` (D3D12Texture.h:200) --
	// the exact assert the first engagement leg died on at the L1 draw
	// (Saved/BND-eng.log:1905, 2026-08-28). The engine says the rule in words
	// at CapsuleShadowRendering.cpp:624 and ships the pattern this copies at
	// DistortionRendering.cpp:602 and ReflectionEnvironmentCapture.cpp:2722.
	// HALF RESOLUTION PER AXIS since 2026-08-28 (a quarter of BND-eng2's
	// fill). Rounded UP so the consumer's `pixel >> 1` is always in bounds;
	// the conservatism this needs is the VS's cone dilation, argued there.
	const FIntPoint BoundSize(FMath::DivideAndRoundUp(Inputs.TargetSize.X, 2),
	                          FMath::DivideAndRoundUp(Inputs.TargetSize.Y, 2));
	FRDGTextureRef BoundTex = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2DArray(
			BoundSize, PF_G32R32F,
			FClearValueBinding(FLinearColor(kVoxelMarchBoundEmpty, kVoxelMarchBoundEmpty, 0.0f, 0.0f)),
			TexCreate_RenderTargetable | TexCreate_ShaderResource |
				TexCreate_TargetArraySlicesIndependently,
			uint16(SliceCount)),
		TEXT("VoxelMarch.Bound"));

	for (int32 Level = 0; Level < SliceCount; ++Level)
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelMarchBoundDrawParameters>();
		if (!Pool.BindShaderParameters(GraphBuilder, *Params))
		{
			return nullptr; // cannot happen after the list bind succeeded; refused anyway
		}
		// THE MARCH'S OWN RAY MODEL, copied from the caller's copy of the
		// march's own view block. Field for field; see FVoxelMarchBoundInputs
		// for why a POD and not the shared struct type.
		Params->MarchViewToTranslatedWorld = Inputs.ViewToTranslatedWorld;
		Params->MarchRayOriginLocalUU = Inputs.RayOriginLocalUU;
		Params->MarchVolumeExtentUU = Inputs.VolumeExtentUU;
		Params->MarchViewRectMin = Inputs.ViewRectMin;
		Params->MarchViewRectSize = Inputs.ViewRectSize;
		Params->MarchInvProjDiag = Inputs.InvProjDiag;
		Params->MarchTemporalAAJitter = Inputs.TemporalAAJitter;
		Params->MarchBrickOriginVoxel = Inputs.FrameOriginVoxel;
		Params->MarchBoundLevel = uint32(Level);
		// 4x the pixel half-width slope: covers the 2x2 block's worst ray
		// offset (0.707 px) plus the odd-rect raster skew (up to ~1 px) with
		// margin -- the VS carries the full argument. Guarded > 0 by the
		// decline above.
		Params->MarchBoundDilateSlope = 4.0f * Inputs.PixelConeSlope;
		Params->MarchBoundList = GraphBuilder.CreateSRV(List, PF_R32_UINT);
		Params->MarchBoundDrawArgs = Args;
		// Each slice is bound exactly once, so EClear here is the whole
		// clear -- no separate clear pass, and no slice is ever left holding
		// pooled-allocator garbage, which would decode as a PLAUSIBLE
		// interval: the worst available outcome. THE CLEAR PROVABLY RUNS PER
		// SLICE: each of these passes binds its OWN per-slice RTV (the
		// TargetArraySlicesIndependently flag above) with EClear, the load
		// action executes at render-pass begin whether or not the indirect
		// draw has instances, the pass is NeverCull, and RDG cannot merge an
		// EClear binding into a predecessor's render pass
		// (FRenderTargetBinding::CanMergeBefore, ShaderParameterMacros.h:510,
		// refuses exactly that). The consumer still carries a fail-open
		// decode for the state this makes impossible -- see the loader.
		Params->RenderTargets[0] = FRenderTargetBinding(
			BoundTex, ERenderTargetLoadAction::EClear, /*MipIndex*/ 0, /*ArraySlice*/ int16(Level));

		TShaderMapRef<FVoxelMarchBoundVS> VertexShader(ShaderMap);
		TShaderMapRef<FVoxelMarchBoundPS> PixelShader(ShaderMap);
		// The RASTER grid is the half-res target; the ray model inside the
		// shaders stays the full-res rect (see the PS). Both sizes are in
		// the event name so a capture shows the half-res arm engaged.
		const FIntPoint TargetSize = BoundSize;
		FRDGBufferRef DrawArgs = Args;
		const uint32 ArgsOffset = uint32(Level) * sizeof(FRHIDrawIndirectParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("VoxelMarch.BoundRaster(L%d, %dx%d half-res of %dx%d)", Level,
			               TargetSize.X, TargetSize.Y, Inputs.TargetSize.X,
			               Inputs.TargetSize.Y),
			Params, ERDGPassFlags::Raster | ERDGPassFlags::NeverCull,
			[Params, VertexShader, PixelShader, TargetSize, DrawArgs,
			 ArgsOffset](FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer PSOInit;
				RHICmdList.ApplyCachedRenderTargets(PSOInit);
				// BO_Min ON BOTH CHANNELS IS THE WHOLE DATA STRUCTURE: the
				// running min of (tNear, -tFar) across every cube covering
				// the pixel IS the convex-hull interval. Blend factors are
				// ignored by min/max ops; One keeps the PSO hash honest.
				PSOInit.BlendState =
					TStaticBlendState<CW_RG, BO_Min, BF_One, BF_One, BO_Min, BF_One,
					                  BF_One>::GetRHI();
				// CM_None: back faces MUST rasterise -- a camera inside a
				// cube sees only back faces, and every ray's exit point lies
				// on one. The interval is a property of the per-instance box
				// (nointerpolation), so double coverage writes the same
				// value twice and min makes it idempotent.
				PSOInit.RasterizerState =
					TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
				// No depth target at all: occlusion is the march's business
				// (its prepass t_max), not the bound's -- a bound behind an
				// occluder still soundly clamps a walk the prepass will end
				// early anyway.
				PSOInit.DepthStencilState =
					TStaticDepthStencilState<false, CF_Always>::GetRHI();
				PSOInit.BoundShaderState.VertexDeclarationRHI =
					GEmptyVertexDeclaration.VertexDeclarationRHI;
				PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
				PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				PSOInit.PrimitiveType = PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, PSOInit, 0);
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(),
				                    *Params);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(),
				                    *Params);
				RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, float(TargetSize.X),
				                       float(TargetSize.Y), 1.0f);
				RHICmdList.DrawPrimitiveIndirect(DrawArgs->GetIndirectRHICallBuffer(),
				                                 ArgsOffset);
			});
	}

	// ---- the cull engagement readback ---------------------------------
	//
	// Both buffers of one dispatch into one slot, so the identity the log
	// checks (considered == culled + sum(drawn)) compares numbers from the
	// same frame. No free slot = an unmeasured frame, which biases nothing.
	{
		FBoundCullReadbackSlot* Free = nullptr;
		for (FBoundCullReadbackSlot& Slot : GBoundCullRing)
		{
			if (!Slot.bInFlight)
			{
				Free = &Slot;
				break;
			}
		}
		if (Free != nullptr)
		{
			if (!Free->Stats.IsValid())
			{
				Free->Stats =
					MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.BoundCullStatsRb"));
				Free->Args = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.BoundArgsRb"));
			}
			AddEnqueueCopyPass(GraphBuilder, Free->Stats.Get(), CullStats, 3 * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, Free->Args.Get(), Args,
			                   uint32(SliceCount) * 4u * sizeof(uint32));
			Free->SliceCount = SliceCount;
			Free->bInFlight = true;
		}
	}

	return BoundTex;
}

#include "VoxelGpuPoolComponent.h"

#include "VoxelQuadVertexFactory.h"
#include "PrimitiveSceneProxy.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "RHIResourceUtils.h"

// Draws every chunk in the pool with a single mesh batch.
namespace VoxelGpuPoolCull
{
	// HARD CEILING ON DRAW RANGES, and it is a correctness limit rather than a
	// taste one. The renderer selects batch elements with
	// `(1ull << BatchElementIndex) & BatchElementMask` (FMeshBatch), which is
	// undefined for any index >= 64. GPUCullMaxRanges used to default to 256, so
	// a sufficiently fragmented view was one shift away from undefined
	// behaviour that would have presented as random missing terrain.
	static constexpr int32 kMaxBatchElements = 64;

	static int32 GEnabled = 0;
	static FAutoConsoleVariableRef CVarEnabled(
		TEXT("voxel.Stream.GPUCull"), GEnabled,
		TEXT("Frustum-cull the GPU pool per chunk and draw only the surviving pool ranges. 1 = on, 0 = off ")
		TEXT("(default). 0 is one draw over the whole pool, which is the pre-cull behaviour and pays for ")
		TEXT("every resident quad regardless of where the camera looks."),
		ECVF_RenderThreadSafe);

	// Merging tolerance, in quads. Two surviving ranges separated by a gap
	// smaller than this are drawn as one, which re-draws the gap. That is a good
	// trade well past the point it looks wasteful: a quad costs six vertex
	// invocations and no pixels if it is off-screen, while a draw costs a state
	// change and a command. Tuned by measurement, not taste -- raise it if the
	// range count is high, lower it if the drawn-quad count is.
	//
	// THIS USED TO DO NOTHING. It was declared, documented, and referenced only
	// from a comment; the merge ran exclusively off a threshold derived from
	// GPUCullMaxRanges, so tuning this knob silently changed nothing and any
	// "tuned merge gap" measurement would have been measuring the default. It is
	// now the FIRST merge pass, with the range-cap merge below it as the backstop
	// that guarantees the element limit.
	static int32 GMergeGapQuads = 4096;
	static FAutoConsoleVariableRef CVarMergeGap(
		TEXT("voxel.Stream.GPUCullMergeGap"), GMergeGapQuads,
		TEXT("Quads. Surviving pool ranges closer together than this are merged into one draw, re-drawing ")
		TEXT("the gap between them. Trades redrawn off-screen quads against draw count. 0 disables this ")
		TEXT("pass, leaving only the range-cap merge."),
		ECVF_RenderThreadSafe);

	// The draw-range budget the merge works down to. Clamped to
	// kMaxBatchElements wherever it is read -- see that constant.
	static int32 GMaxRanges = kMaxBatchElements;
	static FAutoConsoleVariableRef CVarMaxRanges(
		TEXT("voxel.Stream.GPUCullMaxRanges"), GMaxRanges,
		TEXT("Draw-range budget for the cull. The merge always reaches it by construction. CLAMPED TO 64: ")
		TEXT("the renderer's batch-element mask is a uint64 and index >= 64 is undefined."),
		ECVF_RenderThreadSafe);

	// The budget actually used, never above the element-mask limit.
	inline int32 GetMaxRanges() { return FMath::Clamp(GMaxRanges, 1, kMaxBatchElements); }

	// Control experiment: run the whole range/merge/multi-draw path but treat
	// EVERY chunk as visible. If the picture is correct with this on and wrong
	// with it off, the frustum test is selecting the wrong chunks; if it is wrong
	// both ways, the fault is in emitting many draw elements per batch. One cvar
	// separates two hypotheses that produce the identical symptom.
	static int32 GDebugAllVisible = 0;
	static FAutoConsoleVariableRef CVarDebugAllVisible(
		TEXT("voxel.Stream.GPUCullDebugAllVisible"), GDebugAllVisible,
		TEXT("1 = skip the frustum test and treat every chunk as visible, while still going through the "
		     "range merge and multi-element draw path. Isolates 'the cull picks the wrong chunks' from "
		     "'many draw elements per batch is broken'."),
		ECVF_RenderThreadSafe);

	// The control DebugAllVisible could not be. With every chunk visible the
	// surviving runs are contiguous, so the merge collapses them to ONE range and
	// the experiment degenerates into the single full draw it was meant to be
	// compared against. This one ignores the runs entirely and chops [0, NumQuads)
	// into N equal contiguous ranges that together cover the pool exactly once, so
	// N draw elements are exercised while the drawn SET is provably identical to
	// the full draw. Any difference in the picture is the multi-element draw path,
	// not the chunk selection.
	static int32 GDebugSplit = 0;
	static FAutoConsoleVariableRef CVarDebugSplit(
		TEXT("voxel.Stream.GPUCullDebugSplit"), GDebugSplit,
		TEXT("N>1: ignore the frustum and draw the whole pool as N equal contiguous ranges. Covers exactly "
		     "the same quads as the single full draw, so it isolates 'many draw elements per batch' from "
		     "'the cull picks the wrong chunks'. 0/1 = off."),
		ECVF_RenderThreadSafe);

	// Draws the chunks the frustum test REJECTED instead of the ones it accepted.
	// A one-bit answer to "is the frustum test picking the right chunks": if it is,
	// this renders (nearly) nothing. Merging is skipped here, so what reaches the
	// screen is exactly the rejected set and nothing else.
	static int32 GDebugInvert = 0;
	static FAutoConsoleVariableRef CVarDebugInvert(
		TEXT("voxel.Stream.GPUCullDebugInvert"), GDebugInvert,
		TEXT("1 = draw exactly the runs the frustum test rejected, unmerged. If the cull is correct this "
		     "renders almost nothing; terrain still on screen is geometry the cull is wrongly discarding."),
		ECVF_RenderThreadSafe);

	inline bool IsEnabled() { return GEnabled != 0; }
}

class FVoxelGpuPoolSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FVoxelGpuPoolSceneProxy(UVoxelGpuPoolComponent* Component,
	                        const TArray<uint64>& InQuads,
	                        const TArray<uint32>& InChunkIds,
	                        const TArray<FVector4f>& InOrigins,
	                        const TArray<FVector4f>& InParams,
	                        const TArray<UVoxelGpuPoolComponent::FChunkRun>& InRuns,
	                        const FString& InPoolName,
	                        int32 InChunkTableCapacity,
	                        FVoxelGpuPoolBuffersRef InSharedBuffers)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, PoolName(InPoolName)
		, SharedBuffers(MoveTemp(InSharedBuffers))
		, Quads(InQuads)
		, ChunkIds(InChunkIds)
		, Origins(InOrigins)
		, Params(InParams)
		, Runs(InRuns)
		, ChunkEdgeVoxels(Component->GetChunkEdgeVoxels())
		, NumQuads(Component->GetHighWaterMarkQuads())
		, BufferQuads(InQuads.Num())
		, NumChunks(InOrigins.Num())
		, MaxChunks(FMath::Max(InOrigins.Num() * 4, InChunkTableCapacity))
	{
		UMaterialInterface* Material = Component->GetChunkMaterialOrDefault();
		MaterialProxy = Material->GetRenderProxy();
		MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetFeatureLevel());

		#if !(UE_BUILD_SHIPPING)
		{
			TArray<UMaterialInterface*> UsedForVerification;
			UsedForVerification.Add(Material);
			SetUsedMaterialForVerification(UsedForVerification);
		}
		#endif
	}

	virtual ~FVoxelGpuPoolSceneProxy()
	{
		VertexFactory.ReleaseResource();
	}

	void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override
	{
		if (BufferQuads == 0 || NumChunks == 0)
		{
			return;
		}

		// Static, NOT Dynamic -- this is load-bearing, and it is the opposite of
		// what "we update this every frame" instinctively suggests.
		//
		// These buffers are written in sub-ranges (see UpdateQuadRange_RenderThread).
		// In the D3D12 RHI only the *static* lock path honours a lock offset: it
		// allocates a staging buffer of exactly the locked size and issues a
		// CopyBufferRegion into the destination at that offset
		// (D3D12Buffer.cpp:750, :801, :818). The *dynamic* path ignores the offset
		// completely and hands back the buffer's base address (:659), then renames
		// the whole buffer to a fresh upload allocation on every lock after the
		// first (:667, :697) -- so everything outside the range just written
		// becomes uninitialised garbage. Marking these Dynamic silently corrupted
		// the chunk table and put every partial write at quad 0.
		// --- the persistent half (Wave D / D1) ----------------------------
		//
		// UPLOADED ON FIRST CREATION ONLY. This one branch is what removes the
		// CPU-shadow clobber: every later proxy rebinds buffers that already
		// hold the live geometry instead of overwriting them with whatever the
		// CPU shadow happens to contain. Once the GPU writes quads directly,
		// re-uploading here would silently revert them.
		check(SharedBuffers.IsValid());
		if (!SharedBuffers->IsValid() || SharedBuffers->CapacityQuads < Quads.Num())
		{
			// First proxy, or the pool outgrew the allocation. The latter means
			// the GPU-resident contents are genuinely gone, so re-uploading from
			// the CPU shadow is the correct and only thing to do.
			SharedBuffers->QuadBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
				RHICmdList, TEXT("VoxelGpuPool.Quads"),
				EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource
					| EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer,
				MakeConstArrayView(Quads));
			SharedBuffers->QuadSRV = RHICmdList.CreateShaderResourceView(
				SharedBuffers->QuadBuffer,
				FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(SharedBuffers->QuadBuffer));
			SharedBuffers->QuadUAV = RHICmdList.CreateUnorderedAccessView(
				SharedBuffers->QuadBuffer,
				FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(SharedBuffers->QuadBuffer));

			SharedBuffers->ChunkIdBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
				RHICmdList, TEXT("VoxelGpuPool.ChunkIds"),
				EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource
					| EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::StructuredBuffer,
				MakeConstArrayView(ChunkIds));
			SharedBuffers->ChunkIdSRV = RHICmdList.CreateShaderResourceView(
				SharedBuffers->ChunkIdBuffer,
				FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(SharedBuffers->ChunkIdBuffer));
			SharedBuffers->ChunkIdUAV = RHICmdList.CreateUnorderedAccessView(
				SharedBuffers->ChunkIdBuffer,
				FRHIViewDesc::CreateBufferUAV().SetTypeFromBuffer(SharedBuffers->ChunkIdBuffer));

			SharedBuffers->CapacityQuads = Quads.Num();

			UE_LOG(LogTemp, Log, TEXT("%s: created persistent pool buffers, capacity %d quads"),
			       *PoolName, SharedBuffers->CapacityQuads);
		}
		else
		{
			// The line that makes the clobber impossible rather than unlikely.
			UE_LOG(LogTemp, Log,
			       TEXT("%s: REBOUND persistent pool buffers (capacity %d quads) — no re-upload, "
			            "GPU-written quads preserved across proxy recreation"),
			       *PoolName, SharedBuffers->CapacityQuads);
		}

		QuadBuffer = SharedBuffers->QuadBuffer;
		QuadBufferSRV = SharedBuffers->QuadSRV;
		ChunkIdBuffer = SharedBuffers->ChunkIdBuffer;
		ChunkIdSRV = SharedBuffers->ChunkIdSRV;

		TArray<FVector4f> PaddedOrigins = Origins;
		PaddedOrigins.SetNumZeroed(MaxChunks);
		OriginBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkOrigins"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(PaddedOrigins));
		OriginSRV = RHICmdList.CreateShaderResourceView(
			OriginBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(OriginBuffer));

		TArray<FVector4f> PaddedParams = Params;
		PaddedParams.SetNumZeroed(MaxChunks);
		ParamsBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkParams"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(PaddedParams));
		ParamsSRV = RHICmdList.CreateShaderResourceView(
			ParamsBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ParamsBuffer));

		VertexFactory.SetQuadBufferSRV(QuadBufferSRV);
		VertexFactory.SetPoolBuffers(OriginSRV, ChunkIdSRV, ParamsSRV);
		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogTemp, Log,
		       TEXT("%s: %d chunks, %d quads, %d triangles â€” ONE primitive, ONE draw"),
		       *PoolName, NumChunks, NumQuads, NumQuads * 2);

	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bDynamicRelevance = true;
		Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		// Matches FWaterChunkSceneProxy. Inert for an opaque material, so this
		// costs terrain nothing; it is what a TRANSLUCENT pool (the water
		// instance) needs in order to receive translucent self-shadowing the
		// same way the per-brick water components do.
		Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		// Must come after SetPrimitiveViewRelevance, which is what fills in
		// bOpaque. Same expression as FVoxelChunkSceneProxy so the two renderers
		// contribute to the velocity pass identically -- the factory already
		// implements VertexFactoryGetPreviousWorldPosition, so the geometry for
		// it was always there; only the relevance flag that asks for it was
		// missing, and without it TSR reprojects this terrain from depth alone.
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
		return Result;
	}

	void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	                            const FSceneViewFamily& ViewFamily,
	                            uint32 VisibilityMap,
	                            FMeshElementCollector& Collector) const override
	{
		if (NumQuads == 0 || !QuadBufferSRV.IsValid() || MaterialProxy == nullptr)
		{
			// Silence is ambiguous: a pool that draws nothing looks identical
			// whether the renderer never asked or the batch was rejected. Say
			// which.
			if (ElementsLogged.Increment() == 1)
			{
				UE_LOG(LogTemp, Warning,
				       TEXT("%s draw SKIPPED: numQuads=%d srv=%d materialProxy=%d"),
				       *PoolName, NumQuads, QuadBufferSRV.IsValid() ? 1 : 0, MaterialProxy != nullptr ? 1 : 0);
			}
			return;
		}

		if (ElementsLogged.Increment() == 1)
		{
			UE_LOG(LogTemp, Log,
			       TEXT("%s draw SUBMITTED: numQuads=%d views=%d visMap=0x%x"),
			       *PoolName, NumQuads, Views.Num(), VisibilityMap);
		}

		// Editor Wireframe view mode, mirroring FVoxelChunkSceneProxy. Without
		// this the pooled terrain is the one thing in the level that stays solid
		// when you switch to Wireframe -- which reads as "the pool is drawing
		// with the wrong material" rather than "wireframe is unimplemented here".
		// Same blue as the component path, so the two are indistinguishable when
		// voxel.Stream.GPUMaxLevel puts both renderers in one frame.
		const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;
		const FMaterialRenderProxy* BatchMaterialProxy = MaterialProxy;
		if (bWireframe)
		{
			auto* WireframeMaterialInstance = new FColoredMaterialRenderProxy(
				GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : nullptr,
				FLinearColor(0, 0.5f, 1.f));
			Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
			BatchMaterialProxy = WireframeMaterialInstance;
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			FMeshBatch& Mesh = Collector.AllocateMesh();
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = BatchMaterialProxy;
			Mesh.bWireframe = bWireframe;
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastShadow = IsShadowCast(Views[ViewIndex]);

			// ONE element covering EVERY chunk, unless culling is on.
			//
			// One draw over the whole pool is the design, and it is what removes
			// the per-chunk cost from the renderer. What it also removes is
			// per-chunk FRUSTUM CULLING, and that is not free: measured on a
			// settled scene, the pooled path's frame time does not depend on what
			// is on screen at all (18.58 -> 19.05 ms when the camera is pointed at
			// almost nothing) while the per-chunk component path gets 64% cheaper
			// over the same change. A renderer whose cost is invariant to
			// visibility is a renderer that is not culling.
			//
			// So: cull per chunk on the render thread, then draw the surviving
			// pool RANGES. Chunks own contiguous spans, and the pool fills in
			// roughly spatial order, so neighbours in the pool are usually
			// neighbours in the world and the surviving spans merge back down to
			// far fewer draws than there are chunks. This keeps one primitive --
			// nothing here touches FScene -- and trades "one draw" for "one draw
			// per visible run", which is the trade the measurement says is worth
			// making.
			// LOCAL, not a member. GetDynamicMeshElements is called CONCURRENTLY --
			// bSupportsParallelGDME defaults to true, so the renderer gathers the
			// main view and every shadow cascade on parallel tasks, all through this
			// one const method on this one proxy. Holding the cull's working set in
			// mutable members therefore had several views writing the same array at
			// once, and what got drawn was a mixture of their answers: a plausible
			// count of quads, spatially unrelated to any one view's frustum. That is
			// the whole under-selection. It also crashed -- reassigning the array
			// while another thread iterated it asserted in the D3D12 RHI.
			TArray<FQuadRange> Ranges;
			const bool bCull = VoxelGpuPoolCull::IsEnabled() && Runs.Num() > 0 && NumQuads > 0;
			if (bCull)
			{
				BuildCulledRanges(*Views[ViewIndex], Ranges);
			}

			if (!bCull || Ranges.Num() == 0)
			{
				// Not culling, or the cull produced nothing usable -- draw
				// everything, which is always correct and never worse than
				// before this option existed.
				//
				// UserData stays null, so the factory binds its BaseQuad = 0
				// buffer and the shader computes QuadIndex = 0 + VertexId/6 --
				// the identical expression to before BaseQuad existed. This path
				// is unchanged, not merely equivalent, and that is deliberate:
				// it is the control every cull measurement is read against.
				FMeshBatchElement& Element = Mesh.Elements[0];
				Element.IndexBuffer = nullptr;
				Element.FirstIndex = 0;
				// Up to the high-water mark only: everything above it has never
				// been written, so there is nothing there to draw.
				Element.NumPrimitives = uint32(NumQuads) * 2;
				Element.MinVertexIndex = 0;
				Element.MaxVertexIndex = uint32(NumQuads) * 6 - 1;
				Element.NumInstances = 1;
				Element.PrimitiveUniformBuffer = GetUniformBuffer();
			}
			else
			{
				// EVERY RANGE DRAWS FROM VERTEX 0 AND NAMES ITS START EXPLICITLY.
				//
				// The obvious encoding -- FirstIndex = Range.First * 6, and let
				// SV_VertexID carry the offset into the factory's
				// QuadIndex = VertexId/6 -- is what shipped, and it is wrong.
				// SV_VertexID does not include the draw's base vertex on D3D12;
				// RHISupportsAbsoluteVertexID (DataDrivenShaderPlatformInfo.h)
				// returns true only for Vulkan, and FLocalVertexFactory threads
				// the base through a uniform buffer for exactly this reason.
				//
				// Measured with the frustum test bypassed and the pool tiled into
				// N exact contiguous ranges (GPUCullDebugSplit): at N = 2, 8 and
				// 64 only the first 1/N of the pool reached the screen, while the
				// frame cost stayed flat and then rose (12.26 -> 11.86 -> 14.07 ms
				// p50) -- every range drew its full quad count starting at pool
				// quad 0, and N=64 simply added 63 draw calls on top. That is a
				// renderer silently drawing the wrong geometry, which is the
				// failure mode this pool is worst at showing.
				//
				// So: FirstIndex = 0 everywhere, and the start goes through
				// FVoxelQuadRangeParameters. Correct whether or not the platform
				// includes the base vertex, because VertexId now unambiguously
				// runs [0, Count*6) for every range.
				Mesh.Elements.Reset();
				for (const FQuadRange& Range : Ranges)
				{
					// ONE FRAME RESOURCE, not a local and not a proxy member.
					// GetDynamicMeshElements only gathers; the bindings are read
					// later when the draw commands are built, by which time a
					// stack local is gone. The collector owns this until the
					// frame ends.
					FVoxelQuadRangeUserData& RangeData =
						Collector.AllocateOneFrameResource<FVoxelQuadRangeUserData>();
					FVoxelQuadRangeParameters RangeParams;
					RangeParams.BaseQuad = Range.First;
					RangeData.RangeUniformBuffer =
						TUniformBufferRef<FVoxelQuadRangeParameters>::CreateUniformBufferImmediate(
							RangeParams, UniformBuffer_SingleFrame);

					FMeshBatchElement& Element = Mesh.Elements.AddDefaulted_GetRef();
					Element.IndexBuffer = nullptr;
					Element.FirstIndex = 0;
					Element.NumPrimitives = Range.Count * 2;
					Element.MinVertexIndex = 0;
					Element.MaxVertexIndex = Range.Count * 6 - 1;
					Element.NumInstances = 1;
					Element.PrimitiveUniformBuffer = GetUniformBuffer();
					Element.UserData = &RangeData;
				}
			}

			Collector.AddMesh(ViewIndex, Mesh);
		}
	}

	// --- incremental update, render thread -------------------------------
	//
	// Writes only the quads that changed. The alternative -- rebuilding the
	// whole buffer -- is 75 MB per change at cascade scale, which is what makes
	// streaming through a pool viable or not.
	// NewQuads/NewIds hold ONLY the dirty range, indexed from zero; First is
	// where it lands in the GPU buffer.
	// One lock/unlock pair per RUN. NewQuads/NewIds are the flattened payload;
	// each run says where its slice starts in them and where it lands in the
	// pool. Several exact runs, not one span covering the untouched geometry
	// between them -- see UVoxelGpuPoolComponent::DirtyQuadRanges.
	void UpdateQuadRange_RenderThread(FRHICommandListBase& RHICmdList,
	                                  const TArray<uint64>& NewQuads,
	                                  const TArray<uint32>& NewIds,
	                                  const TArray<FVoxelQuadUploadRun>& UploadRuns,
	                                  int32 NewNumQuads)
	{
		if (!QuadBuffer.IsValid() || UploadRuns.Num() == 0)
		{
			NumQuads = NewNumQuads;
			return;
		}

		for (const FVoxelQuadUploadRun& Run : UploadRuns)
		{
			if (Run.Count == 0)
			{
				continue;
			}

			const uint32 QuadSizeBytes = Run.Count * sizeof(uint64);
			if (void* Dst = RHICmdList.LockBuffer(QuadBuffer, Run.First * sizeof(uint64),
			                                      QuadSizeBytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, NewQuads.GetData() + Run.SrcOffset, QuadSizeBytes);
				RHICmdList.UnlockBuffer(QuadBuffer);
			}

			const uint32 IdSizeBytes = Run.Count * sizeof(uint32);
			if (void* Dst = RHICmdList.LockBuffer(ChunkIdBuffer, Run.First * sizeof(uint32),
			                                      IdSizeBytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, NewIds.GetData() + Run.SrcOffset, IdSizeBytes);
				RHICmdList.UnlockBuffer(ChunkIdBuffer);
			}
		}

		NumQuads = NewNumQuads;
	}

	// The chunk table is tiny (one float4 + one float2 per chunk), so it is
	// rewritten wholesale rather than tracked range by range.
	void UpdateChunkTable_RenderThread(FRHICommandListBase& RHICmdList,
	                                   const TArray<FVector4f>& NewOrigins,
	                                   const TArray<FVector4f>& NewParams,
	                                   const TArray<UVoxelGpuPoolComponent::FChunkRun>& NewRuns)
	{
		// Origins and Runs are the cull's two inputs and must not disagree: a run
		// naming a chunk id the table no longer describes would be culled against
		// a stale box. They are updated together, from the same game-thread
		// snapshot, for that reason.
		Origins = NewOrigins;
		// Assigned unconditionally. This is only ever called with a fresh
		// game-thread snapshot of both, so an EMPTY run list means the pool
		// genuinely holds no live chunks -- the previous "keep the old runs if the
		// new list is empty" guard turned that state into stale runs pointing at
		// quads that no longer belong to anyone.
		Runs = NewRuns;

		if (!OriginBuffer.IsValid() || NewOrigins.Num() > MaxChunks)
		{
			return;   // outgrew the table; the caller rebuilds the render state
		}

		const uint32 OriginBytes = uint32(NewOrigins.Num()) * sizeof(FVector4f);
		if (void* Dst = RHICmdList.LockBuffer(OriginBuffer, 0, OriginBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewOrigins.GetData(), OriginBytes);
			RHICmdList.UnlockBuffer(OriginBuffer);
		}

		const uint32 ParamsBytes = uint32(NewParams.Num()) * sizeof(FVector4f);
		if (void* Dst = RHICmdList.LockBuffer(ParamsBuffer, 0, ParamsBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewParams.GetData(), ParamsBytes);
			RHICmdList.UnlockBuffer(ParamsBuffer);
		}
	}

	int32 GetMaxChunks() const { return MaxChunks; }

	// Same expression FVoxelChunkSceneProxy and FWaterChunkSceneProxy use. For
	// an opaque material this is the base class's answer anyway; it is spelled
	// out because the translucent (water) instance is the case where the two
	// answers could diverge.
	bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }

	uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	mutable FVoxelQuadVertexFactory VertexFactory;

	// Which pool this is, for the log lines below. There is more than one
	// instance now and they are diagnosed almost entirely from those lines.
	FString PoolName;

	// The component's buffers, not the proxy's. Held by shared pointer so this
	// proxy dying does not take the GPU-resident geometry with it — see
	// FVoxelGpuPoolBuffers for why that matters.
	FVoxelGpuPoolBuffersRef SharedBuffers;

	TArray<uint64> Quads;
	TArray<uint32> ChunkIds;
	TArray<FVector4f> Origins;
	TArray<FVector4f> Params;
	// Which quads belong to which chunk, sorted by pool offset. The cull's
	// input; see UVoxelGpuPoolComponent::FChunkRun for why the proxy cannot
	// derive this itself.
	TArray<UVoxelGpuPoolComponent::FChunkRun> Runs;
	int32 ChunkEdgeVoxels = 32;

	struct FQuadRange { uint32 First = 0; uint32 Count = 0; };

	// Frustum-test every run, then merge the survivors back into as few pool
	// ranges as possible. Runs arrive sorted by pool offset, so the merge is a
	// single linear pass.
	//
	// RE-ENTRANT BY CONSTRUCTION. Everything this touches is either read-only
	// proxy state or a local -- see the note in GetDynamicMeshElements about
	// bSupportsParallelGDME. The earlier version kept the working arrays as
	// mutable members "reused across frames so the per-frame cull allocates
	// nothing", which is a sound instinct for a method called once per frame and
	// wrong for one called once per frame PER VIEW, in parallel.
	void BuildCulledRanges(const FSceneView& View, TArray<FQuadRange>& CulledRanges) const
	{
		const FMatrix& LocalToWorldMatrix = GetLocalToWorld();
		uint32 VisibleQuads = 0;

		// N EQUAL RANGES OVER THE WHOLE POOL, no frustum test. See GDebugSplit.
		// Capped at the batch-element limit for the same reason GetMaxRanges() is:
		// asking for 256 parts would index a uint64 mask past bit 63.
		const int32 SplitCount = FMath::Min(VoxelGpuPoolCull::GDebugSplit,
		                                    VoxelGpuPoolCull::kMaxBatchElements);
		if (SplitCount > 1 && NumQuads > 0)
		{
			const uint32 Total = uint32(NumQuads);
			const uint32 Parts = uint32(FMath::Min(SplitCount, NumQuads));
			for (uint32 I = 0; I < Parts; ++I)
			{
				// Computed from the boundaries rather than a running total so the
				// parts tile [0, Total) exactly, with no rounding gap or overlap.
				const uint32 Begin = uint32((uint64(Total) * I) / Parts);
				const uint32 End = uint32((uint64(Total) * (I + 1)) / Parts);
				if (End > Begin)
				{
					CulledRanges.Add(FQuadRange{ Begin, End - Begin });
				}
			}
			// Deliberately NOT merged: adjacent ranges have a zero gap, so the
			// merge below would collapse them straight back to one draw and the
			// experiment would test nothing.
			if (CullLogCounter.Increment() % 600 == 1)
			{
				UE_LOG(LogTemp, Log, TEXT("%s cull: DEBUG SPLIT into %d ranges covering %d quads"),
				       *PoolName, CulledRanges.Num(), NumQuads);
			}
			return;
		}

		// WHICH FRUSTUM. GetDynamicMeshElements is called for shadow gathers as
		// well as for the camera (FProjectedShadowInfo::GatherDynamicMeshElements),
		// and there the view handed in is a SNAPSHOT of the main view -- so
		// View.ViewFrustum is still the camera's. Culling shadow casters against
		// the camera means every caster outside the camera frustum stops casting.
		// The renderer's own answer to this is a per-gather cull frustum on the
		// view, which is the shadow's bounds; its planes are expressed in
		// pre-shadow-translated world space, so the translation has to come back
		// out before they can be tested against absolute world boxes. Same
		// adjustment FHierarchicalStaticMeshSceneProxy makes for foliage.
		FConvexVolume ShadowFrustumLocal;
		const FConvexVolume* Frustum = &View.ViewFrustum;
		const bool bShadowGather = View.GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		if (bShadowGather)
		{
			const FConvexVolume& ShadowFrustum = *View.GetDynamicMeshElementsShadowCullFrustum();
			for (const FPlane& Src : ShadowFrustum.Planes)
			{
				FPlane Norm = Src / Src.Size();
				Norm.W -= (FVector(Norm) | View.GetPreShadowTranslation());
				ShadowFrustumLocal.Planes.Add(Norm);
			}
			ShadowFrustumLocal.Init();
			Frustum = &ShadowFrustumLocal;
		}

		// Why each run was dropped. Every one of these paths is a `continue` that
		// looks exactly like a frustum rejection from outside, and one of them
		// silently dropping most of the pool is indistinguishable in the picture
		// from a cull that aims wrongly.
		int32 SkippedEmpty = 0, SkippedBadId = 0, SkippedAboveWatermark = 0, SkippedHidden = 0, SkippedFrustum = 0;
		uint32 RunQuads = 0;

		for (const UVoxelGpuPoolComponent::FChunkRun& Run : Runs)
		{
			RunQuads += Run.NumQuads;
			if (Run.NumQuads == 0 || !Origins.IsValidIndex(int32(Run.ChunkId)))
			{
				++(Run.NumQuads == 0 ? SkippedEmpty : SkippedBadId);
				continue;
			}
			// Ranges above the high-water mark hold nothing that was ever
			// written; drawing them would be reading uninitialised pool.
			if (Run.FirstQuad + Run.NumQuads > uint32(NumQuads))
			{
				++SkippedAboveWatermark;
				continue;
			}

			const FVector4f& Entry = Origins[int32(Run.ChunkId)];
			const float Scale = Entry.W;
			if (Scale <= 0.f)
			{
				++SkippedHidden;
				continue; // hidden entry: collapsed to a point, nothing to draw
			}

			// Same extent AddChunk grows LocalBounds by, so the cull can never be
			// tighter than the bounds the scene already culled the whole
			// primitive against.
			const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Scale;
			const FVector Min(Entry.X, Entry.Y, Entry.Z);
			const FBox LocalBox(Min, Min + FVector(EdgeUU));
			const FBox WorldBox = LocalBox.TransformBy(LocalToWorldMatrix);

			// One-shot: the actual numbers the frustum test is fed, against the
			// view it is tested with. A cull that selects the wrong chunks and a
			// cull that selects none look identical from the outside, and both
			// read as "the pool is broken"; this prints the operands instead of
			// inferring them.
			if (CullSpaceLogged.Increment() == 1)
			{
				UE_LOG(LogTemp, Warning,
				       TEXT("%s cullspace: chunk0 local=(%.0f,%.0f,%.0f) edge=%.0f -> worldCentre=(%.0f,%.0f,%.0f) ext=(%.0f,%.0f,%.0f) | viewOrigin=(%.0f,%.0f,%.0f) | l2wTrans=(%.0f,%.0f,%.0f)"),
				       *PoolName, Entry.X, Entry.Y, Entry.Z, EdgeUU,
				       WorldBox.GetCenter().X, WorldBox.GetCenter().Y, WorldBox.GetCenter().Z,
				       WorldBox.GetExtent().X, WorldBox.GetExtent().Y, WorldBox.GetExtent().Z,
				       View.ViewMatrices.GetViewOrigin().X, View.ViewMatrices.GetViewOrigin().Y,
				       View.ViewMatrices.GetViewOrigin().Z,
				       LocalToWorldMatrix.GetOrigin().X, LocalToWorldMatrix.GetOrigin().Y,
				       LocalToWorldMatrix.GetOrigin().Z);
			}

			const bool bInFrustum = VoxelGpuPoolCull::GDebugAllVisible != 0 ||
			                        Frustum->IntersectBox(WorldBox.GetCenter(), WorldBox.GetExtent());
			const bool bKeep = VoxelGpuPoolCull::GDebugInvert != 0 ? !bInFrustum : bInFrustum;
			if (!bKeep)
			{
				++SkippedFrustum;
				continue;
			}

			VisibleQuads += Run.NumQuads;
			CulledRanges.Add(FQuadRange{ Run.FirstQuad, Run.NumQuads });
		}

		// Inverted: what reaches the screen must be exactly the rejected set, so
		// merging (which only ever ADDS quads) would destroy the experiment.
		if (VoxelGpuPoolCull::GDebugInvert != 0)
		{
			if (CullLogCounter.Increment() % 600 == 1)
			{
				UE_LOG(LogTemp, Log,
				       TEXT("%s cull: DEBUG INVERT drawing the REJECTED set: ranges=%d quads=%u/%d"),
				       *PoolName, CulledRanges.Num(), VisibleQuads, NumQuads);
			}
			return;
		}

		// PASS 1: MERGE ON THE TOLERATED GAP (voxel.Stream.GPUCullMergeGap).
		//
		// This is what that cvar always claimed to do and never did. Its own
		// declaration described the trade exactly -- an off-screen quad costs six
		// vertex invocations and no pixels, a draw costs a state change and a
		// command -- and then nothing read it, because the range-cap merge below
		// was the only merge that existed. A knob that silently ignores you is
		// worse than no knob, so it is wired here rather than deleted: it is the
		// only control over redrawn quads at range counts that are already under
		// the cap, which is the common case.
		//
		// Runs into the cap merge below, which is what still GUARANTEES the
		// element limit; this pass only ever reduces the count it starts from.
		const uint32 MergeGap = uint32(FMath::Max(0, VoxelGpuPoolCull::GMergeGapQuads));
		if (MergeGap > 0 && CulledRanges.Num() > 1)
		{
			TArray<FQuadRange> Merged;
			Merged.Reserve(CulledRanges.Num());
			Merged.Add(CulledRanges[0]);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				FQuadRange& Last = Merged.Last();
				const uint32 LastEnd = Last.First + Last.Count;
				const uint32 Gap = CulledRanges[I].First >= LastEnd ? CulledRanges[I].First - LastEnd : 0u;
				if (Gap <= MergeGap)
				{
					// Max, not +=: runs are sorted by pool offset but a merged
					// range can already extend past the next one's end.
					const uint32 NewEnd = FMath::Max(LastEnd, CulledRanges[I].First + CulledRanges[I].Count);
					Last.Count = NewEnd - Last.First;
				}
				else
				{
					Merged.Add(CulledRanges[I]);
				}
			}
			CulledRanges = MoveTemp(Merged);
		}

		// PASS 2: MERGE TO A DRAW-CALL BUDGET, CHEAPEST GAPS FIRST.
		//
		// The first version merged on a fixed gap threshold and then discarded
		// the whole result if it still exceeded the range cap. That is the wrong
		// shape twice over: the threshold is a magic constant with no relation to
		// what a draw costs, and the discard meant a scene the cull had correctly
		// reduced to 25% visible was drawn in full anyway -- measured
		// visibleQuads=2205034/8808161 with ranges=0, i.e. all the work and none
		// of the benefit.
		//
		// The real objective is: no more than N draws, with as few redrawn
		// off-screen quads as possible. So sort the GAPS between adjacent visible
		// runs, and merge the smallest ones until the range count fits. Merging
		// the smallest gap first is optimal for this objective -- each merge
		// removes exactly one draw and costs exactly that gap in redrawn quads.
		const int32 MaxRanges = VoxelGpuPoolCull::GetMaxRanges();
		if (CulledRanges.Num() > MaxRanges)
		{
			TArray<uint32> Sorted;
			Sorted.Reserve(CulledRanges.Num() - 1);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				const uint32 PrevEnd = CulledRanges[I - 1].First + CulledRanges[I - 1].Count;
				Sorted.Add(CulledRanges[I].First >= PrevEnd ? CulledRanges[I].First - PrevEnd : 0u);
			}
			// The (n - MaxRanges)th smallest gap is the threshold that leaves
			// exactly MaxRanges ranges. Sorting is O(n log n) on a few thousand
			// entries, once per view per frame.
			Sorted.Sort();
			const int32 MergesNeeded = CulledRanges.Num() - MaxRanges;
			const uint32 GapThreshold = Sorted[FMath::Clamp(MergesNeeded - 1, 0, Sorted.Num() - 1)];

			TArray<FQuadRange> Merged;
			Merged.Reserve(MaxRanges);
			Merged.Add(CulledRanges[0]);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				FQuadRange& Last = Merged.Last();
				const uint32 LastEnd = Last.First + Last.Count;
				const uint32 Gap = CulledRanges[I].First >= LastEnd ? CulledRanges[I].First - LastEnd : 0u;
				if (Gap <= GapThreshold)
				{
					const uint32 NewEnd = FMath::Max(LastEnd, CulledRanges[I].First + CulledRanges[I].Count);
					Last.Count = NewEnd - Last.First;
				}
				else
				{
					Merged.Add(CulledRanges[I]);
				}
			}
			CulledRanges = MoveTemp(Merged);
		}

		// Too fragmented to be worth it, or nothing visible at all -- either way
		// the caller falls back to the single full draw. Nothing visible is
		// deliberately NOT treated as "draw nothing": a wrong cull that hides the
		// world is the failure mode this pool is worst at diagnosing, so the
		// conservative branch is the one that draws.
		// Nothing visible -> fall back to the full draw rather than drawing
		// nothing. A wrong cull that hides the world is the failure this pool is
		// worst at diagnosing, so the conservative branch is the one that draws.
		// The range cap no longer needs a bail-out: the merge above satisfies it
		// by construction.
		if (VisibleQuads == 0)
		{
			CulledRanges.Reset();
		}

		// What the merged ranges actually submit, as against VisibleQuads which is
		// the pre-merge total. The gap between the two is the cost of merging --
		// quads redrawn because they sit inside a tolerated gap. Both numbers are
		// needed to tune GPUCullMergeGap; one alone cannot say whether merging is
		// paying for itself.
		uint32 DrawnQuads = 0;
		for (const FQuadRange& R : CulledRanges)
		{
			DrawnQuads += R.Count;
		}

		// Logged PERIODICALLY, not once. The first version logged on the first
		// frame only, which is useless for this diagnostic: at t=0 the pool holds
		// a handful of chunks and the camera may legitimately see none of them,
		// so "visibleQuads=0" reads identically whether the cull is working or
		// rejecting everything. That ambiguity hid a broken cull for a whole
		// verification cycle -- and because the empty case falls back to the full
		// draw, the picture was correct either way and proved nothing.
		if (CullLogCounter.Increment() % 600 == 1)
		{
			UE_LOG(LogTemp, Log,
			       TEXT("%s cull: runs=%d runQuads=%u/%d visibleQuads=%u (%.1f%%) ranges=%d drawnQuads=%u (%.1f%%) "
			            "| mergeGap=%u maxRanges=%d | skipped empty=%d badId=%d aboveHWM=%d hidden=%d frustum=%d "
			            "| shadowGather=%d"),
			       *PoolName, Runs.Num(), RunQuads, NumQuads, VisibleQuads,
			       NumQuads > 0 ? 100.0 * double(VisibleQuads) / double(NumQuads) : 0.0,
			       CulledRanges.Num(), DrawnQuads,
			       NumQuads > 0 ? 100.0 * double(DrawnQuads) / double(NumQuads) : 0.0,
			       MergeGap, MaxRanges,
			       SkippedEmpty, SkippedBadId, SkippedAboveWatermark, SkippedHidden, SkippedFrustum,
			       bShadowGather ? 1 : 0);
		}
	}

	// Concurrent, for the same reason the cull's working set had to stop being a
	// member: several views run this method at once. A racing ++ on a plain int
	// is undefined behaviour and, more practically, would let the periodic log
	// miss or duplicate its slot.
	mutable FThreadSafeCounter CullLogCounter;
	mutable FThreadSafeCounter CullSpaceLogged;



	FBufferRHIRef QuadBuffer, ChunkIdBuffer, OriginBuffer, ParamsBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV, ChunkIdSRV, OriginSRV, ParamsSRV;

	mutable FThreadSafeCounter ElementsLogged;
	int32 NumQuads = 0;      // drawn
	int32 BufferQuads = 0;   // allocated
	int32 NumChunks = 0;
	// Headroom in the chunk table so ordinary streaming churn never has to
	// rebuild the render state just to add one more chunk.
	int32 MaxChunks = 0;

	FMaterialRenderProxy* MaterialProxy = nullptr;
	FMaterialRelevance MaterialRelevance;
};

UVoxelGpuPoolComponent::UVoxelGpuPoolComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CastShadow = true;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UVoxelGpuPoolComponent::InitPool(uint32 CapacityQuads)
{
	Pool.Init(CapacityQuads);
	PooledQuads.SetNumZeroed(int32(CapacityQuads));
	QuadChunkIds.SetNumZeroed(int32(CapacityQuads));

	// Reserve the hidden chunk at index 0. Everything freed points here.
	ChunkOrigins.Reset();
	ChunkOrigins.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
	ChunkParams.Reset();
	// Hidden chunk: neutral climate, and a surface height so far below the chunk
	// that the shader's surface-proximity gate reads "at the surface" -- matching
	// BuildChunkVertexData's own fallback when no world subsystem is available.
	// Nothing is drawn from this entry (scale 0 collapses it), so the values only
	// have to be harmless.
	ChunkParams.Add(FVector4f(0.5f, 0.5f, kNoSurfaceGate, 0.0f));
	FreeChunkIds.Reset();

	Allocations.Reset();
	AllocationChunkIds.Reset();
	NumLiveChunks = 0;
	LocalBounds = FBox(ForceInit);
	DirtyQuadRanges.Reset();
	bChunkTableDirty = false;
	bRunsDirty = false;
}

int32 UVoxelGpuPoolComponent::AddChunk(const TArray<uint64>& InQuads,
                                       const FVector3f& OriginUU, int32 Level,
                                       const FVector4f& Params)
{
	check(Pool.GetCapacityQuads() > 0);   // InitPool first

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		// Out of CONTIGUOUS room, which is not the same as out of space --
		// see FVoxelGpuGeometryPool. The caller decides whether to compact.
		//
		// Counted as well as logged: the caller's only signal is INDEX_NONE, and
		// every caller in the tree treats that as "no geometry for this chunk"
		// and moves on. See GetAllocFailureCount for why a per-occurrence warning
		// is not enough on its own.
		++AllocFailureCount;
		AllocFailureQuads += InQuads.Num();
		// Warn on the first, then at powers of ten: a pool that has genuinely run
		// out fails on nearly every subsequent chunk, and a per-chunk warning at
		// that rate buries the streaming log it would be diagnosed from.
		if (FMath::IsPowerOfTwo(AllocFailureCount) || AllocFailureCount == 1)
		{
			UE_LOG(LogTemp, Warning,
			       TEXT("%s: no room for %d quads (%u free, largest run %u) -- "
			            "GEOMETRY DROPPED, failure %lld of this run (%lld quads total)"),
			       *PoolName, InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun(),
			       (long long)AllocFailureCount, (long long)AllocFailureQuads);
		}
		return INDEX_NONE;
	}

	const float Scale = float(1 << Level);
	// Re-use a table entry a removed chunk gave back before appending a new one.
	// See FreeChunkIds for why the append-only version was a cliff rather than a
	// slow leak on any pool with real churn.
	uint32 ChunkId;
	if (FreeChunkIds.Num() > 0)
	{
		ChunkId = FreeChunkIds.Pop(EAllowShrinking::No);
		ChunkOrigins[int32(ChunkId)] = FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale);
		ChunkParams[int32(ChunkId)] = Params;
	}
	else
	{
		ChunkId = uint32(ChunkOrigins.Num());
		ChunkOrigins.Add(FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale));
		ChunkParams.Add(Params);
	}

	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}

	const int32 Handle = Allocations.Add(Alloc);
	AllocationChunkIds.Add(ChunkId);
	check(AllocationChunkIds.Num() == Allocations.Num());
	++NumLiveChunks;

	// Grow the bounds by this chunk's extent. A chunk's quads never leave its
	// own ChunkEdgeVoxels cube, scaled by the mip level (32 voxels for a terrain
	// render chunk, 8 for a vxc::WaterBrick8).
	const float EdgeUU = float(ChunkEdgeVoxels) * 10.0f * Scale;
	LocalBounds += FBox(
		FVector(OriginUU.X, OriginUU.Y, OriginUU.Z),
		FVector(OriginUU.X + EdgeUU, OriginUU.Y + EdgeUU, OriginUU.Z + EdgeUU));

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	bChunkTableDirty = true;
	bRunsDirty = true;
	PushUpdatesToProxy();
	UpdateBounds();
	return Handle;
}

void UVoxelGpuPoolComponent::RemoveChunk(int32 Handle)
{
	RemoveChunkInternal(Handle, /*bRecycleChunkId=*/true);
}

void UVoxelGpuPoolComponent::RemoveChunkInternal(int32 Handle, bool bRecycleChunkId)
{
	if (!Allocations.IsValidIndex(Handle) || !Allocations[Handle].IsValid())
	{
		return;
	}

	const FVoxelGpuPoolAllocation Alloc = Allocations[Handle];

	// Point the freed quads at the hidden chunk (scale 0) so they collapse to
	// a degenerate point rather than continuing to draw stale geometry.
	for (uint32 I = 0; I < Alloc.NumQuads; ++I)
	{
		QuadChunkIds[int32(Alloc.Offset + I)] = kHiddenChunkId;
	}

	// Only now is the table entry unreferenced by any quad, which is what makes
	// handing it back safe.
	if (bRecycleChunkId && AllocationChunkIds.IsValidIndex(Handle))
	{
		const uint32 ChunkId = AllocationChunkIds[Handle];
		if (ChunkId != kHiddenChunkId)
		{
			// Neutralise the entry as well as freeing it: if anything ever does
			// reach it before it is re-issued, scale 0 collapses it rather than
			// drawing a stale origin.
			ChunkOrigins[int32(ChunkId)] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			FreeChunkIds.Add(ChunkId);
			bChunkTableDirty = true;
		}
	}

	Pool.Free(Alloc);
	Allocations[Handle] = FVoxelGpuPoolAllocation{};
	--NumLiveChunks;

	// The allocation set changed even when the table entry did not (the realloc
	// caller keeps its entry), so the runs are stale either way.
	bRunsDirty = true;

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	PushUpdatesToProxy();
}

void UVoxelGpuPoolComponent::ClearChunks()
{
	if (Pool.GetCapacityQuads() > 0)
	{
		InitPool(Pool.GetCapacityQuads());
	}
	MarkRenderStateDirty();
	UpdateBounds();
}


// How close two dirty runs must be before they are merged into one.
//
// The trade is real in both directions: merging uploads quads nobody wrote,
// while not merging costs an extra LockBuffer/UnlockBuffer per run. The old
// code sat at one extreme (always merge, one run, unbounded waste).
//
// DEFAULT 0 = MERGE ONLY WHAT ACTUALLY TOUCHES. That is the setting that makes
// the path-2 correctness property hold exactly: a merged run can span quads the
// CPU did not write, which for a non-zero gap could include a GPU-written
// range. Raise it only for measured upload-call savings on a CPU-only pool, and
// never above the smallest GPU-written allocation.
//
// This knob is PROVEN LIVE, not assumed: voxel.Stream.PoolUploadStats prints
// the run count and bytes per update, so changing the gap visibly changes both.
// That check exists because this project already shipped
// voxel.Stream.GPUCullMergeGap, which was declared, documented, and never read
// by anything -- a knob that silently ignores you is worse than no knob.
static int32 GVoxelPoolDirtyMergeGap = 0;
static FAutoConsoleVariableRef CVarVoxelPoolDirtyMergeGap(
	TEXT("voxel.Stream.PoolDirtyMergeGap"),
	GVoxelPoolDirtyMergeGap,
	TEXT("Quads of gap tolerated when merging two dirty pool runs into one upload. ")
	TEXT("0 (default) merges only touching/overlapping runs. Higher trades wasted upload ")
	TEXT("bytes for fewer buffer locks. Verify with voxel.Stream.PoolUploadStats 1."),
	ECVF_Default);

// Prints what each incremental upload actually cost. A mechanism count, not a
// frame time -- it is not vulnerable to contention or to the clamp, so it is
// safe to quote from a shared box.
static int32 GVoxelPoolUploadStats = 0;
static FAutoConsoleVariableRef CVarVoxelPoolUploadStats(
	TEXT("voxel.Stream.PoolUploadStats"),
	GVoxelPoolUploadStats,
	TEXT("1 = log runs and bytes uploaded per incremental pool update. This is how the ")
	TEXT("dirty-run merge gap is shown to be live rather than decorative."),
	ECVF_Default);

void UVoxelGpuPoolComponent::MarkQuadsDirty(uint32 First, uint32 Count)
{
	if (Count == 0)
	{
		return;
	}
	const uint32 Last = First + Count - 1;

	// Insert in ascending First order, then sweep once and coalesce. The list
	// holds one entry per chunk touched since the last upload, which the apply
	// budget already bounds to a handful per frame, so linear is right here and
	// an interval tree would be strictly more code for no measurable gain.
	int32 Index = 0;
	while (Index < DirtyQuadRanges.Num() && DirtyQuadRanges[Index].First < First)
	{
		++Index;
	}
	DirtyQuadRanges.Insert(FDirtyRange{ First, Last, true }, Index);

	const uint64 Gap = uint64(FMath::Max(0, GVoxelPoolDirtyMergeGap));
	for (int32 I = 0; I + 1 < DirtyQuadRanges.Num(); )
	{
		FDirtyRange& A = DirtyQuadRanges[I];
		const FDirtyRange& B = DirtyQuadRanges[I + 1];
		// Touching, overlapping, or within the gap. +1 makes [0,3] and [4,7]
		// adjacent rather than separate at Gap 0.
		if (uint64(B.First) <= uint64(A.Last) + 1ull + Gap)
		{
			A.Last = FMath::Max(A.Last, B.Last);
			DirtyQuadRanges.RemoveAt(I + 1, EAllowShrinking::No);
		}
		else
		{
			++I;
		}
	}
}

void UVoxelGpuPoolComponent::PushUpdatesToProxy()
{
	// No live proxy yet: the CPU arrays are the source of truth and the proxy
	// will pick them up whole when it is created.
	if (LiveProxy == nullptr)
	{
		MarkRenderStateDirty();
		DirtyQuadRanges.Reset();
		bChunkTableDirty = false;
		bRunsDirty = false;
		return;
	}

	// The chunk table outgrew its headroom -- rebuild rather than overrun it.
	if (ChunkOrigins.Num() > LiveProxy->GetMaxChunks())
	{
		MarkRenderStateDirty();
		DirtyQuadRanges.Reset();
		bChunkTableDirty = false;
		bRunsDirty = false;
		return;
	}

	// Flatten the dirty runs into one staging payload. Several small runs beat
	// one span covering all the untouched geometry between them -- see
	// DirtyQuadRanges' comment for why that is both a correctness and a
	// bandwidth argument.
	TArray<FVoxelQuadUploadRun> UploadRuns;
	uint32 TotalQuadsToUpload = 0;
	for (const FDirtyRange& R : DirtyQuadRanges)
	{
		const uint32 RunCount = R.Last - R.First + 1;
		UploadRuns.Add(FVoxelQuadUploadRun{ R.First, RunCount, TotalQuadsToUpload });
		TotalQuadsToUpload += RunCount;
	}
	const int32 NewNumQuads = int32(Pool.GetHighWaterMark());
	// The runs travel on the table update, so a run-only change has to send the
	// table too. It is two small vectors per chunk; the quad buffer, which is the
	// expensive one, is still written by range.
	const bool bTableDirty = bChunkTableDirty || bRunsDirty;

	FVoxelGpuPoolSceneProxy* Proxy = LiveProxy;

	// Copy ONLY the dirty slice, never the whole pool.
	//
	// This used to hand the render command a copy of the entire PooledQuads and
	// QuadChunkIds arrays -- at cascade capacity that is ~170 MB of memcpy per
	// chunk added. It did not corrupt anything, which is why it survived the
	// small-scale tests; it simply ate the streaming apply budget alive. The
	// near ring stalled at 178 of ~1900 chunks while every coarser ring filled
	// normally, and the visible symptom was the coarse rings' exposed interiors
	// where the missing fine terrain should have been.
	//
	// Copying a full buffer per incremental update is precisely the cost
	// ADR-0006 exists to remove, so getting it wrong here forfeits the win the
	// pool was built for.
	TArray<uint64> QuadsSlice;
	TArray<uint32> IdsSlice;
	QuadsSlice.Reserve(int32(TotalQuadsToUpload));
	IdsSlice.Reserve(int32(TotalQuadsToUpload));
	for (const FVoxelQuadUploadRun& Run : UploadRuns)
	{
		QuadsSlice.Append(PooledQuads.GetData() + Run.First, int32(Run.Count));
		IdsSlice.Append(QuadChunkIds.GetData() + Run.First, int32(Run.Count));
	}

	if (GVoxelPoolUploadStats != 0 && UploadRuns.Num() > 0)
	{
		// Span-equivalent cost, for comparison: what the single-span version
		// would have uploaded for exactly this set of dirty chunks.
		const uint32 SpanFirst = UploadRuns[0].First;
		const uint32 SpanLast = UploadRuns.Last().First + UploadRuns.Last().Count - 1;
		const uint32 SpanQuads = SpanLast - SpanFirst + 1;
		UE_LOG(LogTemp, Log,
		       TEXT("%s upload: %d run(s), %u quads (%u KB) — a single span would have been "
		            "%u quads (%u KB), gap=%d"),
		       *PoolName, UploadRuns.Num(), TotalQuadsToUpload,
		       (TotalQuadsToUpload * uint32(sizeof(uint64))) / 1024u,
		       SpanQuads, (SpanQuads * uint32(sizeof(uint64))) / 1024u, GVoxelPoolDirtyMergeGap);
	}
	// The chunk table is two small vectors per chunk, so it stays whole.
	TArray<FVector4f> OriginsCopy = ChunkOrigins;
	TArray<FVector4f> ParamsCopy = ChunkParams;
	// Runs travel with the table because they describe the same thing: which
	// chunk owns which quads. Rebuilt whenever the ALLOCATION set changed, not
	// whenever the table did -- see bRunsDirty. Still not per frame: residency
	// changes a handful of times a second, the camera moves every frame, and only
	// the second of those needs re-culling.
	TArray<FChunkRun> RunsCopy = bTableDirty ? BuildChunkRuns() : TArray<FChunkRun>();

	ENQUEUE_RENDER_COMMAND(VoxelGpuPoolIncrementalUpdate)(
		[Proxy, QuadsSlice = MoveTemp(QuadsSlice), IdsSlice = MoveTemp(IdsSlice),
		 OriginsCopy = MoveTemp(OriginsCopy), ParamsCopy = MoveTemp(ParamsCopy), RunsCopy = MoveTemp(RunsCopy),
		 UploadRuns = MoveTemp(UploadRuns), NewNumQuads, bTableDirty](FRHICommandListImmediate& RHICmdList)
	{
		if (bTableDirty)
		{
			Proxy->UpdateChunkTable_RenderThread(RHICmdList, OriginsCopy, ParamsCopy, RunsCopy);
		}
		Proxy->UpdateQuadRange_RenderThread(RHICmdList, QuadsSlice, IdsSlice,
		                                    UploadRuns, NewNumQuads);
	});

	DirtyQuadRanges.Reset();
	bChunkTableDirty = false;
	bRunsDirty = false;

	// Tell the RENDERER the pool grew, not just the component.
	//
	// UpdateBounds() only recomputes this component's cached bounds; the scene
	// keeps whatever bounds it was given when the proxy was created. On the
	// incremental path the proxy is created once and never rebuilt, so without
	// this the renderer culls the pool against the bounds of however few chunks
	// happened to be resident on the first frame -- measured as a 6.4 m box at
	// the player's feet while 2.4 million quads sat in the buffer, drawing
	// nothing because the primitive was frustum-culled almost every frame.
	//
	// MarkRenderTransformDirty, not MarkRenderStateDirty: this pushes bounds
	// and transform to the existing proxy, where the latter would throw the
	// proxy away and rebuild every buffer -- the exact cost the incremental
	// path exists to avoid.
	UpdateBounds();
	MarkRenderTransformDirty();
}

void UVoxelGpuPoolComponent::DestroyRenderState_Concurrent()
{
	LiveProxy = nullptr;
	Super::DestroyRenderState_Concurrent();
}

int32 UVoxelGpuPoolComponent::UpdateChunk(int32 Handle, const TArray<uint64>& InQuads)
{
	if (!Allocations.IsValidIndex(Handle) || !Allocations[Handle].IsValid())
	{
		return INDEX_NONE;
	}

	const FVoxelGpuPoolAllocation Existing = Allocations[Handle];

	// Fits the slot it already has: rewrite in place. This is the case that
	// matters -- an actively dug chunk re-meshes constantly and its quad count
	// barely moves, so free+realloc would churn the allocator for nothing.
	if (uint32(InQuads.Num()) <= Existing.NumQuads)
	{
		const uint32 ChunkId = AllocationChunkIds[Handle];
		for (int32 I = 0; I < InQuads.Num(); ++I)
		{
			PooledQuads[int32(Existing.Offset) + I] = InQuads[I];
			QuadChunkIds[int32(Existing.Offset) + I] = ChunkId;
		}
		// Any tail the chunk no longer needs is hidden rather than left drawing
		// its previous contents.
		for (uint32 I = uint32(InQuads.Num()); I < Existing.NumQuads; ++I)
		{
			QuadChunkIds[int32(Existing.Offset + I)] = kHiddenChunkId;
		}
		MarkQuadsDirty(Existing.Offset, Existing.NumQuads);
		PushUpdatesToProxy();
		return Handle;
	}

	// Outgrew its slot. Reallocate, reusing the chunk's existing table entry so
	// the table does not grow on every edit -- which is why this removal passes
	// bRecycleChunkId=false: the entry is not free, it is about to be re-pointed
	// at the same chunk's new range. Handing it to FreeChunkIds here and then
	// writing quads that reference it would let a LATER AddChunk issue the same
	// id to a different chunk, and two chunks sharing a table entry means one of
	// them silently draws at the other's origin.
	const uint32 ChunkId = AllocationChunkIds[Handle];

	RemoveChunkInternal(Handle, /*bRecycleChunkId=*/false);

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		// The chunk is gone and its entry is now referenced by nothing, so give
		// it back rather than stranding it.
		if (ChunkId != kHiddenChunkId)
		{
			ChunkOrigins[int32(ChunkId)] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			FreeChunkIds.Add(ChunkId);
			bChunkTableDirty = true;
		}
		// Counted with the AddChunk failures, and this is the WORSE of the two:
		// RemoveChunkInternal has already run, so a chunk that was on screen a
		// moment ago is now gone. On a full pool an ordinary re-mesh therefore
		// DELETES existing terrain rather than merely failing to add new terrain,
		// and it does so with no other trace. This path had no log at all.
		++AllocFailureCount;
		AllocFailureQuads += InQuads.Num();
		UE_LOG(LogTemp, Warning,
		       TEXT("%s: re-mesh outgrew its slot and the pool is full (%d quads wanted, %u free, "
		            "largest run %u) -- RESIDENT GEOMETRY DROPPED, failure %lld of this run"),
		       *PoolName, InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun(),
		       (long long)AllocFailureCount);
		return INDEX_NONE;
	}
	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}
	Allocations[Handle] = Alloc;
	AllocationChunkIds[Handle] = ChunkId;
	++NumLiveChunks;

	// The chunk moved. Its table entry is unchanged -- that is the whole point of
	// reusing it -- but the run that names its quads is now wrong, and a stale run
	// does not merely fail to draw this chunk: it points the cull at somebody
	// else's quads.
	bRunsDirty = true;

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	PushUpdatesToProxy();
	return Handle;
}

TArray<UVoxelGpuPoolComponent::FChunkRun> UVoxelGpuPoolComponent::BuildChunkRuns() const
{
	// One entry per LIVE allocation. A freed handle has NumQuads == 0 and its
	// quads already point at the hidden entry, so skipping it here is the same
	// statement the renderer already makes about it -- there is nothing there.
	TArray<FChunkRun> Runs;
	Runs.Reserve(Allocations.Num());
	for (int32 Handle = 0; Handle < Allocations.Num(); ++Handle)
	{
		const FVoxelGpuPoolAllocation& Alloc = Allocations[Handle];
		if (Alloc.NumQuads == 0 || !AllocationChunkIds.IsValidIndex(Handle))
		{
			continue;
		}
		const uint32 ChunkId = AllocationChunkIds[Handle];
		if (ChunkId == kHiddenChunkId)
		{
			continue;
		}
		Runs.Add(FChunkRun{ ChunkId, Alloc.Offset, Alloc.NumQuads });
	}
	// Sorted by pool offset so the proxy can merge neighbours without sorting
	// every frame -- the merge is the difference between one draw per visible
	// chunk and one draw per visible RUN of chunks, and streaming fills the pool
	// in roughly spatial order, so neighbours in the pool are usually neighbours
	// in the world.
	Runs.Sort([](const FChunkRun& A, const FChunkRun& B) { return A.FirstQuad < B.FirstQuad; });
	return Runs;
}

void UVoxelGpuPoolComponent::SetChunkMaterial(UMaterialInterface* InMaterial)
{
	ChunkMaterial = InMaterial;
	MarkRenderStateDirty();
}

UMaterialInterface* UVoxelGpuPoolComponent::GetChunkMaterialOrDefault() const
{
	return ChunkMaterial ? ChunkMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
}

void UVoxelGpuPoolComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
                                              bool bGetDebugMaterials) const
{
	OutMaterials.Add(GetChunkMaterialOrDefault());
}

// ---------------------------------------------------------------------------
// voxel.Stream.PoolClobberTest — the experiment that can actually fail
//
// The clobber this wave fixes is invisible in the happy path: with no GPU
// writer yet, the CPU shadow and the GPU buffer hold identical bytes, so a
// re-upload and a rebind are indistinguishable. A test that merely forces
// proxy recreation and observes "nothing broke" proves nothing at all.
//
// So this makes them distinguishable the only honest way: it DELIBERATELY
// CORRUPTS the CPU shadow, then forces recreation.
//
//   old behaviour (re-upload) -> the corruption reaches the GPU, and the
//                                terrain visibly loses the clobbered quads
//   new behaviour (rebind)    -> the buffer is never rewritten, the corruption
//                                stays on the CPU, and the terrain is unchanged
//
// This is the pool's equivalent of GPUCullDebugSplit: the two hypotheses
// predict visibly different pictures, so the run can come back either way.
//
// DESTRUCTIVE AND DEBUG-ONLY. It leaves the CPU shadow wrong on purpose, so the
// session it runs in is spent — any later edit to a clobbered chunk will write
// from the corrupted shadow. Never leave this in a measurement run.
static void VoxelGpuPoolClobberTest(const TArray<FString>& Args)
{
	const int32 NumToClobber = (Args.Num() > 0) ? FMath::Max(1, FCString::Atoi(*Args[0])) : 200000;

	int32 Found = 0;
	for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
	{
		UVoxelGpuPoolComponent* Comp = *It;
		if (Comp == nullptr || Comp->IsTemplate() || !IsValid(Comp))
		{
			continue;
		}
		++Found;
		Comp->DebugClobberShadowAndRecreate(NumToClobber);
	}

	if (Found == 0)
	{
		UE_LOG(LogTemp, Error,
		       TEXT("voxel.Stream.PoolClobberTest: no live pool component — nothing to test. ")
		       TEXT("Run this after the world has streamed, with voxel.Stream.GPU on."));
	}
}

static FAutoConsoleCommand GVoxelGpuPoolClobberTestCmd(
	TEXT("voxel.Stream.PoolClobberTest"),
	TEXT("DESTRUCTIVE DEBUG. Corrupts the first N quads of the CPU shadow, then forces proxy "
	     "recreation. If the pool buffers are persistent (Wave D / D1) the terrain is UNCHANGED, "
	     "because the corruption never reaches the GPU. If they are re-uploaded, the terrain "
	     "visibly loses those quads. Usage: voxel.Stream.PoolClobberTest [N=200000]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&VoxelGpuPoolClobberTest));

void UVoxelGpuPoolComponent::DebugClobberShadowAndRecreate(int32 NumQuadsToClobber)
{
	const int32 Drawn = int32(Pool.GetHighWaterMark());
	if (Drawn == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s clobber test: pool is empty, nothing to prove"), *PoolName);
		return;
	}

	const int32 N = FMath::Min(NumQuadsToClobber, Drawn);

	// Point them at the hidden chunk (scale 0), which is the pool's own
	// "collapse to a point" encoding — so if this DOES reach the GPU the result
	// is unambiguous missing geometry rather than random garbage that might be
	// mistaken for a shader bug.
	for (int32 I = 0; I < N; ++I)
	{
		QuadChunkIds[I] = kHiddenChunkId;
		PooledQuads[I] = 0;
	}

	UE_LOG(LogTemp, Warning,
	       TEXT("%s clobber test: corrupted the first %d of %d drawn quads in the CPU shadow, "
	            "now forcing proxy recreation. EXPECT: terrain UNCHANGED (buffers rebound). "
	            "A visible hole means the re-upload clobber is still live."),
	       *PoolName, N, Drawn);

	MarkRenderStateDirty();
}

// Lazily allocates the (empty) shared holder. The BUFFERS inside it are created
// on the render thread by the first proxy that runs CreateRenderThreadResources;
// this only guarantees there is somewhere for them to live that outlives any one
// proxy.
FVoxelGpuPoolBuffersRef UVoxelGpuPoolComponent::GetOrCreatePoolBuffers()
{
	if (!PoolBuffers.IsValid())
	{
		PoolBuffers = MakeShared<FVoxelGpuPoolBuffers, ESPMode::ThreadSafe>();
	}
	return PoolBuffers;
}

void UVoxelGpuPoolComponent::BeginDestroy()
{
	// Drop our reference ON THE RENDER THREAD.
	//
	// These are RHI resources whose views were handed to a vertex factory, and
	// the render thread may still be a frame or two behind. Moving the last
	// reference into a render command means the actual release happens in
	// render-thread order behind everything that could still be reading them,
	// rather than wherever the garbage collector happens to run. Getting this
	// wrong does not fail as a compile error or even a visible glitch — it fails
	// as a crash on exit, which is why it is explicit rather than left to the
	// shared pointer's destructor.
	if (PoolBuffers.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VoxelGpuPoolReleaseBuffers)(
			[Buffers = MoveTemp(PoolBuffers)](FRHICommandListImmediate&) mutable
		{
			Buffers.Reset();
		});
		PoolBuffers.Reset();
	}

	Super::BeginDestroy();
}

FPrimitiveSceneProxy* UVoxelGpuPoolComponent::CreateSceneProxy()
{
	if (Pool.GetHighWaterMark() == 0)
	{
		return nullptr;
	}
	// The whole capacity is uploaded, not just the used prefix: incremental
	// writes address absolute pool offsets, so the buffer has to be that big
	// from the start. Only [0, HighWaterMark) is ever drawn.
	TArray<uint64> UsedQuads = PooledQuads;
	TArray<uint32> UsedIds = QuadChunkIds;

	// What is actually about to be drawn, in terms the eye cannot check. A
	// pooled draw has no per-chunk state, so when it renders wrong the only
	// way to tell "the CPU tables are bad" from "the shader reads them wrong"
	// is to print the tables.
	{
		const int32 Drawn = int32(Pool.GetHighWaterMark());
		int32 HiddenQuads = 0, OutOfRangeQuads = 0;
		uint32 MaxIdSeen = 0;
		for (int32 I = 0; I < Drawn; ++I)
		{
			const uint32 Id = UsedIds[I];
			HiddenQuads += (Id == kHiddenChunkId) ? 1 : 0;
			OutOfRangeQuads += (Id >= uint32(ChunkOrigins.Num())) ? 1 : 0;
			MaxIdSeen = FMath::Max(MaxIdSeen, Id);
		}
		UE_LOG(LogTemp, Log,
		       TEXT("%s upload: drawn=%d hidden=%d outOfRange=%d maxId=%u "
		            "tableEntries=%d (%d free) hiddenEntry=(%.1f,%.1f,%.1f,scale=%.3f)"),
		       *PoolName, Drawn, HiddenQuads, OutOfRangeQuads, MaxIdSeen, ChunkOrigins.Num(), FreeChunkIds.Num(),
		       ChunkOrigins[0].X, ChunkOrigins[0].Y, ChunkOrigins[0].Z, ChunkOrigins[0].W);

		// Where the geometry actually is, versus where the renderer will look
		// for it. A pooled draw that renders nothing is nearly always one of
		// these two disagreeing.
		const FBoxSphereBounds WorldBounds = CalcBounds(GetComponentTransform());
		UE_LOG(LogTemp, Log,
		       TEXT("%s placement: comp@(%.0f,%.0f,%.0f) firstChunk=(%.0f,%.0f,%.0f,scale=%.1f) "
		            "boundsOrigin=(%.0f,%.0f,%.0f) boundsExtent=(%.0f,%.0f,%.0f)"),
		       *PoolName, GetComponentLocation().X, GetComponentLocation().Y, GetComponentLocation().Z,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].X : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Y : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Z : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].W : 0.f,
		       WorldBounds.Origin.X, WorldBounds.Origin.Y, WorldBounds.Origin.Z,
		       WorldBounds.BoxExtent.X, WorldBounds.BoxExtent.Y, WorldBounds.BoxExtent.Z);
	}

	FVoxelGpuPoolSceneProxy* Proxy =
		new FVoxelGpuPoolSceneProxy(this, UsedQuads, UsedIds, ChunkOrigins, ChunkParams, BuildChunkRuns(),
		                            PoolName, ChunkTableCapacity, GetOrCreatePoolBuffers());
	LiveProxy = Proxy;
	return Proxy;
}

FBoxSphereBounds UVoxelGpuPoolComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBounds.IsValid)
	{
		return FBoxSphereBounds(FBox(FVector::ZeroVector, FVector(1.0))).TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}

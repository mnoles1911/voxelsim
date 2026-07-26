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
	static int32 GEnabled = 0;
	static FAutoConsoleVariableRef CVarEnabled(
		TEXT("voxel.Stream.GPUCull"), GEnabled,
		TEXT("Frustum-cull the GPU pool per chunk and draw only the surviving pool ranges. 1 = on (default). ")
		TEXT("0 = one draw over the whole pool, which is the pre-cull behaviour and pays for every resident ")
		TEXT("quad regardless of where the camera looks."),
		ECVF_RenderThreadSafe);

	// Merging tolerance, in quads. Two surviving ranges separated by a gap
	// smaller than this are drawn as one, which re-draws the gap. That is a good
	// trade well past the point it looks wasteful: a quad costs six vertex
	// invocations and no pixels if it is off-screen, while a draw costs a state
	// change and a command. Tuned by measurement, not taste -- raise it if the
	// range count is high, lower it if the drawn-quad count is.
	static int32 GMergeGapQuads = 4096;
	static FAutoConsoleVariableRef CVarMergeGap(
		TEXT("voxel.Stream.GPUCullMergeGap"), GMergeGapQuads,
		TEXT("Quads. Surviving pool ranges closer together than this are merged into one draw, re-drawing ")
		TEXT("the gap between them. Trades redrawn off-screen quads against draw count."),
		ECVF_RenderThreadSafe);

	// Above this, stop merging and just draw the whole pool. A view that can see
	// nearly everything gains nothing from culling and would pay for hundreds of
	// draws to prove it.
	static int32 GMaxRanges = 256;
	static FAutoConsoleVariableRef CVarMaxRanges(
		TEXT("voxel.Stream.GPUCullMaxRanges"), GMaxRanges,
		TEXT("If the cull cannot get below this many draw ranges, fall back to one draw over the whole pool."),
		ECVF_RenderThreadSafe);

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
	                        int32 InChunkTableCapacity)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, PoolName(InPoolName)
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
		QuadBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.Quads"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(Quads));
		QuadBufferSRV = RHICmdList.CreateShaderResourceView(
			QuadBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(QuadBuffer));

		ChunkIdBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkIds"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(ChunkIds));
		ChunkIdSRV = RHICmdList.CreateShaderResourceView(
			ChunkIdBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ChunkIdBuffer));

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
		       TEXT("%s: %d chunks, %d quads, %d triangles — ONE primitive, ONE draw"),
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
			if (!bLoggedElements)
			{
				bLoggedElements = true;
				UE_LOG(LogTemp, Warning,
				       TEXT("%s draw SKIPPED: numQuads=%d srv=%d materialProxy=%d"),
				       *PoolName, NumQuads, QuadBufferSRV.IsValid() ? 1 : 0, MaterialProxy != nullptr ? 1 : 0);
			}
			return;
		}

		if (!bLoggedElements)
		{
			bLoggedElements = true;
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
			const bool bCull = VoxelGpuPoolCull::IsEnabled() && Runs.Num() > 0 && NumQuads > 0;
			CulledRanges.Reset();
			if (bCull)
			{
				BuildCulledRanges(*Views[ViewIndex]);
			}

			if (!bCull || CulledRanges.Num() == 0)
			{
				// Not culling, or the cull produced nothing usable -- draw
				// everything, which is always correct and never worse than
				// before this option existed.
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
				// SV_VertexID includes the draw's start vertex for a non-indexed
				// draw, and the vertex factory derives its quad index straight
				// from it (QuadIndex = VertexId / 6). So a range starting at quad
				// F is expressed as FirstIndex = F*6 and the factory addresses the
				// pool correctly with no extra state. That equivalence is the
				// reason this works at all, and it is worth checking with a
				// screenshot rather than trusting: getting it wrong draws the
				// wrong geometry rather than none.
				Mesh.Elements.Reset();
				for (const FQuadRange& Range : CulledRanges)
				{
					FMeshBatchElement& Element = Mesh.Elements.AddDefaulted_GetRef();
					Element.IndexBuffer = nullptr;
					Element.FirstIndex = Range.First * 6;
					Element.NumPrimitives = Range.Count * 2;
					Element.MinVertexIndex = Range.First * 6;
					Element.MaxVertexIndex = (Range.First + Range.Count) * 6 - 1;
					Element.NumInstances = 1;
					Element.PrimitiveUniformBuffer = GetUniformBuffer();
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
	void UpdateQuadRange_RenderThread(FRHICommandListBase& RHICmdList,
	                                  const TArray<uint64>& NewQuads,
	                                  const TArray<uint32>& NewIds,
	                                  uint32 First, uint32 Count, int32 NewNumQuads)
	{
		if (!QuadBuffer.IsValid() || Count == 0)
		{
			NumQuads = NewNumQuads;
			return;
		}

		const uint32 QuadOffsetBytes = First * sizeof(uint64);
		const uint32 QuadSizeBytes = Count * sizeof(uint64);
		if (void* Dst = RHICmdList.LockBuffer(QuadBuffer, QuadOffsetBytes, QuadSizeBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewQuads.GetData(), QuadSizeBytes);
			RHICmdList.UnlockBuffer(QuadBuffer);
		}

		const uint32 IdOffsetBytes = First * sizeof(uint32);
		const uint32 IdSizeBytes = Count * sizeof(uint32);
		if (void* Dst = RHICmdList.LockBuffer(ChunkIdBuffer, IdOffsetBytes, IdSizeBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewIds.GetData(), IdSizeBytes);
			RHICmdList.UnlockBuffer(ChunkIdBuffer);
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
		if (NewRuns.Num() > 0)
		{
			Runs = NewRuns;
		}

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
	// Rebuilt every frame per view. mutable because GetDynamicMeshElements is
	// const; the renderer calls it once per view per frame on the render thread.
	mutable TArray<FQuadRange> CulledRanges;

	// Frustum-test every run, then merge the survivors back into as few pool
	// ranges as possible. Runs arrive sorted by pool offset, so the merge is a
	// single linear pass.
	void BuildCulledRanges(const FSceneView& View) const
	{
		const FMatrix& LocalToWorldMatrix = GetLocalToWorld();
		uint32 VisibleQuads = 0;

		for (const UVoxelGpuPoolComponent::FChunkRun& Run : Runs)
		{
			if (Run.NumQuads == 0 || !Origins.IsValidIndex(int32(Run.ChunkId)))
			{
				continue;
			}
			// Ranges above the high-water mark hold nothing that was ever
			// written; drawing them would be reading uninitialised pool.
			if (Run.FirstQuad + Run.NumQuads > uint32(NumQuads))
			{
				continue;
			}

			const FVector4f& Entry = Origins[int32(Run.ChunkId)];
			const float Scale = Entry.W;
			if (Scale <= 0.f)
			{
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
			if (!bLoggedCullSpace)
			{
				bLoggedCullSpace = true;
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

			if (VoxelGpuPoolCull::GDebugAllVisible == 0 &&
			    !View.ViewFrustum.IntersectBox(WorldBox.GetCenter(), WorldBox.GetExtent()))
			{
				continue;
			}

			VisibleQuads += Run.NumQuads;
			CulledRanges.Add(FQuadRange{ Run.FirstQuad, Run.NumQuads });
		}

		// MERGE TO A DRAW-CALL BUDGET, CHEAPEST GAPS FIRST.
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
		const int32 MaxRanges = FMath::Max(1, VoxelGpuPoolCull::GMaxRanges);
		if (CulledRanges.Num() > MaxRanges)
		{
			GapScratch.Reset();
			GapScratch.Reserve(CulledRanges.Num() - 1);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				const uint32 PrevEnd = CulledRanges[I - 1].First + CulledRanges[I - 1].Count;
				GapScratch.Add(CulledRanges[I].First >= PrevEnd ? CulledRanges[I].First - PrevEnd : 0u);
			}
			// The (n - MaxRanges)th smallest gap is the threshold that leaves
			// exactly MaxRanges ranges. Sorting is O(n log n) on a few thousand
			// entries, once per view per frame.
			TArray<uint32> Sorted = GapScratch;
			Sorted.Sort();
			const int32 MergesNeeded = CulledRanges.Num() - MaxRanges;
			const uint32 GapThreshold = Sorted[FMath::Clamp(MergesNeeded - 1, 0, Sorted.Num() - 1)];

			MergeScratch.Reset();
			MergeScratch.Add(CulledRanges[0]);
			for (int32 I = 1; I < CulledRanges.Num(); ++I)
			{
				FQuadRange& Last = MergeScratch.Last();
				const uint32 LastEnd = Last.First + Last.Count;
				const uint32 Gap = CulledRanges[I].First >= LastEnd ? CulledRanges[I].First - LastEnd : 0u;
				if (Gap <= GapThreshold)
				{
					Last.Count = (CulledRanges[I].First + CulledRanges[I].Count) - Last.First;
				}
				else
				{
					MergeScratch.Add(CulledRanges[I]);
				}
			}
			CulledRanges = MergeScratch;
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
		if (++CullLogCounter % 600 == 1)
		{
			UE_LOG(LogTemp, Log,
			       TEXT("%s cull: runs=%d visibleQuads=%u/%d (%.1f%%) ranges=%d drawnQuads=%u (merge gap %d)"),
			       *PoolName, Runs.Num(), VisibleQuads, NumQuads,
			       NumQuads > 0 ? 100.0 * double(VisibleQuads) / double(NumQuads) : 0.0,
			       CulledRanges.Num(), DrawnQuads, VoxelGpuPoolCull::GMergeGapQuads);
		}
	}

	mutable uint32 CullLogCounter = 0;
	mutable bool bLoggedCullSpace = false;
	// Reused across frames so the per-frame cull allocates nothing.
	mutable TArray<uint32> GapScratch;
	mutable TArray<FQuadRange> MergeScratch;



	FBufferRHIRef QuadBuffer, ChunkIdBuffer, OriginBuffer, ParamsBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV, ChunkIdSRV, OriginSRV, ParamsSRV;

	mutable bool bLoggedElements = false;
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
		UE_LOG(LogTemp, Warning,
		       TEXT("%s: no room for %d quads (%u free, largest run %u)"),
		       *PoolName, InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun());
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


void UVoxelGpuPoolComponent::MarkQuadsDirty(uint32 First, uint32 Count)
{
	if (Count == 0)
	{
		return;
	}
	const uint32 Last = First + Count - 1;
	if (!DirtyQuads.bValid)
	{
		DirtyQuads = { First, Last, true };
	}
	else
	{
		DirtyQuads.First = FMath::Min(DirtyQuads.First, First);
		DirtyQuads.Last = FMath::Max(DirtyQuads.Last, Last);
	}
}

void UVoxelGpuPoolComponent::PushUpdatesToProxy()
{
	// No live proxy yet: the CPU arrays are the source of truth and the proxy
	// will pick them up whole when it is created.
	if (LiveProxy == nullptr)
	{
		MarkRenderStateDirty();
		DirtyQuads = {};
		bChunkTableDirty = false;
		return;
	}

	// The chunk table outgrew its headroom -- rebuild rather than overrun it.
	if (ChunkOrigins.Num() > LiveProxy->GetMaxChunks())
	{
		MarkRenderStateDirty();
		DirtyQuads = {};
		bChunkTableDirty = false;
		return;
	}

	const uint32 First = DirtyQuads.bValid ? DirtyQuads.First : 0;
	const uint32 Count = DirtyQuads.bValid ? (DirtyQuads.Last - DirtyQuads.First + 1) : 0;
	const int32 NewNumQuads = int32(Pool.GetHighWaterMark());
	const bool bTableDirty = bChunkTableDirty;

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
	if (Count > 0)
	{
		QuadsSlice.Append(PooledQuads.GetData() + First, int32(Count));
		IdsSlice.Append(QuadChunkIds.GetData() + First, int32(Count));
	}
	// The chunk table is two small vectors per chunk, so it stays whole.
	TArray<FVector4f> OriginsCopy = ChunkOrigins;
	TArray<FVector4f> ParamsCopy = ChunkParams;
	// Runs travel with the table because they describe the same thing: which
	// chunk owns which quads. Rebuilt only when the table is dirty, not per
	// frame -- residency changes a handful of times a second, the camera moves
	// every frame, and only the second of those needs re-culling.
	TArray<FChunkRun> RunsCopy = bChunkTableDirty ? BuildChunkRuns() : TArray<FChunkRun>();

	ENQUEUE_RENDER_COMMAND(VoxelGpuPoolIncrementalUpdate)(
		[Proxy, QuadsSlice = MoveTemp(QuadsSlice), IdsSlice = MoveTemp(IdsSlice),
		 OriginsCopy = MoveTemp(OriginsCopy), ParamsCopy = MoveTemp(ParamsCopy), RunsCopy = MoveTemp(RunsCopy),
		 First, Count, NewNumQuads, bTableDirty](FRHICommandListImmediate& RHICmdList)
	{
		if (bTableDirty)
		{
			Proxy->UpdateChunkTable_RenderThread(RHICmdList, OriginsCopy, ParamsCopy, RunsCopy);
		}
		Proxy->UpdateQuadRange_RenderThread(RHICmdList, QuadsSlice, IdsSlice,
		                                    First, Count, NewNumQuads);
	});

	DirtyQuads = {};
	bChunkTableDirty = false;

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
		                            PoolName, ChunkTableCapacity);
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

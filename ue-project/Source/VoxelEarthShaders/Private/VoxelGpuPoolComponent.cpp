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
	                        const TArray<FVector2f>& InClimate)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, Quads(InQuads)
		, ChunkIds(InChunkIds)
		, Origins(InOrigins)
		, Climate(InClimate)
		, NumQuads(Component->GetHighWaterMarkQuads())
		, BufferQuads(InQuads.Num())
		, NumChunks(InOrigins.Num())
		, MaxChunks(FMath::Max(InOrigins.Num() * 4, 1024))
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

		TArray<FVector2f> PaddedClimate = Climate;
		PaddedClimate.SetNumZeroed(MaxChunks);
		ClimateBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkClimate"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(PaddedClimate));
		ClimateSRV = RHICmdList.CreateShaderResourceView(
			ClimateBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(ClimateBuffer));

		VertexFactory.SetQuadBufferSRV(QuadBufferSRV);
		VertexFactory.SetPoolBuffers(OriginSRV, ChunkIdSRV, ClimateSRV);
		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGpuPool: %d chunks, %d quads, %d triangles — ONE primitive, ONE draw"),
		       NumChunks, NumQuads, NumQuads * 2);

	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bDynamicRelevance = true;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
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
				       TEXT("VoxelGpuPool draw SKIPPED: numQuads=%d srv=%d materialProxy=%d"),
				       NumQuads, QuadBufferSRV.IsValid() ? 1 : 0, MaterialProxy != nullptr ? 1 : 0);
			}
			return;
		}

		if (!bLoggedElements)
		{
			bLoggedElements = true;
			UE_LOG(LogTemp, Log,
			       TEXT("VoxelGpuPool draw SUBMITTED: numQuads=%d views=%d visMap=0x%x"),
			       NumQuads, Views.Num(), VisibilityMap);
		}

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			FMeshBatch& Mesh = Collector.AllocateMesh();
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = MaterialProxy;
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastShadow = IsShadowCast(Views[ViewIndex]);

			// ONE element covering EVERY chunk. This is the whole point: the
			// number of chunks resident has no bearing on how many primitives
			// or draws the renderer sees.
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
	                                   const TArray<FVector2f>& NewClimate)
	{
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

		const uint32 ClimateBytes = uint32(NewClimate.Num()) * sizeof(FVector2f);
		if (void* Dst = RHICmdList.LockBuffer(ClimateBuffer, 0, ClimateBytes, RLM_WriteOnly))
		{
			FMemory::Memcpy(Dst, NewClimate.GetData(), ClimateBytes);
			RHICmdList.UnlockBuffer(ClimateBuffer);
		}
	}

	int32 GetMaxChunks() const { return MaxChunks; }

	uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	mutable FVoxelQuadVertexFactory VertexFactory;

	TArray<uint64> Quads;
	TArray<uint32> ChunkIds;
	TArray<FVector4f> Origins;
	TArray<FVector2f> Climate;

	FBufferRHIRef QuadBuffer, ChunkIdBuffer, OriginBuffer, ClimateBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV, ChunkIdSRV, OriginSRV, ClimateSRV;

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
	ChunkClimate.Reset();
	ChunkClimate.Add(FVector2f(0.5f, 0.5f));

	Allocations.Reset();
	NumLiveChunks = 0;
	LocalBounds = FBox(ForceInit);
}

int32 UVoxelGpuPoolComponent::AddChunk(const TArray<uint64>& InQuads,
                                       const FVector3f& OriginUU, int32 Level,
                                       const FVector2f& Climate)
{
	check(Pool.GetCapacityQuads() > 0);   // InitPool first

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		// Out of CONTIGUOUS room, which is not the same as out of space --
		// see FVoxelGpuGeometryPool. The caller decides whether to compact.
		UE_LOG(LogTemp, Warning,
		       TEXT("VoxelGpuPool: no room for %d quads (%u free, largest run %u)"),
		       InQuads.Num(), Pool.GetFreeQuads(), Pool.GetLargestFreeRun());
		return INDEX_NONE;
	}

	const uint32 ChunkId = uint32(ChunkOrigins.Num());
	const float Scale = float(1 << Level);
	ChunkOrigins.Add(FVector4f(OriginUU.X, OriginUU.Y, OriginUU.Z, Scale));
	ChunkClimate.Add(Climate);

	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}

	const int32 Handle = Allocations.Add(Alloc);
	++NumLiveChunks;

	// Grow the bounds by this chunk's extent. A chunk's quads never leave its
	// own 32-voxel cube, scaled by the mip level.
	const float EdgeUU = 320.0f * Scale;
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
	TArray<FVector2f> ClimateCopy = ChunkClimate;

	ENQUEUE_RENDER_COMMAND(VoxelGpuPoolIncrementalUpdate)(
		[Proxy, QuadsSlice = MoveTemp(QuadsSlice), IdsSlice = MoveTemp(IdsSlice),
		 OriginsCopy = MoveTemp(OriginsCopy), ClimateCopy = MoveTemp(ClimateCopy),
		 First, Count, NewNumQuads, bTableDirty](FRHICommandListImmediate& RHICmdList)
	{
		if (bTableDirty)
		{
			Proxy->UpdateChunkTable_RenderThread(RHICmdList, OriginsCopy, ClimateCopy);
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
		const uint32 ChunkId = QuadChunkIds[int32(Existing.Offset)];
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
	// the table does not grow on every edit.
	const uint32 ChunkId = QuadChunkIds[int32(Existing.Offset)];
	const FVector4f Origin = ChunkOrigins[int32(ChunkId)];
	const FVector2f Climate = ChunkClimate[int32(ChunkId)];

	RemoveChunk(Handle);

	const FVoxelGpuPoolAllocation Alloc = Pool.Alloc(uint32(InQuads.Num()));
	if (!Alloc.IsValid())
	{
		return INDEX_NONE;
	}
	for (int32 I = 0; I < InQuads.Num(); ++I)
	{
		PooledQuads[int32(Alloc.Offset) + I] = InQuads[I];
		QuadChunkIds[int32(Alloc.Offset) + I] = ChunkId;
	}
	Allocations[Handle] = Alloc;
	++NumLiveChunks;

	MarkQuadsDirty(Alloc.Offset, Alloc.NumQuads);
	PushUpdatesToProxy();
	return Handle;
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
		       TEXT("VoxelGpuPool upload: drawn=%d hidden=%d outOfRange=%d maxId=%u "
		            "tableEntries=%d hiddenEntry=(%.1f,%.1f,%.1f,scale=%.3f)"),
		       Drawn, HiddenQuads, OutOfRangeQuads, MaxIdSeen, ChunkOrigins.Num(),
		       ChunkOrigins[0].X, ChunkOrigins[0].Y, ChunkOrigins[0].Z, ChunkOrigins[0].W);

		// Where the geometry actually is, versus where the renderer will look
		// for it. A pooled draw that renders nothing is nearly always one of
		// these two disagreeing.
		const FBoxSphereBounds WorldBounds = CalcBounds(GetComponentTransform());
		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGpuPool placement: comp@(%.0f,%.0f,%.0f) firstChunk=(%.0f,%.0f,%.0f,scale=%.1f) "
		            "boundsOrigin=(%.0f,%.0f,%.0f) boundsExtent=(%.0f,%.0f,%.0f)"),
		       GetComponentLocation().X, GetComponentLocation().Y, GetComponentLocation().Z,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].X : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Y : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].Z : 0.f,
		       ChunkOrigins.Num() > 1 ? ChunkOrigins[1].W : 0.f,
		       WorldBounds.Origin.X, WorldBounds.Origin.Y, WorldBounds.Origin.Z,
		       WorldBounds.BoxExtent.X, WorldBounds.BoxExtent.Y, WorldBounds.BoxExtent.Z);
	}

	FVoxelGpuPoolSceneProxy* Proxy =
		new FVoxelGpuPoolSceneProxy(this, UsedQuads, UsedIds, ChunkOrigins, ChunkClimate);
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

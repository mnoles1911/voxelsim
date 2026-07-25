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
	                        const TArray<FVector4f>& InOrigins)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel())
		, Quads(InQuads)
		, ChunkIds(InChunkIds)
		, Origins(InOrigins)
		, NumQuads(InQuads.Num())
		, NumChunks(InOrigins.Num())
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
		if (NumQuads == 0 || NumChunks == 0)
		{
			return;
		}

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

		OriginBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
			RHICmdList, TEXT("VoxelGpuPool.ChunkOrigins"),
			EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer,
			MakeConstArrayView(Origins));
		OriginSRV = RHICmdList.CreateShaderResourceView(
			OriginBuffer, FRHIViewDesc::CreateBufferSRV().SetTypeFromBuffer(OriginBuffer));

		VertexFactory.SetQuadBufferSRV(QuadBufferSRV);
		VertexFactory.SetPoolBuffers(OriginSRV, ChunkIdSRV);
		VertexFactory.InitResource(RHICmdList);

		UE_LOG(LogTemp, Log,
		       TEXT("VoxelGpuPool: %d chunks, %d quads, %d triangles — ONE primitive, ONE draw"),
		       NumChunks, NumQuads, NumQuads * 2);

		Quads.Empty();
		ChunkIds.Empty();
		Origins.Empty();
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
			return;
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

	uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	mutable FVoxelQuadVertexFactory VertexFactory;

	TArray<uint64> Quads;
	TArray<uint32> ChunkIds;
	TArray<FVector4f> Origins;

	FBufferRHIRef QuadBuffer, ChunkIdBuffer, OriginBuffer;
	FShaderResourceViewRHIRef QuadBufferSRV, ChunkIdSRV, OriginSRV;

	int32 NumQuads = 0;
	int32 NumChunks = 0;

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

	Allocations.Reset();
	NumLiveChunks = 0;
	LocalBounds = FBox(ForceInit);
}

int32 UVoxelGpuPoolComponent::AddChunk(const TArray<uint64>& InQuads,
                                       const FVector3f& OriginUU, int32 Level)
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

	MarkRenderStateDirty();
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

	MarkRenderStateDirty();
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
	// Only the used prefix is uploaded; the reserved tail has never been written.
	const int32 Used = int32(Pool.GetHighWaterMark());
	TArray<uint64> UsedQuads(PooledQuads.GetData(), Used);
	TArray<uint32> UsedIds(QuadChunkIds.GetData(), Used);
	return new FVoxelGpuPoolSceneProxy(this, UsedQuads, UsedIds, ChunkOrigins);
}

FBoxSphereBounds UVoxelGpuPoolComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBounds.IsValid)
	{
		return FBoxSphereBounds(FBox(FVector::ZeroVector, FVector(1.0))).TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}

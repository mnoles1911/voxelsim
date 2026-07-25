// Many chunks, ONE primitive, ONE draw call (ADR-0006, G2/G3).
//
// This is the component the whole ADR exists for. UVoxelGpuChunkComponent
// proves a chunk can be drawn from GPU-resident quads, but it is one primitive
// per chunk -- which leaves the FScene::AddPrimitive funnel that ADR-0006
// measured as the frame-time ceiling exactly where it was.
//
// Here every resident chunk lives in one shared quad buffer, every quad names
// its owning chunk, and the vertex shader looks up that chunk's origin. So a
// single draw covers the entire pool, and streaming a chunk in or out writes
// into buffers and changes a triangle count -- it never touches the renderer's
// primitive list.
//
// Deliberately NOT yet wired to the streaming cascade: this proves the draw
// shape first. G3 replaces the append-only upload here with the tested
// FVoxelGpuGeometryPool suballocator and drives it from the desired set.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelGpuGeometryPool.h"
#include "VoxelGpuPoolComponent.generated.h"

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class VOXELEARTHSHADERS_API UVoxelGpuPoolComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelGpuPoolComponent();

	// Sets the pool's capacity in quads. Must be called before the first
	// AddChunk. For scale: the whole 2 km cascade measured 9,441,170 quads.
	void InitPool(uint32 CapacityQuads);

	// Adds one chunk's quads at a component-space origin. Quads are expected in
	// the GPU mesher's packed form, already re-based out of brick-local coords.
	// Returns a handle, or INDEX_NONE if the pool has no contiguous room.
	// Climate is temperature/precipitation already remapped to 0..1 across this
	// world's p1..p99 window -- the same values the CPU path writes into vertex
	// colour B/A for the biome LUT.
	int32 AddChunk(const TArray<uint64>& InQuads, const FVector3f& OriginUU, int32 Level,
	               const FVector2f& Climate = FVector2f(0.5f, 0.5f));

	// Releases a chunk's range back to the pool.
	//
	// The quads are NOT removed from the buffer -- one draw covers the whole
	// pool, so a freed range would otherwise keep rendering stale geometry.
	// Instead its quads are pointed at the reserved hidden chunk, whose scale
	// is zero, which collapses every vertex to a point. They cost vertex
	// invocations until the range is reused, and produce no pixels.
	void RemoveChunk(int32 Handle);

	// Re-meshes a resident chunk in place.
	//
	// Edits are the reason this exists. Implementing a re-mesh as
	// RemoveChunk + AddChunk would fragment the pool worst on exactly the
	// chunks that re-mesh most -- the ones being actively dug. When the new
	// quad count still fits the existing allocation the range is reused; only
	// a chunk that outgrew its slot falls back to reallocating.
	//
	// Returns the handle (possibly a new one if it had to reallocate), or
	// INDEX_NONE if there was no room.
	int32 UpdateChunk(int32 Handle, const TArray<uint64>& InQuads);

	// Drops every chunk.
	void ClearChunks();

	int32 GetNumChunks() const { return NumLiveChunks; }
	int32 GetNumQuads() const { return PooledQuads.Num(); }
	uint32 GetHighWaterMarkQuads() const { return Pool.GetHighWaterMark(); }
	uint32 GetFreeQuads() const { return Pool.GetFreeQuads(); }
	uint32 GetLargestFreeRun() const { return Pool.GetLargestFreeRun(); }

	void SetChunkMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetChunkMaterialOrDefault() const;

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
	                              bool bGetDebugMaterials = false) const override;
	virtual void DestroyRenderState_Concurrent() override;
	//~ End UPrimitiveComponent

private:
	// One flat buffer for every chunk's quads, in insertion order.
	TArray<uint64> PooledQuads;

	// Parallel to PooledQuads: which chunk each quad belongs to. This is what
	// removes per-draw state and lets one draw span the pool.
	TArray<uint32> QuadChunkIds;

	// xyz = chunk origin in component space (unreal units), w = mip scale.
	TArray<FVector4f> ChunkOrigins;

	// Parallel to ChunkOrigins, indexed by the same chunk id.
	TArray<FVector2f> ChunkClimate;

	// Chunk id 0 is RESERVED as the hidden chunk: origin (0,0,0), scale 0.
	// Freed quads point at it and collapse to a degenerate point.
	static constexpr uint32 kHiddenChunkId = 0;

	FVoxelGpuGeometryPool Pool;
	TArray<FVoxelGpuPoolAllocation> Allocations;  // indexed by handle
	int32 NumLiveChunks = 0;

	// Ranges written since the last upload, in quads. Streaming touches a few
	// chunks per frame out of thousands, so uploading the whole pool for each
	// change would be absurd -- at cascade scale that is 75 MB per edit.
	struct FDirtyRange { uint32 First = 0; uint32 Last = 0; bool bValid = false; };
	FDirtyRange DirtyQuads;
	bool bChunkTableDirty = false;

	void MarkQuadsDirty(uint32 First, uint32 Count);
	void PushUpdatesToProxy();

	// Set while a proxy is live so updates can go straight to it instead of
	// rebuilding the render state. Render-thread lifetime is owned by the
	// renderer; this is only ever dereferenced inside a render command.
	class FVoxelGpuPoolSceneProxy* LiveProxy = nullptr;

	FBox LocalBounds = FBox(ForceInit);

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ChunkMaterial = nullptr;
};

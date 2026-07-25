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
#include "VoxelGpuPoolComponent.generated.h"

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class VOXELEARTHSHADERS_API UVoxelGpuPoolComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelGpuPoolComponent();

	// Adds one chunk's quads at a component-space origin. Quads are expected in
	// the GPU mesher's packed form, already re-based out of brick-local coords.
	// Returns the chunk's index in the pool.
	int32 AddChunk(const TArray<uint64>& InQuads, const FVector3f& OriginUU, int32 Level);

	// Drops every chunk. The buffers are rebuilt on the next render state update.
	void ClearChunks();

	int32 GetNumChunks() const { return ChunkOrigins.Num(); }
	int32 GetNumQuads() const { return PooledQuads.Num(); }

	void SetChunkMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetChunkMaterialOrDefault() const;

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
	                              bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent

private:
	// One flat buffer for every chunk's quads, in insertion order.
	TArray<uint64> PooledQuads;

	// Parallel to PooledQuads: which chunk each quad belongs to. This is what
	// removes per-draw state and lets one draw span the pool.
	TArray<uint32> QuadChunkIds;

	// xyz = chunk origin in component space (unreal units), w = mip scale.
	TArray<FVector4f> ChunkOrigins;

	FBox LocalBounds = FBox(ForceInit);

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ChunkMaterial = nullptr;
};

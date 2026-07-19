#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelMeshTypes.h"
#include "VoxelChunkComponent.generated.h"

// Render chunk primitive (docs/m1-plan.md decisions table: one
// UVoxelChunkComponent + one FVoxelChunkSceneProxy per render chunk,
// re-mesh unit on edit). Doctrine
// (docs/voxel-earth-implementation-plan.md SS3.3 Band 1): a hand-rolled
// FPrimitiveSceneProxy, NOT ProceduralMeshComponent / UDynamicMesh.
//
// This component only carries CPU-side quad data (produced by
// UVoxelWorldSubsystem from vxc::meshBrick, chunk-local); the scene proxy
// (FVoxelChunkSceneProxy, defined in VoxelChunkComponent.cpp) converts it to
// GPU buffers on scene proxy creation, following the engine's
// CustomMeshComponent plugin
// (Engine/Plugins/Runtime/CustomMeshComponent/.../CustomMeshComponent.cpp)
// as the API-correct UE 5.7 template for FLocalVertexFactory /
// FStaticMeshVertexBuffers / FDynamicMeshIndexBuffer32.
UCLASS(ClassGroup = Voxel, meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelChunkComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelChunkComponent(const FObjectInitializer& ObjectInitializer);

	// Replace this chunk's mesh (chunk-local voxel-space quads) and mark
	// render state dirty so a fresh scene proxy is built next frame.
	// InChunkEdgeVoxels is the chunk edge length in voxels
	// (VoxelCoords::ChunkEdgeVoxels in stage 1), used for local bounds.
	void SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, int32 InChunkEdgeVoxels);

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* NewMaterial) override;
	virtual int32 GetNumMaterials() const override;
	// Required: the render-thread material verifier rejects any FMeshBatch
	// whose material is missing from this list (engine defaults are exempt,
	// which masked the omission until an authored material was used —
	// symptom: terrain renders with the default material, invisible with
	// M_VoxelTerrain, "not present in GetUsedMaterials" in the log).
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const override;
	//~ End UPrimitiveComponent Interface

private:
	TArray<FVoxelChunkQuad> ChunkQuads;
	int32 ChunkEdgeVoxels = 32;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;

	friend class FVoxelChunkSceneProxy;
};

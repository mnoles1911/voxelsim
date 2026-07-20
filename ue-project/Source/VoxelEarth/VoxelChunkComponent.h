#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelMeshTypes.h"
#include "VoxelChunkComponent.generated.h"

class UMaterialInstanceDynamic;

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

	// M2 mip rings (docs/m2-plan.md decisions table): "one component type
	// serves all levels ... position scale = VoxelSizeUU << level". Set once
	// by UVoxelWorldSubsystem right after NewObject, before RegisterComponent
	// -- never changes for the lifetime of this component (a chunk's level
	// never changes; it is destroyed and a new one spawned instead). Quad
	// coordinates in ChunkQuads stay in level-relative voxel units (0..31);
	// only world placement (this scale) differs by level.
	void SetLevel(int32 InLevel) { ChunkLevel = InLevel; }
	int32 GetLevel() const { return ChunkLevel; }

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

	// --- Chunk-state debug tints (docs/debug-tooling-plan.md P1, mode 2 +
	// voxel.Debug.ChunkStates) ------------------------------------------------

	// Lazily creates a MID over ChunkMaterial (this component derives from
	// UPrimitiveComponent, not UMeshComponent, so the usual
	// CreateDynamicMaterialInstance helper isn't available -- this is its
	// hand-rolled equivalent: UMaterialInstanceDynamic::Create + SetMaterial)
	// and sets its DebugTint vector parameter (M_VoxelTerrain, multiplied into
	// BaseColor; see Tools/create_voxel_material.py). Only ever called by
	// FVoxelWorldImpl::UpdateChunkStateTints, which itself only runs while
	// voxel.Debug.ChunkStates is live under mode 2 -- so the MID is never
	// created at all with the layer off (constraint: "no MIDs created" when
	// voxel.Debug=0 / the layer is off).
	void SetDebugTint(const FLinearColor& Tint);

	// Drops the MID (if any) and reverts to the shared ChunkMaterial, so
	// toggling debug off costs nothing further (constraint: "no MIDs created"
	// when voxel.Debug's chunk-state layer is off).
	void ClearDebugTint();

private:
	TArray<FVoxelChunkQuad> ChunkQuads;
	int32 ChunkEdgeVoxels = 32;
	int32 ChunkLevel = 0;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;

	// Only created on first SetDebugTint call; nullptr (and unused) whenever
	// chunk-state debug tinting has never been engaged this session.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DebugTintMID;

	friend class FVoxelChunkSceneProxy;
};

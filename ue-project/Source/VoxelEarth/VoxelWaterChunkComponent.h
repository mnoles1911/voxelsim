#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelMeshTypes.h"
#include "VoxelWaterChunkComponent.generated.h"

// W2 (docs/voxel-earth-implementation-plan.md SS3.7 Layer B; task spec item
// 4 "Rendering v0"): "WaterChunkComponent mirroring the terrain chunk
// pattern (custom proxy -- doctrine applies to water voxels too)". A lean
// sibling of UVoxelChunkComponent (VoxelChunkComponent.h), NOT a subclass:
// terrain's ring mip-level scale/cross-fade machinery (SetLevel/
// ApplyRingFadeParams) is a rendering-LOD concern that doesn't apply here --
// v0 active water only ever renders at the true-voxel scale, one component
// per resident vxc::WaterBrick8 (8^3 voxels = 80cm cube), never mip-leveled.
// Reuses FVoxelChunkQuad (VoxelMeshTypes.h) unchanged: vxc::Quad and
// vxc::WaterBrick8-driven meshBrick<8> output share the exact same field
// layout regardless of whether the sampler is terrain material or water
// fill-fraction (see UVoxelWaterSubsystem.cpp's meshing sampler).
UCLASS(ClassGroup = Voxel, meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UWaterChunkComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UWaterChunkComponent(const FObjectInitializer& ObjectInitializer);

	// Replace this brick's mesh (brick-local voxel-space quads, 0..7 on each
	// axis -- vxc::WaterBrick8::kEdge) and mark render state dirty so a fresh
	// scene proxy is built next frame. An empty InQuads array is a valid
	// "currently no visible faces" state (e.g. a brief transient moment
	// between drain and re-mesh); the caller (UVoxelWaterSubsystem) is
	// responsible for destroying the component entirely once the CA no
	// longer stores this brick at all (WaterCA::findBrick returns null) --
	// this method alone never does that.
	//
	// InCornerHeights is PARALLEL to InQuads, one packed word per quad, four
	// 8-bit surface heights in the corner order the scene proxy and
	// VoxelQuadDecode.ush share (0=(u0,v0) 1=(u0,v1) 2=(u1,v1) 3=(u1,v0)).
	// UVoxelWaterSubsystem::EmitWaterQuads is the one producer and it fills both
	// arrays in lockstep -- see there for why the heights ride alongside the
	// quads instead of inside FVoxelChunkQuad (that struct and its 8-byte packed
	// form are a contract shared with the GPU mesher, and the packed word is
	// full).
	void SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, TArray<uint32>&& InCornerHeights);

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* NewMaterial) override;
	virtual int32 GetNumMaterials() const override;
	// Required: see UVoxelChunkComponent::GetUsedMaterials's identical doc
	// comment -- the render-thread material verifier rejects any FMeshBatch
	// whose material is missing from this list.
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const override;
	//~ End UPrimitiveComponent Interface

private:
	TArray<FVoxelChunkQuad> ChunkQuads;

	// Parallel to ChunkQuads -- see SetChunkQuads. Always the same length; the
	// proxy indexes the two together and a mismatch would shift every corner
	// height by the difference, so SetChunkQuads checks it rather than trusting
	// the caller.
	TArray<uint32> ChunkCornerHeights;

	// vxc::WaterBrick8::kEdge (8), duplicated as a plain int32 rather than
	// pulling in a voxel-core header (doctrine: this UHT-parsed header stays
	// voxel-core-free) -- VoxelWorldSubsystem.cpp's static_assert pattern for
	// VoxelCoords::VoxelSizeUU vs vxc::kVoxelSizeMm is the precedent; the
	// equivalent check for this constant lives in VoxelWaterSubsystem.cpp,
	// the one .cpp allowed to include both this header and waterca.h.
	static constexpr int32 kBrickEdgeVoxels = 8;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;

	friend class FWaterChunkSceneProxy;
};

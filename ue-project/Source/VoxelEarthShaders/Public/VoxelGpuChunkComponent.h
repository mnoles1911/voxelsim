// One chunk of terrain drawn straight from GPU-resident quads (ADR-0006, G2).
//
// WHAT THIS IS FOR. G2's gate is "a single chunk renders identically to the
// CPU-meshed component". This component is the GPU half of that A/B: give it
// the SAME quads the CPU path was given, and any visual difference is the DRAW
// path, not the mesher. That separation is the whole point -- the generation
// half is already proven bit-exact by voxel.GPU.VerifyRegion, so the A/B should
// only ever be able to indict the renderer.
//
// It deliberately holds ONE chunk. The pool that holds thousands
// (FVoxelGpuGeometryPool) is built and tested, but wiring the whole cascade
// through it is G3; proving one chunk draws correctly has to come first, or a
// wrong quad and a wrong pool offset look identical.
//
// Dynamic relevance, per docs/gpu-g2-draw-path.md: the draw count changes as
// geometry streams, and there is no supported way to change a cached static
// batch's NumPrimitives without re-caching the primitive's draw commands.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelGpuChunkComponent.generated.h"

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class VOXELEARTHSHADERS_API UVoxelGpuChunkComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelGpuChunkComponent();

	// The chunk's geometry, packed as the GPU mesher emits it (word0 | word1<<32).
	// Replaces whatever was there and rebuilds the scene proxy.
	void SetQuads(const TArray<uint64>& InQuads);

	// Mip level for this chunk. Quads are stored in level-relative voxel units,
	// so the shader scales by (1 << Level) -- the same rule the CPU path uses.
	void SetChunkLevel(int32 InLevel);

	int32 GetNumQuads() const { return Quads.Num(); }

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ End UPrimitiveComponent

private:
	TArray<uint64> Quads;

	UPROPERTY()
	int32 ChunkLevel = 0;
};

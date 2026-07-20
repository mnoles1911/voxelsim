#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelOceanActor.generated.h"

class UStaticMeshComponent;
class UPostProcessComponent;
class AExponentialHeightFog;

// W1 first slice (docs/voxel-earth-implementation-plan.md SS3.7 "Implicit
// static" water state; plan SS4 water track W1): the world *looks* hydrated
// with ZERO voxel water data and no simulation. Ocean = implicit infinite
// water at z<0 (sea level z=0 == voxel z=0 == UE world origin z=0, per
// VoxelCoords.h). Spawned by AVoxelEarthGameMode::BeginPlay alongside the
// light rig (same "no authored map yet, spawn from code" reasoning).
//
// Doctrine note: this is a placeholder cosmetic surface, not voxel terrain,
// so it deliberately does NOT use a UProceduralMeshComponent (that would
// blur the line with the custom scene-proxy voxel rendering path) -- a
// single UStaticMeshComponent using the engine's /Engine/BasicShapes/Plane,
// scaled huge and recentred under the camera, is the doctrine-clean choice
// per the task spec.
UCLASS()
class VOXELEARTH_API AVoxelOceanActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelOceanActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	// /Engine/BasicShapes/Plane scaled to PlaneSizeUU on a side, recentred
	// (XY only, Z pinned to sea level 0) under the camera every tick --
	// see UpdateFollowPlane.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Ocean")
	TObjectPtr<UStaticMeshComponent> OceanPlane;

	// Global (bUnbound) post-process volume-equivalent for the underwater
	// tint; blend weight toggled 0/1 by camera depth -- see
	// UpdateUnderwaterState. Attached to the actor so it streams with it
	// (there is exactly one ocean actor, so this never needs pooling).
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Ocean")
	TObjectPtr<UPostProcessComponent> UnderwaterPostProcess;

	// Spawned once in BeginPlay, hidden until the camera first goes
	// underwater; visibility toggled by UpdateUnderwaterState thereafter.
	UPROPERTY(Transient)
	TObjectPtr<AExponentialHeightFog> UnderwaterFog;

	// Recenters OceanPlane's XY under the first player controller's camera
	// (falls back to its pawn if the camera manager isn't ready yet), Z
	// pinned to sea level (0). The recenter position is snapped to
	// FollowSnapUU so the mesh transform only ever changes in whole grid
	// cells instead of drifting by sub-cell amounts every tick -- since the
	// material's ripple perturbation samples absolute WorldPosition (not
	// mesh-local vertex UVs; see Tools/create_ocean_material.py), the shading
	// itself is already invariant to how the mesh is recentred, but snapping
	// avoids needless sub-cm transform churn and keeps this robust even if a
	// future material pass adds mesh-space UVs (texture swim would otherwise
	// appear as the plane's origin drifts a fraction of a texel per frame).
	void UpdateFollowPlane();

	// Toggles the underwater fog + post-process tint by camera depth
	// (camera Z < 0 == underwater), logging LogVoxelEarth once per
	// above/below transition (only signal available without a screenshot
	// for this branch -- see task verification notes).
	void UpdateUnderwaterState();

	// Last-known underwater state, so UpdateUnderwaterState only acts (and
	// logs) on a transition, not every tick.
	bool bUnderwater = false;

	// /Engine/BasicShapes/Plane is a 100x100 UU (1m x 1m) quad; scale factor
	// below reaches PlaneSizeUU on each side.
	static constexpr double SourcePlaneSizeUU = 100.0;
	static constexpr double PlaneSizeUU = 4000000.0; // 40 km

	static constexpr double FollowSnapUU = 100.0; // 1 m
};

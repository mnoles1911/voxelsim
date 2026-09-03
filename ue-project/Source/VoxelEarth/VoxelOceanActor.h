#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// Self-contained include for the PlaneSizeUU derivation below: this header
// used VoxelCoords::kNumLevels while riding a unity-build neighbour's include
// for years; the first adaptive (standalone) compile of the .cpp broke it.
#include "VoxelCoords.h"
#include "VoxelOceanActor.generated.h"

class UStaticMeshComponent;
class UPostProcessComponent;
class UMaterialInstanceDynamic;

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
	// treatment; blend weight ramped between 0 and 1 by camera depth -- see
	// UpdateUnderwaterState. Attached to the actor so it streams with it
	// (there is exactly one ocean actor, so this never needs pooling).
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Ocean")
	TObjectPtr<UPostProcessComponent> UnderwaterPostProcess;

	// THE UNDERWATER LOOK ITSELF, as a post-process material instead of a
	// constant. Created in BeginPlay from /Game/Voxel/M_Underwater and pushed
	// into UnderwaterPostProcess->Settings.WeightedBlendables; null when that
	// asset is missing, which is a supported configuration (see BeginPlay's
	// fallback -- the old constant tint comes back and a Warning is logged).
	//
	// WHY A MATERIAL AT ALL, since the thing it replaces was three floats. The
	// three floats were measured at the OCEAN, at sea level, before the water
	// surface had any volumetric model, and they are now applied unchanged to
	// 2,049 baked lake basins standing at ~1650 m. The surface those lakes are
	// drawn with (/Game/Voxel/M_WaterVoxel, the same material the sheets, the
	// ribbons and the near-field voxels use) says one thing about what the water
	// is made of and the constant tint says another, so looking INTO a pond and
	// swimming IN it disagree. A constant cannot be made to agree: the
	// disagreement is depth-dependent and per-pixel, and a single tint has
	// neither. The material gets the camera's submerged depth every tick
	// (UVoxelWaterSubsystem::SubmergedDepthUUAtWorld) and computes extinction
	// from it.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> UnderwaterMID;

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

	// Ramps the underwater post-process weight toward the water subsystem's
	// verdict for the camera position (UVoxelWaterSubsystem::IsUnderwaterAtWorld
	// -- NOT `camera Z < 0`, see the .cpp for the dry-cavern bug that rule
	// caused), and feeds the material its submerged depth. Logs LogVoxelEarth
	// once per above/below transition -- still the only signal available
	// without a screenshot for this branch, see the task verification notes.
	void UpdateUnderwaterState(float DeltaTime);

	// Last-known underwater state. Used for TWO different things now, and
	// keeping them separate is the point: this bool is the LOGGING edge and the
	// blend TARGET, while UnderwaterBlendWeight below is the continuous value
	// actually handed to the component. Before the blend existed they were the
	// same variable and the function early-returned when nothing had changed;
	// with a ramp the function must run every tick, so the transition log has to
	// hang off this bool's edge rather than off "we did some work this frame",
	// or it would fire once per frame for the whole time you are in the water.
	bool bUnderwater = false;

	// The value actually written to UnderwaterPostProcess->BlendWeight, ramped
	// toward 0/1 at 1/UnderwaterBlendSeconds per second.
	//
	// WHAT THIS FIXES: the weight used to snap 0 -> 1 in a single frame at the
	// waterline. Every underwater cue -- extinction, vignette, and previously a
	// whole fog actor becoming visible -- arrived at once, on one frame, which
	// reads as a shutter rather than as entering water, and is worst in exactly
	// the case that happens most (bobbing across the surface while swimming,
	// where the predicate can flip on consecutive frames).
	float UnderwaterBlendWeight = 0.f;

	// The last submerged depth pushed into the material, in METRES. Held rather
	// than recomputed during the fade-OUT: once the camera is above the surface
	// the depth query correctly answers 0, and pushing that while the weight is
	// still ramping down would collapse the extinction to nothing in one frame
	// -- reintroducing, at the exit, precisely the pop the ramp was added to
	// remove. So the depth freezes at its last submerged value and only the
	// weight moves.
	float LastSubmergedDepthM = 0.f;

	// /Engine/BasicShapes/Plane is a 100x100 UU (1m x 1m) quad; scale factor
	// below reaches PlaneSizeUU on each side.
	static constexpr double SourcePlaneSizeUU = 100.0;
	// DERIVED FROM THE FAR FIELD, NOT FROM THE CASCADE, AND THE DIFFERENCE HAS
	// BITTEN TWICE. It was a flat 4,000,000 UU (+-20 km) that became a VISIBLE
	// HARD EDGE when the far field grew past it; it was then derived from the
	// CASCADE edge, which was right only while voxels were the farthest thing
	// drawn. Since 2026-09-02 the farthest ground is the CLIPMAP again (outer
	// half-extent = 8 x the cascade edge -- AVoxelClipmapActor: HalfIndex(32) /
	// HoleHalfIndex(16) x 2^(NumLevels-1), i.e. 2 x 2^2 at 3 levels), and an
	// ocean sized to the cascade alone would end at +-10 km under 65 km of
	// clipmap terrain: a straight water edge across every coastal vista.
	//
	// The ring radii are geometric -- Outer(L) = 64 m * 2^L -- so the reach
	// follows VoxelCoords::kNumLevels with no include of the streaming
	// subsystem (which would be circular from here). kClipmapExtentMultiple
	// restates the clipmap arithmetic for the same reason; the static_assert
	// in VoxelOceanActor.cpp checks it against the actor's real constants.
	// The plane spans the full DIAMETER, hence the 2x, with 25% margin so a
	// camera near the cascade edge still sees water to the horizon.
	//
	// FREE TO OVERSIZE: this is ONE quad on a scaled unit plane; the margin is
	// generous rather than tight. (This constant is the fifth thing in this
	// codebase found sizing itself off a reach the far field outgrew -- grep
	// for constants in METRES or UU before assuming any reach sweep is done.)
	static constexpr double kOutermostRingOuterMetres =
		64.0 * double(int64(1) << (VoxelCoords::kNumLevels - 1));
	static constexpr double kClipmapExtentMultiple = 8.0; // 2 * 2^(NumLevels-1), NumLevels 3
	static constexpr double PlaneSizeUU =
		2.0 * (kClipmapExtentMultiple * kOutermostRingOuterMetres) * 100.0 * 1.25;

	static constexpr double FollowSnapUU = 100.0; // 1 m

	// How long the underwater post-process takes to reach full strength.
	//
	// 0.15 s is chosen to be SHORT ENOUGH NOT TO BE A TRANSITION EFFECT and long
	// enough to cover the pop: at 60 fps it is 9 frames, at 30 fps it is 5, so
	// the cue arrives gradually at both and there is no frame rate at which it
	// degenerates back to a snap. It is deliberately not longer -- a slow fade
	// makes the water feel like a screen effect being applied to you rather than
	// a medium you moved into, and it would also lag the surface crossing badly
	// enough to be visible while swimming at the waterline.
	//
	// If this ever needs to differ between entering and exiting (entering fast,
	// exiting slower, is the usual asymmetry), split it into two constants
	// rather than averaging them.
	static constexpr float UnderwaterBlendSeconds = 0.15f;

	// The material parameter names, spelled once. These are a CONTRACT with
	// /Game/Voxel/M_Underwater (generated by ue-project/Tools/) -- a typo here
	// does not fail to compile and does not warn at runtime, it silently sets
	// nothing, so the names live in one place where they can be compared against
	// the generator. SubmergedDepthM is the only one this actor drives; see
	// BeginPlay for why the material's other three parameters
	// (UnderwaterExtinctionScale, UnderwaterAmbientGain, UnderwaterAmbientColor)
	// are deliberately left at the values the material itself authors.
	static const FName ParamSubmergedDepthM;
};

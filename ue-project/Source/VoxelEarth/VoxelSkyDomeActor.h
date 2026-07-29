#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelSkyDomeActor.generated.h"

class UStaticMeshComponent;

// The NIGHT SKY's geometry: one huge camera-following sphere carrying
// /Game/Voxel/M_NightSky, which is where the stars and the textured, phased moon
// disc are actually drawn.
//
// It is the mesh half of a two-part feature. The other half is
// UVoxelSkySubsystem::ApplySkyMaterialParams, which writes /Game/Voxel/
// MPC_VoxelSky every frame; the material reads the collection, not this actor,
// so nothing here ever needs a pointer to the subsystem or a MID (the argument
// for an MPC over per-object MIDs is Tools/create_sky_material.py's, under
// "PARAMETERS: ONE MATERIAL PARAMETER COLLECTION, NOT A MID", and it rests on
// VoxelChunkComponent.h:155's anti-MID doctrine).
//
// WHY A SEPARATE ACTOR RATHER THAN A COMPONENT ON THE EXISTING SkyRigActor.
// That was the obvious alternative -- the sky rig actor already exists, already
// hosts two components, and is already placed at the spawn column -- and it is
// not merely untidy but WRONG, for one concrete reason: SkyRigActor's ROOT is
// the USkyAtmosphereComponent, running in ESkyAtmosphereTransformMode::
// PlanetTopAtComponentTransform so that the planet's ground level follows that
// component's world transform (VoxelSkySubsystem.cpp:1389-1401). That transform
// must stay pinned at the spawn column. This dome must move with the camera
// every tick. Attaching the dome to that actor and then moving the actor drags
// the atmosphere's entire planet with it -- which is exactly the misplaced
// horizon-sphere artifact that the PlanetTopAtComponentTransform fix exists to
// prevent -- and attaching it without moving the actor gives up the camera
// tracking. Two lifetimes, two transforms, two actors.
//
// A second, smaller reason: the material has to be resolved in a CONSTRUCTOR
// (see below), which needs a CDO, which a UWorldSubsystem does not have.
//
// SHAPE AND RADIUS. /Engine/BasicShapes/Sphere with its stock outward-facing
// normals -- M_NightSky is two-sided precisely so no inverted-sphere asset is
// needed (create_sky_material.py, "TWO-SIDED"). The radius must EXCEED the
// farthest drawn terrain, because M_NightSky keeps depth testing on so that a
// mountain occludes the stars behind it; a dome inside the clipmap would itself
// be the thing occluded. See VoxelSky::GetDomeRadiusUU for the number and for
// the measured extent it is checked against at BeginPlay.
//
// FACETING DOES NOT MATTER, which is worth stating because a 32x16-segment
// sphere looks like an obviously wrong choice for a sky. Every quantity
// M_NightSky evaluates comes from d = normalize(-CameraVectorWS), and for a
// given screen pixel the shaded point P lies ON that pixel's view ray by
// construction, so normalize(P - camera) is the pixel's exact ray direction no
// matter where along the ray the dome's faceted surface happens to intersect
// it. The tessellation cancels out entirely. The one precondition is that the
// camera stays INSIDE the dome, which is what UpdateFollowCamera guarantees by
// identity rather than by margin.
UCLASS()
class VOXELEARTH_API AVoxelSkyDomeActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelSkyDomeActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	// /Engine/BasicShapes/Sphere, uniformly scaled to VoxelSky::GetDomeRadiusUU
	// and recentred on the camera every tick.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Sky")
	TObjectPtr<UStaticMeshComponent> DomeMesh;

	// Recentres the dome on the first player controller's camera (falling back
	// to its pawn, then doing nothing), in all three axes. Same resolution chain
	// and same early-outs as AVoxelOceanActor::UpdateFollowPlane
	// (VoxelOceanActor.cpp:105-131), which is the precedent this follows.
	//
	// WHAT IS DELIBERATELY NOT COPIED: the ocean's FMath::GridSnap. That snap
	// exists to stop the plane's origin drifting a fraction of a texel per frame
	// under a future mesh-space-UV material (VoxelOceanActor.h:56-64). No such
	// hazard exists here -- M_NightSky's shading is a function of the view
	// DIRECTION alone and is invariant to the dome's position outright (see the
	// faceting note on the class) -- so the snap would buy nothing and would
	// cost the one property worth having: with an exact follow, "the camera is
	// inside the dome" is an identity, not an inequality anyone has to re-check
	// when the radius or the snap size changes.
	void UpdateFollowCamera();

	// Applies voxel.Sky.DomeEnabled and voxel.Sky.DomeRadiusUU. Both are live
	// knobs, so this runs every tick rather than once at BeginPlay; both writes
	// early-out on an unchanged value and the visibility flip is logged once per
	// transition, never per frame.
	void ApplyDomeCvars();

	// The stock sphere's own radius in UU, read from UStaticMesh::GetBounds()
	// rather than assumed. AVoxelOceanActor hard-codes its plane's 100 UU
	// (VoxelOceanActor.h:79), which is fine for a quad whose size is obvious;
	// the sphere's is not written down anywhere in this repo, and getting it
	// wrong scales the dome by a factor of two in a direction nothing would
	// report. 0 means the mesh never loaded.
	double SourceRadiusUU = 0.0;

	// Last radius actually pushed into the scale, so the cvar can move at
	// runtime without rewriting the transform sixty times a second. Negative =
	// never applied.
	double AppliedRadiusUU = -1.0;

	// Last visibility pushed, so voxel.Sky.DomeEnabled logs on transition only.
	// Starts at the "not yet decided" value so the first tick always logs the
	// state the run is actually in -- a capture has to be able to say whether
	// the dome was on from its own log.
	int32 AppliedEnabled = -1;
};

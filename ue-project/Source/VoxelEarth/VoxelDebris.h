#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCoords.h" // VoxelCoords::FVoxelCoord -- UE-only, voxel-core-free
#include "VoxelDebris.generated.h"

class UStaticMeshComponent;
class UInstancedStaticMeshComponent;

// M5 destruction (first slice, docs/voxel-earth-implementation-plan.md SS3.5
// "disconnected islands promoted to rigid voxel debris bodies (Chaos)").
//
// A cosmetic, client-side falling-debris body for one disconnected voxel
// island detected by the connectivity flood-fill (vxc::findDisconnectedIslands)
// after a chop/dig severs a piece of world from the ground. See
// docs/status.md "M5 -- Destruction" for the deterministic/cosmetic split:
//   * The AUTHORITATIVE effect of a chop is the edit-log REMOVAL of the island
//     voxels (deterministic connectivity decision + replicated edit entries),
//     applied in UVoxelWorldSubsystem before this actor is ever spawned.
//   * THIS actor is pure presentation. Its Chaos rigid-body fall/tumble is NOT
//     part of world state, is never replicated, and may differ per client. It
//     touches no edit log and no authoritative voxel data.
//
// v0 simplifications (documented as follow-ups in docs/status.md):
//   * Physics proxy is a single Chaos rigid body (a 1m cube body carrying
//     gravity; collision responses are all Ignore because terrain has NO Chaos
//     collision in this project -- SS3.3 "Chaos only for dynamic debris
//     bodies, not per-chunk terrain"). It free-falls under gravity and is
//     settled onto the voxel surface by a per-tick DDA raycast against the
//     voxel world (UVoxelWorldSubsystem::RaycastVoxelWorld), NOT by a physical
//     contact -- full voxel-vs-Chaos collision is later.
//   * The visible form is an InstancedStaticMesh of one engine cube per island
//     voxel (the "engine cubes" option in the plan), NOT a re-meshed voxel
//     proxy -- reusing the chunk scene proxy for a moving local mesh is a
//     later refinement.
//   * On settle the island rests as a Chaos body; re-integrating it back into
//     the static voxel grid as settled voxels is a documented follow-up.
UCLASS()
class VOXELEARTH_API AVoxelDebris : public AActor
{
	GENERATED_BODY()

public:
	AVoxelDebris();

	virtual void Tick(float DeltaSeconds) override;

	// Builds the physics body + instanced cubes from a detached island's world
	// voxel coordinates and starts it falling. Call once, immediately after
	// SpawnActor. Coords are level-0 voxel-lattice coords (the same the island
	// detector and the collapse pass both emit).
	//
	// Only the island's SURFACE SHELL is instanced -- a voxel with all six
	// face-neighbours also in the island is invisible from every angle, so
	// drawing it is pure cost. For a chopped tree canopy that changes almost
	// nothing; for a collapsed roof or wall slab (M5 large-edit collapse) it is
	// the difference between thousands of instances and tens of thousands.
	//
	// MaxInstances is the caller's remaining per-edit cosmetic budget (see
	// PromoteDetachedIslands' debris caps). If the shell is larger than that,
	// it is uniformly strided down to fit -- the piece still reads as a solid
	// falling mass, just sparser. Returns the number of instances actually
	// created so the caller can debit its budget.
	int32 InitFromIsland(const TArray<VoxelCoords::FVoxelCoord>& IslandVoxels, int32 MaxInstances = MaxInstancesPerBody);

	// Per-body instance ceiling, independent of the caller's per-edit budget.
	static constexpr int32 MaxInstancesPerBody = 8192;

private:
	void SettleOnSurface(double SurfaceTopZUU);

	// Root: the Chaos rigid body (invisible 1m cube; gravity only, ignores all
	// collision -- see class comment). Drives the actor transform while falling.
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> PhysicsBody;

	// Visible form: one engine cube instance per island voxel, attached to
	// (and carried by) PhysicsBody.
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> VoxelISM;

	// Half the island's world-space AABB height (UU), including the outer
	// half-voxel skin -- the offset from the body centre down to the island's
	// lowest face, used to rest that face on the surface at settle time.
	double AabbHalfHeightUU = 0.0;

	int32 VoxelCount = 0;
	float AgeSeconds = 0.f;
	bool bSettled = false;

	// Safety net: if no surface is found under the debris within this long
	// (should never happen over solid terrain), freeze it in place rather than
	// falling forever.
	static constexpr float MaxFallSeconds = 20.f;
};

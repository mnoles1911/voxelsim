#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCoords.h" // VoxelCoords::FVoxelCoord -- UE-only, voxel-core-free
#include "VoxelDebris.generated.h"

class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UVoxelDebrisLifecycle;
class UProceduralMeshComponent;

// Captured before terrain removal. SourceCell is the original zero-based VAC
// coordinate; all cells use the world 100mm lattice. No live-world lookups.
struct FVoxelDebrisCellAppearance
{
    VoxelCoords::FVoxelCoord Coord{};
    uint8 Material=0;
    FColor BaseRGB=FColor::Black;
    FIntVector SourceCell=FIntVector::ZeroValue;
    uint8 SourceYawQuarter=0;
    bool Approved=false,Needle=false,FoliageMask=false;
};

// M5 destruction (first slice, docs/voxel-earth-implementation-plan.md SS3.5
// "disconnected islands promoted to rigid voxel debris bodies (Chaos)").
//
// Detached island presentation/physics proxy. Terrain removal is authoritative
// in UVoxelWorldSubsystem before this actor spawns. Persistent-sized islands
// now join VoxelObjectRegistry, save with stable IDs, and replicate server
// transforms; client replicas do not run their own physics. Tiny cosmetic
// chips retain their local ten-second lifetime and are not persisted.
//
// v0 simplifications (documented as follow-ups in docs/status.md):
//   * Physics proxy is a single Chaos rigid body (a 1m cube body carrying
//     gravity; collision responses are all Ignore because terrain has NO Chaos
//     collision in this project -- SS3.3 "Chaos only for dynamic debris
//     bodies, not per-chunk terrain"). It free-falls under gravity and is
//     settled onto the voxel surface by a per-tick DDA raycast against the
//     voxel world (UVoxelWorldSubsystem::RaycastVoxelWorld), NOT by a physical
//     contact -- full voxel-vs-Chaos collision is later.
//   * Coordinate-only callers retain instanced engine cubes. Captured
//     appearance callers use a bounded procedural surface with original-source
//     face RGB/UV and explicit foliage policy, retained by object snapshots.
//   * On settle the island rests as a Chaos body; re-integrating it back into
//     the static voxel grid as settled voxels is a documented follow-up.
UCLASS()
class VOXELEARTH_API AVoxelDebris : public AActor
{
	GENERATED_BODY()

public:
	AVoxelDebris();

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	bool PersistentState(FArchive& Ar);
	bool CaptureObjectState(TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& Geometry,TArray<uint8>& Dynamic);
	bool RestoreObjectState(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic);
    bool RestoreObjectState(const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& Geometry,const TArray<uint8>& Dynamic) {
        if(!Geometry||!RestoreObjectState(*Geometry,Dynamic))return false;
        CachedObjectGeometry=Geometry;return true;
    }

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

    // Coordinate-aligned snapshot: exact count/order required; invalid input
    // returns zero without mutating the actor. At most 512K captured cells;
    // returns rendered cell count. Stored stride budget survives restoration.
    int32 InitFromIslandWithAppearance(const TArray<VoxelCoords::FVoxelCoord>& IslandVoxels,
        const TArray<FVoxelDebrisCellAppearance>& Cells,int32 MaxInstances=MaxInstancesPerBody);

	// Per-body instance ceiling, independent of the caller's per-edit budget.
	static constexpr int32 MaxInstancesPerBody = 8192;

private:
    int32 InitIsland(const TArray<VoxelCoords::FVoxelCoord>& IslandVoxels,int32 MaxInstances);
    bool GeometryState(FArchive& Ar);
    void BuildAppearanceShell(const TArray<VoxelCoords::FVoxelCoord>& Shell,int32 Stride,const FVector& Centre);
    TArray<FVoxelDebrisCellAppearance> PersistentAppearance;
    int32 PersistentInstanceBudget=MaxInstancesPerBody;
    UPROPERTY(Transient) TArray<TObjectPtr<UProceduralMeshComponent>> AppearanceMeshes;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UVoxelDebrisLifecycle> Cleanup;
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
	TArray<VoxelCoords::FVoxelCoord> PersistentVoxels;
	TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> CachedObjectGeometry;
	float AgeSeconds = 0.f;
	bool bSettled = false;

	// Safety net: if no surface is found under the debris within this long
	// (should never happen over solid terrain), freeze it in place rather than
	// falling forever.
	static constexpr float MaxFallSeconds = 20.f;
};

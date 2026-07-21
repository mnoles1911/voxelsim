#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
// Full definition rather than a forward declaration: TUniquePtr<FVoxelLightField>
// below is destroyed inside UHT's generated default constructor, which needs the
// complete type. VoxelLightField.h is UE-only (CoreMinimal + VoxelMeshTypes) and
// pulls in no voxel-core headers, so the "UHT-parsed headers stay voxel-core-free"
// doctrine still holds.
#include "VoxelLightField.h"
#include "VoxelGI.generated.h"

class UVoxelChunkComponent;

// Voxel cone-traced GI driver (M4) ------------------------------------------
//
// Owns the FVoxelLightField for one world and all of the policy around it:
// what gets voxelized, when it gets solved, how much of that is allowed to
// happen in a frame, and which chunks need re-shading afterwards.
//
// CLIENT-SIDE RENDERING ONLY, outside the determinism boundary. This
// subsystem reads render-chunk geometry and writes vertex colours; it never
// calls into worldgen, never touches the edit log, and nothing it produces is
// replicated or digested. It is also DEFAULT OFF (voxel.GI.Enabled 0) and,
// when off, does no per-frame work at all -- IsTickable() is false and
// NotifyChunkMeshUpdated returns on its first branch -- so the M1 60fps gate
// is unaffected by its mere existence.
//
// HOW EDITS PROPAGATE (no deterministic path is touched to achieve this):
// UVoxelWorldSubsystem already remeshes every chunk an edit dirties and pushes
// the result through UVoxelChunkComponent::SetChunkQuads. That call is the
// only hook this module needs -- it fires on stream-in AND on every
// edit-driven remesh, carries the post-edit geometry, and is already rate
// limited by the streaming system's own applies-per-frame budget. So "dig a
// tunnel" arrives here as "re-voxelize these bricks", and the dirty radius
// around them is queued for re-solve.
//
// HONEST SCOPE NOTE. The shading output is a single SCALAR per vertex, folded
// into VertexColor.G -- the channel M_VoxelTerrain already multiplies into
// BaseColor. That was a deliberate slice-1 choice: it needs no material asset
// edit, no custom global shader and no render-pass integration, so the whole
// feature is CPU-side and reviewable. The cost is real: the bounce is
// monochrome (no coloured bleed), it modulates albedo rather than the ambient
// term specifically, and its spatial resolution is the vertex rate of a greedy
// mesh. See docs/status.md's M4 section for the follow-ups that lift each of
// those.
UCLASS()
class VOXELEARTH_API UVoxelGISubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	virtual void Tick(float DeltaSeconds) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject

	// Called by UVoxelChunkComponent::SetChunkQuads on every mesh update
	// (stream-in and edit-driven remesh alike). Cheap and non-blocking: it
	// only enqueues. Returns immediately when GI is off.
	void NotifyChunkMeshUpdated(UVoxelChunkComponent* Component);

	// Read access for the scene proxy. Never null once Initialize has run.
	const FVoxelLightField& GetField() const { return *Field; }

	// Centre the field was last built around (the view origin). The scene
	// proxy fades GI out toward this radius so the R0/R1 ring boundary does
	// not also become a lighting boundary.
	FVector GetFieldCentreUU() const { return FieldCentreUU; }

private:
	void ClearAllState();
	void MarkBrickNeighbourhoodDirty(const FIntVector& BrickCoord, int32 RadiusBricks);
	void PushDirty(const FIntVector& Key);
	FVector ResolveViewOriginUU() const;

	TUniquePtr<FVoxelLightField> Field;

	// Chunks whose geometry changed and still need voxelizing into the field.
	TArray<TWeakObjectPtr<UVoxelChunkComponent>> PendingVoxelize;

	// FIFO of bricks needing a cone-trace solve. TSet mirrors the array for
	// O(1) dedupe -- a 200-chunk explosion enqueues each affected brick once,
	// not once per overlapping dirty radius.
	TArray<FIntVector> DirtyQueue;
	TSet<FIntVector> DirtySet;

	// Bricks whose irradiance changed and whose chunk therefore needs its
	// scene proxy rebuilt to pick the new vertex colours up.
	TArray<FIntVector> RefreshQueue;
	TSet<FIntVector> RefreshSet;

	// brick coord -> the component that produced it, so a solved brick can
	// find the chunk to re-shade.
	TMap<FIntVector, TWeakObjectPtr<UVoxelChunkComponent>> BrickComponents;

	// Round-robin cursor for the slow background re-solve that lets the
	// progressive bounce converge and keeps stale bricks fresh.
	TArray<FIntVector> RefreshRotation;
	int32 RefreshCursor = 0;

	FVector FieldCentreUU = FVector::ZeroVector;
	bool bCoarseDirty = false;
	bool bHasState = false;
	double LastEvictSeconds = 0.0;
	double LastStatSeconds = 0.0;
	int32 FramesSinceCoarseRebuild = 0;
};

// Free helpers so FVoxelChunkSceneProxy can consult GI policy without pulling
// the whole subsystem header into its hot loop. All of these read cvars.
namespace VoxelGI
{
	VOXELEARTH_API bool IsEnabled();
	VOXELEARTH_API float GetStrength();
	VOXELEARTH_API float GetAmbientFloor();
	VOXELEARTH_API int32 GetMaxQuadSpanVoxels();
	VOXELEARTH_API float GetFadeStartUU();
	VOXELEARTH_API float GetFadeEndUU();
	VOXELEARTH_API int32 GetDebugLevel();
}

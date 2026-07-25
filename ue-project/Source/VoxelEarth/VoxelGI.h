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
// TWO HOOKS, not one, since ADR-0006. The pooled renderer
// (voxel.Stream.GPU 1) never creates a UVoxelChunkComponent, so SetChunkQuads
// never fires for it; FVoxelWorldImpl::ApplyMeshResult calls
// NotifyPooledChunkMeshUpdated at the same point instead, from the same
// stream-in and edit-driven remesh flow. Both feed one field and one budget.
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

	// Same event, from the POOLED (ADR-0006) streaming path. That path packs
	// the CPU mesher's quads straight into the GPU geometry pool and returns
	// WITHOUT ever creating a UVoxelChunkComponent, so NotifyChunkMeshUpdated
	// has nothing to take and was never called: under voxel.Stream.GPU 1 the
	// field stayed empty and voxel.GI.Enabled 1 was a silent no-op.
	//
	// ChunkOriginUU is the chunk's WORLD origin -- exactly what
	// GetComponentLocation() returns on the component path -- and the quads are
	// chunk-local voxel units, identical to UVoxelChunkComponent::ChunkQuads.
	// Quads are consumed by move because there is no component to own them;
	// with GI off the first branch returns before the move, so the caller's
	// array is untouched and this costs one cvar read.
	void NotifyPooledChunkMeshUpdated(const FVector& ChunkOriginUU, int32 ChunkLevel,
	                                  TArray<FVoxelChunkQuad>&& Quads);

	// Read access for the scene proxy. Never null once Initialize has run.
	const FVoxelLightField& GetField() const { return *Field; }

	// Centre the field was last built around (the view origin). The scene
	// proxy fades GI out toward this radius so the R0/R1 ring boundary does
	// not also become a lighting boundary.
	FVector GetFieldCentreUU() const { return FieldCentreUU; }

private:
	void ClearAllState();
	void RunConvergenceHarness();
	void MarkBrickNeighbourhoodDirty(const FIntVector& BrickCoord, int32 RadiusBricks);
	void PushDirty(const FIntVector& Key);
	FVector ResolveViewOriginUU() const;

	TUniquePtr<FVoxelLightField> Field;

	// Chunks whose geometry changed and still need voxelizing into the field.
	TArray<TWeakObjectPtr<UVoxelChunkComponent>> PendingVoxelize;

	// Same, for pooled chunks. A separate queue rather than a variant entry in
	// PendingVoxelize because the two carry genuinely different things: the
	// component queue holds a weak pointer and reads the origin and the quads
	// back off the component at drain time, whereas a pooled chunk has no
	// UObject at all and this queue must therefore OWN everything
	// FVoxelLightField::VoxelizeChunk needs. FVoxelChunkQuad is 9 bytes, so a
	// full queue is single-digit MB and transient.
	//
	// Both queues drain in the SAME budget loop against the same
	// voxel.GI.MaxVoxelizePerFrame, so total per-frame voxelization work is
	// bounded exactly as it was before the pooled path existed.
	struct FPendingPooledChunk
	{
		FVector OriginUU = FVector::ZeroVector;
		TArray<FVoxelChunkQuad> Quads;
	};
	TArray<FPendingPooledChunk> PendingPooledVoxelize;

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
	//
	// POOLED BRICKS DELIBERATELY HAVE NO ENTRY HERE, and that absence is the
	// correct behaviour rather than a gap: the re-shade phase writes vertex
	// colours into a component's colour vertex buffer, and for a pooled chunk
	// there is no component and no per-chunk buffer to write. Baked per-vertex
	// GI simply does not exist on that renderer; the pooled path gets its
	// lighting from the GPU volume (docs/gpu-gi-volume-design.md), which reads
	// the field directly and replaces this phase outright.
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

	// -VoxelGIConverge=<N> harness state (see RunConvergenceHarness).
	int32 ConvergePasses = 0;
	float ConvergeSettleSeconds = 40.f;
	bool bConvergeLegacy = false;
	bool bConvergeSeed = false;
	int32 ConvergeSeedValue = 0;
	bool bConvergeDone = false;
	double FirstTickSeconds = 0.0;
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
	VOXELEARTH_API int32 GetDebugVis();
}

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
// Full definition rather than a forward declaration: TUniquePtr<FVoxelLightField>
// below is destroyed inside UHT's generated default constructor, which needs the
// complete type. VoxelLightField.h is UE-only (CoreMinimal + VoxelMeshTypes) and
// pulls in no voxel-core headers, so the "UHT-parsed headers stay voxel-core-free"
// doctrine still holds.
#include "VoxelLightField.h"
// FVoxelGIVolumeSettings by value below. VoxelEarth already depends on
// VoxelEarthShaders (never the reverse), and this header is UE-only -- it pulls
// in no voxel-core headers, so the "UHT-parsed headers stay voxel-core-free"
// doctrine still holds.
#include "VoxelGIVolume.h"
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

	// voxel.GI.VolumeDigTest. Public because the console command reaches it
	// through the subsystem; see StepDigTest for what it measures and why.
	void StartDigTest(double RadiusUU);

	// voxel.GI.VolumeRecentreTest. Forces a staged re-centre of N bricks on the
	// currently resident field; see the definition for why the scripted flight
	// cannot verify re-centring on its own.
	void ForceVolumeRecentre(int32 ShiftBricks);

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

	// --- GPU volume driver (docs/gpu-gi-volume-design.md §3, §4) ------------
	void PushVolumeUpload(const FIntVector& Key);
	// Establishes the volume origin from the field centre, brick-snapped, and
	// expressed in POOL-PRIMITIVE space. False until the pool exists.
	bool EnsureVolumeOrigin();
	// Encodes and uploads at most Budget bricks (Budget < 0 = the whole queue).
	// Returns the number of bricks encoded. Game thread; reads the field under
	// one FReadScope and hands the staged bytes to the render thread.
	int32 DrainVolumeUploads(int32 Budget);
	void RunVolumeCheck(int32 NumSamples);

	// The whole per-frame volume driver: allocate, re-centre, upload, and
	// re-publish the uniform buffer when any of its inputs moved. Called from
	// Tick. This is also the answer to voxel.GI.Volume's "read per frame" claim,
	// which before Wave B was false -- UpdateParameters_RenderThread had three
	// callers and none of them ran per frame, so Enabled/DebugVis were latched.
	void TickVolume();
	// True when the camera has left the dead zone around the volume centre.
	bool VolumeNeedsRecentre() const;
	// Starts a staged re-centre: recomputes the brick-snapped origin, and
	// schedules the whole texture to be re-encoded and re-uploaded a few
	// brick-rows at a time. Does NOT move the origin uniform -- see StepRecentre.
	void BeginVolumeRecentre();
	// Advances a staged re-centre by one frame's worth of brick-rows. Commits the
	// new origin (one ENQUEUE_RENDER_COMMAND, one frame) when the last row lands.
	void StepVolumeRecentre();
	// Zeroes, re-encodes from the field, and uploads texel Z rows [Z0, Z1) of the
	// volume, addressed by the STAGING origin. Game thread, one FReadScope.
	void RestageVolumeZRange(int32 Z0, int32 Z1);
	// Runs any in-flight re-centre to completion and drains the whole upload
	// queue, so VolumeShadow and the GPU texture agree and both are addressed by
	// the committed origin. The equivalence harness needs that; nothing else does.
	void FlushVolume();
	// Rebuilds FVoxelGIVolumeSettings from the cvars and the committed origin,
	// and re-publishes only if something actually changed.
	void PushVolumeParamsIfChanged();
	// Drives voxel.GI.VolumeDigTest's phases from the tick.
	void StepDigTest();

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

	// --- GPU volume state ---------------------------------------------------
	//
	// Bricks whose TEXELS are stale. Fed from exactly the three events
	// docs/gpu-gi-volume-design.md §3.3 lists -- solved, re-voxelized, evicted
	// -- which is the same work RefreshQueue carries on the component path.
	//
	// A SEPARATE array rather than RefreshQueue itself, deliberately: the
	// re-shade drain POPS RefreshQueue destructively, and voxel.Stream.GPUMaxLevel
	// puts both renderers in one frame, so sharing one array would have the two
	// drains stealing entries from each other. Same events, same dedupe shape,
	// its own cursor.
	TArray<FIntVector> VolumeUploadQueue;
	TSet<FIntVector> VolumeUploadSet;

	// CPU mirror of exactly the bytes staged to the volume, Dim^3 * 4 PER
	// VOLUME -- Scheme A has two, split by the sign of the face normal. This is
	// what voxel.GI.VolumeCheck compares the field against -- "what would the
	// shader return" has to be answered from the bytes that were actually
	// uploaded, not from a re-encode, or the harness cannot catch an addressing
	// or run-merging bug. 1 MB at the default Dim=64, 67 MB at 256.
	TArray<uint8> VolumeShadow;    // (+X,+Y,+Z,v)
	TArray<uint8> VolumeShadowNeg; // (-X,-Y,-Z,v)

	// World UU of texel (0,0,0)'s CELL ORIGIN (not its centre), snapped to a
	// whole 320 UU brick so the texel lattice coincides with the field's cell
	// lattice and no sample ever resamples. Texel i is world
	// VolumeOriginWorldUU + (i+0.5)*40, which is what the CPU sampler's
	// P/40 - 0.5 convention expects with no half-texel fixup.
	//
	// This is the STAGING origin: the one VolumeShadow is addressed in, and the
	// one texels are encoded against. While a re-centre is in flight it runs
	// AHEAD of the origin the shader is using (CommittedOriginPoolUU below);
	// outside a re-centre the two agree. Keeping them separate is what makes
	// "swap the origin uniform on exactly the frame the last upload lands"
	// expressible at all.
	FVector VolumeOriginWorldUU = FVector::ZeroVector;
	// Same origin in units of cells, so brick key -> texel base is integer.
	FIntVector VolumeCellOrigin = FIntVector::ZeroValue;
	// The origin the GPU uniform buffer currently holds, in pool space. Moves
	// exactly once per re-centre, on the frame the staged upload finishes.
	FVector3f CommittedOriginPoolUU = FVector3f::ZeroVector;
	// Pool component world location, cached at EnsureVolumeOrigin. The pool's
	// rebase does not move for the life of a session; re-reading it per frame
	// would be a TObjectIterator scan per frame for a constant.
	FVector PoolWorldUU = FVector::ZeroVector;
	int32 VolumeDim = 0;
	bool bVolumeOriginSet = false;
	int32 VolumeBricksUploaded = 0;
	int32 VolumeRunsUploaded = 0;
	double FirstVolumeUploadSeconds = 0.0;
	bool bVolumeCheckDone = false;
	bool bLoggedNoPool = false;
	bool bVolumeAllocated = false;
	// Bricks the volume driver pushed this frame, for the voxel.GI.Debug 1 line.
	int32 VolumeUploadedThisFrame = 0;

	// voxel.GI.VolumeDigTest state. Phase 0 = idle, 1 = waiting for the dig's
	// remeshes to be ingested, 2 = waiting for the re-solve.
	int32 DigTestPhase = 0;
	double DigTestPhaseSeconds = 0.0;
	TArray<FIntVector> DigTestBricks;

	// --- staged re-centre (docs/gpu-gi-volume-design.md §4) -----------------
	//
	// Brick-row bounds still to be restaged, in BRICK rows (8 texels each), taken
	// from BOTH ENDS INWARD. That order is the point: the camera sits near the
	// volume centre, so the rows that are wrong for longest are the ones furthest
	// from it -- the ones the distance fade is already attenuating -- and the
	// camera's own neighbourhood is only disturbed on the last frame or two
	// before the origin swap makes everything correct at once.
	bool bVolumeRecentring = false;
	int32 RecentreLoRow = 0;
	int32 RecentreHiRow = 0;
	int32 RecentreFrames = 0;
	int32 RecentreCount = 0;
	double RecentreStartSeconds = 0.0;
	// Resident bricks bucketed by their brick-row under the STAGING origin, so
	// restaging a row does not rescan the whole resident set. Built once per
	// re-centre.
	TArray<TArray<FIntVector>> RecentreRowBricks;
	// voxel.GI.Debug >= 2 transient accounting, see RestageVolumeZRange.
	int64 RecentreStepOccupied = 0;
	int64 RecentreOccupiedBeforeLast = 0;
	int64 RecentreTotalOccupied = 0;
	double RecentreStepNearestUU = 0.0;
	double RecentreNearestBeforeLastUU = 0.0;

	// Last settings published to the render thread, so the per-frame refresh can
	// skip the uniform-buffer rebuild when nothing moved.
	FVoxelGIVolumeSettings LastVolumeSettings;
	bool bVolumeSettingsValid = false;

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

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

	// Would NotifyPooledChunkMeshUpdated actually INGEST this chunk?
	//
	// WHY THIS EXISTS, AND IT IS NOT AN OPTIMISATION (Wave D / D1). GI is the
	// only consumer left anywhere that reads a chunk's quad CONTENTS on the CPU
	// -- everything else wants a count or the packed bytes, which the geometry
	// pool now receives without the CPU touching them. So "does GI want this
	// chunk" is exactly the question that decides whether a mesh job may leave
	// its quads on the GPU, and asking it in the wrong direction is silent both
	// ways: too permissive and GPU-meshed chunks stop contributing to the light
	// field (the defect VoxelMeshTypes.h's UnpackVoxelChunkQuad comment exists to
	// prevent, arrived at through a different door); too restrictive and the
	// no-readback path simply never runs.
	//
	// Mirrors the two conditions NotifyPooledChunkMeshUpdated and the voxelize
	// drain actually apply -- level 0 only, and inside voxel.GI.RadiusUU of the
	// field centre -- with a margin on the radius, because this is asked at
	// DISPATCH and the answer is used at apply, some hundreds of milliseconds of
	// camera motion later. Same 1.25 factor EvictFarBricks uses, for consistency
	// rather than by separate derivation.
	//
	// Conservative by construction: it returns true whenever it is unsure, and
	// true costs a readback rather than a missing brick.
	bool WantsChunkQuads(const FVector& ChunkOriginUU, int32 ChunkLevel) const;

	// --- P7 arm identity, for the perf-run summary -------------------------
	//
	// UVoxelPerfRunSubsystem stamps these next to p95 and the hitch count, for
	// the same reason it already stamps the sun and the camera pose: a
	// frame-time number read without the arm it was taken on is the mistake
	// those fields exist to stop, and GI's arm is now three independent
	// switches (Enabled / Volume / SourceBricks) plus a runtime outcome
	// (anchored) that no cvar reports.
	//
	// ANCHORED IS THE ONE THAT CANNOT BE INFERRED FROM THE COMMAND LINE. A leg
	// can have voxel.GI.Enabled 1 and voxel.GI.Volume 1 and still have sampled
	// nothing all run, because EnsureVolumeOrigin refused to identify the
	// terrain pool (P7-a). That leg's p95 describes GI being off while every
	// switch says it is on.
	bool IsVolumeAnchored() const { return bVolumeOriginSet; }
	int32 GetPoolIdentityRefusals() const { return PoolIdentityRefusals; }

	// voxel.GI.VolumeDigTest. Public because the console command reaches it
	// through the subsystem; see StepDigTest for what it measures and why.
	void StartDigTest(double RadiusUU);

	// voxel.GI.VolumeRecentreTest. Forces a staged re-centre of N bricks on the
	// currently resident field; see the definition for why the scripted flight
	// cannot verify re-centring on its own.
	void ForceVolumeRecentre(int32 ShiftBricks);

	// voxel.GI.RelightTest. Carves through the real edit path and times until the
	// dig is FULLY relit -- both the solve queue and the re-shade queue drained.
	// This is the cost side of lowering voxel.GI.MaxChunkRefreshesPerFrame.
	void StartRelightTest(double RadiusUU);

	// --- local lights (docs/sky-and-local-light-plan.md §2.2, phase L1) ------
	//
	// THE REGISTERED LIGHT LIST. A local light is a POSITION, a reach and a
	// strength; registering one asks this subsystem to splat an un-crush scalar
	// into VolumeLocal's A channel so that a deferred point light at the same
	// place has a real BaseColor to illuminate. It is NOT a light source in its
	// own right and it emits nothing: see the channel contract in the plan, and
	// the composition contract in docs/lighting-weather-plan.md §2.3.
	//
	// The caller owns whatever renders (a UPointLightComponent, usually); this
	// owns only the volume side. Returns an id for RemoveLocalLight, or 0 if the
	// registration was refused.
	int32 AddLocalLight(const FVector& PositionUU, double RadiusUU, float Intensity);
	// Moves an already-registered light. Marks BOTH the old and the new
	// neighbourhood dirty -- forgetting the old one leaves a lit ghost where the
	// light used to be, which reads as a bug in the falloff rather than as a
	// missing clear.
	bool MoveLocalLight(int32 LightId, const FVector& PositionUU);
	bool RemoveLocalLight(int32 LightId);
	// Removes every registered light AND destroys the actors the console test
	// command spawned for them. Both halves in one call on purpose: the L1 gate's
	// torch-off arm has to remove the un-crush and the deferred light together, or
	// it is measuring something else.
	void ClearLocalLights();
	int32 NumLocalLights() const { return LocalLights.Num(); }

	// voxel.GI.LocalLightTest. Places a torch at the camera: registers a local
	// light and (unless voxel.GI.LocalLightDeferred is 0) spawns the
	// UPointLightComponent that actually emits. Public because the console command
	// reaches it through the subsystem, as StartDigTest and StartRelightTest do.
	void PlaceLocalLightTestAtCamera(double RadiusUU, float Intensity);

	// Read access for the scene proxy. Never null once Initialize has run.
	const FVoxelLightField& GetField() const { return *Field; }

	// Centre the field was last built around (the view origin). The scene
	// proxy fades GI out toward this radius so the R0/R1 ring boundary does
	// not also become a lighting boundary.
	FVector GetFieldCentreUU() const { return FieldCentreUU; }

private:
	// Defined with the members below (it is data, and it belongs beside the array
	// that holds it); forward-declared here only so the splat helpers can take it
	// by reference.
	struct FVoxelLocalLight;

	void ClearAllState();
	void RunConvergenceHarness();
	void MarkBrickNeighbourhoodDirty(const FIntVector& BrickCoord, int32 RadiusBricks);
	void PushDirty(const FIntVector& Key);

	// --- P7 march arm ------------------------------------------------------
	//
	// PushMarchDirty queues one GI brick key for the GPU cone march. It is the
	// march arm's entire ingest: the key arrives on the EXISTING notify hooks,
	// which a direct-to-pool apply already calls with an empty quad array, so
	// nothing new streams and nothing is read back.
	//
	// TickMarchBricks drains up to voxel.GI.MarchBricksPerFrame of them,
	// resolves each into (corner in the march frame, texel in the volume) in
	// DOUBLE, and hands the batch to VoxelGIMarch::Enqueue_GameThread.
	void PushMarchDirty(const FIntVector& Key);
	void TickMarchBricks();
	// bOutResolved false means nothing authoritative was available yet (the
	// player camera manager has never updated and there is no pawn), and the
	// return value is the PREVIOUS centre carried forward rather than a fresh
	// reading. Callers must not run world queries or latch a volume origin from
	// an unresolved centre -- see the function body for the three things a
	// silently-zero centre broke, one of which was fatal.
	FVector ResolveViewOriginUU(bool& bOutResolved) const;

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

	// --- local light splat (docs/sky-and-local-light-plan.md §2.2) -----------
	//
	// Recomputes VolumeLocal's A channel for one box of LOCAL texels from the
	// whole registered light list, writes the CPU mirror and uploads the box.
	//
	// FROM SCRATCH, over every light that overlaps the box, rather than
	// incrementally per light. A is the MAX over all local sources, and max is not
	// invertible under removal -- the same reason RebuildCoarse is a full rebuild
	// -- so "subtract the light that moved" is not expressible. Recomputing also
	// makes every dirty box idempotent, which is what lets the re-centre restage
	// double-cover a boundary texel without consequence.
	void SplatLocalBox(const FIntVector& Min, const FIntVector& Size);
	// Uploads at most Budget dirty boxes (Budget < 0 = the whole queue).
	int32 DrainLocalSplats(int32 Budget);
	// Pushes the texel box a light covers onto the dirty queue. Clipped to the
	// volume; a no-op when the light is entirely outside it.
	void MarkLocalLightDirty(const FVector& PositionUU, double RadiusUU);
	// Whole volume, as LocalDim/8 Z slabs rather than one box, so the drain budget
	// still bounds the work. Used when the feature is switched on, when the queue
	// overflows, and when the volume's origin commits.
	void MarkLocalVolumeDirty();
	// A brick was re-voxelized or evicted: any light whose reach covers it has a
	// stale occlusion march. This is what makes digging through a wall let the
	// torchlight follow, and what clears the "not resident = not an occluder"
	// assumption in FVoxelLightField::LocalLightTransmittance once the brick
	// actually arrives.
	void MarkLocalLightsDirtyForBrick(const FIntVector& BrickCoord);
	// The un-crush a point receives from every registered light: inverse-square
	// falloff x the light field's line-march occlusion, maxed over lights, 0..1.
	// Read scope supplied by the caller because the splat evaluates thousands of
	// these under one lock.
	float LocalUnCrushAt(const FVector& PointUU, const FVoxelLightField::FReadScope& Read) const;
	// One light's contribution at one point. THE single definition of the falloff
	// and the occlusion: the splat calls it per texel and voxel.GI.VolumeCheck's
	// reference arm calls it per tap, so the two cannot drift apart -- a harness
	// with its own copy of the formula compares a thing against itself.
	float LocalLightContributionAt(const FVoxelLocalLight& Light, const FVector& PointUU,
	                              const FVoxelLightField::FReadScope& Read) const;
	// Restages the LOCAL texels that lie in main-volume texel rows [Z0, Z1).
	// Called from RestageVolumeZRange so VolumeLocal moves in the SAME rows as its
	// siblings and commits on the SAME frame: a volume that re-centred
	// independently would leave torch light displaced by up to the dead zone
	// (2560 UU) mid-flight, which is §5 item 7.
	void RestageLocalZRange(int32 Z0, int32 Z1);
	// Local texel edge, UU. The two volumes cover the same box, so this is
	// VolumeDim*CellSize / VolumeLocalDim -- 80 UU at the defaults. Derived, never
	// a second hardcoded constant, because the box identity is what makes the
	// factory able to reuse Interpolants.GIUVW.
	double LocalTexelUU() const;
	// Runs any in-flight re-centre to completion and drains the whole upload
	// queue, so VolumeShadow and the GPU texture agree and both are addressed by
	// the committed origin. The equivalence harness needs that; nothing else does.
	void FlushVolume();
	// Rebuilds FVoxelGIVolumeSettings from the cvars and the committed origin,
	// and re-publishes only if something actually changed.
	void PushVolumeParamsIfChanged();
	// Drives voxel.GI.VolumeDigTest's phases from the tick.
	void StepDigTest();
	void StepRelightTest();

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

	// CPU mirror of VolumeLocal, LocalDim^3 * 4 -- 3.4 MB at the default 96.
	// Same job as the two above (voxel.GI.VolumeCheck has to compare against the
	// bytes that were actually uploaded, not against a re-encode) and the same
	// RGBA8 stride, but sized from VolumeLocalDim, NOT from VolumeDim: the two
	// volumes cover the same box at different resolutions, and copying the
	// sibling's Dim^3*4 would allocate 8x too much at the defaults and index
	// wrongly at every other pair of dims. Only A is written in L1.
	TArray<uint8> VolumeLocalShadow;
	int32 VolumeLocalDim = 0;
	// Registered local lights. Tiny by construction (a handful), so every lookup
	// here is a linear scan -- deliberately, because a map keyed by id would add
	// bookkeeping to something the splat iterates in full anyway.
	struct FVoxelLocalLight
	{
		FVector PositionUU = FVector::ZeroVector;
		double RadiusUU = 1000.0;
		// Peak un-crush at the light's core, 0..1. NOT an intensity in any
		// radiometric unit -- the light's actual brightness lives on the deferred
		// light, per §2.3. 1.0 means "a surface here should be lit against its full
		// albedo", which is the un-crushed state, not a bright one.
		float Intensity = 1.0f;
		int32 Id = 0;
		// Only set for lights the console test command spawned, so ClearLocalLights
		// can take the deferred half away with the un-crush half. A real caller
		// registers a position and owns its own component.
		TWeakObjectPtr<AActor> TestActor;
	};
	TArray<FVoxelLocalLight> LocalLights;
	int32 NextLocalLightId = 1;
	struct FVoxelLocalSplatBox
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Size = FIntVector::ZeroValue;
	};
	TArray<FVoxelLocalSplatBox> LocalSplatQueue;
	int32 LocalBoxesSplatted = 0;
	// Texels that actually received a non-zero un-crush, i.e. the ones that paid
	// for an occlusion march AND survived it. Reported beside the box count because
	// the two together are what tell "the splat ran and found geometry" from "the
	// splat ran into a wall" -- the same reason the volume driver reports bricks
	// AND runs.
	int64 LocalTexelsLit = 0;
	double LocalSplatMs = 0.0;
	// Last resolved value of voxel.GI.LocalLights, so the switch's own value is
	// logged when it moves and the feature can re-arm (allocate, splat everything)
	// on the way up.
	bool bLocalLightsActive = false;

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
	// DOUBLE, and kept unnarrowed: the camera-relative origin is derived from it
	// each commit, and narrowing twice is how a 40 UU cell picks up a 1 UU error.
	FVector CommittedVolumeOriginWorldUU = FVector::ZeroVector;
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
	// PHASE 5: A PERMANENT DEFERRAL MUST NOT LOOK LIKE A STARTUP WAIT.
	//
	// bLoggedNoPool fires once, at Log severity, saying "deferred, retried every
	// tick". That is correct for the seconds before the first chunk lands and
	// WRONG FOREVER AFTER, and in a log the two are indistinguishable.
	//
	// Under terrain quad retirement they stop being the same thing:
	// ApplyMeshResult takes its NumQuads == 0 return and never reaches
	// GetOrCreateGpuPool, so the terrain pool is never created, the deferral is
	// permanent, and GI goes absent for WATER too -- with no error, because the
	// shader gates on bInsideVolume. This escalates once the wait has outlasted
	// any plausible startup.
	bool bLoggedNoPoolEscalated = false;
	// P7-a. Times EnsureVolumeOrigin declined to latch an origin because it
	// could not positively identify the TERRAIN pool. Counted rather than only
	// logged because the two benign outcomes are logged once and then go quiet,
	// and "it deferred for one tick at startup" and "it has been deferring for
	// the whole leg" are otherwise the same log line.
	//
	// CUMULATIVE, AND DELIBERATELY NOT A GATE. It rises every tick until the
	// terrain pool exists, so on a healthy leg it settles at some small startup
	// number and stops. NEVER compare it against a fixed threshold: a
	// monotonically growing statistic against a fixed bar must fire in any
	// long-enough run and therefore carries no information -- that rule cost
	// this project a retracted archive-wide invalidation on 2026-08-21. It is
	// read as a PAIR with anchored= on the same line: anchored=1 with a small
	// count is startup; anchored=0 with a count that keeps climbing is the
	// defect.
	//
	// PROVEN TO FIRE, NOT ASSUMED TO: voxel.GI.PoolIdentityMutate 1 forces the
	// name half of the identification to fail, which drives exactly this
	// increment. A counter that is declared, reset, printed and never
	// incremented reads 0 forever and looks like a clean result; one was found
	// in this tree on 2026-08-21.
	int32 PoolIdentityRefusals = 0;
	// Separate from bLoggedNoPool: a NAME MISMATCH is a wiring defect and gets
	// its own Error, while "no pool yet" is an ordinary startup wait. Folding
	// them into one flag is what let a defect print a reassuring line.
	bool bLoggedPoolNameMismatch = false;
	bool bVolumeAllocated = false;
	// VolumeLocal is allocated lazily and INDEPENDENTLY, because
	// voxel.GI.LocalLights ships off and a 3.4 MB texture nothing samples is a
	// cost with no consumer. Tracked separately from bVolumeAllocated so turning
	// the switch on mid-session re-enters EnsureAllocated_RenderThread exactly
	// once.
	bool bLocalVolumeAllocated = false;
	// Bricks the volume driver pushed this frame, for the voxel.GI.Debug 1 line.
	int32 VolumeUploadedThisFrame = 0;

	// voxel.GI.VolumeDigTest state. Phase 0 = idle, 1 = waiting for the dig's
	// remeshes to be ingested, 2 = waiting for the re-solve.
	int32 DigTestPhase = 0;
	double DigTestPhaseSeconds = 0.0;
	TArray<FIntVector> DigTestBricks;

	// -VoxelGIRelightAfter=<seconds>. Command line rather than -ExecCmds for the
	// same reason -VoxelGIVis is: ExecCmds lands before the field has streamed in,
	// and a relight test on an empty field measures nothing. Fires once.
	float RelightAfterSeconds = 0.f;
	bool bRelightArmed = false;

	// voxel.GI.RelightTest state.
	bool bRelightTestActive = false;
	double RelightTestStartSeconds = 0.0;
	int32 RelightTestPeakDirty = 0;
	int32 RelightTestPeakRefresh = 0;
	TArray<FIntVector> RelightTestBricks;
	// See StepRelightTest: an edit does not dirty bricks synchronously, so the
	// drain wait must not begin until work has actually arrived.
	bool bRelightWorkSeen = false;

	// Chunks the voxelize drain skipped because they were further than
	// voxel.GI.RadiusUU from the view origin, accumulated between debug lines.
	//
	// THE FIELD THAT DECIDES FINDING B-M. "The light field is empty under motion"
	// has two very different causes with the same symptom: chunks never arriving
	// (an ingest or streaming problem) or chunks arriving and being REJECTED for
	// range (the camera is simply too far above the terrain). One counter tells
	// those apart, and it is cheaper than either of the runs that were planned to
	// do the same job.
	int32 VoxRejectedRadius = 0;

	// --- PHASE ATTRIBUTION (voxel.GI.Debug 1, the "GI phase:" line) ---------
	//
	// WHY THIS EXISTS. Until now the only timed phase in this subsystem was
	// SolveBricks, and the only other number was the whole tick. So "what does
	// GI cost" had exactly two answers -- the cone march, and everything else
	// lumped together -- and the one published decomposition of GI's cost
	// (docs/status.md:4994) was obtained by ABLATION on a renderer this project
	// no longer ships (component path, voxel.Stream.GPU 0, voxel.GI.Volume 0).
	// These accumulate the phases the pooled path actually runs.
	//
	// EACH TERM CARRIES ITS OWN COUNT, and that is not tidiness. The brick
	// stats line divided each term by its own count correctly and then printed
	// a TOTAL by SUMMING THE PER-CHUNK MEANS as though the denominators
	// matched; on an arm where one term covered 96 chunks and another 83,671 it
	// inflated the answer by 0.74 ms/chunk and REVERSED the verdict
	// (docs/measurements/armA-drawpath-ceiling-2026-08-19.txt, "THE PRINTED
	// `TOTAL` LINE IS WRONG"). So: every mean printed here is
	// total-ms-over-its-own-count, and the total is total-ms-over-frames. A sum
	// of means is never computed anywhere in this file.
	//
	// Accumulated between debug lines and reset when one prints, exactly like
	// VoxRejectedRadius above -- the pre-existing per-frame fields on the "GI:"
	// line sample only the frame the line happens to fire on, which is a
	// 1-in-60 sample of a bursty workload.
	double StatVoxMs = 0.0;
	int32 StatVoxChunks = 0;      // chunks actually voxelized (both queues)
	double StatCoarseMs = 0.0;
	int32 StatCoarseRuns = 0;     // RebuildCoarse calls
	double StatSolveMs = 0.0;
	int32 StatSolveBricks = 0;    // bricks handed to SolveBricks
	double StatUploadMs = 0.0;
	int32 StatUploadBricks = 0;   // bricks encoded + staged by DrainVolumeUploads
	double StatTickMs = 0.0;
	int32 StatTickFrames = 0;     // ticks that got past the enabled/Field guards

	// --- quadsRetainedForGI: the cost nobody has ever measured --------------
	//
	// WantsChunkQuads (below) is consulted by
	// FVoxelWorldImpl::SubmitGpuMeshJob (VoxelWorldSubsystem.cpp:10389-10399),
	// and a `true` forces that chunk OFF the D1 direct-to-pool path
	// (no-readback GPU meshing, PR #161) and onto the GPU READBACK path --
	// purely so this subsystem can have the quads for its own ingest. At the
	// default voxel.GI.RadiusUU that is every level-0 chunk within 87.5 m of
	// the camera: the near field, which is exactly where the player is.
	//
	// This is a cost to the STREAMING path rather than to the frame, which is
	// why no frame-time arm has ever seen it, and it lands on a GPU mesh fork
	// already measured as throughput-bound. Counting it makes it visible
	// per-second instead of only by reading two files.
	//
	// THE COMPANION COUNTER IS THE POINT, not decoration. `retained` alone
	// cannot distinguish "the gate ran and said no" from "the gate never ran"
	// -- and both read as zero. With `declined` beside it: both zero means the
	// call site is not reaching us at all (GI off, or the hook lost), which is
	// a broken instrument rather than a good result. This project has shipped
	// probes that measured nothing while reporting success.
	//
	// mutable: WantsChunkQuads is const and called from the streaming submit
	// path; this is pure telemetry and no caller's behaviour depends on it.
	mutable int32 StatQuadsRetainedForGI = 0;
	mutable int32 StatQuadsDeclined = 0;

	// --- P7-c arm counters: proving the arm ENGAGED, not merely that it is set
	//
	// StatQuadsRetiredByArm counts level-0 candidates WantsChunkQuads declined
	// BECAUSE OF voxel.GI.SourceBricks, as distinct from the ones it declines
	// on distance. Without this split the arm's success reading
	// (quadsRetainedForGI = 0) is byte-identical to the failure reading where
	// the call site stopped firing -- which is the failure this project has
	// shipped repeatedly and which the retained/declined pair was already added
	// to catch once.
	//
	// StatIngestDroppedByArm counts chunks the two ingest hooks refused for the
	// same reason. It is the SECOND half of the same claim: the demand can only
	// be gone if BOTH the request (WantsChunkQuads) and the acceptance
	// (Notify*ChunkMeshUpdated) stopped. A non-zero retire count with a zero
	// drop count means the component path is still feeding the field and the
	// arm is only half applied -- which on voxel.Stream.GPU 0 is exactly what
	// would happen if only WantsChunkQuads were gated.
	//
	// mutable for StatQuadsRetiredByArm only: WantsChunkQuads is const.
	mutable int32 StatQuadsRetiredByArm = 0;
	int32 StatIngestDroppedByArm = 0;

	// --- P7 march arm state ------------------------------------------------
	//
	// The queue is keys only -- no geometry, no quads, no payload. That is the
	// point of the arm: the bricks are already resident on the GPU and the CPU
	// never needs to see them.
	//
	// SET PLUS ARRAY for PushDirty's reason: the array preserves arrival order
	// (a brick that streamed in first should light first), and the set makes
	// the duplicate test O(1) -- a chunk that re-meshes several times in one
	// frame must not be dispatched several times.
	TArray<FIntVector> MarchDirtyQueue;
	TSet<FIntVector> MarchDirtySet;

	// Bricks handed to VoxelGIMarch::Enqueue_GameThread this interval. THE
	// GAME THREAD'S OWN COUNT, and deliberately not a claim about the GPU: the
	// render-thread half can still refuse (volumes not UAV-capable, nothing
	// resident yet) and says so from where it happens. Reported in the GI arm
	// line so "the arm is set" and "the arm submitted work" stay distinguishable
	// -- both read as zero otherwise, and this project has been fooled by that
	// exact pair before.
	int32 StatMarchBricksSubmitted = 0;

	// --- ARM VERIFICATION, and it re-checks for the whole leg --------------
	//
	// -VoxelGIOn / -VoxelGIOff (VoxelEarthGameMode.cpp) set voxel.GI.Enabled at
	// GameMode init and read it straight back. That read-back proves the set
	// took AT THAT MOMENT. It cannot prove the value SURVIVES, because
	// -ExecCmds runs later in startup and SetByConsole outranks the SetByCode
	// the switch used -- so a leg carrying both would log "CONFIRMED" and then
	// be silently flipped underneath itself a few hundred lines further down
	// the log. That exact shape was hit on this project today with
	// r.ShadowQuality: confirmed from its first log line while the override
	// landed 190 lines later.
	//
	// So the switch records what it ASKED FOR here, and Tick re-compares it
	// against the live cvar once a second for the entire run. A one-shot check
	// answers "did it take"; this answers "did it hold", which is the question
	// a control arm actually depends on.
	//
	// -1 = no arm switch passed, and then none of this costs anything: the
	// comparison never runs and IsTickable is unaffected.
	int32 ArmRequestedEnabled = -1;
	bool bArmFirstCheckDone = false;
	int32 ArmViolations = 0;
	double LastArmCheckSeconds = 0.0;

	// --- staged re-centre (docs/gpu-gi-volume-design.md §4) -----------------
	//
	// Brick-row bounds still to be restaged, in BRICK rows (8 texels each). Each
	// step takes from whichever END IS FURTHER FROM THE CAMERA'S OWN ROW, so the
	// last row standing is always the camera's.
	//
	// That is the whole mitigation, and it is narrower than it first looks.
	// Staging over ~8 frames means most of the volume is stale by the penultimate
	// frame regardless -- measured at 69.3% of occupied texels -- so ordering
	// cannot reduce HOW MUCH is wrong, only WHERE. What it buys is that the wrong
	// region is the far field, which the distance fade is already attenuating,
	// rather than the ground under the player. An earlier version took two rows
	// off the low end and one off the high end, which drifted the meeting point
	// off the camera and put a stale row under it for one frame
	// (nearestStaleRowToCamera = 0 UU).
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
	// Whether FieldCentreUU above is a real reading this tick or the previous
	// value carried forward because nothing authoritative was available yet.
	// See ResolveViewOriginUU. Two things consult it and both must: the
	// camAboveSurface diagnostic (an unresolved centre made it query elevation
	// at the world origin, which is FATAL on an unbaked fine tile) and
	// EnsureVolumeOrigin (which LATCHES, so a zero centre anchors the GI volume
	// permanently in the wrong place).
	bool bFieldCentreResolved = false;
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
	// P7-c measurement arm (voxel.GI.SourceBricks) OR the P7 march arm
	// (voxel.GI.MarchBricks). True means the CPU quad ingest is RETIRED. On the
	// SourceBricks arm nothing replaces it (a streaming arm, never a lighting
	// one); on the march arm the GPU cone march does. See both cvars in
	// VoxelGI.cpp.
	VOXELEARTH_API bool IsQuadIngestRetired();

	// P7 march arm (voxel.GI.MarchBricks). True means irradiance is produced by
	// VoxelGIMarch.usf reading the resident brick pool, and the whole CPU
	// voxelize/solve/encode/upload chain is retired.
	VOXELEARTH_API bool IsMarchBricksEnabled();
}

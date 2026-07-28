// Many chunks, ONE primitive, ONE draw call (ADR-0006, G2/G3).
//
// This is the component the whole ADR exists for. UVoxelGpuChunkComponent
// proves a chunk can be drawn from GPU-resident quads, but it is one primitive
// per chunk -- which leaves the FScene::AddPrimitive funnel that ADR-0006
// measured as the frame-time ceiling exactly where it was.
//
// Here every resident chunk lives in one shared quad buffer, every quad names
// its owning chunk, and the vertex shader looks up that chunk's origin. So a
// single draw covers the entire pool, and streaming a chunk in or out writes
// into buffers and changes a triangle count -- it never touches the renderer's
// primitive list.
//
// Deliberately NOT yet wired to the streaming cascade: this proves the draw
// shape first. G3 replaces the append-only upload here with the tested
// FVoxelGpuGeometryPool suballocator and drives it from the desired set.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
// FThreadSafeCounter64 (FVoxelGpuPoolBuffers::RunBoundsCycles). Explicit because
// CoreMinimal.h pulls in the 32-bit FThreadSafeCounter but not this one.
#include "HAL/ThreadSafeCounter64.h"
#include "RHIResources.h"
#include "VoxelGpuGeometryPool.h"
#include "VoxelGpuQuadPayload.h"
#include "VoxelGpuPoolComponent.generated.h"

class FRDGPooledBuffer;

// The pool's two big GPU buffers, owned by the COMPONENT rather than the scene
// proxy (Wave D / D1).
//
// WHY THIS EXISTS, AND WHAT IT FIXES. These used to be created inside
// FVoxelGpuPoolSceneProxy::CreateRenderThreadResources and destroyed with the
// proxy, and CreateSceneProxy re-uploaded the whole CPU shadow every time a new
// proxy was built. That is correct only while the CPU shadow is the sole author
// of the quads. Once the GPU writes into the pool directly, any of the four
// MarkRenderStateDirty paths throws the proxy away and re-uploads stale CPU
// content over GPU-written geometry -- which presents as TERRAIN REVERTING TO
// OLDER GEOMETRY AFTER AN UNRELATED EVENT, the hardest failure in this wave to
// trace back to its cause.
//
// The fix is one level of ownership, not an immortal vertex factory. The
// factory bakes these SRVs into a uniform buffer in InitRHI, but that uniform
// buffer holds nothing else of substance -- so a new proxy simply builds a
// fresh one pointing at the SAME SRVs. The quads survive because the buffer was
// never destroyed and never re-uploaded.
//
// Held by shared pointer so the last reference can be dropped on the render
// thread (see UVoxelGpuPoolComponent::BeginDestroy) rather than wherever the
// component happens to be collected.
struct FVoxelGpuPoolBuffers
{
	// Declared, not defaulted inline: the two FRDGPooledBuffer references below
	// are held through a forward declaration, so the destructor has to be
	// instantiated in the one translation unit that has the complete type.
	FVoxelGpuPoolBuffers();
	~FVoxelGpuPoolBuffers();

	FBufferRHIRef QuadBuffer;
	FBufferRHIRef ChunkIdBuffer;

	FShaderResourceViewRHIRef QuadSRV;
	FShaderResourceViewRHIRef ChunkIdSRV;

	// D1: the GPU-write half. Created alongside the SRVs so a compute pass can
	// write quads in place.
	FUnorderedAccessViewRHIRef QuadUAV;
	FUnorderedAccessViewRHIRef ChunkIdUAV;

	// The same two buffers, wrapped so RDG can track them (Wave D / D1).
	//
	// WHY BOTH FORMS. The vertex factory binds the raw SRVs above and bakes them
	// into a uniform buffer in InitRHI, which is a non-RDG world. The mesher's
	// write is a compute pass, which is an RDG world. Wrapping the SAME
	// FRHIBuffer in an FRDGPooledBuffer is what lets one buffer live in both:
	// RegisterExternalBuffer brings it into a graph, RDG inserts the
	// UAVCompute transition, and its default epilogue access is SRVMask -- i.e.
	// it hands the buffer back in exactly the state the draw wants, with no
	// manual barrier anywhere.
	//
	// FRDGBufferDesc::CreateStructuredDesc's default usage is
	// Static|UnorderedAccess|ShaderResource|StructuredBuffer, which is exactly
	// how the buffers below are created. That is not a coincidence to be
	// preserved by luck -- if the creation flags ever change, these descs must
	// change with them or RDG will be describing a buffer that is not there.
	TRefCountPtr<FRDGPooledBuffer> QuadPooled;
	TRefCountPtr<FRDGPooledBuffer> ChunkIdPooled;

	// Elements the buffers were created with. The whole capacity is allocated up
	// front because writes address ABSOLUTE pool offsets; if a later proxy wants
	// more than this, the buffers must be rebuilt (and the GPU content is
	// genuinely gone, so that path re-uploads from the CPU shadow).
	int32 CapacityQuads = 0;

	// Set to 1 by the RENDER thread once the buffers and their RDG wrappers
	// exist; read by the GAME thread to decide whether a mesh job may be
	// dispatched direct-to-pool.
	//
	// A counter rather than a bool because the write and the read are on
	// different threads and this is the file's existing idiom for that
	// (FVoxelGpuPoolSceneProxy::ElementsLogged). It only ever goes 0 -> 1, so
	// a stale read costs one job the readback path and nothing else -- the
	// direction that fails safe. The render command that performs a direct write
	// re-checks on the render thread, where the answer cannot be stale.
	FThreadSafeCounter GpuWritable;

	// Direct writes that reached the render thread and could not be performed.
	//
	// LIVES HERE, NOT ON THE COMPONENT, because it is written from inside a
	// render command and the component is a UObject that may have been collected
	// by the time that command runs. This holder is kept alive by the same
	// shared reference the command already captures, so the counter cannot
	// outlive its storage. See UVoxelGpuPoolComponent::GetGpuDirectWritesDropped
	// for why a dropped write must be counted rather than only logged.
	FThreadSafeCounter DroppedWrites;

	// Render-thread cycles spent in FVoxelGpuPoolSceneProxy::RebuildRunBounds,
	// and the number of times it ran. Wave S0 (docs/speculative-generation-plan.md
	// §4): the deep review prices this at 45-90 ns per run over every resident
	// chunk, per applied chunk -- ~2-4 ms of render-thread CPU each -- and that
	// estimate is the entire case for batching publication. It has never been
	// measured. This is the measurement.
	//
	// HERE FOR THE SAME REASON AS DroppedWrites: RebuildRunBounds runs on the
	// render thread inside a command that captured this holder by shared
	// reference, and the component is a UObject that may already be gone. Drained
	// from the game thread by UVoxelGpuPoolComponent::GetAndResetPushStats.
	//
	// Cycles rather than milliseconds so the accumulate side is one
	// FPlatformTime::Cycles64() pair and no floating-point work runs inside the
	// gather-adjacent path being measured.
	FThreadSafeCounter64 RunBoundsCycles;
	FThreadSafeCounter64 RunBoundsCalls;
	FThreadSafeCounter64 RunBoundsRunsWalked;

	bool IsValid() const { return QuadBuffer.IsValid() && ChunkIdBuffer.IsValid(); }
};

using FVoxelGpuPoolBuffersRef = TSharedPtr<FVoxelGpuPoolBuffers, ESPMode::ThreadSafe>;

// One contiguous run of quads to write into the GPU buffer. First is the
// destination offset in the pool; SrcOffset indexes the flat staging array the
// render command carries, since several runs travel in one payload.
struct FVoxelQuadUploadRun
{
	uint32 First = 0;
	uint32 Count = 0;
	uint32 SrcOffset = 0;
};

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class VOXELEARTHSHADERS_API UVoxelGpuPoolComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelGpuPoolComponent();

	// Sets the pool's capacity in quads. Must be called before the first
	// AddChunk. For scale: the whole 2 km cascade measured 9,441,170 quads.
	void InitPool(uint32 CapacityQuads);

	// One resident chunk's contiguous span of the quad pool, plus the chunk-table
	// entry that describes where it is in space.
	//
	// Exists so the scene proxy can FRUSTUM-CULL. The pool's whole design is one
	// primitive and one draw covering every resident quad, and the measured cost
	// of that is a renderer whose frame time does not depend on what is on
	// screen: pointing the camera at almost nothing left it unchanged (18.58 ->
	// 19.05 ms) while the per-chunk component path got 64% cheaper. Culling needs
	// to know, per chunk, which quads are its own -- which the component knows
	// from its allocations and the proxy cannot recover, because the proxy's
	// copy of the per-quad chunk id array is not kept current by the incremental
	// upload path (that writes the GPU buffer only).
	struct FChunkRun
	{
		uint32 ChunkId = 0;
		uint32 FirstQuad = 0;
		uint32 NumQuads = 0;
	};

	// One chunk's quads waiting to be copied GPU-side into its allocation
	// (Wave D / D1). Accumulated by AddChunkFromGpu and drained by
	// PushUpdatesToProxy.
	//
	// THEY RIDE THE SAME RENDER COMMAND AS THE TABLE UPDATE, AND THE ORDER
	// INSIDE IT IS THE WHOLE CORRECTNESS ARGUMENT. A pool range becomes drawable
	// when NumQuads grows past it (the uncull path) or when its FChunkRun reaches
	// the proxy (the cull path) -- both of which travel on that update. If the
	// write were enqueued after it, there would be a window in which the renderer
	// draws a range the GPU has not written yet. So the copy passes are recorded
	// FIRST, in the same command, and nothing can reorder the two.
	//
	// Public only so the file-local pass builder can name it.
	// S2-1. A freed range whose ids must be repointed at the hidden chunk on the
	// GPU instead of through the CPU shadow. Same lifetime and same drain point
	// as FPendingGpuWrite -- see TakePendingGpuHides.
	struct FPendingGpuHide
	{
		uint32 DstFirst = 0;
		uint32 NumQuads = 0;
		// Carried rather than read from kHiddenChunkId at the pass, for the same
		// reason FPendingGpuWrite carries ChunkId: the pass is a free function on
		// the render thread and the constant is private to this class. Stamped at
		// enqueue, where it is in scope.
		uint32 HiddenId = 0;
	};

	struct FPendingGpuWrite
	{
		FVoxelGpuQuadPayloadRef Src;
		uint32 DstFirst = 0;
		uint32 NumQuads = 0;
		uint32 ChunkId = 0;
	};

	// A surface height that can never gate anything off. Used for the hidden
	// chunk and by callers with no world subsystem to sample, mirroring
	// BuildChunkVertexData's own "no subsystem -> always surface" fallback.
	// -1e30 rather than -FLT_MAX so the shader's `- kSurfaceBandUU` stays finite.
	static constexpr float kNoSurfaceGate = -1.0e30f;

	// Adds one chunk's quads at a component-space origin. Quads are expected in
	// the GPU mesher's packed form, already re-based out of brick-local coords.
	// Returns a handle, or INDEX_NONE if the pool has no contiguous room.
	//
	// Params carries the per-chunk shading inputs the 8-byte quad packing has no
	// room for -- climate in xy, surface height in z, one spare in w. See
	// FVoxelQuadVertexFactoryParameters::ChunkParams for the exact layout and
	// the reason z is chunk-relative.
	int32 AddChunk(const TArray<uint64>& InQuads, const FVector3f& OriginUU, int32 Level,
	               const FVector4f& Params = FVector4f(0.5f, 0.5f, kNoSurfaceGate, 0.0f));

	// Adds one chunk whose quads are ALREADY IN GPU MEMORY (Wave D / D1).
	//
	// Identical to AddChunk in everything the CPU still owns -- the allocation,
	// the chunk-table entry, the bounds, the runs the cull reads -- and different
	// in exactly one respect: the quads and their chunk ids are written by a
	// COMPUTE PASS instead of by two Lock/Memcpy/Unlock pairs, so no quad byte
	// crosses PCIe and none is copied on the game or render thread.
	//
	// Src must hold at least NumQuads quads at its SrcFirst; the allocation is
	// exactly NumQuads slots. Returns a handle, or INDEX_NONE if the pool has no
	// contiguous room -- the SAME outcome AddChunk gives, counted in the same
	// AllocFailure counters, because a full pool drops geometry identically
	// whichever mesher produced it.
	//
	// THE CPU SHADOW IS DELIBERATELY NOT WRITTEN for this range. See PooledQuads
	// for what that means and why it is the safe direction.
	//
	// Returns INDEX_NONE without allocating if IsGpuWritable() is false.
	int32 AddChunkFromGpu(const FVoxelGpuQuadPayloadRef& Src, uint32 NumQuads,
	                      const FVector3f& OriginUU, int32 Level,
	                      const FVector4f& Params = FVector4f(0.5f, 0.5f, kNoSurfaceGate, 0.0f));

	// Whether a direct GPU write can land yet: true once the persistent buffers
	// and their RDG wrappers exist, which happens on the render thread when the
	// FIRST scene proxy is created -- and a proxy is only created once the pool
	// holds something, so the very first chunks of a session necessarily arrive
	// by some other route.
	//
	// That bootstrap is self-healing and needs no special case: a caller that
	// finds this false submits its job on the readback path, the resulting
	// AddChunk creates the pool's first proxy, and every job after that goes
	// direct. Game thread.
	bool IsGpuWritable() const;

	// Direct-to-pool census. GpuDirectWrites counts chunks written by a compute
	// pass; GpuDirectWritesDropped counts payloads that reached the render thread
	// and could not be written because the buffers were not there.
	//
	// The second is a counter and not only a log line for the same reason
	// GetAllocFailureCount is: a dropped write leaves a pool range allocated,
	// named by a live chunk-table entry, holding whatever was there before --
	// which is zeros and the hidden chunk id on a fresh buffer, i.e. INVISIBLE
	// TERRAIN THAT NOTHING ELSE REPORTS. Non-zero means geometry is missing.
	int64 GetGpuDirectWrites() const { return GpuDirectWrites; }
	int32 GetGpuDirectWritesDropped() const
	{
		return PoolBuffers.IsValid() ? PoolBuffers->DroppedWrites.GetValue() : 0;
	}

	// What one window's worth of publication actually cost, on BOTH threads.
	//
	// Wave S0 (docs/speculative-generation-plan.md §4, executing T0-1). Every
	// AddChunk / AddChunkFromGpu / UpdateChunk / RemoveChunk ends in its own
	// PushUpdatesToProxy, and each of those copies the whole chunk table, runs
	// BuildChunkRuns (a walk plus a sort), enqueues a render command, and has the
	// render thread rebuild every run's bounds. The deep review's §1a says that is
	// the ~600 chunks/s plateau. Nothing has ever measured it, so the batching
	// wave would otherwise be built on an estimate.
	//
	// RunsBuilt is deliberately separate from Pushes: a push with neither
	// bChunkTableDirty nor bRunsDirty set skips BuildChunkRuns entirely, and the
	// ratio is how you tell "publication is frequent" from "publication is
	// expensive".
	//
	// AllocationsWalked is the term that does NOT scale with residency:
	// BuildChunkRuns reserves and walks Allocations, which is append-only, so it
	// grows with chunks ever added. If this climbs across a leg while
	// GetNumChunks() is flat, that is the finding.
	//
	// Render-thread figures come from the shared buffer holder and are therefore
	// whatever had landed by the time the game thread asked -- they lag the
	// game-thread figures by a frame or two and are not exactly co-windowed. That
	// is fine for a 5 s census and wrong for anything finer; do not quote them
	// per-frame.
	struct FPoolPushStats
	{
		int64 Pushes = 0;               // PushUpdatesToProxy calls that reached the render command
		int64 RunsBuilt = 0;            // ...of which ran BuildChunkRuns
		int64 AllocationsWalked = 0;    // total Allocations entries walked by those builds
		int64 RunsEmitted = 0;          // total live runs those builds produced
		double TableCopyMs = 0.0;       // game thread: OriginsCopy + ParamsCopy
		double BuildRunsMs = 0.0;       // game thread: BuildChunkRuns (walk + sort)
		double RunBoundsMs = 0.0;       // RENDER thread: RebuildRunBounds
		int64 RunBoundsCalls = 0;       // RENDER thread
		int64 RunBoundsRunsWalked = 0;  // RENDER thread
	};
	FPoolPushStats GetAndResetPushStats();

	// Releases a chunk's range back to the pool.
	//
	// The quads are NOT removed from the buffer -- one draw covers the whole
	// pool, so a freed range would otherwise keep rendering stale geometry.
	// Instead its quads are pointed at the reserved hidden chunk, whose scale
	// is zero, which collapses every vertex to a point. They cost vertex
	// invocations until the range is reused, and produce no pixels.
	void RemoveChunk(int32 Handle);

	// Re-meshes a resident chunk in place.
	//
	// Edits are the reason this exists. Implementing a re-mesh as
	// RemoveChunk + AddChunk would fragment the pool worst on exactly the
	// chunks that re-mesh most -- the ones being actively dug. When the new
	// quad count still fits the existing allocation the range is reused; only
	// a chunk that outgrew its slot falls back to reallocating.
	//
	// Returns the handle (possibly a new one if it had to reallocate), or
	// INDEX_NONE if there was no room.
	int32 UpdateChunk(int32 Handle, const TArray<uint64>& InQuads);

	// Drops every chunk.
	void ClearChunks();

	int32 GetNumChunks() const { return NumLiveChunks; }
	int32 GetNumQuads() const { return PooledQuads.Num(); }
	uint32 GetHighWaterMarkQuads() const { return Pool.GetHighWaterMark(); }
	uint32 GetFreeQuads() const { return Pool.GetFreeQuads(); }
	uint32 GetLargestFreeRun() const { return Pool.GetLargestFreeRun(); }
	// 1 means unfragmented. Rising while GetFreeQuads() stays healthy is the
	// early warning that allocations are about to start failing on a pool that
	// still looks half empty.
	int32 GetFreeRunCount() const { return Pool.GetFreeRunCount(); }

	// Cumulative count of AddChunk/UpdateChunk calls that found no room, and the
	// quads they wanted. NON-ZERO MEANS GEOMETRY IS MISSING FROM THE WORLD.
	//
	// This is a counter rather than only a log line because the failure is
	// otherwise invisible in every aggregate: a chunk that fails to allocate is
	// simply not added, so liveChunks, highWater and the resident quad count all
	// stay self-consistently smaller and nothing looks wrong. A cascade that
	// overflowed the pool and dropped a third of its chunks reads as a cheaper,
	// faster run -- i.e. exactly like a success. Wave F moves R0 to 128 m against
	// a pool already at ~93% of capacity, so this is the difference between a
	// result and an artefact.
	int64 GetAllocFailureCount() const { return AllocFailureCount; }
	int64 GetAllocFailureQuads() const { return AllocFailureQuads; }

	// Allocations is APPEND-ONLY by default: AddChunk and AddChunkFromGpu both
	// Allocations.Add(...), freed slots are zeroed in place, and Reset() only
	// happens in InitPool. So this is a count of chunks EVER ADDED over the
	// pool's lifetime, not resident chunks -- GetNumChunks() is that. It is the
	// growth term in BuildChunkRuns's Reserve()-and-walk, which runs once per
	// publication.
	//
	// S1-2: under voxel.Stream.PoolRecycleHandles this still counts every append
	// ever made -- FreeHandles lets a freed slot be REUSED rather than growing
	// the array, so with the gate on this stops climbing once the pool reaches
	// steady state instead of tracking chunks ever added. See GetFreeHandleCount.
	int32 GetNumAllocationsEver() const { return Allocations.Num(); }

	// S1-2 (docs/speculative-generation-plan.md §2.2). Handles sitting in
	// FreeHandles, available for the next AddChunk/AddChunkFromGpu instead of a
	// fresh append. Appended to the "Voxel GPU pool:" log line alongside
	// allocsEver so the recycling is visible as it works: with
	// voxel.Stream.PoolRecycleHandles on, allocsEver should plateau near
	// liveChunks once churn saturates the free list, instead of climbing for the
	// whole session (measured in docs/measurements/s0-apply-census-2026-07-27.txt,
	// "NEW FINDING 1": 28,042 -> 68,416 allocsEver against 28,042 -> 18,389 live
	// runs over one 120s flight leg).
	int32 GetFreeHandleCount() const { return FreeHandles.Num(); }

	// Dirty-range algebra, exposed for automation tests ONLY (same precedent as
	// DebugGetChunkRuns).
	//
	// WHY THIS IS EXPOSED AT ALL. S1-1's UnmarkQuadsDirty guards a race that the
	// tick ordering makes rare -- it did not fire once across four full flight
	// legs (docs/measurements/s1-1-batch-publish-2026-07-27.txt). Ground rule 13
	// says a path with no recorded executions is untested however green
	// everything around it is, and the failure mode if the interval algebra is
	// wrong is INVISIBLE TERRAIN THAT REPORTS AS LOADED. So the algebra gets
	// direct coverage instead of waiting for a leg to happen to exercise it.
	//
	// Pure list manipulation: no pool, no proxy, no GPU, no InitPool required.
	void DebugMarkQuadsDirty(uint32 First, uint32 Count) { MarkQuadsDirty(First, Count); }
	void DebugUnmarkQuadsDirty(uint32 First, uint32 Count) { UnmarkQuadsDirty(First, Count); }
	// (First, Last) inclusive, in ascending order. Copied out rather than
	// exposing FDirtyRange, which stays private.
	TArray<TPair<uint32, uint32>> DebugGetDirtyRanges() const
	{
		TArray<TPair<uint32, uint32>> Out;
		Out.Reserve(DirtyQuadRanges.Num());
		for (const FDirtyRange& R : DirtyQuadRanges)
		{
			Out.Emplace(R.First, R.Last);
		}
		return Out;
	}

	// S1-1's hazard counter -- see UnmarkQuadsDirty. Non-zero on the first
	// batched leg proves the free-then-reallocate-in-one-frame race is REAL and
	// is being handled; zero across a full flight means either it cannot occur or
	// the call is not wired, and those must not be confused with each other.
	int64 GetDirtyOverlapsResolved() const { return DirtyOverlapsResolved; }
	int64 GetDirtyOverlapQuadsResolved() const { return DirtyOverlapQuadsResolved; }

	// --- S2-3: PARKING. Hide a chunk without giving up its geometry. ---------
	//
	// This is the mechanism T4-1 is built on (docs/speculative-generation-plan.md
	// Wave S4): speculatively generated terrain has to sit somewhere the renderer
	// ignores until admission asks for it, and re-admitting it has to be free.
	// It also pays for itself before any speculation exists -- ring oscillation
	// under motion currently re-meshes ground that was resident seconds ago.
	//
	// IT IS FAR CHEAPER THAN THE ORIGINAL DESIGN ASSUMED, and the reason is in
	// the shader. VoxelQuadVertexFactory.ush reads ChunkOrigins[ChunkId].w as the
	// chunk's SCALE and multiplies every decoded corner by it, so w == 0
	// collapses that chunk's quads to a degenerate point WHATEVER the per-quad id
	// buffer holds. ComputeRunBounds already returns Hidden on Entry.W <= 0 and
	// BuildCulledRanges already skips it.
	//
	// So park = write one float and set bChunkTableDirty. No quad traffic in
	// either direction, no Pool.Free, and no per-quad id stamp -- which means
	// parking never enters the id-recycling path at all, and does NOT depend on
	// the GPU hide pass (T1-4) the plan originally sequenced it behind.
	//
	// The run is untouched too: ChunkId, FirstQuad and NumQuads are all
	// unchanged, so bRunsDirty is deliberately NOT set. Only the table entry
	// moves, and RebuildRunBounds re-derives Hidden from it.
	//
	// LIFETIME: the caller keeps the handle. A parked chunk still owns its pool
	// range, its Allocations slot and its chunk-table entry, so it still counts
	// against GetNumChunks(), the pool capacity and the chunk-table floor. That
	// is the whole risk surface -- see the subsystem-side park registry for why
	// its cap is a correctness boundary rather than a tuning knob.
	// Unpark takes no params, and that is deliberate: ParkChunk only zeroes the
	// SCALE in ChunkOrigins, so ChunkParams[ChunkId] -- climate and surface gate
	// -- is never disturbed and there is nothing to restore. Re-deriving it here
	// would mean recomputing a value that was never lost, and would make unpark
	// depend on a scene component the eviction path does not have.
	void ParkChunk(int32 Handle);
	void UnparkChunk(int32 Handle, const FVector3f& OriginUU, int32 Level);

	// Mark the chunk table dirty and record that a flush is OWED, without
	// forcing one. Park/unpark use this instead of PushUpdatesToProxy.
	//
	// THIS IS THE DIFFERENCE BETWEEN PARKING PAYING AND COSTING. Measured with
	// park/unpark publishing eagerly: on the circle profile 89% of parked
	// geometry was adopted and throughput still fell 33%, because every adopt
	// traded one meshing round trip for one FULL POOL PUBLICATION -- 41,726
	// adopts, 41,726 publications. After S1-1 the publication IS the expensive
	// thing, so a cache that publishes once per hit cannot win
	// (docs/measurements/s1-close-2026-07-27.txt).
	//
	// Why not wrap the caller in an FScopedBatch instead: tried, and it took
	// parking from 28,117 parks at 84% hit to ZERO parks with throughput halved.
	// Confirmed as cause rather than variance, and NOT diagnosed. This is the
	// smaller change -- it needs no scope around anything, and the adopt path is
	// inside RecomputeDesiredSet where no scope exists.
	//
	// Safe because the table entry is the whole payload: no quads move, no run
	// changes. Worst case is one frame of a chunk staying visible after park, or
	// staying hidden after adopt. The streaming tick's own FScopedBatch closes
	// every tick and flushes anything owed, so nothing can sit indefinitely.
	void MarkChunkTableDirtyDeferred();
	// Parked chunks are still live allocations; this is how many of GetNumChunks()
	// are currently hidden.
	int32 GetNumParkedChunks() const { return NumParkedChunks; }

	// Hold one of these across a run of adds/removes and they publish ONCE, at
	// the outermost close, instead of once each (S1-1,
	// docs/speculative-generation-plan.md; measured in
	// docs/measurements/s0-apply-census-2026-07-27.txt).
	//
	// WHY THIS IS THE WAVE. Every AddChunk / AddChunkFromGpu / UpdateChunk /
	// RemoveChunk ends in its own PushUpdatesToProxy, and S0 measured what one of
	// those costs: 0.275 ms early in a fill, 2.108 ms under flight, of which
	// 98-99% is this publication. The dirty-range machinery already accumulates
	// correctly across several mutations -- DirtyQuadRanges coalesces,
	// bChunkTableDirty and bRunsDirty are latches -- so the ONLY thing forcing
	// per-chunk cost is the eager flush at the bottom of each mutator. This
	// removes that, and nothing else.
	//
	// Re-entrant by depth, because ApplyMeshResult is reachable from the edit
	// path as well as the streaming tick and must keep its existing flush
	// semantics when it is not inside a scope.
	//
	// GATED: with voxel.Stream.PoolBatchPublish 0 (the default) this is inert and
	// every mutator flushes exactly as it did before.
	class VOXELEARTHSHADERS_API FScopedBatch
	{
	public:
		explicit FScopedBatch(UVoxelGpuPoolComponent* InPool);
		~FScopedBatch();

		// Non-copyable, non-movable: the depth counter is the whole mechanism and
		// a stray copy would decrement it twice.
		FScopedBatch(const FScopedBatch&) = delete;
		FScopedBatch& operator=(const FScopedBatch&) = delete;

	private:
		TWeakObjectPtr<UVoxelGpuPoolComponent> Pool;
	};

	void SetChunkMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetChunkMaterialOrDefault() const;

	// Names this pool in every log line it emits. There is more than one pool
	// instance now (terrain under voxel.Stream.GPU, water under
	// voxel.Water.GPU), and a pooled primitive is diagnosed almost entirely
	// from its log lines -- two pools both printing "VoxelGpuPool draw
	// SUBMITTED" would make the one number that matters unattributable.
	// Must be set BEFORE the proxy is created; the proxy takes a copy.
	void SetPoolName(const FString& InName) { PoolName = InName; }

	// Edge length, in voxels, of one entry in this pool -- 32 for a terrain
	// render chunk, 8 for a vxc::WaterBrick8. Used only to grow LocalBounds by
	// the right amount per AddChunk. Getting it too LARGE is merely a looser
	// cull; too small culls away geometry that is genuinely there, so the
	// default stays at terrain's 32.
	void SetChunkEdgeVoxels(int32 InEdgeVoxels) { ChunkEdgeVoxels = FMath::Max(1, InEdgeVoxels); }
	// The proxy's frustum cull needs the same extent AddChunk grows the bounds
	// by, so that a culled chunk can never be tighter than the bounds the scene
	// already tested the whole primitive against.
	int32 GetChunkEdgeVoxels() const { return ChunkEdgeVoxels; }

	// Floor on the chunk table's GPU allocation, in entries. Exceeding whatever
	// the proxy was built with is not an error but it IS a full render-state
	// rebuild (PushUpdatesToProxy falls back to MarkRenderStateDirty), which
	// re-uploads the entire quad buffer. Terrain grows past its floor a handful
	// of times while the cascade fills and then stops; a pool whose resident
	// count oscillates around the floor would pay that repeatedly, so it wants a
	// floor above its own steady state. Must be set before the proxy is created.
	void SetChunkTableCapacity(int32 InEntries) { ChunkTableCapacity = FMath::Max(1, InEntries); }
	// So callers can LOG the floor instead of repeating the literal they passed.
	// The startup line did exactly that and went stale within one commit of the
	// floor moving.
	int32 GetChunkTableCapacity() const { return ChunkTableCapacity; }

	//~ UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials,
	                              bool bGetDebugMaterials = false) const override;
	virtual void DestroyRenderState_Concurrent() override;
	//~ End UPrimitiveComponent

	//~ Begin UObject
	// Drops the persistent buffers' last reference on the RENDER thread. Not
	// DestroyRenderState_Concurrent -- that fires on every MarkRenderStateDirty,
	// which is precisely the event these buffers exist to survive.
	virtual void BeginDestroy() override;
	//~ End UObject

	// DEBUG ONLY — voxel.GPU.VerifyPoolWrite.
	//
	// A GPU write can only be checked by reading back what is ACTUALLY in the
	// pool buffers; every CPU-side number about it (the allocation, the run, the
	// quad count) would be equally consistent if the compute pass had written
	// nothing at all, or written it at the wrong offset. So the verify harness
	// needs the buffers themselves. Read-only in intent; nothing in the shipping
	// path calls this.
	FVoxelGpuPoolBuffersRef DebugGetPoolBuffers() const { return PoolBuffers; }

	// DEBUG ONLY — the run list the proxy culls and draws from. The harness
	// checks the bytes at a RUN's offset rather than at an offset it worked out
	// itself, because the run is what the renderer will actually read.
	TArray<FChunkRun> DebugGetChunkRuns() const { return BuildChunkRuns(); }

	// DESTRUCTIVE DEBUG — see voxel.Stream.PoolClobberTest. Corrupts the first N
	// quads of the CPU shadow and forces proxy recreation, so that "rebound" and
	// "re-uploaded" predict different pictures instead of being indistinguishable.
	void DebugClobberShadowAndRecreate(int32 NumQuadsToClobber);

private:
	// See FVoxelGpuPoolBuffers. The holder is allocated on demand; the buffers
	// inside it are created on the render thread by the FIRST proxy and reused
	// by every proxy after it.
	FVoxelGpuPoolBuffersRef PoolBuffers;
	FVoxelGpuPoolBuffersRef GetOrCreatePoolBuffers();

	// One flat buffer for every chunk's quads, in insertion order.
	//
	// NOT WRITTEN FOR GPU-MESHED RANGES (Wave D / D1), and that is a deliberate
	// asymmetry rather than an oversight. AddChunkFromGpu leaves this array's
	// slice for its chunk holding zeros, and QuadChunkIds' slice holding 0 --
	// which is kHiddenChunkId, scale 0, collapsed to a point.
	//
	// So the shadow is not merely stale for those ranges, it is stale in the ONE
	// safe direction: anything that ever did re-upload it over GPU-written quads
	// would make them INVISIBLE, not garbage. That matters because the paths
	// that could re-upload are the rare-and-silent ones this wave keeps being
	// bitten by -- a capacity rebuild (D4-R1) is the remaining one, and it
	// invalidates every GPU range whatever the shadow holds.
	//
	// It also means RemoveChunk needs no GPU-specific branch: it points the ids
	// at the hidden chunk and marks the range dirty exactly as it does for a CPU
	// chunk, and the resulting upload writes hidden ids over the GPU content,
	// which is precisely the intended effect.
	TArray<uint64> PooledQuads;

	// Parallel to PooledQuads: which chunk each quad belongs to. This is what
	// removes per-draw state and lets one draw span the pool.
	TArray<uint32> QuadChunkIds;

	// xyz = chunk origin in component space (unreal units), w = mip scale.
	TArray<FVector4f> ChunkOrigins;

	// Parallel to ChunkOrigins, indexed by the same chunk id. See
	// FVoxelQuadVertexFactoryParameters::ChunkParams for the channel layout.
	TArray<FVector4f> ChunkParams;

	// Chunk id 0 is RESERVED as the hidden chunk: origin (0,0,0), scale 0.
	// Freed quads point at it and collapse to a degenerate point.
	static constexpr uint32 kHiddenChunkId = 0;

	// Table entries whose chunk was removed, available for the next AddChunk.
	//
	// Without this the chunk table only ever GREW: AddChunk appended a fresh
	// entry every time and RemoveChunk gave nothing back, so the table tracked
	// "chunks ever added" rather than "chunks resident". Terrain streaming hides
	// that -- its churn is slow -- but water re-meshes at 10 Hz and its bricks
	// appear and vanish continuously, so the table crosses the proxy's MaxChunks
	// headroom within a minute or two. Crossing it is not a slow leak but a
	// cliff: PushUpdatesToProxy falls back to MarkRenderStateDirty, which throws
	// the proxy away and re-uploads the WHOLE pool buffer -- the exact cost the
	// incremental path exists to avoid, now on a repeating timer.
	//
	// Recycling is safe because RemoveChunk first repoints every one of the
	// chunk's quads at kHiddenChunkId, so no quad in the pool still references
	// the id being handed back.
	TArray<uint32> FreeChunkIds;

	FVoxelGpuGeometryPool Pool;
	TArray<FVoxelGpuPoolAllocation> Allocations;  // indexed by handle
	// Parallel to Allocations: which table entry each handle owns. Kept
	// explicitly rather than re-derived from QuadChunkIds[Offset] so that
	// freeing the entry does not depend on the allocation still being non-empty.
	TArray<uint32> AllocationChunkIds;
	int32 NumLiveChunks = 0;

	// Allocations-array handles a removal gave back, available for the next
	// AddChunk/AddChunkFromGpu instead of a fresh Allocations.Add(...).
	//
	// Without this Allocations only ever GREW: both add paths appended and
	// RemoveChunkInternal zeroed a freed slot in place but never handed it back,
	// so BuildChunkRuns's Reserve()-and-walk tracked chunks EVER ADDED, not
	// resident, and its cost was unbounded in SESSION LENGTH rather than world
	// size. Measured over a 120s flight leg (§2.2,
	// docs/measurements/s0-apply-census-2026-07-27.txt "NEW FINDING 1"): mean
	// allocations walked climbed 28,042 -> 68,416 while live runs emitted FELL
	// 28,042 -> 18,389 -- a 3.72x and rising ratio of wasted walk, on every
	// publication.
	//
	// Recycling a handle is safe because by the time RemoveChunkInternal pushes
	// it, every quad in its range has already been repointed at kHiddenChunkId
	// and its Allocations entry has already been zeroed -- so nothing in the pool
	// buffer, and no live FChunkRun, still names it.
	//
	// GATED on voxel.Stream.PoolRecycleHandles (default 0): with the gate off
	// RemoveChunkInternal never pushes here, so this stays permanently empty and
	// AddChunk/AddChunkFromGpu always append -- byte-identical to before this
	// existed. The gate lives on the PUSH side only, not the pop side (see
	// AcquireAllocationHandle): flipping it on mid-session must not retroactively
	// hand out handles that were freed while it was off, but once it is on there
	// is no reason to refuse a handle that is legitimately sitting here.
	//
	// NOT pushed at all when RemoveChunkInternal is called with
	// bRecycleChunkId=false -- UpdateChunk's realloc branch, which writes
	// Allocations[Handle] itself a few lines later, reusing that EXACT handle
	// deliberately. Handing the same index to a concurrent AddChunk in between
	// would let two live chunks alias one pool slot; see RemoveChunkInternal.
	TArray<int32> FreeHandles;

	// Acquires an Allocations/AllocationChunkIds slot for a new chunk: pops and
	// reuses a handle FreeHandles holds, or appends a fresh one when the list is
	// empty (always true with voxel.Stream.PoolRecycleHandles off, since nothing
	// pushes then). AddChunk and AddChunkFromGpu both call this instead of
	// Allocations.Add(...) directly -- one acquire path so the two do not drift
	// apart (docs/backlog.md's "two copies of one calibration" failure mode).
	// UpdateChunk's realloc branch does NOT call this: it already owns a handle
	// and writes Allocations[Handle] in place, on purpose (see FreeHandles).
	int32 AcquireAllocationHandle(const FVoxelGpuPoolAllocation& Alloc, uint32 ChunkId);

	// See GetAllocFailureCount. Cumulative for the pool's lifetime; never reset
	// by ClearChunks, because the question these answer is "did this RUN ever
	// drop geometry", not "is it dropping geometry right now".
	int64 AllocFailureCount = 0;
	int64 AllocFailureQuads = 0;

	// Latches MaybeLogSaturation's one Error-level line for the pool's
	// lifetime. Never reset by ClearChunks, for the same reason
	// AllocFailureCount is not: the question is "did this run ever get this
	// close", not "is it this close right now".
	bool bSaturationErrorLogged = false;

	// Fires the pool's ONE loud, latched saturation signal -- Error level,
	// not the throttled Warning cadence AllocFailureCount's own log uses --
	// the first time EITHER an allocation fails or resident quads cross 95%
	// of capacity, whichever happens first. Safe (and meant) to call on
	// every allocation, success or failure: it is a no-op once latched.
	// bAllocFailed only changes the log text, both triggers share one latch.
	void MaybeLogSaturation(bool bAllocFailed);

	// Ranges written since the last upload, in quads. Streaming touches a few
	// chunks per frame out of thousands, so uploading the whole pool for each
	// change would be absurd -- at cascade scale that is 75 MB per edit.
	//
	// A LIST, NOT ONE SPAN (Wave D / D1, path 2). This used to be a single
	// {First, Last} merged with Min/Max, which meant two chunks written at
	// distant pool offsets marked EVERYTHING BETWEEN THEM dirty and re-uploaded
	// it from the CPU shadow. That is a large amount of pointless upload on the
	// CPU path -- and once the GPU writes quads into the pool directly, it is
	// silent corruption: any GPU-written range caught in the gap between two
	// CPU-written chunks gets overwritten with stale shadow content, on every
	// add or remove.
	//
	// The span was always an OVER-APPROXIMATION of what the CPU actually wrote:
	// every dirty region comes from a write to one chunk's range, and the
	// Min/Max merge threw that precision away. Keeping the intervals is not
	// extra bookkeeping, it is declining to discard information already in hand.
	//
	// The property that makes this the whole fix for path 2: an interval only
	// ever covers quads the CPU WROTE, so a GPU-written range is untouchable by
	// construction -- no GPU-range tracking, nothing to keep in sync, and no way
	// for the two to drift apart.
	struct FDirtyRange { uint32 First = 0; uint32 Last = 0; bool bValid = false; };
	TArray<FDirtyRange> DirtyQuadRanges;
	bool bChunkTableDirty = false;

	// Drained by PushUpdatesToProxy -- see FPendingGpuWrite.
	TArray<FPendingGpuWrite> PendingGpuWrites;
	// Drained alongside PendingGpuWrites, into the SAME command. See S2-1.
	TArray<FPendingGpuHide> PendingGpuHides;

	// See GetGpuDirectWrites. The DROPPED half lives on FVoxelGpuPoolBuffers,
	// which outlives this component from a render command's point of view.
	int64 GpuDirectWrites = 0;

	// Game-thread half of GetAndResetPushStats. The render-thread half lives on
	// FVoxelGpuPoolBuffers (RunBoundsCycles) for the usual lifetime reason.
	// Accumulated in PushUpdatesToProxy; the two FPlatformTime::Seconds pairs are
	// gated on voxel.Stream.PoolPushStats so the instrument cannot be what is
	// being measured, but the COUNTS are unconditional -- they are increments on a
	// path that already does a table copy and a sort.
	FPoolPushStats PushStats;

	// Enqueues one render command carrying every pending GPU write, for the two
	// PushUpdatesToProxy branches that return before building their own. Returns
	// the writes so the main branch can fold them into ITS command instead.
	TArray<FPendingGpuWrite> TakePendingGpuWrites();

	TArray<FPendingGpuHide> TakePendingGpuHides();
	void FlushGpuWritesStandalone(TArray<FPendingGpuWrite>&& Writes, TArray<FPendingGpuHide>&& Hides);

	// --- S1-1 batched publication ------------------------------------------
	//
	// The real body of what PushUpdatesToProxy used to do. PushUpdatesToProxy is
	// now the DEFER POINT and this is the DO IT point; FScopedBatch is what sits
	// between them. Split this way round so every existing mutator keeps calling
	// PushUpdatesToProxy and none of them had to learn about batching.
	void FlushUpdatesToProxy();

	// Open scopes. >0 means a mutator's PushUpdatesToProxy only records that a
	// flush is owed. Depth rather than a bool because ApplyMeshResult is reachable
	// from the edit path as well as the streaming tick.
	// (FScopedBatch is a nested class, so it already has access to these -- no
	// friend declaration, which would name a DIFFERENT class at namespace scope.)
	int32 BatchDepth = 0;
	bool bFlushPending = false;

	// The interval SUBTRACT, and the reason the batching above is not simply a
	// deferral. THIS IS THE ONE GENUINELY DANGEROUS PART OF S1-1.
	//
	// Unbatched, every add and remove flushes its own render command, so a free's
	// hidden-id upload always lands in an EARLIER command than a later add's GPU
	// write. Batched into one command that ordering inverts: within a single
	// command VoxelGpuPoolAddWritePasses runs FIRST (that is D1's whole
	// correctness argument -- see FPendingGpuWrite) and the merged
	// DirtyQuadRanges upload runs AFTER it. So a range that was freed and then
	// re-issued to a GPU-meshed chunk IN THE SAME FRAME would have the freed
	// range's stale hidden ids written straight over the geometry the compute
	// pass just wrote.
	//
	// The symptom is the worst kind this pool produces: INVISIBLE TERRAIN THAT
	// REPORTS AS LOADED. No counter moves, the chunk is resident, the run is
	// valid, and the quads decode to a degenerate point.
	//
	// The fix is to subtract, not to reorder. For a range the GPU is about to
	// write, the authoritative content is that pending write; any dirty interval
	// still covering it is stale CPU shadow by definition. So AddChunkFromGpu
	// removes its allocated range from DirtyQuadRanges immediately after a
	// successful Pool.Alloc, splitting any interval that straddles it. Mirrors
	// MarkQuadsDirty's insert-and-coalesce.
	//
	// Counted, because a hazard that never fires and a fix that is not wired up
	// look identical from the outside: PoolDirtyOverlapsResolved non-zero on the
	// first leg proves the race is real AND handled; zero across a full flight
	// means either it cannot occur or this is not being called, and both are
	// worth knowing before the gate is flipped.
	void UnmarkQuadsDirty(uint32 First, uint32 Count);
	int64 DirtyOverlapsResolved = 0;
	int64 DirtyOverlapQuadsResolved = 0;

	// S2-3. Chunks currently hidden by ParkChunk -- still allocated, still
	// holding a table entry, just drawing nothing.
	int32 NumParkedChunks = 0;

	// Set whenever the set of live ALLOCATIONS changes -- which is not the same
	// event as the chunk table changing, and conflating the two is what made the
	// proxy's cull test one chunk's bounds against another chunk's quads.
	// UpdateChunk's realloc branch moves a chunk to a new pool offset while
	// deliberately keeping its table entry (so an actively-dug chunk does not burn
	// a fresh entry per edit), so it leaves the table clean and the runs wrong: the
	// run still names the offset the chunk used to occupy, which the allocator is
	// free to hand to somebody else. A run's ChunkId then describes a different
	// chunk from the quads at its FirstQuad, and a perfectly correct frustum test
	// on a perfectly correct bounding box selects the wrong geometry.
	bool bRunsDirty = false;

	// Rebuilt from Allocations whenever the chunk table changes, and handed to
	// the proxy with it. Live chunks only -- a freed handle contributes nothing.
	TArray<FChunkRun> BuildChunkRuns() const;

	void MarkQuadsDirty(uint32 First, uint32 Count);
	void PushUpdatesToProxy();

	// RemoveChunk's body. bRecycleChunkId is false for the one caller that means
	// to keep the table entry alive across the removal -- UpdateChunk's realloc
	// branch, which re-uses the same entry for the chunk's new range so that an
	// actively-dug chunk does not consume a fresh table slot on every edit.
	void RemoveChunkInternal(int32 Handle, bool bRecycleChunkId);

	// Set while a proxy is live so updates can go straight to it instead of
	// rebuilding the render state. Render-thread lifetime is owned by the
	// renderer; this is only ever dereferenced inside a render command.
	class FVoxelGpuPoolSceneProxy* LiveProxy = nullptr;

	FBox LocalBounds = FBox(ForceInit);

	FString PoolName = TEXT("VoxelGpuPool");
	int32 ChunkEdgeVoxels = 32;
	int32 ChunkTableCapacity = 1024;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> ChunkMaterial = nullptr;
};

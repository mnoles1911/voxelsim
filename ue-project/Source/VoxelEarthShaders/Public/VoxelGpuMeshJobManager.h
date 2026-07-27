// Asynchronous GPU meshing of chunk regions (ADR-0006, G3 stage 1).
//
// WHAT THIS IS FOR. VoxelGpuWorldGen::RunRegionBlocking runs the same kernels,
// but it calls SubmitAndBlockUntilGPUIdle and FlushRenderingCommands and is
// documented as a verification tool. This class runs the identical seven-pass
// graph (literally the same AddRegionPasses function) with NO blocking call
// anywhere: jobs are dispatched into an RDG graph, their results are staged
// into FRHIGPUBufferReadback objects, and Tick() polls IsReady() until they
// land.
//
// TWO INVARIANTS ARE LOAD-BEARING, AND BOTH ARE THINGS THE BLOCKING PATH GETS
// AWAY WITH BECAUSE IT FLUSHES.
//
// 1. Every job OWNS everything it hands the render thread. RunRegionBlocking
//    captures its request and its output arrays BY REFERENCE into the render
//    command; that is only safe because the calling stack frame is parked in
//    FlushRenderingCommands until the command has run. Here the caller returns
//    immediately, so a job is a shared, ref-counted object that owns its
//    request, its readbacks and its staging arrays, and the render commands
//    hold a reference to it. Nothing is captured by reference.
//
// 2. Every dispatch produces EXACTLY ONE outcome. A streaming system that can
//    lose a job strands the terrain column that was waiting on it, and there is
//    no signal — the chunk simply never arrives. So there is no "returns false
//    on error" path here: Submit() always returns a job id, and every job id is
//    eventually delivered to OnJobComplete exactly once, whether it succeeded,
//    failed validation, failed to dispatch, timed out, or was cancelled by
//    shutdown. Callers cannot drop a job by forgetting to check a return value,
//    because there is no return value to forget.
//
// SCOPE. Stage 1 is the runner only. Nothing here is wired into DispatchJobs or
// the streaming path yet.
//
// WAVE D / D2 UPDATE. The brick-local -> chunk-local rebase no longer happens
// on the CPU by default: Submit sets FVoxelGpuRegionRequest::bChunkLocalQuads
// from voxel.GPU.MeshChunkLocal (default 1), which selects a MeshEmitMain
// permutation that bakes the offset in on the GPU. Setting that cvar to 0
// restores the CPU rebase, and it is a genuine control rather than a fallback:
// voxel.GPU.VerifyAsyncMesh byte-compares both against MeshChunkBricks, so the
// two paths agreeing is what makes the permutation trustworthy.
//
// WAVE D / D1 UPDATE. THE QUAD READBACK IS GONE ON THE DEFAULT PATH.
//
// Under voxel.GPU.MeshDirectToPool (default 1) a job never stages its quads
// through system memory at all. Phase 1 still reads the 4-byte total -- that is
// the only thing the CPU needs in order to allocate a pool range, and it is the
// completion event the exactly-one-outcome contract hangs on. Phase 2 stops
// being a readback and becomes a GPU->GPU COMPACTION into a buffer sized to the
// quads that actually exist; the result then carries an FVoxelGpuQuadPayload
// (a handle to GPU memory) instead of a TArray<uint64>, and
// UVoxelGpuPoolComponent::AddChunkFromGpu copies it into the chunk's allocated
// pool range with a compute pass. Per chunk that removes ~10 KB of readback,
// ~16 KB of re-upload, an unpack, a repack, and two Lock/Memcpy/Unlock pairs on
// the render thread -- to end with the same bytes in the same buffer.
//
// It also removes a WHOLE ROUND TRIP from delivery latency: there is no second
// readback to wait for and no second poll quantum, because nothing on the CPU
// reads the compaction's output.
//
// Setting voxel.GPU.MeshDirectToPool 0 restores the D3 two-phase readback
// exactly, on the same binary, and it is a genuine control rather than a
// fallback -- voxel.GPU.VerifyPoolWrite compares BOTH against MeshChunkBricks.
//
// The direct path REQUIRES bChunkLocalQuads, and that requirement is ANDed in
// here rather than trusted to the caller: brick-local quads need
// RebaseQuadsToChunkLocal, which is a CPU pass over a stream the direct path
// never has. See Submit.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "VoxelGpuQuadPayload.h"
#include "VoxelGpuWorldGen.h"

// Geometry of the region that meshes exactly one 32^3 render chunk.
//
// A render chunk is 4x4x4 bricks. The mesher needs a one-brick halo on every
// side to read apron/AO neighbours, so the dispatch is 6x6x6 bricks = 48x48
// columns by 6 bricks in Z, and the 4x4x4 interior bricks it meshes ARE the
// chunk's bricks, in the chunk's own order.
//
// That gives MaskCount = 4*4*4*48 = 3,072, comfortably under the 65,536 masks
// the single-workgroup ScanSumsMain can scan, so a chunk never needs splitting
// into z-slabs the way the 64x64 bench fixtures would at depth.
namespace VoxelGpuChunkRegion
{
	inline constexpr uint32 kChunkEdgeVoxels = 32;
	inline constexpr uint32 kBrickEdge = 8;
	inline constexpr uint32 kColumns = 48;   // 32 + 2*8 halo
	inline constexpr uint32 kBricksZ = 6;    // 4 + 2 halo
	inline constexpr uint32 kInteriorBricks = 4;
	inline constexpr uint32 kMaskCount = kInteriorBricks * kInteriorBricks * kInteriorBricks * 48;

	// Sets DispatchColumns / OriginVx / OriginVy / BrickZMin / BricksZ on Req for
	// the given level-0 chunk key. The caller still has to fill the raster window
	// (RasterOriginPx, RasterSize, PixelSizeMm, ElevationMm, ClimatePacked) and
	// the seed -- that needs voxel-core, which this module deliberately does not
	// link.
	VOXELEARTHSHADERS_API void SetChunkFootprint(FVoxelGpuRegionRequest& Req,
	                                             int32 ChunkX, int32 ChunkY, int32 ChunkZ);
}

// Why a job ended. Exactly one of these is delivered per Submit().
enum class EVoxelGpuMeshJobStatus : uint8
{
	Success,
	// The request never reached the GPU: it failed ValidateRegionRequest, or the
	// RHI cannot run the kernels at all.
	Rejected,
	// The graph was built but something went wrong before the readback landed.
	DispatchFailed,
	// The readback was enqueued but Lock() failed or came back null.
	ReadbackFailed,
	// IsReady() never went true inside the timeout. This is what device loss
	// and a hung queue look like from here, and it is the reason a timeout
	// exists at all: without it those cases are indistinguishable from "still
	// working", which is exactly how a job gets silently lost.
	TimedOut,
	// The manager was shut down or destroyed with this job still outstanding.
	Cancelled,
};

VOXELEARTHSHADERS_API const TCHAR* LexToString(EVoxelGpuMeshJobStatus Status);

struct FVoxelGpuMeshJobResult
{
	uint64 JobId = 0;
	// Whatever the caller passed to Submit, echoed back untouched. The streaming
	// integration will put a packed chunk key here.
	uint64 UserTag = 0;

	EVoxelGpuMeshJobStatus Status = EVoxelGpuMeshJobStatus::Cancelled;
	FString Error;

	// CHUNK-LOCAL packed quads, byte-identical in layout to
	// PackVoxelChunkQuad(FVoxelChunkQuad) -- i.e. exactly what
	// UVoxelGpuPoolComponent::AddChunk consumes. Produced chunk-local by the
	// GPU under voxel.GPU.MeshChunkLocal 1, or rebased on the CPU in Tick under
	// 0; the contract on this field is the same either way.
	//
	// EMPTY WHEN GpuQuads IS SET (Wave D / D1). The quads then never came to the
	// CPU at all, and NumQuads below is the only thing that describes them.
	TArray<uint64> Quads;

	// How many quads this chunk meshed to. VALID ON BOTH PATHS -- equal to
	// Quads.Num() on the readback path, and the only quad count that exists on
	// the direct path.
	//
	// Every consumer that only wanted a COUNT should read this rather than
	// Quads.Num(): the streaming path's zero-quad census, the buried/solid/sky
	// skip soundness checks, ResidentQuads and LastQuadCount all ask "how much
	// geometry", not "give me the geometry". Reading Quads.Num() instead makes
	// every GPU-meshed chunk look like an empty one -- plausible,
	// self-consistent, and wrong in the direction that hides missing terrain.
	uint32 NumQuads = 0;

	// Wave D / D1. Non-null when the quads were left in GPU memory: a handle to
	// the buffer holding exactly NumQuads packed quads, to be handed to
	// UVoxelGpuPoolComponent::AddChunkFromGpu. Null on the readback path and on
	// every non-Success outcome.
	//
	// The game thread MOVES THIS AROUND AND NOTHING ELSE -- see the threading
	// note on FVoxelGpuQuadPayload. Dropping it (a stale result, a full pool) is
	// safe and releases the GPU memory on the render thread.
	FVoxelGpuQuadPayloadRef GpuQuads;

	// --- Wave D / D6: the footprint band ------------------------------------
	//
	// RAW voxel z as BandReduceMain wrote them: the max of
	// ColumnSurfaceTopVoxel and the min of ColumnDeepestAirVoxel over the band
	// window. VoxelStreaming::MakeFootprintBand turns them into an
	// FFootprintBand; the widening constants stay on the VoxelEarth side
	// because every constant mirrored into HLSL is a determinism liability, and
	// this module does not link voxel-core anyway.
	//
	// ON THE SAME RESULT OBJECT ON PURPOSE, AND THIS IS THE LOAD-BEARING PART.
	// The band could have been its own async stream. It must not be: that would
	// give a job TWO things to deliver exactly once, and make "delivered quads
	// but no band" representable. Doubling the exactly-one-outcome surface is
	// the class of change that produced the stranded-(X,Y)-column bug this
	// project has already fixed once. Riding the existing result means
	// DrainResults keeps doing FootprintBandCache.Add,
	// FootprintBlindJobInFlight.Remove and --LevelJobsInFlight in one pass over
	// one result, unchanged.
	//
	// The two readbacks are enqueued in the SAME graph as the quad total and
	// harvested ALL-OR-NONE in phase 1 — the same rule the brick-local control
	// path's Counts/Offsets already follow — so there is still exactly one
	// completion event, not two.
	//
	// bBandValid is false when the request did not ask for a band (BandEdge 0),
	// which is every job but the first in an (X,Y) footprint. False means "this
	// job carries no band", never "the band is zero" — a band of zeroes claims
	// the world is empty.
	bool bBandValid = false;
	int32 BandMaxSurfaceTopVoxel = 0;
	int32 BandMinDeepestAirVoxel = 0;

	// Wall-clock, all measured from the game thread's point of view except
	// DispatchToReadyMs.
	//
	// DispatchToReadyMs is render-thread timestamps: from the moment
	// GraphBuilder.Execute() returned to the moment a poll first saw IsReady()
	// true. That is the number that describes the GPU, and it is quantised by
	// the poll interval (one game-thread tick).
	//
	// *** DO NOT COMPARE ACROSS D3; QUOTE SubmitToDeliverMs. ***
	//
	// D3 changed what this measures. It is now the moment the 4-BYTE TOTAL
	// landed — when the GPU finished the mesh chain. It used to be the moment
	// the whole ~810 KB readback landed, so it included a transfer that no
	// longer happens on this path.
	//
	// So this number WILL be lower after D3, and lower for a reason that has
	// nothing to do with the GPU being faster. A before/after quoted from this
	// field is a flattering artefact of a metric that changed definition, which
	// is exactly how this programme has previously published figures it then
	// had to retract. SubmitToDeliverMs covers both phases and is the honest
	// end-to-end figure; quote that one.
	double DispatchToReadyMs = 0.0;
	// Submit() to the OnJobComplete call. Includes queueing behind the in-flight
	// cap, the render thread being a frame behind, and the poll quantisation.
	double SubmitToDeliverMs = 0.0;

	// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): the two stage
	// splits that tell a genuine per-job GPU cost apart from Little's-law
	// queueing (deep-review-streaming-perf-2026-07-27.md S1d) -- SubmitToDeliverMs
	// alone cannot distinguish "this job is slow" from "this job waited behind a
	// deep queue". Both are 0.0 unless voxel.GPU.MeshLatencyStats is on: the
	// manager's own timestamps this relies on (FJob::PromotedSeconds) are only
	// stamped under that cvar, so a stats-off run reports these as unmeasured
	// rather than as a false zero.
	//
	// Submit() to the moment this job was promoted out of the manager's Queued
	// array in Tick(). This is QUEUE WAIT ONLY -- time spent behind MaxInFlight
	// with no GPU work happening yet. It does NOT mean "this job is cheap to
	// run"; a QueuedMs-dominated SubmitToDeliverMs means the fork is depth-bound
	// at the current MaxInFlight/throughput ratio, not that any one job costs
	// little.
	double QueuedMs = 0.0;
	// First observed IsReady()==true (FJob::ReadySeconds, stamped in
	// PollInFlight phase 1 -- see that stamp's own comment for why it is BEFORE
	// the harvest budget check) to this Deliver() call. This is POLL
	// QUANTISATION PLUS HARVEST-BUDGET DEFERRAL, not GPU work -- the GPU
	// finished before ReadySeconds was even stamped. It does NOT mean the
	// harvest copy itself is slow; a job deferred by voxel.GPU.MeshHarvestCap
	// sits here, ready and waiting, for however many ticks the budget stays
	// exhausted.
	double ReadyToDeliverMs = 0.0;

	bool IsOk() const { return Status == EVoxelGpuMeshJobStatus::Success; }
};

DECLARE_DELEGATE_OneParam(FVoxelGpuMeshJobComplete, FVoxelGpuMeshJobResult&&);

class VOXELEARTHSHADERS_API FVoxelGpuMeshJobManager
{
public:
	// OnComplete is required and is called for EVERY submitted job, on the game
	// thread, from inside Tick() (or from the destructor / Shutdown() for jobs
	// still outstanding at that point).
	//
	// MaxInFlight caps how many jobs have GPU work outstanding at once. Queued
	// jobs beyond that wait; they are not rejected.
	explicit FVoxelGpuMeshJobManager(FVoxelGpuMeshJobComplete OnComplete,
	                                 int32 InMaxInFlight = 8,
	                                 double InTimeoutSeconds = 10.0);

	// Delivers Cancelled for anything still outstanding. Does NOT flush the
	// render thread: any render command still in flight holds its own reference
	// to the job it touches, so it stays valid after this returns.
	~FVoxelGpuMeshJobManager();

	FVoxelGpuMeshJobManager(const FVoxelGpuMeshJobManager&) = delete;
	FVoxelGpuMeshJobManager& operator=(const FVoxelGpuMeshJobManager&) = delete;

	// Takes ownership of Region. Always succeeds in the sense that the returned
	// id WILL be reported to OnComplete exactly once -- a request that cannot
	// possibly run is reported as Rejected on the next Tick rather than refused
	// here. Game thread only.
	//
	// bRequestGpuResidentQuads (Wave D / D1) asks for the quads to be left in GPU
	// memory and delivered as an FVoxelGpuQuadPayload instead of a TArray. It is
	// a REQUEST, not an instruction: the manager ANDs it with
	// voxel.GPU.MeshDirectToPool and with Region.bChunkLocalQuads, and a caller
	// must therefore check FVoxelGpuMeshJobResult::GpuQuads rather than assume
	// which form it will get back. Callers that cannot consume GPU-resident
	// quads -- anything needing the quad CONTENTS on the CPU, which today is the
	// voxel GI ingest -- pass false and get the readback path.
	uint64 Submit(FVoxelGpuRegionRequest&& Region, uint64 UserTag = 0,
	              bool bRequestGpuResidentQuads = false);

	// Promotes queued jobs, polls readbacks, delivers finished ones. Game thread
	// only. Cheap and safe to call every frame with nothing outstanding.
	void Tick();

	// Delivers Cancelled for every queued and in-flight job. Idempotent.
	void CancelAll();

	int32 NumQueued() const { return Queued.Num(); }
	int32 NumInFlight() const { return InFlight.Num(); }
	bool HasWork() const { return NumQueued() > 0 || NumInFlight() > 0; }

	int32 GetMaxInFlight() const { return MaxInFlight; }
	void SetMaxInFlight(int32 InMaxInFlight) { MaxInFlight = FMath::Max(1, InMaxInFlight); }

	// Defined entirely in the .cpp. Public only so the file-local rebase helper
	// can name it; nothing outside the implementation can do anything with it.
	struct FJob;

private:
	void DispatchBatch(TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>>&& Batch);
	// D3 phase 2: fetch exactly the quads the 4-byte total said exist. This is
	// now the voxel.GPU.MeshDirectToPool 0 control path -- kept honest rather
	// than vestigial, because it is what the direct path is compared against.
	void DispatchQuadFetch(TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>>&& Batch);
	// D1 phase 2: right-size the payload on the GPU and read back NOTHING. The
	// job is deliverable the moment this is ENQUEUED, not when it lands.
	//
	// Takes PAYLOADS rather than jobs on purpose -- Deliver() moves a job's
	// payload away on the game thread in the same Tick this is called from, so a
	// render command reaching through the job would race it. See the definition.
	void DispatchQuadCompact(TArray<FVoxelGpuQuadPayloadRef>&& Batch);
	void PollInFlight();
	void Deliver(const TSharedPtr<FJob, ESPMode::ThreadSafe>& Job,
	             EVoxelGpuMeshJobStatus Status, const FString& Error);

	FVoxelGpuMeshJobComplete OnJobComplete;
	int32 MaxInFlight = 8;
	double TimeoutSeconds = 10.0;
	uint64 NextJobId = 1;

	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> Queued;
	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> InFlight;
};

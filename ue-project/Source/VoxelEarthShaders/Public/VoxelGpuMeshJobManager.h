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
// TWO QUEUES, NOT A PRIORITY FIELD (T4-1, 2026-07-28). Submit takes a
// bLowPriority flag that routes a job to QueuedLowPriority instead of Queued.
// Demand drains first every tick, up to voxel.GPU.MeshBatchCap; low-priority
// work then drains up to voxel.GPU.MeshSpeculativeBatchCap ON TOP of that,
// bounded by MaxInFlight. With nothing low-priority queued the behaviour is
// byte-identical to before.
//
// The subtle part, and the version that was built first and was wrong: giving
// low-priority the LEFTOVER of MeshBatchCap makes it a dead path. The demand
// queue carries ~236 jobs for an entire flight leg, so "promote low-priority
// only when Queued is empty" never fires once. MeshBatchCap is a per-tick BURST
// limiter protecting render-thread pass setup, not a capacity limit -- the
// capacity limit is MaxInFlight, and the GPU was measured running 16 jobs in
// flight at cap 4 against 179 at cap 32. Speculation exists to use that gap, so
// it needs an allowance of its own rather than a share of somebody else's.
//
// Invariant 2 still holds across both queues: CancelAll drains BOTH into the
// cancelled set. Resetting QueuedLowPriority without delivering it would strand
// every speculative chunk silently, which is precisely the failure invariant 2
// exists to make impossible.
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
#include "VoxelBrickPool.h"
#include "VoxelGpuQuadPayload.h"
#include "VoxelGpuWorklist.h"
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

	// --- P1-C: the halo-free footprint the brick volume is packed from ------
	//
	// THE MARCHER NEEDS NO APRON, AND THE PRODUCER CANNOT SHARE THE MESHER'S.
	// Two separate facts, and both of them force this function to exist.
	//
	// The one that MAKES it possible: a marcher reads neighbours by index --
	// face normals come from the DDA's crossed axis, AO from occupancy bits
	// already in registers, colour from the voxel itself -- so it needs none of
	// the one-brick halo the greedy mesher reads for AO and face neighbours.
	// The dispatch is 32x32x4 rather than 48x48x6: 3.375x less voxelize work
	// per chunk (docs/ray-marching-plan-2026-08-19.md section 8).
	//
	// The one that FORCES it: brickpack.ush's decodeBrick has no brick origin.
	// It decomposes a region into whole chunks counted from the region's OWN
	// corner, so pointing it at the mesher's 48x48x6 footprint would pack
	// bricks 0..3 -- the HALO corner -- rather than the interior 1..4. Every
	// per-brick test would pass and the world would be displaced by one brick
	// on every axis. That is why this is a derived region and not a flag.
	//
	// Returns false, and leaves OutReq untouched, if MeshReq is not the
	// standard single-chunk footprint SetChunkFootprint produces. The raster
	// window is copied verbatim -- the halo-free footprint is strictly inside
	// the halo one, and a window that over-covers is legal where one that
	// under-covers is not -- and every asset instance's anchor is rebased by
	// the one brick the origin moved.
	VOXELEARTHSHADERS_API bool MakeBrickRegion(const FVoxelGpuRegionRequest& MeshReq,
	                                           FVoxelGpuRegionRequest& OutReq);
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

// --- the two brick gates, readable from outside this module -----------------
//
// voxel.GPU.BrickPack and voxel.GPU.BrickPackResident are declared next to the
// job manager because that is where the GPU producer reads them. They are
// exported here because THE CPU PRODUCER MUST OBEY THE SAME TWO SWITCHES: a
// brick volume half-fed by an arm that ignores the master gate is worse than one
// that is off, and "packed and discarded" has to mean discarded on both arms or
// the publication-stubbed measurement arm silently stops being a control.
//
// Accessors rather than extern int32s so the cvar stays owned by one translation
// unit, and rather than IConsoleManager lookups at the call site so a per-chunk
// read is a load and not a hash.
//
// GAME THREAD. Both are read once per job and captured by value into worker
// task bodies, which is this project's established pattern for a gate a worker
// needs (bPredictedEmpty, bComputeBand, bLatencyStatsEnabled are the same shape).
VOXELEARTHSHADERS_API bool VoxelGpuBrickPackEnabled();
VOXELEARTHSHADERS_API bool VoxelGpuBrickPackResidentEnabled();

// Tier B.1: whether the manager may AMORTISE the GPU worldgen/pack passes by
// fusing Z-sibling brick-only jobs into one tall region per column stack --
// one set of passes for K chunks instead of K sets. voxel.GPU.WorldGenBatch
// (default 0: today's per-chunk graphs, byte-identical control) with a
// -VoxelGpuWorldGenBatch=<n> command-line override for the -ExecCmds
// startup-window reason set out on VoxelGpuBrickPackEnabled. Same accessor
// idiom as the two brick gates above, for the same "the gate is no longer a
// cvar" lesson recorded there.
VOXELEARTHSHADERS_API bool VoxelGpuWorldGenBatchEnabled();

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

	// --- P1-C: the packed brick volume, left in GPU memory ------------------
	//
	// Non-null when the job packed bricks (voxel.GPU.BrickPack) and the pack
	// succeeded. Under voxel.GPU.BrickPackResident (the default) the manager has
	// ALREADY published a reference of this into the global brick pool by the
	// time the result is delivered -- residency is the streaming path's job, not
	// the caller's -- and this handle is here so a gate can stand up a private
	// pool and publish the same bytes into it instead.
	//
	// It rides this result rather than its own async stream for the reason the
	// band does: a second stream would give a job TWO things to deliver exactly
	// once, and "delivered quads but no volume" would become representable.
	// Nothing about the mesh path depends on it, and dropping it is safe -- it
	// releases the GPU memory on the render thread.
	FVoxelGpuBrickPayloadRef BrickVolume;

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

	// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2), completed
	// 2026-08-23 into a FULL PARTITION of SubmitToDeliverMs: the stage splits
	// that tell a genuine per-job GPU cost apart from Little's-law queueing
	// (deep-review-streaming-perf-2026-07-27.md S1d) -- SubmitToDeliverMs alone
	// cannot distinguish "this job is slow" from "this job waited behind a deep
	// queue", and on 2026-08-23 that distinction was worth 2.3 SECONDS: the
	// fork's submit->deliver read p50=2281.5 ms with gpuDemandPending=240 while
	// the demand queue drains at voxel.GPU.MeshBatchCap (4) per tick -- so
	// Little's law alone predicts over a second of pure queue wait before any
	// GPU work starts, and nothing in the log could confirm or deny it.
	//
	// THE FOUR STAGES TELESCOPE. All four are differences of the same five
	// stamps (Submit, Promoted, Dispatched, Ready, Deliver), so on any job where
	// bLatencyStagesComplete is true:
	//
	//   QueuedMs + PromoteToDispatchMs + DispatchToReadyMs + ReadyToDeliverMs
	//     == SubmitToDeliverMs, exactly.
	//
	// A breakdown with an unexplained remainder is how this project produced
	// (and retracted) the jobGHz=0.04 conclusion; the telescoping property is
	// the guard against a second one. The streaming side's 5-second window
	// prints the four means next to the total and their residual.
	//
	// ALWAYS MEASURED as of 2026-08-23. The claim that used to stand here --
	// "both are 0.0 unless voxel.GPU.MeshLatencyStats is on" -- went stale when
	// the timeout fix made FJob::PromotedSeconds unconditional (the timeout must
	// not change behaviour with a stats cvar), and every other stamp was already
	// unconditional. voxel.GPU.MeshLatencyStats now gates nothing here; see its
	// definition for the history.
	//
	// Submit() to the moment this job was promoted out of the manager's Queued
	// array in Tick(). This is QUEUE WAIT ONLY -- time spent behind MaxInFlight
	// and the per-tick MeshBatchCap with no GPU work happening yet. It does NOT
	// mean "this job is cheap to run"; a QueuedMs-dominated SubmitToDeliverMs
	// means the fork is depth-bound at the current queue-depth/promotion-rate
	// ratio, not that any one job costs little.
	double QueuedMs = 0.0;
	// Promotion (game thread, in Tick) to FJob::DispatchSeconds -- stamped on
	// the RENDER thread the moment GraphBuilder.Execute() returned for the
	// batch this job rode in. So this is the render thread being behind plus
	// the graph build and pass setup for the whole batch: the cross-thread
	// handoff the other three stages cannot see. Without it the four stages did
	// not sum to the total and the gap was silently attributed to nothing.
	double PromoteToDispatchMs = 0.0;
	// First observed IsReady()==true (FJob::ReadySeconds, stamped in
	// PollInFlight phase 1 -- see that stamp's own comment for why it is BEFORE
	// the harvest budget check) to this Deliver() call. This is POLL
	// QUANTISATION PLUS HARVEST-BUDGET DEFERRAL, not GPU work -- the GPU
	// finished before ReadySeconds was even stamped. It does NOT mean the
	// harvest copy itself is slow; a job deferred by voxel.GPU.MeshHarvestCap
	// sits here, ready and waiting, for however many ticks the budget stays
	// exhausted.
	double ReadyToDeliverMs = 0.0;
	// True iff all five stamps existed at delivery, i.e. the four stage fields
	// above are a complete, exactly-summing partition of SubmitToDeliverMs.
	// False on any job that skipped a stage (Rejected never dispatched;
	// TimedOut never went ready; Cancelled can die anywhere), whose stage
	// fields are then partial and MUST NOT be summed into a breakdown -- that
	// is how an unexplained remainder gets born. Consumers aggregate stages
	// only from results with this set, and count the rest separately.
	bool bLatencyStagesComplete = false;

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
	// bLowPriority: promoted ONLY when no demand job is waiting.
	//
	// WHY THIS EXISTS RATHER THAN "SUBMIT IT LAST". T4-1 originally relied on
	// submitting speculative work at the end of the tick, on the reasoning that
	// demand would already have taken what it wanted. That controls WHEN a job
	// enters the queue and does nothing about what happens after: this queue is
	// strict FIFO, so a speculative job sits AHEAD of every demand job submitted
	// on later ticks. Measured with ~235 jobs queued at the shipped
	// MeshBatchCap, speculation would not have filled idle GPU -- it would have
	// inserted latency into the demand path
	// (docs/measurements/t41-premise-2026-07-28.txt).
	//
	// Low-priority jobs are otherwise identical: same states, same delivery, same
	// exactly-once contract. They are never starved into a different OUTCOME,
	// only a later one, and the caller is expected to treat them as droppable.
	uint64 Submit(FVoxelGpuRegionRequest&& Region, uint64 UserTag = 0,
	              bool bRequestGpuResidentQuads = false, bool bLowPriority = false);

	// Promotes queued jobs, polls readbacks, delivers finished ones. Game thread
	// only. Cheap and safe to call every frame with nothing outstanding.
	void Tick();

	// Delivers Cancelled for every queued and in-flight job. Idempotent.
	void CancelAll();

	int32 NumQueued() const { return Queued.Num() + QueuedLowPriority.Num(); }
	// Demand only. This is the number that decides whether the GPU has room a
	// LOW-PRIORITY job could use without delaying anything -- NumQueued() lumps
	// both and would always look busy.
	int32 NumQueuedDemand() const { return Queued.Num(); }
	int32 NumQueuedLowPriority() const { return QueuedLowPriority.Num(); }
	int32 NumInFlight() const { return InFlight.Num(); }
	bool HasWork() const { return NumQueued() > 0 || NumInFlight() > 0; }

	// THE THREE STAGES OF Tick(), IN MILLISECONDS SINCE THE LAST READ.
	//
	// Tick() was measured at ~18-19 ms per hitch frame -- the largest single
	// item in the streaming tick, and 90% of the bucket the project calls
	// "dispatch" (which also spans this call; see VoxelWorldSubsystem's
	// ThisFrameGpuManagerTickMs). Throughput is MaxJobsInFlight x frame rate,
	// so this is the world's generation ceiling and it deserves a breakdown
	// rather than one number -- the same argument that produced the dispatch
	// brackets, applied one level down.
	//
	// Read-and-reset, so the caller owns the window and two readers cannot
	// halve each other's totals.
	struct FTickStageMs
	{
		double PromoteMs = 0.0;   // selecting jobs into the batch
		double PollMs = 0.0;      // PollInFlight: harvesting readbacks
		double BrickFlushMs = 0.0; // the pool Flush this tick owns
		double EnqueueMs = 0.0;   // building and enqueueing the render command
	};
	FTickStageMs GetAndResetTickStageMs()
	{
		const FTickStageMs Out = TickStageMs;
		TickStageMs = FTickStageMs();
		return Out;
	}

	// --- Tier B.1 (voxel.GPU.WorldGenBatch) window counters -----------------
	//
	// THE COUNTERS EXIST SO A SILENT NO-OP IS IMPOSSIBLE. This project has a
	// recorded history of switches that were on and did nothing (the fork that
	// carried zero traffic under retirement; the GPU gate that tested nothing
	// for a long stretch), so the batch path counts every unit it dispatches
	// AND every chunk that declined to batch, with the reason. If the switch
	// is armed and the log shows zero stacks, the fallback columns say exactly
	// why -- there is no state in which the feature can quietly not run.
	//
	// Consumed by the manager's own periodic log line ("[gpu-batch] ...",
	// ~5 s cadence while armed). Deliberately NOT also exposed through a
	// public read-and-reset accessor: two readers of a reset-on-read window
	// halve each other's totals, which is the exact trap FTickStageMs's
	// comment above documents.
	//
	// StackPasses / ClassicPasses are DERIVED from per-shape constants next to
	// the dispatch code (kPassesPerStackDispatch etc. in the .cpp), not
	// counted at the AddPass sites -- keep the constants in step with the
	// pass-adding code they describe; each is commented at its definition.

	// Fell back to the per-chunk path because:
	enum class EBatchFallback : uint8
	{
		QuadMesh,   // the job still produces quads (voxel.Terrain.RetireQuads 0)
		Band,       // the job carries its footprint's band request
		Assets,     // the job stamps asset instances (span-table merge not built)
		Mismatch,   // a Z-sibling's raster window / seed differed from the head's
		Single,     // no contiguous Z-sibling was queued alongside it
		Invalid,    // the assembled stack region failed validation (loud-logged)
		COUNT
	};

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
	FTickStageMs TickStageMs;
	double TimeoutSeconds = 10.0;
	uint64 NextJobId = 1;

	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> Queued;
	// Drained only when Queued is empty. See Submit's bLowPriority: this is what
	// lets speculative work use spare GPU without delaying demand, which
	// submit-last ordering alone cannot do against a FIFO queue.
	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> QueuedLowPriority;
	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> InFlight;

	// Tier B.1 window counters (game thread; the two crosscheck counters live
	// as file-scope atomics in the .cpp because they are incremented on the
	// render thread by poll commands that must not capture `this`).
	int32 BatchStacks = 0;
	int32 BatchStackChunks = 0;
	int32 BatchStackPasses = 0;
	int32 BatchClassicJobs = 0;
	int32 BatchClassicPasses = 0;
	int32 BatchFallbacks[uint8(EBatchFallback::COUNT)] = { 0 };
	double LastBatchLogSeconds = 0.0;
	bool bBatchArmingLogged = false;
	// P1: the WorldGenBatch + PoolAlloc conflict warning, once per run.
	bool bPoolAllocStackConflictLogged = false;

	void NoteBatchFallback(EBatchFallback Reason) { ++BatchFallbacks[uint8(Reason)]; }
	void MaybeLogBatchWindow();

	// --- P3 spine (-VoxelGpuWorklist; see VoxelGpuWorklist.h's banner) ------
	//
	// The ring itself: records appended in DispatchBatch (game thread, after
	// the P1 shell loop so ChunkSlot is real), flushed once per Tick. All the
	// counters below are game-thread and window-read by MaybeLogWorklistWindow,
	// the single reader (FTickStageMs's two-readers rule).
	FVoxelGpuWorklist Worklist;
	// Why a promoted chunk did NOT get a record, by first failing reason --
	// the [gpu-lean] diagnosis pattern: "armed but records=0" must read as a
	// named missing precondition, not a mystery.
	// P3 Voxelize stage (cumulative, wlvox line). Converted: record consumed
	// this flush AND asset-free -- the chunk's VoxelizeMain is skipped.
	// FallbackAssets: consumed but carries assets (designed exclusion until
	// the flush-level asset buffer lands). Fallback: everything else a
	// column-converted chunk would also have fallen back for (deferred,
	// refused, stack-fused).
	int64 WorklistVoxConverted = 0;
	int64 WorklistVoxFallback = 0;
	int64 WorklistVoxFallbackAssets = 0;
	// P3 fused ClassifyTotals (cumulative, wlct line): same three meanings.
	int64 WorklistCtConverted = 0;
	int64 WorklistCtFallback = 0;
	int64 WorklistCtFallbackAssets = 0;
	int64 WorklistSkipNoPack = 0;    // no brick region (quad-only leg, or shell refused)
	int64 WorklistSkipQuadMesh = 0;  // job still emits quads (RetireQuads off)
	int64 WorklistSkipBand = 0;      // job carries its footprint's band readback
	int64 WorklistSkipNoAlloc = 0;   // voxel.GPU.PoolAlloc not armed / no shell
	int64 WorklistSkipNoAtlas = 0;   // request carries an inline raster, not the atlas
	// Passes-per-tick, the counter that gets read FIRST on any worklist leg:
	// the pass term is the 50k wall (15/chunk = 25x the ~500/tick hitch cliff
	// at 50k chunks/s), so the gate is this number going FLAT as chunk rate
	// rises -- never throughput, which the 2026-08-23 four-arm sweep proved
	// insensitive to pass count at ~2,100 chunks/s.
	int64 WorklistWinTicks = 0;
	int64 WorklistWinChunks = 0;
	int64 WorklistWinPasses = 0;
	int64 WorklistWinPassesMaxTick = 0;
	// DispatchBatch's share of THIS tick's passes, folded (plus the spine's
	// per-tick constant) into the window by Tick. Split because the spine
	// flushes on batchless ticks too: tallying only in DispatchBatch left
	// those ticks' 2-3 real passes uncounted, which is how the window line
	// read mean=0.0 while the GPU dispatched every tick (2026-08-23).
	int64 WorklistBatchPassesThisTick = 0;
	// Skip total at the last window boundary, so the quiet gate compares the
	// WINDOW'S skips. Gating on the cumulative total kept every post-flight
	// linger window printing zeros forever once any chunk had ever skipped.
	int64 WorklistPrevSkipTotal = 0;
	// Cumulative ring identity: appended == consumed + pending, or records
	// are being lost/double-consumed and the window line says DRIFT.
	uint64 WorklistCumAppended = 0;
	uint64 WorklistCumConsumed = 0;
	uint64 WorklistCumRefused = 0;
	double LastWorklistLogSeconds = 0.0;
	bool bWorklistArmingLogged = false;
	// --- P3 stage 1, the converted Column kernel (-VoxelGpuWorklistColumns) --
	// conv: chunks whose ColumnMain pass was replaced by their slice of the
	// once-per-tick indirect dispatch. fb: eligible records that were NOT
	// consumed by this tick's flush (ring backlog past the budget, refused
	// ring, or stage unarmed at flush) -- those chunks ran classic and their
	// arena slice, if any, went unread. Cumulative, printed by the armed-only
	// wlcols line; the failing readings are documented at the log site.
	int64 WorklistColConverted = 0;
	int64 WorklistColFallback = 0;
	// True when DispatchBatch flushed the worklist this tick (column stage:
	// the flush must precede the batch render command); Tick then skips its
	// own flush and clears the flag.
	bool bWorklistFlushedThisTick = false;
	void MaybeLogWorklistWindow();
};

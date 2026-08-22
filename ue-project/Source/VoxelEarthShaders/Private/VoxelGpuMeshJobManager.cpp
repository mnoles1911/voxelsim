#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuWorldGenGraph.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Misc/ScopeExit.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuMeshJob, Log, All);

// Wave D / D2. Which MeshEmitMain permutation the async runner dispatches.
//
// This is an A/B, not a feature flag, and it is the one that matters: at 1 the
// GPU emits chunk-local quads and RebaseQuadsToChunkLocal never runs; at 0 the
// GPU emits brick-local quads and the CPU rebases them exactly as it did
// before D2. Both are supposed to produce byte-identical results, and
// voxel.GPU.VerifyAsyncMesh compares BOTH against MeshChunkBricks — so
// flipping this and re-running is the whole correctness argument for the
// permutation, runnable in one session on one binary.
static int32 GVoxelGpuMeshChunkLocal = 1;
static FAutoConsoleVariableRef CVarVoxelGpuMeshChunkLocal(
	TEXT("voxel.GPU.MeshChunkLocal"),
	GVoxelGpuMeshChunkLocal,
	TEXT("1 = the GPU emits chunk-local quads (VXC_MESH_CHUNK_LOCAL permutation, no CPU rebase). ")
	TEXT("0 = the GPU emits brick-local quads and FVoxelGpuMeshJobManager rebases them on the CPU. ")
	TEXT("Default 1. Read once per Submit, so it takes effect on the next job."),
	ECVF_Default);

// --- P1-C / P2: the resident brick volume ----------------------------------
//
// OFF BY DEFAULT, AND THAT IS THE PHASE'S WHOLE CLAIM. With this at 0 the job
// dispatches exactly the graph it dispatched yesterday: no second region, no
// extra passes, no extra readback, byte-identical. With it at 1 the job ALSO
// packs its chunk into the brick volume on a second, halo-free 32x32x4 region in
// the SAME FRDGBuilder, and publishes the result into the brick pool. The mesh
// chain is untouched either way -- nothing marches yet, so this is additive and
// off-path by construction rather than by care.
//
// Latched per job at Submit, like voxel.GPU.MeshChunkLocal, so a flip mid-flight
// cannot leave a job that dispatched a brick region waiting on a readback
// nobody enqueued.
static int32 GVoxelGpuBrickPack = 1;   // PROTOTYPE DEFAULT: the marcher needs bricks
static FAutoConsoleVariableRef CVarVoxelGpuBrickPack(
	TEXT("voxel.GPU.BrickPack"),
	GVoxelGpuBrickPack,
	TEXT("1 = every mesh job also packs its chunk into the resident brick volume ")
	TEXT("(BrickClassify -> Scan x2 -> BrickPack on a halo-free 32x32x4 region, in the same graph). ")
	TEXT("0 (default) = byte-identical to the graph without it. Read once per Submit."),
	ECVF_Default);

// THE 'PUBLICATION STUBBED' ARM, and it is a measurement instrument rather than
// a safety valve. docs/ray-marching-plan-2026-08-19.md section 8 asks for
// exactly this experiment: run the producer with BrickPack on and publication
// stubbed -- generate, pack, discard -- and measure chunks/s. That isolates the
// producer's cost from the pool's, which is the only way to know which of the
// two a throughput change came from.
static int32 GVoxelGpuBrickPackResident = 1;
static FAutoConsoleVariableRef CVarVoxelGpuBrickPackResident(
	TEXT("voxel.GPU.BrickPackResident"),
	GVoxelGpuBrickPackResident,
	TEXT("1 (default) = a packed chunk is published into the global brick pool. ")
	TEXT("0 = packed and DISCARDED -- the 'publication stubbed' arm, which measures the producer's ")
	TEXT("cost with the pool's removed. Only meaningful with voxel.GPU.BrickPack 1."),
	ECVF_Default);

// --- render-thread cost caps (2026-07-27 line-flight instrumentation) -------
//
// THE MEASUREMENT THESE EXIST FOR. Six instrumented 20 m/s line-flight legs on
// real terrain: with this fork ON, 10.1% of flight frames hitched (>33.3 ms)
// against 0.028% with it OFF. The hitch frames are render-thread-BUSY dominated
// — median renderMs 75 ms on fork-on legs vs 3 ms on fork-off, with RHI flat at
// 1.7 ms in BOTH arms and identical game-thread cost (GPU legs 40 fps, CPU legs
// 59 fps). Hitches appear if and only if the fork is delivering: pearson 0.88
// against per-window deliveries, and 63 fork-idle windows had zero hitches.
//
// So the cost is render-thread CPU spent in this file's own bookkeeping — RDG
// pass setup, readback Lock/memcpy/Unlock, and render-command overhead — not
// GPU execution and not the game thread. Both caps below bound how much of that
// work any ONE render command may do, spreading it over frames instead of
// letting a burst land in a single one. Neither drops work: what does not fit
// stays queued / stays in flight and is picked up by the next tick.
//
// Both are A/B-able: <= 0 restores the previous unbounded behaviour exactly, so
// the fix can be measured against itself on one binary the same way
// voxel.GPU.MeshChunkLocal is.
static int32 GVoxelGpuMeshBatchCap = 4;
static FAutoConsoleVariableRef CVarVoxelGpuMeshBatchCap(
	TEXT("voxel.GPU.MeshBatchCap"),
	GVoxelGpuMeshBatchCap,
	TEXT("Max queued jobs one DispatchBatch may promote into a single FRDGBuilder. ")
	TEXT("Default 4 -- the caps sweep was monotone: 32/64 -> 367 hitches / 77.2k chunks, 8/16 -> 8 / 86.5k, ")
	TEXT("4/8 -> 8 / 89.4-89.6k over two legs, so the smallest batch measured is also the fastest. ")
	TEXT("<= 0 means unlimited (pre-cap behaviour). ")
	TEXT("Each job adds ~7 compute passes + 3-4 copy passes, so an uncapped burst ")
	TEXT("frame built graphs of 100+ passes on the render thread."),
	ECVF_Default);


// How many SPECULATIVE (low-priority) jobs may promote per tick, on top of
// MeshBatchCap's demand allowance. 0 disables speculative promotion entirely
// while leaving submission intact, which is the clean A/B for T4-1: the
// speculative queue still fills, so its depth is observable with the feature's
// GPU effect switched off.
static int32 GVoxelGpuMeshSpeculativeBatchCap = 4;
static FAutoConsoleVariableRef CVarVoxelGpuMeshSpeculativeBatchCap(
	TEXT("voxel.GPU.MeshSpeculativeBatchCap"),
	GVoxelGpuMeshSpeculativeBatchCap,
	TEXT("Speculative GPU mesh jobs promoted per tick, on top of voxel.GPU.MeshBatchCap. 0 = never promote speculative work."),
	ECVF_Default);
// Wave D / D1. Whether a job's quads stay on the GPU or come back through
// system memory.
//
// AT 1 (default) phase 2 is a GPU->GPU compaction and the result carries an
// FVoxelGpuQuadPayload; the pool component copies it into the chunk's allocated
// range with a compute pass, and no quad byte is ever touched by a CPU. AT 0
// the D3 two-phase readback runs exactly as it did before D1: fetch the live
// quads, memcpy them out on the render thread, unpack, repack, upload.
//
// The same shape as voxel.GPU.MeshChunkLocal, and for the same reason: this is
// an A/B runnable in one session on one binary, and voxel.GPU.VerifyPoolWrite
// compares BOTH against MeshChunkBricks. A path that has no control is a path
// whose PASS says nothing about the other one.
//
// Read once per Submit and LATCHED on the job. A flip between dispatch and
// delivery would otherwise hand the streaming path a result of a shape it did
// not ask for -- an empty Quads array where it expected geometry, which reads
// as an all-air chunk rather than as an error.
static int32 GVoxelGpuMeshDirectToPool = 1;
static FAutoConsoleVariableRef CVarVoxelGpuMeshDirectToPool(
	TEXT("voxel.GPU.MeshDirectToPool"),
	GVoxelGpuMeshDirectToPool,
	TEXT("1 = a mesh job's quads stay in GPU memory and are copied straight into the geometry pool ")
	TEXT("(no quad readback, no CPU staging, no re-upload; only the 4-byte total crosses PCIe). ")
	TEXT("0 = the D3 two-phase readback, which is the control. Default 1. ")
	TEXT("Requires voxel.GPU.MeshChunkLocal 1 -- brick-local quads need a CPU rebase this path does ")
	TEXT("not have, and the requirement is enforced in Submit rather than assumed. ")
	TEXT("Read once per Submit, so it takes effect on the next job."),
	ECVF_Default);

static int32 GVoxelGpuMeshHarvestCap = 8;
static FAutoConsoleVariableRef CVarVoxelGpuMeshHarvestCap(
	TEXT("voxel.GPU.MeshHarvestCap"),
	GVoxelGpuMeshHarvestCap,
	TEXT("Max ready jobs one poll may HARVEST (readback Lock/memcpy/Unlock). ")
	TEXT("Default 8, swept together with voxel.GPU.MeshBatchCap (see its comment). ")
	TEXT("<= 0 means unlimited (pre-cap behaviour). ")
	TEXT("IsReady() checks stay unbounded — they are cheap; it is the copies that ")
	TEXT("cost, and with 150-256 jobs in flight one poll could do all of them."),
	ECVF_Default);

// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2): gates the one NEW
// FPlatformTime::Seconds() call this item adds (FJob::PromotedSeconds, stamped
// in Tick's promote loop) -- the instrument must not be able to become what is
// being measured. This module has no VoxelDebug dependency (that is a
// VoxelEarth-module type), so it uses this file's own G.../CVarRef idiom
// instead of a VoxelDebug:: accessor. Off by default; the streaming-side
// voxel.Stream.LatencyStats is the one meant to be flipped for a leg -- this
// one exists to be flipped alongside it if the mesh-job manager is ever
// exercised standalone (e.g. a future bench harness) without the streaming
// module above it.
static int32 GVoxelGpuMeshLatencyStats = 0;
static FAutoConsoleVariableRef CVarVoxelGpuMeshLatencyStats(
	TEXT("voxel.GPU.MeshLatencyStats"),
	GVoxelGpuMeshLatencyStats,
	TEXT("Stamp FJob::PromotedSeconds (Submit -> promoted out of Queued in Tick), which feeds ")
	TEXT("FVoxelGpuMeshJobResult::QueuedMs. Default 0. Game thread only (Tick() already requires it), ")
	TEXT("so no thread-safety concern reading this cvar directly where it is checked."),
	ECVF_Default);

// Defined here rather than in a file of its own because this is the only place
// that CREATES one, and because the release it performs is the mirror of
// ReleaseReadbacksOnRenderThread below -- the two should be read together.
FVoxelGpuQuadPayload::FVoxelGpuQuadPayload() = default;

FVoxelGpuQuadPayload::~FVoxelGpuQuadPayload()
{
	if (!Quads.IsValid())
	{
		return;
	}
	// The last reference to an RHI resource, dropped in RENDER-THREAD ORDER
	// behind every command that could still be reading it. A payload dies on
	// whichever thread happened to hold the last handle -- the game thread for a
	// stale result or a full pool, the render thread for one that was consumed --
	// and the game-thread case is exactly the one that fails as a crash on exit
	// rather than as anything a compiler or a test would catch. Same reasoning,
	// same shape, as UVoxelGpuPoolComponent::BeginDestroy.
	ENQUEUE_RENDER_COMMAND(VoxelGpuQuadPayloadRelease)(
		[Buffer = MoveTemp(Quads)](FRHICommandListImmediate&) mutable
	{
		Buffer.SafeRelease();
	});
}

// See the declarations for why the CPU producer has to read the same two gates.
bool VoxelGpuBrickPackEnabled()
{
	// -VoxelBrickPack=<n>, for the -ExecCmds startup-window reason set out above
	// VoxelBrickPackOnCpuEnabled in VoxelBrickPool.cpp. A producer switch that
	// only takes effect after streaming has begun leaves a fixed residue of
	// chunks produced the old way, for the whole run.
	static const int32 CmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelBrickPack="), Value);
		return Value;
	}();
	return CmdLine >= 0 ? CmdLine != 0 : GVoxelGpuBrickPack != 0;
}

bool VoxelGpuBrickPackResidentEnabled()
{
	return GVoxelGpuBrickPackResident != 0;
}

const TCHAR* LexToString(EVoxelGpuMeshJobStatus Status)
{
	switch (Status)
	{
	case EVoxelGpuMeshJobStatus::Success:        return TEXT("Success");
	case EVoxelGpuMeshJobStatus::Rejected:       return TEXT("Rejected");
	case EVoxelGpuMeshJobStatus::DispatchFailed: return TEXT("DispatchFailed");
	case EVoxelGpuMeshJobStatus::ReadbackFailed: return TEXT("ReadbackFailed");
	case EVoxelGpuMeshJobStatus::TimedOut:       return TEXT("TimedOut");
	case EVoxelGpuMeshJobStatus::Cancelled:      return TEXT("Cancelled");
	}
	return TEXT("<unknown>");
}

void VoxelGpuChunkRegion::SetChunkFootprint(FVoxelGpuRegionRequest& Req,
                                            int32 ChunkX, int32 ChunkY, int32 ChunkZ)
{
	Req.DispatchColumns = FUintVector2(kColumns, kColumns);

	// One brick of halo on the negative side; the 48-column width supplies the
	// other one on the positive side.
	Req.OriginVx = ChunkX * int32(kChunkEdgeVoxels) - int32(kBrickEdge);
	Req.OriginVy = ChunkY * int32(kChunkEdgeVoxels) - int32(kBrickEdge);

	// A chunk is 4 bricks tall, so its first brick is at brick-z ChunkZ*4; back
	// off one for the halo.
	Req.BrickZMin = ChunkZ * 4 - 1;
	Req.BricksZ = kBricksZ;

	Req.bMeshChain = true;
}

bool VoxelGpuChunkRegion::MakeBrickRegion(const FVoxelGpuRegionRequest& MeshReq,
                                          FVoxelGpuRegionRequest& OutReq)
{
	// Only the standard single-chunk footprint. Anything else and the one-brick
	// shift below is arithmetic about a shape that is not there -- see the
	// declaration for what packing the wrong bricks looks like.
	if (MeshReq.DispatchColumns.X != kColumns || MeshReq.DispatchColumns.Y != kColumns ||
	    MeshReq.BricksZ != kBricksZ)
	{
		return false;
	}

	OutReq = MeshReq;

	// Drop the halo: one brick on every axis, on the negative side, and the
	// interior extent on the positive.
	OutReq.DispatchColumns = FUintVector2(kChunkEdgeVoxels, kChunkEdgeVoxels);
	OutReq.OriginVx = MeshReq.OriginVx + int32(kBrickEdge);
	OutReq.OriginVy = MeshReq.OriginVy + int32(kBrickEdge);
	OutReq.BrickZMin = MeshReq.BrickZMin + 1;
	OutReq.BricksZ = kInteriorBricks;

	// This region generates and packs. It does NOT mesh: the mesh chain is the
	// other region's job and duplicating it would be the one thing this phase
	// promised not to do.
	OutReq.bMeshChain = false;
	OutReq.bBrickPack = true;
	OutReq.bChunkLocalQuads = false;
	OutReq.QuadWriteBase = 0;

	// The band is a property of the COLUMNS, and the mesh region already
	// produces one for whichever job owns its footprint. Producing a second from
	// a narrower window would be a different reduction over a different grid --
	// and a band that is not an outer bound skips chunks, i.e. holes.
	OutReq.BandEdge = 0;
	OutReq.BandOriginI = 0;
	OutReq.BandOriginJ = 0;

	// THE RING SKIRT IS DROPPED, AND IT IS A DECISION WORTH SEEING. The skirt
	// rewrites cells in a chunk's lateral aprons so the MESHER emits a retaining
	// wall at a ring boundary; regionCellMat applies it against the fixed
	// interior [8, 40) of a 48-column dispatch, which does not exist here.
	// docs/brick-volume-format.md section 8 is explicit that ring-boundary
	// handling is a TRAVERSAL concern for a marcher rather than a producer
	// apron. So the volume holds the world as generated, and the marcher will
	// own the ring seam. ValidateRegionRequest would refuse a non-zero mask on a
	// 32-column region anyway -- this makes the intent explicit instead of
	// leaving it to a rejection.
	OutReq.RingSkirtMask = 0;

	// The raster window is copied verbatim: the halo-free footprint is strictly
	// inside the halo one, so the window over-covers, and over-covering is legal
	// where under-covering silently clamps and diverges from the CPU.

	// Every asset anchor is relative to the region origin, which has just moved
	// by one brick of LEVEL-L cells -- i.e. 8 << CoarseLevel level-0 voxels,
	// because AnchorRelVx is in level-0 voxel units relative to OriginVx * 2^L.
	// AnchorVz is absolute and does not move.
	const int32 AnchorShift = int32(kBrickEdge) << FMath::Clamp(MeshReq.CoarseLevel, 0, 5);
	for (FVoxelGpuRegionRequest::FAssetInstance& Inst : OutReq.AssetInstances)
	{
		Inst.AnchorRelVx -= AnchorShift;
		Inst.AnchorRelVy -= AnchorShift;
	}

	return true;
}

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------

namespace
{
	// Where a job is. Written by whichever thread owns the transition and read by
	// the other, so it is an atomic with acquire/release ordering rather than a
	// plain int -- Error and the staging arrays are published by the same
	// release-store that moves the state, and read after the matching acquire.
	// Where a job is.
	//
	// WAVE D / D3 SPLIT THIS IN TWO. There used to be one round trip: dispatch,
	// then read back Counts + Offsets + the whole upper-bound quad buffer and
	// work out afterwards how much of it was live. That is ~810 KB per chunk to
	// recover ~7-12 KB of quads (see VoxelQuadScan.usf for the arithmetic and
	// why it, not the kernel, sets streaming throughput).
	//
	// Now frame N reads back FOUR BYTES -- the scan total -- and frame N+k reads
	// back exactly that many quads. Two round trips instead of one, which costs
	// latency and saves ~70-100x the bandwidth. That is the right trade for a
	// streaming path whose in-flight cap already absorbs latency, and it is
	// transitional either way: once the pool's buffers are UAV-writable and
	// RDG-registered (D1), phase 2 becomes a GPU->GPU copy into the allocated
	// range and reads back NOTHING.
	//
	// WAVE D / D1 SPLIT PHASE 2 IN TWO, AND ONLY ONE HALF READS ANYTHING BACK.
	// Under voxel.GPU.MeshDirectToPool the quads never come to the CPU: phase 2
	// is a compaction pass into a right-sized GPU buffer, nothing waits for it,
	// and the job goes STRAIGHT from TotalDone to ReadbackDone in the same tick.
	// QuadsDispatched is therefore reached only on the readback control path.
	enum class EJobState : int32
	{
		Queued = 0,      // game thread owns it
		Dispatched,      // graph executed; the 4-byte total readback is pending
		TotalDone,       // total has landed; the game thread must start phase 2
		QuadsDispatched, // phase 2 enqueued; the sized quad readback is pending
		ReadbackDone,    // everything this job owed has been copied out (or left on the GPU)
		Failed,          // render thread gave up; Error says why
	};
}

struct FVoxelGpuMeshJobManager::FJob
{
	uint64 JobId = 0;
	uint64 UserTag = 0;

	// OWNED. The render thread reads this; nothing else may mutate it after
	// Submit. This is the field that would be a dangling reference if it were
	// captured the way RunRegionBlocking captures its request.
	FVoxelGpuRegionRequest Region;

	VoxelGpuWorldGen::FRegionGraphSizes Sizes;
	uint32 InteriorX = 0;
	uint32 InteriorY = 0;
	uint32 InteriorZ = 0;

	// Created and destroyed on the render thread, kept alive by this object's
	// refcount for as long as any render command can still touch them.
	//
	// TotalReadback is phase 1 and always present. QuadsReadback is phase 2.
	// Counts/Offsets are only created for the brick-local control path, which
	// is the one that still has to rebase on the CPU and therefore still needs
	// the per-mask tables; the chunk-local path never reads them.
	TUniquePtr<FRHIGPUBufferReadback> TotalReadback;
	// Wave D / D6. Phase 1 as well, harvested all-or-none with the total, so a
	// job still has exactly one completion event rather than two async streams
	// that must both satisfy exactly-one-outcome. Null when the request did not
	// ask for a band.
	TUniquePtr<FRHIGPUBufferReadback> BandReadback;
	TUniquePtr<FRHIGPUBufferReadback> CountsReadback;
	TUniquePtr<FRHIGPUBufferReadback> OffsetsReadback;
	TUniquePtr<FRHIGPUBufferReadback> QuadsReadback;

	// The quad buffer, kept alive ACROSS GRAPHS. Today's Voxel.Quads is an RDG
	// transient and dies at GraphBuilder.Execute(); phase 2 runs in a different
	// graph a frame or more later, so it has to outlive the first one. This is
	// the reference that makes that true, and dropping it is what frees the
	// memory.
	TRefCountPtr<FRDGPooledBuffer> QuadBuffer;

	// Wave D / D1. Set at Submit and never read from the render thread, so it
	// needs no synchronisation: it selects which phase 2 the GAME thread starts.
	// Latched rather than re-read at delivery -- see the cvar's comment.
	bool bDirectToPool = false;

	// --- P1-C: the brick half of the job ----------------------------------
	//
	// bBrickPack is latched at Submit and ANDed with whether MakeBrickRegion
	// could actually derive a region, so a job either has a complete brick
	// region or none at all. There is no half state to check for later.
	bool bBrickPack = false;
	bool bBrickResident = false;
	// PHASE 5. False = BRICK-ONLY: this job runs the generation half and the
	// brick region and produces NO QUADS at all -- no mesh chain, no quad buffer,
	// no 4-byte total readback, no pool write.
	//
	// Latched at Submit like every other per-job gate, so a mid-flight flip
	// cannot leave a job waiting on a readback nobody enqueued -- the exact
	// failure voxel.GPU.BrickPack's own latch exists to prevent.
	//
	// THE BAND SURVIVES THIS, which is what makes it cheap. The band is a pure
	// function of the columns and AddRegionPasses emits it outside the mesh
	// block (VoxelGpuWorldGen.cpp, "lets the gate run band-only probes
	// cheaply"), so the buried-chunk skip and the cold-band throttle keep their
	// input. Nothing else downstream needs teaching: NumQuads stays 0 and the
	// existing zero-quad branch carries the job to ReadbackDone.
	bool bQuadMesh = true;
	FVoxelGpuRegionRequest BrickRegion;
	FVoxelBrickChunkKey BrickKey;
	FVoxelBrickChunkShading BrickShading;
	FIntVector BrickOriginVoxel = FIntVector::ZeroValue;
	// Phase 1, harvested ALL-OR-NONE with the quad total: the two dword counts
	// the pool allocation is made from. Eight bytes, and the only thing on this
	// path that crosses PCIe.
	TUniquePtr<FRHIGPUBufferReadback> BrickTotalReadback;
	uint32 BrickTotals[2] = { 0, 0 };
	// Constructed on the render thread in DispatchBatch, holding the three
	// scratch arenas and the L1 mask. Handed to the pool (and to the result) on
	// the game thread at delivery.
	FVoxelGpuBrickPayloadRef BrickPayload;

	// Wave D / D1. Non-null once the direct path has taken this job's quads.
	// Constructed on the game thread holding QuadBuffer (so the payload is
	// correct even if the compaction never runs), then owned by the render
	// thread. Delivered in place of RawQuads.
	FVoxelGpuQuadPayloadRef Payload;

	// Staged by the render thread, consumed by the game thread after State goes
	// to ReadbackDone.
	TArray<uint32> Counts;
	TArray<uint32> Offsets;
	TArray<uint64> RawQuads;
	// What QuadTotalMain said, i.e. how many quads phase 2 actually fetches.
	uint32 NumQuads = 0;
	// Wave D / D6: BandReduceMain's two raw voxel-z extrema. bBandValid only
	// goes true once the copy has actually landed, so a band that never arrived
	// reads as absent rather than as {0, 0} -- which would claim the whole
	// footprint is empty.
	int32 Band[2] = { 0, 0 };
	bool bBandValid = false;
	FString Error;

	std::atomic<int32> State{ int32(EJobState::Queued) };
	// Set by the game thread before enqueuing a poll, cleared by the render
	// thread when that poll has looked at this job. Stops polls from stacking up
	// when the render thread is behind.
	std::atomic<int32> PollPending{ 0 };
	// Set by the game thread when it has given up on this job (timeout /
	// cancellation). The render thread checks it so a late poll does no work.
	std::atomic<int32> Abandoned{ 0 };
	// Set by the game thread when it has enqueued phase 2 for this job. The
	// state does not move to QuadsDispatched until the render command runs, so
	// without this the next tick would see TotalDone again and enqueue a second
	// fetch — two readbacks racing to fill one array.
	std::atomic<int32> QuadFetchStarted{ 0 };

	double SubmitSeconds = 0.0;
	double DispatchSeconds = 0.0;
	double ReadySeconds = 0.0;
	// S0-3: Submit() to the moment this job left the Queued array in Tick's
	// promote loop -- i.e. how long it waited behind MaxInFlight before a slot
	// opened. Stamped only under voxel.GPU.MeshLatencyStats (see that cvar);
	// 0.0 otherwise, which Deliver()'s existing non-zero guard already treats
	// as "not measured" the same way it does for DispatchSeconds/ReadySeconds.
	double PromotedSeconds = 0.0;

	void SetState(EJobState New) { State.store(int32(New), std::memory_order_release); }
	EJobState GetState() const { return EJobState(State.load(std::memory_order_acquire)); }
};

namespace
{
	using FJobPtr = TSharedPtr<FVoxelGpuMeshJobManager::FJob, ESPMode::ThreadSafe>;

	// Copies one readback out. Returns false and fills OutError on any failure,
	// so a partial copy can never be mistaken for a good one.
	bool CopyReadback(FRHIGPUBufferReadback& Readback, void* Dest, uint32 Bytes,
	                  const TCHAR* Name, FString& OutError)
	{
		if (Bytes == 0)
		{
			return true;
		}
		const void* Src = Readback.Lock(Bytes);
		if (Src == nullptr)
		{
			OutError = FString::Printf(TEXT("%s readback lock returned null"), Name);
			return false;
		}
		FMemory::Memcpy(Dest, Src, Bytes);
		Readback.Unlock();
		return true;
	}

	// Releases a BATCH of jobs' readbacks on the render thread. Capturing the
	// jobs by shared pointer is what keeps them alive until the command runs --
	// the manager may well have forgotten about them by then.
	//
	// ONE COMMAND FOR THE WHOLE TICK, not one per job. This used to be called
	// per delivered job, which at the observed steady-state delivery rate
	// (~18 jobs/frame) was ~18 ENQUEUE_RENDER_COMMANDs per frame whose bodies do
	// almost nothing: the command overhead and the render thread's per-command
	// dispatch dominated the actual resets. The work done is byte-for-byte the
	// same, and so is its position in the render-command stream — the batch is
	// enqueued at exactly the point the last per-job command used to be, i.e.
	// after this tick's poll command, which is the only ordering that matters
	// (a poll must never see readbacks a later-enqueued release has already
	// reset).
	void ReleaseReadbacksOnRenderThread(TArray<FJobPtr>&& Jobs)
	{
		if (Jobs.Num() == 0)
		{
			return;
		}
		ENQUEUE_RENDER_COMMAND(VoxelGpuMeshReleaseReadbacks)(
			[Batch = MoveTemp(Jobs)](FRHICommandListImmediate&)
		{
			for (const FJobPtr& Job : Batch)
			{
				if (!Job.IsValid())
				{
					continue;
				}
				Job->TotalReadback.Reset();
				Job->BandReadback.Reset();
				Job->CountsReadback.Reset();
				Job->OffsetsReadback.Reset();
				Job->QuadsReadback.Reset();
				// Frees the persistent quad buffer back to RDG's pool.
				// Deliberately on the render thread with everything else: this
				// is an RHI resource reference and it is the last thing holding
				// it.
				//
				// NOT ALWAYS THE LAST, since D1: on the direct path the delivered
				// payload holds its own reference to this same buffer (until the
				// compaction swaps it for a small one), so this drops the
				// MANAGER's claim and the geometry survives. That is the intended
				// behaviour and it is why the payload is reference-counted rather
				// than a raw handle.
				Job->QuadBuffer.SafeRelease();
			}
		});
	}

	// maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 + slice, and the
	// interior bricks of a chunk-sized region ARE the chunk's 4x4x4 bricks in
	// the same x-fastest order MeshChunkBricks walks. So interior brick
	// (ix,iy,iz) sits at chunk-local voxel origin (ix*8, iy*8, iz*8) and the
	// rebase is a straight add on the three packed position fields.
	//
	// Ported from RebaseQuadsToRegionLocal in VoxelGpuVerify.cpp, with the
	// difference that this targets CHUNK-local coordinates (0..31, what the pool
	// and the CPU mesher use) rather than region-local: the chunk's first brick
	// is interior brick (0,0,0), whose region-local origin is (8,8,8), so the
	// +1-brick term that version adds is exactly what has to come back out.
	TArray<uint64> RebaseQuadsToChunkLocal(const FVoxelGpuMeshJobManager::FJob& Job)
	{
		TArray<uint64> Rebased;
		Rebased.Reserve(Job.RawQuads.Num());

		const uint32 InteriorX = Job.InteriorX;
		const uint32 InteriorY = Job.InteriorY;

		for (int32 MaskIndex = 0; MaskIndex < Job.Counts.Num(); ++MaskIndex)
		{
			const uint32 Count = Job.Counts[MaskIndex];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = Job.Offsets[MaskIndex];

			const uint32 MeshBrickIndex = uint32(MaskIndex) / 48u;
			const uint32 Ix = MeshBrickIndex % InteriorX;
			const uint32 Iy = (MeshBrickIndex / InteriorX) % InteriorY;
			const uint32 Iz = MeshBrickIndex / (InteriorX * InteriorY);

			const uint32 BrickOrigin[3] = { Ix * 8u, Iy * 8u, Iz * 8u };

			for (uint32 Q = 0; Q < Count; ++Q)
			{
				const int32 SrcIndex = int32(Start + Q);
				if (!Job.RawQuads.IsValidIndex(SrcIndex))
				{
					// Cannot happen unless the scan is corrupt; the caller checks
					// NumQuads against MaxQuads first. Bail rather than read past
					// the end.
					return Rebased;
				}
				const uint64 Packed = Job.RawQuads[SrcIndex];
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);

				const uint32 Axis  =  W0        & 0xfu;
				const uint32 Dir   = (W0 >>  4) & 0xfu;
				const uint32 Slice = (W0 >>  8) & 0xffu;
				const uint32 U0    = (W0 >> 16) & 0xffu;
				const uint32 V0    = (W0 >> 24) & 0xffu;

				const uint32 U = (Axis + 1u) % 3u;
				const uint32 V = (Axis + 2u) % 3u;

				const uint32 NewW0 = Axis | (Dir << 4)
				                   | ((Slice + BrickOrigin[Axis]) << 8)
				                   | ((U0    + BrickOrigin[U])    << 16)
				                   | ((V0    + BrickOrigin[V])    << 24);
				Rebased.Add(uint64(NewW0) | (uint64(W1) << 32));
			}
		}
		return Rebased;
	}
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

FVoxelGpuMeshJobManager::FVoxelGpuMeshJobManager(FVoxelGpuMeshJobComplete OnComplete,
                                                 int32 InMaxInFlight,
                                                 double InTimeoutSeconds)
	: OnJobComplete(MoveTemp(OnComplete))
	, MaxInFlight(FMath::Max(1, InMaxInFlight))
	, TimeoutSeconds(FMath::Max(0.1, InTimeoutSeconds))
{
	// The "cannot lose a job" guarantee is only a guarantee if there is somewhere
	// to deliver to. Failing here is far better than discovering it as a chunk
	// that never arrives.
	checkf(OnJobComplete.IsBound(),
	       TEXT("FVoxelGpuMeshJobManager requires a bound completion delegate — every job must be deliverable"));
}

FVoxelGpuMeshJobManager::~FVoxelGpuMeshJobManager()
{
	CancelAll();
}

uint64 FVoxelGpuMeshJobManager::Submit(FVoxelGpuRegionRequest&& Region, uint64 UserTag,
                                       bool bRequestGpuResidentQuads, bool bLowPriority)
{
	check(IsInGameThread());

	FJobPtr Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->JobId = NextJobId++;
	Job->UserTag = UserTag;
	Job->Region = MoveTemp(Region);
	// Latched per job rather than read at delivery: a cvar flip between
	// dispatch and readback would otherwise rebase quads that were already
	// rebased on the GPU, which is silent and looks like a mesher bug.
	Job->Region.bChunkLocalQuads = GVoxelGpuMeshChunkLocal != 0;

	// Wave D / D1. THE bChunkLocalQuads TERM IS THE POINT OF ANDING THIS HERE
	// RATHER THAN TRUSTING THE CALLER. The direct path hands the pool the bytes
	// the emit pass wrote, and only the chunk-local permutation writes bytes the
	// pool can use -- brick-local quads need RebaseQuadsToChunkLocal, a CPU pass
	// over a stream this path never brings to the CPU. Without the term,
	// voxel.GPU.MeshChunkLocal 0 plus the default MeshDirectToPool 1 would pile
	// every brick's geometry into the same 8-voxel cube at the chunk origin: not
	// a crash, not an error, just terrain that is wrong in a way that looks like
	// a mesher bug. Same refusal ValidateRegionRequest already makes about
	// QuadWriteBase, for the same reason.
	Job->bDirectToPool = bRequestGpuResidentQuads
		&& GVoxelGpuMeshDirectToPool != 0
		&& Job->Region.bChunkLocalQuads;

	// PHASE 5: brick-only. Read once, here, for the latch reason on bQuadMesh.
	// VoxelTerrainQuadsRetired is already ANDed with voxel.GPU.BrickPack, so this
	// cannot turn the mesh chain off on a job that will not pack anything either.
	Job->bQuadMesh = !VoxelTerrainQuadsRetired();
	if (!Job->bQuadMesh)
	{
		// bMeshChain is the pre-existing "stop after voxelization" switch the
		// region request has always carried -- used until now only by the
		// verification gates. It is exactly the shape this needs, so Phase 5
		// costs no new kernel and no new shader.
		Job->Region.bMeshChain = false;
		// Nothing to put in the quad pool, so the direct-to-pool path must not
		// be armed: it would allocate a range for geometry that does not exist.
		Job->bDirectToPool = false;
	}

	// P1-C. Latched here, and ANDed with whether a brick region can actually be
	// derived from this footprint -- a job either carries a complete brick
	// region or none, so nothing downstream has to check for a half state.
	// Non-chunk footprints (the bench fixtures, the blocking verify path) simply
	// do not pack, which is correct: the brick volume is a per-render-chunk
	// structure and there is no such thing as packing two thirds of one.
	FString BrickRegionError;
	// VoxelGpuBrickPackEnabled(), NOT the raw cvar. THE GATE IS NO LONGER "A
	// CVAR" -- it is a cvar with a command-line override, and only the accessor
	// knows that. Reading GVoxelGpuBrickPack directly here is what made
	// -VoxelBrickPack=1 produce a job with bMeshChain false AND bBrickPack false,
	// which the promotion guard then correctly rejected as producing nothing --
	// the fork carried zero traffic and `added (gpu 0, cpu 28123)` was the only
	// sign. The invariant moved when the override landed; every read has to move
	// with it.
	if (VoxelGpuBrickPackEnabled() &&
	    VoxelGpuChunkRegion::MakeBrickRegion(Job->Region, Job->BrickRegion) &&
	    // VALIDATED HERE TOO, and not because MakeBrickRegion is suspect. The
	    // mesh region is validated in Tick before it reaches a graph; the brick
	    // region is derived rather than submitted, so nothing else would ever
	    // look at it. AddRegionPasses assumes a validated request -- the
	    // difference between a constraint and an assumption is whether anything
	    // checks it.
	    VoxelGpuWorldGen::ValidateRegionRequest(Job->BrickRegion, BrickRegionError))
	{
		Job->bBrickPack = true;
		Job->bBrickResident = GVoxelGpuBrickPackResident != 0;

		// The chunk the brick region covers, in its OWN level's units. The
		// origins are exact multiples of the chunk edge by construction
		// (SetChunkFootprint plus MakeBrickRegion's one-brick shift), so these
		// divisions are exact and sign-safe -- an exact multiple divides the
		// same way whichever direction the truncation goes.
		Job->BrickKey.X = Job->BrickRegion.OriginVx / int32(VoxelGpuChunkRegion::kChunkEdgeVoxels);
		Job->BrickKey.Y = Job->BrickRegion.OriginVy / int32(VoxelGpuChunkRegion::kChunkEdgeVoxels);
		Job->BrickKey.Z = Job->BrickRegion.BrickZMin / int32(VoxelGpuChunkRegion::kInteriorBricks);
		Job->BrickKey.Level = FMath::Clamp(Job->BrickRegion.CoarseLevel, 0, 5);
		// Latched with the key, from the same region, for the same reason: the
		// record is written at completion and the game thread that sampled this
		// is long gone by then.
		Job->BrickShading = Job->BrickRegion.BrickShading;
		Job->BrickOriginVoxel = FIntVector(
			Job->BrickRegion.OriginVx,
			Job->BrickRegion.OriginVy,
			Job->BrickRegion.BrickZMin * int32(VoxelGpuChunkRegion::kBrickEdge));
	}
	else if (VoxelGpuBrickPackEnabled() && !BrickRegionError.IsEmpty())
	{
		// Loud, because the alternative is a cvar that is on and does nothing --
		// the failure mode this project has paid for twice. The mesh half of the
		// job is unaffected either way.
		UE_LOG(LogTemp, Error,
		       TEXT("voxel.GPU.BrickPack is on but the derived brick region is invalid: %s"),
		       *BrickRegionError);
	}

	Job->SubmitSeconds = FPlatformTime::Seconds();

	// Low-priority work goes to its own queue and is promoted only when the
	// demand queue is empty -- see Submit's declaration for why submit-last
	// ordering is not sufficient against a FIFO drain.
	if (bLowPriority)
	{
		QueuedLowPriority.Add(Job);
	}
	else
	{
		Queued.Add(Job);
	}
	return Job->JobId;
}

void FVoxelGpuMeshJobManager::Deliver(const FJobPtr& Job, EVoxelGpuMeshJobStatus Status, const FString& Error)
{
	FVoxelGpuMeshJobResult Result;
	Result.JobId = Job->JobId;
	Result.UserTag = Job->UserTag;
	Result.Status = Status;
	Result.Error = Error;
	const double DeliverSeconds = FPlatformTime::Seconds();
	Result.SubmitToDeliverMs = (DeliverSeconds - Job->SubmitSeconds) * 1000.0;
	if (Job->ReadySeconds > 0.0 && Job->DispatchSeconds > 0.0)
	{
		Result.DispatchToReadyMs = (Job->ReadySeconds - Job->DispatchSeconds) * 1000.0;
	}
	// S0-3. Both computed from timestamps that already exist by the time this
	// runs (or are 0.0 under voxel.GPU.MeshLatencyStats off, in which case the
	// guard below leaves the result at its 0.0 default -- same "not measured"
	// convention as DispatchToReadyMs above).
	if (Job->PromotedSeconds > 0.0 && Job->SubmitSeconds > 0.0)
	{
		Result.QueuedMs = (Job->PromotedSeconds - Job->SubmitSeconds) * 1000.0;
	}
	if (Job->ReadySeconds > 0.0)
	{
		Result.ReadyToDeliverMs = (DeliverSeconds - Job->ReadySeconds) * 1000.0;
	}

	if (Status == EVoxelGpuMeshJobStatus::Success)
	{
		if (Job->Payload.IsValid())
		{
			// Wave D / D1. The quads are in GPU memory and Result.Quads stays
			// EMPTY. NumQuads is then the only description of the geometry the
			// CPU gets, and it is the same number phase 1 read back and sized
			// the compaction from.
			Result.NumQuads = Job->Payload->NumQuads;
			Result.GpuQuads = MoveTemp(Job->Payload);
		}
		else
		{
			// Under the D2 permutation the shader has already baked each brick's
			// chunk-local origin into the quad positions, so the stream is
			// pool-ready and the CPU rebase would double-apply the offset. The
			// brick-local branch below is kept, not vestigial: it is the control
			// voxel.GPU.MeshChunkLocal 0 selects, and the two must agree.
			// PHASE 5: a brick-only job took no readback and holds no scan
			// tables, so the rebase has nothing to rebase FROM. Its RawQuads is
			// already empty and empty is the true answer, so it takes the same
			// branch the chunk-local path does rather than walking a scan that
			// was never produced.
			Result.Quads = (Job->bQuadMesh && !Job->Region.bChunkLocalQuads)
				? RebaseQuadsToChunkLocal(*Job)
				: MoveTemp(Job->RawQuads);

			// Derived from the ARRAY, not from Job->NumQuads, so the readback
			// path's invariant NumQuads == Quads.Num() holds by construction
			// rather than by agreement. RebaseQuadsToChunkLocal bails early on a
			// corrupt scan, and a count that disagreed with the array it
			// describes is exactly the kind of thing that surfaces later as a
			// pool range sized for quads that are not there.
			Result.NumQuads = uint32(Result.Quads.Num());
		}

		// Wave D / D6. Only on Success, and only from a job that actually
		// harvested one — phase 1 is all-or-none, so a successful job that
		// asked for a band has one, and a failed job publishes nothing rather
		// than a {0, 0} band claiming its whole footprint is empty.
		Result.bBandValid = Job->bBandValid;
		Result.BandMaxSurfaceTopVoxel = Job->Band[0];
		Result.BandMinDeepestAirVoxel = Job->Band[1];

		// --- P1-C / P2: make the volume resident ---------------------------
		//
		// The totals have landed, so the size of this chunk's two arena
		// allocations is now known and the pool can be asked for them. This is
		// the game thread, which is where FVoxelBrickPool expects to be called
		// from; nothing is dispatched here -- Tick's Flush batches every write
		// in the tick into one graph.
		if (Job->bBrickPack && Job->BrickPayload.IsValid())
		{
			const uint32 OccWords = Job->BrickTotals[0];
			const uint32 MatWords = Job->BrickTotals[1];
			// The scratch buffers were sized to the worst case -- every brick
			// MIXED, 16 occupancy dwords and 132 material dwords each -- so a
			// total above that did not come from the scan. Bounded here because
			// the pool would otherwise allocate from it and the copy would read
			// past the end of a buffer that is genuinely too small.
			const uint32 MaxOcc = Job->BrickPayload->BrickCount * 16;
			const uint32 MaxMat = Job->BrickPayload->BrickCount * 132;
			if (OccWords > MaxOcc || MatWords > MaxMat)
			{
				UE_LOG(LogTemp, Error,
				       TEXT("Job %llu: brick totals (%u occ, %u mat dwords) exceed the worst case for ")
				       TEXT("%u bricks (%u, %u). Not published — this is a scan or readback fault, not ")
				       TEXT("a big chunk."),
				       Job->JobId, OccWords, MatWords, Job->BrickPayload->BrickCount, MaxOcc, MaxMat);
			}
			else
			{
				Job->BrickPayload->OccWords = OccWords;
				Job->BrickPayload->MatWords = MatWords;
				if (Job->bBrickResident)
				{
					GetGlobalVoxelBrickPool().AddChunkFromGpu(
						Job->BrickPayload, Job->BrickKey, Job->BrickShading);
				}
				// Handed on either way. Under voxel.GPU.BrickPackResident 0 the
				// caller is the only thing holding it, which is what makes
				// "generate, pack, discard" an actual discard rather than a
				// quietly-still-resident run.
				Result.BrickVolume = Job->BrickPayload;
			}
			Job->BrickPayload.Reset();
		}
	}

	OnJobComplete.ExecuteIfBound(MoveTemp(Result));
}

void FVoxelGpuMeshJobManager::Tick()
{
	check(IsInGameThread());

	// STAGE BRACKETS. See GetAndResetTickStageMs for why this function has them:
	// it is the largest item in the streaming tick and therefore the world's
	// generation ceiling, and it had exactly one number.
	const double TickStage0 = FPlatformTime::Seconds();

	// --- 1. promote queued jobs, one RDG graph for the whole batch ----------
	//
	// Rejections are collected rather than delivered inline, for the same
	// reentrancy reason as the harvest below.
	//
	// CAPPED PER TICK (2026-07-27). MaxInFlight alone is not a per-frame bound:
	// with the streaming path's in-flight cap, 150-256 jobs were observed
	// outstanding, so a frame that drains a backlog could promote a hundred-plus
	// jobs into ONE FRDGBuilder — ~7 compute passes + 3-4 copy passes each, all
	// of it render-thread CPU in pass setup, which is what the hitch profile
	// points at. The cap does not reduce throughput, it flattens bursts: 32 jobs
	// per tick at the measured 40-60 fps sustains 1,280-1,920 dispatches/s
	// against an observed peak demand of ~850/s, so steady state never touches
	// it. Anything over the cap simply stays at the head of Queued and goes out
	// next tick, in order.
	//
	// The cap counts PROMOTED jobs, not loop iterations: a rejected job never
	// reaches the graph and costs no pass setup, so draining a run of rejects in
	// one tick is both free and desirable (it gets them delivered sooner).
	const int32 BatchCap = GVoxelGpuMeshBatchCap > 0 ? GVoxelGpuMeshBatchCap : MAX_int32;

	// LOW PRIORITY GETS ITS OWN PER-TICK ALLOWANCE, NOT A SHARE OF BatchCap.
	//
	// This is the whole mechanism, and the obvious design is wrong. "Promote
	// low-priority only when Queued is empty" sounds like the safe reading of
	// strict priority, but it makes speculation a dead code path: at the shipped
	// MeshBatchCap 4 the demand queue carries ~236 jobs for the whole flight
	// (docs/measurements/t41-premise-2026-07-28.txt), so Queued is never empty,
	// demand takes all 4 slots every tick, and a low-priority job would wait
	// forever behind it.
	//
	// BatchCap is NOT a capacity limit -- it is a per-tick burst limiter that
	// exists to keep one FRDGBuilder from taking a hundred-plus jobs of pass
	// setup on the render thread (see its comment above). The real capacity limit
	// is MaxInFlight, and the GPU sits far below it: 16 in flight at cap 4, while
	// the same GPU ran 179 concurrently at cap 32. THAT GAP IS THE IDLE CAPACITY
	// T4-1 exists to use.
	//
	// So: demand promotes up to BatchCap as it always did -- byte-identical when
	// nothing low-priority is queued -- and low-priority promotes up to its own
	// separate SpecBatchCap on top, still bounded by MaxInFlight and still taken
	// only after demand has had its fill this tick. Demand's throughput and
	// ordering are untouched; speculation rides in the slack.
	const int32 SpecBatchCap = FMath::Max(0, GVoxelGpuMeshSpeculativeBatchCap);

	TArray<FJobPtr> Batch;
	TArray<TPair<FJobPtr, FString>> Rejected;
	int32 DemandPromoted = 0;
	int32 LowPriorityPromoted = 0;
	while (InFlight.Num() + Batch.Num() < MaxInFlight)
	{
		// Demand first, always, while it has both work and allowance.
		const bool bDemandCanRun = Queued.Num() > 0 && DemandPromoted < BatchCap;
		const bool bLowCanRun = QueuedLowPriority.Num() > 0 && LowPriorityPromoted < SpecBatchCap;
		if (!bDemandCanRun && !bLowCanRun)
		{
			break;
		}
		const bool bTakeLowPriority = !bDemandCanRun;
		TArray<FJobPtr>& Source = bTakeLowPriority ? QueuedLowPriority : Queued;
		FJobPtr Job = Source[0];
		Source.RemoveAt(0, EAllowShrinking::No);

		// S0-3: this IS "promoted out of Queued", whichever way the job goes
		// from here -- into the batch below or straight to Rejected. Stamping
		// before the validation checks means a rejected job's QueuedMs still
		// describes real queueing time, not a zero from a job that was never
		// actually going to run.
		if (GVoxelGpuMeshLatencyStats != 0)
		{
			Job->PromotedSeconds = FPlatformTime::Seconds();
		}

		FString ValidationError;
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			Rejected.Emplace(Job, TEXT("Requires SM6 (64-bit integer shader ops). Relaunch with -sm6."));
			continue;
		}
		if (!VoxelGpuWorldGen::ValidateRegionRequest(Job->Region, ValidationError))
		{
			Rejected.Emplace(Job, ValidationError);
			continue;
		}
		// A JOB MUST PRODUCE SOMETHING -- and as of Phase 5 that is no longer the
		// same statement as "must produce quads".
		//
		// THIS GUARD IS WHY THE FORK PACKED NOTHING UNDER RETIREMENT. It read
		// `bMeshChain must be true -- this manager exists to produce quads`, which
		// was exactly true when it was written and became false the moment a
		// brick-only job existed. Every such job was REJECTED here, before it ever
		// reached a graph, and the streaming path did the correct thing with a
		// rejection: it fell back to the CPU worker. So the fork silently carried
		// zero traffic (`added (gpu 0, cpu 30113)` against a control's
		// `(gpu 6120, cpu 90899)`), and because the CPU arm publishes inside
		// DrainResults' apply budget while the fork does not, the leg lost both
		// the fork's chunks AND the rate.
		//
		// Same failure shape as the edit-path exemption fixed alongside it: a
		// constraint that was true in one context, silently inherited into
		// another. Neither was a wrong line when written.
		if (!Job->Region.bMeshChain && !Job->bBrickPack)
		{
			Rejected.Emplace(Job, TEXT("a job must produce quads or bricks; this one asked for neither"));
			continue;
		}

		Job->Sizes = VoxelGpuWorldGen::ComputeRegionGraphSizes(Job->Region);
		Job->InteriorX = Job->Sizes.BricksX - 2;
		Job->InteriorY = Job->Sizes.BricksY - 2;
		Job->InteriorZ = Job->Sizes.BricksZ - 2;

		// Counted here, not at the take, so a REJECTED job still costs no
		// allowance -- preserving BatchCap's documented "counts promoted jobs,
		// not loop iterations" behaviour for demand exactly as before.
		if (bTakeLowPriority) { ++LowPriorityPromoted; } else { ++DemandPromoted; }
		Batch.Add(MoveTemp(Job));
	}

	if (Batch.Num() > 0)
	{
		InFlight.Append(Batch);
		DispatchBatch(MoveTemp(Batch));
	}

	for (const TPair<FJobPtr, FString>& R : Rejected)
	{
		Deliver(R.Key, EVoxelGpuMeshJobStatus::Rejected, R.Value);
	}

	// --- 2. poll and harvest ------------------------------------------------
	const double TickStage1 = FPlatformTime::Seconds();
	TickStageMs.PromoteMs += (TickStage1 - TickStage0) * 1000.0;
	PollInFlight();
	const double TickStage2 = FPlatformTime::Seconds();
	TickStageMs.PollMs += (TickStage2 - TickStage1) * 1000.0;

	// --- 3. P2: one render command for every brick write this tick ----------
	//
	// After the harvest, because the harvest is what delivers jobs and delivery
	// is what publishes into the pool. Cheap and safe with nothing pending; it
	// returns without enqueueing anything.
	GetGlobalVoxelBrickPool().Flush();
	const double TickStage3 = FPlatformTime::Seconds();
	TickStageMs.BrickFlushMs += (TickStage3 - TickStage2) * 1000.0;
}

void FVoxelGpuMeshJobManager::DispatchBatch(TArray<FJobPtr>&& Batch)
{
	// THE ENQUEUE ONLY, NOT THE GRAPH. The lambda below runs on the RENDER
	// thread; what the GAME thread pays here is constructing the command and
	// handing it over. If this is large it means the enqueue is blocking, which
	// is a different and much more serious finding than a slow graph -- worth
	// being able to tell apart.
	//
	// DispatchBatch is called from inside Tick's PROMOTE stage, so this time is
	// ALSO counted in PromoteMs. Subtract it when reading: promote-proper is
	// PromoteMs - EnqueueMs. Stated here because a breakdown whose parts overlap
	// silently is worse than one number.
	const double DispatchBatchStart = FPlatformTime::Seconds();
	// ONE graph for the whole batch. Every job's seven passes plus its three
	// readback copies go into the same FRDGBuilder, which is executed once.
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshDispatchBatch)(
		[Jobs = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// Jobs that made it into the graph. Anything that falls out below is
		// terminated here and now with a Failed state, so no job can leave this
		// command still sitting in Queued.
		TArray<FJobPtr> Built;
		Built.Reserve(Jobs.Num());

		for (const FJobPtr& Job : Jobs)
		{
			if (Job->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}

			const VoxelGpuWorldGen::FRegionGraphResources Graph =
				VoxelGpuWorldGen::AddRegionPasses(GraphBuilder, Job->Region);

			// PHASE 5: a brick-only job asked for no mesh chain, so no quad
			// buffers is the CORRECT outcome rather than a failure. Checked
			// against the job's own latch and not against the buffers being
			// null, because "null because we asked for nothing" and "null
			// because the chain broke" must not become the same condition --
			// that is how a real dispatch failure would start reading as an
			// intended one.
			if (Job->bQuadMesh)
			{
				if (Graph.Quads == nullptr || Graph.Counts == nullptr ||
				    Graph.Offsets == nullptr || Graph.Total == nullptr)
				{
					Job->Error = TEXT("mesh chain produced no quad buffers");
					Job->SetState(EJobState::Failed);
					continue;
				}

				// The quad buffer has to survive this graph: phase 2 fetches from
				// it in a later one. Everything else here dies at Execute, as it
				// should.
				Job->QuadBuffer = GraphBuilder.ConvertToExternalBuffer(Graph.Quads);

				// PHASE 1 READS FOUR BYTES. That is the whole point of D3 — see the
				// EJobState comment.
				Job->TotalReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.QuadTotal"));
				AddEnqueueCopyPass(GraphBuilder, Job->TotalReadback.Get(), Graph.Total, sizeof(uint32));
			}
			else if (Graph.Quads != nullptr || Graph.Total != nullptr)
			{
				// LOUD, because it means bMeshChain did not take: the job would
				// then be paying for the whole mesh chain on the GPU while
				// reporting itself as brick-only, and the only visible symptom
				// would be a throughput number that never improved.
				UE_LOG(LogTemp, Error,
				       TEXT("Job %llu is brick-only but the region still produced quad buffers. ")
				       TEXT("bMeshChain was not honoured; the mesh chain is still being dispatched."),
				       Job->JobId);
			}

			// ...twelve, when this job is the one producing its footprint's
			// band (Wave D / D6). Same graph, same phase, same delivery: the
			// point of folding it in here rather than giving it its own stream
			// is that a job still has exactly one outcome. See the comment on
			// FVoxelGpuMeshJobResult::bBandValid.
			if (Job->Region.BandEdge > 0 && Graph.Band != nullptr)
			{
				Job->BandReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Band"));
				AddEnqueueCopyPass(GraphBuilder, Job->BandReadback.Get(), Graph.Band,
				                   2 * uint32(sizeof(int32)));
			}

			// ...except on the brick-local control path, which rebases on the
			// CPU and so genuinely needs the per-mask tables. Kept honest
			// rather than lean: voxel.GPU.MeshChunkLocal 0 is meant to be the
			// PREVIOUS behaviour, and that included these reads.
			// PHASE 5: ...and not at all on a brick-only job, which has no mask
			// tables because it has no mesh chain. Without this term the
			// combination voxel.Terrain.RetireQuads 1 + voxel.GPU.MeshChunkLocal 0
			// would enqueue a copy from a NULL buffer -- a combination nobody
			// would run on purpose and exactly the kind that gets run by accident
			// while bisecting something else.
			if (Job->bQuadMesh && !Job->Region.bChunkLocalQuads)
			{
				Job->CountsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Counts"));
				Job->OffsetsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Offsets"));

				AddEnqueueCopyPass(GraphBuilder, Job->CountsReadback.Get(), Graph.Counts,
				                   Job->Sizes.CountsBytes());
				AddEnqueueCopyPass(GraphBuilder, Job->OffsetsReadback.Get(), Graph.Offsets,
				                   Job->Sizes.CountsBytes());
			}

			// --- P1-C: the brick region, in THIS graph ---------------------
			//
			// A SECOND AddRegionPasses call rather than a flag on the first,
			// because the two regions are different shapes: the mesher needs its
			// one-brick halo and the packer must not have one (brickpack.ush
			// decomposes from the region corner, so a halo region would pack the
			// halo). Same FRDGBuilder, so this is one graph, one submission, and
			// one place to read the split in a ProfileGPU capture.
			//
			// A failure here does NOT fail the job. The brick volume is off-path
			// in this phase: nothing draws from it, so a chunk that meshed
			// correctly and failed to pack is a chunk with no volume, not a hole
			// in the world. It stays loud in the log rather than becoming an
			// outcome.
			if (Job->bBrickPack)
			{
				// A SCOPE, BECAUSE THE SPLIT IS THE DELIVERABLE. The brick
				// region runs its own ColumnMain and VoxelizeMain, and those
				// passes carry the SAME RDG event names as the mesh region's --
				// so without this a ProfileGPU capture shows two of each and no
				// way to tell which cost what. The phase is gated on "BrickPack's
				// added GPU cost in the ProfileGPU split"; a capture that cannot
				// attribute it does not answer the question.
				RDG_EVENT_SCOPE(GraphBuilder, "Voxel.BrickRegion");

				const VoxelGpuWorldGen::FRegionGraphResources BrickGraph =
					VoxelGpuWorldGen::AddRegionPasses(GraphBuilder, Job->BrickRegion);

				if (BrickGraph.BrickDesc == nullptr || BrickGraph.BrickOcc == nullptr ||
				    BrickGraph.BrickMat == nullptr || BrickGraph.BrickChunkMask == nullptr ||
				    BrickGraph.BrickTotals == nullptr)
				{
					UE_LOG(LogTemp, Error,
					       TEXT("Job %llu asked for a brick pack and the graph produced no brick buffers. ")
					       TEXT("The mesh half of this job is unaffected."), Job->JobId);
					Job->bBrickPack = false;
				}
				else
				{
					Job->BrickPayload = MakeShared<FVoxelGpuBrickPayload, ESPMode::ThreadSafe>();
					// These have to survive this graph: the pool write runs in a
					// later one, once the totals have said how much to allocate.
					Job->BrickPayload->Desc = GraphBuilder.ConvertToExternalBuffer(BrickGraph.BrickDesc);
					Job->BrickPayload->Occ = GraphBuilder.ConvertToExternalBuffer(BrickGraph.BrickOcc);
					Job->BrickPayload->Mat = GraphBuilder.ConvertToExternalBuffer(BrickGraph.BrickMat);
					Job->BrickPayload->ChunkMask =
						GraphBuilder.ConvertToExternalBuffer(BrickGraph.BrickChunkMask);
					Job->BrickPayload->SrcBrickFirst = 0;
					Job->BrickPayload->SrcChunkIndex = 0;
					Job->BrickPayload->BrickCount = BrickGraph.Sizes.NumBricks;
					Job->BrickPayload->OriginVoxel = Job->BrickOriginVoxel;

					Job->BrickTotalReadback =
						MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.BrickTotals"));
					AddEnqueueCopyPass(GraphBuilder, Job->BrickTotalReadback.Get(),
					                   BrickGraph.BrickTotals, 2 * uint32(sizeof(uint32)));
				}
			}

			Built.Add(Job);
		}

		// No SubmitAndBlockUntilGPUIdle. Execute records the work and returns;
		// the readbacks land whenever the GPU gets to them, and Tick's poll is
		// what notices.
		GraphBuilder.Execute();

		const double Now = FPlatformTime::Seconds();
		for (const FJobPtr& Job : Built)
		{
			Job->DispatchSeconds = Now;
			Job->SetState(EJobState::Dispatched);
		}
	});

	TickStageMs.EnqueueMs += (FPlatformTime::Seconds() - DispatchBatchStart) * 1000.0;
}

// PHASE 2. The total has landed, so fetch exactly that many quads out of the
// buffer phase 1 kept alive.
//
// THIS IS THE TRANSITIONAL HALF OF D3. Once the pool's buffers are UAV-writable
// and RDG-registered (D1), this stops being a readback at all: it becomes
// Pool.Alloc(NumQuads) plus a GPU->GPU copy into the allocated range, the quad
// stream never touches system memory, and delivery happens off the 4-byte total
// alone. The structure here is deliberately the shape that change slots into —
// one place that turns a known quad count into a destination.
void FVoxelGpuMeshJobManager::DispatchQuadFetch(TArray<FJobPtr>&& Batch)
{
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshFetchQuads)(
		[Jobs = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		TArray<FJobPtr> Built;
		Built.Reserve(Jobs.Num());

		for (const FJobPtr& Job : Jobs)
		{
			if (Job->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}
			if (!Job->QuadBuffer.IsValid())
			{
				Job->Error = TEXT("quad buffer did not survive phase 1");
				Job->SetState(EJobState::Failed);
				continue;
			}

			FRDGBufferRef Quads = GraphBuilder.RegisterExternalBuffer(
				Job->QuadBuffer, TEXT("Voxel.Async.QuadsPersistent"));

			// Exactly the live range. Offsets are exclusive-scanned from zero,
			// so the live quads are [QuadWriteBase, QuadWriteBase + NumQuads)
			// and the base is zero on every path today — the copy starts at 0
			// and the harvest trims any prefix.
			const uint32 Bytes =
				(Job->Sizes.QuadWriteBase + Job->NumQuads) * uint32(sizeof(uint64));

			Job->QuadsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Quads"));
			AddEnqueueCopyPass(GraphBuilder, Job->QuadsReadback.Get(), Quads, Bytes);

			Built.Add(Job);
		}

		GraphBuilder.Execute();

		for (const FJobPtr& Job : Built)
		{
			Job->SetState(EJobState::QuadsDispatched);
		}
	});
}

// PHASE 2, DIRECT PATH (Wave D / D1). The total has landed, so move exactly
// that many quads out of the emit pass's upper-bound buffer into one sized to
// them -- and read back NOTHING.
//
// THE JOB IS ALREADY DELIVERABLE WHEN THIS IS ENQUEUED. That is the whole
// latency argument for D1 and it is worth stating plainly: nothing on the CPU
// reads this pass's output, so there is no readback to poll, no second poll
// quantum, and no reason for the game thread to wait. The caller sets
// ReadbackDone in the same tick it calls this.
//
// CORRECTNESS DOES NOT DEPEND ON THIS PASS RUNNING. The payload was built on
// the game thread already pointing at the phase-1 quad buffer at QuadWriteBase,
// so if this command were dropped the pool write would still copy the right
// quads -- just out of a buffer 60-100x larger than it needs to be. What this
// buys is MEMORY: a delivered chunk waiting on the streaming apply budget pins
// ~10 KB instead of the 786 KB static bound, and that queue is not bounded by
// the fork's in-flight cap (a delivered job has already left it) while
// deliveries have been measured outrunning applies under load.
// TAKES THE PAYLOADS, NOT THE JOBS, AND THAT IS NOT TIDINESS. Deliver() moves
// Job->Payload out on the GAME thread in this same Tick, so a render command
// that reached for it through the job would find it null -- or, worse, find it
// mid-move. The payload is self-contained and reference-counted, so capturing it
// directly makes this command independent of the job's lifetime entirely: it
// cannot see a half-torn-down job and does not need the Abandoned check every
// other render command here has, because there is no job state left to protect.
void FVoxelGpuMeshJobManager::DispatchQuadCompact(TArray<FVoxelGpuQuadPayloadRef>&& Batch)
{
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshCompactQuads)(
		[Payloads = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		for (const FVoxelGpuQuadPayloadRef& Payload : Payloads)
		{
			if (!Payload.IsValid() || !Payload->Quads.IsValid() || Payload->NumQuads == 0)
			{
				continue;
			}

			FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(
				Payload->Quads, TEXT("Voxel.Async.QuadsPersistent"));

			FRDGBufferRef Compacted = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2, Payload->NumQuads),
				TEXT("Voxel.Async.QuadsCompact"));

			VoxelGpuWorldGen::AddQuadCompactPass(GraphBuilder, Compacted, Src,
			                                     Payload->SrcFirst, Payload->NumQuads);

			// Swap the payload onto the small buffer. Both fields are
			// render-thread-owned (see FVoxelGpuQuadPayload) and the only later
			// reader is the pool component's write pass, in a render command the
			// game thread enqueues strictly after this one.
			//
			// The old reference is dropped by the assignment, AFTER
			// ConvertToExternalBuffer and after the pass reading it is recorded.
			// RDG holds its own reference to a registered external buffer for the
			// duration of the graph, so the source cannot go away underneath the
			// pass this graph is about to execute.
			Payload->Quads = GraphBuilder.ConvertToExternalBuffer(Compacted);
			Payload->SrcFirst = 0;
		}

		GraphBuilder.Execute();
	});
}

void FVoxelGpuMeshJobManager::PollInFlight()
{
	const double Now = FPlatformTime::Seconds();

	// --- poll both pending phases in one render command ---------------------
	TArray<FJobPtr> ToPoll;
	for (const FJobPtr& Job : InFlight)
	{
		const EJobState State = Job->GetState();
		if (State != EJobState::Dispatched && State != EJobState::QuadsDispatched)
		{
			continue;
		}
		int32 Expected = 0;
		if (Job->PollPending.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel))
		{
			ToPoll.Add(Job);
		}
	}

	if (ToPoll.Num() > 0)
	{
		// Latched here rather than read on the render thread, for the same
		// reason Submit latches MeshChunkLocal: one poll should behave like one
		// poll, not change budget halfway through.
		const int32 HarvestCap = GVoxelGpuMeshHarvestCap;

		ENQUEUE_RENDER_COMMAND(VoxelGpuMeshPoll)(
			[Jobs = MoveTemp(ToPoll), HarvestCap](FRHICommandListImmediate&)
		{
			// HOW MUCH COPYING THIS ONE COMMAND MAY DO (2026-07-27).
			//
			// The walk itself and every IsReady() stay unbounded: they are a
			// load and a compare, and skipping them would only delay noticing.
			// What is bounded is the HARVEST — Lock / memcpy / Unlock across up
			// to four or five staging buffers per job, and on phase 2 a memcpy
			// of the whole live quad stream. With 150-256 jobs in flight a
			// single poll could do every one of those in one render command,
			// which is a render-thread stall of exactly the shape the hitch
			// legs show.
			//
			// Jobs over budget are LEFT UNTOUCHED in Dispatched /
			// QuadsDispatched with their readbacks still ready; PollPending is
			// still cleared for them on the way out, so the next tick re-polls
			// them and harvests them then. Nothing is dropped and no state
			// moves, so this cannot interact with the timeout, the all-or-none
			// rule, or delivery.
			//
			// It also transitively bounds phase 2: at most one job reaches
			// TotalDone per phase-1 harvest, so the next tick's
			// DispatchQuadFetch graph is bounded by this cap too.
			int32 HarvestBudget = HarvestCap > 0 ? HarvestCap : MAX_int32;

			for (const FJobPtr& Job : Jobs)
			{
				ON_SCOPE_EXIT { Job->PollPending.store(0, std::memory_order_release); };

				if (Job->Abandoned.load(std::memory_order_acquire) != 0)
				{
					continue;
				}

				const EJobState State = Job->GetState();

				// --- phase 1: the 4-byte total (+ the control path's tables) --
				if (State == EJobState::Dispatched)
				{
					const bool bNeedTables = Job->bQuadMesh && !Job->Region.bChunkLocalQuads;
					const bool bNeedBand = Job->Region.BandEdge > 0;
					// P1-C. Rides phase 1 for the band's reason: one job, one
					// completion event. A brick total that landed on its own
					// would be a second async stream to satisfy exactly once.
					const bool bNeedBricks = Job->bBrickPack;
					// PHASE 5: a brick-only job never enqueued a quad total, so
					// requiring it here would fail every such job with "phase 1
					// readback objects missing" -- which is the failure mode this
					// whole state machine is built to make impossible.
					const bool bNeedQuadTotal = Job->bQuadMesh;
					if ((bNeedQuadTotal && !Job->TotalReadback.IsValid()) ||
					    (bNeedBricks && !Job->BrickTotalReadback.IsValid()) ||
					    (bNeedBand && !Job->BandReadback.IsValid()) ||
					    (bNeedTables && (!Job->CountsReadback.IsValid() ||
					                     !Job->OffsetsReadback.IsValid())))
					{
						Job->Error = TEXT("phase 1 readback objects missing");
						Job->SetState(EJobState::Failed);
						continue;
					}
					// All or none: a total that landed without its tables would
					// have the control path rebasing against stale offsets, and
					// a total that landed without its band would deliver a
					// result the streaming path reads as "this footprint has no
					// band" — which is silent, and costs a whole (X,Y) column
					// its cold-band throttle release.
					if ((bNeedQuadTotal && !Job->TotalReadback->IsReady()) ||
					    (bNeedBricks && !Job->BrickTotalReadback->IsReady()) ||
					    (bNeedBand && !Job->BandReadback->IsReady()) ||
					    (bNeedTables && (!Job->CountsReadback->IsReady() ||
					                     !Job->OffsetsReadback->IsReady())))
					{
						continue;
					}

					// Stamped on FIRST OBSERVED READY, before the budget check,
					// not on harvest. DispatchToReadyMs is documented as "the
					// moment a poll first saw IsReady() true"; deferring the
					// copies must not silently re-point that at a later poll,
					// which would inflate the one number that describes the GPU
					// by however long this fix spread the CPU work over. Guarded
					// because a deferred job is re-polled and would otherwise
					// restamp.
					if (Job->ReadySeconds <= 0.0)
					{
						Job->ReadySeconds = FPlatformTime::Seconds();
					}

					// Over budget: leave the job exactly as it is — still
					// Dispatched, readbacks still ready — and let the next poll
					// harvest it. Placed AFTER the all-or-none readiness gate so
					// the budget is only ever spent on a job that will actually
					// copy, and BEFORE the first Lock so a job is never half
					// harvested.
					if (HarvestBudget <= 0)
					{
						continue;
					}
					--HarvestBudget;

					// The GPU number. This is what D3 exists to fetch, and it
					// is 4 bytes.
					//
					// PHASE 5: brick-only leaves NumQuads at its initial 0, which
					// is not a shortcut -- 0 is the TRUE quad count of a job that
					// dispatched no mesh chain, and the zero-quad branch in the
					// phase-2 starter already carries such a job straight to
					// ReadbackDone without a payload. Nothing downstream needs a
					// brick-only special case.
					FString Error;
					bool bOk = true;
					if (bNeedQuadTotal)
					{
						bOk = CopyReadback(*Job->TotalReadback, &Job->NumQuads,
						                   sizeof(uint32), TEXT("QuadTotal"), Error);
					}

					if (bOk && bNeedBricks)
					{
						// The two dword counts the pool allocation is made from.
						// A failure here is NOT allowed to fail the job -- see
						// the dispatch note -- so it clears the brick half and
						// leaves the mesh half alone.
						FString BrickError;
						if (!CopyReadback(*Job->BrickTotalReadback, Job->BrickTotals,
						                  2 * uint32(sizeof(uint32)), TEXT("BrickTotals"), BrickError))
						{
							UE_LOG(LogTemp, Error,
							       TEXT("Job %llu: brick totals readback failed (%s). The chunk meshes ")
							       TEXT("normally and simply has no resident volume."),
							       Job->JobId, *BrickError);
							Job->bBrickPack = false;
							Job->BrickPayload.Reset();
						}
					}

					if (bOk && bNeedBand)
					{
						bOk = CopyReadback(*Job->BandReadback, Job->Band,
						                   2 * uint32(sizeof(int32)), TEXT("Band"), Error);
						Job->bBandValid = bOk;
					}

					if (bOk && bNeedTables)
					{
						const uint32 MaskCount = Job->Sizes.MaskCount;
						Job->Counts.SetNumUninitialized(int32(MaskCount));
						Job->Offsets.SetNumUninitialized(int32(MaskCount));
						bOk = CopyReadback(*Job->CountsReadback, Job->Counts.GetData(),
						                   Job->Sizes.CountsBytes(), TEXT("Counts"), Error) &&
						      CopyReadback(*Job->OffsetsReadback, Job->Offsets.GetData(),
						                   Job->Sizes.CountsBytes(), TEXT("Offsets"), Error);
					}

					if (!bOk)
					{
						Job->Error = Error;
						Job->SetState(EJobState::Failed);
						continue;
					}
					Job->SetState(EJobState::TotalDone);
					continue;
				}

				// --- phase 2: exactly NumQuads quads --------------------------
				if (State == EJobState::QuadsDispatched)
				{
					if (!Job->QuadsReadback.IsValid())
					{
						Job->Error = TEXT("phase 2 readback object missing");
						Job->SetState(EJobState::Failed);
						continue;
					}
					if (!Job->QuadsReadback->IsReady())
					{
						continue;
					}

					// Same budget as phase 1, and phase 2 is the expensive half
					// — this memcpy is the whole live quad stream, ~7-12 KB per
					// chunk, where phase 1's is four bytes.
					if (HarvestBudget <= 0)
					{
						continue;
					}
					--HarvestBudget;

					const uint32 Elements = Job->Sizes.QuadWriteBase + Job->NumQuads;
					Job->RawQuads.SetNumUninitialized(int32(Elements));

					FString Error;
					if (!CopyReadback(*Job->QuadsReadback, Job->RawQuads.GetData(),
					                  Elements * uint32(sizeof(uint64)), TEXT("Quads"), Error))
					{
						Job->Error = Error;
						Job->SetState(EJobState::Failed);
						continue;
					}
					Job->SetState(EJobState::ReadbackDone);
				}
			}
		});
	}

	// --- start phase 2 for anything whose total has landed -------------------
	//
	// Game thread, because it is the thread that owns the decision and because
	// it is where the "is this count even sane" check belongs. Batched into one
	// render command for the same reason phase 1 is.
	//
	// TWO PHASE TWOS (Wave D / D1), chosen by the flag latched at Submit. The
	// range check and the zero-quad short-circuit below are SHARED, deliberately:
	// they are statements about the total the GPU reported, not about what is
	// done with it, and duplicating them into each branch is how the two paths
	// would drift.
	{
		TArray<FJobPtr> ToFetch;
		TArray<FVoxelGpuQuadPayloadRef> ToCompact;
		for (const FJobPtr& Job : InFlight)
		{
			if (Job->GetState() != EJobState::TotalDone)
			{
				continue;
			}
			int32 Expected = 0;
			if (!Job->QuadFetchStarted.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel))
			{
				continue;
			}

			if (Job->NumQuads > Job->Sizes.MaxQuads)
			{
				// Caught HERE rather than after a fetch, because the fetch would
				// be sized from this number: a corrupt total would ask for a
				// copy far larger than the buffer.
				Job->Error = FString::Printf(
					TEXT("QuadTotalMain reports %u quads but the buffer holds at most %u"),
					Job->NumQuads, Job->Sizes.MaxQuads);
				Job->SetState(EJobState::Failed);
				continue;
			}

			if (Job->NumQuads == 0)
			{
				// An all-air or all-solid chunk. Nothing to fetch, so it skips
				// phase 2 entirely and delivers a frame earlier — which is not
				// an edge case at level 0, where most of the vertical stack is
				// one or the other.
				//
				// The direct path takes this branch UNCHANGED and publishes no
				// payload: a zero-quad chunk allocates no pool range, exactly as
				// it creates no component on the CPU path. ApplyMeshResult's
				// Quads.Num() == 0 branch is what handles it either way.
				Job->RawQuads.Reset();
				Job->SetState(EJobState::ReadbackDone);
				continue;
			}

			if (Job->bDirectToPool)
			{
				// Wave D / D1. Build the payload HERE, on the game thread,
				// already pointing at the phase-1 buffer -- so it is valid
				// whether or not the compaction command below ever runs, and the
				// job can be delivered without waiting for anything.
				//
				// QuadWriteBase is 0 on every path today; it is carried rather
				// than assumed because a copy that starts at the wrong offset
				// produces geometry that is plausible and somebody else's.
				Job->Payload = MakeShared<FVoxelGpuQuadPayload, ESPMode::ThreadSafe>();
				Job->Payload->Quads = Job->QuadBuffer;
				Job->Payload->SrcFirst = Job->Sizes.QuadWriteBase;
				Job->Payload->NumQuads = Job->NumQuads;

				// Deliverable NOW. There is no readback to poll, so the job goes
				// straight to ReadbackDone and the harvest loop below picks it up
				// in this same Tick -- a whole round trip and a whole poll
				// quantum earlier than the readback path.
				Job->SetState(EJobState::ReadbackDone);
				// A COPY of the handle, not the job. Deliver() moves the job's
				// own reference away later in this same Tick.
				ToCompact.Add(Job->Payload);
				continue;
			}

			ToFetch.Add(Job);
		}

		if (ToFetch.Num() > 0)
		{
			DispatchQuadFetch(MoveTemp(ToFetch));
		}
		// AFTER the fetch dispatch, so that on a mixed tick the two commands go
		// out in the order their jobs were promoted. Nothing depends on it
		// today -- the two sets are disjoint by construction -- but the ordering
		// is free and the alternative is an ordering nobody chose.
		if (ToCompact.Num() > 0)
		{
			DispatchQuadCompact(MoveTemp(ToCompact));
		}
	}

	// Harvest, in two phases.
	//
	// PHASE 1 decides and detaches; PHASE 2 delivers. They are separate because a
	// completion callback is free to call back into the manager -- Submit is the
	// obvious one, but CancelAll or destroying the manager are both legitimate
	// reactions to a failure, and either would mutate InFlight underneath a loop
	// that was still iterating it. Detaching first means the delivery loop owns
	// its jobs outright and nothing it triggers can invalidate them.
	struct FPending
	{
		FJobPtr Job;
		EVoxelGpuMeshJobStatus Status;
		FString Error;
	};
	TArray<FPending> Finished;

	for (int32 I = InFlight.Num() - 1; I >= 0; --I)
	{
		const FJobPtr Job = InFlight[I];
		const EJobState State = Job->GetState();

		if (State == EJobState::ReadbackDone)
		{
			// NumQuads came off the GPU in phase 1 and was range-checked before
			// phase 2 was sized from it, so there is nothing left to derive
			// here. On the brick-local control path the per-mask tables are
			// present too and RebaseQuadsToChunkLocal walks them; on the
			// chunk-local path they were never read.
			InFlight.RemoveAt(I, EAllowShrinking::No);

			// Drop the QuadWriteBase prefix, leaving exactly this job's live
			// quads. The prefix is empty on every path today.
			if (Job->Sizes.QuadWriteBase > 0 && Job->RawQuads.Num() > 0)
			{
				Job->RawQuads.RemoveAt(0, int32(Job->Sizes.QuadWriteBase), EAllowShrinking::No);
			}
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::Success, FString() });
			continue;
		}

		if (State == EJobState::Failed)
		{
			InFlight.RemoveAt(I, EAllowShrinking::No);
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::DispatchFailed, Job->Error });
			continue;
		}

		if (Now - Job->SubmitSeconds > TimeoutSeconds)
		{
			// Device loss, a wedged queue, or a render thread that never ran the
			// command. Give up on it, but keep the job object alive for any
			// render command still holding a reference.
			Job->Abandoned.store(1, std::memory_order_release);
			InFlight.RemoveAt(I, EAllowShrinking::No);
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::TimedOut,
				FString::Printf(TEXT("readback not ready after %.1f s"), TimeoutSeconds) });
		}
	}

	// Deliver first, release once. The releases are collected across the whole
	// delivery loop and flushed as a single render command — see
	// ReleaseReadbacksOnRenderThread. Collecting them does NOT extend any job's
	// lifetime past this function: these jobs are already detached from InFlight
	// and the command holds its own reference either way.
	//
	// A callback that reenters (Submit, CancelAll, destroying the manager) is
	// still safe: it can only touch jobs still in Queued/InFlight, and every job
	// in Finished was removed from InFlight before this loop started, so the two
	// sets are disjoint and a reentrant CancelAll's own release command simply
	// lands ahead of this one.
	TArray<FJobPtr> ToRelease;
	ToRelease.Reserve(Finished.Num());
	for (const FPending& P : Finished)
	{
		Deliver(P.Job, P.Status, P.Error);
		ToRelease.Add(P.Job);
	}
	ReleaseReadbacksOnRenderThread(MoveTemp(ToRelease));
}

void FVoxelGpuMeshJobManager::CancelAll()
{
	TArray<FJobPtr> Outstanding;
	Outstanding.Append(Queued);
	// LOW-PRIORITY JOBS ARE CANCELLED LIKE ANY OTHER. Resetting the queue without
	// appending it here would drop them silently, and invariant #2 at the top of
	// this header is that every job id submitted is delivered to OnJobComplete
	// EXACTLY ONCE. A dropped one leaks its GpuJobsPending entry on the streaming
	// side, which logs an Error about a leaked dispatch slot -- a long way from
	// the cause.
	Outstanding.Append(QueuedLowPriority);
	Outstanding.Append(InFlight);
	Queued.Reset();
	QueuedLowPriority.Reset();
	InFlight.Reset();

	for (const FJobPtr& Job : Outstanding)
	{
		Job->Abandoned.store(1, std::memory_order_release);
		Deliver(Job, EVoxelGpuMeshJobStatus::Cancelled, TEXT("manager shut down"));
	}

	// One command for all of them, same as the steady-state path. Queued and
	// InFlight were reset before delivery, so this is still idempotent: a
	// reentrant CancelAll finds nothing outstanding and enqueues nothing.
	ReleaseReadbacksOnRenderThread(MoveTemp(Outstanding));
}

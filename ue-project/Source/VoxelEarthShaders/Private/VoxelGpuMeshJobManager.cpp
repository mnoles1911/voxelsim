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

// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2) -- NOW VESTIGIAL,
// kept registered so legs and scripts that set it do not start erroring.
//
// It existed to gate the one FPlatformTime::Seconds() call S0-3 added
// (FJob::PromotedSeconds, stamped in Tick's promote loop) so the instrument
// could not become what was being measured. Then the timeout-clock fix
// (2026-08-22: 4,480 of 8,984 jobs spuriously timed out because the timeout
// ran from SUBMIT, not promotion) made PromotedSeconds drive the in-flight
// TIMEOUT, which must not change behaviour with a stats cvar -- so the stamp
// went unconditional, and with it every stage field on the result is measured
// on every run. As of 2026-08-23 this cvar gates NOTHING in this file; the
// stage breakdown prints in the streaming side's 5-second window regardless.
static int32 GVoxelGpuMeshLatencyStats = 0;
static FAutoConsoleVariableRef CVarVoxelGpuMeshLatencyStats(
	TEXT("voxel.GPU.MeshLatencyStats"),
	GVoxelGpuMeshLatencyStats,
	TEXT("VESTIGIAL (2026-08-23): the stamps it used to gate are unconditional now (the in-flight ")
	TEXT("timeout depends on FJob::PromotedSeconds, and a timeout must not move with a stats cvar). ")
	TEXT("Setting it does nothing; kept registered so existing legs do not error."),
	ECVF_Default);

// --- Tier B.1: amortise the worldgen/pack passes across Z-sibling chunks ----
//
// THE PROBLEM THIS REMOVES, in the numbers that forced it. Every brick-only
// job costs its own region graph: ~12 compute passes plus a readback copy for
// the brick chain, all of it render-thread pass setup that is FIXED PER CHUNK.
// That fixed cost, not ALU and not bandwidth, is the ceiling --
// voxel.GPU.MeshBatchCap's own comment records the sweep that proved it (32/64
// gave 367 hitches / 77.2k chunks against 4/8's 8 / 89.4k: bigger bursts of
// per-chunk graphs are WORSE). So the fix is not a bigger cap; it is making
// the pass count constant in the number of chunks.
//
// WHAT THE BATCHED SHAPE IS. Streaming demand arrives as vertical COLUMN
// STACKS -- the derivation in docs/marcher-handoff-2026-08-22.md gives 8.3
// level-0 chunks per column, and the CPU-side finding that "the 8-16
// Z-siblings rebuild the same column grid identically" has an exact GPU twin:
// each sibling's ColumnMain re-derives the same 32x32 columns. So the batch
// unit is a CONTIGUOUS Z-RUN of same-column brick-only jobs, fused into ONE
// tall region (32x32 columns, BricksZ = 4K) and dispatched through the SAME
// AddRegionPasses the per-chunk path uses. No generation kernel changed:
// worldgen.ush reads its per-region scalars only at kernel entry, and
// brickpack.ush's decodeBrick already decomposes a multi-chunk region
// chunk-major -- the batched region is a shape the kernels were built for and
// the streaming path simply never sent. K chunks then cost ONE Column, ONE
// Voxelize (with the per-column cave/cavern reductions done once per stack
// instead of once per chunk), ONE classify, ONE set of scans, ONE pack, and
// ONE (2+2K)-dword totals readback: ~14 passes per stack against ~15 PER
// CHUNK before.
//
// WHAT IT DOES NOT DO. The pool flush still writes per chunk (2 word copies +
// desc write + record, ~4 small passes each) -- that is the remaining linear
// term, kept because per-chunk pool allocations are not contiguous and fusing
// those writes needs new kernels over a per-chunk table, a separate change
// with its own gate. And it does NOT reintroduce a readback: the only bytes
// that cross PCIe per stack are the (2+2K)-dword totals -- fewer than the K
// separate 2-dword readbacks they replace.
//
// BIT-EXACTNESS IS BY CONSTRUCTION, THEN CHECKED ANYWAY. Same kernels, same
// per-world-coordinate inputs (sibling raster windows are verified IDENTICAL
// before grouping, not assumed), so per-chunk cells are the same bytes; the
// chunk-major pack order makes each chunk's arena run contiguous and
// internally identical to a single-chunk dispatch. voxel.GPU.VerifyCoarse is
// untouched and still proves the kernels against the CPU; the new
// voxel.GPU.VerifyBrickStack proves batched == per-chunk on columns, cells,
// descriptors, arena words and totals; and EVERY live stack cross-checks
// sum(per-chunk totals) == region totals at harvest, failing the whole stack
// loudly rather than publishing a wrong split.
//
// DEFAULT 0: the control graph is byte-identical with the switch off, which
// is what makes the A/B one flag on one binary. Command-line override for
// VoxelGpuBrickPackEnabled's -ExecCmds startup-window reason; the CVAR is
// also honoured mid-run because a flip cannot corrupt anything -- both paths
// produce the same bytes, so the only residue of a mid-run flip is in the
// stats, not the world.
static int32 GVoxelGpuWorldGenBatch = 0;
static FAutoConsoleVariableRef CVarVoxelGpuWorldGenBatch(
	TEXT("voxel.GPU.WorldGenBatch"),
	GVoxelGpuWorldGenBatch,
	TEXT("1 = fuse contiguous Z-sibling brick-only mesh jobs into one tall region per column ")
	TEXT("stack: ONE set of worldgen+pack passes and ONE totals readback for K chunks, instead ")
	TEXT("of K of each. 0 (default) = today's per-chunk graphs, byte-identical. Outputs are ")
	TEXT("bit-exact either way (gate: voxel.GPU.VerifyBrickStack); read once per Tick."),
	ECVF_Default);

bool VoxelGpuWorldGenBatchEnabled()
{
	// -VoxelGpuWorldGenBatch=<n> outranks the cvar, for the reason recorded on
	// VoxelGpuBrickPackEnabled: -ExecCmds cvars land after streaming has
	// begun. Here that is a STATS blemish rather than a correctness one (the
	// two paths emit identical bytes), but a leg whose counters cover only
	// half the run is still a leg someone will misread.
	static const int32 CmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorldGenBatch="), Value);
		return Value;
	}();
	return CmdLine >= 0 ? CmdLine != 0 : GVoxelGpuWorldGenBatch != 0;
}

// --- P3 prep (2026-08-23): the promotion quotas become sweepable ------------
//
// WHY THESE EXIST. The armed-fork legs (Saved/p1p2-armed.log, p2-verify-armed.log)
// put 93% of the fork's submit->deliver latency in QUEUE WAIT: mean
// queued=2,187.5 ms of submitToDeliver=2,350.0 ms over n=25,387 complete jobs,
// against dispatchToReady=70.1 ms of actual GPU+poll time. Little's law closes
// the loop exactly: delivered rate 89/s == MeshBatchCap (4) x the leg's ~24 fps,
// and the observed gpuInFlight ~12 == 4/tick x the ~3-tick promote->deliver
// latency. THE PER-TICK PROMOTION QUOTA IS THE FORK'S THROUGHPUT CEILING, not
// MaxInFlight (256, never approached) and not the GPU (idle-dominated).
//
// The quotas were already cvars, but -ExecCmds cvars land after streaming has
// begun (the standing reason on VoxelGpuBrickPackEnabled), so an A/B sweep off
// the cvar silently measures a blend of two regimes. These latches make the
// sweep honest: -VoxelGpuMeshBatchCap=N / -VoxelGpuMeshSpecCap=N /
// -VoxelGpuMeshHarvestCap=N outrank the cvars for the whole process.
//
// THE OLD SWEEP DOES NOT PRE-ANSWER THE NEW ONE. MeshBatchCap's own comment
// records 32/64 hitching (367 hitches vs 4/8's 8) -- but that sweep ran with
// every job paying TWO region graphs (the 48x48x6 mesh region plus the 32x32x4
// brick region; see VoxelGpuLeanBrickJobsEnabled below). With the lean switch
// removing the larger graph, the per-job render-thread and GPU cost the old
// sweep choked on is ~4.4x smaller, so the knee is expected somewhere new --
// which is exactly what the sweep is for. Failure reading: raising the cap
// moves hitches, not chunks/s -- then pass setup still binds and the next fix
// is fused multi-chunk dispatch, not a bigger cap.
// -VoxelGpuPrimary=1 (default off, latched): see the exported declaration in
// VoxelGpuMeshJobManager.h. Defined here because this file owns the quota and
// lean defaults it re-points; the streaming module links against the same
// definition so both sides latch ONE parse of ONE flag.
bool VoxelGpuPrimaryEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuPrimary="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

static int32 VoxelGpuMeshBatchCapEffective()
{
	static const int32 CmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuMeshBatchCap="), Value);
		return Value;
	}();
	// GPU-primary default: 64, NOT the shipped 4 and NOT unbounded.
	//
	// The 4 was sized by a sweep in which every job paid TWO region graphs
	// (~17 passes: 32/64 hitched at 367 vs 4/8's 8), and Little's law made it
	// the fork's whole throughput ceiling once the graphs got cheap: delivered
	// 89/s == 4 x ~24 fps with the GPU idle-dominated and MaxInFlight never
	// approached. Under primary the promoted population is lean band-free
	// brick jobs (~5 passes, the band carriers capped at one per footprint by
	// seed-only) and, with WorldGenBatch armed, the cap counts STACK HEADS of
	// ~8 chunks each -- so 64 is ~320 passes/tick per-chunk worst case (the
	// render-thread cost of ~19 of the old classic jobs) and up to ~530
	// chunks/tick fused, ~16k chunks/s at 30 fps: above the CPU's measured
	// ~10.5k/s power-limited ceiling, on the road to the 50k/s target.
	//
	// Unbounded stays wrong here: cold fill queues hundreds of jobs, and one
	// FRDGBuilder taking MaxInFlight lean graphs in a burst frame is exactly
	// the 100+-pass hitch shape the cap exists to flatten.
	//
	// FAILING READINGS: hitches climbing with chunks/s flat -- pass setup
	// still binds, sweep DOWN with -VoxelGpuMeshBatchCap=N (which outranks
	// this default); delivered pinned at 64 x fps -- the quota binds again,
	// sweep UP and report the knee.
	return CmdLine >= 0 ? CmdLine : (VoxelGpuPrimaryEnabled() ? 64 : GVoxelGpuMeshBatchCap);
}

static int32 VoxelGpuMeshSpecCapEffective()
{
	static const int32 CmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuMeshSpecCap="), Value);
		return Value;
	}();
	return CmdLine >= 0 ? CmdLine : GVoxelGpuMeshSpeculativeBatchCap;
}

static int32 VoxelGpuMeshHarvestCapEffective()
{
	static const int32 CmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuMeshHarvestCap="), Value);
		return Value;
	}();
	// GPU-primary default: unbounded (0). The copies this cap was sized
	// against were whole-quad-stream memcpys on the readback path; the primary
	// configuration is marcher/brick (DirectToPool, PoolAlloc, lean band-free
	// jobs), where the only readbacks left to harvest are the per-footprint
	// band seeds and per-job totals -- a handful of ints each. At the shipped
	// 8/tick the cap would quantise band-seed delivery to 8 x fps (~240/s),
	// putting a ~65 s floor under warming the ~15.6k-footprint band cache that
	// the buried-skip and the cold-band throttle both feed from. A leg that
	// runs primary WITH quads un-retired should restore -VoxelGpuMeshHarvestCap=8
	// explicitly (it outranks this default).
	return CmdLine >= 0 ? CmdLine : (VoxelGpuPrimaryEnabled() ? 0 : GVoxelGpuMeshHarvestCap);
}

// --- LEAN BRICK-ONLY JOBS (-VoxelGpuLeanBrickJobs, default OFF) -------------
//
// WHAT THE FULL PATH WASTES. DispatchBatch runs AddRegionPasses(Job->Region) --
// the 48x48x6-brick MESH region, with its one-brick halo -- for EVERY job,
// before the halo-free 32x32x4 brick region that actually feeds the pool. On a
// brick-only job (voxel.Terrain.RetireQuads, the marcher build) that first
// graph's outputs are consumed by exactly one thing: the 2-int footprint BAND
// readback, and only on level-0 jobs that asked for one. A coarse brick-only
// job, and a level-0 job whose footprint band is already cached (the subsystem's
// -VoxelGpuBandColdOnly latch), reads NOTHING from it -- yet still pays
// 48x48=2,304 columns + 48x48x48 voxelize against the brick region's 1,024
// columns + 32x32x32: 3.4x the generation work, in a second set of ~7 passes,
// per chunk, for nothing.
//
// It also pays a LATENCY price: the band readback is the only fence left on a
// P1 (voxel.GPU.PoolAlloc) brick-only job. Without it the job has NO readback
// at all and is deliverable the tick after dispatch instead of riding the
// poll-quantised readback path (dispatchToReady 70.1 ms mean on p1p2-armed).
//
// Armed, a job that is brick-only AND band-free AND packing skips the mesh
// region graph entirely. Counted both ways (window line below): lean= must grow
// while armed or the switch is dead -- the "on and doing nothing" state this
// project keeps paying for is made unrepresentable by the counters, not by
// hope. Default OFF: the control graph is byte-identical without the flag.
static bool VoxelGpuLeanBrickJobsEnabled()
{
	// -1 sentinel so "absent" and "=0" stay distinguishable: an explicit
	// -VoxelGpuLeanBrickJobs=0 must win over -VoxelGpuPrimary's implied ON
	// (primary changes defaults, never outranks an explicit flag), or the
	// control arm of a lean A/B under primary would be unrunnable.
	static const bool bEnabled = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuLeanBrickJobs="), Value);
		return Value >= 0 ? Value != 0 : VoxelGpuPrimaryEnabled();
	}();
	return bEnabled;
}

// --- STACK-CLAIM (-VoxelGpuStackClaim, default OFF) -------------------------
//
// THE ROUND-TRIP-FREE PRODUCER'S MISSING THIRD. Tonight's measured chain: P1
// claims a classic job's ranges in-graph with zero readback (1,101,676 claims,
// 0 FAIL); P2 publishes index cells from the GPU (verify pass=8319 FAIL=0);
// but B.1's K-chunk fused stacks -- the only shape whose PASS COUNT is
// constant in K -- were DISABLED under the armed pool, because a stack's
// members landed through the (2+2K)-dword totals readback and a CPU-side
// prefix harvest the armed pool cannot use. So the armed configuration was
// forced to per-chunk graphs, and the per-tick promotion quota (the fork's
// measured ceiling: delivered 89/s == MeshBatchCap 4 x ~24 fps, queued
// p50 2.2-13 s, gpuDemandPending pinned at 253-255) counted CHUNKS.
//
// Armed, a fused stack's members claim their own ranges IN THE STACK'S GRAPH:
// the claim kernel reads member c's totals pair at [2+2c] and derives its
// shared-scratch prefix in-kernel (the arithmetic the CPU harvest used to do
// from the readback), and the write passes land words/descs/record through
// the member's own claim. NOTHING comes back -- the stack totals readback is
// not enqueued at all -- and the promotion quota now counts STACKS of ~8
// chunks (the measured column stack), an ~8x amortisation of both the quota
// and the per-chunk pass setup, on top of the mesh-region graphs the stack
// path already skips.
//
// Requires voxel.GPU.WorldGenBatch (or -VoxelGpuWorldGenBatch=1) AND
// -VoxelGpuPoolAlloc=1; without either it changes nothing. Default OFF:
// tonight's armed configuration is bit-for-bit the control. Correctness is
// watched by the SAME instruments as the classic claims -- the page-bitmap
// double-grant gate, the [brick-gpualloc] xcheck samples (stack members'
// shells enter the same verify ring), and voxel.GPU.VerifyBrickStack for the
// generation half. FAILING READINGS: xcheck FAIL>0 or doubleGrant>0
// invalidates the leg outright; [gpu-batch] stacks=0 with this armed means
// the fusion never fired and the arm measured the classic path wearing a new
// flag.
static bool VoxelGpuStackClaimEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuStackClaim="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// --- P3 SPINE (-VoxelGpuWorklist, default OFF) ------------------------------
//
// Runs the persistent worklist ring (VoxelGpuWorklist.h) under real traffic:
// every lean-eligible brick chunk appends a 64-byte record at dispatch, one
// Flush per tick uploads the segment, runs the args-setup pass, and dispatches
// the indirect spine prover whose group count comes off the GPU-owned cursor.
// A ~5 s proof readback (16 bytes) compares GPU consumption against the host
// mirror exactly -- see FVoxelGpuWorklist::Flush.
//
// WHAT THIS ARM DOES NOT DO YET, SAID PLAINLY SO A FLAT NUMBER CANNOT BE
// MISREAD: the generation kernels are not converted, so every chunk still
// pays its classic per-chunk/per-stack passes and PASSES-PER-TICK DOES NOT
// FLATTEN on this arm -- it gains the constant +2/tick spine (args + prover).
// Throughput will not move either (the 2026-08-23 four-arm sweep moved pass
// count 4.3x and throughput 0% at ~2,100 chunks/s; today's limiter is
// admission, not passes). What this arm buys is the VERIFIED dispatch spine
// the kernel conversion lands on, stage by stage -- the piece of the 50k
// arithmetic where 15 passes/chunk (25x the ~500/tick hitch cliff at 50k
// chunks/s) becomes ~14 passes/TICK (36x under it), because pass count stops
// scaling with N at all.
//
// Command-line latched, not a cvar: -ExecCmds lands after streaming has
// begun, and a mid-run flip would split the ring across two configurations
// (the -VoxelSurfaceMip reasoning, verbatim).
static bool VoxelGpuWorklistEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklist="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// Ring capacity in records (-VoxelGpuWorklistCap). 4,096 x 64 B = 256 KiB,
// ~5 ticks of the 50k target rate (834 records/tick at 60 fps); a full ring
// refuses appends and COUNTS them rather than growing -- back-pressure is a
// visible number, not a realloc.
static uint32 VoxelGpuWorklistCapacity()
{
	static const uint32 Cap = []
	{
		int32 Value = 4096;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistCap="), Value);
		return uint32(FMath::Clamp(Value, 64, 1 << 20));
	}();
	return Cap;
}

// Records the consuming dispatch may take per tick (-VoxelGpuWorklistBudget).
// 1,024 default: 1.2x the 834 records/tick that 50,000 chunks/s needs at
// 60 fps, so the budget is never the honest bottleneck in a 50k leg while
// still bounding a burst's dispatch footprint.
static uint32 VoxelGpuWorklistBudget()
{
	static const uint32 Budget = []
	{
		int32 Value = 1024;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistBudget="), Value);
		return uint32(FMath::Clamp(Value, 1, 1 << 20));
	}();
	return Budget;
}

// --- P3 STAGE 1: the CONVERTED Column kernel (-VoxelGpuWorklistColumns) -----
//
// On top of -VoxelGpuWorklist=1: the flush graph dispatches ColumnWorklistMain
// (one indirect pass per tick, 16 groups per consumed record) into a
// flush-level column arena, and every chunk whose record was consumed this
// tick SKIPS its own ColumnMain pass -- its VoxelizeMain reads the arena slice
// instead. Passes drop by 1 x chunks/tick (17 -> 16 on the lean-alloc shape);
// they go FLAT only when the remaining six stages convert. Requires the flush
// to run BEFORE the batch graph in the same tick (DispatchBatch does that when
// this is armed), because the batch's VoxelizeMain reads what the flush's
// column dispatch wrote. Command-line latched, same reasoning as the spine.
static bool VoxelGpuWorklistColumnsEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistColumns="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The stage-2 byte gate (-VoxelGpuWorklistVerifyCols): every converted chunk
// ALSO runs the classic ColumnMain into a transient plus a 16-group compare
// pass; mismatching dwords ride the proof readback and any nonzero is a loud
// leg-invalidating Error (see FVoxelGpuWorklist::Flush). Costs +2 passes per
// converted chunk -- a verify arm, never a production one.
static bool VoxelGpuWorklistVerifyColsEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyCols="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistVoxelize=1 (P3 stage 2), on top of -VoxelGpuWorklistColumns:
// the flush graph ALSO dispatches VoxelizeWorklistMain (one indirect pass per
// tick) into a flush-level cell arena, and every asset-free chunk whose record
// was consumed this tick skips its own VoxelizeMain pass too -- its brick
// chain reads the arena slice through brickpack.ush's CellReadBase. 16 -> 15
// passes on the lean-alloc shape. Requires the column stage (the voxelize
// kernel READS the column arena); armed without it, the worklist refuses to
// dispatch it and every chunk falls back, counted. Command-line latched.
static bool VoxelGpuWorklistVoxelizeEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVoxelize="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The Voxelize stage's byte gate (-VoxelGpuWorklistVerifyVox): every cell-fed
// chunk ALSO runs the classic VoxelizeMain into the transient plus a 512-group
// compare pass into stats [6..7], riding the proof readback. +2 passes per
// converted chunk -- a verify arm, never a production one.
static bool VoxelGpuWorklistVerifyVoxEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyVox="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistClassify=1 (P3 stage 3), on top of -VoxelGpuWorklistVoxelize:
// the flush graph ALSO dispatches the fused ClassifyTotals pair (two indirect
// passes per tick: one group per brick classify off the cell arena, then a
// one-group-per-record in-group scan + totals), and every cell-fed chunk skips
// its classic BrickClassifyMain, BOTH 3-pass scans and BrickTotalMain -- EIGHT
// passes, the conversion's largest single cut (15 -> 7 on the lean-alloc
// shape). BrickPackMain reads offsets through BrickScanReadBase; the claim
// reads totals through TotalsReadBase. Command-line latched.
static bool VoxelGpuWorklistClassifyEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistClassify="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The fused stage's byte gate (-VoxelGpuWorklistVerifyCT): every totals-fed
// chunk ALSO runs the classic classify + scans + totals into transients plus
// a 1-group compare of offsets + totals into stats [8..9], riding the proof
// readback. +9 passes per converted chunk on the verify arm (the eight
// classic passes back plus the compare) -- a verify arm, never production.
static bool VoxelGpuWorklistVerifyCtEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyCT="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistAssetStamp=1 (P3 stage 4), on top of
// -VoxelGpuWorklistVoxelize: Flush stages every consumed asset record's
// resolved instances into a per-flush blob and dispatches the
// ORDER-PRESERVING gather (one thread per column, instances walked in slice
// order -- the classic per-instance pass order, in-thread) between the
// voxelize and classify dispatches. This is what admits ASSET CHUNKS to the
// whole cell-arena chain: wlvox/wlct fbAssets stop growing and those chunks
// drop their VoxelizeMain, their per-instance stamp passes and (with the
// classify stage) the eight count-side passes. Command-line latched.
static bool VoxelGpuWorklistAssetStampEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistAssetStamp="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The stamp's byte gate (-VoxelGpuWorklistVerifyStamp): every converted ASSET
// chunk ALSO runs classic VoxelizeMain + the classic per-instance AssetStamp
// passes into the transient plus a 512-group compare of the STAMPED cells
// into stats [10..11], riding the proof readback. Verify arm only.
static bool VoxelGpuWorklistVerifyStampEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyStamp="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistPack=1 (P3 stage 5), on top of -VoxelGpuWorklistClassify:
// the flush graph ALSO dispatches the converted Pack (one indirect dispatch
// per tick, one group per brick, plus one small mask-arena clear), and every
// totals-fed chunk skips its classic BrickChunkMask clear and BrickPackMain
// -- TWO more passes (7 -> 5 on the lean-alloc shape). The claim/write
// passes source descriptors and words from the pack arenas through
// FRegionGraphResources' read bases; descriptor offsets stay CHUNK-RELATIVE
// (packBrickInto's split). Command-line latched.
static bool VoxelGpuWorklistPackEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistPack="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The Pack stage's byte gate (-VoxelGpuWorklistVerifyPack): every pack-fed
// chunk ALSO runs the classic mask clear + BrickPackMain into transients
// (reading the SAME arena cells and offsets) plus a 1-group compare of desc
// + bounded words + mask into stats [12..13]. Verify arm only.
static bool VoxelGpuWorklistVerifyPackEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyPack="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistClaim=1 (P3 stage 6), on top of -VoxelGpuWorklistPack:
// the flush graph ALSO dispatches the Claim stage -- the claim + both word
// copies + desc/record as THREE indirect dispatches per tick
// (VoxelWorklistClaim.usf) -- and every claim-fed chunk adds ZERO brick
// passes to the batch graph (5 -> 0 on the lean-alloc shape): production is
// fully inside the flush graph, delivery keeps the lean-alloc no-readback
// shape, and the pass term goes flat in N. Command-line latched.
static bool VoxelGpuWorklistClaimEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistClaim="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// The Claim stage's byte gate (-VoxelGpuWorklistVerifyClaim): one extra
// indirect dispatch per tick comparing the LANDED pool state (descriptors,
// bounded word ranges, the chunk record or its fail-zero) against the
// stage's own sources through the shared factored text, into stats [14..15].
// The classic claim cannot be re-run as a reference (a bump allocator is not
// idempotent), so this is the VerifyBrickStack shape the plan doc names.
static bool VoxelGpuWorklistVerifyClaimEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistVerifyClaim="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

// -VoxelGpuWorklistCellBudget=<n> (default 256): the per-flush record cap
// while the Voxelize stage is armed. The cell arena costs 128 KiB per record
// (32,768 cells x 4 B) -- 256 is a 32 MiB arena and 15,360 chunks/s of
// consume headroom at 60 ticks; the ring's default budget of 1,024 would be
// 128 MiB. Sustained pending>0 on the window line is the raise-me signal.
static uint32 VoxelGpuWorklistCellBudget()
{
	static const uint32 Value = []
	{
		int32 Parsed = 256;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuWorklistCellBudget="), Parsed);
		return uint32(FMath::Max(Parsed, 1));
	}();
	return Value;
}

namespace VoxelGpuLeanDetail
{
	// Render-thread atomics, VoxelGpuBatchDetail's reason verbatim: the counts
	// are taken inside DispatchBatch's render command, which must not capture
	// `this`. Window-read by MaybeLogLeanWindow on the game thread; a torn read
	// misplaces a job across two windows, never invents or loses one.
	static std::atomic<int64> GLeanJobs{ 0 };       // mesh region SKIPPED
	static std::atomic<int64> GFullQuadJobs{ 0 };   // kept: job still emits quads
	static std::atomic<int64> GFullBandJobs{ 0 };   // kept: job carries a band request
	static std::atomic<int64> GFullNoPackJobs{ 0 }; // kept: job packs no bricks

	// The armed-only window line, ~5 s cadence, game thread (called from Tick).
	// CUMULATIVE counts, deliberately -- a window that happened to be idle must
	// not print zeros that read as "the switch died"; growth between lines is
	// the signal. THE FAILING READING: lean stays 0 while band/quads/noPack
	// grow -- the switch is armed and declining everything, and the dominant
	// reason names which precondition is missing.
	inline void MaybeLogWindow()
	{
		static double LastLogSeconds = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (LastLogSeconds <= 0.0)
		{
			LastLogSeconds = Now;
			return;
		}
		if (Now - LastLogSeconds < 5.0)
		{
			return;
		}
		LastLogSeconds = Now;
		const int64 Lean = GLeanJobs.load(std::memory_order_relaxed);
		const int64 Quad = GFullQuadJobs.load(std::memory_order_relaxed);
		const int64 Band = GFullBandJobs.load(std::memory_order_relaxed);
		const int64 NoPack = GFullNoPackJobs.load(std::memory_order_relaxed);
		if (Lean + Quad + Band + NoPack == 0)
		{
			return; // armed and idle: no jobs dispatched yet, nothing to claim
		}
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-lean] mesh-region graphs skipped=%lld kept=%lld ")
		       TEXT("(kept because: quads %lld, band %lld, noPack %lld) (cumulative)"),
		       Lean, Quad + Band + NoPack, Quad, Band, NoPack);
	}
}

namespace VoxelGpuBatchDetail
{
	// Longest Z-run one stack may fuse. NOT a correctness bound -- validation
	// allows 1,024 chunks (the 65,536-brick single-workgroup scan ceiling) --
	// but a transient-memory and pass-duration one: the stack's Cells scratch
	// is 128 KiB per chunk (32x32x32 cells x 4 B), so 64 caps it at 8 MiB,
	// and a real column stack is 8-16 chunks anyway. Raising it is safe;
	// measure hitches when you do.
	constexpr int32 kMaxStackChunks = 64;

	// Pass-count constants for the window counters. DERIVED FROM THE CODE
	// SHAPE, NOT COUNTED AT THE AddPass SITES -- if AddRegionPasses grows or
	// loses a pass, update these with it (they are the only two places the
	// counter can lie).
	//
	// Stack: Column, Voxelize, mask clear, BrickClassify, 3+3 scans,
	// BrickPack, BrickTotal, BrickStackTotals = 13 compute + 1 readback copy.
	constexpr int32 kPassesPerStackDispatch = 14;
	// Classic brick-only job: its 48x48x6 mesh region still adds Column +
	// Voxelize (nothing reads them; RDG may cull, counted anyway because they
	// are recorded), then the halo-free brick region's 12, then the 2-dword
	// totals copy.
	constexpr int32 kPassesPerClassicBrickJob = 15;
	// Classic quad-mesh job (RetireQuads 0): 7 mesh-chain passes + quad total
	// + copies; nominal.
	constexpr int32 kPassesPerClassicQuadJob = 12;

	// P3 spine tally constants, same DERIVED-FROM-CODE-SHAPE rule as above.
	// These feed the [gpu-worklist] passes-per-tick line -- the number read
	// FIRST on any worklist leg, because the 50k gate is this going FLAT with
	// chunk rate, not throughput moving.
	//
	// Lean brick job under voxel.GPU.PoolAlloc: the halo-free brick region's
	// 12 + claim + 4 alloc writes, no readback.
	constexpr int32 kPassesPerLeanAllocJob = 17;
	// Lean brick job on the readback path: brick region 12 + 2-dword totals
	// copy.
	constexpr int32 kPassesPerLeanReadbackJob = 13;
	// Classic (non-lean) brick job under PoolAlloc: mesh region 2 + brick
	// region 12 + claim + 4 writes, totals copy gone.
	constexpr int32 kPassesPerClassicAllocJob = 19;
	// Claim-based fused stack: the 13 compute passes (no stack-totals
	// readback copy) + claim + 4 writes PER MEMBER.
	constexpr int32 kStackClaimBasePasses = 13;
	constexpr int32 kPassesPerStackMemberClaim = 5;
	// The spine itself: args-setup + prover, once per tick regardless of N.
	// This is the shape the converted chain inherits: ~14 passes per TICK
	// (one per stage plus args) = 840/s at 60 fps, 36x under the ~500/tick
	// hitch cliff, AT ANY chunk rate.
	constexpr int32 kWorklistSpinePassesPerTick = 2;

	// Crosscheck counters. File-scope atomics rather than manager members
	// because they are incremented on the RENDER thread inside poll commands
	// that deliberately do not capture `this` (the manager may be destroyed
	// with commands still in flight). One manager exists in practice; if a
	// second ever does, these become a shared total, which for a pass/fail
	// tally is still the right read.
	static std::atomic<int32> GCrosscheckPass{ 0 };
	static std::atomic<int32> GCrosscheckFail{ 0 };

	// P3 Column stage, render-side failure: a job carried an arena slice but
	// the batch graph found no arena to register (the armed flush never
	// created it). Every count here is a chunk that silently fell back to
	// classic ColumnMain FROM the converted path -- the window line prints
	// it, and any growth is a bug in the flush/batch ordering, not noise.
	static std::atomic<int64> GWorklistColArenaMissing{ 0 };
	// Same failure, Voxelize stage: a job flagged cell-fed reached the batch
	// graph and no cell arena existed to read. Every count is a chunk that
	// fell back to classic VoxelizeMain from the converted path -- printed on
	// the wlvox line, any growth is a flush/batch ordering bug.
	static std::atomic<int64> GWorklistVoxArenaMissing{ 0 };
	// Same failure, fused ClassifyTotals stage: a job flagged totals-fed
	// reached the batch graph and the scan/totals arenas did not exist.
	static std::atomic<int64> GWorklistCtArenaMissing{ 0 };
	// Same failure, Pack stage.
	static std::atomic<int64> GWorklistPackArenaMissing{ 0 };
}

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

// See the declaration for what this collapses and for the counters that make
// each half readable as dead. Latched, like every other gate in this file: the
// -ExecCmds startup window opens after the first chunks have already been
// submitted, so a cvar cannot gate a cold-start measurement.
bool VoxelGpuJobLeanEnabled()
{
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuJobLean="), Value);
		return Value != 0;
	}();
	return bEnabled;
}

namespace VoxelGpuJobCostDetail
{
	// The bytes a deep copy of this request's arrays costs, split the way the
	// window line reports them. Counted from the arrays themselves rather than
	// from a remembered constant, because the whole point of the number is that
	// nobody knows what it is per chunk on this path.
	inline int64 RasterCopyBytes(const FVoxelGpuRegionRequest& Req)
	{
		return int64(Req.ElevationMm.Num()) * int64(sizeof(int32))
		     + int64(Req.ClimatePacked.Num()) * int64(sizeof(uint32));
	}

	inline int64 AssetCopyBytes(const FVoxelGpuRegionRequest& Req)
	{
		return int64(Req.AssetInstances.Num())
		         * int64(sizeof(FVoxelGpuRegionRequest::FAssetInstance))
		     + int64(Req.AssetColStarts.Num()) * int64(sizeof(uint32))
		     + int64(Req.AssetSpans.Num()) * int64(sizeof(uint32));
	}
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

namespace
{
	// The one shape test both derivations make. Only the standard single-chunk
	// footprint: anything else and the one-brick shift is arithmetic about a
	// shape that is not there -- see MakeBrickRegion's declaration for what
	// packing the wrong bricks looks like.
	inline bool IsStandardChunkFootprint(const FVoxelGpuRegionRequest& MeshReq)
	{
		return MeshReq.DispatchColumns.X == VoxelGpuChunkRegion::kColumns
		    && MeshReq.DispatchColumns.Y == VoxelGpuChunkRegion::kColumns
		    && MeshReq.BricksZ == VoxelGpuChunkRegion::kBricksZ;
	}

	// Everything the halo-free derivation does AFTER the struct has been
	// copied (or moved) across. Reads each field before it writes it, so it is
	// correct in place -- which is what lets the moving overload share it.
	void ApplyBrickRegionShape(FVoxelGpuRegionRequest& OutReq);
}

bool VoxelGpuChunkRegion::MakeBrickRegion(const FVoxelGpuRegionRequest& MeshReq,
                                          FVoxelGpuRegionRequest& OutReq)
{
	if (!IsStandardChunkFootprint(MeshReq))
	{
		return false;
	}

	OutReq = MeshReq;
	ApplyBrickRegionShape(OutReq);
	return true;
}

// See the declaration for the byte arithmetic, the lean gate this mirrors, and
// why each thing MeshReq loses is safe.
bool VoxelGpuChunkRegion::MakeBrickRegionMoveAssets(FVoxelGpuRegionRequest& MeshReq,
                                                    FVoxelGpuRegionRequest& OutReq)
{
	// The refusal happens BEFORE anything is taken, so a refused derivation
	// leaves MeshReq exactly as it found it -- same contract as the copying
	// overload, which matters because Submit falls back to that one.
	if (!IsStandardChunkFootprint(MeshReq))
	{
		return false;
	}

	// Taken FIRST, into locals, so the struct assignment below sees three empty
	// arrays and allocates nothing for them. Doing it the other way round --
	// copy, then overwrite with moves -- would pay the whole copy and then
	// throw it away, which is the bug this function exists to remove.
	TArray<FVoxelGpuRegionRequest::FAssetInstance> Instances = MoveTemp(MeshReq.AssetInstances);
	TArray<uint32> ColStarts = MoveTemp(MeshReq.AssetColStarts);
	TArray<uint32> Spans = MoveTemp(MeshReq.AssetSpans);

	// The raster arrays are still COPIED. They are empty under
	// -VoxelGpuRasterAtlas (ValidateRegionRequest refuses a request that is
	// both atlas-armed and window-filled), so this costs nothing there; on the
	// inline-window control path they are real, and moving them would make
	// ValidateRegionRequest(Job->Region) fail in Tick's promote loop.
	OutReq = MeshReq;

	OutReq.AssetInstances = MoveTemp(Instances);
	OutReq.AssetColStarts = MoveTemp(ColStarts);
	OutReq.AssetSpans = MoveTemp(Spans);

	ApplyBrickRegionShape(OutReq);
	return true;
}

namespace
{
void ApplyBrickRegionShape(FVoxelGpuRegionRequest& OutReq)
{
	using namespace VoxelGpuChunkRegion;

	// Drop the halo: one brick on every axis, on the negative side, and the
	// interior extent on the positive.
	OutReq.DispatchColumns = FUintVector2(kChunkEdgeVoxels, kChunkEdgeVoxels);
	OutReq.OriginVx += int32(kBrickEdge);
	OutReq.OriginVy += int32(kBrickEdge);
	OutReq.BrickZMin += 1;
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
	// Ceiling 6, mirroring FillLooseParameters' CoarseScale clamp: the stale
	// literal 5 halved the shift at level 6, displacing every asset anchor in
	// a ring-6 brick region by 8x32 level-0 voxels -- plausible trees, wrong
	// places, no error.
	const int32 AnchorShift = int32(kBrickEdge) << FMath::Clamp(OutReq.CoarseLevel, 0, 6);
	for (FVoxelGpuRegionRequest::FAssetInstance& Inst : OutReq.AssetInstances)
	{
		Inst.AnchorRelVx -= AnchorShift;
		Inst.AnchorRelVy -= AnchorShift;
	}
}
} // namespace

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

// One batched column stack (Tier B.1, voxel.GPU.WorldGenBatch): the shared
// half of K Z-sibling jobs that were fused into a single tall region dispatch.
//
// OWNERSHIP. Built on the game thread during Tick's grouping, then read-only
// there; the render thread creates the readback in DispatchBatch and is the
// only thread that ever touches Totals / bHarvested / bFailed (poll commands
// run serially on the render thread, so plain fields suffice -- the same
// single-writer argument FJob's staging arrays rely on). Members publish their
// per-chunk numbers into their OWN FJob fields before the release-store that
// moves their state, which is the fence Deliver()'s game-thread reads pair
// with.
//
// EXACTLY-ONCE IS UNTOUCHED: the stack is shared STATE, not a shared OUTCOME.
// Every member job still delivers its own result exactly once through the
// same states as before; the stack only replaces K private 2-dword readbacks
// with one (2+2K)-dword readback they all harvest from.
struct FVoxelGpuBrickStack
{
	// The fused region: bottom member's brick region with BricksZ = 4K and
	// bPerChunkBrickTotals set. Validated at grouping time, before any member
	// is pointed at this object.
	FVoxelGpuRegionRequest Region;
	int32 NumChunks = 0;

	// STACK-CLAIM (-VoxelGpuStackClaim, 2026-08-23): true when this stack's
	// members land through the GPU allocator -- per-member claim passes read
	// the per-chunk totals IN THE GRAPH and no TotalsReadback exists. Set on
	// the game thread at assembly, before any member is pointed at this
	// object; read on both threads afterwards, immutable. The poll's stack
	// phase-1 branch is BYPASSED for such members (there is nothing to
	// harvest); they take the ordinary no-readback road to TotalDone.
	bool bClaimBased = false;

	// Created and used on the render thread; released there too (see ~).
	TUniquePtr<FRHIGPUBufferReadback> TotalsReadback;

	// RENDER THREAD ONLY. [0]=region occ, [1]=region mat, then per chunk c:
	// [2+2c]=occ, [3+2c]=mat -- BrickStackTotalsMain's layout, verbatim.
	TArray<uint32> Totals;
	bool bHarvested = false;
	// Set on harvest failure OR on the sum-vs-region cross-check failing; every
	// member that polls afterwards fails with Error instead of publishing a
	// volume built from a split the GPU disagrees with.
	bool bFailed = false;
	FString Error;

	~FVoxelGpuBrickStack()
	{
		// The readback wraps RHI staging memory; free it in render-thread
		// order behind any command still touching it -- FVoxelGpuBrickPayload's
		// destructor argument, verbatim. Raw-pointer capture because
		// TUniquePtr is move-only and the command only needs to delete.
		if (FRHIGPUBufferReadback* Readback = TotalsReadback.Release())
		{
			ENQUEUE_RENDER_COMMAND(VoxelGpuBrickStackRelease)(
				[Readback](FRHICommandListImmediate&)
			{
				delete Readback;
			});
		}
	}
};

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

	// --- P1 (voxel.GPU.PoolAlloc): this job claims its own pool ranges -------
	//
	// Latched in DispatchBatch, NOT in Submit, because stack membership (which
	// keeps the readback path) is only decided at promote time. When true, the
	// dispatch adds claim + write passes to the SAME graph as generation, no
	// BrickTotalReadback is enqueued, no BrickPayload is built, and Deliver
	// publishes nothing -- the chunk became resident on the GPU's own timeline.
	// A brick-only job with no band then has NO readback at all: it is
	// deliverable the moment its graph is enqueued, which is the death of the
	// per-chunk fence this phase exists for.
	bool bGpuPoolAlloc = false;
	// Whether a resident SHELL (descriptor block + record slot) is held for
	// this job. Separate from bGpuPoolAlloc so Deliver can release the shell of
	// a job whose brick half was dropped after the shell was taken.
	bool bGpuShellAllocated = false;
	uint32 GpuChunkSlot = 0;
	uint32 GpuBrickBase = 0;
	// P3 Column stage: this job's slice in the worklist column arena for THIS
	// tick's consume window, or MAX_uint32 when its columns were not computed
	// by the indirect dispatch (stage off, record refused/deferred, atlas
	// missing). Written on the game thread in DispatchBatch after the flush,
	// before the batch render command is enqueued; read only inside it.
	uint32 WorklistColumnSlice = MAX_uint32;
	// P3 Voxelize stage: this job's CELLS were also computed by the indirect
	// dispatch (into the cell arena at WorklistColumnSlice * 32768). Only
	// ever true with a valid WorklistColumnSlice, and only for asset-free
	// jobs -- the AssetStamp exclusion lives at the set site.
	bool bWorklistCellsFed = false;
	// P3 fused ClassifyTotals: this job's brick offsets/totals were also
	// computed (arenas at slice * 64 / slice * 2). Implies bWorklistCellsFed.
	bool bWorklistTotalsFed = false;
	// P3 Pack: this job's descriptors and word payloads were also packed
	// (pack arenas). Implies bWorklistTotalsFed.
	bool bWorklistPackFed = false;
	// P3 Claim (stage 6): this job's pool claim AND all four writes ran in
	// the flush graph -- the batch graph must add NO brick passes for it (a
	// classic claim here would double-claim the slot and leak the ranges).
	// Implies bWorklistPackFed; mirrors the GPU's worklistClaimEligible.
	bool bWorklistClaimFed = false;
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

	// Tier B.1: non-null iff this job was fused into a batched column stack.
	// StackChunkIndex is the job's z-slot inside it (0 = bottom), which is
	// simultaneously decodeBrick's chunkIndex, the L1-mask index, and the
	// descriptor slice [64*idx, 64*idx+64) -- one number, three contracts,
	// and BrickStackTotalsMain's layout is keyed on the same one.
	TSharedPtr<FVoxelGpuBrickStack, ESPMode::ThreadSafe> BrickStack;
	int32 StackChunkIndex = INDEX_NONE;

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
	// The manager's monotonic tick number at promotion. Delivery subtracts it
	// to get this job's RESIDENCY IN TICKS, which is the denominator of the
	// throughput ceiling (MaxInFlight / residency x tick rate). Ticks rather
	// than seconds on purpose: the ceiling is a slots-per-tick argument, and a
	// millisecond residency divided by a variable frame time is a different and
	// much less useful number.
	int64 PromotedTickSeq = 0;

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
				// Tier B.1: drop this job's share of its stack (if any) here on
				// the render thread. The last share freed destroys the stack,
				// whose destructor hands the readback to a render command --
				// so the thread the LAST holder happens to die on never
				// matters, same discipline as the payloads.
				Job->BrickStack.Reset();
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

	// --- Tier B.1 grouping predicates --------------------------------------

	// A job the batched path can take at all: brick-only, band-less,
	// asset-less. Each exclusion is a counted fallback, not a rejection --
	// the job dispatches through the classic per-chunk path exactly as it
	// would with the switch off.
	//
	//   quads   -- a quad-mesh job needs its 48x48x6 halo region per chunk
	//              anyway, so fusing only its brick half would complicate two
	//              paths to amortise neither.
	//   band    -- one job per (X,Y) footprint carries the band request, and
	//              the band rides the mesh region. That job goes classic; its
	//              K-1 siblings still stack, so a column loses one member,
	//              not the batch.
	//   assets  -- instances index per-REQUEST span tables (ColStartsBase);
	//              merging tables across requests is a rebase this build does
	//              not attempt. Mostly surface chunks; the counter says how
	//              much it costs.
	bool IsStackableBrickJob(const FVoxelGpuMeshJobManager::FJob& Job)
	{
		return Job.bBrickPack
			&& !Job.bQuadMesh
			&& Job.Region.BandEdge == 0
			&& Job.BrickRegion.AssetInstances.Num() == 0;
	}

	// Same vertical column at the same ring level -- the necessary condition.
	bool SameBrickColumn(const FVoxelGpuMeshJobManager::FJob& A,
	                     const FVoxelGpuMeshJobManager::FJob& B)
	{
		return A.BrickKey.X == B.BrickKey.X
			&& A.BrickKey.Y == B.BrickKey.Y
			&& A.BrickKey.Level == B.BrickKey.Level;
	}

	// The sufficient condition: the sibling would have dispatched over the
	// SAME inputs the stack will use. The raster arrays are memcmp'd, not
	// trusted: sibling windows are filled by the same caller from the same
	// tiles and SHOULD be identical, but a window filled while a fine tile was
	// still decoding comes from the coarse fallback (the residency-gating
	// lesson on FootprintChunkZRangeCached), and a stack built on the head's
	// window would then hand that sibling terrain generated from data it was
	// never given. Bit-exactness is the promise; the memcmp is what makes it
	// a property instead of a hope. Cost: the windows are tens of pixels.
	bool CompatibleForStack(const FVoxelGpuMeshJobManager::FJob& Head,
	                        const FVoxelGpuMeshJobManager::FJob& Cand)
	{
		const FVoxelGpuRegionRequest& H = Head.BrickRegion;
		const FVoxelGpuRegionRequest& C = Cand.BrickRegion;
		if (H.Seed != C.Seed || H.CoarseLevel != C.CoarseLevel ||
		    H.PixelSizeMm != C.PixelSizeMm ||
		    H.RasterOriginPx != C.RasterOriginPx || H.RasterSize != C.RasterSize ||
		    H.ElevationMm.Num() != C.ElevationMm.Num() ||
		    H.ClimatePacked.Num() != C.ClimatePacked.Num())
		{
			return false;
		}
		if (H.ElevationMm.Num() > 0 &&
		    FMemory::Memcmp(H.ElevationMm.GetData(), C.ElevationMm.GetData(),
		                    H.ElevationMm.Num() * sizeof(int32)) != 0)
		{
			return false;
		}
		if (H.ClimatePacked.Num() > 0 &&
		    FMemory::Memcmp(H.ClimatePacked.GetData(), C.ClimatePacked.GetData(),
		                    H.ClimatePacked.Num() * sizeof(uint32)) != 0)
		{
			return false;
		}
		return true;
	}

	// --- Tier B.1: the fused dispatch --------------------------------------
	//
	// ONE AddRegionPasses call for the whole stack -- the same function, the
	// same kernels, the same event names the per-chunk path records, over a
	// region that is simply K chunks tall. Members' 48x48x6 mesh regions are
	// NOT dispatched: a stackable job is brick-only and band-less, so that
	// region's outputs feed nothing (no readback references them; RDG culls
	// unreferenced passes, and skipping the recording removes even the setup).
	// Either way nothing observable changes, which is what lets the control
	// leg stay byte-identical.
	// BindPoolAlloc / PoolAllocBufs / PoolAllocLayout: DispatchBatch's lazy
	// allocator binding, reached through here so a CLAIM-BASED stack (see
	// FVoxelGpuBrickStack::bClaimBased) can add its members' claim+write passes
	// into this same graph. A readback-based stack never touches them.
	void AddBrickStackPasses(FRDGBuilder& GraphBuilder, const TArray<FJobPtr>& Members,
	                         TArray<FJobPtr>& Built,
	                         TFunctionRef<bool()> BindPoolAlloc,
	                         const VoxelGpuWorldGen::FBrickPoolAllocBuffers& PoolAllocBufs,
	                         const FVoxelBrickPoolAllocLayout& PoolAllocLayout)
	{
		if (Members.Num() == 0 || !Members[0]->BrickStack.IsValid())
		{
			return;
		}
		const TSharedPtr<FVoxelGpuBrickStack, ESPMode::ThreadSafe> Stack = Members[0]->BrickStack;

		bool bAnyLive = false;
		for (const FJobPtr& Member : Members)
		{
			bAnyLive |= Member->Abandoned.load(std::memory_order_acquire) == 0;
		}
		if (!bAnyLive)
		{
			// Every member was cancelled between promote and this command;
			// they have already been delivered. Nothing to generate for.
			return;
		}

		// A scope, for the BrickRegion scope's reason: the stack runs the same
		// event names as everything else, and a ProfileGPU capture must be able
		// to attribute the fused cost to the fusion.
		RDG_EVENT_SCOPE(GraphBuilder, "Voxel.BrickStack(%d chunks)", Stack->NumChunks);

		const VoxelGpuWorldGen::FRegionGraphResources Graph =
			VoxelGpuWorldGen::AddRegionPasses(GraphBuilder, Stack->Region);

		if (Graph.BrickDesc == nullptr || Graph.BrickOcc == nullptr ||
		    Graph.BrickMat == nullptr || Graph.BrickChunkMask == nullptr ||
		    Graph.BrickStackTotals == nullptr)
		{
			// Unlike the classic path, where a brick failure leaves a mesh half
			// to deliver, a stack member is brick-only: no volume means the job
			// produced nothing, and saying so is what routes the streaming path
			// to its CPU fallback instead of stranding the column.
			for (const FJobPtr& Member : Members)
			{
				Member->Error = TEXT("batched stack region produced no brick buffers");
				Member->SetState(EJobState::Failed);
			}
			return;
		}

		// --- STACK-CLAIM: land the members through the GPU allocator, HERE -----
		//
		// Same graph, no payloads, no readback. Each live shelled member claims
		// its ranges off its own totals pair (TotalsChunkIndexPlusOne = c+1;
		// the kernel derives the shared-scratch prefix the CPU harvest used to
		// compute) and the write passes copy its slice straight into the
		// arenas. The scratch never becomes an external buffer -- it dies with
		// this graph, exactly like a classic P1 job's. A member without a
		// shell (ShellRefused, already counted) adds no passes and delivers
		// "produced nothing", the classic path's own semantics for that state.
		if (Stack->bClaimBased)
		{
			const uint32 BricksPerChunk = Graph.Sizes.NumBricks / uint32(Stack->NumChunks);
			const bool bBound = BindPoolAlloc();
			if (!bBound)
			{
				// Loud and fatal for the stack, not silent: without the
				// allocator buffers no member can land, and "armed but landed
				// nothing" must never read as an empty world with healthy
				// counters -- the fourth silent no-op this project refuses to
				// grow.
				UE_LOG(LogTemp, Error,
				       TEXT("[gpu-batch] claim-based stack of %d chunks: allocator buffers ")
				       TEXT("unavailable; every member fails loudly."), Stack->NumChunks);
				for (const FJobPtr& Member : Members)
				{
					Member->Error = TEXT("stack-claim: allocator buffers unavailable");
					Member->SetState(EJobState::Failed);
				}
				return;
			}
			for (const FJobPtr& Member : Members)
			{
				if (Member->Abandoned.load(std::memory_order_acquire) != 0)
				{
					continue;
				}
				if (Member->bGpuPoolAlloc)
				{
					const uint32 C = uint32(Member->StackChunkIndex);
					FRDGBufferRef Claim = VoxelGpuWorldGen::AddBrickPoolClaimPass(
						GraphBuilder, PoolAllocBufs, PoolAllocLayout,
						Graph.BrickStackTotals, Member->GpuChunkSlot,
						BricksPerChunk * 16u, BricksPerChunk * 132u,
						/*TotalsChunkIndexPlusOne*/ C + 1u,
						/*TotalsNumChunks (arms the split gate)*/ uint32(Stack->NumChunks));
					VoxelGpuWorldGen::AddBrickPoolAllocWritePasses(
						GraphBuilder, PoolAllocBufs, Claim,
						Graph.BrickOcc, Graph.BrickMat,
						Graph.BrickDesc, Graph.BrickChunkMask,
						BricksPerChunk, Member->GpuChunkSlot, Member->GpuBrickBase,
						uint32(FMath::Clamp(Member->BrickKey.Level, 0, 15)),
						Member->BrickOriginVoxel, Member->BrickShading,
						BricksPerChunk * 16u, BricksPerChunk * 132u,
						/*SrcDescBase*/ C * BricksPerChunk, /*ChunkMaskBase*/ C * 2u);
				}
				// Built either way: a shell-less member's outcome is "Success,
				// no volume", delivered through the ordinary no-readback road.
				Built.Add(Member);
			}
			// NO TotalsReadback -- that is the point. The poll's stack branch
			// is bypassed by bClaimBased and every member walks the classic
			// no-readback phase 1 to TotalDone.
			return;
		}

		// ONE set of external buffers, referenced by every member's payload.
		// Each payload holds its own TRefCountPtr, so the scratch lives until
		// the LAST member's pool write has consumed its slice, and dying
		// members simply drop their share -- no per-stack lifetime bookkeeping.
		const TRefCountPtr<FRDGPooledBuffer> Desc = GraphBuilder.ConvertToExternalBuffer(Graph.BrickDesc);
		const TRefCountPtr<FRDGPooledBuffer> Occ = GraphBuilder.ConvertToExternalBuffer(Graph.BrickOcc);
		const TRefCountPtr<FRDGPooledBuffer> Mat = GraphBuilder.ConvertToExternalBuffer(Graph.BrickMat);
		const TRefCountPtr<FRDGPooledBuffer> Mask = GraphBuilder.ConvertToExternalBuffer(Graph.BrickChunkMask);
		const uint32 BricksPerChunk = Graph.Sizes.NumBricks / uint32(Stack->NumChunks);

		for (const FJobPtr& Member : Members)
		{
			if (Member->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}
			Member->BrickPayload = MakeShared<FVoxelGpuBrickPayload, ESPMode::ThreadSafe>();
			Member->BrickPayload->Desc = Desc;
			Member->BrickPayload->Occ = Occ;
			Member->BrickPayload->Mat = Mat;
			Member->BrickPayload->ChunkMask = Mask;
			// The three faces of StackChunkIndex -- descriptor slice, L1-mask
			// slot, totals slot -- are all keyed on decodeBrick's chunk-major
			// order, which for a 1x1xK region is ascending z from the region
			// floor. Grouping sorted members by BrickKey.Z, so index i IS
			// chunk i of the dispatch.
			Member->BrickPayload->SrcBrickFirst = uint32(Member->StackChunkIndex) * BricksPerChunk;
			Member->BrickPayload->SrcChunkIndex = uint32(Member->StackChunkIndex);
			Member->BrickPayload->BrickCount = BricksPerChunk;
			Member->BrickPayload->OriginVoxel = Member->BrickOriginVoxel;
			// SrcOccFirst/SrcMatFirst stay 0 until the totals land -- they are
			// prefix sums of the per-chunk totals, filled at harvest.
			Built.Add(Member);
		}

		// The one readback of the whole stack: (2 + 2K) dwords of SIZES. The
		// geometry itself never comes back -- the same no-readback property
		// the per-chunk brick path has, one buffer wider.
		Stack->TotalsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.BrickStackTotals"));
		AddEnqueueCopyPass(GraphBuilder, Stack->TotalsReadback.Get(), Graph.BrickStackTotals,
		                   (2u + 2u * uint32(Stack->NumChunks)) * sizeof(uint32));
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

	// The per-CHUNK half of this manager's game-thread cost, bracketed
	// CONTIGUOUSLY so the three buckets telescope to the total and any drift is
	// a broken bracket rather than an unattributed remainder. See FJobCostWindow
	// for why a denominator-free ms figure is what produced the 0.149 ms/chunk
	// reading in the first place.
	const double SubT0 = FPlatformTime::Seconds();

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
	const double SubT1 = FPlatformTime::Seconds(); // hdr: alloc + region move + latches

	// -VoxelGpuJobLean: may this job's asset span tables MOVE into the brick
	// region instead of being deep-copied into it?
	//
	// THE GATE MIRRORS DispatchBatch'S LEAN GATE, TERM FOR TERM, plus the atlas.
	// Anything looser and a job whose mesh region still gets dispatched would
	// find its arrays gone; anything tighter and the collapse is dead on the
	// only configuration that matters. The one term that can still change AFTER
	// this point is bBrickPack (DispatchBatch clears it when the pool refuses a
	// shell), and that case is covered in the declaration: it dispatches a mesh
	// graph with no readback attached and discards the result either way.
	const bool bMoveAssets =
		VoxelGpuJobLeanEnabled()
		&& VoxelGpuLeanBrickJobsEnabled()
		&& !Job->bQuadMesh
		&& Job->Region.BandEdge == 0
		&& Job->Region.bRasterAtlas;
	// Counted BEFORE the derivation, off the arrays as they stand, because
	// afterwards the moving path has emptied the originals and the number would
	// read zero -- which is exactly the shape of a saving that is really a
	// broken measurement.
	const int64 RasterBytes = VoxelGpuJobCostDetail::RasterCopyBytes(Job->Region);
	const int64 AssetBytes = VoxelGpuJobCostDetail::AssetCopyBytes(Job->Region);

	FString BrickRegionError;
	// VoxelGpuBrickPackEnabled(), NOT the raw cvar. THE GATE IS NO LONGER "A
	// CVAR" -- it is a cvar with a command-line override, and only the accessor
	// knows that. Reading GVoxelGpuBrickPack directly here is what made
	// -VoxelBrickPack=1 produce a job with bMeshChain false AND bBrickPack false,
	// which the promotion guard then correctly rejected as producing nothing --
	// the fork carried zero traffic and `added (gpu 0, cpu 28123)` was the only
	// sign. The invariant moved when the override landed; every read has to move
	// with it.
	// Hoisted out of the condition below so the "did the collapse actually run"
	// counter is the truth rather than the intent: the gate can be armed and
	// the derivation still refuse (a non-chunk footprint), and a counter that
	// reported a saving in that case would be the eleventh feature in this
	// project to read healthy while doing nothing. Short-circuit order is
	// preserved exactly -- no derivation without BrickPack, no validation
	// without a derivation.
	bool bBrickRegionDerived = false;
	bool bAssetsMoved = false;
	if (VoxelGpuBrickPackEnabled())
	{
		if (bMoveAssets)
		{
			bBrickRegionDerived =
				VoxelGpuChunkRegion::MakeBrickRegionMoveAssets(Job->Region, Job->BrickRegion);
			bAssetsMoved = bBrickRegionDerived;
		}
		else
		{
			bBrickRegionDerived =
				VoxelGpuChunkRegion::MakeBrickRegion(Job->Region, Job->BrickRegion);
		}
	}

	if (bBrickRegionDerived &&
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
		// Ceiling 6 (was a stale literal 5 from the six-level world): a level-6
		// brick region keyed as level 5 collides with the true level-5 chunk at
		// the same coordinates -- one overwrites the other in the pool and the
		// march index, a wrong world with no error anywhere.
		Job->BrickKey.Level = FMath::Clamp(Job->BrickRegion.CoarseLevel, 0, 6);
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
	const double SubT2 = Job->SubmitSeconds; // brick: derive + validate + key

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

	{
		const double SubT3 = FPlatformTime::Seconds();
		++JobCost.Submits;
		JobCost.SubmitHdrMs += (SubT1 - SubT0) * 1000.0;
		JobCost.SubmitBrickMs += (SubT2 - SubT1) * 1000.0;
		JobCost.SubmitQueueMs += (SubT3 - SubT2) * 1000.0;
		JobCost.SubmitTotalMs += (SubT3 - SubT0) * 1000.0;
		JobCost.BrickCopyRasterBytes += RasterBytes;
		JobCost.BrickCopyAssetBytes += AssetBytes;
		if (bAssetsMoved)
		{
			++JobCost.LeanAssetMoves;
			JobCost.LeanAssetBytesSaved += AssetBytes;
		}
		else if (bBrickRegionDerived)
		{
			// A derivation that ran and COPIED. Jobs that derived nothing at
			// all (BrickPack off, non-chunk footprint) are in neither column
			// on purpose: they never paid the copy, so counting them as
			// "declined to move" would make the switch look inert on a leg
			// where there was nothing to move.
			++JobCost.LeanAssetCopies;
		}
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
	// S0-3. All computed from timestamps that are stamped unconditionally now
	// (PromotedSeconds went unconditional with the timeout-clock fix;
	// DispatchSeconds/ReadySeconds always were). The >0.0 guards are for jobs
	// that never REACHED a stage -- Rejected never dispatched, TimedOut never
	// went ready -- whose stage fields stay 0.0 meaning "did not happen", and
	// bLatencyStagesComplete below is what tells a consumer the difference
	// between that and a measured near-zero.
	if (Job->PromotedSeconds > 0.0 && Job->SubmitSeconds > 0.0)
	{
		Result.QueuedMs = (Job->PromotedSeconds - Job->SubmitSeconds) * 1000.0;
	}
	// The cross-thread stage the original three-way split was missing: promoted
	// on the game thread -> GraphBuilder.Execute() returned on the render
	// thread. Without it queued + dispatchToReady + readyToDeliver fell short
	// of submitToDeliver by exactly this much, and the shortfall was invisible.
	if (Job->DispatchSeconds > 0.0 && Job->PromotedSeconds > 0.0)
	{
		Result.PromoteToDispatchMs = (Job->DispatchSeconds - Job->PromotedSeconds) * 1000.0;
	}
	if (Job->ReadySeconds > 0.0)
	{
		Result.ReadyToDeliverMs = (DeliverSeconds - Job->ReadySeconds) * 1000.0;
	}
	// The four stages telescope to the total exactly when every stamp exists --
	// see the field's declaration for why partial jobs must not be summed.
	Result.bLatencyStagesComplete =
		Job->SubmitSeconds > 0.0 && Job->PromotedSeconds > 0.0 &&
		Job->DispatchSeconds > 0.0 && Job->ReadySeconds > 0.0;

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

	// --- P1: give the shell back when the volume will never land -------------
	//
	// A GPU-claimed job that failed (rejected after the shell was somehow
	// taken, dispatch failed, timed out, cancelled) -- or that dropped its
	// brick half on the render thread (null brick buffers, allocator buffers
	// unavailable) -- holds a resident shell whose record will never be
	// written. Left alone it is a permanent ghost: a chunk the index reports
	// and the marcher reads as empty forever. The slot guard inside
	// ReleaseGpuChunkShell makes this a no-op if a re-add already replaced the
	// entry. The one ghost this does NOT catch is a claim that failed ON THE
	// GPU (arena exhausted) in an otherwise successful job -- the CPU cannot
	// see that without a readback, which is the whole design; it shows up
	// instead as claimFail plus `unwritten` on the [brick-gpualloc] line.
	if (Job->bGpuShellAllocated &&
	    (Status != EVoxelGpuMeshJobStatus::Success || !Job->bBrickPack))
	{
		GetGlobalVoxelBrickPool().ReleaseGpuChunkShell(Job->BrickKey, Job->GpuChunkSlot);
		Job->bGpuShellAllocated = false;
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
	// The per-TICK denominator FTickStageMs has never had. Counted before any
	// early exit can exist, so ticks= is every call and not just the busy ones
	// -- a per-frame cost divided by busy frames is how a per-tick term
	// disguises itself as a per-chunk one.
	++JobCost.Ticks;
	++TickSeq;
	// Sampled at tick START, before any promotion, so the depth is what the
	// promote loop was actually facing rather than what it left behind.
	JobCost.QueueDemandSum += int64(Queued.Num());
	JobCost.QueueLowSum += int64(QueuedLowPriority.Num());
	JobCost.InFlightSum += int64(InFlight.Num());
	JobCost.InFlightMax = FMath::Max(JobCost.InFlightMax, InFlight.Num());

	// Tier B.1. Latched once per tick so one tick behaves like one tick; safe
	// to flip mid-run because both paths emit identical bytes (the residue of
	// a flip is in the stats, never the world).
	//
	// P1: STACKING IS OFF WHILE voxel.GPU.PoolAlloc IS ARMED. A stack member's
	// bricks land through the readback path (per-chunk totals split), which the
	// armed pool refuses -- and teaching the claim kernels the stack shape is
	// scope B.1 has not earned: it is measured throughput-neutral (3.4x fewer
	// passes, zero throughput moved), so the combination costs nothing tonight
	// and the interplay is a counted fallback rather than a silent one.
	bool bBatchArmed = VoxelGpuWorldGenBatchEnabled();
	// STACK-CLAIM lifts the conflict: with -VoxelGpuStackClaim=1 a stack's
	// members claim their own ranges in the stack's graph (see the accessor),
	// so the totals readback the armed pool refuses is not needed. Without it,
	// the disable stands exactly as before.
	const bool bStackClaim = VoxelGpuStackClaimEnabled() && VoxelGpuPoolAllocEnabled();
	if (bBatchArmed && VoxelGpuPoolAllocEnabled() && !bStackClaim)
	{
		if (!bPoolAllocStackConflictLogged)
		{
			bPoolAllocStackConflictLogged = true;
			UE_LOG(LogVoxelGpuMeshJob, Warning,
			       TEXT("[gpu-batch] voxel.GPU.WorldGenBatch is armed but voxel.GPU.PoolAlloc is too; ")
			       TEXT("stacking is DISABLED for this run (stack members would need the totals ")
			       TEXT("readback the armed pool refuses). Arm -VoxelGpuStackClaim=1 to fuse ")
			       TEXT("stacks through the GPU allocator instead."));
		}
		bBatchArmed = false;
	}
	if (bBatchArmed && !bBatchArmingLogged)
	{
		bBatchArmingLogged = true;
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-batch] voxel.GPU.WorldGenBatch ARMED: contiguous Z-sibling brick-only ")
		       TEXT("jobs fuse into one tall region per column stack (cap %d chunks/stack). ")
		       TEXT("Window counters follow every ~5 s; zero stacks with nonzero fallbacks means ")
		       TEXT("the feature is running and DECLINING, with the reasons in the line."),
		       VoxelGpuBatchDetail::kMaxStackChunks);
	}

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
	// P3 prep: the command-line latch outranks the cvar (see the accessor for
	// the queued=93% measurement that made these quotas the thing to sweep).
	const int32 BatchCapRaw = VoxelGpuMeshBatchCapEffective();
	const int32 BatchCap = BatchCapRaw > 0 ? BatchCapRaw : MAX_int32;

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
	const int32 SpecBatchCap = FMath::Max(0, VoxelGpuMeshSpecCapEffective());

	TArray<FJobPtr> Batch;
	TArray<TPair<FJobPtr, FString>> Rejected;
	// Tier B.1: per promoted stackable head, the head plus every Z-sibling
	// swept out of the same queue. Turned into fused stacks after the loop.
	TArray<TArray<FJobPtr>> StackSweeps;
	// --- -VoxelGpuJobLean, half four: BatchCap IS THE 50k WALL ---------------
	//
	// THE ARITHMETIC, AND IT IS NOT CLOSE. BatchCap defaults to 64 under
	// -VoxelGpuPrimary, so the fork can promote at most 64 chunks per tick:
	// 64 x 60 fps = 3,840 chunks/s, against a target of 50,000/s, which needs
	// 834 promotions per tick. One constant, a 13x shortfall. The measured GPU
	// fork (~2,078/s) sits at 54% of it, which is exactly the regime in which a
	// cap does not LOOK like the binding constraint and is about to become one.
	//
	// WHY THE CAP EXISTS, AND WHY THAT REASON HAS BEEN REMOVED FOR SOME JOBS.
	// BatchCap's own comment is explicit: it is a per-tick BURST limiter
	// protecting RENDER-THREAD PASS SETUP, not a capacity limit -- "~7 compute
	// passes + 3-4 copy passes each", and the sweep behind it measured 32/64
	// giving 367 hitches against 4/8's 8. Every one of those hitches was pass
	// setup that scaled with the number of promoted chunks.
	//
	// Worklist stage 6 (-VoxelGpuWorklistClaim) took a claim-fed chunk's brick
	// passes to ZERO: "a claim-fed job adds ZERO brick passes to the batch
	// graph ... production is fully inside the flush graph". For such a chunk
	// the entire justification for charging it a slice of BatchCap is gone --
	// and this file already accepts that argument once, for Tier B.1 stack
	// siblings: "a sibling rides its head's allowance instead of consuming one
	// -- the per-chunk reason for the cap is exactly what the fusion removes".
	// Same argument, same conclusion, a different mechanism.
	//
	// SO PASS-FREE JOBS GET THEIR OWN ALLOWANCE, and it is bounded by the
	// resource that actually bounds them: the worklist's per-flush consume
	// budget. Promoting more pass-free chunks in a tick than the flush can
	// consume does not make them pass-free -- the surplus falls back to the
	// classic chain and adds ~15 passes each, which is precisely the hitch
	// BatchCap exists to prevent. The Write triple's 65,535-group ceiling
	// (442 records) caps it in turn, and that ceiling is itself a 26,520/s wall
	// on the claim stage -- a finding for the worklist lane, not this one.
	//
	// THE REFUTATION IS BUILT IN. passFreeOverCap counts only promotions that
	// happened while DemandPromoted was ALREADY at BatchCap -- i.e. promotions
	// the shipped gate would have blocked. If it reads 0 with the switch armed,
	// this bought nothing: either nothing is claim-fed, or the cap was never
	// the binding constraint on this leg. Both are findings and neither is a
	// green light. Off, PassFreeCap is 0, the head test can never take the
	// pass-free branch, and the loop is byte-identical.
	const int32 PassFreeCap = (VoxelGpuJobLeanEnabled() && VoxelGpuWorklistEnabled()
	                           && VoxelGpuWorklistClaimEnabled() && VoxelGpuPoolAllocEnabled())
		? int32(FMath::Min(VoxelGpuWorklistCellBudget(),
		                   65535u / FVoxelGpuWorklist::kWriteGroupsPerRecord))
		: 0;

	int32 DemandPromoted = 0;
	int32 DemandPassFree = 0;
	int32 LowPriorityPromoted = 0;
	while (InFlight.Num() + Batch.Num() < MaxInFlight)
	{
		// Demand first, always, while it has both work and allowance.
		//
		// THE HEAD IS PEEKED, NOT POPPED, so the allowance decision costs
		// nothing and strict FIFO is untouched: a head that cannot be afforded
		// stays exactly where it is, and low-priority still gets its turn
		// exactly as it does today when demand exhausts BatchCap.
		bool bHeadPassFree = false;
		bool bDemandCanRun = false;
		if (Queued.Num() > 0)
		{
			const FJobPtr& Head = Queued[0];
			// Predicted from fields latched at Submit, and it mirrors the
			// claim stage's own eligibility (brick-only, band-free, packing,
			// atlas-fed). A prediction that turns out wrong costs the tick a
			// few extra passes, never correctness: the record simply is not
			// consumed and the chunk runs classic, which is the same fallback
			// every deferred record already takes.
			bHeadPassFree = PassFreeCap > 0
				&& !Head->bQuadMesh
				&& Head->Region.BandEdge == 0
				&& Head->bBrickPack
				&& Head->BrickRegion.bRasterAtlas;
			bDemandCanRun = bHeadPassFree ? (DemandPassFree < PassFreeCap)
			                              : (DemandPromoted < BatchCap);
		}
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
		// STAMPED UNCONDITIONALLY, not just for latency stats. It used to be
		// gated on GVoxelGpuMeshLatencyStats because its only consumer was
		// QueuedMs, a diagnostic. It now also drives the in-flight TIMEOUT,
		// which must not change behaviour depending on whether a stats cvar is
		// on. One FPlatformTime::Seconds() per promoted job is nothing against
		// the seven compute passes that follow it.
		Job->PromotedSeconds = FPlatformTime::Seconds();
		Job->PromotedTickSeq = TickSeq;

		FString ValidationError;
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			Rejected.Emplace(Job, TEXT("Requires SM6 (64-bit integer shader ops). Relaunch with -sm6."));
			continue;
		}
		// -VoxelGpuJobLean, half three: a LEAN job's mesh region is never
		// dispatched, so validating it and sizing its graph are two per-chunk
		// terms paid for a request that no graph will ever see.
		//
		// WHY THE COVERAGE IS NOT LOST. The asset block of this validation is
		// the only part that reads anything variable, and Submit already ran
		// the identical check against the BRICK region -- which, on this path,
		// holds those very instances (see MakeBrickRegionMoveAssets). Every
		// other check is about the footprint SHAPE, which SetChunkFootprint
		// constructs and which MakeBrickRegion re-derives and Submit
		// re-validates. A job that reached here with bBrickPack set has
		// therefore already passed an equivalent validation this same call
		// stack. A job WITHOUT bBrickPack is not lean and takes the full path
		// below, unchanged.
		//
		// WHY ComputeRegionGraphSizes GOES WITH IT. Every field of FJob::Sizes
		// is read on the quad/readback path only -- CountsBytes, MaskCount,
		// MaxQuads, QuadWriteBase -- and InteriorX/Y/Z feed
		// RebaseQuadsToChunkLocal, which a brick-only job never reaches. They
		// are zeroed rather than left stale, so a future reader gets a
		// defensible zero instead of a plausible number from another shape.
		const bool bLeanPromote = VoxelGpuJobLeanEnabled()
			&& VoxelGpuLeanBrickJobsEnabled()
			&& !Job->bQuadMesh
			&& Job->Region.BandEdge == 0
			&& Job->bBrickPack;
		if (bLeanPromote)
		{
			++JobCost.LeanPromoteValSkipped;
		}
		else
		{
			++JobCost.LeanPromoteValRan;
			if (!VoxelGpuWorldGen::ValidateRegionRequest(Job->Region, ValidationError))
			{
				Rejected.Emplace(Job, ValidationError);
				continue;
			}
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

		if (bLeanPromote)
		{
			// Zeroed, not left stale: Sizes.BricksX is 0 here, and
			// `0 - 2` on the unsigned Interior fields would publish 4294967294
			// -- a number that is wrong in the direction that looks like data.
			Job->Sizes = VoxelGpuWorldGen::FRegionGraphSizes();
			Job->InteriorX = 0;
			Job->InteriorY = 0;
			Job->InteriorZ = 0;
		}
		else
		{
			Job->Sizes = VoxelGpuWorldGen::ComputeRegionGraphSizes(Job->Region);
			Job->InteriorX = Job->Sizes.BricksX - 2;
			Job->InteriorY = Job->Sizes.BricksY - 2;
			Job->InteriorZ = Job->Sizes.BricksZ - 2;
		}

		// Counted here, not at the take, so a REJECTED job still costs no
		// allowance -- preserving BatchCap's documented "counts promoted jobs,
		// not loop iterations" behaviour for demand exactly as before.
		if (bTakeLowPriority)
		{
			++LowPriorityPromoted;
		}
		else if (bHeadPassFree)
		{
			// Charged to the pass-free allowance, not to BatchCap.
			++DemandPassFree;
			++JobCost.PassFreePromotes;
			if (DemandPromoted >= BatchCap)
			{
				// THE REFUTATION. This promotion would not have happened under
				// the shipped gate. A window in which this stays 0 means the
				// switch converted nothing.
				++JobCost.PassFreeOverCap;
			}
		}
		else
		{
			++DemandPromoted;
		}
		Batch.Add(MoveTemp(Job));

		// --- Tier B.1: sweep this head's Z-siblings out of the same queue ---
		//
		// THE CAP SEMANTICS CHANGE IS THE FEATURE. BatchCap exists to bound
		// per-tick GRAPH SETUP on the render thread (its comment records the
		// sweep: bigger per-chunk bursts hitched). A fused stack adds roughly
		// ONE classic job's worth of passes however many chunks it carries, so
		// a sibling rides its head's allowance instead of consuming one -- the
		// per-chunk reason for the cap is exactly what the fusion removes.
		// MaxInFlight still bounds JOB count (readbacks, poll volume), so the
		// sweep stops at the room it actually has.
		//
		// A COPY of the head handle, deliberately: Batch.Add below can
		// reallocate the array a reference would point into.
		if (bBatchArmed)
		{
			const FJobPtr Promoted = Batch.Last();
			if (!Promoted->bBrickPack)
			{
				// Not a brick producer (bench fixture shapes, non-chunk
				// footprints); nothing the batch path could fuse. Not counted:
				// it was never a candidate.
			}
			else if (Promoted->bQuadMesh)
			{
				NoteBatchFallback(EBatchFallback::QuadMesh);
			}
			else if (Promoted->Region.BandEdge > 0)
			{
				NoteBatchFallback(EBatchFallback::Band);
			}
			else if (Promoted->BrickRegion.AssetInstances.Num() > 0)
			{
				NoteBatchFallback(EBatchFallback::Assets);
			}
			else
			{
				TArray<FJobPtr>& Sweep = StackSweeps.AddDefaulted_GetRef();
				Sweep.Add(Promoted);
				for (int32 SIdx = 0; SIdx < Source.Num(); )
				{
					if (InFlight.Num() + Batch.Num() >= MaxInFlight ||
					    Sweep.Num() >= VoxelGpuBatchDetail::kMaxStackChunks)
					{
						break;
					}
					const FJobPtr& Cand = Source[SIdx];
					if (!IsStackableBrickJob(*Cand) || !SameBrickColumn(*Promoted, *Cand))
					{
						++SIdx;
						continue;
					}
					if (!CompatibleForStack(*Promoted, *Cand))
					{
						// Same column, different inputs: residency moved
						// between the two Submits, so fusing them would hand
						// one chunk terrain generated from the other's window.
						// It stays queued and heads its own dispatch later.
						NoteBatchFallback(EBatchFallback::Mismatch);
						++SIdx;
						continue;
					}
					FJobPtr Sibling = Cand;
					Source.RemoveAt(SIdx, EAllowShrinking::No);
					// The main loop's promote bookkeeping, verbatim, minus the
					// allowance increment -- see the cap note above.
					Sibling->PromotedSeconds = FPlatformTime::Seconds();
					Sibling->PromotedTickSeq = TickSeq;
					FString SiblingError;
					if (!VoxelGpuWorldGen::ValidateRegionRequest(Sibling->Region, SiblingError))
					{
						Rejected.Emplace(Sibling, SiblingError);
						continue;
					}
					Sibling->Sizes = VoxelGpuWorldGen::ComputeRegionGraphSizes(Sibling->Region);
					Sibling->InteriorX = Sibling->Sizes.BricksX - 2;
					Sibling->InteriorY = Sibling->Sizes.BricksY - 2;
					Sibling->InteriorZ = Sibling->Sizes.BricksZ - 2;
					Sweep.Add(Sibling);
					Batch.Add(MoveTemp(Sibling));
				}
			}
		}
	}

	// WHY THE PROMOTE LOOP STOPPED. Three counters, evaluated once per tick,
	// and they are the manager's own version of the streaming side's
	// exitCap=/exitEmpty= pair -- which the handoff's standing rule says must
	// be read together with dispatched/drained. Order matters: the cap is
	// checked first because the loop condition is what enforces it, and a tick
	// that hit the cap may ALSO have work left queued.
	if (InFlight.Num() + Batch.Num() >= MaxInFlight)
	{
		++JobCost.PromoteExitCap;
	}
	else if (Queued.Num() > 0 || QueuedLowPriority.Num() > 0)
	{
		++JobCost.PromoteExitQuota;
	}
	else
	{
		++JobCost.PromoteExitEmpty;
	}

	// --- Tier B.1: turn the sweeps into fused stack dispatches --------------
	//
	// Sort each sweep by chunk z and fuse every maximal CONTIGUOUS run of 2+
	// into one stack. Contiguity is not a preference: the fused region is one
	// solid span of bricks, so a gap would generate chunks nobody asked for
	// and -- worse -- deliver them to nobody. A duplicate key (two queued jobs
	// for the same chunk) breaks a run the same way and each part stands
	// alone.
	for (TArray<FJobPtr>& Sweep : StackSweeps)
	{
		Sweep.Sort([](const FJobPtr& A, const FJobPtr& B)
		{
			return A->BrickKey.Z < B->BrickKey.Z;
		});
		int32 RunStart = 0;
		for (int32 I = 1; I <= Sweep.Num(); ++I)
		{
			const bool bRunEnds = (I == Sweep.Num()) ||
				(Sweep[I]->BrickKey.Z != Sweep[I - 1]->BrickKey.Z + 1);
			if (!bRunEnds)
			{
				continue;
			}
			const int32 RunLen = I - RunStart;
			if (RunLen < 2)
			{
				// A lone chunk fuses with nobody; it dispatches classic,
				// exactly as with the switch off -- counted, so "armed but
				// nothing stacked" is a readable state instead of a mystery.
				NoteBatchFallback(EBatchFallback::Single);
				RunStart = I;
				continue;
			}
			TSharedPtr<FVoxelGpuBrickStack, ESPMode::ThreadSafe> Stack =
				MakeShared<FVoxelGpuBrickStack, ESPMode::ThreadSafe>();
			Stack->NumChunks = RunLen;
			// Latched at assembly, immutable after -- the poll and the graph
			// build both read it. (bStackClaim implies PoolAlloc is armed.)
			Stack->bClaimBased = bStackClaim;
			// The bottom member's halo-free region stretched over the run:
			// same 32x32 columns, same raster window (memcmp-verified equal
			// across members), K chunks of z. decodeBrick decomposes it back
			// into exactly the run's chunks, in the same ascending-z order the
			// members were just sorted into.
			Stack->Region = Sweep[RunStart]->BrickRegion;
			Stack->Region.BricksZ = uint32(RunLen) * VoxelGpuChunkRegion::kInteriorBricks;
			Stack->Region.bPerChunkBrickTotals = true;
			FString StackError;
			if (!VoxelGpuWorldGen::ValidateRegionRequest(Stack->Region, StackError))
			{
				// Loud, because the members will silently dispatch classic and
				// the only other trace would be a throughput number that never
				// improved -- the exact shape of the fork that carried zero
				// traffic under retirement.
				UE_LOG(LogVoxelGpuMeshJob, Error,
				       TEXT("[gpu-batch] assembled stack region INVALID (%s); its %d chunks ")
				       TEXT("dispatch per-chunk instead"),
				       *StackError, RunLen);
				NoteBatchFallback(EBatchFallback::Invalid);
				RunStart = I;
				continue;
			}
			for (int32 M = 0; M < RunLen; ++M)
			{
				Sweep[RunStart + M]->BrickStack = Stack;
				Sweep[RunStart + M]->StackChunkIndex = M;
			}
			++BatchStacks;
			BatchStackChunks += RunLen;
			BatchStackPasses += VoxelGpuBatchDetail::kPassesPerStackDispatch;
			RunStart = I;
		}
	}
	if (bBatchArmed)
	{
		// Whatever did not fuse dispatches classic; tallied so the window line
		// always shows where every promoted chunk went.
		for (const FJobPtr& BatchJob : Batch)
		{
			if (!BatchJob->BrickStack.IsValid())
			{
				++BatchClassicJobs;
				BatchClassicPasses += BatchJob->bBrickPack
					? VoxelGpuBatchDetail::kPassesPerClassicBrickJob
					: VoxelGpuBatchDetail::kPassesPerClassicQuadJob;
			}
		}
	}

	if (Batch.Num() > 0)
	{
		// Counted here, where the batch is final (heads plus every Z-sibling
		// the sweep took), so promoteMs/promoted is a true per-chunk figure.
		JobCost.Promoted += int64(Batch.Num());
		++JobCost.Batches;
		InFlight.Append(Batch);
		DispatchBatch(MoveTemp(Batch));
	}

	for (const TPair<FJobPtr, FString>& R : Rejected)
	{
		Deliver(R.Key, EVoxelGpuMeshJobStatus::Rejected, R.Value);
	}

	if (bBatchArmed)
	{
		MaybeLogBatchWindow();
	}
	// The lean window line, same armed-only rule as the batch one: a control
	// leg must not gain a log line.
	if (VoxelGpuLeanBrickJobsEnabled())
	{
		VoxelGpuLeanDetail::MaybeLogWindow();
	}

	// --- P3 spine: one Flush per tick, every tick while armed ---------------
	//
	// Flushing on EMPTY ticks is the point, not waste: the constant-passes-
	// per-tick shape (upload when there is one, args + prover always) is
	// exactly what the converted chain will cost, so the spine's own overhead
	// is measured honestly from day one. A control leg (switch off) reaches
	// none of this -- no buffers, no passes, no log lines, byte-identical.
	if (VoxelGpuWorklistEnabled())
	{
		if (!Worklist.IsInitialized())
		{
			Worklist.Init(VoxelGpuWorklistCapacity());
			// The Voxelize arm is a latched flag plus a budget, not an input
			// set (it shares the column stage's inputs) -- handed over once,
			// here, so every Flush from the first one applies the cell-arena
			// consume clamp consistently.
			Worklist.SetVoxelizeStageArmed(VoxelGpuWorklistVoxelizeEnabled(),
			                               VoxelGpuWorklistCellBudget());
			Worklist.SetClassifyStageArmed(VoxelGpuWorklistClassifyEnabled());
			Worklist.SetAssetStampStageArmed(VoxelGpuWorklistAssetStampEnabled());
			Worklist.SetPackStageArmed(VoxelGpuWorklistPackEnabled());
			if (VoxelGpuWorklistClaimEnabled() &&
			    VoxelGpuWorklistCellBudget() > 65535u / FVoxelGpuWorklist::kWriteGroupsPerRecord)
			{
				// The Write triple is Take x 148 groups in X; D3D caps an
				// indirect dispatch dimension at 65,535 groups, so a consume
				// budget above 442 records would silently clip the word
				// copies -- torn pool payloads with no error anywhere.
				// Refuse the arm loudly instead.
				UE_LOG(LogVoxelGpuMeshJob, Error,
				       TEXT("[gpu-worklist] CLAIM STAGE ARM REFUSED: -VoxelGpuWorklistCellBudget=%u ")
				       TEXT("exceeds the Write triple's %u-record ceiling (65,535 groups / %u per ")
				       TEXT("record). Lower the budget or split the dispatch before arming."),
				       VoxelGpuWorklistCellBudget(),
				       65535u / FVoxelGpuWorklist::kWriteGroupsPerRecord,
				       FVoxelGpuWorklist::kWriteGroupsPerRecord);
			}
			else if (VoxelGpuWorklistClaimEnabled())
			{
				// The Claim stage's pool binder (the plan doc's decision: the
				// worklist does not include the pool's lifecycle). Runs on
				// the RENDER thread inside the flush command; BindPoolAlloc's
				// body, callback-shaped. Captures nothing -- the pool is the
				// process-lifetime global.
				Worklist.SetClaimStageArmed(
					true,
					[](FRDGBuilder& GraphBuilder, FRHICommandListImmediate& RHICmdList,
					   FVoxelGpuWorklist::FPoolBindings& Out) -> bool
					{
						FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
						const FVoxelBrickPoolBuffersRef Buffers = Pool.GetBuffers();
						FVoxelBrickPool::EnsureCreated_RenderThread(RHICmdList, Buffers);
						if (!Buffers.IsValid() || !Buffers->IsValid() || !Buffers->HasGpuAlloc())
						{
							return false;
						}
						Out.Layout = Pool.GetGpuAllocLayout();
						Out.ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
						Out.PoolDesc = GraphBuilder.RegisterExternalBuffer(
							Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
						Out.PoolOcc = GraphBuilder.RegisterExternalBuffer(
							Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
						Out.PoolMat = GraphBuilder.RegisterExternalBuffer(
							Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
						Out.PoolTable = GraphBuilder.RegisterExternalBuffer(
							Buffers->ChunkTablePooled, TEXT("VoxelBrickPool.ChunkTable"));
						Out.AllocState = GraphBuilder.RegisterExternalBuffer(
							Buffers->AllocStatePooled, TEXT("VoxelBrickPool.AllocState"));
						Out.AllocBitmap = GraphBuilder.RegisterExternalBuffer(
							Buffers->AllocBitmapPooled, TEXT("VoxelBrickPool.AllocBitmap"));
						Out.AllocSide = GraphBuilder.RegisterExternalBuffer(
							Buffers->AllocSidePooled, TEXT("VoxelBrickPool.AllocSide"));
						return true;
					},
					VoxelGpuWorklistVerifyClaimEnabled());
			}
		}
		if (!bWorklistArmingLogged)
		{
			bWorklistArmingLogged = true;
			UE_LOG(LogVoxelGpuMeshJob, Log,
			       TEXT("[gpu-worklist] ARMED: ring cap %u records, budget %u/tick; args + ")
			       TEXT("indirect prover dispatch every tick; proof readback (16 B) every ~5 s ")
			       TEXT("of consumption. Kernels NOT converted yet: passes/tick will NOT ")
			       TEXT("flatten on this leg -- the gate here is proof ok + records flowing. ")
			       TEXT("FAILING READINGS: proof FAIL (leg invalid); records=0 with chunks ")
			       TEXT("flowing (see the skips= reasons); proofs landed=0 with records ")
			       TEXT("flowing (GPU consumption unverified -- the spine may be dead)."),
			       VoxelGpuWorklistCapacity(), VoxelGpuWorklistBudget());
			if (VoxelGpuWorklistColumnsEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] COLUMN STAGE ARMED (-VoxelGpuWorklistColumns): the ")
				       TEXT("Column kernel is CONVERTED -- one indirect dispatch per tick ")
				       TEXT("replaces one ColumnMain pass per lean chunk (17 -> 16 passes on ")
				       TEXT("the lean-alloc shape). The other six stages remain per-chunk, so ")
				       TEXT("passes/tick drops but does NOT flatten, and throughput is NOT ")
				       TEXT("expected to move (admission is today's limiter). Read the wlcols ")
				       TEXT("line: conv must grow; fb growing with conv=0 means the stage ")
				       TEXT("converts nothing. Verify arm: -VoxelGpuWorklistVerifyCols=1."));
			}
			if (VoxelGpuWorklistVoxelizeEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] VOXELIZE STAGE ARMED (-VoxelGpuWorklistVoxelize): the ")
				       TEXT("Voxelize kernel is CONVERTED -- a second indirect dispatch per tick ")
				       TEXT("replaces one VoxelizeMain pass per asset-free lean chunk (16 -> 15 ")
				       TEXT("passes on the lean-alloc shape). Consume clamped to %u records/flush ")
				       TEXT("(-VoxelGpuWorklistCellBudget) for the 128 KiB/record cell arena. ")
				       TEXT("Asset chunks fall back, counted on the wlvox line's fbAssets. ")
				       TEXT("Requires -VoxelGpuWorklistColumns=1%s. Verify arm: ")
				       TEXT("-VoxelGpuWorklistVerifyVox=1."),
				       VoxelGpuWorklistCellBudget(),
				       VoxelGpuWorklistColumnsEnabled()
					       ? TEXT("") : TEXT(" -- MISSING on this leg, so EVERY chunk will fall back"));
			}
			if (VoxelGpuWorklistAssetStampEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] ASSETSTAMP STAGE ARMED (-VoxelGpuWorklistAssetStamp): ")
				       TEXT("asset instances ride the flush as a per-flush blob and the ")
				       TEXT("order-preserving gather stamps the cell arena -- ASSET CHUNKS now ")
				       TEXT("enter the converted chain (fbAssets should stop growing; wlstamp ")
				       TEXT("conv must grow on any flight with assets). Requires ")
				       TEXT("-VoxelGpuWorklistVoxelize=1%s. Verify arm: -VoxelGpuWorklistVerifyStamp=1."),
				       VoxelGpuWorklistVoxelizeEnabled()
					       ? TEXT("") : TEXT(" -- MISSING on this leg, so EVERY chunk will fall back"));
			}
			if (VoxelGpuWorklistPackEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] PACK STAGE ARMED (-VoxelGpuWorklistPack): the Pack ")
				       TEXT("kernel is CONVERTED -- one indirect dispatch (+ a mask-arena ")
				       TEXT("clear) per tick replaces the chunk-mask clear and BrickPackMain ")
				       TEXT("per totals-fed chunk (7 -> 5 on the lean-alloc shape); the ")
				       TEXT("claim/write passes source from the pack arenas. Requires ")
				       TEXT("-VoxelGpuWorklistClassify=1%s. Read the wlpack line. Verify arm: ")
				       TEXT("-VoxelGpuWorklistVerifyPack=1."),
				       VoxelGpuWorklistClassifyEnabled()
					       ? TEXT("") : TEXT(" -- MISSING on this leg, so EVERY chunk will fall back"));
			}
			if (VoxelGpuWorklistClaimEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] CLAIM STAGE ARMED (-VoxelGpuWorklistClaim): the ")
				       TEXT("claim + all four pool writes are CONVERTED -- three indirect ")
				       TEXT("dispatches per tick replace FIVE per-chunk passes, and a ")
				       TEXT("claim-fed chunk adds ZERO brick passes to the batch graph ")
				       TEXT("(5 -> 0 on the lean-alloc shape; production is fully inside the ")
				       TEXT("flush graph). Requires -VoxelGpuWorklistPack=1%s. Read the ")
				       TEXT("wlclaim line. Verify arm: -VoxelGpuWorklistVerifyClaim=1."),
				       VoxelGpuWorklistPackEnabled()
					       ? TEXT("") : TEXT(" -- MISSING on this leg, so EVERY chunk will fall back"));
			}
			if (VoxelGpuWorklistClassifyEnabled())
			{
				UE_LOG(LogVoxelGpuMeshJob, Log,
				       TEXT("[gpu-worklist] CLASSIFYTOTALS STAGE ARMED (-VoxelGpuWorklistClassify): ")
				       TEXT("the fused classify + scan + totals is CONVERTED -- two indirect ")
				       TEXT("dispatches per tick replace EIGHT per-chunk passes (classify, two ")
				       TEXT("3-pass scans, totals; 15 -> 7 on the lean-alloc shape). Cell-fed ")
				       TEXT("chunks only; asset chunks fall back with the Voxelize stage's. ")
				       TEXT("Requires -VoxelGpuWorklistVoxelize=1%s. Read the wlct line. ")
				       TEXT("Verify arm: -VoxelGpuWorklistVerifyCT=1."),
				       VoxelGpuWorklistVoxelizeEnabled()
					       ? TEXT("") : TEXT(" -- MISSING on this leg, so EVERY chunk will fall back"));
			}
		}
		++WorklistWinTicks;
		// Column stage armed: DispatchBatch already flushed this tick (it has
		// to -- the batch graph reads what the flush's column dispatch wrote,
		// and render commands execute in enqueue order). Flushing again here
		// would double the args/prover passes and split the tick's consume
		// window in two. Ticks with no batch still flush here, so the spine's
		// constant per-tick shape (and the proof cadence) is unchanged.
		if (!bWorklistFlushedThisTick)
		{
			Worklist.Flush(VoxelGpuWorklistBudget());
		}
		bWorklistFlushedThisTick = false;
		// The tick's pass tally folds HERE, not in DispatchBatch, because the
		// spine flushes on batchless ticks too and those ticks' 2-3 passes
		// are real GPU work: args + prover always, plus the Column indirect
		// dispatch once the stage is armed (recorded even at Take == 0 --
		// constant pass count per tick is the property being bought).
		// Tallying only inside DispatchBatch was the 2026-08-23 dead-counter
		// bug: every batchless tick tallied 0 while dispatching 2-3 passes,
		// and the window line read mean=0.0 over hundreds of ticks.
		const int64 WorklistPassesThisTick = WorklistBatchPassesThisTick
			+ VoxelGpuBatchDetail::kWorklistSpinePassesPerTick
			+ (Worklist.IsColumnStageArmed() ? 1 : 0)
			+ (Worklist.IsVoxelizeStageArmed() ? 1 : 0)
			+ (Worklist.IsAssetStampStageArmed() ? 1 : 0)
			+ (Worklist.IsClassifyStageArmed() ? 2 : 0)
			+ (Worklist.IsPackStageArmed() ? 2 : 0)
			// Claim stage: claim + write + desc/record (+ the byte gate on
			// the verify arm) -- three (four) indirect dispatches per tick.
			+ (Worklist.IsClaimStageArmed()
			       ? (VoxelGpuWorklistVerifyClaimEnabled() ? 4 : 3) : 0);
		WorklistBatchPassesThisTick = 0;
		WorklistWinPasses += WorklistPassesThisTick;
		WorklistWinPassesMaxTick = FMath::Max(WorklistWinPassesMaxTick, WorklistPassesThisTick);
		MaybeLogWorklistWindow();
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

	MaybeLogJobCostWindow();
}

// The Tier B.1 window line. ~5 s cadence, and it prints ONLY when something
// happened -- an armed-but-idle manager stays quiet, an armed-and-declining
// one prints zero stacks WITH the reasons, which is the state the counters
// exist to make impossible to miss. The crosscheck pair comes from the
// render-thread atomics (see VoxelGpuBatchDetail); everything else is
// game-thread. Read-and-reset with a single reader (this function), for
// FTickStageMs's two-readers-halve-each-other reason.
void FVoxelGpuMeshJobManager::MaybeLogBatchWindow()
{
	const double Now = FPlatformTime::Seconds();
	if (LastBatchLogSeconds <= 0.0)
	{
		LastBatchLogSeconds = Now;
		return;
	}
	if (Now - LastBatchLogSeconds < 5.0)
	{
		return;
	}

	const int32 CrossPass = VoxelGpuBatchDetail::GCrosscheckPass.exchange(0, std::memory_order_relaxed);
	const int32 CrossFail = VoxelGpuBatchDetail::GCrosscheckFail.exchange(0, std::memory_order_relaxed);
	int32 FallbackTotal = 0;
	for (int32 F = 0; F < int32(EBatchFallback::COUNT); ++F)
	{
		FallbackTotal += BatchFallbacks[F];
	}
	if (BatchStacks == 0 && BatchClassicJobs == 0 && FallbackTotal == 0 &&
	    CrossPass == 0 && CrossFail == 0)
	{
		LastBatchLogSeconds = Now;
		return;
	}

	// "vs ~N per-chunk": what the SAME chunks would have cost as classic
	// per-chunk dispatches -- the number the fusion exists to beat.
	const int32 PerChunkEquivalent =
		BatchStackChunks * VoxelGpuBatchDetail::kPassesPerClassicBrickJob;
	// THE CROSSCHECK COLUMN MUST NOT READ AS A VERDICT IT DID NOT REACH. The
	// pass/fail pair lives in the TOTALS-READBACK harvest path, and
	// -VoxelGpuStackClaim deletes that readback by design -- so under
	// stack-claim the pair is structurally zero forever. Tonight's sweep read
	// "crosscheck 0 ok / 0 FAIL" on an armed leg as if it were a green gate;
	// it was a gate that never ran. Print N/A and point at the gates that ARE
	// live on that path (the claim kernel's in-GPU split check and the bitmap
	// double-grant counter, both on the [brick-gpualloc] line) instead of a
	// zero that reads as health.
	const bool bClaimPath = VoxelGpuStackClaimEnabled() && VoxelGpuPoolAllocEnabled();
	const FString CrossText = (bClaimPath && CrossPass == 0 && CrossFail == 0)
		? FString(TEXT("N/A under StackClaim (no totals readback -- verdicts live on [brick-gpualloc]: claimFail + doubleGrant)"))
		: FString::Printf(TEXT("%d ok / %d FAIL"), CrossPass, CrossFail);
	UE_LOG(LogVoxelGpuMeshJob, Log,
	       TEXT("[gpu-batch] %.1fs window: %d stacks / %d chunks (~%d passes, vs ~%d per-chunk); ")
	       TEXT("classic %d jobs (~%d passes); fallbacks quadmesh %d band %d assets %d raster %d ")
	       TEXT("single %d invalid %d; crosscheck %s"),
	       Now - LastBatchLogSeconds, BatchStacks, BatchStackChunks, BatchStackPasses,
	       PerChunkEquivalent, BatchClassicJobs, BatchClassicPasses,
	       BatchFallbacks[uint8(EBatchFallback::QuadMesh)],
	       BatchFallbacks[uint8(EBatchFallback::Band)],
	       BatchFallbacks[uint8(EBatchFallback::Assets)],
	       BatchFallbacks[uint8(EBatchFallback::Mismatch)],
	       BatchFallbacks[uint8(EBatchFallback::Single)],
	       BatchFallbacks[uint8(EBatchFallback::Invalid)],
	       *CrossText);

	BatchStacks = 0;
	BatchStackChunks = 0;
	BatchStackPasses = 0;
	BatchClassicJobs = 0;
	BatchClassicPasses = 0;
	FMemory::Memzero(BatchFallbacks, sizeof(BatchFallbacks));
	LastBatchLogSeconds = Now;
}

// The P3 spine window line, ~5 s cadence, armed-only (a control leg gains no
// log line). Passes-per-tick sits FIRST and the chunk rate beside it, because
// that pair is the 50k gate: 15 passes/chunk at 50,000 chunks/s is 25x the
// ~500-passes-per-tick hitch cliff, and the converted chain's ~14/tick is 36x
// UNDER it -- flatness against chunk rate is the win, throughput is not
// (four-arm sweep, 2026-08-23: pass count moved 4.3x, throughput 0%).
//
// FAILING READINGS, in the order they should be checked:
//   identity DRIFT           -> records lost or double-consumed; leg invalid.
//   records=0, chunks flowing -> the spine carries no traffic; the skips=
//                                reasons name the missing precondition.
//   proof landed=0 w/ records -> GPU consumption UNVERIFIED; treat the spine
//                                as dead until a proof lands (the [gpu-worklist]
//                                proof lines come from FVoxelGpuWorklist).
//   refusedFull growing       -> ring back-pressure; raise -VoxelGpuWorklistCap
//                                or the budget, and say which in the report.
//   PASS TALLY DEAD marker    -> the passes/tick number itself measured
//                                nothing this window (mean below the 2/tick
//                                args+prover floor every armed tick pays);
//                                nothing about pass counts on the leg is
//                                believable. mean=0.0 without the marker
//                                cannot happen any more -- if it appears,
//                                the tripwire is broken too.
void FVoxelGpuMeshJobManager::MaybeLogWorklistWindow()
{
	const double Now = FPlatformTime::Seconds();
	if (LastWorklistLogSeconds <= 0.0)
	{
		LastWorklistLogSeconds = Now;
		return;
	}
	if (Now - LastWorklistLogSeconds < 5.0)
	{
		return;
	}
	const double WindowSeconds = Now - LastWorklistLogSeconds;
	const FVoxelGpuWorklist::FWindow W = Worklist.ReadAndResetWindow();
	const FVoxelGpuWorklist::FProofStatus P = Worklist.GetProofStatus();
	WorklistCumAppended += W.Appended;
	WorklistCumConsumed += W.Consumed;
	WorklistCumRefused += W.RefusedFull;
	const int64 SkipTotal = WorklistSkipNoPack + WorklistSkipQuadMesh + WorklistSkipBand
	                      + WorklistSkipNoAlloc + WorklistSkipNoAtlas;
	// The quiet gate reads the WINDOW'S skips, not the cumulative counters.
	// Gating on the cumulative total was the other half of the 2026-08-23
	// dead-counter reading: once any chunk had ever skipped, every
	// post-flight linger window printed forever -- all zeros -- and the
	// summary's tail -1 read exactly the linger line leg-summary.sh exists
	// to avoid. Now an idle window stays quiet, so the LAST window line of
	// a leg is its last ACTIVE window.
	if (WorklistWinChunks == 0 && W.Appended == 0 && SkipTotal == WorklistPrevSkipTotal)
	{
		// Armed and idle (no jobs promoted this window): stay quiet, the
		// [gpu-batch] rule. Armed-and-declining still prints, with reasons.
		// Reset EVERYTHING windowed: the spine constant accrued real passes
		// on these idle ticks, and leaking them into the next active window
		// would overstate it.
		WorklistWinTicks = 0;
		WorklistWinChunks = 0;
		WorklistWinPasses = 0;
		WorklistWinPassesMaxTick = 0;
		LastWorklistLogSeconds = Now;
		return;
	}
	// appended == consumed + pending over the CUMULATIVE counters (refusals
	// were never appended). The host mirror makes this an arithmetic identity;
	// a drift is a code bug in the ring, and everything measured on the leg
	// is void until it is explained.
	const int64 Drift = int64(WorklistCumAppended) - int64(WorklistCumConsumed) - int64(W.Pending);
	const double MeanPasses = WorklistWinTicks > 0
		? double(WorklistWinPasses) / double(WorklistWinTicks) : 0.0;
	const bool bUnproven = P.Landed == 0 && WorklistCumConsumed > 0;
	// The counter's own tripwire. Every armed tick tallies at least the
	// args + prover constant, so a printed window whose mean sits below that
	// floor is not a quiet spine -- it is this tally measuring nothing (the
	// eighth self-reporting-healthy counter of 2026-08-23 was exactly this
	// line). checked-not-assumed, because "mean=0.0" must never again parse
	// as a plausible reading.
	const bool bTallyDead = WorklistWinTicks > 0
		&& WorklistWinPasses < int64(VoxelGpuBatchDetail::kWorklistSpinePassesPerTick)
		                       * WorklistWinTicks;
	UE_LOG(LogVoxelGpuMeshJob, Log,
	       TEXT("[gpu-worklist] %.1fs window: passes/tick mean=%.1f max=%lld over %lld ticks ")
	       TEXT("(%lld chunks, %.0f chunks/s); records appended=%llu consumed=%llu pending=%u ")
	       TEXT("refusedFull=%llu; identity %s; skips noPack=%lld quads=%lld band=%lld ")
	       TEXT("noAlloc=%lld noAtlas=%lld (cumulative); proof landed=%llu failed=%llu%s%s%s"),
	       WindowSeconds, MeanPasses, WorklistWinPassesMaxTick, WorklistWinTicks,
	       WorklistWinChunks, WindowSeconds > 0.0 ? double(WorklistWinChunks) / WindowSeconds : 0.0,
	       W.Appended, W.Consumed, W.Pending, W.RefusedFull,
	       Drift == 0 ? TEXT("ok") : *FString::Printf(TEXT("DRIFT=%lld (LEG INVALID)"), Drift),
	       WorklistSkipNoPack, WorklistSkipQuadMesh, WorklistSkipBand,
	       WorklistSkipNoAlloc, WorklistSkipNoAtlas,
	       P.Landed, P.Failed,
	       P.Failed > 0 ? TEXT(" (PROOF FAILED -- LEG INVALID)") : TEXT(""),
	       bUnproven ? TEXT(" (NO PROOF LANDED -- GPU consumption UNVERIFIED)") : TEXT(""),
	       bTallyDead ? TEXT(" (PASS TALLY DEAD -- mean below the args+prover floor)") : TEXT(""));
	// P3 Column stage line, armed-only (a spine-only or control leg must not
	// gain a log line). THE FAILING READINGS: conv=0 with fb growing -- the
	// stage is armed and converting nothing (ring refusing, records deferred
	// past the budget, or SetColumnStageInputs refusing a null atlas);
	// arenaMissing>0 -- jobs reached the batch graph with a slice but no
	// arena existed to read (flush/batch ordering bug, every one fell back
	// classic); colverify checked=0 with conv>0 and the verify switch armed
	// -- the byte gate is dead and "no mismatches" is vacuous.
	if (VoxelGpuWorklistColumnsEnabled())
	{
		const int64 ArenaMissing =
			VoxelGpuBatchDetail::GWorklistColArenaMissing.load(std::memory_order_relaxed);
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlcols conv=%lld fb=%lld arenaMissing=%lld ")
		       TEXT("colverify checked=%llu mism=%llu (cumulative)%s%s"),
		       WorklistColConverted, WorklistColFallback, ArenaMissing,
		       P.ColumnsChecked, P.ColumnDwordMismatches,
		       P.ColumnDwordMismatches > 0 ? TEXT(" (COLUMN VERIFY FAILED -- LEG INVALID)") : TEXT(""),
		       ArenaMissing > 0 ? TEXT(" (ARENA MISSING -- converted jobs fell back)") : TEXT(""));
	}
	// P3 Voxelize stage line, armed-only, the wlcols contract restated for
	// cells. THE FAILING READINGS: conv=0 with fb growing -- the stage is
	// armed and converting nothing; conv=0 with fbAssets growing and fb
	// quiet -- every chunk this leg carries assets (a flight-path fact, not
	// a bug, but the stage is buying nothing); arenaMissing>0 -- flush/batch
	// ordering broke; voxverify checked=0 with conv>0 under the verify
	// switch -- the byte gate is dead and "no mismatches" is vacuous.
	if (VoxelGpuWorklistVoxelizeEnabled())
	{
		const int64 VoxArenaMissing =
			VoxelGpuBatchDetail::GWorklistVoxArenaMissing.load(std::memory_order_relaxed);
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlvox conv=%lld fb=%lld fbAssets=%lld arenaMissing=%lld ")
		       TEXT("voxverify checked=%llu mism=%llu (cumulative)%s%s"),
		       WorklistVoxConverted, WorklistVoxFallback, WorklistVoxFallbackAssets,
		       VoxArenaMissing,
		       P.VoxCellsChecked, P.VoxCellMismatches,
		       P.VoxCellMismatches > 0 ? TEXT(" (VOXELIZE VERIFY FAILED -- LEG INVALID)") : TEXT(""),
		       VoxArenaMissing > 0 ? TEXT(" (CELL ARENA MISSING -- converted jobs fell back)") : TEXT(""));
	}
	// P3 fused ClassifyTotals line, armed-only, the wlvox contract restated.
	// A ctverify mismatch is POOL CORRUPTION (pack offsets / claim sizes
	// wrong), not cosmetics -- the loudest of the three stage gates.
	if (VoxelGpuWorklistClassifyEnabled())
	{
		const int64 CtArenaMissing =
			VoxelGpuBatchDetail::GWorklistCtArenaMissing.load(std::memory_order_relaxed);
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlct conv=%lld fb=%lld fbAssets=%lld arenaMissing=%lld ")
		       TEXT("ctverify checked=%llu mism=%llu (cumulative)%s%s"),
		       WorklistCtConverted, WorklistCtFallback, WorklistCtFallbackAssets,
		       CtArenaMissing,
		       P.CtDwordsChecked, P.CtDwordMismatches,
		       P.CtDwordMismatches > 0 ? TEXT(" (CLASSIFYTOTALS VERIFY FAILED -- LEG INVALID)") : TEXT(""),
		       CtArenaMissing > 0 ? TEXT(" (SCAN ARENAS MISSING -- converted jobs fell back)") : TEXT(""));
	}
	// P3 AssetStamp line, armed-only. THE FAILING READINGS: conv=0 while
	// wlvox conv grows AND the flight carries assets (the stage is armed and
	// admitting nothing -- check the arming order); mism>0 (LEG INVALID: a
	// wrong asset voxel is in the pool); checked=0 with conv>0 under the
	// verify switch (dead gate). conv=0 on an asset-free flight is the
	// expected reading, not a failure.
	if (VoxelGpuWorklistAssetStampEnabled())
	{
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlstamp conv=%lld stampverify checked=%llu mism=%llu ")
		       TEXT("(cumulative)%s"),
		       WorklistStampConverted,
		       P.StampCellsChecked, P.StampCellMismatches,
		       P.StampCellMismatches > 0 ? TEXT(" (ASSETSTAMP VERIFY FAILED -- LEG INVALID)") : TEXT(""));
	}
	// P3 Pack line, armed-only, the wlct contract restated. A packverify
	// mismatch is the POOL PAYLOAD being wrong -- the loudest gate of all.
	if (VoxelGpuWorklistPackEnabled())
	{
		const int64 PackArenaMissing =
			VoxelGpuBatchDetail::GWorklistPackArenaMissing.load(std::memory_order_relaxed);
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlpack conv=%lld arenaMissing=%lld packverify ")
		       TEXT("checked=%llu mism=%llu (cumulative)%s%s"),
		       WorklistPackConverted, PackArenaMissing,
		       P.PackDwordsChecked, P.PackDwordMismatches,
		       P.PackDwordMismatches > 0 ? TEXT(" (PACK VERIFY FAILED -- LEG INVALID)") : TEXT(""),
		       PackArenaMissing > 0 ? TEXT(" (PACK ARENAS MISSING -- converted jobs fell back)") : TEXT(""));
	}
	// P3 Claim line (stage 6), armed-only. THE FAILING READINGS: conv=0 while
	// wlpack conv grows -- the stage is armed and feeding nothing (check the
	// arming chain); mism>0 -- the LANDED POOL STATE is wrong, leg invalid
	// outright; checked=0 with conv>0 under the verify switch -- the byte
	// gate is DEAD and "no mismatches" is vacuous, never a pass. Claim
	// failures (arena exhausted etc.) stay on the [brick-gpualloc] counters,
	// now attributed per flush.
	if (VoxelGpuWorklistClaimEnabled())
	{
		UE_LOG(LogVoxelGpuMeshJob, Log,
		       TEXT("[gpu-worklist] wlclaim conv=%lld claimverify checked=%llu mism=%llu ")
		       TEXT("(cumulative)%s%s"),
		       WorklistClaimConverted,
		       P.ClaimDwordsChecked, P.ClaimDwordMismatches,
		       P.ClaimDwordMismatches > 0 ? TEXT(" (CLAIM VERIFY FAILED -- LEG INVALID)") : TEXT(""),
		       (VoxelGpuWorklistVerifyClaimEnabled() && WorklistClaimConverted > 0 &&
		        P.ClaimDwordsChecked == 0)
			       ? TEXT(" (DEAD GATE: conv>0, checked=0 -- NOT a pass)") : TEXT(""));
	}
	WorklistWinTicks = 0;
	WorklistWinChunks = 0;
	WorklistWinPasses = 0;
	WorklistWinPassesMaxTick = 0;
	WorklistPrevSkipTotal = SkipTotal;
	LastWorklistLogSeconds = Now;
}

// --- [gpu-jobcost]: the manager's own cost, WITH ITS DENOMINATORS -----------
//
// ~5 s cadence, silent on a window in which nothing was submitted and nothing
// was in flight, so a CPU-only leg's log is unchanged.
//
// WHY EVERY NUMBER IS PRINTED AS A RATE PER SOMETHING. The 2026-08-23 headline
// -- 0.149 ms of game thread per chunk routed to the GPU -- was
// dispatch=1,547 ms divided by gpuForked=10,391. dispatch= brackets both the
// per-chunk submit loop AND this manager's whole per-frame Tick, and those two
// do not scale with the same thing. Over a 5 s window at 30-60 fps the tick
// term alone is 150-300 ticks; 1,547 ms over 200 ticks is 7.7 ms per TICK,
// which is the same order as this class's own recorded "~18-19 ms per hitch
// frame". So the division may be against the wrong denominator entirely, and
// nothing in the tree could tell. These print both.
//
// FAILING READINGS -- state them, because "did work move?" and "is it faster?"
// are different questions and this codebase has repeatedly answered the second
// while the first was zero:
//
//  * subUs near zero while the budget line's dispatch= stays ~1,500 ms:
//    this manager's per-chunk half is NOT the cost. Read tickUs next.
//  * tickUs x ticks accounting for the bulk: the cost is PER TICK, and every
//    per-chunk optimisation anywhere in this file is beside the point. The
//    fix is then a lower tick rate for this work or a smaller in-flight
//    population, not cheaper chunks.
//  * enqDispUs / enqPollUs dominating: the game thread is BLOCKING at the
//    render-command handoff. Constructing those commands is a MoveTemp and a
//    few bools, so any material time in these brackets is render-thread
//    backpressure -- a finding about the render thread, not about this file.
//  * rasterB > 0: the raster atlas is DECLINING and the inline 46 KB window
//    fill is live. Every "the atlas fixed it" claim is then void for this leg.
//  * assetMove=0 with submits flowing and -VoxelGpuJobLean armed: the collapse
//    converted nothing. The gate needs quads retired, the lean switch, a
//    band-free request and the atlas -- the line prints assetCopy= beside it so
//    "armed and declining" is a readable state and not a mystery.
//  * bytesSaved=0 with assetMove>0: the moves ran and there was nothing to
//    move (no assets on this leg). The switch is not broken; the leg has no
//    asset traffic, and this collapse cannot be credited with anything.
//  * promoteExit all three near zero: the promote loop did not run. Broken
//    instrument, not a healthy manager.
//  * promoteExit cap= dominating: DEPTH-bound. Read `drained` on the streaming
//    side BEFORE believing it -- the 90,000-deep backlog is what makes a cap
//    reading a trap, and raising a cap in front of a starved drain makes it
//    strictly worse.
//  * promoteExit quota= dominating: QUOTA-bound at MeshBatchCap with work
//    queued. The per-tick promotion allowance is the limit; neither depth nor
//    the GPU is.
//  * promoteExit empty= dominating with inFlight low: STARVED. Nothing in this
//    manager is the limit; the producer upstream is.
//  * residSamples=0 while promoted>0: jobs are being promoted and never
//    delivered. ceiling/s is then meaningless and MUST NOT be quoted -- that
//    is a stranded population, which is the failure the exactly-one-outcome
//    contract exists to make impossible.
//  * ceiling/s below the measured fork rate: the derivation is wrong, not the
//    fork. Suspect residency being sampled from a non-representative
//    population before quoting anything.
//  * poolReplaced>0 on a COLD FILL: chunks are being generated over chunks
//    that are already resident. Each one is a wasted submit, shell, graph,
//    claim and delivery -- whole chunks of work, not microseconds. With
//    evictions=0 the only mechanism is AllocateForChunk's same-key
//    replacement. poolReplaced/shellsTaken is the redundancy rate and it
//    multiplies straight into the 7.1x that cold start needs.
//  * passFreeOverCap=0 with -VoxelGpuJobLean armed: half four converted
//    NOTHING -- either no job is claim-fed (check the worklist wlclaim line's
//    conv=) or MeshBatchCap was never the binding constraint on this leg
//    (check promoteExit quota=). Both are findings; neither is a green light,
//    and no throughput difference between arms is attributable until this
//    number is non-zero.
//  * passFreeOverCap large while the measured fork rate does not move: the cap
//    was not the wall. Read promoteExit again -- it will now say which of
//    depth, quota or starvation replaced it.
//  * leanPromoteSkip=0 with -VoxelGpuJobLean armed: half three converted
//    nothing; same four preconditions as the asset move.
//  * revalSkip=0 with -VoxelGpuJobLean armed: something LEFT the pool's
//    resident map while the shells were being taken (an eviction, or two
//    queued jobs for one chunk key), which is exactly when the revalidation is
//    NOT redundant. That is the switch behaving correctly, not failing -- but
//    revalRan growing every window on a leg reporting evictions=0 means
//    duplicate keys are reaching one batch, which is a finding of its own.
void FVoxelGpuMeshJobManager::MaybeLogJobCostWindow()
{
	const double Now = FPlatformTime::Seconds();
	if (LastJobCostLogSeconds <= 0.0)
	{
		LastJobCostLogSeconds = Now;
		return;
	}
	if (Now - LastJobCostLogSeconds < 5.0)
	{
		return;
	}

	if (JobCost.Submits == 0 && JobCost.PollJobsVisited == 0 && JobCost.Delivered == 0)
	{
		JobCost = FJobCostWindow();
		LastJobCostLogSeconds = Now;
		return;
	}

	const double SubN = double(FMath::Max<int64>(JobCost.Submits, 1));
	const double TickN = double(FMath::Max<int64>(JobCost.Ticks, 1));
	const double DelN = double(FMath::Max<int64>(JobCost.Delivered, 1));
	// The three Submit buckets are contiguous, so this must print ~0.00.
	const double SubDriftMs = JobCost.SubmitTotalMs
		- JobCost.SubmitHdrMs - JobCost.SubmitBrickMs - JobCost.SubmitQueueMs;
	// Everything this manager charges the game thread per tick, whatever it is
	// per: the four handoffs plus the delivery loop. Deliberately NOT compared
	// against TickStageMs -- that accessor is read-and-reset and has exactly one
	// reader (the subsystem's log line); reading it here would halve its totals,
	// which is the trap its own comment documents.
	const double TickChargedMs = JobCost.EnqDispatchMs + JobCost.EnqPollMs
		+ JobCost.EnqFetchMs + JobCost.EnqReleaseMs + JobCost.DeliverMs;

	// THE CEILING. Fork throughput cannot exceed MaxInFlight / residency x tick
	// rate, whatever else is fixed, because a slot cannot be reused until the
	// job in it is delivered. Derived from THIS window's own residency and tick
	// rate rather than from a nominal frame time, so it is a measurement and
	// not an assumption. Zero when nothing delivered -- see the failing reading.
	const double WindowSeconds = FMath::Max(Now - LastJobCostLogSeconds, 1e-6);
	const double TicksPerSec = double(JobCost.Ticks) / WindowSeconds;
	const double MeanResidencyTicks = JobCost.ResidencySamples > 0
		? double(JobCost.ResidencyTickSum) / double(JobCost.ResidencySamples)
		: 0.0;
	const double CeilingPerSec = MeanResidencyTicks > 0.0
		? (double(MaxInFlight) / MeanResidencyTicks) * TicksPerSec
		: 0.0;

	UE_LOG(LogVoxelGpuMeshJob, Log,
	       TEXT("[gpu-jobcost] submits=%lld subUs=%.2f (hdr %.2f + brick %.2f + queue %.2f, ")
	       TEXT("drift %.3fms) | copyPerChunk rasterB=%.0f assetB=%.0f | ticks=%lld ")
	       TEXT("promoted=%lld batches=%lld pollJobs/tick=%.0f | chargedMs=%.1f = enqDisp %.1f ")
	       TEXT("+ enqPoll %.1f + enqFetch %.1f + enqRel %.1f + deliver %.1f | perTickUs=%.1f ")
	       TEXT("perDeliverUs=%.1f delivered=%lld | jobLean=%s assetMove=%lld assetCopy=%lld ")
	       TEXT("bytesSaved=%lld revalSkip=%lld revalRan=%lld leanPromoteSkip=%lld/%lld ")
	       TEXT("passFree=%lld overCap=%lld")
	       TEXT(" || FLOW qDemand=%.0f qLow=%.0f inFlight=%.0f/max %d/cap %d ")
	       TEXT("promoteExit cap=%lld quota=%lld empty=%lld | residTicks=%.2f n=%lld ")
	       TEXT("tickHz=%.1f CEILING=%.0f/s | shellsTaken=%lld poolReplaced=%lld (%.1f%%)"),
	       JobCost.Submits,
	       (JobCost.SubmitTotalMs * 1000.0) / SubN,
	       (JobCost.SubmitHdrMs * 1000.0) / SubN,
	       (JobCost.SubmitBrickMs * 1000.0) / SubN,
	       (JobCost.SubmitQueueMs * 1000.0) / SubN,
	       SubDriftMs,
	       double(JobCost.BrickCopyRasterBytes) / SubN,
	       double(JobCost.BrickCopyAssetBytes) / SubN,
	       JobCost.Ticks, JobCost.Promoted, JobCost.Batches,
	       double(JobCost.PollJobsVisited) / TickN,
	       TickChargedMs,
	       JobCost.EnqDispatchMs, JobCost.EnqPollMs, JobCost.EnqFetchMs,
	       JobCost.EnqReleaseMs, JobCost.DeliverMs,
	       (TickChargedMs * 1000.0) / TickN,
	       (JobCost.DeliverMs * 1000.0) / DelN,
	       JobCost.Delivered,
	       VoxelGpuJobLeanEnabled() ? TEXT("ON") : TEXT("off"),
	       JobCost.LeanAssetMoves, JobCost.LeanAssetCopies, JobCost.LeanAssetBytesSaved,
	       JobCost.LeanRevalSkipped, JobCost.LeanRevalRan,
	       JobCost.LeanPromoteValSkipped, JobCost.LeanPromoteValRan,
	       JobCost.PassFreePromotes, JobCost.PassFreeOverCap,
	       double(JobCost.QueueDemandSum) / TickN,
	       double(JobCost.QueueLowSum) / TickN,
	       double(JobCost.InFlightSum) / TickN,
	       JobCost.InFlightMax, MaxInFlight,
	       JobCost.PromoteExitCap, JobCost.PromoteExitQuota, JobCost.PromoteExitEmpty,
	       MeanResidencyTicks, JobCost.ResidencySamples,
	       TicksPerSec, CeilingPerSec,
	       JobCost.ShellsTaken, JobCost.PoolReplaced,
	       JobCost.ShellsTaken > 0
	           ? 100.0 * double(JobCost.PoolReplaced) / double(JobCost.ShellsTaken) : 0.0);

	if (VoxelGpuJobLeanEnabled() && JobCost.Submits > 0 && JobCost.LeanAssetMoves == 0)
	{
		UE_LOG(LogVoxelGpuMeshJob, Warning,
		       TEXT("[gpu-jobcost] -VoxelGpuJobLean is ARMED and moved NOTHING this window ")
		       TEXT("(%lld submits, %lld copied). The move needs ALL of: quads retired ")
		       TEXT("(voxel.Terrain.RetireQuads), -VoxelGpuLeanBrickJobs, a band-free request ")
		       TEXT("(-VoxelGpuBandSeedOnly) and the raster atlas (-VoxelGpuRasterAtlas). ")
		       TEXT("Treat this window's asset-copy saving as NOT MEASURED."),
		       JobCost.Submits, JobCost.LeanAssetCopies);
	}

	if (JobCost.PoolReplaced > 0)
	{
		UE_LOG(LogVoxelGpuMeshJob, Warning,
		       TEXT("[gpu-jobcost] %lld of %lld shells REPLACED an already-resident chunk ")
		       TEXT("(%.1f%%). On a cold fill nothing should be resident yet, so each one is a ")
		       TEXT("whole chunk regenerated over itself: a wasted submit, shell, graph, claim ")
		       TEXT("and delivery. Cross-check the pool's evictions= before reading it as ")
		       TEXT("duplicate submission -- with evictions>0 this is churn, and only with ")
		       TEXT("evictions=0 is AllocateForChunk's same-key replacement the sole mechanism ")
		       TEXT("left."),
		       JobCost.PoolReplaced, JobCost.ShellsTaken,
		       JobCost.ShellsTaken > 0
		           ? 100.0 * double(JobCost.PoolReplaced) / double(JobCost.ShellsTaken) : 0.0);
	}

	JobCost = FJobCostWindow();
	LastJobCostLogSeconds = Now;
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

	// --- P1 (voxel.GPU.PoolAlloc): take each job's SHELL on the game thread --
	//
	// The shell is the fixed-size CPU half (descriptor block + record slot);
	// the variable word ranges are claimed by this batch's own graph. Taken
	// HERE, in the same function that enqueues the graph, because everything
	// the ordering rules below depend on follows from that adjacency:
	//
	//   1. The index Added entry queued by the shell is delivered by the pool's
	//      Flush, which runs later this tick -- so the index upload lands after
	//      this graph's record write.
	//   2. A shell can be evicted by a LATER shell in this same loop (the
	//      evict-until-it-fits path), so after the loop every job is
	//      RE-VALIDATED against the resident map and a stolen shell drops its
	//      brick half rather than claiming into a slot somebody else now owns.
	//   3. Every free those evictions queued is flushed BEFORE this batch is
	//      enqueued (FlushPendingGpuFrees below), so the free pass reads side
	//      table entries no claim in this batch has overwritten yet.
	if (VoxelGpuPoolAllocEnabled())
	{
		FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
		// -VoxelGpuJobLean, half two: skip rule 2's revalidation pass when it is
		// PROVABLY a no-op.
		//
		// A shell can only be stolen if something was REMOVED from the resident
		// map while these shells were being taken. There are two such removals
		// and the obvious guard only catches one: EvictOne (counted by
		// GetEvictions) and AllocateForChunk's same-key replacement (counted by
		// nothing -- two queued jobs for one chunk key, a state the Tier B.1
		// grouping comment already names as reachable). Gating on evictions
		// alone would therefore be exactly the kind of derived-instead-of-
		// checked join this project has paid for repeatedly.
		//
		// So gate on the identity instead: every successful allocation
		// increments ChunksAdded AND grows Resident by one, UNLESS it removed
		// something first. If the two deltas agree across the loop, nothing was
		// removed by any mechanism, named or not, and no shell can have moved.
		//
		// What it saves, per chunk, per tick: one TMap lookup keyed on a
		// 16-byte FVoxelBrickChunkKey plus one ~64-byte FResidentChunk copy out,
		// for an outcome the identity says cannot have occurred. Every
		// GPU-primary arm measured to date reports evictions=0 for the whole leg.
		const bool bLeanReval = VoxelGpuJobLeanEnabled();
		const int32 ResidentBefore = Pool.GetNumResidentChunks();
		const int64 AddedBefore = Pool.GetChunksAdded();
		for (const FJobPtr& Job : Batch)
		{
			if (!Job->bBrickPack)
			{
				continue;
			}
			if (Job->BrickStack.IsValid() && !Job->BrickStack->bClaimBased)
			{
				// Should be unreachable -- readback-based stacking is disabled
				// while armed -- but counted rather than assumed, because
				// "cannot happen" that silently starts happening is this
				// project's recurring bill. A CLAIM-BASED stack member falls
				// through: it takes a shell exactly like a classic job, and
				// its claim rides the stack's graph instead of its own.
				Pool.NoteGpuAllocFallback(FVoxelBrickPool::EGpuAllocFallback::Stacked);
				continue;
			}
			if (!Job->bBrickResident)
			{
				// The pack-and-discard measurement arm keeps the readback path:
				// it publishes nothing, so it cannot conflict with the armed
				// allocator, and its whole point is the producer's cost.
				Pool.NoteGpuAllocFallback(FVoxelBrickPool::EGpuAllocFallback::Discard);
				continue;
			}
			FVoxelBrickPool::FResidentChunk Shell;
			if (Pool.AllocateGpuChunkShell(Job->BrickKey, Job->BrickOriginVoxel, Shell))
			{
				Job->bGpuPoolAlloc = true;
				Job->bGpuShellAllocated = true;
				Job->GpuChunkSlot = Shell.ChunkSlot;
				Job->GpuBrickBase = Shell.BrickBase;
			}
			else
			{
				// Counted inside AllocateGpuChunkShell. The job keeps its mesh
				// half and simply has no resident volume -- a missing chunk,
				// never a corrupted one. It must NOT fall back to the readback
				// path: publication there allocates from the CPU arenas the
				// armed pool has retired.
				Job->bBrickPack = false;
			}
		}
		// Rule 2: drop the brick half of any job whose shell was stolen by a
		// later allocation's eviction inside this very loop.
		// The two deltas, KEPT rather than discarded once compared. Their
		// difference is the exact number of chunks that LEFT the resident map
		// while this batch's shells were being taken -- see FJobCostWindow's
		// PoolReplaced for why that count is the most valuable number in this
		// file, and for what it means on a leg reporting evictions=0.
		const int64 ShellsAdded = Pool.GetChunksAdded() - AddedBefore;
		const int64 ResidentGrew = int64(Pool.GetNumResidentChunks() - ResidentBefore);
		JobCost.ShellsTaken += ShellsAdded;
		JobCost.PoolReplaced += (ShellsAdded - ResidentGrew);
		const bool bSkipReval = bLeanReval && (ResidentGrew == ShellsAdded);
		if (bSkipReval)
		{
			JobCost.LeanRevalSkipped += int64(Batch.Num());
		}
		else
		{
			JobCost.LeanRevalRan += int64(Batch.Num());
		}
		for (const FJobPtr& Job : Batch)
		{
			if (bSkipReval)
			{
				break;
			}
			if (!Job->bGpuPoolAlloc)
			{
				continue;
			}
			FVoxelBrickPool::FResidentChunk Current;
			if (!Pool.DebugGetResidentChunk(Job->BrickKey, Current) ||
			    !Current.bGpuArenas || Current.ChunkSlot != Job->GpuChunkSlot)
			{
				Job->bGpuPoolAlloc = false;
				Job->bGpuShellAllocated = false;
				Job->bBrickPack = false;
				// STOLEN, not refused: this shell WAS allocated (and counted
				// into `shells`) before a later allocation's eviction took its
				// slot, and its claim never runs -- the distinction the
				// shells-vs-claims reconciliation needs (see the enum).
				Pool.NoteGpuAllocFallback(FVoxelBrickPool::EGpuAllocFallback::ShellStolen);
			}
		}
		// Rule 3: frees before claims, always.
		Pool.FlushPendingGpuFrees();
	}

	// --- P3 spine: records + the passes-per-tick tally ----------------------
	//
	// HERE, not in Tick, because both need what the P1 shell loop just
	// decided: ChunkSlot is only real after AllocateGpuChunkShell, and
	// bGpuPoolAlloc/bBrickPack after the rule-2 revalidation. Game thread,
	// before the enqueue -- the render command below knows nothing about it.
	if (VoxelGpuWorklistEnabled() && Worklist.IsInitialized())
	{
		TArray<FVoxelGpuChunkWorkRecord> NewRecords;
		NewRecords.Reserve(Batch.Num());
		// Parallel to NewRecords: which job each record serves, so the Column
		// stage can hand the job its arena slice after the flush.
		TArray<FJobPtr> RecordJobs;
		RecordJobs.Reserve(Batch.Num());
		// Parallel to NewRecords when the AssetStamp stage is armed: record
		// i's resolved-instance payload (empty for asset-free records).
		// Flush stages a payload into the per-flush blob only if the record
		// is consumed that same flush; otherwise it is dropped and the job
		// below falls back classic like any deferred record.
		const bool bStampArmed = Worklist.IsAssetStampStageArmed();
		TArray<FVoxelWorklistAssetPayload> RecordPayloads;
		TSet<FVoxelGpuBrickStack*> TalliedStacks;
		int64 PassesThisTick = 0;
		const bool bLeanArmed = VoxelGpuLeanBrickJobsEnabled();
		for (const FJobPtr& Job : Batch)
		{
			// The tally first: every promoted job costs passes, record or not.
			// Constants are DERIVED FROM THE CODE SHAPE (their rule, stated at
			// their definitions); the graph below is what they describe.
			if (Job->BrickStack.IsValid())
			{
				if (!TalliedStacks.Contains(Job->BrickStack.Get()))
				{
					TalliedStacks.Add(Job->BrickStack.Get());
					PassesThisTick += Job->BrickStack->bClaimBased
						? VoxelGpuBatchDetail::kStackClaimBasePasses
						  + VoxelGpuBatchDetail::kPassesPerStackMemberClaim * Job->BrickStack->NumChunks
						: VoxelGpuBatchDetail::kPassesPerStackDispatch;
				}
			}
			else
			{
				const bool bLean = bLeanArmed && Job->bBrickPack && !Job->bQuadMesh
					&& Job->Region.BandEdge == 0;
				if (bLean)
				{
					PassesThisTick += Job->bGpuPoolAlloc
						? VoxelGpuBatchDetail::kPassesPerLeanAllocJob
						: VoxelGpuBatchDetail::kPassesPerLeanReadbackJob;
				}
				else if (Job->bBrickPack)
				{
					PassesThisTick += Job->bGpuPoolAlloc
						? VoxelGpuBatchDetail::kPassesPerClassicAllocJob
						: VoxelGpuBatchDetail::kPassesPerClassicBrickJob;
				}
				else
				{
					PassesThisTick += VoxelGpuBatchDetail::kPassesPerClassicQuadJob;
				}
			}

			// The record, for the chunks the converted chain will serve: brick
			// half live, no quad chain, no band readback, GPU-allocated slot,
			// and a request the atlas empties of its raster (a record built
			// from an inline-window request would be un-runnable by the chain
			// it exists for). First failing reason counted, [gpu-lean] style.
			if (!Job->bBrickPack)                     { ++WorklistSkipNoPack; continue; }
			if (Job->bQuadMesh)                       { ++WorklistSkipQuadMesh; continue; }
			if (Job->Region.BandEdge != 0)            { ++WorklistSkipBand; continue; }
			if (!Job->bGpuPoolAlloc)                  { ++WorklistSkipNoAlloc; continue; }
			if (!Job->BrickRegion.bRasterAtlas)       { ++WorklistSkipNoAtlas; continue; }

			FVoxelGpuChunkWorkRecord R;
			R.OriginVx = Job->BrickRegion.OriginVx;
			R.OriginVy = Job->BrickRegion.OriginVy;
			R.BrickZMin = Job->BrickRegion.BrickZMin;
			R.LevelFlags = (uint32(FMath::Clamp(Job->BrickRegion.CoarseLevel, 0, 15)) & 0xFu)
			             | ((Job->BrickRegion.RingSkirtMask & 0xFu) << 4)
			             | ((Job->BrickRegion.AssetInstances.Num() > 0 ? 1u : 0u) << 8);
			R.ChunkSlot = Job->GpuChunkSlot;
			// Low 32 bits of the manager's JobId (starts at 1, so 0 is the
			// prover's malformed-record tripwire until 2^32 jobs have run).
			R.GenId = uint32(Job->JobId);
			// No flush-level asset buffer exists yet -- that is conversion
			// work (the order-preserving AssetStamp gather needs it). Base 0
			// with a TRUTHFUL count, so the day the buffer lands, records
			// with AssetCount > 0 are already flowing to size it against.
			R.AssetBase = 0;
			R.AssetCount = uint32(Job->BrickRegion.AssetInstances.Num());
			// The shell's descriptor-block base: the Claim stage's one
			// CPU-dispensed input. Truthful on every record (the AssetCount
			// precedent -- flow the data before the consumer).
			R.BrickBase = Job->GpuBrickBase;
			Job->BrickShading.Pack(R.ShadingClimatePacked, R.ShadingGradPacked,
			                       R.ShadingSurfaceZBits);
			NewRecords.Add(R);
			RecordJobs.Add(Job);
			if (bStampArmed)
			{
				FVoxelWorklistAssetPayload& P = RecordPayloads.AddDefaulted_GetRef();
				const FVoxelGpuRegionRequest& Reg = Job->BrickRegion;
				if (Reg.AssetInstances.Num() > 0)
				{
					P.Instances.Reserve(Reg.AssetInstances.Num());
					for (const FVoxelGpuRegionRequest::FAssetInstance& Inst : Reg.AssetInstances)
					{
						FVoxelWorklistAssetInstance& W = P.Instances.AddDefaulted_GetRef();
						W.AnchorRelVx = Inst.AnchorRelVx;
						W.AnchorRelVy = Inst.AnchorRelVy;
						W.AnchorVz = Inst.AnchorVz;
						W.GridOriginZ = Inst.GridOriginZ;
						W.RotOriginX = Inst.RotOriginX;
						W.RotOriginY = Inst.RotOriginY;
						W.YawQuarter = Inst.YawQuarter;
						W.SizeX = Inst.SizeX;
						W.SizeY = Inst.SizeY;
						W.SizeZ = Inst.SizeZ;
						// Payload-relative for now; Flush rebases into the
						// flush blob as it concatenates.
						W.ColStartsBase = Inst.ColStartsBase;
					}
					P.ColStarts = Reg.AssetColStarts;
					P.Spans = Reg.AssetSpans;
				}
			}
		}
		const bool bColumnsArmed = VoxelGpuWorklistColumnsEnabled();
		// The spine's own constant (args + prover + armed Column dispatch) is
		// NOT tallied here: it is paid every armed tick, batch or no batch,
		// so Tick owns it -- tallying it here was half of the dead-counter
		// bug (batchless ticks tallied 0 while dispatching 2-3 passes).
		TArray<uint32> RecordMono;
		if (NewRecords.Num() > 0)
		{
			// Refusals (ring full) are counted inside Append; the chunk is
			// unharmed either way -- the classic path below is still what
			// generates it. Nothing retries: a refused record's chunk simply
			// never enters the ring, and RefusedFull growing is the
			// back-pressure reading the capacity latch exists to surface.
			Worklist.Append(NewRecords, bColumnsArmed ? &RecordMono : nullptr,
			                bStampArmed ? &RecordPayloads : nullptr);
		}

		// --- Column stage: flush HERE, before the batch render command ------
		//
		// Render commands execute in enqueue order, so flushing now puts the
		// upload + args pass + indirect Column dispatch AHEAD of the batch
		// graph whose VoxelizeMain will read the arena. With the stage off
		// the flush stays in Tick (after the batch), exactly where the spine
		// was measured -- arm B of the spine A/B does not move.
		if (bColumnsArmed)
		{
			if (RecordJobs.Num() > 0)
			{
				// Process-wide generation inputs, from any eligible job (all
				// jobs share the seed, the pitch and the process atlas; the
				// eligibility gate above required bRasterAtlas).
				const FVoxelGpuRegionRequest& AnyRegion = RecordJobs[0]->BrickRegion;
				Worklist.SetColumnStageInputs(AnyRegion.RasterAtlas, AnyRegion.Seed,
				                              AnyRegion.PixelSizeMm);
			}
			Worklist.Flush(VoxelGpuWorklistBudget());
			bWorklistFlushedThisTick = true;

			// Hand each record's job its arena slice -- or count WHY not.
			// THE FAILING READINGS: fallback growing while converted stays 0
			// means the stage is armed and converting nothing (ring refusing,
			// or records deferred past the budget every tick); both stuck at
			// 0 with records flowing means this mapping itself is broken.
			const FVoxelGpuWorklist::FLastFlush LF = Worklist.GetLastFlush();
			for (int32 RIdx = 0; RIdx < RecordJobs.Num(); ++RIdx)
			{
				// A stack-fused member's region is dispatched through
				// AddBrickStackPasses, which takes no column feed -- its
				// arena slice would go unread while the tally claimed the
				// ColumnMain saving. Fallback, counted, until the stack path
				// learns the feed.
				if (RecordJobs[RIdx]->BrickStack.IsValid())
				{
					++WorklistColFallback;
					if (Worklist.IsVoxelizeStageArmed())
					{
						++WorklistVoxFallback;
					}
					if (Worklist.IsClassifyStageArmed())
					{
						++WorklistCtFallback;
					}
					continue;
				}
				const uint32 Mono = RecordMono.IsValidIndex(RIdx) ? RecordMono[RIdx] : MAX_uint32;
				const bool bConsumedThisFlush =
					Mono != MAX_uint32 && (Mono - LF.ConsumeFirst) < LF.Take;
				if (bConsumedThisFlush && Worklist.IsColumnStageArmed())
				{
					RecordJobs[RIdx]->WorklistColumnSlice = Mono - LF.ConsumeFirst;
					++WorklistColConverted;
					// The converted job's region graph drops its ColumnMain
					// pass (-1); the verify arm puts it back as the byte
					// reference and adds the compare (+2, net +1).
					PassesThisTick += VoxelGpuWorklistVerifyColsEnabled() ? 1 : -1;
					// P3 stage 2: the same record's CELLS, when the Voxelize
					// stage is armed and the chunk carries no assets.
					// AssetStamp writes cells between Voxelize and the brick
					// chain, and stamping into the shared arena would put UAV
					// barriers between every other chunk's reads of it -- so
					// asset chunks keep their classic Voxelize + AssetStamp
					// (the kernel's group-uniform hasAssets early-out mirrors
					// this exactly), counted apart from deferred records
					// because the two mean different things: fbAssets is a
					// designed exclusion, fb is the ring falling behind.
					if (Worklist.IsVoxelizeStageArmed())
					{
						const bool bJobAssets =
							RecordJobs[RIdx]->BrickRegion.AssetInstances.Num() > 0;
						if (!bJobAssets || bStampArmed)
						{
							RecordJobs[RIdx]->bWorklistCellsFed = true;
							++WorklistVoxConverted;
							if (bJobAssets)
							{
								// The stamp stage's own conversion count. Its
								// classic per-instance stamp passes also
								// disappear, but they were never in the pass
								// tally (the per-job constants predate
								// assets), so the tally's -1 below
								// UNDERSTATES the real win for these chunks
								// -- stated rather than silently wrong.
								++WorklistStampConverted;
							}
							// Same shape as the column term: drops its
							// VoxelizeMain (-1); the active verify arm puts
							// it back and adds the compare (net +1).
							PassesThisTick += (bJobAssets
								? VoxelGpuWorklistVerifyStampEnabled()
								: VoxelGpuWorklistVerifyVoxEnabled()) ? 1 : -1;
							// P3 stage 3: the fused ClassifyTotals rides the
							// cell feed. Drops classify + both 3-pass scans +
							// totals (-8, the conversion's largest cut);
							// verify puts all eight back and adds the compare
							// (net +1).
							if (Worklist.IsClassifyStageArmed())
							{
								RecordJobs[RIdx]->bWorklistTotalsFed = true;
								++WorklistCtConverted;
								PassesThisTick += VoxelGpuWorklistVerifyCtEnabled() ? 1 : -8;
								// P3 stage 5: the Pack rides the totals feed.
								// Drops the mask clear + BrickPackMain (-2);
								// verify puts both back and adds the compare
								// (net +1).
								if (Worklist.IsPackStageArmed())
								{
									RecordJobs[RIdx]->bWorklistPackFed = true;
									++WorklistPackConverted;
									PassesThisTick += VoxelGpuWorklistVerifyPackEnabled() ? 1 : -2;
									// Stage 6: the flush graph claims and
									// lands this chunk's pool state; the
									// batch graph adds NO brick passes for
									// it. The claim verify is a per-TICK
									// indirect dispatch, tallied with the
									// spine constant, not here.
									if (Worklist.IsClaimStageArmed())
									{
										RecordJobs[RIdx]->bWorklistClaimFed = true;
										++WorklistClaimConverted;
										PassesThisTick += -5;
									}
								}
							}
						}
						else
						{
							// Stamp stage NOT armed: the designed exclusion.
							++WorklistVoxFallbackAssets;
							if (Worklist.IsClassifyStageArmed())
							{
								++WorklistCtFallbackAssets;
							}
						}
					}
				}
				else
				{
					++WorklistColFallback;
					if (Worklist.IsVoxelizeStageArmed())
					{
						++WorklistVoxFallback;
					}
					if (Worklist.IsClassifyStageArmed())
					{
						++WorklistCtFallback;
					}
				}
			}
		}
		// Handed to Tick, which adds the spine constant and folds the total
		// into the window -- see the fold there for why the split exists.
		WorklistBatchPassesThisTick += PassesThisTick;
		WorklistWinChunks += Batch.Num();
	}

	// ONE graph for the whole batch. Every job's seven passes plus its three
	// readback copies go into the same FRDGBuilder, which is executed once.
	//
	// WorklistPtr (P3 Column stage): raw pointer to the member ring so the
	// graph can register the column arena the flush command just filled.
	// Same lifetime standing as FVoxelGpuWorklist::Flush's own `this`
	// capture: by the time the manager can be destroyed the game thread has
	// stopped enqueuing, and destruction drains the render queue behind any
	// command still holding this pointer.
	FVoxelGpuWorklist* const WorklistPtr =
		(VoxelGpuWorklistColumnsEnabled() && VoxelGpuWorklistEnabled()
		 && Worklist.IsInitialized()) ? &Worklist : nullptr;
	const bool bVerifyColsArmed = VoxelGpuWorklistVerifyColsEnabled();
	const bool bVerifyVoxArmed = VoxelGpuWorklistVerifyVoxEnabled();
	const bool bVerifyCtArmed = VoxelGpuWorklistVerifyCtEnabled();
	const bool bVerifyStampArmed = VoxelGpuWorklistVerifyStampEnabled();
	const bool bVerifyPackArmed = VoxelGpuWorklistVerifyPackEnabled();
	// TIGHT bracket, around the HANDOFF ONLY. TickStageMs.EnqueueMs spans this
	// whole function (shell loop, worklist records, enqueue) and so cannot
	// answer the one question that decides this investigation: is the game
	// thread WAITING here? Constructing this command is a MoveTemp of an array
	// of shared pointers and five bools -- nanoseconds -- so anything this
	// bracket reports above that is the render thread refusing to take it.
	const double EnqDispatchStart = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshDispatchBatch)(
		[Jobs = MoveTemp(Batch), WorklistPtr, bVerifyColsArmed,
		 bVerifyVoxArmed, bVerifyCtArmed, bVerifyStampArmed,
		 bVerifyPackArmed](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// P3 Column stage: the arena and stats buffers, registered once for
		// the whole batch. Arena null (stage never dispatched) makes every
		// job below fall back to its classic ColumnMain -- counted, loudly,
		// never silent.
		FVoxelGpuWorklist::FColumnStageBindings WorklistCols;
		if (WorklistPtr != nullptr)
		{
			WorklistCols = WorklistPtr->RegisterColumnStage(GraphBuilder);
		}

		// --- P1: the pool's arenas, registered ONCE for every claiming job ---
		//
		// Lazily, on the first job that claims: an unarmed batch must add no
		// pass, register no buffer and differ in no byte. EnsureCreated runs
		// here because this graph can be the pool's very first GPU touch --
		// the flush that used to create the buffers may never have run yet.
		// The layout read is safe cross-thread: written once at Init (game
		// thread), before any job could have been armed, immutable after.
		VoxelGpuWorldGen::FBrickPoolAllocBuffers PoolAllocBufs;
		FVoxelBrickPoolAllocLayout PoolAllocLayout;
		bool bPoolAllocBound = false;
		const auto BindPoolAlloc = [&]() -> bool
		{
			if (bPoolAllocBound)
			{
				return PoolAllocBufs.IsValid();
			}
			bPoolAllocBound = true;
			FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
			const FVoxelBrickPoolBuffersRef Buffers = Pool.GetBuffers();
			FVoxelBrickPool::EnsureCreated_RenderThread(RHICmdList, Buffers);
			if (!Buffers.IsValid() || !Buffers->IsValid() || !Buffers->HasGpuAlloc())
			{
				return false;
			}
			PoolAllocLayout = Pool.GetGpuAllocLayout();
			PoolAllocBufs.PoolDesc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
			PoolAllocBufs.PoolOcc = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
			PoolAllocBufs.PoolMat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
			PoolAllocBufs.PoolTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled, TEXT("VoxelBrickPool.ChunkTable"));
			PoolAllocBufs.AllocState = GraphBuilder.RegisterExternalBuffer(Buffers->AllocStatePooled, TEXT("VoxelBrickPool.AllocState"));
			PoolAllocBufs.AllocBitmap = GraphBuilder.RegisterExternalBuffer(Buffers->AllocBitmapPooled, TEXT("VoxelBrickPool.AllocBitmap"));
			PoolAllocBufs.AllocSide = GraphBuilder.RegisterExternalBuffer(Buffers->AllocSidePooled, TEXT("VoxelBrickPool.AllocSide"));
			return PoolAllocBufs.IsValid();
		};

		// Jobs that made it into the graph. Anything that falls out below is
		// terminated here and now with a Failed state, so no job can leave this
		// command still sitting in Queued.
		TArray<FJobPtr> Built;
		Built.Reserve(Jobs.Num());

		// --- Tier B.1: fused stacks first, one region graph per stack -------
		//
		// Members are grouped back together by their shared stack object (the
		// batch array carries them interleaved with classic jobs in promote
		// order). Raw-pointer key: the members' shared handles keep the stack
		// alive for the map's whole lifetime, and the map dies with this
		// command.
		{
			TMap<FVoxelGpuBrickStack*, TArray<FJobPtr>> StackGroups;
			for (const FJobPtr& Job : Jobs)
			{
				if (Job->BrickStack.IsValid())
				{
					StackGroups.FindOrAdd(Job->BrickStack.Get()).Add(Job);
				}
			}
			for (TPair<FVoxelGpuBrickStack*, TArray<FJobPtr>>& Group : StackGroups)
			{
				AddBrickStackPasses(GraphBuilder, Group.Value, Built,
				                    BindPoolAlloc, PoolAllocBufs, PoolAllocLayout);
			}
		}

		for (const FJobPtr& Job : Jobs)
		{
			// Stack members were dispatched above, as one fused region.
			if (Job->BrickStack.IsValid())
			{
				continue;
			}
			if (Job->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}

			// --- LEAN BRICK-ONLY JOBS (-VoxelGpuLeanBrickJobs) ----------------
			//
			// The mesh-region graph below exists, on a brick-only job, for ONE
			// consumer: the band readback -- and only when this job asked for a
			// band. A brick-only, band-free, packing job reads nothing from it,
			// so armed, it is skipped whole: 2,304 columns + 110,592-cell
			// voxelize (48x48x6 with halo) that the 1,024-column/32,768-cell
			// brick region below repeats anyway -- 3.4x the generation work --
			// and, under voxel.GPU.PoolAlloc, the job's LAST readback, i.e. the
			// difference between the poll-quantised path (dispatchToReady
			// 70.1 ms mean, p1p2-armed) and deliverable-at-enqueue.
			//
			// The decision is per JOB, not per batch: a mixed batch (a level-0
			// cold-band job beside coarse ones) leans the jobs that can and
			// counts the ones that cannot, by reason. Byte-identical with the
			// switch off.
			const bool bLeanJob = VoxelGpuLeanBrickJobsEnabled()
				&& !Job->bQuadMesh
				&& Job->Region.BandEdge == 0
				&& Job->bBrickPack;
			if (bLeanJob)
			{
				VoxelGpuLeanDetail::GLeanJobs.fetch_add(1, std::memory_order_relaxed);
			}
			else if (VoxelGpuLeanBrickJobsEnabled())
			{
				// Counted BY REASON, so "armed but lean=0" reads as a diagnosis
				// instead of a mystery: all-quads means RetireQuads is off,
				// all-band means -VoxelGpuBandColdOnly is not armed (or the
				// band cache never warms), all-noPack means BrickPack is off.
				if (Job->bQuadMesh)
				{
					VoxelGpuLeanDetail::GFullQuadJobs.fetch_add(1, std::memory_order_relaxed);
				}
				else if (Job->Region.BandEdge > 0)
				{
					VoxelGpuLeanDetail::GFullBandJobs.fetch_add(1, std::memory_order_relaxed);
				}
				else
				{
					VoxelGpuLeanDetail::GFullNoPackJobs.fetch_add(1, std::memory_order_relaxed);
				}
			}

			if (!bLeanJob)
			{
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
			} // !bLeanJob -- the mesh-region graph and its three consumers

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

				// P3 Column stage: this chunk's columns are already in the
				// worklist arena (the flush command ran first) -- feed the
				// region graph its slice so it skips its own ColumnMain. A
				// job with a slice but NO arena is the render-side failure
				// (the stage armed and the flush never created it): fall back
				// classic and COUNT it -- silence here would be the eighth
				// feature found running while doing nothing.
				VoxelGpuWorldGen::FWorklistColumnFeed ColumnFeed;
				const bool bFeedColumns =
					Job->WorklistColumnSlice != MAX_uint32 && WorklistCols.Arena != nullptr;
				if (bFeedColumns)
				{
					ColumnFeed.Arena = WorklistCols.Arena;
					ColumnFeed.SliceIndex = Job->WorklistColumnSlice;
					ColumnFeed.bVerify = bVerifyColsArmed && WorklistCols.Stats != nullptr;
					ColumnFeed.VerifyStats = WorklistCols.Stats;
					// P3 stage 2: the cell feed rides the column feed (a
					// cell-fed job is by construction column-fed). A job
					// FLAGGED cell-fed with no cell arena is the same
					// render-side failure as the column case: fall back to
					// classic VoxelizeMain and COUNT it.
					if (Job->bWorklistCellsFed)
					{
						if (WorklistCols.CellArena != nullptr)
						{
							ColumnFeed.CellArena = WorklistCols.CellArena;
							ColumnFeed.bVerifyVox =
								bVerifyVoxArmed && WorklistCols.Stats != nullptr;
							// A cell-fed job with assets is only ever flagged
							// when the stamp stage staged them (the set site
							// requires the stage armed, and Flush stages
							// exactly the records it consumes) -- the region
							// graph's checkf holds by that construction.
							if (Job->BrickRegion.AssetInstances.Num() > 0)
							{
								ColumnFeed.bCellsIncludeAssets = true;
								ColumnFeed.bVerifyStamp =
									bVerifyStampArmed && WorklistCols.Stats != nullptr;
							}
						}
						else
						{
							VoxelGpuBatchDetail::GWorklistVoxArenaMissing.fetch_add(
								1, std::memory_order_relaxed);
						}
					}
					// The totals feed rides the CELL feed: without the cell
					// arena the fused stage classified nothing for this
					// chunk, so a missing cell arena voids both.
					if (Job->bWorklistTotalsFed)
					{
						if (ColumnFeed.CellArena != nullptr &&
						    WorklistCols.OccOffsetsArena != nullptr &&
						    WorklistCols.MatOffsetsArena != nullptr &&
						    WorklistCols.TotalsArena != nullptr)
						{
							ColumnFeed.OccOffsetsArena = WorklistCols.OccOffsetsArena;
							ColumnFeed.MatOffsetsArena = WorklistCols.MatOffsetsArena;
							ColumnFeed.TotalsArena = WorklistCols.TotalsArena;
							ColumnFeed.bVerifyCt =
								bVerifyCtArmed && WorklistCols.Stats != nullptr;
							// P3 stage 5: the pack feed rides the totals feed.
							if (Job->bWorklistPackFed)
							{
								if (WorklistCols.PackDescArena != nullptr &&
								    WorklistCols.PackOccArena != nullptr &&
								    WorklistCols.PackMatArena != nullptr &&
								    WorklistCols.PackMaskArena != nullptr)
								{
									ColumnFeed.PackDescArena = WorklistCols.PackDescArena;
									ColumnFeed.PackOccArena = WorklistCols.PackOccArena;
									ColumnFeed.PackMatArena = WorklistCols.PackMatArena;
									ColumnFeed.PackMaskArena = WorklistCols.PackMaskArena;
									ColumnFeed.bVerifyPack =
										bVerifyPackArmed && WorklistCols.Stats != nullptr;
								}
								else
								{
									VoxelGpuBatchDetail::GWorklistPackArenaMissing.fetch_add(
										1, std::memory_order_relaxed);
								}
							}
						}
						else
						{
							VoxelGpuBatchDetail::GWorklistCtArenaMissing.fetch_add(
								1, std::memory_order_relaxed);
							if (Job->bWorklistPackFed)
							{
								VoxelGpuBatchDetail::GWorklistPackArenaMissing.fetch_add(
									1, std::memory_order_relaxed);
							}
						}
					}
				}
				else if (Job->WorklistColumnSlice != MAX_uint32)
				{
					VoxelGpuBatchDetail::GWorklistColArenaMissing.fetch_add(
						1, std::memory_order_relaxed);
					if (Job->bWorklistCellsFed)
					{
						VoxelGpuBatchDetail::GWorklistVoxArenaMissing.fetch_add(
							1, std::memory_order_relaxed);
					}
				}

				const VoxelGpuWorldGen::FRegionGraphResources BrickGraph =
					VoxelGpuWorldGen::AddRegionPasses(GraphBuilder, Job->BrickRegion,
					                                  bFeedColumns ? &ColumnFeed : nullptr);

				if (BrickGraph.BrickDesc == nullptr || BrickGraph.BrickOcc == nullptr ||
				    BrickGraph.BrickMat == nullptr || BrickGraph.BrickChunkMask == nullptr ||
				    BrickGraph.BrickTotals == nullptr)
				{
					UE_LOG(LogTemp, Error,
					       TEXT("Job %llu asked for a brick pack and the graph produced no brick buffers. ")
					       TEXT("The mesh half of this job is unaffected."), Job->JobId);
					Job->bBrickPack = false;
				}
				else if (Job->bGpuPoolAlloc)
				{
					// --- P1: claim + write IN THIS GRAPH, read back NOTHING --
					//
					// The claim pass consumes BrickTotals where the scan wrote
					// them; the write passes land words, descriptors and the
					// record at the claimed ranges. No payload survives this
					// graph (the scratch dies with it, as it should), no
					// BrickTotalReadback exists, and for a brick-only job with
					// no band the whole job now has NO readback: it is
					// deliverable the moment this command is enqueued. That is
					// the fence this phase exists to kill.
					//
					// If binding fails (allocator buffers could not be
					// created), the chunk keeps its shell and lands nothing --
					// the record is zero, the verify counts it unwritten, and
					// the window line says so. Loud, not fatal: the mesh half
					// is unaffected, exactly like the null-buffer branch above.
					//
					// STAGE 6 (bWorklistClaimFed): the claim AND all four
					// writes already ran in the FLUSH graph, whose render
					// command executed before this one -- this job adds ZERO
					// brick passes here. Adding the classic claim anyway
					// would DOUBLE-CLAIM the slot: the second claim's side
					// entry overwrites the first's and the first ranges leak
					// forever. Never fall back classically for a claim-fed
					// job -- the flush side owns it, success or (counted,
					// loud) failure.
					if (Job->bWorklistClaimFed)
					{
						// Nothing to add. Delivery keeps the lean-alloc
						// shape; the record landed on the GPU timeline
						// before this command's own passes.
					}
					else if (BindPoolAlloc())
					{
						FRDGBufferRef Claim = VoxelGpuWorldGen::AddBrickPoolClaimPass(
							GraphBuilder, PoolAllocBufs, PoolAllocLayout,
							BrickGraph.BrickTotals, Job->GpuChunkSlot,
							BrickGraph.Sizes.BrickOccWordsMax, BrickGraph.Sizes.BrickMatWordsMax,
							/*TotalsChunkIndexPlusOne*/ 0, /*TotalsNumChunks*/ 0,
							BrickGraph.BrickTotalsReadBase,
							BrickGraph.BrickOccSrcBase, BrickGraph.BrickMatSrcBase);
						VoxelGpuWorldGen::AddBrickPoolAllocWritePasses(
							GraphBuilder, PoolAllocBufs, Claim,
							BrickGraph.BrickOcc, BrickGraph.BrickMat,
							BrickGraph.BrickDesc, BrickGraph.BrickChunkMask,
							BrickGraph.Sizes.NumBricks, Job->GpuChunkSlot, Job->GpuBrickBase,
							uint32(FMath::Clamp(Job->BrickKey.Level, 0, 15)),
							Job->BrickOriginVoxel, Job->BrickShading,
							BrickGraph.Sizes.BrickOccWordsMax, BrickGraph.Sizes.BrickMatWordsMax,
							// Both ELEMENT bases -- desc a BRICK index, mask a
							// DWORD index (SliceIndex * 2, exactly as the
							// stack path passes C * 2). This used to divide
							// the mask base by 2 on a claim the kernel never
							// multiplied back, so every slice > 0 record read
							// its L1 masks from the wrong elements -- fixed
							// 2026-08-23 (stage-6 audit), with the two word
							// bases below closing the desc-rebase and
							// allSolid halves of the same seam.
							BrickGraph.BrickDescReadBase,
							BrickGraph.ChunkMaskReadBase,
							BrickGraph.BrickOccSrcBase,
							BrickGraph.BrickMatSrcBase);
					}
					else
					{
						UE_LOG(LogTemp, Error,
						       TEXT("Job %llu: voxel.GPU.PoolAlloc armed but the allocator buffers are ")
						       TEXT("unavailable; the chunk has a shell and no volume."), Job->JobId);
					}
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

	const double DispatchBatchEnd = FPlatformTime::Seconds();
	TickStageMs.EnqueueMs += (DispatchBatchEnd - DispatchBatchStart) * 1000.0;
	JobCost.EnqDispatchMs += (DispatchBatchEnd - EnqDispatchStart) * 1000.0;
	++JobCost.EnqDispatchN;
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
	//
	// THE THREE WALKS ARE COUNTED, NOT ASSUMED. This function walks InFlight
	// three times per tick and InFlight is bounded by MaxInFlight (288 under
	// -VoxelGpuPrimary: JobsInFlightPerCore 8 x 36 logical cores). That is a
	// per-TICK cost, and pollJobs/ticks is what says whether it is the one the
	// budget line has been attributing per chunk.
	JobCost.PollJobsVisited += int64(InFlight.Num()) * 3;

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
		const int32 HarvestCap = VoxelGpuMeshHarvestCapEffective();

		// Same tight-bracket reason as the batch command: this handoff moves one
		// array and copies one int.
		const double EnqPollStart = FPlatformTime::Seconds();
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

				// --- Tier B.1, phase 1 for a fused stack member ---------------
				//
				// The K members share ONE readback; the first member polled
				// after it lands harvests the whole (2+2K)-dword table (one
				// budget charge for the stack -- the amortisation shows up in
				// the harvest too), cross-checks it, and every member then
				// fills its own pair plus its prefix starts from the shared
				// table. All render-thread, serial across poll commands, so
				// the stack's plain fields need no atomics.
				// STACK-CLAIM: a claim-based member has no stack readback to
				// harvest -- its totals were consumed by its claim pass in the
				// stack's own graph -- so it takes the classic no-readback
				// phase 1 below (all its bNeed* terms are false) instead of
				// this branch, which would fail it for the readback's absence.
				if (State == EJobState::Dispatched && Job->BrickStack.IsValid() &&
				    !Job->BrickStack->bClaimBased)
				{
					FVoxelGpuBrickStack& Stack = *Job->BrickStack;
					if (Stack.bFailed)
					{
						Job->Error = Stack.Error;
						Job->SetState(EJobState::Failed);
						continue;
					}
					if (!Stack.TotalsReadback.IsValid())
					{
						Job->Error = TEXT("stack totals readback missing");
						Job->SetState(EJobState::Failed);
						continue;
					}
					if (!Stack.bHarvested && !Stack.TotalsReadback->IsReady())
					{
						continue;
					}
					// First observed ready, before the budget check -- the
					// classic path's stamping rule, verbatim.
					if (Job->ReadySeconds <= 0.0)
					{
						Job->ReadySeconds = FPlatformTime::Seconds();
					}
					if (!Stack.bHarvested)
					{
						if (HarvestBudget <= 0)
						{
							continue;
						}
						--HarvestBudget;

						const uint32 Dwords = 2u + 2u * uint32(Stack.NumChunks);
						Stack.Totals.SetNumUninitialized(int32(Dwords));
						FString Error;
						if (!CopyReadback(*Stack.TotalsReadback, Stack.Totals.GetData(),
						                  Dwords * sizeof(uint32), TEXT("BrickStackTotals"), Error))
						{
							Stack.bFailed = true;
							Stack.Error = Error;
							Job->Error = Error;
							Job->SetState(EJobState::Failed);
							continue;
						}

						// THE LIVE CROSS-CHECK. sum(per-chunk) must equal the
						// region pair the same kernel derived the way
						// BrickTotalMain does. A mismatch means the per-chunk
						// split is wrong -- the chunk-major assumption, the
						// kernel, or the scan -- and publishing from a wrong
						// split writes one chunk's bricks into another chunk's
						// pool range: plausible terrain, wrong place. So the
						// WHOLE stack fails loudly and the streaming path
						// re-produces the chunks on its CPU arm.
						uint64 SumOcc = 0;
						uint64 SumMat = 0;
						for (int32 C = 0; C < Stack.NumChunks; ++C)
						{
							SumOcc += Stack.Totals[2 + 2 * C];
							SumMat += Stack.Totals[3 + 2 * C];
						}
						if (SumOcc != uint64(Stack.Totals[0]) || SumMat != uint64(Stack.Totals[1]))
						{
							Stack.bFailed = true;
							Stack.Error = FString::Printf(
								TEXT("stack totals cross-check FAILED: per-chunk sums (occ %llu, ")
								TEXT("mat %llu) != region totals (%u, %u) over %d chunks"),
								SumOcc, SumMat, Stack.Totals[0], Stack.Totals[1], Stack.NumChunks);
							UE_LOG(LogVoxelGpuMeshJob, Error, TEXT("[gpu-batch] %s"), *Stack.Error);
							VoxelGpuBatchDetail::GCrosscheckFail.fetch_add(1, std::memory_order_relaxed);
							Job->Error = Stack.Error;
							Job->SetState(EJobState::Failed);
							continue;
						}
						VoxelGpuBatchDetail::GCrosscheckPass.fetch_add(1, std::memory_order_relaxed);
						Stack.bHarvested = true;
					}

					// This member's own numbers. The prefix sums are what turn
					// "chunk c of the batch" into "this contiguous scratch run"
					// -- the pool's word copies start there, and the desc-write
					// base subtracts it back out (see AddFlushPasses).
					const int32 Idx = Job->StackChunkIndex;
					uint32 OccFirst = 0;
					uint32 MatFirst = 0;
					for (int32 C = 0; C < Idx; ++C)
					{
						OccFirst += Stack.Totals[2 + 2 * C];
						MatFirst += Stack.Totals[3 + 2 * C];
					}
					Job->BrickTotals[0] = Stack.Totals[2 + 2 * Idx];
					Job->BrickTotals[1] = Stack.Totals[3 + 2 * Idx];
					if (Job->BrickPayload.IsValid())
					{
						Job->BrickPayload->SrcOccFirst = OccFirst;
						Job->BrickPayload->SrcMatFirst = MatFirst;
					}
					// Brick-only by construction, so NumQuads stays 0 and the
					// phase-2 starter's zero-quad branch carries the job to
					// ReadbackDone -- the same road every brick-only job takes.
					Job->SetState(EJobState::TotalDone);
					continue;
				}

				// --- phase 1: the 4-byte total (+ the control path's tables) --
				if (State == EJobState::Dispatched)
				{
					const bool bNeedTables = Job->bQuadMesh && !Job->Region.bChunkLocalQuads;
					const bool bNeedBand = Job->Region.BandEdge > 0;
					// P1-C. Rides phase 1 for the band's reason: one job, one
					// completion event. A brick total that landed on its own
					// would be a second async stream to satisfy exactly once.
					// P1: a GPU-claimed job enqueued NO brick readback -- its
					// totals were consumed on the GPU by the claim pass -- so
					// requiring one here would fail every such job with
					// "readback objects missing".
					const bool bNeedBricks = Job->bBrickPack && !Job->bGpuPoolAlloc;
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
		JobCost.EnqPollMs += (FPlatformTime::Seconds() - EnqPollStart) * 1000.0;
		++JobCost.EnqPollN;
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

		const double EnqFetchStart = FPlatformTime::Seconds();
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
		// Both phase-2 handoffs together: they are disjoint by construction and
		// a tick has at most one of each, so one bracket answers the same
		// "is the game thread waiting here" question for both.
		JobCost.EnqFetchMs += (FPlatformTime::Seconds() - EnqFetchStart) * 1000.0;
		++JobCost.EnqFetchN;
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

		// TIMED FROM PROMOTION, NOT SUBMISSION -- and that distinction was
		// costing HALF of all GPU mesh work.
		//
		// SubmitSeconds is stamped in Submit(), when the job joins the QUEUE.
		// Using it here charged a job's queue wait against a budget whose own
		// message calls itself "readback not ready", i.e. GPU time. Under a
		// cold fill the queue is deep -- MaxInFlight and MeshBatchCap both
		// throttle promotion by design -- so a job could sit for 9 s and get
		// 1 s of actual GPU time before being declared timed out.
		//
		// Measured 2026-08-22: 8,984 jobs dispatched, 4,480 TIMED OUT (~50%),
		// each abandoned after a full 10 s and redone on the CPU worker path.
		//
		// AND IT WAS SELF-REINFORCING, which is why it presents as a cliff
		// rather than a constant tax: a timed-out job occupies its in-flight
		// slot for the whole timeout, so half the pipeline fills with zombies,
		// which lengthens the queue, which makes the next batch more likely to
		// time out the same way.
		//
		// PromotedSeconds is the moment the job actually went to the GPU, which
		// is what "readback not ready after N s" has always claimed to measure.
		// Falling back to SubmitSeconds keeps a job that somehow reached
		// InFlight without a promotion stamp from becoming immortal.
		const double ClockStart = Job->PromotedSeconds > 0.0 ? Job->PromotedSeconds : Job->SubmitSeconds;
		if (Now - ClockStart > TimeoutSeconds)
		{
			// Device loss, a wedged queue, or a render thread that never ran the
			// command. Give up on it, but keep the job object alive for any
			// render command still holding a reference.
			Job->Abandoned.store(1, std::memory_order_release);
			InFlight.RemoveAt(I, EAllowShrinking::No);
			// The queue wait is REPORTED rather than charged, so a deep queue is
			// still visible here -- it just no longer counts against the GPU.
			const double QueuedSec = (Job->PromotedSeconds > 0.0 && Job->SubmitSeconds > 0.0)
				? (Job->PromotedSeconds - Job->SubmitSeconds) : 0.0;
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::TimedOut,
				FString::Printf(TEXT("readback not ready after %.1f s on the GPU (queued %.1f s before that)"),
				                Now - ClockStart, QueuedSec) });
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
	// Deliver() is the manager's per-DELIVERED-CHUNK cost, and it is a different
	// quantity from the streaming side's apply= (0.054 ms/chunk, the handoff's
	// second blocker). Bracketed here so the two can be told apart instead of
	// both being charged to whichever bucket happens to span them: this covers
	// the result marshal, the brick-pool publication and OnJobComplete's own
	// body, which is where DrainResults' enqueue lives.
	const double DeliverStart = FPlatformTime::Seconds();
	for (const FPending& P : Finished)
	{
		// Residency in TICKS, sampled from every delivery including failures
		// and timeouts. Those belong in the population: a job that occupied a
		// slot for the whole 10 s timeout is exactly the kind of resident this
		// ceiling is about, and excluding it would flatter the number in the
		// direction that hides the problem.
		if (P.Job->PromotedTickSeq > 0)
		{
			JobCost.ResidencyTickSum += (TickSeq - P.Job->PromotedTickSeq);
			++JobCost.ResidencySamples;
		}
		Deliver(P.Job, P.Status, P.Error);
		ToRelease.Add(P.Job);
	}
	const double DeliverEnd = FPlatformTime::Seconds();
	JobCost.Delivered += int64(Finished.Num());
	JobCost.DeliverMs += (DeliverEnd - DeliverStart) * 1000.0;

	ReleaseReadbacksOnRenderThread(MoveTemp(ToRelease));
	if (Finished.Num() > 0)
	{
		JobCost.EnqReleaseMs += (FPlatformTime::Seconds() - DeliverEnd) * 1000.0;
		++JobCost.EnqReleaseN;
	}
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

// VoxelMarchChunkIndex.cpp -- the GPU-side (level, chunkCoord) -> slot lookup.
//
// VoxelMarchChunkIndex.h owns the design argument: why this exists at all, why
// one dword per entry rather than the format doc's separate bitmask, and why v1
// is a full rebuild. This file is the mechanism.

#include "VoxelMarchChunkIndex.h"

// RHI.h and RHICommandList.h BEFORE VoxelBrickPool.h, and explicitly.
//
// VoxelBrickPool.h declares FBufferRHIRef members and takes an
// FRHICommandListImmediate&, but includes neither -- it gets them transitively
// from whatever its includers happened to include first. Inside the unity blob
// that is always true and this file compiled clean; on the adaptive non-unity
// path every file takes when it is modified, it is not, and the errors name
// VoxelBrickPool.h rather than the change that triggered them.
//
// Exactly the trap VoxelFluidRender.cpp records about RHIStaticStates.h. Fixed
// here rather than left for whoever edits this file next to rediscover.
#include "RHI.h"
#include "RHICommandList.h"

#include "VoxelBrickPool.h"
#include "VoxelMarchRenderer.h" // VoxelMarchGetArm -- the absent-annotation writer's arming gate

#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
// THE TAIL-GROUP SCOPE, one line per ENQUEUE_RENDER_COMMAND body. See
// VoxelRenderFrame.h: these bodies run on the RENDER THREAD BETWEEN scene
// renders -- i.e. in the tail bucket -- and tailMs is where the parked->moving
// delta is expected to live. Costs one compare on a latched int unless the leg
// is run with -VoxelRenderFrame=2.
#include "VoxelRenderFrame.h"

// Wave 1.3 (delta upload): the scatter kernel, its cvars, and the verify
// readback. Explicit rather than transitive, for the same reason RHI.h is
// above: this file compiles alone on the adaptive non-unity path.
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RHIBreadcrumbs.h"                         // RHI_BREADCRUMB_EVENT_STAT (5.8 spelling)

// ---------------------------------------------------------------------------
// STREAMING-SIDE GPU STATS -- the split for the unattributed +5.47 ms.
//
// THE SPELLING MATTERS AND THREE OF THEM ARE DEAD. In 5.8 SCOPED_GPU_STAT,
// RDG_GPU_STAT_SCOPE and RDG_RHI_GPU_STAT_SCOPE are UE_DEPRECATED_MACRO and
// expand to NOTHING -- they compile, they look armed, and they measure zero.
// RHI_BREADCRUMB_EVENT_STAT (RHIBreadcrumbs.h:1302) and RDG_EVENT_SCOPE_STAT
// (RenderGraphEvent.h:480) are the live spellings. These sites use the first;
// see the note at each one for why the RDG form cannot be used here.
//
// WHAT THE COLUMN IS. Every DECLARE_GPU_STAT_NAMED stat has its per-frame
// EXCLUSIVE (Busy + Wait) milliseconds written to the CSV profiler once per
// frame, end-of-pipe, as GPU/<StatName> (GPUProfiler.cpp:1065-1067). The
// engine also emits GPU/Unaccounted -- queue time inside NO stat scope
// (GPUProfiler.cpp:800, accumulated at :1556 only when the stat stack is
// empty). So the GPU/ columns of one row SUM to the frame's queue busy time
// and the decomposition checks itself.
//
// KEEP THESE SIBLINGS, NEVER NESTED. Exclusive time is charged to the
// innermost stat only; nesting would silently move a term and make "which
// number am I reading" a live question. Each scope below wraps one standalone
// FRDGBuilder in one ENQUEUE_RENDER_COMMAND, so they are siblings by
// construction.
//
// ARMING: -csvGpuStats on the command line (r.GPUCsvStatsEnabled defaults 0 --
// with it off the GPU/ columns are simply ABSENT, no error), plus
// `CsvProfile FRAMES=N`. A CSV with no GPU/ column measured nothing.
// ---------------------------------------------------------------------------
DECLARE_GPU_STAT_NAMED(VoxelStreamChunkIndex, TEXT("VoxelStreamChunkIndex"));

DEFINE_LOG_CATEGORY_STATIC(LogVoxelMarchIndex, Log, All);

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (the VOXEL_WORLDGEN_USF pattern, VoxelGpuWorldGen.cpp).
#define VOXEL_MARCH_INDEX_SCATTER_USF "/VoxelEarth/VoxelMarchIndexScatter.usf"

namespace
{
	// =======================================================================
	// WAVE 1.3: UPLOAD ONLY WHAT CHANGED
	// =======================================================================
	//
	// THE MEASURED PROBLEM. MarkDirtyAndUpload ended with `Staged = Cells` -- a
	// full copy of the 56 MiB grid (kGridSlots(7) x 128^3 dwords) -- and
	// Register() handed all of it to QueueBufferUpload with
	// ERDGInitialDataFlags::None, which copies it AGAIN inside RDG and uploads
	// the whole thing. A typical flush changes ~9,500 cells of 14.7 million
	// (0.065%); at ~180 flushes per 5 s window that is ~10 GB of game-thread
	// memcpy per window, plus the same again in RDG and over PCIe. The
	// instrumentation said so exactly: FApplyDeltaMs uploadMs=3,146-3,190 per
	// window against a streaming tick totalling ~3,700 ms, with
	// uploadMs + addedMs == sinkMs to the tenth. The delta was ALREADY IN HAND
	// -- ApplyDelta receives Removed and Added lists -- and was thrown away.
	//
	// DEFAULT 0 == TODAY'S FULL-UPLOAD BEHAVIOUR, so a control leg is
	// byte-identical and the A/B lives in one binary -- the same discipline
	// voxel.March.IndexContentHash records, and for the same reason: the last
	// two-build comparison here produced a frame-time move nobody could
	// attribute.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexDeltaUpload(
		TEXT("voxel.March.IndexDeltaUpload"), 1,
		TEXT("1 (DEFAULT since 2026-08-23) = upload only changed chunk-index cells as "
		     "[cell,value] pairs scattered into the persistent GPU buffer by a small compute "
		     "pass; 0 = the old full 56 MiB staged copy + QueueBufferUpload per flush. "
		     "A flush was expected to change ~9,500 of 14.7M cells; MEASURED it averages 145 "
		     "(2,236,670 cells over 15,457 delta uploads on a 30 m/s line leg), so the delta "
		     "path moved 70.6 MB across a whole flight where the full path moved ~850 GB. "
		     "The first upload after attach is always full, and Seed/oversized dirty sets fall "
		     "back to full -- see voxel.March.IndexDeltaMaxCells and GetUploadStats()."),
		ECVF_RenderThreadSafe);

	// THE FALLBACK THRESHOLD, AND WHY 1,048,576. At 8 B per pair against 4 B
	// per cell flat, delta bytes only exceed full bytes past kCells/2 = 7.3M
	// cells -- but bytes are not the only cost. The pair list is built from a
	// TSet merge plus a per-cell gather on the game thread, and around 1M
	// cells that work approaches the flat 56 MiB memcpy it replaces while
	// buying only an 8 MiB-vs-56 MiB transfer. More decisive: nothing ROUTINE
	// touches a million cells. A flush that dirties >7% of the grid is a
	// structural event -- a reseed, a teardown, a mass eviction -- exactly the
	// class of event where the full path's "stale cells are structurally
	// impossible" guarantee is worth more than the bytes. It also bounds the
	// scatter at 16,384 groups and the staged pair list at 8 MiB.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexDeltaMaxCells(
		TEXT("voxel.March.IndexDeltaMaxCells"), 1048576,
		TEXT("Dirty-cell count above which a delta staging falls back to a full upload "
		     "(counted in GetUploadStats().FullBecauseLarge). Default 1048576 (~7% of the "
		     "grid): routine flushes are ~9,500 cells, so only structural events -- reseeds, "
		     "mass evictions -- cross it, and those are exactly where the full path's "
		     "no-stale-cells guarantee is wanted. Lower it to exercise the fallback on a leg."),
		ECVF_RenderThreadSafe);

	// THE CORRECTNESS GATE, and it is end-to-end on purpose. The existing FNV
	// content hash (voxel.March.IndexContentHash) hashes the CPU grid -- which
	// the delta path does not change, so it cannot by itself catch a GPU
	// buffer that drifted from the CPU shadow (a missed dirty cell, a wrong
	// pair, a scatter that lost a race). This gate closes the loop: after a
	// delta scatter, the WHOLE persistent buffer is copied back and FNV-hashed
	// with the same function over the same order, and compared against the
	// hash of the CPU grid at the exact staging the buffer was patched to
	// equal -- i.e. the hash a FULL upload of that state would carry.
	//
	// A wrong cell is PERSISTENT divergence: the GPU buffer stays wrong until
	// that cell is next rewritten, so a sampled gate (one readback in flight
	// at a time) still catches the bug class even at flush rates that outrun
	// the readback. Costs a 56 MiB copy plus ~17.5 ms of render-thread FNV per
	// sample (the measured rate of the game-thread hash: 3,146 ms over ~180
	// flushes), so it is a measurement leg's switch, never a shipping one.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexDeltaVerify(
		TEXT("voxel.March.IndexDeltaVerify"), 0,
		TEXT("1 = after each sampled delta scatter or GPU publish, read the whole index "
		     "buffer back and FNV-compare it against the CPU grid state it was patched to "
		     "equal (the hash a full upload of the same state would have). Results in "
		     "GetUploadStats() VerifyPasses/VerifyFailures; a failure logs an Error with "
		     "both hashes. Only meaningful with voxel.March.IndexDeltaUpload 1. EXPENSIVE "
		     "(56 MiB readback + ~17.5 ms hash per sample); for correctness legs, default 0."),
		ECVF_RenderThreadSafe);

	// THE METER WAS THE LOAD (2026-08-23). On the P1/P2 armed legs the game
	// thread's largest single cost was not streaming at all -- it was the
	// EXPECTED-HASH half of the verify above: the 56 MiB whole-grid FNV in
	// MarkDirtyAndUpload, run once per dirty flush, measured at uploadMs
	// 1,232-1,547 ms per 5 s window (25-30% of wall; ~13 ms per flush) on
	// p1p2-armed and p2-verify-armed, against addedMs of 1-2 ms for the actual
	// index bookkeeping. Every conclusion about "brickFlush cost" on those legs
	// was really this hash: the manager's brickFlushMs bracket wraps the pool
	// Flush whose index sink ends here.
	//
	// The fix leans on the verify's OWN sampling argument (FVerifySlot: "a
	// wrong cell is PERSISTENT divergence... any later sample catches the bug
	// class"): a sample that is skipped costs nothing to correctness, so the
	// expected hash does not need computing for flushes that will not sample.
	// This throttles the HASH, game-thread side, to at most one per period;
	// unhashed flushes stage/publish exactly as before with no verify armed
	// (the bHashNowValid=false path that already existed).
	//
	// Default 0 = hash every dirty flush, byte-identical to tonight's legs.
	// 500 is the recommended verify-leg setting: ~2 samples/s still catches
	// persistent divergence within a second while cutting the meter's cost
	// ~25x. Does NOT throttle the comparator's hash (voxel.March.VerifySource
	// / voxel.March.IndexContentHash force per-flush freshness; the comparator
	// reads the value every frame). FAILING READING: VerifyPasses+VerifyFailures
	// stuck at 0 with the verify armed and this set -- the throttle ate every
	// sample (period too long against the leg length), and the leg verified
	// nothing; it must be read as NOT MEASURED, never as 0 FAIL.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexDeltaVerifyPeriodMs(
		TEXT("voxel.March.IndexDeltaVerifyPeriodMs"), 0,
		TEXT("Minimum milliseconds between EXPECTED-HASH computations for "
		     "voxel.March.IndexDeltaVerify (the 56 MiB game-thread FNV -- measured 25-30% of "
		     "wall on the 2026-08-23 armed legs at the default). 0 = every dirty flush "
		     "(tonight's behaviour). Flushes between samples stage without a verify, which "
		     "the sampling design already treats as correct. Recommended 500 for verify legs."),
		ECVF_RenderThreadSafe);

	// THE VERIFY GATE USED TO CRASH THE RHI, and the fix is the readback ring
	// in the header (FVerifySlot). The crash, for the record:
	//
	//   Assertion failed: Fence->SyncPoints[GPUIndex] == nullptr
	//   D3D12DirectCommandListManager.cpp:63
	//   "The fence for the current GPU node has already been issued."
	//
	// 28 s into a leg, at 15,457 flushes per flight. The single readback was
	// re-enqueued while its previous fence was still outstanding: IsReady()
	// polls a fence that is only re-armed when the copy pass EXECUTES, so
	// between AddEnqueueCopyPass and graph execution it still reads signalled
	// from the readback's previous completed use -- and Register() runs up to
	// three times per frame (marcher, GI, shadow march), so a poll in that
	// window retired garbage, freed the guard flag, and let a second copy be
	// issued on the same fence. The ring + the ArmedFrame gate close both
	// halves; see the header comment on FVerifySlot for the full argument.

	// =======================================================================
	// PHASE 2 (docs/gpu-streaming-architecture.md): GPU-WRITTEN RESIDENCY
	// =======================================================================
	//
	// A chunk becomes resident by the GPU writing its index cell -- the
	// publish kernel derives the cell with THE MARCHER'S OWN wrap function
	// (VoxelMarchIndexCell.ush, shared include) and composes the entry value
	// itself -- instead of the game thread snapshotting [cell, value] pairs
	// from the CPU shadow for Register() to scatter a frame later.
	//
	// WHAT CHANGES AND WHAT DOES NOT. The CPU shadow (Cells) is still updated
	// by ApplyDelta, still carries every counter, and is still the reference
	// the verify gate compares against -- but it stops being on the residency
	// path: the publish command lands directly behind the pool's brick writes
	// on the render thread, so the marcher finds the chunk resident without
	// Register() staging anything. What the CPU still supplies per entry, and
	// why, is spelled out at the kernel (VoxelMarchIndexScatter.usf); the
	// short version is that the SLOT is the Phase 1 seam -- today it is the
	// CPU allocator's answer carried in the entry, and under the GPU
	// suballocator it becomes a buffer the kernel reads instead.
	//
	// DEFAULT 0 == TODAY'S BEHAVIOUR, byte-identical control leg, same
	// discipline as every switch in this file. Requires IndexDeltaUpload 1
	// (the ladder lives inside that branch); with the delta switch off this
	// one is inert.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexGpuResident(
		TEXT("voxel.March.IndexGpuResident"), 0,
		TEXT("1 = a flushed chunk's march-index cell is written by the GPU publish kernel "
		     "in a render command enqueued directly behind the pool flush (the GPU derives "
		     "cell and value; the marcher finds the chunk resident with no staging in "
		     "Register()); 0 (DEFAULT) = today's CPU-staged pair scatter. Needs "
		     "voxel.March.IndexDeltaUpload 1. The CPU shadow stays authoritative for "
		     "counters and for the voxel.March.IndexDeltaVerify cross-check, which is the "
		     "gate that proves the GPU-derived cells agree with what the CPU would have "
		     "written -- run it on any leg that flips this on. Fallbacks (seed/first/"
		     "pending/large/lost) stage full exactly as the delta path does; see "
		     "GetUploadStats()."),
		ECVF_RenderThreadSafe);

	// =======================================================================
	// MAKING kAnySolidBit REAL (voxel.March.IndexAnySolid)
	// =======================================================================
	//
	// THE SKIP HAS ALWAYS BEEN THERE AND HAS NEVER FIRED.
	// VoxelBrickTraverse.ush's chunk lookup early-outs on
	// VOXEL_MARCH_INDEX_ANYSOLID_BIT with no record fetch at all -- its own
	// comment calls it "the cheapest possible skip and it is why the bit rides
	// the index dword instead of the record" -- and the census word that counts
	// it (cellIndexEmpty) reads EXACTLY ZERO on every leg ever run, because all
	// four writers of a resident entry hardcode the bit set. A chunk that is
	// resident and entirely air is instead rejected at `LevelAndFlags & 0x10`,
	// AFTER a scattered ~16 B fetch into the ~15.7 MB chunk table.
	//
	// WHAT THE CENSUS MEASURED, of the lookups that PAY FOR THE RECORD FETCH:
	//
	//     pose      wasted as air     wasted fetches/frame
	//     down          35.11%              2.20 M
	//     horizon       69.13%             23.06 M
	//     sky           73.10%             23.30 M
	//
	// AND THE EXPECTATION THAT GOES WITH IT, stated here so nobody quotes the
	// percentage as a speedup: the whole chunk table is 15.7 MB and fits this
	// box's 64 MB Infinity Cache, and neighbouring rays in a wave probe the
	// SAME chunks, so these are L2 misses that hit MALL, not DRAM. The move to
	// expect is ~0.2-0.4 ms, not anything proportional to 73%. A measured small
	// win is still a win; a 73% headline attached to a 5% result is not.
	//
	// DEFAULT 0, like every switch in this file, so a control leg is
	// byte-identical and the A/B lives in one binary.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexAnySolid(
		TEXT("voxel.March.IndexAnySolid"), 0,
		TEXT("1 = after each index write, a compute pass re-reads the chunk RECORD for every "
		     "pool slot, re-runs the marcher's own origin+level validation against the index "
		     "cell that names it, and CLEARS the entry's anySolid bit (bit 30) on positive "
		     "proof that the chunk is entirely air -- turning VoxelBrickTraverse.ush's "
		     "never-taken cheapest skip on. 0 (DEFAULT) = the bit stays the hardcoded 1 it has "
		     "always been. ENGAGEMENT PROOF is the census word cellIndexEmpty "
		     "(voxel.March.HoleStats 1) leaving zero for the first time; CORRECTNESS PROOF is "
		     "voxel.March.IndexAnySolidAudit reading wrongClear=0 over a non-zero checked "
		     "count. The bit is a HINT and the record is truth: every uncertain case in the "
		     "kernel leaves the bit SET, because a wrongly-set bit costs a fetch and a wrongly-"
		     "cleared one deletes ground."),
		ECVF_RenderThreadSafe);

	// THE RED ARM, AND IT RUNS FIRST.
	//
	// A green leg after an unproven detector means nothing. This forces the
	// refine kernel to clear bit 30 on EVERY identity-matched cell including
	// the ones the record proves are solid -- i.e. it commits, deliberately,
	// the exact defect the audit exists to catch. The audit must then report
	// wrongClear > 0. If it does not, the audit cannot fail and no subsequent
	// green reading from it is evidence of anything.
	//
	// IT ALSO DELETES TERRAIN, ON PURPOSE. The marcher will skip every solid
	// chunk whose bit this clears, so a poison leg's capture is a broken world
	// and that is the second, independent confirmation -- the counter and the
	// image have to agree about the same corruption.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexAnySolidPoison(
		TEXT("voxel.March.IndexAnySolidPoison"), 0,
		TEXT("RED ARM. 1 = the refine pass clears anySolid on every identity-matched index "
		     "cell, INCLUDING chunks the record proves are solid. This is a deliberate "
		     "corruption whose purpose is to make voxel.March.IndexAnySolidAudit fire: "
		     "wrongClear MUST become non-zero and the capture MUST show missing terrain. Run "
		     "this BEFORE any green leg -- a detector that has not been shown capable of "
		     "failing has not been shown to work. Needs voxel.March.IndexAnySolid 1. NEVER "
		     "ship, never quote a timing from a poison leg."),
		ECVF_RenderThreadSafe);

	// THE CORRECTNESS GATE. One thread per pool slot, run LAST in the graph --
	// after the index write and after the refine pass -- so it reads exactly
	// the state every march pass from there until the next flush consumes.
	// Placing it ahead of the refine would be a test that CANNOT FAIL: on the
	// full-upload path the buffer is created in that graph with every anySolid
	// bit already set.
	//
	// It checks the invariant directly and over the whole resident set, not
	// only over the cells rays happened to touch: no resident, identity-matched
	// index cell whose record says solid may have bit 30 clear. That is
	// strictly broader coverage than a marcher-side re-fetch, and it is
	// indifferent to HOW a bit got wrong -- a wrong cell, a stale read, an
	// ordering mistake and a logic bug all land in the same counter.
	//
	// THE DENOMINATOR IS PART OF THE READING. wrongClear=0 over checked=0 is
	// not a pass, it is an audit that did not run, and the log line prints both
	// for exactly that reason.
	TAutoConsoleVariable<int32> CVarVoxelMarchIndexAnySolidAudit(
		TEXT("voxel.March.IndexAnySolidAudit"), 0,
		TEXT("1 = after each index write and refine, a compute pass walks every pool slot and counts "
		     "index cells that say AIR (bit 30 clear) over a record that says SOLID -- the "
		     "one error direction that deletes ground. Reported in GetUploadStats() as "
		     "auditChecked / auditWrongClear; ANY non-zero wrongClear outranks every "
		     "performance number on the leg. Read the CHECKED count too: zero there is an "
		     "audit that never ran, not a clean one. Prove it can fire with "
		     "voxel.March.IndexAnySolidPoison first. Costs one dispatch of ChunkCapacity "
		     "threads plus a 48 B readback per sampled flush."),
		ECVF_RenderThreadSafe);

	// Matches [numthreads(64, 1, 1)] in every kernel in the .usf. Restated here
	// because the dispatch size is computed from it and a mismatch drops the
	// tail of the pair list -- which is a handful of silently wrong index
	// cells, the exact failure shape this feature must never produce.
	constexpr int32 kScatterGroupSize = 64;

	// EVERY SHADER CLASS IN THIS FILE PUSHES THESE, NOT JUST THE ONE THAT USES
	// THEM, AND THAT IS NOT TIDINESS -- IT IS A BOOT FATAL OTHERWISE.
	//
	// The three entry points live in ONE .usf, so the refine kernel's body is
	// in the translation unit for every one of them. A macro it references that
	// only the refine class defines makes VoxelMarchIndexScatterMain and
	// VoxelMarchIndexPublishMain fail to compile -- and a shader that fails to
	// compile does not fail a BUILD, it fails an editor LAUNCH, which costs a
	// whole leg to discover. Caught by toolsoxel-check-indexscatter-shader.ps1
	// on the first run of the very change that introduced it, which is exactly
	// what that script exists for.
	//
	// The refine kernel deliberately gives the stat-word offsets NO #ifndef
	// fallback: a dropped SetDefine must be a compile error, not a silent write
	// into word 0 that reads as a plausible census. The guard case in that
	// script asserts the failure mode still holds.
	void VoxelMarchIndexScatterSetDefines(FShaderCompilerEnvironment& OutEnvironment)
	{
		// THE RECORD STRIDE, PUSHED FROM THE POOL'S OWN CONSTANT so the refine
		// kernel is not a THIRD hand-spelling of it beside
		// VoxelBrickTraverse.ush's #define and FVoxelBrickPool's
		// kChunkRecordDwords. The kernel still cross-checks it against the
		// BOUND VoxelBrickChunkRecordDwords at runtime and refuses to clear
		// anything when they disagree -- the same guard the marcher runs, with
		// the same reasoning: a stride that moved on one side only reads a
		// neighbouring record's fields, and most of them survive an origin
		// validation often enough to look like an answer.
		OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_INDEX_REFINE_RECORD_DWORDS"),
		                         uint32(FVoxelBrickPool::kChunkRecordDwords));
		// The stats word offsets, from the enum that also sizes the readback. A
		// hand mirror here reads a plausible number out of the wrong slot --
		// the failure the hole-stats census pushes its own words to avoid.
		using FIdx = FVoxelMarchChunkIndex;
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_EXAMINED"),
		                         int32(FIdx::RefineStat_Examined));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_NOMATCH"),
		                         int32(FIdx::RefineStat_NoMatch));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_ZERO_RECORD"),
		                         int32(FIdx::RefineStat_RefusedZeroRecord));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_BAD_ORIGIN"),
		                         int32(FIdx::RefineStat_RefusedOrigin));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_BAD_LEVEL"),
		                         int32(FIdx::RefineStat_RefusedLevel));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_BAD_CELL"),
		                         int32(FIdx::RefineStat_RefusedCell));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_BAD_STRIDE"),
		                         int32(FIdx::RefineStat_RefusedStride));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_INCONSISTENT"),
		                         int32(FIdx::RefineStat_RefusedInconsistent));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_CLEARED"),
		                         int32(FIdx::RefineStat_Cleared));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_CAS_LOST"),
		                         int32(FIdx::RefineStat_CasLost));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_LEFT_SOLID"),
		                         int32(FIdx::RefineStat_LeftSolid));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_ALREADY_CLEAR"),
		                         int32(FIdx::RefineStat_AlreadyClear));
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_AUDIT_SOLID"),
		                         int32(FIdx::RefineStat_AuditSolid));
		// Sizes the kernel's groupshared tally AND the readback; one number,
		// one place.
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_STAT_WORDS"),
		                         int32(FIdx::kRefineStatWords));
		// Matches [numthreads(N,1,1)] and the group count this file computes.
		OutEnvironment.SetDefine(TEXT("VOXEL_REFINE_GROUP_SIZE"), kScatterGroupSize);
	}

	// One thread per changed cell; pairs are deduplicated BY CONSTRUCTION on
	// the host (built from a TSet keyed by cell), so no two threads in a
	// dispatch write the same address -- see the .usf header for why that is
	// load-bearing rather than tidy.
	class FVoxelMarchIndexScatterCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMarchIndexScatterCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchIndexScatterCS, FGlobalShader);

		// PUSHED BY EVERY CLASS IN THIS FILE, not only the one that reads them.
		// This kernel uses none of these defines; it compiles the refine
		// kernel's BODY anyway, because all three entry points share one .usf.
		// Omitting the call here made VoxelMarchIndexScatterMain fail with
		// "use of undeclared identifier 'VOXEL_REFINE_STAT_BAD_STRIDE'" -- an
		// EDITOR BOOT fatal, not a build failure. See the function's comment.
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			VoxelMarchIndexScatterSetDefines(OutEnvironment);
		}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MarchIndexDeltaPairs)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchChunkIndexRW)
			SHADER_PARAMETER(uint32, MarchIndexDeltaCount)
			SHADER_PARAMETER(uint32, MarchIndexCellCount)
		END_SHADER_PARAMETER_STRUCT()
	};

	// The Phase 2 publish kernel: entries in, cells derived and written on the
	// GPU. Same .usf as the scatter, second entry point; the kernel header
	// owns the entry layout and the removal-guard argument.
	class FVoxelMarchIndexPublishCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMarchIndexPublishCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchIndexPublishCS, FGlobalShader);

		// Same reason as the scatter class above: this kernel reads none of
		// these defines and compiles the refine kernel's body regardless.
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			VoxelMarchIndexScatterSetDefines(OutEnvironment);
		}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MarchIndexPublishEntries)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchChunkIndexRW)
			SHADER_PARAMETER(uint32, MarchIndexPublishFirst)
			SHADER_PARAMETER(uint32, MarchIndexPublishCount)
			SHADER_PARAMETER(uint32, MarchIndexPublishMode)
			SHADER_PARAMETER(FUintVector, MarchIndexPublishDimChunks)
			SHADER_PARAMETER(uint32, MarchIndexPublishCellsPerLevel)
			SHADER_PARAMETER(uint32, MarchIndexPublishCellCount)
		END_SHADER_PARAMETER_STRUCT()
	};

	// Dwords per publish entry: [x, y, z, gridSlot, chunkSlot]. Restated from
	// the kernel for the same tail-dropping reason as kScatterGroupSize.
	constexpr int32 kPublishEntryDwords = 5;

	// THE anySolid REFINE / AUDIT KERNEL. One thread per POOL SLOT, driven
	// from the RECORD side rather than from a per-flush cell list, and that
	// choice is the design:
	//
	//   * It is INDIFFERENT TO WHICH UPLOAD PATH RAN. The full staged upload,
	//     the delta pair scatter and the Phase 2 publish all leave the same
	//     question -- "does this cell's record say air" -- and a record-driven
	//     pass answers it identically for all three, including the fallback
	//     ladder (seed / first / pending / large / lost) that has no cell list
	//     at all.
	//   * It is SELF-HEALING. A full upload re-writes bit 30 back to the
	//     shadow's hardcoded 1; this pass runs in the same graph, after it, and
	//     re-derives the answer. There is no state to keep in sync and nothing
	//     to lose.
	//   * It cannot outrun the records. A record only changes in a flush, every
	//     flush writes the index, this pass runs where the index was written,
	//     and the pool issues its record writes BEFORE the index sink on both
	//     producers (the CPU arm's "record last" bucket order; the GPU shell's
	//     "the index learns about the chunk on the next Flush, which runs after
	//     the caller has enqueued the graph that writes the record"). So the
	//     record this pass reads is never older than the entry it is refining.
	//
	// The pool bindings come from VOXEL_BRICK_POOL_PARAMETERS() and are filled
	// by FVoxelBrickPool::BindShaderParameters, so the chunk table, its slot
	// count and the record stride reach this kernel through the SAME names and
	// the SAME filler the marcher uses. The kernel reads three of them; the
	// rest go unbound, which is what the macro is for.
	class FVoxelMarchIndexRefineCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMarchIndexRefineCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchIndexRefineCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			VoxelMarchIndexScatterSetDefines(OutEnvironment);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICK_POOL_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchChunkIndexRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MarchIndexRefineStats)
			// Where THIS pass's 12-word block starts. The two arms share one
			// buffer and both write a word named Cleared -- the refine's prize
			// and the audit's alarm -- so the base is what keeps a hole count
			// from being added to a saving.
			SHADER_PARAMETER(uint32, MarchIndexRefineStatBase)
			SHADER_PARAMETER(FUintVector, MarchIndexRefineDimChunks)
			SHADER_PARAMETER(uint32, MarchIndexRefineCellsPerLevel)
			SHADER_PARAMETER(uint32, MarchIndexRefineCellCount)
			SHADER_PARAMETER(uint32, MarchIndexRefineRingGrids)
			SHADER_PARAMETER(uint32, MarchIndexRefineCoverLevel)
			SHADER_PARAMETER(uint32, MarchIndexRefineCoverGridSlot)
			// 0 = refine (clear on proof), 1 = audit (count, write nothing).
			SHADER_PARAMETER(uint32, MarchIndexRefineMode)
			// 1 = the red arm: clear even when the record proves solid.
			SHADER_PARAMETER(uint32, MarchIndexRefinePoison)
		END_SHADER_PARAMETER_STRUCT()
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelMarchIndexScatterCS, VOXEL_MARCH_INDEX_SCATTER_USF,
                        "VoxelMarchIndexScatterMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchIndexPublishCS, VOXEL_MARCH_INDEX_SCATTER_USF,
                        "VoxelMarchIndexPublishMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchIndexRefineCS, VOXEL_MARCH_INDEX_SCATTER_USF,
                        "VoxelMarchIndexRefineMain", SF_Compute);

namespace
{
	// THE ALIASING PROOF, AS A COMPILE-TIME CHECK RATHER THAN A PARAGRAPH.
	//
	// Two level-0 chunks collide in the toroidal grid only if they are kDimXY
	// chunks apart on an axis. A level-0 chunk is 32 voxels * 0.1 m = 3.2 m, so
	// that distance is 128 * 3.2 = 409.6 m. R0 spans 128 m of RADIUS (0-128,
	// kDefaultRingPresets), i.e. 256 m across, i.e. 80 chunks.
	//
	// 80 < 128, so no two chunks resident at level 0 can land on the same cell,
	// which is what lets the shader wrap with a mask and carry no origin at all.
	// If R0 is ever widened past ~204 m of radius this stops holding, and the
	// symptom is one chunk silently shadowing another -- turned into a hole by
	// the marcher's record validation, but a hole nobody ordered.
	constexpr double kLevel0ChunkMeters = 3.2;
	constexpr double kRing0RadiusMeters = 128.0;
	constexpr int32 kRing0SpanChunks = int32((2.0 * kRing0RadiusMeters) / kLevel0ChunkMeters);
	static_assert(kRing0SpanChunks < int32(FVoxelMarchChunkIndex::kDimXY),
	              "R0 is now wide enough for two level-0 chunks to alias in the index grid. "
	              "Raise kDimXY (it must stay a power of two) or narrow R0.");
	// AND THE SAME PROOF FOR Z, which the original assert did not make. It was
	// missing, kDimZ was 64 against a span of 80, and the runtime check that
	// should have caught it was a tautology -- so the aliasing was real and
	// silent. Both axes are now proved by the same expression.
	static_assert(kRing0SpanChunks < int32(FVoxelMarchChunkIndex::kDimZ),
	              "R0 is now tall enough for two level-0 chunks to alias vertically in the index "
	              "grid. Raise kDimZ (it must stay a power of two) or narrow R0.");

	// P3-B2b-1 marches levels 0 and 1. Entries at any level the grid does not
	// cover are IGNORED rather than folded in -- silently folding an L2 chunk
	// into an L1 grid would put terrain at twice its size in the same cells,
	// which reads as terrain rather than as an error.
	//
	// THE TEST ITSELF NOW LIVES IN FVoxelMarchChunkIndex::GridSlotForLevel, which
	// is the single authority for the mapping now that a level is not its own
	// slot. The local kIndexedLevels constant that used to sit here was deleted
	// rather than left unused: a second spelling of "which levels are carried" is
	// exactly the drift this class keeps paying for.

	// PHASE 6: THE SAME PROOF, FOR COVER, ON ALL THREE AXES.
	//
	// A cover chunk is 32 * 50 mm = 1.6 m. Two cover chunks alias only if they
	// are kDimXY apart on an axis. The index admits cover only within
	// +/-kCoverBandRadiusChunks of the band centre, so the simultaneous span is
	// at most 2 * 40 = 80 chunks -- the identical 80 < 128 that proves ring 0
	// safe. Unlike the ring proof, this one's premise is not overridable on the
	// command line: it is enforced by AdmitToSlot, which counts what it refuses.
	constexpr int32 kCoverSpanChunks = 2 * FVoxelMarchChunkIndex::kCoverBandRadiusChunks;
	static_assert(kCoverSpanChunks < int32(FVoxelMarchChunkIndex::kDimXY),
	              "the cover band is now wide enough for two cover chunks to alias in the index "
	              "grid. Narrow kCoverBandRadiusChunks or raise kDimXY (power of two).");
	static_assert(kCoverSpanChunks < int32(FVoxelMarchChunkIndex::kDimZ),
	              "the cover band is now tall enough for two cover chunks to alias vertically in "
	              "the index grid. Narrow kCoverBandRadiusChunks or raise kDimZ (power of two).");
	// The cover level must survive the record's four-bit LevelAndFlags field AND
	// the VisBuffer's three-bit level field, or a cover hit decodes as a ring hit
	// somewhere in the middle of the cascade.
	static_assert(FVoxelMarchChunkIndex::kCoverLevel >= 0 &&
	                  FVoxelMarchChunkIndex::kCoverLevel < 8,
	              "the cover level must fit the VisBuffer's three-bit level field (0..7) and the "
	              "chunk record's four-bit LevelAndFlags[0:3].");
	// TWO SPELLINGS, ONE COMPILE ERROR. The pool owns the cover level because the
	// pool owns the key; this class spells it too, for the grid-slot mapping.
	// This file is the only one that includes both, so it is the only place the
	// two can be tied -- the same device kBricksPerChunk already uses.
	static_assert(FVoxelMarchChunkIndex::kCoverLevel == FVoxelBrickPool::kCoverLevel,
	              "the march index and the brick pool disagree about which level ground cover is "
	              "keyed at; one of them would index cover as terrain.");
	static_assert(FVoxelMarchChunkIndex::kCoverLevel >= int32(FVoxelMarchChunkIndex::kRingGrids),
	              "the cover level collides with a ring level; GridSlotForLevel would answer the "
	              "ring slot and cover would be indexed as terrain.");
}

FVoxelMarchChunkIndex& GetGlobalVoxelMarchChunkIndex()
{
	static FVoxelMarchChunkIndex Index;
	return Index;
}

FVoxelMarchChunkIndex::FVoxelMarchChunkIndex()
{
	// THE SENTINEL PAIR, SET HERE AND NOT ONLY IN Seed. FIntVector's default
	// constructor does not initialise, and `= default` therefore left
	// ObservedMin/Max holding stack garbage on any path that reached ApplyDelta
	// before the first Seed. GetCumulativeCoordSpan survived that because it is
	// a diagnostic nobody gates on; GetResidentChunkZBound would not -- garbage
	// there is a bound narrower than the resident set, i.e. a hole. Same values
	// Seed writes, so nothing downstream can tell the two paths apart.
	for (uint32 S = 0; S < kGridSlots; ++S)
	{
		ObservedMin[S] = FIntVector(MAX_int32, MAX_int32, MAX_int32);
		ObservedMax[S] = FIntVector(MIN_int32, MIN_int32, MIN_int32);
	}
}
FVoxelMarchChunkIndex::~FVoxelMarchChunkIndex() = default;

void FVoxelMarchChunkIndex::AttachToGlobalPool()
{
	check(IsInGameThread());
	if (bAttached)
	{
		return;
	}
	bAttached = true;

	// 56 MiB (kGridSlots x 128^3 dwords -- the "4 MiB" this comment used to
	// claim was the old one-level kDimZ=64 grid), zeroed. Zero is "not
	// resident" by construction -- kResidentBit
	// clear -- so an unseeded grid reads as an empty world rather than as
	// garbage slots, which is the safe direction.
	//
	// ZEROED UNCONDITIONALLY, NOT VIA SetNumZeroed's SIZING PATH. This is a
	// global that outlives a UWorld, so the SECOND attach -- a new PIE session
	// after the first was stopped -- finds Cells already kCells long, and
	// TArray::SetNumZeroed only zeroes elements it ADDS. It was therefore a
	// no-op on re-attach and the grid kept every resident bit from the previous
	// world. Seed() does not cover this either: it resets the counters and the
	// spans, but never the cells.
	//
	// The symptom was a second PIE session that rendered pure sky
	// (shadowmarch sky=1027408 of considered=1027408) with indexEntries frozen
	// at 131047 against a 131072 pool -- an index full of chunks no live record
	// owned, so nothing could be released and everything evicted.
	Cells.SetNumUninitialized(int32(kCells));
	FMemory::Memzero(Cells.GetData(), SIZE_T(Cells.Num()) * sizeof(uint32));

	// ONE CALL, and the header explains why: registering and snapshotting
	// separately leaves a window in which a flush delivers a delta against an
	// index that was never seeded, and the symptom is a handful of chunks
	// missing from the marched world with every counter reading healthy.
	// Registering late is the NORMAL case -- the pool reaches ~87,800 chunks
	// during the cold fill and the marcher attaches long after -- so the
	// snapshot is the bulk of the work and the deltas are the tail.
	TArray<FVoxelBrickIndexEntry> Snapshot;
	GetGlobalVoxelBrickPool().SetIndexSink(
		[this](const FVoxelBrickIndexDelta& Delta) { ApplyDelta(Delta); }, Snapshot);
	Seed(Snapshot);

	UE_LOG(LogVoxelMarchIndex, Display,
	       TEXT("Voxel march chunk index attached: seeded %d chunks of %d offered "
	            "(INDEXED per level %d/%d/%d/%d/%d/%d; OFFERED per level %d/%d/%d/%d/%d/%d; "
	            "%d dropped for being above the %u levels this grid carries; "
	            "grid %ux%ux%u, %llu MiB)."),
	       NumEntries, Snapshot.Num(),
	       PerSlotEntries[0], PerSlotEntries[1], PerSlotEntries[2],
	       PerSlotEntries[3], PerSlotEntries[4], PerSlotEntries[5],
	       OfferedPerLevel[0], OfferedPerLevel[1], OfferedPerLevel[2],
	       OfferedPerLevel[3], OfferedPerLevel[4], OfferedPerLevel[5],
	       DroppedWrongLevel, kLevels, kDimXY, kDimXY, kDimZ,
	       uint64(kCells) * sizeof(uint32) / (1024ull * 1024ull));
}

void FVoxelMarchChunkIndex::Detach()
{
	check(IsInGameThread());
	if (!bAttached)
	{
		return;
	}
	TArray<FVoxelBrickIndexEntry> Ignored;
	GetGlobalVoxelBrickPool().SetIndexSink(nullptr, Ignored);
	bAttached = false;

	// THE GPU-SIDE COPY MUST GO TOO, and it is a separate lifetime from Cells.
	// Register() hands the march passes whatever Pooled holds; leaving it alive
	// across a world teardown lets a stopped world's index keep answering
	// lookups in the next one, which reads as terrain that is present but
	// unmarchable. bStagedValid=false forces the next Register to rebuild from
	// the (re-seeded) cells rather than trusting Staged.
	Staged.Reset();
	bStagedValid = false;
	bDirty = true;

	// THE DELTA MACHINERY GOES WITH IT. Pooled is about to be released, so
	// there is nothing left to patch: a staged delta surviving past here would
	// be scattered into the NEXT world's freshly seeded buffer, overwriting a
	// handful of its cells with the previous world's entries -- a few chunks
	// of stale terrain, not an error. bDeltaBaseEstablished=false is what
	// forces the next world's first staging to be full. The flag/pairs clear
	// takes the stage lock because Register() consumes them on the render
	// thread; the sets are game-thread-only and need none.
	{
		FScopeLock Lock(&DeltaStageLock);
		bStagedDeltaValid = false;
		bStagedHashValid = false;
		StagedDeltaPairs.Reset();
	}
	DeltaPendingCells.Reset();
	DeltaStagedCells.Reset();
	bDeltaBaseEstablished = false;
	PendingStagedBytes = 0;
	// Phase 2 state goes with it, for the same reason as the staged delta: a
	// publish scratch surviving a teardown would scatter the previous world's
	// entries into the next world's buffer. The lost flag is cleared so the
	// next world does not stage an unexplained healing full for a buffer that
	// no longer exists. (Publish scratch is per-flush and normally empty here
	// anyway; a verify slot still in flight is left to retire naturally -- its
	// content and expected hash were captured as a consistent pair.)
	GpuPublishRemoves.Reset();
	GpuPublishAdds.Reset();
	bGpuPublishLost.store(false);

	// POOLED IS RENDER-THREAD STATE AND MUST BE RELEASED THERE. Register()
	// writes it via QueueBufferExtraction while a graph is building, so
	// clearing it from the game thread here is a genuine data race on the
	// pointer, not a theoretical one.
	//
	// It cannot simply be left alone either: with bStagedValid false, Register
	// falls through to RegisterExternalBuffer(Pooled) and would hand the NEXT
	// world the previous world's index buffer until the first flush replaced
	// it -- a shorter window of exactly the bug this teardown exists to close.
	//
	// Enqueueing is what satisfies both. Render commands run in order, so the
	// release lands after any in-flight graph that still references the buffer
	// and before the next world's first Register. `this` is a global, so there
	// is no lifetime question about the capture.
	ENQUEUE_RENDER_COMMAND(VoxelMarchChunkIndexDetach)(
		[this](FRHICommandListImmediate&)
		{
			VOXEL_RENDER_FRAME_SCOPE_TAIL(TailChunkIndex);
			Pooled.SafeRelease();
			// THE COARSE LEVEL GOES WITH THE INDEX, IN THE SAME COMMAND. If
			// these outlived the index buffer, the next world's first frames
			// would march a fresh index against the PREVIOUS world's block
			// bitfields -- Occupied set where the new world holds nothing
			// (slower, harmless) and, in the direction that matters, Occupied
			// CLEAR over ground the new world does hold, which is a skip over
			// real terrain and therefore a hole. Same lifetime, same command,
			// no window.
			PooledBlockOccupied.SafeRelease();
			PooledBlockAnyAbsent.SafeRelease();
		});

	// A DETACHED INDEX MUST REPORT EMPTY, because it IS empty -- it is wired to
	// no pool and owns no chunk. Leaving NumEntries at its last value made
	// GetNumEntries() report 131047 for an index holding nothing, which is
	// exactly the kind of plausible-but-false counter that has cost this
	// feature three separate wrong conclusions.
	//
	// The cells are zeroed HERE as well as on attach so the detached state is
	// self-consistent rather than "empty by counter, stale by content". Attach
	// keeps its own memzero: it is the one that must also handle the very first
	// sizing, and an attach that has not been preceded by a detach.
	FMemory::Memzero(Cells.GetData(), SIZE_T(Cells.Num()) * sizeof(uint32));
	// The coarse level's shadow follows the cells it describes, for the reason
	// the cells are zeroed here at all: a detached index that reports empty by
	// counter and stale by content is the shape this class has already been
	// caught in once. ResetBlocks leaves Occupied clear and AnyAbsent SET,
	// which is the honest description of a grid holding nothing.
	ResetBlocks();
	// The upload mirror goes with it, and the generation is reset to 0 -- the
	// "nothing has ever been mirrored" state, which is the only one in which the
	// all-ones fallback is legitimate. A mirror surviving the detach would
	// upload this world's residency into the next world's buffers.
	//
	// UNDER THE LOCK, AND IT IS NOT OPTIONAL -- the same rule StagedDeltaPairs
	// carries a paragraph about. RegisterWithBlocks memcpys out of these arrays
	// on the RENDER thread while holding this lock; Reset() frees the
	// allocation, so doing it unguarded is a DANGLING POINTER, not a torn value.
	// (It was unguarded when the mirror was a one-shot staging, which was
	// already wrong and merely narrower.)
	{
		FScopeLock Lock(&DeltaStageLock);
		BlockMirrorOccupied.Reset();
		BlockMirrorAnyAbsent.Reset();
		BlockMirrorAllSky.Reset();
		BlockShadowGeneration = 0;
		PooledBlockGeneration = 0;
	}
	CellOwner.Reset();
	NumEntries = 0;
	FMemory::Memzero(PerSlotEntries, sizeof(PerSlotEntries));

	// The spans, offer buckets and alias counters are diagnostics about a
	// session rather than residency, and Seed() resets them on the next attach.
}

namespace
{
	// (level-0 chunk coord) -> cell. MIRRORED IN VoxelBrickTraverse.ush's
	// VoxelMarchIndexCell and nowhere else. Two's complement makes
	// uint(x) & (dim-1) the correct modulo for negative coordinates, which is
	// the whole reason the dims are powers of two.
	// SLOT, NOT LEVEL. The cover level is 7 and the grid does not carry eight
	// sub-grids; GridSlotForLevel is the one place that mapping is spelled, and
	// the shader reads the cover slot as a uniform for the same reason.
	FORCEINLINE uint32 CellOf(const FIntVector& ChunkCoord, int32 Slot)
	{
		const uint32 cx = uint32(ChunkCoord.X) & (FVoxelMarchChunkIndex::kDimXY - 1u);
		const uint32 cy = uint32(ChunkCoord.Y) & (FVoxelMarchChunkIndex::kDimXY - 1u);
		const uint32 cz = uint32(ChunkCoord.Z) & (FVoxelMarchChunkIndex::kDimZ - 1u);
		return uint32(Slot) * FVoxelMarchChunkIndex::kCellsPerLevel +
		       cx + FVoxelMarchChunkIndex::kDimXY * (cy + FVoxelMarchChunkIndex::kDimXY * cz);
	}
}

// THE RUNTIME HALF OF THE ALIASING PROOF.
//
// The static_asserts above prove no two level-0 chunks can share a cell GIVEN
// kDefaultRingPresets. But ring extents are overridable on the command line
// (-VoxelRingInnerMeters= / -VoxelRingOuterMeters=), and a compile-time proof
// cannot see a runtime override. So the index watches the chunk-coordinate
// extent it is actually handed, and says so once if it approaches a grid
// dimension.
//
// It complains at HALF the dimension rather than at the dimension, because by
// the time the observed span EQUALS the grid the aliasing has already happened
// and been absorbed -- silently, as a hole. Half is where there is still margin
// to act.
void FVoxelMarchChunkIndex::NoteObservedSpan(const FIntVector& Coord, int32 Slot)
{
	if (Slot < 0 || Slot >= int32(kGridSlots))
	{
		return;
	}
	const int32 Level = Slot;   // named for the field writes below
	// PER LEVEL, because level-1 chunk coordinates are a different space from
	// level-0 ones and folding them together produced a span that described
	// neither. CUMULATIVE AND SAID TO BE -- it tracks how far the camera has
	// travelled, not what is resident now, and it no longer warns about
	// anything. Aliasing is counted where it happens instead.
	ObservedMin[Level].X = FMath::Min(ObservedMin[Level].X, Coord.X);
	ObservedMin[Level].Y = FMath::Min(ObservedMin[Level].Y, Coord.Y);
	ObservedMin[Level].Z = FMath::Min(ObservedMin[Level].Z, Coord.Z);
	ObservedMax[Level].X = FMath::Max(ObservedMax[Level].X, Coord.X);
	ObservedMax[Level].Y = FMath::Max(ObservedMax[Level].Y, Coord.Y);
	ObservedMax[Level].Z = FMath::Max(ObservedMax[Level].Z, Coord.Z);
}

FIntVector FVoxelMarchChunkIndex::GetCumulativeCoordSpan(int32 Level) const
{
	// TAKES A LEVEL AND MAPS IT, because every caller has a level in hand and
	// only this class knows which slot holds it.
	const int32 Slot = GridSlotForLevel(Level);
	if (Slot < 0 || ObservedMax[Slot].X < ObservedMin[Slot].X)
	{
		return FIntVector::ZeroValue;
	}
	return FIntVector(ObservedMax[Slot].X - ObservedMin[Slot].X + 1,
	                  ObservedMax[Slot].Y - ObservedMin[Slot].Y + 1,
	                  ObservedMax[Slot].Z - ObservedMin[Slot].Z + 1);
}

bool FVoxelMarchChunkIndex::GetResidentChunkZBound(int32 Level, int32& OutMinZ,
                                                   int32& OutMaxZ) const
{
	// See the long note at the declaration for why a CUMULATIVE union is the
	// correct source for a CONTAINMENT question while being the wrong source
	// for the aliasing question the same fields once answered.
	const int32 Slot = GridSlotForLevel(Level);
	if (Slot < 0)
	{
		return false;
	}
	// THE SAME CONDITION MarchIndexLevelPopulated PUBLISHES, read from the same
	// counter (VoxelMarchRenderer.cpp's bind uses GetNumEntriesAtLevel, which
	// is this array). Spelled against PerSlotEntries rather than against the
	// span so the two answers cannot disagree about an empty slot -- the span
	// stays valid forever once anything has been seen, and cutting an empty
	// slot would silently change what the marcher's shell test means there.
	if (PerSlotEntries[Slot] <= 0)
	{
		return false;
	}
	// Never observed: the sentinel pair from Seed/the constructor. Distinct
	// from "empty" above, and refused for the same reason.
	if (ObservedMax[Slot].Z < ObservedMin[Slot].Z)
	{
		return false;
	}
	OutMinZ = ObservedMin[Slot].Z;
	OutMaxZ = ObservedMax[Slot].Z;
	return true;
}

// ===========================================================================
// PHASE 6: THE COVER BAND, AND THE CONSERVATION LAW OVER IT
// ===========================================================================

void FVoxelMarchChunkIndex::SetCoverBandCentreChunk(const FIntVector& CoverChunkCoord)
{
	check(IsInGameThread());
	CoverBandCentreChunk = CoverChunkCoord;
	bCoverBandCentreSet = true;
}

// Ring slots admit everything -- their bound is the ring preset and the
// static_asserts above. The cover slot admits only its band, and COUNTS WHAT IT
// REFUSES, which is what turns the compile-time aliasing proof into a runtime
// bound rather than a hope.
bool FVoxelMarchChunkIndex::AdmitToSlot(const FIntVector& Coord, int32 Slot)
{
	if (Slot != int32(kCoverGridSlot))
	{
		return true;
	}

	// THE MUTATION ARM, AND IT FIRES ON THE PATH THE LAW DEPENDS ON.
	//
	// An arm that only bites when something is ALREADY out of band would report
	// nothing on a leg where the band happens to hold everything -- a third
	// silent instrument, which is the failure this project has now found twelve
	// times. So it refuses the FIRST cover entry it is ever offered and counts
	// it nowhere: offered goes up, admitted does not, dropped does not, and the
	// law is short by exactly one. Its precondition (a cover entry was offered)
	// is IDENTICAL to the law's precondition, so if the verdict is not
	// "NOT EXERCISED" then this arm has fired.
	if (bMutateCoverConservation && !bCoverMutationFired)
	{
		bCoverMutationFired = true;
		UE_LOG(LogVoxelMarchIndex, Warning,
		       TEXT("Voxel cover index: MUTATION ARM ACTIVE (voxel.Cover.MutateIndex 1) -- cover "
		            "chunk (%d,%d,%d) refused and counted NOWHERE, on purpose. The cover "
		            "conservation law must now read VIOLATED. If it still reads CONSERVED, the "
		            "law is decorative and every cover funnel it has blessed is unverified."),
		       Coord.X, Coord.Y, Coord.Z);
		return false;
	}

	const FIntVector D(FMath::Abs(Coord.X - CoverBandCentreChunk.X),
	                   FMath::Abs(Coord.Y - CoverBandCentreChunk.Y),
	                   FMath::Abs(Coord.Z - CoverBandCentreChunk.Z));
	if (D.X > kCoverBandRadiusChunks || D.Y > kCoverBandRadiusChunks ||
	    D.Z > kCoverBandRadiusChunks)
	{
		++CoverDroppedOutOfBand;
		return false;
	}
	return true;
}

// THREE OUTCOMES, NOT TWO, and the third is the one that matters.
//
// offered == 0 is NOT a pass. It is "nothing was ever offered", which a leg with
// the producer off and a leg with a broken publisher produce identically. The
// ring counters read zero for exactly that reason for four legs and were read as
// evidence; this says which it is in words.
FVoxelMarchChunkIndex::ECoverConservation
FVoxelMarchChunkIndex::CheckCoverConservation(FString& OutMessage) const
{
	const int32 Offered = CoverOffered;
	const int32 Admitted = CoverAdmitted;
	const int32 Dropped = CoverDroppedOutOfBand;
	if (Offered == 0)
	{
		OutMessage = FString(
			TEXT("cover funnel NOT EXERCISED -- the index has never been offered a cover chunk. "
			     "voxel.Cover.Produce/voxel.Cover.Resident are off, the publisher is not wired to "
			     "this index, or the producer found no cover on this ground. THIS IS NOT A "
			     "CONSERVATION RESULT and the zeroes below are not evidence about anything."));
		return ECoverConservation::NotExercised;
	}
	if (Offered == Admitted + Dropped)
	{
		OutMessage = FString::Printf(
			TEXT("cover funnel CONSERVED -- offered %d == admitted %d + droppedOutOfBand %d "
			     "(resident now %d, alias collisions %d). Run voxel.Cover.MutateIndex 1 once on a "
			     "leg that offers cover: this line MUST read VIOLATED there, or the law is "
			     "decorative."),
			Offered, Admitted, Dropped, PerSlotEntries[kCoverGridSlot],
			AliasCollisions[kCoverGridSlot]);
		return ECoverConservation::Conserved;
	}
	OutMessage = FString::Printf(
		TEXT("cover funnel VIOLATED -- offered %d != admitted %d + droppedOutOfBand %d "
		     "(short by %d). Either an offer is being discarded on a path that counts nothing, or "
		     "voxel.Cover.MutateIndex is on."),
		Offered, Admitted, Dropped, Offered - Admitted - Dropped);
	return ECoverConservation::Violated;
}

// THE OBSERVED COLLISION. Called on every accepted add, before the cell is
// written. If the cell already belongs to a DIFFERENT chunk at this level, that
// chunk is about to be shadowed -- which is the hole the old span guard could
// only guess at.
void FVoxelMarchChunkIndex::NoteCellOwner(uint32 Cell, const FIntVector& Coord, int32 Slot)
{
	const int32 Level = Slot;   // named for the message below
	if (const FIntVector* Existing = CellOwner.Find(Cell))
	{
		if (*Existing != Coord)
		{
			if (Slot >= 0 && Slot < int32(kGridSlots))
			{
				++AliasCollisions[Slot];
			}
			if (!bAliasComplained)
			{
				bAliasComplained = true;
				UE_LOG(LogVoxelMarchIndex, Warning,
				       TEXT("Voxel march chunk index: chunk (%d,%d,%d) at level %d landed on "
				            "the cell already held by (%d,%d,%d). ONE OF THEM IS NOW A HOLE. "
				            "Two chunks collide only if they are %u apart on an axis, so the "
				            "resident band is wider than the grid at this level -- raise "
				            "kDimXY/kDimZ (powers of two) or narrow the ring. This is counted "
				            "per level; read voxel.March.Stats for the totals rather than "
				            "treating this one line as the magnitude."),
				       Coord.X, Coord.Y, Coord.Z, Level,
				       Existing->X, Existing->Y, Existing->Z, kDimXY);
			}
		}
	}
	CellOwner.Add(Cell, Coord);
}

void FVoxelMarchChunkIndex::Seed(const TArray<FVoxelBrickIndexEntry>& Snapshot)
{
	NumEntries = 0;
	DroppedWrongLevel = 0;
	CoverOffered = 0;
	CoverAdmitted = 0;
	CoverDroppedOutOfBand = 0;
	bCoverMutationFired = false;
	FMemory::Memzero(PerSlotEntries, sizeof(PerSlotEntries));
	FMemory::Memzero(OfferedPerLevel, sizeof(OfferedPerLevel));
	FMemory::Memzero(AliasCollisions, sizeof(AliasCollisions));
	CellOwner.Reset();
	// THE COARSE LEVEL RESTARTS WITH THE GRID IT DESCRIBES. Seed rewrites the
	// whole index, so a block count carried over from the previous world would
	// claim residency for cells the attach memzero just cleared -- Occupied set
	// over empty ground, which is only slower -- AND, worse, could leave
	// AnyAbsent CLEAR over ground that is now entirely absent, which drops a
	// bCrossedAbsentChunk the fallthrough ladder gates on. Reset here, beside
	// the loop that rebuilds it, rather than at the attach memzero, so the
	// authority for "what a seeded block count is" is the function that fills
	// it.
	ResetBlocks();
	// EVERY SLOT, INCLUDING COVER. Looping to kLevels left the cover slot's span
	// at its default and GetCumulativeCoordSpan would have read an uninitialised
	// pair -- a plausible number about a slot nothing had touched.
	for (uint32 S = 0; S < kGridSlots; ++S)
	{
		ObservedMin[S] = FIntVector(MAX_int32, MAX_int32, MAX_int32);
		ObservedMax[S] = FIntVector(MIN_int32, MIN_int32, MIN_int32);
	}
	for (const FVoxelBrickIndexEntry& E : Snapshot)
	{
		if (E.Key.Level >= 0 && E.Key.Level < kOfferBuckets)
		{
			++OfferedPerLevel[E.Key.Level];
		}
		const bool bCover = (E.Key.Level == kCoverLevel);
		if (bCover)
		{
			++CoverOffered;
		}
		const int32 Slot = GridSlotForLevel(E.Key.Level);
		if (Slot < 0)
		{
			++DroppedWrongLevel;
			continue;
		}
		const FIntVector Coord(E.Key.X, E.Key.Y, E.Key.Z);
		if (!AdmitToSlot(Coord, Slot))
		{
			continue;   // counted inside AdmitToSlot, or deliberately not (mutation arm)
		}
		if (bCover)
		{
			++CoverAdmitted;
		}
		NoteObservedSpan(Coord, Slot);
		// anySolid is not in the snapshot; it is re-derived from the record by
		// the shader, which is authoritative anyway. The bit is set here so the
		// cheap index-side reject stays available, and a chunk that turns out to
		// be all air is rejected one step later by the record instead.
		{
			const uint32 SeedCell = CellOf(Coord, Slot);
			NoteCellOwner(SeedCell, Coord, Slot);
			// The sky licence, withdrawn before the cell stops saying so.
			// STRUCTURALLY DEAD ON THIS PATH -- ResetBlocks memzeroed both the
			// grid and every sky count immediately above, so no cell here can
			// carry a mark. Called anyway, because "structurally unreachable"
			// is a claim this file has been wrong about before and the cost is
			// one predictable branch on a path that runs once per attach.
			ClearBlockCellSkyIfMarked(Cells[int32(SeedCell)], Coord, Slot);
			Cells[int32(SeedCell)] = kResidentBit | kAnySolidBit | (E.ChunkSlot & kSlotMask);
			// THE COARSE LEVEL, AT THE SAME MOMENT AND ON THE SAME CONDITION.
			// AdmitToSlot has already refused everything it means to refuse and
			// ResetBlocks left every count at zero, so this loop is the whole
			// seeded population -- one increment per resident cell, and the
			// grid the marcher skips against is built from the same admissions
			// the grid it skips into is.
			//
			// NO TRANSITION TEST HERE, unlike the ApplyDelta add: the grid was
			// memzeroed at attach, so no cell in this loop can already be
			// resident. AdmitToSlot's alias counter is what would say otherwise
			// and it is checked separately.
			NoteBlockCellResident(Coord, Slot);
		}
		++NumEntries;
		++PerSlotEntries[Slot];
	}
	// A SEED IS NEVER A DELTA. The grid was memzeroed at attach and rewritten
	// here wholesale; the delta sets know nothing about the cells the memzero
	// cleared, so a delta staging after a reseed would leave every
	// previously-resident cell alive on the GPU -- the second-PIE-session ghost
	// world this class already fixed once, reintroduced through the upload
	// path. Force the full path instead of trusting the tracking.
	bForceFullUpload = true;
	bDirty = true;
	MarkDirtyAndUpload();
}

// ---------------------------------------------------------------------------
// THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip) -- the CPU half
// ---------------------------------------------------------------------------
//
// A COUNT PER 4^3 BLOCK OF CELLS, AND TWO BITS DERIVED FROM IT. See the header
// for why one level and not a pyramid, and for why the two bits are a
// correctness requirement rather than fidelity.
//
// GAME THREAD ONLY, like every other mutable on the shadow side. The bitfields
// reach the GPU through the staging in MarkDirtyAndUpload and are consumed by
// RegisterWithBlocks in the same call that hands out the index, so there is no
// moment at which a marcher can hold one and not the other.

void FVoxelMarchChunkIndex::ResetBlocks()
{
	check(IsInGameThread());
	// SetNumUninitialized + an explicit fill, NOT SetNumZeroed, and the reason
	// is the one this file already records for Cells: this object outlives a
	// UWorld, so on the SECOND attach the arrays are already the right length
	// and SetNumZeroed only touches elements it ADDS -- it would be a silent
	// no-op and the previous world's residency would survive into the next one.
	BlockResidentCount.SetNumUninitialized(int32(kNumBlocks));
	FMemory::Memzero(BlockResidentCount.GetData(), SIZE_T(BlockResidentCount.Num()));
	BlockOccupiedWords.SetNumUninitialized(int32(kBlockWords));
	BlockAnyAbsentWords.SetNumUninitialized(int32(kBlockWords));
	BlockSkyCount.SetNumUninitialized(int32(kNumBlocks));
	FMemory::Memzero(BlockSkyCount.GetData(), SIZE_T(BlockSkyCount.Num()));
	BlockSkyTag.SetNumUninitialized(int32(kNumBlocks));
	// 0xFFFF is "no tag yet", outside the 9-bit tag range by construction, so
	// the FIRST mark into any block always takes the reset branch and the count
	// starts from a tag that is genuinely this block's rather than inheriting a
	// previous world's.
	FMemory::Memset(BlockSkyTag.GetData(), 0xFF, SIZE_T(BlockSkyTag.Num()) * sizeof(uint16));
	BlockAllSkyWords.SetNumUninitialized(int32(kBlockWords));
	// OCCUPIED CLEAR, ANYABSENT SET, and that pairing is the conservative start
	// in BOTH directions at once rather than in the obvious one. Occupied clear
	// says "descend into nothing", which cannot invent terrain. AnyAbsent set
	// says "the ground under this block is not held", which is what the walk
	// would have concluded chunk by chunk over an empty grid -- so the
	// fallthrough ladder sees exactly what it sees today rather than a quieter
	// version of it. All-zero would have been the tidy default and would have
	// silently DISARMED the ladder over an empty world.
	FMemory::Memzero(BlockOccupiedWords.GetData(),
	                 SIZE_T(BlockOccupiedWords.Num()) * sizeof(uint32));
	FMemory::Memset(BlockAnyAbsentWords.GetData(), 0xFF,
	                SIZE_T(BlockAnyAbsentWords.Num()) * sizeof(uint32));
	// ALL-SKY STARTS CLEAR, which is the conservative direction for THIS bit and
	// the opposite of AnyAbsent's. Clear means "no block is provably sky", so an
	// unseeded grid licenses no skip at all and the arm is inert until the
	// streaming side has actually proved something. Set would have claimed the
	// entire world is empty over a grid that has never been written -- every ray
	// a hole, no counter moving.
	FMemory::Memzero(BlockAllSkyWords.GetData(),
	                 SIZE_T(BlockAllSkyWords.Num()) * sizeof(uint32));
}

void FVoxelMarchChunkIndex::RefreshBlockBits(uint32 Block)
{
	// THE ONE PLACE A COUNT BECOMES A BIT. Both bits come from the same number
	// and are written together, so they cannot disagree about a block -- which
	// is the only way this pair could produce a hole that the index alone would
	// not.
	const uint32 Count = uint32(BlockResidentCount[int32(Block)]);
	const int32 Word = int32(Block >> 5);
	const uint32 Mask = 1u << (Block & 31u);
	if (Count > 0u)
	{
		BlockOccupiedWords[Word] |= Mask;
	}
	else
	{
		BlockOccupiedWords[Word] &= ~Mask;
	}
	// STRICTLY LESS THAN THE WHOLE BLOCK. Equality means every cell in the block
	// is resident and there is nothing absent to report; anything short of that
	// means the walk would have met at least one non-resident chunk in here and
	// set bCrossedAbsentChunk on it.
	if (Count < kChunksPerBlock)
	{
		BlockAnyAbsentWords[Word] |= Mask;
	}
	else
	{
		BlockAnyAbsentWords[Word] &= ~Mask;
	}
	// ---- THE SKY LICENCE, AND IT NEEDS BOTH HALVES -------------------------
	//
	// ALL 64 CELLS PROVED SKY *AND* NOTHING RESIDENT. The second half is not
	// belt-and-braces: a cell can hold a stale sky mark while a chunk is
	// resident in it, because becoming resident overwrites the dword and the
	// count is only decremented at the write sites -- so a count of 64 alone
	// could outlive the marks it counts. `Count == 0` is maintained exactly, by
	// the two residency notes, at every transition. ANDing the two makes the
	// licence at least as conservative as residency, which is the arm's floor.
	//
	// COMPUTED HERE, in the one place a count becomes a bit, for the reason the
	// other two are: three bits derived from three counters in three places
	// drift, and a drifted sky bit is not a slow frame, it is deleted terrain.
	if (BlockSkyCount.Num() == int32(kNumBlocks) &&
	    uint32(BlockSkyCount[int32(Block)]) == kChunksPerBlock && Count == 0u)
	{
		BlockAllSkyWords[Word] |= Mask;
	}
	else
	{
		BlockAllSkyWords[Word] &= ~Mask;
	}
}

// A cell just gained a valid open-sky mark for Coord.
//
// THE TAG RESET IS THE ALIAS DEFENCE. All 64 cells of a block share one tag
// (a block spans 4 chunks per axis, 128 % 4 == 0, so no block straddles a
// 128-chunk tag boundary), so a mark arriving under a DIFFERENT tag proves the
// count describes coords that are no longer the ones being asked about -- the
// camera has moved a torus period. Resetting to zero costs the block its licence
// until all 64 cells are re-marked under the new tag. That is lost benefit; the
// alternative, keeping the count, is a block licensed by a different part of the
// world, which is a hole.
void FVoxelMarchChunkIndex::NoteBlockCellSky(const FIntVector& Coord, int32 Slot)
{
	if (BlockSkyCount.Num() == 0)
	{
		return;   // before the first attach; nothing to describe yet
	}
	const uint32 Block = BlockOf(Coord, Slot);
	const uint16 Tag = uint16(AbsentTagOf(Coord));
	if (BlockSkyTag[int32(Block)] != Tag)
	{
		BlockSkyTag[int32(Block)] = Tag;
		BlockSkyCount[int32(Block)] = 0;
	}
	uint8& Count = BlockSkyCount[int32(Block)];
	// SATURATE, the same argument NoteBlockCellResident makes: a wrap past 64
	// would read as 0 and merely lose the licence, but a count that could
	// exceed 64 would need `>= 64` at the read site and then a double-count
	// would license a block that is not fully marked. Bounded here instead.
	if (Count < uint8(kChunksPerBlock))
	{
		++Count;
	}
	RefreshBlockBits(Block);
}

// A cell is ABOUT to be overwritten with something that is not this coord's
// open-sky mark. Called BEFORE the write, with the value the cell still holds --
// see the declaration for why that ordering is the safety property.
void FVoxelMarchChunkIndex::ClearBlockCellSkyIfMarked(uint32 Existing, const FIntVector& Coord,
                                                      int32 Slot)
{
	if (BlockSkyCount.Num() == 0)
	{
		return;
	}
	// A RESIDENT ENTRY IS NOT A SKY MARK. kResidentBit set means the low bits
	// are a POOL SLOT, not a reason code, and slot 3 would otherwise read as
	// kAbsentReasonOpenSky -- a resident chunk in slot 3 decrementing a count it
	// never incremented, until the count underflows and a block that is not
	// fully sky reads as one. This is the whole reason the test leads with the
	// resident bit rather than masking the reason field first.
	if ((Existing & kResidentBit) != 0u ||
	    (Existing & kAbsentReasonMask) != kAbsentReasonOpenSky)
	{
		return;
	}
	const uint32 Block = BlockOf(Coord, Slot);
	// Only OUR tag's mark counts against OUR count. An alias's mark was never
	// added to this count (NoteBlockCellSky would have reset the count when the
	// tag changed), so decrementing for it would under-count and, worse, could
	// underflow.
	if (BlockSkyTag[int32(Block)] != uint16(AbsentTagOf(Coord)))
	{
		return;
	}
	uint8& Count = BlockSkyCount[int32(Block)];
	// FLOOR AT ZERO. An underflow reads as 255, never equals kChunksPerBlock,
	// and would therefore hold the licence CLEAR forever -- inert, not wrong,
	// which is the direction this file demands but still worth not doing.
	if (Count > 0u)
	{
		--Count;
	}
	RefreshBlockBits(Block);
}

// THE NUMBER THE WHOLE ARM IS JUDGED ON, and it is swept rather than maintained.
//
// A full pass over 262,144 bytes once per perf window is nothing, and it cannot
// drift from the state it describes -- which an incrementally maintained pair of
// tallies demonstrably can, since that is precisely the failure mode the sky
// count itself has to be defended against above.
//
// THREE NUMBERS BECAUSE ONE IS UNREADABLE. `Licensed` alone says nothing:
// 100 licensed blocks is triumphant against 120 touched and meaningless against
// 120,000. `Touched` (any sky mark at all) is the denominator, and `Partial`
// (marked but not fully) is what says whether sky CLUSTERS or is scattered --
// the property three previous skip arms lacked and the one that decides whether
// a block-granular advance can amortise.
void FVoxelMarchChunkIndex::GetBlockSkyCensus(uint64& OutLicensed, uint64& OutPartial,
                                              uint64& OutTouched) const
{
	OutLicensed = 0;
	OutPartial = 0;
	OutTouched = 0;
	if (BlockSkyCount.Num() != int32(kNumBlocks))
	{
		return;   // never attached: leave all three at zero, and the caller says so
	}
	for (int32 i = 0; i < int32(kNumBlocks); ++i)
	{
		const uint32 C = uint32(BlockSkyCount[i]);
		if (C == 0u)
		{
			continue;
		}
		++OutTouched;
		if (C >= kChunksPerBlock)
		{
			++OutLicensed;
		}
		else
		{
			++OutPartial;
		}
	}
}

void FVoxelMarchChunkIndex::NoteBlockCellResident(const FIntVector& Coord, int32 Slot)
{
	if (BlockResidentCount.Num() == 0)
	{
		return;   // before the first attach; nothing to describe yet
	}
	const uint32 Block = BlockOf(Coord, Slot);
	uint8& Count = BlockResidentCount[int32(Block)];
	// SATURATE RATHER THAN WRAP, and say so out loud. A count that wrapped past
	// 64 would read as 0, clear Occupied over a block full of resident chunks,
	// and skip real ground -- the exact defect this whole change is built to
	// never produce. The callers are both transition-guarded so this cannot
	// fire; the check costs one compare against a value already in a register
	// and turns a latent hole into a bounded over-estimate.
	if (Count < uint8(kChunksPerBlock))
	{
		++Count;
	}
	RefreshBlockBits(Block);
}

void FVoxelMarchChunkIndex::NoteBlockCellAbsent(const FIntVector& Coord, int32 Slot)
{
	if (BlockResidentCount.Num() == 0)
	{
		return;
	}
	const uint32 Block = BlockOf(Coord, Slot);
	uint8& Count = BlockResidentCount[int32(Block)];
	// FLOOR AT ZERO, same argument mirrored: an underflow reads as 255, holds
	// Occupied set forever (slow, not wrong) but also holds AnyAbsent CLEAR
	// forever, which silently stops the ladder being told about absent ground.
	// That one IS a hole, so the guard is not symmetric politeness.
	if (Count > 0u)
	{
		--Count;
	}
	RefreshBlockBits(Block);
}

void FVoxelMarchChunkIndex::ApplyDelta(const FVoxelBrickIndexDelta& Delta)
{
	check(IsInGameThread());
	if (Cells.Num() == 0 || Delta.IsEmpty())
	{
		return;
	}

	// Wave 1.3: remember WHICH cells this flush writes, so MarkDirtyAndUpload
	// can stage just those instead of the whole 56 MiB grid. Tracked only
	// while the delta switch is on: with it off every staging is full anyway,
	// and an untended set would grow for the life of the process. Read once --
	// console commands execute on this thread, so the value cannot flip
	// between here and the MarkDirtyAndUpload this call ends with.
	const bool bTrackDelta = CVarVoxelMarchIndexDeltaUpload.GetValueOnGameThread() != 0;
	// Phase 2: while the GPU-resident mode is on, this flush's changes are
	// ALSO collected as publish entries -- coord + gridSlot + slot, the form
	// the kernel derives cells from -- alongside the cell tracking above. Both
	// are kept because the ladder can still refuse the publish leaf (pending
	// CPU pairs, oversized flush) and fall back to paths that need the cell
	// sets; double bookkeeping at ~145 cells per measured flush is noise.
	const bool bGpuPublish =
		bTrackDelta && CVarVoxelMarchIndexGpuResident.GetValueOnGameThread() != 0;
	// voxel.March.HoleStats 2: an eviction writes WHY the cell is empty
	// instead of a bare 0, so the marcher's uncovered breakdown can tell
	// "evicted" from "never admitted". Read once per flush, same rule as
	// bTrackDelta above.
	const bool bAnnotateAbsent = AreAbsentMarksArmed();

	// REMOVED BEFORE ADDED, AND IT IS NOT A STYLE CHOICE. Both halves can name
	// the SAME SLOT in one delta, because a slot freed by an eviction can be
	// re-allocated to a different chunk inside the same flush. Applied the other
	// way round the index ends up mapping the OLD key to a slot that now holds
	// the NEW chunk -- which is not a missing chunk, it is one chunk's bricks
	// drawn at another chunk's coordinates, and it looks like terrain.
	const double RemovedStart = FPlatformTime::Seconds();
	for (const FVoxelBrickIndexEntry& E : Delta.Removed)
	{
		const int32 RemSlot = GridSlotForLevel(E.Key.Level);
		if (RemSlot < 0)
		{
			continue;
		}
		const uint32 Cell = CellOf(FIntVector(E.Key.X, E.Key.Y, E.Key.Z), RemSlot);
		// Phase 2: emit the removal for the GPU's guarded clear UNCONDITIONALLY
		// (level-filtered only). The guard below -- resident AND still naming
		// this slot -- is executed independently by BOTH sides: here against
		// the shadow, in the kernel against the GPU buffer. In-sync buffers
		// give the same verdict, so emission is not the filter and must not
		// be: an emit gated on the SHADOW's verdict would encode "the shadow
		// already knew", which is exactly the derived-not-verified join this
		// gate exists to check.
		if (bGpuPublish)
		{
			GpuPublishRemoves.Add(uint32(E.Key.X));
			GpuPublishRemoves.Add(uint32(E.Key.Y));
			GpuPublishRemoves.Add(uint32(E.Key.Z));
			GpuPublishRemoves.Add(uint32(RemSlot));
			GpuPublishRemoves.Add(E.ChunkSlot & kSlotMask);
		}
		// Only clear the cell if it still names THIS slot. A cell already
		// re-pointed by an earlier Added in the same batch must not be undone.
		// THE RESIDENT BIT IS PART OF THE MATCH, AND SLOT 0 IS WHY.
		//
		// A cleared cell reads 0, and `0 & kSlotMask` is 0 -- which is a LEGAL
		// SLOT. So a removal naming slot 0 matched any empty cell and decremented
		// NumEntries and PerSlotEntries for a chunk that was never in the grid.
		// The counters never reset outside Seed, so the drift is permanent and
		// the visible symptom is an entry count that disagrees with the pool's
		// residency by a slowly growing amount -- read as a streaming problem.
		//
		// THIS WAS UNREACHABLE UNTIL NOW AND IS NOT ANY MORE, which is the whole
		// reason to state it here. RemoveChunk had NO CALLER EVER (its own header
		// says GetEvictions() reads zero only because nothing calls it), so
		// Delta.Removed was always empty. Two things changed together: the detail
		// ring now releases cover through RemoveChunk on every group release, and
		// AdmitToSlot can REFUSE an entry the pool still considers resident and
		// will later emit a Removed for -- an entry whose cell this index never
		// wrote. That second case is exactly the false match above.
		//
		// A slot is unique among resident chunks, so slot equality plus the
		// resident bit cannot collide: no other resident chunk can be holding the
		// slot being retired.
		const uint32 Existing = Cells[int32(Cell)];
		if ((Existing & kResidentBit) != 0u &&
		    (Existing & kSlotMask) == (E.ChunkSlot & kSlotMask))
		{
			const FIntVector RemCoord(E.Key.X, E.Key.Y, E.Key.Z);
			// The eviction annotation (voxel.March.HoleStats 2): the cell was
			// resident and is now being cleared, which is the ONE moment
			// "evicted" is a fact rather than an inference -- so it is written
			// here and nowhere else. The tag pins it to THIS coord: a chunk
			// 128 cells away that later maps to this cell reads a mismatched
			// tag and classifies as never-admitted, which for it is the truth.
			// Ring levels only -- an absent COVER chunk is the normal state
			// ("no cover here" stores nothing), and annotating cover would
			// bury the ring signal. Disarmed this is the bare 0 it always was,
			// so a control run's index stream is byte-identical.
			const bool bAnnotateThis = bAnnotateAbsent && RemSlot != int32(kCoverGridSlot);
			// The sky licence. `Existing` is resident here so this is a no-op by
			// the resident test -- the decrement for this cell was taken when it
			// BECAME resident, in the Added loop below. Kept for the same reason
			// the seed site keeps it: the invariant is "every overwrite of a
			// cell consults the licence first", and an exception is how the next
			// writer added to this function forgets.
			ClearBlockCellSkyIfMarked(Existing, RemCoord, RemSlot);
			Cells[int32(Cell)] =
				bAnnotateThis ? MakeAbsentEntry(kAbsentReasonEvicted, RemCoord) : 0u;
			if (bAnnotateThis)
			{
				++AbsentEvictedMarks;
			}
			if (bTrackDelta)
			{
				DeltaPendingCells.Add(Cell);
			}
			// THE COARSE LEVEL, INSIDE THE SAME GUARD THE ENTRY COUNTERS ARE
			// INSIDE. This branch is the one place a cell stops being resident,
			// and it is already slot-equality guarded against the double-count
			// that once drifted NumEntries permanently. The block count is the
			// same kind of quantity and gets the same protection by sitting
			// here rather than beside the emit above.
			NoteBlockCellAbsent(RemCoord, RemSlot);
			--NumEntries;
			--PerSlotEntries[RemSlot];
			// Only drop the ownership record if it still names THIS chunk; a
			// cell re-pointed by an earlier Added in the same batch belongs to
			// the new owner, not to the one being retired.
			if (const FIntVector* Owner = CellOwner.Find(Cell))
			{
				if (*Owner == RemCoord)
				{
					CellOwner.Remove(Cell);
				}
			}
		}
	}
	const double AddedStart = FPlatformTime::Seconds();
	ApplyDeltaMs.RemovedMs += (AddedStart - RemovedStart) * 1000.0;
	ApplyDeltaMs.RemovedCount += Delta.Removed.Num();
	for (const FVoxelBrickIndexEntry& E : Delta.Added)
	{
		if (E.Key.Level >= 0 && E.Key.Level < kOfferBuckets)
		{
			++OfferedPerLevel[E.Key.Level];
		}
		const bool bCoverAdd = (E.Key.Level == kCoverLevel);
		if (bCoverAdd)
		{
			++CoverOffered;
		}
		const int32 AddSlot = GridSlotForLevel(E.Key.Level);
		if (AddSlot < 0)
		{
			++DroppedWrongLevel;
			continue;
		}
		const FIntVector AddCoord(E.Key.X, E.Key.Y, E.Key.Z);
		if (!AdmitToSlot(AddCoord, AddSlot))
		{
			continue;
		}
		if (bCoverAdd)
		{
			++CoverAdmitted;
		}
		NoteObservedSpan(AddCoord, AddSlot);
		const uint32 Cell = CellOf(AddCoord, AddSlot);
		NoteCellOwner(Cell, AddCoord, AddSlot);
		if ((Cells[int32(Cell)] & kResidentBit) == 0u)
		{
			++NumEntries;
			++PerSlotEntries[AddSlot];
			// THE COARSE LEVEL, ON THE INDEX'S OWN TRANSITION TEST AND NOT ON A
			// SECOND ONE. A re-add to a cell that is already resident (a new
			// slot for the same coord, or an alias) must not increment: the
			// count is RESIDENT CELLS, and counting the same cell twice would
			// leave Occupied set after the cell was finally cleared -- ground
			// claimed to exist that does not, which costs only time -- and
			// leave AnyAbsent clear over a block with absent cells in it, which
			// drops a bCrossedAbsentChunk and is a hole. Reading the index's
			// own guard rather than adding one is what keeps the two counts
			// from ever disagreeing about what a transition is.
			NoteBlockCellResident(AddCoord, AddSlot);
		}
		// THE SKY LICENCE, AND THIS IS THE SITE THAT MATTERS MOST.
		//
		// A cell the streamer marked as open sky can be admitted later -- the
		// anchor moves, the memo re-derives against a tile that has since become
		// resident, and the admitted top rises over ground the analytic bound
		// had covered. That is the one transition where a stale licence would
		// let the marcher skip a block containing a chunk the pool now HOLDS.
		//
		// OUTSIDE the transition test above deliberately: that test guards a
		// RESIDENCY count, and a re-add to an already-resident cell must not
		// touch it. The sky count is a different question -- "does this cell
		// still say sky" -- and the answer must be reconciled on every write,
		// re-add included, or a cell that was marked between two adds keeps its
		// contribution forever.
		ClearBlockCellSkyIfMarked(Cells[int32(Cell)], AddCoord, AddSlot);
		Cells[int32(Cell)] = kResidentBit | kAnySolidBit | (E.ChunkSlot & kSlotMask);
		if (bTrackDelta)
		{
			DeltaPendingCells.Add(Cell);
		}
		// Phase 2: additions are emitted only when ADMITTED -- admission
		// (level mapping, cover band, the mutation arm) is index POLICY and
		// stays CPU-side with its counters; what the GPU derives is the cell
		// and the value, not the decision. Keyed by the CPU's cell so the
		// LAST add to a cell in one flush wins, matching the sequential
		// shadow apply this loop just performed -- and so no two publish
		// threads write one cell. (Keying by the CPU cell while the kernel
		// derives its own is deliberate: if the two spellings ever drift the
		// dedup is grouped wrong but the writes go where the KERNEL says, and
		// the verify hash catches the drift.)
		if (bGpuPublish)
		{
			FGpuPublishAdd& Add = GpuPublishAdds.FindOrAdd(Cell);
			Add.Coord = AddCoord;
			Add.GridSlot = AddSlot;
			Add.Slot = E.ChunkSlot & kSlotMask;
		}
	}

	const double UploadStart = FPlatformTime::Seconds();
	ApplyDeltaMs.AddedMs += (UploadStart - AddedStart) * 1000.0;
	ApplyDeltaMs.AddedCount += Delta.Added.Num();

	bDirty = true;
	MarkDirtyAndUpload();
	// WHAT uploadMs MEASURES NOW DEPENDS ON THE PATH. Full path (the default):
	// paid once per flush regardless of how many entries moved -- a 56 MiB
	// `Staged = Cells` memcpy, plus the whole-grid FNV when the hash is on;
	// this is the term that measured 3,146-3,190 ms per 5 s window and
	// motivated the delta path. Delta path (voxel.March.IndexDeltaUpload 1):
	// proportional to cells changed since the last consumed staging --
	// typically ~9,500 pairs, ~74 KiB. If uploadMs still dominates WITH the
	// delta switch on, either the hash/verify cvars are on (whole-grid FNV,
	// size-independent) or the fallback counters in GetUploadStats() will say
	// the full path is running anyway, and why.
	ApplyDeltaMs.UploadMs += (FPlatformTime::Seconds() - UploadStart) * 1000.0;
}

// ---------------------------------------------------------------------------
// The absent-annotation writer (voxel.March.HoleStats 2)
// ---------------------------------------------------------------------------

bool FVoxelMarchChunkIndex::AreAbsentMarksArmed() const
{
	// The GPU publish kernel (Phase 2) clears cells to LITERAL 0 on the GPU;
	// an annotated shadow would fail the delta-verify hash on the very next
	// gated flush, so the writer stands down whenever that mode is on. The
	// perf line prints which gate closed it -- a disarmed writer must be
	// readable as disarmed, not as "nothing pending, nothing evicted".
	if (CVarVoxelMarchIndexGpuResident.GetValueOnGameThread() != 0)
	{
		return false;
	}
	// HoleStatsLevel >= 2 is the breakdown arm; below it the annotations would
	// be dead weight in the delta stream AND a control-leg divergence -- the
	// requirement is that voxel.March.HoleStats 0 legs stream a byte-identical
	// index, and the cheapest proof is to never write.
	return VoxelMarchGetArm().HoleStatsLevel >= 2;
}

void FVoxelMarchChunkIndex::NoteChunkAdmitted(const FIntVector& Coord, int32 Level)
{
	check(IsInGameThread());
	if (Cells.Num() == 0 || !AreAbsentMarksArmed())
	{
		return;
	}
	const int32 Slot = GridSlotForLevel(Level);
	if (Slot < 0 || Slot == int32(kCoverGridSlot))
	{
		return; // rings only -- see the header
	}
	const uint32 Cell = CellOf(Coord, Slot);
	const uint32 Existing = Cells[int32(Cell)];
	if ((Existing & kResidentBit) != 0u)
	{
		// Already resident (re-admission of parked/adopted geometry). Nothing
		// to explain: a ray cannot miss a chunk the index holds, and if the
		// pool later drops it the eviction annotation takes over.
		return;
	}
	const uint32 Value = MakeAbsentEntry(kAbsentReasonPending, Coord);
	if (Existing == Value)
	{
		// RecomputeDesiredSet re-scans candidates every anchor move; the
		// second and later admissions of the same still-pending chunk must
		// not re-dirty the cell or the delta stream doubles for free.
		return;
	}
	// The sky licence: this cell is about to say "pending", not "sky". A chunk
	// on its way IN is the last thing a skip may advance through.
	ClearBlockCellSkyIfMarked(Existing, Coord, Slot);
	Cells[int32(Cell)] = Value;
	if (CVarVoxelMarchIndexDeltaUpload.GetValueOnGameThread() != 0)
	{
		DeltaPendingCells.Add(Cell);
	}
	bAbsentMarksPending = true;
	++AbsentPendingMarks;
}

void FVoxelMarchChunkIndex::NoteChunkNoLongerAdmitted(const FIntVector& Coord, int32 Level)
{
	check(IsInGameThread());
	if (Cells.Num() == 0 || !AreAbsentMarksArmed())
	{
		return;
	}
	const int32 Slot = GridSlotForLevel(Level);
	if (Slot < 0 || Slot == int32(kCoverGridSlot))
	{
		return;
	}
	const uint32 Cell = CellOf(Coord, Slot);
	const uint32 Existing = Cells[int32(Cell)];
	// Clear ONLY a pending annotation that names exactly this coord. Resident
	// cells belong to the delta path; an evicted annotation is history this
	// cancellation did not create; an alias's annotation is not ours to touch.
	// Without this narrowing, queue-cap truncation (which drops the FARTHEST
	// admissions first) would leave "pending" painted over ground the
	// streaming system has in fact walked away from, and the throughput bucket
	// would absorb a coverage problem -- the exact conflation the three-way
	// split exists to remove.
	if ((Existing & kResidentBit) != 0u ||
	    (Existing & kAbsentReasonMask) != kAbsentReasonPending ||
	    ((Existing >> kAbsentTagShift) & kAbsentTagMask) != AbsentTagOf(Coord))
	{
		return;
	}
	// The sky licence. Existing is a PENDING annotation on this path (the guard
	// above proved it), so this is a no-op by the reason test -- the decrement
	// was taken when the pending mark overwrote the sky one. Present for the
	// invariant, not for an effect.
	ClearBlockCellSkyIfMarked(Existing, Coord, Slot);
	Cells[int32(Cell)] = 0u;
	if (CVarVoxelMarchIndexDeltaUpload.GetValueOnGameThread() != 0)
	{
		DeltaPendingCells.Add(Cell);
	}
	bAbsentMarksPending = true;
}

// ---------------------------------------------------------------------------
// THE OPEN-SKY WRITER (voxel.Stream.SkyMark)
// ---------------------------------------------------------------------------
//
// ITS OWN ARMING PREDICATE, and the difference from AreAbsentMarksArmed() is
// the whole reason this feature exists rather than being a fourth reason code
// on the old writer.
//
// AreAbsentMarksArmed() requires voxel.March.HoleStats >= 2. That is correct
// for the pending/evicted annotations -- they are a diagnostic, and a control
// leg's index stream must stay byte-identical. But it is exactly what made the
// absent-reason bits useless to the marcher: IN EVERY PERF RUN, EVERY ABSENT
// CELL READS NONE. A fast path cannot be built on a fact that disappears
// whenever the clock is running. So this writer is armed by its own streaming
// switch and is indifferent to HoleStats.
//
// IT KEEPS ONE GATE, AND THAT ONE IS CORRECTNESS. With
// voxel.March.IndexGpuResident on, the Phase 2 publish kernel writes literal 0
// into cells on the GPU while the shadow would hold a mark -- a guaranteed
// delta-verify FAIL, the same collision the older annotations stand down for.
// Standing down is COUNTED (the caller's refusedOther bucket, and the perf
// line names this gate by name), because a disarmed writer must read as
// disarmed and never as "no sky anywhere".
bool FVoxelMarchChunkIndex::IsOpenSkyWriterArmed() const
{
	// THE SAME cvar OBJECT AreAbsentMarksArmed() reads, not a
	// FindConsoleVariable("voxel.March.IndexGpuResident") by name. A string
	// lookup that misses returns null and the gate silently opens -- which
	// would let the writer run in exactly the mode whose publish kernel clears
	// these cells to 0, and the failure would surface as a delta-verify hash
	// mismatch a long way from here.
	if (CVarVoxelMarchIndexGpuResident.GetValueOnGameThread() != 0)
	{
		return false;
	}
	return Cells.Num() > 0;
}

// A cell the sky trim proved is above the terrain surface and declined to
// admit. Modelled line for line on NoteChunkAdmitted above -- same slot
// resolution, same resident refusal, same idempotence guard, same
// DeltaPendingCells / bAbsentMarksPending bookkeeping -- so the two writers
// cannot drift into different notions of what a cell write costs.
//
// THE ORDERING THAT KEEPS A MARK FROM OUTLIVING ITS JUSTIFICATION, stated
// explicitly because getting it wrong is a hole and holes are what the owner
// reports on sight:
//
//  * A MARK NEVER LANDS ON A CELL SOMETHING ELSE OWNS. kResidentBit set means
//    the pool holds a chunk here -- either this coord or a torus alias 128
//    cells away -- and the write is refused outright. A pending or evicted
//    annotation is also refused: pending means a chunk is on its way into this
//    cell and the mark would be overwritten within the flush anyway, while
//    evicted is history that belongs to a chunk, not to the sky.
//
//  * EVERY STREAMING TRANSITION DESTROYS A MARK, at the transition, not after
//    it. Admission writes kResidentBit | slot through ApplyDelta's Added loop;
//    NoteChunkAdmitted writes the PENDING annotation; eviction writes EVICTED
//    or a bare 0. All three overwrite the whole dword, so the instant streaming
//    decides anything else about this cell the sky claim is gone. There is no
//    ordering in which a mark survives the decision that justified it, because
//    the mark is not a separate bit that must be cleared -- IT IS THE REASON
//    FIELD ITSELF, and any other reason evicts it by writing.
//
//  * AND THE RE-ADMISSION EDGE ERRS UNMARKED. If a marked cell is admitted, the
//    admission wins and the cell is resident. If the admission is later
//    cancelled, NoteChunkNoLongerAdmitted clears to 0 = NONE = NOT SKY, and the
//    mark is only restored when the streaming pass RE-DERIVES the proof from
//    scratch. Unmarked costs time; wrongly marked deletes terrain, so both
//    edges resolve to the expensive side rather than the invisible one.
//
// THE TAG IS WHAT MAKES THE ALIAS CASE SAFE IN THE OTHER DIRECTION. A cell can
// hold a valid open-sky mark stamped for a coord 128 cells away; the marcher's
// read compares AbsentTagOf(wanted) and treats a mismatch as NOT SKY. So the
// worst an alias can do is cost a walk, which is the direction every doubt in
// this file resolves to.
FVoxelMarchChunkIndex::EOpenSkyMark FVoxelMarchChunkIndex::NoteChunkOpenSky(const FIntVector& Coord,
                                                                            int32 Level)
{
	check(IsInGameThread());
	if (!IsOpenSkyWriterArmed())
	{
		return EOpenSkyMark::RefusedOther;
	}
	const int32 Slot = GridSlotForLevel(Level);
	if (Slot < 0 || Slot == int32(kCoverGridSlot))
	{
		return EOpenSkyMark::RefusedOther; // rings only -- see the header
	}
	const uint32 Cell = CellOf(Coord, Slot);
	const uint32 Existing = Cells[int32(Cell)];
	if ((Existing & kResidentBit) != 0u)
	{
		// The pool holds a chunk in this cell. Never paint over it: at best
		// this coord's own chunk arrived between the trim and here, at worst it
		// is a torus alias and marking would delete a DIFFERENT column's ground.
		return EOpenSkyMark::RefusedResident;
	}
	const uint32 Value = MakeAbsentEntry(kAbsentReasonOpenSky, Coord);
	if (Existing == Value)
	{
		// Already marked, for this exact coord. RecomputeDesiredSet re-scans
		// every un-admitted candidate on anchor movement, so without this the
		// delta stream would carry the whole marked band again on every pass --
		// the same trap NoteChunkAdmitted guards, and a much larger one here
		// because a column offers a whole band rather than one cell.
		return EOpenSkyMark::RefusedOther;
	}
	if ((Existing & kAbsentReasonMask) != kAbsentReasonNone)
	{
		// A pending or evicted annotation, or an alias's sky mark. All three
		// belong to a chunk transition this function did not cause and must not
		// erase -- pending in particular is a chunk on its way IN, and
		// converting it to "sky" would tell the marcher to skip ground the
		// streamer is actively fetching.
		return EOpenSkyMark::RefusedOther;
	}
	Cells[int32(Cell)] = Value;
	// AFTER the write, and the asymmetry against ClearBlockCellSkyIfMarked's
	// before-the-write rule is the point: a licence is GRANTED only once the
	// cell actually says so, and WITHDRAWN before the cell stops saying so. Both
	// orderings put the window on the conservative side -- there is no instant
	// at which a block claims all-sky over a cell that does not carry the mark.
	NoteBlockCellSky(Coord, Slot);
	if (CVarVoxelMarchIndexDeltaUpload.GetValueOnGameThread() != 0)
	{
		DeltaPendingCells.Add(Cell);
	}
	bAbsentMarksPending = true;
	++OpenSkyMarks;
	return EOpenSkyMark::Written;
}

void FVoxelMarchChunkIndex::FlushAbsentMarks()
{
	check(IsInGameThread());
	if (!bAbsentMarksPending)
	{
		return;
	}
	bAbsentMarksPending = false;
	// One staging per admission PASS, not per admission -- RecomputeDesiredSet
	// admits in bursts (~8 calls/second) and calls this once at its tail. The
	// upload itself rides the same machinery as every other cell write:
	// MarkDirtyAndUpload stages the delta pairs (or the full grid), Register()
	// consumes them, and the delta-verify gate covers these cells exactly as
	// it covers residency writes.
	bDirty = true;
	MarkDirtyAndUpload();
}

void FVoxelMarchChunkIndex::MarkDirtyAndUpload()
{
	if (!bDirty)
	{
		return;
	}
	bDirty = false;
	++Uploads;

	// ---- THE COARSE OCCUPANCY LEVEL, STAGED ON EVERY FLUSH -----------------
	//
	// HERE, BEFORE THE PATH LADDER, AND UNCONDITIONALLY. Every route out of
	// this function publishes SOMETHING -- a full snapshot, a pair scatter, or
	// a Phase 2 publish command -- and every one of them can change which cells
	// are resident, so every one of them owes the marcher a matching coarse
	// level. Staging above the ladder is what makes that true without a second
	// list of which branches need it; the ladder has already grown three leaves
	// and would grow a fourth without this one being updated.
	//
	// A WHOLE 64 KiB SNAPSHOT AND NOT A DELTA. The delta machinery below exists
	// because the index is 64 MiB and a flush moves ~9,500 cells; the bitfields
	// are 32 KiB each, so the whole thing is smaller than one flush's pair list
	// and there is nothing to save. It also buys the property that matters: a
	// complete snapshot CANNOT be missing a block that the index staging
	// carries, which is the failure a block-level delta would have to be proved
	// against.
	//
	// SNAPSHOTTED FROM THE SHADOW ON THIS THREAD, for the reason `Staged =
	// Cells` is: by the time the render thread reads this, the game thread may
	// be part-way through the next flush. Under the same lock the pair list
	// uses, because these arrays are assigned (and so may reallocate) rather
	// than overwritten in place.
	//
	// REFRESHED, NOT CONSUMED. The render side never clears this; it compares
	// generations. That is the whole fix for the one-shot staging that spent a
	// static leg on the all-ones fallback -- see the mirror's declaration.
	{
		FScopeLock Lock(&DeltaStageLock);
		BlockMirrorOccupied = BlockOccupiedWords;
		BlockMirrorAnyAbsent = BlockAnyAbsentWords;
		BlockMirrorAllSky = BlockAllSkyWords;
		// BUMPED WITH THE COPY, UNDER THE SAME LOCK, so a render thread that
		// sees this generation is guaranteed to see the bytes that go with it.
		++BlockShadowGeneration;
	}

	const bool bDeltaSwitch = CVarVoxelMarchIndexDeltaUpload.GetValueOnGameThread() != 0;
	const bool bVerifyWanted =
		bDeltaSwitch && CVarVoxelMarchIndexDeltaVerify.GetValueOnGameThread() != 0;

	// FNV-1a over the whole grid. Order-dependent by construction, which is what
	// is wanted: two grids holding the same chunks in different CELLS are
	// different worlds to a ray, and a commutative checksum would call them
	// equal. 56 MiB of adds once per dirty frame, on the game thread, and only
	// while the volume is still moving. Also computed when the delta VERIFY
	// gate wants it: the hash of Cells at staging time is exactly the hash a
	// FULL upload of this state would carry, which is what the readback on the
	// other side is compared against.
	uint64 HashNow = 0;
	bool bHashNowValid = false;
	// The verify-only consumer is THROTTLED (see the PeriodMs cvar for the
	// 25-30%-of-wall measurement that forced this); the comparator is not --
	// it reads the hash every frame, so a stale value there is a wrong
	// instrument, where a skipped verify sample is just a smaller sample.
	bool bWantHashNow = bContentHashEnabled;
	if (!bWantHashNow && bVerifyWanted)
	{
		const int32 PeriodMs = CVarVoxelMarchIndexDeltaVerifyPeriodMs.GetValueOnGameThread();
		if (PeriodMs <= 0)
		{
			bWantHashNow = true;
		}
		else
		{
			// Game thread only, like every mutable on this path.
			static double LastVerifyHashSeconds = 0.0;
			const double NowSeconds = FPlatformTime::Seconds();
			if (NowSeconds - LastVerifyHashSeconds >= double(PeriodMs) / 1000.0)
			{
				LastVerifyHashSeconds = NowSeconds;
				bWantHashNow = true;
			}
		}
	}
	if (bWantHashNow)
	{
		// THROUGH HashableCell, WHICH DROPS BIT 30, and that is a cost this
		// change had to land WITH the feature rather than after it. Once the
		// GPU refine pass clears anySolid, the shadow (which always writes 1)
		// and the GPU buffer legitimately differ in exactly that bit, and an
		// unmasked FNV would report IndexDeltaVerify FAILED on every single
		// sample -- a correctness gate that cries wolf is a correctness gate
		// that gets turned off. Masking here and in PollDeltaVerify's readback
		// hash keeps the gate covering residency, slot and every absent-reason
		// bit, and hands the one bit it stops covering to the refine kernel's
		// own audit arm. Precedent: AreAbsentMarksArmed() already stands the
		// annotation writers down rather than let them indict a mode they were
		// not asked about.
		uint64 Hash = 1469598103934665603ull;
		for (uint32 V : Cells)
		{
			Hash ^= uint64(HashableCell(V));
			Hash *= 1099511628211ull;
		}
		ContentHash = Hash;
		HashNow = Hash;
		bHashNowValid = true;
	}

	// BYTE ACCOUNTING, SETTLED BEFORE EITHER PATH STAGES. If the previous
	// staging is still waiting for Register(), the one built now REPLACES it
	// -- only one crosses to the GPU -- so its bytes must come back out of
	// UploadBytes. If it WAS consumed, its bytes crossed and stay counted.
	// Reading the consumed flags here races Register() clearing them on the
	// render thread; a stale "unconsumed" subtracts one staging that did in
	// fact upload, an undercount of at most one staging per race -- noted
	// rather than fenced, because the counter is a diagnostic and the flags
	// follow the same handoff discipline the full path has always used.
	{
		bool bPrevUnconsumed = bStagedValid;
		{
			FScopeLock Lock(&DeltaStageLock);
			bPrevUnconsumed = bPrevUnconsumed || bStagedDeltaValid;
		}
		if (!bPrevUnconsumed)
		{
			PendingStagedBytes = 0;
		}
	}

	// -----------------------------------------------------------------------
	// CHOOSE THE PATH. Everything below stages data for Register() to fold
	// into a graph; nothing here touches the GPU. The full path is the
	// default and byte-identical to the pre-delta code.
	// -----------------------------------------------------------------------
	//
	// The fallback ladder, in precedence order, each counted so a leg can be
	// read from GetUploadStats() alone:
	//   seed     -- Seed() rewrote the grid's meaning; the delta sets know
	//               nothing about cells the attach memzero cleared.
	//   lost     -- a Phase 2 publish command dropped its entries; a full
	//               staging is the heal (see FullBecauseLost).
	//   first    -- no full upload has been staged since the last Detach, so
	//               there is nothing on the GPU to patch.
	//   pending  -- a FULL staging is already waiting for Register(). Re-stage
	//               full: the fresh snapshot absorbs this flush too, and a
	//               delta staged beside a pending full has two answers racing
	//               for the same buffer.
	//   large    -- the dirty set crossed voxel.March.IndexDeltaMaxCells; see
	//               that cvar's comment for why ~7% of the grid is the line.
	// Below the ladder, voxel.March.IndexGpuResident chooses between the two
	// incremental arms: the Phase 2 publish (GPU derives and writes the cells,
	// enqueued behind this flush's brick writes) or the CPU-staged pair
	// scatter (consumed by the next Register()). Both are checked by the same
	// verify gate against the same shadow.
	bool bStageFull = true;
	if (bDeltaSwitch)
	{
		if (bForceFullUpload)
		{
			++UploadStats.FullBecauseSeed;
		}
		else if (bGpuPublishLost.exchange(false))
		{
			// A publish command dropped its entries (no buffer to patch --
			// structurally unreachable, but if it happened the GPU is now
			// missing cells the shadow holds). A full staging is the heal:
			// it carries every cell's current value.
			++UploadStats.FullBecauseLost;
		}
		else if (!bDeltaBaseEstablished)
		{
			++UploadStats.FullBecauseFirst;
		}
		else if (bStagedValid)
		{
			// Reading bStagedValid here races Register() clearing it on the
			// render thread; a stale TRUE only means one extra full staging,
			// which is the safe direction. A stale FALSE cannot happen before
			// consumption: only the render thread clears it, and only after
			// QueueBufferUpload has already copied the staged data out.
			++UploadStats.FullBecausePending;
		}
		else
		{
			// Clamped to what one dispatch can address: the scatter is 1-D and
			// D3D12 caps a dispatch dimension at 65,535 groups -- 65,535 x 64
			// threads = 4,194,240 pairs. A cvar raised past that would not make
			// the delta path handle more cells; it would silently DROP the tail
			// of the pair list, which is the silently-wrong-cell failure this
			// whole feature is built to never produce. The publish leaf shares
			// the bound: its two dispatch ranges are each below the total.
			const int32 MaxCells = FMath::Clamp(
				CVarVoxelMarchIndexDeltaMaxCells.GetValueOnGameThread(), 0, 65535 * 64);

			// ---- PHASE 2 LEAF: publish instead of staging ------------------
			//
			// Taken only when nothing older is still on its way up. If a
			// CPU-staged pair list is waiting for Register(), publishing NOW
			// would let those OLDER values overwrite these cells when the
			// pairs finally scatter -- stale terrain, no error -- so the
			// flush falls back to the CPU path, which merges. That happens
			// once per mid-flight ON-flip and then the sets stay drained.
			bool bTryPublish = CVarVoxelMarchIndexGpuResident.GetValueOnGameThread() != 0;
			if (bTryPublish)
			{
				bool bPendingCpuPairs = false;
				{
					FScopeLock Lock(&DeltaStageLock);
					bPendingCpuPairs = bStagedDeltaValid;
				}
				if (bPendingCpuPairs)
				{
					++UploadStats.GpuFellBackPendingCpu;
					bTryPublish = false;
				}
				// Belt: the publish scratch must cover every pending cell. It
				// does whenever ApplyDelta ran with the mode on; if the cvar
				// flipped between tracking and here (same thread, so only via
				// an exotic reentrancy), an empty scratch against non-empty
				// pending cells must NOT consume them -- fall back instead.
				if (bTryPublish && GpuPublishRemoves.Num() == 0 && GpuPublishAdds.Num() == 0 &&
				    DeltaPendingCells.Num() > 0)
				{
					bTryPublish = false;
				}
			}
			if (bTryPublish)
			{
				const int32 TotalEntries =
					GpuPublishRemoves.Num() / kPublishEntryDwords + GpuPublishAdds.Num();
				if (TotalEntries > MaxCells)
				{
					++UploadStats.FullBecauseLarge;
				}
				else
				{
					// These cells go up with the publish command; nothing is
					// left pending for the CPU staging machinery.
					DeltaPendingCells.Reset();
					DeltaStagedCells.Reset();
					EnqueueGpuPublish(bVerifyWanted && bHashNowValid, HashNow);
					bStageFull = false;
				}
			}
			else
			{
			// Merge this flush's dirty cells into the set the staged pairs
			// must cover. If the PREVIOUS pair list was already consumed, its
			// cells are on the GPU and the covered set restarts from empty; if
			// it was NOT consumed, the new list must cover the union --
			// dropping a not-yet-uploaded cell here is a silently wrong index
			// entry, the one failure this feature must never produce.
			{
				FScopeLock Lock(&DeltaStageLock);
				if (!bStagedDeltaValid)
				{
					DeltaStagedCells.Reset();
				}
			}
			for (uint32 C : DeltaPendingCells)
			{
				DeltaStagedCells.Add(C);
			}
			DeltaPendingCells.Reset();

			if (DeltaStagedCells.Num() > MaxCells)
			{
				++UploadStats.FullBecauseLarge;
			}
			else
			{
				// ---- THE DELTA STAGING -------------------------------------
				// Values are snapshotted from Cells HERE, on the game thread,
				// for the same reason the full path snapshots (`Staged =
				// Cells`) instead of letting Register() read Cells: by the
				// time the render thread consumes this, the game thread may be
				// mid-way through the next flush's writes. The set is keyed by
				// cell, so each cell appears ONCE in the pair list and the
				// scatter dispatch has no write-write races.
				FScopeLock Lock(&DeltaStageLock);
				const int32 NumCells = DeltaStagedCells.Num();
				StagedDeltaPairs.Reset();
				StagedDeltaPairs.Reserve(NumCells * 2);
				for (uint32 C : DeltaStagedCells)
				{
					StagedDeltaPairs.Add(C);
					StagedDeltaPairs.Add(Cells[int32(C)]);
				}
				bStagedDeltaValid = true;
				if (bVerifyWanted && bHashNowValid)
				{
					StagedContentHash = HashNow;
					bStagedHashValid = true;
				}
				else
				{
					bStagedHashValid = false;
				}

				// Bytes: replace, don't accumulate, a staging that was never
				// consumed -- GetUploadBytes is the number that decides
				// whether this feature worked, so it must count bytes that
				// cross, not bytes that were prepared and superseded.
				UploadBytes -= PendingStagedBytes;
				PendingStagedBytes = uint64(StagedDeltaPairs.Num()) * sizeof(uint32);
				UploadBytes += PendingStagedBytes;

				++UploadStats.DeltaUploads;
				UploadStats.DeltaCellsStaged += uint64(NumCells);
				UploadStats.LastStagedCells = uint32(NumCells);
				bStageFull = false;
			}
			}   // (CPU staging arm of the Phase 2 leaf split)
		}
	}

	if (!bStageFull)
	{
		// Publish scratch that was not consumed by the publish leaf (mode off,
		// or a fallback took the CPU staging arm) is DISCARDED here: the path
		// that ran covers the same cells. Cheap no-op resets when the mode is
		// off and the scratch was never populated.
		GpuPublishRemoves.Reset();
		GpuPublishAdds.Reset();
		return;
	}

	// THE UPLOAD IS QUEUED INTO THE MARCHER'S OWN GRAPH, NOT WRITTEN BEHIND IT.
	//
	// This used to be an ENQUEUE_RENDER_COMMAND doing RHILockBuffer / memcpy /
	// UnlockBuffer directly on the pooled buffer -- an UNSYNCHRONISED WRITE to a
	// resource RDG believes it owns and is reading from inside marcher passes.
	// RDG cannot order what it cannot see, so a flush landing in the same frame
	// as a march could have the GPU read a half-updated index.
	//
	// It was never proved to have fired: the diagnostic branch that would have
	// implicated it (identical index hashes with swinging counts) did not occur,
	// and the swings turned out to be genuinely different worlds. It is fixed
	// here ON ITS OWN TERMS rather than because a measurement demanded it -- an
	// unsynchronised write does not become correct by not having been caught.
	//
	// The staged copy is kept until Register() folds it into a graph, so the
	// ordering rule the pool's seam provides is preserved: the pool enqueues its
	// write first and this lands after it, on the same command list.
	//
	// THE DELTA PATH KEEPS THE SAME DISCIPLINE: its scatter is an RDG compute
	// pass added by Register() into the same graph that reads the buffer, so
	// both paths are ordered by the graph and neither writes behind it.
	Staged = Cells;
	bStagedValid = true;
	bForceFullUpload = false;
	bDeltaBaseEstablished = true;
	// A full snapshot supersedes any staged delta AND any accumulated dirty
	// tracking: every cell's current value is in Staged, so the sets restart.
	{
		FScopeLock Lock(&DeltaStageLock);
		bStagedDeltaValid = false;
		bStagedHashValid = false;
		StagedDeltaPairs.Reset();
	}
	DeltaPendingCells.Reset();
	DeltaStagedCells.Reset();
	// A full snapshot also supersedes any publish scratch this flush built:
	// every cell it would have written is in Staged at its current value.
	GpuPublishRemoves.Reset();
	GpuPublishAdds.Reset();

	UploadBytes -= PendingStagedBytes;
	PendingStagedBytes = uint64(Cells.Num()) * sizeof(uint32);
	UploadBytes += PendingStagedBytes;

	++UploadStats.FullUploads;
	UploadStats.LastStagedCells = uint32(Cells.Num());
}

// PHASE 2: the flush's residency travels to the GPU HERE, in its own render
// command, instead of waiting in a staging for the next marcher graph.
//
// THE ORDERING ARGUMENT, in full, because the hazard this file once fixed was
// an unsynchronised write to a buffer RDG believed it owned:
//
//   1. AGAINST THE BRICKS. This command is enqueued from MarkDirtyAndUpload,
//      which runs inside the pool's index sink -- and FVoxelBrickPool::Flush
//      enqueues its OWN render command (the arena and record writes) BEFORE it
//      calls the sink. Render commands execute in order on the render thread,
//      so the publish lands strictly after the brick writes for the same
//      batch: the GPU can never march an index entry whose bricks are not in
//      the arenas yet. This is the exact seam guarantee the CPU-staged path
//      has always leaned on, inherited rather than re-derived.
//   2. AGAINST THE MARCH PASSES. The scatter is an RDG pass with a UAV on the
//      persistent buffer; every march pass reads the same buffer as an SRV in
//      its own graph, registered external. RDG carries an external resource's
//      access state ACROSS graphs, and graphs execute serially on the render
//      thread, so a later graph's SRV read is transitioned against this
//      graph's UAV write -- no march can observe a half-published index.
//   3. AGAINST A FULL RE-UPLOAD. Register()'s full path replaces the pooled
//      buffer via QueueBufferExtraction, which could orphan a publish -- but
//      cannot: build and execute of any Register() graph happen inside ONE
//      render command (the scene renderer's), and render commands are atomic,
//      so by the time this command runs, any full consume it could race has
//      fully executed and Pooled already names the replacement. And a full
//      staged AFTER this flush necessarily absorbed it (it snapshots the
//      shadow, which ApplyDelta updated before this was enqueued).
//
// The Removed-before-Added invariant rides the two dispatch phases: removals
// are the first range and RDG's UAV barrier between the passes orders them
// ahead of every addition -- the same rule ApplyDelta enforces sequentially,
// preserved for the same reason (a slot freed and re-used in one flush).
void FVoxelMarchChunkIndex::EnqueueGpuPublish(bool bVerifyWanted, uint64 ExpectedHash)
{
	const int32 RemoveCount = GpuPublishRemoves.Num() / kPublishEntryDwords;
	const int32 AddCount = GpuPublishAdds.Num();
	if (RemoveCount == 0 && AddCount == 0)
	{
		return;
	}

	// One flat buffer, removals first, additions appended -- the two dispatch
	// ranges. Values were fixed on THIS thread when ApplyDelta built the
	// scratch, so nothing here can race the next flush's shadow writes.
	TArray<uint32> Entries = MoveTemp(GpuPublishRemoves);
	GpuPublishRemoves.Reset();
	Entries.Reserve(Entries.Num() + AddCount * kPublishEntryDwords);
	for (const TPair<uint32, FGpuPublishAdd>& P : GpuPublishAdds)
	{
		Entries.Add(uint32(P.Value.Coord.X));
		Entries.Add(uint32(P.Value.Coord.Y));
		Entries.Add(uint32(P.Value.Coord.Z));
		Entries.Add(uint32(P.Value.GridSlot));
		Entries.Add(P.Value.Slot & kSlotMask);
	}
	GpuPublishAdds.Reset();

	++UploadStats.GpuPublishes;
	UploadStats.GpuCellsWritten += uint64(AddCount);
	UploadStats.GpuCellsCleared += uint64(RemoveCount);
	UploadStats.LastStagedCells = uint32(RemoveCount + AddCount);
	// These bytes cross unconditionally when the command runs (the lost path
	// is counted separately and heals via a full), so they are counted here,
	// not staged-and-maybe-replaced like the pair path's.
	UploadBytes += uint64(Entries.Num()) * sizeof(uint32);

	ENQUEUE_RENDER_COMMAND(VoxelMarchIndexGpuPublish)(
		[this, Entries = MoveTemp(Entries), RemoveCount, AddCount, bVerifyWanted,
		 ExpectedHash](FRHICommandListImmediate& RHICmdList) mutable
	{
		// THE INDEX'S ONLY PER-FRAME RENDER-THREAD SITE. Its h= is EXPECTED to
		// read 0 on a stock leg (voxel.March.IndexGpuResident defaults off, the
		// leg's own line reads publishes=0) and a zero here is NOT evidence the
		// chunk index is free -- its real per-frame cost is a game-thread
		// QueueBufferUpload this bucket cannot see. Stated at the site as well
		// as in the log so neither reader has to find the other.
		VOXEL_RENDER_FRAME_SCOPE_TAIL(TailChunkIndex);
		// Retire any completed verify samples first, so a slot can free up for
		// the one this command may arm. `this` is a global; no lifetime issue.
		PollDeltaVerify();
		PollRefineStats();

		if (!Pooled.IsValid())
		{
			// Structurally unreachable: the ladder requires an established
			// base (a full upload extracted a buffer), and Detach clears the
			// delta machinery before enqueueing the buffer's release, so no
			// publish can be enqueued after it. If it fires anyway, the
			// entries are DROPPED -- the GPU is now missing cells the shadow
			// holds -- so the game thread is told to stage full and heal.
			++UploadStats.GpuLostNoBuffer;
			bGpuPublishLost.store(true);
			return;
		}

		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamChunkIndex, "VoxelStreamChunkIndex");
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Buffer =
			GraphBuilder.RegisterExternalBuffer(Pooled, TEXT("VoxelMarch.ChunkIndex"));
		// Copies the entry data now (same initial-data semantics as the pair
		// path), so the captured array's lifetime ends with this lambda.
		FRDGBufferRef EntriesBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("VoxelMarch.ChunkIndexPublishEntries"), sizeof(uint32),
			Entries.Num(), Entries.GetData(), Entries.Num() * sizeof(uint32));

		TShaderMapRef<FVoxelMarchIndexPublishCS> Shader(
			GetGlobalShaderMap(GMaxRHIFeatureLevel));

		if (RemoveCount > 0)
		{
			FVoxelMarchIndexPublishCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelMarchIndexPublishCS::FParameters>();
			Params->MarchIndexPublishEntries = GraphBuilder.CreateSRV(EntriesBuffer);
			Params->MarchChunkIndexRW = GraphBuilder.CreateUAV(Buffer, PF_R32_UINT);
			Params->MarchIndexPublishFirst = 0;
			Params->MarchIndexPublishCount = uint32(RemoveCount);
			Params->MarchIndexPublishMode = 0;
			Params->MarchIndexPublishDimChunks = FUintVector(kDimXY, kDimXY, kDimZ);
			Params->MarchIndexPublishCellsPerLevel = kCellsPerLevel;
			Params->MarchIndexPublishCellCount = uint32(kCells);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VoxelMarch.IndexPublishClears(%d)", RemoveCount),
				ERDGPassFlags::Compute, Shader, Params,
				FComputeShaderUtils::GetGroupCount(RemoveCount, kScatterGroupSize));
		}
		if (AddCount > 0)
		{
			FVoxelMarchIndexPublishCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelMarchIndexPublishCS::FParameters>();
			Params->MarchIndexPublishEntries = GraphBuilder.CreateSRV(EntriesBuffer);
			Params->MarchChunkIndexRW = GraphBuilder.CreateUAV(Buffer, PF_R32_UINT);
			Params->MarchIndexPublishFirst = uint32(RemoveCount);
			Params->MarchIndexPublishCount = uint32(AddCount);
			Params->MarchIndexPublishMode = 1;
			Params->MarchIndexPublishDimChunks = FUintVector(kDimXY, kDimXY, kDimZ);
			Params->MarchIndexPublishCellsPerLevel = kCellsPerLevel;
			Params->MarchIndexPublishCellCount = uint32(kCells);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("VoxelMarch.IndexPublishAdds(%d)", AddCount),
				ERDGPassFlags::Compute, Shader, Params,
				FComputeShaderUtils::GetGroupCount(AddCount, kScatterGroupSize));
		}

		// The anySolid refine, after both publish phases for the reason it runs
		// after the scatter on the other arm: the addition phase writes the
		// hardcoded anySolid 1 and a clear made ahead of it would be undone.
		AddAnySolidPasses(GraphBuilder, Buffer);

		// The verify sample, in THIS graph, after both phases: the readback
		// then holds exactly the state the expected hash describes.
		if (bVerifyWanted)
		{
			EnqueueDeltaVerify(GraphBuilder, Buffer, ExpectedHash);
		}

		GraphBuilder.Execute();
	});
}

// THE INDEX HALF, UNCHANGED. Split out of Register only so the coarse level can
// be consumed in the SAME call without this function growing a second concern;
// every branch, flag and comment below is the pre-block code verbatim.
FRDGBufferRef FVoxelMarchChunkIndex::Register(FRDGBuilder& GraphBuilder)
{
	// Retire a completed verify readback (if any) before possibly arming a new
	// one below. Render thread, like everything else in this function.
	PollDeltaVerify();
	PollRefineStats();

	if (bStagedValid)
	{
		// THE FULL PATH -- and with voxel.March.IndexDeltaUpload at its default
		// 0 it is the ONLY path, byte-identical to the pre-delta code: a
		// control leg exercises exactly this.
		//
		// Created through RDG so the upload and every later read are ordered by
		// the graph rather than by luck.
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(Staged.Num())),
			TEXT("VoxelMarch.ChunkIndex"));
		GraphBuilder.QueueBufferUpload(Buffer, Staged.GetData(),
		                               Staged.Num() * sizeof(uint32),
		                               ERDGInitialDataFlags::None);
		// Held across frames so a frame with no flush still has an index. RDG
		// extraction is what makes a transient buffer outlive its graph.
		GraphBuilder.QueueBufferExtraction(Buffer, &Pooled);
		// THE FULL PATH NEEDS THE REFINE MOST, NOT LEAST. Staged is a copy of
		// the CPU shadow, and the shadow's every resident entry carries the
		// hardcoded anySolid 1 -- so a full upload UNDOES every clear the last
		// refine made. Running it here, in the same graph, after the upload
		// pass RDG has already ordered ahead of it, is what makes the arm
		// survive the fallback ladder (seed / first / pending / large / lost)
		// instead of silently switching itself off on exactly the flushes that
		// change the most.
		AddAnySolidPasses(GraphBuilder, Buffer);
		bStagedValid = false;
		// A consumed full snapshot supersedes any delta pairs staged before the
		// game thread noticed it was pending (the staging ladder normally
		// prevents the overlap; this is the render-side belt to that brace).
		{
			FScopeLock Lock(&DeltaStageLock);
			bStagedDeltaValid = false;
			bStagedHashValid = false;
		}
		return Buffer;
	}

	// THE DELTA PATH (voxel.March.IndexDeltaUpload 1): patch the PERSISTENT
	// buffer in place with a compute scatter of [cell, value] pairs, instead
	// of creating-and-uploading 56 MiB to change ~9,500 cells.
	//
	// WHY THIS CANNOT RACE A MARCH PASS -- the ordering argument, spelled out
	// because the hazard this file once fixed was exactly an unsynchronised
	// write to a buffer RDG believed it owned:
	//
	//   * The scatter is an RDG pass with a UAV declaration on the SAME
	//     FRDGBufferRef this function returns for the marchers to read as an
	//     SRV. Within this graph, RDG sees write-then-read on one resource and
	//     inserts the barrier; the marchers cannot observe a half-scattered
	//     index.
	//   * Across graphs (the GI pass builds its own FRDGBuilder), the buffer
	//     travels as a registered external, and RDG carries an external
	//     resource's access state across graph boundaries -- graphs execute in
	//     submission order on the render thread, so a later graph's SRV read
	//     is transitioned against this graph's UAV write, not against luck.
	//   * The pair data itself is copied out of StagedDeltaPairs synchronously
	//     inside CreateStructuredBuffer (ERDGInitialDataFlags::None semantics,
	//     same as the full path's QueueBufferUpload), under the stage lock, so
	//     the game thread can never reallocate the array under this read --
	//     the one hazard the delta path has that the fixed-size Staged never
	//     did.
	{
		FScopeLock Lock(&DeltaStageLock);
		if (bStagedDeltaValid)
		{
			if (!Pooled.IsValid())
			{
				// Structurally unreachable -- delta staging requires a full
				// upload to have been staged first, and Detach clears the flag
				// before enqueueing the buffer's release -- but if it is ever
				// reached there is nothing to patch, and the contract below
				// (nullptr == never uploaded, caller must skip) is the only
				// safe answer. Patching nothing would present a null SRV that
				// reads as zeros, and zero is a LEGAL entry ("not resident"):
				// the whole world would silently be empty.
				bStagedDeltaValid = false;
				bStagedHashValid = false;
				return nullptr;
			}

			FRDGBufferRef Buffer =
				GraphBuilder.RegisterExternalBuffer(Pooled, TEXT("VoxelMarch.ChunkIndex"));

			const uint32 NumPairs = uint32(StagedDeltaPairs.Num() / 2);
			if (NumPairs > 0)
			{
				// Copies the pair data NOW (default initial-data flags), which
				// is what makes releasing the stage lock at the end of this
				// block safe.
				FRDGBufferRef PairsBuffer = CreateStructuredBuffer(
					GraphBuilder, TEXT("VoxelMarch.ChunkIndexDeltaPairs"), sizeof(uint32),
					StagedDeltaPairs.Num(), StagedDeltaPairs.GetData(),
					StagedDeltaPairs.Num() * sizeof(uint32));

				FVoxelMarchIndexScatterCS::FParameters* Params =
					GraphBuilder.AllocParameters<FVoxelMarchIndexScatterCS::FParameters>();
				Params->MarchIndexDeltaPairs = GraphBuilder.CreateSRV(PairsBuffer);
				Params->MarchChunkIndexRW = GraphBuilder.CreateUAV(Buffer, PF_R32_UINT);
				Params->MarchIndexDeltaCount = NumPairs;
				Params->MarchIndexCellCount = uint32(kCells);

				TShaderMapRef<FVoxelMarchIndexScatterCS> Shader(
					GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("VoxelMarch.IndexDeltaScatter(%u cells)", NumPairs),
					ERDGPassFlags::Compute, Shader, Params,
					FComputeShaderUtils::GetGroupCount(int32(NumPairs), kScatterGroupSize));
			}
			// AFTER THE SCATTER AND BEFORE THE VERIFY COPY, in that order and
			// deliberately. After the scatter, because the scatter re-writes
			// this flush's cells with the shadow's hardcoded anySolid 1 and
			// would undo a clear made ahead of it. Before the verify copy,
			// because the readback must hold the state the marcher will
			// actually read -- and the hash it is compared against masks bit
			// 30 out on both sides (HashableCell), so a refined buffer and an
			// unrefined shadow still agree about everything the verify covers.
			AddAnySolidPasses(GraphBuilder, Buffer);
			bStagedDeltaValid = false;

			// The verify gate, sampled: copy the whole patched buffer back and
			// hash it against the CPU state it was patched to equal. Enqueued
			// in THIS graph, after the scatter, so the snapshot is exactly
			// this staging -- later scatters land in later graphs. The ring
			// handles "no slot free" by counting a skip.
			if (bStagedHashValid)
			{
				const uint64 Expected = StagedContentHash;
				bStagedHashValid = false;
				EnqueueDeltaVerify(GraphBuilder, Buffer, Expected);
			}
			return Buffer;
		}
	}

	if (!Pooled.IsValid())
	{
		// Never uploaded. The caller must treat this as "no residency" and skip
		// its pass -- binding a null SRV reads as zeros, zero is a legal index
		// entry (not resident), and the whole world would be empty with no error
		// anywhere. That is the failure this return value exists to prevent.
		return nullptr;
	}
	return GraphBuilder.RegisterExternalBuffer(Pooled, TEXT("VoxelMarch.ChunkIndex"));
}

// ---------------------------------------------------------------------------
// THE INDEX AND ITS COARSE LEVEL, HANDED OUT TOGETHER
// ---------------------------------------------------------------------------
//
// COHERENCE IS STRUCTURAL HERE AND NOT ARGUED. The three buffers are staged by
// one MarkDirtyAndUpload from one snapshot of one shadow and consumed by this
// one call into one FRDGBuilder, so every pass the caller adds afterwards sees
// all three at the same generation. There is no ordering rule to get right
// because there is no window in which one has landed and another has not:
//
//   * a staged block snapshot is uploaded into THIS graph, and RDG orders the
//     upload ahead of every later read of the same resource in the same graph;
//   * an unstaged frame registers the persistent copies, which by then hold the
//     state the last consumed staging put there -- the same state the index's
//     own Pooled holds, for the same reason;
//   * the Phase 2 publish (voxel.March.IndexGpuResident, DEFAULT 0) writes the
//     index from its own render command, enqueued by the same flush that staged
//     these words. Render commands run in order and a march only ever happens
//     inside a graph that called this function first, so the coarse level is
//     never BEHIND the index at the moment a ray reads it. If it were ever
//     ahead, that is a block claiming Occupied over ground not yet published --
//     descend into nothing, slower, no hole. Over-covering costs time;
//     under-covering is a hole, and the asymmetry is why this is written down.
FVoxelMarchChunkIndex::FBuffers
FVoxelMarchChunkIndex::RegisterWithBlocks(FRDGBuilder& GraphBuilder)
{
	FBuffers Out;
	Out.Index = Register(GraphBuilder);
	if (Out.Index == nullptr)
	{
		// NEVER UPLOADED. All three stay null together so a caller cannot bind
		// a coarse level for an index that does not exist -- the contract the
		// index half already had, extended rather than duplicated.
		return Out;
	}

	++BlockBindCalls;

	// ---- IS WHAT THE GPU HOLDS THE GENERATION WE WANT, AND DOES IT STILL
	//      EXIST? Two questions, and BOTH have to be asked.
	//
	// The generation half is the ordinary one: a flush changed the shadow, so
	// re-upload. The IsValid() half is the one that was missing and is the whole
	// reason this leg was contaminated. QueueBufferExtraction writes the pooled
	// pointer at graph EXECUTE, not at queue time, so an upload queued into a
	// graph that is then abandoned -- and VoxelMarchBindPool's march site
	// declines with `continue` AFTER this function has run -- leaves the pointer
	// null. Under the old one-shot staging there was then nothing left to upload
	// FROM until the next flush; on a static leg that is seconds of all-ones
	// fallback. Testing the pointer as well as the generation makes any such
	// failure self-heal on the very next bind.
	//
	// PooledBlockGeneration is therefore set OPTIMISTICALLY at queue time, which
	// is safe only because it is never read alone.
	bool bUploaded = false;
	{
		FScopeLock Lock(&DeltaStageLock);
		const bool bHaveMirror = BlockShadowGeneration != 0 &&
		                         BlockMirrorOccupied.Num() == int32(kBlockWords) &&
		                         BlockMirrorAnyAbsent.Num() == int32(kBlockWords) &&
		                         BlockMirrorAllSky.Num() == int32(kBlockWords);
		const bool bGpuCopyUsable = PooledBlockOccupied.IsValid() &&
		                            PooledBlockAnyAbsent.IsValid() &&
		                            PooledBlockAllSky.IsValid() &&
		                            PooledBlockGeneration == BlockShadowGeneration;
		if (bHaveMirror && !bGpuCopyUsable)
		{
			Out.BlockOccupied = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
				TEXT("VoxelMarch.BlockOccupied"));
			GraphBuilder.QueueBufferUpload(Out.BlockOccupied, BlockMirrorOccupied.GetData(),
			                               BlockMirrorOccupied.Num() * sizeof(uint32),
			                               ERDGInitialDataFlags::None);
			GraphBuilder.QueueBufferExtraction(Out.BlockOccupied, &PooledBlockOccupied);

			Out.BlockAnyAbsent = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
				TEXT("VoxelMarch.BlockAnyAbsent"));
			GraphBuilder.QueueBufferUpload(Out.BlockAnyAbsent, BlockMirrorAnyAbsent.GetData(),
			                               BlockMirrorAnyAbsent.Num() * sizeof(uint32),
			                               ERDGInitialDataFlags::None);
			GraphBuilder.QueueBufferExtraction(Out.BlockAnyAbsent, &PooledBlockAnyAbsent);

			// ALL THREE MOVE TOGETHER, ON ONE GENERATION. Uploading the sky
			// bitfield on a different schedule from the residency pair would let
			// the marcher hold a sky licence from one flush against a residency
			// picture from another -- and the two disagreeing is exactly the
			// state in which a block reads all-sky while a chunk has landed in
			// it. One generation stamp, one condition, three uploads.
			Out.BlockAllSky = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
				TEXT("VoxelMarch.BlockAllSky"));
			GraphBuilder.QueueBufferUpload(Out.BlockAllSky, BlockMirrorAllSky.GetData(),
			                               BlockMirrorAllSky.Num() * sizeof(uint32),
			                               ERDGInitialDataFlags::None);
			GraphBuilder.QueueBufferExtraction(Out.BlockAllSky, &PooledBlockAllSky);
			PooledBlockGeneration = BlockShadowGeneration;

			// ERDGInitialDataFlags::None copies the data now, exactly as the
			// index's full path does, so releasing the lock below is safe.
			bUploaded = true;
		}
	}
	if (bUploaded)
	{
		return Out;
	}

	if (PooledBlockOccupied.IsValid() && PooledBlockAnyAbsent.IsValid() &&
	    PooledBlockAllSky.IsValid())
	{
		Out.BlockOccupied =
			GraphBuilder.RegisterExternalBuffer(PooledBlockOccupied, TEXT("VoxelMarch.BlockOccupied"));
		Out.BlockAnyAbsent =
			GraphBuilder.RegisterExternalBuffer(PooledBlockAnyAbsent, TEXT("VoxelMarch.BlockAnyAbsent"));
		Out.BlockAllSky =
			GraphBuilder.RegisterExternalBuffer(PooledBlockAllSky, TEXT("VoxelMarch.BlockAllSky"));
		return Out;
	}

	// ---- THE ALL-ONES FALLBACK ------------------------------------------
	//
	// An index buffer exists and a coarse level does not. Structurally that is
	// the frames between an attach's first index upload and its first block
	// consume, and nothing else -- but "structurally unreachable" is a claim
	// this file has been wrong about before, and the failure mode here is not
	// survivable: an UNBOUND Buffer<uint> reads as ZEROS, zeros mean "no chunk
	// in this block is resident", and the marcher would skip the entire world
	// in one jump. Every ray a hole, no error anywhere.
	//
	// SO THE FALLBACK IS THE OPPOSITE BIT PATTERN, NOT A NULL. All ones means
	// "every block might hold something": the walk descends into every block and
	// behaves exactly as the control arm does. The arm goes INERT rather than
	// wrong, which is the only acceptable direction, and BlockFallbackBinds is
	// what stops that inertness being silent -- see its declaration.
	// ---- AND IT IS A DEFECT NOW, NOT A TRANSIENT --------------------------
	//
	// The legitimate window for this path is the handful of binds between the
	// index's first upload and that graph's extraction landing. Past the grace
	// count it means the coarse level's lifetime is broken again, and the arm is
	// silently behaving as the control -- so it says so, once, naming the cause
	// rather than leaving the next person to re-derive it from a bare number.
	//
	// A LOG AND NOT A check(): the failure is INERT, not corrupting. Every ray
	// still renders correctly against an all-ones grid -- that is the entire
	// point of choosing all-ones -- so crashing a leg over it would destroy
	// exactly the run that could diagnose it.
	const uint64 Fallbacks = ++BlockFallbackBinds;
	if (Fallbacks > kBlockFallbackGraceBinds &&
	    !bBlockFallbackComplained.exchange(true))
	{
		UE_LOG(LogVoxelMarchIndex, Warning,
		       TEXT("Voxel march coarse occupancy level: %llu binds have fallen back to the "
		            "ALL-ONES grid (of %llu binds), past the %llu-bind startup grace. The "
		            "block bitfields are not surviving from one bind to the next, so the "
		            "marcher is descending into every block and CANNOT SKIP ANYTHING: "
		            "voxel.March.BlockSkip 1 is behaving as the control, and any frame time "
		            "measured on this leg is not the arm's.\n"
		            "THE CAUSE IS A LIFETIME, NOT THE TRAVERSAL. The mirror is uploaded only "
		            "when the pooled buffer is missing or its generation is stale, and "
		            "QueueBufferExtraction populates that pointer at graph EXECUTE -- so this "
		            "fires when the extraction is not landing (a graph built and abandoned, a "
		            "pass culled, or a bind on a graph that never executes). Check "
		            "FVoxelMarchChunkIndex::RegisterWithBlocks and the four VoxelMarchBindPool "
		            "call sites before touching VoxelBrickTraverse.ush -- the traversal half "
		            "measured clean at 11.2e9 blocks consulted and 24.74%% skipped."),
		       (unsigned long long)Fallbacks,
		       (unsigned long long)BlockBindCalls.load(std::memory_order_relaxed),
		       (unsigned long long)kBlockFallbackGraceBinds);
	}
	static const TArray<uint32> AllOnes = []
	{
		TArray<uint32> A;
		A.Init(0xFFFFFFFFu, int32(kBlockWords));
		return A;
	}();
	Out.BlockOccupied = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
		TEXT("VoxelMarch.BlockOccupied.Fallback"));
	GraphBuilder.QueueBufferUpload(Out.BlockOccupied, AllOnes.GetData(),
	                               AllOnes.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
	Out.BlockAnyAbsent = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
		TEXT("VoxelMarch.BlockAnyAbsent.Fallback"));
	GraphBuilder.QueueBufferUpload(Out.BlockAnyAbsent, AllOnes.GetData(),
	                               AllOnes.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
	// THE SKY BITFIELD'''S FALLBACK IS ALL ZEROS, NOT ALL ONES, AND THE
	// ASYMMETRY IS THE SAFETY ARGUMENT RATHER THAN AN OVERSIGHT.
	//
	// For the two above, zeros mean "no chunk in this block is resident" and
	// would skip the world -- hence the inversion. For this one, zeros mean "no
	// block is provably sky", so the marcher licenses no skip and descends
	// exactly as the control does. All-ones here would claim the ENTIRE WORLD is
	// open sky over a grid that has never been written, which is every ray a
	// hole with no counter moving. The conservative pattern is whichever one
	// makes the arm inert, and for this bit that is zero.
	//
	// STILL ALLOCATED AND STILL UPLOADED rather than left null: an unbound
	// Buffer<uint> also reads as zeros, but binding nothing is how a shader
	// parameter validation failure turns into a startup crash. Explicit zeros
	// cost 32 KiB on a path that only runs during the startup grace.
	static const TArray<uint32> AllZeros = []
	{
		TArray<uint32> A;
		A.Init(0u, int32(kBlockWords));
		return A;
	}();
	Out.BlockAllSky = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), kBlockWords),
		TEXT("VoxelMarch.BlockAllSky.Fallback"));
	GraphBuilder.QueueBufferUpload(Out.BlockAllSky, AllZeros.GetData(),
	                               AllZeros.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
	// NOT EXTRACTED. A fallback must never become the persistent copy: the next
	// real staging has to win, and a pooled all-ones grid would make the arm
	// permanently inert with nothing to say so.
	return Out;
}

// The render-thread half of voxel.March.IndexDeltaVerify, now a RING -- the
// header comment on FVerifySlot owns the crash post-mortem this replaces. The
// gate still SAMPLES rather than stalls: a full ring counts a skip, and
// sampling is sufficient because a wrong cell is PERSISTENT divergence (the
// buffer stays wrong until that exact cell is rewritten), so any later sample
// catches the bug class.
void FVoxelMarchChunkIndex::EnqueueDeltaVerify(FRDGBuilder& GraphBuilder,
                                               FRDGBufferRef IndexBuffer,
                                               uint64 ExpectedHash)
{
	FVerifySlot* Free = nullptr;
	for (int32 i = 0; i < kVerifySlots; ++i)
	{
		if (!VerifySlots[i].bInFlight)
		{
			Free = &VerifySlots[i];
			break;
		}
	}
	if (Free == nullptr)
	{
		++UploadStats.VerifySkippedNoSlot;
		return;
	}
	if (!Free->Readback.IsValid())
	{
		Free->Readback =
			MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.IndexDeltaVerify"));
	}
	// Enqueued AFTER the scatter/publish passes in the same graph, so RDG
	// orders write -> copy and the readback holds exactly the state the
	// expected hash describes.
	AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), IndexBuffer,
	                   uint32(kCells) * sizeof(uint32));
	Free->ExpectedHash = ExpectedHash;
	// The frame gate. IsReady() is MEANINGLESS until the graph that holds the
	// copy pass has executed -- the fence is only re-armed then -- so this
	// slot must not be polled in the frame that armed it. Strictly-greater in
	// Poll guarantees at least one frame boundary, and the render thread has
	// executed every prior frame's graphs by then.
	Free->ArmedFrame = GFrameNumberRenderThread;
	Free->bInFlight = true;
}

// ---------------------------------------------------------------------------
// MAKING kAnySolidBit REAL -- the passes, and the safety argument per writer
// ---------------------------------------------------------------------------
//
// THE INDEX BIT IS A HINT; THE RECORD IS TRUTH. Everything below is arranged
// around one asymmetry:
//
//     bit says SOLID, chunk is air  -> lose the saving, stay correct.  SAFE.
//     bit says AIR, chunk has solid -> the marcher skips real ground.  A HOLE.
//
// AND THE USUAL STALE-INDEX PROTECTION DOES NOT COVER THIS PATH. The marcher's
// `anySolid == 0` early-out returns BEFORE the record fetch -- its own comment
// says "The record is never validated on this path" -- so the RecOrigin/level
// check that turns every OTHER stale-index failure into a harmless miss is
// bypassed entirely. There is no downstream net. The proof has to live in the
// writer, which is why this pass exists at all rather than a flag on the
// snapshot.
//
// WHAT IS LEFT UNCHANGED, DELIBERATELY: all four existing writers of a resident
// entry keep their hardcoded `| kAnySolidBit` (Seed, ApplyDelta's add loop, and
// the publish kernel's addition arm). They are the SAFE default and they are
// also the only value the CPU can honestly produce -- see kAnySolidBit's block
// in the header for why "add a solidity flag to the snapshot" is dead on
// arrival. This pass is the only thing that ever clears the bit, and it clears
// it only after proving air FROM THE RECORD THAT IS IN THE SLOT NOW.
//
// THE FIVE REFUSALS, each of which would be a hole if it were an assumption
// instead of a test:
//
//   1. A ZEROED RECORD VALIDATES AS AIR AT CHUNK (0,0,0) LEVEL 0. Both free
//      passes zero a retired record; RecOrigin == (0,0,0) then MATCHES
//      WantOrigin at that chunk and LevelAndFlags == 0 matches level 0, with
//      anySolid clear. That is a live instance of absence-reads-as-air sitting
//      directly on this path. The kernel refuses on (dw0|dw1|dw2|dw3) == 0.
//   2. AN ORIGIN THAT IS NOT A CHUNK ORIGIN. WantOrigin is ChunkCoord * 32, so
//      the inverse is only defined for a multiple of 32. A record whose origin
//      is not one is garbage and is refused rather than rounded.
//   3. A LEVEL THAT MAPS TO NO GRID SLOT. GridSlotForLevel is the single
//      authority; the kernel mirrors it and refuses anything it would answer
//      -1 for. Deriving the grid slot FROM THE RECORD and then requiring the
//      whole computed cell to equal the cell being refined is what stops a
//      level-1 record clearing a level-0 cell.
//   4. THE STRIDE CROSS-CHECK. The same one the marcher runs, for the same
//      reason: a stride that moved on one side only reads a neighbouring
//      record's fields, and enough of them survive validation to look like an
//      answer.
//   5. A SELF-INCONSISTENT RECORD. anySolid is derived by the record's own
//      writers from the 64-bit L1 brick mask ((MaskLo|MaskHi) != 0 in
//      poolAllocLevelAndFlags; P->BrickSolid alongside P->bAnySolid in
//      BuildChunkRecord), so a record claiming AIR over a NON-empty mask is a
//      contradiction the storage cannot legitimately produce. The kernel
//      requires BOTH to agree before clearing. That is not belt-and-braces: it
//      is the one check that is independent of dword 3, so a shared misread of
//      the flags field cannot pass it. It costs nothing -- the record is 16
//      dwords, 64 B, ONE cache line, and dwords 5-6 are already in it.
//
// AND ONE TRAP THAT LOOKS LIKE A SHORTCUT AND IS EXACTLY INVERTED: OccWords ==
// 0 IS NOT PROOF OF AIR. 64 uniform-SOLID bricks also have zero occupancy
// words. Nothing here reads OccWords.
//
// WHAT THE MARCHER SEES ON A CORRECT CLEAR: nothing new. Both empty paths --
// the index early-out and the record's own `LevelAndFlags & 0x10` reject --
// already set C.bResident = true, so the fallthrough ladder and `substituted`
// are unaffected by moving the rejection earlier. That is checked as gate 4
// (substituted must not rise) rather than merely asserted here.
//
// allSolid (record bit 5) IS NOT HOISTED ALONGSIDE, and that was verified
// rather than assumed: zero code readers across all 40 LevelAndFlags matches. A
// cleared anySolid means the ray needs NOTHING further; allSolid means it needs
// EVERYTHING, just sooner. Only the first is a skip.
void FVoxelMarchChunkIndex::AddAnySolidPasses(FRDGBuilder& GraphBuilder, FRDGBufferRef IndexBuffer)
{
	const bool bRefine = CVarVoxelMarchIndexAnySolid.GetValueOnRenderThread() != 0;
	const bool bAudit = CVarVoxelMarchIndexAnySolidAudit.GetValueOnRenderThread() != 0;
	if ((!bRefine && !bAudit) || IndexBuffer == nullptr)
	{
		return;
	}

	// THE POOL, THROUGH ITS OWN FILLER. Nothing here re-derives the chunk
	// table's SRV, its slot count or the record stride: they arrive by the
	// same names, from the same function, as the marcher's. False means the
	// pool has nothing to march -- no arenas yet, or nothing resident -- which
	// is ordinary in the first frames of a run and is a reason to do nothing,
	// never a reason to guess.
	FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();

	// One stats buffer shared by both passes in this graph. Zero-filled first:
	// an un-cleared UAV would read the previous allocation's dwords and the
	// audit would report a wrongClear nobody committed -- and a FALSE alarm on
	// this counter costs as much investigator time as a real one.
	FRDGBufferRef StatsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(kRefineStatBufferWords)),
		TEXT("VoxelMarch.IndexAnySolidStats"));
	FRDGBufferUAVRef StatsUAV = GraphBuilder.CreateUAV(StatsBuffer, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, StatsUAV, 0u);

	TShaderMapRef<FVoxelMarchIndexRefineCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	auto FillCommon = [&](FVoxelMarchIndexRefineCS::FParameters* Params) -> bool
	{
		if (!Pool.BindShaderParameters(GraphBuilder, *Params))
		{
			return false;
		}
		Params->MarchChunkIndexRW = GraphBuilder.CreateUAV(IndexBuffer, PF_R32_UINT);
		Params->MarchIndexRefineStats = StatsUAV;
		Params->MarchIndexRefineDimChunks = FUintVector(kDimXY, kDimXY, kDimZ);
		Params->MarchIndexRefineCellsPerLevel = kCellsPerLevel;
		Params->MarchIndexRefineCellCount = uint32(kCells);
		// GridSlotForLevel's two halves, bound rather than spelled a second
		// time in HLSL. The cover level is 7 and the grid carries eight
		// sub-grids with cover last; getting that mapping wrong in the shader
		// would resolve a cover record onto a ring cell, and the whole-cell
		// equality test is what makes binding these enough.
		Params->MarchIndexRefineRingGrids = kRingGrids;
		Params->MarchIndexRefineCoverLevel = uint32(kCoverLevel);
		Params->MarchIndexRefineCoverGridSlot = kCoverGridSlot;
		return true;
	};

	bool bDispatched = false;
	auto AddOne = [&](uint32 Mode, uint32 StatBase, uint32 Poison, const TCHAR* Name)
	{
		FVoxelMarchIndexRefineCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelMarchIndexRefineCS::FParameters>();
		if (!FillCommon(Params))
		{
			return;
		}
		Params->MarchIndexRefineMode = Mode;
		Params->MarchIndexRefineStatBase = StatBase;
		Params->MarchIndexRefinePoison = Poison;
		// ONE THREAD PER POOL SLOT, sized from the value the POOL just bound
		// rather than from a second copy of the capacity. The kernel
		// bounds-checks against the same uniform, so a dispatch and a guard
		// derived from one number cannot disagree about where the table ends.
		const uint32 Groups = FMath::DivideAndRoundUp(Params->VoxelBrickChunkSlots,
		                                              uint32(kScatterGroupSize));
		if (Groups == 0)
		{
			return;
		}
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("%s(%u slots)", Name, Params->VoxelBrickChunkSlots),
			ERDGPassFlags::Compute, Shader, Params, FIntVector(int32(Groups), 1, 1));
		bDispatched = true;
	};

	if (bRefine)
	{
		AddOne(0u, uint32(kRefineStatBaseRefine),
		       CVarVoxelMarchIndexAnySolidPoison.GetValueOnRenderThread() != 0 ? 1u : 0u,
		       TEXT("VoxelMarch.IndexAnySolidRefine"));
	}

	// ---- THE AUDIT, AFTER THE REFINE, AND THE ORDER IS THE WHOLE GATE ------
	//
	// It has to read the buffer THE MARCHER WILL READ. Every march pass from
	// here until the next flush consumes the state this graph leaves behind,
	// so auditing that state -- after the index write and after the refine
	// pass -- is auditing exactly what the rays get.
	//
	// AUDITING FIRST WOULD BE A TEST THAT CANNOT FAIL, and it was written that
	// way once before being caught here. On the full-upload path the buffer is
	// CREATED in this graph and every entry in it carries the shadow's
	// hardcoded anySolid 1, so an audit placed ahead of the refine would find
	// nothing wrong no matter how badly the refine behaved -- including under
	// the poison arm, which is the one leg whose entire purpose is to make
	// this counter fire.
	//
	// The staleness an audit-first placement would have covered -- a record
	// changing between graphs while the bit stayed cleared -- is closed
	// elsewhere and by construction: every producer of a record is also a
	// producer of an index add, and an index add writes bit 30 back to 1. And
	// this pass walks EVERY pool slot on EVERY flush, so a bit that somehow
	// went wrong out of band is caught at the next flush regardless.
	if (bAudit)
	{
		AddOne(1u, uint32(kRefineStatBaseAudit), 0u, TEXT("VoxelMarch.IndexAnySolidAudit"));
	}

	// THE ARM'S PROOF OF LIFE, INCREMENTED ONLY WHERE A DISPATCH WAS ACTUALLY
	// ADDED. Counting the call instead of the dispatch is how a switch that is
	// on and does nothing reads as healthy -- the failure this project has
	// paid for more than any other. With the cvar on and the pool not yet
	// bindable (no arenas, nothing resident) this stays 0, and 0 with the cvar
	// on is a diagnosis, not a reassurance.
	if (bDispatched)
	{
		++UploadStats.RefineDispatches;
	}
	else
	{
		return;
	}

	// The readback, sampled on the same ring discipline as the delta verify's
	// and for the same recorded crash: arm a FREE slot or skip, and never poll
	// a slot in the frame that armed it. A skipped sample is a smaller sample,
	// not a wrong one -- a wrongly cleared bit is PERSISTENT (nothing sets it
	// back until that cell is rewritten), so any later sample still catches
	// the bug class.
	FRefineStatsSlot* Free = nullptr;
	for (int32 i = 0; i < kRefineStatsSlots; ++i)
	{
		if (!RefineStatsSlots[i].bInFlight)
		{
			Free = &RefineStatsSlots[i];
			break;
		}
	}
	if (Free == nullptr)
	{
		return;
	}
	if (!Free->Readback.IsValid())
	{
		Free->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelMarch.IndexAnySolidStats"));
	}
	AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), StatsBuffer,
	                   uint32(kRefineStatBufferWords) * sizeof(uint32));
	// BOTH BLOCKS TRAVEL IN ONE SAMPLE. An arm that was not dispatched left
	// its block at the zeros AddClearUAVPass wrote, and zeros fold to nothing
	// -- so the reader needs no flag for which arms ran, and cannot mistake
	// one arm's words for the other's.
	Free->ArmedFrame = GFrameNumberRenderThread;
	Free->bInFlight = true;
}

void FVoxelMarchChunkIndex::PollRefineStats()
{
	for (int32 i = 0; i < kRefineStatsSlots; ++i)
	{
		FRefineStatsSlot& Slot = RefineStatsSlots[i];
		if (!Slot.bInFlight || !Slot.Readback.IsValid() ||
		    GFrameNumberRenderThread <= Slot.ArmedFrame || !Slot.Readback->IsReady())
		{
			continue;
		}
		Slot.bInFlight = false;

		const uint32 NumBytes = uint32(kRefineStatBufferWords) * sizeof(uint32);
		const uint32* Data = static_cast<const uint32*>(Slot.Readback->Lock(NumBytes));
		if (Data == nullptr)
		{
			continue;
		}
		uint32 Buf[kRefineStatBufferWords];
		FMemory::Memcpy(Buf, Data, NumBytes);
		Slot.Readback->Unlock();

		++UploadStats.RefineStatsSamples;

		{
			const uint32* W = Buf + kRefineStatBaseAudit;
			// THE AUDIT'S TWO WORDS. Checked is the denominator -- resident,
			// identity-matched cells whose record says SOLID -- and it must be
			// non-zero for the other one to be a reading rather than a silence.
			// ITS OWN WORD, not Examined -- see RefineStat_AuditSolid. On a
			// healthy leg this must equal the refine arm's leftSolid, which is
			// the same population counted by the other pass; they agreed to
			// the unit (4,708,059) on the 2026-08-27 green leg.
			UploadStats.AuditChecked += uint64(W[RefineStat_AuditSolid]);
			UploadStats.AuditWrongClear += uint64(W[RefineStat_Cleared]);
			if (W[RefineStat_Cleared] != 0)
			{
				UE_LOG(LogVoxelMarchIndex, Error,
				       TEXT("Voxel march index anySolid AUDIT FAILED: %u index cells say AIR "
				            "(bit 30 clear) over a chunk record that says SOLID, of %u "
				            "identity-matched solid chunks checked. The marcher's cheapest "
				            "skip is now skipping REAL GROUND on those chunks -- a hole, not "
				            "an error, and it will not show in `uncovered` (that word is 25%% "
				            "on a healthy leg and is not an arc detector). Set "
				            "voxel.March.IndexAnySolid 0 and treat every capture and every "
				            "timing since the last clean audit as suspect. If "
				            "voxel.March.IndexAnySolidPoison is 1 this is the EXPECTED result "
				            "and the red arm has passed."),
				       W[RefineStat_Cleared], W[RefineStat_Examined]);
			}
		}
		{
			const uint32* W = Buf + kRefineStatBaseRefine;
			UploadStats.RefineExamined += uint64(W[RefineStat_Examined]);
			UploadStats.RefineNoMatch += uint64(W[RefineStat_NoMatch]);
			UploadStats.RefineCleared += uint64(W[RefineStat_Cleared]);
			UploadStats.RefineCasLost += uint64(W[RefineStat_CasLost]);
			UploadStats.RefineLeftSolid += uint64(W[RefineStat_LeftSolid]);
			UploadStats.RefineAlreadyClear += uint64(W[RefineStat_AlreadyClear]);
			UploadStats.RefineRefused +=
				uint64(W[RefineStat_RefusedZeroRecord]) + uint64(W[RefineStat_RefusedOrigin]) +
				uint64(W[RefineStat_RefusedLevel]) + uint64(W[RefineStat_RefusedCell]) +
				uint64(W[RefineStat_RefusedStride]) + uint64(W[RefineStat_RefusedInconsistent]);
			// THE STRIDE WORD IS AN ALARM, NOT A STATISTIC. It can only be
			// non-zero if VoxelBrickPool::kChunkRecordDwords and the define
			// this kernel was compiled with have separated, in which case the
			// marcher's own stride guard has already emptied the world -- but
			// this says so at the site rather than leaving a blank screen to
			// be diagnosed from scratch.
			if (W[RefineStat_RefusedStride] != 0)
			{
				UE_LOG(LogVoxelMarchIndex, Error,
				       TEXT("Voxel march index anySolid refine: the record stride cross-check "
				            "failed on %u slots -- the bound VoxelBrickChunkRecordDwords and "
				            "the kernel's compiled VOXEL_MARCH_INDEX_REFINE_RECORD_DWORDS "
				            "disagree. No bit was cleared (the refusal is the safe direction), "
				            "but the MARCHER reads the same table at its own stride and its "
				            "guard empties the world. Rebuild the shaders."),
				       W[RefineStat_RefusedStride]);
			}
		}
	}
}

void FVoxelMarchChunkIndex::PollDeltaVerify()
{
	for (int32 i = 0; i < kVerifySlots; ++i)
	{
		FVerifySlot& Slot = VerifySlots[i];
		if (!Slot.bInFlight || !Slot.Readback.IsValid() ||
		    GFrameNumberRenderThread <= Slot.ArmedFrame || !Slot.Readback->IsReady())
		{
			continue;
		}
		Slot.bInFlight = false;

		const uint32 NumBytes = uint32(kCells) * sizeof(uint32);
		const uint32* Data = static_cast<const uint32*>(Slot.Readback->Lock(NumBytes));
		if (Data == nullptr)
		{
			continue;
		}
		// The SAME hash, in the SAME order, as the game-thread FNV over Cells
		// -- so "GPU buffer after the sampled write" and "what a full upload
		// of that state would have carried" are compared as one number each.
		// ~17.5 ms of render thread per sample (measured rate of the same
		// loop on the game thread); the cvar's help text owns that cost.
		// THE SAME MASK AS THE GAME-THREAD SIDE, through the same function.
		// Bit 30 is the GPU refine pass's to own (see MarkDirtyAndUpload's
		// note); every other bit is still compared exactly as before, so a
		// wrong slot, a lost cell or a drifted wrap still fails here.
		uint64 Hash = 1469598103934665603ull;
		for (uint32 c = 0; c < uint32(kCells); ++c)
		{
			Hash ^= uint64(HashableCell(Data[c]));
			Hash *= 1099511628211ull;
		}
		Slot.Readback->Unlock();

		if (Hash == Slot.ExpectedHash)
		{
			++UploadStats.VerifyPasses;
		}
		else
		{
			++UploadStats.VerifyFailures;
			UE_LOG(LogVoxelMarchIndex, Error,
			       TEXT("Voxel march index DELTA VERIFY FAILED: GPU buffer hash 0x%016llx != "
			            "expected 0x%016llx (the hash of the CPU grid state this buffer was "
			            "patched to equal). The GPU-written index differs from what the CPU "
			            "would have written -- at least one cell is wrong, which renders as a "
			            "hole or as another chunk's terrain, not as an error. If "
			            "voxel.March.IndexGpuResident is on, suspect the publish path first "
			            "(shared-wrap drift, a lost entry, a guard mismatch); fall back to "
			            "voxel.March.IndexGpuResident 0, then voxel.March.IndexDeltaUpload 0, "
			            "and treat every leg since the last VerifyPasses as suspect."),
			       Hash, Slot.ExpectedHash);
		}
	}
}

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

#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

// Wave 1.3 (delta upload): the scatter kernel, its cvars, and the verify
// readback. Explicit rather than transitive, for the same reason RHI.h is
// above: this file compiles alone on the adaptive non-unity path.
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

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

	// One thread per changed cell; pairs are deduplicated BY CONSTRUCTION on
	// the host (built from a TSet keyed by cell), so no two threads in a
	// dispatch write the same address -- see the .usf header for why that is
	// load-bearing rather than tidy.
	class FVoxelMarchIndexScatterCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMarchIndexScatterCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchIndexScatterCS, FGlobalShader);

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

	// Matches [numthreads(64, 1, 1)] in the kernel. Restated here because the
	// dispatch size is computed from it and a mismatch drops the tail of the
	// pair list -- which is a handful of silently wrong index cells, the exact
	// failure shape this feature must never produce.
	constexpr int32 kScatterGroupSize = 64;

	// The Phase 2 publish kernel: entries in, cells derived and written on the
	// GPU. Same .usf as the scatter, second entry point; the kernel header
	// owns the entry layout and the removal-guard argument.
	class FVoxelMarchIndexPublishCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMarchIndexPublishCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMarchIndexPublishCS, FGlobalShader);

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
}

IMPLEMENT_GLOBAL_SHADER(FVoxelMarchIndexScatterCS, VOXEL_MARCH_INDEX_SCATTER_USF,
                        "VoxelMarchIndexScatterMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMarchIndexPublishCS, VOXEL_MARCH_INDEX_SCATTER_USF,
                        "VoxelMarchIndexPublishMain", SF_Compute);

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

FVoxelMarchChunkIndex::FVoxelMarchChunkIndex() = default;
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
			Pooled.SafeRelease();
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
			Cells[int32(SeedCell)] = kResidentBit | kAnySolidBit | (E.ChunkSlot & kSlotMask);
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
			Cells[int32(Cell)] = 0u;
			if (bTrackDelta)
			{
				DeltaPendingCells.Add(Cell);
			}
			--NumEntries;
			--PerSlotEntries[RemSlot];
			// Only drop the ownership record if it still names THIS chunk; a
			// cell re-pointed by an earlier Added in the same batch belongs to
			// the new owner, not to the one being retired.
			const FIntVector RemCoord(E.Key.X, E.Key.Y, E.Key.Z);
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
		}
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

void FVoxelMarchChunkIndex::MarkDirtyAndUpload()
{
	if (!bDirty)
	{
		return;
	}
	bDirty = false;
	++Uploads;

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
	if (bContentHashEnabled || bVerifyWanted)
	{
		uint64 Hash = 1469598103934665603ull;
		for (uint32 V : Cells)
		{
			Hash ^= uint64(V);
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
		// Retire any completed verify samples first, so a slot can free up for
		// the one this command may arm. `this` is a global; no lifetime issue.
		PollDeltaVerify();

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

		// The verify sample, in THIS graph, after both phases: the readback
		// then holds exactly the state the expected hash describes.
		if (bVerifyWanted)
		{
			EnqueueDeltaVerify(GraphBuilder, Buffer, ExpectedHash);
		}

		GraphBuilder.Execute();
	});
}

FRDGBufferRef FVoxelMarchChunkIndex::Register(FRDGBuilder& GraphBuilder)
{
	// Retire a completed verify readback (if any) before possibly arming a new
	// one below. Render thread, like everything else in this function.
	PollDeltaVerify();

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
		uint64 Hash = 1469598103934665603ull;
		for (uint32 c = 0; c < uint32(kCells); ++c)
		{
			Hash ^= uint64(Data[c]);
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

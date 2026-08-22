// VoxelMarchChunkIndex.h -- THE MISSING PIECE BETWEEN THE POOL AND THE MARCHER.
//
// P3-B1 of docs/ray-marching-plan-2026-08-19.md.
//
// STATUS: BUILT (P3-B1). The design below was reviewed before it was
// implemented, which is why the rationale reads as decisions rather than as
// description.
//
// ===========================================================================
// WHY THIS FILE HAS TO EXIST AT ALL
// ===========================================================================
//
// FVoxelBrickPool is residency. It owns three arenas and a table of 32 B
// records, and it can answer "where did chunk (level, x, y, z) land?" -- ON THE
// CPU, through a TMap, in FindChunkSlot.
//
// THE GPU CANNOT ASK THAT QUESTION. The chunk table is a flat array addressed by
// SLOT, and nothing maps a coordinate to a slot. A ray stepping through space
// has a coordinate and needs a slot, at every chunk cell, in the inner loop. So
// between "the volume is resident" and "the marcher can walk it" there is a data
// structure that does not exist, and it is this one.
//
// The pool's own header says so and assigns it here:
//
//     "P3 NOTE, so it is not discovered later: this is the CPU-side half. The
//      GPU-side L2 acceleration -- a dense toroidal chunk grid per ring level,
//      addressed like FVoxelFluidOccupancy's rolling window -- belongs with the
//      marcher that walks it."          (VoxelBrickPool.h, FindChunkSlot)
//
// It appears in neither section 5 nor section 7 of the plan. That gap is now
// recorded in the plan's status board, and this is the largest single piece of
// new work in the phase.
//
// ===========================================================================
// THE STRUCTURE
// ===========================================================================
//
// One DENSE, TOROIDAL grid of uint32 per ring level, addressed exactly the way
// FVoxelFluidOccupancy addresses its rolling window (VoxelFluidOccupancy.h:
// 31-49) -- which is what docs/brick-volume-format.md section 5 directs. The
// reason is that the code shape is already GPU-resident, already unit-tested,
// and its RecentreTo semantics are precisely what ring recentring needs.
// Nothing here is a new idea; it is an existing one applied to chunks instead
// of voxels.
//
// DIMENSIONS ARE POWERS OF TWO so the toroidal wrap is one AND against a mask
// rather than an integer modulo. That is not micro-optimisation: the wrap runs
// once per chunk cell in the top-level DDA, and VoxelFluidCollision.ush's
// VoxelFluidStorageVoxel makes exactly this trade for exactly this reason.
//
// SIZING, from kDefaultRingPresets (VoxelWorldSubsystem.h:147) and a 32-voxel
// chunk. R0 is 0-128 m, so its diameter is 256 m; a level-0 chunk is
// 32 * 0.1 m = 3.2 m, giving 80 chunks across. Rounded up to 128 for the
// power-of-two wrap. Every ring spans the same chunk count by construction --
// that is what a cascade is -- so 128 horizontal serves all six levels.
//
// Vertical is NOT the same number. The world is a terrain shell, not a cube, so
// a full 128 vertically would be mostly empty. 64 is the starting figure (205 m
// of band at level 0) and it is the first thing to tighten if the footprint
// matters: 128 * 128 * 128 * 4 B = 8 MiB per level, 48 MiB across six. Against a
// 388 MiB pool commit that is affordable, and at P3-B1 -- level 0 only -- it is
// 8 MiB. (It was 64 deep and 4 MiB until the Z-aliasing fix; see kDimZ.)
//
// ---------------------------------------------------------------------------
// ONE ENTRY IS ONE DWORD, A DELIBERATE DEVIATION FROM THE FORMAT DOC
// ---------------------------------------------------------------------------
//
//     bit 31      resident
//     bit 30      anySolid   (from the record's LevelAndFlags bit 4)
//     bits 0..29  chunk slot (also the record index)
//
// docs/brick-volume-format.md section 5 asks for the grid "plus a 1-bit-per-
// chunk mask so the top-level DDA rejects a chunk cell with one bit". Packing
// resident and anySolid INTO the slot dword serves that purpose in ONE load: an
// absent or empty chunk is rejected on a bit test, and a present one already
// has its slot in hand. A separate bitmask would cost a second load on the
// common path to save nothing.
//
// THE BITMASK STILL EARNS ITS PLACE LATER, and this is not a rejection of it. A
// 1-bit-per-chunk mask lets a hierarchical walk test 32 chunk cells with one
// dword and SKIP RUNS of empty space -- which is the whole performance case,
// and which P3-B1 deliberately does not build. It arrives with the hierarchical
// skipping in P3-B2, alongside the thing that makes it worth having.
//
// ===========================================================================
// HOW IT IS BUILT, AND WHY v1 IS BLUNT ON PURPOSE
// ===========================================================================
//
// v1: WHEN THE RESIDENT SET HAS CHANGED, REBUILD THE WHOLE LEVEL AND UPLOAD IT.
// No delta tracking, no dirty regions, no incremental recentre. 4 MiB at level
// 0, only on frames where something moved.
//
// That is blunt, and it buys three correctness properties the incremental
// version would each have to earn separately:
//
//   * RECENTRING IS FREE. A ring that slides changes which coordinates map
//     where; a full rebuild simply writes the new mapping. The incremental
//     version has to invalidate exactly the entries that fell out of the
//     window, which is the class of bug that renders as terrain from 51 m away
//     appearing in the wrong place.
//   * THE EVICTION HAZARD IS FREE. FVoxelBrickPool::GetEvictions() is zero
//     TODAY only because nothing calls RemoveChunk. When the marcher becomes
//     the draw path at P4 that stops being true, and a stale index entry then
//     points a ray at a freed slot -- reading another chunk's bricks, which
//     renders as plausible terrain in the wrong place rather than as a crash.
//     A full rebuild cannot hold a stale entry.
//   * IT IS MEASURABLE BEFORE IT IS OPTIMISED. If the rebuild shows up in a
//     frame time, that is a number. Optimising it first would be optimising a
//     cost nobody has seen.
//
// ORDERING, A REQUIREMENT AND NOT A PREFERENCE: the index upload must be
// enqueued AFTER FVoxelBrickPool::Flush() in the same frame. Both are render
// commands posted from the game thread and render commands execute in order, so
// this is a call-order rule in the caller and nothing more -- but an index
// naming slots the flush has not written yet points rays at uninitialised
// arenas, and the result looks like terrain rather than like an error.
//
// ===========================================================================
// WHAT THIS NEEDS FROM THE POOL, AND IT IS ONE THING
// ===========================================================================
//
// The resident set as (level, chunkCoord, slot, anySolid), at flush time. The
// pool has all four -- FResidentChunk carries Key and ChunkSlot, and anySolid
// is already in the record it wrote.
//
// It must NOT come through DebugGetResidentChunk: that is marked DEBUG ONLY, it
// is a per-key lookup rather than an enumeration, and building the draw path on
// a debug accessor is how a debug accessor becomes load-bearing.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "Templates/SharedPointer.h"

class FRDGBuilder;
class FRDGPooledBuffer;

// The shader-side binding, mirroring VOXEL_FLUID_OCCUPANCY_PARAMETERS() term
// for term so the two traversal sources present the same shape to the marcher
// and VOXEL_MARCH_SOURCE really is a one-line swap.
//
//   MarchChunkIndex          the dense grid, one dword per chunk cell
//   MarchIndexOriginChunk    the grid's min corner, in LEVEL-L chunk coords
//   MarchIndexDimChunks      edge lengths; powers of two, so the wrap is a mask
//   MarchIndexWrapChunk      the rolling window's storage offset
#define VOXEL_MARCH_CHUNK_INDEX_PARAMETERS() \
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex) \
	SHADER_PARAMETER(FIntVector,  MarchIndexOriginChunk) \
	SHADER_PARAMETER(FUintVector, MarchIndexDimChunks) \
	SHADER_PARAMETER(FUintVector, MarchIndexWrapChunk)

class VOXELEARTHSHADERS_API FVoxelMarchChunkIndex
{
public:
	FVoxelMarchChunkIndex();
	~FVoxelMarchChunkIndex();

	FVoxelMarchChunkIndex(const FVoxelMarchChunkIndex&) = delete;
	FVoxelMarchChunkIndex& operator=(const FVoxelMarchChunkIndex&) = delete;

	// Horizontal edge, in chunks. Power of two -- see the header note.
	static constexpr uint32 kDimXY = 128;
	// Vertical edge, in chunks.
	//
	// WAS 64, AND THAT WAS A LATENT ALIASING BUG. 64 chunks is 204.8 m at level
	// 0, against R0's 256 m vertical span (128 m of radius, both ways). 80 > 64,
	// so two resident chunks 64 apart in Z landed on the same cell -- one
	// shadowing the other, turned into a hole by the record validation rather
	// than into wrong terrain, but a hole nobody ordered.
	//
	// The original static_assert proved the XY case (80 < 128) and said nothing
	// about Z, and the runtime drop-check that was supposed to cover it was a
	// TAUTOLOGY -- `Coord.Z - (Coord.Z & ~63)` is `Coord.Z & 63`, always below
	// 64 -- so "0 dropped outside the band" was not evidence of anything.
	//
	// 128 makes the same argument hold on all three axes, and costs 4 MiB.
	static constexpr uint32 kDimZ = 128;
	static constexpr uint32 kCellsPerLevel = kDimXY * kDimXY * kDimZ;
	// LEVELS INDEXED. P3-B2b-1 is two rings, L0 + L1, so two grids laid out
	// contiguously: a level-L cell is at L * kCellsPerLevel + the wrapped xyz.
	//
	// The dimensions need no change per level and that is the cascade property,
	// not a coincidence: ring L has outer radius 128 * 2^L m and chunk size
	// 3.2 * 2^L m, so the chunks across a ring are 80 AT EVERY LEVEL. The
	// existing static_assert therefore generalises as written.
	//
	// 8 MiB per level, 48 MiB across six.
	//
	// THE FULL REBUILD IS NOW THE THING TO WATCH. Rebuild-and-upload-everything
	// was chosen so stale slots are structurally impossible, and at one level on
	// dirty frames it was free. At six it is 48 MiB per dirty frame, and during
	// cold fill that is most frames -- roughly 2.9 GB/s at 60 Hz, which is a
	// streaming regression introduced by the renderer rather than a rounding
	// error.
	//
	// IT IS INSTRUMENTED, NOT PRE-OPTIMISED. The plan's next step is a per-level
	// dirty rebuild (the pool's delta already carries Key.Level, and most
	// flushes touch one or two levels), then scatter upload only if that shows
	// up in a frame time. Measure before optimising is the rule that kept the
	// first version blunt and it applies to its own successor. GetUploadBytes()
	// is what decides it.
	static constexpr uint32 kLevels = 6;

	// ===================================================================
	// PHASE 6: THE COVER GRID. GRID SLOT IS NOT RING LEVEL.
	// ===================================================================
	//
	// Ground cover (grass, flowers, reeds, small bush -- 230 species, 223 of them
	// at 50 mm) is ONE LATTICE FINER than ring 0, not coarser. It is carried in
	// the same FVoxelBrickPool as terrain, under level 7: the largest value that
	// fits BOTH the chunk record's four-bit LevelAndFlags[0:3] and the VisBuffer's
	// three-bit level field, with 6 left free for a seventh ring.
	//
	// Laying the grids out BY LEVEL would then need eight sub-grids to hold seven
	// populated ones -- 64 MiB of address space to carry 56 MiB of use -- so the
	// level-to-slot mapping is explicit and lives in exactly one place
	// (GridSlotForLevel), which the shader reads as the MarchCoverIndexGrid
	// uniform rather than spelling as a literal.
	//
	// WHY COVER SHARES THIS INDEX AND THIS POOL AT ALL, since a separate store
	// was the obvious alternative. The marcher's decode -- index lookup, record
	// validation, descriptor kind switch, occupancy dwords, popcount-ranked
	// palette -- would have to be duplicated for a second set of bindings, and a
	// second copy of a format decode is the single defect shape this project has
	// paid for most often. Sharing means a cover voxel is read by THE SAME code
	// and shaded by THE SAME VoxelMaterialColor as a terrain voxel, and the only
	// thing that differs is the ray rescale (1/2 instead of 2^L) and this slot.
	//
	// REQUIREMENT C1 IS SATISFIED BY SHARING, NOT DESPITE IT. C1
	// (docs/detail-assets-in-the-volume-2026-08-19.md section 5.3) forbids a
	// dense 3D array over cover's BOUNDING BOX, measured at 290.1 MiB against
	// 25.4 MiB of payload -- an 11x swing driven by terrain RELIEF. This grid is
	// not over cover's bounding box: it is a fixed 8 MiB toroidal window over the
	// MARCHED REACH, the same 8 MiB on a cliff as on a plain, and the payload
	// behind it is suballocated per chunk with nothing stored for a chunk that
	// has no cover (vxc::packCoverChunk returns anyCover=false and the publisher
	// stores nothing at all -- no zeroed pack, no reserved slot).
	static constexpr int32 kCoverLevel = 7;
	static constexpr uint32 kRingGrids = kLevels;
	static constexpr uint32 kCoverGridSlot = kRingGrids;
	static constexpr uint32 kGridSlots = kRingGrids + 1u;
	static constexpr uint32 kCells = kGridSlots * kCellsPerLevel;

	// Which sub-grid a POOL LEVEL lives in, or -1 for a level this index does not
	// carry. THE SINGLE AUTHORITY for the mapping.
	//
	// -1 is a DROP the caller counts, never a fold-in: silently folding an
	// unmapped level into slot 0 would put another lattice's chunks in ring 0's
	// cells, which renders as terrain in the wrong place rather than as an error.
	static int32 GridSlotForLevel(int32 Level)
	{
		if (Level >= 0 && Level < int32(kRingGrids))
		{
			return Level;
		}
		return (Level == kCoverLevel) ? int32(kCoverGridSlot) : -1;
	}

	// THE COVER BAND, IN COVER CHUNKS, AND IT IS LEVEL 0's PROOF REUSED.
	//
	// A cover chunk is 32 * 50 mm = 1.6 m. Two cover chunks alias in this grid
	// only if they are kDimXY apart on an axis: 128 * 1.6 = 204.8 m. Admitting
	// only cover within +/-40 chunks (+/-64 m) of the band centre bounds the
	// simultaneous span at 80 chunks on EVERY axis -- the identical 80 < 128 that
	// makes the level-0 grid safe, reached by bounding what ENTERS rather than by
	// hoping, which is the fix direction the 2026-08-21 index work registered.
	//
	// Z IS BOUNDED TOO, AND THAT IS THE POINT OF DOING IT THIS WAY. Terrain
	// relief across a 128 m disc exceeds 204.8 m on alpine ground, so a vertical
	// bound derived from relief cannot be proved and a vertical bound derived
	// from the marched reach can. Cover 64 m above or below the camera is not
	// marched, so nothing is lost that would have been drawn.
	static constexpr int32 kCoverBandRadiusChunks = 40;
	static constexpr double kCoverChunkMeters = 1.6;   // 32 cells x 50 mm
	static constexpr double kCoverMaxReachMeters =
		double(kCoverBandRadiusChunks) * kCoverChunkMeters;

	// AND WHY ALIASING CANNOT HAPPEN AT LEVEL 0, which is what makes the wrap
	// safe without an origin. Two chunks collide in this grid only if they are
	// kDimXY apart on an axis -- 128 chunks * 3.2 m = 409.6 m at level 0. R0
	// spans 0-128 m of RADIUS, i.e. 256 m across, i.e. 80 chunks. 80 < 128, so
	// no two chunks resident at level 0 can ever land on the same cell.
	//
	// It is checked rather than asserted in prose: see the static_assert in the
	// .cpp, which now covers ALL THREE AXES against the ring preset. And because
	// that assert encodes a COMPILE-TIME fact about a RUNTIME-OVERRIDABLE
	// setting (-VoxelRingOuterMeters=), the index also tracks the chunk-coordinate
	// span it actually observes and complains once if it approaches a grid
	// dimension. A proof whose premise can be changed on the command line needs a
	// runtime guard as well.
	static constexpr uint32 kResidentBit = 0x80000000u;
	static constexpr uint32 kAnySolidBit = 0x40000000u;
	static constexpr uint32 kSlotMask    = 0x3FFFFFFFu;

	// Attaches to the global brick pool: registers the delta sink AND seeds the
	// grid from the current resident set, in ONE call, because registering late
	// is the normal case and a register-then-snapshot pair leaves a window where
	// a delta lands on an unseeded index. GAME THREAD ONLY. Idempotent.
	void AttachToGlobalPool();
	void Detach();

	// WHETHER THE SINK IS EVEN CONNECTED, and it is not a curiosity.
	//
	// The attach is driven by VoxelMarchPublishSource, which the FLUID subsystem
	// calls -- so a leg that runs without fluids never attaches, every cover
	// counter here stays zero, and "the index was never listening" reads exactly
	// like "no cover was ever produced". Those have different owners. Any caller
	// reporting a zero from this class must say which one it is.
	bool IsAttached() const { return bAttached; }

	// ---- PHASE 6: the cover band centre ------------------------------------
	//
	// The camera's COVER CHUNK coordinate. Entries further than
	// kCoverBandRadiusChunks from it on any axis are refused and COUNTED, which
	// is what turns the aliasing proof above from an argument into a bound.
	//
	// GAME THREAD ONLY, and it must be set from the same place the pool's
	// eviction focus is, so residency and indexing agree about where the camera
	// is. Never set, the band centre is the origin -- which is the honest
	// behaviour for a feature nothing is driving, and the drop counter says so.
	void SetCoverBandCentreChunk(const FIntVector& CoverChunkCoord);

	// Cover entries the index is holding right now, and the two numbers that say
	// why it is not more.
	//
	// OFFERED = ADMITTED + DROPPED IS A CONSERVATION LAW, and the stats line
	// checks it rather than printing three numbers and hoping. A counter that
	// only ever reads zero is indistinguishable from a counter nothing
	// increments -- this project has found that exact defect eleven times -- so
	// the three are gated against each other instead of read alone.
	//
	// BOTH SIDES ARE CUMULATIVE ON PURPOSE. Residency is not in the law: a
	// removal lowers residency without lowering the offer count, so a law stated
	// over residency would break every time a cover chunk was released and the
	// break would mean nothing. Residency is reported beside it as a description,
	// never gated on.
	int32 GetCoverEntries() const { return PerSlotEntries[kCoverGridSlot]; }
	int32 GetCoverOffered() const { return CoverOffered; }
	int32 GetCoverAdmitted() const { return CoverAdmitted; }
	int32 GetCoverDroppedOutOfBand() const { return CoverDroppedOutOfBand; }
	int32 GetCoverAliasCollisions() const { return AliasCollisions[kCoverGridSlot]; }

	// THREE OUTCOMES, AND THE THIRD IS THE ONE THAT MATTERS.
	//
	// NotExercised is NOT a pass. A leg with the producer off and a leg with a
	// broken publisher both leave every cover counter at zero, and a two-valued
	// check calls that "conserved" -- which is exactly how the ring counters read
	// zero for four legs and were mined for mechanism. The caller must print the
	// message, not the enum.
	enum class ECoverConservation : uint8
	{
		NotExercised,
		Conserved,
		Violated,
	};
	ECoverConservation CheckCoverConservation(FString& OutMessage) const;

	// MUTATION ARM for the conservation law. It refuses the FIRST cover entry the
	// index is ever offered and counts it nowhere, so its precondition is
	// identical to the law's: if the verdict is not NotExercised, this arm fired.
	// An arm that only bit when something was already out of band would report
	// nothing on a leg whose band held everything.
	void SetMutateCoverConservation(bool bOn) { bMutateCoverConservation = bOn; }

	// Registers the grid into a graph. Returns null before the first upload,
	// which the caller must treat as "no residency" rather than binding a null
	// SRV -- an unbound SRV reads as zeros, and zero is a legal index entry
	// (not resident), so the whole world would simply be empty with no error.
	FRDGBufferRef Register(FRDGBuilder& GraphBuilder);

	// Resident chunks currently indexed at level 0. Printed beside the marcher's
	// stats: zero here against 87,800 in the pool is the two halves not having
	// met, and it must never be able to look like an empty world.
	// APPLYDELTA, SPLIT INTO ITS TWO HALVES. This function turned out to be the
	// ENTIRE cost of FVoxelBrickPool::Flush -- 2,600-3,500 ms per 5 s window
	// across both call sites -- which makes it the streaming tick's dominant
	// item and therefore the world's generation ceiling.
	//
	// The split matters because Removed was ALWAYS EMPTY until brick removal
	// landed (Wave 1.2, 2026-08-22). If Removed dominates, that wave made the
	// ceiling worse and the number is mine; if Added dominates, the cost
	// predates it. Guessing either way from the totals is not good enough.
	struct FApplyDeltaMs
	{
		double RemovedMs = 0.0;
		double AddedMs = 0.0;
		double UploadMs = 0.0;   // MarkDirtyAndUpload: once per flush, size-independent
		int64 RemovedCount = 0;
		int64 AddedCount = 0;
	};
	FApplyDeltaMs GetAndResetApplyDeltaMs()
	{
		const FApplyDeltaMs Out = ApplyDeltaMs;
		ApplyDeltaMs = FApplyDeltaMs();
		return Out;
	}

	int32 GetNumEntries() const { return NumEntries; }
	// Per level, for the mode fingerprint: two legs with equal totals can hold
	// completely different level splits, and a ring cascade's behaviour depends
	// on which ring is populated.
	int32 GetNumEntriesAtLevel(uint32 Level) const
	{
		const int32 Slot = GridSlotForLevel(int32(Level));
		return (Slot >= 0) ? PerSlotEntries[Slot] : 0;
	}
	// Uploads performed, and entries dropped for being outside the grid's
	// vertical band. A non-zero drop count is not an error -- the band is finite
	// -- but it bounds how much of the world the marcher can possibly see.
	uint64 GetUploads() const { return Uploads; }
	// Bytes pushed across all uploads. At six levels one dirty frame is 48 MiB,
	// so this is the number that decides whether the per-level dirty rebuild is
	// required or merely available.
	uint64 GetUploadBytes() const { return UploadBytes; }

	// A CONTENT HASH OF THE WHOLE GRID, and it exists because equal counts are
	// not equal contents.
	//
	// Two legs reported indexResidency 26,217 and 26,211 -- 0.02% apart -- while
	// the comparator's `lost` term swung 150x. Identical rays, near-identical
	// residency, wildly different result. A count cannot distinguish "the same
	// 26,000 chunks" from "26,000 chunks, a different few hundred of them", and
	// that difference changes the geometry the rays traverse.
	//
	// So the grid hashes itself at upload. Two legs whose hashes match were
	// marching THE SAME WORLD, and any remaining difference is in the walk or in
	// GPU execution -- which is a completely different investigation from "the
	// volume differed".
	uint64 GetContentHash() const { return ContentHash; }

	// THE HASH IS OFF BY DEFAULT, AND THE MEASUREMENT IS WHY.
	//
	// MarkDirtyAndUpload ran an FNV-1a over the WHOLE 4 MiB grid on the GAME
	// THREAD, once per flush, unconditionally. Measured 2026-08-22 on a moving
	// leg: 3,146-3,190 ms PER 5 SECOND WINDOW -- against a streaming tick that
	// totals ~3,700 ms. It was ~85% of the tick, and therefore the world's
	// generation ceiling, since throughput is MaxJobsInFlight x frame rate.
	//
	// ITS ONLY CONSUMER IS THE SOURCE COMPARATOR (VoxelMarchRenderer.cpp:4471,
	// feeding MarchIndexHash), which is voxel.March.VerifySource -- DEFAULT 0,
	// and which additionally never arms today because its settle criterion mixes
	// render frames with 5-second perf ticks. So the most expensive thing in the
	// streaming tick existed to feed a disabled diagnostic.
	//
	// Enabled explicitly by the comparator rather than by a cvar read in here,
	// so this class keeps no opinion about who wants it.
	void SetContentHashEnabled(bool bEnabled) { bContentHashEnabled = bEnabled; }
	bool IsContentHashEnabled() const { return bContentHashEnabled; }
	// WAS A DEAD COUNTER. It was declared, reset, printed in the attach log as
	// "%d dropped outside the chunk band" -- and NEVER INCREMENTED ANYWHERE. It
	// reported 0 forever, which read as "nothing is being dropped" when it
	// actually meant "nothing is being counted". Repurposed to the drop that is
	// real now that the grid is per-level: an entry offered at a level this
	// index does not carry.
	int32 GetDroppedWrongLevel() const { return DroppedWrongLevel; }
	// HOW MANY ENTRIES THE POOL OFFERED at each level, before any filtering.
	// This is the question "is L1 empty because the pool never offered it, or
	// because we dropped it" answered directly instead of inferred -- the two
	// have completely different owners and completely different fixes.
	int32 GetOfferedAtLevel(int32 Level) const
	{
		return (Level >= 0 && Level < kOfferBuckets) ? OfferedPerLevel[Level] : 0;
	}
	static constexpr int32 kOfferBuckets = 8;

	// THE EXACT NUMBER OF TIMES ONE CHUNK SHADOWED ANOTHER, per level.
	//
	// This replaces a SPAN HEURISTIC that was wrong in three separate ways and
	// fired a catastrophic-sounding warning into every leg's log while three
	// legs of ratios were read out of that same log:
	//
	//   1. ObservedMin/Max were CUMULATIVE OVER THE SESSION and never reset, so
	//      they described the union of everything ever resident as the camera
	//      moved. It reported a 9,603-chunk span -- 30.7 km, the clipmap extent.
	//      ALIASING REQUIRES SIMULTANEITY: two chunks that were never resident
	//      at the same moment cannot shadow each other, and a union over time
	//      cannot tell the difference.
	//   2. Removals never shrank it, so an evicted chunk contributed forever.
	//   3. It called itself "level-0" while being fed level-1 coordinates too
	//      once the grid went per-level -- two coordinate spaces in one span.
	//
	// A collision is now OBSERVED rather than inferred: an add that lands on a
	// cell already owned by a different chunk coordinate IS the shadowing, and
	// it is counted where it happens. Exact, one map probe on the add path, and
	// it cannot report a catastrophe that is not occurring.
	int32 GetAliasCollisions(int32 Level) const
	{
		const int32 Slot = GridSlotForLevel(Level);
		return (Slot >= 0) ? AliasCollisions[Slot] : 0;
	}
	// The cumulative span is KEPT, per level, but it is a diagnostic about how
	// far the camera has travelled -- NOT an aliasing claim. Named so it cannot
	// be read as one again.
	FIntVector GetCumulativeCoordSpan(int32 Level) const;

private:
	void ApplyDelta(const struct FVoxelBrickIndexDelta& Delta);
	void Seed(const TArray<struct FVoxelBrickIndexEntry>& Snapshot);
	void MarkDirtyAndUpload();
	void NoteObservedSpan(const FIntVector& Coord, int32 Slot);
	void NoteCellOwner(uint32 Cell, const FIntVector& Coord, int32 Slot);
	// false => outside the cover band, refused and counted. Always true for a
	// ring level: rings are bounded by their own presets and the static_asserts.
	bool AdmitToSlot(const FIntVector& Coord, int32 Slot);

	// The CPU shadow. 4 MiB, one dword per cell, resident for the process.
	TArray<uint32> Cells;
	// Staged for the next graph. See MarkDirtyAndUpload.
	TArray<uint32> Staged;
	bool bStagedValid = false;
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	bool bAttached = false;
	bool bDirty = false;
	int32 NumEntries = 0;
	FApplyDeltaMs ApplyDeltaMs;
	// BY GRID SLOT, NOT BY LEVEL, and renamed so the two cannot be confused at a
	// call site. GetNumEntriesAtLevel maps through GridSlotForLevel.
	int32 PerSlotEntries[kGridSlots] = {};
	int32 DroppedWrongLevel = 0;
	int32 OfferedPerLevel[kOfferBuckets] = {};
	// The cover funnel. offered == resident + droppedOutOfBand, checked.
	int32 CoverOffered = 0;
	int32 CoverAdmitted = 0;
	int32 CoverDroppedOutOfBand = 0;
	bool bCoverMutationFired = false;
	FIntVector CoverBandCentreChunk = FIntVector::ZeroValue;
	bool bCoverBandCentreSet = false;
	bool bMutateCoverConservation = false;
	// The chunk-coordinate extent actually seen, per axis. The static_assert
	// proves no aliasing GIVEN the ring presets; this catches the presets being
	// overridden on the command line, which the assert cannot see.
	FIntVector ObservedMin[kGridSlots];
	FIntVector ObservedMax[kGridSlots];
	int32 AliasCollisions[kGridSlots] = {};
	bool bAliasComplained = false;
	// cell -> the chunk coordinate currently occupying it. Only occupied cells,
	// so it is the size of the resident set (tens of thousands), not of the grid.
	TMap<uint32, FIntVector> CellOwner;
	uint64 Uploads = 0;
	uint64 UploadBytes = 0;
	uint64 ContentHash = 0;
	bool bContentHashEnabled = false;
};

// The one the marcher consumes. Same free-function shape as
// GetGlobalVoxelBrickPool, and for the same reason: defined construction order,
// and a test can stand up its own without fighting a singleton.
VOXELEARTHSHADERS_API FVoxelMarchChunkIndex& GetGlobalVoxelMarchChunkIndex();

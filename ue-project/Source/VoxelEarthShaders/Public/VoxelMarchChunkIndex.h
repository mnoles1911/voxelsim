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

#include <atomic>

class FRDGBuilder;
class FRDGPooledBuffer;
class FRHIGPUBufferReadback;

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
	// existing static_assert therefore generalises as written -- INCLUDING to
	// level 6 (2026-08-23, the 8.19 km ring): 2 * 8192 m / 204.8 m = 80, the
	// same safe number, because the ring keeps the Outer/ChunkEdge == 40
	// construction ratio. That ratio is the entire aliasing argument; a ring
	// that broke it (e.g. a 10.24 km level 6, ratio 50, span 100) would still
	// fit under 128 but would have to prove it separately.
	//
	// 8 MiB per level, 56 MiB across seven.
	//
	// THE FULL REBUILD IS NOW THE THING TO WATCH. Rebuild-and-upload-everything
	// was chosen so stale slots are structurally impossible, and at one level on
	// dirty frames it was free. At seven ring levels plus the cover slot it is
	// 64 MiB per dirty frame (56 MiB before level 6 landed), and during
	// cold fill that is most frames -- roughly 3.8 GB/s at 60 Hz, which is a
	// streaming regression introduced by the renderer rather than a rounding
	// error.
	//
	// IT IS INSTRUMENTED, NOT PRE-OPTIMISED. The plan's next step is a per-level
	// dirty rebuild (the pool's delta already carries Key.Level, and most
	// flushes touch one or two levels), then scatter upload only if that shows
	// up in a frame time. Measure before optimising is the rule that kept the
	// first version blunt and it applies to its own successor. GetUploadBytes()
	// is what decides it.
	//
	// 7 SINCE 2026-08-23: grid for the 8.19 km ring (VoxelCoords::kNumLevels).
	// This is the LAST widening this constant can take -- kCoverLevel (7) is
	// the ceiling of the record's four-bit and the VisBuffer's three-bit level
	// fields, and with seven ring grids kCoverLevel == kRingGrids exactly, so
	// an eighth ring would collide with cover in GridSlotForLevel (the
	// static_assert in the .cpp fires).
	//
	// AND ON 2026-08-30 IT DID, exactly as written: R7 (the 8 km cascade) took
	// slot 7 and the assert fired, so kCoverLevel moved 7 -> 8 in both of its
	// spellings and kGridSlots became 9. The paragraph above is kept because its
	// PREDICTION was right and is the reason the collision was a compile error
	// rather than cover chunks decoding as ring hits. Note the default it names
	// (-VoxelMaxRingLevel absent = 5) is stale twice over; it is 7 now. The grid exists whether or not the
	// ring streams; the default run (-VoxelMaxRingLevel absent = 5) simply
	// leaves slot 6 empty: +8 MiB of buffer, +8 MiB on the rare FULL uploads
	// (attach and structural events), and zero extra ROUTINE traffic -- the
	// delta path only moves dirty cells and an unstreamed slot never dirties.
	static constexpr uint32 kLevels = 11;  // 11 ring levels: R0 64 m .. R10 65536 m

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
	static constexpr int32 kCoverLevel = 11;  // R9/R10 took slots 9 and 10 at the 65 km cascade
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

	// How many ring LEVELS the streaming side is actually populating this run
	// (GetMaxRingLevel() + 1), pushed once by UVoxelWorldSubsystem at
	// streaming init. THE MARCHER'S DEFAULT RING COUNT FOLLOWS THIS
	// (voxel.March.RingCount 0 = follow) so that arming the 8 km ring with
	// -VoxelMaxRingLevel=6 cannot leave the renderer silently walking six
	// rings over seven resident levels -- a switch that is on and draws
	// nothing is the failure this project has paid for most often, and the
	// symptom (nothing past 4 km) would read as a streaming bug, not a
	// mismatched cvar. Lives HERE because this object is the one handle both
	// modules already share: VoxelEarth cannot be included by the renderer
	// (dependency direction), and a second spelling of the cascade depth in
	// cvars is exactly the drift this class keeps paying for.
	//
	// Atomic because the write is game-thread (init, before the first march)
	// and the read is the render thread's ring-spec build; relaxed is enough
	// -- there is no data ordered behind it, it is a single self-contained
	// count, and a one-frame-late read at worst walks the old ring count for
	// that frame against an index whose extra slot is merely still empty.
	void SetStreamedRingLevels(int32 NumLevels)
	{
		StreamedRingLevels.store(FMath::Clamp(NumLevels, 1, int32(kRingGrids)), std::memory_order_relaxed);
	}
	int32 GetStreamedRingLevels() const
	{
		return StreamedRingLevels.load(std::memory_order_relaxed);
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

	// ---- THE CPU SHADOW DOES NOT OWN kAnySolidBit ANY MORE -----------------
	//
	// Every CPU writer of a resident entry still sets kAnySolidBit
	// UNCONDITIONALLY (Seed and ApplyDelta's add loop, and the GPU publish
	// kernel's addition arm), and that is deliberate rather than left over:
	// the CPU CANNOT ANSWER THE QUESTION. Under voxel.GPU.PoolAlloc (default
	// on) the live producer is FVoxelBrickPool::AllocateGpuChunkShell, which
	// queues its index add BEFORE the record has been computed at all;
	// FResidentChunk, FVoxelBrickIndexEntry and FVoxelGpuBrickPayload carry no
	// solidity field, and the only structure that does -- FVoxelBrickCpuPack::
	// bAnySolid -- belongs to the fallback producer that is not running. A
	// "solidity flag in the snapshot" would therefore light up only the
	// producer nobody uses.
	//
	// So the truth lives on the GPU, in the record, and the bit is made real
	// THERE: VoxelMarchIndexRefineMain (VoxelMarchIndexScatter.usf) re-reads
	// the record that is in the slot NOW, re-runs the marcher's own
	// origin-and-level validation against it, and CLEARS bit 30 by CAS only
	// after positive proof of air. Every early return in that kernel leaves
	// the bit SET, because the two error directions are not symmetric:
	//
	//     bit says SOLID, chunk is air  -> lose the saving, stay correct.
	//     bit says AIR, chunk has solid -> the marcher skips real ground.
	//
	// WHICH IS WHY THE HASHES BELOW MASK IT. The CPU shadow keeps its
	// hardcoded 1 while the GPU buffer holds the refined 0, so Cells[] and the
	// GPU buffer legitimately differ in exactly this bit and
	// voxel.March.IndexDeltaVerify would fail on every sample. Both sides of
	// that comparison hash through HashableCell(), which drops bit 30 --
	// the same "an instrument must not indict a change it was not asked
	// about" move AreAbsentMarksArmed() already makes for the absent
	// annotation. The verify still covers residency, slot and every absent
	// bit; it stops covering the ONE bit the GPU is now authoritative for,
	// which the refine kernel's own audit arm covers instead.
	static constexpr uint32 kHashCellMask = ~kAnySolidBit;
	static uint32 HashableCell(uint32 CellValue) { return CellValue & kHashCellMask; }

	// ---- the refine/audit kernel's stats buffer -----------------------------
	//
	// PUBLIC because the kernel's ModifyCompilationEnvironment pushes these
	// offsets as shader defines, which is the only arrangement in which the
	// CPU reader and the GPU writer cannot drift -- the same reason the
	// hole-stats census pushes VoxelMarchHoleWord rather than hand-mirroring
	// it. Order IS the buffer layout: appending a word is safe, reordering is
	// a silent misread of a plausible number out of the wrong slot.
	//
	// UNDER THE AUDIT MODE the kernel writes the SAME layout into the second
	// block (kRefineStatBaseAudit) and two of the words carry a different
	// reading: Examined becomes "resident, identity-matched, record says
	// SOLID" (the denominator that must be non-zero for a pass to mean
	// anything) and Cleared becomes "and its index bit 30 was CLEAR" -- the
	// hole count. GetUploadStats() folds them into AuditChecked /
	// AuditWrongClear so no caller has to carry that knowledge.
	static constexpr int32 kRefineStatWords = 13;
	enum ERefineStatWord : int32
	{
		RefineStat_Examined = 0,
		RefineStat_NoMatch,
		RefineStat_RefusedZeroRecord,
		RefineStat_RefusedOrigin,
		RefineStat_RefusedLevel,
		RefineStat_RefusedCell,
		RefineStat_RefusedStride,
		RefineStat_RefusedInconsistent,
		RefineStat_Cleared,
		RefineStat_CasLost,
		RefineStat_LeftSolid,
		RefineStat_AlreadyClear,
		// THE AUDIT'S OWN DENOMINATOR, and it is a separate word because it
		// started life sharing Examined and that was a counter-name lie.
		// The shared prologue increments Examined for EVERY slot, so an audit
		// that also used it for "identity-matched chunks the record proves
		// solid" reported checked = slots + solid chunks -- 87,283,419 where
		// the real denominator was 4,708,059, an 18x inflation that would have
		// made a clean wrongClear=0 look far stronger than it was. Caught on
		// the 2026-08-27 red leg only because checked EXCEEDED
		// dispatches x ChunkCapacity, which is impossible for one thread per
		// slot counting once.
		RefineStat_AuditSolid,
	};
	static_assert(int32(RefineStat_AuditSolid) + 1 == kRefineStatWords,
	              "the refine stats readback is sized by the enum; the kernel indexes the "
	              "same offsets and a short buffer reads another allocation's dwords.");
	// TWO BLOCKS IN ONE BUFFER, refine at 0 and audit at kRefineStatWords, and
	// that separation is not tidiness. Both arms can be armed on the same leg
	// (that is the ONLY combination in which the audit is judging the refine),
	// and both write the word named Cleared -- the refine's "bits I cleared"
	// and the audit's "bits that are wrongly clear". Sharing one block would
	// add a prize to an alarm and report the sum as either.
	static constexpr int32 kRefineStatBufferWords = kRefineStatWords * 2;
	static constexpr int32 kRefineStatBaseRefine = 0;
	static constexpr int32 kRefineStatBaseAudit = kRefineStatWords;

	// ---- THE COARSE OCCUPANCY LEVEL (voxel.March.BlockSkip) ----------------
	//
	// ONE LEVEL ABOVE THE CHUNK INDEX AND EXACTLY ONE. The index is 99.7%
	// empty -- 50,052 resident chunks in a 16,777,216-cell, 64 MiB grid -- and
	// empty space costs the marcher one scattered 4-byte load into that 64 MiB
	// per 3.2 m at level 0. A ray at the horizon crosses hundreds of them:
	// VoxelMarch.March measures 1.108 ms looking straight down against 5.638 ms
	// at sky, 5.1x, tracking EMPTY SPACE CROSSED rather than geometry hit.
	//
	// WHY NOT A PYRAMID, settled and not to be relitigated: our own spike
	// measured a SECOND coarse level moving miss-cost 56.5 -> 34.6 steps and
	// hit-cost 48.6 -> 68.9. Levoy 1990 published the reason -- a level pays
	// only where P(empty) x distance skipped exceeds the cost of the test, and
	// that product collapses at both ends of the ray. Literature at 0.55%
	// occupancy: one occupancy map 9.0x, a distance field on top +4.5%, a third
	// structure +0%. ONE LEVEL.
	//
	// TWO BITS PER BLOCK AND THAT IS A CORRECTNESS REQUIREMENT, NOT FIDELITY.
	// The skip must not perturb bCrossedAbsentChunk, which gates the
	// fine->coarse fallthrough ladder that keeps black arcs out of LOD
	// boundaries. The walk sets that flag for NON-RESIDENT chunks and
	// deliberately NOT for resident-empty ones (a resident-empty chunk is real
	// air; falling through there would let a coarser level plug cave mouths and
	// overhangs with rock). So the skip has to know which kind of emptiness it
	// is skipping, and it READS that rather than deriving it -- see the shader's
	// use of MarchBlockAnyAbsent.
	//
	// WHAT THE TWO BITS CAN AND CANNOT SAY, stated here because the difference
	// is invisible from the names. Both are derived from ONE count -- resident
	// cells in the block -- as
	//     Occupied  = (ResidentCount >  0)
	//     AnyAbsent = (ResidentCount < 64)
	// which makes the state (Occupied 0, AnyAbsent 0) -- "all 64 chunks resident
	// and every one of them genuinely empty air" -- UNREACHABLE. That is not a
	// defect and it costs no correctness: a block holding ANY resident chunk
	// reads Occupied and is descended into chunk by chunk, so resident-empty
	// chunks are still walked individually and still do not set the flag.
	//
	// AND IT STAYS A DERIVATION EVEN NOW THAT kAnySolidBit IS REAL, which is a
	// deliberate decision and not an oversight. This paragraph used to say the
	// state was unreachable "until a real solidity signal exists" and that the
	// second buffer was kept so the flag could become a READ on the day one
	// did. That day is voxel.March.IndexAnySolid (2026-08-27) -- the GPU refine
	// pass clears bit 30 on proof of air -- and the derivation is STILL right,
	// because these counts live on the GAME THREAD and the solidity now lives
	// on the GPU. Wiring the block bits to the refined value would mean
	// reading a GPU-owned bit back to the CPU once per flush, which is the
	// per-chunk readback direct-to-pool exists to have removed.
	//
	// The two levels stay independent and both stay safe: the block level is
	// derived from residency and over-reports Occupied (costing a descent), and
	// the chunk level then rejects the air chunks on the index dword the
	// descent already loaded. Over-reporting at the coarse level and proving at
	// the fine one is the same direction of safety the refine kernel itself
	// runs on. The second buffer is still kept rather than folded into
	// !Occupied, for the reason it always was: a derivation that has silently
	// detached from what it claims to describe is the failure this file keeps
	// paying for.
	//
	// 4x4x4 BLOCKS OF THE EXISTING GRID. 32,768 blocks per slot x 8 slots =
	// 262,144 blocks = 32 KiB per bitfield, against the 64 MiB it bounds.
	static constexpr uint32 kBlockChunks = 4;
	static constexpr uint32 kBlockDimXY = kDimXY / kBlockChunks;
	static constexpr uint32 kBlockDimZ = kDimZ / kBlockChunks;
	static constexpr uint32 kBlocksPerSlot = kBlockDimXY * kBlockDimXY * kBlockDimZ;
	static constexpr uint32 kNumBlocks = kGridSlots * kBlocksPerSlot;
	static constexpr uint32 kBlockWords = kNumBlocks / 32u;
	// How many chunk cells one block covers. The counter below is uint8 and that
	// is EXACT rather than lucky: 64 fits, 256 would not.
	static constexpr uint32 kChunksPerBlock = kBlockChunks * kBlockChunks * kBlockChunks;
	// THE TOROIDAL SEAM, PINNED. The grid wraps with `coord & (dim-1)`, so a
	// block is a region of the WRAPPED cell grid, not of world chunk space. That
	// is only coherent while the block size DIVIDES the dimension -- otherwise
	// one 4^3 block would straddle the wrap and hold two disjoint pieces of the
	// world, and the two spellings of the block index
	// ((coord & (dim-1)) / 4 and (coord / 4) & (dim/4 - 1)) would stop agreeing.
	// 128 % 4 == 0, and this is what says so.
	static_assert(kDimXY % kBlockChunks == 0 && kDimZ % kBlockChunks == 0,
	              "a 4^3 block must not straddle the toroidal wrap: kBlockChunks has to "
	              "divide kDimXY and kDimZ. If a dimension stops being a multiple of the "
	              "block size, the block index and the cell index describe different "
	              "regions and the marcher skips ground it holds -- a hole.");
	static_assert(kChunksPerBlock == 64,
	              "BlockResidentCount is uint8, and the AnyAbsent derivation spells 64 "
	              "directly. Both follow the block size; change one and change both.");
	static_assert(kNumBlocks % 32u == 0,
	              "the block bitfields are packed 32 blocks to a dword with no tail.");

	// (chunk coord, grid slot) -> block index, the MIRROR of CellOf and derived
	// the same way round: wrap first, then divide. Mirrored again in
	// VoxelMarchIndexBlockCompute (VoxelMarchIndexCell.ush) for the GPU reader,
	// under exactly the discipline CellOf's mirror lives under.
	static uint32 BlockOf(const FIntVector& Coord, int32 Slot)
	{
		const uint32 bx = (uint32(Coord.X) & (kDimXY - 1u)) / kBlockChunks;
		const uint32 by = (uint32(Coord.Y) & (kDimXY - 1u)) / kBlockChunks;
		const uint32 bz = (uint32(Coord.Z) & (kDimZ - 1u)) / kBlockChunks;
		return uint32(Slot) * kBlocksPerSlot + bx + kBlockDimXY * (by + kBlockDimXY * bz);
	}

	// ---- THE ABSENT-CELL ANNOTATION (voxel.March.HoleStats 2) --------------
	//
	// A non-resident cell used to be literally 0, which threw away exactly
	// what the dark-arcs investigation needs: WHY the chunk is not held.
	// These bits are the CPU's answer, written on admission (reason 1) and on
	// eviction (reason 2 replaces the old bare 0), read by the marcher in the
	// same load its residency test already pays. The 9-bit tag records which
	// torus wrap wrote the annotation so an aliased cell cannot lend its
	// history to a different chunk 128 cells away -- a tag mismatch on the GPU
	// side is classified as never-admitted, which is a deduction (any
	// transition of the wanted coord would have stamped its own tag), not a
	// guess. Mirrored BY HAND as VOXEL_MARCH_ABSENT_* in
	// VoxelMarchIndexCell.ush, the same mirror discipline kResidentBit lives
	// under, proven by the same voxel.March.IndexDeltaVerify readback gate.
	//
	// ARMED ONLY at voxel.March.HoleStats >= 2 and with
	// voxel.March.IndexGpuResident OFF: a control run's index stream must be
	// byte-identical, and the Phase 2 publish kernel clears cells to literal 0
	// on the GPU while the shadow would hold a tag -- a guaranteed
	// delta-verify FAIL, so the writer stands down instead (and the perf line
	// prints its write counters, so a disarmed writer is visible rather than
	// read as "nothing pending, nothing evicted").
	static constexpr uint32 kAbsentReasonMask    = 0x3u;
	static constexpr uint32 kAbsentReasonNone    = 0u;
	static constexpr uint32 kAbsentReasonPending = 1u;
	static constexpr uint32 kAbsentReasonEvicted = 2u;
	// ---- THE OPEN-SKY MARK (voxel.Stream.SkyMark, 2026-08-25) --------------
	//
	// Reason 3 was "reserved, classified as unattributable" and NOTHING HAS
	// EVER WRITTEN IT -- MakeAbsentEntry has exactly two call sites, pending
	// and evicted. It is claimed here for the state the marcher has never had:
	//
	//     THE COVERAGE RULE CONSIDERED THIS CHUNK AND AFFIRMATIVELY DECLINED
	//     IT, BECAUSE IT IS PROVABLY ABOVE THE TERRAIN SURFACE. OPEN SKY.
	//
	// WHY THAT IS THE MISSING FACT. Only a thin surface shell streams -- 4.4
	// chunks per column at level 0, 50,052 resident of 16,777,216 cells. AIR IS
	// NOT RESIDENT, so a non-resident cell has always had to be treated as
	// BLOCKING: a chunk that has not streamed may contain surface. That is why
	// voxel.March.ZCut skipped 0.00% at the horizon and why voxel.March.BlockSkip
	// paid 23.7 block tests to avoid 11.2 cells and ran 30% slower -- there was
	// nothing anywhere a skip was PERMITTED to advance through. This code is the
	// permission.
	//
	// AND IT COSTS THE MARCHER NOTHING TO CARRY. It rides the same dword the
	// residency test already loads, in bits a non-resident entry was throwing
	// away. No new buffer, no new load, no new inner-loop test. Any design that
	// added an inner-loop test is dead on arrival here; three arms proved it.
	//
	// THE DEFAULT BIT PATTERN MEANS "NOT SKY", AND THAT IS THE WHOLE SAFETY
	// ARGUMENT FOR THE ENCODING. Every way a cell can come to be unwritten,
	// cleared, uploaded before this feature existed, or zero-filled reads as
	// reason 0 = NONE, and NONE is not sky. The bits that mean sky can only
	// appear where this class wrote them. An inverted encoding -- where the
	// zero pattern meant sky -- would make a freshly cleared grid, a Seed(), a
	// GPU publish clear and a torus wrap all claim the world is empty, and the
	// marcher would skip the ground. The direction is not arbitrary.
	//
	// THE TAG IS PART OF THE MARK, not decoration. Bits [10:2] carry
	// AbsentTagOf(Coord) exactly as the other two reasons do, so a cell holding
	// a torus ALIAS's sky mark (a coord 128 cells away) does not read as sky for
	// the coord actually being asked about. The GPU-side test is
	// reason == OPENSKY && tag == AbsentTagOf(wanted), and a tag mismatch falls
	// back to NOT SKY -- the safe side, exactly as the never-admitted deduction
	// does for the reason split.
	//
	// UNCHANGED FOR THE HOLE BREAKDOWN, DELIBERATELY. VoxelMarchClassifyAbsent
	// still maps reason 3 to VOXEL_MARCH_MISS_NEVER, which is byte-identical to
	// what an open-sky cell reads TODAY (unwritten = NONE = NEVER) and is also
	// literally true of it: never admitted, and now with the reason recorded.
	// Widening the four-bucket histogram to five would shift every word index in
	// the hole-stats readback and rebase an instrument that is currently
	// working, to add a bucket the dedicated skyMark counters already report.
	static constexpr uint32 kAbsentReasonOpenSky = 3u;
	static constexpr uint32 kAbsentTagShift      = 2u;
	static constexpr uint32 kAbsentTagMask       = 0x1FFu;
	static_assert(kAbsentReasonOpenSky <= kAbsentReasonMask,
	              "the reason field is two bits; a fifth code needs a wider field AND a matching "
	              "widening of the tag shift in BOTH this file and VoxelMarchIndexCell.ush.");
	static_assert(kAbsentReasonNone == 0u,
	              "THE SAFETY ARGUMENT FOR THE OPEN-SKY ENCODING: every unwritten, cleared, "
	              "zero-filled or pre-feature cell must read as NOT SKY. That holds only while "
	              "the all-zero pattern is a reason code that is not the sky code.");

	// (coord >> 7) per axis, 3 bits each. The 7 is log2(kDimXY) == log2(kDimZ);
	// the static_asserts pin that so a future grid resize cannot silently make
	// every tag stale.
	static uint32 AbsentTagOf(const FIntVector& Coord)
	{
		static_assert(kDimXY == 128 && kDimZ == 128,
		              "AbsentTagOf hardcodes >>7 == log2(dim); update the shift and the "
		              ".ush mirror together");
		return ((uint32(Coord.X) >> 7) & 7u) | (((uint32(Coord.Y) >> 7) & 7u) << 3) |
		       (((uint32(Coord.Z) >> 7) & 7u) << 6);
	}
	static uint32 MakeAbsentEntry(uint32 Reason, const FIntVector& Coord)
	{
		return (Reason & kAbsentReasonMask) | (AbsentTagOf(Coord) << kAbsentTagShift);
	}

	// Attaches to the global brick pool: registers the delta sink AND seeds the
	// grid from the current resident set, in ONE call, because registering late
	// is the normal case and a register-then-snapshot pair leaves a window where
	// a delta lands on an unseeded index. GAME THREAD ONLY. Idempotent.
	void AttachToGlobalPool();
	void Detach();

	// ---- the absent-annotation writer (voxel.March.HoleStats 2) ------------
	//
	// NoteChunkAdmitted: the streaming admission just created a record and
	// queued a job for this chunk -- stamp its cell "admitted, not yet
	// resident" so a marcher ray that misses it can be attributed to
	// THROUGHPUT rather than to coverage or eviction. Writes only a
	// non-resident cell (a resident one means the pool already holds it, and
	// its eventual removal writes the evicted annotation instead). Coord is
	// the brick/march chunk coordinate, Level the ring level 0..5; the cover
	// grid deliberately has no annotations -- absent cover chunks are the
	// NORMAL state ("no cover here" stores nothing at all) and annotating them
	// would drown the ring signal in noise.
	//
	// NoteChunkNoLongerAdmitted: the admission was cancelled before the chunk
	// ever generated (queue-cap truncation, or an unload of a record that
	// never produced bricks). Clears ONLY a pending annotation that names this
	// exact coord back to 0 -- an evicted annotation is history worth keeping,
	// and a resident cell belongs to the delta path.
	//
	// Both are cheap shadow writes; nothing uploads until FlushAbsentMarks(),
	// which the streaming tick calls once after its admission pass so a burst
	// of admissions costs one staging, not one per chunk. All three are
	// no-ops unless voxel.March.HoleStats >= 2 (a control run's index stream
	// must stay byte-identical) and voxel.March.IndexGpuResident is 0 (the
	// GPU publish kernel writes literal 0s and would fail delta-verify against
	// an annotated shadow). GAME THREAD ONLY.
	void NoteChunkAdmitted(const FIntVector& Coord, int32 Level);
	void NoteChunkNoLongerAdmitted(const FIntVector& Coord, int32 Level);
	void FlushAbsentMarks();

	// ---- the open-sky writer (voxel.Stream.SkyMark) ------------------------
	//
	// The streaming side's sky trim proved this chunk is above the terrain
	// surface and declined to admit it; record that decision in the cell.
	// Coord is the brick/march chunk coordinate, Level the ring level; the
	// cover slot is refused for the same reason the other annotations refuse
	// it. GAME THREAD ONLY. Rides FlushAbsentMarks like its two siblings.
	//
	// NOT GATED ON voxel.March.HoleStats, AND THAT IS THE ENTIRE POINT OF THIS
	// ADDITION. The pending/evicted annotations beside it are armed only at
	// HoleStats >= 2, which is why in EVERY perf run every absent cell reads
	// NONE -- the information exists only on legs nobody measures, and a
	// marcher fast path cannot be built on a fact that is absent whenever the
	// clock is running. This one is armed whenever its own streaming switch is
	// on, whatever HoleStats says.
	//
	// THE ONE GATE IT KEEPS is voxel.March.IndexGpuResident, and that is
	// correctness rather than policy: the Phase 2 publish kernel writes literal
	// 0 into cells on the GPU while the shadow would hold a mark, a guaranteed
	// delta-verify FAIL. Standing down there is counted, not silent --
	// IsOpenSkyWriterArmed is what the perf line reads to say so.
	//
	// RETURNS THE OUTCOME rather than void, because the caller's cell counters
	// need a denominator that closes: written + refusedResident + refusedOther
	// == offered. A writer that reports nothing turns "the sink is disconnected"
	// and "the sky is empty" into the same reading, which is the failure
	// IsAttached() was added for one layer up.
	enum class EOpenSkyMark : uint8
	{
		Written,          // the cell now carries this coord's open-sky mark
		RefusedResident,  // kResidentBit is set: a chunk holds this cell (torus alias, or a race)
		RefusedOther,     // already marked, annotated pending/evicted, or the writer is disarmed
	};
	EOpenSkyMark NoteChunkOpenSky(const FIntVector& Coord, int32 Level);

	// Whether the open-sky writer would currently write. Separate from
	// AreAbsentMarksArmed() on purpose: that one also requires HoleStats >= 2,
	// which is the gate this feature exists to escape.
	bool IsOpenSkyWriterArmed() const;
	// Monotonic, never reset, same rule as the pending/evicted mark counters:
	// a rate can be derived from a monotonic count, but a zero must stay
	// distinguishable from "nobody incremented this".
	uint64 GetOpenSkyMarks() const { return OpenSkyMarks; }
	// Recomputed by a full sweep, not maintained incrementally -- see the
	// definition. Cheap (262,144 byte reads) and called once per perf window.
	void GetBlockSkyCensus(uint64& OutLicensed, uint64& OutPartial, uint64& OutTouched) const;

	// The writer's own liveness, printed on the perf line beside the GPU's
	// reason split: a split read against zero writes here is a split over
	// stale bits and must be said so. Never reset -- monotonic, like the
	// cover offer counters, and for the same reason: a rate can be derived,
	// but a zero must be distinguishable from "nobody incremented this".
	uint64 GetAbsentPendingMarks() const { return AbsentPendingMarks; }
	uint64 GetAbsentEvictedMarks() const { return AbsentEvictedMarks; }
	// True when the two cvars currently allow annotation writes; the perf
	// line prints WHICH gate closed it when false.
	bool AreAbsentMarksArmed() const;

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

	// ---- THE COARSE LEVEL'S BUFFERS, HANDED OUT WITH THE INDEX -------------
	//
	// ONE CALL, AND THE COHERENCE ARGUMENT IS THAT IT IS ONE CALL. The block
	// bitfields describe the residency of the very cells the index carries, so a
	// marcher that saw one without the other would skip ground the index holds
	// (a hole) or descend into ground it does not (slower, harmless). There is
	// no ordering rule to get right here because there is no window: all three
	// buffers are staged together in MarkDirtyAndUpload from one snapshot of one
	// shadow, and consumed together here, into one graph, before any pass this
	// function's caller adds can read any of them.
	//
	// Index == nullptr keeps its existing meaning -- never uploaded, the caller
	// must skip its pass -- and the block members are nullptr in exactly the
	// same case, so a caller cannot end up binding one and not the other.
	struct FBuffers
	{
		FRDGBufferRef Index = nullptr;
		FRDGBufferRef BlockOccupied = nullptr;
		FRDGBufferRef BlockAnyAbsent = nullptr;
		// THE THIRD BIT, AND ITS SAFE DIRECTION IS THE OPPOSITE OF THE OTHER
		// TWO. BlockOccupied's unbound value (zeros) means "nothing resident
		// here", which would skip the world -- hence its all-ones fallback.
		// BlockAllSky's unbound value (zeros) means "no block is provably sky",
		// which makes the arm INERT. So this one needs no inverted fallback:
		// every way it can fail to arrive leaves the marcher descending exactly
		// as the control does. Stated because the asymmetry looks like an
		// oversight and is not.
		FRDGBufferRef BlockAllSky = nullptr;
	};
	FBuffers RegisterWithBlocks(FRDGBuilder& GraphBuilder);

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
	// Bytes STAGED for upload, and it now tells the truth per path: a full
	// staging adds the whole grid (kCells * 4 B = 56 MiB), a delta staging adds
	// 8 B per changed cell (a [cell, value] dword pair). A staging that is
	// replaced before Register() consumes it is subtracted back out, so this
	// tracks bytes that actually cross to RDG rather than bytes that were
	// merely prepared. This is the number that decided the delta path was
	// required: ~10 GB per 5 s window staged to move ~0.065% of the grid.
	uint64 GetUploadBytes() const { return UploadBytes; }
	// See BlockFallbackBinds. Non-zero while a block-skip leg is running means
	// some binds marched against an all-ones coarse level, i.e. against the
	// control, and their timings are not the arm's. READ IT AGAINST
	// GetBlockBindCalls, never against a frame count -- there are up to four
	// binds per frame and mixing the two produced one wrong reading already.
	uint64 GetBlockFallbackBinds() const
	{
		return BlockFallbackBinds.load(std::memory_order_relaxed);
	}
	// The denominator for the above. Every RegisterWithBlocks call that got as
	// far as needing a coarse level (i.e. the index was bound).
	uint64 GetBlockBindCalls() const
	{
		return BlockBindCalls.load(std::memory_order_relaxed);
	}

	// ---- Wave 1.3: delta-upload accounting ---------------------------------
	//
	// Every counter here exists because the full-upload cost hid for weeks
	// behind a stale "4 MiB" comment. The split says WHICH path ran and WHY,
	// so the next reader can tell "the delta path is off" from "the delta path
	// is on but always falling back" from the counters alone -- those have
	// completely different owners.
	struct FUploadStats
	{
		// Stagings that carried the whole grid vs only changed cells. With
		// voxel.March.IndexDeltaUpload at its default 0, FullUploads counts
		// every flush and DeltaUploads stays 0 -- the control-leg fingerprint.
		uint64 FullUploads = 0;
		uint64 DeltaUploads = 0;
		// Cells staged across all delta stagings, and the size of the most
		// recent staging (kCells for a full one) -- "cells uploaded per flush".
		uint64 DeltaCellsStaged = 0;
		uint32 LastStagedCells = 0;
		// Why the FULL path ran while the delta switch was ON. "First" is the
		// designed cold start (there is nothing on the GPU to patch), "Seed" is
		// a reseed after attach (the whole grid changed meaning), "Pending"
		// means a full staging was already waiting and absorbed this flush,
		// "Large" is the voxel.March.IndexDeltaMaxCells threshold.
		uint32 FullBecauseFirst = 0;
		uint32 FullBecauseSeed = 0;
		uint32 FullBecausePending = 0;
		uint32 FullBecauseLarge = 0;
		// voxel.March.IndexDeltaVerify results: the delta-patched GPU buffer,
		// read back whole and FNV-hashed, against the hash of the CPU grid it
		// was patched to equal. A single failure is a wrong world.
		uint64 VerifyPasses = 0;
		uint64 VerifyFailures = 0;
		// Stagings the verify gate wanted to sample but could not, because
		// every readback ring slot was still in flight. NOT an error -- a
		// sampled gate is sufficient (a wrong cell is persistent divergence)
		// -- but if this dwarfs Passes+Failures the ring is undersized for the
		// flush rate and the gate's coverage claim weakens.
		uint64 VerifySkippedNoSlot = 0;

		// ---- Phase 2: GPU-written residency (voxel.March.IndexGpuResident) --
		//
		// Flushes whose index cells were written by the PUBLISH kernel -- the
		// GPU deriving cell and value itself -- rather than staged as
		// CPU-snapshotted pairs. "Cells written by GPU vs CPU" is
		// GpuCellsWritten + GpuCellsCleared against DeltaCellsStaged (plus the
		// full uploads, which are always CPU-snapshotted).
		uint64 GpuPublishes = 0;
		// Addition entries dispatched (residency the marcher discovers without
		// the game thread staging anything for it).
		uint64 GpuCellsWritten = 0;
		// Removal entries dispatched -- evictions cleared by the GPU's guarded
		// clear. Counts entries OFFERED to the guard, not guard matches: the
		// guard's refusals (a retirement for a cell this index never wrote)
		// are correct behaviour, and the CPU shadow's NumEntries is the
		// residency census that would drift if a clear were ever lost.
		uint64 GpuCellsCleared = 0;
		// The publish leaf refused a flush because CPU-staged pairs were still
		// waiting for Register(): scattering newer values ahead of an older
		// staged pair list would let the stale pairs overwrite them later.
		// The flush fell back to the CPU staging path, which merges. Expected
		// exactly once per ON-flip mid-flight; growing steadily means the
		// marcher stopped calling Register() and pairs never drain.
		uint32 GpuFellBackPendingCpu = 0;
		// The publish render command found no pooled buffer to patch --
		// structurally unreachable (the ladder requires an established base,
		// and Detach clears the mode's state before releasing the buffer),
		// but if it ever fires the entries were DROPPED, so the next staging
		// is forced full to heal, and FullBecauseLost counts that healing.
		uint32 GpuLostNoBuffer = 0;
		uint32 FullBecauseLost = 0;

		// ---- THE anySolid REFINE PASS (voxel.March.IndexAnySolid) -----------
		//
		// READ THE ++ SITE, NOT THE NAME -- these are folded from the kernel's
		// own stats buffer (VoxelMarchIndexRefineMain), one readback per
		// sampled dispatch, and each word is written at exactly one place in
		// that kernel.
		//
		// RefineDispatches is the ARM'S PROOF OF LIFE. Zero here with the cvar
		// on is "the pass never ran", which must never be read as "nothing to
		// clear". RefineStatsSamples is how many of those dispatches got a
		// readback slot; a window with dispatches>0 and samples==0 measured
		// nothing and its Cleared/WrongClear are NOT zeros, they are absences.
		uint64 RefineDispatches = 0;
		uint64 RefineStatsSamples = 0;
		// Slots the kernel looked at (the denominator), and the outcomes.
		// Examined counts every thread that got as far as reading a record;
		// Cleared is the prize. Refused is every conservative bail added
		// together -- a zeroed record, a mis-aligned origin, a level that maps
		// to no grid slot, a cell out of range, a stride cross-check failure,
		// and the record self-inconsistency check (anySolid clear over a
		// NON-empty L1 brick mask). Every one of them leaves bit 30 SET.
		uint64 RefineExamined = 0;
		uint64 RefineCleared = 0;
		uint64 RefineRefused = 0;
		// Identity did not match: the cell is not resident, or it names some
		// other slot. Ordinary and large -- most pool slots are not the tenant
		// of the cell their record's coordinate maps to once the torus has
		// moved -- and separated from Refused so a genuine refusal cannot hide
		// inside it.
		uint64 RefineNoMatch = 0;
		// The compare-and-swap lost: the cell changed between the read and the
		// write, so the clear was ABANDONED and the bit stayed set. Safe by
		// construction; counted because a large number would mean the pass is
		// racing something it should not be.
		uint64 RefineCasLost = 0;
		// The chunk was proved solid, so the bit was left alone. Examined ==
		// NoMatch + Refused + Cleared + CasLost + LeftSolid + AlreadyClear is
		// the identity that says the partition is complete.
		uint64 RefineLeftSolid = 0;
		uint64 RefineAlreadyClear = 0;

		// ---- THE AUDIT ARM (voxel.March.IndexAnySolidAudit) -----------------
		//
		// THE RED GATE, and it runs LAST in the graph -- after the index write
		// and after the refine pass -- because that is the state every march
		// pass from there until the next flush actually consumes. Auditing
		// ahead of the refine would be a test that cannot fail: on the
		// full-upload path the buffer is created in that graph with every
		// entry carrying the shadow's hardcoded 1.
		//
		// AuditChecked is resident, identity-matched cells whose record says
		// SOLID: the population that MUST have bit 30 set. It is the
		// denominator and it must be non-zero for AuditWrongClear=0 to mean
		// anything at all; a zero here is an audit that did not run, which is
		// exactly the confirmation-that-cannot-fail this project keeps paying
		// for.
		//
		// AuditWrongClear is the hole counter: a chunk the record proves has
		// solid content whose index entry says AIR. ANY non-zero value means
		// the marcher is skipping real ground, and it outranks every
		// performance number on the leg. voxel.March.IndexAnySolidPoison is
		// the arm that proves this counter CAN fire.
		uint64 AuditChecked = 0;
		uint64 AuditWrongClear = 0;
	};
	// Snapshot by value. Verify counters are written on the render thread and
	// the rest on the game thread; reads are diagnostic and a one-frame-stale
	// verify count is acceptable, a torn uint64 on x64 is not possible for
	// aligned words.
	FUploadStats GetUploadStats() const { return UploadStats; }

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
	// MarkDirtyAndUpload ran an FNV-1a over the WHOLE grid (56 MiB at today's
	// 7-slot shape; the "4 MiB" this comment used to claim was the old
	// one-level kDimZ=64 grid) on the GAME THREAD, once per flush,
	// unconditionally. Measured 2026-08-22 on a moving
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

	// ===================================================================
	// THE RESIDENT-Z BOUND (voxel.March.ZCut) -- WHY THE CUMULATIVE SPAN
	// IS THE RIGHT SOURCE HERE AND THE WRONG ONE ABOVE
	// ===================================================================
	//
	// The block above retracts ObservedMin/Max as an ALIASING claim, and the
	// reason it fails there is that aliasing requires SIMULTANEITY: a union
	// over time cannot tell two chunks that coexisted from two that merely
	// took turns. That objection does not apply to the question asked here,
	// which is the opposite shape --
	//
	//     "name a Z interval that CONTAINS every chunk this slot holds"
	//
	// -- and a union over time is a superset of the instantaneous set BY
	// CONSTRUCTION. Removals never shrinking it is the DEFECT there and the
	// SAFETY MARGIN here. That asymmetry is the whole argument, and it is why
	// this accessor exists next to that retraction rather than reusing it.
	//
	// AUTHORITATIVE BECAUSE IT SHARES A CALL SITE WITH RESIDENCY ITSELF. Every
	// cell that ever carries kResidentBit is written in exactly two places --
	// Seed's admit loop and ApplyDelta's Added loop -- and BOTH call
	// NoteObservedSpan with that coordinate immediately before the write, in
	// the same iteration. The GPU publish path (GpuPublishAdds) is filled
	// inside that same ApplyDelta iteration from the same admitted coordinate,
	// so the GPU writer cannot publish residency this span has not already
	// seen. NoteChunkAdmitted / NoteChunkNoLongerAdmitted write only ABSENT
	// annotations and never kResidentBit, so they cannot widen the set behind
	// this bound's back.
	//
	// USABLE ONLY WHILE THE SLOT HOLDS SOMETHING, and that is the same
	// condition MarchIndexLevelPopulated publishes to the shader. A slot with
	// no resident chunk has no shell, and the marcher's shell test
	// short-circuits to "every absent chunk here is a hole" for exactly that
	// case (VoxelMarchAbsentTouchesShell). A consumer that CUT an empty slot
	// would have to reproduce that short-circuit to keep the flag's meaning;
	// refusing to cut it instead is the conservative answer and costs an empty
	// slot nothing it was not already paying.
	//
	// WHAT IT DEGRADES INTO, stated because it is a real limitation and not a
	// hypothetical: a long flight widens this monotonically until the interval
	// covers the whole flown envelope and the cut stops removing anything.
	// That failure mode is LOSS OF BENEFIT, never a hole -- the bound can only
	// ever be too WIDE. A shrinking bound is Stage 2 work and needs a per-Z
	// occupancy count that eviction can decrement; do not "fix" this one by
	// resetting it on eviction, which would make it narrower than the set it
	// describes and is precisely how a hole gets shipped.
	//
	// Returns false -- and leaves the outputs untouched -- when the slot holds
	// nothing, when the level is not carried, or when nothing was ever
	// observed. FALSE MEANS "DO NOT CUT", never "the extent is empty".
	bool GetResidentChunkZBound(int32 Level, int32& OutMinZ, int32& OutMaxZ) const;

private:
	void ApplyDelta(const struct FVoxelBrickIndexDelta& Delta);
	void Seed(const TArray<struct FVoxelBrickIndexEntry>& Snapshot);
	void MarkDirtyAndUpload();
	// The two render-thread halves of the delta verify gate. Enqueue finds a
	// free ring slot and copies the whole index buffer into it inside the graph
	// that just scattered (Register's delta branch, or the publish command),
	// remembering the hash that buffer state must equal; Poll hashes completed
	// readbacks and compares. RENDER THREAD ONLY, both.
	void EnqueueDeltaVerify(FRDGBuilder& GraphBuilder, FRDGBufferRef IndexBuffer,
	                        uint64 ExpectedHash);
	void PollDeltaVerify();
	// Phase 2: consumes the flush's publish scratch (removals + deduplicated
	// additions) into a render command that scatters them into the persistent
	// index buffer via the publish kernel. GAME THREAD; called only from
	// MarkDirtyAndUpload's publish leaf, which owns the fallback ladder.
	void EnqueueGpuPublish(bool bVerifyWanted, uint64 ExpectedHash);
	void NoteObservedSpan(const FIntVector& Coord, int32 Slot);
	// ---- the coarse level's mutation points --------------------------------
	// Sized, and set to "nothing resident anywhere" -- Occupied clear, AnyAbsent
	// SET. That is the conservative start in both directions at once: nothing is
	// claimed solid that is not, and every skip that does happen still tells the
	// fallthrough ladder the truth.
	void ResetBlocks();
	// One cell BECAME resident / STOPPED being resident. Called from the two
	// sites that already carry the residency transition test and from nowhere
	// else: a caller that guesses at the transition double-counts, and the count
	// is what both bits mean.
	void NoteBlockCellResident(const FIntVector& Coord, int32 Slot);
	void NoteBlockCellAbsent(const FIntVector& Coord, int32 Slot);
	// A cell just gained a valid open-sky mark for Coord.
	void NoteBlockCellSky(const FIntVector& Coord, int32 Slot);
	// A cell is ABOUT to be overwritten with something that is not this coord's
	// open-sky mark. Call BEFORE the write, with the value the cell still holds.
	//
	// ORDERED BEFORE THE WRITE ON PURPOSE, and the ordering is the safety
	// property: the block's licence is withdrawn while the cell still proves it
	// was owed, so the licence can never outlive the mark that justified it. The
	// reverse order -- write, then reconcile -- has a window in which the block
	// still claims all-sky over a cell that no longer says so, and that window
	// is a hole. A no-op when Existing is not this coord's sky mark, which is
	// the overwhelming majority of calls.
	void ClearBlockCellSkyIfMarked(uint32 Existing, const FIntVector& Coord, int32 Slot);
	// count -> the two bits. The ONE place a count is turned into a bit.
	void RefreshBlockBits(uint32 Block);
	void NoteCellOwner(uint32 Cell, const FIntVector& Coord, int32 Slot);

	// ---- absent-annotation state (voxel.March.HoleStats 2) -----------------
	uint64 OpenSkyMarks = 0;
	// Marks written since the last FlushAbsentMarks; a bool because the cells
	// themselves already sit in DeltaPendingCells -- this only remembers that
	// an upload is owed.
	bool bAbsentMarksPending = false;
	uint64 AbsentPendingMarks = 0;
	uint64 AbsentEvictedMarks = 0;
	// false => outside the cover band, refused and counted. Always true for a
	// ring level: rings are bounded by their own presets and the static_asserts.
	bool AdmitToSlot(const FIntVector& Coord, int32 Slot);

	// The CPU shadow. One dword per cell, resident for the process. 56 MiB at
	// today's shape (kGridSlots(7) x 128^3) -- NOT 4 MiB; that figure dates
	// from one level grid at kDimZ=64, before the Z-aliasing fix and the cover
	// slot, and it survived in comments long enough to make the full-upload
	// cost look 14x cheaper than it was.
	TArray<uint32> Cells;
	// Staged for the next graph (FULL upload path). See MarkDirtyAndUpload.
	TArray<uint32> Staged;
	bool bStagedValid = false;

	// ---- THE COARSE OCCUPANCY LEVEL'S SHADOW (voxel.March.BlockSkip) --------
	//
	// RESIDENT CELLS PER 4^3 BLOCK, and it is a COUNT rather than a bit because
	// a bit cannot be maintained incrementally: clearing one cell in a block
	// says nothing about the other 63. 262,144 bytes, game thread only, NEVER
	// UPLOADED -- what crosses is the two 32 KiB bitfields derived from it.
	TArray<uint8> BlockResidentCount;
	// The two bitfields, packed 32 blocks per dword. Derived from the count in
	// ONE function (RefreshBlockBits) so there is a single authority for what a
	// count means, and refreshed at the two cell-residency TRANSITIONS the index
	// already detects -- never recomputed wholesale outside Seed.
	TArray<uint32> BlockOccupiedWords;
	TArray<uint32> BlockAnyAbsentWords;
	// ---- THE SKY-LICENSED BLOCK (voxel.March.BlockSkySkip) -----------------
	//
	// One bit per block: EVERY ONE of its 64 chunk cells carries a valid
	// open-sky mark and NONE of them is resident. That is the predicate the
	// existing block level never had -- Occupied==0 means "all 64 are
	// non-resident", which licenses nothing, and it is exactly why the
	// residency-only arm paid 23.7 block tests to avoid 11.2 cells and ran 30%
	// slower. This one licenses a 4-chunk advance off one load.
	//
	// COUNT PLUS TAG, AND THE TAG IS NOT OPTIONAL. The grid is toroidal, so a
	// cell can hold a valid sky mark stamped by a coord 128 cells away. The
	// per-cell GPU read defends against that by comparing AbsentTagOf(wanted),
	// but a BLOCK bit has no coord to compare against -- so the defence has to
	// live on the CPU side, and it works because ALL 64 CELLS OF A BLOCK SHARE
	// ONE TAG. The tag is (coord >> 7) per axis, a block spans 4 chunks per
	// axis, blocks are aligned (128 % 4 == 0), so no block can straddle a
	// 128-chunk tag boundary. BlockSkyTag records which tag the count is FOR;
	// a mark arriving with a different tag resets the count to zero, which
	// costs the block its licence until all 64 are re-marked. Lost benefit,
	// never a stale claim.
	//
	// 0xFFFF is "no tag yet" -- outside the 9-bit tag range by construction.
	TArray<uint8> BlockSkyCount;
	TArray<uint16> BlockSkyTag;
	TArray<uint32> BlockAllSkyWords;
	// ---- THE UPLOAD MIRROR, AND WHY IT IS NOT A ONE-SHOT STAGING -----------
	//
	// A PERSISTENT MIRROR THAT IS NEVER CONSUMED, refreshed by the same
	// MarkDirtyAndUpload call that stages the index, from the same instant of
	// the same shadow -- which is still the whole coherence argument. A FULL
	// 64 KiB snapshot rather than a delta: against the delta path's own ~74 KiB
	// of pairs this is noise, and a whole snapshot cannot be missing a block the
	// index staging carries.
	//
	// IT WAS A ONE-SHOT `bStagedBlocksValid` AND THAT WAS THE BUG, recorded here
	// because the shape is the interesting part and it will be tempting to
	// "simplify" it back. The flag was cleared the moment the upload was QUEUED,
	// but QueueBufferExtraction only writes the pooled pointer when the graph
	// EXECUTES. Any call that queued an upload into a graph whose extraction
	// never landed -- and the march site declines with `continue` AFTER
	// VoxelMarchBindPool has already consumed the staging -- left NOTHING TO
	// UPLOAD FROM until the next flush. On a moving world the next flush is a
	// few milliseconds away and the gap is invisible; on a STATIC leg it is
	// seconds, and the marcher spends them on the all-ones fallback, structurally
	// unable to skip anything. Measured: 210 fallback binds on a static pitch-0
	// leg, and a timing A/B taken mostly on frames where the arm could only lose.
	//
	// A GENERATION INSTEAD OF A FLAG IS WHAT MAKES IT SELF-HEALING. The mirror
	// is always uploadable, so the render side can ask "is what the GPU holds
	// the generation I want, and does it still exist" and re-upload whenever the
	// answer is no -- on the very next frame, without waiting for a flush.
	TArray<uint32> BlockMirrorOccupied;
	TArray<uint32> BlockMirrorAnyAbsent;
	TArray<uint32> BlockMirrorAllSky;
	// Bumped by MarkDirtyAndUpload under DeltaStageLock, together with the copy.
	// 0 means "nothing has ever been mirrored", which is the only state in which
	// the all-ones fallback is legitimate.
	uint64 BlockShadowGeneration = 0;
	// What the pooled buffers below were last UPLOADED from. Set optimistically
	// at queue time, which is safe ONLY because the IsValid() half of the test
	// is checked with it: if the extraction never landed, the pointer is null
	// and the upload is retried whatever this says.
	uint64 PooledBlockGeneration = 0;
	// The persistent GPU copies, the twins of Pooled. Render-thread state, and
	// released on the render thread with it for the same reason.
	TRefCountPtr<FRDGPooledBuffer> PooledBlockOccupied;
	TRefCountPtr<FRDGPooledBuffer> PooledBlockAnyAbsent;
	TRefCountPtr<FRDGPooledBuffer> PooledBlockAllSky;
	// TIMES RegisterWithBlocks HAD TO BIND THE ALL-ONES FALLBACK because no
	// block buffer existed yet (see the fallback's own note). IT MUST BE
	// READABLE rather than inferred, because the fallback is INERT BY DESIGN --
	// it makes the arm behave exactly like the control -- and an arm that is
	// silently inert is this project's house failure. A block-skip leg whose
	// fallback count is climbing has not measured the arm at all.
	//
	// COUNTED IN BINDS, NOT FRAMES, AND THE PAIR BELOW IS WHY THAT HAD TO BE
	// SAID. VoxelMarchBindPool is called up to FOUR times a frame (the march,
	// the emit hook, and two verify passes), so this counter's denominator is
	// binds and not frames -- and printing it beside a per-window frame count
	// invited exactly one wrong reading already ("210 over 349 frames" is 210
	// of ~1,400 binds, not 60% of frames). BlockBindCalls is the denominator, so
	// the ratio is computable rather than guessed at.
	//
	// BOTH ARE CUMULATIVE FOR THE PROCESS and deliberately not windowed: the
	// question they answer is "has this ever happened since boot", and the
	// grace-period check below turns "still happening after startup" into a
	// named defect rather than a number someone has to interpret.
	std::atomic<uint64> BlockFallbackBinds{0};
	std::atomic<uint64> BlockBindCalls{0};
	// Binds allowed to fall back before it is reported as a defect. The
	// legitimate window is the handful of binds between the index's first upload
	// and that graph's extraction landing -- at most one frame's worth of the
	// four bind sites, plus the same again across an attach. 64 is two orders of
	// margin on that and still catches the failure this exists for, which
	// produced 210.
	static constexpr uint64 kBlockFallbackGraceBinds = 64;
	// One-shot latch for the defect log, so a broken lifetime says so once
	// rather than every frame.
	std::atomic<bool> bBlockFallbackComplained{false};

	// ---- Wave 1.3: the delta upload path (voxel.March.IndexDeltaUpload) ----
	//
	// Cells written since the LAST staging. Game thread only; populated by
	// ApplyDelta's remove and add loops, and only while the delta switch is on
	// -- with the switch off every staging is full and an untended set would
	// grow for the life of the process.
	TSet<uint32> DeltaPendingCells;
	// Cells covered by the CURRENTLY STAGED pair list. Kept separate from
	// pending so that a staging Register() has already consumed can be dropped,
	// while one it has not must be merged into the next -- losing a cell here
	// is a wrong index entry that renders as a hole or as another chunk's
	// terrain, with no error anywhere.
	TSet<uint32> DeltaStagedCells;
	// Flat [cell, value] dword pairs, deduplicated by cell (the sets above are
	// keyed by cell), values snapshotted from Cells on the GAME thread at
	// staging time -- Register() must never read Cells itself, the game thread
	// may be rewriting it. Guarded by DeltaStageLock, and the lock is NOT
	// optional the way it would be for Staged: Staged never changes size, so a
	// concurrent overwrite tears data at worst; this array changes size every
	// staging, so an unguarded overwrite can REALLOCATE while the render
	// thread reads the old allocation -- a dangling pointer, not a torn value.
	TArray<uint32> StagedDeltaPairs;
	bool bStagedDeltaValid = false;
	FCriticalSection DeltaStageLock;
	// Whether a FULL upload has been staged since the last Detach, i.e.
	// whether there is a base on the GPU that a delta can legally patch. The
	// first upload after creation (or after a teardown) must be full -- there
	// is nothing to patch into.
	bool bDeltaBaseEstablished = false;
	// Seed() sets this: a reseed rewrites the meaning of the whole grid, so
	// the next staging must be full regardless of how few cells moved.
	bool bForceFullUpload = false;
	// Bytes of the staging that has not been consumed yet, so a replaced
	// staging can be subtracted back out of UploadBytes (see GetUploadBytes).
	uint64 PendingStagedBytes = 0;
	FUploadStats UploadStats;
	// voxel.March.IndexDeltaVerify: hash of Cells at the moment the current
	// delta pairs were staged (game thread writes, render thread reads under
	// DeltaStageLock). The readback that checks the GPU buffer against it is
	// the RING below.
	uint64 StagedContentHash = 0;
	bool bStagedHashValid = false;

	// THE VERIFY READBACK RING, replacing a single readback that CRASHED THE
	// D3D12 RHI (Fence->SyncPoints[GPUIndex] == nullptr, 28 s into a leg).
	//
	// The single-buffer design assumed one bInFlight flag was enough. It was
	// not, for a reason specific to RDG readbacks: AddEnqueueCopyPass only
	// RECORDS the copy -- the readback's fence is cleared and re-issued when
	// the graph EXECUTES. Between pass-add and execution, IsReady() polls a
	// fence still signalled from the readback's PREVIOUS completed use, so a
	// poll in that window reads stale-ready, retires the previous buffer's
	// bytes against the new expected hash, clears the flag -- and the next
	// staging re-enqueues the same readback while the first copy is still
	// outstanding. Two issued copies on one fence is exactly the assert. The
	// window is real here because Register() runs up to three times per frame
	// (marcher, GI, shadow march), each in its own graph, at 15,000+ flushes
	// per flight.
	//
	// The fix is the same shape the hole-stats and shadow-march readbacks
	// already run at high rates without crashing: N slots, arm a FREE one or
	// skip (counted -- sampling is sufficient because a wrong cell is
	// persistent divergence), and NEVER poll a slot in the frame that armed it
	// (ArmedFrame gate) so a stale fence cannot be mistaken for a result.
	// Each slot carries ITS OWN expected hash: with more than one copy in
	// flight, "the hash this buffer state was patched to equal" is per-sample
	// state, and the single shared VerifyExpectedHash was wrong the moment a
	// second sample armed.
	//
	// Two slots on purpose, not four: each holds a 56 MiB staging allocation
	// while armed, and the gate is a measurement leg's switch -- coverage
	// needs any nonzero sampling rate, not a deep pipeline.
	struct FVerifySlot
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
		uint64 ExpectedHash = 0;
		uint32 ArmedFrame = 0;
	};
	static constexpr int32 kVerifySlots = 2;
	FVerifySlot VerifySlots[kVerifySlots];

	// ---- the anySolid refine pass's own readback ---------------------------
	//
	// THE SAME RING SHAPE AS FVerifySlot ABOVE, and for the same reason: a
	// single readback re-armed before its fence retired is what crashed the
	// D3D12 command list manager here once already ("The fence for the current
	// GPU node has already been issued"). Arm a FREE slot or skip; never poll
	// a slot in the frame that armed it.
	//
	// Cheap where the verify ring is not: the payload is kRefineStatWords
	// dwords, not 56 MiB, so four slots cost nothing and the sampling rate can
	// follow the flush rate instead of trailing it.
	//
	// The AUDIT arm reuses the same buffer and the same readback, writing into
	// the two words the refine arm leaves alone: Examined becomes "resident,
	// identity-matched, record says SOLID" (the denominator) and Cleared
	// becomes "and its index bit 30 was CLEAR" (the hole count). Sharing the
	// buffer keeps one readback path rather than two; the mode uniform is what
	// says which reading applies, and GetUploadStats() folds them into
	// AuditChecked / AuditWrongClear so no caller has to know.
	struct FRefineStatsSlot
	{
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bInFlight = false;
		uint32 ArmedFrame = 0;
	};
	static constexpr int32 kRefineStatsSlots = 4;
	FRefineStatsSlot RefineStatsSlots[kRefineStatsSlots];

	// Adds the refine pass (if armed) and then the audit pass (if armed) for
	// an index buffer THIS GRAPH HAS JUST WRITTEN. Render thread only.
	// Refine before audit: the audit must judge the state the marcher will
	// read, and on the full-upload path anything ahead of the refine is
	// looking at a buffer that was created with every anySolid bit set.
	//
	// CALLED ONLY WHERE THE INDEX WAS WRITTEN -- the full upload, the delta
	// scatter, the GPU publish -- and never on the frames that merely
	// re-register the persistent buffer. That is not a saving, it is the
	// correctness condition: the refine pass reads records, and a record can
	// only have changed in a flush, and a flush always writes the index.
	void AddAnySolidPasses(FRDGBuilder& GraphBuilder, FRDGBufferRef IndexBuffer);
	void PollRefineStats();

	// ---- Phase 2: GPU-written residency (voxel.March.IndexGpuResident) ------
	//
	// Per-flush scratch for the publish kernel, populated by ApplyDelta while
	// the mode is on and consumed (or discarded -- a full staging supersedes
	// it) by the same MarkDirtyAndUpload call that ends the flush. GAME THREAD
	// ONLY; never alive across flushes, which is what keeps it out of the
	// stage lock. Removals are a flat 5-dword-per-entry list (no dedup needed
	// -- the kernel's guard makes racing removals converge); additions are
	// keyed by cell so the LAST add to a cell in one flush wins, matching the
	// shadow's sequential apply, and no two publish threads write one cell.
	struct FGpuPublishAdd
	{
		FIntVector Coord;
		int32 GridSlot = 0;
		uint32 Slot = 0;
	};
	TArray<uint32> GpuPublishRemoves;
	TMap<uint32, FGpuPublishAdd> GpuPublishAdds;
	// Set by the publish render command if it ever finds no buffer to patch
	// (entries dropped); the next MarkDirtyAndUpload forces a full staging to
	// heal, counted as FullBecauseLost. Atomic because it is the one flag that
	// crosses render -> game.
	std::atomic<bool> bGpuPublishLost{ false };

	// See SetStreamedRingLevels. Defaults to 6 -- the shipped 4 km cascade --
	// so a run where nothing pushes it behaves exactly as before level 6
	// existed.
	std::atomic<int32> StreamedRingLevels{ 6 };

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

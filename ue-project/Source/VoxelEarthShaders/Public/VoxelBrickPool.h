// Residency for the ray-marcher's brick volume (docs/ray-marching-plan-2026-08-19.md
// P2). THE BYTE LAYOUT IS docs/brick-volume-format.md; this file owns where those
// bytes LIVE and nothing about what they mean.
//
// WHAT THIS IS, AND WHAT IT IS DELIBERATELY NOT. It is modelled on
// UVoxelGpuPoolComponent -- same suballocator, same "allocate a range, write it
// GPU-side, keep a table entry" shape -- with two things removed on purpose:
//
//   * NO UPrimitiveComponent. UVoxelGpuPoolComponent is a component because it
//     draws: it needs a scene proxy, and its persistent buffers are created BY
//     that proxy, which is why voxel.GPU.VerifyPoolWrite has to spin waiting for
//     IsGpuWritable(). Nothing marches yet, so a component here would be
//     inheriting a bootstrap problem in exchange for nothing. The buffers are
//     created by the first flush.
//   * NO VERTEX FACTORY. There is no draw, no run list, no cull walk, no chunk
//     params. Visibility is a ray property in P3+; there is nothing to build.
//
// So this is a plain class with a lifetime the caller owns, which is also what
// makes it testable and what lets voxel.GPU.VerifyBrickPack stand up a PRIVATE
// pool rather than measuring whatever the streaming path happened to leave in a
// global one.
//
// THREE ARENAS AND A TABLE.
//
//   BrickDesc    8 B per brick slot, 64 slots per chunk, ALWAYS -- including
//                collapsed bricks, whose slots must exist so BrickBase +
//                brickIndex stays addressable with no indirection.
//   BrickOcc     16 dwords per MIXED brick.
//   BrickMat     the local palette (4 dwords) plus payload, per MIXED brick.
//   ChunkTable   one 32 B FVoxelMarchChunk per resident chunk.
//
// Occupancy and materials are separate arenas rather than one, for format
// section 3's reason: one 64 B occupancy load per entered mixed brick, then up
// to 22 DDA steps against registers with zero further memory traffic.
// Interleaving materials would drag payload bytes through the cache on every
// empty step.
//
// THE SUBALLOCATOR IS FVoxelGpuGeometryPool, UNCHANGED. It is first-fit over an
// offset-sorted, always-coalesced free list with unit tests behind it, and it
// counts in whatever unit the caller hands it -- "quads" is that class's
// vocabulary, not a constraint. Here the unit is a DWORD for the two payload
// arenas and a DESCRIPTOR SLOT for the table. Nothing was forked.
//
// EVERY ALLOCATION FROM THE DESCRIPTOR POOL IS EXACTLY 64 SLOTS, which is what
// makes ChunkSlot = BrickBase / 64 sound: with a 64-aligned capacity and none
// but 64-sized allocations, first-fit and coalescing preserve 64-alignment by
// induction. It is checked rather than assumed -- see AddChunkFromGpu.
//
// LIFETIME, AND THE HONEST LIMIT OF THIS PHASE. Nothing draws from this pool,
// so nothing in the streaming path knows to free from it: UVoxelGpuPoolComponent
// is freed by VoxelWorldSubsystem at eviction, park and unpark, and hooking
// those four sites belongs with the phase that makes the marcher the draw path.
// Until then this pool bounds itself: a re-add of the same key frees the old
// ranges first, and a capacity-pressured add evicts the resident chunk FARTHEST
// FROM THE EVICTION FOCUS rather than failing (insertion order when no focus has
// been set -- see EvictionOrder for why that fallback is the wrong order and why
// LRU is not the fix). THAT MATTERS TO THE MEASUREMENT AND MUST BE READ WITH IT --
// resident bytes at settle are an UPPER bound on the live set while
// GetEvictions() is zero, and a capacity artefact once it is not. Quote both
// numbers or neither.

#pragma once

#include <atomic>

// WHAT THIS HEADER DEPENDS ON, DECLARED RATHER THAN BORROWED.
//
// It declares FBufferRHIRef members (FVoxelBrickPoolBuffers) and takes an
// FRHICommandListImmediate reference (UploadCpuWrites_RenderThread). For a while
// it declared neither and simply inherited both from whatever its includer
// happened to pull in first.
//
// THE UNITY BUILD HID THAT COMPLETELY, which is the part worth remembering. In a
// unity blob some earlier translation unit had always included RHIResources.h,
// so every build reported Succeeded; a standalone compile of one consumer failed
// with nine errors naming this file. UBT chooses the adaptive non-unity path
// from git status, so the person who trips it is whoever next edits a file that
// includes this one -- a stranger to the defect, holding nine errors about a
// header they never touched, and unable to reproduce it for anyone helping them.
//
// There is no grep for this. The only reliable check is a non-unity compile of a
// consumer, which is why the rule here is simply: if this header names a type,
// this header declares it. Same family as everything else this pool has been
// bitten by -- a check reporting success while the thing under test was never
// checked.
#include "CoreMinimal.h"
#include "RHIResources.h"     // FBufferRHIRef, held by value in FVoxelBrickPoolBuffers
#include "RenderGraphFwd.h"   // FRDGBufferRef / FRDGBufferSRVRef, for the marcher seam
#include "Templates/Function.h"   // TFunction, for FVoxelBrickIndexSink
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "VoxelGpuGeometryPool.h"

class FRDGBuilder;
class FRDGPooledBuffer;
// Only ever a reference parameter here, so a declaration is enough and the RHI
// command list header stays out of every consumer. Same call this module makes
// in VoxelFluidSim.h.
class FRHICommandListImmediate;
class FRHIShaderResourceView;
class FRHIUnorderedAccessView;

// Which chunk a record describes. LEVEL IS PART OF THE KEY, not an attribute of
// it: ring level L chunk (0,0,0) and level L+1 chunk (0,0,0) are different
// chunks covering different ground, and the marcher addresses the table by
// (level, brickCoord) precisely because GI cone marching steps ACROSS rings.
// See docs/brick-volume-format.md section 6.
struct FVoxelBrickChunkKey
{
	int32 X = 0;
	int32 Y = 0;
	int32 Z = 0;
	// Ring level 0..5. Stored as int32 rather than uint8 so the key packs
	// without padding surprises and hashes reproducibly.
	int32 Level = 0;

	bool operator==(const FVoxelBrickChunkKey& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z && Level == Other.Level;
	}
	bool operator!=(const FVoxelBrickChunkKey& Other) const { return !(*this == Other); }
};

inline uint32 GetTypeHash(const FVoxelBrickChunkKey& Key)
{
	return HashCombine(HashCombine(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)),
	                   HashCombine(::GetTypeHash(Key.Z), ::GetTypeHash(Key.Level)));
}

// One chunk's packed bricks, left where the GPU wrote them.
//
// The FVoxelGpuQuadPayload contract, verbatim, and for the same reasons: the
// game thread CONSTRUCTS this and then only MOVES THE HANDLE AROUND. It must
// never dereference the buffers, and the destructor releases them on the render
// thread because they are RHI resources that render commands may still be a
// frame or two behind on.
//
// THE OFFSETS INSIDE Desc ARE CHUNK-RELATIVE. That is the whole reason this
// intermediate exists rather than the producer writing straight into the pool:
// the pool base cannot be known until the totals are read back, and keeping the
// producer's bases at zero is what makes these buffers the exact form
// docs/brick-volume-format.md defines and vxc::packChunkBricksCanonical
// produces. AddChunkFromGpu adds the base in one kernel, once.
struct VOXELEARTHSHADERS_API FVoxelGpuBrickPayload
{
	// Out of line, for FVoxelGpuQuadPayload's incomplete-type reason.
	FVoxelGpuBrickPayload();
	~FVoxelGpuBrickPayload();

	FVoxelGpuBrickPayload(const FVoxelGpuBrickPayload&) = delete;
	FVoxelGpuBrickPayload& operator=(const FVoxelGpuBrickPayload&) = delete;

	// RENDER THREAD ONLY after construction.
	TRefCountPtr<FRDGPooledBuffer> Desc;
	TRefCountPtr<FRDGPooledBuffer> Occ;
	TRefCountPtr<FRDGPooledBuffer> Mat;
	TRefCountPtr<FRDGPooledBuffer> ChunkMask;

	// Which chunk of the producing dispatch this payload describes. Both are 0
	// for the one-chunk-per-job shape the streaming path uses; they exist so a
	// batched region can hand out one payload per chunk without copying.
	uint32 SrcBrickFirst = 0;
	uint32 SrcChunkIndex = 0;
	// 64 by contract. Carried rather than assumed so a mismatch is a rejected
	// add with a message, not a half-written chunk.
	uint32 BrickCount = 0;

	// What BrickTotalMain reported: the dwords this chunk actually needs in each
	// arena. THE ONLY NUMBERS THAT CROSSED PCIe ON THIS PATH.
	uint32 OccWords = 0;
	uint32 MatWords = 0;

	// Where this chunk's arena words START inside the scratch buffers above.
	//
	// ZERO for the one-chunk-per-job shape, where the producer's write bases
	// are zero and the chunk's words begin at the front of its own buffers.
	// NON-ZERO only for a chunk handed out of a BATCHED stack region
	// (voxel.GPU.WorldGenBatch, Tier B.1): there the scratch buffers hold the
	// whole stack and the scans ran across it, so chunk c's words are the
	// contiguous run starting at the sum of its predecessors' totals -- the
	// pack order is chunk-major (decodeBrick), which is what makes the run
	// contiguous at all. Its DESCRIPTOR offset fields are then BATCH-relative,
	// and Flush's desc-write pass folds `PoolBase - SrcOccFirst` in as one
	// wrapped add; see AddFlushPasses_RenderThread for why the wrap is sound.
	//
	// Filled by the job manager at harvest, from the per-chunk totals readback
	// -- the same numbers OccWords/MatWords come from, so the four cannot
	// drift apart without the batch's own sum-vs-region cross-check failing.
	uint32 SrcOccFirst = 0;
	uint32 SrcMatFirst = 0;

	// The chunk's min corner in LEVEL-L voxel coordinates, for the record.
	FIntVector OriginVoxel = FIntVector::ZeroValue;
};

using FVoxelGpuBrickPayloadRef = TSharedPtr<FVoxelGpuBrickPayload, ESPMode::ThreadSafe>;

// ---------------------------------------------------------------------------
// The SAME chunk, packed on a worker thread instead of by a compute kernel.
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS AT ALL, because a second producer for one format is exactly
// the shape this project has been burned by. Measured on p1-brickgate-r1/r2: the
// brick pool is fed ONLY from the GPU mesh fork, and that fork dispatched 4,564
// of 88,151 chunks (5.2%) because its in-flight queue (cap 256) saturated in the
// first streaming window and never drained. Everything overflowing goes to the
// CPU worker, which had no brick code, so the volume held 4,368 chunks / 8.6 MiB
// of a target 88,151 / ~181 MiB. Raising the queue cap moves that ratio and
// leaves the structural half untouched: EDITED AND OVERLAY CHUNKS AND EVERY
// POST-EDIT RE-MESH ARE GAME-THREAD CPU ONLY, so without a CPU producer the
// brick volume silently diverges from the world after any edit. That is a
// correctness gap, not a coverage percentage, and only this path closes it.
//
// IT IS NOT A SECOND IMPLEMENTATION OF THE FORMAT. The producer is
// vxc::packChunkBricksCanonical -- the very reference voxel.GPU.VerifyBrickPack
// byte-compared the GPU kernel against (16/16 chunks, every failure counter
// zero), with 15 test cases and 726 assertions behind it. This struct is the
// engine-side carrier for what that function already returns; the fields below
// are copied out of vxc::ChunkBrickPack, never re-derived. In particular
// bAnySolid/bAllSolid come from the packer's own walk of the CELL data, because
// allSolid is not derivable from the descriptors and looks as though it is --
// the one field a re-derivation would get wrong while passing every other check.
//
// THE CONVERSION LIVES IN THE VoxelEarth MODULE, not here: this module does not
// link voxel-core (see kBricksPerChunk's comment in the .cpp), so the seam is a
// plain dword carrier and the caller does the copy.
//
// OFFSETS ARE CHUNK-RELATIVE, exactly as FVoxelGpuBrickPayload's are and for the
// same reason -- the pool base is not known until the arenas are asked. Flush
// folds the base in, and it is the only place that does.
struct VOXELEARTHSHADERS_API FVoxelBrickCpuPack
{
	// 64 descriptors, 2 dwords each, in vxc::chunkBrickIndex order. ALWAYS 128
	// dwords, collapsed bricks included -- a chunk is 64 descriptor slots, which
	// is what makes BrickBase + brickIndex addressable with no indirection.
	TArray<uint32> Desc;
	// 16 dwords per MIXED brick, ascending brick index. Empty for a chunk whose
	// bricks are all uniform, which is legal and stores COLLAPSED.
	TArray<uint32> Occ;
	// Local palette plus payload per MIXED brick, ascending brick index.
	TArray<uint32> Mat;

	// Format section 5's L1 mask: bit chunkBrickIndex set iff that brick is
	// non-empty. Straight from vxc::ChunkBrickPack::brickSolid.
	uint64 BrickSolid = 0;
	// Format section 6, LevelAndFlags bits [4] and [5]. From the packer.
	bool bAnySolid = false;
	bool bAllSolid = false;

	// The chunk's min corner in LEVEL-L voxel coordinates, for the record.
	FIntVector OriginVoxel = FIntVector::ZeroValue;

	uint32 OccWords() const { return uint32(Occ.Num()); }
	uint32 MatWords() const { return uint32(Mat.Num()); }
	// What this pack will cost the arenas if it becomes resident. 512 B of
	// descriptors and a 32 B record always, plus whatever the mixed bricks need.
	uint64 ResidentBytes() const;
};

using FVoxelBrickCpuPackRef = TSharedPtr<FVoxelBrickCpuPack, ESPMode::ThreadSafe>;

// ---------------------------------------------------------------------------
// The shader-parameter block every consumer of the brick volume needs
// ---------------------------------------------------------------------------
//
// Paste into a BEGIN_SHADER_PARAMETER_STRUCT and fill with
// FVoxelBrickPool::BindShaderParameters. Deliberately the same shape as
// VOXEL_FLUID_OCCUPANCY_PARAMETERS(), because that is the seam the marcher
// already consumes and a like-for-like swap is one less thing to get wrong.
//
// THE NAMES ARE THE CONTRACT. A consumer that spells one differently gets a
// silently unbound resource rather than a compile error -- a marcher reading a
// null SRV sees zeros, and zero is a legal descriptor (uniform AIR) and a legal
// record (anySolid clear). So the whole world would simply be empty, with no
// error anywhere. That is why this is a macro and not three lines of prose.
//
// THE TYPES ARE THE CONTRACT TOO, and they are the ones
// ue-project/Shaders/VoxelBrickPoolWrite.usf already declares for the same
// arenas: descriptors are StructuredBuffer<uint2> (8 B per brick slot, format
// section 2) and the other three are StructuredBuffer<uint>. Do not re-type the
// descriptors as uint: the pool's own writer reads them as uint2 and the pairing
// of OccWord/MatWord is what makes a descriptor one thing.
//
// THE FOUR CAPACITIES ARE NOT DECORATION. A record names a BrickBase and a
// descriptor names a 28-bit dword offset; both are absolute into arenas whose
// size the shader otherwise cannot know. A marcher that bounds-checks against
// these turns a corrupt index into a visibly missing chunk instead of a read of
// somebody else's payload, which is the one failure on this path that looks like
// terrain.
//
// NO ShaderParameterMacros.h HERE, and that is not an oversight against the
// self-containment rule at the top of this file: SHADER_PARAMETER_RDG_BUFFER_SRV
// is expanded inside the CONSUMER pass parameter struct, never here, so this
// header names no type from it. Including it would push the shader parameter
// machinery into every consumer of the pool, most of which never bind anything.
#define VOXEL_BRICK_POOL_PARAMETERS() \
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, VoxelBrickDesc) \
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,  VoxelBrickOcc) \
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,  VoxelBrickMat) \
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,  VoxelBrickChunkTable) \
	SHADER_PARAMETER(uint32, VoxelBrickDescSlots) \
	SHADER_PARAMETER(uint32, VoxelBrickOccWords) \
	SHADER_PARAMETER(uint32, VoxelBrickMatWords) \
	SHADER_PARAMETER(uint32, VoxelBrickChunkSlots) 	SHADER_PARAMETER(uint32, VoxelBrickChunkRecordDwords)

// One resident chunk, as an INDEX entry: which chunk it is, and which slot holds
// it. The whole of what a GPU-side (level, chunkCoord) -> slot lookup needs.
struct FVoxelBrickIndexEntry
{
	FVoxelBrickChunkKey Key;
	uint32 ChunkSlot = 0;
};

// What changed about the resident set in ONE flush.
//
// WHY A DELTA AND NOT A SNAPSHOT PER FRAME. A settled 4 km cascade is ~87,800
// resident chunks; a full snapshot is ~1.4 MB rebuilt and re-uploaded every
// frame to describe a set that typically moves by tens of entries. The pool
// already batches exactly this granularity -- one flush is one graph -- so the
// delta is free here and the snapshot is not.
//
// APPLY Removed BEFORE Added. Both halves can name the SAME SLOT in one delta,
// because a slot freed by an eviction can be re-allocated to a different chunk
// inside the same flush. Applied in the other order the index ends up mapping
// the OLD key to a slot that now holds the NEW chunk -- which is not a missing
// chunk, it is one chunk's bricks drawn at another chunk's coordinates, and it
// looks like terrain.
//
// THE RECORD IS AUTHORITATIVE, THE INDEX IS A HINT. Every 32 B record carries
// its own OriginVoxel and ring level (format section 6), so a consumer that
// checks the record it landed on against the key it looked up cannot be fooled
// by a stale index at all -- it gets a miss instead of a wrong chunk. That check
// is cheap (it is already in the cache line the lookup fetched) and it is the
// difference between a bug that is visible and one that is not.
struct FVoxelBrickIndexDelta
{
	TArray<FVoxelBrickIndexEntry> Added;
	// Retired slots, WITH the key that named them, so a consumer keyed by
	// (level, coord) can erase without keeping a reverse map. The slot is
	// carried too because a consumer that stores slot-major wants it and
	// deriving one from the other is the join this project keeps getting wrong.
	TArray<FVoxelBrickIndexEntry> Removed;

	bool IsEmpty() const { return Added.Num() == 0 && Removed.Num() == 0; }
};

// Called at the END of FVoxelBrickPool::Flush, on the GAME THREAD, with the
// delta for the batch that flush just enqueued.
//
// ORDERING, AND IT IS THE POINT OF THE SEAM. The pool enqueues its own render
// command FIRST and then calls this, so a consumer that enqueues its index
// upload from here lands AFTER the pool write on the same command list. The GPU
// therefore never sees an index entry for a slot the pool has not written yet.
// The reverse case -- an index that still names a slot the pool just cleared --
// is safe by construction because a cleared record reads as anySolid clear,
// i.e. "nothing here", which is why the clear writes zeros rather than being
// skipped.
using FVoxelBrickIndexSink = TFunction<void(const FVoxelBrickIndexDelta&)>;

// --- P1 (voxel.GPU.PoolAlloc): the GPU allocator's layout --------------------
//
// Computed ONCE by FVoxelBrickPool::Init when the switch is armed, then bound to
// every allocator kernel as parameters -- the ChunkRecordDwords rule: one
// authoritative copy, never restated as a literal in HLSL. See
// VoxelBrickPoolAlloc.usf's header for the allocator design this describes
// (size-class bump + free stacks over the WHOLE occ/mat arenas -- the one
// allocator both producers claim from) and for the state-buffer map these
// offsets index.
struct FVoxelBrickPoolAllocLayout
{
	uint32 OccRegionFirst = 0;  // 0 under the one-allocator rule: it owns the arena
	uint32 OccRegionWords = 0;
	uint32 MatRegionFirst = 0;
	uint32 MatRegionWords = 0;
	uint32 OccClassStep = 0;    // dwords per occ size class (and per bitmap page)
	uint32 OccClasses = 0;
	uint32 MatClassStep = 0;
	uint32 MatClasses = 0;
	uint32 FreeStackCap = 0;    // entries per class free stack
	uint32 OccTopsFirst = 0;    // state-buffer dword offsets
	uint32 MatTopsFirst = 0;
	uint32 OccStackFirst = 0;
	uint32 MatStackFirst = 0;
	uint32 OccBitmapFirst = 0;  // bitmap-buffer dword offsets
	uint32 MatBitmapFirst = 0;
	// The claim-size demand histogram (state-buffer offsets + geometry). Lives
	// between the counter block and the stack tops; the counter readback covers
	// it by construction because it reads through MatTopsFirst + MatClasses.
	// See kGpuAllocOccHistBucketWords for why it exists and what it decides.
	uint32 OccHistFirst = 0;
	uint32 OccHistBucketWords = 0;
	uint32 OccHistBuckets = 0;
	uint32 MatHistFirst = 0;
	uint32 MatHistBucketWords = 0;
	uint32 MatHistBuckets = 0;
	uint32 StateDwords = 0;     // buffer sizes, for creation
	uint32 BitmapDwords = 0;
	uint32 SideDwords = 0;

	bool IsValid() const { return StateDwords > 0; }
};

// The GPU side, in a shared holder for UVoxelGpuPoolComponent's lifetime reason:
// a render command that is a frame behind must not be able to outlive the object
// that owns the buffers it writes.
struct VOXELEARTHSHADERS_API FVoxelBrickPoolBuffers
{
	FVoxelBrickPoolBuffers();
	~FVoxelBrickPoolBuffers();

	// NOT COPYABLE, AND THE REASON IS A COMPILER ONE AS WELL AS A DESIGN ONE.
	// VOXELEARTHSHADERS_API is __declspec(dllexport) while this module builds,
	// and dllexport forces MSVC to EMIT every implicitly defined member --
	// including the copy constructor, whose unwind path instantiates
	// ~TRefCountPtr<FRDGPooledBuffer> and therefore needs the complete type this
	// header deliberately only forward-declares. Deleting the copy is what keeps
	// the forward declaration legal (it suppresses the implicit move too), and it
	// is true to the design anyway: this holder is owned by exactly one
	// TSharedPtr and shared by reference, for the lifetime reason at the top of
	// the struct. FVoxelGpuQuadPayload deletes its copies for the same reason.
	FVoxelBrickPoolBuffers(const FVoxelBrickPoolBuffers&) = delete;
	FVoxelBrickPoolBuffers& operator=(const FVoxelBrickPoolBuffers&) = delete;

	FBufferRHIRef DescBuffer;
	FBufferRHIRef OccBuffer;
	FBufferRHIRef MatBuffer;
	FBufferRHIRef ChunkTableBuffer;

	// --- P1 (voxel.GPU.PoolAlloc): the GPU allocator's own three buffers -----
	//
	// Created alongside the arenas (EnsureCreated_RenderThread) ONLY when the
	// switch was armed at Init -- their dword counts below are zero otherwise,
	// and IsValid() deliberately does not include them: an unarmed pool is
	// complete without an allocator. Zero-initialised, which IS the allocator's
	// initial state: bump cursors 0, stack tops 0, no bitmap bit set, every
	// side-table entry "nothing allocated".
	FBufferRHIRef AllocStateBuffer;
	FBufferRHIRef AllocBitmapBuffer;
	FBufferRHIRef AllocSideBuffer;
	TRefCountPtr<FRDGPooledBuffer> AllocStatePooled;
	TRefCountPtr<FRDGPooledBuffer> AllocBitmapPooled;
	TRefCountPtr<FRDGPooledBuffer> AllocSidePooled;
	uint32 AllocStateDwords = 0;
	uint32 AllocBitmapDwords = 0;
	uint32 AllocSideDwords = 0;

	bool HasGpuAlloc() const
	{
		return AllocStatePooled.IsValid() && AllocBitmapPooled.IsValid()
		    && AllocSidePooled.IsValid();
	}

	// The RDG half. RegisterExternalBuffer brings these into a graph, RDG emits
	// the UAVCompute transition, and the epilogue hands them back as SRVs. No
	// manual barrier anywhere.
	TRefCountPtr<FRDGPooledBuffer> DescPooled;
	TRefCountPtr<FRDGPooledBuffer> OccPooled;
	TRefCountPtr<FRDGPooledBuffer> MatPooled;
	TRefCountPtr<FRDGPooledBuffer> ChunkTablePooled;

	uint32 DescSlots = 0;
	uint32 OccWords = 0;
	uint32 MatWords = 0;
	uint32 ChunkSlots = 0;

	bool IsValid() const
	{
		return DescPooled.IsValid() && OccPooled.IsValid() && MatPooled.IsValid()
		    && ChunkTablePooled.IsValid();
	}

	// Bytes actually committed to VRAM. This is the CAPACITY, not the used
	// prefix -- the arenas are allocated once at full size, exactly as the quad
	// pool's are, so this is the number a VRAM census sees and it does not move.
	// FVoxelBrickPool::GetResidentBytes is the one that tracks the world.
	uint64 GetCapacityBytes() const;
};

using FVoxelBrickPoolBuffersRef = TSharedPtr<FVoxelBrickPoolBuffers, ESPMode::ThreadSafe>;

// Arena capacities, in their own units. Defaults are the cvars.
//
// SIZED FROM THE MEASURED CENSUS, NOT FROM A GUESS. vxc_volumeprobe walked the
// real cascade at 10 cm / 4 km and reported cells 128.5 MiB, occupancy 56.0 MiB,
// descriptors 23.1 MiB, palette 14.0 MiB.
//
// RESIZED 2026-08-19 WITH THE CHANGE THAT MADE THE CPU PATH PACK, and it had to
// move in the same change: at 5.2% coverage nothing was ever pressured, and at
// full coverage the old defaults are 1.35x OVER on two axes at once. The right
// chunk target is 88,151 -- every chunk that gets a mesh job, not the 50,560
// that produce geometry, because AddChunkFromGpu has no all-air/all-solid early
// return and a zero-quad chunk stores COLLAPSED (64 descriptors and a 32 B
// record) and belongs in the pool. 88,151 x 64 = 5.64M descriptor slots against
// the old 4.19M capacity, and 88,151 chunks against the old 65,536.
//
// The descriptor and chunk defaults are therefore derived from 88,151 chunks
// with ~1.49x headroom, NOT from the census's ~47,000 (which counted only
// chunks carrying geometry and so cannot see the collapsed majority). The two
// payload arenas stay derived from the CENSUS rather than from chunk count, and
// deliberately: occupancy and material bytes are paid only by MIXED bricks, so
// they do not scale with the collapsed chunks the chunk count grew by. Census
// 56.0 MiB occupancy and 142.5 MiB material (cells + palette), each carrying
// ~1.5x for churn, fragmentation, and the fact that a settled cascade is not the
// worst pose.
//
// The census's fifth line -- a 193.3 MiB FLAT BRICK INDEX -- is deliberately not
// allocated here. That is the cost of a dense addressing scheme; this design
// addresses through a 32 B record per resident chunk (~1.5 MiB across the same
// cascade) and pays a table lookup instead. Anyone comparing the pool's resident
// bytes against the census's ~415 MiB headline must subtract that line first, or
// they are comparing two different data structures.
struct FVoxelBrickPoolConfig
{
	uint32 ChunkCapacity = 0;    // resident chunks, i.e. records and 64-slot descriptor blocks
	uint32 OccWordCapacity = 0;  // dwords
	uint32 MatWordCapacity = 0;  // dwords
};

// WAVE 2. The per-chunk terms the marcher needs to shade, carried in the chunk
// record's dwords 7..9.
//
// THESE ARE THE QUAD PATH'S OWN FOUR VALUES, not a new invention:
// SampleChunkParamsForPool computes exactly these and the vertex factory
// consumes them as ChunkParams.xyzw. The marcher needs them because its shading
// is missing the SURFACE-PROXIMITY GATE, not merely a biome tint -- a +Z face
// two hundred metres down a cave must not be tinted as turf, and that test
// needs the chunk's fitted surface plane.
//
// PLAIN POD, PASSED BY VALUE, AND THAT IS THE MODULE BOUNDARY DOING ITS JOB.
// VoxelEarthShaders may not depend on VoxelEarth, and VoxelClimateProbe lives
// in VoxelEarth -- so the values are CPU-sampled there and handed here as
// numbers. Nothing in this module knows what a biome is.
struct FVoxelBrickChunkShading
{
	// Remapped 0..255 across the world's p1..p99 range, exactly as
	// FVoxelClimateBytes carries them. 128 is the neutral midpoint.
	uint8 Temperature = 128;
	uint8 Precipitation = 128;

	// The chunk's fitted surface plane, RELATIVE TO THE CHUNK ORIGIN Z.
	//
	// STAYS FLOAT32 AND DOES NOT BECOME FIXED POINT, because -1.0e30f is a
	// legal in-band value meaning "no gate, always surface"
	// (UVoxelGpuPoolComponent::kNoSurfaceGate) written by three existing
	// producers. Any fixed-point form would have to encode a sentinel four
	// orders of magnitude outside the data range.
	float SurfaceZRelUU = -1.0e30f;

	// [0:11] QY+2048, [12:23] QX+2048, at scale 256 -- the EXACT packing
	// VoxelClimate::PackSurfaceGradients produces (QX * 4096 + QY) and
	// VoxelQuadVertexFactory.ush already decodes. Mirrored rather than
	// re-derived: a second encoding of the same quantity is how two renderers
	// come to disagree about where the ground is. Zero means a flat plane.
	uint32 SurfaceGradPacked = 0;

	// What a producer with no climate passes, and what every gate and unit test
	// passes. Reproduces today's marcher behaviour exactly: mid-range climate,
	// gate disabled.
	static FVoxelBrickChunkShading Neutral() { return FVoxelBrickChunkShading(); }

	bool IsNeutral() const
	{
		return Temperature == 128 && Precipitation == 128
		    && SurfaceGradPacked == 0 && SurfaceZRelUU == -1.0e30f;
	}

	// THE ONE PLACE THE BITS ARE LAID OUT. BuildChunkRecord and the GPU kernel's
	// parameter fill both go through this, so there is nothing left in HLSL that
	// can diverge from the C++.
	void Pack(uint32& OutClimate, uint32& OutGrad, uint32& OutSurfaceZBits) const
	{
		OutClimate = uint32(Temperature) | (uint32(Precipitation) << 8);
		OutGrad = SurfaceGradPacked;
		OutSurfaceZBits = *reinterpret_cast<const uint32*>(&SurfaceZRelUU);
	}
};

class VOXELEARTHSHADERS_API FVoxelBrickPool
{
public:
	FVoxelBrickPool();
	~FVoxelBrickPool();

	FVoxelBrickPool(const FVoxelBrickPool&) = delete;
	FVoxelBrickPool& operator=(const FVoxelBrickPool&) = delete;

	// Sizes the arenas. Safe to call once; a second call with a different
	// configuration is refused rather than silently resizing, because resizing
	// would strand every resident chunk in a buffer nobody re-produces -- the
	// D4-R1 failure the quad pool logs about.
	//
	// REFUSES A CAPACITY THAT COULD OVERFLOW A DESCRIPTOR'S 28-BIT OFFSET FIELD.
	// The kernel MASKS, so an arena above 2^28 dwords (1 GiB) would produce a
	// wrong world rather than an error.
	void Init(const FVoxelBrickPoolConfig& Config);
	bool IsInitialised() const { return bInitialised; }
	const FVoxelBrickPoolConfig& GetConfig() const { return Config; }

	// Makes one chunk resident. GAME THREAD ONLY.
	//
	// Allocates from the three arenas, records the write, and returns the chunk
	// SLOT (which is also the record index). INDEX_NONE means the add was
	// refused -- the payload is then dropped and its GPU memory released, and
	// GetAllocFailures() has moved.
	//
	// Re-adding a key that is already resident FREES THE OLD RANGES FIRST and
	// reuses the slot, which is what makes a re-meshed chunk a replacement
	// rather than a leak.
	//
	// Nothing is dispatched here. The writes are batched and enqueued by
	// Flush(), for the reason UVoxelGpuPoolComponent batches its own: one render
	// command per chunk at streaming rates is the cost this whole wave exists to
	// remove.
	int32 AddChunkFromGpu(const FVoxelGpuBrickPayloadRef& Payload, const FVoxelBrickChunkKey& Key,
	                      const FVoxelBrickChunkShading& Shading);

	// The same, for a chunk vxc::packChunkBricksCanonical packed on a worker or
	// on the game thread. GAME THREAD ONLY, identical contract to the above:
	// same arenas, same suballocator, same re-add-frees-first rule, same
	// eviction, same INDEX_NONE on refusal.
	//
	// The ONE difference is where the bytes come from, and it is confined to
	// Flush: a GPU payload is copied arena-to-arena by four compute passes,
	// while this one is a Lock/Memcpy/Unlock into the same destination ranges --
	// UVoxelGpuPoolComponent::UpdateQuadRange_RenderThread's exact shape, on
	// buffers created with the same usage flags. The descriptor rebase and the
	// 32 B record are folded in CPU-side there, arithmetic-for-arithmetic with
	// BrickDescPoolWriteMain and BrickChunkRecordMain.
	int32 AddChunkFromCpu(const FVoxelBrickCpuPackRef& Pack, const FVoxelBrickChunkKey& Key,
	                      const FVoxelBrickChunkShading& Shading);

	// ---- PHASE 6: the level ground cover is keyed at -------------------------
	//
	// THE POOL OWNS THIS NUMBER because the pool owns the key. Ring levels are
	// 0..5; 6 is left free for a seventh ring; 7 is the largest value that fits
	// BOTH this key's four-bit LevelAndFlags[0:3] in the chunk record AND the
	// VisBuffer's three-bit level field, which is what lets a cover hit travel
	// from the march kernel to the emit pixel shader with no new packing.
	//
	// A COVER CHUNK IS A CANONICAL BRICK CHUNK IN EVERY OTHER RESPECT: 32^3
	// cells, 64 bricks, packed by vxc::packChunkBricksCanonical (through
	// vxc::packCoverChunk), stored in these same three arenas by these same
	// suballocators. The ONLY thing the level changes is the physical size of a
	// cell -- 50 mm instead of 100 mm -- and the only two places that has to be
	// known are FocusDistSqOf here and the ray rescale in the traversal.
	//
	// FVoxelMarchChunkIndex spells it too, for its grid-slot mapping; the two are
	// tied by a static_assert in VoxelMarchChunkIndex.cpp, which includes both.
	static constexpr int32 kCoverLevel = 7;
	// Cover cells per level-0 voxel. 100 mm / 50 mm, and it is checked against
	// vxc::kVoxelSizeMm on the producer side by vxc::coverVolumeInit, which
	// refuses a pitch that does not tile the world lattice.
	static constexpr int64 kCoverCellsPerVoxel0 = 2;

	// Where the player is, in LEVEL-0 VOXEL units, for eviction order.
	//
	// LEVEL-0 VOXELS AND NOT WORLD UNITS, deliberately. A resident chunk's
	// position is (Key.XYZ * 32) << Key.Level level-0 voxels by construction --
	// the same arithmetic FVoxelMarchChunk::OriginVoxel carries -- so the pool
	// can rank every level of the cascade against one point without knowing the
	// voxel size, the world origin, or that a streaming subsystem exists. That
	// is the smallest thing that can be handed in; a camera in UE units would
	// make this class depend on VoxelCoords.
	//
	// Optional: with no focus ever set, eviction falls back to insertion order,
	// which is what it always did.
	void SetEvictionFocusVoxel0(int64 X, int64 Y, int64 Z);

	// --- P3: the marcher's seam ---------------------------------------------
	//
	// Everything below exists so a draw path never has to touch
	// DebugGetBuffers(). That accessor is marked DEBUG ONLY and means it: it
	// hands out the raw holder with no residency check, no SRV typing and no
	// statement about which thread may look at it, which is exactly what a
	// verify gate wants and exactly what a renderer must not build on.

	// Brings the four arenas into a graph. RENDER THREAD ONLY.
	//
	// Every member is null until the first Flush has actually created the RHI
	// buffers, which is the honest answer to "nothing is resident yet" -- the
	// arenas are created lazily by the first flush, so before that there is
	// nothing to register rather than something empty to register.
	struct FRDGRefs
	{
		FRDGBufferRef Desc = nullptr;
		FRDGBufferRef Occ = nullptr;
		FRDGBufferRef Mat = nullptr;
		FRDGBufferRef ChunkTable = nullptr;

		bool IsValid() const
		{
			return Desc != nullptr && Occ != nullptr && Mat != nullptr && ChunkTable != nullptr;
		}
	};
	FRDGRefs Register(FRDGBuilder& GraphBuilder);

	// Fills a VOXEL_BRICK_POOL_PARAMETERS() block. RENDER THREAD ONLY.
	//
	// RETURNS FALSE WHEN THERE IS NOTHING TO MARCH, and the caller must skip its
	// pass rather than dispatch against a half-filled struct. False means one of
	// two things and both are ordinary: the pool has never flushed (no arenas
	// yet), or nothing is resident. Neither is an error and neither is logged --
	// it is the first few frames of every run.
	//
	// Templated on the parameter struct for FVoxelFluidOccupancyVolume's reason:
	// this header must not need to know any consumer's type, and every consumer
	// has the same eight field names because they came from the macro.
	template <typename ParametersType>
	bool BindShaderParameters(FRDGBuilder& GraphBuilder, ParametersType& Parameters)
	{
		FRDGBufferSRVRef DescSRV = nullptr;
		FRDGBufferSRVRef OccSRV = nullptr;
		FRDGBufferSRVRef MatSRV = nullptr;
		FRDGBufferSRVRef TableSRV = nullptr;
		if (!CreateSRVs(GraphBuilder, DescSRV, OccSRV, MatSRV, TableSRV))
		{
			return false;
		}
		Parameters.VoxelBrickDesc = DescSRV;
		Parameters.VoxelBrickOcc = OccSRV;
		Parameters.VoxelBrickMat = MatSRV;
		Parameters.VoxelBrickChunkTable = TableSRV;
		Parameters.VoxelBrickDescSlots = Config.ChunkCapacity * kBricksPerChunk;
		Parameters.VoxelBrickOccWords = Config.OccWordCapacity;
		Parameters.VoxelBrickMatWords = Config.MatWordCapacity;
		Parameters.VoxelBrickChunkSlots = Config.ChunkCapacity;
		// THE RECORD STRIDE, BOUND RATHER THAN ASSUMED. Every reader of this
		// table indexes Slot * stride, and the shader side carries its own
		// VOXEL_MARCH_CHUNK_RECORD_DWORDS #define. Binding the C++ constant lets
		// the reader CHECK the two agree instead of trusting that whoever grew
		// the record remembered the .ush -- see VoxelMarchLookupChunk.
		Parameters.VoxelBrickChunkRecordDwords = uint32(kChunkRecordDwords);
		return true;
	}

	// Registers the index consumer AND hands back the current resident set, in
	// ONE call. GAME THREAD ONLY.
	//
	// THE TWO HALVES ARE ONE CALL ON PURPOSE. A consumer that registered and
	// then snapshotted would have a window -- however narrow -- in which a flush
	// delivers a delta against an index that was never seeded, and the symptom
	// of that is a handful of chunks missing from the marched world with every
	// counter reading healthy. Registering late is the NORMAL case here, not an
	// edge one: the pool reaches ~87,800 chunks during the cold fill and the
	// marcher is initialised long after, so the snapshot is the bulk of the work
	// and the deltas are the tail.
	//
	// Pass a null sink to detach. The snapshot is filled either way.
	void SetIndexSink(FVoxelBrickIndexSink InSink, TArray<FVoxelBrickIndexEntry>& OutSnapshot);

	// The resident set, as index entries. GAME THREAD ONLY. O(resident).
	void SnapshotResidentIndex(TArray<FVoxelBrickIndexEntry>& Out) const;

	// --- residency by ring level --------------------------------------------
	//
	// WHAT THIS IS FOR, since it is a console-command-grade walk over the whole
	// resident map: the marcher's first step is LEVEL 0 ONLY, and whether that
	// scope covers enough of the screen to measure anything is decided by what
	// fraction of residency L0 actually is. docs/gpu-g0-sizing.md claims ring
	// chunk counts are flat by construction and R0 is ~80% of resident chunks;
	// the same claim was measured WRONG for quads on 2026-08-19 (R0 was 18.4%,
	// the distribution near-uniform). So this measures it for bricks rather than
	// letting either number be assumed.
	//
	// 16 buckets because that is what the record's LevelAndFlags [0:3] can name,
	// not because 16 levels exist -- a key outside the range is counted apart so
	// it shows up as a defect instead of silently landing in bucket 0.
	static constexpr int32 kLevelBuckets = 16;
	struct FLevelCensus
	{
		int32 Chunks[kLevelBuckets] = {};
		uint64 ResidentBytes[kLevelBuckets] = {};
		int32 OutOfRangeChunks = 0;
	};
	void GetLevelCensus(FLevelCensus& Out) const;

	// THE RACE-FREE HALF OF THE CENSUS, AND WHY IT EXISTS.
	//
	// GetLevelCensus WALKS the resident map. The marcher's settle gate called it
	// from the RENDER THREAD every frame while the game thread was streaming, and
	// UE's own check fired: "Container has changed during ranged-for iteration"
	// (SparseArray.h:1190), every leg, with a stack through
	// FVoxelMarchRenderExtension::PreRenderBasePass_RenderThread.
	//
	// A walk cannot be made safe here without a lock on the streaming hot path,
	// and the gate only ever wanted ONE NUMBER. So the count is maintained as the
	// map is mutated -- five sites, all on the game thread -- and read atomically.
	// GetLevelCensus stays for game-thread callers that want the byte breakdown.
	int32 GetResidentChunkCountAtLevel(int32 Level) const
	{
		return (Level >= 0 && Level < kLevelBuckets)
			? LevelChunkCounts[Level].load(std::memory_order_relaxed)
			: 0;
	}

	// --- the 32 B chunk record, in ONE place ---------------------------------
	//
	// LIFTED OUT OF THE UPLOAD PATH SO IT CAN BE GATED. The marcher is about to
	// mirror this layout, and "confirm it against the writer, not the doc" is
	// only a meaningful instruction if the writer is something a test can reach.
	// Inside UploadCpuWrites_RenderThread it was not: that function needs an RHI,
	// so the exact bit positions of the one field a consumer cannot recover from
	// anywhere else -- LevelAndFlags -- were checkable only by reading.
	//
	// Field for field with BrickChunkRecordMain (VoxelBrickPoolWrite.usf) and
	// with docs/brick-volume-format.md section 6. Three implementations now, and
	// voxel.GPU.VerifyBrickPack already byte-compares two of them; this is the
	// third and it is the one that runs offline.
	//
	//   [0..2] OriginVoxel xyz, LEVEL-L voxel coords, as int32 bit patterns
	//   [3]    LevelAndFlags: [0:3] ring level, [4] anySolid, [5] allSolid
	//   [4]    BrickBase, the chunk's first descriptor slot in the pool
	//   [5]    L1 mask, low  32 bits (bit chunkBrickIndex = brick is non-empty)
	//   [6]    L1 mask, high 32 bits
	//   [7]    ClimatePacked:   [0:7] temperature u8, [8:15] precipitation u8,
	//                          [16:31] ZERO                       (Wave 2, step 2)
	//   [8]    SurfaceGradPacked: [0:11] QY+2048, [12:23] QX+2048, [24:31] ZERO.
	//                          The WHOLE DWORD ZERO means a flat plane. Bit order
	//                          is NOT free choice -- VoxelClimate::PackSurfaceGradients
	//                          returns QX*4096 + QY, and VoxelQuadVertexFactory.ush
	//                          decodes (bits>>12)&0xFFF as dz/dx. Storing it any
	//                          other way makes this a SECOND encoding rather than
	//                          a transcription of the shipped one.
	//   [9]    SurfaceZRelUU:   asuint(float32). -1.0e30f is kNoSurfaceGate, a
	//                          legal in-band value meaning "no gate", which is
	//                          why this stays float and does not become fixed
	//                          point.
	//   [10..15] reserved, ZERO -- and they are WRITTEN rather than left alone,
	//          because a slot being reused would otherwise keep the previous
	//          tenant's values in fields a later format version may define.
	//
	// WHY 16 DWORDS AND NOT 10. Only 8 and 16 divide a 64 B cache line. The
	// traversal fetches dwords 0..6 for EVERY chunk a ray enters, and the
	// shading fetch wants 7..9 from the same line; at 10 or 12 dwords most
	// records straddle two lines. 16 puts one whole record in exactly one line,
	// always. Cost at the 131,072-chunk default: 4 MiB -> 8 MiB, about 1% of the
	// pool's committed total, and it buys six spare dwords in a format that
	// costs a gate, three writers, a clear kernel and a doc to move.
	static constexpr int32 kChunkRecordDwords = 16;
	// 64 descriptor slots per chunk, ALWAYS, collapsed bricks included. Public
	// because BindShaderParameters has to hand the descriptor-arena capacity to a
	// shader and deriving it from a literal at the call site is how the two halves
	// of a bound drift. The .cpp restates it in its own detail namespace (it has
	// to: that namespace is what keeps the unity build from colliding) and a
	// static_assert there ties the two together, so drift is a compile error.
	static constexpr uint32 kBricksPerChunk = 64;
	// Shading is REQUIRED AND NOT DEFAULTED, deliberately. A defaulted argument
	// would let a producer that was never plumbed compile, link, run, and ship a
	// flat mid-range climate with the surface gate disabled -- which renders as
	// perfectly plausible terrain. Callers that genuinely have no shading say so
	// with FVoxelBrickChunkShading::Neutral(), and that is greppable.
	static void BuildChunkRecord(const FIntVector& OriginVoxel, uint32 RingLevel, bool bAnySolid,
	                             bool bAllSolid, uint32 BrickBase, uint64 BrickSolid,
	                             const FVoxelBrickChunkShading& Shading,
	                             uint32 OutRecord[kChunkRecordDwords]);

	// Retires a chunk: frees its ranges and queues a record clear, so the slot
	// reads as "nothing here" the instant the clear lands. Returns false if the
	// key was not resident. GAME THREAD ONLY.
	bool RemoveChunk(const FVoxelBrickChunkKey& Key);

	// The (level, chunk coordinate) lookup the format requires the table to
	// support. INDEX_NONE when the chunk is not resident.
	//
	// P3 NOTE, so it is not discovered later: this is the CPU-side half. The
	// GPU-side L2 acceleration -- a dense toroidal chunk grid per ring level,
	// addressed like FVoxelFluidOccupancy's rolling window -- belongs with the
	// marcher that walks it, and the resident LevelAndFlags field is what makes
	// building it a lookup change rather than a format change.
	int32 FindChunkSlot(const FVoxelBrickChunkKey& Key) const;

	// Enqueues every pending write and clear as ONE render command containing
	// ONE graph. Clears are recorded before writes, so a slot retired and reused
	// in the same batch cannot end up cleared. GAME THREAD ONLY.
	void Flush();

	// --- P1: GPU-side pool allocation (voxel.GPU.PoolAlloc) ------------------
	//
	// See VoxelBrickPoolAlloc.usf for the whole design. The short form: when the
	// switch is armed at Init, the occ/mat WORD arenas belong to ONE allocator
	// living in a GPU buffer (size-class bump + free stacks), the generation
	// graph claims its own ranges and writes its own record, and the totals
	// readback -- the per-chunk fence -- does not exist on that path. Descriptor
	// slots and the record slot stay CPU-allocated for both producers (fixed
	// size, and their identity must be CPU-visible for eviction and the index).

	// The class steps, spelled once. 128 dwords rounds the occ worst case
	// (1,024) into 8 classes; 512 rounds the mat worst case (8,448) into 17.
	// Against the measured per-chunk means (~194 occ / ~208 mat dwords) the
	// round-up wastes about a quarter of a step per chunk per arena -- counted
	// exactly by the PaddedCum/ActualCum counters, so the estimate never has to
	// be believed.
	static constexpr uint32 kGpuAllocOccClassStep = 128;
	static constexpr uint32 kGpuAllocMatClassStep = 512;
	// Claim-size demand histogram bucket widths, in dwords. THE INSTRUMENT THE
	// STEP CHANGE MUST WAIT FOR: the first honest legs (2026-08-23) measured
	// padding 39.7-40.8% against the ~5-15% estimated above, and the recorded
	// MEANS are exactly the numbers that cannot choose the fix -- 194 occ mean:
	// ceil(194/64)*64 = 256 = ceil(194/128)*128, so the once-recommended
	// "OccStep 128 -> 64" sweep moves occ padding by ZERO if claims cluster at
	// the mean, while step 96 (misaligned with the cluster) could make it
	// WORSE. Which of these is true depends on the distribution's shape, so
	// the claim kernel counts every accepted total into (words-1)/width
	// buckets and the window harvest prints quantiles plus the EXACT projected
	// padding at candidate steps (any step that is a multiple of the width
	// projects exactly, because bucket upper edges land on step boundaries).
	// Widths: occ 16 -> 64 buckets over the 1,024-dword worst case; mat 64 ->
	// 132 buckets over 8,448. 196 state dwords and two InterlockedAdds per
	// claim, both unmeasurable.
	static constexpr uint32 kGpuAllocOccHistBucketWords = 16;
	static constexpr uint32 kGpuAllocMatHistBucketWords = 64;
	// Free-stack depth per class -- since 2026-08-23 this constant is only the
	// FLOOR: the armed default is max(this, ChunkCapacity), which makes stack
	// overflow IMPOSSIBLE BY CONSTRUCTION rather than unlikely (see the Init
	// comment for the induction). The original 2,048 was written when "nothing
	// in streaming frees at all today" was true; the first legs with real
	// eviction leaked 16,736 / 1,164 / 29,012 ranges across three runs of
	// identical code (the spread tracks eviction volume -- 347,709 frees on
	// the worst leg), because eviction bursts concentrate in the one or two
	// classes real chunks share and 2,048 is ~128x below the provable bound.
	// A leaked range is VRAM that never comes back; at the 50k chunks/s target
	// the bursts are ~18x tonight's and this would have been arena exhaustion
	// in minutes. Overflow still LEAKS-and-counts, never corrupts -- leakedRuns
	// and the stackPeak counters remain the gate that proves the bound holds.
	static constexpr uint32 kGpuAllocFreeStackCap = 2048;

	// Latched at Init from VoxelGpuPoolAllocEnabled(). The arming decision is
	// per-process, deliberately: claims and the CPU arenas must agree for the
	// whole run on who owns the words.
	bool IsGpuAllocArmed() const { return bGpuAllocArmed; }
	const FVoxelBrickPoolAllocLayout& GetGpuAllocLayout() const { return GpuAllocLayout; }

	// Declared ahead of its definition below (the DEBUG section) so the two
	// signatures here can name it; C++ allows the reference parameter on the
	// incomplete type.
	struct FResidentChunk;

	// The CPU half of a GPU-allocated chunk: the 64-slot descriptor block and
	// the record slot -- everything whose size is FIXED and whose identity the
	// CPU must keep. GAME THREAD ONLY. Same re-add-frees-first and
	// evict-until-it-fits semantics as AddChunkFrom*; the resident entry is
	// marked bGpuArenas and its word fields stay zero (the GPU side table is
	// the authority on the ranges). Queues the index Added entry, which the
	// next Flush delivers -- AFTER the caller has enqueued the graph that
	// writes the record, which is what makes the index seam's ordering
	// guarantee hold on this path too. OriginVoxel is the chunk's min corner in
	// level-L voxel coords -- not needed to allocate, carried so the sampled
	// verify can check the landed record against what the CPU BELIEVED, rather
	// than re-deriving it (derived, not verified, is the failure family).
	bool AllocateGpuChunkShell(const FVoxelBrickChunkKey& Key, const FIntVector& OriginVoxel,
	                           FResidentChunk& OutChunk);

	// Undo of the above for a job that failed after its shell was taken
	// (rejected, timed out, cancelled, or its graph refused the brick region).
	// ExpectSlot guards against a re-add having already replaced the entry --
	// in that case the old shell was freed by the replacement and this call
	// must not touch the new tenant. GAME THREAD ONLY.
	void ReleaseGpuChunkShell(const FVoxelBrickChunkKey& Key, uint32 ExpectSlot);

	// Enqueues one render command freeing every pending GPU-allocated range
	// (slot list -> BrickPoolFreeMain). GAME THREAD ONLY.
	//
	// THE ORDERING RULE, AND IT IS LOAD-BEARING: this must run BEFORE any
	// command that could re-CLAIM a slot freed here is enqueued. The free pass
	// reads the side table; a claim for a reused slot overwrites that entry, so
	// a free that lands after the claim would free the NEW tenant's live
	// ranges. FVoxelGpuMeshJobManager::DispatchBatch therefore calls this after
	// its shell allocations and before its ENQUEUE, and Flush() calls it before
	// its own command (which carries the CPU producer's claims). Cheap no-op
	// when nothing is pending.
	void FlushPendingGpuFrees();

	// Why a GPU-alloc-armed producer fell back to the readback path for one
	// chunk. Counted so the window line can say the arm is DECLINING work and
	// why, instead of the arm silently carrying less than it appears to.
	enum class EGpuAllocFallback : uint8
	{
		Stacked,       // Tier B.1 stack member -- stacks keep the readback path
		Discard,       // voxel.GPU.BrickPackResident 0: pack-and-discard arm
		ShellRefused,  // descriptor/record slot allocation failed (pool full)
		// A shell that WAS allocated (and counted into `shells`) but was evicted
		// by a later allocation in the same dispatch loop before its claim graph
		// was enqueued -- the job drops its brick half and no claim ever runs.
		// SPLIT OUT of ShellRefused (2026-08-23) because the two sit on opposite
		// sides of the shells-vs-claims reconciliation: a refused shell never
		// incremented `shells`, a stolen one did, and while both lived in one
		// counter the identity `shells == claims + claimFails + stolen` could
		// not be checked -- which is how a 40-count shells/claims mismatch on an
		// otherwise-clean leg went unexplained. The window line now prints the
		// residual (`shellsUnclaimed`); its FAILING reading is a value that
		// stays non-zero at quiescence AFTER the GPU counters have landed --
		// each such shell is a descriptor block and record slot resident with
		// no volume behind it, i.e. a chunk the world will show as missing.
		ShellStolen,
	};
	void NoteGpuAllocFallback(EGpuAllocFallback Reason);

	// The buffer holder, for the render-side callers this phase adds (the job
	// manager registers the arenas into its own generation graph). Never null
	// once Init has run -- the same guarantee Register() documents.
	FVoxelBrickPoolBuffersRef GetBuffers() const { return Buffers; }

	// Creates the RHI arenas (and, when armed, the allocator buffers) if they
	// do not exist yet. RENDER THREAD ONLY. Factored out of the flush command
	// because the generation graph can now touch the pool BEFORE the first
	// flush ever runs, and "the first flush creates the buffers" stopped being
	// a safe bootstrap the moment that became true.
	static void EnsureCreated_RenderThread(FRHICommandListImmediate& RHICmdList,
	                                       const FVoxelBrickPoolBuffersRef& Buffers);

	// Drops every allocation and queues a clear of nothing -- the records are
	// left as they are, because a Reset with no re-add is only used at teardown.
	void Reset();

	// --- the numbers P2 is gated on ----------------------------------------
	int32 GetNumResidentChunks() const { return Resident.Num(); }
	uint32 GetUsedOccWords() const { return OccArena.GetUsedQuads(); }
	uint32 GetUsedMatWords() const { return MatArena.GetUsedQuads(); }
	uint32 GetUsedDescSlots() const { return DescArena.GetUsedQuads(); }
	// The gap between this and the free total IS the fragmentation, per arena.
	uint32 GetLargestFreeOccRun() const { return OccArena.GetLargestFreeRun(); }
	uint32 GetLargestFreeMatRun() const { return MatArena.GetLargestFreeRun(); }
	uint32 GetLargestFreeDescRun() const { return DescArena.GetLargestFreeRun(); }
	int32 GetOccFreeRunCount() const { return OccArena.GetFreeRunCount(); }
	int32 GetMatFreeRunCount() const { return MatArena.GetFreeRunCount(); }
	// Adds that could not be satisfied even after eviction. THE P2 GATE IS ZERO.
	int64 GetAllocFailures() const { return AllocFailures; }
	// Chunks dropped to make room. Non-zero means resident bytes are a capacity
	// artefact and not a census of the world -- see the header note.
	int64 GetEvictions() const { return Evictions; }
	int64 GetChunksAdded() const { return ChunksAdded; }
	int64 GetWritesDropped() const { return WritesDropped; }
	// Split by producer, because "the pool is full" and "the CPU arm is off" are
	// different diagnoses and the totals above cannot tell them apart.
	// CHUNKS ADMITTED CARRYING NEUTRAL SHADING, i.e. no climate and no surface
	// gate. THIS IS THE COUNTER THAT CATCHES A PRODUCER NOBODY PLUMBED.
	//
	// Neutral is a legitimate answer for exactly one producer -- the cover ring,
	// which is 50 mm vegetation with no biome -- so in a healthy streaming leg
	// this must equal the cover chunk count and nothing more. Anything above it
	// is a terrain producer that compiled, linked, ran, and quietly shipped a
	// flat mid-range climate: no error, no null, and terrain that looks entirely
	// plausible. That is precisely how -VoxelBrickPack=1 once produced a fork
	// carrying zero traffic, and how the weathering pass removed 20 voxels of
	// 90,000 for months.
	// FLUSH, SPLIT INTO PREP AND ENQUEUE. Flush() measured ~18 ms per tick and is
	// the streaming tick's dominant cost, but its game-thread work before the
	// render command is only some moves and a Reserve -- which raises the
	// possibility that the ENQUEUE ITSELF is blocking on render-thread
	// backpressure. That is a completely different problem from a slow flush:
	// one is fixed by doing less work here, the other by unblocking the render
	// thread, and the single number cannot tell them apart.
	struct FFlushStageMs
	{
		double PrepMs = 0.0;     // building the index delta and moving the queues
		double EnqueueMs = 0.0;  // ENQUEUE_RENDER_COMMAND -- suspected backpressure
		double SinkMs = 0.0;     // handing the delta to the march index
	};
	FFlushStageMs GetAndResetFlushStageMs()
	{
		const FFlushStageMs Out = FlushStageMs;
		FlushStageMs = FFlushStageMs();
		return Out;
	}

	int64 GetChunksAddedWithNeutralShading() const { return ChunksAddedNeutralShading; }
	int64 GetChunksAddedFromCpu() const { return ChunksAddedFromCpu; }
	int64 GetChunksAddedFromGpu() const { return ChunksAdded - ChunksAddedFromCpu; }
	// Bytes the CPU arm has pushed across PCIe through Lock/Memcpy/Unlock. The
	// GPU arm moves its bytes arena-to-arena and costs none of this.
	int64 GetCpuUploadBytes() const { return CpuUploadBytes; }
	// Evictions that were ranked by distance from the focus rather than by
	// insertion order. The gap between this and GetEvictions() is how much of
	// the eviction traffic ran on the FIFO fallback because no focus was set.
	int64 GetEvictionsByDistance() const { return EvictionsByDistance; }

	// What the resident set actually costs, by the format's own accounting:
	// 8 B per descriptor slot in use, 4 B per used dword in each arena, 32 B per
	// record. This is the number to compare against a census; GetCapacityBytes
	// on the buffers is the number a VRAM tool sees.
	uint64 GetResidentBytes() const;

	// DEBUG ONLY -- voxel.GPU.VerifyBrickPack reads the arenas back through
	// this. Never null once Init has run.
	FVoxelBrickPoolBuffersRef DebugGetBuffers() const { return Buffers; }

	// DEBUG ONLY. Where one resident chunk landed, so a verify gate can rebase
	// what it read back instead of guessing the base -- the single most
	// expensive mistake available on this path (format section 6b).
	struct FResidentChunk
	{
		FVoxelBrickChunkKey Key;
		uint32 ChunkSlot = 0;
		uint32 BrickBase = 0;   // descriptor slots
		uint32 OccBase = 0;     // dwords
		uint32 MatBase = 0;     // dwords
		uint32 OccWords = 0;
		uint32 MatWords = 0;
		uint64 AddSequence = 0;
		// P1 (voxel.GPU.PoolAlloc): this chunk's occ/mat words were claimed by
		// the GPU allocator, so OccBase/MatBase/OccWords/MatWords above are 0 --
		// the CPU never learned them (that ignorance is the whole point; the
		// GPU-side side table holds them for the free pass). Freeing this chunk
		// must queue a GPU free (FreeResident does), never touch the CPU word
		// arenas, and never trust the four zeros as a size.
		bool bGpuArenas = false;
	};
	bool DebugGetResidentChunk(const FVoxelBrickChunkKey& Key, FResidentChunk& Out) const;

	// DEBUG ONLY. What Flush would dispatch if it ran right now.
	//
	// These exist because the pending lists carry an ORDERING RULE that no
	// offline check can reach and that the GPU cannot report: a clear is
	// recorded before every write in the batch, which is only sound because
	// RemoveChunk and eviction DROP any write the clear would invalidate. If
	// that half ever stops happening, a retired slot is resurrected by a write
	// recorded for the chunk that used to own it -- silently, and only on the
	// frame where a slot is freed and reused together. Automation covers it
	// here, on the CPU, because the alternative is covering it in a screenshot.
	int32 DebugGetPendingWriteCount() const { return PendingWrites.Num(); }
	int32 DebugGetPendingClearCount() const { return PendingClears.Num(); }

private:
	FVoxelBrickPoolBuffersRef GetOrCreateBuffers();
	// Out of line, and it is the reason this header only needs RenderGraphFwd:
	// creating an SRV needs the full FRDGBuilder, and pulling RenderGraphBuilder.h
	// into a public header would push it into every consumer. Same argument as
	// FVoxelFluidOccupancyVolume::CreateBitsSRV.
	//
	// Returns false, and touches none of the out-params, when there is nothing to
	// bind. RENDER THREAD ONLY.
	bool CreateSRVs(FRDGBuilder& GraphBuilder, FRDGBufferSRVRef& OutDesc, FRDGBufferSRVRef& OutOcc,
	                FRDGBufferSRVRef& OutMat, FRDGBufferSRVRef& OutTable);

	void FreeResident(const FResidentChunk& Chunk);
	// Evicts ONE chunk: the resident chunk farthest from the eviction focus if
	// one has been set, otherwise the earliest-inserted. Returns false only when
	// nothing is resident.
	bool EvictOne();
	// Everything AddChunkFromGpu and AddChunkFromCpu do identically: drop an
	// existing entry for the key, take the three allocations (evicting until
	// they fit), and install the resident record. Shared rather than copied,
	// because a second copy of the evict-until-it-fits loop is a second place
	// for the give-back-what-succeeded rule to be forgotten. Fills OutChunk and
	// returns true, or reports the refusal and returns false.
	bool AllocateForChunk(const FVoxelBrickChunkKey& Key, uint32 OccWords, uint32 MatWords,
	                      FResidentChunk& OutChunk);
	// Level-0 voxel distance, squared, from the focus to a resident chunk's
	// centre. Only meaningful while bHasEvictionFocus.
	int64 FocusDistSqOf(const FVoxelBrickChunkKey& Key) const;
	// Rebuilds EvictionOrder farthest-focus-first. Cheap amortised: it runs when
	// the list is exhausted or when the focus has moved far enough that the
	// order is stale, not per eviction.
	void RebuildEvictionOrder();

	struct FPendingWrite
	{
		// EXACTLY ONE of these is set. Payload means the bytes are already in
		// GPU memory and Flush copies them with the four brick passes; CpuPack
		// means they are in system memory and Flush uploads them directly.
		FVoxelGpuBrickPayloadRef Payload;
		FVoxelBrickCpuPackRef CpuPack;
		// Which chunk this write is FOR. Carried because the index delta is built
		// from the flushed batch, and a write that knew only its slot could not
		// say what now lives there.
		FVoxelBrickChunkKey Key;
		uint32 ChunkSlot = 0;
		uint32 BrickBase = 0;
		uint32 OccBase = 0;
		uint32 MatBase = 0;
		uint32 RingLevel = 0;
		// The per-chunk shading terms this write will put in dwords 7..9. Carried
		// on the write rather than looked up at flush time for the same reason
		// OriginVoxel is: the flush runs later, on a different thread, and the
		// producer that knew the values is long gone by then.
		FVoxelBrickChunkShading Shading;
		// The record's origin, carried here for the CPU arm because it has no
		// payload to read it from. Unused on the GPU arm.
		FIntVector OriginVoxel = FIntVector::ZeroValue;
	};

	// RENDER THREAD ONLY, and it deliberately does NOT execute the graph.
	// FRDGBuilder::Execute() asserts that no RDG event scope is still open, so
	// the scope has to close before the caller executes -- which it does by
	// being a function that returns. The first in-engine Flush had the scope and
	// the Execute in one block and asserted on its first dispatch; see the
	// definition. Static because it touches no pool state: everything it needs
	// was moved out of the pool under the game thread before the render command
	// was enqueued.
	//
	// bBatchedFlush is voxel.GPU.BrickFlushBatch, READ ON THE GAME THREAD in
	// Flush() and captured by value -- the render thread must see the value the
	// batch was queued under, not whatever the cvar says when the command runs.
	// false records byte-for-byte the passes this function always recorded;
	// true fuses each producing dispatch's chunks into ~4 table-driven passes
	// (see the definition for the table and the live cross-check).
	static void AddFlushPasses_RenderThread(FRDGBuilder& GraphBuilder,
	                                        const FVoxelBrickPoolBuffersRef& Buffers,
	                                        const TArray<FPendingWrite>& Writes,
	                                        const TArray<uint32>& Clears,
	                                        bool bBatchedFlush);

	// The CPU arm's half of Flush, and it runs AFTER the graph above executes.
	//
	// THE ORDER IS THE CLEAR-BEFORE-WRITE RULE, not convenience. Record clears
	// are recorded in the graph; a slot retired and re-allocated inside one batch
	// must see its clear before the write that repopulates it. A raw
	// Lock/Memcpy/Unlock recorded before Execute() would land first and be
	// zeroed by the clear that follows -- a chunk that is allocated, accounted
	// for, and blank. Running the uploads after Execute() puts them on the same
	// command list in the only order that is sound.
	//
	// RENDER THREAD ONLY. Static for AddFlushPasses_RenderThread's reason:
	// everything it needs was moved out from under the game thread first.
	static void UploadCpuWrites_RenderThread(FRHICommandListImmediate& RHICmdList,
	                                         const FVoxelBrickPoolBuffersRef& Buffers,
	                                         const TArray<FPendingWrite>& Writes);

	FVoxelBrickPoolConfig Config;
	bool bInitialised = false;

	FVoxelGpuGeometryPool DescArena;   // unit: descriptor slots, always 64 at a time
	FVoxelGpuGeometryPool OccArena;    // unit: dwords
	FVoxelGpuGeometryPool MatArena;    // unit: dwords

	TMap<FVoxelBrickChunkKey, FResidentChunk> Resident;
	// Published from the game thread as Resident is mutated, read from the render
	// thread. See GetResidentChunkCountAtLevel for why a walk was not an option.
	std::atomic<int32> LevelChunkCounts[kLevelBuckets] = {};
	void NoteResidentDelta(const FVoxelBrickChunkKey& Key, int32 Delta)
	{
		if (Key.Level >= 0 && Key.Level < kLevelBuckets)
		{
			LevelChunkCounts[Key.Level].fetch_add(Delta, std::memory_order_relaxed);
		}
	}
	// THE ORDER EVICTION WALKS, FRONT FIRST, and what it means depends on
	// whether a focus has been set.
	//
	// WITHOUT a focus it is insertion order, which is what this was and which is
	// the WORST possible order once the pool is under pressure: the chunks that
	// loaded earliest are the ones nearest the player, so a FIFO evicts the
	// ground under the camera and keeps the horizon. That never showed up while
	// the pool held 8.6% of the world and evicted nothing; it becomes visible the
	// moment coverage lands.
	//
	// WITH a focus it is sorted farthest-first, rebuilt by RebuildEvictionOrder.
	// TRUE LRU IS NOT AVAILABLE HERE AND WOULD NOT HELP: nothing reads a chunk
	// after it is added -- there is no marcher yet -- so every recency stamp
	// would equal the add stamp and LRU would degenerate to exactly the FIFO it
	// was meant to replace. Distance is the only ordering the pool can actually
	// observe, and it is also the right one.
	//
	// Keys may be stale (removed, or re-added later); the sequence number on the
	// resident record is what makes a stale entry recognisable, so this never
	// needs an O(n) removal. New adds are APPENDED, i.e. evicted last, which is
	// correct without a re-sort: a chunk that just streamed in is near.
	TArray<FVoxelBrickChunkKey> EvictionOrder;
	// Read cursor into EvictionOrder, so taking the front is O(1) instead of the
	// O(n) RemoveAt(0) this used to do on every eviction.
	int32 EvictionCursor = 0;
	uint64 NextAddSequence = 1;

	// Eviction focus, in level-0 voxels. See SetEvictionFocusVoxel0.
	bool bHasEvictionFocus = false;
	int64 EvictionFocus[3] = { 0, 0, 0 };
	// Where the focus was when EvictionOrder was last sorted. The order is
	// rebuilt when the focus has moved farther than kEvictionFocusRebuildVoxels
	// from this, or when the cursor runs off the end -- not per eviction, which
	// would be an O(n log n) sort inside an allocation retry loop.
	int64 EvictionOrderFocus[3] = { 0, 0, 0 };
	bool bEvictionOrderSorted = false;

	TArray<FPendingWrite> PendingWrites;
	TArray<uint32> PendingClears;

	// --- the index seam's state ---------------------------------------------
	//
	// THE EVICTION HAZARD, WRITTEN DOWN HERE BECAUSE HERE IS WHERE IT WILL BITE
	// AND NOT WHERE IT WILL BE READ FIRST. P4, not today.
	//
	// Right now `evictions 0` is a CORRECTNESS PROPERTY and not a happy accident:
	// nothing in the streaming path calls RemoveChunk, and the pool is sized so
	// a full cascade never pressures the arenas. Both halves of Removed below are
	// therefore always empty today, which means a consumer that never implemented
	// removal at all would pass every test that currently exists. That is the
	// trap, stated plainly.
	//
	// WHEN THE MARCHER BECOMES THE DRAW PATH THAT STOPS BEING TRUE. Streaming
	// will free from this pool at eviction, park and unpark -- the four sites the
	// class header's lifetime note already names -- and from that moment the
	// index MUST apply this delta in the SAME flush that the pool applies its
	// clears. An index updated a frame later names slots whose ranges have been
	// handed to somebody else, and because a re-allocated slot holds a REAL chunk
	// rather than garbage, the symptom is one chunk drawn at another chunk's
	// coordinates. Nothing errors. Nothing is null. It looks like terrain.
	//
	// Two things make that survivable and both are cheap: apply Removed BEFORE
	// Added (they can name the same slot in one delta), and validate the record
	// you land on against the key you looked up -- the record carries its own
	// OriginVoxel and ring level, in the cache line the lookup already fetched.
	FVoxelBrickIndexSink IndexSink;
	// Retirements queued since the last flush, in lockstep with PendingClears.
	// Kept as its own array rather than by widening PendingClears because that
	// array is handed to the render thread as a plain slot list and the index
	// half must not follow it there.
	TArray<FVoxelBrickIndexEntry> PendingIndexRemovals;

	// --- P1: GPU-side pool allocation state ----------------------------------
	// Latched at Init; the layout is only meaningful while armed.
	bool bGpuAllocArmed = false;
	FVoxelBrickPoolAllocLayout GpuAllocLayout;
	// Chunk slots whose GPU-side ranges must be returned. Drained by
	// FlushPendingGpuFrees under the ordering rule on its declaration.
	TArray<uint32> PendingGpuFreeSlots;
	// Index Added entries for shells allocated since the last Flush. Delivered
	// with (and ordered exactly like) the classic delta -- after the render
	// commands that write the records those slots name.
	TArray<FVoxelBrickIndexEntry> PendingGpuIndexAdds;
	// The CPU producer's packs while armed: uploaded and claimed through the
	// GPU allocator in Flush's render command -- the one-allocator rule. The
	// classic FPendingWrite path stays for the UNARMED pool only.
	struct FPendingGpuCpuWrite
	{
		FVoxelBrickCpuPackRef Pack;
		FVoxelBrickChunkKey Key;
		uint32 ChunkSlot = 0;
		uint32 BrickBase = 0;
		uint32 RingLevel = 0;
		FVoxelBrickChunkShading Shading;
		FIntVector OriginVoxel = FIntVector::ZeroValue;
	};
	TArray<FPendingGpuCpuWrite> PendingGpuCpuWrites;
	// Game-thread tallies for the [brick-gpualloc] window line. The GPU-side
	// truth (claims, frees, bytes in flight, the FAIL counters) lives in the
	// state buffer and reaches the log via the async window readback.
	int64 GpuShellsAllocated = 0;
	int64 GpuFreesQueued = 0;
	int64 GpuFallbackStacked = 0;
	int64 GpuFallbackDiscard = 0;
	int64 GpuFallbackShellRefused = 0;
	int64 GpuFallbackShellStolen = 0;  // see EGpuAllocFallback::ShellStolen
	// A small ring of recent shells for the sampled verify: fresh claims are
	// where an allocator bug shows first, and sampling a ring is O(1) where a
	// walk of ~100k residents per window is not. Stale entries (evicted or
	// replaced since) are recognised by slot mismatch and skipped. The origin
	// is carried from the producer so the verify checks the record against the
	// CPU's belief instead of a re-derivation.
	struct FRecentGpuShell
	{
		FVoxelBrickChunkKey Key;
		uint32 ChunkSlot = 0;
		FIntVector OriginVoxel = FIntVector::ZeroValue;
	};
	TArray<FRecentGpuShell> RecentGpuShells;
	int32 RecentGpuShellCursor = 0;
	// Samples + counter readback + window line, called from Flush while armed.
	void MaybePumpGpuAllocWindow();

	FVoxelBrickPoolBuffersRef Buffers;

	int64 AllocFailures = 0;
	int64 Evictions = 0;
	// Announces the FIRST eviction this pool ever performs, once, loudly.
	//
	// THREE SEPARATE INVARIANTS REST ON evictions BEING ZERO, and every one of
	// them fails QUIETLY rather than loudly the moment that stops being true:
	//
	//   1. PARKED-CHUNK ADOPTION. Streaming can adopt parked geometry without
	//      running a new mesh job, which is correct only because the chunk is
	//      still resident here. Evict it and nothing re-produces it -- there is
	//      no job to re-run, so the chunk is simply absent from the volume.
	//   2. THE INDEX DELTA. FVoxelBrickIndexDelta::Removed is always empty
	//      today, so a consumer that never implemented removal passes every
	//      test that exists. The first eviction is the moment that stops being
	//      an untested path and starts being a live one.
	//   3. SIZING HEADROOM. The arenas are sized so a full cascade never
	//      pressures them. An eviction means either the world got bigger than
	//      the sizing assumed or the arenas fragmented, and resident bytes stop
	//      being a census of the world and become a capacity artefact.
	//
	// A LOG AND NOT A check(). Eviction is designed behaviour that this pool is
	// supposed to perform one day; halting the editor the first time it works
	// correctly would be the wrong trade. Error rather than Warning because all
	// three consequences above are silent, and because Error is what the leg
	// harness greps -- a Warning in a five-second log stream is how this would
	// be discovered instead of announced. Once per pool, then silent: the
	// running count is already on voxel.Brick.Stats.
	bool bAnnouncedFirstEviction = false;
	int64 EvictionsByDistance = 0;
	int64 ChunksAdded = 0;
	int64 ChunksAddedFromCpu = 0;
	int64 ChunksAddedNeutralShading = 0;
	FFlushStageMs FlushStageMs;
	int64 CpuUploadBytes = 0;
	int64 WritesDropped = 0;
};

// --- the CPU arm's switches and its cost, in one place ----------------------
//
// voxel.Brick.PackOnCpu. THE CONTROL ARM FOR THIS WHOLE CHANGE, and it is a
// measurement instrument rather than a safety valve, exactly like
// voxel.GPU.BrickPackResident. A CPU-side pack is per-chunk work on the
// streaming path -- the very path the ray-marching wave exists to speed up --
// and 0 here restores the pre-change behaviour so the cost is an A/B and not an
// assumption. Read on the GAME THREAD and captured by value into a worker task,
// which is this project's established pattern for getting a gate into a job body.
VOXELEARTHSHADERS_API bool VoxelBrickPackOnCpuEnabled();

// voxel.GPU.PoolAlloc -- P1 of docs/gpu-streaming-architecture.md, the keystone.
// 1 = arena ranges for GPU-produced chunks are claimed BY THE GENERATION GRAPH
// (an InterlockedAdd in a compute pass) and the per-chunk totals readback -- the
// fence the whole GPU arm queues behind -- does not run; the CPU producer's packs
// claim through the same GPU allocator in the pool flush. 0 (default) = today's
// behaviour, byte-identical: CPU allocation from the readback, so a control leg
// needs no rebuild.
//
// LATCHED AT FIRST CALL, cvar and command line both, and the first call happens
// at pool Init -- deliberately stricter than the sibling gates' cvar-stays-live
// convention, because arming decides WHO OWNS THE ARENA WORDS and a mid-run flip
// would put two allocators over one range, which is the corruption this whole
// design exists to make impossible. Use -VoxelGpuPoolAlloc=1 for legs (the
// harness delivers cvars through -ExecCmds, after streaming has begun -- the
// same startup window every producer switch here documents).
VOXELEARTHSHADERS_API bool VoxelGpuPoolAllocEnabled();

// voxel.Brick.SuppressQuadMesh. MEASUREMENT ONLY, AND IT EMPTIES THE WORLD --
// see the cvar's own comment. 1 makes the CPU mesh worker pack bricks and skip
// quad meshing, which is the Phase 5 shape of that worker; it exists so the
// claim "the brick packer's cost is transitional" can be MEASURED rather than
// argued. AND-ed with voxel.Brick.PackOnCpu here, so it cannot leave a worker
// that produces nothing.
//
// IT CANNOT SEE voxel.GPU.BrickPack. That master gate lives with the GPU
// producer in another translation unit, so THE CALLER must AND this with its
// own ShouldPack(); VoxelWorldSubsystem.cpp does, and says why. Without that,
// BrickPack 0 + PackOnCpu 1 + SuppressQuadMesh 1 empties the world through an
// arm the master switch was supposed to have turned off.
VOXELEARTHSHADERS_API bool VoxelBrickSuppressQuadMeshEnabled();

// voxel.Terrain.RetireQuads -- PHASE 5. Off by default and not a product switch:
// with it on the world renders nearly empty beyond the near field until the
// marcher can skip empty space, cross rings and reach past 51.2 m.
//
// Already AND-ed with BOTH voxel.GPU.BrickPack and voxel.Brick.PackOnCpu, so it
// cannot leave a producer that stopped meshing without ever packing -- with
// PackOnCpu off that would have silenced the CPU worker, i.e. ~92% of traffic. Read by BOTH arms -- the CPU worker through
// VoxelBrickSuppressQuadMeshEnabled, and the GPU fork, which dispatches a
// brick-only job (bMeshChain false, no quad buffer, no quad total readback).
//
// Water is unaffected and must stay so: it runs its own independent
// UVoxelGpuPoolComponent instances. See docs/phase5-quad-retirement-plan.md.
VOXELEARTHSHADERS_API bool VoxelTerrainQuadsRetired();

// Worker-thread safe. Called once per chunk the CPU arm packs, with the wall
// time vxc::packChunkBricksCanonical took, so "what did this cost" is a measured
// number that survives to voxel.Brick.Stats.
// bFromDense says the pack read the voxels the MESHER had already read rather
// than sampling them again. It is reported separately for the reason every other
// counter on this path is: an optimisation that silently stops being taken looks
// exactly like one that was never worth taking, and this project has shipped
// that mistake before. If packs > 0 and dense packs == 0, the reuse is off.
VOXELEARTHSHADERS_API void VoxelBrickNoteCpuPack(double PackMilliseconds, bool bFromDense);
VOXELEARTHSHADERS_API int64 VoxelBrickGetCpuPackCount();
VOXELEARTHSHADERS_API int64 VoxelBrickGetCpuPackFromDenseCount();
VOXELEARTHSHADERS_API double VoxelBrickGetCpuPackMs();

// --- the three terms the Phase 5 question is made of ------------------------
//
// Phase 5 of the ray-marching plan retires terrain quad meshing, so the CPU
// worker stops meshing and only packs. Whether that is a SAVING is the whole
// question, and it decomposes exactly:
//
//   today   = mesh + pack   (the mesher materialises the voxels; the pack is free
//                            of its own fill because FDenseChunkSink hands them over)
//   phase 5 =        fill + pack   (nothing meshes, so the packer must materialise
//                                   the voxels itself)
//
// So the comparison is `mesh` against `fill`, and both are MEASURED here rather
// than inferred from sampler counts. That distinction is the reason these exist:
// the arithmetic says the mesher reads 64,000 cells per chunk against the
// packer's 32,768 and should therefore cost more -- and on this project seven
// such inferences were falsified 7/7 in a single day.
//
// All three are WORKER-THREAD time summed across threads. The game-thread edit
// re-mesh is deliberately NOT counted into them: it runs on a different thread
// with a different sampler, and mixing it in would make a per-chunk worker mean
// that describes no thread at all.
VOXELEARTHSHADERS_API void VoxelBrickNoteCpuFill(double FillMilliseconds);
VOXELEARTHSHADERS_API int64 VoxelBrickGetCpuFillCount();
VOXELEARTHSHADERS_API double VoxelBrickGetCpuFillMs();
VOXELEARTHSHADERS_API void VoxelBrickNoteCpuMesh(double MeshMilliseconds);
VOXELEARTHSHADERS_API int64 VoxelBrickGetCpuMeshCount();
VOXELEARTHSHADERS_API double VoxelBrickGetCpuMeshMs();

// Wall seconds from the FIRST chunk this process packed to the most recent one.
//
// THE THROUGHPUT METRIC THAT SURVIVES voxel.Brick.SuppressQuadMesh. Cold fill is
// normally read off `loaded=`, which counts GEOMETRY PUBLICATION -- so under the
// suppression arm it peaked at 3,243 against 50,504 in every other leg while the
// brick pool filled perfectly normally to 87,753. A metric that silently changes
// meaning under the one arm it is needed for is worse than no metric, so this
// one is denominated in packs and cannot.
//
// Reads as the fill duration when the world has settled, and keeps growing if
// the world is still churning -- so read it with jobsInFlight, exactly as every
// other convergence-sensitive number on this project must be.
VOXELEARTHSHADERS_API double VoxelBrickGetPackSpanSeconds();

// voxel.Brick.PackReuseMesherVoxels. 1 (default) = the packer consumes the
// voxels the mesher already read; 0 = it samples them itself, which is the
// pre-2026-08-19 form and cost 4.56x more (0.743 against 0.163 ms/chunk).
//
// KEPT AS A CONTROL RATHER THAN DELETED. With the reuse unconditional, the
// number that justifies it could never be re-measured on a later binary -- and
// a control that stops existing is how a measured win quietly becomes a
// believed one.
VOXELEARTHSHADERS_API bool VoxelBrickPackReuseMesherVoxelsEnabled();

// The one the streaming path publishes into. A free function rather than a
// static member so its construction order is defined and so a test can stand up
// its own pool without fighting a singleton -- voxel.GPU.VerifyBrickPack does
// exactly that.
VOXELEARTHSHADERS_API FVoxelBrickPool& GetGlobalVoxelBrickPool();

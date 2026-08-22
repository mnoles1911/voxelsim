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

DEFINE_LOG_CATEGORY_STATIC(LogVoxelMarchIndex, Log, All);

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

	// 4 MiB, zeroed. Zero is "not resident" by construction -- kResidentBit
	// clear -- so an unseeded grid reads as an empty world rather than as
	// garbage slots, which is the safe direction.
	Cells.SetNumZeroed(int32(kCells));

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
	}

	const double UploadStart = FPlatformTime::Seconds();
	ApplyDeltaMs.AddedMs += (UploadStart - AddedStart) * 1000.0;
	ApplyDeltaMs.AddedCount += Delta.Added.Num();

	bDirty = true;
	MarkDirtyAndUpload();
	// PAID ONCE PER FLUSH REGARDLESS OF HOW MANY ENTRIES MOVED, and it hashes
	// the WHOLE 4 MiB grid with FNV-1a on the game thread. If this dominates,
	// the fix is flush frequency or an incremental hash -- nothing to do with
	// Removed or Added, and nothing to do with Wave 1.2.
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
	UploadBytes += uint64(Cells.Num()) * sizeof(uint32);

	// FNV-1a over the whole grid. Order-dependent by construction, which is what
	// is wanted: two grids holding the same chunks in different CELLS are
	// different worlds to a ray, and a commutative checksum would call them
	// equal. 4 MiB of adds once per dirty frame, on the game thread, and only
	// while the volume is still moving.
	if (bContentHashEnabled)
	{
		uint64 Hash = 1469598103934665603ull;
		for (uint32 V : Cells)
		{
			Hash ^= uint64(V);
			Hash *= 1099511628211ull;
		}
		ContentHash = Hash;
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
	Staged = Cells;
	bStagedValid = true;
}

FRDGBufferRef FVoxelMarchChunkIndex::Register(FRDGBuilder& GraphBuilder)
{
	if (bStagedValid)
	{
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
		return Buffer;
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

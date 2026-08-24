#pragma once
// The eviction index: a (Level, ChunkX>>2, ChunkY>>2) bucket index over
// FVoxelWorldImpl::ChunkRecords, so RecomputeDesiredSet's hysteresis-exit scan
// can walk the records whose evict verdict could have changed instead of all of
// them.
//
// WHY THIS FILE EXISTS, AND WHY IT CONTRADICTS A RECORDED DECISION.
//
// docs/status.md ("Deliberately not done") says, in as many words: "Did not
// make the exit scan incremental/bucketed... A round-robin cursor would have
// been real work that measured as noise afterwards -- the classic mis-fix this
// task exists to avoid." That note is not wrong; it is OUT OF DATE, and only
// one number moved. It was reasoned when `tracked` was ~10k and the whole exit
// scan was 0.5 ms. The marcher-era figure in VoxelWorldSubsystem.cpp is
// 271,549 tracked records -- a 27x change in the premise -- and the exit scan
// measures 45-78 ms per 5 s window, ~30% of RecomputeDesiredSet, now that
// Phase 1 (commit b34c80e) has isolated exitScanMs to the record walk ALONE
// and stopped it carrying the fine-tier residency tick and the queue filter.
// 27x more records is what makes the walk worth narrowing; nothing else about
// the old reasoning has been overturned.
//
// It is also NOT the round-robin cursor that note rejected, and the difference
// is the whole point. A cursor amortises the same total work over more calls
// and therefore lets a record's verdict go stale for a while. This does not
// defer anything: every record whose verdict could differ from "keep" is
// examined on every call, exactly as before. It only declines to re-derive
// "keep" for records that provably cannot have changed.
//
// WHAT THE BUCKETING EXPLOITS. The exit verdict for a record is
//
//     evict = bBeyondOuter || bInsideInner || bBeyondVertical
//
// and the first two are pure functions of (Level, ChunkX, ChunkY) and the
// anchor's XY -- an annulus test on the chunk CENTRE, with no dependence on
// chunk Z and no dependence on anything stored in the record. So a fixed
// world-aligned 4x4 lattice cell (the bucket) has a bounded set of possible
// centres, and a couple of dozen flops decide whether ANY member of it could
// answer "evict". In the shipping configuration nearly every bucket cannot:
// -VoxelNoHierarchicalCoverage is off by default, which forces bInsideInner
// false, so the only XY exit is the outer unload ring and every bucket well
// inside it is skippable outright. What is left to walk is the buckets
// straddling each level's unload circumference -- O(perimeter), not O(area).
//
// THE BUCKET KEY IS DELIBERATELY THE ONE THAT ALREADY EXISTS.
// VoxelWorldSubsystem.cpp's TileCensusSinceLog defines "tile key = Level,
// ChunkX>>2, ChunkY>>2 -- Z is deliberately NOT split, since a tile may span
// multiple chunk-Z and the design accepts that union", and says it is expected
// to be removed or gated once tile batching ships. Same spelling here rather
// than a second one, so the two cannot drift apart and the census stays
// readable against this index.
//
// bDeepAnchorRelative IS THE ONE THING THE BUCKET CANNOT BOUND, so it is held
// out. Those records add a VERTICAL test against the anchor's Z, and the
// bucket key carries no Z at all (see above), so no XY bound can rule one out.
// They live in their own flat set and are walked in full on every call --
// i.e. exactly today's cost for exactly today's records, no better and no
// worse. This is cheap because the flag is IMMUTABLE AFTER ADMISSION
// (FChunkRecord::bDeepAnchorRelative is written only at the two admission
// sites and never again), so which container a record belongs to is decided
// once, at Insert, and can never need revisiting. If a leg ever shows
// deepTracked as a large fraction of tracked -- the player parked deep
// underground -- the narrowing degrades gracefully toward "no change", it does
// not become wrong.
//
// WHICH WAY IT ERRS (the VoxelFootprintBand.h question, and the only one that
// matters here). The bucket bound must never say "skippable" for a bucket that
// holds a record which would evict; the reverse -- saying "walk me" for a
// bucket whose records all keep -- costs nothing but a few wasted distance
// tests, which is precisely what the un-bucketed scan pays for every record
// anyway. So the bound is built to be conservative twice over:
//
//   1. The centre extremes are computed with the IDENTICAL expression the
//      per-record test uses, `(double(Index) + 0.5) * ChunkEdge`, off the
//      SAME hoisted per-level tables the caller passes in by pointer. Chunk
//      centre is monotonic in the chunk index, and IEEE-754 square and add are
//      monotonic on non-negative doubles, so max(|dx|)^2 + max(|dy|)^2 is >=
//      every real member's DistSq and min likewise <=.
//   2. AND the index range is then padded a WHOLE CHUNK outward on each side.
//      Argument 1 is a real proof, but it assumes the two sides contract
//      floating point identically, and MSVC is free to fuse a multiply-add in
//      one translation unit and not the other. One chunk of slack is ~10^12
//      ulps of margin on a 1-ulp risk, and it also absorbs any off-by-one I
//      could have made in "which chunk indices does bucket B contain". It
//      widens the straddle band from 4 chunks to 6, i.e. it buys the safety
//      for about a third of the available narrowing. Shrinking it is a
//      measured lever for later; do not shrink it on reasoning alone.
//
// NOT THE "INNER-EVICT LUNE" FAILURE MODE, and the difference is worth stating
// because the two look alike and one of them is a real bug that Phase 2 had to
// fix. Phase 2's incremental admission compares the anchor against a PREVIOUS
// SCAN's anchor, and the entry pass is gated per level (a level only re-scans
// when its own coarse chunk changes), so the anchor can wander 1.41 chunk edges
// between two scans of the same level against only 0.25 chunk edges of inner
// hysteresis -- an endpoint-only test there misses records in the lune between
// the two positions, which is why it needs an accumulated ScanAnchorBoxMin/Max.
//
// This bound has no such window. THE EXIT SCAN IS NOT LEVEL-GATED: it runs over
// every record on every RecomputeDesiredSet call, by design (see the
// MaxRingLevel `continue` in the entry loop -- "the exit scan above still runs
// for every level so nothing already resident can be stranded"). So the bucket
// bound is a purely SPATIAL bound, over the chunk indices a bucket contains,
// evaluated against the one anchor this call was handed. There is no earlier
// anchor in it to be stale against and therefore no lune to miss. If the exit
// scan is ever made level-gated or incremental, this bound stops being sound on
// exactly Phase 2's argument, and it would then need Phase 2's anchor box --
// that is the one change to this function that must not be made without
// revisiting this file.
//
// The radii are reused rather than respelled, which is the other half of Phase
// 2's rule: FEvictScanParams takes POINTERS to RecomputeDesiredSet's own
// hoisted ExitChunkEdgeUU / ExitUnloadOuterUUSq / ExitInnerEvictUUSq tables, so
// the bucket bound and the per-record verdict read the same doubles out of the
// same arrays. There is no third spelling of a ring radius here.
//
// AND THE DECIDING ARITHMETIC IS NOT IN THIS FILE AT ALL, which is the same
// argument one layer up. This index only chooses WHICH records get handed to
// the caller's visitor; the per-record `DistSq > OuterSq` / `< InnerSq` test
// that actually evicts is the untouched original code in
// RecomputeDesiredSet. The gate on this work is "LevelEvict{Inner,Outer,
// Vertical} unchanged, exactly", and the surest way to hold it is for the
// lines that produce those counts to be textually the same lines.
//
// STALENESS IS THE FAILURE MODE, so it is tripwired rather than trusted. An
// index that misses one ChunkRecords mutation site does not crash; it silently
// stops offering some records and they never get evicted. Every scan therefore
// asserts Num() == Records.Num() and logs an Error on any drift, and every
// key handed out is re-Find()ed in the real map so an orphan is counted rather
// than dereferenced.

#include "CoreMinimal.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h" // LogVoxelStream -- the census below rides the same 5 s window as the rest of streaming

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelStreaming
{
// Command line, not a cvar, and latched in a function-local static -- the same
// reason VoxelStreamAdmission::HierarchicalCoverageEnabled gives verbatim:
// -ExecCmds cvars land AFTER streaming has begun, so a cvar would flip this
// mid-run with a half-populated index behind it, which is the one state this
// structure has no defined behaviour for. One read, decided before the first
// recompute.
//
// DEFAULT OFF. This changes which records the exit scan looks at, and that has
// to be measured against a control arm, not assumed. With it off the four
// mutation hooks return immediately, the index stays empty, and
// RecomputeDesiredSet takes the original unconditional walk -- zero cost, zero
// behaviour change, which is what makes the control arm a real control.
inline bool EvictionIndexEnabled()
{
	// DEFAULT ON AS OF 2026-08-24; -VoxelNoBucketedExitScan is the control.
	// Measured together with incremental admission (they were never separated),
	// so the pair ships together and the pair is what the numbers describe.
	static const bool bEnabled =
		!FParse::Param(FCommandLine::Get(), TEXT("VoxelNoBucketedExitScan"));
	return bEnabled;
}

// Correctness arm for a leg, not a shipping mode. Walks EVERY bucket including
// the ones the bound called skippable, and for each record in a skipped bucket
// re-derives the XY verdict and logs an Error if it says evict. That is a
// direct test of the only thing that can silently break: a bucket bound that
// is not conservative. It cannot change behaviour -- the records in a skipped
// bucket are still not visited -- so a false alarm costs a log line, never an
// eviction. Slower than the control arm by construction; never leave it on for
// a timing leg.
inline bool EvictionIndexVerifyEnabled()
{
	static const bool bVerify = FParse::Param(FCommandLine::Get(), TEXT("VoxelBucketedExitScanVerify"));
	return bVerify;
}

// Everything the bucket bound needs, all of it already computed once per call
// by RecomputeDesiredSet's hoisted per-level tables. Passed BY POINTER to those
// same arrays on purpose: the bound and the per-record test then read literally
// the same doubles, so the two cannot disagree about where a ring edge is. The
// project's hardest constraint on this function is that admit and evict radii
// are evaluated against the SAME anchor -- the inner-edge tripwire in
// RecomputeDesiredSet exists because the alternative measured 11,779 unloads/s
// with the player standing still -- and this struct is how that is enforced:
// there is no way to hand the index an anchor or a radius the caller is not
// itself using.
struct FEvictScanParams
{
	FVector Anchor = FVector::ZeroVector;
	const double* ChunkEdgeUU = nullptr;     // [VoxelCoords::kNumLevels]
	const double* UnloadOuterUUSq = nullptr; // [VoxelCoords::kNumLevels]
	const double* InnerEvictUUSq = nullptr;  // [VoxelCoords::kNumLevels]
	bool bHierarchicalCoverage = false;
};

class FVoxelEvictionIndex
{
public:
	// --- Mutation. Must be called from EVERY site that adds to or removes from
	// ChunkRecords, and there are exactly four (grep `ChunkRecords.Add` and
	// `ChunkRecords.Remove` -- if that grep ever returns five, one of them is
	// missing a hook here and the tripwire in ForEachEvictCandidate will say so
	// within one recompute). ---

	// Idempotent, and that is not decoration. The two admission sites are both
	// guarded by an earlier ChunkRecords.Find() early-out so a key cannot
	// already be present -- but TMap::Add REPLACES rather than fails, so if
	// that guard is ever weakened the map would quietly hold one record and a
	// naive index would hold two entries for it. Re-inserting an existing key
	// moves it between the bucketed and deep containers if the flag differs
	// (it cannot today: bDeepAnchorRelative is immutable after admission, which
	// is exactly why there is no separate Update method to forget to call).
	void Insert(const VoxelCoords::FVoxelLevelChunkKey& Key, bool bDeepAnchorRelative)
	{
		if (!EvictionIndexEnabled())
		{
			return;
		}
		const bool bWasDeep = DeepRecords.Contains(Key);
		if (bWasDeep || FindBucketFor(Key) != nullptr)
		{
			if (bWasDeep == bDeepAnchorRelative)
			{
				return; // already indexed, same container
			}
			RemoveInternal(Key, bWasDeep);
		}
		if (bDeepAnchorRelative)
		{
			DeepRecords.Add(Key);
		}
		else
		{
			Buckets.FindOrAdd(BucketOf(Key)).Add(Key);
		}
		++TrackedNum;
	}

	void Remove(const VoxelCoords::FVoxelLevelChunkKey& Key)
	{
		if (!EvictionIndexEnabled())
		{
			return;
		}
		RemoveInternal(Key, DeepRecords.Contains(Key));
	}

	int32 Num() const { return TrackedNum; }

	// --- The scan. Calls Visit(Key, Record&) for every record whose evict
	// verdict could be anything other than "keep". ---
	//
	// VISIT ORDER DIFFERS FROM THE UNBUCKETED WALK, and this is the ONE
	// observable difference the switch makes. It is called out rather than
	// buried because it is the only place a reader should look if an A/B pair
	// disagrees about anything other than timing.
	//
	// What is unaffected: WHICH records evict and HOW MANY. The caller's body
	// only appends to PendingUnloadKeys / PendingUnloadSet / EvictedThisCall
	// and stamps fields on the record it was handed -- no cross-record state,
	// no early termination -- so the evicted SET is identical and so are
	// LevelEvict{Inner,Outer,Vertical}, which is what this work is gated on.
	// The two pending JOB queues are re-sorted by SortPendingQueues at the end
	// of the same function, so their order is unaffected too.
	//
	// What IS affected: the order of PendingUnloadKeys. That array is a LIFO
	// stack drained under kMaxUnloadPopsPerFrame, and it is never sorted, so
	// its order comes from the exit scan's walk order -- previously TMap hash
	// order (spatially scattered), now bucket order (spatially clustered).
	// Under a saturated unload budget that changes which of the already-queued
	// chunks park FIRST, not which ones park. Every queued key still drains,
	// and all of them are beyond the unload ring by definition, so this is
	// ordering within off-screen cleanup. Worth knowing about; not worth
	// imposing a sort on 271k records to avoid.
	//
	// (The previous order was arbitrary too -- TMap iteration follows hash
	// bucket layout, which changes with load factor -- so nothing could
	// legitimately have depended on it.)
	template <typename MapT, typename VisitT>
	void ForEachEvictCandidate(const FEvictScanParams& Params, MapT& Records, VisitT&& Visit)
	{
		checkSlow(Params.ChunkEdgeUU && Params.UnloadOuterUUSq && Params.InnerEvictUUSq);
		const bool bVerify = EvictionIndexVerifyEnabled();

		// THE STALENESS TRIPWIRE. A missed mutation hook is silent by nature --
		// the world just stops evicting some chunks -- so it is checked rather
		// than trusted, once per call, O(1). Count equality does not prove key
		// equality, but no realistic miss (a new Add or Remove site added
		// without a hook) can keep the counts equal even for one call.
		if (TrackedNum != Records.Num())
		{
			if (!bWarnedDrift)
			{
				bWarnedDrift = true;
				UE_LOG(LogVoxelStream, Error,
				       TEXT("Eviction index is STALE: index holds %d record(s), ChunkRecords holds %d. ")
				       TEXT("A ChunkRecords.Add/Remove site is missing its EvictionIndex hook -- chunks in the ")
				       TEXT("un-indexed set will never be evicted. Relaunch without -VoxelBucketedExitScan and ")
				       TEXT("fix the hook; see VoxelEvictionIndex.h."),
				       TrackedNum, Records.Num());
			}
		}

		int64 Examined = 0;
		int64 BucketsVisited = 0;
		int64 BucketsSkipped = 0;
		int64 RecordsSkipped = 0;
		int32 MaxBucket = 0;

		for (auto It = Buckets.CreateIterator(); It; ++It)
		{
			TSet<VoxelCoords::FVoxelLevelChunkKey>& Bucket = It.Value();
			// A bucket empties when the anchor walks away from that ground.
			// Dropped rather than kept, because the scan below is O(buckets)
			// even when it skips them all, and a long flight would otherwise
			// leave a permanent trail of empty cells to iterate.
			if (Bucket.Num() == 0)
			{
				It.RemoveCurrent();
				continue;
			}
			MaxBucket = FMath::Max(MaxBucket, Bucket.Num());
			const bool bCanSkip = BucketCanSkip(It.Key(), Params);
			if (bCanSkip)
			{
				++BucketsSkipped;
				RecordsSkipped += Bucket.Num();
				if (!bVerify)
				{
					continue;
				}
				// Verify arm only: prove the skip was safe. Deliberately does
				// NOT visit -- a false positive here must produce a log line,
				// not an eviction, or the arm would be changing the answer it
				// exists to check.
				for (const VoxelCoords::FVoxelLevelChunkKey& Key : Bucket)
				{
					if (WouldEvictXY(Key, Params) && !bWarnedUnsafeSkip)
					{
						bWarnedUnsafeSkip = true;
						UE_LOG(LogVoxelStream, Error,
						       TEXT("Eviction index SKIP WAS UNSAFE: L%d chunk (%d,%d,%d) in bucket (%d,%d,%d) ")
						       TEXT("evicts on XY but its bucket was called skippable. The bucket bound is not ")
						       TEXT("conservative -- see 'WHICH WAY IT ERRS' in VoxelEvictionIndex.h."),
						       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z, It.Key().X, It.Key().Y, It.Key().Z);
					}
				}
				continue;
			}
			++BucketsVisited;
			for (const VoxelCoords::FVoxelLevelChunkKey& Key : Bucket)
			{
				if (auto* Record = Records.Find(Key))
				{
					++Examined;
					Visit(Key, *Record);
				}
				else
				{
					++OrphanKeys;
				}
			}
		}

		// The deep set, in full, every call -- see the header note. No bound
		// exists for it, so there is nothing to skip and nothing to prove.
		for (const VoxelCoords::FVoxelLevelChunkKey& Key : DeepRecords)
		{
			if (auto* Record = Records.Find(Key))
			{
				++Examined;
				Visit(Key, *Record);
			}
			else
			{
				++OrphanKeys;
			}
		}

		// "DID WORK ACTUALLY MOVE?" IS A SEPARATE QUESTION FROM "IS IT FASTER",
		// and this file exists in a codebase where a fork in RecomputeDesiredSet
		// once carried zero traffic for weeks. examined= against tracked= is the
		// answer: it is the count of records the scan actually touched, so a
		// structure that is silently visiting everything reads examined ==
		// tracked no matter what the timer says, and a structure that is
		// silently visiting nothing reads examined == 0 while looking fast.
		AccumExamined += Examined;
		AccumTracked += TrackedNum;
		AccumBucketsVisited += BucketsVisited;
		AccumBucketsSkipped += BucketsSkipped;
		AccumRecordsSkipped += RecordsSkipped;
		AccumCalls += 1;
		MaxBucketSize = FMath::Max(MaxBucketSize, MaxBucket);
	}

	// Emits the census line at most once per 5 s. CALLED BY THE CALLER, FROM
	// OUTSIDE ITS exitScanMs TIMER, and that placement is the whole reason it
	// is not folded into ForEachEvictCandidate: a UE_LOG is tens of
	// microseconds and MaxExitScanMs is a MAX over the window, so one logging
	// call per window inside the timer would put a visible spike into the very
	// number this work is gated on. Cheap and inlined away on the calls that do
	// not log.
	void MaybeLogCensus();

private:
	// The bucket key, spelled exactly as TileCensusSinceLog spells its tile key
	// (Level, ChunkX>>2, ChunkY>>2). Arithmetic >> floors for negative chunk
	// coordinates, which is what makes the lattice world-aligned rather than
	// mirrored about the origin -- the same reliance the ComputeRetainReplacementZMask
	// parent lookup calls out with "// >> floors for negatives too".
	static FIntVector BucketOf(const VoxelCoords::FVoxelLevelChunkKey& Key)
	{
		return FIntVector(Key.Level, Key.Key.X >> 2, Key.Key.Y >> 2);
	}

	TSet<VoxelCoords::FVoxelLevelChunkKey>* FindBucketFor(const VoxelCoords::FVoxelLevelChunkKey& Key)
	{
		TSet<VoxelCoords::FVoxelLevelChunkKey>* Bucket = Buckets.Find(BucketOf(Key));
		return (Bucket && Bucket->Contains(Key)) ? Bucket : nullptr;
	}

	void RemoveInternal(const VoxelCoords::FVoxelLevelChunkKey& Key, bool bDeep)
	{
		if (bDeep)
		{
			if (DeepRecords.Remove(Key) > 0)
			{
				--TrackedNum;
			}
			return;
		}
		if (TSet<VoxelCoords::FVoxelLevelChunkKey>* Bucket = Buckets.Find(BucketOf(Key)))
		{
			if (Bucket->Remove(Key) > 0)
			{
				--TrackedNum;
			}
			// The bucket is left in place if it just emptied; the scan drops
			// empties as it walks them, which keeps removal O(1) and does not
			// pay a TMap::Remove on the DrainUnloads path, where thousands of
			// records can leave in one call.
		}
	}

	// Can NO record in this bucket answer "evict" on the XY tests? See "WHICH
	// WAY IT ERRS" at the top of the file for why the pad is there and why the
	// centre expression is a character-for-character copy of the per-record one.
	static bool BucketCanSkip(const FIntVector& Bucket, const FEvictScanParams& Params)
	{
		const int32 Level = Bucket.X;
		const double ChunkEdge = Params.ChunkEdgeUU[Level];

		// Bucket B covers chunk indices [B*4, B*4+3] on each of X and Y.
		// Padded one chunk outward on each side; `* 4` rather than `<< 2`
		// because a left shift of a negative bucket coordinate only became
		// well-defined in C++20 and this must not depend on that.
		const int32 XLo = Bucket.Y * 4 - 1, XHi = Bucket.Y * 4 + 4;
		const int32 YLo = Bucket.Z * 4 - 1, YHi = Bucket.Z * 4 + 4;

		const double DxLo = (double(XLo) + 0.5) * ChunkEdge - Params.Anchor.X;
		const double DxHi = (double(XHi) + 0.5) * ChunkEdge - Params.Anchor.X;
		const double DyLo = (double(YLo) + 0.5) * ChunkEdge - Params.Anchor.Y;
		const double DyHi = (double(YHi) + 0.5) * ChunkEdge - Params.Anchor.Y;

		// Chunk centre is monotonic in the chunk index, so the two endpoints
		// bracket every centre between them: the farthest the box can reach on
		// an axis is the larger endpoint magnitude, and the nearest is zero if
		// the anchor lies between the endpoints, else the smaller magnitude.
		const double MaxAbsDx = FMath::Max(FMath::Abs(DxLo), FMath::Abs(DxHi));
		const double MaxAbsDy = FMath::Max(FMath::Abs(DyLo), FMath::Abs(DyHi));
		const double MinAbsDx = (DxLo <= 0.0 && DxHi >= 0.0) ? 0.0 : FMath::Min(FMath::Abs(DxLo), FMath::Abs(DxHi));
		const double MinAbsDy = (DyLo <= 0.0 && DyHi >= 0.0) ? 0.0 : FMath::Min(FMath::Abs(DyLo), FMath::Abs(DyHi));

		const double MaxDistSq = FMath::Square(MaxAbsDx) + FMath::Square(MaxAbsDy);

		// Outer unload ring: could anything in here be BEYOND it?
		if (MaxDistSq > Params.UnloadOuterUUSq[Level])
		{
			return false;
		}
		// Inner evict radius: could anything in here be INSIDE it? Level 0 has
		// no inner hole to evict into, and hierarchical coverage forces the
		// whole inner test false at the per-record site (a coarse chunk under
		// the finer rings is DESIRED there, not stale), so both arms are
		// mirrored here rather than re-derived -- one spelling of the rule.
		if (Level > 0 && !Params.bHierarchicalCoverage)
		{
			const double MinDistSq = FMath::Square(MinAbsDx) + FMath::Square(MinAbsDy);
			if (MinDistSq < Params.InnerEvictUUSq[Level])
			{
				return false;
			}
		}
		return true;
	}

	// Verify-arm only. A transcription of the per-record XY verdict in
	// RecomputeDesiredSet, used to prove a skip was safe. Deliberately omits
	// the vertical test: only non-deep records are ever in a bucket, and
	// bBeyondVertical is unreachable for them.
	static bool WouldEvictXY(const VoxelCoords::FVoxelLevelChunkKey& Key, const FEvictScanParams& Params)
	{
		const double ChunkEdge = Params.ChunkEdgeUU[Key.Level];
		const double CenterX = (double(Key.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(Key.Key.Y) + 0.5) * ChunkEdge;
		const double DistSq =
			FMath::Square(CenterX - Params.Anchor.X) + FMath::Square(CenterY - Params.Anchor.Y);
		const bool bBeyondOuter = DistSq > Params.UnloadOuterUUSq[Key.Level];
		const bool bInsideInner =
			Key.Level > 0 && DistSq < Params.InnerEvictUUSq[Key.Level] && !Params.bHierarchicalCoverage;
		return bBeyondOuter || bInsideInner;
	}

	// Not thread-safe, and does not need to be: all four mutation sites and the
	// scan are game-thread streaming code (AddCandidate, DropFarthestOverCap,
	// DrainUnloads, RecomputeDesiredSet). ChunkRecords itself carries the same
	// assumption.
	//
	// The census is emitted from this file rather than from MaybeLogCounters,
	// and that is a merge-surface decision as much as a tidiness one: this
	// structure landed alongside two other in-flight changes to
	// VoxelWorldSubsystem.cpp, and every line that can live here instead of
	// there is a conflict that does not happen. Same 5 s window as the
	// streaming counters it sits beside in the log, so a reader can line them
	// up.
	TMap<FIntVector, TSet<VoxelCoords::FVoxelLevelChunkKey>> Buckets;
	// Records admitted ONLY by the anchor-relative deep box. No XY bound can
	// clear their vertical test, so they are never bucketed. See the file
	// header.
	TSet<VoxelCoords::FVoxelLevelChunkKey> DeepRecords;
	int32 TrackedNum = 0;

	int64 AccumExamined = 0;
	int64 AccumTracked = 0;
	int64 AccumBucketsVisited = 0;
	int64 AccumBucketsSkipped = 0;
	int64 AccumRecordsSkipped = 0;
	int64 AccumCalls = 0;
	int64 OrphanKeys = 0;
	int32 MaxBucketSize = 0;
	double LastCensusLogSeconds = 0.0;
	bool bWarnedDrift = false;
	bool bWarnedUnsafeSkip = false;
};

} // namespace VoxelStreaming

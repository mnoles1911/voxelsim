#include "VoxelEvictionIndex.h"

#include "HAL/PlatformTime.h"

namespace VoxelStreaming
{
void FVoxelEvictionIndex::MaybeLogCensus()
{
	if (!EvictionIndexEnabled())
	{
		// The caller invokes this unconditionally so the switch lives in one
		// place; with the index off there is nothing to report and the control
		// arm must not even pay a clock read.
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (LastCensusLogSeconds == 0.0)
	{
		// First scan of the run: start the window here rather than at zero,
		// or the first line would report a single call and be mistaken for a
		// steady-state reading.
		LastCensusLogSeconds = Now;
		return;
	}
	if (Now - LastCensusLogSeconds < 5.0 || AccumCalls == 0)
	{
		return;
	}

	// THE "DID WORK ACTUALLY MOVE" LINE. Read `examined` against `tracked`:
	//
	//   examined ~= tracked        the structure is narrowing NOTHING and the
	//                              exit scan is doing exactly what it did
	//                              before, whatever exitScanMs says. This is
	//                              the failure this counter exists to catch --
	//                              a fork in RecomputeDesiredSet once carried
	//                              zero traffic for weeks and only a counter
	//                              like this would have said so.
	//   examined == 0              the structure is narrowing EVERYTHING, i.e.
	//                              it is empty or the bound is inverted. Fast
	//                              and wrong; LevelEvict* will have collapsed
	//                              to zero alongside it.
	//   examined ~= perimeter      what the design predicts. At the shipping
	//                              rings (R0 128 m through R5 4 km, unload
	//                              x1.25) the straddle band is O(circumference
	//                              / 4 chunks), padded to 6, against an
	//                              O(area) record population.
	//
	// deepTracked is the part that cannot narrow at all (no XY bound clears a
	// vertical test) and is therefore the floor on `examined`. maxBucket is the
	// pathology check: buckets are 4x4 chunks in XY unioned over ALL chunk-Z,
	// so an unexpectedly deep column shows up here before it shows up as a
	// timing. orphans must stay 0 -- a key this index offered that ChunkRecords
	// does not hold is a mutation hook that ran in the wrong order.
	const double WindowSeconds = Now - LastCensusLogSeconds;
	const int64 BucketsTotal = AccumBucketsVisited + AccumBucketsSkipped;
	const double ExaminedPct =
		(AccumTracked > 0) ? (100.0 * double(AccumExamined) / double(AccumTracked)) : 0.0;
	UE_LOG(LogVoxelStream, Log,
	       TEXT("Voxel exit-scan index (%.1f s window): calls=%lld tracked=%lld examined=%lld (%.1f%% of tracked) ")
	       TEXT("skippedRecords=%lld | buckets: visited=%lld skipped=%lld total=%lld maxBucket=%d | ")
	       TEXT("deepTracked=%d orphans=%lld"),
	       WindowSeconds, (long long)AccumCalls, (long long)AccumTracked, (long long)AccumExamined, ExaminedPct,
	       (long long)AccumRecordsSkipped, (long long)AccumBucketsVisited, (long long)AccumBucketsSkipped,
	       (long long)BucketsTotal, MaxBucketSize, DeepRecords.Num(), (long long)OrphanKeys);

	LastCensusLogSeconds = Now;
	AccumExamined = 0;
	AccumTracked = 0;
	AccumBucketsVisited = 0;
	AccumBucketsSkipped = 0;
	AccumRecordsSkipped = 0;
	AccumCalls = 0;
	OrphanKeys = 0;
	MaxBucketSize = 0;
}
} // namespace VoxelStreaming

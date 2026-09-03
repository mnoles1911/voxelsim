// VoxelRecomputeProfile.h -- what RecomputeDesiredSet actually spends, one
// level below the stage timers that already exist.
//
// ===========================================================================
// WHAT IS ALREADY ANSWERED, SO NOTHING HERE RE-ASKS IT
// ===========================================================================
// `Voxel recompute (sum since last log)` (VoxelWorldSubsystem.cpp:10821)
// already splits the window into fine / exitScan / queueFilter / sort / entry
// per level. It had never been aggregated. Over every active window of three
// legs (docs/recompute-cost-census-2026-08-23.md):
//
//     leg        windows  ms/win   entry    exit    sort   fine   qFilter  RESIDUAL
//     ahead-on        73   709.2   61.1%   37.3%    1.3%   0.2%     0.1%     0.06%
//     gp-ctl2         84   811.7   53.1%   45.2%    1.4%   0.2%     0.1%     0.06%
//     pool-pri        82   965.7   58.7%   39.1%    1.9%   0.2%     0.1%     0.05%
//
// So: entry scan plus exit walk are 90-99% of recompute, everything else is
// under 2%, and THE EXISTING BUCKETS ALREADY ACCOUNT FOR 99.94%. There is no
// hidden stage. The residual bucket this file's log line prints exists to keep
// that true, not because anything is currently missing.
//
// And the rise through a run is the EXIT half, not the entry half (ahead-on,
// by quartile): total 465 -> 883 ms/window, entry 355 -> 513 (1.45x), exit
// 101 -> 355 (3.5x). Exit is O(tracked records) and records climb all fill;
// entry is O(Span^2) per level and only grows as more levels come active.
//
// ===========================================================================
// WHAT IS NOT ANSWERED, AND IS WHAT THIS FILE IS FOR
// ===========================================================================
// Inside `entryMs[L]` there is no split at all. The cell body does geometry,
// then a Z-range memo lookup, then a Z loop with two hash probes per Z cell,
// then admission. Nobody knows the shares, and the four candidate fixes
// (bounded enumeration, memo shape, probe elimination, Z-range narrowing) each
// only pay off against a different one of them.
//
// ===========================================================================
// WHY THIS IS COUNTERS AND NOT TIMERS
// ===========================================================================
// The Z loop runs ~2.4 MILLION times per 5 s window. FPlatformTime::Seconds is
// ~20-25 ns; a pair around each Z cell would add ~110 ms per window to a
// 709 ms measurement -- a 15% perturbation of the thing being measured, which
// is the instrument-becomes-the-measurement failure this codebase already
// records at the apply-stage timers.
//
// So the loop-internal work is COUNTED, exactly and for free (one increment on
// an int64 already in cache), and the times come from the per-level stage
// timer that already exists. The per-operation costs are then recovered by
// LEAST SQUARES over the levels and windows: six levels have wildly different
// mixes (R0 is Z-cell heavy, R5 is footprint heavy) and a leg gives ~500
// (entryMs, counts) observations. tools/fit-recompute-costs.py does the fit
// and prints the resulting time split.
//
// THE FIT'S OWN RESIDUAL IS THE RECONCILIATION DELTA, and it is printed. A
// model that cannot explain entryMs from the counters is visible as a large
// residual instead of quietly producing plausible-looking shares -- which is
// the whole reason to prefer this over a hand-assigned constant per operation.
//
// The ONE timer inside the sweep is MemoFillMs, and it is safe on its own
// terms: a fill is a memoized amplifier column, so it is rare (hits outnumber
// fills by orders of magnitude once a level is warm) and expensive (hundreds
// of ns at least). Timing something rare and expensive is the case where a
// clock read is negligible, and the hit/fill counters beside it say directly
// how rare -- if memoFill ever approaches memoHit, THAT is the failing reading
// and the timer must come out.
//
// ===========================================================================
// FAILING READINGS, STATED BOTH WAYS AT EVERY SITE
// ===========================================================================
// Each stage carries a CALL COUNT beside its milliseconds, for one reason:
// `exitScan=0.00ms n=0` (the stage never ran) and `exitScan=0.00ms n=250` (it
// ran 250 times and is genuinely free) are different facts, and without n they
// print identically. Three lanes in this codebase have already been found
// inert while reading as healthy that way.
//
//   residual > 5% of total          a stage is missing a bucket, or a bucket
//                                   is being accumulated outside the timer.
//   residual NEGATIVE               timers OVERLAP -- a nested stage is being
//                                   added as a top-level one, so shares are
//                                   double-counted. Distinct from the above
//                                   and only visible because the residual is
//                                   printed SIGNED.
//   any stage n=0 across a window   that stage never ran; nothing about it may
//   in which recompute ran          be concluded from this leg.
//   cells=0 with entryMs>0          the counters are outside the loop they
//                                   claim to describe -- the twelve-instruments
//                                   -outside-the-path failure, exactly.
//   zCells/cells collapsing to ~1   the Z range has stopped being a range;
//                                   either the memo is broken or the skirt is
//                                   off, and the per-cell arithmetic below is
//                                   about a different world than the leg.
//   memoFill approaching memoHit    the memo is not memoizing; the fill timer
//                                   is now on a hot path and is itself a cost.
//   fit residual > 10% of entryMs   the cost model does not explain the time.
//                                   Do NOT read the shares in that case -- the
//                                   fit is reporting that it failed, which is
//                                   the only outcome a bad model is allowed to
//                                   produce.
//
// UE-FREE, like VoxelRingOrder.h and VoxelEditedLaneGate.h, so the
// reconciliation arithmetic is exercised by tools/test-voxel-ring-order.cpp
// with a plain compiler. The clock is read at the CALL SITE and handed in as
// elapsed seconds -- that keeps every decision in this header testable while
// leaving FPlatformTime::Seconds where it belongs.

#pragma once

#include <cstdint>

namespace VoxelRecomputeProfile
{
	// Top-level stages. These MUST partition the recompute timer: every
	// microsecond inside RecomputeDesiredSet belongs to exactly one, or to the
	// residual. Adding a stage here without bracketing it at the call site
	// makes the residual grow, which is the point -- the residual is the
	// contract, not a rounding line.
	enum class EStage : uint8_t
	{
		Prologue = 0,    // resets, anchor derivation, cutoff relaxation
		FineResidency,   // the fine-tier tick (documented as able to stall for seconds)
		ExitScan,        // the O(ChunkRecords) eviction walk
		QueueFilter,     // the evicted-key filter over the pending queues
		EntryScan,       // all per-level annulus sweeps
		Sort,            // SortPendingQueues + TruncatePendingJobQueue
		LiveConsume,     // mode-2 delta adjudication
		Epilogue,        // stamping, deferral clears, counter flushes
		Count
	};

	constexpr int32_t kNumStages = int32_t(EStage::Count);
	// Must be >= VoxelCoords::kNumLevels (8 as of the 2026-09-02 cascade cut).
	// Not asserted here: this header stays plain-compiler testable and cannot
	// include VoxelCoords.h; whoever wires a consumer should assert it there.
	constexpr int32_t kMaxLevels = 8;

	inline const char* StageName(EStage S)
	{
		switch (S)
		{
			case EStage::Prologue:      return "prologue";
			case EStage::FineResidency: return "fine";
			case EStage::ExitScan:      return "exitScan";
			case EStage::QueueFilter:   return "queueFilter";
			case EStage::EntryScan:     return "entryScan";
			case EStage::Sort:          return "sort";
			case EStage::LiveConsume:   return "liveConsume";
			case EStage::Epilogue:      return "epilogue";
			default:                    return "?";
		}
	}

	// Milliseconds AND how many times the stage was entered. Never one without
	// the other -- see the failing readings above.
	struct FStageAccum
	{
		double Ms = 0.0;
		int64_t N = 0;
	};

	// Per-level entry-scan traffic. Counters only; the single timer is called
	// out in the header note.
	struct FEntryCounters
	{
		int64_t CellsVisited = 0;   // cell-loop bodies entered
		int64_t IncrSkipped = 0;    // skipped by the incremental crossing test
		int64_t GeoRejected = 0;    // inner-pad / outer / seam, before any memo work
		int64_t MemoHit = 0;        // FootprintChunkZRangeCached served from the memo
		int64_t MemoFill = 0;       // ... computed (an amplifier column)
		double MemoFillMs = 0.0;    // the one in-loop timer; rare + expensive by construction
		int64_t ZCells = 0;         // Z-loop iterations
		int64_t RecordProbes = 0;   // ChunkRecords.Find
		int64_t ParkProbes = 0;     // ParkedGeometry.Find
		int64_t Admits = 0;
		int64_t RejBudget = 0;
		int64_t RejCutoff = 0;
		int64_t RejFine = 0;
		int64_t DeferralOps = 0;    // DeferredFootprints add/remove

		void Add(const FEntryCounters& O)
		{
			CellsVisited += O.CellsVisited; IncrSkipped += O.IncrSkipped;
			GeoRejected += O.GeoRejected;   MemoHit += O.MemoHit;
			MemoFill += O.MemoFill;         MemoFillMs += O.MemoFillMs;
			ZCells += O.ZCells;             RecordProbes += O.RecordProbes;
			ParkProbes += O.ParkProbes;     Admits += O.Admits;
			RejBudget += O.RejBudget;       RejCutoff += O.RejCutoff;
			RejFine += O.RejFine;           DeferralOps += O.DeferralOps;
		}
	};

	struct FExitCounters
	{
		int64_t RecordsWalked = 0;
		int64_t VerticalTests = 0;
		int64_t EvictsQueued = 0;

		void Add(const FExitCounters& O)
		{
			RecordsWalked += O.RecordsWalked;
			VerticalTests += O.VerticalTests;
			EvictsQueued += O.EvictsQueued;
		}
	};

	// One log window. Game thread only.
	class FWindow
	{
	public:
		// Elapsed seconds are computed by the caller and handed in, so the
		// clock stays at the call site and every decision here stays testable.
		void AddStage(EStage S, double Seconds)
		{
			FStageAccum& A = Stages[int32_t(S)];
			A.Ms += Seconds * 1000.0;
			++A.N;
		}

		// The recompute timer that already exists -- the number `recompute=`
		// prints. Everything else is held against THIS.
		void AddRecomputeTotal(double Seconds)
		{
			TotalMs += Seconds * 1000.0;
			++TotalN;
		}

		void AddEntry(int32_t Level, const FEntryCounters& C)
		{
			if (Level >= 0 && Level < kMaxLevels) { Entry[Level].Add(C); }
		}
		void AddExit(const FExitCounters& C) { Exit.Add(C); }

		double GetTotalMs() const { return TotalMs; }
		int64_t GetTotalN() const { return TotalN; }
		const FStageAccum& GetStage(EStage S) const { return Stages[int32_t(S)]; }
		const FEntryCounters& GetEntry(int32_t Level) const { return Entry[Level]; }
		const FExitCounters& GetExit() const { return Exit; }

		double NamedMs() const
		{
			double Sum = 0.0;
			for (int32_t I = 0; I < kNumStages; ++I) { Sum += Stages[I].Ms; }
			return Sum;
		}

		// SIGNED, deliberately. Positive means a stage is unbucketed; NEGATIVE
		// means two timers overlap and shares are double-counted. Collapsing
		// these to an absolute value would hide the second, which is the
		// harder bug and the one that makes every share wrong at once.
		double ResidualMs() const { return TotalMs - NamedMs(); }

		double ResidualPct() const
		{
			return TotalMs > 0.0 ? 100.0 * ResidualMs() / TotalMs : 0.0;
		}

		// A stage that never ran, in a window where recompute did. Not an
		// error by itself (an underground window runs no entry scan), but it
		// means no conclusion about that stage may be drawn from this window,
		// and the log prints it so the reader is not left inferring it from a
		// 0.00 that could equally have meant "free".
		bool StageNeverRan(EStage S) const
		{
			return TotalN > 0 && Stages[int32_t(S)].N == 0;
		}

		void Reset() { *this = FWindow(); }

	private:
		FStageAccum Stages[kNumStages] = {};
		FEntryCounters Entry[kMaxLevels] = {};
		FExitCounters Exit;
		double TotalMs = 0.0;
		int64_t TotalN = 0;
	};
} // namespace VoxelRecomputeProfile

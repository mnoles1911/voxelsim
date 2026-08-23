// VoxelRingOrder.h -- distance-ordered annulus enumeration for the entry scan.
//
// ===========================================================================
// THE WASTE THIS REMOVES, MEASURED
// ===========================================================================
// RecomputeDesiredSet's entry scan walks its square row-major
// (VoxelWorldSubsystem.cpp:16347, Cy outer, Cx inner, -ScanSpan..+ScanSpan) and
// gate (a) stops ADMITTING once AdmissionsThisLevel reaches Cap/4. It does not
// stop ENUMERATING. Every remaining cell still pays the footprint Z-range memo,
// the Z loop, and two hash probes per Z cell, and is then thrown away.
//
// Aggregated from the per-reason admission split over three matched cold-start
// legs (docs/gpu-residency-admit-budget-2026-08-23.md):
//
//     leg        rejBudget            rejCutoff   rejFine  rejNearest
//     gp-ctl2    22,269,674  99.13%     195,889         0           0
//     gp-ctl     36,940,946  99.47%     198,005         0           0
//     pool-pri   36,314,438  99.99%       2,245         0           0
//
// 22-37 MILLION enumerate-then-discard cells per leg, 99%+ of them on the
// budget gate. Level 0 alone is 71-83% of it.
//
// ===========================================================================
// WHY AN EARLY BREAK IS THE WRONG FIX, AND WHAT IS
// ===========================================================================
// Row-major order means the first Cap/4 cells are the scan box CORNER -- the
// "southmost rows, west to east" strip the -VoxelNearestAdmit comment at
// VoxelWorldSubsystem.cpp:3866 documents, which starts at the FAR edge of the
// ring. Breaking out early would admit exactly the wrong chunks and hand the
// nearest-first sort a queue that only contains far ones.
//
// So change the ORDER, not add a break. This table yields the square offsets
// in nondecreasing LATTICE radius (DX*DX+DY*DY), which lets the sweep:
//
//   1. START past the inner-pad disc instead of iterating and skipping it
//      (for L>0 that disc is most of the square),
//   2. STOP before the corners outside AdmitOuter (the square wastes
//      (4-pi)/4 = 21% on corners even with no budget in play),
//   3. STOP when gate (a) latches, having visited the cells it actually
//      wanted.
//
// Visited cells go from O(Span^2) to O(Budget + sqrt(Budget)). That is the
// question the 50k budget doc says to ask of every fix -- it REMOVES THE TERM
// rather than reducing the constant, and Span^2 is precisely the term that
// grows when the rings widen to the requested 8-10 km.
//
// ===========================================================================
// THE PRECISION BOUND -- the load-bearing claim, and it is checked
// ===========================================================================
// The table orders by LATTICE radius r = sqrt(DX^2+DY^2), an integer-derived
// quantity fixed for a span. The sweep real metric is the distance from the
// ANCHOR POINT to the cell CENTRE. The anchor sits somewhere inside its own
// chunk, so the two differ by at most the half-diagonal of a chunk:
//
//     h = sqrt(0.5) = 0.7071 chunk edges
//     trueDist/edge  in  [ r - h , r + h ]
//
// Everything below is derived from that one inequality and nothing else:
//
//   * WindowFor() widens the requested radii by h in both directions, so the
//     window can only ever be too WIDE. A cell the body would have accepted
//     can never be outside it.
//   * StopRadiusSqAfterLatch() returns (sqrt(latchRSq) + 2h)^2, because a cell
//     whose true distance is within the farthest ADMITTED cell true distance
//     can sit up to 2h lattice units further out. Continuing to that radius is
//     what makes the visited prefix a superset of the true nearest set.
//
// tools/test-voxel-ring-order.cpp checks both numerically against brute force
// over a grid of anchor fractional offsets, and it is the SAME HEADER the
// engine compiles -- no reimplementation, per the project standing rule that
// the instrument must run the engine binding.
//
// ===========================================================================
// COST
// ===========================================================================
// One table per distinct span, built once and kept for the session: the order
// depends only on the span, never on the anchor. (2S+1)^2 offsets at 4 bytes;
// at today level-0 span of ~42 that is 7,225 cells = 29 KB, and a handful of
// distinct spans exist across the cascade. Construction is one sort of that
// array, microseconds, amortised to nothing over a session.
//
// UE-FREE ON PURPOSE. No UE type appears here, so tools/test-voxel-ring-order
// compiles this exact file with a plain C++ compiler. A test that re-derives
// the ordering instead of calling it would be testing itself.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VoxelRingOrder
{
	// Half-diagonal of a chunk, in chunk edges. THE constant this file is
	// built on; see the precision block above.
	constexpr double kHalfDiag = 0.70710678118654752440;

	// Past this the table would cost more than the walk it saves ((2*512+1)^2
	// offsets = 4.2 MB). Callers fall back to the row-major walk and COUNT it
	// -- silently degrading to the old behaviour is how a feature becomes
	// inert while every indicator reads healthy.
	constexpr int32_t kMaxCachedSpan = 512;

	struct FOffset
	{
		int16_t DX = 0;
		int16_t DY = 0;
	};

	// One span ordering. Immutable after construction.
	class FTable
	{
	public:
		explicit FTable(int32_t InSpan) : Span(InSpan)
		{
			const int32_t Edge = 2 * InSpan + 1;
			Offsets.reserve(size_t(Edge) * size_t(Edge));
			for (int32_t DY = -InSpan; DY <= InSpan; ++DY)
			{
				for (int32_t DX = -InSpan; DX <= InSpan; ++DX)
				{
					Offsets.push_back(FOffset{int16_t(DX), int16_t(DY)});
				}
			}
			// Sorted by squared lattice radius. The DY/DX tiebreak is only for
			// determinism -- two cells at the same radius are interchangeable
			// for admission, but a run-to-run reordering would make two legs
			// of the same config differ for no reason, which is the one thing
			// an A/B must never do.
			std::stable_sort(Offsets.begin(), Offsets.end(),
			                 [](const FOffset& A, const FOffset& B)
			                 {
				                 const int32_t RA = int32_t(A.DX) * A.DX + int32_t(A.DY) * A.DY;
				                 const int32_t RB = int32_t(B.DX) * B.DX + int32_t(B.DY) * B.DY;
				                 if (RA != RB) { return RA < RB; }
				                 if (A.DY != B.DY) { return A.DY < B.DY; }
				                 return A.DX < B.DX;
			                 });
		}

		int32_t GetSpan() const { return Span; }
		int32_t Num() const { return int32_t(Offsets.size()); }
		const FOffset& At(int32_t I) const { return Offsets[size_t(I)]; }

		int32_t RadiusSqAt(int32_t I) const
		{
			const FOffset& O = Offsets[size_t(I)];
			return int32_t(O.DX) * O.DX + int32_t(O.DY) * O.DY;
		}

		// First index whose squared lattice radius is >= RSq. Binary search
		// over the sorted radii; O(log N), no allocation.
		int32_t LowerBound(int32_t RSq) const
		{
			int32_t Lo = 0;
			int32_t Hi = Num();
			while (Lo < Hi)
			{
				const int32_t Mid = Lo + (Hi - Lo) / 2;
				if (RadiusSqAt(Mid) < RSq) { Lo = Mid + 1; } else { Hi = Mid; }
			}
			return Lo;
		}

		// First index whose squared lattice radius is > RSq.
		int32_t UpperBound(int32_t RSq) const
		{
			int32_t Lo = 0;
			int32_t Hi = Num();
			while (Lo < Hi)
			{
				const int32_t Mid = Lo + (Hi - Lo) / 2;
				if (RadiusSqAt(Mid) <= RSq) { Lo = Mid + 1; } else { Hi = Mid; }
			}
			return Lo;
		}

	private:
		int32_t Span = 0;
		std::vector<FOffset> Offsets;
	};

	// Session cache. Game thread only, matching every caller. Tables are never
	// freed: a handful of spans exist across a cascade and each is tens of KB.
	// Returns nullptr past kMaxCachedSpan -- the caller MUST handle that by
	// falling back to its row-major walk and counting it.
	const FTable* Get(int32_t Span);

	// Half-open index range [Begin, End) covering every cell whose TRUE centre
	// distance can lie in [InnerChunks, OuterChunks] chunk edges, for ANY
	// anchor position inside its own chunk. Widened by kHalfDiag on both sides,
	// so it is conservative in the only direction that is safe: it can include
	// cells the body will reject, never exclude one the body would accept.
	struct FWindow
	{
		int32_t Begin = 0;
		int32_t End = 0;
	};

	inline FWindow WindowFor(const FTable& Table, double InnerChunks, double OuterChunks)
	{
		FWindow W;
		// Inner: a cell at lattice r can be as close as (r - h) edges, so the
		// first cell that might reach inside InnerChunks has r >= Inner - h.
		// Floor before squaring so the bound only ever moves outward.
		const double InnerLat = InnerChunks - kHalfDiag;
		if (InnerLat <= 0.0)
		{
			W.Begin = 0;
		}
		else
		{
			const double Lat = std::floor(InnerLat);
			W.Begin = Table.LowerBound(int32_t(Lat * Lat));
		}
		// Outer: a cell at lattice r is at least (r - h) away, so any cell with
		// r > Outer + h is beyond reach for every anchor position. Ceil before
		// squaring, same direction.
		const double OuterLat = std::ceil(OuterChunks + kHalfDiag);
		const double MaxLat = double(Table.GetSpan()) * 1.4142135623730951;
		if (OuterLat >= MaxLat)
		{
			W.End = Table.Num();
		}
		else
		{
			W.End = Table.UpperBound(int32_t(OuterLat * OuterLat));
		}
		if (W.End < W.Begin) { W.End = W.Begin; }
		return W;
	}

	// After the per-level budget latches at squared lattice radius LatchRSq,
	// the walk must continue to THIS squared radius before stopping, or the
	// visited prefix is not guaranteed to contain the true nearest set.
	//
	// Derivation, and it is the whole justification for the overrun: the
	// farthest cell admitted before the latch has true distance at most
	// (sqrt(LatchRSq) + h) edges. A cell whose true distance is no greater
	// than that can have lattice radius up to (that + h) = sqrt(LatchRSq) + 2h.
	// Stopping at the latch radius itself would drop cells that are genuinely
	// NEARER than ones already admitted -- the exact defect row-major order
	// has, just smaller.
	//
	// The overrun band is ~2*pi*r*2h cells; at a 512 budget r is ~12.8 and the
	// band is ~115 cells, ~22% on top of the budget. Bounded, and it is the
	// price of the guarantee.
	inline int32_t StopRadiusSqAfterLatch(int32_t LatchRSq)
	{
		const double R = std::sqrt(double(LatchRSq)) + 2.0 * kHalfDiag;
		return int32_t(std::ceil(R * R));
	}
} // namespace VoxelRingOrder

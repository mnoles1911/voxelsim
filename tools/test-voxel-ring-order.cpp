// test-voxel-ring-order.cpp -- verify VoxelRingOrder.h and VoxelEditedLaneGate.h
// against brute force, with NO build and NO editor.
//
// WHY THIS EXISTS. Both headers make claims that are geometric, not empirical,
// and a geometric claim can be checked exactly. The claims are:
//
//   R1  the table is a permutation of the scan square -- no cell added, none
//       lost. (A "faster" enumeration that quietly drops cells is the
//       silently-lost-chunk failure this codebase has paid for repeatedly.)
//   R2  squared lattice radius is nondecreasing along the table.
//   R3  WindowFor is CONSERVATIVE: for every anchor position inside its chunk,
//       every cell whose true centre distance lies in the requested band is
//       inside the returned window. Checked against brute force over a grid of
//       fractional anchor offsets.
//   R4  StopRadiusSqAfterLatch is CONSERVATIVE: stopping there never omits a
//       cell whose true distance is <= the true distance of the farthest cell
//       admitted before the latch.
//   R5  the saving is real: the window plus the budget stop visits far fewer
//       cells than the square. Printed, not asserted -- it is a measurement.
//
//   E1  the edited-lane gate never skips a call on which a verdict could have
//       changed. Brute-forced: move the anchor in small steps, recompute every
//       footprint verdict directly, and assert that whenever any verdict
//       differs from the last full sweep, the gate asked for a FullSweep.
//   E2  the gate DOES skip -- a gate that always sweeps is as broken as one
//       that never does, and E1 alone cannot tell them apart.
//
//   P1  the recompute profile reconciles: total minus the named stages is the
//       printed residual, exactly.
//   P2  the residual is SIGNED. Overlapping timers (a nested stage added as a
//       top-level one) must produce a NEGATIVE residual, not a small positive
//       one -- double-counted shares are the harder bug and an absolute value
//       would hide them.
//   P3  "never ran" and "ran and cost nothing" are distinguishable. Both print
//       0.00 ms; only the call count separates them, and three lanes in this
//       codebase have already been found inert because it did not.
//
// This compiles THE ENGINE HEADERS THEMSELVES, not a copy. That is the rule
// that cost this project three green probes measuring a world the engine was
// not running.
//
//   tools/run-voxel-ring-order-test.sh     (or: see the compile line inside)

#include "../ue-project/Source/VoxelEarth/VoxelEditedLaneGate.h"
#include "../ue-project/Source/VoxelEarth/VoxelRecomputeProfile.h"
#include "../ue-project/Source/VoxelEarth/VoxelRingOrder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

static int gFailures = 0;

static void Check(bool Cond, const char* What)
{
	if (!Cond)
	{
		std::printf("FAIL  %s\n", What);
		++gFailures;
	}
}

// ---------------------------------------------------------------------------
// R1, R2
// ---------------------------------------------------------------------------
static void TestPermutationAndOrder(int Span)
{
	const VoxelRingOrder::FTable* T = VoxelRingOrder::Get(Span);
	Check(T != nullptr, "table exists for a legal span");
	if (!T) { return; }

	const int Edge = 2 * Span + 1;
	Check(T->Num() == Edge * Edge, "R1 table size == (2S+1)^2");

	std::set<std::pair<int, int>> Seen;
	int PrevRSq = -1;
	for (int I = 0; I < T->Num(); ++I)
	{
		const VoxelRingOrder::FOffset& O = T->At(I);
		Check(O.DX >= -Span && O.DX <= Span && O.DY >= -Span && O.DY <= Span,
		      "R1 offset inside the square");
		const bool bNew = Seen.insert({O.DX, O.DY}).second;
		Check(bNew, "R1 no duplicate offset");
		const int RSq = T->RadiusSqAt(I);
		Check(RSq >= PrevRSq, "R2 squared lattice radius is nondecreasing");
		PrevRSq = RSq;
	}
	Check(int(Seen.size()) == Edge * Edge, "R1 every square cell present");

	// The binary searches must agree with a linear scan.
	for (int RSq = 0; RSq <= 2 * Span * Span; RSq += 7)
	{
		int LinearLo = T->Num();
		int LinearHi = T->Num();
		for (int I = 0; I < T->Num(); ++I)
		{
			if (LinearLo == T->Num() && T->RadiusSqAt(I) >= RSq) { LinearLo = I; }
			if (LinearHi == T->Num() && T->RadiusSqAt(I) > RSq) { LinearHi = I; }
		}
		Check(T->LowerBound(RSq) == LinearLo, "R2 LowerBound matches a linear scan");
		Check(T->UpperBound(RSq) == LinearHi, "R2 UpperBound matches a linear scan");
	}
}

// ---------------------------------------------------------------------------
// R3 -- WindowFor is conservative for every anchor position in its chunk.
// ---------------------------------------------------------------------------
static void TestWindowConservative(int Span, double InnerChunks, double OuterChunks)
{
	const VoxelRingOrder::FTable* T = VoxelRingOrder::Get(Span);
	if (!T) { return; }
	const VoxelRingOrder::FWindow W = VoxelRingOrder::WindowFor(*T, InnerChunks, OuterChunks);

	// Index of each offset, so "is it in the window" is a lookup.
	std::vector<int> IndexOf(size_t(2 * Span + 1) * size_t(2 * Span + 1), -1);
	auto Slot = [Span](int DX, int DY) {
		return size_t(DY + Span) * size_t(2 * Span + 1) + size_t(DX + Span);
	};
	for (int I = 0; I < T->Num(); ++I)
	{
		IndexOf[Slot(T->At(I).DX, T->At(I).DY)] = I;
	}

	// The anchor sits at (fx, fy) inside its own chunk; a cell at lattice
	// offset (DX,DY) has centre (DX + 0.5 - fx, DY + 0.5 - fy) chunk edges away.
	for (int SX = 0; SX < 5; ++SX)
	{
		for (int SY = 0; SY < 5; ++SY)
		{
			const double FX = double(SX) / 4.0; // 0 .. 1 inclusive, the extremes included
			const double FY = double(SY) / 4.0;
			for (int DY = -Span; DY <= Span; ++DY)
			{
				for (int DX = -Span; DX <= Span; ++DX)
				{
					const double CX = double(DX) + 0.5 - FX;
					const double CY = double(DY) + 0.5 - FY;
					const double D = std::sqrt(CX * CX + CY * CY);
					if (D < InnerChunks || D > OuterChunks)
					{
						continue; // the body would reject it anyway
					}
					const int I = IndexOf[Slot(DX, DY)];
					Check(I >= W.Begin && I < W.End,
					      "R3 a cell inside the true band is inside the window");
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// R4 -- the budget stop never omits a cell nearer than one already admitted.
// ---------------------------------------------------------------------------
static void TestStopRuleConservative(int Span, int Budget)
{
	const VoxelRingOrder::FTable* T = VoxelRingOrder::Get(Span);
	if (!T) { return; }

	for (int SX = 0; SX < 5; ++SX)
	{
		for (int SY = 0; SY < 5; ++SY)
		{
			const double FX = double(SX) / 4.0;
			const double FY = double(SY) / 4.0;
			auto TrueDist = [&](int I) {
				const double CX = double(T->At(I).DX) + 0.5 - FX;
				const double CY = double(T->At(I).DY) + 0.5 - FY;
				return std::sqrt(CX * CX + CY * CY);
			};

			// Walk in table order, "admitting" every cell, and latch at Budget.
			int Admitted = 0;
			int LatchRSq = -1;
			int StopIndex = T->Num();
			double FarthestAdmitted = 0.0;
			for (int I = 0; I < T->Num(); ++I)
			{
				if (LatchRSq >= 0 && T->RadiusSqAt(I) > VoxelRingOrder::StopRadiusSqAfterLatch(LatchRSq))
				{
					StopIndex = I;
					break;
				}
				if (LatchRSq < 0)
				{
					const double D = TrueDist(I);
					if (D > FarthestAdmitted) { FarthestAdmitted = D; }
					if (++Admitted >= Budget) { LatchRSq = T->RadiusSqAt(I); }
				}
			}
			if (LatchRSq < 0) { continue; } // budget never latched at this span

			// Nothing beyond the stop may be nearer than the farthest admitted.
			for (int I = StopIndex; I < T->Num(); ++I)
			{
				Check(TrueDist(I) >= FarthestAdmitted - 1e-9,
				      "R4 no cell nearer than the farthest admitted is left unvisited");
			}
		}
	}
}

// ---------------------------------------------------------------------------
// R5 -- the measurement, printed not asserted.
// ---------------------------------------------------------------------------
static void ReportSaving(int Span, double InnerChunks, double OuterChunks, int Budget)
{
	const VoxelRingOrder::FTable* T = VoxelRingOrder::Get(Span);
	if (!T) { return; }
	const VoxelRingOrder::FWindow W = VoxelRingOrder::WindowFor(*T, InnerChunks, OuterChunks);

	int Visited = 0;
	int Admitted = 0;
	int LatchRSq = -1;
	for (int I = W.Begin; I < W.End; ++I)
	{
		if (LatchRSq >= 0 && T->RadiusSqAt(I) > VoxelRingOrder::StopRadiusSqAfterLatch(LatchRSq))
		{
			break;
		}
		++Visited;
		if (LatchRSq < 0 && ++Admitted >= Budget) { LatchRSq = T->RadiusSqAt(I); }
	}
	const int Square = T->Num();
	std::printf("      span=%-4d inner=%-6.1f outer=%-6.1f budget=%-5d  square=%-8d "
	            "window=%-8d visited=%-7d  %.1fx fewer\n",
	            Span, InnerChunks, OuterChunks, Budget, Square, W.End - W.Begin, Visited,
	            Visited > 0 ? double(Square) / double(Visited) : 0.0);
}

// ---------------------------------------------------------------------------
// E1, E2 -- the edited-lane gate.
// ---------------------------------------------------------------------------
static void TestEditedLaneGate()
{
	const double Edge = 256.0;
	const double InnerAdmitUU = 0.0;
	const double OuterUU = 4000.0;
	const double AdmitOuterUU = 4181.0;

	// A spread of edited footprints, one of them deliberately close to the
	// outer edge so the gate has something that can actually flip.
	std::vector<VoxelEditedLane::FFootprint> Edits;
	for (int I = 0; I < 40; ++I)
	{
		Edits.push_back({I * 3 - 20, (I * 7) % 31 - 15});
	}

	auto Verdict = [&](const VoxelEditedLane::FFootprint& FP, double AX, double AY) {
		const double CX = (double(FP.X) + 0.5) * Edge;
		const double CY = (double(FP.Y) + 0.5) * Edge;
		const double D = std::sqrt((CX - AX) * (CX - AX) + (CY - AY) * (CY - AY));
		return D >= InnerAdmitUU && D <= AdmitOuterUU;
	};
	auto DistUU = [&](const VoxelEditedLane::FFootprint& FP, double AX, double AY) {
		const double CX = (double(FP.X) + 0.5) * Edge;
		const double CY = (double(FP.Y) + 0.5) * Edge;
		return std::sqrt((CX - AX) * (CX - AX) + (CY - AY) * (CY - AY));
	};

	VoxelEditedLane::FLevelGate Gate;
	double AX = 0.0, AY = 0.0;
	std::vector<bool> LastKnown(Edits.size(), false);
	bool bHaveLastKnown = false;
	int FullSweeps = 0, Skips = 0;

	for (int Step = 0; Step < 4000; ++Step)
	{
		AX += 11.0; // ~4% of a chunk edge per call, a plausible flight cadence
		AY += 5.0;

		const VoxelEditedLane::EPlan Plan =
			Gate.PlanForCall(AX, AY, int32_t(Edits.size()));

		if (Plan == VoxelEditedLane::EPlan::FullSweep)
		{
			++FullSweeps;
			Gate.BeginFullSweep();
			for (size_t I = 0; I < Edits.size(); ++I)
			{
				Gate.NoteEnumerated(DistUU(Edits[I], AX, AY), InnerAdmitUU, OuterUU, AdmitOuterUU);
				LastKnown[I] = Verdict(Edits[I], AX, AY);
			}
			Gate.EndFullSweep(AX, AY);
			bHaveLastKnown = true;
		}
		else
		{
			++Skips;
			// E1: nothing the gate skipped may have changed its verdict.
			if (bHaveLastKnown)
			{
				for (size_t I = 0; I < Edits.size(); ++I)
				{
					Check(Verdict(Edits[I], AX, AY) == LastKnown[I],
					      "E1 the gate skipped a call on which a verdict changed");
				}
			}
			Gate.EndDirtySweep();
		}
	}

	Check(FullSweeps > 0, "E2 the gate does sweep when geometry demands it");
	Check(Skips > 0, "E2 the gate does skip");
	std::printf("      edited gate over 4000 calls: full=%d skipped=%d (%.1f%% skipped) slack=%.0fuu\n",
	            FullSweeps, Skips, 100.0 * double(Skips) / 4000.0, Gate.GetSlackUU());

	// Overflow must degrade to FullSweep, never to a drop.
	VoxelEditedLane::FLevelGate G2;
	G2.BeginFullSweep();
	G2.NoteEnumerated(1000.0, InnerAdmitUU, OuterUU, AdmitOuterUU);
	G2.EndFullSweep(0.0, 0.0);
	for (int I = 0; I < VoxelEditedLane::kMaxDirty + 10; ++I) { G2.MarkDirty(I, 0); }
	Check(G2.HasOverflowed(), "E2 dirty overflow latches");
	Check(G2.PlanForCall(0.0, 0.0, 40) == VoxelEditedLane::EPlan::FullSweep,
	      "E2 dirty overflow degrades to FullSweep, never to a drop");

	// A truncated sweep must not publish a slack.
	VoxelEditedLane::FLevelGate G3;
	G3.BeginFullSweep();
	G3.NoteEnumerated(1000.0, InnerAdmitUU, OuterUU, AdmitOuterUU);
	G3.NoteFullSweepTruncated();
	G3.EndFullSweep(0.0, 0.0);
	Check(G3.PlanForCall(0.0, 0.0, 40) == VoxelEditedLane::EPlan::FullSweep,
	      "E2 a budget-truncated sweep resumes next call");
}

// ---------------------------------------------------------------------------
// P1, P2, P3 -- the recompute profile's reconciliation arithmetic.
// ---------------------------------------------------------------------------
static void TestRecomputeProfile()
{
	using namespace VoxelRecomputeProfile;

	// P1: a clean partition reconciles to zero.
	{
		FWindow W;
		W.AddRecomputeTotal(0.100);             // 100 ms of recompute
		W.AddStage(EStage::Prologue, 0.002);
		W.AddStage(EStage::FineResidency, 0.001);
		W.AddStage(EStage::ExitScan, 0.037);
		W.AddStage(EStage::QueueFilter, 0.000);
		W.AddStage(EStage::EntryScan, 0.055);
		W.AddStage(EStage::Sort, 0.003);
		W.AddStage(EStage::LiveConsume, 0.001);
		W.AddStage(EStage::Epilogue, 0.001);
		Check(std::fabs(W.GetTotalMs() - 100.0) < 1e-9, "P1 total is the recompute timer");
		Check(std::fabs(W.NamedMs() - 100.0) < 1e-9, "P1 named stages sum to the total");
		Check(std::fabs(W.ResidualMs()) < 1e-9, "P1 a clean partition has zero residual");
	}

	// P1: an unbucketed stage shows up as a POSITIVE residual of its size.
	{
		FWindow W;
		W.AddRecomputeTotal(0.100);
		W.AddStage(EStage::EntryScan, 0.055);
		W.AddStage(EStage::ExitScan, 0.030);
		Check(std::fabs(W.ResidualMs() - 15.0) < 1e-9,
		      "P1 unbucketed work is the residual, to the millisecond");
		Check(W.ResidualPct() > 14.0 && W.ResidualPct() < 16.0, "P1 residual percentage");
	}

	// P2: overlapping timers must go NEGATIVE, not quietly small.
	{
		FWindow W;
		W.AddRecomputeTotal(0.100);
		W.AddStage(EStage::EntryScan, 0.090);
		W.AddStage(EStage::ExitScan, 0.037);
		// EntryScan double-counted as a nested stage on top:
		W.AddStage(EStage::Sort, 0.020);
		Check(W.ResidualMs() < 0.0, "P2 overlapping timers produce a NEGATIVE residual");
		Check(std::fabs(W.ResidualMs() + 47.0) < 1e-9, "P2 the overlap is the residual magnitude");
	}

	// P3: never-ran and ran-for-free both print 0.00; only N separates them.
	{
		FWindow W;
		W.AddRecomputeTotal(0.100);
		W.AddStage(EStage::QueueFilter, 0.0); // ran, cost nothing
		Check(W.GetStage(EStage::QueueFilter).Ms == 0.0, "P3 a free stage costs 0 ms");
		Check(W.GetStage(EStage::QueueFilter).N == 1, "P3 ... but was entered once");
		Check(!W.StageNeverRan(EStage::QueueFilter), "P3 a free stage is not a never-ran stage");
		Check(W.GetStage(EStage::ExitScan).Ms == 0.0, "P3 an unentered stage also costs 0 ms");
		Check(W.GetStage(EStage::ExitScan).N == 0, "P3 ... and was entered zero times");
		Check(W.StageNeverRan(EStage::ExitScan), "P3 the two are distinguishable");
	}

	// Counters accumulate per level and do not bleed across levels.
	{
		FWindow W;
		FEntryCounters C;
		C.CellsVisited = 7225;
		C.ZCells = 240000;
		C.MemoHit = 7000;
		C.MemoFill = 225;
		W.AddEntry(0, C);
		W.AddEntry(0, C);
		W.AddEntry(3, C);
		Check(W.GetEntry(0).CellsVisited == 14450, "entry counters accumulate");
		Check(W.GetEntry(3).CellsVisited == 7225, "entry counters are per level");
		Check(W.GetEntry(1).CellsVisited == 0, "entry counters do not bleed");
		Check(W.GetEntry(0).ZCells == 480000, "Z-cell counter accumulates");
	}

	std::printf("      profile: 8 named stages, residual signed, N kept beside every Ms\n");
}

int main()
{
	std::printf("VoxelRingOrder / VoxelEditedLaneGate -- standalone checks\n\n");

	std::printf("  R1/R2 permutation and ordering\n");
	for (int Span : {1, 2, 5, 17, 42, 61})
	{
		TestPermutationAndOrder(Span);
	}

	std::printf("  R3 window conservativeness\n");
	TestWindowConservative(20, 0.0, 18.0);
	TestWindowConservative(20, 7.0, 18.0);
	TestWindowConservative(30, 13.5, 27.25);

	std::printf("  R4 budget stop conservativeness\n");
	TestStopRuleConservative(20, 32);
	TestStopRuleConservative(30, 128);
	TestStopRuleConservative(42, 512);

	std::printf("  R5 saving (measurement, not an assertion)\n");
	ReportSaving(42, 0.0, 42.0, 512);   // level 0: no inner pad, budget binds
	ReportSaving(42, 0.0, 42.0, 128);
	ReportSaving(42, 21.0, 42.0, 512);  // a coarse ring: inner pad plus budget
	ReportSaving(42, 21.0, 42.0, 1 << 30); // inner pad and corners only, no budget
	ReportSaving(61, 30.0, 61.0, 512);

	std::printf("  E1/E2 edited-footprint lane gate\n");
	TestEditedLaneGate();

	std::printf("  P1/P2/P3 recompute profile reconciliation\n");
	TestRecomputeProfile();

	std::printf("\n%s (%d failure%s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures,
	            gFailures == 1 ? "" : "s");
	return gFailures == 0 ? 0 : 1;
}

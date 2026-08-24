// VoxelEditedLaneGate.h -- bounds mode 2's edited-footprint lane.
//
// ===========================================================================
// THE PROBLEM
// ===========================================================================
// VoxelWorldSubsystem.cpp:16769-16804, inside the mode-2 live consume:
//
//     if (EditedFootprintMaxZ[Level].Num() > 0 || EditedFootprintMinZ[Level].Num() > 0)
//     {
//         TSet<FIntPoint> LiveEditedSeen;               // fresh allocation, per level, per call
//         ...
//         for (const auto& EditedPair : EditedFootprintMaxZ[Level]) { Enumerate(...); }
//         for (const auto& EditedPair : EditedFootprintMinZ[Level]) { Enumerate(...); }
//     }
//
// Every live call, every level, the WHOLE edited map is re-walked and every
// wanted footprint is re-enumerated through EnumerateSurfaceFootprintCandidates
// -- the full per-footprint Z walk with its hash probes. It is O(edit-map size)
// and it is UNBUDGETED, while every other live lane is O(delta) or explicitly
// capped (cold is capped at kLiveColdFootprintBudget = 2048 per call).
//
// So in an edited world THIS lane, not the proposals, is what mode 2 costs. It
// is also the wrong shape twice over: it repeats identical work on every call,
// and it scales with how much the player has ever dug rather than with what
// changed.
//
// ===========================================================================
// THE OBSERVATION THAT REMOVES THE TERM
// ===========================================================================
// An edited footprint needs re-enumerating only when its ANSWER can have
// changed. The answer is EntryFootprintXYWanted, which is a function of the
// footprint centre distance against three fixed radii (InnerAdmit, Outer,
// AdmitOuter) plus the seam-parent test. So the answer changes only when:
//
//   (a) the edit itself changed -- the footprint was just added to, or moved
//       in, EditedFootprintMin/MaxZ; or
//   (b) the anchor moved far enough that some footprint CROSSED one of those
//       radii.
//
// (a) is a dirty set, maintained at the mutation sites -- O(edits made).
//
// (b) is the part that looks like it needs an O(edit-map) test every call, and
// does not. At each full sweep, record the SMALLEST distance from any
// enumerated footprint centre to the nearest decision radius. Call it the
// SLACK. No footprint can cross any radius until the anchor has moved by at
// least the slack, because moving the anchor by d changes every footprint
// centre distance by at most d. So:
//
//     accumulated anchor motion since the last full sweep  <  slack
//         =>  no verdict can have changed  =>  dirty set only.
//
// One scalar compare per call. The full sweep then happens on a cadence set by
// GEOMETRY -- how close the nearest edit sits to a ring edge -- instead of by
// tick rate.
//
// The degenerate case is a footprint sitting exactly on a radius: slack 0, full
// sweep every call. That is correct (its verdict genuinely can flip on any
// motion) and it is EXACTLY today behaviour, so this gate can never be worse
// than what it replaces. Stating that is the point: the switch has a defined
// floor, not just a hoped-for ceiling.
//
// ===========================================================================
// FAILING READINGS, STATED IN ADVANCE
// ===========================================================================
// Never gate on a statistic that cannot come out the other way. The counters
// this gate feeds (editFull=, editDirty=, editSkip=, editDefer=, editSlack=):
//
//   editSkip = 0 across a window with edits present and the anchor moving
//       -> the gate NEVER SKIPS. Either the slack is being published as 0
//          (a footprint on a radius, legitimate -- editSlack= says so) or
//          NoteEnumerated is not being called and the slack was never
//          computed. The two are separable only because editSlack is printed
//          next to editSkip.
//   editFull = editSkip + editDirty is FALSE by construction; the identity
//       that must hold is editFull + editDirty + editSkip == live consume
//       calls with edits present. A drift there means a call took none of the
//       three paths, which is the silent-lane failure this project has now
//       found in three places.
//   editDefer climbing without editFull climbing -> the per-call budget is
//       below the edit-map size and the tail is starving. The deferred
//       remainder is re-swept next call by construction (the dirty flags are
//       not cleared for it), so this is throughput, not a hole -- but a
//       sustained climb means a dig can take many recomputes to appear.
//   editSlack collapsing to 0 and staying there while editFull climbs -> the
//       gate is armed but geometry is defeating it; the fix is not here, it
//       is that an edit is parked on a ring edge.
//
// A gate that only ever reports "skipped" is as broken as one that never
// skips: editFull must be seen NON-ZERO after a teleport or a long flight, or
// the slack arithmetic is wrong in the unsafe direction and edits are being
// missed. That is the mutation to run -- see the doc.
//
// UE-FREE ON PURPOSE, like VoxelRingOrder.h, so tools/test-voxel-ring-order.cpp
// exercises this exact code with a plain compiler.

#pragma once

#include <cmath>
#include <cstdint>

namespace VoxelEditedLane
{
	// The dirty set is a fixed-capacity array, not a hash set: edits arrive in
	// small clustered bursts (a player digs where the player is), the common
	// occupancy is single digits, and a fixed array costs no allocation on the
	// live path at all -- which matters because the lane this replaces
	// allocated a fresh TSet per level per call.
	//
	// OVERFLOW IS NOT A DROP. Past capacity the gate latches
	// bDirtyOverflow and answers FullSweep until the next full sweep clears
	// it. Degrading to today behaviour is the safe direction; silently
	// forgetting a dirty footprint would be a missing-chunk bug.
	constexpr int32_t kMaxDirty = 256;

	// Per-call cap on footprints a FULL sweep may enumerate, mirroring the
	// cold lane kLiveColdFootprintBudget. The remainder is counted deferred
	// and the sweep is NOT marked complete, so the next call resumes it.
	constexpr int32_t kFullSweepBudget = 512;

	struct FFootprint
	{
		int32_t X = 0;
		int32_t Y = 0;

		bool operator==(const FFootprint& O) const { return X == O.X && Y == O.Y; }
	};

	enum class EPlan : uint8_t
	{
		Nothing,    // no edits at this level, or nothing can have changed and nothing is dirty
		DirtyOnly,  // enumerate the dirty set only
		FullSweep,  // walk both edited maps
	};

	// One level worth of state. Game thread only.
	class FLevelGate
	{
	public:
		// ---- mutation side -------------------------------------------------
		// Called wherever EditedFootprintMin/MaxZ gains or changes an entry.
		void MarkDirty(int32_t X, int32_t Y)
		{
			const FFootprint FP{X, Y};
			for (int32_t I = 0; I < DirtyNum; ++I)
			{
				if (Dirty[I] == FP) { return; }
			}
			if (DirtyNum >= kMaxDirty)
			{
				bDirtyOverflow = true; // -> FullSweep, never a drop
				return;
			}
			Dirty[DirtyNum++] = FP;
		}

		int32_t GetDirtyNum() const { return DirtyNum; }
		const FFootprint& DirtyAt(int32_t I) const { return Dirty[I]; }
		bool HasOverflowed() const { return bDirtyOverflow; }

		// ---- decision ------------------------------------------------------
		// EditedCount is EditedFootprintMaxZ[L].Num() + EditedFootprintMinZ[L].Num().
		EPlan PlanForCall(double AnchorX, double AnchorY, int32_t EditedCount) const
		{
			if (EditedCount <= 0)
			{
				return EPlan::Nothing;
			}
			if (!bHaveSweptOnce || bDirtyOverflow || bSweepIncomplete)
			{
				// Never swept, lost track of which are dirty, or the last
				// sweep ran out of budget mid-way. All three mean the full
				// walk, and all three are counted apart at the log site.
				return EPlan::FullSweep;
			}
			const double DX = AnchorX - SweptAnchorX;
			const double DY = AnchorY - SweptAnchorY;
			if ((DX * DX + DY * DY) >= SlackUU * SlackUU)
			{
				return EPlan::FullSweep; // a verdict may have flipped
			}
			return DirtyNum > 0 ? EPlan::DirtyOnly : EPlan::Nothing;
		}

		// ---- sweep bracket -------------------------------------------------
		void BeginFullSweep()
		{
			PendingSlackUU = kSlackInfinity;
			bSweepIncomplete = false;
		}

		// Fold one enumerated footprint into the slack. DistUU is the same
		// centre distance the caller already computed for EntryFootprintXYWanted
		// -- passed in rather than recomputed, so the gate and the verdict can
		// never disagree about where a footprint is.
		//
		// The three radii are the three the verdict tests. Any FOURTH radius
		// added to EntryFootprintXYWanted MUST be added here in the same
		// commit, exactly as the incremental sweep radius list at
		// VoxelWorldSubsystem.cpp:16320 says of itself -- a decision radius the
		// slack does not know about is a verdict this gate will skip past.
		void NoteEnumerated(double DistUU, double InnerAdmitUU, double OuterUU,
		                    double AdmitOuterUU)
		{
			Fold(std::fabs(DistUU - InnerAdmitUU));
			Fold(std::fabs(DistUU - OuterUU));
			Fold(std::fabs(DistUU - AdmitOuterUU));
		}

		// Budget ran out: the sweep is a prefix, so it must not publish a
		// slack (the unswept tail was never measured) and the next call must
		// resume it.
		void NoteFullSweepTruncated() { bSweepIncomplete = true; }

		void EndFullSweep(double AnchorX, double AnchorY)
		{
			if (bSweepIncomplete)
			{
				return; // stays FullSweep next call; nothing published
			}
			SweptAnchorX = AnchorX;
			SweptAnchorY = AnchorY;
			SlackUU = PendingSlackUU;
			bHaveSweptOnce = true;
			bDirtyOverflow = false;
			DirtyNum = 0;
		}

		// A dirty-only pass answered everything that could have changed; the
		// swept anchor and slack stay as they were (the geometry did not move
		// far enough to invalidate them), so only the dirty set clears.
		void EndDirtySweep() { DirtyNum = 0; }

		// For the log line: how far the anchor may still travel before a full
		// sweep is forced. 0 means a footprint sits on a radius.
		double GetSlackUU() const { return bHaveSweptOnce ? SlackUU : 0.0; }

		void Reset()
		{
			DirtyNum = 0;
			bDirtyOverflow = false;
			bHaveSweptOnce = false;
			bSweepIncomplete = false;
			SlackUU = 0.0;
			SweptAnchorX = 0.0;
			SweptAnchorY = 0.0;
		}

	private:
		static constexpr double kSlackInfinity = 1.0e30;

		void Fold(double D)
		{
			if (D < PendingSlackUU) { PendingSlackUU = D; }
		}

		FFootprint Dirty[kMaxDirty] = {};
		int32_t DirtyNum = 0;
		bool bDirtyOverflow = false;
		bool bHaveSweptOnce = false;
		bool bSweepIncomplete = false;
		double SlackUU = 0.0;
		double PendingSlackUU = kSlackInfinity;
		double SweptAnchorX = 0.0;
		double SweptAnchorY = 0.0;
	};
} // namespace VoxelEditedLane

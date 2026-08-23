# Removing two Span-shaped terms from recompute

2026-08-23. Follow-on to `docs/gpu-residency-admit-budget-2026-08-23.md`, which
established that **99.13-99.99% of all admission rejections are the per-call
budget gate (a)** and that the sweep *stops admitting but does not stop
enumerating*.

New files, all owned by this work, none of them `VoxelWorldSubsystem.cpp`:

| file | what |
|---|---|
| `ue-project/Source/VoxelEarth/VoxelRingOrder.h` / `.cpp` | distance-ordered annulus enumeration |
| `ue-project/Source/VoxelEarth/VoxelEditedLaneGate.h` | the edited-footprint lane's motion/slack gate |
| `tools/test-voxel-ring-order.cpp` | brute-force checks of both |
| `tools/run-voxel-ring-order-test.sh` | runs them — no build, no editor |

The 50k budget doc's question is *does it reduce the constant, or remove the
term?* Both of these remove a term. Neither needed a build lane to verify.

---

## 1. Distance-ordered enumeration — `-VoxelRingOrderScan`

### The waste

`VoxelWorldSubsystem.cpp:16347` walks the square row-major. Gate (a)
(`:14673`, `AdmissionsThisLevel >= Cap / 4`) stops admitting and the walk keeps
going, paying the Z-range memo, the Z loop and two hash probes per Z cell for
every remaining cell. 22-37 million enumerate-then-discard cells per leg.

An early `break` is the wrong fix and the codebase already says why
(`:3866`): row-major means the first `Cap/4` cells are the scan box's corner —
"the southmost rows, west to east", a strip that starts at the ring's **far**
edge. So change the order, not add a break.

### The mechanism

`VoxelRingOrder::FTable` holds one span's `(2S+1)²` offsets sorted by squared
lattice radius `DX²+DY²`. The order depends only on the span, never on the
anchor, so a table is built **once per distinct span and kept for the session**
(29 KB at today's level-0 span of 42; a handful of spans exist across the
cascade). That is what makes this free: there is no per-call sort.

Three savings fall out, and only the third needs the budget:

1. **`WindowFor()` starts past the inner-pad disc.** For L>0 that disc is most
   of the square, and today every one of its cells is iterated and skipped.
2. **`WindowFor()` ends before the corners.** The square wastes `(4-π)/4 = 21%`
   on corners outside `AdmitOuter` even with no budget in play.
3. **`StopRadiusSqAfterLatch()` stops the walk when gate (a) latches**, having
   visited the cells it actually wanted.

### The one inequality everything rests on

The table orders by lattice radius; the sweep's real metric is anchor-point to
cell-centre distance. The anchor sits inside its own chunk, so with
`h = sqrt(0.5) = 0.7071` chunk edges:

```
    trueDist / edge   in   [ r - h , r + h ]
```

Every bound is derived from that and nothing else. `WindowFor` widens by `h` in
both directions, so the window can only ever be too **wide** — it may include
cells the body rejects, never exclude one the body would accept.
`StopRadiusSqAfterLatch` returns `(sqrt(latchRSq) + 2h)²`, because a cell whose
true distance is inside the farthest admitted cell's true distance can sit up
to `2h` lattice units further out. Stopping at the latch radius itself would
drop cells genuinely nearer than ones already admitted — the same defect
row-major has, just smaller.

### Measured, tonight, without a build

`bash tools/run-voxel-ring-order-test.sh` compiles the engine headers
themselves with clang and brute-forces the claims against every anchor
fractional offset on a 5×5 grid:

```
R1 the table is a permutation of the square      PASS
R2 squared lattice radius is nondecreasing       PASS
R3 WindowFor is conservative                     PASS
R4 the budget stop is conservative               PASS
E1 the edited gate never skips a changed verdict PASS
E2 the edited gate does skip                     PASS
```

and R5, the measurement (printed, not asserted):

| span | inner | outer | budget | square | window | visited | saving |
|---|---|---|---|---|---|---|---|
| 42 | 0 | 42 | 512 | 7,225 | 5,785 | **641** | **11.3x** |
| 42 | 0 | 42 | 128 | 7,225 | 5,785 | **185** | **39.1x** |
| 42 | 21 | 42 | 512 | 7,225 | 4,540 | **724** | **10.0x** |
| 42 | 21 | 42 | ∞ | 7,225 | 4,540 | 4,540 | 1.6x |
| 61 | 30 | 61 | 512 | 15,129 | 9,440 | **792** | **19.1x** |

Two things to read off that table. The `budget = ∞` row is the **floor**: even
with no budget ever latching, skipping the inner disc and the corners is 1.6x
on a coarse ring, for free. And the saving **grows with span** — 11.3x at 42,
19.1x at 61 — which is the term removal stated as a number. Visited cells go
from `O(Span²)` to `O(Budget + sqrt(Budget))`, and `Span²` is exactly what
grows when the rings widen to the requested 8-10 km.

**The checks fail when mutated.** Deleting the half-diagonal slack from
`StopRadiusSqAfterLatch` and from `WindowFor`'s inner edge produces **1,102
failures** across R3 and R4. Restoring it returns PASS. A bound that has only
ever been seen to hold is not evidence that it is load-bearing; this one is.

### Hook D — `VoxelWorldSubsystem.cpp:16347`

Replace the two loop headers, keep the cell body **verbatim**. The switch reads
`VoxelStreamAdmission::RingOrderScanEnabled()` (`-VoxelRingOrderScan`,
command-line-latched like every streaming-topology switch in that file, for
`-VoxelPendingJobCap`'s reason). Off = the existing loops, byte-identical.

```cpp
		// -VoxelRingOrderScan: distance-ordered enumeration, so gate (a) can
		// stop the walk having visited the cells it wanted rather than the
		// scan box's corner. See ue-project/Source/VoxelEarth/VoxelRingOrder.h
		// for the half-diagonal bound both radii below are widened by.
		const VoxelRingOrder::FTable* RingTable =
			VoxelStreamAdmission::RingOrderScanEnabled() ? VoxelRingOrder::Get(ScanSpan) : nullptr;
		if (RingTable == nullptr)
		{
			if (VoxelStreamAdmission::RingOrderScanEnabled())
			{
				// Span past kMaxCachedSpan. Falls back to row-major and SAYS
				// SO -- a switch that silently degrades to the old path is how
				// a feature reads healthy while measuring nothing.
				++LevelRingOrderFallbacksSinceLog[Level];
			}
			for (int32 Cy = AnchorChunk.Y - ScanSpan; Cy <= AnchorChunk.Y + ScanSpan; ++Cy)
			{
				for (int32 Cx = AnchorChunk.X - ScanSpan; Cx <= AnchorChunk.X + ScanSpan; ++Cx)
				{
					/* ... EXISTING CELL BODY, unchanged ... */
				}
			}
		}
		else
		{
			const VoxelRingOrder::FWindow RingWin = VoxelRingOrder::WindowFor(
				*RingTable, LevelInnerAdmitUU / ChunkEdge, AdmitOuterUU / ChunkEdge);
			LevelRingOrderSkippedCellsSinceLog[Level] +=
				int64(RingTable->Num()) - int64(RingWin.End - RingWin.Begin);
			int32 RingStopRSq = -1;
			for (int32 RI = RingWin.Begin; RI < RingWin.End; ++RI)
			{
				if (RingStopRSq >= 0 && RingTable->RadiusSqAt(RI) > RingStopRSq)
				{
					LevelRingOrderStoppedCellsSinceLog[Level] += int64(RingWin.End - RI);
					// A STOPPED SCAN IS NOT A FULL SCAN. Identical bookkeeping
					// to -VoxelCutoffClamp (:16272), for the identical reason:
					// the outer band was never evaluated, so work is
					// outstanding by construction, the deferral CLEAR in
					// TruncatePendingJobQueue must stay guarded, and the NEXT
					// scan must not be eligible for incremental diffing.
					// Reusing bLevelScanClampedThisCall gets all three at once
					// (it already feeds bLevelLastScanClamped below the loop).
					bAdmissionDeferredWork[Level] = true;
					bLevelScanClampedThisCall[Level] = true;
					if (VoxelStreamAdmission::IncrementalAdmissionEnabled())
					{
						// Same hand-back the clamp performs: entries the walk
						// will not re-attempt go back to the backlog rather
						// than being dropped.
						for (int32 RJ = RI; RJ < RingWin.End; ++RJ)
						{
							DeferredFootprints[Level].Add(FIntPoint(
								AnchorChunk.X + RingTable->At(RJ).DX,
								AnchorChunk.Y + RingTable->At(RJ).DY));
						}
					}
					break;
				}
				const int32 Cx = AnchorChunk.X + RingTable->At(RI).DX;
				const int32 Cy = AnchorChunk.Y + RingTable->At(RI).DY;
				{
					/* ... EXISTING CELL BODY, unchanged ... */
				}
				// Gate (a) has latched: everything past the stop radius is
				// provably farther than something already admitted.
				if (RingStopRSq < 0 && Cap > 0 && AdmissionsThisLevel >= Cap / 4)
				{
					RingStopRSq =
						VoxelRingOrder::StopRadiusSqAfterLatch(RingTable->RadiusSqAt(RI));
				}
			}
		}
```

Add near the top of the file: `#include "VoxelRingOrder.h"`, and three window
counters beside the existing `LevelClampSkippedCellsSinceLog` block
(`:7770`-ish): `LevelRingOrderSkippedCellsSinceLog[]`,
`LevelRingOrderStoppedCellsSinceLog[]`, `LevelRingOrderFallbacksSinceLog[]`.
Add `VoxelRingOrderScan` to `tools/frontend-switch-classification.txt`.

**Three constraints on landing it, all of them recorded lessons:**

* **Do not combine with `-VoxelGpuResidency >= 1`.** The GPU mirror predicts
  full-disc decisions; a stopped CPU walk reads as spurious `adMISS` in its
  comparator. Exactly the warning `-VoxelCutoffClamp` already carries
  (`:16258`).
* `bLevelScanClampedThisCall` is deliberately reused rather than a new flag.
  The recorded one-way-latch bug (428 full-annulus rescans in 14 s) came from a
  global flag gating a per-level clear; a *second* per-level "this scan was
  partial" flag would be a second thing to forget to set.
* The composition with `-VoxelNearestAdmit` is the interesting one and it is
  **benign**: nearest-admit collects candidates and sorts them, so it does not
  latch gate (a) at all and `RingStopRSq` stays -1. The walk then runs the full
  window, which is still the 1.6x floor. Ring order and nearest-admit solve the
  same problem at different costs; ring order is the cheaper half and does not
  need the sort.

### Counters and failing readings, `ringSkip= ringStop= ringFall=`

| reading | means |
|---|---|
| `ringSkip = 0` and `ringStop = 0`, switch on | **the feature never fired.** No timing on that leg counts. |
| `ringFall > 0` | span exceeded `kMaxCachedSpan`; that level measured the OLD path. |
| `ringStop > 0` but `candidatesRejected` unchanged | the stop fires but the body still enumerates — the hook landed on the wrong loop. |
| `ringStop` up, `loaded`/`dispatched` **flat** | **the win.** |
| `ringStop` up, `loaded` **falling** | the stop is cutting real work: the deferral flags are not re-arming and chunks are being lost. **This is the failing reading that matters most**, and it is why `bAdmissionDeferredWork` and `bLevelScanClampedThisCall` are in the break block. |

The mutation that proves it fired: `-VoxelRingOrderScan` with the budget forced
tiny (`-VoxelPendingJobCap=64`) must drive `ringStop` into the millions and
`candidatesRejected` down by the same order. If `candidatesRejected` does not
move, nothing was saved.

---

## 2. The edited-footprint lane — `-VoxelEditedLaneGate`

### The waste

`VoxelWorldSubsystem.cpp:16769-16804`, inside the mode-2 live consume: every
call, every level, the **whole** edited map is re-walked, a fresh
`TSet<FIntPoint>` is allocated per level, and every wanted footprint is
re-enumerated through `EnumerateSurfaceFootprintCandidates`. O(edit-map size),
unbudgeted, while every other live lane is O(delta) or capped (cold at
`kLiveColdFootprintBudget = 2048`). In an edited world this lane, not the
proposals, is what mode 2 costs — and it repeats identical work every call.

### The observation that removes the term

A footprint's answer is `EntryFootprintXYWanted`, a function of its centre
distance against three fixed radii. It can only change when (a) the edit itself
changed, or (b) the anchor moved far enough for something to cross a radius.

(a) is a dirty set maintained at the six mutation sites — O(edits made).

(b) looks like it needs an O(edit-map) test every call and does not. At each
full sweep, record the **smallest** distance from any enumerated footprint
centre to the nearest decision radius — the **slack**. Moving the anchor by `d`
changes every footprint's centre distance by at most `d`, so:

```
    accumulated anchor motion since the last full sweep  <  slack
        =>  no verdict can have changed  =>  dirty set only
```

One scalar compare per call. Full sweeps then happen on a cadence set by
**geometry** — how close the nearest edit sits to a ring edge — instead of by
tick rate. Measured in the standalone test over 4,000 calls with 40 edits and a
flying anchor: **403 full sweeps, 3,597 skipped, 89.9% skipped.**

**It has a defined floor.** A footprint sitting exactly on a radius gives slack
0 and a full sweep every call — which is correct, and is exactly today's
behaviour. The gate can never be *worse* than what it replaces. The test also
checks the two degradations: dirty-set overflow latches to `FullSweep` (never a
drop), and a budget-truncated sweep publishes no slack and resumes next call.

### Hook E — two sites

**E1, the mutation side.** `RebuildEditedFootprintsFromOverlay` and the
incremental edit path are the only writers: `:23076`, `:23078`, `:23086`,
`:23088`, `:23132`, `:23135`, `:23168`, `:23169`. After each `FindOrAdd` pair,
one line naming the same footprint:

```cpp
			EditedLaneGate[Level].MarkDirty(Ancestor.X, Ancestor.Y);
```

(`EditedLaneGate[0].MarkDirty(Chunk.X, Chunk.Y)` at the level-0 sites.)
`RebuildEditedFootprintsFromOverlay` rebuilds everything, so it should call
`EditedLaneGate[L].Reset()` per level first — a rebuild invalidates the slack,
and `Reset` makes the next call a `FullSweep` by construction.

**E2, the consume side.** Replace the `if (EditedFootprintMaxZ[Level].Num() > 0
|| ...)` block at `:16769` with:

```cpp
			const int32 EditedCount =
				EditedFootprintMaxZ[Level].Num() + EditedFootprintMinZ[Level].Num();
			const VoxelEditedLane::EPlan EditPlan =
				VoxelStreamAdmission::EditedLaneGateEnabled()
					? EditedLaneGate[Level].PlanForCall(Anchor.X, Anchor.Y, EditedCount)
					: (EditedCount > 0 ? VoxelEditedLane::EPlan::FullSweep
					                   : VoxelEditedLane::EPlan::Nothing);
			if (EditPlan == VoxelEditedLane::EPlan::FullSweep)
			{
				++LiveOutcome.EditedFullSweeps;
				EditedLaneGate[Level].BeginFullSweep();
				int32 EditBudget = VoxelEditedLane::kFullSweepBudget;
				/* ... the existing LiveEditedSeen lambda and the two map loops,
				   with these two lines added inside EnumerateEditedFootprint,
				   after EditDistSq is computed and the XYWanted test passes:

				       EditedLaneGate[Level].NoteEnumerated(
				           FMath::Sqrt(EditDistSq), LP.InnerAdmitUU, LP.OuterUU,
				           LP.AdmitOuterUU);
				       if (--EditBudget < 0)
				       {
				           ++LiveOutcome.EditedDeferred;
				           EditedLaneGate[Level].NoteFullSweepTruncated();
				           return;
				       }
				 ... */
				EditedLaneGate[Level].EndFullSweep(Anchor.X, Anchor.Y);
			}
			else if (EditPlan == VoxelEditedLane::EPlan::DirtyOnly)
			{
				++LiveOutcome.EditedDirtyPasses;
				for (int32 DI = 0; DI < EditedLaneGate[Level].GetDirtyNum(); ++DI)
				{
					const VoxelEditedLane::FFootprint& FP = EditedLaneGate[Level].DirtyAt(DI);
					/* the SAME EnumerateEditedFootprint body, on FIntPoint(FP.X, FP.Y) */
				}
				EditedLaneGate[Level].EndDirtySweep();
			}
			else
			{
				++LiveOutcome.EditedSkipped;
			}
```

Member: `VoxelEditedLane::FLevelGate EditedLaneGate[VoxelCoords::kNumLevels];`
beside `EditedFootprintMaxZ` at `:6262`. Include
`"VoxelEditedLaneGate.h"`. Four new `FVoxelResidencyLiveOutcome` fields
(`EditedFullSweeps`, `EditedDirtyPasses`, `EditedSkipped`, `EditedDeferred`) —
**those are in `VoxelResidencyGpu.h`, which I own; say the word and I will add
them and the log line so the hook is a pure call-site edit.**

**The radius list is a contract.** `NoteEnumerated` folds the three radii
`EntryFootprintXYWanted` tests. Any fourth radius added to that verdict must be
added to `NoteEnumerated` in the same commit — the same rule the incremental
sweep's radius list states about itself at `:16320`, and the same "derived, not
verified" detach this project has paid for five times.

### Counters and failing readings, `editFull= editDirty= editSkip= editDefer= editSlack=`

| reading | means |
|---|---|
| `editSkip = 0` with edits present and the anchor moving | **the gate never skips.** Either the slack is genuinely 0 (`editSlack=` says so — a footprint parked on a ring edge) or `NoteEnumerated` is not being called and the slack was never computed. Separable only because `editSlack` prints beside it. |
| `editFull = 0` after a teleport or a long flight | **the gate never sweeps** — the slack arithmetic is wrong in the unsafe direction and edits are being missed. A gate that only ever skips is as broken as one that never does. |
| `editFull + editDirty + editSkip` ≠ live consumes with edits present | a call took none of the three paths — the silent-lane failure found in three places already tonight. |
| `editDefer` climbing without `editFull` climbing | the per-call budget is under the edit-map size and the tail is starving. Not a hole (the sweep resumes), but a dig takes many recomputes to appear. |
| `editSlack` collapsing to 0 and staying there | geometry is defeating the gate; the fix is not in the gate. |

The mutation: dig a hole, fly away, fly back. `editFull` must spike on the
return leg (the footprint crossed `AdmitOuter`) and the dug chunks must be
present. If `editFull` stays flat and the hole is missing, the slack is unsafe.

---

## 3. What these are worth against the 50k budget

Recompute is 246-266 ms/window and scales with **span**, not throughput. These
two remove its two Span-shaped terms:

* the entry sweep: `O(Span²)` → `O(Budget + sqrt(Budget))`, measured 11.3x at
  today's level-0 span and **19.1x at span 61** — the saving grows as the rings
  widen, which is the whole point.
* the edited lane: `O(edit-map)` every call → `O(dirty)` with geometry-paced
  full sweeps, measured 89.9% of calls skipped.

Neither reduces a constant. Both delete a term.

Everything above is checkable tonight with `bash
tools/run-voxel-ring-order-test.sh`; nothing above has been flown, and the leg
that would settle it is the one in
`docs/gpu-residency-admit-budget-2026-08-23.md` §6 with `-VoxelRingOrderScan`
added as a fourth arm.

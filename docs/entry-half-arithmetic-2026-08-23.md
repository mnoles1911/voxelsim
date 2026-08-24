# The entry half: the arithmetic, and a correction to my own number

2026-08-23. Joins the census (`docs/recompute-cost-census-2026-08-23.md`) to
`-VoxelRingOrderScan` (`docs/ring-order-and-edited-lane-2026-08-23.md`).

**Headline: my earlier "11.3x fewer cells" was the cold-ring bound, not the
steady-state answer. At the admission density the legs actually show it is
1.25-1.63x.** The harness that produced the first number could only model the
flattering case; it has been fixed and now asserts the unflattering one.

---

## 1. The fit — what `entryMs` is a function of

From data already on disk: `entryMs[L]` per window (`Voxel recompute (sum…)`),
`scans[L]` per window (`Voxel recompute (max…)`, the `scans R0=…` field), and
rejections per level per reason (`Voxel admission detail`). Three legs,
**1,164 (level, window) observations**.

Model: `entryMs[L,w] = A_L · scans[L,w] + b · candidates[L,w]`

```
   R0  A = 6.706 ms/scan     R3  A = 5.881        per-candidate b = 2 ns
   R1  A = 6.115             R4  A = 5.732        share of entryMs = 0.2%
   R2  A = 6.045             R5  A = 5.083
   entryMs actual=114,335  predicted=107,561  residual=5.9%   R2 = 0.9100
```

### The refusal

**The candidate term is not identifiable, and I am reporting that instead of
its coefficient.** Dropping it entirely changes nothing:

```
   scans only          R2 = 0.9100
   scans + candidates  R2 = 0.9100   (delta +0.0000)
   per-scan costs move by less than 0.5% at every level
```

Rejection counts vary enormously within a level (sd/mean 1.4 to 4.1) and
`entryMs` does not respond. Correlation with `scans` is only 0.55-0.81, so this
is **not** simple collinearity — the data say the cost genuinely does not track
how many candidates a scan rejects. Any per-rejection figure I quoted from this
fit would be an artefact.

### What the fit does say, and it is the useful part

**`entryMs` is a fixed cost per scan**, ~5.1-6.7 ms, near-identical across all
six rings — which is what you expect when every ring's scan square holds about
the same cell count (`Span ≈ 42`, `(2·42+1)² = 7,225`). Per window:

| leg | scans/window | entryMs/window | **ms per scan** | admissions/scan |
|---|---|---|---|---|
| `ahead-on` | 63.9 | 433.6 | **6.79** | 422.5 |
| `gp-ctl2` | 61.0 | 431.0 | **7.06** | 428.9 |
| `pool-pri` | 84.9 | 566.8 | **6.68** | 464.7 |

`6.8 ms / 7,225 cells = ` **940 ns per cell**. That is far too much for a
geometric reject, so the cost is the per-cell *body* — the Z-range memo lookup
and the Z loop's hash probes — paid on **every cell of a mostly-unchanged
annulus, every scan**. The lever on the entry half is therefore **cells visited
per scan**, and nothing else.

---

## 2. The correction

`-VoxelRingOrderScan` reduces cells visited two ways: the window (inner disc +
corners, pure geometry) and the budget stop. My first harness admitted on
**every** visited cell, so the budget latched after `Cap/4` cells and printed
11.3x. Real rings do not admit on every cell: the legs show **423-465
admissions per 7,225-cell scan = 5.9-6.4%, about 1 in 16.**

The harness now takes that density. Same code, three regimes:

| regime | span 42, Cap/4=256 | span 42, Cap/4=512 | span 61, Cap/4=256 |
|---|---|---|---|
| cold ring (1-in-1) — the **upper bound** | 39.1x @128 | **11.3x** | 19.1x |
| **measured density (1-in-16)** | **1.63x** | **1.25x** | **3.35x** |
| no budget latch — the **floor** | 1.25-1.59x | 1.25-1.59x | 1.60x |

And the radial census predictor (below), on the two extreme admission
placements at span 42, `Cap/4 = 256`:

```
   admissions spread through the disc   visits 4,501 of 7,225   1.6x
   admissions in an outer crescent      visits 7,225 of 7,225   1.0x
```

**An outer crescent defeats the budget stop entirely** — and an outer crescent
is exactly what steady flight produces, because the resident interior admits
nothing and the new work is on the leading edge. The test now asserts this
(`C2`), and asserting it is what caught the overclaim: `C2` **failed** on first
run with my guessed threshold, printing the 1.6x/1.0x pair above.

### The millisecond arithmetic

At `-VoxelPendingJobCap` 1024 (`Cap/4 = 256`), 1.63x:

```
   ahead-on   entry 433.6 ms/window  ->  266 ms   saving 168 ms/window
   gp-ctl2    entry 431.0            ->  264      saving 167
   pool-pri   entry 566.8            ->  348      saving 219
```

At cap 2048 (`Cap/4 = 512`) the budget does not latch inside the window at all
and only the floor applies, 1.25x: **saving 87 ms/window**.

**So `-VoxelRingOrderScan` is worth 87-219 ms/window, not the ~390 ms my
earlier figure implied.** On a thread now known to be 61-68% busy that is still
worth having — but it is a 1.25-1.6x on one half of recompute, and it should be
sold as that.

**What the entry half costs after it lands: ~264-348 ms/window.**

---

## 3. What the next term is

`entryMs = scans × cells-per-scan × 940 ns`. Ring order attacks the middle
factor and gets 1.25-1.6x. The two terms left are:

**(a) 940 ns per cell.** The cells that remain are the *resident interior* —
re-memo'd and re-probed every scan although nothing about them changed. This is
the largest single number in the entry half and it has no bounded mechanism.

`-VoxelIncrementalAdmission` exists for exactly this and skips cells whose
verdict cannot have changed between two scan anchors. **It was not on in any of
the three census legs.** It has run twice ever (`p2-on`, `final-allon`), and on
`p2-on` the per-scan cost is **7.42 ms against `ahead-on`'s 6.79** — no better.
That is **not a clean A/B** (`p2-on` lacks the whole GPU-primary stack), so it
is a flag, not a finding: *the one switch aimed at the dominant term has never
been measured against a matched control, and the one leg carrying it is not
faster per scan.* That A/B is a leg, not a build.

**(b) `scans` itself** — 57-85 per window, i.e. 10-14 recomputes each sweeping
six rings. Halving the scan rate halves the entry half exactly, and the
recompute-cadence storm already documented at `VoxelWorldSubsystem.cpp:16830`
(disp=427/5 s parked) shows the cadence is not always earned.

---

## 4. Built: `FRadialCensus` — the refusal turned into a measurement

Added to `ue-project/Source/VoxelEarth/VoxelRingOrder.h`. Cells and admissions
per **equal-area radius band** (32 bands over squared radius), per level, per
window, plus `PredictBudgetStopVisited(census, budget)` — the number a
ring-ordered, budget-stopped walk *would* visit, computed **before the switch
is flipped**.

That makes the saving a **falsifiable prediction**: run one leg with
`-VoxelRadialCensus` and `-VoxelRingOrderScan` **off**, predict the visit count,
then run with the scan on and compare against `ringSkip`/`ringStop`. Predicted
vs actual is a gate that can fail in both directions.

**Failing readings, both ways:**

| reading | means |
|---|---|
| every admission in band 0 | the census is being fed one radius for every cell — the band math is not wired |
| `Cells` all zero with `entryMs > 0` | the census is outside the loop it describes |
| admissions concentrated in the **outermost** band | true and unwelcome: the budget stop cannot help and `-VoxelRingOrderScan` is worth only its window bound. **The reading the feature would rather not produce, which is why it must be able to.** |
| `PredictBudgetStopVisited` returning 0 for an unbound budget | a predictor reporting an infinite speedup for a disabled feature — asserted against in `C3` |

Verified: `bash tools/run-voxel-ring-order-test.sh` — C1 bounded and monotone,
C2 the crescent case, C3 no-budget-means-every-cell. **C2 was observed failing**
(it is what caught the overclaim), and the earlier R3/R4 mutation still produces
1,102 failures.

### Hook H — `-VoxelRadialCensus`, off = byte-identical

`VoxelWorldSubsystem.cpp`, the cell loop at `:16347`. Member beside
`AccumLevelEntryMs` (`:7649`):

```cpp
	VoxelRingOrder::FRadialCensus LevelRadialCensus[VoxelCoords::kNumLevels];
```

Once per level, before the loop:

```cpp
		if (VoxelStreamAdmission::RadialCensusEnabled())
		{
			LevelRadialCensus[Level].MaxRadiusSq = 2 * ScanSpan * ScanSpan; // the square's corner
		}
```

Inside the loop body, after `Cx`/`Cy` are known and after the admission attempt
for that footprint has returned (so `bAdmittedHere` is decided):

```cpp
				if (VoxelStreamAdmission::RadialCensusEnabled())
				{
					const int32 DX = Cx - AnchorChunk.X;
					const int32 DY = Cy - AnchorChunk.Y;
					LevelRadialCensus[Level].Note(DX * DX + DY * DY, bAdmittedHere);
				}
```

And in `MaybeLogCounters`, beside the recompute lines, one line per level plus
the prediction, then `LevelRadialCensus[L] = {}`:

```
Voxel radial census (window): R0[cells=... admits=... predVisited=... predFactor=...] | R1[...] ...
```

`predVisited` is `PredictBudgetStopVisited(LevelRadialCensus[L],
EffectivePendingJobCap / 4)` — the same expression gate (a) uses, so the
prediction cannot budget to a different rule than the thing it predicts.

Register `VoxelRadialCensus` in `tools/frontend-switch-classification.txt`
(done).

---

## 5. The window-divisor sibling — found in my own file

Asked for, and there is one. `FVoxelResidencyGpu::FImpl::MaybeLog` gated on a
**hardcoded `5.0` seconds** while every flight leg passes
`-VoxelPerfLogInterval=2`. So the `[gpu-resid]` windows and the `LogVoxelPerf`
windows were **2.5x apart**, and every side-by-side reading of the two families
— "prop per window" against "recompute per window", which is exactly what
`docs/gpu-residency-admit-budget-2026-08-23.md` does — was wrong by that factor.

Fixed: the interval is now **read** from the same switch the perf logger reads,
and the **real elapsed seconds are printed** on both `[gpu-resid]` lines as
`win=%.1fs`. A window whose length is asserted rather than measured is a
denominator nobody checks.

**This does not change any conclusion in that doc** — `prop`, `rejBud` and
`ms ad` are all from the same window and their ratios hold — but the absolute
"per 5 s window" labels on the `[gpu-resid]` figures were per 2 s, so those
rates were understated by 2.5x, in the direction that made mode 2's admit lane
look *better* than it was.

---

## 6. Summary for sequencing

| item | worth | status |
|---|---|---|
| mode 2 exit half | 37-45% of recompute + all its growth | built, queued as L6 |
| `-VoxelRingOrderScan` | **87-219 ms/window** (1.25-1.63x on entry), was overclaimed as ~390 | built; needs `-VoxelRadialCensus` first to predict, then falsify |
| `-VoxelIncrementalAdmission` A/B | the 940 ns/cell term — the largest in the entry half | **exists, never measured against a matched control** |
| scan-rate reduction | linear in `scans`, 57-85/window | not designed |
| per-rejection cost | **not identifiable** — reported as such | closed |

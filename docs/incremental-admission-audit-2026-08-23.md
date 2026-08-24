# `-VoxelIncrementalAdmission`: audit, a retraction, and the one leg

2026-08-23. Asked: audit it for the failure modes this project keeps producing,
ship whatever the leg needs to be read unambiguously, state the arm matrix, and
**if it is sound and simply never measured, say so and stop**.

**Short answer: the logic is sound, and it is not unmeasured — it has already
run correctly once, and my claim last turn that it "was no better" is
retracted.** What is missing is one matched pair and one counter.

---

## 1. The audit — the logic is sound

| failure mode | verdict |
|---|---|
| **One-way latch** (per-level state, non-per-level clear) — the recorded `:12503` shape | **Sound.** The deferral clear (`:17269`) is per-level and *all four* guards are per-level: `bLevelScannedThisCall[L]`, `LevelCandidatesRejectedThisCall[L] == 0`, `!bLevelHeldBackThisCall[L]`, `!bLevelScanClampedThisCall[L]`. No global flag gates a per-level clear. |
| **Uncounted path** | **Impossible by construction.** The eligibility gate (`:16137-16208`) is an `if / else if` chain in which every branch increments a counter and the terminal `else` increments `LevelIncrScansSinceLog[L]`. `Σincr + Σcauses == Σscans` is therefore an *identity*, not an approximation. **Checked on both legs that carry the switch: delta = 0, exactly.** |
| **"Never fired" vs "fired and was free"** | **Distinguishable.** `incr[L]` counts scans that took the fast path, so `incr=0` and `incr=N` differ, and the four causes attribute the remainder. |
| **Mode-2 interaction** | **Handled, and correctly argued.** `bResidLive` forces a full sweep because a proposal lost to GPU list overflow "was rejected by NOBODY on the CPU, so it is in no backlog set, and its cell crossed no radius" — a diff would skip it forever. Counted as `config`. |
| **`-VoxelCutoffClamp` interaction** | **Handled.** Any finite cutoff, or a clamped predecessor, forces a full sweep; a clamped scan can neither be incremental nor serve as a predecessor. |
| **Deferral-armed scans** | **Handled by design.** In the admission-limited regime rejections are never zero, so the deferral flag never clears — which is why `bLevelRefillRescan` exists: a refill rescan's predecessor *is* valid because everything it left behind is in `DeferredFootprints`. |

Nothing here needs rewriting.

---

## 2. The retraction

Last turn I wrote: *"on `p2-on` the per-scan cost is 7.42 ms against `ahead-on`'s 6.79 — no better."*

**That is wrong and I withdraw it.** `p2-on.log` is the **pre-fix build**: it
prints `forced=`/`deferred=`, the format whose own source comment says *"that
build printed `forced=` (any `bHasRecomputedLevel` clear — refill rescans
included, which made it read ~60% warm)… Do not diff the fields across the two
formats."* On that leg the fast path fired on **824 of 4,910 scans = 16.8%**.

So `p2-on` measured the switch paying its backlog-insert cost while mostly
*not* taking the fast path. It is not evidence against the feature. I read a
number off a leg without checking which build wrote it — the same class of
mistake as reading the linger window.

`tools/incremental-admission-check.py` now **refuses** that format outright
rather than letting anyone repeat it, and the refusal has been observed firing.

---

## 3. What the feature actually does, on the leg that ran it correctly

`final-allon.log` is a current-build leg with the switch on:

```
IDENTITY   incr(4010) + causes(6) = 4016   vs scans 4016   delta=0  OK
TRAFFIC    whole leg 4010/4016 = 99.9%
           WARM ONLY 3558/3558 = 100.0%
causes     first=6, edit=0, underground=0, config=0
```

Against `ahead-on` (switch off):

| | `final-allon` (on) | `ahead-on` (off) | |
|---|---|---|---|
| **ms per scan** | **3.900** | 6.787 | **1.74x** |
| **footprints per hitch frame** | **1,389** | 6,085 | **4.38x** (lower bound) |
| footprints per hitch, per level | `[1252, 34, 11, 7, 11, 74]` | `[3299, 1426, 638, 291, 277, 154]` | R1 **42x**, R2 **58x**, R3 **42x**, R0 2.6x |
| admissions per scan | 423.6 | 422.5 | **identical** |
| entryMs/window | 200.8 | 433.6 | |

**This is the "work deleted, not moved" shape**, and it has the right per-level
signature: the coarse rings collapse (they cross rarely, so almost nothing
changes between their scans) while level 0 falls only 2.6x (it crosses
constantly, so its margin is proportionally large). Admissions per scan are
*identical* — the same admission work for 57% of the cost.

**It is confounded and therefore not a verdict.** `final-allon` also carries
`-VoxelNearestAdmit` and `-VoxelBucketedExitScan`, and lacks the entire
GPU-primary stack. Neither confound plausibly reduces cell visits
(`NearestAdmit` *adds* a collect-and-sort; `BucketedExitScan` is the exit half,
not `entryMs`), but "plausibly" is not a measurement. **One matched pair
settles it.**

---

## 4. The instrumentation gap that would have made the leg ambiguous

**`ThisFrameLevelFootprints[L]` — the counter that proves deletion rather than
relocation — has no window accumulator and is printed only on the hitch line**
(`LogVoxelPerf, Warning`, above the 33.3 ms threshold; declared `:7512`, reset
every frame `:8837`, printed once at `:9638`).

Three consequences, and the third is the bad one:

1. A clean run prints it **not at all**.
2. Hitch frames are the **worst** frames — by construction the ones most likely
   to be full sweeps — so the sampled values are biased **toward the fallback
   path**.
3. **The sample rate falls as the feature works.** Fewer hitches, fewer
   samples. The instrument's visibility is *anti-correlated* with the effect it
   measures.

The 4.38x above survives all three, which is why it is quoted as a **lower
bound**. Hook I removes the bias.

Second, smaller gap: the four fallback causes are **global ints, not
per-level** (`IncrFullFirstSinceLog` etc.). `incr R0=0` with `config=120` does
not say which ring fell back — and level 0 has its own permanent fallback (band
skip) while `bResidLive` forces *all* levels full. Hook J.

---

## 5. A correctness hazard I introduced, and the fix

**Hook D as written is wrong.** `-VoxelRingOrderScan`'s budget stop sets
`bLevelScanClampedThisCall[Level] = true`, which flows into
`bLevelLastScanClamped[Level]`. But the incremental gate only consults that
flag **inside** `if (VoxelStreamAdmission::CutoffClampEnabled() && …)`. With the
cutoff clamp off — its default — a ring-order-stopped scan would be accepted as
an incremental **predecessor**, and the next diff would trust verdicts that
scan never produced for every cell past the stop radius. That is the
silently-lost-chunk failure the backlog exists to prevent.

**Hook D correction.** In the eligibility chain at `:16192`, the clamp branch
must become:

```cpp
			else if ((VoxelStreamAdmission::CutoffClampEnabled() &&
			          (LevelAdmissionCutoffDistSq[Level] < DBL_MAX || bLevelLastScanClamped[Level])) ||
			         (VoxelStreamAdmission::RingOrderScanEnabled() && bLevelLastScanClamped[Level]))
```

i.e. **a partial predecessor disqualifies a diff regardless of which switch
made it partial.** Cleaner still, and the version I recommend: drop the
`CutoffClampEnabled()` qualifier entirely and test `bLevelLastScanClamped[Level]`
on its own — the flag already means "the last scan of this level did not
evaluate its outer annulus", and *that* is the question the gate is asking. The
qualifier only ever risked a future third partial-scan mechanism inheriting the
bug.

Until this lands, **`-VoxelRingOrderScan` and `-VoxelIncrementalAdmission` must
not be combined.**

---

## 6. Hooks

### Hook I — the deleted-work counter needs a window (the one that matters)

Beside `AccumLevelEntryMs` (`:7649`):

```cpp
	int64 AccumLevelFootprints[VoxelCoords::kNumLevels] = {};
```

Accumulate where `ThisFrameRecomputeMs` is folded into `AccumRecomputeMs`
(`:9453`):

```cpp
		for (int32 L = 0; L < VoxelCoords::kNumLevels; ++L)
		{
			AccumLevelFootprints[L] += ThisFrameLevelFootprints[L];
		}
```

Print on the recompute sum line (`:10821`), which is where its denominator
(`entryMs`, `scans`) already lives, and reset it with the rest at `:12633`:

```cpp
	       TEXT("… | entryMs %s | footprints %s"),
	       …,
	       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%lld"), L,
	                                    (long long)AccumLevelFootprints[L]); }));
```

**Why the sum line and not a new one:** the ratio that decides the feature is
footprints ÷ scans, and putting the numerator on a different line from the
denominator is how the incremental line already ended up needing a manual join
across two greps.

### Hook J — per-level fallback causes

Make the four `IncrFull*SinceLog` ints `[VoxelCoords::kNumLevels]`, index them
at `:16152-16199` with `Level`, print per level at `:10902`, reset at `:12699`.
Then the identity is checkable **per ring** instead of only in aggregate.

### Hook D correction

§5 above.

---

## 7. The arm matrix — one leg, and what decides it

Four arms, matched flight legs, same seed/route/sky pins, `-VoxelPerfFlight=line`:

| arm | flags |
|---|---|
| **A control** | the current default stack |
| **B incremental** | A + `-VoxelIncrementalAdmission` |
| **C ring order** | A + `-VoxelRingOrderScan` |
| **D both** | **only after the Hook D correction lands.** Before it, D is unsound, not just uninformative. |

Read with:

```
python tools/incremental-admission-check.py Saved/B.log Saved/A.log --settle=<B>,<A>
python tools/recompute-census.py Saved/A.log Saved/B.log
```

**What decides it, in order, and both directions at each step:**

1. **IDENTITY** `incr + causes == scans`, delta 0. Non-zero means the if/else
   chain broke and **nothing else in the leg may be read**.
2. **TRAFFIC** warm `incr%`. Near 100 = fired. Near 0 = the leg measures the
   switch's backlog-insert overhead only. *The cold portion cannot decide this
   and the tool refuses to use it* — `incr=0` for the first windows is correct,
   and reading five of them as "zero traffic" is a mistake already made here.
3. **DELETION** footprints per scan must **fall**. This is the counter that
   separates deleted from moved, and no timing can substitute. Falling ms/scan
   with flat footprints = the work moved.
4. **COST** ms/scan. Expect ~6.8 → ~3.9 from `final-allon`. A *rise* means the
   backlog inserts cost more than the skipped cells saved — the real risk in
   the admission-limited regime, where `DeferredFootprints` takes ~200k inserts
   per second at peak.
5. **COLD SETTLE, on the matched pair. This is the only number that ships it.**

On (5), the rule from tonight's fine-tier lock is written into the tool: if
ms/scan falls more than 1.10x and settle does **not** improve, it prints

> `** MECHANISM WORKED, THROUGHPUT DID NOT. … a cost removed from a thread that
> was not the binding one. Do not ship on the mechanism. **`

That warning has been **observed firing** (fed a settle pair where the
mechanism improves and settle does not), as has the old-format refusal. Neither
is a gate that can only come out one way.

**Predicted outcome, stated in advance so the leg can falsify it:** B beats A
on ms/scan by ~1.7x and on footprints by ≥4x; entryMs/window falls ~433 → ~250;
recompute falls ~230 ms/window of a ~709 ms total. Whether that reaches cold
settle is **not** predicted — recompute shares the game thread with apply and
dispatch, and at 61-68% occupancy a 230 ms/window saving is spendable but not
automatically spent.

---

## 8. The fourth window divisor — it is 40 log lines

Asked to flag one if I saw it. **The literal string `(5s window)` is hardcoded
in 40 `UE_LOG` format strings in `VoxelWorldSubsystem.cpp`**, while every
flight leg passes `-VoxelPerfLogInterval=2`. Measured from the timestamps of
`Voxel job flow (5s window)` itself:

```
ahead-on      median gap 2.01 s     label says 5.00 s     2.49x
gp-ctl2       median gap 2.01 s                            2.49x
final-allon   median gap 2.01 s                            2.49x
p2-on         median gap 2.01 s                            2.49x
```

It is a **label** rather than a divisor, so no arithmetic inside the engine is
wrong — but it misleads exactly as the divisor did, on the lines everyone reads
and every tool parses. Concretely, from the handoff:

> `candidatesRejected` = 2.4 MILLION per 5 s window = **485,000 candidate
> evaluations/s**

The window was 2 s, so that is **1.2 million evaluations per second** — the
waste is **2.5x larger** than reported, not smaller. Every "per 5 s window"
rate quoted off these legs needs the same correction. (Share-of-total figures —
my census's 61.1% entry / 37.3% exit / 0.06% residual — are unaffected: they
divide by the same window on both sides.)

The fix is one helper: print the measured elapsed seconds instead of a literal,
the way `[gpu-resid]` now prints `win=%.1fs`. Forty call sites, mechanical, and
it belongs to whoever owns that file — **flagged, not touched.**

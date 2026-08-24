# Wave A: recompute on the MOVING segment

2026-08-23. Re-frames `docs/incremental-admission-audit-2026-08-23.md` against
the sharpened gate: **steady, above 100 fps, at 20 m/s. Parked is already
fine.**

Read with `python tools/flying-segment.py Saved/<leg>.log` (new). Phase
boundaries come from the run's own clock (`preflight warmup for N s`,
`flight ended at t=`), never a guessed offset. Every per-tick figure divides by
the tick count inside the phase, so it is window-length independent — which
matters, because the `(5s window)` label is wrong on all of these legs
(measured median gap **2.01 s**).

---

## 1. The flying-vs-parked delta IS recompute

`ahead-on.log`. Same build, same settled world; the only difference is whether
the anchor moves.

| phase | tick rate | streaming tick | **recompute** | entry | exit |
|---|---|---|---|---|---|
| **PARKED** (linger, 59 s) | **112.7/s** | 0.16 ms | **0.06 ms** | 0.04 | 0.03 |
| PREFLIGHT (cold fill, 90 s) | 84.0/s | 2.07 ms | 0.72 ms (35%) | 0.60 | 0.10 |
| **FLYING** (122 s @ 20 m/s) | **44.1/s** | **11.93 ms** | **8.49 ms (71%)** | **4.97** | **3.40** |

**Recompute costs 140x more per tick flying than parked**, and is **71% of the
entire streaming tick** while flying. Parked runs 112.7 ticks/s; flying, 44.1.
That is the delta the gate is about, and a leg-wide average hides it — which is
what I have been quoting until now.

### Sizing the prize honestly

44.1 ticks/s ⇒ a **~22.7 ms mean flying frame**:

```
   streaming tick   11.93 ms   53% of the mean flying frame
     recompute       8.49 ms   37%
       entry scan    4.97 ms   22%
       exit walk     3.40 ms   15%
```

**So: 8.49 ms of a 22.7 ms mean flying frame — 37%.** Not the whole gap, and
not a rounding error. The largest single game-thread item while moving.

**The hitch half, separately** — a mean cannot fail it: 608 hitch frames in
122 s of flight = **5.0/s**, against 0.03/s parked. On a hitch frame recompute
is mean **14.9 ms**, p50 17.1, p95 29.7, max 53.9 — about **45% of a 33.3 ms
hitch frame**.

---

## 2. The cure, measured on the moving segment

`final-allon.log` (warm `incr` 100.0%, identity delta 0) vs `ahead-on`,
**FLYING phase only**:

| | off | on | |
|---|---|---|---|
| **entry scan / tick** | 4.97 ms | **0.90 ms** | **5.5x** ← `-VoxelIncrementalAdmission` |
| **exit walk / tick** | 3.40 ms | **0.93 ms** | **3.7x** ← `-VoxelBucketedExitScan` |
| recompute / tick | 8.49 ms | **1.90 ms** | **4.5x** |
| streaming tick | 11.93 ms | **6.44 ms** | 1.85x |
| **tick rate** | 44.1/s | **81.1/s** | **1.84x** |
| hitches | 5.0/s | **3.3/s** | 1.5x |
| recompute on a hitch frame | 14.9 ms | **7.3 ms** | 2.0x |

**My "1.74x" was leg-wide and understated it badly. On the moving segment the
entry half is 5.5x.**

The split also **un-confounds most of the leg**: entry and exit fall by
different factors and are attacked by different switches —
`-VoxelIncrementalAdmission` cannot touch the exit walk and
`-VoxelBucketedExitScan` cannot touch the entry scan. The third confound,
`-VoxelNearestAdmit`, *adds* a collect-and-sort to the entry path, so 5.5x is
if anything conservative.

### Necessary, not sufficient — said before anyone reads it as a win

81.1 ticks/s is a **~12.3 ms frame ≈ 81 fps**, still under the 100 fps gate,
and hitches fall only 5.0 → 3.3/s. **Both halves of the gate still fail.** This
removes the largest item; it does not close the gap alone.

### The first question, asked of this arm

*Does it worsen the moving p95?* **No.** Every moving-segment number moves the
right way: tick rate up 1.84x, hitches down 1.5x, recompute-on-a-hitch down
2.0x. There is no cold-start-for-flying trade here.

---

## 3. Hook D — the fix, final text

The hazard: ring order's budget stop sets `bLevelScanClampedThisCall[Level]`,
but the incremental gate reads that flag only inside a `CutoffClampEnabled()`
qualifier, and the clamp is **off by default** — so a ring-order-stopped scan
would be accepted as an incremental **predecessor**, and the next diff would
trust verdicts it never produced past the stop radius. **Silent wrong terrain.**

Ship the version that drops the qualifier. At `VoxelWorldSubsystem.cpp:16192`,
replace:

```cpp
			else if (VoxelStreamAdmission::CutoffClampEnabled() &&
			         (LevelAdmissionCutoffDistSq[Level] < DBL_MAX || bLevelLastScanClamped[Level]))
```

with:

```cpp
			// A PARTIAL PREDECESSOR DISQUALIFIES A DIFF, whichever switch made
			// it partial. bLevelLastScanClamped means exactly "the last scan of
			// this level did not evaluate its outer annulus", which is the
			// question this branch asks -- so it is tested on its own. It used
			// to be tested only under CutoffClampEnabled(), correct while the
			// clamp was the only mechanism that could stop a scan early.
			// -VoxelRingOrderScan's budget stop is a second one, and with the
			// clamp off (its default) the flag would have been ignored and a
			// stopped scan accepted as a predecessor: the next diff would trust
			// verdicts past the stop radius that no scan ever produced --
			// silently-lost chunks, which is what DeferredFootprints exists to
			// make impossible. A third mechanism now inherits the fix.
			else if (bLevelLastScanClamped[Level] ||
			         (VoxelStreamAdmission::CutoffClampEnabled() &&
			          LevelAdmissionCutoffDistSq[Level] < DBL_MAX))
```

Until this lands, `-VoxelRingOrderScan` and `-VoxelIncrementalAdmission` must
not be combined. After it lands they compose: ring order shrinks the cells a
full sweep visits, incremental removes most full sweeps, and a stopped scan
correctly forces its successor full.

**Failing reading, both ways:** with the fix in and both switches on,
`IncrFullConfigSinceLog` must be **non-zero** (stopped scans are forcing full
successors) and must **not** equal the scan count (or ring order has disabled
incremental entirely). `config=0` with `ringStop>0` means the flag never
reaches the gate — the bug, unfixed.

---

## 4. Hook I — the window accumulator (mandatory before the leg)

`ThisFrameLevelFootprints[L]` prints only on the hitch line above 33.3 ms, so
its **sample rate falls as the feature works** and its samples are drawn from
the frames most likely to be full sweeps. Without a window accumulator the leg
cannot show deletion.

Beside `AccumLevelEntryMs` (`:7649`):

```cpp
	// The DELETED-WORK proof. ThisFrameLevelFootprints is per frame and prints
	// only on the hitch line (:9638), i.e. only above 33.3 ms -- so a clean run
	// shows it not at all, its samples are biased toward the full-sweep frames
	// that hitch, and ITS SAMPLE RATE FALLS AS THE FEATURE WORKS. A window sum
	// removes all three. It goes next to its own denominator (entryMs, scans)
	// on the recompute sum line: a numerator on a different line from its
	// denominator is how the incremental line came to need a manual join
	// across two greps.
	int64 AccumLevelFootprints[VoxelCoords::kNumLevels] = {};
```

Accumulate where `ThisFrameRecomputeMs` folds into `AccumRecomputeMs` (`:9453`):

```cpp
		for (int32 L = 0; L < VoxelCoords::kNumLevels; ++L)
		{
			AccumLevelFootprints[L] += ThisFrameLevelFootprints[L];
		}
```

Print on the recompute sum line (`:10821`); reset with the rest at `:12633`:

```cpp
	       TEXT("... | entryMs %s | footprints %s"),
	       ...,
	       *JoinPerLevel([&](int32 L) { return FString::Printf(TEXT("R%d=%lld"), L,
	                                    (long long)AccumLevelFootprints[L]); }));
```

**Failing readings:** `footprints=0` with `entryMs>0` ⇒ the counter is outside
the loop it describes. `footprints` **flat** while `ms/scan` falls ⇒ the work
was **moved, not deleted** — the reading no timing can produce, and the reason
this hook is mandatory rather than nice.

---

## 5. The deeper term: what attacks 940 ns/cell — and why not yet

The fit says entry is a fixed cost per scan that does not track candidates
rejected, so the cost is the per-cell body: one Z-range memo lookup, then a Z
loop doing `ChunkRecords.Find` and `ParkedGeometry.Find` per Z cell. At a
typical Z range of ~10 that is 10-20 hash probes per footprint against a
271k-entry map — cache misses at ~45 ns each. 940 ns/cell is exactly that
shape.

**The mechanism: a per-footprint residency bitmask.** Store, per
`(Level, X, Y)`, a base Z plus a 64-bit mask of which Z chunks are resident,
maintained at the same sites that mutate `ChunkRecords` — the feedback pattern
already working in `FVoxelResidencyGpu`'s shadow mirror, with the same
discipline: **the mask is a mirror, never an authority**; a miss or an
out-of-range Z falls through to the map and is counted. The Z loop then costs
**one** probe per footprint instead of ten to twenty. Predicted 940 → ~200-300
ns/cell.

**It should not be built yet, and that is the recommendation.** Incremental
admission takes entry from 4.97 to 0.90 ms/tick by removing ~80% of the cells
outright. A 3-4x on the per-cell body applied to what remains is worth
**~0.6 ms of a 6.44 ms streaming tick** — smaller than several items not yet
examined, and inside the noise of a matched pair. The order that pays:

1. land `-VoxelIncrementalAdmission` with Hook I, on a matched flying pair;
2. **re-measure the residual entry cost on the moving segment**;
3. build the mask only if what is left is still worth 3-4x.

Building it first would optimise a term the cheaper fix has already mostly
deleted — the same mistake as reading a leg-wide average when the gate is about
the moving segment.

---

## 6. The arm matrix, re-pointed at the gate

| arm | flags |
|---|---|
| **A control** | current default stack |
| **B incremental** | A + `-VoxelIncrementalAdmission` |
| **C exit** | A + `-VoxelBucketedExitScan` |
| **D both** | A + B + C |

`-VoxelRingOrderScan` is **not** in this matrix. At honest admission density it
is worth 1.25-1.63x on the entry half — 4.97 → ~3.3 ms/tick, against
incremental's 4.97 → 0.90. It is the smaller lever on the same term and cannot
combine until Hook D lands. Measure B first.

Read every arm in this order:

```
python tools/flying-segment.py              Saved/<arm>.log        # FLYING only
python tools/incremental-admission-check.py Saved/B.log Saved/A.log --settle=<b>,<a>
python tools/recompute-census.py            Saved/<arm>.log
```

**Both halves of the gate, both directions, at every arm:**

| | pass | fail |
|---|---|---|
| steady | hitches/s **down**, recompute-on-a-hitch down | hitches/s flat or up — a stutter is a stutter whatever p95 says |
| fps | flying tick rate **up**, ms/tick down | tick rate flat while ms/scan falls ⇒ the cost came off a thread that was not binding (the fine-tier-lock shape; `incremental-admission-check.py` prints this and it has been observed firing) |
| deletion | `footprints` **falls** with `ms/scan` | `footprints` flat ⇒ moved, not deleted |
| identity | `incr + causes == scans`, delta 0 | non-zero ⇒ read nothing else from the leg |

**Predicted, so the leg can falsify it:** B takes the flying streaming tick
from 11.93 to ~7.0 ms/tick and the flying tick rate from ~44 to ~70/s; hitches
fall but do not reach zero; **the 100 fps gate is not met by B alone.**

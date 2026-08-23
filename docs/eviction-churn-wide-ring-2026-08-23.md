# Wide-ring eviction: what was measured, what it was read as, and the three arms that separate it

2026-08-23. Ground: the exit scan's radii, the hysteresis band, the brick pool's
eviction trigger. Authored, not built, not measured — the build lane runs the arms.

## The reading that started it

| arm | evictions | peak resident0 | pool slots |
|---|---|---|---|
| default rings | **0** | 45,964 | 262,144 |
| `-VoxelRingScale=1.25 -VoxelUnloadRingMult=1.15 -VoxelLeadHorizonSec=3` | **91,422** | 66,649 | 262,144 |

Both numbers come from `Voxel brick lifetime`, i.e. from
`FVoxelBrickPool` — **not** from the ring exit scan. The per-ring
`Voxel evictions/level` line is a different counter with a different cause.

## Correction 1: `resident0` is one of seven buckets

`GetResidentChunkCountAtLevel(0)` returns `LevelChunkCounts[0]`. The pool's
occupancy is `Resident.Num()` over all `kNumLevels = 7`. Comparing 66,649
against 262,144 and concluding "~25% of the pool" is off by the level count.

Against the settled 4 km per-level census recorded in the source
(`26206/14842/14662/13389/13383/14332 = 96,814`, level 0 = 27.1%, so
total ≈ 3.69 × resident0):

| arm | resident0 | extrapolated total | of 262,144 |
|---|---|---|---|
| default | 45,964 | ~169,700 | **65%** |
| wide | 66,649 | ~246,200 | **94%** |

That is exactly the shape of the data: one arm comfortably under the cap and
evicting **zero**, the other at the cap and evicting on nearly every admission.
It also explains the default arm's literal 0 — the flight never fills the pool,
so the pool never evicts. **Capacity was never measured, only assumed away.**

`residentAll=`, `perLevel=` and the three arena fill percentages are now on the
`Voxel brick lifetime` line so the division cannot be done by hand again.

## Correction 2: raising the occ arena tested one arena, not capacity

`AllocateForChunk` evicts when **any** of desc / occ / mat refuses. Occ
192 → 288 MiB moving evictions 92,012 → 91,422 (0.6%) says occ was not the
binding arena. It says nothing about slots or mat. The owner has already hit
eviction at 59.5% *slot* occupancy for exactly this reason.

## The lead-horizon floor is arithmetically excluded

`-VoxelLeadHorizonSec` was the prime suspect. It cannot be the cause:

- The floor is `min(SmoothedAnchorSpeed × LeadSec, AdmitOuterUU(Level))`,
  clamped per ring in `TruncatePendingJobQueue`.
- It changes only the **queue cutoff** (`DropFarthestOverCap`). It never widens
  the admit radius.
- Admission's outer radius is `AdmitOuterUU = Outer + max(halfDiag, overlap×edge)`,
  which at `Outer/edge = 40×scale` is `Outer × 1.014` at scale 1.25 and never
  exceeds `Outer × 1.10` even at the maximum overlap of 4.
- Eviction is at `Outer × mult ≥ Outer × 1.05` (clamp floor).

So the floor cannot hold a chunk the exit scan wants gone — it cannot put a
chunk outside `AdmitOuterUU` into the desired set at all. The admit and evict
radii are also evaluated against the **same** `Anchor` in the same call (the
speculative path is the only user of `PredictedAnchorLocation`), so the
recorded two-anchor failure is not present either.

## Correction 3: the multiplier is the wrong parameter

`Outer[L]/ChunkEdge[L] == 40` at every level, so multiplier `m` **is** a band of
`40(m−1)` chunk edges — and `-VoxelRingScale` multiplies `Outer`, so it
multiplies the band:

| arm | Outer/edge | band (chunk edges) | span / 120 |
|---|---|---|---|
| default, scale 1.00 m 1.25 | 40 | 10.0 | 101 |
| wide, scale 1.25 m 1.15 | 50 | 7.5 | 116 |
| wide, scale 1.25 m 1.25 | 50 | 12.5 | 126 — refused |

Widening the ring 25% widened the hysteresis band from the 6.0 edges `m=1.15`
buys at scale 1 to 7.5. **The jitter the band protects against did not get 25%
bigger.** At 240 m/s and 60 Hz the anchor moves 4 m = 1.25 level-0 chunk edges
per tick; the desired-set rescan gate is a quarter edge. The inner edge already
recorded this lesson ("*A FRACTION OF AN EDGE, never the outer edge's 1.25x
multiplier*"); the outer edge never learned it.

`-VoxelUnloadBandChunks=<f>` makes the band additive. Negative (default) = use
the multiplier = byte-identical control. Against the span budget
(`40×scale + band ≤ 59.5`):

- **Same ring width, cheaper.** scale 1.25 band 4.0 → resident radius 54 chunks
  vs 57.5. `(54/57.5)² = 0.88`, ~12% fewer resident chunks for the identical
  160 m R0. Against ~246k that is **~217k, 83%** — back under the cap.
- **Same span, wider ring.** band 4.0 allows scale 1.3875 = R0 177.6 m vs 160 m,
  at the same 116-of-120 span.
- **More speculative lead.** The joint budget `(kDim−8)·edge − (Outer + keep)`
  goes 40 m → 51.2 m at scale 1.25.

4.0 is a **candidate, not a measured setting**. It leaves 3.29 chunk edges
(10.5 m at L0) between admit and evict.

## The answer to "can ring width and hysteresis both be had"

Yes, at scale ≤ 1.3875, and the arithmetic is now printed at startup
(`Ring span budget:` — resident radius, span, headroom, residency multiple).
Past that it routes to the index, exactly as the earlier note said: kDim 256 is
448 MiB and buys `40×scale + band ≤ 123.5`, i.e. scale ~3 at band 4.

What is **not** purchasable is 3 s of fine terrain at 240 m/s (720 m R0, span
517). That was already recorded and is unchanged by this work.

## The three arms that separate the causes

The three wide-arm switches are already independent latched knobs
(`-VoxelRingScale`, `-VoxelUnloadRingMult`, `-VoxelLeadHorizonSec`); no work was
needed to make them separable. What was missing is a reading that distinguishes
capacity from churn, and arms that move residency in **both** directions.

Run all four against the same seed, spawn, flight and duration.

| # | arm | predicted evictions | predicted residentAll |
|---|---|---|---|
| A | control (default rings) | 0 | ~65% |
| B | `-VoxelRingScale=1.25 -VoxelUnloadRingMult=1.15 -VoxelLeadHorizonSec=3` | ~91k | ~94% |
| C | B, but `-VoxelUnloadRingMult=1.25` | **higher than B** | ~104% (saturated) |
| D | B, but `-VoxelUnloadBandChunks=4` instead of the mult | **near 0** | ~83% |

**C is the discriminator, and it is the one that can prove me wrong.** The
standing hypothesis is that the band at 1.15 is too *narrow*. If that is right,
C's evictions **fall** — widening the band removes the churn. If capacity is
right, C's evictions **rise** — the wider band is more residency. The two
hypotheses predict opposite signs on the same leg.

D is the proposed fix and is only meaningful if C rises.

## Readings, and the failing side of each

- `Voxel brick lifetime`: `residentAll=n/cap`, `perLevel=`, `arenas desc/occ/mat`.
  FAILING: all under 100% while evictions climb → not capacity, look at the exit
  scan. FAILING: `residentAll == resident0` exactly → the level counters are not
  maintained and the instrument is lying.
- `Voxel evictions/level`: `R%d[in= out= vert= stat=]`. `stat=` counts evictions
  at a ring whose **anchor chunk had not moved** since the previous recompute —
  the 11,779-unloads/s standing-still signature, now countable on a moving leg.
  HEALTHY is 0 everywhere. FAILING: `stat=` 0 on a leg otherwise known to churn
  → the anchor chunk is moving every call and this is admission racing a moving
  anchor, a different bug.
- `Ring span budget:` at startup — resident radius in chunks, toroidal span,
  headroom cells, residency relative to the default arm's 50.0 chunks.
- Gate, unchanged: per-ring `loaded`/`pending` healthy, `holes`/`uncovered` not
  regressed, stationary `unloaded/s` flat, time-to-settle not worsened.

## The tripwire that could not fire

The outer admit-vs-evict thrash check ran only under
`if (RingOverlapChunks > 0)`, and overlap defaults to 0 — so it has never been
evaluated on a shipped run, including through the 91,422 evictions. It has three
ways to trip at overlap 0: `-VoxelRingScale` below ~0.7 (the half-diagonal pad is
`0.7071/(40·scale)` of Outer, so at scale 0.25 the admit pad is `Outer×1.0707`
against a multiplier clamped as low as 1.05), `-VoxelRingOuterMeters` moving one
ring, and a band below the admit pad. It is unconditional now.

The **inner** tripwire is intact but inert: `-VoxelHierarchicalCoverage` is
default-on since 2026-08-23, which forces `bInsideInner` false, so there is no
inner band left to churn. Its `in=` column reading 0 on every ring is therefore
expected, not evidence.

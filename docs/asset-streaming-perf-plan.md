# Asset streaming performance plan

2026-08-17, branch `claude/f6-interior-rim-injection`. Analysis and design only —
no source change ships with this document.

**The effect this plan targets:** with assets on, the world loads ~35x slower
than it should and never finishes. Terrain-only streaming settles at 41,069
chunks / 33.4M quads with an empty queue in ~130 s. The same pose with
`-VoxelAssetDir` reached 5,201 chunks after 300 s (~17 chunks/s against the
~600/s plateau), with 96 worker jobs perpetually in flight and ~1,300 pending.
Screenshots show a half-loaded world.

The headline cause already measured this session is **vertical over-admission**
(~9x more chunk layers per footprint than terrain needs). Reading the code adds
three more mechanisms, and they **multiply**: we admit ~9x the chunks, the
brick-level empty test is effectively disabled in every one of them, and every
sampled air voxel in every one of them takes a global lock. One of the four is
also a **correctness bug that deletes tree crowns**, currently masked by the
general collapse.

Terms used throughout:

* **Footprint** — one (X, Y) column of chunks at one LOD level. Level-0
  footprints are 3.2 m squares.
* **Chunk layer** — one 32-voxel-tall chunk in a footprint's vertical stack.
  "Admitting a layer" means one more chunk to generate, mesh and track.
* **The band** — `FFootprintBand` (VoxelFootprintBand.h): two numbers per
  level-0 footprint ("highest solid voxel anywhere", "lowest voxel that can be
  air") that let every other chunk in the stack be proven empty without a
  worker job.
* **Instance** — one resolved asset placement: a specific tree/rock, species
  and seed chosen, standing at a specific anchor voxel.

---

## 1. Measured baseline

| Measurement | Value | Source |
|---|---|---|
| Terrain-only settle | 41,069 chunks / 33.4M quads, empty queue, ~130 s | session A/B, same pose/seed |
| Assets-on after 300 s | 5,201 chunks, 96 jobs in flight, ~1,300 pending, still climbing | session A/B |
| Terrain's real need | mean **1.45** chunk layers per XY footprint (1.53 on synthetic) | vxc_assetprobe shell census |
| Asset-aware bound admits | mean **13.48** layers (real tiles; 13.88 synthetic re-run) | vxc_assetprobe, 63,001 / 62,500 footprints |
| — widening component | 8.4 layers (25 m for 89–90% of footprints, 45 m for ~10%) | census components line |
| — dilation slack component | 6.1–6.7 layers | census components line |
| Layer table (tightened) | L0: 24 m cell, 45 m cap, **15 m radius**, 60‰ · L1: **5 m cell, 25 m cap, 12 m radius, 1000‰** · L2: 2.2 m, 5 m, 6 m, 1000‰ · L3: detail | probe header |
| Placement reality (128×128 m) | L1: 921 sites → **175 instances** (19% survive the veto) · L2: 4058 → 1459 · L0: 3 → 1 · L3 (detail): 8,263 = **84% of all instances** | vxc_assetprobe --banks |
| Bank traffic in UE | 26,462,610 `bankGrid` requests in one capture | docs/asset-render-gap.md |
| Height distribution vs cap | L0 (44 sp): cap 45 m, **p50 5.0 m**, p90 35, p99 45 · L1 (105 sp): cap 25 m, **p50 7.0 m**, p90 20, p99 25; over 10 m: 37% · L2 (24 sp): cap 5 m, p50 1.6, p90 4.3 | forge.manifest.decode on shipped species.vxm (terrain-lattice, seedsBaked>0) |

The distribution row is the owner's stated intent made a number: **most rocks
and trees are short** — the dense layer reserves 25 m of sky for a
median-7 m population, and L0 reserves 45 m for a median-5 m one. Caveat
carried deliberately: these are **species-count** percentiles, not
placement-frequency percentiles — forests are biased toward tall canopy
species by biome pick-weights, so weight by placement before promising a
number from this table.

Already shipped (commit 3c43880): caps tightened to tallest baked occupant
(60→45, 34→25, 7.5→5 m) and the widening made per-footprint via
`assetTopAboveSurfaceMm`. That took 16.49 → 13.48. Everything below is what
remains.

Pricing runs done for this plan (`vxc_assetprobe`, synthetic, ±400 m, seed
20260719 — same tool, relative movements are the finding):

| Configuration | Mean extra layers |
|---|---|
| As exported | 13.88 |
| L1 cap 25 → 10 m (the "split L1" proxy) | 9.65 |
| L1 radius 12 → 4 m | 10.63 |
| L1 r=4 m + cap 10 m + L0 r=8 m combined | 5.26 |
| Exact per-footprint occupancy (proposal C, estimated) | **~1–2** |

---

## 2. Diagnosis: four mechanisms, and how they multiply

### 2.1 The admission bound charges every footprint for a tree that is not there

`ComputeFootprintChunkZRange` (VoxelWorldSubsystem.cpp:7084) widens every
footprint's top by `assetTopAboveSurfaceMm` — a **pure-hash** query that only
knows whether a *lattice site* exists in reach, and if so answers the **layer
cap**. L1 is a 5 m lattice at density 1000‰ with a 12 m reach: a 3.2 m
footprint dilated by 12 m covers ~30 L1 cells, so essentially **every footprint
on the planet** has an L1 site "in reach" and pays 25 m of sky (8.4 layers
mean). But only **19% of L1 sites survive the biome veto and become trees**
(placement census), and the placed trees are one per ~10 m — so >90% of that
reservation is for trees that will never exist, and even where a tree exists,
the cap (25 m) generally exceeds the instance's real height.

The bound is built this way because it must run before `instancesForRect` and
may not pay a column evaluation (the veto-only rule in assetplacement.h). That
constraint is real for the *analytic* bound — but the z-range memo is exactly
the place that CAN afford columns, because it is per-footprint and cached
(`FootprintZRangeCached`). See proposal C.

### 2.2 Dilation slack: where the 6.1 layers actually come from

`assetAwareSurfaceUpperBoundMm` (assetplacement.h:463) prices each layer as
`terrainBound(rect ⊕ layer.maxRadius) + layer.maxHeightMm`. The dilation is
correct in the worst case — a tree anchored uphill stands on ITS ground and
reaches in — but with L1's 12 m radius the terrain bound is evaluated over a
27.2 m square instead of 3.2 m, and `surfaceUpperBoundMm` is a **max over
every tile-raster cell the rect touches plus the detail allowance**. On any
slope that max sits the local relief above the footprint's own ground:
measured mean 21.5 m ≈ 6.7 chunk layers, charged on ~100% of footprints
because L1 is always "in reach". The **ZMin side is NOT inflated** (it derives
from the corner minimum only), and the all-solid floor bound is sound
unmodified (assets fill air only — a chunk with no air gains nothing), so the
sky side is the entire problem.

Two independent reducers: (a) exact per-instance answers (anchor ground +
instance height — no dilation term at all, proposal C); (b) tightening
`maxRadiusMm` to the widest baked occupant, the exact sibling of the height
tightening that shipped — `assetTightenLayerCaps` (assetmanifest.cpp:197)
tightens height only, and the manifest doesn't yet carry per-species reach
(the bake knows it: `validateGrid` in assetbank.cpp computes `reach` from the
box corners at load). Priced: L1 12→4 m alone is −3.25 layers.

**A height split does NOT touch this component.** The 6.1 layers are radius ×
slope, priced on every footprint L1 can reach; re-filing tall species moves
only the 8.4-layer widening half. Anchor-localized admission (proposal C)
reclaims both at once, because the exact answer — "ground at the actual
anchor + that instance's actual height" — has neither a cap term nor a
dilated-rect term.

### 2.3 NEW — the per-voxel bank mutex (why throughput collapsed, not just residency)

`AssetField::materialAt` (assetfield.h:207) is called for **every air voxel
the mesher samples** (~64,000 samples per level-0 chunk: 64 bricks × 10³
brick+apron), and for each terrain-lattice instance in the list it calls
`materialOfInstance` → `banks_->bankGrid(bankId, seedIndex)` —
**per voxel, per instance**. `AssetBankLibrary::bankGrid` (assetbank.cpp:149)
takes a **global `std::mutex`** and does an `unordered_map` find on every
call. An instance-bearing chunk therefore takes on the order of 10⁵ lock
acquisitions, and all 96 workers contend on the same mutex — a classic lock
convoy. That is the shape of the observed failure (96 in flight forever,
~17 chunks/s), and the 26.4M-requests capture confirms the volume. The grids
themselves are immutable after load; the lock protects a lookup whose answer
never changes for a given (instance, chunk).

### 2.4 NEW — the terrain-only band deletes crowns (soundness), and the brick skip is dead (waste)

Two halves of the same blind spot:

* **Band skip, unsound with assets.** The band's `MaxSurfaceTopVoxel` is
  reduced from `ColumnSurfaceTopVoxel` — **terrain only** (VoxelFootprintBand.h:139;
  no asset term anywhere in the file). `BandProvesChunkEmpty`'s all-air half is
  consulted at dispatch (`BuriedSkipEnabled`, **default ON**,
  VoxelWorldSubsystem.cpp:9746) and optionally at admission. Once a footprint's
  band is cached, every crown chunk above the terrain top is "proven" all-air,
  skipped, and settled with zero quads — **the tree's top is deleted**. The
  holes are timing-dependent (crown chunks dispatched before the footprint's
  seeding job drains still mesh correctly), which is why this reads as flaky
  streaming rather than a bounds bug. Note the *analytic* sky skip
  (`IsChunkProvablyAllAir` → asset-aware `FootprintSurfaceUpperBoundMm`) was
  correctly widened — the exact band that runs before it was not.
  `-VoxelVerifyBuriedSkip` with assets on should reproduce this as
  predicted-empty-with-quads violations; run it before and after the fix.

* **SkipBrick widening is blanket — and triggered by ground cover.** The
  per-brick air test adds `AssetTallestVoxSnapshot` (**45 m = 14 chunk
  layers**) to every brick's ceiling whenever the chunk has *any* resolved
  instance (VoxelWorldSubsystem.cpp:10281). Two problems: `AInsts` includes
  **detail-lattice instances** (84% of all instances — L3 ground cover on a
  0.8 m lattice at 1000‰, present nearly everywhere) which can never put a
  voxel in the grid, so the trigger fires in ~every chunk; and the widening is
  the global tallest bake, not this chunk's instances. Net: the air half of
  the brick skip is effectively **disabled wherever assets are on**, so every
  admitted sky chunk meshes all 64 bricks through the composed sampler —
  which is where the mutex of 2.3 gets multiplied.

### 2.5 NEW — redundant resolution up the stack, and wasted detail work

* `instancesForRect` is resolved once per **chunk**, but its input is the
  chunk's XY rect — identical for every chunk in the footprint's Z stack. With
  ~14 admitted layers, the site enumeration and per-candidate-anchor
  `Amp.column` calls are repeated ~14x for identical answers.
* The resolve includes the **detail layers**: ~121 L3 candidate cells per
  chunk rect, each costing an `Amp.column` for anchor facts, producing
  instances that `materialOfInstance` unconditionally answers `MAT_AIR` for.
  ~10% extra column work per job plus a 6x longer per-voxel instance loop,
  for zero composed voxels.
* The 34×34 column grid itself is Z-independent and rebuilt per chunk (known;
  `-VoxelL0GridCacheProbe` measures it). Over-admission multiplies this too:
  9x the layers means 9x the identical grid rebuilds that the band would
  normally suppress — and the cold-band throttle serialises the first job of
  every footprint, so cold fill pays the deep stacks on the critical path.

**The multiplication:** ~9x admitted chunks (2.1/2.2) × dead brick skip in
each (2.4) × ~10⁵ serialized lock acquisitions per meshed chunk (2.3) ×
~14x redundant resolve (2.5). No single fix un-collapses streaming; the two
P0s below unblock throughput, and proposal C removes the over-admission.

### 2.6 Owner directives (2026-08-17), and what they change here

**Surface-only placement.** All assets spawn at or above surface ground level
(ground / lake-bed and up); underground and cave placement is explicitly
forgone for now. What the code already does, checked for this plan:

* Placement is **already surface-only by construction**: anchors come from
  `assetColumnFactsFromSample` (assetfield.h:93), which seats every candidate
  at the surface column's top solid voxel; the `anchorSolid` veto only
  *rejects* cave-mouth columns. There is no underground site enumeration to
  skip — the only per-candidate cost is the one `materialAt` read already
  counted in 2.5.
* Depth pays **nothing on the streaming side today**: the downward bound
  `assetBottomBelowSurfaceMm` (assetplacement.h:368) has **zero callers**;
  `ComputeFootprintChunkZRange`'s ZMin derives from the terrain corner
  minimum only; and the all-solid floor path (`FootprintSolidFloorMmCached` →
  `Amplifier::solidBelowBoundMm`, :7033) is not asset-aware — which is sound,
  because assets fill air only, and a chunk with no air gains nothing. So
  there is no depth reclaim to bank now.
* What surface-only DOES license: shrink `maxDepthMm` (authored 8 m L0 /
  4 m L1 / 2 m L2) toward the shallow anchor-seat depth actually baked.
  The manifest already carries per-species `depthMm` (assetmanifest.h:139),
  so `assetTightenLayerCaps` can tighten depth exactly as it tightens height,
  **today, with no re-export**. Value is contract hygiene and
  future-proofing (any future consumer of the downward bound starts tight),
  not a measurable win — stated so nobody chases it as one. Folded into
  proposal I's hygiene batch.
* Under proposals A/E, the per-instance z-ranges make below-surface bricks
  reject on a box test immediately, so surface-only intent is also enforced
  structurally in the hot path.

**Most rocks and trees are short** (the distribution row in §1). This is
owner licence for **distribution-based admission instead of worst-case
caps**, with a rare-tall handling path. Proposal C is that idea taken to its
limit — per-instance, not per-quantile: short trees reserve short sky, the
one 45 m spruce reserves 45 m over its own footprint only, and no cap needs
choosing at all. A quantile cap alone (e.g. L1 at p90) is **unsound** — the
p99 tree gets its crown truncated, the exact too-tight failure this plan must
never ship — so every quantile-shaped option must pair the lowered cap with
somewhere sound for the tall tail to live (the split, proposal H) or with
per-footprint exactness (C). The distribution data re-prices H below.

---

## 3. Fix set, ranked by (impact × safety) / effort

Safety is stated against the two failure polarities: **too tight** = crowns
truncated / holes (silent, reads as a rendering bug); **too loose** = streaming
starvation (today's state). "Digest-safe" = changes no composed material, so no
worldgen version bump and no golden re-bless.

### A. ACCEPT — hoist bank-grid resolution out of the per-voxel loop (P0, throughput)

**Change:** after `instancesForRect` in the level-0 job
(VoxelWorldSubsystem.cpp:10199), resolve each instance ONCE into a flat array
`{instance, const AssetGrid*, rotated origin, XY box, zMin/zMax}` (new helper
beside `AssetField::materialOfInstance`; grids are immutable so the pointer is
valid for the process lifetime). The `GridSampler` asset branch walks that
array with a cheap box reject before touching the grid — **zero locks, zero
map lookups per voxel**. Also resolve **terrain-lattice layers only** for the
mesher (a `terrainOnly` flag on `instancesForRect` or a UE-side filter):
detail instances compose nothing by construction (assetfield.h:171) and are
84% of the list.

**Invariant:** byte-identical composed materials — same grids, same
deterministic instance order, only the lookup is hoisted. Gate with the
existing `-VoxelL0GridVerify` harness (reference sampler kept on the old
per-voxel path for the A/B). Digest-safe.

**Impact:** removes ~10⁵ mutex acquisitions/chunk under 96-way contention —
the single largest throughput restore available. **Effort:** small.
**Risk:** minimal.

### B. ACCEPT — make the empty-chunk proof asset-aware; fence it until then (P0, correctness)

**Change, interim (one line):** with an asset field installed, disable the
**all-air half** of `BandProvesChunkEmpty`'s consumers (dispatch skip at
:9752, admission skip at :8308). The all-solid half stays — it is sound with
assets (monotone air-fill).

**Change, real:** store the footprint's asset top beside the band (the value
is already computed per footprint in `ComputeFootprintChunkZRange`; stash it
in `FFootprintZRange` / next to `FootprintBandCache`) and test
`InteriorZMin > MaxSurfaceTopVoxel + AssetTopVox(X,Y)`. With proposal C, that
asset top becomes the **exact crown top**, and the band once again proves the
(now few) genuinely-empty sky chunks without a job — this is the "empty-chunk
fast path" the task asks about, and it is cheap because it reuses the memo.

**Invariant:** the skip may only claim less emptiness than the truth
(VoxelFootprintBand.h's own polarity rule). Verify with
`-VoxelVerifyBuriedSkip` on an assets-on leg: 0 violations required.
Digest-safe. **Impact:** stops crown deletion; later re-arms the biggest
job-avoidance lever. **Effort:** trivial (fence) + small (real fix).

### C. ACCEPT — exact per-footprint occupancy admission (P1, the 9x killer)

**Change:** in `ComputeFootprintChunkZRange` (levels 0–1), replace the
cap-based widening with the **exact crown top of the instances that actually
stand there**: resolve `instancesForRect` (terrain layers only) for the
footprint rect, take max over instances of `anchorVz + grid extent above
anchor`, in voxels; 0 where no instance resolves. Memoized in
`FootprintZRangeCache` exactly as today. Levels 2–4 keep the cap-based
widening (a level-2 chunk is 12.8 m tall; the cap costs ≤2 layers there and
the exact query over a 128 m rect would be expensive).

**Why this is sound where the cap was needed:** the veto-only rule exists so
the *pure-hash* bound never needs to know what the policy decided. Admission
is allowed to know — it just wasn't allowed to pay for columns. The z-range
memo already pays 4 `Amp.column` calls per footprint and is cached; the exact
resolve adds ~40–90 columns per footprint (L1 ~36 cells + L2 ~49 in reach,
L3 skipped) **once**, and proposal D makes even that shared with the worker.
The resolver is the same deterministic code compose uses, over the same
amplifier, so the admitted top is the compose-time truth by construction —
`>=` every voxel any instance can place (`validateGrid` pins grid extents at
load). Too-tight is structurally excluded; too-loose is today's behaviour.

**Guard rails:** (1) a `-VoxelAssetExactAdmit=0` switch restoring the cap
rule; (2) a verify mode asserting exactTop ≤ capTop per footprint (the cap is
an upper bound on the exact answer — any violation is a wiring fault); (3)
the existing edit hatches (`EditedFootprintMaxZ`) are untouched. Digest-safe
(admission only).

**Impact:** mean extra layers 13.5 → ~1–2 (crowns only where trees stand:
~175 canopy instances per 1,600 footprints, rocks ≤5 m). Desired set lands
within ~1.2–1.5x terrain-only. This **dominates the L1 split** (see H) and
most of the dilation slack (the exact answer has no dilation term).
**Effort:** medium. **Risk:** low with the guards; the one real hazard is
divergence between admission-time and compose-time resolution, and they are
the same function over the same immutable tables.

### D. ACCEPT — per-footprint instance cache shared across the Z stack and with admission (P1)

**Change:** `FIntPoint footprint → shared_ptr<const FResolvedInstances>`
(instances + grid pointers + boxes + exact top), produced once by whoever asks
first (admission under C, or the first level-0 job), passed into worker tasks
by value in the dispatch capture (the established `AssetTallestVoxSnapshot`
pattern — game thread reads, worker never touches Impl state). Pruned with
`FootprintZRangeCache` on the same trigger (`PruneFootprintZRangeCache`).

**Invariant:** the cached set is a pure function of (footprint, seed, tables),
all immutable after install — same correctness argument as the z-range memo.
**Impact:** removes the ~14x redundant resolve of 2.5 and makes proposal C's
marginal cost ~zero; with A, a worker job's asset cost becomes "walk a small
prebuilt array". **Effort:** small–medium once A exists.

### E. ACCEPT — per-instance SkipBrick (P1)

**Change:** replace the blanket `MaxTopInterior += AssetTallestVoxSnapshot`
(:10283) with a test against the resolved instance boxes A/D already built:
widen only bricks whose XY range intersects an instance's box, by **that
instance's own top**; never widen for detail instances. The unused
`AssetTallestVoxSnapshot` plumbing retires.

**Invariant:** widening derives from exact per-instance extents (load-time
validated); too-tight impossible, too-loose gone. Digest-safe. **Impact:**
re-arms the brick air-skip everywhere crowns aren't — the admitted sky chunks
that remain (real crown chunks) stop meshing 64 bricks of nothing.
**Effort:** small.

### F. ACCEPT — surface-shell-first dispatch bias (P2, polish)

**Where order is decided today:** admission pushes
`FSortEntry{DistSq3D, key}` into `PendingJobKeysByLevel[level]`
(VoxelWorldSubsystem.cpp:8253); `SortPendingQueues` re-sorts per level by 3D
chunk-centre distance each recompute; the pop loop applies per-ring quotas and
floors (`-VoxelRingQuota`). So "near-to-far within each ring" already exists.

**Change:** add a sort-key penalty (equivalent to +50–100 m) for chunks whose
`Cz` lies above the footprint's terrain-top chunk (stash `SurfaceTopChunkZ`
in `FFootprintZRange` — the corner max is already computed). The walkable
shell streams first; crown chunks fill in behind. This is the cheap form of
"two-phase admission": same records, same set, ordering only — both failure
polarities are impossible by construction (nothing is dropped or delayed
indefinitely; the queues drain in full).

**Impact:** after C the sky population is small, so this buys cold-fill and
motion *feel* (ground under the player first), not settle time. **Effort:**
small. A literal two-phase scheduler (separate background queue) is REJECTED
as redundant with this bias plus the existing per-ring quotas.

### G. ACCEPT (measure-first) — admission-side band skip default-on (P2)

After B's real fix, flip `voxel.Stream.AdmissionBandSkip` to 1 (mode 2
measure-only first, as its own comment prescribes): proven-empty chunks never
become records, freeing R0's admission budget and queue slots for chunks with
geometry. The machinery, counters (`BandAdmitWarm/Cold`,
`BandSkippedAtAdmission`) and edit vetoes already exist. Digest-safe; the A/B
is one cvar.

### H. CONDITIONAL — split L1 by height (owner-signaled; decide after C ships)

The owner's "most are short" intent points here, and the distribution sizes
it concretely: L1 at a 10 m cap keeps 63% of species on the dense lattice
(p50 is 7 m), and the 37% over 10 m move to a sparser tall lattice whose
sites are localized — the 25 m widening then lands only near actual tall
trees. Priced with `--l1cap 10000` as the proxy: 13.88 → 9.65 (−4.2 layers).

Why it is still not first in line: **C achieves the same end per-instance
(−12 layers including the dilation half, which a split does not touch) with
no re-export, no re-bake, and no visual change.** After C, the split's
residual value is confined to where the cap still rules — the pure-hash
analytic bound at levels 2–4, where 25 m costs ≤2 level-2 layers — so its
expected post-C win is small. Costs if taken: re-file species
(`assign_layer`, forge/manifest.py:345), re-export species.vxm, **placement
moves** (different lattice ⇒ different world), kWorldGenVersion bump,
digest/golden re-bless, owner approval of the visual change. Carrying the
caveat from §1: size any final split on **placement-weighted** heights (biome
pick-weights favour tall canopy species), not species counts.

**Decision point:** after step 5's measurements and a placement-weighted
distribution. If C lands its residency/settle targets, H stays shelved; if
the per-footprint resolve proves unaffordable anywhere, H is the fallback the
owner has already signaled for.

### I. DEFER — tighten layer radii (and depth caps) to the widest/deepest baked occupant

The sibling of the shipped height tightening, two parts with different costs:

* **Radii** — add per-species `reachMm` to the manifest (the exporter knows
  every bake box; `validateGrid` already computes reach at load), extend
  `assetTightenLayerCaps` to radii. Priced: L1 12→4 m is −3.25 layers, and it
  shrinks *every* analytic bound query (levels 2–4 keep using the analytic
  bound under C, so this still helps the far rings and the sky-trim). Needs a
  manifest format field + re-export of species.vxm (no bank re-bake), which
  is a worldgen-version event. **Do it with the next scheduled re-export.**
* **Depth caps** — per the surface-only directive (§2.6): the manifest
  already carries per-species `depthMm`, so `assetTightenLayerCaps` can
  tighten `maxDepthMm` (8/4/2 m authored) to the deepest baked seat **now,
  with no re-export**. Measured reclaim today: zero (`assetBottomBelowSurfaceMm`
  has no callers). Ship it as hygiene alongside the radius work so any future
  consumer of the downward bound starts tight.

---

## 4. Benchmarking and metrics plan

The owner's ask: know where the per-frame budget and streaming cost go, split
by ground terrain / trees / rocks / animals / water / sky / other.

### 4.1 Streaming-side attribution (per-job, per-level)

Extend `VoxelStreaming::FJobResult` (VoxelWorldSubsystem.cpp:1310) — it
already carries `JobMs`/`GridMs`/`JobCycles`/`GridCycles` and the brick-skip
census, so the pattern is established:

* `float AssetResolveMs` — wall time in `instancesForRect` (+ grid-pointer
  resolution under A).
* `uint16 AssetInstancesTerrain, AssetInstancesDetail` — resolved counts.
* `uint32 AssetGridReads` — accumulated locally in the sampler lambda (plain
  int, no atomics on the hot path), pushed once per job.
* `bool bHadAssetVoxels` — any non-air asset compose in this chunk.

Mesh time is then attributable as `JobMs − GridMs − AssetResolveMs`. Drain
these into per-level windows exactly as `LevelWorkerJobMsWindow` does, and add
to the 5 s `"Voxel streaming:"` line (:5554):
`assetJobs= assetResolveMsP50= assetGridReads= wasteL0=`.

**The waste ratio directly:** per level, admitted-vs-nonempty. The tallies
already exist on one side (`LevelJobsDispatchedTotal`, `LevelRecordsAdded`,
`LevelSkySkippedTotal`, `BuriedSkipsByLevel`); add
`LevelZeroQuadResultsTotal[level]` and `LevelAssetChunksTotal[level]`
(QuadCount()>0 / bHadAssetVoxels at DrainResults). Waste = zero-quad results ÷
dispatched, reported per level in the same log line.

`vxc::Counters` (voxel-core/include/voxelcore/counters.h) gains
`assetInstanceResolves` and `assetVoxelsComposed`, following the existing
relaxed-atomic pattern; `FCountingBankSource` (:2956) already gives the
per-species histogram and stays as-is.

### 4.2 Frame-side attribution

What exists: `STATGROUP_VoxelEarth` (VoxelDebug.h:36–39 —
`STAT_VoxelSubsystemTick`, `STAT_VoxelWorkerJob`, `STAT_VoxelGameThreadMesh`,
`STAT_VoxelEditApply`) plus per-subsystem `STATGROUP_Tickables` rows for
water, sky, weather, fluids, agents (wildlife), GI.

The one thing a stat scope CANNOT give: trees/rocks vs ground **within a
chunk mesh** — they are one merged buffer per chunk. Attribute by **resident
quads per bucket** instead: at `ApplyMeshResult`, classify each quad's
`MaterialId` into {ground, tree, rock, water-mark, other} (asset material ids
are known from the banks/manifest) and keep `ResidentQuadsByBucket[]` beside
`ResidentQuads`. The render-thread is the bound resource at 2K
(docs: game thread ~75% idle), and its cost tracks submitted quads — so
quads-by-bucket is the honest proxy for "what trees cost the frame", with the
standing caveat from the draw-path memo that removed work does not
automatically become frame time. Cross-check with `stat gpu` / `ProfileGPU`
captures on the standard leg (base pass + shadow depths are where voxel quads
land).

New stat scopes worth adding (cheap, coarse): `STAT_VoxelAssetResolve` around
the game-thread/admission resolve under C–D. Do NOT put a scope in the
per-voxel sampler; that cost is carried by the per-job numbers above.

Animals are actors, not voxels: their streaming cost is zero here and their
frame cost is already visible as `UVoxelAgentSubsystem` tick +
primitive/instance counts; report them as their own row from existing stats.

### 4.3 The standard leg, and headline metrics

Use `tools/voxel-run-leg.ps1` (cold-fill driver) unchanged — it already
encodes the settle rule ("loaded unchanged for N samples AND nothing in
flight AND nothing pending" — a lull is not a finish line), the sky pins, and
the `-ExecCmds` quoting trap. One benchmark = the pair, same pose/seed:

1. terrain-only (no `-VoxelAssetDir`)
2. assets-on

plus a 60 s flight leg (`UVoxelPerfRunSubsystem` self-timed) for motion
numbers. Headline metrics, one line each, targets chosen so today's state
fails all of them and a healthy build passes without tuning:

| Metric | Definition | Target |
|---|---|---|
| Settle ratio | assets-on time-to-settle ÷ terrain-only | **≤ 1.5x** (today: DNF at 300 s / 130 s) |
| Residency ratio | assets-on settled chunks ÷ terrain-only (41,069) | **≤ 1.3x** |
| L0 waste | zero-quad level-0 results ÷ level-0 dispatches, settled window | **≤ 25%** |
| Job cost ratio | assets-on level-0 JobMs p50 ÷ terrain-only | **≤ 1.3x** |
| Soundness | `-VoxelVerifyBuriedSkip` + sky verify violations, assets on | **0** |
| Crown integrity | A/B captures of a known tall-tree site (owner judges) | no missing tops |

The residency ratio is the honest price of trees: crown chunks are real
geometry and SHOULD cost something — the target says "pay for the trees that
exist, not the trees that could have existed".

---

## 5. Recommended execution order

| Step | What | Needs owner? | Gate before merge |
|---|---|---|---|
| 1 | **B-interim**: fence the band's all-air half when assets installed | No | `-VoxelVerifyBuriedSkip` clean; crown A/B capture |
| 2 | **A**: grid-pointer hoist + terrain-only resolve filter | No | `-VoxelL0GridVerify` byte-identical; job-cost ratio |
| 3 | **Metrics wave** (4.1 + quads-by-bucket + leg pair baseline) | No | one settled pair logged with all headline lines |
| 4 | **D**: per-footprint instance cache | No | resolve counters drop ~14x; digest unchanged |
| 5 | **C**: exact occupancy admission (L0–L1) behind `-VoxelAssetExactAdmit` | No | verify exactTop ≤ capTop; residency + settle ratios; crown A/B |
| 6 | **B-real**: asset-aware band; **E**: per-instance SkipBrick | No | verify modes clean; L0 waste target |
| 7 | **F** dispatch bias; **G** admission band skip (mode 2 → 1) | No | A/B on the leg pair |
| 8 | **I**: manifest `reachMm` + radius tightening (+ depth-cap tightening, no re-export needed) | **Yes** for the re-export half | next scheduled export |
| — | **H**: L1 height split (owner-signaled) | **Yes** (re-file + re-export + visual change) | decision point after step 5, on placement-weighted heights |

Everything in steps 1–7 is admission/scheduling/lookup work that moves no
composed voxel: digest-safe, no golden re-bless, no owner sign-off. The two
deferred items are the only worldgen-version events, and neither is on the
critical path.

Expected end state: assets-on settles within ~1.5x of terrain-only at the same
pose, worker jobs drain to zero, trees keep their crowns near and far, and the
log answers "what did trees cost this frame" in one line.

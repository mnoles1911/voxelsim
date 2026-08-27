# The marcher height-bound pyramid — soundness, cost, and the go/no-go number

Date: 2026-08-27. Read-only investigation; no repo file was modified other than this one.

Labels: **[V]** verified from code, data, or a probe run in this session, with file:line or the
command; **[I]** inferred, with the reasoning shown; **[C]** cited from a paper held locally.

> **Revision note.** An earlier draft of this document returned NO-GO. That verdict was wrong,
> and it was wrong for one specific reason: I priced the scheme at `-84480,53760`, the
> *abandoned* spawn, instead of `-61440,-61440`, the spawn every current leg uses. I have since
> read the pose-split legs' own command lines — `Saved/ZZ-pitch-90.log`, `ZZ-pitch0.log`,
> `ZZ-pitch-20.log`, `ZZ-final.log` all carry `VoxelSpawnAt=-61440,-61440` **[V]** — and
> re-measured there. **The verdict inverts to GO, decisively.** What survives from the earlier
> draft is flagged in place; what is retracted is in §1.1.

---

## 0. Verdict

1. **The hypothesis is sound**, and at the real site it is worth a great deal. §2.

2. **GO on the pyramid.** At `-61440,-61440` the terrain within the march reach spans
   **1,485.6 m to 2,856.4 m** — `R ≈ 1,371 m` of relief, measured across six transects **[V]**.
   Against a leaf slack of **3.84 m** on the fine tier, `R/S ≈ 357`. The threshold this document
   proposes is `R > 4S`. It clears it by two orders of magnitude.

3. **And a ceiling is NOT a substitute here — which is exactly your original point, now with
   numbers.** A ceiling must take the *maximum* over the whole reach, so it is pinned at
   ~2,860 m by one massif. Local `H` ranges from 1,489 m to 2,860 m. **The pyramid recovers up
   to 1,371 m of vertical bound that a single number is forced to throw away.** The camera in
   the pose-split legs sits ~667 m *below* the ceiling but up to ~703 m *above* local `H` in the
   low directions. A ceiling barely fires; a heightfield terminates those rays outright. §4.

4. **Your load-bearing claim — build the finest level and MAX-REDUCE UPWARD — is CONFIRMED, now
   on real fine tiles at the real site.** A direct level-5 (102.4 m) query returns a bound
   **188.8 m** above the surface; a 3.2 m leaf returns **3.84 m**, and max-reduction carries the
   leaf's tightness upward. **49x.** §3.

5. **The 72-corner refusal is not the reason, and is fine-tier-only.** The real obstacle to a
   direct coarse query is Lipschitz slack. §3.1.

---

## 1. Site: what is true, and what I retract

### 1.1 Retracted

The earlier draft's headline — "the measurement site has no tiles" — was **attached to the wrong
spawn** and its conclusions do not transfer. Specifically retracted:

- That the pose split (1.108 / 4.45 / 5.638 ms) was measured over featureless ground. **It was
  not.** Those legs ran at `-61440,-61440` **[V, their own command lines]**, which is inside
  coverage, at 2,187.6 m, in alpine terrain.
- The NO-GO verdict and the R/S table that supported it.

### 1.2 What stands, correctly scoped

`-84480,53760` **is** outside the baked tile set, and this is worth keeping as a live
demonstration of the absence hazard rather than as a finding about current legs:

- The coarse set is 289 tiles spanning x,y ∈ [-16, 0] at a 15,360 m pitch
  (`tools/drainage-ladder.ps1:68`), i.e. y ∈ [-245,760, +15,360) m **[V]**. No `.vxtl` anywhere
  under `D:/voxelsim/tile-cache` has a positive y index **[V]**. `y = +53,760` is 38.4 km
  outside.
- The edge is pinned empirically at x = -40,000 m **[V]**: real terrain at y = 14,000
  (1,473–1,489 m), sea level at y = 16,000 — exactly where 15,360 m predicts.
- A control at (500,000, 500,000) returns `surface range: -0.14 .. 0.26 m` **[V]** — **the
  absence fallback is sea level**, reproduced live. This is `tilestore.cpp:1759` observed rather
  than argued, and it is the hazard §6 is about.
- Probed there, the world looks like a plausible flat plain: 0.11 m of relief over 4 km, local
  grade 1.4% **[V]**. That is the trap. It is not a plain; it is missing data drawn as one.

**The actionable residue:** any historical number taken at `-84480,53760` was measured over the
sea-level fallback and should not be compared against a current leg. `-61440,-61440` is inside
both the coarse set (tile `-4_-4`) **and** the fine set — `-4_-4.vxtl` is one of the 15 tiles in
`.../s16/` **[V]** — which is presumably why the spawn moved.

### 1.3 The real site

`vxc_terrainprobe` at `-61440,-61440`, coarse tier, 4 km transect **[V]**:

```
surface range: 2193.93 m .. 1942.34 m
S(120 m) = 39.07 m,  S/d = 0.326
local grade 53.2%
```

Six transects within the march reach **[V]**:

| site | surface range | R along transect |
|---|---|---|
| `-61440,-61440` (spawn) | 2187.6 .. 1937.4 m | 250.2 m |
| `-65440,-61440` | 2473.5 .. 2194.0 m | 279.6 m |
| `-61440,-65440` | 1929.8 .. 1485.6 m | 444.2 m |
| `-57440,-57440` | 2856.4 .. 2185.5 m | 670.9 m |
| `-61440,-57440` | 2483.1 .. 2856.3 m | 373.2 m |
| `-60000,-60000` | 2733.7 .. 2155.4 m | 578.3 m |

**Envelope across all six: 1,485.6 m to 2,856.4 m, `R ≈ 1,371 m`.** These are single +x
transects, so this is a **lower** bound on the true 2D relief over the ±4,198 m box.

Camera heights, both verified: the pose-split legs carry **no** `-VoxelSpawnAltM`, so they use
the default +5 m ground spawn (`VoxelEarthGameMode.cpp:4380-4410`) **[V]** — camera at
~2,192.6 m. The later capture legs use `SpawnAltM=220` **[V]** — camera at 2,407.6 m, matching
the coordinator's reading of ground top 2,187.6 m, which agrees with my transect's spawn column
(2,187.58 m) to 2 cm **[V]**.

---

## 2. Is the hypothesis sound?

### 2.1 Where the upper bound is sound

The amplifier's contract, `amplifier.h:420-455` **[V]**:

> A PROVABLE UPPER BOUND on the DISPLACED surface over every column in the inclusive
> voxel-index rectangle ... `surfaceUpperBoundMm(...) >= surfaceMm(vx, vy) + D(vx, vy, z)`

and the soundness argument, `amplifier.h:433-441` **[V]**:

> materialAt is unconditionally MAT_AIR above the surface (stratigraphyAt's `depthMm < 0` test),
> and the cave and cavern passes only ever CARVE — no pass in the amplifier can turn air into
> solid.

That is why **caves, caverns, karst and overhangs are non-issues for an UPPER bound**: they all
remove solid. Only something that *adds* solid above the surface can break it.

| Thing above the surface | Breaks it? | Evidence |
|---|---|---|
| Caves / caverns | **No** — carve only | `amplifier.h:437-439` **[V]** |
| Karst conduits | **No** — carve only; the table is empty in every shipping config | `amplifier.h:678-681` **[V]** |
| 3D density band | **No** — removed at worldgen v20; the bound's +350 mm came off with it | `amplifier.h:426-430`, `amplifier.cpp:2085-2096` **[V]** |
| Asset crowns | **No** — already composed in, per layer | §5 **[V]** |
| `MAT_WATERMARK` (debug) | Would, and is already paid for by a constant widening | `amplifier.cpp:2103-2122` **[V]** |
| **Player edits** | **YES** | §6.3 — the one live hole **[I]** |

The bound **declines rather than guessing**: `kSurfaceBoundDeclined = INT64_MAX` (`core.h:463`)
**[V]**.

### 2.2 The geometry, at the real site

`H` bounds only from above; below it the marcher must walk normally. The value of the scheme is
the fraction of marched distance above `H`, and the slack sets the floor.

**The slack floor** — `detailAllowanceMm` is taken at its ceiling, not the footprint's own value
(`amplifier.cpp:2040-2067`) **[V]**:

- coarse (30 m) tier: `kDetailMaxAtMaxSlopeMm = 15,509 mm` — `amplifier.cpp:940` **[V]**
- fine (1.875 m) tier: `kFineDetailMaxAtMaxSlopeMm = 2,280 mm` — `amplifier.cpp:941` **[V]**

**This is why the fine tier matters so much here.** Measured at the real spawn, level-0 leaf
slack is **16.90 m** on the coarse tier and **3.84 m** on the fine tier **[V]** — 4.4x tighter,
and the real spawn is fine-covered.

**The ray condition.** For an eye `E` above local ground at pitch `p`, the ray is above `H` at
distance `t` iff `h(0) - h(t) > S - E - t*tan(p)`. With `S = 3.84 m` on the fine tier and the
default +5 m spawn, the threshold `S - E` is **negative** — the camera starts *above* its own
column's `H` by ~1.2 m, and every metre the ground ahead drops adds directly to the skip.

Against `R ≈ 1,371 m` of relief in reach, that is not a marginal effect. A ray from the spawn
toward the 1,485 m valley is above local `H` (≈1,489 m) by **~703 m** of vertical clearance,
i.e. provably in empty space for essentially its whole length. **[I, from the verified R and S]**

The earlier draft's probability table is withdrawn: it was computed on the wrong site's structure
function, and at `S/d = 0.326` (versus 0.0016 at the abandoned spawn) the Gaussian model it used
is no longer the interesting question. The relief simply dominates the slack by 357x.

---

## 3. MAX-REDUCE UPWARD: confirmed on real tiles at the real site

### 3.1 The corner cap is not the obstacle, and is fine-tier-only

`kSurfaceBoundMaxCornersPerAxis = 72` (`amplifier.h:276`), refusal at `amplifier.cpp:1907`
**[V]**. The corner count is driven by the **tile pixel size**, not the footprint alone
(`amplifier.cpp:1902-1906`) **[V]**:

```
nx = ceil(span_mm / pxMm) + 1 + 3        (+ 8 more on a tier that prefilters)
```

- Coarse tier (`pxMm = 30,000`): a 204.8 m footprint needs `nx ~ 19`. **No decline.** The cap
  bites near 1.7–2.0 km, exactly as `test_amplifier.cpp:767-771` pins **[V]**.
- Fine tier (`pxMm = 1,875`, no halo): a 204.8 m footprint needs `ceil(205,800/1875) + 4 = 114`.
  **> 72, declines.** **[V, arithmetic on the verified formula]**

So the remembered "declined at level 6, ~113 corners" is a fine-tier fact. Your instinct was
right; the cap is not why.

### 3.2 The real reason, measured on the real ground

The bound is a Lipschitz envelope around one centre evaluation (`amplifier.cpp:1990-2038`) **[V]**:

```
slack = ceil(lipDx * halfW / pxMm) + ceil(lipDy * halfH / pxMm) + detailAllowance
```

`halfW` grows with the footprint and `lipDx` is a **max** over a growing grid, so slack grows
**superlinearly**. Measured with the probe's own adversarial sweep (`terrainprobe.cpp:688`),
**0 declines and 0 violations on every row** **[V]**:

**Upper slack (m), direct query at each footprint size:**

| level | edge | **real FINE @ `-61440,-61440`** | real coarse @ same | real coarse @ `0,0` | synth 1.875 m |
|---|---|---|---|---|---|
| 0 | 3.2 m | **3.84** | 16.90 | 15.82 | 24.14 |
| 1 | 6.4 m | 5.35 | 17.83 | 16.09 | 44.94 |
| 2 | 12.8 m | 9.68 | 19.91 | 16.71 | 98.60 |
| 3 | 25.6 m | 22.75 | 24.41 | 18.05 | 226.79 |
| 4 | 51.2 m | 66.27 | 35.19 | 21.10 | 542.71 |
| 5 | 102.4 m | **188.83** | 61.33 | 28.17 | 1241.53 |

Commands **[V]**:
`vxc_terrainprobe .../s1 20260719 -61440 -61440 4000 --fine-dir .../s16` and
`vxc_terrainprobe --synthetic 20260719 0 0 1000 <pxMm>`.

**Read the first column.** On the real fine tier at the real spawn, a direct level-5 query
returns **188.83 m**; a 3.2 m leaf returns **3.84 m**. Max of sound upper bounds over a
partition is a sound upper bound over the union, and it is tight to the tightest leaf — so a
max-reduced pyramid carries **3.84 m** all the way to level 5. **49x tighter than asking the
amplifier directly.** **[V]** for the numbers, **[I]** for the max-reduce soundness, which is
immediate.

**State it as the strong claim.** Max-reduce upward is not a workaround for a refusal. It is the
only way to get a usable coarse bound at all, and the effect is a factor of 49 on the ground the
project actually flies over.

### 3.3 Leaf size: level 0 (3.2 m) on the fine tier

The earlier draft recommended a 25.6 m leaf. **That was a coarse-tier conclusion and it does not
transfer.** On the coarse tier slack is nearly all constant floor, so a coarse leaf costs little
(15.82 -> 18.05 m from L0 to L3). On the **fine** tier the floor is only 2.28 m, so the Lipschitz
term dominates and the leaf choice is decisive: **3.84 m at L0 versus 22.75 m at L3 — 5.9x**
**[V]**. Build the leaf at **level 0** wherever the fine tier is resident; fall back to a coarse
leaf (16.90 m) where it is not, and record which tier each cell came from.

---

## 4. Why one number is not enough here

### 4.1 The ceiling is pinned by the highest peak in reach

This is the part your framing got right and the earlier draft got wrong.

A ceiling is `max H` over the whole march footprint. At this site that maximum is set by the
massif at `-57440,-57440`: **2,856.4 m** **[V]**, so the ceiling sits at ~2,860 m. Meanwhile
local `H` ranges down to **1,489 m**. The ceiling therefore discards **1,371 m** of vertical
bound in every direction that does not point at the massif.

Concretely, for the pose-split legs' camera at ~2,192.6 m:

| bound | value | camera relative to it | effect on an upward ray |
|---|---|---|---|
| march box (today) | ±4,198 m | inside | walks to the wall or the 512-chunk cap |
| **ceiling** (one number) | ~2,860 m | **667 m below** | crosses at `t = 667/sin(θ)` — 3,842 m at θ=10°. Barely fires. |
| **pyramid** (per column), low directions | ~1,489 m | **703 m above** | provably empty from `t = 0` |

**[V]** for the H values; **[I]** for the trigonometry.

So at the real site the ceiling is close to inert for the poses that cost the most, and the
pyramid is not a refinement of it — it is the mechanism. **This is the opposite of what the
earlier draft concluded, and the reason is entirely that the earlier draft priced a site with
0.11 m of relief.**

(For the `SpawnAltM=220` capture legs the camera is at 2,407.6 m — still 453 m below the ceiling,
still up to 918 m above local `H` in the low directions. Same conclusion, larger margins.)

### 4.2 One coarse level, or a pyramid?

Your spike (56.5 -> 34.6 miss steps but 48.6 -> 68.9 hit steps for a second coarse level) and the
literature both say the first level carries most of the win. Tevs et al. **[C,
`scratchpad/tevs2008.txt`]** report maximum mipmaps overtaking uniform+binary search only above
512²–1024² field resolution (Table 5), and their advantage over relaxed cone step mapping is
*negative* on throughput (CSM 227 fps vs. maximum mipmap 110 at 640×480, Table 4) — maximum
mipmaps win on **dynamic** fields and **artefact-free** thin structures. Take that as a caution
against building depth for its own sake.

**Recommendation: one leaf level plus two max-reductions (3.2 / 12.8 / 51.2 m), and measure
before adding a third.** The DDA needs a level whose cell is comparable to the ring-cascade
segment length; beyond that a level cannot skip anything the cascade was not already skipping
(rule 1).

### 4.3 Build cost and residency

- **Source**: `FVoxelWorldImpl::FootprintSurfaceUpperBoundMmCached(Level, ChunkX, ChunkY)`
  (`VoxelWorldSubsystem.cpp:14274`) — already memoised per column, already called for every
  footprint on every recompute (`:11739`, `:14653`, `:15128`) **[V]**. The pyramid is a
  **reduction over work already being done**, not new worldgen.
- **Extent / resolution**: the march reach is 8,396 m. At a 3.2 m leaf that is 2,624² cells —
  27.5 MB at R32F, which is too much to rebuild eagerly and is the one place this design costs
  something real. Two options, in order of preference:
  1. **Leaf at 12.8 m** (656² = 1.7 MB, slack ~9.68 m fine **[V]**) — still 2.5x tighter than
     the best coarse-tier leaf, and 20x tighter than a direct L5 query.
  2. Leaf at 3.2 m only within the inner rings, coarsening outward — the rings already are the
     LOD pyramid, so match them.
- **Residency**: toroidal re-anchor, as the raster atlas does — fill only newly exposed rows and
  columns as the anchor moves. Do not rebuild.
- **Update on edit**: mandatory, §6.3.

---

## 5. Crowns — already inclusive, add nothing

`assetAwareSurfaceUpperBoundMm` (`assetplacement.h:464-487`) **[V]** composes **per layer**:

```cpp
const int64_t reachVox = int64_t(L.maxRadiusMm) / int64_t(kVoxelSizeMm) + 1; // round outward
const int64_t b = surfaceUpperBoundMm(rect.vx0 - reachVox, ..., rect.vy1 + reachVox);
if (b == kSurfaceBoundDeclined) return kSurfaceBoundDeclined;
if (b + int64_t(L.maxHeightMm) > best) best = b + int64_t(L.maxHeightMm);
```

A crown standing on ground *outside* the cell is covered by the reach dilation; the layer's own
`maxHeightMm` caps everything above the base. `FootprintSurfaceUpperBoundMm` calls it whenever an
asset field is installed (`VoxelWorldSubsystem.cpp:14263-14273`) **[V]**.

**Use `FootprintSurfaceUpperBoundMm` and add no margin.** Do not add a global 45 m — the
per-footprint composition is strictly tighter, and the codebase says so
(`VoxelWorldSubsystem.cpp:6350-6354`) **[V]**:

> measured over 63,001 footprints, that gives 89.2% of them the L1 cap (25 m) rather than this
> global 45 m maximum

45 m is confirmed as the tallest **baked** terrain-lattice species, from the manifest and clamped
to the layer cap (`VoxelWorldSubsystem.cpp:6121-6141`) **[V]**; the comment there records the
60 m authored cap as the trap it is. The global value is used by `SkipBrick` only.

---

## 6. Absence — the failure mode most likely to ship

### 6.1 The representation, and it already exists

`kSurfaceBoundDeclined = INT64_MAX` (`core.h:463`) **[V]**. The decline is **already +infinity**,
it already **propagates** through the asset composition (`assetplacement.h:468`, `:481`) **[V]**,
and `FootprintSurfaceUpperBoundMmCached` already refuses to cache a value computed while a fine
tile was still decoding (`VoxelWorldSubsystem.cpp:14290-14300`) **[V]**:

> a stale bound here UNDER-STATES the surface ... "a chunk containing terrain is never generated.
> A hole in the world."

**Rules for the GPU field:**

1. **R32_FLOAT**, and an unbuilt / declined / non-resident cell holds `+INF` — literally
   `asfloat(0x7F800000)`. Not a large finite number, not zero, not sea level.
2. **Clear to `+INF`, never to 0.** A zero-cleared height texture reads as "terrain tops out at
   sea level", which at this site deletes 2.8 km of mountain. This is the single most likely way
   this ships broken.
3. **Max-reduction propagates `+INF` for free** — `max(x, INF) = INF` — so one absent leaf poisons
   its parents automatically, which is the correct direction.
4. The shader test `RayZ > H` is `false` for `H = +INF` **for free**: no sentinel comparison, no
   branch, no chance of an inverted test. This is why `+INF` is right and a sentinel like `-1` is
   not.
5. **Never** derive the field from residency; a "not streamed yet" cell holds `+INF` and nothing
   else. The bound must come from `FootprintSurfaceUpperBoundMm`, which is analytic.
6. **Record the tier per cell.** A cell built from a coarse leaf (16.90 m slack) and one built
   from a fine leaf (3.84 m) are both sound but differ by 4.4x; mixing them silently makes the
   field's tightness unpredictable and unmeasurable.

### 6.2 The absence that already bit this investigation, twice

- At `-84480,53760` the sea-level fallback presented as a plausible flat plain and produced a
  confident, wrong NO-GO (§1.1). **Treat any `H` at exactly sea level as suspect.**
- The same draft then generalised from that one site. The lesson is the memory entry's:
  flat-terrain numbers do not transfer. **A single-site R is not a verdict** — which is why §9
  now carries six.

### 6.3 Player edits — the one real soundness hole

The analytic bound knows nothing about a block placed on a hilltop. The streaming layer's existing
escape hatch is `EditedFootprintMaxZ[level]` (`VoxelWorldSubsystem.cpp:6855-6862`) **[V]**. Every
`H` cell must be `max`ed with the edited max-Z for its footprint and **invalidated to `+INF` on
any edit landing above it**. If the edit path cannot be hooked cleanly, gate the feature off
whenever the edit log is non-empty. A too-low `H` deletes terrain silently.

---

## 7. Where it composes in the shader

**Clamp `TMaxUU` / advance `TMinUU` at the caller. Do not touch the box.**

`TMaxUU` occurs 33 times in `ue-project/Shaders/VoxelBrickTraverse.ush` **[V]** (`grep -c`,
counted before reading anything into it). The **real readers** — the sites bounding how far a walk
goes:

- `:3545` `for (int ci = 0; ci < 512 && t < TMaxUU; ++ci)` — brick-hier outer loop
- `:3910` `const float tChunkEnd = min(tChunkExitTrue, TMaxUU);`
- `:4269` `const float SegOut = min(VoxelMarchRingOuterUU(SegL) * RingTScale, TMaxUU);` — **the ring cascade segment bound**
- `:4860` `const float SegOut = min(TMaxUU, MarchCoverReachUU);` — the cover-seg path
- `:4071-4078` decide `TermReason` from `t < TMaxUU`

All are downstream of the value passed in, so a single clamp at ray build flows into every one —
the same shape as the existing depth clamp (`VoxelMarch.usf:1358-1359`,
`TMax = min(TMax, SceneTUU)`) **[V]**.

**Do not touch `VoxelMarch.usf:872`.** The camera-centred box
(`R.TExitUU = min(min(THi.x, THi.y), THi.z)`) is symmetric about the frame origin, which is what
guarantees the camera is inside it. Asymmetric Z faces give `THi.z < TLo.z` for a camera above the
ceiling, `bValid` goes false, the ray is dropped — and because `bValid` is a dead test today, that
is written out as a miss with **zero instrumentation**. The frame would go empty when you fly
high, silently. **[V]**

**The pyramid proper:** a 2D DDA over the `H` texture run **once, before** the per-level segment
loop at `:4269`, producing an advanced `TMinUU` and a clamped `TMaxUU`. It must add no test inside
the segment loop (rule 1) and must not consult the retry ladder at `:4173` / `:4718` (rule 3).

---

## 8. The gates

**Image first. This project shipped a "-7.6% win" that was the marcher deleting a mountain, and
the timing inverted to +3.1% once the image was made honest.**

1. **Engagement — proves it fired, on real rays.** A counter at the clamp site recording (a) rays
   consulted, (b) rays where the bound strictly reduced the walked interval, (c) summed `t`
   removed. Report **(b)/(a)** and refuse to read timing from any leg where it is 0. A leg where
   the arm did nothing is a void leg, not a null result.

2. **The falsifier, and it must be shown able to fire.** A debug arm that walks the ray
   **unclamped** and counts hits found inside the interval the bound called empty. Must be **0**.
   Then *deliberately corrupt* the field — subtract 50 m from every `H` cell — and confirm the
   counter goes **non-zero**. A confirmation that cannot come out the other way is not one.

3. **Tier accounting.** Report what fraction of consulted cells came from a fine leaf, a coarse
   leaf, and `+INF`. A field that is silently all-`+INF` is inert and will otherwise read as a
   clean null.

4. **Regression watch.** `substituted` must not rise. Do **not** use `uncovered` as an arc
   detector — 0.02pp corresponds to visible black arcs, and coverage cannot be judged from a
   parked capture.

5. **The image gate.** Same-pose A/B captures at `-61440,-61440`, including at least one yaw
   pointing **away** from the massif — that is where the pyramid does its most aggressive
   skipping and therefore where a too-low `H` would delete terrain first.

---

## 9. The go/no-go number

### Name it: **R/S — relief over slack**

- **R** = `max(H) - min(H)` over the march footprint (±4,198 m).
- **S** = the leaf-level `upper slack` — floor `2.280 m` fine / `15.509 m` coarse.

**A pyramid beats a single ceiling by at most `R`**, because the ceiling is forced to take the
maximum. If `R` is not several times `S`, `H` is constant to within its own error and the pyramid
*is* the ceiling.

**Threshold: build if `R > 4S`.**

### Computable today, from an instrument that already exists

`vxc_terrainprobe <tiledir> <seed> <x> <y> <len> [--fine-dir DIR]` prints both halves —
`surface range` gives `R` along a transect, the `ADVERSARIAL BOUND SWEEP` gives `S` per level. No
new tool, no editor, no build. **[V — every number here was obtained that way.]**

### Measured

| site | R | S (leaf) | **R/S** | verdict |
|---|---|---|---|---|
| **`-61440,-61440` — the real leg spawn, fine tier** | **1,371 m** (6 transects in reach) | **3.84 m** | **357** | **GO** |
| same, coarse-tier leaf | 1,371 m | 16.90 m | 81 | **GO** |
| same, spawn transect alone | 250.2 m | 3.84 m | 65 | **GO** |
| `0,0` (deep seabed) | 280.6 m over 2 km | 15.82 m | 17.7 | GO |
| `-40000,-30000` (high plateau) | 22.4 m over 4 km | 16.54 m | 1.4 | no-go *at that site* |
| `-84480,53760` (abandoned spawn, no tiles) | 0.11 m over 4 km | 15.27 m | 0.007 | inert — and it is not real ground |

**[V]**, all from probe runs this session. `R` from single +x transects is a lower bound on the
true 2D relief, so the GO rows are conservative.

### Verdict

**GO.** At the site the project actually measures, `R/S = 357` against a threshold of 4. The
plateau row (`R/S = 1.4`) is worth keeping because it shows the metric can say no and where — this
world contains ground where the pyramid would be inert, so the tier/`+INF` accounting in §8.3 is
not optional.

---

## 10. Recommended order of work

1. **Build the leaf + 2 max-reductions from `FootprintSurfaceUpperBoundMmCached`**, fine leaf
   where resident, coarse leaf otherwise, `+INF` on decline or absence. Toroidal re-anchor. Start
   at a **12.8 m leaf** (1.7 MB, 9.68 m slack) unless the 3.2 m leaf's memory can be scoped to the
   inner rings.
2. **Wire the 2D DDA before the segment loop at `:4269`**, producing advanced `TMinUU` / clamped
   `TMaxUU`. Gates per §8 — engagement counter and the corruption-fires falsifier land in the same
   change, not after it.
3. **Measure at `-61440,-61440` on the pose split**, with at least one yaw away from the massif.
4. **Do not bother with the standalone ceiling.** §4.1 shows it is pinned at 2,860 m by one massif
   and barely fires at this site; it is not a cheaper first step here, it is a different and much
   weaker mechanism.

---

## Appendix — what was NOT re-derived

Not revisited, per instruction: the open-sky mark; the elevation raster atlas as a ceiling; the 4³
block occupancy arm; the beam pre-pass; per-step tests inside the ring cascade; residency-derived
emptiness; skipping the retry ladder on level-L evidence.

Corrections to the brief's framing, both evidence-backed:

- The 72-corner refusal is **fine-tier only**; on the coarse tier the bound accepts footprints to
  ~1.7 km. The real obstacle to a direct coarse query is Lipschitz slack (§3.2).
- `scratchpad/zclamp-design.md` §3 cites the tallest baked asset as "~17 m". The current manifest
  path gives **45 m** global / 25 m for 89.2% of footprints
  (`VoxelWorldSubsystem.cpp:6350-6354`) **[V]**. Neither needs to be used directly — §5.

One further note on `voxel.March.ZCut`, which is **not** evidence against this design: its slab is
fed by `Index.GetResidentChunkZBound(...)` (`VoxelMarchRenderer.cpp:5473`) **[V]** — residency-
derived, from a cumulative union documented to widen "until the cut removes nothing". It is a
bound that provably degrades to inert as a session runs, and says nothing about a world-derived
bound either way.

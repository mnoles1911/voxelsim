# Procedural placement for the 828 environmental assets

Date: 2026-08-15
Status: design + voxel-core implementation built and tested; UE wiring specified, not written
Prior art: `docs/asset-streaming-design.md` (the streaming half, built 2026-08-10),
`voxel-core/include/voxelcore/assetplacement.h`, `voxel-core/include/voxelcore/assetgrid.h`,
`asset-forge/docs/aquatic-species.md`, `asset-forge/docs/biomes/README.md`

This session had no access to the Unreal editor (another session held the box) and
did not create a worktree (one checkout, owner policy). Everything below is
headless C++ that compiles and runs, plus static analysis of asset-forge. Every
claim about current behaviour carries a `file:line` or a measurement. §9 says
plainly what is specified and unbuilt, and what needs the editor.

> **Read `docs/asset-placement-architecture.md` alongside this.** That pass and
> this one were run independently and agree on the broad shape. This document is
> the *built* half — the code, its tests and its measurements — and §11 below
> reconciles the two, including the one place I disagree and why, with evidence.
> Where they agree I have adopted the architecture doc's version rather than
> restating my own; two of its corrections changed what I built, and both are
> load-bearing.

---

## 0. The problem in one paragraph

There are **828 authored species** in `asset-forge/specs/` across ten kinds — 131
land animals, 127 birds, 106 fish, 102 rocks, 89 grasses, 87 flowers, 78 trees,
57 bushes, 33 reeds, 18 cetaceans. Every one carries a **per-biome weight** and a
**`placement` group**, and every aquatic one carries a **`detail` group**.
**Nothing reads any of it.** `assetplacement.h` scatters anonymous *sites* on four
size-class lattices and stops there, deliberately: it says so itself, "Deliberately
carries no species: choosing a species needs biome, and the whole point of the split
is that this half is computable without one." This document is the other half.

The owner's framing is the spec, and it names four problems that do not share a
solution:

> *"The placement logic will vary by type. Trees and plant life + animals vary by
> biome. Fish and aquatic animals vary by freshwater vs saltwater bodies. Rocks and
> trees follow their own algorithms and patterns for clustering and grouping
> according to slope and environment."*

---

## 1. The constraint everything else bends around

`Amplifier::surfaceUpperBoundMm` is a **proof** that a chunk is all air, and the
streamer does not generate chunks it can prove empty. `assetplacement.h:18-23`
quotes `bench/terrainprobe.cpp:666-669` on what a wrong bound costs: *"a bound that
is too tight is not a lost optimisation, it is terrain that never generates. A hole
in the world."*

The existing resolution is the **veto-only rule** (`assetplacement.h:47-72`):

> A POLICY MAY ONLY VETO A SITE. It may not create one, move one, or substitute a
> taller asset than its layer's declared maximum.

Under that rule `assetTopAboveSurfaceMm` is a function of the layer table alone, so
it is an upper bound over *every* policy including ones not yet written. **Every
mechanism in this document is a veto**, and §5 says explicitly why each one is.
There is exactly one place where this document *lowers* the bound rather than
leaving it alone — §2.2, the terrain/detail lattice split — and that one is
argued and guarded separately because lowering a bound is the dangerous direction.

---

## 2. The taxonomy: which of the four classes an asset is in, and who decides

`forge/kinds.py:29-58` already records the single most important property, and it
is not a matter of taste:

| Kind | `lattice` | Voxel size | Class |
|---|---|---|---|
| `tree`, `rock` | `terrain` | 10 cm = `kVoxelSizeMm` | **1 — terrain-lattice** |
| `grass`, `reed`, `flower`, `bush` | `detail` | 5 cm | **2 — ground cover** |
| `quadruped`, `bird` | `detail` | 1–5 cm | **3 — land detail entities** |
| `fish`, `cetacean` | `detail` | 1–10 cm | **4 — aquatic detail entities** |

`AssetGrid::onTerrainLattice()` (`assetgrid.h:162`) is the runtime twin of that
column and answers `voxelSizeMm_ == kVoxelSizeMm`. It exists and nothing consumed
it either.

### 2.1 Class 1 and class 2 are *not* the same problem, and the difference is the bound

A tree and a boulder **join the world voxel grid**. They are destructible as
terrain is, they are addressable in the world grid, they are solid above the
surface, **and they therefore widen the streaming bound.**

A grass tuft carries its own 5 cm grid and its own transform. It never enters the
world lattice at all. It is placed permanently — it is a pure function of position,
so nothing about it is saved and nothing about it despawns — but **it puts no voxel
in the world grid, so it cannot break the all-air proof.**

### 2.2 The change this justifies, and the guard on it

`AssetLayer` gains one field:

```cpp
bool terrainLattice = true;   // does this layer put voxels in the WORLD grid?
```

`assetTopAboveSurfaceMm`, `assetBottomBelowSurfaceMm` and `assetMaxReachMm` skip
layers where it is false.

**This lowers a bound, which is the direction that puts holes in worlds**, so it
gets three guards rather than a comment:

1. The default is `true`. A layer that never mentions the field is a terrain layer
   and pays the full bound. Forgetting to set the field is the safe direction.
2. `assetLayerAdmitsVoxelSize(layer, voxelSizeMm)` refuses a mismatch — a 10 cm
   asset filed on a detail layer, or a 5 cm asset filed on a terrain layer — at
   bake and load time, beside the height check that already exists
   (`assetLayerAdmitsHeight`). This is the check that makes the skip sound: a
   detail layer provably contains nothing that is on the world lattice.
3. `test_assetplacement.cpp` pins that a detail layer contributes exactly zero to
   both bounds and to the reach, and that flipping the same layer to
   `terrainLattice = true` makes it contribute again. Both directions, because a
   gate tested only in the easy direction is the failure `tilestreaming.h:173-188`
   already recorded.

The win is not only tidiness. The bound's early-out means **the densest layer
answers first**: with ground cover on a terrain layer, every chunk footprint on
the planet takes `maxHeightMm` of the grass layer — 500 mm at the test table's
values — and 500 mm straddles a level-0 chunk boundary about one time in six.
That is ~15% of the world's footprints admitting an extra 32-voxel chunk layer to
carry vegetation that is not in the voxel grid.

---

## 3. Class 1 — trees and rocks: biome, slope, elevation, clustering

### 3.1 What a site becomes

`assetSiteInCell` produces an `AssetSite`: a cell, a jittered anchor, a layer, a
bank seed index and a yaw. The policy turns that into an `AssetInstance` or
rejects it. New header: **`voxelcore/assetpolicy.h`**.

```
site  --(1) habitat gate--> survives?  --(2) cluster veto--> survives?
      --(3) biome-weighted species pick--> species, or reject (all weights zero)
      --(4) ground anchor--> anchorZVox, or REJECT if the ground is not there
```

Steps 1–3 are vetoes. Step 4 is the anti-float guard and is discussed in §7.

### 3.2 What the gate reads, and where it comes from

| Gate | Species field | Spec path | Column fact |
|---|---|---|---|
| Biome | `biomeWeightPerMille[10]` | `biomes.*` | `ColumnSample::biome` |
| Elevation band | `elevMinMm`, `elevMaxMm` | `placement.elev_min_m`, `elev_max_m` | `ColumnSample::surfaceMm` |
| Slope band | `slopeMinPct`, `slopeMaxPct` | `placement.slope_max_pct` (min: **not in the spec yet**, §8.3) | `ColumnSample::slopeMmPerM` |
| Distance to water | `waterMaxMm` | `placement.water_max_m` | **not servable today**, §9 |
| Clustering | `clusterQ10` | `placement.cluster` | — (pure hash) |
| Abundance | `abundanceQ10` | `placement.abundance` | — (pure hash) |
| Spacing | `spacingMm` | `placement.spacing_m` | — (chooses the layer, §3.3) |

**`ColumnSample` did not carry biome or slope, and both were computed and thrown
away.** `Amplifier::column` classifies the biome at `amplifier.cpp:2499` and
immediately reduces it to `surfaceMat`, which is lossy in exactly the way that
matters here — `MAT_SAND` is both `BEACH` and `DESERT`, `MAT_ROCK` is both
`BARE_ROCK` and alpine above the rock line. Slope (`slopeMmPerM`) is computed in
`evalSurface` and is `private`.

So this design adds two fields to `ColumnSample`:

```cpp
BiomeId biome        = TEMPERATE_FOREST;  // classifyBiome's own answer, unreduced
int64_t slopeMmPerM  = 0;                 // the carrier's analytic gradient
```

This is the mechanism `cave` and `cavern` already established and the header
already argues for (`amplifier.h:76-82`): anything a downstream consumer needs and
cannot derive from `vz` rides on the `ColumnSample`. **It cannot change worldgen
output** — `materialAt`/`stratigraphyAt` do not read either field — and that is
verified, not asserted: the digest is unchanged (§8.1).

**This is the one place I disagree with `asset-placement-architecture.md`**,
which says at its §2.3: *"Do not widen `ColumnSample`; it is mirrored in
`GpuColumnSample`."* The evidence says the premise does not hold:

* `GpuColumnSample` (`shaders/worldgen.ush:1187-1194`, mirrored at
  `bench/gpu_harness.cpp:261-268`) has **five** members. `ColumnSample`
  (`amplifier.h:60`) has **eight** — the same five plus `waterSurfaceMm`,
  `CaveColumn cave` and `CavernColumn cavern`. It has not been a 1:1 mirror
  since the M4 cave pass landed, and `worldgen.ush:1330` says outright that
  `GpuColumnSample` *could not* be widened to carry the cavern site surface.
* The parity check is **by named field**, not by size or `memcmp`:
  `gpu_harness.cpp:2310-2320` records `surfaceMm`, `topsoilMm`, `subsoilMm`,
  `bedrockDepthMm` and `surfaceMat` one at a time. A member the GPU does not
  have is a member the check does not look for.
* The `static_assert(sizeof(GpuColumnSample) == 20)` is on the **GPU-side**
  struct against the HLSL layout, not on `ColumnSample`.

So the mirror obligation attaches to fields `materialAt` reads, and these are not
among them — which is why the digest did not move.

**Where the architecture doc is nonetheless right, and what I did not build.**
Its `Amplifier::surfaceInfo(vx, vy)` is the correct query for a *different*
caller, and the cost argument behind it is real: a full `column()` pays the cave
reduction (34 hashes) and the cavern reduction, which a placement gate does not
need. The distinction is which path is asking:

| path | has a `ColumnSample` already? | wants |
|---|---|---|
| Chunk generation (classes 1–2) | **yes** — `GeneratedWorld::columns` builds one per column, and the anchor check *needs* the full one, because verifying the anchor voxel is `Amplifier::materialAt(col, …)` and that reads the carve passes | the two free fields |
| Entity spawn (classes 3–4) | **no** — a spawner has a position and nothing else | `surfaceInfo` |

Both, then, for different callers — and `surfaceInfo` is **not built here**
(§9). Two fields that were already computed and dropped are free for the path
that places trees; a cheaper query is the right thing for the path that places
deer, and that path has no code yet at all.

### 3.3 Spacing chooses the layer; it is not a rejection test

`placement.spacing_m` across the library runs 0.15 m (buffalo grass) to 3000 m
(a named landmark rock). A per-instance minimum-distance test is a relaxation
problem and cannot be a pure hash. The lattice **is** the spacing mechanism:
`AssetLayer::cellMm` is *"the minimum possible spacing between two assets of this
class and roughly their mean spacing at full density"* (`assetplacement.h:113-115`).

So `spacing_m` selects which of the four layers a species is filed on, at bake
time, and the residual within a layer is expressed as density:

```
densityPerMille(species) = 1000 * (cellMm / spacingMm)^2 * abundance
```

clamped to [0, 1000]. A species whose spacing is *larger* than its layer's cell
thins the lattice; a species whose spacing is smaller than the cell cannot be
served by that layer and belongs on a denser one. `assetSpeciesLayerFits` refuses
the second case at load rather than silently under-spacing.

Measured layer assignment over the 828 specs (`spacing_m` percentiles by kind):
trees 2.5–700 m (median 7.5), rocks 1–3000 m (median 14), bushes 0.8–4 m,
flowers 0.3–2.6 m, grass 0.15–2 m, reeds 0.3–3 m. Four size classes cover it with
the emergent class carrying the long tail by density rather than by pitch.

### 3.4 Clustering — the mechanism, and the measurement that justifies it

The owner named it: *"Rocks and trees follow their own algorithms and patterns for
clustering and grouping."* `placement.cluster` is authored across the library and
means it: grass 0.35–1.0 (median 0.95), reeds 0.25–1.0 (median 0.95), trees
0.05–0.95 (median 0.6), rocks 0.05–1.0 (median 0.35).

**The measurement first, because it changes what the mechanism has to do.** The
claim usually made is "a uniform hash scatter is Poisson and real forests are
clumped". Measured on the existing scatter, that claim is *wrong in an
interesting direction*. `assetSiteInCell` is one site per lattice cell with a
jitter inside the cell — a **jittered grid**, which is not Poisson but
**anti-clumped**. Counting sites in quadrats and taking the index of dispersion
(variance/mean; 1.0 is Poisson, >1 clumped, <1 regular):

| Pattern | Index of dispersion | Empty-quadrat fraction |
|---|---|---|
| Poisson (reference) | 1.00 | e^-λ |
| **Existing scatter, density 400‰** | **0.60** | 0.00 |
| Clustered, strength 1.0 (this design) | **2.6** | 0.19 |

Numbers are from `test_assetpolicy.cpp`'s `assetcluster_measures_as_clumped_...`,
which prints them on every run. **0.60 is more regular than random.** A forest
scattered that way does not read as "randomly placed trees"; it reads as an
orchard, and it reads as an orchard *more strongly* than a Poisson scatter would.
That is the finding worth having before writing any clustering code, and it is the
reason clustering is not a polish item.

**The mechanism: a mean-preserving density modulation, which is a veto.**

```
gainQ10 = 1024 + (clusterQ10 * valueNoise2Fade(seed, x, y, clusterLatticeMm, CH_ASSET_CLUSTER+li)) / 32768
keep iff  occDraw * 1024  <  densityPerMille * gainQ10
```

Three properties, each of which is a test:

* **It is a pure veto.** It only ever removes sites from the set
  `assetSiteInCell` already produced. It cannot create one, move one, or raise a
  height. The bound never learns it exists and does not need to.
* **It preserves density in expectation.** `valueNoise2Fade` has mean ≈ 0, so
  `E[gainQ10] = 1024` and `E[keep] = densityPerMille` regardless of the field's
  distribution. This is why the field modulates the *threshold* rather than being
  thresholded itself: thresholding a bell-shaped field at a uniform quantile
  silently changes the population by a large factor, and a species that quietly
  ships at a third of its authored abundance is this project's signature failure.
  Measured: clustered vs unclustered site count agree to within 1.4% over
  ~26,000 cells.
* **It is one extra hash per surviving cell**, on the generation path only, never
  on the admission path.

Cluster lattice: `clusterLatticeMm` is a property of the layer, sized at ~4× the
cell pitch, so a grove is a handful of cells across rather than one cell (which
would be indistinguishable from noise) or a hundred (which would be a biome).

### 3.5 Scree: slope is a band, not a ceiling

*"Scree lies below cliffs."* `placement.slope_max_pct` is a ceiling only, and a
ceiling cannot express talus: scree sits on ground steep enough to be talus
(~25–35°) and *below* the 70%-grade line at which `classifyBiome` returns
`BARE_ROCK` (`biome.h:220`). A species with `slopeMinPct = 40, slopeMaxPct = 68`
is scree; one with `slopeMinPct = 0, slopeMaxPct = 20` is a valley-floor boulder.
The band is in the C++ species record today; `placement.slope_min_pct` does not
exist in `forge/spec.py` and is requested in §8.3.

This deliberately does **not** try to read "is there a cliff uphill of me". That
needs a second column evaluation at an offset, which is affordable on the
generation path but is a different mechanism with a different cost, and the slope
band gets most of the look for one field.

---

## 4. Class 2 — ground cover

Same scatter, same policy, same gates. Three differences, all of them consequences
of not being in the world grid:

1. **It does not widen the bound** (§2.2).
2. **Its layer is the dense one.** `cellMm` in the hundreds of mm, `densityPerMille`
   near 1000, `seedCount` small — a grass tuft's variety comes from there being
   thousands of them, not from each being distinct.
3. **Its habitat gates are the interesting part, not its clustering.** Reeds
   ringing a lake edge is `waterMaxMm` plus a tight elevation band relative to the
   water surface, not a cluster field. This is the one gate that is **specified and
   unservable today** — see §9.

Ground cover is otherwise class 1 with a flag, and it shares
`assetpolicy.h` entirely. That is deliberate: two code paths for "scatter a plant"
is how a fix lands in one of them.

---

## 5. Classes 3 and 4 — detail entities

New header: **`voxelcore/assetdetail.h`**.

A detail entity is not a function of position the way a tree is. It is spawned near
the player, it has no saved state, and it is deleted when the player leaves
(`spec.py:1310-1315`). What stays deterministic is the **group**: a herd, a flock,
a shoal. `detail.school_min`/`school_max`/`school_radius_m` are authored across all
106 fish and 18 cetaceans and mean exactly this.

### 5.1 The group scatter

Groups sit on their own lattice, hashed the same way sites are, with the group
*centre* jittered in its cell and the members placed inside `school_radius_m` of
it. `assetDetailGroupsInDisc(seed, layer, centreXMm, centreYMm, radiusMm)`
enumerates every group whose members could reach a disc — the despawn radius,
typically — which is the entity-spawn analogue of `assetSitesForRect`.

Group size is drawn between `school_min` and `school_max` from one hash, so a
shoal is one decision and its members come from consecutive bank seeds, exactly as
`spec.py:1330-1332` describes.

**Detail entities never touch the streaming bound**, because they are never in the
world voxel grid. `assetdetail.h` has no bound function and deliberately exposes
none; there is nothing for it to bound.

### 5.2 Class 3 — land animals and birds

The gate is biome plus the same elevation/slope band, and then **the ground**. A
quadruped stands on four feet on a plane; a bird perches or stands. §7 is about
this.

Note what `biomes.py` did on 2026-08-15: `bare_rock` gained `quadruped` (an ibex
and a chamois stand on ground far steeper than 35°) and `ocean` gained `bird`. The
policy honours `hosts` implicitly — a species whose weight for a biome is zero is
never picked there — so those two edits already work through this design with no
special case.

### 5.3 Class 4 — fish and cetaceans, and salt versus fresh

**The world does not have a salinity concept and does not need one.** It has two
independent water terms and they are distinguishable *before* they are composed:

```cpp
// lakes.h:1391  — the sea. It is not baked; it IS the datum.
constexpr int32_t oceanSurfaceMmAt(int32_t groundMm) {
    return groundMm < kSeaLevelMm ? kSeaLevelMm : kNoWaterMm;
}
// lakes.h:1411  — and here the provenance is destroyed.
constexpr int32_t implicitWaterDatumMm(int32_t bakedSurfaceMm, int32_t groundMm);
```

`implicitWaterDatumMm` takes the **max** of the baked water (lakes ∪ rivers, via
`IWaterSampler::waterSurfaceMmAtVoxel`) and the sea. After that line the answer is
a plain `int32_t` with no provenance — `lakes.h:1406-1410` says why the sea is
composed there rather than being given its own sampler. So the salinity question
must be answered from **the two operands, not the result.**

`assetdetail.h` does exactly that:

```cpp
enum class WaterSalinity : uint8_t { kNone, kFresh, kBrackish, kSalt };
constexpr WaterSalinity classifyWaterSalinity(int32_t groundMm, int32_t bakedSurfaceMm);
```

with the tri-state resolved by which operand wins the `max()`:

| sea | baked | result | the real place |
|---|---|---|---|
| none | none | `kNone` | dry land |
| present | none | `kSalt` | open sea, ground below the datum |
| none | present | `kFresh` | an inland lake or river above the datum |
| present | present, baked > sea | `kFresh` | a coastal lake perched above sea level |
| present | present, baked == sea | `kBrackish` | **a river mouth** — the reach's datum has descended to meet `kSeaLevelMm`, and `lakes.h:1417-1424` says they are the same number there on purpose |
| present | present, baked < sea | `kSalt` | the sea drowns the baked surface |

Two things this design refuses to do, and both are refusals of a tempting shortcut:

* **It does not use `classifyBiome(...) == OCEAN`.** That gate fires at
  `surfaceMm < −3000` (`biome.h:216`), not at `< kSeaLevelMm`, so the band
  [−3 m, 0) is salt water that classifies as `BEACH`. The two "ocean" definitions
  in this codebase have never agreed and a fish spawner keyed to the biome would
  find no water in the top three metres of every shore in the world.
* **It does not add a salinity byte to `ColumnSample`.** The information is not in
  the amplifier — the baked water arrives from an `IWaterSampler` the amplifier
  only holds in debug-marker mode (`amplifier.h:67-73`) — and inventing a
  populated-only-sometimes field is how `waterSurfaceMm` came to mean both "marker
  off" and "no water here".

`detail.water` (`any`/`ocean`/`river`/`lake`/`shallow`/`reef`) maps onto this as an
admission mask per species; `reef` is `kSalt` plus a shallow depth band, which is
what `spec.py:1337-1342` says the row is a request for.

### 5.4 `detail.depth_*` — the decision

`asset-forge/docs/aquatic-species.md` §3.3 lays the problem out and I am the
consumer it was written for. Restating it exactly:

* `detail.depth_min_m`, `detail.depth_max_m` and `detail.min_water_depth_m` exist,
  and **already mean exactly the right thing for an aquatic plant.**
* They are scoped `kinds=('fish','cetacean')`, and **that scoping gates the app's
  sliders, not `validate`.** Patching a kelp with a depth succeeds silently.
* **All 828 specs carry the 0.3/6.0 defaults** because the canonical spec is
  full-schema, so an authored 15.0 on a kelp is indistinguishable from the 0.3 a
  saguaro is carrying. There is no "unset" to test against.
* `placement.elev_min_m` floors at −10 m (measured), so a Lophelia reef at
  200–1000 m authors −10 like every shallow coral. `cold-water-coral.json`'s own
  notes say so: *"DEPTH IS THE PROBLEM AND IT IS NOT SOLVED."*

**The decision: one set of depth fields, not two.** Depth below the water surface
is the same physical quantity for a kelp and a trout, and the right shape is to
**widen the existing `detail.depth_*` scoping to the aquatic plant and rock kinds**,
not to add a parallel `placement.water_depth_*` group. Two ways to say the same
thing is how the two get different answers. Concretely, in voxel-core:

```cpp
struct AssetSpecies {
    ...
    int32_t depthMinMm, depthMaxMm;   // below the WATER SURFACE
    int32_t minWaterDepthMm;          // total column depth this species needs
    uint8_t waterMask;                // fresh / brackish / salt admission
};
```
— one record, used identically by a fish, a cetacean, a kelp and a reef boulder.
The only difference between them is that a plant's `depthMinMm == depthMaxMm` is
its rooting depth and a fish's is a band it holds within.

**What I changed, and what I did not.** `forge/spec.py` is on the ask-first list
(§0 coordination), so **I changed nothing in asset-forge.** The exact diff being
requested is in §8.3, and it is three lines. The 76 ocean-hosted plants and rocks
that state their depth band in prose are **not** bulk-edited here, for a reason
worth stating: I checked, and the prose mostly does *not* contain a number. Of the
76, exactly one (`cold-water-coral`) quotes a range. Filling in 76 depth bands
would be inventing data and calling it authored — precisely the failure the
aquatic-species doc says it declined to commit ("the alternative was writing
numbers into `detail.depth_*` that would look authored and be indistinguishable
from a default"). The fields go in first; the authoring is a species-list pass and
belongs to whoever owns the ocean list.

**The `unset` problem is real and is not solved by widening the scope.** With every
spec carrying 0.3/6.0, a consumer still cannot tell an authored shallow band from
a default. The mitigation this design adopts is to make the default *harmless
rather than invisible*: a species is only asked for a depth band when it is
already admitted as aquatic (its kind is `fish`/`cetacean`, or it is a plant/rock
with an ocean or freshwater biome weight), so a saguaro's 0.3 is never read. That
does not distinguish an authored 0.3 from a default 0.3 on a *kelp*, and nothing
can without an explicit sentinel. §8.3 requests one.

---

## 6. Where the streaming bound stays sound — the audit

Every mechanism in this document, against the veto-only rule:

| Mechanism | Effect on the site set | Can it raise `assetTopAboveSurfaceMm`? |
|---|---|---|
| Biome weight = 0 | removes | no |
| Elevation band | removes | no |
| Slope band | removes | no |
| Cluster modulation | removes | no |
| Species pick | replaces a site with an instance of a species **filed on that site's layer** | no — every species is height-checked against its layer by `assetLayerAdmitsHeight` at load |
| Ground anchor failure | removes | no |
| Detail-entity groups | never in the world grid | no — they are not in the bound at all |
| **Detail-lattice layer skip (§2.2)** | — | **it LOWERS the bound**, and is guarded by three separate mechanisms (§2.2) |

`assetTopAboveSurfaceMm` and `assetAwareSurfaceUpperBoundMm` are **unchanged** by
this work except for honouring `terrainLattice`. The policy is a separate header
that the bound does not include and cannot be reached from.

---

## 7. The anti-float guard

> *"an asset placed at a position nobody verified is solid ground is an asset
> floating in the air, and there are 828 species to do it to."*

This project's three recent silent no-ops share one shape: **something attached at
a point that was derived rather than verified eventually detaches.** Placement's
version is worse because it is invisible in aggregate — 400 trees in a chunk, three
of them a metre up, and nothing logs it.

The guard is structural, not a check. **There is no way to construct an
`AssetInstance` without handing the constructor a ground sample**, and the
constructor returns `false` rather than a floating instance:

```cpp
bool assetAnchorToGround(const AssetSite&, const AssetColumnFacts&, AssetInstance& out);
```

`AssetColumnFacts` carries `surfaceMm` and a `solid` flag. If the caller has no
column — an unloaded tile, a declined bound, a query outside the world — it cannot
fabricate one, and the instance is refused. **Refusing to place a tree is a tree
that is not there; placing one on nothing is a tree in the sky, and only the second
one reads as a rendering bug.**

The same rule holds for the other three classes:

* Ground cover: identical, same function.
* Land animals: `assetPlaceLandMember` takes the ground elevation and refuses
  without it.
* Fish: `assetPlaceWaterMember` takes **both** the water surface and the bed, and
  refuses if the member's z is not strictly between them. A fish whose depth band
  is deeper than the water is clamped into the column or refused — never emitted
  at its authored depth into mud.

And the guard is swept, not merely asserted: `test_assetpolicy.cpp` and
`test_assetdetail.cpp` each place thousands of instances over a synthetic sloping
world and check that **not one** sits off its surface, with a
`CHECK(placed > N)` vacuous-truth guard on every sweep — the guard the fine-tier
gate went without (`tilestreaming.h:173-188`).

---

## 8. What was built, and what it costs

### 8.1 voxel-core

| File | Change |
|---|---|
| `include/voxelcore/assetplacement.h` | **extended**: `AssetLayer::terrainLattice`, bound/reach honour it, `assetLayerAdmitsVoxelSize` |
| `include/voxelcore/assetpolicy.h` | **new**: species table, habitat gates, cluster veto, biome-weighted pick, verified ground anchoring |
| `include/voxelcore/assetdetail.h` | **new**: salinity classification, group scatter, land and water member placement |
| `include/voxelcore/assetfield.h` | **new**: the asset term itself, and the bank seam |
| `include/voxelcore/generator.h` | **extended**: `GeneratedWorld::setAssetField`, composition in `makeBrick` and `materialAt` |
| `include/voxelcore/world.h` | **extended**: `World::setAssetField` passthrough |
| `include/voxelcore/core.h` | **extended**: `topSolidVoxelZ` — one rule, now shared by `surfaceBrickRange` and the anchor |
| `include/voxelcore/amplifier.h`, `src/amplifier.cpp` | **extended**: `ColumnSample::biome`, `ColumnSample::slopeMmPerM` |
| `include/voxelcore/hash.h`, `hash_channel_registry.h` | **extended**: four new channel blocks, registered |
| `tests/test_assetplacement.cpp` | lattice-split tests, both directions |
| `tests/test_assetpolicy.cpp` | **new** — gates, veto-only property, the clustering measurement |
| `tests/test_assetdetail.cpp` | **new** — the salinity table, the in-column sweep |
| `tests/test_assetfield.cpp` | **new** — composition, and the two defects §11.1 is about |

### 8.1a Three bugs the new tests caught before anything shipped

Recorded because all three had the project's signature shape — code that runs,
reports success and is quietly wrong — and none would have produced an error.

1. **The cluster field lost 17% of the population.** The first cut modulated a
   fixed ceiling of certainty: `keep iff draw * 1024 < 1000 * gain`. `E[gain]`
   is exactly 1024, so the arithmetic looked mean-preserving — but `keep` is
   capped at certainty, so once `gain` exceeded 1024 the test was already true
   for every draw and the entire upper half of the modulation bought nothing
   while the lower half removed sites at full strength. Measured **−4%, −8% and
   −17%** at cluster strengths 256, 512 and 1024. It would have shipped every
   clustered species at a fraction of its authored abundance, and it *measures
   as clustering* — the statistic that catches "clustering did nothing" says
   nothing about this. Fixed by modulating the species' own joint keep
   probability, which has headroom above it; `assetClusterKeeps` carries the
   whole account.
2. **A deep-water fish was placed 18 m underground.** The depth band was clamped
   one-sided: `bandBot` was pulled up to the bed and then pushed straight back
   down to a `bandTop` that was still far below it. Caught by the in-column
   sweep, which asserts `z` strictly between bed and surface.
3. The synthetic VXA blob had `runCount` and `voxelSizeMm` transposed — caught
   by the parser, which is the one of the three that would not have been silent.

Integer-only throughout (voxel-core's CI bans `float`/`double` under `include/` and
`src/`). Placement is worldgen: it must give the same answer on every machine and
eventually in HLSL.

### 8.2 Verification

See §10.

### 8.3 What is requested of asset-forge, and not done here

`forge/spec.py`, `forge/biomes.py` and `forge/kinds.py` are ask-first
(coordination note). **Nothing in asset-forge was changed.** Three requests, in
priority order:

1. **Widen the `kinds` scope of `detail.depth_min_m`, `detail.depth_max_m`,
   `detail.min_water_depth_m` and `detail.water`** from `_SWIM_KINDS` to include
   `grass`, `reed`, `bush`, `flower` and `rock`. Three-line change; makes the
   sliders visible for the 76 ocean plants that currently state their band in
   prose. §5.4.
2. **A sentinel for "unset" on the depth fields** — e.g. `depth_max_m = 0.0`
   meaning "not an aquatic species", so a consumer can tell an authored 0.3 from a
   full-schema default. Without one, widening the scope makes the fields visible
   but still not readable. §5.4.
3. **`placement.slope_min_pct`**, 0–70, `kinds=_LAND_KINDS`. Scree, talus and
   crevice plants are slope *bands*, and a ceiling alone cannot say "steep, but not
   a cliff". §3.5.

Also worth the owner's attention and *not* requested as a change here:
`placement.elev_min_m` floors at −10 m, which no ocean species can use. If the
depth fields land as above, elevation stops being the wrong tool for depth and the
floor matters less; if they do not, the floor needs to reach the sea bed.

---

### 8.4 The clustering numbers, as measured

Printed by `test_assetpolicy.cpp` on every run, so they cannot go stale silently.

**Index of dispersion** over quadrats (variance ÷ mean; 1.00 is Poisson, above is
clumped, below is regular). Measured on the canopy layer over ~15 km:

| pattern | dispersion | busiest quadrat |
|---|---|---|
| the bare scatter, full lattice | **0.59** | 24 |
| unclustered control at the same population | **0.85** | 24 |
| **clustered, strength 1.0** | **3.24** | **30** |

The control matters: comparing a clustered pattern against the *full* lattice
would confound clustering with thinning, so the unclustered control is drawn at
the identical keep probability and the only difference is the field.

**Density preservation**, over the same window:

| species occupancy | strength 0.25 | 0.5 | 1.0 |
|---|---|---|---|
| sparse (125‰, 8× headroom) | 0% | 0% | **0%** |
| half the lattice (500‰, 2× headroom) | 0% | 0% | **−3%** |

and the field's mean gain measures **1016** against a target of 1024 over a
million samples. The −3% is upper-tail clipping against certainty and is
characterised rather than tolerated: it appears only where a species already
occupies half its lattice *and* clustering is at full strength.

**One measurement lesson worth keeping.** The first version of the density test
used a 3.8 km window and reported −8%, which looked like a systematic bias in
the field. It was not — the coarse octave's lattice is 288 m, so 3.8 km is ~13
cells across and the "bias" was a sample of thirteen numbers. Widening to 15 km
(~53 cells) took it to 0%. *Density is preserved globally and varies locally,
and a field that did not vary locally would not be clustering* — so any
measurement of it has to span many periods of the coarsest octave. The same
mistake had made the direct mean-gain measurement read 1150.

## 9. What is specified and unbuilt

Honest list. Nothing below is claimed as working.

* **`placement.water_max_m` — distance to a watercourse — is not servable.** It is
  authored on 20-odd riverbank species (willows to 120 m, jungle understorey to
  80 m). There is no distance-to-water field anywhere in voxel-core; the water
  samplers answer "is there water at this exact column" and nothing answers "how
  far to the nearest". Serving it needs either a baked distance field on the tile
  (a bake-format change) or a bounded radial search on the generation path
  (affordable, since it is not the admission path, but it is a real cost and a real
  design). **The gate is present in the species record and is currently a no-op
  when `waterMaxMm == 0`, which is what 800 of the 828 specs carry.** It is wired
  to a `distanceToWaterMm` field on `AssetColumnFacts` that no caller can fill
  today, and the test that covers it drives it from a synthetic value. Said
  plainly here so nobody reads "reeds ring a lake" as shipped.
* **Reeds ringing a lake edge** is exactly the above, and is the most visible thing
  this design does not deliver.
* **The species bank is not loaded.** `AssetGrid` reads one baked `.vxa`;
  nothing builds the (species, seed) library. `AssetField` takes an
  `IAssetBankSource` and the tests supply a synthetic one; the real loader —
  search path, decode, LRU — is the host's and is unwritten. **This is no longer
  blocked on the material append**, which has landed: `kMaterialCount` is now
  47, `MAT_BARK` (16) through `MAT_BEAK_HORN` (46), so
  `asset-streaming-design.md` §5.1 is done. Corrected here because that document
  still says otherwise.
* **Species-level groves inside a saturated biome.** Where the summed biome
  weight already fills the lattice, the cluster veto has no headroom and does
  nothing — correctly, since a veto cannot gather what is already everywhere.
  Getting "beech here, oak there at unchanged total cover" needs the cluster
  field folded into the *pick weights* rather than applied after the pick, which
  costs one field evaluation per eligible species instead of one per site.
  Not built; the cost is the reason, and it is a real one on the ground-cover
  layer.
* **`Amplifier::surfaceInfo(vx, vy)`.** The cheap query the entity spawner needs,
  proposed by `asset-placement-architecture.md` §2.3 and not written here (§3.2).
  Classes 3–4 have no code that calls anything yet, so it would have no caller.
* **The layer table is not tuned, and it is a price list.** The tables in the
  tests are illustrative. `asset-placement-architecture.md` §2.4 makes the point
  that matters: because the bound is policy-independent, *vetoed regions still
  pay it* — `assetTopAboveSurfaceMm` widens wherever the lattice has a site,
  including deserts where every tree is vetoed. So tall must be sparse, and the
  real table should be chosen against a measured widening distribution over real
  tiles rather than by eye. **No such measurement was taken here**, and no
  `vxc_assetprobe` bench exists.
* **No spec→`AssetSpecies` importer exists.** The C++ record is the contract; the
  thing that reads 828 JSON files and emits a table is unwritten. It belongs in
  asset-forge (it is a bake step, and it is where the JSON schema lives), and it
  should run `assetLayerAdmitsHeight` and `assetLayerAdmitsVoxelSize` at bake so a
  mis-filed asset is refused by the tool that files it.
* **Levels 1–5 get nothing.** Unchanged from `asset-streaming-design.md` §3: assets
  are level-0 only, so trees stop existing beyond 128 m. That is a visible design
  consequence and the owner's call.

### What needs the editor

Everything that renders. Specifically:

* Wiring `ComputeFootprintChunkZRange` and `IsChunkProvablyAllAir` to
  `assetAwareSurfaceUpperBoundMm` — the change whose failure mode is a hole in the
  world, and the one that must be measured before anything else is tuned
  (`asset-streaming-design.md` §6.4).
* The level-0 worker's `GridSampler` and `SkipBrick`. **These now consult the
  same `vxc::AssetField` `GeneratedWorld` composes**, rather than a parallel
  sampler — see §11.1.
* Detail entities need an actor/ISM spawner that does not exist: nothing in the UE
  project spawns a non-persistent entity from a voxel grid today.
* **Whether any of this looks right.** Per house practice the captures are the
  owner's to judge, not mine. The clustering numbers in §8.4 say the pattern is
  clumped; they do not say a forest looks like a forest.

---

## 11. Reconciliation with `asset-placement-architecture.md`

Both passes ran independently on the same brief. Where they agree I have taken
that document's version. Three of its points changed what I built.

### 11.1 Adopted: the composition point is voxel-core, not the UE `GridSampler`

Its §2.1 is right and it is the single most important correction. Wiring assets
only into the mesher's sampler leaves two silent defects, and I built against
both rather than taking them on faith:

* **`World::applyToOverlay` rebuilds an edited brick from
  `gen_.makeBrick(e.key)`** (`world.h:125-127`). Without the asset term in
  `makeBrick`, digging one voxel of a tree regenerates that 8³ region as bare
  terrain and the rest of the tree in it vanishes — persisted and replicated.
  `assetfield_digging_one_voxel_does_not_delete_the_rest_of_the_brick_s_tree`
  performs a real edit through `World` and counts: **121 bark voxels before, 120
  after.**
* **`World::materialAt` feeds raycasts, digging, the region graph and collapse.**
  `assetfield_the_per_voxel_path_and_the_brick_path_agree_voxel_for_voxel`
  compares the two paths over 4096 voxels, **1248 of them asset**, with a
  vacuous-truth guard so "they agree" cannot be a statement about bare terrain.

So `vxc::AssetField` exists, `GeneratedWorld` composes it in both `makeBrick` and
`materialAt`, and `World::setAssetField` is the bring-up door. Precedence is
overlay > asset > terrain, with assets writing **only into air** — which also
makes the composition monotone, and that is load-bearing: `assetplacement.h`'s
bound assumes assets are solid *above* the surface, so a composition that could
remove solid would break the all-solid floor bound while the sky bound went on
looking correct.

### 11.2 Adopted: the anchor is verified against the carve passes

Its §2.2. `topSolidVoxelZ` is a function of `surfaceMm` alone, and caves and
caverns run after the surface — a cave mouth is a column with a real surface
whose top voxel is open shaft. `AssetColumnFacts::anchorSolid` carries
`Amplifier::materialAt(col, topSolidVoxelZ(col.surfaceMm)) != MAT_AIR`, defaults
to **false**, and `assetResolveSite` refuses without it.

I went one step further than the doc asks: the formula moved to
`core.h::topSolidVoxelZ` and `GeneratedWorld::surfaceBrickRange` now calls it, so
the voxel a tree stands on and the voxel a brick is materialised for are one
rule rather than two derivations of it.

### 11.3 Disagreed, with evidence: `ColumnSample` and `GpuColumnSample`

Set out in full at §3.2. Short version: `GpuColumnSample` has five members,
`ColumnSample` has eight, the parity check compares by named field, and the
digest is unchanged at `e02458de2be47309`. The doc's *preferred* API —
`Amplifier::surfaceInfo` — is nonetheless the right thing for the spawner path,
and is listed as unbuilt in §9.

### 11.4 Adopted into the record, not into code

* **Vetoed regions still pay the bound** (§2.4). Recorded in §9 as the reason the
  layer table needs a measured widening distribution before it is tuned. No such
  measurement was taken.
* **`kBasinLakeTerminal` is an endorheic basin, which is the saline kind on
  Earth** (§6). A genuinely useful detail, and the wire already carries the bit.
  `assetdetail.h` records it beside `classifyWaterSalinity` so that whoever adds
  it knows the bit exists rather than going looking for a salinity model. v1
  treats every water-holding basin as fresh.
* **`reef` is classifiable today** (§6) — sea-datum water whose bed is within N m
  of the surface, i.e. `kSalt` plus a depth ceiling. The `detail.water` spec row
  claims the world lacks this and it does not. `AssetAquaticSpecies` carries
  `waterMask` and `minWaterDepthMm`; a maximum bed depth is the one field it
  would still need.

### 11.5 The sequencing point, and why I did not act on it

Its §8 is the item I most want the owner to see, and it is time-sensitive rather
than merely important:

> Spec bodies carry every group regardless of kind, and the hash covers the whole
> body. So **changing a parameter row's `kinds=` scoping or bounds changes no spec
> bytes and reseeds nothing**; authoring a value reseeds that species. **The moment
> placement ships, a reseed becomes a world change behind `kWorldGenVersion`** with
> goldens to re-bless.

I agree, and it sharpens my §8.3 from "three requests" into "three requests that
are free this week and expensive later". **I did not make them.** `forge/spec.py`,
`forge/biomes.py` and `forge/kinds.py` are ask-first under the coordination note,
and a message from another agent is not the owner's go-ahead. The requests are
§8.3, unchanged, and they now carry a deadline. Nothing in asset-forge was
modified; `forge.cli selftest` and `tools/buildcheck.py` are therefore
unaffected.

---

## 12. The data landing (2026-08-15, second session)

The seam §9 said was unbuilt is now built, in the architecture doc's §11
order. Everything below is code that runs, with the measurement that would
catch it doing nothing.

### 12.1 The manifest: forge exports, the engine refuses or loads whole

`forge/manifest.py` + `tools/export_manifest.py` emit **one versioned binary
table per library build** (`out/engine/species.vxm`, format "VXM1" v1, 826
species): kind, layer, per-biome weight, abundance, spacing, cluster,
elevation band, slope band (min field present, always 0 until the spec grows
`slope_min_pct`), depth band, herd/shoal parameters and the bank reference,
per species — plus **the four-layer scatter table in the header**, so a
species can never be read against a layer table it was not filed under.
`voxelcore/assetmanifest.h` is the reader, on the `AssetParseError` model:
thirteen named refusals, including one no other parser here has — **the biome
names travel in the file and are verified by spelling against BiomeId order**,
because a silent reorder would turn every rainforest weight into a desert
weight and every one of those is a valid weight.

The export REPORTS what it bends, by name: 28 species authored tighter spacing
than their layer's cell (served at the cell pitch), 6 hero landmarks whose
900 m+ spacing folds below one per-mille (ABSENT from the world — per-mille
pick weights cannot express one-arch-per-3-km, and a scatter lattice should
not try; they need a landmark placement pass), and 2 species taller than every
layer (hero-sequoia, hero-arch-colossal — refused outright). The C++ fold
(`assetSpeciesTableFromManifest`) counts the same events on its side: over the
real manifest, **438 kept / 382 detail entities / 6 too rare / 0 no-biome**,
pinned loosely by `test_assetmanifest.cpp` against the fixture the exporter
actually wrote (`tests/fixtures/asset_species_v1.vxm`).

### 12.2 The banks: baked, validated by the tool that files them

`tools/export_banks.py` baked **688 VXA v3 files** — every terrain-kind
species except the four heavy heroes and the fold-to-zero landmarks, at four
seeds each — into `out/engine/banks/<name>/<name>-NNNN.vxa`, checking every
baked grid against its layer's declared box at bake time. That check earned
its keep immediately: **16 species refused on the first pass**, every one a
baked individual overshooting its authored nominal (worst +18%, small-leaved
lime 25 m authored, 29.6 m baked) or a hero wider than any sane dilation
(natural-arch: 45.2 m of reach). The layer table now files at nominal x1.3 and
its caps/radii are the MEASURED maxima over the full bake (52.5 m, 14.5 m),
not the spec's claims.

`voxelcore/assetbank.h` (`AssetBankLibrary`) is the production
`IAssetBankSource`: per-species-bank residency, sorted-filename seed order,
seedIndex reduced modulo the valid count, every file re-validated at load
(nine named refusals), and **misses are counters, not silence** — a species
whose bank never loads composes as air AND as a number, because "the table
loaded and nothing renders" must be a one-line diagnosis.

### 12.3 `Amplifier::surfaceInfo` and the gradient direction

Built as the architecture doc specified: surface, slope magnitude AND signed
per-axis gradient, biome and climate in one call, sharing one private
climate+dither helper with `column()` so a spawner and the world it spawns
into cannot disagree about a boundary (pinned bit-for-bit over a 2,500-column
sweep). The gradient direction — computed inside `evalSurface` since v9 and
thrown away — now survives; the uphill probe measures 98% agreement between
"step 15 m along the dominant component" and "the surface rose" on sloped
ground. Worldgen digest unchanged by all of this: `e02458de2be47309`.

### 12.4 The widening census, and what it did to the layer table

`vxc_assetprobe` (bench/assetprobe.cpp) is the measurement §9 said nobody had
taken. Over 32-voxel chunk footprints it reports the distribution of
`assetTopAboveSurfaceMm`, the **extra admitted chunk layers** against the
terrain-only bound (dilation slack and widening split out), and — with
`--banks` — the §10 placement counters and the anchor contact audit.

What it found, and what changed because of it:

* The architecture's illustrative table (canopy = 14 m) does not fit the
  authored library: **half of all trees are taller than 14 m at under 8 m
  spacing** (Sitka spruce: 45 m at 7 m). A 14 m canopy cap would demote 39 of
  78 trees to a 24 m lattice and gut every conifer forest.
* The first composition charged the widest layer's dilation to every
  footprint on the planet for a layer present on ~13% of them. The composed
  bound is now **per layer** — each terrain layer with a site in reach buys
  `bound(rect (+) its own radius) + its own cap` — strictly tighter, soundness
  argued at the function, pinned by the updated dilation tests and the
  unchanged adversarial dominance sweep.
* Tuned numbers (synthetic, 512 m square, seed 20260719): mean extra
  admitted chunk layers **22.98 → 17.02**; on the real wet-alpine fine tile
  (bake-out/-3_-3.vxtl): **24.58**, of which 17.2 chunks is dilation slack —
  steep ground pays reach x slope and no cap tuning touches that term.
* **The honest residue, for the owner:** even after tuning, a world with this
  library placeable costs ~17-25 extra admitted chunk layers per footprint
  against a surface shell of ~1.6-2.0 — an admission multiplier of roughly
  10x, paid everywhere the canopy lattice exists, vetoed or not. Capping the
  canopy at 14 m (priced with `--l1cap 14000`) only reaches 11.4 / 18.9,
  because the slack term survives. The two real levers are design work, not
  tuning: a SOUND climate-conditioned veto of the bound's terms (skip the
  canopy term only where no column in reach could classify tree-hosting —
  computable from the same raster stencil the bound already reads, with
  margins for dither and interpolation), and accepting that in forests the
  "extra" chunks are not waste at all (they hold crowns). Neither is built;
  the census is what makes the decision priceable.

The placement census over a 128 m square of real amplifier ground: 32,669
sites -> 9,898 instances, 34 species, **0 floating anchors of 9,898
audited** (independently re-read through `Amplifier::materialAt`, not
trusting the resolver), 1,635 bank-served terrain instances, 3.9 M stampable
voxels, 0 bank refusals.

### 12.5 The digest moved, deliberately

`vxc_bench --assets <dir>` installs the real manifest + banks and composes
the asset term into every brick and quad it digests, through the same
air-only monotone rule as `GeneratedWorld::makeBrick`:

* terrain-only: `e02458de2be47309` — unchanged, the terrain function is
  untouched;
* `--assets asset-forge/out/engine`: **`3b5fe7ec61c6581a`** — the ran-flag
  fires.

`kWorldGenVersion` is bumped **23 → 24** (core.h records the contract: from
here on the manifest bytes, bank bytes and layer table are worldgen input).
A new pinned golden
(`assetfield_installed_field_moves_the_world_digest_and_the_digest_is_pinned`)
digests a wooded region through `makeBrick` against the real baked
tundra-pine fixture and asserts both halves: installed-field digest differs
from bare, and equals `cfcd3f62789f4d0e` until somebody re-blesses it on
purpose.

### 12.6 `bathy_shore` answered, and `water_max_m` stays closed

From the bake code (`terrain_service/bake/basins.py::bathymetry_planes`,
called at `bake/pipeline.py:4963` with `survey.basins`): the wet set is
**lake basins only** — rivers are the separate graded water plane, the sea is
the datum and is never baked. So the shore-distance plane serves "reeds ring
a lake" and nothing else; wiring it into `distanceToWaterMm` would
deterministically starve every riverbank willow while looking wired. The gate
therefore still fails closed, unchanged from §9, and serving rivers needs the
bake's distance transform to include the water plane's wet cells — a
bake-format change, flagged for whoever owns bake_ver next.

### 12.7 Still unbuilt after this landing

* Ground-cover banks (the 266 kept species without banks — their `.vxa` are
  UE-side render assets, not world voxels) and everything §9 lists under
  "needs the editor": the UE admission gate wiring, per-chunk instancing, the
  wildlife spawner.
* The aquatic depth bands are still not AUTHORED (only widened in scope);
  the sequencing warning in §11.5 now has teeth, because a reseed is now a
  v24+ worldgen change.
* The landmark heroes need their own placement pass; per-mille pick weights
  cannot express them and this document refuses to pretend otherwise.
* The two design levers in §12.4 for the admission price, priced but not
  built.

## 10. Verification

> §12 supersedes the digest half of this section: as of the data landing the
> suite is at **697 pass, exit 0**, the terrain-only digest is still
> `e02458de2be47309`, and `vxc_bench --assets` produces `3b5fe7ec61c6581a` —
> the digest now moves exactly when the field is installed, which is what §9
> of the architecture doc demands.

```
cd voxel-core && cmake --build build --config Release --target vxc_tests
./build/tests/Release/vxc_tests.exe
```

* **Baseline before this work: 644 pass, 0 fail, exit 0.**
* **After: 684 pass, 0 fail, exit 0.** 40 new tests; every pre-existing test
  unchanged and still passing.
* **Worldgen digest unchanged: `e02458de2be47309` before and after**
  (`vxc_bench --radius 8 --digest`). This is the evidence that adding `biome`
  and `slopeMmPerM` to `ColumnSample`, extracting `topSolidVoxelZ`, fixing
  `rotatedOrigin*` and threading an (uninstalled) `AssetField` through
  `GeneratedWorld` changed no voxel.
  **And it is the correct result *at this stage only*.**
  `asset-placement-architecture.md` §9 is right that once a real layer table and
  manifest are installed the digest MUST move, and that an unchanged digest then
  means the field is not wired. Nothing installs an `AssetField` in production
  yet, so an unchanged digest here means exactly what it should: the mechanism
  is built, the data is not.
* Clean under MSVC `/W4 /WX` — no warnings in any new or changed file.
* Float-ban lint (CI's `float-ban` job, run locally) clean across every file
  this work touched. The only hits in the tree are pre-existing in
  `fluidoccupancy.h`.
* asset-forge **untouched**, so `python -m forge.cli selftest` (PASS at 824) and
  `python tools/buildcheck.py` (PASS at 828) are unaffected by construction —
  not a single file under `asset-forge/` was modified.

### A bug found in shipped code, outside the plan

`AssetGrid::rotatedOriginX` / `rotatedOriginY` (`src/assetgrid.cpp:244-263`) were
**off by one in four of their eight cases** — every case that negates an axis
subtracted the exclusive upper bound instead of the last index. They were wrong
from the day they were written and **nothing called them**: `atYaw` is thoroughly
tested and is self-consistent (its coordinates are zero-based inside the rotated
box, so it cannot notice an error in where that box sits in the world), and
`rotatedOrigin*` had no content to disagree with. The two only meet in a caller
placing a yawed instance, and there was none until `assetfield.h`.

The symptom, when it finally appeared, was **three of every four trees
invisible** — yaw 0 drew, yaws 1–3 sampled one voxel outside the box on the
negated axis and read the out-of-range `MAT_AIR` that `at()` answers by design.
No crash, no warning, just a thinner forest. Fixed, and pinned by
`assetgrid_rotated_origin_puts_a_yawed_box_where_atYaw_reads_it`, which asserts
the two against each other over a box with unequal extents and a negative origin
— a cube at the origin passes with all four bugs present.

---

## 13. v25 — the contract §9 declared was called in on 2026-08-16

Every digest above that reads `3b5fe7ec61c6581a` was measured against the
2026-08-15 export. That export is gone. `kWorldGenVersion` is now **25** and the
current pair, same conditions throughout (radius 8, seed 20260719, brick 16):

* terrain-only: **`e02458de2be47309`** — still, and this is the third version in
  a row it has not moved. The terrain function is untouched by all of this.
* `--assets asset-forge/out/engine`: **`41ec6bbf103f18dc`** (was
  `3b5fe7ec61c6581a`).

**What moved, none of it in this repo:**

* **Trunk taper.** A real missing term, not a tuning change — `skeleton._radii`
  derived thickness only from where branches fork, so an unforked bole kept its
  base radius to the crown and a birch measured a taper ratio of 1.00. Every
  tree in the 688-file bank is now a different tree.
* **`quad.eye`.** Float bounds on the only one of 34 `kind="int"` rows made spec
  validation non-idempotent, silently reseeding 192 specs — 9 trees and 34 rocks
  among them, all terrain kinds with banks.
* **25 quadrupeds off the 5 cm lattice.** Detail kinds, so no banks, but they
  are in the manifest and therefore in its bytes.

**How the staleness was found, since that is the part worth keeping.** Nothing
checked the export against its source. `export_banks.py` skipped a bake whenever
the **file existed**, so a spec could move any distance and its bank would go on
serving the old tree forever — the engine composing pre-taper trees with nothing
to say so. `asset-forge/tools/enginecheck.py` now stamps each species'
`spec_hash` at bake and compares; it caught this within hours of being written,
and `european-beech` differing by 5,920 bytes from its own spec is what a
"deliberate worldgen change" looks like when it is *not* deliberate.

**Fixtures.** `tests/fixtures/asset_species_v1.vxm` is refreshed, because a test
named `assetmanifest_reads_the_file_asset_forge_actually_wrote` should not read
a file asset-forge wrote a day ago. `asset_tundra_pine_0002.vxa` is deliberately
**not** refreshed: it is an *input* to the reader tests, which pin its exact
dimensions, origin, run count and voxel values. Refreshing it churns six
assertions to prove nothing about the reader, and the pinned golden
`cfcd3f62789f4d0e` is computed from it and so is unchanged.

Suite after: **697 pass, 0 fail.**

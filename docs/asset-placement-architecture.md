# Architecture: procedural placement of environmental assets

Date: 2026-08-15
Status: **BUILT — reviewed 2026-08-19.** Written as a plan; the 2026-08-15/19
programme implemented it, and in two places went past it (per-kind × per-biome
densities and named per-biome rule overrides, worldgen v28, which this document
does not describe — see `docs/placement-spec-schema.md`). Supersedes parts of
`docs/asset-streaming-design.md` — see §2.1, which contradicts that document's
wiring list on a point that matters. Read the scope paragraph below as the
starting condition, not the current one.

**Scope** *(the starting condition, 2026-08-15)*. 828 authored species (131
quadruped, 127 bird, 106 fish, 102 rock, 89 grass, 87 flower, 78 tree, 57 bush,
33 reed, 18 cetacean) and nothing spawns any of them. All but the 382 animals
(deferred until animation exists) now do. This says, for each of the owner's
four classes, what the rule reads, what it decides, where it runs, and **what
measurement catches each failure mode it introduces**.

## What has changed since `asset-streaming-design.md` (2026-08-10)

Two of that document's blockers are retired:

* The material append **landed**. `core.h` carries `MAT_BARK` (16) through
  `MAT_BEAK_HORN` (46), `kMaterialCount` = 47, and
  `AssetGrid::materialsWithinEngine()` now passes for real assets. Its §5.1 is
  done.
* The 5 cm / 10 cm conflict was resolved **by re-authoring, not by a bake-time
  reduction**. `forge/kinds.py` makes lattice a property of the KIND: terrain
  kinds (tree, rock) are 10 cm and `forge.cli.selftest` refuses anything else.
  Its §5.2 "any-solid downsample" is no longer needed.
* VXA is at **version 3**: `voxelSizeMm()`, `onTerrainLattice()`, per-voxel part
  ids and joint origins. The reader refuses v1 rather than guessing scale.

## 1. One substrate, four consumers

| class | reads | decides | runs at |
|---|---|---|---|
| **1. Trees & rocks** | scatter site (pure hash) + anchor column + species manifest | veto / species / anchor-z, then stamps voxels | **both** streaming admission (height bound only) and chunk generation |
| **2. Ground cover** | scatter site + the chunk's already-built column grid + shore plane | instance list per surface chunk | chunk generation, attached to the anchor chunk |
| **3. Land animals & birds** | biome/slope at candidate + herd/flock spec + ground raycast | transient spawn/despawn | entity spawn, near the player |
| **4. Fish & cetaceans** | water classification + depth band + shoal spec | transient spawn/despawn in a water volume | entity spawn |

Shared ingredients: the **scatter** (`assetplacement.h` as built — per-size-class
lattices, `assetSiteInCell` / `assetSitesForRect`, hash channels 50–61); the
**policy**, which obeys the veto-only rule and therefore never touches the
streaming bound; and a new **species manifest** baked by asset-forge that
finally reads the authored data.

## 2. Trees and rocks: a third term in the world function

### 2.1 The composition point is voxel-core, NOT the UE GridSampler

`asset-streaming-design.md` §6.3 says to wire the asset sample into the level-0
worker's `GridSampler` after `Amplifier::materialAt`. **That is necessary and
not sufficient**, and doing only it produces two silent defects:

1. **Digging one voxel of a tree deletes the rest of that brick's tree.**
   `World::applyToOverlay` materialises the overlay brick from
   `gen_.makeBrick(key)` and then applies the edit. If `GeneratedWorld::makeBrick`
   does not include the asset term, the first edit anywhere in an asset-bearing
   brick regenerates it as bare terrain — the tree's voxels in that 8³ region
   vanish, silently, replicated to every client.
2. **A tree that renders but is not there.** `World::materialAt` feeds
   `IsSolidAtVoxel`, raycasts, the region graph and collapse. Assets present only
   in the mesher's sampler are air to a raycast and to digging.

So: a new `vxc::AssetField` owns the layer table, the decoded species banks, the
manifest and the policy; `GeneratedWorld` takes an optional pointer and composes
it inside `makeBrick` / `materialAt`. The UE worker consults the same object.
This also puts assets inside `vxc_bench --digest` and the golden tests, which is
where a change whose failure mode is silent belongs.

Precedence, pinned by a test: overlay > asset > terrain, and an asset voxel
never replaces `MAT_BEDROCK`.

### 2.2 The anchor is verified, not derived

`anchorVz` is computed by the same formula `surfaceBrickRange` uses for topmost
solid — not a parallel derivation, the same rule. **Then check
`materialAt(col, anchorVz) != MAT_AIR` and veto if air**: the surface formula
ignores the carve passes, so a cave mouth or sinkhole shaft can make the
"topmost solid voxel" air and a tree anchored there floats over a hole. One
extra call on a column already in hand.

This is the project's "derived attachment eventually detaches" hazard in its
worst form — 828 species to do it to.

### 2.3 Policy: every step a veto

Needs `(surfaceMm, slope, BiomeId, climate)` at the anchor, none of which is
currently exposed. Add a public `Amplifier::surfaceInfo(vx, vy)` — one
`evalSurface` plus the climate read, cheaper than a full `column()`. Do **not**
widen `ColumnSample`; it is mirrored in `GpuColumnSample`.

Order: biome/species bank → elevation band → slope → spacing/abundance →
cluster field → anchor-in-carve check. Each an integer per-mille threshold
against a dedicated hash draw; register new channels in
`hash_channel_registry.h`, which exists precisely to catch channel reuse.

### 2.4 The bound stays sound, and the layer table is the price list

Nothing above touches the bound, by construction. But **the veto-only rule means
vetoed regions pay anyway**: `assetTopAboveSurfaceMm` widens wherever the
LATTICE has a site, including deserts where the policy vetoes every tree. With a
30 m layer at high density that is the "constant widening" the file's own header
rejects. So the table must be tiered so tall = sparse:

* L0 emergent — cell 24 m, max height 30 m, low density: rainforest emergents
  and hero boulders only.
* L1 canopy — cell 8 m, max height 14 m: most trees, large rocks.
* L2 small — cell 3 m, max height 5 m.
* L3 ground cover — cell ~0.6–1 m, never stamped, excluded from the terrain
  bound (§3).

**Measure before tuning.** A headless bench can report the widening
distribution per footprint over real tiles — counts, not clocks.

## 3. Ground cover: chunk-keyed permanent detail

Runs in the level-0 chunk generation worker, against the already-built 34×34
column grid rather than fresh `column()` calls. Instances attach to the anchor
chunk, so eviction removes them and regeneration reproduces them bit-identically
— no new residency system. Layer 3 is excluded from the terrain bound soundly:
the bound exists to prove chunks all-air so generation can be skipped, ground
cover contributes no terrain voxels, and its anchor chunk is a surface chunk
which is never skipped.

Rendering is the real open cost: one-time greedy mesh per `(species, seed)` from
the `AssetGrid`, instanced per chunk on the pattern `VoxelAgentSubsystem` and
`AVoxelDebris` already use.

Reeds ring shorelines by reading the bake's signed shore-distance plane
(`bathy_shore`) — positive in water, negative on land, exactly
`placement.water_max_m`'s semantics.

Class 2 is part of the worldgen contract but **not** the terrain digest — give
it its own golden so a placement change and a terrain change cannot mask each
other.

## 4. Clustering: cheap where it must be, honest where it is not

Clustering never runs on the admission path, because it is implemented entirely
as vetoes and the bound is policy-independent. Three tiers:

1. **Stand/grove field** — bilinear blend of corner hashes on a coarse lattice,
   two octaves (~96 m forest density, ~24 m grove). A site survives iff the
   field clears a threshold set by `placement.cluster`, with abundance
   renormalised so mean density is conserved. 4–8 hashes per site,
   generation-time only.
2. **Directional clustering — scree below cliffs.** Not expressible as an
   isotropic field. The carrier's analytic gradient DIRECTION is already
   computed inside `evalSurface` and only its magnitude survives; expose it.
   Rock sites get a density boost where the local grade is 25–70% and a probe
   ~15 m upslope reads above the 70% BARE_ROCK gate. Two surface evals buys the
   named look.
3. **Edge clustering** — reeds at the water line via `bathy_shore` bands. The
   bake already computes this globally because it does not decompose per column.
   Do not re-derive it.

**Unsolved and said plainly:** cross-site interaction (no small tree under a big
one) is not computable site-locally. v1 lets understory test the coarser layer's
LATTICE OCCUPANCY only — pure hash, no policy — which over-vetoes but stays
veto-only.

**Measurement:** clustering is exactly the feature that can silently no-op. Pin
it with a statistic, not a screenshot: per-annulus pair counts over
`instancesForRect` must exceed the Poisson expectation by a pinned margin for a
clustered species, and must not for an unclustered one.

## 5. Slope, elevation, biome, water

Precedence mirrors `classifyBiome`'s own gate order, because that order is
already the world's truth: water first (a submerged anchor rejects all land
kinds), then slope (>70% grade is BARE_ROCK and hosts only what `biomes.py`
says), then elevation band within a biome, then biome weight, then water
proximity.

## 6. Salt versus fresh — found, not invented

The code already distinguishes everything the specs need:

| spec value | mechanism | salinity |
|---|---|---|
| `ocean` | sea datum, `ground < kSeaLevelMm` | salt |
| `lake` | basin table, `holdsWater()` | fresh (but see below) |
| `river` | river plane / ribbon; graph in `rivernet.h` | fresh |
| `shallow` | `bathy_shore` small-positive, or small `kSeaLevelMm − surface` | either |
| `reef` | sea-datum water whose seabed is within N m of the surface | salt |

The `reef` spec row calls itself "a request for" a classification the world
lacks. **It does not lack it** — no new world data is needed.

One refinement the wire already carries: **`kBasinLakeTerminal` is an endorheic
lake**, which is the saline kind on Earth. v1 treats all water-holding basins as
fresh; carry the terminal bit through the manifest so a later pass can keep
trout out of salt lakes without re-plumbing.

**Spawn-volume verification:** a fish spawns only where ground is below the
datum by at least the species' minimum depth — the same composition rule that
keeps lakes from floating. Count both sides: placed and rejected-dry. Zero
placed against a large queried is a wiring fault, and the counters make it a
number.

## 7. Transient detail entities

Deterministic eligibility, stochastic instances. **Where** a species can occur
and how densely is a pure function of `(seed, position)`, so two clients agree
this valley is deer country; **which individuals exist right now** is runtime
RNG, never persisted. That is what `detail.entity_class = "detail"` promises,
and it keeps classes 3–4 out of the worldgen version contract entirely.

Aquatic spawner: enumerate water, do not search for it — the `farwater.h`
lesson. Shoals spawn as one decision, N consecutive seeds.

## 8. The authored data, and a sequencing point that matters

Established from `spec.py`: spec bodies carry every group regardless of kind, and
the hash covers the whole body. So **changing a parameter row's `kinds=` scoping
or bounds changes no spec bytes and reseeds nothing**; authoring a value reseeds
that species; adding or removing a field reseeds the library.

1. **Widen scoping, not schema**: add the ocean-hosted plant kinds to
   `detail.depth_*`, and widen `placement.elev_min_m`'s −10 m floor to −200 m.
   Zero reseed. Today a kelp forest that reaches −30 m in reality cannot say so.
2. **Author the aquatic depth bands NOW, before anything reads them.** Each edit
   reseeds that species, which today costs nothing because no world contains
   these instances. **The moment placement ships, a reseed becomes a world
   change behind `kWorldGenVersion`.** This applies to all placement tuning: do
   the authoring sweep first.
3. Do not move `placement.*` / `biomes.*` into `SEED_INVARIANT` yet — but if
   habitat tuning is ever to be decoupled from appearance, the deadline is
   placement's ship date, and it should be one deliberate library-wide reseed
   rather than a series.
4. Export the manifest from forge as a versioned binary table with a
   parse-refusal path, on the `AssetParseError` model.

## 9. Determinism

Classes 1–2 are part of `f(seed, x, y, z)`. Any change to the layer table,
manifest, policy or cluster field is a worldgen change: bump
`kWorldGenVersion`, re-bless goldens deliberately. The veto-only rule's payoff is
precisely that these frequent changes never require re-auditing the streaming
bound — they cost a version bump, not a hole-in-the-world risk.

**The digest must see assets**, and the first landing SHOULD change it. A
landing that leaves the digest unchanged means the field is not wired — the
digest doubles as the ran-flag.

## 10. Failure modes and their instruments

| failure | measurement |
|---|---|
| wired but nothing stamps | counters: sitesConsidered / vetoed-by-reason / stamped / refused-by-gate; and an unchanged worldgen digest means not wired |
| tree floats over a cave mouth | anchor `materialAt != MAT_AIR` veto, plus a contact-audit bench asserting 100% anchors solid-below over N instances on real tiles |
| crown truncated by the bound | `assetLayerAdmitsHeight` at bake AND at load; the adversarial sloping-surface sweep extended to the real policy, keeping its vacuous-truth guard |
| renders but not solid | identity test: `makeBrick` vs the mesher-sampler path must match voxel-for-voxel |
| edit near a tree deletes brick-mates | unit test on `applyEdit` in an asset-bearing brick |
| clustering silently Poisson | the pair-count statistic, asserted in both directions |
| fish in dry ground | spawn-volume assert plus placed/rejected counters |
| bound too expensive rather than too tight | widening-distribution bench before wiring; admitted-chunk-count A/B with assets on and off |

## 11. Sequencing

**Headless now, no editor:** forge data-shape fixes and the authoring sweep
(free only until placement ships) → `Amplifier::surfaceInfo` → `vxc::AssetField`
with policy, anchor and composition into `GeneratedWorld` → cluster field and
its statistic → `vxc_assetprobe` bench → ground-cover enumeration and its golden
→ bake the terrain-kind library into VXA v3 banks.

**Needs the editor:** UE wiring with the §2.1 correction; VXA→mesh bake and
per-chunk instancing for ground cover; the wildlife spawner subsystem.

## 12. What could not be determined from the code

* Whether `bathy_shore`'s wet set includes river and sea shorelines or lake
  basins only — check the terrain-service codec before relying on it for
  riverbank species.
  **ANSWERED 2026-08-15, from the bake code: LAKE BASINS ONLY.**
  `terrain_service/bake/basins.py::bathymetry_planes` builds the wet mask
  exclusively from basin records (`for r in records: if not r.is_lake:
  continue`), and `bake/pipeline.py:4963` feeds it `survey.basins` — the
  depression survey. Rivers live in the separate graded water plane
  (SECTION_WATER, built in B6, after the bathymetry pass), and the sea is
  never baked at all (it IS the datum, `lakes.h:1391`). So `bathy_shore`
  answers "distance to the nearest LAKE shoreline": right for reeds ringing a
  lake, wrong for a riverbank willow and for beach species, and §3's claim
  that it carries `placement.water_max_m`'s semantics is TRUE ONLY FOR LAKES.
  Per the design doc's §9 the gate stays failing closed (a species with
  `waterMaxMm > 0` is refused when the distance is unknown) rather than being
  wired to a lake-only answer that would deterministically starve every
  riverbank species; serving rivers needs the distance transform run over
  `wet |= water plane` at bake, which is a bake-format decision, not a
  client-side one.
* Whether `validate()` fills missing params into loaded bodies (affects only the
  reseed accounting in §8; the giant-kelp body carrying every group suggests
  yes).
* All UE file:line references are as recorded on 2026-08-10 and may have
  drifted.
* Whether any render-path assumption ties detail primitives to admitted chunk
  volumes — an editor question.

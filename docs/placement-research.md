# Placement research: trees and rocks on procedural terrain

**Date:** 2026-08-17. **Scope:** outside knowledge only — industry talks, academic papers, and geomorphology, each mapped onto our placement system. A parallel audit of the placement code itself is a separate document. **No code was changed for this doc.**

## Our system (baseline, taken as given)

Deterministic hash-lattice site enumeration per layer (24 m / 5 m / 2.2 m cells); per-species per-biome weights x abundance folded into a weighted pick; occupancy cap per site; two-octave multiplicative noise "grove field" for clustering (measured index of dispersion ~1900 vs 851 uniform); tolerance gates on elevation band + slope band + (stubbed) distance-to-water; layer-level density per-mille. Hard constraint: placement stays a pure function of (seed, tile rasters) for multiplayer determinism — no iterative global simulation at runtime. **Baked channels computed offline per tile are allowed** (hydrology/water rows are already baked this way).

Two owner-visible defects anchor everything below:

- **(a) Trees underwater in a lake.** No standing-water veto today. Fine columns already carry `waterSurfaceMm`, so the veto is one gate away.
- **(b) Mountainsides and ridgelines nearly bare.** Authored `slope_max` ~45% while real conifer forests routinely hold 60–80% slopes; the temp-adjusted treeline may also be binding earlier than intended.

Effort tiers used throughout: **data-only** (authoring/tuning, no new code), **bake tool** (new offline per-tile raster), **voxel-core policy** (runtime placement-rule change).

---

## 1. Industry practice (GDC / engine frameworks)

### 1.1 Horizon Zero Dawn — density maps sampled by a blue-noise-like pattern

Jaap van Muijden, *GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn*, GDC 2017. [GDC Vault](https://www.gdcvault.com/play/1024120/GPU-Based-Procedural-Placement-in) · [slides at Guerrilla](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)

The canonical modern pipeline. Artists author a **node graph per "ecotope"** (biome) that combines terrain channels — **slope, elevation, orientation, painted ecotope masks, distance to roads/water** — into a **density map per asset type**. The runtime samples a precomputed low-discrepancy point pattern (blue-noise-like dither) against the density map: a point survives where density exceeds its threshold rank. Large-footprint assets are placed first and **stamp suppression into the density maps of smaller assets**, so trees push out bushes push out grass. Everything is a pure function of world position + authored data, so the world regenerates identically around any player — the same determinism contract we have.

**Maps to us:** our grove field x biome weights ≈ their density map; our hash lattice ≈ their dither pattern. The two ideas worth stealing are (1) *density is a product of many cheap channel responses, each authored as a curve, not a hard band*, and (2) *cross-layer suppression*: a placed 24 m-layer tree should damp 2.2 m-layer shrub density in its footprint. Determinism-safe: **yes** (their whole point). Effort: response curves = data-only + small voxel-core policy; cross-layer suppression = voxel-core policy (read coarser-layer picks, which are already deterministic, when placing finer layers).

### 1.2 Far Cry 5 — biome-driven placement fed by baked hydrology and cliff data

Etienne Carrier, *Procedural World Generation of Far Cry 5*, GDC 2018. [GDC Vault](https://www.gdcvault.com/play/1025215/Procedural-World-Generation-of-Far)

Houdini-based offline tools "generate biomes, texture the terrain, setup freshwater networks, generate cliff rocks and more" across 100 km². The pipeline bakes terrain-derived data — water flow, moisture, slope/cliff classification — and vegetation/rock placement consumes those baked rasters per biome. Notably, **cliff rocks are a first-class procedural output of the terrain pass**, not hand-scattered props, and the freshwater network feeds vegetation conditioning.

**Maps to us:** this is the strongest industry endorsement of our "bake channels offline, gate at placement" architecture. Their moisture-from-flow is our topographic wetness index proposal (§3.1); their cliff-rock pass is our talus channel (§4.1). Determinism-safe: **yes** (all offline). Effort: bake tool.

### 1.3 Witcher 3 / REDengine 3 — procedural vegetation coverage offline and in realtime

Marcin Gollent, *Landscape Creation and Rendering in REDengine 3*, GDC 2014. [GDC Vault](https://www.gdcvault.com/play/1020197/Landscape-Creation-and-Rendering-in)

Vegetation coverage "procedurally generated both offline and in real-time" from terrain material layers, so artists never hand-place ground cover. The transferable idea: **vegetation reads the terrain's material/texture decision**, coupling "what the ground looks like" and "what grows on it" through one shared channel, so grass never grows on a rock texture.

**Maps to us:** if our surface voxel material (rock vs soil vs scree, from curvature/slope in §3.3) becomes a baked channel, both rendering and placement should read the *same* channel — one authority, no derived-not-verified drift. Determinism-safe: **yes**. Effort: bake tool + data-only weights.

### 1.4 Ghost of Tsushima — procedural grass, art-directed by channels

Eric Wohllaib, *Procedural Grass in Ghost of Tsushima*, GDC 2021. [GDC Vault](https://www.gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass)

Mostly a GPU rendering talk (blade-level generation), but the placement layer conditions grass type/height/color on terrain and painted maps to make procedurally identical systems read as distinct, art-directed fields. Relevant lesson for us at the 2.2 m layer only: **within-species parameter variation (height/lean/color) driven by the same channels as placement** is very cheap believability. Determinism-safe: **yes** (hash-driven variation). Effort: data-only, and only worth it after the two defects are fixed.

### 1.5 Unreal Engine PCG framework — the common vocabulary

[UE PCG overview, Epic docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-overview)

The generic modern pattern, useful as a checklist: surface-sample points → points carry **density, steepness, height, seed** attributes → filter/modulate density by attribute curves and noise → self-pruning by bounds overlap → spawn. Two checklist items we lack: **density filtering by attribute curve** (we use hard min/max bands — see defect (b)) and **self-pruning by footprint** (our occupancy cap is per-site, not per-neighborhood). Determinism-safe: yes. Effort: voxel-core policy.

---

## 2. Academic ecosystem placement

### 2.1 Deussen et al. 1998 — competition, self-thinning, succession

Deussen, Hanrahan, Lintermann, Měch, Pharr, Prusinkiewicz, *Realistic modeling and rendering of plant ecosystems*, SIGGRAPH 1998. [PDF](http://algorithmicbotany.org/papers/ecosys.sig98.pdf)

The founding paper. Plants are seeded stochastically, grow, and **compete for space: when two circles overlap, the smaller/shade-intolerant one dies** (self-thinning). Succession emerges: fast pioneers get replaced by shade-tolerant climax species. Output distributions have the signature look of real forests: clustered but with even spacing inside clusters, size-correlated gaps, edge effects near water.

**Maps to us:** the full simulation is iterative and global — banned at runtime, but **fine as an offline bake with fixed iteration count and seeded RNG** if we ever want per-tile "forest maturity" maps. The cheap distillation, though, is what matters: *spacing should scale with plant size* (a mature 24 m-layer tree suppresses neighbors over a radius its own crown implies). Our per-site occupancy cap approximates this only within one cell. Determinism-safe: baked = **yes**; runtime iterative = no. Effort: distilled policy = voxel-core policy; full bake = bake tool (large).

### 2.2 FON — field of neighborhood competition

Berger & Hildenbrandt, *A new approach to spatially explicit modelling of forest dynamics: spacing, ageing and neighbourhood competition of mangrove trees*, Ecological Modelling 132 (2000). [DOI](https://doi.org/10.1016/S0304-3800(00)00298-2)

Each tree projects a **circular "field of neighborhood" whose radius scales with stem size**; competition strength at a point is the sum of overlapping fields; establishment probability of a new tree is reduced where the summed field is high. It is the ecology-standard formalization of "big trees make a dead zone for saplings, proportional to their size."

**Maps to us:** a deterministic, *local-window* FON-lite: when the fine layer places, it sums an analytic suppression kernel from already-decided coarse-layer picks within one cell ring, and folds that into the weighted pick as a multiplier. No iteration, no global state — reads only (seed, rasters, coarser-layer results), which are already pure. This is the same mechanism as HZD's footprint stamping (§1.1) wearing an ecology hat. Determinism-safe: **yes**. Effort: voxel-core policy.

### 2.3 Cordonnier / Galin et al. — authored ecosystems over terrain

Cordonnier, Galin, Gain, Benes, Guérin, Peytavie, Cani, *Authoring Landscapes by Combining Ecosystem and Terrain Erosion Simulation*, ACM TOG (SIGGRAPH 2017); and Kapp, Gain, Guérin, Peytavie, Galin, *Data-driven Authoring of Large-scale Ecosystems*, ACM TOG (SIGGRAPH Asia 2020). [Galin publication list with PDFs](https://perso.liris.cnrs.fr/eric.galin/articles.html)

The 2017 paper couples ecosystem simulation with erosion so vegetation and landform shape each other; its channel set is the field's consensus: **moisture, sunlight/insolation, temperature, slope, soil presence** drive viability per species. The 2020 paper is more directly usable: it **distills expensive simulations into compact density + neighborhood statistics learned from data, then synthesizes plant distributions fast** — i.e., you don't run the simulation at placement time, you bake its statistical fingerprint.

**Maps to us:** validates our exact split (expensive stuff offline, cheap pure function at runtime). Concretely: per-(biome, species) *pair-correlation targets* (how clumped, at what radius) could be authored or distilled offline into grove-field octave settings and FON-lite radii, instead of hand-tuning one global grove field for all species. Determinism-safe: **yes**. Effort: data-only (reuse grove field machinery per species) up to bake tool.

### 2.4 Multi-class blue noise & per-class radii Poisson disk

Wei, *Multi-class blue noise sampling*, SIGGRAPH 2010. [Project page](https://www.liyiwei.org/papers/noise-sig10/) · Bridson, *Fast Poisson disk sampling in arbitrary dimensions*, SIGGRAPH 2007 sketch. [PDF](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf)

Wei: sample **multiple classes with a per-class-pair minimum-distance matrix** so each species AND the union of all species are well-spaced (blue noise). This is the principled answer to "oaks shouldn't touch oaks, but an oak and a fern can be close."

**Maps to us:** we should *not* adopt the sequential/dart-throwing algorithms directly (order-dependent, awkward across tile borders). The deterministic route to the same statistics: our per-layer hash lattices *are* per-class samplers — giving **each species (or species group) its own lattice phase offset and cell size derived from its footprint radius**, plus the FON-lite cross-class suppression (§2.2), approximates the conflict matrix with zero iteration. Determinism-safe: **yes** as lattices; classic dart-throwing per tile = yes only if seeded and border-padded, but not recommended. Effort: voxel-core policy.

### 2.5 Recursive Wang tiles — deterministic blue noise against a density map

Kopf, Cohen-Or, Deussen, Lischinski, *Recursive Wang tiles for real-time blue noise*, SIGGRAPH 2006. [Project page](https://johanneskopf.de/publications/blue_noise/)

Precomputed tiles of **progressive, ranked blue-noise points**: intersect the point ranks with any density map and you get blue-noise placement at exactly that density, O(1) per query, fully deterministic, infinite and non-periodic. This is the academically clean version of what HZD ships.

**Maps to us:** the drop-in upgrade *if* our hash lattice ever shows visible grid alignment at 24 m spacing (worth one A/B render — the visual-bisection method — before adopting). One precomputed tile set becomes an asset; site enumeration reads (tile id, rank threshold = density) instead of hashing lattice cells. Same purity, better spectrum. Determinism-safe: **yes**. Effort: voxel-core policy (medium; only on visual evidence of lattice artifacts).

---

## 3. Terrain-derived channels we can BAKE from data we already have

These are the highest believability-per-effort items in the literature because every industry pipeline above conditions on some subset of them, and we already bake hydrology per tile.

### 3.1 Topographic wetness index (TWI) — moisture from flow accumulation / slope

Beven & Kirkby's TOPMODEL wetness index, `TWI = ln(a / tan β)` where `a` is upslope contributing area per unit contour width and `β` is local slope. [Overview](https://en.wikipedia.org/wiki/Topographic_wetness_index)

Predicts where soil stays wet: hollows and valley floors with big catchments score high, steep convex ridges score low. It is *the* standard moisture proxy in ecology and in Far Cry 5-style pipelines, and we already have flow accumulation in the hydrology pyramid — the bake is one raster op over data we own.

**Maps to us:** new baked per-tile channel `twi` → (i) modulates pick weights per species (willow/alder weight rises with TWI, pine falls), (ii) replaces half of what the stubbed distance-to-water gate was for. Directly attacks defect (b)'s *feel*: ridgelines stay sparser than hollows for a reason the eye reads, instead of a hard slope cliff. Determinism-safe: **yes** (offline raster). Effort: **bake tool** (small — inputs already exist).

### 3.2 Aspect + insolation — south-slope dryness, north-slope forest

McCune & Keon, *Equations for potential annual direct incident radiation and heat load*, J. Veg. Sci. 13 (2002). [DOI](https://doi.org/10.1111/j.1654-1103.2002.tb02087.x)

A closed-form **heat load index from latitude, slope, aspect** (folding aspect about the SW-facing maximum). In the real northern hemisphere, north and east faces hold denser, moister forest; south/southwest faces run drier, grassier, with higher treelines of drought-tolerant species. Analytic, no simulation.

**Maps to us:** computable per column from the normal we already have — arguably doesn't even need a bake, but baking `heatload` keeps runtime pure-lookup. Use as a pick-weight multiplier per species and as a **modifier on the treeline band** (treeline higher on warm aspects). Cheap, and it breaks up the "uniform sock of forest" look on mountains. Determinism-safe: **yes**. Effort: **data-only to small bake tool**.

### 3.3 Curvature — ridges shed, hollows collect (soil depth proxy)

Zevenbergen & Thorne, *Quantitative analysis of land surface topography*, Earth Surf. Proc. Landforms 12 (1987). [DOI](https://doi.org/10.1002/esp.3290120107)

Standard plan/profile curvature from a heightfield 3x3 stencil. Convex (ridge, nose) positions shed water and soil → thin rocky soil, stunted or absent trees, **exposed rock**; concave (hollow, foot-slope) positions accumulate both → deep soil, the biggest trees. Ecology calls this the topographic position / soil-depth gradient.

**Maps to us:** new baked channel `curvature` (or a classified `soil` channel from curvature+slope) → tree pick weights and *max-size/species selection* (big species need concave/flat sites), and the inverse for rocks: **convexity is a rock-density multiplier** (§4.2). Also the shared channel Witcher-style surface texturing should read (§1.3). Determinism-safe: **yes**. Effort: **bake tool** (small stencil op).

### 3.4 Distance-to-water — riparian bands and un-stubbing the gate

Un-glamorous but load-bearing: a per-tile **distance transform from lake extents + river polylines** (both already exist in the bake). Riparian species (willows, sedges, cottonwood analogues) live in a 1–2 cell band; upland species avoid it; rocks along rivers become cobbles (§4.3). Remember the lake-extent rounding rule: for *vetoes* the water mask must over-cover; for "near-water bonus" bands, under-cover, so the bonus never lands in open water.

**Maps to us:** fills the stubbed `distance-to-water` gate with real data; adds a riparian weight bump. Determinism-safe: **yes**. Effort: **bake tool** (distance transform is standard; inputs exist).

### 3.5 Standing-water veto — defect (a), one gate

Needs no citation beyond every pipeline in §1 doing it implicitly (HZD/UE PCG both filter on water/wetness channels). We already carry `waterSurfaceMm` per fine column. Gate: **veto any tree site whose ground is below the local water surface** (with a small emergent-shore tolerance band, e.g. allow reeds/mangrove-analogue species only, if we want life at the margin rather than a sterile ring). Determinism-safe: **yes**. Effort: **voxel-core policy, one gate** — the single highest believability-per-effort item in this document.

### 3.6 Treeline + slope response — defect (b)

Two literature anchors. (1) Real-world treeline is a **growing-season temperature isotherm (~6–7 °C)**, with krummholz (stunted, shrub-form trees) in a transition band below the hard line, and treeline *locally raised on warm aspects and lowered in cold-air-drainage hollows* — see [tree line overview](https://en.wikipedia.org/wiki/Tree_line) (Körner's work is the standard reference). (2) Closed conifer forest routinely occupies **60–80% slopes** (31–39°); the true hard limit for trees is closer to soil failure / cliff angles (~45°+ slope *angle*, not percent) than to our authored 45% cap.

**Maps to us:** (i) raise authored `slope_max` toward ~70–80% for conifer-analogues (data-only), better: replace the hard band with a **response curve** — full weight to ~55%, tapering to zero by ~90–100% (voxel-core policy shared with §1.1/§1.5); (ii) make the treeline a **band, not a line**: below-treeline full forest, then a krummholz band (smaller assets, lower density), modulated by heat load (§3.2); (iii) verify which gate is actually binding on the bare ridges before tuning — measure, don't guess, per the plateau-area lesson. Determinism-safe: **yes**. Effort: **data-only + small voxel-core policy**.

---

## 4. Rock-specific models

### 4.1 Talus / scree cones below cliffs

Scree slopes form below rock faces by mass wasting and settle at the **angle of repose, ~34–38°**, forming cones below chutes and aprons below faces; supply scales with the cliff area above. [Scree overview](https://en.wikipedia.org/wiki/Scree). Far Cry 5's cliff-rock pass (§1.2) is the game-industry version.

**Maps to us:** bake a `talus` channel: for each column, deterministic downhill propagation of "debris flux" seeded where slope exceeds a cliff threshold (this is a *local, fixed-pass* raster op like our hydrology bake, not a runtime simulation), deposited where slope falls back under ~38°. Rocks' pick weight and size distribution scale with talus flux; below big cliffs you get dense cobble-to-boulder fields for free, exactly where the eye demands them. Determinism-safe: **yes** (offline). Effort: **bake tool** (medium).

### 4.2 Boulder frequency vs slope and curvature

Field observation across geomorphology: exposed boulders concentrate on **convex, high-slope, thin-soil positions** (ridge noses, cliff bases) and are rare on deep-soil concave positions — the exact inverse of big trees (§3.3). So one baked curvature channel serves both: `rock_weight ∝ convexity × slope`, `big_tree_weight ∝ concavity`.

**Maps to us:** rock species weights read `curvature` + `slope` with the opposite sign from trees; no new channel beyond §3.3. Also anti-correlate rocks with the grove field (rocks in the gaps between groves reads naturally; rocks *inside* dense groves reads wrong). Determinism-safe: **yes**. Effort: **data-only** once §3.3 exists.

### 4.3 Floodplain cobbles and downstream sorting

Rivers sort sediment: transport competence falls with velocity, so **grain size decreases downstream** (classic exponential downstream fining, Sternberg 1875) and, at a given reach, floodplains carry **cobble/gravel bars on the inside of bends and along active channels** — the [Hjulström curve](https://en.wikipedia.org/wiki/Hjulstr%C3%B6m_curve) is the standard velocity-vs-grain-size relation. Peytavie et al., *Procedural Riverscapes* (CGF 2019) is the graphics treatment of river-corridor detail ([Galin list](https://perso.liris.cnrs.fr/eric.galin/articles.html)).

**Maps to us:** we now carry real discharge Q up the pyramid. Rock species selection near rivers: within the riparian distance band (§3.4), pick cobble/gravel assets with **size decreasing as local Q increases downstream-wise** (or simply: cobbles-not-boulders in floodplains, boulders only where slope is high — mountain reaches). Determinism-safe: **yes**. Effort: **data-only** once §3.4 exists.

### 4.4 Rock piles as assets — aperiodic tiling

Peytavie, Galin, Grosjean, Mérillou, *Procedural Generation of Rock Piles Using Aperiodic Tiling*, CGF (Pacific Graphics 2009). [Galin list](https://perso.liris.cnrs.fr/eric.galin/articles.html)

Generates convincing *piles* (stacked, contact-resolved rocks) from precomputed aperiodic tile sets rather than physics. For us this is an **asset-forge concern, not a placement concern**: authoring a few multi-rock "pile" assets and letting the talus channel place them beats placing many single rocks and hoping they cluster. Determinism-safe: **yes**. Effort: **data-only** (new asset archetypes) — noted here, belongs to asset-forge's queue.

---

## 5. Ranked shortlist (believability per effort)

| # | Recommendation | Fixes / adds | Channels needed (existing → new) | Determinism | Effort |
|---|----------------|--------------|----------------------------------|-------------|--------|
| 1 | **Standing-water veto**: veto tree sites with ground below local water surface; optional reed-only shore band (§3.5) | Defect (a), owner-visible | `waterSurfaceMm` (existing) → none | yes | voxel-core policy (one gate) |
| 2 | **Slope response curves + treeline band**: replace hard `slope_max` 45% with per-species taper to ~80–100%; krummholz band below treeline; verify which gate binds first (§3.6) | Defect (b), owner-visible | slope, temp-adjusted treeline (existing) → none | yes | data-only + small policy |
| 3 | **TWI moisture channel** from hydrology flow accumulation / slope; species weights read it (§3.1) | Ridge-vs-hollow believability; groundwork for all moisture species | flow accumulation, slope (existing) → `twi` | yes | bake tool (small) |
| 4 | **Distance-to-water raster** from lake extents + river polylines; un-stub the gate; riparian species band (§3.4) | Riparian bands; completes a stubbed gate | lake extents, river polylines (existing) → `dist_water` | yes | bake tool (small) |
| 5 | **Talus channel for rocks**: fixed-pass debris flux from cliff-threshold slopes, deposit under ~38°; rock density/size reads it (§4.1) | Rocks appear where the eye expects them; mountains stop being empty *and* clean | slope raster (existing) → `talus` | yes | bake tool (medium) |
| 6 | **Curvature channel**: convex → rocks + stunted trees, concave → deep soil + big trees; share with surface texturing (§3.3, §4.2) | Ridgeline character; rock/tree anti-correlation for free | heightfield (existing) → `curvature` | yes | bake tool (small) |
| 7 | **Aspect/heat-load multiplier** (McCune-Keon): species mix + treeline offset by slope/aspect (§3.2) | Breaks uniform "forest sock" on mountains | column normals (existing) → optional `heatload` | yes | data-only / tiny bake |
| 8 | **FON-lite cross-layer suppression**: coarse-layer picks damp fine-layer density in a size-scaled radius (HZD stamping ≡ FON) (§1.1, §2.2, §2.4) | Correct spacing statistics; understory reads as understory | coarser-layer picks (existing, deterministic) → none | yes | voxel-core policy (medium) |

Items 1–2 are the two owner-visible defects and are almost free. Items 3–6 are one small bake-tool wave over rasters we already own — together they give mountains, ridges, hollows, and riverbanks each a *reason* to look the way they do. Items 7–8 are polish with solid literature behind them.

**Deliberately not recommended:** runtime ecosystem simulation of any kind (Deussen-style succession, dart-throwing Poisson) — banned by the determinism contract and unnecessary: every effect they produce that the eye can see (clustering, size-spacing correlation, species sorting) is reachable via baked channels + local suppression, which is also what HZD and Far Cry 5 shipped. Recursive Wang tiles (§2.5) are the sanctioned replacement *if and only if* an A/B render shows lattice alignment artifacts.

## Sources

- van Muijden, *GPU-Based Run-Time Procedural Placement in Horizon Zero Dawn*, GDC 2017 — [GDC Vault](https://www.gdcvault.com/play/1024120/GPU-Based-Procedural-Placement-in), [Guerrilla slides](https://www.guerrilla-games.com/read/gpu-based-procedural-placement-in-horizon-zero-dawn)
- Carrier, *Procedural World Generation of Far Cry 5*, GDC 2018 — [GDC Vault](https://www.gdcvault.com/play/1025215/Procedural-World-Generation-of-Far)
- Gollent, *Landscape Creation and Rendering in REDengine 3*, GDC 2014 — [GDC Vault](https://www.gdcvault.com/play/1020197/Landscape-Creation-and-Rendering-in)
- Wohllaib, *Procedural Grass in Ghost of Tsushima*, GDC 2021 — [GDC Vault](https://www.gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass)
- Epic Games, *Procedural Content Generation Overview* — [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-overview)
- Deussen et al., *Realistic modeling and rendering of plant ecosystems*, SIGGRAPH 1998 — [PDF](http://algorithmicbotany.org/papers/ecosys.sig98.pdf)
- Berger & Hildenbrandt, FON competition model, Ecological Modelling 2000 — [DOI](https://doi.org/10.1016/S0304-3800(00)00298-2)
- Cordonnier et al. 2017; Kapp et al. 2020; Peytavie et al. 2009 (rock piles); Peytavie et al. 2019 (riverscapes) — [Galin publications](https://perso.liris.cnrs.fr/eric.galin/articles.html)
- Wei, *Multi-class blue noise sampling*, SIGGRAPH 2010 — [project page](https://www.liyiwei.org/papers/noise-sig10/)
- Kopf et al., *Recursive Wang tiles for real-time blue noise*, SIGGRAPH 2006 — [project page](https://johanneskopf.de/publications/blue_noise/)
- Bridson, *Fast Poisson disk sampling*, SIGGRAPH 2007 — [PDF](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf)
- Beven & Kirkby topographic wetness index — [overview](https://en.wikipedia.org/wiki/Topographic_wetness_index)
- McCune & Keon, heat load index, J. Veg. Sci. 2002 — [DOI](https://doi.org/10.1111/j.1654-1103.2002.tb02087.x)
- Zevenbergen & Thorne, surface curvature, ESPL 1987 — [DOI](https://doi.org/10.1002/esp.3290120107)
- Treeline ecology (Körner) — [overview](https://en.wikipedia.org/wiki/Tree_line); scree / angle of repose — [overview](https://en.wikipedia.org/wiki/Scree); Hjulström curve — [overview](https://en.wikipedia.org/wiki/Hjulstr%C3%B6m_curve)

# Biome placement survey — every land biome, censused and mapped (2026-08-18)

The owner's ask: screenshots of vegetation placement plus top-down overlay maps
from every biome, with the expectation that **each scene shows all vegetation
asset types (trees, grass, bushes, flowers, ground cover) and rocks — no
animals**. This document is the data half: the cross-biome census, the overlay
maps, and the placement problems the census surfaced. The editor captures are
the other half and are listed at the end as an ordered capture plan.

## Conditions (read before trusting any number)

* Worldgen **v27** (`1f61e8c`: Whittaker precip boundary 800→450 mm/yr, dry-land
  cliff gate removed — BARE_ROCK is submarine-only). Both probes were **rebuilt
  after that commit** — the previous binaries reproduced the v26 census and were
  discarded. The rebuilt `vxc_climateprobe` census matches the commit message
  exactly (TEMPERATE_FOREST 9.67% of world, GRASSLAND 12.87%, BARE_ROCK 1.07%,
  all subsea-cliff).
* Sites are the v27 representative interior sites from
  `vxc_climateprobe <coarse s1> 20260719 64`. SAVANNA has a real interior
  sample at this stride (no edge-only fallback needed).
* Census: `vxc_assetprobe` per site, seed 20260719, placement square **256 m a
  side = 6.5536 ha** (`--place-region 128`), coarse tiles always
  (`/d/vox-wet-cache/.../000000000135276f/s1`), fine tiles where the s16 cache
  holds them. Banks/manifest verified current by `enginecheck.py` (PASS).
* **Fine vs coarse instrument difference.** Fine-tile sites (alpine-lake,
  grassland, desert, temperate_forest-fine) census the baked world: baked
  lake/river datum, shore-distance plane, real placement channels. Coarse-only
  sites census the amplified-coarse world with **sentinel channels**: no baked
  water, water distance unknown, so **water-gated (riparian) species fail
  closed by instrument** — a reed/riparian zero at a coarse-only site is not
  evidence about the world. Overlay maps at coarse-only sites draw the
  amplified-coarse ground (the same surface the census placed on; sea is the
  only water datum, no shore distance) and are labelled so by the probe.
* Anchor audits: **all 271,005 instances across the ten sites have solid
  anchors, none submerged.**

## Cross-biome census, instances per hectare

| biome | canopy | small-tree | shrub | grass+gc | flower | rock | reed |
|---|---:|---:|---:|---:|---:|---:|---:|
| beach | 44.3 | 40.9 | 514.8 | 2140.0 | **445.6** | 477.3 | **921.0** |
| grassland (fine) | 111.7 | 89.1 | 790.4 | 2043.3 | **1850.6** | 9.6 | **14.0** |
| temperate_forest | 117.6 | 124.2 | 1431.3 | 1521.8 | **1435.2** | 6.3 | 0.0 |
| temperate_forest (fine, old site) | 67.7 | 99.0 | 2056.7 | 669.6 | **513.5** | 2.0 | 0.0 |
| rainforest | 56.3 | **0.0** | 986.3 | 3152.6 | **725.9** | 42.9 | 0.0 |
| desert (fine) | 65.5 | 6.3 | 1640.0 | 1231.7 | **560.8** | 3.1 | 0.5 |
| savanna | 98.7 | 44.7 | 742.0 | 2945.4 | **1070.6** | 16.0 | **117.0** |
| taiga | 125.1 | 153.8 | 1075.4 | 2627.1 | **806.1** | 12.7 | 0.0 |
| tundra_alpine | **0.0** | **0.0** | 892.0 | 2062.5 | **285.0** | 22.1 | **390.5** |
| alpine-lake (fine, owner's site) | 34.3 | 28.4 | 769.8 | 679.2 | **296.6** | 16.8 | **29.3** |

**Bold numbers are placed-but-invisible or true zeros — see the findings.**
Every flower and reed column is bold because those instances render as nothing
in the engine today (finding 1).

Dominant species per biome (by instance count):

| biome | dominant species |
|---|---|
| beach | rubble-apron (rock), dwarf-eelgrass / sea-lettuce / gutweed (subtidal grass) |
| grassland | meadow-grass, buffalo-grass, cocksfoot, little-bluestem |
| temperate_forest | butchers-broom, salal (shrubs), meadow-grass, herb-robert |
| rainforest | jungle-groundcover, understory-fern, coffee-shrub, feather-moss |
| desert | saltbush, big-sagebrush, dry-tussock, creosote-bush |
| savanna | red-oat-grass, dry-tussock, buffalo-grass, devils-thorn |
| taiga | reindeer-lichen, feather-moss, bilberry-mat, meadow-grass |
| tundra_alpine | arctic-willow-thicket, crowberry-mat, reindeer-lichen, tussock-cottongrass |
| alpine-lake | salal, herb-robert, bramble-thicket, meadow-grass |

**No animals, confirmed:** across all 271,005 instances the only kinds placed
are tree / bush / grass / flower / rock / reed. Fish, bird and quadruped
species carry layer 255 (`kAssetLayerNotScattered`), are dropped by
`assetSpeciesTableFromManifest`, and structurally cannot come out of the
resolver (they are the "382 detail entities" the probe header reports
excluded).

## Findings, ranked

### 1. Flowers and reeds place everywhere and render nowhere (the survey's headline)

Every one of the **71 flower species and 24 reed species** placed across the
ten sites has **no bank** in `asset-forge/out/engine/banks/` (all tree, bush,
grass and rock species are fully banked). The census places them — 14.6% to
38.0% of each site's instances — and the engine then draws nothing: all these
species sit on the detail layer (L3), whose renderer
(`ue-project/Source/VoxelEarth/VoxelDetailAssetSubsystem.cpp:488`) fetches the
grid from the same bank library and counts a missing bank as "composes as
nothing" (`BankMisses`).

Attribution: `asset-forge/tools/export_banks.py` bakes `KINDS_TERRAIN =
("tree", "rock")` by default; bush and grass got banks in the height-promotion
bake (`44e3486`); flowers and reeds never got a bake because the exporter's
header says detail kinds' "bank path is the UE side's business" — written
before task #7 made the UE side consume the same banks. The two halves each
assume the other bakes flowers.

Consequence for the owner's expectation: **no biome can currently show
flowers**, the beach shows no wrack line (all 6 wrack species are reeds), the
tundra shows no cottongrass (its most numerous "reed", 2,559 instances), the
savanna no pampas-plume, and the alpine lake margin shows none of its 192
placed water plants (water-lily, yellow-flag-iris, etc.). Per-site invisible
share: grassland 38.0%, temperate_forest 31.0%, beach 29.8%, savanna 23.6%,
tundra 18.5%, alpine-lake 17.6%, taiga 16.8%, desert 16.0%, rainforest 14.6%.

This is a report, not a fix: the fix is a flower/reed bank bake (or the
separate detail-art path the exporter header imagined), plus whatever
`enginecheck` coverage stops manifest-kept species from being bankless.

### 2. The rainforest has almost no trees

56.3 canopy/ha — **less than the desert** (65.5/ha) and 4.4x below taiga
(279/ha trees total). Zero small trees. Only **4 tree species** carry
rainforest weight at this site: cecropia (254), coast-palm (96), strangler-fig
(17), **jungle-emergent (2)** — two emergents in 6.5 hectares. Gate
attribution says why: **biome weight 0 refuses 88.6%** of (site, species)
pairs — the highest of any biome — with slope and elevation refusing almost
nothing (0.4% / 0.9%). The rainforest tree roster is simply too thin in the
manifest. The overlay map makes it visceral: scattered dark-green dots in a
sea of ground cover.

### 3. Rocks are near-absent inland

Beach: 477 rocks/ha (rubble-apron, wave-polished-boulder — working as
designed). Everywhere else: **2–22/ha**, i.e. one or two rocks in a whole
screenshot. Worst: temperate_forest-fine **2.0/ha** (13 rocks in 6.5 ha),
desert **3.1/ha** (20 rocks — tafoni-sandstone 7, limestone-slab 5,
ventifact-boulder 5, in a biome that reads as rocky by definition),
temperate_forest 6.3/ha, grassland 9.6/ha. Attribution is biome weight, not
terrain: slope gates refuse ~0% of rock pairs inland. The inland rock species
exist (granite-boulder, leaf-littered-slab, karst-breakdown-block place in
every biome) — their densities/weights are just an order of magnitude below
the beach set. If the owner wants rocks visible in every scene, inland rock
weight is the knob.

### 4. Tundra/alpine has zero trees — by design, and the census proves it

Treeline gate: **all 23,935 placement columns at the tundra site are above the
temperature-adjusted treeline** (`treeline: below 0, above 23935`), so every
tree pair is refused. This is the treeline doing its job — krummholz is
present as shrubs (arctic-willow-thicket, 3,303 instances). Not a bug; noted
so nobody hunts one. If the owner wants the classic scattered-dwarf-tree
ecotone look, that is a spec decision (a tree species with treeline
tolerance), not a placement fix.

### 5. Biome character inversions worth the owner's eye

* **Desert canopy (65.5/ha) exceeds rainforest canopy (56.3/ha).** The desert
  is a dense Sonoran look — saguaro 207 + joshua-tree 93 in 6.5 ha. Legitimate
  as art direction, but combined with finding 2 the two biomes read inverted.
* **Savanna places 143 trees/ha** (mopane 304 + hawthorn-scrub 278 + acacia
  74…). That is closed woodland, not savanna; the scattered-tree look needs
  either lower tree weight or a clumping mechanism.
* **Desert shrub 1,640/ha** is the second-shrubbiest census after the old
  temperate site — saltbush/sagebrush steppe rather than barren desert. May be
  intended; flagged for judgement.

### 6. Instrument caveats on the zeros

The reed zeros at temperate_forest, rainforest and taiga are **coarse-only
sites**: no baked water → water-gated species fail closed by instrument (the
water-distance gate refused 6.7%–13.3% of pairs there). The temperate-fine
site also shows reed 0.0 with baked water loaded — but its 256 m square simply
holds no lake or river reach. Do not read inland reed zeros as world truth
until a fine-tile riparian site is censused; the alpine-lake site (booming
with 62 riparian species, 2,094 instances) shows the riparian machinery works
where water exists.

## Overlay maps

All under `bake-out/biome-survey/overlays/` (hillshade + contours + water +
treeline, one dot per instance, colored by class; per-class density heatmaps
alongside as `<name>.density-<class>.png`). Raw probe outputs (`.json`,
`.instances.csv`, `.ground.bin`, `.log` with full gate attribution) under
`bake-out/biome-survey/`. Coarse-only sites' ground is amplified-coarse and
labelled so by the probe (sea-only datum, no shore-distance plane).

| biome | map |
|---|---|
| beach | `bake-out/biome-survey/overlays/beach.map.png` |
| grassland | `bake-out/biome-survey/overlays/grassland.map.png` |
| temperate_forest (v27 site, coarse) | `bake-out/biome-survey/overlays/temperate_forest.map.png` |
| temperate_forest (fine, old site) | `bake-out/biome-survey/overlays/temperate_forest-fine.map.png` |
| rainforest | `bake-out/biome-survey/overlays/rainforest.map.png` |
| desert | `bake-out/biome-survey/overlays/desert.map.png` |
| savanna | `bake-out/biome-survey/overlays/savanna.map.png` |
| taiga | `bake-out/biome-survey/overlays/taiga.map.png` |
| tundra_alpine | `bake-out/biome-survey/overlays/tundra_alpine.map.png` |
| alpine-lake | `bake-out/biome-survey/overlays/alpine-lake.map.png` |

## Capture plan (for the editor session — one editor per box, captures owned there)

Ground-level and low-altitude framings so placement is legible, per the
owner's ask (no more 350 m vistas). Two shots per site: EYE = spawn altitude
ground +1.7 m, pitch -5°; DRONE = ground +30 m, pitch -40°. Sites marked
COARSE-ONLY have no fine tiles in the s16 cache — the editor amplifies from
coarse there; expect longer settle, and confirm terrain loaded from the log
before capturing (blank capture = unloaded terrain, not a rendering bug).

Ordered for value: fine-tile sites first (baked ground, water present), then
coarse.

1. **alpine-lake** `-SpawnAt '-39661,-57292'` — fine. EYE facing the lake
   margin (riparian belt: 62 species place here; note the water plants
   themselves are invisible until finding 1 is fixed), then DRONE.
2. **desert** `-SpawnAt '-104865,-75855'` — fine. EYE among the saguaro/
   sagebrush; DRONE to show the bare wash channels on the overlay.
3. **grassland** `-SpawnAt '-92445,-75855'` — fine. EYE; this is the biome
   where 38% of instances (all flowers) are invisible — the capture documents
   the gap.
4. **temperate_forest** `-SpawnAt '-113145,-162885'` — fine (old v26 site,
   still temperate under v27; the v27 representative site -92445,-88305 is
   COARSE-ONLY — prefer the fine site for captures). EYE under canopy; DRONE.
5. **beach** `-SpawnAt '-220905,-96585'` — COARSE-ONLY. EYE along the shore
   (rubble-apron + sea-grape; the wrack line will be missing — finding 1);
   DRONE.
6. **rainforest** `-SpawnAt '-100725,-171165'` — COARSE-ONLY. EYE + DRONE;
   this is the money shot for finding 2 (canopy sparser than the desert's).
7. **savanna** `-SpawnAt '-67575,-158745'` — COARSE-ONLY. DRONE first (tree
   density reads from above), then EYE.
8. **taiga** `-SpawnAt '-158745,-79995'` — COARSE-ONLY. EYE.
9. **tundra_alpine** `-SpawnAt '-75855,-96585'` — COARSE-ONLY. EYE (all-shrub
   above-treeline look; zero trees is correct here).

Reproduction: rebuilt `vxc_climateprobe`/`vxc_assetprobe` at v27; per-site
command lines are in `bake-out/biome-survey/*.log` headers.

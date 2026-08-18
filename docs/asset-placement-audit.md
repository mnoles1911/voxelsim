# Asset placement: what already mirrors nature, what is stubbed, what is missing

Date: 2026-08-17. Written against the code as of commit 87e99b3, with fresh
`vxc_assetprobe` censuses at four sites (conditions in §2). Companion to
`docs/asset-placement-design.md` (the build record) and
`docs/asset-placement-architecture.md`; this document is the *audit* — it
scores what exists against natural placement rules, measures where it can,
and ranks the work left.

**The owner's question, answered up front.** Yes — a genuinely sophisticated
placement system already exists and runs: every species carries per-biome
weights, an elevation band, a slope limit, spacing, abundance and a clustering
strength, and the resolver honours all of them deterministically with zero
floating anchors in 278,732 audited placements. But two of the natural rules
the question names are not delivered: **proximity to water is authored on 112
placeable species and served to none of them** (the gate exists and fails
closed, so every riverbank willow, reed bed and riverbed boulder is refused
everywhere), and **density does not differ between rich biomes** (a savanna
currently carries exactly as many canopy trees as a temperate forest, for a
reason measured below). Rocks, ground cover and aquatic life each have their
own gaps, itemised in §5. So: the foundation is real and tested; the work
remaining is one missing input (water distance), one density correction, a
spec field, and presentation for the 648 species that place but never render.

---

## 1. The scorecard

One line per natural rule. "Implemented" means designed, built, tested and
measured; "stubbed" means the gate exists in code but no production input
feeds it; "missing" means not designed.

| Natural rule | Status | Where / evidence |
|---|---|---|
| Biome affinity (per-species, per-biome) | **Implemented** | `assetpolicy.h` weighted pick; weights × abundance folded at import (`assetmanifest.h`) |
| Elevation bands | **Implemented** | `assetSpeciesTolerates`; meaningfully authored where it matters (mangroves 0–8 m, krummholz 600–3200 m), generic 0–2000 m elsewhere |
| Slope ceiling | **Implemented** | `slopeMaxMmPerM`, same currency as the biome cliff gate |
| Slope *band* (scree, talus) | **Stubbed** | `slopeMinMmPerM` exists in the record and manifest format; `placement.slope_min_pct` does not exist in the forge spec, so every species carries 0 (`manifest.py:494`) |
| Clustering (groves/stands) | **Implemented, with one dead zone** | Two-octave mean-preserving field, dispersion measured 0.85 → ~3.2 (clumped past twice Poisson), density preserved within 3%; **does nothing in weight-saturated biomes** — §4.3 |
| Density (how many, where) | **Implemented, one honest defect** | Layer per-mille is the only working knob (commit 87e99b3: L1 1000→350‰, L2→550‰); biome richness cannot thin a saturated biome — savanna == forest, §4.2 |
| Proximity to water | **STUBBED — fails closed** | `assetColumnFactsFromSample` hardwires the "unknown" sentinel (`assetfield.h:100-105`); 112 scattered species with `water_max_m > 0` are refused *everywhere* — §3 |
| Riverbank / lakeshore guilds (reeds, willows, cobble bars) | **Stubbed** | Entirely blocked on the line above; the species are authored and baked |
| Wet-ground veto (no oaks on a lake bed) | **Missing** | Nothing in the placement path knows a column is under a lake or river (§3.4); only the OCEAN biome (< −3 m) protects sea floor |
| Ground anchoring (no floating trees, no trees over cave mouths) | **Implemented** | Anchor verified against the carve passes, refuses on failure; 0 bad anchors in every audit run |
| Aspect / moisture microhabitat (north-slope moss, south-slope drought plants) | **Missing** | `Amplifier::surfaceInfo` computes the signed gradient direction (98% uphill agreement) and placement never reads it |
| Altitude-dependent size (stunted trees at treeline) | **Missing** | Bank seed is a pure hash (`assetplacement.h:249-257`) — the same four individuals at valley floor and treeline. The library's workaround is species-level (alpine-krummholz) |
| Succession / disturbance (age structure, clearings) | **Missing** | No mechanism; `temperate-sapling` exists as a species, placed like anything else |
| Dead wood (snags, fallen logs) | **Missing (data)** | The 828-spec library has one dead terrestrial species (`desert-dead`); no temperate/boreal snag or log |
| Forest edges (canopy→shrub gradient at biome boundaries) | **Missing (mechanism); partly emergent** | Biome edges are per-column dithered speckle, not a structured ecotone; shrubs bleed across only where authors gave them weight in both biomes |
| Talus **below cliffs** (uphill-neighbour awareness) | **Designed, not built** | Needs a second column at an offset; the slope band was chosen to deliver most of the look first (`assetpolicy.h:113-119`) |
| Rock guilds by setting (riverbed, desert clutter) | **Data gaps** | Riverbed rocks all water-gated (absent everywhere, §3); desert has **zero** species on the 2.2 m rock/shrub lattice — §5.2 |
| Fish/aquatic: fresh vs salt, depth bands, shoals | **Designed + built in core, no consumer** | `assetdetail.h` salinity table (measured against the datum composition), group scatter, in-column placement — consumed only by its own tests; no spawner exists (§5.4) |
| Herds / flocks (land animals, birds) | **Same as above** | 382 detail-entity species in the manifest, zero in-game presence |
| Ground cover placement | **Implemented; invisible** | L3 places 85% of all instances; every UE call passes `terrainOnly=true` and no ground-cover renderer exists (§5.3) |

---

## 2. What was measured, and under what conditions

`vxc_assetprobe` (the sanctioned census tool), manifest + banks from
`asset-forge/out/engine`, seed 20260719, placement census over 128×128 m at
each site. Climate is the **real coarse-tile climate** in every run
(`--coarse tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1`,
289 tiles — the probe grew this flag in commit 780cb37 precisely because a
synthetic-climate census once reported 3,741 instances where the engine
composed zero). Elevation source varies by site and is disclosed per row,
because the fine bake does not cover the whole world:

| Site | Origin (m) | Elevation source | Sites | Instances | L0/L1/L2/L3 | Species |
|---|---|---|---|---|---|---|
| Alpine lake area | −39661, −57292 | coarse 30 m fallback (fine tile −3_−4 not baked) | 30,303 | 22,725 | 0 / **283** / 1,778 / 20,664 | 92 |
| Forest pose (rainforest, 654 m) | −234232, −242023 | real fine tile −16_−16 (bd3d0ddc7 bake) | 30,255 | 4,919 | 0 / **55** / **0** / 4,864 | **15** |
| Desert (tile −3_−11, 96.6% desert) | −38400, −161280 | coarse 30 m | 30,224 | 8,859 | 0 / **76** / **0** / 8,783 | 35 |
| Savanna (tile 2_−8, 100% savanna) | 38400, −115200 | coarse 30 m | 30,244 | 22,623 | 0 / **277** / 1,649 / 20,697 | 94 |
| Wet-alpine tile interior | −38400, −38400 | real fine tile −3_−3 (wet bake) | 30,262 | 13 | 0 / 13 / 0 / 0 | 1 |

Every run: **0 floating anchors** (each independently re-read through
`Amplifier::materialAt`), 0 bank refusals. The probe prints counts, not
per-biome or per-species attribution — a `--species` histogram would settle
the residuals flagged below and is a small, worthwhile addition.

Three readings from that table, each explained by the manifest arithmetic in
§4 and §3:

1. **Savanna carries the same canopy density as everywhere rich (277 vs 283).**
   Both sites saturate the occupancy cap, and above the cap only the layer's
   own density acts. A savanna should read as scattered trees over grass; it
   reads as woodland. §4.2.
2. **The rainforest has fewer trees than the desert (55 vs 76).** Not a joke:
   **80% of rainforest canopy pick-weight is water-gated** (2,592 of 3,256
   summed per-mille) and therefore refused by the closed water gate, and most
   of the rest is coastal-elevation-banded (mangroves ≤ 6–8 m, at a 654 m
   site) or slope-capped on steep ground. The wettest biome in the game is
   starved by the missing water input. §3.2.
3. **L2 (boulders and large shrubs, 2.2 m lattice) is zero in both desert and
   rainforest.** Desert: zero L2 species exist — every desert rock authors
   14–120 m spacing and files on L0/L1 (data gap, §5.2). Rainforest: all four
   L2 species are water-gated or coastal (riffle-slab ≤ 2 m from water,
   wild-banana ≤ 60 m, sea-grape ≤ 25 m elevation) — blocked on §3.

The wet-alpine interior row (13 instances) is included as evidence that the
gates veto hard on real steep high ground; per-column attribution (treeline
vs slope vs bare-rock classification) needs the species histogram to split.

---

## 3. The water-distance gap — the single highest-impact item

### 3.1 What is stubbed, precisely

`AssetSpecies::waterMaxMm` ("only appears within this distance of a
watercourse") is read by `assetSpeciesTolerates` (`assetpolicy.h:312-317`)
and **fails closed**: when the distance is unknown, the species is refused
rather than guessed. The one production binding that fills column facts,
`assetColumnFactsFromSample` (`assetfield.h:93-106`), hardwires the
"unknown" sentinel with a comment saying nothing in voxel-core answers "how
far to the nearest watercourse". Both halves are deliberate and correct —
placing a riverbank willow on the strength of not knowing where the river is
would be this project's signature derived-not-verified failure. But the
consequence is:

### 3.2 What is locked out, counted

**112 scattered species author `water_max_m > 0` and none of them can ever
place** (fresh count over all 828 specs, 2026-08-17):

| kind | water-gated / total | examples (authored reach) |
|---|---|---|
| tree | 20 / 78 (26%) | sitka-spruce 120 m, western-red-cedar 100 m, bigleaf-maple 80 m, black-mangrove 15 m, fever-tree 50 m |
| grass | 29 / 89 (33%) | lady-fern 60 m, floodplain-sedge 10 m, brook-moss-cushion 2 m |
| flower | 25 / 87 (29%) | marsh-marigold 6 m, water-mint 4 m, flowering-rush 3 m |
| rock | 17 / 102 (17%) | river-cobble 40 m, gravel-bar 3 m, riffle-slab 2 m, plunge-pool-boulder 3 m |
| reed | 14 / 33 (42%) | bulrush 3 m, papyrus 6 m, broadleaf-cattail 2 m |
| bush | 7 / 57 (12%) | guelder-rose 60 m, tamarisk-scrub 30 m |

(23 birds also author it; they are spawner-side and blocked on §5.4 anyway.)
The whole riparian look — reeds ringing lakes, willows and alders along
rivers, cobble bars in channels — is this list. So is much of the rainforest
(§2 reading 2), because its canopy was authored, reasonably, as a
water-loving guild. Every entry is already baked and in the manifest;
serving one number turns all of it on.

### 3.3 The concrete servable form, priced

The game now has water authority the 2026-08-15 design did not: lake basins
with extents, river ribbon polylines, fine-tier water rows, and a baked
shore-distance plane. The design's §12.6 already established the trap: the
existing `bathy_shore` distance plane covers **lake basins only**, so wiring
it as-is would deterministically starve every river species while looking
wired. The right shape:

* **Where it is computed: the bake**, not the engine. Extend the bake's
  existing shore-distance transform so its wet set is
  `lake extents ∪ river water-plane wet cells ∪ (ground below sea level)` —
  the union the redistribute-don't-delete water pipeline already knows how to
  enumerate. This is a change to a distance transform that already runs, per
  fine tile, not a new pass.
* **Format**: one u8 plane per fine tile, subsampled 4× (7.5 m pixels —
  finer than needed for a gate whose authored values are 2–120 m, and 16× the
  storage win over full-resolution), quantised at 2 m per step, 255 = "≥ 508 m
  / unknown". Raw 4 MB per tile before the block compression every other
  plane already gets; the authored maximum anywhere in the library is 250 m,
  so the range is never the limit.
* **Who serves it**: `FineTileSampler` decodes the plane like any other row;
  `assetColumnFactsFromSample` grows the one lookup and stops writing the
  sentinel where the plane is resident. Where the plane is not resident the
  sentinel stays and the gate keeps failing closed — the residency-gate
  lesson of commit 780cb37 (a non-resident tile once answered elevation 0
  with default climate and resolved a species 1,367 m underground) says that
  refusal must remain the default.
* **Determinism**: the plane is tile bytes, i.e. worldgen input under the v24
  contract — same answer on every machine, no runtime search, no
  order-dependence. Bake-format bump (`bake_ver`) plus `kWorldGenVersion`
  bump, goldens re-blessed deliberately.
* **Cost honestly stated**: a chamfer distance transform over 8192² is
  seconds per tile at bake; the real costs are the format bump, the reader,
  the re-bake of shipped tiles, and the golden churn. No admission-path cost
  at all — the gate runs on the generation path only.

### 3.4 The same plane closes a latent defect: trees on lake beds

Today **nothing in the placement path knows a column is under water**. The
column facts carry biome, elevation, slope and anchor solidity — not
wetness. The OCEAN biome gate protects ground below −3 m, and BEACH covers
−3 to +4 m (`biome.h:75-76`); an inland lake or a carved river channel at
any elevation classifies by climate, its bed is solid, its slope is gentle,
and an ordinary conifer *will anchor on it*. This has not been reported in a
capture — until tonight assets only existed within 128 m of the camera, and
no capture pose stood in a river — but it is not speculative: it is the
absence of a gate, readable at `assetfield.h:93-106`. The water plane's zero
value ("this column is wet") is the veto, one compare, in the same delivery.
Terrestrial species refuse wet columns; reed-guild species (which author
`water_max_m` of 2–6 m) get their shoreline exactly.

---

## 4. Density and clustering — what is real, what saturates

### 4.1 What the mechanism genuinely delivers

The bare lattice scatter is *more regular than random* (dispersion 0.59–0.85
— an orchard, measured before any clustering code was written), and the
two-octave grove field moves it to ~1.9–3.2 — visibly clumped stands at a
forest scale and a grove scale, per species, with population preserved within
3% (the first cut silently lost 17% and was caught by the density test —
`assetClusterKeeps` carries the account). This is a real, measured, natural
pattern and it is cheap (five hashes per surviving site).

### 4.2 The saturation defect: savanna == forest

The occupancy rule is elegant — the summed weight of tolerating species IS
the chance the site is used, so a desert thins itself with no density table
— but it **caps at certainty**. Temperate forest sums 10,288 per-mille of
canopy weight, savanna 1,851; both are > 1000, so both stand a tree on
exactly 35% of 5 m cells (the layer density). Biome richness differentiates
only *below* the cap (desert 629 effective → 76 instances; rainforest,
water-starved to 664 → 55). Above it, nothing differentiates. Commit 87e99b3
measured the sibling fact: per-species spacing dilution folds into pick
weights and *cancels* in any saturated biome (canopy spacing ×1.75 moved the
census 381 → 376) — which is why layer density was the knob that fixed the
thicket, and why it is global and biome-blind.

The fix is small and data-shaped: **a per-biome density scalar** on the
occupancy test (per-mille, in the manifest, default 1000). Savanna authors
~120–200‰, grassland less, forest 1000. One multiply in
`assetResolveSite`'s occupancy factor, ten numbers per layer in the
manifest, worldgen-versioned like any manifest change. Alternative for zero
core change: rescale savanna/grassland biome weights down so their sums land
under the cap — works today, but couples "which species" to "how many", which
is exactly the coupling the layer-density lesson says to avoid.

### 4.3 The clustering dead zone: saturated biomes get no groves

Where the cap is 1000 there is no headroom and the cluster veto does nothing
— correctly, as a veto cannot gather what is already everywhere
(`assetpolicy.h:500-508`). Consequence: in exactly the biomes that read as
"forest", species do not form groves — beech and oak interleave per-cell
everywhere. The designed fix (design doc §9) is folding the cluster field
into the **pick weights** instead — beech here, oak there, total cover
unchanged — costing one field evaluation per eligible species per site on
the generation path. Note that item 2 above *reduces* this dead zone
wherever it un-saturates a biome, so sequence it after.

---

## 5. Per-class status

### 5.1 Trees — the most complete class

Everything in §1's implemented rows, plus verified anchoring and the
streaming-bound contract. Gaps in priority order: water distance (§3),
per-biome density (§4.2), saturated groves (§4.3), then the unmodelled
niceties (aspect, altitude-size, succession, dead wood). On altitude-size:
the cheap mechanism is biasing the bank-seed pick by elevation within the
band (seeds are already sorted per species; picking low-index = small seeds
near `elev_max` is one integer blend in `assetSiteInCell`'s pick), but it
needs per-seed height metadata in the bank header first — worth doing only
after the visible gaps.

### 5.2 Rocks

Slope ceiling and elevation work today; three gaps:

* **The slope band** (`slope_min_pct`) — the difference between a boulder
  and scree. The C++ record, the manifest format and the design request
  (§8.3, since 2026-08-15) all wait on a three-line forge spec row plus an
  authoring pass over ~30 rock/scree species. Until then `alpine-scree`
  (weight 1000, slope 0–70%) carpets *flat* alpine ground exactly as it
  carpets talus slopes.
* **Riverbed rocks are all water-gated** — riffle-slab, river-cobble,
  gravel-bar, step-pool-boulder, plunge-pool-boulder: the entire fluvial
  rock guild is in §3's locked list. Nothing to do here beyond §3 itself.
* **Desert mid-scale clutter is absent by data**: all 13 desert-weighted
  rocks author 14–120 m spacing (landform-scale pieces on L0/L1); no desert
  species exists at the 2.2 m lattice, so between the buttes the desert
  floor is bare. A data pass (weathered slabs, cobble scatter,
  creosote-scale shrubs with desert weights) fixes it with no code.
* **Talus-below-cliffs** (uphill awareness) — designed, deferred; the slope
  band above delivers most of the look and should land first.

### 5.3 Ground cover (L3) — places, but nothing draws it

The detail lattice resolves 20,000+ instances per 128 m square (85% of all
instances; 266 species) through the same gates as trees. **No renderer
consumes them**: every UE composition call passes `terrainOnly=true`, and
the detail-lattice instances' banks are deliberately never even baked into
world voxels (they carry their own 5 cm grids). Their habitat logic is
already as good as the trees'; their clustering strengths are authored
(grass median 0.95). The missing piece is purely presentation — an
instanced-mesh path fed by `instancesForRect(terrainOnly=false)` per
near-field chunk. Until it exists, every flower, fern, reed and grass tuft
in the library is invisible, and the reed-ring payoff of §3 will not be
*seen* even once it places.

### 5.4 Aquatic species and wildlife — core built, no consumer

`assetdetail.h` is complete and tested in core: salinity classified from the
two water operands before the max() destroys provenance (fresh / brackish /
salt, river mouths brackish by construction), deterministic group scatter
(shoals/flocks/herds), and in-column member placement that refuses a fish
outside its water. **Nothing calls it** — no UE spawner exists, so all 106
fish, 18 cetaceans, 131 quadrupeds and 127 birds are manifest rows only.
Additionally the aquatic *plants* (kelp, coral, seagrass — authored as
grass/bush/rock kinds) still carry `detail.water = "any"` (measured: zero
non-default values across the library) and unauthored depth bands, the exact
sentinel problem design §5.4 documents. The spawner is UE-side work; the
depth-band authoring is a species-list pass that gets cheaper the sooner it
happens (every reseed after v24 is a worldgen change).

### 5.5 Forest edges

No ecotone mechanism exists; the biome boundary is per-column climate dither
(speckle at the 30 m-pixel gradient, not a canopy→shrub→grass band). The
honest assessment: a dedicated edge mechanism (distance-to-biome-boundary)
would be expensive and is not the next dollar's best use — §4.2's
de-saturation plus authors giving shrub species weight in both forest and
grassland (many already do) buys a soft edge for free. Revisit only if
captures still read as hard-edged after items 1–2 land.

---

## 6. The ranked plan

Ranked by believability-per-effort. Effort classes: **S** = data/spec only
(asset-forge), **M** = voxel-core policy change, **L** = new baked input
(bake format + reader + re-bake), **XL** = new UE system. Every item below
stays a pure function of seed + manifest bytes + tile bytes — nothing
introduces runtime state, so multiplayer determinism is preserved by
construction; items marked ⚠ move worldgen output and need the version bump
+ golden re-bless.

| # | Item | Effort | ⚠ | What the player sees |
|---|---|---|---|---|
| 1 | **Serve distance-to-water**: extend the bake's shore-distance wet set to rivers + sea, u8 plane per fine tile, one lookup in `assetColumnFactsFromSample`; includes the wet-column veto | **L** | ⚠ | 112 species turn on: reed-ringed lakes, willow/alder riverbanks, cobble bars, mangrove coasts; rainforest stops being emptier than desert; no tree ever anchors in a lake |
| 2 | **Per-biome density scalar** on the occupancy cap (manifest field + one multiply) | **S+M** | ⚠ | Savanna becomes savanna (scattered trees over grass), grassland opens up, forests stay full — the single cheapest large visual differentiation |
| 3 | **`placement.slope_min_pct`** in the forge spec + authoring pass over scree/talus/crevice species (format field already shipped, waiting) | **S** | ⚠ | Scree confined to slopes below cliffs instead of carpeting flat alpine ground; talus reads as talus |
| 4 | **Species-level groves in saturated forests**: fold the cluster field into pick weights (designed, §4.3) | **M** | ⚠ | Beech groves beside oak groves inside a full forest instead of per-cell interleaving |
| 5 | **Render what already places**: L3 ground-cover instancing, then the detail-entity spawner on `assetdetail.h` | **XL** | — | 648 currently-invisible species appear: grass, flowers, reeds underfoot; fish in the right water, herds and flocks. No placement logic needed — it exists and is tested |

Behind those, in order, all cheap and independent: the data backlog
(desert mid-scale clutter, temperate/boreal dead wood, aquatic depth bands +
`detail.water` scoping — each S, each ⚠ once authored); a `--species`
histogram in `vxc_assetprobe` (closes the attribution residuals in §2, no ⚠);
altitude-biased seed pick for treeline stunting (M, needs per-seed height
metadata); aspect preference from the already-computed gradient (M);
uphill-cliff talus association (M, after item 3 proves insufficient — the
design's own bet is that it mostly will not be needed).

Items 1–4 are placement truth and compound: 1 un-starves the wet guilds, 2
differentiates the dry ones, 3 sharpens the steep ones, 4 organises the rich
ones. Item 5 is the only reason a player would currently conclude "there is
no placement system" for two-thirds of the library — the logic exists; the
pixels do not.

---

## 7. LANDED 2026-08-17 (evening): item 1, plus research recs 2–7 — bake_ver 28 / worldgen v26

Same day as the audit, tasks #6+#8. What of the table above changed state:

* **§3 (water distance) — SERVED.** The bake's wet set is now the full
  `lakes ∪ river plane ∪ sea` union (§12.6's trap closed), shipped as one of
  five new u8 placement planes per fine tile (`SECTION_PLACE_*`,
  docs/vxtl-v2-format.md §6.2), read through
  `FineTileSampler::placementAtVoxel` and the canonical
  `assetchannels.h::assetColumnChannelsAt` binding. Measured on the bv28
  tiles (namespace `-b19d281fd`, six alpine + rainforest (−6,−11) + desert
  (−7,−7)): at the alpine lakeshore (−39661,−57292) **63 riparian species /
  3,870 instances place** (was 0 / 0, ever), cattail-bulrush-bur-reed ringing
  the lake; at a riverside rainforest site (−86471,−168131) **72 riparian
  species / 8,057 instances**, and rainforest canopy beats desert on
  identical real-fine conditions (L1 204 vs 156) — reversing §2's reading 2
  wherever water is inside the authored reach. (A rainforest RIDGE 150+ m
  from any water stays canopy-starved at L1 39: that is the authored 60–120 m
  water binding of the guild doing its job, not a gate failure — 0.5% of its
  refusals attribute to the water gate, 93% to biome weight.) The gate keeps
  failing closed on pre-28 tiles: re-measured on the old corpus, still 0
  riparian, bit-identical census — the sentinel path reproduces the v25
  world exactly.
* **§3.4 (trees on lake beds) — CLOSED, from the rendered datum.** The veto
  now reads the SAME composed `LakeSampler`/`RiverSampler` water surface the
  renderer draws (the first cut read the debug water-marker field, which is
  empty in production — the owner's lake-tree photo). The probe re-audits
  every anchor against that datum independently: **0 submerged of 11,759 /
  18,990 / 20,389** at the lakeshore / riverside / desert censuses, and it
  exits nonzero if one ever is.
* **§1's "aspect/moisture microhabitat" and "slope band" rows** — TWI
  moisture, talus flux, curvature and heat-load planes bake beside the
  distance; pick weights read them through per-species affinities DERIVED at
  import (water_max, biome weights, kind, height — zero spec edits). The
  hard `slope_max` became a response curve sized inversely from height
  (owner: "steeper slopes have small trees"), and the treeline a krummholz
  BAND (±150 m by aspect heat load).
* **`vxc_assetprobe` grew the instruments this document asked for**: the
  `--species` histogram (with riparian call-outs), a per-gate refusal
  attribution (one spelling with the gate itself), the submerged audit, and
  mean placed height per slope bucket.
* **NOT touched**: item 2 (per-biome density scalar), item 4 (groves in
  saturated forests), item 5 (rendering), the §5.2 data passes, and
  research's FON-lite cross-layer suppression — deferred with the task.

# DEM reference library — research + acquisition plan

Status: RESEARCH PLAN, not an implementation. Nothing here changes the
pipeline. Owner directive (2026-08): *"collect a small library of radically
different global terrain (desert vs archipelago vs Icelandic cliffs vs massive
mountains vs fjords vs temperate rolling forests) … looking for radically
different terrain areas that give Lord of the Rings style epicness and scale."*

This document delivers: (1) where real elevation data comes from and under
what license, (2) a concrete 12-region library with bounding boxes sized to
our tiles, each chosen as a distinct landform *grammar*, (3) the three ways
the library could be used and what each costs, (4) the measurements that turn
"is our terrain dramatic enough" into numbers, (5) where the bytes live so we
never hit GitHub's 100 MB wall again. A small inert helper script exists at
`terrain-service/tools/fetch_dem_reference.py` (documented in §7); it does
nothing until explicitly run.

Plain-English rule for this doc: a **DEM** (digital elevation model) is just a
grid of ground heights, one number per cell; "30 m posting" means one height
every 30 m. A **grammar** here means the repeatable structural rules of a
landscape — how relief, slopes and valleys are organised — as opposed to one
pretty view.

---

## 1. Ground truth from our pipeline (verified, with citations)

These numbers were read from the code on branch
`claude/f6-interior-rim-injection`, not assumed:

* **Coarse tier: 512 px at 30 m/px = 15.36 km per tile.**
  `voxel-core/include/voxelcore/tilestore.h:35` (`kTileSize = 512`) and `:74`
  (`scale == 1 ? 30000` mm). Scale 8 is a 3.75 m/px supersample of the same
  checkpoint (`:63-64`).
* **Fine tier: 8192 px at 1.875 m/px, same 15.36 km footprint.**
  `tilestore.h:140-141` (`kFineTileSize = 8192`, `kFineTileScale = 16`),
  `:70-71`.
* **The generator is `terrain-diffusion` with an unconditional checkpoint**
  (`terrain-diffusion-UNLABELED`); what steers it is the *conditioning
  sketch*: 5 channels (elevation + 4 climate), **one value per 7.68 km cell**,
  built by default from Perlin noise quantile-matched to Earth via
  `synthetic_map._compute_map_stats` reading the pinned WorldClim bio rasters
  + `data/global/etopo_10m.tif`. See `terrain-service/docs/worldgen-levers.md`
  §1–2 and `terrain-service/terrain_service/providers/diffusion.py` (module
  docstring; `WorldShapeConfig` at :480 with `frequency_mult`,
  `drop_water_pct`, `cond_snr`, `coarse_pooling`).
* **Conditioning files are PINNED BYTES in the world identity.**
  `terrain-service/terrain_service/conditioning_artifacts.py`: every
  conditioning file carries a sha256 pin in
  `data/conditioning-artifacts.json`; the digest feeds `provider_id`, which is
  the tile-cache namespace and the value edit-log replay checks. **Any DEM we
  adopt as a generation input becomes a world-identity input: it must be
  content-addressed (hash-pinned, fetch-verified) or two clients disagree
  about the world.** This constraint shapes every proposal below.

**The 30 m coincidence, stated precisely.** Copernicus GLO-30 and SRTM are
1-arc-second grids: ~30.9 m spacing north–south everywhere, and
30.9 m × cos(latitude) east–west (GLO-30 additionally thins its east–west
sampling in bands above ~50°N — verify exact bands at acquisition). Our tiles
are a *metric* 30 m grid. So adopting a real DEM needs exactly **one
scale-preserving reprojection** (geographic → local metric grid at 30 m,
single bilinear/cubic pass, ~1:1 sample ratio) and **no pyramid resampling**.
The practical win: relief statistics computed on a GLO-30/SRTM clip at 30 m
posting are directly comparable, lag-for-lag, with statistics computed on our
coarse tiles — no scale correction. On the fine tier, ArcticDEM 2 m → 1.875 m
is a mild upsample and 3DEP/LINZ/Kartverket 1 m → 1.875 m a mild downsample;
both acceptable, neither needed for the routes in §4 that matter first.

---

## 2. Sources: where real terrain comes from

| Source | Resolution | Coverage | License / redistribution | Format | Access |
|---|---|---|---|---|---|
| **Copernicus DEM GLO-30** | 1″ (~30 m) | Global (a few country tiles withheld from the public set) | Free for any use incl. reproduction & distribution with attribution, under the ESA Copernicus DEM licence (confirm exact wording at acquisition) | COG GeoTIFF, float32 m | AWS Open Data `s3://copernicus-dem-30m` (eu-central-1, no account needed); also OpenTopography |
| **Copernicus DEM GLO-90** | 3″ (~90 m) | Global, no withheld tiles | Same licence family | COG GeoTIFF | `s3://copernicus-dem-90m` |
| **SRTM 30 m (SRTMGL1 v3)** | 1″ | 60°N–56°S only (misses Iceland, Lofoten) | US public domain (NASA/USGS) | HGT / GeoTIFF | USGS EarthExplorer, OpenTopography, AWS |
| **ASTER GDEM v3** | 1″ | 83°N–83°S | Free, redistribution permitted with NASA/METI citation | GeoTIFF | NASA Earthdata |
| **ArcticDEM v4.1** | **2 m** mosaics (50 km tiles); 10/32 m too | ≥ ~60°N — Iceland, Norway, Faroes, all Arctic | Public; PGC Acknowledgement Policy (attribution). Alaska post-2022-06 restricted (irrelevant to us). Verified 2026-08 from pgc.umn.edu | GeoTIFF | PGC HTTP + AWS S3 + STAC; OpenTopography |
| **USGS 3DEP** | **1 m** lidar DTM | Conterminous US (near-complete) | US public domain | GeoTIFF | The National Map API, `s3://prd-tnm` |
| **NZ LINZ** | 1 m lidar DEM/DSM (coverage patchy in Fiordland — verify per-AOI) | New Zealand | CC-BY 4.0 (redistribution OK with attribution) | GeoTIFF | LINZ Data Service, OpenTopography |
| **Norwegian Kartverket (høydedata.no)** | DTM 1 m / 10 m | Norway | CC-BY 4.0 | GeoTIFF | hoydedata.no export API |
| **GEBCO_2026 grid** (bathymetry) | 15″ (~460 m) | Global ocean + land | **Public domain**, redistribution permitted (verified 2026-08 from gebco.net) | netCDF / GeoTIFF | download.gebco.net (user-defined AOI) |
| **EMODnet Bathymetry** | ~115 m | European seas (fjord floors, Faroes, skerries) | Free; verify redistribution terms at acquisition | GeoTIFF/ASC | EMODnet portal |

**Recommendation: GLO-30 is the backbone** — one source, one licence, global,
COG-on-S3 (clip without downloading whole tiles), native ~30 m. SRTM is the
public-domain fallback below 60°N (note: SRTM has voids exactly where terrain
is most dramatic, e.g. the Karakoram — GLO-30 does not). ASTER only as
tie-breaker; it is noisier. ArcticDEM / 3DEP / LINZ / Kartverket are the
fine-tier and visual-reference upgrades for specific regions. GEBCO for
below-sea context everywhere; EMODnet where a fjord floor or skerry field
actually matters (GEBCO's 460 m posting cannot see a skerry).

Licence note for the library itself: everything in the backbone set may be
mirrored with attribution, so the corpus can ship. The one house precedent to
copy is WorldClim (see `conditioning_artifacts.py`): where a source forbids
mirroring, pin the upstream URL + sha256 (`split_source` pattern) instead of
rehosting.

---

## 3. The library: 12 regions

Sizing: one coarse tile is 15.36 km. Boxes below are **2×2 tiles (30.72 km)**
unless marked; the Karakoram gets 3×3 (46.08 km) because its relief wavelength
does not fit in less. Boxes are axis-aligned lat/lon around a stated center;
the acquisition step snaps them to the local metric grid. Relief numbers are
map-derived estimates to be *replaced by measured values* (§5) at acquisition
— per the house rule, real numbers before conclusions.

**W2k** below (coined term): the largest elevation gain along any 2 km
horizontal line in the clip — the "wall score". It is the single number that
captures a fjord wall; rolling forest sits near 100–200 m, Milford Sound near
1,500 m.

| # | Region | Grammar | Center | Bbox (lat, lon) | Tiles | Best sources | Epicness in one line |
|---|---|---|---|---|---|---|---|
| 1 | **Milford Sound, Fiordland NZ** | Fjord: glacial trough, near-vertical walls, hanging valleys | 44.62°S 167.90°E | −44.758..−44.482, 167.706..168.094 | 2×2 | GLO-30 + LINZ 1 m (verify AOI) + LINZ/NIWA bathy | Mitre Peak: 1,683 m straight out of the sea in under 2 km; fjord floor −290 m |
| 2 | **Geirangerfjord, Norway** (alternate fjord) | Fjord, branching; more sinuous than Milford | 62.10°N 7.00°E | 61.962..62.238, 6.705..7.295 | 2×2 | GLO-30 + Kartverket DTM1 + ArcticDEM 2 m | ~1,400 m walls in 1.5–2 km; waterfalls off hanging valleys |
| 3 | **Lofoten (Reine), Norway** | Alpine archipelago: glacially over-steepened ridges rising from open sea, near-zero catchment area | 67.95°N 13.00°E | 67.812..68.088, 12.633..13.367 | 2×2 | GLO-30 (60–70°N lon-thinned) + ArcticDEM 2 m + EMODnet bathy | 700–1,100 m granite horns within ~1.5 km of the shoreline, both sides |
| 4 | **Northern Faroes (Enniberg/Viðoy)** | Basalt ramp islands: gentle dip slope one side, sheer sea-cliff scarp the other | 62.32°N 6.55°W | 62.182..62.458, −6.847..−6.253 | 2×2 | GLO-30 + ArcticDEM 2 m + EMODnet | Enniberg: ~750 m of cliff meeting the ocean at effectively zero horizontal distance |
| 5 | **Herðubreið, Iceland** | Tuya + lava tableland: dead-flat porous desert plain, isolated steep-sided flat-topped mountain | 65.18°N 16.35°W | 65.042..65.318, −16.679..−16.021 | 2×2 | GLO-30 + ArcticDEM 2 m | A 1,000 m table mountain standing alone on a plain that is otherwise flat to the horizon |
| 6 | **Skeiðarársandur–Öræfajökull, Iceland** | Glacial outwash: braided black-sand plain at sea level against an ice-capped volcanic escarpment | 63.99°N 16.85°W | 63.852..64.128, −17.165..−16.535 | 2×2 | GLO-30 + ArcticDEM 2 m | Hvannadalshnúkur, 2,110 m, ~12 km from a plain flat enough to land aircraft on |
| 7 | **Hunza–Rakaposhi, Karakoram** | Massive mountains: greatest sustained local relief on Earth; km-deep V-valleys, active glaciers | 36.28°N 74.55°E | 36.073..36.487, 74.293..74.807 | **3×3** | GLO-30 (SRTM voids here) | Rakaposhi: 5,900 m of continuous relief above the Hunza valley in ~11 km |
| 8 | **Wadi Rum, Jordan** | Monumental rock desert: flat sand floor + sheer sandstone/granite massifs, near-zero drainage | 29.57°N 35.42°E | 29.432..29.708, 35.261..35.579 | 2×2 | GLO-30, SRTM | 700–800 m vertical rock faces rising from a dead-flat valley floor (a literal film desert) |
| 9 | **Sossusvlei erg, Namib** | Dune sea: periodic ~1–2 km wavelength megadunes, zero channels | 24.73°S 15.34°E | −24.868..−24.592, 15.188..15.492 | 2×2 | GLO-30, SRTM | 300–380 m star/linear dunes — mountains made of sand, in ranks |
| 10 | **Island in the Sky, Canyonlands, Utah** | Mesa/canyon: layer-cake cliff-and-bench, inverted relief, incision instead of peaks | 38.40°N 109.90°W | 38.262..38.538, −110.076..−109.724 | 2×2 | GLO-30 + 3DEP 1 m | The ground *drops* 600 m in stacked sheer benches below a flat rim |
| 11 | **Guilin–Yangshuo karst, China** | Tower karst: hundreds of isolated 100–300 m limestone towers on a flat alluvial plain | 24.78°N 110.49°E | 24.642..24.918, 110.338..110.642 | 2×2 | GLO-30, SRTM | A forest of stone towers — vertical at 200 m scale, flat at 2 km scale |
| 12 | **Appalachian Plateau, WV** | Temperate rolling forest: maximally dissected plateau, dense dendritic drainage, accordant ridgelines | 38.50°N 80.50°W | 38.362..38.638, −80.676..−80.324 | 2×2 | GLO-30 + 3DEP 1 m | Endless green ridges to the horizon — the Shire-to-Bree connective tissue between the epic set pieces |

Optional 13th if the set feels short on cone shapes: **Mt. Rainier**
(46.85°N 121.76°W; 46.712..46.988, −121.962..−121.558; GLO-30 + 3DEP) —
stratovolcano grammar: radial drainage, monotonic slope-vs-elevation, 2,800 m
sustained rise over 10 km. A lone mountain, literally.

### Why these are different in relief-statistics terms, not vibes

Each row was chosen to occupy a distinct corner of the measurement space in
§5:

* **Fjords (1, 2)** — extreme W2k (1,200–1,600 m) with *low* drainage density
  (walls too steep to hold channels); U-shaped valley cross-sections
  (curvature concentrated at the shoulder, not the floor); bimodal hypsometry
  (water surface + high plateau); relief keeps growing with window size out
  to ~5 km.
* **Alpine archipelago (3)** — like a fjord turned inside out: high W2k, but
  land fraction ~0.3 and catchments so short that `net.mean_path_to_channel`
  collapses; coastline dimension high.
* **Basalt ramp (4)** — the only strongly *asymmetric* slope distribution in
  the set: one mode at 5–10° (dip slope), one at >60° (scarp), and the scarp
  always faces the sea.
* **Tuya + tableland (5)** — bimodal hypsometry with near-zero drainage
  density everywhere (porous lava): flat plain histogram spike + isolated
  high plateau spike, nothing between.
* **Outwash (6)** — the steepest *gradient of roughness* in the library:
  detrended S2(d) is near-zero on the sandur and extreme 6 km away; the
  braided plain has maximal channel area at minimal relief.
* **Karakoram (7)** — amplitude ceiling: 5–10 km windowed relief
  4,000–6,000 m (an order of magnitude past anything our generator currently
  emits); V-shaped cross-sections, high drainage density *and* high relief
  simultaneously — the combination none of the other rows has.
* **Rock desert (8)** and **karst (11)** — both bimodal slope (flat +
  near-vertical) with near-zero drainage, distinguished by spatial scale and
  polarity: Wadi Rum is large connected massifs (relief keeps growing past
  2 km windows), Guilin is small isolated towers (relief saturates by
  ~500 m).
* **Erg (9)** — the only *periodic* surface: structure function S(d)
  oscillates at the dune wavelength instead of growing; zero channels; slope
  distribution capped near the angle of repose (~32°).
* **Mesa/canyon (10)** — hypsometric integral very high (most mass near the
  rim — the inverse of a mountain range); slope distribution concentrated in
  discrete cliff bands; drainage sparse but deeply incised.
* **Rolling forest (12)** — maximal drainage density and junction count,
  moderate unimodal slopes (p90 ~20–25°), hypsometric integral mid-range,
  W2k ~150–250 m. This is the *control*: terrain our current generator is
  already closest to.

---

## 4. How the library would be used — three routes (the menu; none authorized yet)

Ranked by how soon each can put "epic" on screen. Common constraint from §1:
any route that feeds a DEM into generation makes those bytes part of the
world identity — pinned sha256 in a `data/conditioning-artifacts.json`-style
manifest, fetched-and-verified, and it rolls `provider_id` (a new world
namespace).

**(c) Validation targets only — soonest, zero pipeline change, no
authorization needed.** Compute the §5 statistics on each DEM clip and on our
generated tiles; the gap becomes numbers; close it with the levers we already
have (`terrain-service/docs/worldgen-levers.md` §3): `coarse_pooling` with
`elev_coarse_pool_mode=max` is upstream's own documented "more intense
terrain" knob; `frequency_mult[0]` sets landmass scale; `cond_snr[0]` sets
how tightly the model obeys the sketch. Effort: days. Determinism: lever
changes must roll `provider_id` (known gap, levers doc §5), but no new pinned
artifacts. Honest ceiling: levers can plausibly buy 2–3× relief and bigger
landmasses; they cannot buy a fjord — the checkpoint's learned texture and a
Perlin sketch have no U-valley grammar in them. This route tells us *how far*
the existing machine can be pushed before we spend on (a) or (b).

**(a) Regional conditioning swap — the real jump, medium effort.** The
injection point already exists (`world_pipeline.py:899`; levers doc §4): the
sketch can be replaced by GeoTIFFs, one value per **7.68 km cell**, and
`tiff_export.py`/the custom-conditioning branch consumes exactly that. A DEM
region pooled to 7.68 km conditions the diffusion so a region of *our* world
inherits the real region's large-scale structure — where the walls, valleys
and plains are — while the model invents consistent 30 m detail. Two honest
caveats: (i) at 7.68 km per cell, a 2×2-tile clip is only a 4×4-cell sketch —
for conditioning use, acquire the same centers at 4×4–8×8 tiles (61–123 km);
the 2×2 boxes above are the *measurement* extent; (ii) conditioning steers
placement, not texture — the model's slope grammar stays whatever it learned,
so cliffs will be steep-ish, not Enniberg. Effort: ~1–2 weeks including the
identity work (the DEM clip becomes a pinned conditioning artifact;
`provider_id` rolls; this is a new world). Determinism: clean if and only if
the clip is pinned bytes — the whole point of §1's constraint. This is the
best epicness-per-effort once (c) has been exhausted.

**(b) Fine-tune / LoRA the checkpoint on a DEM corpus — deepest, slowest,
riskiest.** Only this changes the *texture grammar itself* — what a slope
looks like at 30 m — because it changes the weights, which is what
conditioning cannot reach. Requires: a training corpus (thousands of 30 m
clips, easily assembled from GLO-30 under its licence), GPU time, and
re-validation of everything downstream (climate channels come from the same
model — a terrain-only fine-tune can skew them). Determinism: a new
checkpoint hash = new `provider_id` = full world regeneration; training
itself is not reproducible bit-for-bit, so the *output weights* are pinned,
never the recipe (same doctrine as tiles). Effort: weeks, GPU rental, real
risk of degrading the model. Do this only if (a) demonstrates the placement
is right but the texture still reads as mush.

**Ranked for "epic soonest": (c) → (a) → (b).** (c) starts today and needs no
authorization; (a) is the visible leap; (b) is the long game.

---

## 5. Measurement: making "dramatic enough" a number

We already own the vocabulary: `voxel-core/bench/terrainprobe.cpp` computes
structure functions and a full drainage suite on *our* side and emits
machine-greppable keys (`--baseline`). The DEM side must speak the same
language so the two columns line up. Reuse these terrainprobe metrics
verbatim (same names, same definitions):

* **S(d)** — structure function, mean |h(x+d)−h(x)| vs lag, with local Hurst
  H (`terrainprobe.cpp:108`); and **S2(d)** — detrended second-difference
  roughness (`:147`), which is the one to trust on sloped ground.
* **Drainage suite** (`drainageStats`, `:1758`; baseline keys at
  `:2861-2885`): `net.drainage_density`, `net.junctions_per_km2`,
  `net.exceedance_slope_beta` + `net.exceedance_fit_r2` (catchment-area
  power law — real dendritic networks fit, manufactured ones over-fit; see
  the warning at `:1554` about pit-filling inventing plausible networks on
  any input), `net.mean_path_to_channel`, `net.channel_components`,
  `fill.mean_depth` / `fill.max_depth`, `raw.interior_sinks_per_km2`.

Add these (computed identically on DEM clips and on generated coarse tiles; a
~100-line numpy script on the DEM side, new rows in terrainprobe's baseline
block on ours):

* **Windowed relief R(w)** — (max−min) inside sliding windows of w = 1, 2.5,
  5, 10 km; report p50 and p95 over window positions. This is the "scale"
  axis: Karakoram R(5 km) ≈ 4,000+ m, WV plateau ≈ 250 m.
* **W2k wall score** — p99.9 over the clip of directional elevation gain
  along 2 km transects (8 directions). The fjord number; secondarily W500m
  for karst/canyon walls.
* **Slope distribution** at 30 m posting — p50/p90/p99 plus a bimodality
  coefficient; separates ramp-scarp (Faroes), flat+vertical (Wadi Rum,
  Guilin), unimodal rolling (WV), repose-capped (erg).
* **Hypsometric integral** — area-normalized elevation CDF integral;
  mesa-country high, mountain-range low, tuya bimodal (report the histogram's
  two largest modes too).
* **TRI/VRM** (terrain ruggedness / vector ruggedness) — cheap, standard,
  good for cross-checking against published values for the same regions.
* **Periodicity check** — first minimum/oscillation of S(d): flags the erg
  (real periodicity) and would also flag any grid artifact of ours (compare
  terrainprobe's seam scan, `:296`).
* **For coastal rows (1–4, 6)** — land fraction, coastline dimension
  (box-count), and with bathymetry merged: the *continuous* W2k across the
  waterline (Milford's wall is really 1,973 m: −290 m floor to +1,683 m
  peak).

Acceptance framing for route (c): pick a target row per grammar (e.g. "our
mountain province should reach R(5 km) p95 ≥ 1,500 m and W2k ≥ 600 m; our
rolling province should hold `net.drainage_density` within 2× of WV") and
tune levers until the numbers move. Per the house rule, screenshots still
judge the result — these numbers only decide what to try next; they are not
the verdict.

---

## 6. Storage, pinning, repo hygiene

This repo has already hit GitHub's 100 MB push wall (`.gitignore:102`; 192 MB
tiles once blocked 112 commits). Rules:

* **No rasters in git, ever** — not even "small" ones. The corpus lives in
  `terrain-service/data/dem-reference/` which gets a `.gitignore` entry, on
  the data drive like the tile caches.
* **The manifest is in git; the bytes are not.** A committed
  `terrain-service/data/dem-reference.json` pins, per clip: region name,
  bbox, source product + exact URL(s), retrieval date, sha256 of the stored
  clip, and the reprojection recipe (target CRS, grid origin, kernel) — the
  `conditioning-artifacts.json` pattern (schema idea, not shared file).
  `fetch_dem_reference.py` verifies sha256 of what arrived *before*
  installing it, exactly like `fetch_conditioning.py`.
* **Sizes are small until the fine tier.** A 2×2-tile coarse clip is
  ~1024×1024 float32 ≈ 4 MB; all twelve regions' coarse clips + GEBCO
  context ≈ under 100 MB total. Fine-tier clips (1–2 m posting over 30 km)
  are 1–4 GB *each* — fetch only when a route actually needs one, never
  mirror casually.
* **Mirroring**: redistributable clips (everything in §2's backbone) may be
  mirrored as assets on a dedicated, never-advanced release tag
  (`dem-reference-v1`, the `conditioning-v1` precedent) so a fresh box needs
  no upstream account. Any source whose terms forbid mirroring gets the
  WorldClim treatment: upstream URL + `#member` + sha256 pin, no rehost.
* **Graduation**: if route (a) is ever authorized, the chosen clip moves from
  this reference manifest into `data/conditioning-artifacts.json` proper and
  becomes part of `provider_id`. Until then nothing here touches world
  identity.

---

## 7. The helper script (inert)

`terrain-service/tools/fetch_dem_reference.py` — written as part of this
plan, **does nothing until explicitly run**, and has not been run against big
data.

* `--list` prints the region table (embedded, same as §3).
* `--tiles REGION` computes which 1°×1° GLO-30/GLO-90 COG tiles cover a
  region's bbox and prints their public HTTPS URLs (AWS Open Data bucket, no
  account) — without downloading anything.
* `--fetch REGION --dest DIR [--source glo30|glo90]` downloads those tiles,
  sha256-hashes each *as received*, and appends to a manifest stub. Guarded
  by `--max-mb` (default 200): it refuses to start a transfer projected past
  the cap, so it cannot be casually pointed at fine-resolution data.
* `--inspect FILE` prints size + sha256 (+ min/max/mean and posting if
  `rasterio` is importable; degrades gracefully if not).
* It HEAD-checks the first URL and fails loudly if the AWS key-naming
  assumption (`Copernicus_DSM_COG_10_N62_00_E007_00_DEM/…`) is wrong, rather
  than fabricating data. (The naming was verified live 2026-08: a HEAD on the
  Lofoten N67/E012 GLO-30 tile returned 200. HEAD only — no raster bytes were
  transferred.) Transfers are verified by byte count against Content-Length,
  not just exit status.

Bathymetry is manual for now: GEBCO's AOI download app (download.gebco.net)
has no stable per-tile URLs to pin; the manifest records the request bbox and
the received file's sha256 instead.

---

## 8. Open items to verify at acquisition time (not before)

1. Exact Copernicus DEM licence wording on redistribution (the AWS registry
   states free public use; the ESA licence PDF was unreachable at research
   time — pin the licence text itself into the manifest when fetched).
2. GLO-30's east–west sampling bands above 50°N (affects Lofoten/Faroes
   reprojection kernel choice only).
3. LINZ 1 m lidar coverage over the Milford box (Fiordland is patchy; the
   region stands on GLO-30 + bathy regardless).
4. ArcticDEM v4.1 acknowledgement text (attribution string for the
   manifest).
5. Each region's measured relief numbers vs the estimates in §3 — the §5
   script replaces every estimate with a measurement before any region is
   used to judge our generator.

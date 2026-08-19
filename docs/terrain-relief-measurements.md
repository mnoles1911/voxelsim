# Terrain relief measurements — our world vs the DEM reference targets

Date: 2026-08-18. Branch `claude/f6-interior-rim-injection`, worldgen v27,
seed 20260719. MEASUREMENT ONLY: nothing in generation, placement, manifests
or specs was changed to produce this. Companion to
`docs/dem-reference-library.md` §5, which asked for exactly these statistics.

**The question**: the owner wants "Lord of the Rings" scale. We previously had
one number — 323 m of rise over a 4 km walk near the alpine lake — and an
instrument (`vxc_terrainprobe`) whose lags stop at 120 m, so it measures
*roughness*, not kilometre-scale *drama*. This document adds the km-scale
instrument, runs it over everything we have baked, and puts our numbers next
to the real-world reference targets.

**Headline, in plain words**: our world already contains individual mountains
with real-DEM-class size — one wall gains 1,964 m over a 2 km line (Milford
Sound's famous wall is 1,683 m), and one 10 km circle spans 5,610 m of relief
(the Hunza valley, the deepest on Earth, spans ~5,900 m in 11 km). What our
world does NOT have: that drama at the coastline (the big walls are all
inland; the best sea-to-summit wall is 1,506 m and there is exactly one),
near-vertical rock (steepness saturates at ~50–58° everywhere; a fjord wall
is 60–80°+), and abundance (6 tiles of 289 have a 1,500 m wall; 257 of 289
never see 1,000 m). The gap is placement and shape grammar, not amplitude.

---

## 1. The instrument

`vxc_reliefprobe` (`voxel-core/bench/reliefprobe.cpp`, built as
`build/bench/Release/vxc_reliefprobe.exe`). A sibling of `vxc_terrainprobe`,
not a mode of it: terrainprobe guarantees byte-identical output for existing
command lines and samples transects/local lattices; this needs a dense region
raster. It does NOT duplicate terrainprobe's S(d)/S2(d)/drainage suite —
those stay authoritative for roughness and channel structure.

It samples `Amplifier::surfaceMm` — the production code path for the surface
the client draws — over the real tile samplers (`TileGridSampler`,
`FineTileSampler`), never a reconstruction (the rule that exists because
Python ground rebuilds have been wrong by 480 m and by 100x). Multithreaded;
fine tiles are `prewarm()`ed first because a cold fine-block query is a
write.

```
vxc_reliefprobe <tiledir> <seed> <x0M> <y0M> <widthM> [heightM]
    [--fine-dir DIR] [--zstd PATH] [--stride-m N] [--threads N]
    [--json PATH] [--region NAME] [--baseline]
```

Output: a human summary, a `--baseline`-style fixed-key table
(`=== VXC_RELIEFPROBE BASELINE v1 ===`, same conventions as terrainprobe),
and `--json` for diffing across regions and against DEM clips.

### Definitions (the DEM side must implement these identically)

Sampling: a grid of `nx = widthM/stride` by `ny = heightM/stride` points,
point (i,j) at world metres `(x0 + i*stride, y0 + j*stride)`, elevation in
metres. **Stride = 30 m** everywhere below: it is both the coarse tier's
native posting (512 px / 15.36 km tile) and the posting of Copernicus
GLO-30/SRTM — so every number is comparable lag-for-lag with a 30 m DEM clip
with no scale correction (`dem-reference-library.md` §1). Sub-30 m amplifier
octaves add decimetre–metre texture that cannot move km-window max−min.

Datum: all statistics are computed on `hc = max(h, 0)` — the GLO-30
convention, which reads ~0 over sea water — except keys suffixed `_bathy`
(raw ground, for the continuous-wall question) and `land_frac` (fraction of
samples with raw h > 0).

1. **Windowed relief R(w)**, w ∈ {1, 2.5, 5, 10} km: max−min of `hc` inside
   an n×n sample window, n = round(w/stride)+1 (spans 990 / 2,490 / 5,010 /
   9,990 m — exact span in the output). Population = every fully-interior
   window position, dense sliding. Reported p50/p90/p99/max + world-metre
   center of the max window.
2. **Wall score W(L)**, L ∈ {2000, 500} m: |hc(x+Lu) − hc(x)| over every
   grid position and the four lattice directions E, N, NE, SE (each
   unordered endpoint pair once — identical to "elevation gain along any
   straight L-metre transect, 8 compass directions", since every pair is one
   transect walked uphill). Realised lags on the 30 m lattice: axis 2,010 m /
   diagonal 1,994.0 m (W2k), axis 510 m / diagonal 509.1 m (W500). Reported
   p50/p99/max + both endpoints of the max.
3. **Slope** at 30 m posting: central differences,
   `atan(sqrt(sx^2+sy^2))`, `sx = (hc[i+1,j]-hc[i-1,j])/(2*30)`. Reported
   mean/p50/p90/p99 and the **bimodality coefficient**
   `BC = (g1^2 + 1)/(g2 + 3)` with population skewness g1 and population
   excess kurtosis g2, no finite-n correction; BC > 5/9 ≈ 0.555 suggests a
   two-humped distribution. A 1°-bin histogram ships in the JSON.
4. **Hypsometric integral**, elevation-relief ratio form:
   `HI = (mean − min)/(max − min)` over all samples.
5. **TRI** (Riley et al. 1999): per interior cell,
   `sqrt( sum over 8 neighbours (h_n − h_c)^2 )` at 30 m posting.
   **VRM** (Sappington et al. 2007), 3×3: unit surface normal
   `(−gx, −gy, 1)/|·|` from the same central differences, then
   `1 − |Σ normals|/9`.

Percentiles are nearest-rank on fixed-width histograms (elevation/relief/
wall 0.1 m; slope 0.02°; TRI 0.01 m; VRM 1e-5); reported value = bin
midpoint, error ≤ half a bin.

Edge rule: the coarse carrier prefilters a 12×12 raw block, so a sample can
probe up to 8 px = 240 m past itself; any query outside the loaded tiles
reads open ocean (sea level). Every region below is therefore inset so that
`config.coarse_misses = 0`: per-tile runs are the tile footprint inset 240 m
(14,880 m square), the world run is inset 480 m, and multi-tile fine regions
inset 240 m. The probe prints a warning whenever any query fell back.

---

## 2. What was measured

Real tiles only. Coarse: all 289 s1 tiles (17×17, world spans x,y ∈
[−245,760, +15,360) m) from
`/d/vox-wet-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1`.
Fine: the 15 s16 tiles present at run time (2026-08-18 ~12:00; the bake was
still producing — savanna −5,−11 and tundra −5,−7 landed minutes before
their runs; every load reported `rejected=0`) from
`tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd/000000000135276f/s16`:
the alpine six (−3..−5, −4..−5), tundra (−5,−7), savanna (−5,−11),
desert/grassland (−7,−5), rainforest (−7,−12), temperate (−8,−11), taiga
(−11,−6), beach (−15,−7), plus two extra tiles (−6,−11) and (−7,−7) that
match no v27 representative site.

Raw outputs (all committed): `docs/measurements/relief-2026-08-18/` —
`world-coarse.{txt,json}`, `tile-sweep-coarse.csv` (all 289 tiles, one row
each), `hotspot-massif.{txt,json}`, `hotspot-west.{txt,json}`,
`alpine-six-{fine,coarse}.{txt,json}`, `coast-t-5_-12.{txt,json}`, and
`fine-t<X>_<Y>.{txt,json}` per fine tile.

---

## 3. The world's drama distribution

World run: 261.12 km square minus the 480 m inset, 75.2M columns, 44.4%
land, elevations −6,161 (seabed) to +6,330 m.

| statistic | p50 | p90 | p99 | max | where the max is |
|---|---|---|---|---|---|
| R(1 km) | 0.1 | 333 | 658 | **1,522 m** | (−30,255, −40,755) |
| R(2.5 km) | 4 | 621 | 1,204 | **2,641 m** | (−29,895, −40,215) |
| R(5 km) | 40 | 971 | 1,829 | **4,046 m** | (−28,185, −40,905) |
| R(10 km) | 195 | 1,501 | 2,740 | **5,610 m** | (−26,025, −39,075) |
| W2k | 0.1 | — | 601 | **1,964 m** | (−33,030, −38,280)→(−33,030, −40,290) |
| W500 | 0.1 | — | 248 | **982 m** | (−31,620, −43,260)→(−31,110, −43,260) |

(p50s are near zero because half the samples are sea surface — the GLO-30
convention. The per-tile sweep below is the land-side view.)

Across the 289 tiles (each tile's own maximum W2k): median tile 319 m, p90
1,005 m, best 1,964 m. **32 tiles have a >1,000 m wall; 6 exceed 1,500 m;
151 exceed 250 m** (the top of the Appalachian reference band). Slope p99
across land-majority tiles: median 39.4°, hardest tile 58.1° — no tile
anywhere produces near-vertical ground at 30 m posting.

### The upper tail — where to go stand

1. **The great wall of the massif** (tile −3,−3, coarse):
   (−33,030, −38,280) → (−33,030, −40,290), climbing 3,560 → 5,525 m. That
   is +1,964 m over 2 km — 117% of Milford Sound's wall — but it starts on
   high ground, not at water.
2. **The desert rampart** (tile −7,−5 — the desert/grassland fine site!):
   fine tier W2k max **1,685 m**, (−93,930, −68,280) → (−92,520, −66,870),
   506 → 2,191 m. Numerically Milford's equal, standing over the desert
   floor two tiles from the survey sites.
3. **The sea wall** (tile −5,−12, coastal, coarse): (−72,930, −182,070) →
   (−72,930, −180,060), **1.2 → 1,506 m** — the only Milford-*shaped* thing
   we have: a 1.5 km wall straight out of the water. One of a kind in this
   world.
4. The west hotspot (tiles −7..−6, −5..−4): second independent massif, W2k
   1,803 m at (−88,140, −70,860) → (−88,140, −68,850), 577 → 2,379 m.
5. Submarine honourable mention: the raw-ground (bathy) W2k max is 2,514 m
   at (−62,160, −241,200) — an underwater cliff on the south edge; invisible
   in game, listed for completeness.

### Regions, side by side

2×2-tile boxes (30.72 km — the DEM library's measurement extent) and the
fine sites. Coarse tier unless marked; fine rows are the baked fine tier
sampled at the same 30 m posting.

| region | elev (m) | R(2.5k) p99 / max | R(10k) max | W2k p99 / max | slope p90 / p99 (°) | BC | HI | TRI mean |
|---|---|---|---|---|---|---|---|---|
| **hotspot massif 2×2t** | 420–6,330 | 2,198 / 2,641 | 5,610 | 1,202 / 1,964 | 39.2 / 52.9 | 0.56 | 0.27 | 28.5 |
| **hotspot west 2×2t** | 15–2,962 | 1,930 / 2,240 | 2,917 | 1,125 / 1,803 | 37.1 / 50.0 | 0.52 | 0.31 | 27.4 |
| alpine six, fine (45.6×30.2 km) | 121–4,983 | 1,441 / 1,543 | 3,597 | 840 / 1,342 | 41.6 / 50.3 | 0.56 | 0.34 | 37.1 |
| alpine-lake tile −3,−4 fine | 882–4,983 | 1,478 / 1,524 | 3,597 | 976 / 1,300 | 46.6 / 53.5 | 0.48 | 0.40 | 53.9 |
| desert/grassland −7,−5 fine | 21–2,205 | 1,687 / 1,937 | 2,173 | 962 / 1,685 | 31.6 / 49.0 | 0.72 | 0.10 | 18.0 |
| coastal wall tile −5,−12 | 0–1,563 | 1,515 / 1,563 | (n/a, 14.9 km) | 975 / 1,505 | 29.4 / 43.3 | 0.65 | 0.13 | 16.3 |
| temperate −8,−11 fine | 0–998 | 805 / 828 | 998 | 566 / 773 | 36.1 / 46.7 | 0.72 | 0.20 | 21.8 |
| rainforest −7,−12 fine | 71–1,084 | 823 / 878 | 1,013 | 497 / 758 | 21.4 / 38.6 | 0.75 | 0.19 | 13.0 |
| savanna −5,−11 fine | 3–731 | 543 / 583 | 707 | 351 / 569 | 33.4 / 43.6 | 0.64 | 0.31 | 23.7 |
| tundra −5,−7 fine | 0–635 | 440 / 478 | 635 | 312 / 459 | 12.4 / 32.8 | 0.64 | 0.56 | 8.5 |
| taiga −11,−6 fine | 0–313 | 211 / 212 | 303 | 161 / 208 | 4.6 / 25.2 | 0.74 | 0.39 | 3.9 |
| beach −15,−7 fine | 0–11 | 8 / 10 | 11 | 4 / 7 | 0.6 / 1.5 | 0.55 | 0.11 | 0.3 |

Fine vs coarse, same rectangles: km-scale relief is decided by the coarse
tier — the alpine-lake tile reads W2k 1,281 m coarse vs 1,300 m fine (+1.5%),
R(10k) 3,571 vs 3,597 m. The fine bake adds roughness (TRI 46.7 → 53.9,
slope mean +2.5°), exactly as designed. So drama is a conditioning/coarse
question; the bake neither creates nor destroys it.

---

## 4. The honest gap to the DEM reference targets

Reference numbers from `docs/dem-reference-library.md` §3 (map-derived
estimates, to be replaced by measured values at DEM acquisition).

| target | reference number | ours, best measured | gap |
|---|---|---|---|
| **Milford Sound wall** | W2k 1,683 m out of the sea (1,973 m from fjord floor) | W2k max 1,964 m — but inland, 3,560→5,525 m; best *sea* wall 1,506 m (one instance) | Magnitude: none. **At the waterline: −11% and it exists once.** No U-valley/hanging-valley grammar anywhere (slope p99 ≤ 53° vs near-vertical walls; the massif's BC 0.56 barely crosses the bimodal line and its two modes are plain-vs-mountainside, not floor-vs-wall). |
| **Hunza/Rakaposhi relief** | ~5,900 m in ~11 km; R(5–10 km) 4,000–6,000 m | R(10 km) max 5,610 m (95%); R(5 km) max 4,046 m | Peak amplitude: essentially none. **Abundance: world R(10 km) p99 is 2,740 m — Hunza-class relief occupies one massif (~3 tiles of 289).** |
| **Canyonlands incision** | ~600 m of stacked sheer benches below a flat rim; HI very high; slope in discrete cliff bands | W500 max 982 m but it is a mountainside, not an incised rim; no measured region pairs a flat rim with cliff bands; hotspot HI 0.27–0.31 (mountain-shaped, not mesa-shaped) | **Grammar entirely absent** — nothing mesa/canyon-like exists to measure. |
| **Appalachian rolling control** | W2k 150–250 m, slope p90 ~20–25°, unimodal, dense drainage | temperate tile: W2k p99 566 / max 773 m, slope p90 36° | We *overshoot* the control: our "rolling" terrain is 2–3× too tall and ~12° too steep at p90. The connective-tissue grammar is missing from the other side. |

Three structural facts the table compresses:

1. **Amplitude is not the deficit.** The generator, at current settings,
   already emits 6,330 m peaks, Hunza-class 10 km relief and
   Milford-class 2 km gains. The owner's 323 m/4 km transect was one mild
   walk near the lake; the same fine site's tile carries a 1,300 m wall.
2. **Placement is.** Drama is inland and rare: p99 ≪ max on every world
   statistic, 6/289 tiles over 1,500 m W2k, and exactly one significant
   land-from-sea wall — because the Perlin sketch is isotropic and blobby
   (levers doc §2), big elevations land in continent interiors, away from
   the histogram's sea-level mass.
3. **Shape grammar is.** Slope p99 saturates at 50–58° in every region at
   30 m posting; nothing is near-vertical, nothing has a flat-floored
   trough, nothing is periodic or benched. That ceiling is the checkpoint's
   learned texture — no measured region escapes it, including the ones
   whose km-scale numbers match real mountain ranges.

---

## 5. Verdict: can the levers close it?

The numbers split the gap in two. The **abundance/placement half is
plausibly lever-territory**: `frequency_mult[0]` is measured (levers doc §3)
to grow landmasses ~60% with land fraction unchanged, which physically puts
more mountain mass adjacent to coasts; `coarse_pooling` +
`elev_coarse_pool_mode=max` is upstream's own documented "more intense
terrain" knob and compresses horizontal space — mechanically it moves R(w)
and W2k at fixed w, which is exactly the axis where our p99 (2,740 m at
10 km) trails our own max (5,610 m); and `cond_snr[0]` tightens obedience to
a sketch whose elevation histogram is already Earth's. A measured lever
sweep — rerun this probe, diff the JSONs — could credibly multiply the
*frequency* of 1,500–2,000 m walls and push R(10 km) p99 toward the current
max, i.e. buy "the Misty Mountains are common and sometimes meet the sea"
(with the levers-doc §5 caveat that any adopted change must roll
`provider_id`). The **grammar half is not lever-territory, on this
evidence**: every region we measured — mild or extreme, coarse or fine —
shares the same ~50–58° slope-p99 ceiling and mountain-shaped hypsometry,
because those come from the checkpoint's learned texture and an isotropic
Perlin sketch, which no frequency/pooling/SNR setting changes; a fjord's
sea-level 1,700 m wall, a canyon's benched rim, or WV-style low rolling
relief are *arrangements*, and the only input that carries arrangement is
the conditioning picture itself. That is route (a) of
`dem-reference-library.md` §4 — regional DEM conditioning at 7.68 km/cell —
and our own measurements now say the model can render DEM-class amplitude
once something tells it where to put it. Recommendation implied by the
numbers: exhaust the cheap lever sweep for abundance, but budget DEM
conditioning as the only credible route to fjord/canyon/rolling grammar.

---

## 6. Reproduction

```sh
# build
cmake --build build --target vxc_reliefprobe --config Release
# world (inset 480 m)
vxc_reliefprobe <s1dir> 20260719 -245280 -245280 260160 260160 \
    --region world-coarse --json out.json
# one tile, coarse (tile tx,ty; inset 240 m)
vxc_reliefprobe <s1dir> 20260719 $((tx*15360+240)) $((ty*15360+240)) 14880 14880
# same tile, baked fine tier
vxc_reliefprobe <s1dir> 20260719 $((tx*15360+240)) $((ty*15360+240)) 14880 14880 \
    --fine-dir <s16dir>
```

The 289-tile sweep is a 20-line loop over `--baseline` output (see
`docs/measurements/relief-2026-08-18/tile-sweep-coarse.csv` header for the
columns); ~6 minutes wall for the whole battery on 12 threads.

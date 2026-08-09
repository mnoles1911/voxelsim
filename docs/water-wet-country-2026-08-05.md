# Wet country: the alpine block, baked at bv13

> **STATUS 2026-08-09: [CURRENT].** Region-selection rationale and measurement
> record; independent of which solver draws the water. Still the reason the
> wet alpine block is the default measurement region under the re-architecture.

> **Current (2026-08-05).** This is why the wet alpine block is now the default
> place to measure water, and what changed when it became so. Two notes:
>
> * Its numbers are **`bake_ver` 13**, in cache root `D:\vox-wet-cache`,
>   namespace `…-ba9c62170`. A `bake_ver` **14** bake of the same six tiles
>   exists in the same root under `…-b10cf6d2c`; it adds slope face contact and
>   the numbers here have not been re-taken against it.
> * The probe figures quoted from this block elsewhere — 0.560% wet, 467 pieces,
>   424.9 km of channel — come from a specific window, and getting the window
>   wrong silently reports a dry world. The exact command is in
>   `docs/watershed-system-plan.md` §10.
>
> How the system works: `docs/watershed-system-plan.md`. What is wrong with it:
> `docs/water-deep-dive-brief-2026-08-05.md`.

Every water number in this project came from one four-tile corridor. This
records a second region chosen for the opposite reason — the most runoff in the
world rather than merely enough of it — and what changed.

Seed 20260719, provider `terrain-diffusion-unlabeled-80b9ca451a23eae4`,
`TERRAIN_VERSION` 8, `BAKE_VERSION` 13, no `--diagnostic`.

## The region, and how it was picked

`tools/survey_world_water.py` (added with this note) ranks all 289 coarse tiles
on Budyko runoff through the bake's own chain and on trunk discharge from a
whole-world filled MFD accumulation. Selected:

**x −5..−3, y −5..−4** — `(-5,-4) (-4,-4) (-3,-4) (-5,-5) (-4,-5) (-3,-5)`

100% land, relief 4,906 m, drainage containment 0.85, eight tiles clear of the
corridor. Alpine heads near −4.4 °C at 2,494 m mean elevation feeding a valley
at 20.2 °C and 731 m — the cold-head / warm-valley pairing the runoff argument
predicts should carry the most water in the world.

## Runoff: the thing that had never been varied

Per-tile mean Budyko runoff, mm/yr:

| | corridor (bv13 cache) | wet block |
|---|---|---|
| tiles | −11,−4 / −11,−5 / −12,−5 / −11,−6 | −5..−3 × −5..−4 |
| runoff | 149 · 209 · 238 · 292 | 445 · 608 · 630 · 676 · 886 · **1262** |
| area mean | ~222 | **751** |

World distribution over 289 tiles: p25 3, p50 13, p75 103, p90 364, p99 1238,
max 1515 mm/yr. The corridor sits near p80; the block spans p88–p99.

**The world is not short of water — it is short of water we had looked at.**

Why cold wins, in the three tiles that show it cleanly:

| tile | precip | temp | PET | runoff | rain → river |
|---|---|---|---|---|---|
| (−4,−4) alpine | 1,449 | −4.4 °C | 189 | 1,262 | **87%** |
| (−15,−16) tropical | 2,724 | 22.0 °C | 1,391 | 1,515 | 56% |
| (−14,−6) arid | 306 | 25.1 °C | 1,729 | 8 | 2.5% |

The tropical tile gets nearly twice the rain of the alpine one and yields only
20% more river. Runoff is rainfall minus evaporative demand.

## What the bake produced

6 tiles, 2,313 CPU-s. Cache root `D:/vox-wet-cache`, namespace
`terrain-diffusion-unlabeled-80b9ca451a23eae4-ba9c62170`.

The namespace name is the same string the corridor's bv13 tiles carry, because
it is content-addressed off the bake fingerprint and nothing about the bake
changed. Isolation is by **cache root**, not by forging an identity: a separate
root leaves every live baseline (`-ba9c62170`, `-b52995abb`, `-b4d02b092`)
byte-untouched, including the flow superblocks the corridor shares.

Terrain identity: `tools/verify_water_only_change.py` PASS on all six —
`elevation_cp` IDENTICAL through the codec's own operator, flow plane
IDENTICAL, bv13, quant 100 mm.

Client load: `vxc_riverribbonprobe` loaded 6/6 with a water plane, 0 refused,
0 unresolved blocks, 0 disagreements between the far-field fill and
`RiverSampler::surfaceAtPixel`.

## The contrast

| | arid corridor bv13 | wet block bv13 | ratio |
|---|---|---|---|
| runoff mm/yr | 149–292 (mean 222) | 445–1262 (mean 751) | **3.4×** |
| trunk Q over the wet mask | 2.68e7 m³/yr (0.85 m³/s) | 1.68e8 m³/yr (5.32 m³/s) | **6.3×** |
| width from the law, p50/p90/max | 1.50 / 1.50 / 8.81 m | 1.50 / 3.83 / 18.28 m | max **2.07×** |
| depth p50 / p90 / max | 0.605 / 0.956 / 1.073 m | 0.599 / 1.260 / 2.052 m | max **1.91×** |
| wet cells, inland tiles only | 245,010/tile (0.365%) | 358,644/tile (0.535%) | **1.46×** |
| longest composed reach | 21,370 m span | 32,883 m span | **1.54×** |

Wet-cell counts are quoted inland-only on purpose. The corridor's raw total is
2,891,487 cells, but 2,156,457 of them (74.6%) are the coastal tile (−11,−6),
where river water paints the seafloor and the lateral fill amplifies it. The
wet block is 100% land, so its numbers need no such subtraction.

## Do width and depth rise with Q? Yes, exactly, and that is the problem

Observed depth against `water_depth_m(Q)`, by Q decade, over the wet block:

| Q m³/yr | n | law | observed p50 |
|---|---|---|---|
| 1e6–1e7 | 248,488 | 0.496 | **0.499** |
| 1e7–1e8 | 103,495 | 1.024 | **1.027** |
| 1e8–1e9 | 3,089 | 1.961 | **1.961** |

Nothing clamps. `CHANNEL_MAX_DEPTH_M` (25 m) needs Q = 9.18e10 m³/yr; the
largest Q observed anywhere here is 1.68e8, i.e. **0.18% of the cap**. The
400 m width cap needs 6.8e10 for even 200 m. Neither is remotely in reach.

The limit is the exponents. Width ∝ Q^0.398, depth ∝ Q^0.352, so 6.3× the
discharge buys 6.3^0.398 = 2.07× the width and 6.3^0.352 = 1.92× the depth —
which is precisely what was measured (2.07× and 1.91×). A **10× visibly bigger
river needs ~320× the discharge**, and this world's whole runoff range spans
about 6× at the trunk. No region of this world can deliver a dramatically
bigger river under these laws.

Below Q ≈ 3.16e5 m³/yr (`Q_PERENNIAL_M3_YR`) both laws floor, which is why the
p50 depth is identical (0.605 vs 0.599) in the two regions: the median wet cell
in both is a sub-perennial headwater at the floor, not a river.

## The long profile, head to mouth

Longest composed reach: span 32,883 m, thalweg 43,992 m, sinuosity 1.34,
**3,977.8 m → 121.3 m = 3,856 m of descent**, mean gradient 87.6 m/km. The
corridor's equivalent falls 864 m at 29.8 m/km and finishes below sea level.

| km | surface m | Q m³/yr | law width | drawn width | depth | m/km |
|---|---|---|---|---|---|---|
| 0 | 3,978 | 1.93e6 | 3.09 | 3.75 | 0.43 | 173 |
| 8 | 2,074 | 2.05e7 | 7.91 | 3.75 | 0.96 | 164 |
| 16 | 1,275 | 3.56e7 | 15.99 | 5.62 | 1.32 | 54 |
| 20 | 965 | 1.52e8 | 18.20 | 3.75 | 1.93 | 40 |
| 32 | 429 | 3.66e7 | 10.42 | 5.62 | 1.15 | 37 |
| 42 | 181 | 6.21e7 | 12.30 | 15.00 | 1.25 | 29 |

Q grows 32× head to mouth and the gradient decays 173 → 29 m/km: a properly
concave long profile.

**Two things to look at, not celebrate.**

1. *The drawn river does not track its own law.* Spearman(law width, distance
   along) = +0.728, but Spearman(**drawn** river width, distance) = +0.401 and
   Spearman(drawn width, Q) = +0.457. At km 20 the law asks for 18.2 m and the
   river plane holds 3.75 m — two pixels. The centreline median width over the
   whole reach is 3.75 m against a law median well above it. Whatever converts
   Q to painted cells is losing most of the growth the law specifies.

2. *42.3% of the longest reach is lake sheet, and the lakes are deep.* Lake
   depth p50 4.82 m, max 45.4 m, against a river-centreline max of 2.05 m. The
   deepest water on this "river" by a factor of 22 is standing water. In the
   corridor the same reach was 0.6% lake. Alpine wet country makes lakes; a
   flythrough here is partly a chain of lakes, and calling the 32.9 km figure a
   river would repeat a mistake this repo has already made once.

## Reproduce

```
python tools/survey_world_water.py accumulate \
    --coarse-dir <cache>/terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1 \
    --out D:/vox-wet-out/worldwater
python tools/survey_world_water.py blocks --dir D:/vox-wet-out/worldwater

python tools/bake_tiles_from_cache.py --seed 20260719 \
    --cache-dir D:/vox-wet-cache \
    --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4 \
    --tiles="-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5" --npz-dir D:/vox-wet-npz
```

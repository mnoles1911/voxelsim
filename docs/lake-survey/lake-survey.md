# Lake survey

`vxc.lakesurvey.v1` -- 12 tiles, seed 20260719, bake_version 7, fingerprint `440587d46334eab7`.

Registry filter: depth >= 2.0 m, area >= 2500 m2, spill above sea level (0 m), tile-spanning basins EXCLUDED.
Water balance: PET = 300 + 25T + 0.05T^3 (floor 100 mm/yr), Budyko n = 2, lake if >= 0.5 m deep, salt if P/PET < 0.35, seasonal if CV >= 55%.

## Per tile

| tile | province | basins | lake | seasonal | playa | spanning excl | submarine excl | near edge | comps | basin% | max depth m | max catch km2 | relief m |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| (-11,-3) | arid 95% | 140 | 72 | 1 | 67 | 192 (88 ha, max 19 m) | 47 | 2 | 59019 | 6.37% | 19.4 | 44.3 | 2076 |
| (-13,-6) | fluvial 42% | 39 | 38 | 0 | 1 | 69 (2358 ha, max 103 m) | 131 | 3 | 85656 | 19.44% | 103.4 | 66.5 | 1091 |
| (-15,-14) | arid 60% | 127 | 89 | 38 | 0 | 143 (150 ha, max 39 m) | 0 | 4 | 9402 | 7.06% | 259.0 | 98.0 | 1816 |
| (-2,-3) | fluvial 49% | 28 | 25 | 0 | 3 | 118 (41 ha, max 14 m) | 0 | 1 | 20271 | 2.73% | 17.1 | 124.6 | 5743 |
| (-2,-4) | fluvial 93% | 151 | 151 | 0 | 0 | 130 (66 ha, max 85 m) | 0 | 3 | 6237 | 2.06% | 86.1 | 82.9 | 4388 |
| (-2,-5) | fluvial 77% | 65 | 62 | 3 | 0 | 117 (1289 ha, max 85 m) | 0 | 7 | 13567 | 7.88% | 85.2 | 131.7 | 2081 |
| (-3,-3) | glacial 81% | 32 | 29 | 0 | 3 | 61 (20 ha, max 10 m) | 0 | 1 | 4259 | 1.47% | 31.4 | 296.1 | 4729 |
| (-5,-12) | arid 47% | 266 | 259 | 0 | 7 | 232 (376 ha, max 38 m) | 109 | 6 | 24715 | 9.47% | 47.5 | 132.4 | 2360 |
| (-6,-1) | arid 100% | 84 | 11 | 11 | 62 | 139 (120 ha, max 11 m) | 27 | 5 | 118736 | 23.01% | 7.9 | 493.7 | 516 |
| (-6,-5) | arid 68% | 65 | 60 | 5 | 0 | 110 (101 ha, max 24 m) | 0 | 2 | 14686 | 2.05% | 64.8 | 77.4 | 2821 |
| (-8,-14) | lowland 57% | 0 | 0 | 0 | 0 | 4 (1 ha, max 1 m) | 34 | 0 | 64084 | 7.62% | 68.1 | 131.7 | 674 |
| (-9,-9) | arid 100% | 0 | 0 | 0 | 0 | 7 (1 ha, max 2 m) | 94 | 0 | 10674 | 1.76% | 33.2 | 1495.5 | 2219 |

**Basins per tile: min 0, median 65, max 266, mean 83.1** over 12 tiles (997 basins total). The spread is the point: a per-tile budget sized on the median would be 4.1x short on the worst tile.

## Distributions over all registered basins

| quantity | min | p25 | median | p75 | p90 | max |
|---|---|---|---|---|---|---|
| depth to spill, m | 2.00 | 2.65 | 3.76 | 5.72 | 9.23 | 252.13 |
| water depth, m | 0.00 | 1.29 | 3.02 | 5.15 | 8.97 | 81.66 |
| area, ha | 0.25 | 0.39 | 0.59 | 1.16 | 2.27 | 864.64 |
| catchment, km2 | 0.00 | 0.07 | 0.41 | 3.98 | 21.84 | 293.46 |
| inflow, 1e3 m3/yr | 0.00 | 0.81 | 14.49 | 291.98 | 1968.33 | 55469.63 |
| precip, mm/yr | 0.00 | 141.18 | 517.65 | 1176.47 | 1788.24 | 2117.65 |
| PET, mm/yr | 100.00 | 751.85 | 1489.50 | 1775.00 | 1853.74 | 1853.74 |
| runoff, mm/yr | 0.00 | 3.58 | 48.05 | 222.55 | 737.02 | 1097.46 |

## Lake vs playa, by province

Province is the bake's own `province_fields` weight, argmax at the basin's deepest cell -- the real classifier, per `tools/worldmaps/README.md`.

| province | basins | dry_playa | salt_flat | seasonal | lake_terminal | lake_overflowing | lake frac | median P mm | median PET mm | median P/PET |
|---|---|---|---|---|---|---|---|---|---|---|
| arid | 387 | 0 | 133 | 31 | 88 | 135 | 58% | 141 | 1425 | 0.16 |
| fluvial | 353 | 4 | 3 | 0 | 23 | 323 | 98% | 1365 | 1591 | 1.21 |
| glacial | 106 | 0 | 0 | 26 | 13 | 67 | 75% | 141 | 292 | 0.46 |
| lowland | 151 | 3 | 0 | 1 | 42 | 105 | 97% | 1035 | 1814 | 0.58 |

## What the exclusions cost

**Tile-spanning (1322 components, 4609 ha, deepest 103 m).** Against 997 registered interior basins that is 57.0% of qualifying components. A basin crossing the tile edge is registered independently by each tile that sees it, from a different padded domain, and the two need not agree; its catchment also crosses the seam. v1 refuses them and counts them (§4.2.4). The v2 fix is HYDROLOGY_RESIDUALS #6's: a fill boundary condition from the superblock's shared `filled` raster.

**Submarine (442 components).** Depressions whose spill is at or below sea level are sea floor, not lake basins -- the ocean already covers them, and a lake surface there would be a second water plane under the first. This exclusion is NOT in the plan and was added after the survey found one: tile (-8,-14)'s largest basin sits at -433 m.

**Near the padded edge (34 of 997 kept basins, 3.4%).** A diagnostic, not an exclusion. NOTE: `pipeline.py`'s shipped `basin_reaches_padded_border` / `padded_border_basin_frac` stats are structurally ZERO on every tile ever baked -- `fill_depressions` never raises a border cell, so `filled - fine` is identically 0 there and no depression can contain one. This near-miss margin is the honest replacement.

## Threshold sensitivity

Every row is the same labelled components re-filtered -- no re-bake, no re-labelling.

| min depth m | min area m2 | basins | per tile median | lakes | playas | bytes/tile at 32 B |
|---|---|---|---|---|---|---|
| 1 | 0 | 4082 | 266 | 2828 | 972 | 26272 |
| 1 | 2500 | 1847 | 101 | 1130 | 550 | 13344 |
| 1 | 10000 | 396 | 18 | 258 | 101 | 3168 |
| 2 | 0 | 1538 | 98 | 1285 | 184 | 11200 |
| 2 | 2500 | 997 | 65 | 796 | 143 | 8512 |
| 2 | 10000 | 293 | 17 | 227 | 43 | 2656 |
| 3 | 0 | 862 | 46 | 770 | 63 | 5984 |
| 3 | 2500 | 656 | 33 | 569 | 59 | 5344 |
| 3 | 10000 | 241 | 12 | 204 | 21 | 2368 |
| 5 | 0 | 356 | 19 | 328 | 12 | 2592 |
| 5 | 2500 | 321 | 16 | 293 | 12 | 2176 |
| 5 | 10000 | 161 | 6 | 143 | 5 | 1440 |
| 10 | 0 | 85 | 4 | 84 | 0 | 736 |
| 10 | 2500 | 84 | 4 | 83 | 0 | 736 |
| 10 | 10000 | 66 | 3 | 65 | 0 | 512 |

## The climate the balance saw, and what it was fitted against

UNEP aridity bands over every registered basin. Earth's land surface is roughly 8 / 12 / 18 / 6 / 56% across these five.

| band | basins | share | lake frac |
|---|---|---|---|
| hyper-arid <0.05 | 66 | 6.6% | 0% |
| arid 0.05-0.20 | 182 | 18.3% | 38% |
| semi-arid 0.20-0.50 | 261 | 26.2% | 93% |
| dry sub-humid 0.50-0.65 | 156 | 15.6% | 99% |
| humid >=0.65 | 332 | 33.3% | 99% |

## Deepest interior lakes (the first-playable spawn sites, plan §11)

| tile | basin | kind | water depth m | area ha | catchment km2 | surface m | -VoxelSpawnAt |
|---|---|---|---|---|---|---|---|
| (-15,-14) | 188 | lake_overflowing | 81.7 | 60.2 | 57.36 | 153.7 | `-218668:-204133` |
| (-6,-5) | 188 | lake_overflowing | 63.5 | 38.5 | 49.49 | 484.9 | `-84512:-70226` |
| (-15,-14) | 224 | lake_overflowing | 57.6 | 62.9 | 67.42 | 103.6 | `-220312:-202492` |
| (-6,-5) | 270 | lake_overflowing | 53.4 | 15.9 | 3.20 | 2167.6 | `-86876:-66986` |
| (-15,-14) | 31 | lake_terminal | 52.7 | 864.6 | 21.88 | 920.8 | `-222377:-211571` |
| (-2,-4) | 24 | lake_overflowing | 41.1 | 9.3 | 0.33 | 1751.2 | `-26893:-61260` |
| (-2,-5) | 344 | lake_overflowing | 40.5 | 6.1 | 0.46 | 1549.9 | `-27816:-62539` |
| (-2,-4) | 329 | lake_overflowing | 36.5 | 46.5 | 1.70 | 2522.6 | `-24988:-51332` |
| (-2,-5) | 332 | lake_overflowing | 35.1 | 16.0 | 5.74 | 1488.3 | `-28804:-62863` |
| (-6,-5) | 254 | lake_overflowing | 34.2 | 2.7 | 0.92 | 1970.5 | `-90752:-67762` |


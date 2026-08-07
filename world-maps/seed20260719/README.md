# World maps — seed 20260719

Generated 2026-08-02 from the 289-tile radius-8 coarse world.

    provider   terrain-diffusion-unlabeled-71e2b362e3241e71
    seed       20260719  (0x000000000135276f)
    origin     tile (-3,-6), radius 8 -> 289 tiles
    extent     261 x 261 km, 50.0% land
    tiles at   D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-71e2b362e3241e71\000000000135276f\s1

Rebuild any of these with `terrain-service/tools/world_maps.py` in the voxelsim
repo. They are STANDING DELIVERABLES: every new seed's world gets this set, so
"is it flat", "where is it hot", "what erodes how" can be answered from an
image instead of from a ground-level screenshot that cannot show a landform.

---

## 01 — heightmap + hillshade

Hypsometric tint over a hillshade, 120 m/px, az 292.5 / alt 45, 3x vertical
exaggeration.

    elevation   -6,399 m  to  +6,125 m      (12.5 km vertical span)
    land        p50 443 m   p95 2,329 m
                27.75% above 1 km, 8.15% above 2 km,
                 1.50% above 3 km, 0.29% above 4 km

Against real Earth (+/-60 deg, ETOPO): HILLIER in the mid range (27.8% above
1 km vs Earth's 21.5%) but much LESS extreme at the top (0.3% above 4 km vs
Earth's 2.1%). Earth has Tibet and the Altiplano; this world has summits but
no high plateaus.

## 02 — biomes

Left panel is authoritative. Classified through `world_map.classify()`, which
parses `voxel-core/include/voxelcore/biome.h` at run time so it cannot drift
from the client.

    GRASSLAND 28.1%   TUNDRA_ALPINE 19.6%   SAVANNA 20.8%   DESERT  9.7%
    TAIGA      6.6%   BEACH          5.5%   RAINFOREST 4.7% TEMP_FOREST 4.9%

DESERT at 9.7% against Earth's ~8.6% is the closest match any channel achieved.
Savanna is high (Earth ~15.6%) and temperate forest low (Earth ~10-15%) --
both client-side and retunable against these tiles without regenerating.

**The province panel in this image is SUPERSEDED — use 03.** It was a
hand-rolled discriminant, not the bake's.

## 03 — landform provinces (authoritative)

Built by calling `bake/province.py:province_fields`, the same function
`bake/pipeline.py` calls, fed by the bake's own `assemble_padded_coarse` and
`assemble_padded_climate`.

    ARID 49.8%   FLUVIAL 26.1%   GLACIAL 17.6%   LOWLAND 6.6%

These are EROSION RULESETS, not biomes — they decide how the bake carves each
place. Validated two ways: it reproduces the bake's own printed
`province_*_frac` to 0.0000 on both baked tiles, and it labels the real Earth
reference corpus correctly on all 9 testable sites (Badlands -> ARID, Smokies
and Nepal -> FLUVIAL 0.99/1.00, Alps and Teton -> GLACIAL).

## 04 — temperature and snow

Left: mean annual temperature (the tiles' bio_1 channel as shipped).
Right: where a snow system matters.

    mean annual, land   p5 -7.4   p50 18.0   p95 26.8 C   (range -22.4..+32.2)

    PERMANENT  18.2% of land   mean annual below 0 C
    SEASONAL   22.1% of land   coldest month below 0 C -- snow system REQUIRED
    RARE        3.8%
    NONE       55.9%

**98.9% of land above 2,000 m is SEASONAL or PERMANENT.** Snow here is an
ALTITUDE phenomenon, not a latitude one, so it wants to be a continuous
function of altitude and season rather than a per-biome flag.

TWO CAVEATS. The coldest-month figure is DERIVED, not data: bio_4 is the SD of
monthly means and this assumes a roughly sinusoidal annual cycle, so coldest
month ~= mean - 1.4 sigma. Weather-grade, not climatology. And the temperature
channel is at COARSE-CELL resolution (7.68 km), so it reflects the cell's mean
elevation, not the terrain under the player -- this map systematically
UNDERSTATES how much of a tall mountain is snow-covered. Apply the lapse rate
at the player's real altitude at runtime; see
`docs/climate-biome-province-consumers.md`.

## 05 — coarse vs fine tier

Same spawn coordinate, same frozen 12:00 03-20 sun, same resolution. Left is
the 30 m coarse tile amplified by the client; right is the 1.875 m tile from
the full bake (depression fill, D8/MFD routing, stream-power incision,
relaxation, meso detail). Ground-level camera — these show material and colour,
NOT landform.

## 06 — fine-tier hillshade, 2 tiles

The baked surface itself, no client detail terms, no voxelisation, no LOD.
Desert (-3,-11) at 546.8 m relief and tundra (-4,-2) at 746.5 m, each 7.68 km
across at native 1.875 m/px. This is what the bake actually produced.

## 07 — vista site index

Every screenshot site pinned on the world hillshade, each with its
`-VoxelSpawnAt` and elevation printed in the legend. This is the map that turns
a coordinate into somewhere you can go: pick a pin, read the coordinate, launch
there and walk around.

    9 sites   all 8 biomes, all 4 provinces
    captured  1200 m above terrain, pitch -20 deg, sun frozen 12:00 03-20

The screenshots themselves are in `vistas/`, named
`vista-<biome>-<province>-t<tx>_<ty>.png` so both labels and the tile are
readable without opening anything. `vistas/README.md` carries the per-site
table (elevation, temperature, precipitation, what is in frame) and the recipe
for travelling there — **clear the edit log first or you land wherever you were
last**, which has already produced a capture named "plains" showing a mountain.

Every biome and province on this map is recomputed at the exact spawn column
through the real classifiers, not read off map 02 or 03. That check caught
three bad sites out of nine, including a "beach" that was a photograph of open
water and two sites on the world edge whose far field was the flat fallback
plane.

---

## Would fine tiles change map 01? No.

The bake's largest routine change is `incision_p99 = 25 m` against a map that
spans 12,524 m and is drawn at 120 m/px — 0.2% of the range, at a pixel 64x
coarser than the detail added. Fine tiles change LOCAL hillshades dramatically
(see 06) and the global map not at all. Baking all 289 tiles would cost roughly
21.7 hours of CPU and 58 GB.

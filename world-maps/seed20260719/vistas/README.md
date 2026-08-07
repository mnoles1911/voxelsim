# Vista screenshots — seed 20260719

Nine fine-tier vistas, one per biome and covering all four landform provinces,
each captured at **1200 m above the terrain, pitch −20°**, 2560×1440, sun frozen
at 12:00 on 03-20.

    provider   terrain-diffusion-unlabeled-71e2b362e3241e71
    seed       20260719  (0x000000000135276f)
    world      tiles tx -11..5, ty -14..2  ->  261 x 261 km

**`../07-vista-site-index.png` is the map of where these are.** Numbered pins on
the world hillshade, with the spawn coordinate printed under each one. It sits
with the other world maps because it is one of them — a standing deliverable
for every seed, not an accessory to this folder.

## Why 1200 m and −20°, and not a ground shot

Eight ground-level captures were reviewed on 2026-08-02 and the world was
reasonably called *flat*. It is not: it spans 12.5 km vertically and contains a
6,125 m massif. A camera standing on the terrain frames the slope it is standing
on, and no amount of it will show a landform. **A ground shot is evidence about
ground cover and material; it is not evidence about landform.** The altitude and
pitch were bracketed on the alpine tile — 600 m/−15° saw only the near slope,
2500 m/−30° was fog-dominated, 1200 m/−20° puts the horizon at about ⅔ frame
with relief still legible.

## Travelling there to walk around

Two things will silently put you somewhere else, so do both:

```powershell
# 1. THE EDIT LOG OVERRIDES -VoxelSpawnAt. A capture on 2026-07-29 launched at
#    the plains exemplar and came up at the ALPINE one, because the previous
#    run's position was still in Saved\VoxelWorlds. Clear it first.
Remove-Item D:\vox-int\ue-project\Saved\VoxelWorlds\*.vxlog -Force -ErrorAction SilentlyContinue

# 2. Launch interactive at the coordinate from the table below.
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
    D:\vox-int\ue-project\VoxelEarth.uproject `
    -game -sm6 -dx12 -windowed -ResX=1600 -ResY=900 `
    -VoxelSpawnAt=-38400,-161280
```

`-VoxelSpawnAt` is **metres**, world XY (`VoxelEarthGameMode.h:7` converts to
UU). Omit `-VoxelSpawnAltM`/`-VoxelSpawnPitch` and you spawn on the ground,
which is what you want for walking. **Give the terrain time to stream before
judging anything** — a blank or half-empty frame is unloaded terrain, not a
rendering bug.

To re-take a vista instead of walking it, use the capture script, which clears
the edit log itself and greps the streaming counters afterwards:

```powershell
D:\vox-int\tools\voxel-capture.ps1 -Name vista-desert-arid `
    -SpawnAt '-38400,-161280' -SpawnAltM 1200 -SpawnPitch -20 -SettleSec 150
```

## The nine sites

`elev`/`T`/`P` are at the spawn column. *In frame* is measured over a 6 km
radius — a stated, checkable number, not a render-accurate frustum.

| # | file | biome | province | tile | `-VoxelSpawnAt` | elev | T | P | in frame (r=6 km) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | `vista-desert-arid-t-3_-11.png` | DESERT | ARID 1.00 | (−3,−11) | `-38400,-161280` | 134 m | 25.6 °C | 94 mm | 80% land, DESERT 98%, relief 465 m |
| 2 | `vista-grassland-arid-t-1_1.png` | GRASSLAND | ARID 1.00 | (−1,1) | `-7680,23040` | 280 m | 19.9 °C | 282 mm | 97% land, GRASSLAND 100%, relief 1,202 m |
| 3 | `vista-temperate_forest-fluvial-t-8_-12.png` | TEMPERATE_FOREST | FLUVIAL 1.00 | (−8,−12) | `-115200,-176640` | 1,073 m | 8.0 °C | 1,976 mm | 100% land, TEMP_FOREST 100%, relief 1,623 m |
| 4 | `vista-savanna-fluvial-t2_-8.png` | SAVANNA | FLUVIAL 1.00 | (2,−8) | `38400,-115200` | 163 m | 26.8 °C | 1,035 mm | 100% land, SAVANNA 100%, relief 376 m |
| 5 | `vista-rainforest-lowland-t-7_-12.png` | RAINFOREST | LOWLAND 0.83 | (−7,−12) | `-100470,-171510` | 114 m | 24.3 °C | 1,741 mm | 100% land, RAINFOREST 87%, relief 900 m |
| 6 | `vista-beach-arid-t-4_-10.png` | BEACH | ARID 0.85 | (−4,−10) | `-61200,-153360` | 1 m | 26.5 °C | 376 mm | 52% land, BEACH 19% / SAVANNA 70%, relief 362 m |
| 7 | `vista-taiga-glacial-t-7_-2.png` | TAIGA | GLACIAL 0.81 | (−7,−2) | `-102390,-23670` | 127 m | −0.5 °C | 235 mm | 94% land, TAIGA 80%, relief 782 m |
| 8 | `vista-tundra_alpine-glacial-t-3_-3.png` | TUNDRA_ALPINE | GLACIAL 1.00 | (−3,−3) | `-38400,-38400` | 3,271 m | −6.1 °C | 94 mm | 100% land, TUNDRA 100%, **relief 3,582 m** |
| 9 | `vista-rainforest-fluvial-t-2_-4.png` | RAINFOREST | FLUVIAL 1.00 | (−2,−4) | `-23040,-53760` | 1,416 m | 21.8 °C | 1,788 mm | 100% land, RAINFOREST 63% / TEMP_FOREST 23% / TUNDRA 14%, **relief 3,379 m** |

Coverage: all 8 biomes, all 4 provinces. #9 duplicates RAINFOREST on purpose —
it is the FLUVIAL exemplar, and at 3,379 m of relief across three biomes it is
the best single picture of how fast this world changes.

## Both labels are recomputed, not assumed

Every biome and province in that table was re-derived at the exact spawn column
by `terrain-service/tools/worldmaps/vista_sites.py`, which calls the same code
the game and the bake call:

* **biome** — `world_map.classify()`, which parses
  `voxel-core/include/voxelcore/biome.h` at run time, so it cannot drift from
  the client.
* **province** — `bake/province.py:province_fields`, the exact function
  `bake/pipeline.py:2465` calls, on the same padded domain built by
  `assemble_padded_coarse` / `assemble_padded_climate`.

Never reimplement either. A hand-rolled province discriminant was tried on
2026-08-02 and produced plausible, wrong shapes over the whole world.

**This check paid for itself: three of the original nine sites were wrong**, and
nothing in the filenames or the pictures would have said so.

* The **beach** shot spawned at **−142 m with zero land inside 6 km** — a
  photograph of open water, filed as a beach.
* The **taiga** shot sat in the **leftmost tile column** and the **rainforest**
  shot in the **top-right tile corner**. A 1200 m camera sees far past its own
  tile, so both filled their far field with the flat fallback plane — which
  reads as a rendering bug and is not one.

All three were replaced using `worldmaps/find_site.py`, which scores a candidate
on the spawn column's own class, on what is actually in frame, and on distance
from the world edge. The replacements are better on every measure: the taiga
site went from 243 m to 782 m of relief, the rainforest site from 66% land to
100% and from 205 m to 900 m of relief.

## Caveats

* **The province at a point is a weight, not a label.** `ARID 0.85` means the
  bake blended 85% of the arid ruleset there. Sites 5, 6 and 7 are genuinely
  mixed; sites 1–4, 8 and 9 are essentially pure.
* **Provinces are erosion rulesets, not biomes.** They decide how the bake
  carved a place, not what grows there. See
  `docs/climate-biome-province-consumers.md` — biome drives vegetation, raw
  climate channels drive weather, province drives surface material.
* **Climate is at coarse-cell resolution** (7.68 km), so the temperature in the
  table is the cell's, not the terrain's under your feet. Apply the lapse rate
  at your real altitude at runtime.
* **The "in frame" figures are a 6 km disc**, which is narrower than what the
  camera actually sees. Treat them as a floor on how mixed the view is.

## Regenerating all of this

```powershell
$env:PYTHONPATH="D:/vox-int/terrain-service"
$PY="D:/terrain-diffusion/.venv/Scripts/python.exe"
$T="D:/voxelsim/tile-cache/terrain-diffusion-unlabeled-71e2b362e3241e71/000000000135276f/s1"

& $PY D:/vox-int/terrain-service/tools/worldmaps/vista_sites.py $T out/vista-sites.md
& $PY D:/vox-int/terrain-service/tools/worldmaps/vista_map.py   $T out/vista-sites.json 00-vista-site-index.png

# to pick sites for a NEW seed:
& $PY D:/vox-int/terrain-service/tools/worldmaps/find_site.py $T BEACH --min-edge-km 32
```

`matplotlib`/`scipy`/`rasterio` are not in the system python — use the
terrain-diffusion venv as shown. See `../../../vox-int/terrain-service/tools/worldmaps/README.md`.

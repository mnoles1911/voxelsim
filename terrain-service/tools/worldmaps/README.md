# World map generators

**Standing deliverable.** Every new seed's world gets this set built and handed
over. They exist because a ground-level screenshot cannot answer "is this world
flat or mountainous" — eight of them produced exactly that wrong conclusion on
2026-08-02 about a world spanning 12.5 km vertically with a 6,125 m massif, and
one stitched map settled it in a single image.

## Run them

`matplotlib`, `scipy` and `rasterio` are NOT in the system python (CI installs
only flask/numpy/pytest — see `bake/noise.py`'s header for why). Use the
terrain-diffusion venv:

```sh
TILES=D:/voxelsim/tile-cache/<provider>/<seed:016x>/s1
PY=D:/terrain-diffusion/.venv/Scripts/python.exe
export PYTHONPATH=D:/vox-int/terrain-service

$PY tools/worldmaps/heightmap.py    "$TILES" out/01-heightmap.png 4
$PY tools/worldmaps/biomes.py       "$TILES" out/02-biomes.png
$PY tools/worldmaps/provinces.py    "$TILES"            # writes 03-provinces
$PY tools/worldmaps/temperature.py  "$TILES" out/04-temperature-and-snow.png
$PY tools/worldmaps/vista_sites.py  "$TILES" out/vista-sites.md
$PY tools/worldmaps/vista_map.py    "$TILES" out/vista-sites.json out/07-vista-site-index.png
```

**Map 07 is a standing deliverable like 01-04**, added at the owner's request:
the hillshade with a numbered pin at every screenshot site and each pin's
`-VoxelSpawnAt` in the legend, which is what turns a coordinate into somewhere
a developer can go. **Regenerate it whenever a site moves** — a stale pin map
is worse than none. `vista_map.py` reads the JSON `vista_sites.py` writes, so
the pins cannot drift from the labels as long as you run them in that order.

Analysis, not maps:

```sh
$PY tools/worldmaps/vs_earth.py          "$TILES"   # hypsometry vs real ETOPO
$PY tools/worldmaps/province_vs_earth.py            # discriminant vs Earth corpus
```

## Rules that make these trustworthy

**Call the real classifiers. Never reimplement.**

* Biomes go through `world_map.classify()`, which parses
  `voxel-core/include/voxelcore/biome.h` at run time, so the map cannot drift
  from the client.
* Provinces go through `bake/province.py:province_fields` — the same function
  `bake/pipeline.py:2465` calls — fed by the bake's own
  `assemble_padded_coarse` and `assemble_padded_climate`.

A hand-rolled province discriminant was tried first and produced plausible,
wrong shapes. Validate a province map by reproducing the `province_*_frac`
values the bake printed for tiles it has baked; that check is exact (0.0000).

**Keep output under ~10 MB.** A 20.7 MB PNG failed to upload with a server 400.
Downsample (120 m/px is plenty for a 261 km world) or use JPEG.

**Two traps that cost an hour each on 2026-08-02:**

* *Check you are comparing the same tile.* An entire investigation "proved" the
  bake wrong because the tiles baked were `(-3,-3)` and `(-2,-4)` while the
  numbers compared came from `(-3,-11)` and `(-4,-2)`. Nothing was broken.
* *Match the sample to the claim.* Nine tiles said this seed peaks at 1,471 m;
  121 said 6,021 m; 289 say 6,143 m. A small-sample census also produced two
  confident-but-wrong causal explanations (tundra share, temperate-forest
  share) that the larger sample overturned.

## What each one answers

| script | question |
|---|---|
| `heightmap.py` | Is the world flat or mountainous? Where is the high ground? |
| `biomes.py` | What kind of place is here? Drives **vegetation**. |
| `provinces.py` | How was this carved? Drives **bake erosion rules** and **surface material**. |
| `temperature.py` | Where is it hot/cold, and where does a **snow system** matter? |
| `vs_earth.py` | Is our hypsometry more or less extreme than real Earth? |
| `province_vs_earth.py` | Does the province discriminant label real Earth correctly? |
| `vista_sites.py` | What biome and province is each screenshot site ACTUALLY at? |
| `vista_map.py` | Where are the screenshot sites? (pins on the world hillshade) |
| `find_site.py` | Where should I stand to photograph biome X? |

## Picking screenshot sites

Sites chosen off a biome map by eye are wrong often enough to be worth
checking. Of the first nine vista sites, **three were bad** and neither the
filenames nor the pictures said so: one "beach" spawned at −142 m with zero
land inside 6 km, and two sat on the world edge, where a 1200 m camera fills
its far field with the flat fallback plane. So:

```sh
$PY tools/worldmaps/find_site.py "$TILES" BEACH --min-edge-km 32   # pick
$PY tools/worldmaps/vista_sites.py "$TILES" out/vista-sites.md     # then VERIFY
$PY tools/worldmaps/vista_map.py "$TILES" out/vista-sites.json out/index.png
```

`vista_sites.py` re-derives both labels at the exact spawn column through the
real classifiers, and reports what is in frame separately from what is under
the camera — where those disagree, the filename is overclaiming. Audition
candidates without editing the production list via
`VISTA_SITES="a:-102390:-23670;b:47370:-169590"`.

Two things about vista sites that a biome map cannot show you: **a vista sees
far past its own tile** (so `--min-edge-km`), and **a coastline needs a land
fraction near half** to be legible at all.

See `docs/climate-biome-province-consumers.md` for which layer drives
vegetation, weather and materials — and which must NOT.

## Known rough edges

These were lifted from a working session rather than written as a polished
tool. `provinces.py` hardcodes its output path and re-derives the tile list;
`temperature.py`'s coldest-month estimate assumes a sinusoidal annual cycle
(`bio_1 - 1.4 x bio_4/100`), which is weather-grade, not climatology. They are
committed because reproducibility beats polish here, not because they are
finished.

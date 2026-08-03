# What drives vegetation, weather, and materials

Decided 2026-08-02, after the first world with real climate contrast made the
question concrete. Three layers now exist and they are easy to confuse, so
this records which consumer reads which — and, more importantly, which
consumer must NOT read which.

| layer | what it is | where it lives |
|---|---|---|
| **Climate channels** | 4 × uint8 planes at 30 m: `temperature`, `seasonality` (bio_4), `precipitation`, `precip_variability` (bio_15) | in every `.vxtl` tile, wire format |
| **Biome** | a *classification* of those channels + elevation + slope | computed at runtime by `classifyBiome` (`voxel-core/include/voxelcore/biome.h`) |
| **Province** | a *classification for erosion physics* — FLUVIAL / GLACIAL / ARID / LOWLAND | `terrain-service/terrain_service/bake/province.py`, consumed at BAKE time |

---

## Vegetation → BIOME, modulated

`classifyBiome` is a vegetation classification by construction. TEMPERATE_FOREST,
RAINFOREST, TAIGA and SAVANNA are plant-community names, derived from the
Whittaker axes (temperature × precipitation × seasonality) that actually decide
what grows. Use it as the primary species-palette selector.

**Biome alone will place trees badly.** Three modulators are required, and none
of them is the province map:

* **Slope.** `classifyBiome` has a cliff gate that yields BARE_ROCK, but canopy
  density should fall off with steepness well before that threshold.
* **Treeline.** Already in the classifier as `kBiomeTreelineBaseMm` plus
  `kBiomeTreelineMmPerDegC`. Respect it; do not re-derive it, or the two will
  drift the way the four climate calibrations once did.
* **Flow accumulation.** This is the one that gets forgotten, and it is what
  makes vegetation read as *placed by water* rather than sprinkled by biome.
  Riparian corridors — denser canopy, different species along channels — come
  from the BAKE's accumulation plane, not from any classification.

## Weather → THE RAW CLIMATE CHANNELS. Not the biome.

**Do not drive weather off the biome label.** The biome is a lossy bucketing of
exactly the variables weather needs. GRASSLAND alone spans roughly 400–800 mm/yr
across a wide temperature band; keying weather to the label gives every
grassland identical weather, which is precisely the monotony this project spent
2026-08-01 removing from the terrain. Read the continuous planes:

| effect | channel |
|---|---|
| rain frequency and intensity | `precipitation` |
| rain vs snow | `temperature`, **with the lapse rate applied at the player's actual altitude** |
| wet/dry season timing | `precip_variability` (bio_15) |
| temperature swing, hard freezes | `seasonality` (bio_4) |

The altitude point is not a detail. A single tile spans up to 4.5 km of relief
in this world, so rain at the valley floor and snow on the ridge is one tile.
Using the tile's mean temperature would erase that.

## Province → SURFACE MATERIAL, and otherwise bake-time only

Province answers "how was this carved", not "what grows" or "what falls from
the sky". Feeding it to weather would be a lossy round trip, because province
is itself *derived from* climate.

Its one strong runtime use is ground material, since erosion history is exactly
what decides what the surface is made of:

* ARID → sand, desert pavement, exposed rock
* GLACIAL → scree, till, bare rock
* FLUVIAL → soil, gravel bars
* LOWLAND → fine sediment

**Dust storms are the honest hybrid.** They need aridity (precipitation
channel), wind, *and* loose sediment — and sediment availability is a province
question. That is the one effect that legitimately reads both.

---

## Standing deliverable

Every new seed's world ships with a stitched global heightmap/hillshade plus a
biome overlay and a province overlay. Ground-level screenshots cannot answer
"is this world flat or mountainous" — eight of them led to exactly that wrong
conclusion about a world spanning 12.5 km vertically with a 6,125 m massif, and
one stitched map settled it.

Build the biome layer by calling `world_map.classify()`, which parses `biome.h`
at run time so it cannot drift from the client. Build the province layer by
calling `province.province_fields` — the same function `bake/pipeline.py` calls
— never a reimplementation.

**Validate a province map by reproducing the `province_*_frac` values the bake
already printed for tiles it has baked.** That check is exact: recomputing
`province_fields` on the tiles actually baked matches the bake to 0.0000 on
every province. Two cautions learned the hard way on 2026-08-02:

* **Check you are comparing the same tile.** A whole investigation was spent
  "proving" the bake wrong because the tiles baked were `(-3,-3)` and
  `(-2,-4)` while the numbers being compared came from `(-3,-11)` and
  `(-4,-2)`. Nothing was broken.
* **A cold desert is GLACIAL, and that is correct.** `province_cold_c` is
  −2.0 °C and temperature dominates aridity in the discriminant, so an arid
  tile at −3 °C classifies GLACIAL. Do not read that as a bug; whether it is
  the *desired* weighting is a separate, open question — see the Earth-corpus
  check in the backlog.

Keep the images under ~10 MB; a 20.7 MB PNG failed to upload. `matplotlib` is
not in the system python — use `D:\terrain-diffusion\.venv\Scripts\python.exe`
with `PYTHONPATH=D:\voxelsim\terrain-service`.

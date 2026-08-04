# World maps — provider `80b9ca451a23eae4`, seed 20260719

Generated 2026-08-03 from the 289-tile coarse world built on the RTX 4090 pod.

    provider_id          terrain-diffusion-unlabeled-80b9ca451a23eae4
    seed                 20260719  (0x000000000135276f)
    checkpoint bundle    xandergos/terrain-diffusion-30m
    checkpoint sha256    bc5cb9d75e3bbe39e8d38c71934fb4560558125ae4cc2803317a5c7a5313ae1d
    conditioning digest  c49e052e75da7a3cd8cb61742df9df86553c117d42e2f61730cf7a0ec64e48d2
    origin               tile (-8,-8), radius 8 -> 289 tiles, tx -16..0, ty -16..0
    extent               261 x 261 km
    generation cost      248 s for all 289 tiles (0.86 s/tile, warm)

## THIS IS A DIFFERENT PLANET FROM `71e2b362e3241e71`

Same seed, same code, **different world**. The old 289-tile world (the one
carrying the nine approved vistas) was generated from conditioning files that
no longer exist, and regenerating any of its tiles here produces different
terrain. See `docs/measurements/world-identity-not-reproducible-2026-08-03.txt`.

**The identity triple above is recorded precisely so this world does not suffer
the same fate.** Both conditioning files that produced it exist on this box.

## The maps

| file | what |
|---|---|
| `01-heightmap-hillshade.png` | hypsometric tint over hillshade, 120 m/px |
| `02-biomes.png` | biomes via `world_map.classify()` (left panel authoritative) |
| `03-provinces.png` | landform provinces via `bake/province.py:province_fields` |
| `04-temperature-and-snow.png` | mean annual temperature + where snow matters |
| `08-water-streams-rivers-lakes.png` | streams/rivers from the **coarse** world at the shipped drawable cut; lakes from the **fine** bake on 256 tiles — see below |

### 01 — relief

    elevation   -6,159 m to +6,312 m      (12.5 km span)
    land        p50 416 m   p95 2,440 m
                26.84% above 1 km   9.11% above 2 km
                 1.79% above 3 km   0.35% above 4 km   0.08% above 5 km

### 03 — provinces (authoritative)

    ARID 52.19%   FLUVIAL 25.79%   GLACIAL 15.65%   LOWLAND 6.37%

Note `02`'s right panel uses a hand-rolled discriminant and disagrees
(ARID 54.76 / FLUVIAL 27.81 / GLACIAL 14.56 / LOWLAND 2.87). **Use `03`** — it
calls the same function the bake calls.

### 04 — temperature and snow

    mean annual, land   p5 -8.0   p50 16.8   p95 26.5 C   (-24.0 .. +32.2)

    PERMANENT  18.40% of land      SEASONAL  24.82%
    RARE        4.69%              NONE      52.09%

**98.5% of land above 2,000 m is SEASONAL or PERMANENT** — snow here is an
altitude phenomenon, not a latitude one. Same caveats as before: the
coldest-month figure is derived (`bio_1 − 1.4 × bio_4/100`, weather-grade, not
climatology) and the temperature channel is at coarse-cell resolution, so this
UNDERSTATES mountain snow cover.

### 08 — water (regenerated 2026-08-04)

Made by `terrain-service/tools/worldmaps/water.py`, the same tool that made the
2026-08-03 version. **Its two halves come from different tiers, and that is the
first thing to know about it:**

    channels   THE 289 COARSE TILES, stitched at 30 m and swept ONCE, here.
               Not from fine tiles -- only 256 of 289 have ever been baked and
               far fewer are resident, so a fine-derived network could not
               cover this map. One domain, one sweep, every catchment whole.
    lakes      THE FINE BAKE at 1.875 m -- the shipped basin registry from
               tools/lake_survey.py dumps, bake_ver 8, on 256 of 289 tiles.
               The 19 unbaked land-bearing tiles are hatched red and carry no
               lakes. 14 more unbaked tiles are all ocean.

Every kernel and constant is the shipped one (`flow.fill_depressions`,
`flow.d8_receivers`, `flow.accumulate_d8`, `water.runoff_field_mm_yr`,
`water.discharge_source`, `water.q_drawable_m3_yr`, `water.water_head_mask`,
`pipeline.basin_filter`, `basins.classify`). Nothing about the water rule is
defined in the map tool.

**Threshold: `q_drawable` = 1,528,564 m³/yr = 0.0484 m³/s.** That is
`water.q_drawable_m3_yr(1.875 m, 1.5 px)` — the FINE tier's pixel pitch, not
this map's. Evaluating the same 1.5 px rule at the coarse 30 m pitch would ask
for a 45 m channel (1.6e9 m³/yr) and erase all but the trunk rivers; the map is
saying where the *game* puts water, and the game writes its plane on the fine
raster. Routing for the discharge is **D8** (`water_flow_single_receiver`);
`mfd_p` and the terrain's area field are untouched.

#### What changed against the 2026-08-03 version

The old map drew at `Q_perennial` (315,576 m³/yr) with an MFD discharge. Both
moved at bake_ver 11. Measured on this world, one factor at a time:

| routing | cut | water cells | channel km | km/km² |
|---|---|---|---|---|
| MFD | Q_perennial 315,576 | 770,016 | 26,304 | 0.879 |
| MFD | q_drawable 1.5 px | 276,472 | 9,453 | 0.316 |
| D8 | Q_perennial | 439,627 | 15,422 | 0.516 |
| **D8** | **q_drawable 1.5 px** | **196,460** | **6,899** | **0.231** |

Row 1 is the old map (its printed 26,304 km reproduces exactly, so nothing but
the cut and the routing changed). Row 4 is this one. The network is **74%
shorter** — but it is not a smaller claim about the world, it is a *truer* one:
the old map drew the whole perennial network, and the bake only ever wrote
water on the drawable part of it.

Broken out: the cut does most of it (770,016 → 276,472 cells, −64%) and D8 does
the rest (276,472 → 196,460, −29%). Note the two class totals move in opposite
directions — **stream 25,222 → 5,866 km** but **major river 42 → 83 km** — which
is the D8 signature: single-receiver routing stops splitting a reach's discharge
eight ways, so trunks carry more and cross the higher class boundaries.

The `water_min_width_px` 2.0 → 1.5 change works the other way and is visible
here too: at D8 the old 2.0 px cut would draw 126,973 cells, the new one draws
196,460 (**+55%**).

    total drawn water cells   196,460  (0.59% of land cells at 30 m)
    of which heads            1,215    (a wet cell with no wet donor)
    perennial (>= 10 L/s)     439,627  -> 243,167 cells, 55.3% of the
                              perennial network, are wet all year and still
                              under the cut. They are the GREY threads on the
                              map, not blue: §4.1's honesty clause, kept
                              visible rather than absorbed by the threshold.

    lakes (unchanged, same dumps)   21,942 registered, 20,038 hold water
        lake_overflowing 13,568  27,022 ha      salt_flat  1,789   27 ha
        lake_terminal     4,006   5,670 ha      dry_playa    115    4 ha
        seasonal          2,464      25 ha

**Carried discharge (the third bake_ver 11 change) does not reach this map, and
that is not an omission.** The pyramid's carried-Q fix exists to make the *fine*
tier agree with what a single sweep over the stitched coarse world already
computes — the coarse world is the reference that measured the shortfall
(1.27e6 against 58.7e6 m³/yr at the test corridor's coast). This map has one
domain and no injection cell, so it always had whole catchments.

## The two worlds are different planets but statistical siblings

| | old `71e2b362` | new `80b9ca45` |
|---|---|---|
| land p50 | 443 m | 416 m |
| land p95 | 2,329 m | 2,440 m |
| above 1 km | 27.75% | 26.84% |
| above 2 km | 8.15% | 9.11% |
| above 4 km | 0.29% | 0.35% |
| ARID | 49.8% | 52.2% |
| FLUVIAL | 26.1% | 25.8% |
| GLACIAL | 17.6% | 15.7% |
| LOWLAND | 6.6% | 6.4% |
| snow PERMANENT | 18.2% | 18.4% |
| snow SEASONAL | 22.1% | 24.8% |

Worth knowing, and reassuring: the conditioning drift changed **which** planet
was generated, not **what kind** of planet. Every distribution lands within a
couple of points. So the worldgen tuning work — the desert fix, the elevation
tails, the orographic coupling — all transferred intact.

## Two caveats on how these were made

**No vista set yet.** This world has **zero baked fine tiles**, so there is no
`vistas/` folder and no map `07`. The fine tier is a hard gate: a camera over
unbaked ground refuses rather than falling back. Baking is ~30 min/tile.

*Amended 2026-08-04:* still no resident fine tiles, but 256 of the 289 tiles
have now been through a real fine bake for `lake_survey.py dump` (bake_ver 8),
which is where map `08`'s lake layer comes from. The dumps are a registry plus
a reduced raster, not a fine tile — a camera still has nothing to stand on.

**`provinces.py` printed a false MATCH.** Its validation compares against two
tiles baked in the OLD world. Running it here it reported
`WORST ABSOLUTE DIFFERENCE: 0.9188` — correctly, they are different planets —
and then printed `MATCH (within 0.005)` anyway, because that line is
unconditional. The MAP is trustworthy (it calls the real
`province_fields`); the validation BANNER is not. The script needs an output
path argument and a validation that can be scoped to the world being mapped.

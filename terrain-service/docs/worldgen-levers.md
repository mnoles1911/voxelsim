# World-generation levers: what actually changes the terrain

Every input to `terrain-diffusion` that changes generated tile bytes, what it
does, what it costs, and which ones are already known to lie to you.

Written 2026-07-25 from the upstream source at `/d/terrain-diffusion` plus
measurements taken against seed 20260719. Facts marked **measured** were run;
everything else is read out of the code and cited by file and line.

> **You do not need a GPU to use any of this.** The whole pipeline runs on CPU
> torch. A 768 km coarse view takes ~35 s, a 15 km window of real 30 m terrain
> ~2.5 min. The rented GPU buys *throughput* (22.5 s/tile for a pregen), not
> capability. Every finding in this document was produced locally.

---

## 1. The pipeline, and where inputs enter

```
   ┌─ SKETCH  (5 channels, one value per 7.68 km cell) ──────────┐
   │  elevation | temperature | temp std | precip | precip CV    │
   │                                                             │
   │  DEFAULT: synthetic_map.py -- Perlin FBm wearing Earth's     │
   │  histogram (see §2). Knobs: frequency_mult, drop_water_pct   │
   │  ALTERNATIVE: your own GeoTIFFs (see §4)                    │
   └───────────────────────────┬─────────────────────────────────┘
                               │  world_pipeline.py:899  <-- THE injection point
   ┌─ COARSE DIFFUSION MODEL ──┴─────────────────────────────────┐
   │  denoises into a 7-channel coarse map, conditioned on the   │
   │  sketch. cond_snr = how tightly it must obey per channel.   │
   └───────────────────────────┬─────────────────────────────────┘
   ┌─ LATENT -> DECODER ───────┴─────────────────────────────────┐
   │  256x upsample -> 30 m/px elevation + climate -> .vxtl      │
   └─────────────────────────────────────────────────────────────┘

   InfiniteDiffusion / infinite-tensor wraps all three stages as lazy
   unbounded tensors over a tile store. That is what makes generation
   infinite and O(1) random-access. It is ORTHOGONAL to everything below --
   none of these levers touch it.
```

The injection point is one branch, `world_pipeline.py:899`:

```python
if not self._has_custom_conditioning_imports():
    return self.synthetic_map_factory(cj0, ci0, cj1, ci1)      # Perlin sketch
cond = self._raw_conditioning_with_imports(ci0, ci1, cj0, cj1).copy()   # your rasters
```

Both branches return `(5, H, W)` and feed an identical downstream path. That is
the entire conditioning API.

**"Conditioning" means nothing more than: the picture the denoiser sees
alongside the noise.** Changing it changes the world without touching a single
weight.

---

## 2. Why the default world looks the way it does

`synthetic_map.py` builds the sketch by **quantile matching**:

1. Read real Earth rasters — ETOPO elevation, WorldClim bio_1/4/12/15 — cropped
   to +/-60 degrees latitude (`_compute_map_stats`).
2. Build a 64-quantile table of Earth's distribution for each channel.
3. Build a 64-quantile table of Perlin FBm's distribution.
4. At sample time map each noise value to its quantile, then to the Earth value
   at that quantile (`transform_perlin`).

So the sketch is **Perlin noise wearing Earth's histogram**. Elevations are
Earth-like in *proportion* — correct amounts of abyss, shelf, lowland, alpine —
while the *shapes* are pure Perlin, which is isotropic and blobby.

**Consequence, measured:** every seed is an archipelago. Seeds 20260719, 7,
424242 and 1337 over a 492 km window gave inland reach (farthest land from open
water) of 77 / 69 / 92 / 77 km against a 246 km measurement ceiling. Land
fraction and temperature vary between seeds; topology does not. Earth's
continents come from plate tectonics — long linear features, coherent cratons —
and a histogram cannot encode that.

### Climate coupling to terrain — updated 2026-08-05

From `finalize_synthetic_map`:

* **Temperature is coupled to elevation** — a real lapse rate,
  `temp += lapse * max(0, elev)`, so mountains are cold.
* **Precipitation is coupled to elevation too, since 2026-08-01** — an
  orographic rain-shadow pass. It is **rain shadow only**; there is still **no
  continentality**, so distance from the ocean does nothing on its own.

**The rain-shadow pass, and where it lives.** The code is a patch this repo owns
against upstream `synthetic_map.py`:
`terrain-service/patches/terrain-diffusion-worldgen.patch`, applied by
`bootstrap_pod.sh`, pinned to upstream `82a0431`. Its parameters are
`WorldShapeConfig` fields (`providers/diffusion.py:549-566`):
`orographic_enabled=True`, `oro_wind_from_deg=270.0`,
`oro_probe_wavelengths=(0.15, 0.30, 0.60, 1.20)` (fractions of the elevation
base wavelength), `oro_barrier_m=1200`, `oro_upslope_m=600`,
`oro_shadow_strength=0.75`, `oro_enhance_strength=0.60`, `oro_sea_blend_m=200`.

For each land cell it looks upwind at four probe distances, takes the largest
elevation the wind had to climb (`barrier`), and applies
`(1 + 0.60·enhance) · (1 − 0.75·shadow)` — ramped in over the first 200 m inland
so coasts do not step. **The wind is a single fixed global bearing**: no
latitude bands, no Hadley cell, no seasons.

**Measured:** correlation between upwind barrier and the rainfall multiplier is
**−0.734**. Mean multiplier is **0.493 behind a barrier over 600 m** against
**1.393** with no barrier. Land under 400 mm/yr went 34.7% → 40.4%. Seam-free by
construction (the elevation noise is re-sampled at absolute world coordinates);
`tools/orographic_check.py` measures max|diff| = 0.0.

**Do not claim which compass direction the dry side faces.** Two coordinate
swaps sit between this pass and the rendered world. The patch claims only that
the effect is *consistent*, and so should you.

> **What this section used to say, and why it was true then.** It said
> precipitation was an independent Perlin field, that there was no orographic
> rainfall at all, and that a 315,000 km² landmass generated at
> `frequency_mult[0]=0.4` still classified **DESERT 0.0%**. That measurement was
> correct at the time and it is what motivated the fix. It was compounded by a
> second problem: `synthetic_map_stats.json` had been built from **hand-written
> latitude formulas** substituted when WorldClim was unreachable, so
> precipitation spanned only **7.8%** of its encodable range over land. Both are
> fixed. The shipped world now measures **DESERT 9.74%, RAINFOREST 4.73%** over
> 289 tiles with all eight mappable biomes non-zero.
>
> One correction to keep, because it caught people out: **the stats rebuild
> alone did not produce deserts.** After both halves of that work the coarse
> census still read DESERT 0.00%. What made deserts exist was a later monotone
> remap of the *model's output* in `adapt_raster_to_tile` (`3b511e3`,
> re-fit in `56257c8`). Fixing the input distribution is not the same as fixing
> the output distribution.

---

## 3. The dials

### Change the world (must roll `provider_id` — see §5)

| dial | default | what it does | evidence |
|---|---|---|---|
| `seed` | — | Picks one realization of a fixed process. **Cannot change the world's character** — statistics come from the quantile tables and `frequency_mult`, neither of which the seed touches. | measured, §2 |
| `frequency_mult` | `[1.5, 3, 3, 3, 3]` | Per-channel Perlin frequency (multiplies a 0.05 base). Index 0 is elevation — **the landmass-scale knob**. Lower = bigger landmasses. | **measured**: 1.5→0.4 took inland reach 123→192 km and largest landmass 196,588→315,202 km² over a 998 km window, with land fraction essentially unchanged (39.9→40.5%). |
| `drop_water_pct` | `0.5` | Land/ocean ratio. Randomly drops that fraction of ocean pixels from the elevation histogram before quantiles are built, shifting the mapping toward land. | ⚠️ **cache-blocked — see §6** |
| `cond_snr` | `[0.3, 0.1, 1.0, 0.1, 1.0]` | Per channel, how tightly the coarse model must obey the sketch. **LOWER IS TIGHTER** — the name reads like a signal-to-noise ratio, but it is the *tangent of a mixing angle*: `t = atan(snr)`, then `cond = cos(t)·sketch + sin(t)·noise` (`world_pipeline.py:985,942`). `snr → 0` is pure sketch; `snr = 1.0` is a 45° equal mix. So elevation at 0.3 (16.7°, ≈96% sketch) is fairly **tight**, `temp`/`precip` at 0.1 (5.7°) are tighter still, and `temp_std`/`precip_cv` at 1.0 are the loose ones. **LOWER it if the sketch is being ignored.** | read (`world_pipeline.py:942,985`) |
| `coarse_pooling` | `1` | Pools the coarse output, compressing horizontal space. Upstream's README calls this its best lever for "more intense terrain without breaking realism". Must divide 64 and 48. | read |
| `elev_coarse_pool_mode` / `p5_coarse_pool_mode` | `avg` | `max` on elevation + `min` on p5 is the README's documented "more extreme, less realistic" combination. | read |
| custom conditioning TIFFs | — | Replaces the sketch outright. §4. | read |

### Do not change the world (performance only)

`latents_batch_size`, `dtype`, `torch_compile`, `caching_strategy`,
`cache_limit`, `decoder_tile_size`, `decoder_tile_stride`, `log_mode`.

Safe to tune for speed without touching identity.

### Handle with care

`native_resolution` (defaults to 90.0; the 30 m checkpoint overrides it),
`coarse_means` / `coarse_stds`, `residual_mean` / `residual_std`,
`latent_compression`. These are normalization constants matched to the trained
weights. Changing them is not a design lever, it is a way to break the model.

---

## 4. Custom conditioning (the full-control route)

`tiff_export.py` takes a directory of GeoTIFFs, one per channel:

```
heightmap.tif          elevation, metres
temperature.tif        degrees C
temperature_std.tif    degrees C   (scaled x100 internally)
precipitation.tif      mm/yr
precipitation_cv.tif   percent
```

Missing files fall back to Perlin; at least one is required. Each input cell
becomes `PIXELS_PER_CELL = 256` output pixels, with 64 cells of edge padding for
context (stripped from the output). `--snr` sets per-channel refinement
strength, same role as `cond_snr` above.

`azgaar-to-tiff` converts an [Azgaar Fantasy Map Generator](https://azgaar.github.io/Fantasy-Map-Generator/)
JSON export into exactly this layout — the practical path for "draw the
continents you want, let diffusion make them real". `--scale` sets input cell
size in km; the most faithful value is the coarse cell size, 7.7 km for the
30 m model.

**This is the only route that gives deserts**, because it is the only one where
you control the precipitation field independently of the terrain (§2).

---

## 5. The identity gap (action required before adopting any of this)

`DiffusionConfig.provider_id()` hashes checkpoint content, conditioning-raster
content, `SamplerConfig`, scale, channel mapping and the tile wire format. It
does **not** know that `frequency_mult`, `drop_water_pct`, `cond_snr`,
`coarse_pooling` or the pool modes exist.

So today, changing any of them **changes tile bytes under an unchanged
`provider_id`** — the exact failure `provider_id` exists to prevent. Two tile
sets generated with different landmass scales would share a cache namespace and
stamp identically into edit logs, and `EditLog::checkProvider()` would report
`kMatch` on worlds that are not the same world.

This is the same class of gap already documented on `SamplerConfig` (hashed but
never actually passed to `WorldPipeline`). **Fix both together**: pass the
pipeline kwargs through from a config field, and hash that field. Until then,
treat every dial in §3 as unusable in production.

---

## 6. Known trap: `drop_water_pct` silently does nothing

`make_synthetic_map_factory` does:

```python
stats = _load_stats_cache()          # data/global/synthetic_map_stats.json
if stats is None:
    stats = _compute_map_stats(frequency_mult, drop_water_pct)
```

The cache is loaded **unconditionally and is not keyed on the parameters**. Once
that file exists, the quantile tables are frozen — so `drop_water_pct`, whose
only effect is on those tables, has no effect at all.

`frequency_mult` escapes this because it *also* flows straight into the noise
configuration (`map_configs`), which is not cached. That asymmetry is precisely
why the landmass-scale experiment worked and why a land-ratio experiment would
have quietly reported "no change".

**To use `drop_water_pct`, delete `data/global/synthetic_map_stats.json` first**
— and note that recomputing it needs the WorldClim bio rasters present, not just
`etopo_10m.tif`.

---

## 7. How to test a lever cheaply

The dials are `WorldPipeline` constructor kwargs, so they need a fresh explorer
process (unlike `seed`, which has a live `POST /api/seed`).

```bash
# 1. run the explorer with the lever set (CPU is fine)
cd <terrain-diffusion>
python -c "from terrain_diffusion.inference.explorer.server import main; \
  main(['--port','8900','--seed','20260719','--device','cpu', \
        '--kwarg','frequency_mult=[0.4,3,3,3,3]'], standalone_mode=False)"

# 2. look at what it did
cd <voxelsim>/terrain-service
python tools/world_map.py --url http://127.0.0.1:8900 --window=-58,72,-76,54
```

`--kwarg` parses values with `json.loads`, so lists and floats both work.

Run two explorers on different ports to A/B a lever against the default without
losing either one's cache. `tools/world_map.py --audition` does the same trick
for seeds, and reports **inland reach** — the farthest any land sits from open
water, which is the number that distinguishes an archipelago from a continent
and therefore predicts whether a world can be arid at all. Land *fraction*
cannot tell them apart.

---

## 8. Levers that turned out not to exist

Recorded so nobody re-runs them.

* **Sea level.** Not a generation input at all — z=0 is inherited from ETOPO's
  datum and is hardcoded downstream (tile format, `biome.h`'s coastal band,
  `caves.h`'s implicit ocean, the water system). **Measured:** rendering at
  lowered waterlines leaves inland reach at exactly 123 km from 0 m through
  −500 m; lowering the sea widens the coastal apron without deepening any
  interior, because the new coastline runs parallel to the old one. It also
  destroys beaches (2.3% → 0.2% at −100 m) as the waterline moves onto the
  steeper continental slope. If ever wanted for art reasons, the cheap
  implementation is an elevation bias in `adapt_raster_to_tile`, which rolls
  `provider_id` honestly and leaves every downstream rule untouched.
* **Seed selection, for continents or deserts.** §2. A seed picks a realization,
  not a process.

---

## Appendix: settled by measurement, 2026-07-25

* Tile axis mapping in `TerrainDiffusionBackend.generate_rasters`
  (`i1 = y*TILE_SIZE, j1 = x*TILE_SIZE`) was an open `# ASSUMPTION:` since
  bring-up, to be "verified at GPU bring-up". It needed no GPU: each cached
  tile's mean elevation must correlate with the coarse cells at its own
  footprint under the correct orientation only. Over the 25 tiles of seed
  20260719, `ci<-y, cj<-x` gives r = **+0.999** against **−0.795** transposed.
  Confirmed. Re-runnable as `tools/world_map.py --verify-axes`.
* One coarse cell = `PIXELS_PER_CELL` 256 × 30 m = **7.68 km**, so a 512 px tile
  is exactly 2 coarse cells across.

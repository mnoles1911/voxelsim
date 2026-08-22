# The land pipeline, end to end

**What this is:** the single current description of how ground gets made — from
the first blob of noise to the 10 cm voxel a player stands on. Stages, who owns
each, the three version numbers, the file map, the constants, what is measured,
and what is still broken.

**Consolidated 2026-08-05.** This absorbs and supersedes the scattered dated
notes listed in §12. Where an older note and this document disagree, this
document is right and the older note carries a banner saying so. Water is
deliberately thin here — it has its own documents (§12) and its own owner.

**Pinned state — version row refreshed 2026-08-19; the prose below it is still
the 2026-08-05 consolidation:**

| number | value | what it covers |
|---|---|---|
| `TERRAIN_VERSION` | **8** | the ground itself — unchanged since consolidation, so every site coordinate here still resolves |
| `BAKE_VERSION` | **28** (was 14) | everything the bake emits that is not ground |
| `kWorldGenVersion` | **28** (was 23) | the client amplifier's maths |
| shipped world | seed `20260719`, 17×17 coarse tiles | the world every measurement below is from unless it says otherwise |
| world source | `D:/voxelsim/tile-cache` | **the single cache root.** Coarse (`s1`), fine (`s16`) and flow superblocks all live under it -- consolidated 2026-08-21 |

**What moved since:** `BAKE_VERSION` 15–28 is water (lake depth and signed shore
distance, ponds, headwaters) and then **placement** — bake 28 adds five per-tile
placement planes. `kWorldGenVersion` 24–28 is the asset era: the asset term
became worldgen input (v24), manifest and bank bytes were called in as worldgen
input (v25), placement started reading the ground's own channels (v26), the biome
rebalance moved the digests (v27), and v28 is per-biome placement. **Only v27
moves the terrain digest** — v24–v26 and v28 are asset-side and leave terrain-only
bit-identical (e02458de2be47309 through v26, ad9c4c2a100b5a28 after v27), which is
why each of those bumps ships a composed-ASSET digest as its ran-flag instead.
`core.h`'s changelog is authoritative and now runs to v28.

---

## 1. The five stages, and what each one owns

Terrain is built in five passes. Each pass hands the next one a coarser-to-finer
version of the same ground; none of them re-decides what an earlier pass settled.

| # | stage | where it runs | resolution | what it decides | cost |
|---|---|---|---|---|---|
| 1 | **Sketch** (conditioning) | CPU, before the model | one value per **7.68 km** cell, 5 channels | roughly where land, mountains, heat and rain go | seconds |
| 2 | **Coarse diffusion** | GPU (or slow CPU) | **30 m/px**, 512² tiles | the actual continents, and 4 climate planes | ~22.5 s/tile on the pod |
| 3 | **Flow superblock** | CPU | 30 m, a 2048² raster per level | how much water crosses each tile's edge | ~1/20th of one fine bake |
| 4 | **Fine bake** | CPU, server (or client offline) | **1.875 m/px**, 8192² tiles | erosion, valleys, channels, per-cell physics | ~130–165 CPU-s, ~5 GiB peak |
| 5 | **Client amplifier** | client CPU **and** GPU, bit-identical | **10 cm voxels** | everything below 1.875 m | per-frame, streaming |

**The 5 channels the sketch and the coarse model carry** are elevation,
temperature (`bio_1`), temperature seasonality (`bio_4`), precipitation
(`bio_12`) and precipitation variability (`bio_15`). All four climate channels
survive into the shipped tile as `uint8` planes at 30 m — the client reads them
directly.

**Nothing stores voxels.** The fine tile is a heightfield of B-spline control
points. The client's amplifier turns it into voxels on the fly, and the CPU and
GPU halves must agree bit for bit (`amplifier.cpp` ↔ `worldgen.ush`, guarded by
`kWorldGenVersion`).

### Why stage 3 exists at all

**A tile cannot compute its own terrain.** The process that carves every valley
— stream-power incision — scales with *discharge*, and discharge is the total
catchment upstream of a cell. A river arriving at a tile edge may drain hundreds
of kilometres that lie outside the tile. Bake that tile alone and the river gets
zero upstream area, so it gets no incision.

**How much damage that actually does was measured, and it is almost none.** An
earlier version of this document argued *"you do not get wrong water, you get
wrong mountains"* — valleys fading out at tile boundaries. Baking a tile with
and without its injected inflow moved **zero elevation cells** past the 100 mm
wire quantum and changed **5 of 67 M flow cells**.

So the superblock earns its place on **determinism, not on looks**. Those 5
cells are 5 cells where two players who baked the same coordinates against
different neighbour sets will permanently disagree about the ground. In
multiplayer that is a correctness bug, not a quality one. Keep the reasoning
above, because it is why you would *expect* large damage — but the measurement
is what happens, and where they conflict the measurement wins.

**The pyramid is cheap because the raster edge never grows.** `superblock_tiles=4`,
so every level is 4 × 512 = **2048 px**, while each level covers 4× more ground
per axis:

    L0   4x4  =   16 coarse tiles     61 km span    2048^2 raster
    L1  16x16 =  256 coarse tiles    246 km span    2048^2 raster
    L2  64x64 = 4096 coarse tiles    983 km span    2048^2 raster

Each level is one priority-flood over ~4 M cells. A fine bake is 9216² =
**85 M cells** — so a whole pyramid level costs about **1/20th of one fine
tile** and buys correct discharge for up to 4096 tiles. Routing flow globally at
fine resolution is impossible in an infinite world; this is what makes
catchment-correct terrain tractable at all.

---

## 2. The four units, and the sizes people keep getting wrong

| unit | size | what it is | on disk |
|---|---|---|---|
| **coarse tile** | 15.36 km, 512² @ 30 m | diffusion output: elevation + 4 climate planes | 1.5 MB |
| **flow superblock** | 61 / 246 / 983 km | hydrology context, 2048² at every level | ~16 MB, one priority-flood |
| **fine tile** | 15.36 km, 8192² @ **1.875 m** | the baked heightfield players stand on | **201 MB raw, 33 MB zstd** |
| **chunk** | client-side | voxels, made from the fine tile at runtime | never stored |

**Where 15.36 km comes from:** 8192 px × 1875 mm = 15,360,000 mm. That is
exactly the footprint of one 512 px coarse tile at 30 m/px, which is why the two
tiers line up with no remainder. There is no named `15.36 km` constant anywhere
— it is always derived, and `size` is a header field rather than a compile-time
constant so that small conformance fixtures can be committed to git.

**A fine tile is 236 km².** That number is why "190 MB per tile" is not the
streaming problem it sounds like: a player can see a few kilometres, not 236
km². At ~0.14 MB/km² compressed, a 2 km view radius is **~1.7 MB** of terrain.

**Compression is measured, not modelled: 6.0×.** On tile (−5, 2),
`pregen._encode_fine` recorded **201.4 MB raw against 33.4 MB `CODEC_ZSTD`**,
with the elevation and flow planes bit-identical on round trip. So:

    per fine tile        ~33 MB compressed  (not 190)
    per km^2             ~0.14 MB
    2 km view radius     ~1.7 MB
    289-tile world       ~9.7 GB            (not 58)

**THOSE FIGURES ARE NOW STALE, and the gap is 12x.** Measured 2026-08-21 over the
15 resident `bake_ver` 28 tiles in `D:/voxelsim/tile-cache`, all written with the
default `--codec zstd`: **mean 414 MB per tile**, range 220-549 MB. Not 33 MB.
The 201.4 -> 33.4 MB figure above was taken on tile (-5, 2) at an earlier bake
version and is still a correct measurement *of that tile at that version* -- the
tile has simply grown a great deal since (basins v2, headwater tables, span and
placement planes). For planning today:

    per fine tile        ~414 MB compressed   (measured, n=15, land-biased)
    256-tile world       ~100 GB              (not 9.7)

The n=15 sample is biased toward land -- those tiles were baked for water and
karst work -- and 66 of the 256 bakeable tiles are all-ocean and will be much
smaller, so **~70-100 GB** is the honest range. Re-measure before quoting this:
it has been wrong by an order of magnitude once already.

**`pregen --codec` now defaults to `zstd`** (since 2026-08-03,
`pregen.py:1232`), and the migration is under way rather than pending: of the 38
resident tiles of the current world, **24 are `CODEC_ZSTD` and 14 are
`CODEC_RAW`** (`bench/bankprobe.cpp:190`). A mixed cache is expected and correct
— see the identity note in §3 for why one tile id can address two files.

There is deliberately **no `auto`** — a flag that quietly fell back to raw would
fill a cache with uncompressed tiles its operator believed were compressed.

The client decodes `CODEC_ZSTD` by binding a zstd at **runtime** through the
platform loader, from `Binaries/ThirdParty/zstd/Win64/libzstd.dll`
(`TryRegisterRuntimeZstd`, fetched by `tools/fetch-zstd.ps1`). UE 5.8's binary
distribution ships no C/C++ zstd, and the decoder is injected rather than linked
because `ThirdParty/Blosc` already statically links a zstd into the same binary
— a second copy of those C symbols at a version nobody chose fails as **wrong
terrain, not a link error**. With no DLL present a `CODEC_ZSTD` tile is refused
whole with `kNoDecompressor` and never decoded as zeros.

---

## 2b. The cache root — one directory, all three tiers

**`D:/voxelsim/tile-cache` is the world.** Coarse (`s1`), fine (`s16`) and the
flow superblocks (`flow0`, `flow1`) all live under it:

    D:/voxelsim/tile-cache/
      terrain-diffusion-unlabeled-80b9ca451a23eae4/000000000135276f/s1/     289 coarse tiles, 435 MB
      terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd/000000000135276f/
        s16/    the bake_ver 28 fine tiles
        flow0/  flow1/   the superblocks those tiles were baked against
        world-identity.json

**Consolidated 2026-08-21, and it was not cosmetic.** Before that date the coarse
tier lived only in `D:/vox-trunk-cache` and `D:/vox-wet-cache` while the fine tier
lived here, so **no single `--cache-dir` could see both** -- `pregen --mode bake`
takes one root and reads coarse from it, writes fine into it. A bake pointed at
the fine root tried to *generate* coarse tiles (and cannot: the checkpoint weights
died with the pod, see `docs/parent-hook-scope.md`), and a bake pointed at a coarse
root could not see the finished fine tiles or their superblocks, so it would
re-bake all 256 and rebuild the pyramid.

The 289 coarse tiles were copied in from `D:/vox-trunk-cache` and verified two
ways, because a filename match proves only that someone typed the right string:

* all 289 files `sha256`-equal to the source, and
* the **L1 flow superblock fingerprint recomputed from the new location still
  reads `066cf1d469ed`** -- the value stored in the `.vxfl` headers that the
  shipped fine tiles were actually baked against. That digest is one sha256 pass
  over all 256 coarse tiles, so it proves the bytes, not the path.

`D:/vox-trunk-cache` and `D:/vox-wet-cache` still hold identical coarse copies.
They are now **redundant, not authoritative** -- see `docs/backlog.md` for the
cleanup item, and mind the junction hazard before any recursive delete.

The engine reads the same root: `DefaultTileDir`, `DefaultFineTileDir` and
`DefaultFineTileProviderId` in `ue-project/Config/DefaultGame.ini` now all name
it. `DefaultTileDir` used to point at `D:/vox-wet-cache`; since the bytes are
identical that repoint changed where the engine reads, not what it reads.

A stale `manifest.json` sat at the cache root describing a **different world**
(seed 424242, provider `...71e2b362e3241e71`, 121 tiles) -- a leftover from an
old pod packaging run, read by nothing. Renamed to
`manifest.STALE-seed424242-provider71e2b362.json` rather than deleted.


## 3. Version identity: three numbers, and what rolls which

This is the part people get wrong most often, so it is stated as a rule.

| number | value | lives in | what it means when it moves |
|---|---|---|---|
| `TERRAIN_VERSION` | **8** | `terrain-service/terrain_service/bake/pipeline.py:457` | **A NEW WORLD.** Every measurement, screenshot and site coordinate anyone holds is invalidated. |
| `BAKE_VERSION` | **28** | `pipeline.py:469` | Tiles are re-baked **onto identical ground**. Products change; the ground does not. |
| `kWorldGenVersion` | **28** | `voxel-core/include/voxelcore/core.h:416` | The client draws different voxels from the same tile. Invalidates edit logs and golden digests. |

**`TERRAIN_VERSION` decides the ground.** Bump it when the surface changes — a
new stage, a new constant in `BakeConstants.as_payload`, a kernel that moves a
height. It feeds `roughness_seed` and `bake_identity_payload`.

**`BAKE_VERSION` decides the products.** Bump it when the bake emits something
new or differently — a new section, a table layout, a threshold that decides
written bytes. It is stamped in the tile header as `bake_ver` and hashed through
`product_identity_payload`. The 9→14 run was all water work (discharge, water
heads, the graded water plane, single-receiver routing, lateral fill, and at 14
making the drawn river touch the slope); 15→28 continued through ponds,
headwaters, and per-cell lake depth + signed shore distance, and then **28 added
the five placement planes** (distance-to-water, standing water, TWI, talus,
curvature/heat) that asset placement reads. The ground did not move for any of
it.

**`kWorldGenVersion` decides what the amplifier does with a tile.** Its full
history lives in a per-version changelog in `core.h` above the constant.

Both bake numbers feed `fine_provider_id` (= coarse provider id + `BAKE_VERSION`
+ bake fingerprint), which is how tiles are content-addressed.

### The identity payload carries no codec, and that matters

`fine_provider_id` guarantees identical **decoded planes**, not identical
*bytes*. The identity payload (`bake_version`, `stage_order`, `geometry`,
`constants`, `provinces`) has no codec field, so the same tile stored
`CODEC_RAW` and `CODEC_ZSTD` shares one id and two different files. That is
deliberate — it is what lets the codec default change without orphaning a single
baked tile — but anything treating the id as an ETag or a byte checksum must key
on `(id, codec)` instead. **The id addresses the world, not the file.**

### ~~Known gap: `core.h` has no v23 entry~~ — CLOSED 2026-08-19

The changelog now carries a full entry for every version through **v28**,
including the v23 one this section was written about. A reader of `core.h` no
longer sees v22's savanna prose sitting above a constant that says 23.

---

## 4. File map

**Stage 1 — the sketch (conditioning)**

| file | role |
|---|---|
| `terrain-service/tools/make_conditioning.py` | builds the conditioning sketch grid (`CELL_KM` 7.68, 160 cells ≈ 1229 km, seed 20260719) |
| `terrain-service/patches/terrain-diffusion-worldgen.patch` | this repo's patch against upstream `synthetic_map.py` — **contains the orographic precipitation code** (§6.2). Pinned to upstream `82a0431`, applied by `bootstrap_pod.sh`. |
| `terrain-service/data/conditioning-artifacts.json` | the pin: sha256 + size + origin URL for every conditioning file |
| `terrain-service/tools/bootstrap_pod.sh` | step 7 `build_stats()` builds `data/global/synthetic_map_stats.json` |
| `terrain-service/tools/orographic_check.py` | verifies the rain-shadow patch is seam-free |

**Stage 2 — coarse diffusion**

| file | role |
|---|---|
| `terrain-service/terrain_service/providers/diffusion.py` | `WorldShapeConfig` — every knob that changes the world, and the identity payload |
| `/d/terrain-diffusion` (external repo) | the model itself; `world_pipeline.py` holds the `cond_snr` mixing maths |
| `terrain-service/docs/worldgen-levers.md` | what each dial does, what it costs, and which ones lie |

**Stage 3 — flow superblock**

| file | role |
|---|---|
| `terrain-service/terrain_service/bake/pipeline.py` | `build_flow_superblock` (`:2735`) — the pyramid itself |
| `terrain-service/terrain_service/pregen.py` | pass 2 orchestration; calls `build_model_superblock` |

**Stage 4 — the fine bake**

| file | role |
|---|---|
| `terrain-service/terrain_service/bake/pipeline.py` | the stage order, `BakeGeometry`, `BakeConstants`, both version constants |
| `bake/province.py` | landform provinces — six per-cell parameter fields (§6.5) |
| `bake/flow.py` | priority-flood fill, D8, MFD accumulation |
| `bake/incise.py` | stream-power incision `K·A^m·S^n`, consumes province fields via `field_scale` |
| `bake/thermal.py` | mass-conserving slope-limited relaxation against the repose field |
| `bake/noise.py` | the bake's octave ladder, consumes province fields via `amp_scale` |
| `bake/basins.py`, `bake/water.py` | the water half — see the water documents |
| `terrain_service/tile_codec.py` | `.vxtl` v2 encode/decode; the Python mirror of the pixel-size table |

**Stage 5 — the client amplifier**

| file | role |
|---|---|
| `voxel-core/src/amplifier.cpp` | the whole amplifier: carrier evaluation, both octave tables, every cap |
| `voxel-core/shaders/worldgen.ush` | the bit-exact HLSL mirror. **Every change lands twice.** |
| `voxel-core/include/voxelcore/carrier.h` | the C² carrier, pixel-size-agnostic across scale 1/8/16 |
| `voxel-core/include/voxelcore/biome.h` | `classifyBiome` and the biome gate constants |
| `voxel-core/include/voxelcore/tilestore.h` | `FineTile`, `FineTileSampler`, the fine-tier geometry constants |
| `voxel-core/include/voxelcore/tilestreaming.h` | the residency gate and prefetch ring |
| `voxel-core/include/voxelcore/detail_rill.h`, `detail_bedding.h` | standalone bounded detail terms |

**Instruments**

| tool | what it answers |
|---|---|
| `vxc_terrainprobe` | slope-by-scale, band fits, and `--calibrate --fine-dir` (solves octave amplitudes against a real fine tile) |
| `vxc_stagedump` | dumps any stage's heightfield for comparison |
| `vxc_bench --radius 16 --digest` | the coarse-tier determinism digest |
| `vxc_gpu` | CPU/GPU bit-exactness |
| `terrain-service/tools/stage_realism_report.py` | the geomorphology metric battery |
| `terrain-service/tools/earth_reference.py` | fetches the real-Earth DTMs the battery compares against |
| `terrain-service/tools/world_map.py` | the standard heightmap / biome / province overlays |

---

## 5. Constants, and where they live

Everything here is `constexpr` on the C++ side with an HLSL twin. The twin is
not optional: a mismatch is silently wrong terrain on one of the two paths.

### Geometry

| constant | value | file |
|---|---|---|
| `kFineTileSize` | 8192 | `voxel-core/include/voxelcore/tilestore.h:140` |
| `kFineTileScale` | 16 | `tilestore.h:141` |
| `tilePixelSizeMm(scale)` | 1 → 30000, 8 → 3750, **16 → 1875** | `tilestore.h:73` |
| `PIXEL_SIZE_MM` (Python mirror) | same table | `terrain_service/tile_codec.py:44` |
| `TILE_SIZE` (coarse px) | 512 | `tile_codec.py:33` |
| `coarse_pixel_m` | 30.0 | `bake/pipeline.py:446` |
| `apron_coarse_px` | 32 (= 960 m) | `bake/pipeline.py:451` |
| derived: `padded_coarse_px` / `padded_fine_px` | 576 / 9216 | `pipeline.py:481,485` |
| `FINE_PX_PER_COARSE_CELL` | 256 (one model cell = **7.68 km** = half a tile) | `providers/diffusion.py:174` |

### World shape (stage 1–2 dials)

| constant | shipped value | file |
|---|---|---|
| `cond_snr` | `(0.3, 0.1, 1.0, 0.1, 1.0)` | `providers/diffusion.py:525` |
| `frequency_mult` | `[1.5, 3, 3, 3, 3]` | `providers/diffusion.py:515` |
| `elev_gain` / `elev_gain_power` | 1.6 / 2.0 | `diffusion.py:590–591` |
| orographic block | `oro_wind_from_deg=270`, `oro_barrier_m=1200`, `oro_upslope_m=600`, `oro_shadow_strength=0.75`, `oro_enhance_strength=0.60`, `oro_sea_blend_m=200` | `diffusion.py:549–566` |

### Biome gates

| constant | value | file |
|---|---|---|
| `kBiomeTempWarmU8` | 185 (= 18 °C) | `biome.h:121` |
| `kBiomeTempHotU8` | 204 | `biome.h:134` |
| `kBiomePrecipSeasonalHighU8` | **89** (= 70.0% CV, via `climatePrecipVarU8FromDeciPct(700)`) | `biome.h:184` |

`kBiomeSeasonalHighU8` (the old value 128) **no longer exists** — it was deleted
at v22, not renamed. §6.3 explains why.

### Amplifier bands and caps

| constant | value | file |
|---|---|---|
| `kDetailOctaves` — landform band | `{25600, 2600}`, `{6400, 1100}` | `amplifier.cpp:474–475` |
| `kDetailOctaves` — microrelief band | `{1600, 500}`, `{400, 165}`, `{200, 95}` | `amplifier.cpp:511–513` |
| `kFineDetailOctaves` | `{3200, 100}`, `{1600, 100}`, `{400, 400}`, `{200, 200}` | `amplifier.cpp:547–558` |
| `kReliefScaleMinQ10` | 102 (the 0.10× relief floor, every octave) | `carrier.h:966` |
| `kDetailCapReliefLoMm` | 3600 | `amplifier.cpp:1254` |
| `kMicroGradCapKQ10` (coarse tier) | 1229 (1.2×) | `amplifier.cpp:1150` |
| `kFineMicroGradCapKQ10` (fine tier) | 1229 (1.2×) | `amplifier.cpp:1182` |
| `kFineDetailSumCapKQ10` | **1024** (1.0×, fine tier only, v23) | `amplifier.cpp:1185` |
| `kBeddingAmpMm` | 120 | `detail_bedding.h:376` |

The two micro caps are separate constants that happen to hold the same value —
`amplifier.cpp:1674` picks between them on the `fine` flag. v21 brought both down
from 1.5× to 1.2×.

---

## 6. Where the world's variety comes from — and the 2026-08-01 rebuild

For weeks the complaint was that the world read as monotonous. The cause turned
out to be a **data file, not a design**. This section is the current state of
that work.

The standing requirement from the owner is worth restating because it decides
arguments: **caused variety, not placed variety.** A player should be able to
see *why* the world changed — a desert because a mountain range is upwind of it,
not a desert because a noise function said "desert here".

### 6.1 The conditioning statistics were fake, and they are now real

`synthetic_map.py` builds the sketch by **quantile matching**: read real Earth
rasters, build a 64-quantile table of Earth's distribution per channel, build
the same for Perlin noise, then map each noise value to the Earth value at its
quantile. The sketch is *Perlin noise wearing Earth's histogram*.

**The histogram was not Earth's.** When WorldClim was unreachable during an
earlier build, `_prep_stats.py` substituted **hand-written latitude formulas**
for `bio_1/4/12/15` — while the real 2019 10-arc-minute rasters sat unused in
`data/global`. The result was a world that was nearly one rainfall everywhere.

Measured, before and after the rebuild (`measurements/conditioning-stats-real-climate-2026-08-01.txt`):

| | fake | real |
|---|---|---|
| precipitation IQR | 441 mm | **851 mm** |
| p5 | 320 mm | **39 mm** |
| p50 | 770 mm | 588 mm |
| max | 2525 mm | **6445 mm** |

**The headline number: precipitation spanned only 7.8% of its encodable range
over land** (p5–p95 = u8 15 to 30, out of 255) against temperature's 43.9%. In
plain terms — the whole planet's rainfall fit inside a sixteenth of the dial.

**Two honest caveats about the "before" numbers.** The often-quoted "DESERT
1.84%, RAINFOREST 0%" appears only in plan prose, never in a measurement file,
and the provinces plan pairs 1.84% with **SAVANNA 0.00%**, not RAINFOREST — and
describes it as a classification of the *sketch*, not of model output. Quote it
as indicative, not as a measurement.

**And the rebuild alone did not fix deserts.** After both halves of Wave 1 the
coarse census still showed DESERT 0.00% and SAVANNA 0.00%
(`measurements/climate-calibration-2026-08-01.txt`). What actually made deserts
exist was a later, separate fix: a monotone remap of the *model's output* in
`adapt_raster_to_tile` (commit `3b511e3`, re-fit on the full pipeline in
`56257c8`).

**Where it ended up**, over 289 tiles and 37.9 M land pixels
(`measurements/biome-screenshot-targets-2026-08-01.txt`):

    DESERT       9.74%
    RAINFOREST   4.73%
    all eight mappable biomes non-zero
    world precipitation p5 0 / p50 376 / p95 1694 mm/yr

The stats file lives at `data/global/synthetic_map_stats.json`, is built by
`bootstrap_pod.sh` step 7, and **is pinned** in
`terrain-service/data/conditioning-artifacts.json` (sha256 `8fe9d083…b1d3`,
12,297 bytes, sourced from GitHub release tag `conditioning-v1`). Note
`builder_reproduces_pin: false` — the pin is the authority, the builder is not.

Shipped in `8ce2890`; pinned in `49bb67b` and `41a73a4`.

### 6.2 Rain shadows: precipitation is now coupled to terrain

**This is the "caused variety" mechanism, and it ships.** It runs in the
*conditioning sketch* (`finalize_synthetic_map`), upstream of the diffusion
model — not in the bake and not on the client. The code is in this repo's patch
against upstream: `terrain-service/patches/terrain-diffusion-worldgen.patch`.

How it works, in one paragraph: for each land cell, look upwind at four probe
distances and take the biggest elevation the wind had to climb over
(`barrier`). Shadow strength ramps to full over 1200 m of barrier; upslope
enhancement ramps over 600 m. The rainfall multiplier is
`(1 + 0.60·enhance) · (1 − 0.75·shadow)`, faded in over the first 200 m inland
so coasts do not step.

Measured: correlation between upwind barrier and the rainfall multiplier is
**−0.734**; mean multiplier is **0.493 behind a barrier over 600 m** against
**1.393** with no barrier; land under 400 mm/yr went from 34.7% to 40.4%. Seams
are zero by construction — the patch re-samples elevation noise at absolute
world coordinates, and `orographic_check.py` measures max|diff| = 0.0.

**Two caveats, both deliberate.** The wind is a **single fixed global bearing** —
no latitude bands, no Hadley cell, no seasons. And the patch refuses to claim
which compass direction the dry side faces, because two coordinate swaps sit
between it and the rendered world; it claims only that the effect is
*consistent*. Finally, the acceptance criterion the plan wrote was "deserts
**visibly** downwind of ranges", and no screenshot backs that up.

**CLOSED 2026-08-05 on the statistics.** The owner was shown that the visual
confirmation was missing and accepted the statistical case instead: *"i accept
statistics for 3"*. So the −0.734 correlation, the 0.493-vs-1.393 multiplier
split, and 34.7% → 40.4% of land under 400 mm/yr are the acceptance record for
this feature. This is the owner's call and it is recorded here so nobody
re-opens it as an outstanding gap.

Note what that does and does not settle: it settles that the rain shadow is
**real and consistent**, which is what the statistics measure. It does not
settle which compass direction the dry side faces in the rendered world — two
coordinate swaps still sit between the patch and the screen, and only a
screenshot could ever resolve that. If a player ever reports the dry side
being on the wrong side of a range, this is the first thing to check, and the
statistics above will not have been wrong.

### 6.3 The savanna gate asked for a contradiction

`classifyBiome` used to require `bio_1 ≥ 18 °C` **and** `bio_4 ≥ 1500`
(temperature seasonality). Checked against 437,571 WorldClim land pixels in the
±60° crop: **the maximum `bio_4` anywhere with `bio_1 ≥ 18 °C` is 1,084.** Zero
pixels of Earth satisfy the gate. Rebuilding the stats raises the achievable
ceiling only to about 1,194.

So **no amount of conditioning work could ever have produced a savanna.** This
is the cleanest example in the project of a gate being unreachable rather than
mistuned, and it is why "we tried harder on the input data" was never going to
be the answer.

**The fix was not a re-derived threshold — the gate changed variable.**
Section 2 of `measurements/biome-gates-2026-08-01.txt` is titled *"NO bio_4
THRESHOLD FIXES SAVANNA — IT IS THE WRONG VARIABLE"*: at `bio_4 ≥ 200` the gate
calls Houston, Brisbane and Miami savanna while rejecting the Serengeti, the
Cerrado and Tsavo. Real savanna is defined by a **wet season and a dry season**,
which is `bio_15` (coefficient of variation of monthly precipitation), not by
hot summers and cold winters.

So `classifyBiome`'s third argument moved to `bio_15` and the threshold is 70%
CV, derived twice and agreeing: `sqrt(4/8) = 70.7%` from the physical definition
of a 4-wet-month regime, and 15.57% of Earth's land empirically against the real
~15.6%. `kBiomePrecipSeasonalHighU8 = 89`. Wire format unchanged, `provider_id`
did not roll.

Shipped as worldgen **v22**, commit `2fce31a`.

### 6.4 Elevation tails, and the lever that was documented backwards

**Tails: stretched, and it shipped.** `elev_gain = 1.6`, `elev_gain_power = 2.0`.
Land above 1 km went 24.31% → **27.05%**, land above 2 km 5.29% → **8.07%**,
coarse maximum 4,799 m → **7,465 m**. A gain of 2.0 was rejected — the table
asks for 11,628 m and the model returns 8,144 m, which is visible clipping.

**`cond_snr[0]` was NOT lowered.** It was tested at 0.15 and **rejected**:
tightening it *reduced* relief, taking land above 1 km from 27.05% down to
24.87% and land fraction from 40.5% to 37.7%. **Shipping keeps 0.30.** If you
have read anywhere that the SNR was lowered, that document is wrong — see §12.

**And the diagnosis that motivated the wave was itself wrong**, which is worth
keeping. The premise was "the table asks for 22.6% of land above 1 km and we
deliver 1.4%". That 1.4% was **local relief per 2 km window**, not elevation
above 1000 m — two different quantities. At gain 1.0 the model actually
*over-delivers* (table 22.2%, model 24.31%).

**The lever documentation was backwards and is now fixed.** `cond_snr` reads
like a signal-to-noise ratio but is the *tangent of a mixing angle*:

    t    = atan(snr)
    cond = cos(t) * sketch + sin(t) * noise

So `snr → 0` is pure sketch and `snr = 1.0` is a 45° equal mix. **Lower means
tighter.** Elevation at 0.3 is 16.7°, roughly 96% sketch — fairly tight.
`terrain-service/docs/worldgen-levers.md` documented this the wrong way round
(the `cond_snr` row of its dial table) and now states it correctly.

### 6.5 Landform provinces — per-cell fields, never per-tile constants

**The problem:** two places with similar relief and similar climate came out
looking like siblings, because the bake applied one global set of constants
everywhere. Iceland is not Poland because of glaciation and volcanism, not
because of a different erosion rate.

**Tier 1 shipped at `BAKE_VERSION` 7** (commit `4f9a6e7`, 2026-08-01) —
`terrain-service/terrain_service/bake/province.py`, a four-province soft
partition (FLUVIAL / GLACIAL / ARID / LOWLAND).

**It is a per-cell field, and this is the load-bearing design decision.** The
obvious design — look up a constant set per tile — is wrong, because two
adjacent tiles baked with different constants **disagree along their shared
edge**. Here the province is a per-cell field and each province-varying constant
becomes a per-cell *parameter* field, so blending between provinces is a
smoothstep on the field rather than a special case in the code. Anyone who
describes provinces as per-tile constant sets is describing a design that was
explicitly rejected.

Six fields ship: `profile_K_dt`, `a_crit_m2`, `stream_m`, `gate_q`,
`meso_amp15_m`, `meso_amp11_m`. `incise.py` consumes them via `field_scale`,
`noise.py` via `amp_scale`. `stream_n` and `incision_cap_m` were left out on
purpose (they are numba kernel scalars); `mfd_p` was cut but is still hashed
into `superblock_inputs_fingerprint`.

**Two engineering details worth keeping.** Every field is computed on the
**padded coarse domain** (576² at 30 m/px) and consumed by `//scale` indexing
rather than `np.repeat` — that is 7.96 MB total instead of 340 MB *per field* at
9216². And every climate discriminant is smoothed to landform scale **before**
it reaches a threshold, because climate arrives 30 m and uint8-quantised
(precipitation's least significant bit is 47 mm/yr) and would otherwise print
30 m blocks into the erosion intensity.

**What provinces do not buy:** a seam guarantee. There was never one to restore.
`APRON_BLIND_SPOT` measured 1.05% of the shipped interior moving past the 100 mm
wire quantum (by up to 78.79 m) when the apron was widened, with the domain
border's influence reaching **3.8 km inward** — four apron widths — because the
depression fill is unbounded and a truncated domain *invents an outlet*. The
honest claim is "no new influence radius", not "seam-safe".

**This also largely closes the old "the bake has no climate conditioning at all"
gap.** That audit found zero climate references in `bake/*.py`; today
`province.py`, `pipeline.py`, `basins.py` and `water.py` all read climate, and
`profile_K_dt` is a per-cell erosion coefficient derived from it. The remaining
unbuilt parts of that plan are debris/boulder fields and biome-varying
micro-roughness, both client-side.

---

## 7. What the client adds, and the caps that keep it honest

The amplifier owns everything below 1.875 m. It is split into named bands, and
the band ownership comment in `amplifier.cpp` is the authority on which band
owns which wavelength.

**Coarse-tier table (`kDetailOctaves`)** — used when there is no fine tile:

* **Landform band** (25.6 m, 6.4 m): hillside-shaping octaves, scaled by
  `reliefScaleQ10` so they vanish on flats.
* **Microrelief band** (1.6 m, 40 cm, 20 cm): floored, not gated — decimetre
  roughness is a property of the material, not of the gradient.

**Fine-tier table (`kFineDetailOctaves`)** — used when a baked tile is loaded:

* **Shaping band** (3.2 m): one octave, because 3.2 m is the first wavelength a
  1.875 m raster cannot resolve.
* **Microrelief band** (1.6 m, 40 cm, 20 cm): keeping 1.6 m *floored* rather
  than promoting it to the gated shaping band is deliberate. Metre-scale energy
  on flat ground is exactly the fix that stopped flat terrain reading as long
  terrace runs at 10 cm voxels, and gating it on slope would undo that.

### The banding is solved — do not reopen it

**The owner complained for weeks that the world had visible stripes. The cause
was the 3D density band, and removing it at worldgen v20 fixed it.**

Why every instrument missed it: the term does not move `amp.surfaceMm` at all.
It displaces where voxels are *placed* relative to that surface, by up to 3.5
voxels. Every prior probe read the height field, so the defect was
**structurally invisible** to all of them.

Measured at site (−69094, 38426), seed 20260719, 262,144 columns: the top solid
voxel moved on **232,282 columns — 88.61%** — by −300 to +300 mm (mean
displacement 212 mm). An FFT of the displacement field gave a dominant
wavelength of **10.04 m** with stripes at **78.7°**, and ~60% of the variance
sitting at 0.8–3.2 m, which is exactly `detail_bedding.h`'s bed thickness.

It was also shown to be untunable rather than mistuned: overhangs went 0.01%
raw → 0.54% (2 contrast passes) → 1.97% (shipped) → 3.57% (4 passes), and every
setting was either banded or inert.

**Owner-confirmed in a live editor session** at that cliff site (camera 250 m,
pitch −20°, sun frozen at 12:00 on 03-20, 25 real diffusion tiles with no
synthetic fallback), verdict recorded verbatim: **"YES THIS LOOKS GOOD. PASS"**.
The v20 placed-voxel dump was verified bit-identical to the panel the owner
approved.

Shipped as `ecda4b6` + `cf0b807`. Two open caveats the measurement itself flags:
NVIDIA digest parity was never run for v20 (AMD RX 7800 XT only), and the 2D
bedding term `kBeddingAmpMm = 120` was kept — it is the first thing to
re-examine if any residual banding appears.

**Any document that still describes banding as an open problem is out of date.**

### The gradient caps, and what v23 changed

The rule the caps exist to enforce: **client detail must not reverse the
carrier's downhill.** If it does, water strands in puddles the bake never made.

v19 split the fine-tier detail into two pools — a routing pool capped at ≤1.0×
the carrier gradient, and a micro pool capped at 1.2× — then capped each
*separately and summed them*. Each pool looked compliant while the total was
licensed to reach **2.2× the carrier gradient**.

**v23 caps the sum** (`kFineDetailSumCapKQ10 = 1024`, i.e. 1.0×, fine tier
only). Stranded area at four sites:

| site | before v23 | after v23 |
|---|---|---|
| (−7, −2) | 35.5% | **3.6%** |
| (−3, −3) | 11.5% | 0.0% |
| (−2, −4) | 0.1% | 0.0% |
| (−8, −12) | 77.2% | 71.4% — **exactly its carrier**, client contributes zero |

v23 also disproved the attribution it inherited: tightening the micro multiplier
from 1.20× to 1.00× moves stranded area by **at most 1.6 percentage points**, so
the micro cap was never the lever. `kFineMicroGradCapKQ10` was deliberately left
at 1229.

The coarse tier is provably untouched (`vxc_bench --radius 16 --digest` =
`cca9b86e78da033e`, byte-identical) and `vxc_gpu` is bit-exact at v23.

**This is not closed.** See §11 — v23 has never been judged for visible
terracing, and the headline 11.3% figure was never re-measured after it.

---

## 8. Production: multiplayer first, offline-capable

**The game must support both online multiplayer and offline single-player.**
Multiplayer is built first. In multiplayer, work moves to the server wherever
that raises client frame rate. In single-player there is no server, so the
client must be *capable* of everything — which means the expensive paths are
written once, as a library, and invoked from two places.

### Who does what in multiplayer

**Server owns:** coarse generation (needs a GPU the player may not have);
superblock construction and the completeness gate (the correctness-critical
step, and it must have one authority); fine baking (~130 s CPU and ~5 GiB peak
per tile — baked once per `(seed, BAKE_VERSION)` and served to everyone who ever
visits, instead of re-paid by every visitor against their frame budget); and
edit ordering.

**Client owns:** voxelisation and meshing from the heightfield (GPU work the
client must do anyway, and far less data than shipping voxels), rendering, and
the water display path.

### The superblock completeness gate must become a publish gate

Today an incomplete superblock prints a warning and bakes anyway:

> `flow superblock L1 (-1,-1) is INCOMPLETE (102 of 256 coarse tiles absent).
> Rivers entering from those tiles contribute nothing, permanently, to every
> tile baked against it.`

And `pipeline.py` is explicit that this never self-heals: *"A tile baked against
an incomplete superblock stays baked that way."* **In production that warning
must become a gate** — a fine tile may not be baked, cached or served until its
superblock is complete. It is cheap: a coarse tile is 1.5 MB against a fine
tile's 201 MB raw — **134× smaller**, or ~22× against the 33 MB compressed
figure — so 256 × 1.5 MB = 384 MB of coarse data buys catchment-correct bakes
for 246 km of world. (Older notes say 127×; that was computed from the retired
190 MB estimate.)

### The coarse frontier must lead the fine frontier

The gate is not free at the edge of generated space, and this was measured on
the shipped world rather than argued. The 20260719 world is 17×17 coarse tiles
(−16…0 on both axes). Superblocks are 4-aligned, so a tile can only bake when
its whole 4×4 block exists: **256 of the 289 tiles qualify and 33 do not, and
the 33 are exactly the index-0 row and column.** 19 of them are land. The lake
survey baked precisely those 256 — not by picking a tidy 16×16, but because the
gate refused the rest.

This is what a frontier always looks like. In an infinite world the newest
region's outer tiles can never bake, because the neighbours that would complete
their block do not exist yet. So **coarse generation must run at least one full
superblock-block ahead of where fine baking is allowed** — one block, not one
tile, and aligned to the block grid rather than to the player. Sizing it off the
player's position gives a ragged frontier that stalls whenever someone walks
toward a corner.

A world that wants no permanently-unbakeable edge must also choose dimensions
that are a multiple of the superblock stride. 17 is not.

### The pyramid must be capped, and the cap is visible in-game

An infinite world cannot have an infinite superblock. Choosing a maximum level
chooses **the largest river basin the world can resolve** — cap at L2 and no
catchment larger than ~983 km exists. That is a world-design decision with a
consequence a player can see, forced by infinity. **It has not been made.**

### Streaming: the tile is a storage unit, not a transfer unit

See §2 for the sizes. The requirement is not "ship 190 MB", it is "ship the
couple of megabytes the player can actually see", which means **fine tiles must
be sliceable into independently addressable sub-blocks**. Partial `FineTile` and
ranged loads now exist (task #52); the client cache is a **12 GiB LRU** keyed on
`<provider_id>/<seed>/s16/<x>_<y>` with identity validation that refuses foreign
tiles — at ~0.3 GiB resident per tile, about 40 tiles ≈ 9,400 km² held locally.

### Offline single-player: the same library, no server

The bake is already engine-free, integer-only and deterministic, so this is a
packaging problem rather than a rewrite. Two costs land on the player's machine.

**Peak memory.** Peak is ~64 bytes per padded cell, so it scales with the *area*
of the bake unit — and the unit is a design parameter:

| coarse_px | tile | peak est. | apron % | CPU for the same ground |
|---|---|---|---|---|
| **512** (production) | 15.36 km | **5.06 GiB** | 21% | 1.0× |
| **256** | 7.68 km | **1.56 GiB** | 36% | 4.8× |
| 128 | 3.84 km | 0.56 GiB | 56% | 28× |

5 GiB is not shippable to consoles or low-end PCs; ~1.6 GiB is. **But shrinking
the unit trades a memory problem for a correctness problem**: `APRON_BLIND_SPOT`
measured border influence reaching 3.8 km inward, which at a 7.68 km tile would
contaminate half of every tile. Bound the depression fill *first*, then shrink.

One loose thread found while measuring, still open: `estimate_peak_bytes`
reports 5.06 GiB against a docstring claiming 6.33 against a measured 6.90 — the
Wave 0 dtype work moved it and **the real peak needs re-taking**. (The 1.27 GiB
of B1 ballast — `gy`/`gx`/`slope`/`delta` never dropped — that older notes list
as unfixed **was fixed in `8ed70a8`**, which frees all four at their real last
use. Only the estimate is stale, not the code.)

**CPU is schedulable.** ~130 s per tile sounds fatal beside a frame budget, but a
tile is 15.36 km — a player crossing at 20 m/s has ~13 minutes of lead time.
Background-thread the bake at low priority and it is a fraction of one core.
**The constraint is memory, not CPU.**

### Water and edits, in one paragraph each

**Water** is split the same way as terrain vs. edits — deterministic content vs.
mutable state. The ocean is a level, lake surfaces and river beds *should be*
baked content, and flowing water is runtime. The bake emits no water voxels;
`BAKE_VERSION` 9–14 is the run of work putting real discharge, water heads and a
graded water plane into the tile. **All of that lives in the water documents
(§12) and this document does not restate it**, because it moves faster than this
one does.

**Edits** are server-authoritative state. A player mines a block or dams a
river; the edit goes in the log (`.vxlog`) and is broadcast. No terrain is ever
re-shipped — the delta is kilobytes against 190 MB. This holds only while every
client has *identical terrain underneath*, which is why the superblock
completeness gate above is a multiplayer-correctness requirement rather than a
quality nicety.

### What is shared and what is mode-specific

**Shared:** the bake library, the amplifier, the tile format and codec, the
edit-log format, the client cache and residency gate.

**Multiplayer-specific:** the bake service and job queue, the completeness gate
as a *publish* gate, CDN and sub-block streaming, edit ordering and broadcast,
frontier pre-baking driven by the population rather than per player.

**Single-player-specific:** in-process scheduling of all three server stages, a
local edit log with no ordering authority, and a bake unit sized for consumer
memory.

The risk to manage is **drift between the two paths**. The defence is content
addressing: if single-player and multiplayer produce different bytes for the
same `fine_provider_id`, that is a bug the identity check catches rather than a
mystery. Never let the single-player path "optimise" into a different result.

### Build order

1. **Superblock completeness gate** — refuse to publish a fine tile whose
   superblock is incomplete. Cheap, correctness-critical, and it blocks
   everything else being trustworthy.
2. **Fine-tile sub-block slicing** — the actual streaming unit (partial reads
   landed; the addressing decision is per-section).
3. **Finish the zstd migration** — the 6.0× ratio is measured and `pregen`
   already defaults to it; 24 of 38 resident tiles are compressed and the
   remaining 14 are not.
4. **Bake service + queue + CDN**, superblock-aligned.
5. **Edit ordering and broadcast** over the existing log.
6. **Bound the depression fill**, then re-evaluate a smaller bake unit.
7. **Cap the hydrology pyramid** — a world-design decision.

**Not on this list, deliberately: mass-baking the world.** Defects baked in now
are permanent, because a shipped tile is never regenerated. The blockers before
mass-baking are the fine-tier detail regression (§11.1), the L1 hydrology gap,
and world identity reproducibility.

---

## 9. How we judge terrain — and what the measurements cannot see

**Read this before quoting any terrain number at anyone.**

### Most of the metric battery is blind to a convincing fake

Slope statistics, geomorphon histograms, hypsometry, drainage density and raw
pit density are **all blind to a spectrum-matched fake** — a surface built to
have the right frequency content will pass every one of them while looking
nothing like erosion.

Only four metrics see through it: **fill volume, curvature asymmetry, Hack's h,
and marginally theta**. And theta is weak even where it works: it separates
terrain classes by only about **1.8×** against a within-scene spread of **2.6×**.
In plain terms, the differences between two parts of the same landscape are
bigger than the difference the metric is supposed to detect.

Practical consequence: a metric passing is not evidence of realism. A metric
failing is evidence of a problem.

### Three metrics that are measurements of us, not of the landscape

* **`curvature_asymmetry` is identically zero on any quantised surface.** It
  cannot be read at all on the 30 m coarse tier or on a 10 cm voxel field. It is
  undefined whenever the vertical quantum exceeds ~1% of the cell size.
* **`theta` on the 1.875 m plains band is a degenerate fit** and should not be
  quoted there.
* **At 30 m, every realism gate passes on a surface that is visibly not a
  plain.** The gates were never a class check. Any CI gate that follows needs
  `frac_flat_coarse_thresh` and `frac_ridge_peak` measured against the reference
  for the tile's *claimed class*, not just the four realism metrics.

### Never quote a single ladder row

On one tile at v22 the amplified stranded figure ranges **1.3% to 93.2% across
windows on the same tile**. Report the row, its carrier, and the spread — a bare
percentage from a drainage ladder is not a finding.

Related, and it burned a whole analysis once: `tools/drainage-ladder.ps1` labels
rows by whole-tile p50 grade while probing an off-centre 384 m quadrant. Sorted
by the grade actually probed, ladders that looked non-monotone are monotone.

### Measure terracing as plateau *area*, not run length

Averaging run length made a 2.6× difference disappear and led to predicting the
wrong outcome. Use area.

### When metrics and the owner disagree, the owner is judging screenshots

The method that actually resolved the banding was **same-ground A/B renders per
pipeline stage**, not metrics. Present captures and the conditions they were
taken under; do not present a verdict. Readings from this side have been wrong
in both directions.

---

## 10. Measurement provenance — which world, which version

Every terrain number ages. This table says what each measurement was taken on,
so a stale figure can be spotted without re-deriving it.

| measurement | world | version | still current? |
|---|---|---|---|
| `terrain-validation-2026-07.md` (frac_ridge_peak, the "0.3–0.6 m regardless of class" detail figure, the class-inversion result) | **a different, unpinned world** — `…UNPINNED-UNVERIFIEDDATA-27ac04bc…`, seed `000000000135276f` | **worldgen v12 era**, measured 2026-07-29 | **NO.** Pre-dates v13/v14/v15 (the gradient caps), v16–v19 (warp, meso, two-pool), v20 (band removal) and the real-climate rebuild. Historical. |
| `drainage-ladder-v13-2026-07-29.txt`, `client-detail-*-2026-07-29.txt`, `v14-verification-2026-07-29.txt` | 20260719 | v13/v14, `BAKE_VERSION` 4 | Superseded numerically by v19–v23; the *methods* still stand. |
| `contour-crookedness`, `repose-field`, `straight-ridge-ablations`, `shape-survey-verdict` (2026-07-30) | 20260719 | v16–v18 | Method current; numbers pre-v19. |
| `placed-voxel-banding-2026-07-31.txt` | 20260719 | v19 → v20 | **Current and closed** — this is the banding proof and the owner's PASS. |
| `band-period`, `meso-band`, `d8-direction-lock` (2026-07-31) | 20260719 | v19/v20 | Current as findings. D8 direction lock is a real 45° routing artifact in the bake — and is **not** the banding the owner saw. |
| `micro-grad-cap-2026-08-01.txt` | 20260719 | v21 | Current. |
| `conditioning-stats-real-climate`, `climate-calibration`, `elevation-tails`, `biome-gates`, `biome-screenshot-targets` (2026-08-01) | 20260719 | v21/v22, conditioning rebuild | **Current.** These are §6. |
| `geomorphon-v21-2026-08-01.txt` | 20260719 | v21 | **Current**, and its §10 recommendation is still unapplied (§11.4). |
| `province-first-real-terrain-2026-08-01.txt` | 20260719 | `BAKE_VERSION` 7 | Current for Tier 1. |
| the v23 sum-cap numbers (§7) | 20260719 | v23 | Current, **but only 4 sites** — not the 11-window paired corpus the 11.3% headline came from. |
| `etopo-build-not-reproducible`, `world-identity-not-reproducible` (2026-08-02/03) | — | — | **Current and open.** Reproducibility gap. |

---

## 11. Open items

These are open. Nothing below is fixed, and several look fixed at a glance.

### 11.1 The fine tier's drainage regression at v19 — task #47

Paired windows on 4 baked s16 tiles whose fine *carrier* drains (under 1%
stranded):

| version | median added stranding | mean | median sinks |
|---|---|---|---|
| v14 | 0.3% | 5.3% | 2 |
| v18 | 0.0% | 4.2% | 0 |
| v22 | **11.3%** | **16.0%** | 5 |

The step is at **v19** — letting the micro pool exceed the carrier gradient —
not at v17.

**v23 (§7) is the only fix ever applied, and it is real but unfinished.** Why
this stays open:

* The 11.3% / 16.0% headline was **never re-run after v23**. The after-numbers
  are 4 sites, not the 11-window corpus.
* v23's own commit says: *"NOT YET JUDGED: whether this reintroduces visible
  terracing… This needs same-ground captures before it goes anywhere near
  main."* **No such capture exists.**
* The corpus caveat is unmet: those 17 tiles were baked against **incomplete
  flow superblocks** (`-7_-12` reads 1.2% coarse against 72.2% fine; `-4_-10`
  decodes to a dead-flat lake floor at −294.70 m; 3 sites are submarine and
  excluded). Re-measure on superblock-complete tiles before tuning anything.
* There is **no dedicated file for this in `docs/measurements/`** — the whole
  record is `amplifier.cpp:1152–1180`, this section, and task #47.

**Related documentation debt outside this document:** `water-production-plan.md`
still says of v23's predecessor *"That commit applied no fix"*, written before
v23 merged. That file belongs to the water side and is flagged, not edited, here.

### 11.2 The amplifier detail band re-strands the ground at voxel scale — task #48

Carrier against amplified, on the same domain (commit `109f959`, worldgen v13):

| | alpine (−5, 15) | plains (−55, 20) |
|---|---|---|
| interior sinks | 0 → **1,625** | 98 → **9,525** |
| stranded area | 0.0% → **87.9%** | 75.2% → **97.4%** |
| mean flow path | 223.6 m → **28.9 m** | 39.3 m → **4.3 m** |

**v23 did not touch this measurement's code path.** The sum cap is gated on
`fine` (`amplifier.cpp:1713`), and 87.9% was measured on the **coarse** tier —
which v23 proves byte-identical. The specific measurement has never been re-run.

### 11.3 `kFineDetailOctaves` amplitude is still uncalibrated — task #3

The comment forbids tuning by eye: *"The plan requires this be set by probe
measurement against the fine tier's measured S2… Do not tune it by eye."*

**Correction to a widely-repeated claim: `vxc_terrainprobe --calibrate
--fine-dir` HAS been run — three times, on 2026-08-01**
(`measurements/geomorphon-v21-2026-08-01.txt` §8). Both flags exist and compose.
The reason it is still open is that **it gave three different answers**:

| site | H_used | r² | amp_3200 | amp_1600 | amp_400 | amp_200 |
|---|---|---|---|---|---|---|
| flat | 0.607 | 0.959 | 488 | 343 | 157 | 101 |
| mid | 0.704 | 0.964 | 1253 | 770 | 323 | 175 |
| steep | 1.025 | 0.983 | 332 | 310 | 92 | 24 |
| **shipped** | | | **100** | **100** | **400** | **200** |

`amp_3200` spans **3.8× across three sites inside one tile**, and that is not
noise (residuals 7.5–8.3%, r² above 0.96). `H_used = 1.025` on the steep site is
outside the fBm range, so that row cannot simply be averaged in. The recorded
verdict: *"the measurement is possible and its answer is 'NOT YET'"* — **no
number was proposed.**

Closing it needs three things that do not exist: a defined calibration-site
corpus, a rule for combining sites, and a re-run of v18's rib-length
measurement.

**The comment is also doubly stale in code:** it derives 900 mm while the
shipped value is 100, and its "that measurement does not exist yet" clause is no
longer true. Fixing the comment is a code change and has not been made.

### 11.4 The coarse shaping octaves were measured but never gated — task #36

`geomorphon-v21-2026-08-01.txt` found the coarse tier **more than doubles** the
1.875 m mean slope on gentle ground (1.363° → 3.229°, +137%), with **98.7% of
the added energy at 3.2 m and above** — the two shaping octaves. It also found
the brief had named the wrong lever: `engageQ10` gates the gradient cap, but the
actual control on those octaves is `kReliefScaleMinQ10` and specifically its
0.10× floor.

**Nothing was gated.** Section 10 of that file is headed *"PROPOSED CHANGE — NOT
APPLIED, AND CONDITIONAL ON ONE MORE MEASUREMENT"* and closes *"Do not ship this
without it"* (a drainage-ladder re-run with the landform band alone scaled
down). `kReliefScaleMinQ10` is still 102 for every octave and has not moved since
v13. Task #36 is marked complete but only its measurement half landed.

### 11.5 No CI job builds `ue-project` — task #61

Nine jobs run in `.github/workflows/ci.yml` (voxel-core on g++/clang/MSVC, the
cross-compiler determinism digest, a float ban, a unity-collision lint, a shader
UB lint, terrain-service pytest, shader compile, docker build). **None compiles
the UE module.**

`ue-build.yml` holds a real, complete `VoxelEarthEditor Win64 Development`
recipe, but it is gated `if: vars.UE_BUILD_RUNNER != ''` and that repository
variable is unset — **the job is skipped on every run.**

Why it cannot run on a hosted runner, all measured: `D:\UE_5.8` is **30.0 GB
across 251,158 files** against a hosted runner's **14 GB** of disk, before
checkout or intermediates; it is an Epic Launcher Installed Build needing an
authenticated account and EULA acceptance per job; MinimalUE is code-plugins-only,
tops out at UE 5.1, and cannot be redistributed under the EULA — while this
project needs a full 5.8 Editor target with a custom `FPrimitiveSceneProxy` and
vertex factory.

**Two traps recorded in that workflow's header, both worth repeating.** A
skipped job reports as a **PASS** to branch protection, so it must not be marked
required until the runner variable is set. And **`Build.bat`'s exit code lies** —
it exits 0 on `RulesError: could not find voxelcore.lib` — so the gate is a
separate step grepping for `Result: Succeeded`.

The substitute that does run is `tools/lint-unity-collisions.py`, an engine-free
lint that models unqualified-lookup scope at file scope. It found 8 real
findings on the pre-fix tree and is clean on main. It is a good check and it is
not a build.

To turn the real job on: a Windows self-hosted runner with UE 5.8, MSVC and the
Win10/11 SDK, plus the `UE_BUILD_RUNNER` variable. Reference box cost:
~3 min 25 s end to end.

### 11.6 Smaller open threads

* **`core.h` has no v23 changelog entry** (§3). Code fix.
* **`kFineDetailOctaves`' comment is stale in code** (§11.3). Code fix.
* **NVIDIA parity has never been run for v20 or later** — AMD RX 7800 XT only.
* **`estimate_peak_bytes` needs re-taking** (§8), and 1.27 GiB of bake ballast is
  unfixed but unblocked.
* **The hydrology pyramid cap is an unmade world-design decision** (§8).
* **World identity is not reproducible** — see `world-identity-not-reproducible-2026-08-03.txt`
  and `etopo-build-not-reproducible-2026-08-02.txt`. A mass-baked world cannot be
  extended later without this.
* ~~The "deserts visibly downwind of ranges" acceptance criterion has no
  screenshot~~ — **closed 2026-08-05**, accepted on the statistics by the owner
  (§6.2). The residual is which compass direction the dry side faces, which
  statistics cannot answer.

---

## 12. Where everything else lives

### Still current, alongside this document

| document | scope |
|---|---|
| `terrain-service/docs/worldgen-levers.md` | every dial into the diffusion model, what it costs, and which ones lie |
| `docs/climate-biome-province-consumers.md` | which consumer reads climate vs. biome vs. province — and which must not |
| `docs/vxtl-v2-format.md` | the tile format |
| `docs/determinism.md` | the float ban and the CPU/GPU mirror contract |
| `docs/adr/` | the architecture decisions, including 0006 (GPU-resident voxel streaming) |
| `docs/manual-verification-checklist.md` | how to take a capture the owner can judge |
| **water:** `water-production-plan.md`, `watershed-system-plan.md`, `water-handover-2026-08-04.md`, `water-waves-plan-2026-08-04.md`, `water-wet-country-2026-08-05.md`, `w3-channel-carving.md`, `river-farfield-actor-2026-08-04.md` | the water half — separately owned |

### Plans, with what actually happened to them

| document | state |
|---|---|
| `docs/worldgen-variety-plan.md` | Waves 0–2 and 4 shipped; Wave 3's measurement shipped and its change did not. Banner records which. |
| `docs/landform-provinces-plan.md` | Tier 1 shipped at `BAKE_VERSION` 7. Tiers 2 and 3 are still design. |
| `docs/terrain-amplification-plan.md` | Phases 0–4 landed, then Phase 4 was **deleted again** at v20. Historical; the shipped state is §7 above. |
| `docs/plans/climate-debris-microrelief-2026-07-31.md` | Gap 1 largely closed by provinces Tier 1; Gaps 2 and 3 unstarted. |

### Superseded — kept for the record, not for reference

| document | superseded by |
|---|---|
| `docs/fine-bake-production-architecture.md` | this document |
| `docs/terrain-validation-2026-07.md` | this document for anything current; kept because its **method** is the geomorphology battery and §9's warnings come from it |
| `docs/terrain-amplification-reconciliation.md` | `terrain-amplification-plan.md`, then this document |
| `docs/research/terrain-amplification-design-doc.md` | the external proposal the plan was built from |
| `docs/rescued/terrain-amplification-plan-agent-a2d207d0.md` | a rescued working copy of the plan at v12 |

### Measurements

**`docs/measurements/` is never pruned.** Negative results and falsified
hypotheses are the most valuable thing in it — this project has re-run dead ends
because a note went missing. Superseded numbers get a provenance label (§10),
never a deletion.

# M4 — Living world: biomes, vegetation, environment (design in progress)

Design sessions with Matt in progress — round 1 decided (2026-07-20), rounds
2-3 (trees/structures, flora/placement) PENDING; do not start implementation
until this doc says decisions are complete.

## Round 1 decisions (Matt)

| Topic | Decision |
|---|---|
| Biome palette | 8 core biomes (ocean/beach, grassland, temperate forest, rainforest, desert, savanna, taiga, tundra/alpine) — WITH the morphology constraint below. |
| Morphology coupling (Matt's requirement) | Biome selection must flow from the terrain-diffusion outputs — never paste a biome onto incompatible landforms (no swamps on cliff faces). Mechanism below. |
| Transitions | Blended ecotones: hash-dithered gradients over 50-200m. |
| Materials | Grow to ~20 (per-biome surfaces + wood/leaf types). |
| Art target | Naturalistic voxel (Teardown-leaning): organic curves, ragged hash-jittered edges — voxels that read as nature. |

## HARD DEPENDENCY before biome tuning (backlog task)

Confirm the REAL terrain-diffusion tile outputs before finalizing the biome
table. Our pipeline assumes **4 climate channels** (temperature, seasonality,
precipitation, precip-variability, each uint8) + int16 elevation — this is
what the plan specifies and what `tile_codec.py` / `tiles.h` encode. The
actual model checkpoint's raster set (channel count, semantics, value ranges,
units) must be verified at cloud bring-up; any difference is a provider-layer
adaptation + `kWorldGenVersion` bump, NOT a redesign. Only after seeing real
climate distributions do we finalize biome COUNT (8 core now; headroom to
~16) and decide which transitional zones earn their own identity vs. ecotone
blends. Tracked in docs/status.md backlog (top row).

## How biome↔terrain consistency works (answer to Matt's question)

Two mechanisms, layered:

1. **The diffusion model already couples them.** terrain-diffusion generates
   elevation AND the four climate channels JOINTLY — its climate outputs are
   conditioned on its own terrain (rain shadows, altitude cooling, coastal
   moisture are learned from Earth data). So gross physical consistency
   (deserts behind mountains, tundra at altitude) largely arrives for free
   in the tile data itself. (Synthetic dev tiles mimic this only crudely —
   final biome tuning happens against real diffusion tiles; another reason
   the cloud bring-up precedes M4 polish.)
2. **Morphology gates in the biome function (our guarantee).** Biome id is
   computed per column as f(temperature, precipitation, ELEVATION, SLOPE,
   dist-to-water) — the last three derived from tile elevation exactly like
   the amplifier already derives slope today. Gates run BEFORE the climate
   lookup: slope > cliff threshold → rock/alpine faces regardless of
   climate; swamp/marsh requires low slope AND low elevation AND high
   moisture AND (later, W-track) proximity to the water table; beaches
   require the coastal band; alpine overrides above the treeline curve
   (elevation adjusted by latitude/temperature). The Whittaker climate
   lookup only picks among biomes whose morphology gates pass. Deterministic,
   per-column, zero replication — same rules the amplifier already lives by.

## Round 1 implementation — biome classification core (landed 2026-07-20, worktree agent)

Materials, biome classifier, and amplifier/GPU wiring for the 9-biome table
above (trees/flora explicitly excluded — rounds 2-3, still pending design).

**Materials** (`voxel-core/include/voxelcore/core.h`, append-only ids 8-14,
`kMaterialCount` 8->15): `MAT_GRASS` (grassland), `MAT_JUNGLE_SOIL`
(rainforest), `MAT_SAVANNA_GRASS`, `MAT_PODZOL` (taiga), `MAT_PERMAFROST`
(tundra/alpine flat ground), `MAT_MUD` (ocean floor), `MAT_CLAY` (defined,
headroom for a future floodplain/wetland biome — not yet mapped by any
biome). `MAT_SNOW` (id 7) is retired from active use (no biome maps to it
any more) but kept stable for any pre-M4 saved edit logs that reference it.

**`voxelcore/biome.h`** (new, header-only, integer-only): `BiomeId` enum
exactly as scoped (`OCEAN, BEACH, GRASSLAND, TEMPERATE_FOREST, RAINFOREST,
DESERT, SAVANNA, TAIGA, TUNDRA_ALPINE`). `classifyBiome(tempU8, precipU8,
seasonalityU8, surfaceMm, slopeMmPerPx)` runs gates in the order this doc
specifies, each a hard cutoff (no ecotone blending yet — that's still
future work per the "Transitions" row above, out of round-1's scope as
literally requested):
1. **Slope gate**: `slopeMmPerPx > kBiomeCliffSlopeMmPerPx` (6000, the same
   raw sum-of-abs-elevation-deltas-per-pixel unit `Amplifier::column`
   already computes) -> `TUNDRA_ALPINE` regardless of climate.
2. **Coastal band**: `surfaceMm` inside `[kBiomeBeachLowerMm, kBiomeBeachUpperMm]`
   (-3m..+4m) -> `BEACH`; below -> `OCEAN`.
3. **Temperature-adjusted treeline**: `surfaceMm > biomeTreelineMm(tempU8)`
   -> `TUNDRA_ALPINE`. Treeline is 2600m at the reference temperature
   (tempU8==128, the synthetic-tile "average"), +/-20m per tempU8 unit,
   clamped so it never dips below the coastal band — very cold climates
   push it down toward sea level, so flat arctic tundra is reachable
   without needing extreme slope.
4. **Whittaker temperature x precipitation table** (only once every gate
   above has passed): cold (`tempU8 < 70`) -> `TAIGA` outright; otherwise
   arid/semi-arid/moderate/wet precipitation bands combined with
   warm/hot temperature bands pick among `GRASSLAND`, `DESERT`, `SAVANNA`,
   `TEMPERATE_FOREST`, `RAINFOREST` — `seasonalityU8 >= 128` is the
   deciding split between `SAVANNA` and `GRASSLAND` (semi-arid band) and
   between `SAVANNA` and `TEMPERATE_FOREST` (moderate-precip band), so the
   same seasonality signal splits both savanna/grassland and forest type
   as scoped.

`biomeSurfaceMaterial(BiomeId, surfaceMm)` maps each biome to its topsoil
material 1:1, except `TUNDRA_ALPINE`, which the function can't tell apart
from its two different gate origins (cold-flat-tundra vs. any-climate
steep-cliff) with only `surfaceMm` as a signal — documented as a known
round-1 simplification: high elevation reads as `MAT_ROCK`, everything
else as `MAT_PERMAFROST`, so a steep low-elevation cliff currently reads
as permafrost rather than bare rock. Revisit with a dedicated rock-face
biome or a slope-aware material picker once real diffusion tiles make
this matter more.

**Wiring**: `Amplifier::column` (amplifier.cpp) calls `classifyBiome` +
`biomeSurfaceMaterial` in place of the old ad-hoc v0 thresholds, feeding
the climate sample, the already-computed `surfaceMm`, and the
already-computed `slopeMmPerPx` — no new inputs, no `ColumnSample`/
`GpuColumnSample` layout change (BiomeId is used internally only, per the
"prefer not adding it to the struct" guidance). `worldgen.hlsl`'s
`ColumnMain` mirrors `classifyBiome`/`biomeTreelineMm`/
`biomeSurfaceMaterial` line-for-line in HLSL (same constants, same gate
order) and calls it in the same place the old inline material logic sat.

**kWorldGenVersion 1->2** (world-breaking: surface material selection
changed for every column). Regenerated goldens:
`amplifier_golden_digest` (test_amplifier.cpp) `0xA7CFA118B16CE0DF ->
0x73B43CAE621CA286`; `mips_chain_determinism_golden` (test_mips.cpp, its
brick generation depends on `Amplifier::column`) `0xACC109F9B1A5AD25 ->
0xE4CF1B376622A38F`; new `biome_map_golden_digest` (test_biome.cpp)
`0xEDBF3C9217ECBBF6`, a direct `classifyBiome`/`biomeSurfaceMaterial`
sweep (no amplifier/tiles involved) pinning the classifier table itself.
`test_hash.cpp` goldens are untouched (hash.h/value-noise primitives
didn't change).

**CPU/GPU mirror verified bit-exact** (`vxc_gpu`, AMD Radeon RX 7800 XT,
seed 20260719): column-only regions mode PASS, digest
`1dbcabb01cfaf2bc` (was `be28ce960bd5bcf6`); `--radius 64m` gate PASS,
144/144 tiles (100%) verified, digest `95a82ba20200f6f2` (was
`e1db29a9b6874012`), 0.104s (target <1s); `--radius 128m` gate PASS,
67/529 tiles (12.7%) sampled-verified, digest `b4c8ec5d0966894b` (was
`583e91d62cefb8a9`), 0.177s (target <1s) — gate timings essentially
unchanged from the pre-M4 baseline (biome classification adds only cheap
integer comparisons to `ColumnMain`, no new dispatches).

**Tests** (`voxel-core/tests/test_biome.cpp`, new; `test_amplifier.cpp`
updated): hot+dry -> `DESERT`; cold (below its lowered treeline) ->
`TAIGA`, cold+elevated (above it) -> `TUNDRA_ALPINE`; a climate that would
Whittaker-pick `RAINFOREST` at slope 0 flips to `TUNDRA_ALPINE` past the
cliff-slope threshold (morphology gate overriding climate, exact boundary
checked at the threshold value and threshold+1); wet+warm -> `RAINFOREST`;
seasonality splitting savanna/grassland and forest types (paired cases,
climate held fixed); coastal band boundaries (beach inclusive at both
edges, ocean just past the lower edge); treeline monotonic in temperature
and floor-clamped to the coastal band; every biome's surface-material
mapping; determinism; the golden digest above.

Build/gates (all HARD GATES from the M4 task met): `vxc_tests` 92/92 pass
(MSVC 14.51/VS 2026, Ninja/Release, `/W4 /WX`) and cross-checked with a
standalone LLVM-MinGW clang++ build (same source list, same goldens,
0 failures) — cross-compiler digest match confirms the classifier is pure
portable integer math as required. `vxc_gpu` PASS bit-exact (see above).
clang `-Wall -Wextra -Wconversion -Wsign-conversion` clean on every file
touched this wave (`core.h`, `biome.h`, `amplifier.cpp`, `amplifier.h`,
`test_amplifier.cpp`, `test_biome.cpp`, `test_mips.cpp`) — pre-existing
`-Wsign-conversion` warnings elsewhere in `brick.h`/`world.h` (not touched
this wave, not part of the standard build's flag set either) are
unrelated and left as-is. `voxel-core/shaders/worldgen.hlsl` compiles
clean to both DXIL and SPIR-V for all 7 kernels via `tools/compile-shaders.ps1`.
float-ban clean (integer types only in the new/changed `voxel-core/src`
and `voxel-core/include` code).

## Pending design (next session with Matt)

Round 2 — trees & structures: generation approach (procedural vs template
vs hybrid), tree scale policy, chop/fall interactivity timing (M5 tie-in),
forest density + distant-tree LOD strategy.
Round 3 — flora & placement: small flora voxel-vs-cosmetic doctrine call,
placement algorithms (hash scatter vs blue-noise vs grove/cluster logic),
rocks/boulders/surface features, riparian vegetation coupling to hydrology.
Also in M4 scope, undesigned: cave pass, voxel light field + cone-traced GI.

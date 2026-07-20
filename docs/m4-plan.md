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

## Pending design (next session with Matt)

Round 2 — trees & structures: generation approach (procedural vs template
vs hybrid), tree scale policy, chop/fall interactivity timing (M5 tie-in),
forest density + distant-tree LOD strategy.
Round 3 — flora & placement: small flora voxel-vs-cosmetic doctrine call,
placement algorithms (hash scatter vs blue-noise vs grove/cluster logic),
rocks/boulders/surface features, riparian vegetation coupling to hydrology.
Also in M4 scope, undesigned: cave pass, voxel light field + cone-traced GI.

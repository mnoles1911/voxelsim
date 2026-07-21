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

## Round 2 decisions (Matt, 2026-07-21) — vegetation DEFERRED, reframed

Matt's call: do NOT build procedural trees/vegetation this phase — good-looking
cubic-voxel trees need real inputs (reference, iteration in a populated world)
and getting them wrong looks worse than none. Decisions captured for when we do:

| Topic | Decision |
|---|---|
| This phase's tree scope | A SINGLE stand-in tree (source/adapt an existing cubic-voxel tree model online) used ONLY as a test fixture — NOT M4 generation. Its purpose is to exercise the CHOP → DISCONNECT → FALL pipeline. This is really M5 (destruction physics) validation, not M4 vegetation. |
| Tree generation (future) | Hybrid: hand-authored voxel archetypes per species + procedural per-instance variation (height/lean/branch/canopy jitter). |
| Small flora (future) | Hybrid: bushes = true voxels (choppable, edit-log world); grass/flowers = cheap cosmetic instanced (non-interactive). |
| Placement (future) | Grove/cluster logic — clustered noise for groves + clearings, density from biome+moisture; deterministic per-column hash. |

### Reframe → M5 physics work (Matt flagged as significant)
The stand-in tree becomes the concrete test case for the destruction physics
that M5 needs anyway: chopping/mining voxels → connectivity flood-fill (DONE,
voxelcore/connectivity.h) identifies the disconnected island → the island must
FALL and settle/degrade as debris. Matt explicitly marks the FALLING-VOXEL
handling (mined/chopped pieces that must fall, via Chaos rigid voxel debris
bodies per plan §3.5) as substantial physics-engine work. Sequence: (1)
stand-in tree fixture, (2) chop→connectivity→island detection wired in UE,
(3) island → Chaos debris body that falls + settles, (4) THEN return to M4
procedural vegetation once the destruction loop feels right.

## Pending design (next session with Matt)

Round 2 — trees & structures: generation approach (procedural vs template
vs hybrid), tree scale policy, chop/fall interactivity timing (M5 tie-in),
forest density + distant-tree LOD strategy.
Round 3 — flora & placement: small flora voxel-vs-cosmetic doctrine call,
placement algorithms (hash scatter vs blue-noise vs grove/cluster logic),
rocks/boulders/surface features, riparian vegetation coupling to hydrology.
Also in M4 scope, undesigned: cave pass, voxel light field + cone-traced GI.

# The pond decision: measured ladder for basin_min_depth_m

2026-08-29. The owner's ask: "pretty frequent standing pools of ponds and
lakes." The dryness knob is `basin_min_depth_m = 1.0`
(terrain-service/bake/pipeline.py:866), which culls 98.79% of raw depressions.
This page replaces the earlier power-law projection (~2.9x / +36%) with
measurement: six alpine tiles dumped at bake_version 28 (seed 20260719,
fingerprint 3b026df59fe0dbf9) and re-registered at four floors via
`lake_survey.py report` -- a free, offline sweep; nothing was rebaked.

## The ladder (6 tiles, 1,415 km^2; filter: area >= 100 m^2, spanning kept)

    floor    lakes   vs today   lake area   median depth   avg spacing
    1.0 m    2,049      --        987 ha       1.93 m         831 m   <- today
    0.75     2,641    x1.29     +6%            1.55           732
    0.5      3,477    x1.70    +12%            1.17           638     <- proposed
    0.25     5,239    x2.56    +20%            0.76           520

The projection overshot because the 100 m^2 area floor removes most of the
shallow bowls the power law counted. At the 0.5 m floor the median NEW pond
is ~1.2 m deep -- standing water, not film.

## The decision (owner's)

- **Floor 1.0 -> 0.5 m (recommended):** x1.70 ponds, +12% water area, one pond
  every ~640 m on average in alpine terrain. Rebake: ~3.3 CPU-h for the 15
  playable tiles; coarse tier untouched; ground bit-identical outside new
  bowls (the floor only gates basin REGISTRATION; it does not shape terrain).
- **0.25 m:** x2.56, but median 0.76 m approaches the terrain-noise floor --
  dimples begin to register as ponds.
- **Leave it:** today's world, zero cost.

## Provenance and caveats

- Dumps: D:/voxelsim/out/lake-survey-bv28 (tiles -3..-5 x -4..-5).
  Reports: D:/voxelsim/out/lake-sweep-bv28/d{0.25,0.5,0.75,1.0}/.
- The survey registry filter mimics the pipeline cull; the d1.0 row matched
  against today's shipped basin table is the calibration check.
- A survey does not survive a bake_version bump (roughness reseeds); re-dump
  after any bump.
- `lake_survey.py` DUMP_FILTER was lowered 1.0 -> 0.2 for these dumps
  (tool-local; rolls no bake identity, ships nothing).

## OUTCOME (2026-08-29, same day)

Shipped and owner-confirmed ("Water looks good. No issues"). Floor 0.5 m,
namespace -b5e821e98, all 15 tiles, verification tile matched this survey's
count to the digit (241). Revert: swap DefaultFineTileProviderId back to
-b19d281fd. See docs/SCOREBOARD.md and the pond-floor memory for the traps.

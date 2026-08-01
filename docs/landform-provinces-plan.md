# Landform provinces — process-varying bake rules

**Status:** design, not implemented. Written 2026-08-01 against `BAKE_VERSION` 6
and worldgen v21.

## The problem, stated as a player would state it

Two places with similar relief and similar climate currently come out looking
like siblings, because they were shaped by *identical physics*. The bake applies
one global set of constants everywhere; the only spatial variation is the
repose/material-strength field added at `bake_ver` 5, which varies how *hard*
the rock is but never *which process* shaped it.

Real landform character is mostly set by which process dominated. Iceland is not
Poland because of glaciation and volcanism, not because of a different erosion
rate. So the fix is to let the bake run different rules in different places.

The design goal is explicitly **more variety than Earth, arriving faster than on
Earth**: a player should be able to walk from rugged glaciated terrain to
subdued temperate forest in a session, not a continent. That is a deliberate
departure from realism and it is worth naming as one, because the diffusion
model is conditioned on real Earth data (ETOPO/WorldClim) and therefore
reproduces Earth's transition *scales* by construction. Provinces alone will not
fix pacing — see "What provinces do not solve".

## The load-bearing principle

**A province is derived from the terrain the model already produced, never
hashed independently of it.**

If a hashed province field says "glacial mountains" where the model produced a
flat coastal plain, the result is incoherent, and players forgive sameness far
more readily than they forgive nonsense. Deriving province from relief,
elevation and climate is also just what geomorphology says: cold + high relief
is glacial, wet + high relief is fluvial, dry is arid.

The exception is the genuinely non-climatic: lithology, volcanism, karst. Those
cannot be inferred from elevation and climate because they are properties of the
rock, not of the weather. Those get a hashed field — and because they are hashed
they are also the ones whose *spatial frequency we control*, which makes them
the lever for "faster variety than Earth".

## Taxonomy

Two families. Climate-derived provinces are a partition (every cell has exactly
one). Geological provinces are an overlay (usually absent).

### Climate-derived, from (relief, elevation, temperature, precipitation)

| province | discriminant | signature | primary bake consequence |
|---|---|---|---|
| `GLACIAL` | cold, high relief | U-valleys, cirques, overdeepened basins, hanging valleys | valley *widening* rather than incision; depressions allowed |
| `FLUVIAL` | temperate/wet, moderate-high relief | V-valleys, dendritic networks | today's pipeline, unchanged |
| `ARID` | low precipitation | pediments, sharp divides, badlands, internal drainage | high incision, near-zero creep, coarse drainage |
| `LOWLAND` | low relief, low elevation | subdued, alluvial, wide floodplains | heavy deposition, low incision |

### Geological overlay, hashed on a low-frequency lattice

| province | signature | primary bake consequence |
|---|---|---|
| `KARST` | closed depressions, **no surface drainage** | depression fill DISABLED — the sinks are the feature |
| `VOLCANIC` | cones, radial drainage, lava plains | additive construction, not only erosion |

## Architecture: province selects FIELDS, not constant sets

This is the decision that makes the whole thing safe, and it is worth being
explicit about because the obvious design is the wrong one.

**Obvious and wrong:** classify a tile as a province, look up a `CONSTANTS`
variant, bake the tile with it. This breaks at every province boundary — two
adjacent tiles baked with different constants disagree along their shared edge,
and the seam guarantee (a tile's interior equals the infinite-domain answer) is
gone.

**Right:** province is a *per-cell field*, and each province-varying constant
becomes a *per-cell parameter field* derived from it. Nothing is looked up per
tile; everything is evaluated per cell from a function of world position.

This preserves the seam guarantee exactly, because:

* the province field is a pure function of world position, computed from the
  coarse raster that the 960 m apron already covers, so two neighbouring tiles
  compute the identical value in their overlap;
* turning a constant into a field is **pointwise** — no pass gains any influence
  radius, so the apron argument is untouched;
* blending between provinces is then free: a smoothstep on the field, not a
  special case in the code.

The pattern already exists in the pipeline — `bake_ver` 5 turned the single
global repose angle into a spatially varying material-strength field for exactly
this reason, and the seam obligation it created is already tested
(`repose_field` world-anchoring tests).

### Which constants become fields

| constant | today | as a field |
|---|---|---|
| `repose_deg` / `repose_max_deg` | field already | province biases it |
| `channel_init_area_m2` (a_crit) | scalar 156 | drainage density per province |
| `profile_K_dt` | scalar | incision rate per province |
| `mfd_p` | scalar 1.1 | flow dispersal per province |
| `meso_amp15_m` | scalar 0.8 | meso relief amplitude per province |
| concavity `m`, `n` | scalars | province-specific slope-area law |

`thermal_iters` deliberately stays **global**. It is an iteration count, not a
per-cell quantity, and it is also a transport *distance* — talus moves roughly
one cell per iteration, so varying it spatially would vary runout in a way that
cannot blend. Vary the per-cell repose threshold instead, which is already a
field and already achieves the visual effect.

### Cost

Province classification is pointwise on the **coarse** raster (512², not 8192²)
and is therefore free relative to any bake stage. The parameter fields must NOT
be materialised at 8192²: the bake audit (2026-08-01) already identified
`regional` as a 576² field `np.repeat`-ed to 9216² wasting 340 MB, and the fix
there — index the coarse array with `//16` at point of use — is the pattern
every province field should follow from the start.

Expected added cost: **near zero for Tier 1**, since it replaces scalar reads
with coarse-array indexed reads inside loops that already exist.

## Implementation tiers

Deliberately ordered so that each tier ships value before the next is started,
and so the risky ones come last.

### Tier 1 — constants become province-derived fields

No new stages, no new flags, no change to pass structure. Buys the
arid / fluvial / lowland range: drainage density, incision rate, relief
amplitude and repose all varying with climate and relief.

This is most of the *visible* variety for a small fraction of the work, and it
is the only tier that is nearly free at runtime.

### Tier 2 — province gates existing stage behaviour

Booleans that cannot blend, so they need deliberate placement:

* `KARST`: skip B2a depression fill. This is a one-line gate on an existing
  stage and produces a landform class nothing else in the pipeline can make.
* `ARID`: internal drainage — allow basins to not reach the sea.

Because these cannot blend, their boundaries must be *placed like geological
contacts* (a lithology margin), not left to fall wherever a smooth field crosses
a threshold. A hard switch mid-slope is a visible seam, and it is the same class
of artifact that the worldgen v20 banding investigation existed to remove.

### Tier 3 — province adds stages

* `GLACIAL` valley widening: a genuinely different kernel (widening proportional
  to ice discharge rather than stream power). This is what produces U-shaped
  cross-sections; no amount of constant-tuning on a fluvial incision law will.
* `VOLCANIC` construction: additive rather than erosive.

Both add wall-clock to a bake that is already the binding constraint on
time-to-first-play (533 s/tile end to end). Neither should start until the bake
audit's items 1–4 have landed and the per-tile cost has come down.

## What provinces do NOT solve

**Pacing.** Provinces vary the terrain *given* the climate; they do not make the
climate change faster. The stated goal — Iceland to Eastern Europe in a session
— is mostly a *climate-gradient* problem, and the lever for it is spatial
compression of the model's conditioning (sample the conditioning fields at 2–4×
the rate so continental gradients arrive 2–4× sooner). Provinces then make each
climate zone *look* correspondingly different. The two are complementary and the
conditioning work should be evaluated first, because it is cheaper and it
determines how much province variety a player actually encounters.

The hashed geological provinces are the exception: their frequency is ours to
choose, so `VOLCANIC` and `KARST` regions can be tuned to ~50–100 km and give
encounter-rate variety independent of climate pacing.

## Validation

Provinces make the existing statistical validation *sharper*, not harder. Task
#14 already compares us against real Earth DTMs per terrain class; provinces
give each class a specific real-world analogue whose statistics are published
and measurably distinct — glacial and fluvial valleys differ in slope-area
concavity, cross-sectional form ratio and hypsometric integral.

Concretely, each province needs: a named real-world reference DTM, a target for
concavity θ, and a target ridge/peak fraction. A province whose output cannot be
distinguished statistically from `FLUVIAL` is not earning its complexity and
should be cut.

## Risks

1. **Incoherence at boundaries** is the failure players will notice first. Tier 1
   blends and is low risk; Tier 2's hard gates are where this bites.
2. **Cost regression.** The bake is already the binding constraint. Tier 3 must
   be costed before it is written, not after.
3. **Tuning surface explodes.** Six provinces × six constants is 36 numbers, and
   the project's own history is that hand-tuned terrain constants drift into
   artifacts nobody can trace. Every province constant should be derived from a
   real-world measurement and recorded with its source, the way the existing
   calibration work does.
4. **We currently cannot see half the climate space.** This seed reaches 7 of 10
   biomes and none of the dry ones, so `ARID` cannot be tuned or judged at all
   until there is a world containing it. That is an argument for doing the
   conditioning work and getting pod time BEFORE Tier 1, not after.

## Recommended order

1. Bake audit items 1–4 (bit-identical, ~350 s → ~230 s). Do not build on a
   known-slow bake.
2. Conditioning spatial compression — determines pacing, and determines whether
   province variety is actually encountered.
3. Generate a world that contains dry climate, so `ARID` can be judged.
4. Tier 1.
5. Re-measure, then decide whether Tier 2/3 earn their cost.

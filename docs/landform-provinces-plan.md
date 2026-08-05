# Landform provinces — process-varying bake rules

**Status, updated 2026-08-05:** Tier 1 **SHIPPED** at `BAKE_VERSION` 7
(commit `4f9a6e7`, 2026-08-01) — it is on `main`, not uncommitted as this line
used to say. Tiers 2 and 3 are still design. The body below was written against
`BAKE_VERSION` 6 and worldgen v21; **`BAKE_VERSION` has since advanced to 14**
(all water work — the ground did not move) and `kWorldGenVersion` to 23.

Current end-to-end state: `docs/world-generation-architecture.md` §6.5.

See "Tier 1 as built" at the bottom for what shipped, what was measured, and the
one finding that should change the recommended order.

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
climate change faster.

**CORRECTION, 2026-08-01.** An earlier revision of this document said the lever
for pacing was "spatial compression of the model's conditioning (sample the
conditioning fields at 2–4× the rate)". That was written from an unverified
inference — the model is conditioned on ETOPO/WorldClim, therefore it inherits
Earth's transition scales, therefore compressing the conditioning compresses the
scales. **Every step of that is wrong**, and it was measured rather than argued:

* **The conditioning is not Earth data.** `world_pipeline.py:676-682` builds it
  from Perlin fBm quantile-matched to Earth's MARGINAL HISTOGRAM
  (`synthetic_map.py:45-132`). Earth's rasters are read once to build 64-knot
  quantile tables; only the value distribution survives. Earth's spatial
  arrangement is discarded before inference ever runs.
* **The pipeline is already 100–1000× faster than Earth.** Measured E-W
  autocorrelation to 1/e: Earth's temperature never decorrelates within
  17,500 km (latitude dominates) and its precipitation takes 2,285 km; the
  shipping sketch decorrelates at 20.6 km and 18.3 km respectively.
* **And it is saturated.** Median |ΔT| over a 31 km walk is 12.0 °C; over 100 km
  it is 12.3 °C. The full swing already arrives by ~30 km, and biome persistence
  reaches its floor by 61 km. Further compression only moves the sub-15 km end,
  which is below one coarse cell (7.68 km).
* **There is a hard aliasing ceiling anyway.** The sketch is sampled at exactly
  the resolution the coarse model generates, so `frequency_mult` has honest
  headroom of 3 → 4. That is 1.3×, not 2–4×.

So pacing is NOT the deficiency. If mechanical compression is ever wanted
regardless, the right knob is `coarse_pooling` (pools the coarse model's OUTPUT,
so the model stays in-distribution) rather than `frequency_mult` (feeds
compressed gradients to a model trained on ~130 km crops). Both are already
`WorldShapeConfig` fields and already hashed into `provider_id`; neither has
ever been run in this project.

**The actual deficiency is contrast and coverage, not rate.**
`finalize_synthetic_map` couples temperature to elevation through a real lapse
rate but never couples precipitation to anything — there is no orographic
rainfall, no rain shadow, no continentality. So **no spatial scale and no
compression factor can produce an arid interior**: the sketch classifies to
DESERT 1.84% and SAVANNA 0.00%. That is why three biomes are unreachable in the
shipped seed, and it is structural rather than unlucky — a different seed does
not fix it. The fix is conditioning contrast, for which `tools/make_conditioning.py`
and the custom-GeoTIFF path (`world_pipeline.py:779-819`) already exist.

> **FIXED 2026-08-01 — this paragraph describes a world we no longer generate.**
> Precipitation **is** coupled to terrain now: an orographic rain-shadow pass in
> `finalize_synthetic_map` (the code is in
> `terrain-service/patches/terrain-diffusion-worldgen.patch`; parameters at
> `providers/diffusion.py:549-566`). Correlation between the upwind barrier and
> the rainfall multiplier is **−0.734**; the mean multiplier is **0.493 behind a
> barrier over 600 m** against **1.393** with none. There is still no
> continentality and the wind is a single fixed global bearing.
>
> The conditioning statistics were also rebuilt from the real WorldClim rasters
> — the previous file had used hand-written latitude formulas substituted when
> WorldClim was unreachable. The shipped world now measures **DESERT 9.74% and
> RAINFOREST 4.73%** over 289 tiles, with all eight mappable biomes non-zero. So
> `ARID` is tunable and judgeable today. See
> `docs/world-generation-architecture.md` §6.1–6.2.

This matters for provinces directly: `ARID` cannot be tuned, judged, or even
encountered until the conditioning produces dry climate at all.

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

Revised 2026-08-01 after the conditioning measurement above.

1. **Bake audit.** Item 1 landed: 350 s → 143 s wall, byte-identical. Remaining
   items in flight. Do not build on a known-slow bake.
2. **Run `terrain-service/tools/world_map.py` at the shipping default.** ~35 s on
   CPU torch, no pod. It runs the COARSE stage only — latent and decoder are lazy
   InfiniteTensors never touched — and its `classify()` mirrors
   `vxc::classifyBiome` by reading thresholds out of `biome.h` directly, so it
   cannot drift from the client. Compare the delivered coarse world's
   biome-transition distances against the sketch measurements above.
   * If they MATCH, variety is already at target and any remaining monotony is
     downstream in voxelsim's rendering or materials, not in generation. That
     would redirect this whole effort.
   * If the delivered world is much SMOOTHER than the sketch, the coarse model is
     the smoother, and `cond_snr` is the suspect rather than any spatial knob.
   Blocked only on `terrain_diffusion` not being installed on this box; torch is
   (CPU build).
3. **Conditioning contrast**, not compression: orographic rainfall / rain shadow
   so that arid interiors can exist at all. Until this lands, `ARID` is
   untunable and three biomes stay unreachable on every seed.
4. **Tier 1 provinces.**
5. Re-measure, then decide whether Tier 2/3 earn their cost.

## Tier 1 as built (2026-08-01, `BAKE_VERSION` 7)

### What shipped

`terrain_service/bake/province.py`. Four climate-derived provinces as a soft
partition on the **576² padded coarse grid**, consumed by `//16` indexing at the
point of use — never `np.repeat`-ed to 9216², which would have cost 340 MB per
field inside the bake's peak stage. All six parameter fields together measure
**7.96 MB**.

Six constants became per-cell fields, in the order the plan's cost argument
predicted:

| constant | how | cost |
|---|---|---|
| `profile_K_dt` | `incise.profile_incision(K_dt=field, field_scale=16)` | free |
| `channel_init_area_m2`, `channel_init_q`, `stream_m` | same elementwise `kfac` block | free |
| `meso_amp15_m` / `meso_amp11_m` | `noise.meso_relief(amp_scale=16)` | free |

Climate is plumbed: `bake_padded_domain` now takes `padded_climate`, gathered
over the same 3×3 ring by `assemble_padded_climate`, and `pregen._coarse_planes`
stops discarding the tile's `(4, 512, 512)` uint8 plane. `ClimateFetch` is a
**separate** callable from `CoarseFetch` on purpose — `CoarseFetch`'s return is
digested byte-for-byte by `superblock_inputs_fingerprint`, and the hydrology
pyramid has no business knowing climate exists.

Not done, deliberately: `stream_n` and `incision_cap_m` are scalars *inside* the
numba Newton kernel, so per-cell is a real kernel change. `mfd_p` is cut — it
drives the superblock MFD at 30 m and 120 m/px and is hashed as a scalar into
`superblock_inputs_fingerprint`, so a per-cell value at 1.875 m would make
parent and child route differently, worsening `HYDROLOGY_RESIDUALS` #3.

### The Tier 1 premise held

`kfac = K_dt * A^m * regional * erodibility * gate * taper` is fully
elementwise, so a per-cell `K_dt` **is** the same arithmetic as folding that
field into the array already passed as `erodibility=`. Verified to 3e-5 m — a
thousandth of the 100 mm wire LSB, the residue of float32 multiplication not
being associative. `tests/test_province.py` keeps that assertion, because if it
ever stops holding the tier's whole cost estimate is wrong.

### What was measured, and the bad news

Method: one fixed synthetic coarse domain, 128² coarse → 2048² fine at 1.875 m,
baked once per province with that province's multipliers applied globally, so
the terrain is identical and only the constants move. Metrics are the four that
`docs/geomorph-validation.md` records as *not* blind. The denominator is the
within-scene spread across four quadrants of the FLUVIAL bake.

**Drainage invariants hold: 0 interior sinks in every bake, scalar and field
path alike.**

**No province clears three within-scene sigmas.** Best is `ARID` on `p99_deg`
at **0.52×** the bar; `GLACIAL` peaks at 0.17×.

| vs FLUVIAL | `dd_km_per_km2` | `hurst_overall` | `frac_above_repose` | `p99_deg` |
|---|---|---|---|---|
| GLACIAL | 0.965× (0.07σ₃) | 1.007× (0.17σ₃) | 0.946× (0.11σ₃) | 0.990× (0.09σ₃) |
| ARID | 1.131× (0.24σ₃) | 0.983× (0.39σ₃) | 1.141× (0.27σ₃) | 1.054× (**0.52σ₃**) |
| LOWLAND | 1.135× (0.25σ₃) | 0.996× (0.11σ₃) | 1.059× (0.11σ₃) | 1.017× (0.16σ₃) |

A single-knob sweep says this is a **calibration gap, not a mechanism gap, but
only barely**: pushing `channel_init_area_m2` to ×256 moves `dd_km_per_km2` from
16.6 to 9.3 — a 44% move, and into the real-world range of 2.4–10.7 — yet still
only 0.82× the bar. `meso_amp` ×4 reaches 0.68× on `hurst_overall`. Nothing
tested reaches 1.0× alone.

Two caveats on the denominator, both pointing the same way:

* the domain is a random fBm, whose four quadrants genuinely differ a lot
  (`dd` σ is 18% of its mean), so this within-scene σ is probably **pessimistic**
  against the doc's cross-scene within-class σ;
* `rasterio` is not installed on this box, so the real exemplar windows could
  not be fetched. **Re-run this on `tools/geomorph_validate.py --sweep bake`
  before concluding anything about the table.**

The first-cut multipliers are recorded in `province.PROVINCE_MULTIPLIERS` with
their reasoning, and they are the least-supported part of this change — exactly
the risk #3 the plan already named. They were deliberately *not* tuned upward to
hit a statistical bar on synthetic terrain, because that is the "hand-tuned
constants drift into artifacts nobody can trace" failure mode. The anchor that
should drive the next pass is real and already in the validation doc:
`channel_head_area_m2` spans **866–8,660 m² across five real terrain classes**,
a 10× contrast, against the 2.5–4× this table currently uses.

### The finding that should change the recommended order

Decoding the shipped fixture tile (`voxel-core/tests/fixtures/tile_s1_seed1_0_0.vxtl`,
the full 1,572,889-byte form) and running the production ring gather on it:

```
temperature          3.29 .. 31.53 C
precipitation     3905.88 .. 5788.24 mm/yr
province mix   fluvial 0.9181   lowland 0.0814   glacial 0.0004   arid 0.0000
```

Precipitation never drops below **3,900 mm/yr anywhere on the tile**. `ARID`'s
weight is not small, it is *zero*, and `GLACIAL`'s is 4e-4. So on the shipped
world Tier 1 delivers FLUVIAL plus a little LOWLAND and essentially nothing else
— which is the plan's own "we currently cannot see half the climate space",
now confirmed at the bake's own input rather than inferred from the biome
classifier.

**Step 3 (conditioning contrast) is therefore a hard prerequisite for judging
Tier 1, not a parallel track.** The machinery is in and tested; two of its four
provinces cannot be encountered, tuned, or judged until the world contains dry
and cold climate at all.

### What is NOT claimed

Not a seam guarantee. `pipeline.APRON_BLIND_SPOT` measured that guarantee
already violated — 1.05% of the shipped interior moving past the 100 mm wire LSB
by up to 78.79 m, with the domain border's influence reaching 3.8 km inward,
because the depression fill is unbounded and a truncated domain invents an
outlet. Province fields neither worsen it nor repair it. The narrower claim that
*is* made and tested: province adds no influence radius beyond its own landform
smooth, which `apron_coarse_px // 4` clamps to at most half the apron on any
geometry.

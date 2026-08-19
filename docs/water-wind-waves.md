# Wind-driven waves and shore breaking

The lake's surface now responds to wind: stronger wind builds taller, longer,
faster waves, they line up with the wind instead of coming from every direction
at once, and where the bed rises under them they break into foam and die at the
waterline.

All of it is a **surface** effect — displacement and shading on the drawn water
surface. Nothing here touches the water simulation. The code is
`ue-project/Tools/water_wave_graph.py`, imported by the water material
generator; `docs/water-architecture.md` remains the architecture entry point.

Status: **LIVE — reviewed 2026-08-19.** It shipped and was rendered on
2026-08-13 (`f2ea97c`, "wind-driven waves and interactive ripples, live") and has
since been tuned against the owner's eye twice: the ripple system turned out
never to have been broken (`c031b03`), and the crest speed the owner kept
rejecting was the **wave-inertia filter**, not the wave maths (`a3a53d3`).
Original status, and the caveat that still applies to the numbers below: built
and compile-checked, not yet rendered — every number below comes from an offline
numpy transcription of the shader maths, not from a frame, so treat them as the
design intent and the frame as the authority. One open question remains in
`docs/backlog.md` §9.2: crest speed at a *frozen* wind has never been judged on
its own.

---

## Why the fast waves are not in the simulation

The owner's suggestion was to drive a target height in the water sim. The split
taken instead — fast waves on the surface, slow level changes in the sim — rests
on three numbers rather than a preference:

- A 20 cm wind chop is **two voxels** on a 10 cm grid, and the CA's fill
  fraction quantises it again on top of that. It would read as a two-step
  staircase flipping on and off. The surface path has no such floor: World
  Position Offset moves a vertex by a float, and the shipped displacement is
  ±2.1 cm — a fifth of a voxel.
- Running it through the CA means touching every water cell every frame across
  **2,049 basins**, to produce something no cell can represent.
- The surface field is evaluated only where it is seen: on water pixels and
  vertices, at the shading rate, free for the submerged volume and for every
  basin off screen.

**What does belong in the sim, and the hook for it.** A slow, basin-wide change
to the water *level* — tide, seiche, storm surge — moves the waterline and wets
new ground, and no surface effect can do that. The clean site is `SurfaceZUU`,
the per-basin drawn water surface (`VoxelWaterSubsystem.h:486`, `:633`). The
sheet's hole test and the basin extent masks already take it as an argument
rather than reading a constant, so a slow offset applied there would move the
drawn surface, the wet extents and the despawn masks together. Not designed
here; flagged because the site is unusually clean.

---

## The wind interface

Two parameters on `MPC_VoxelSky`, to be published by the weather subsystem.

| name | kind | meaning |
|---|---|---|
| `WindVectorMS` | vector | R = wind velocity along world **+X** (north), m/s. G = velocity along world **+Y** (east), m/s. B reserved for a gust factor (unread). A unused. Default `(0,0,0,0)` = dead calm. |
| `WindFieldValid` | scalar | 1 once the weather field has published this run, 0 otherwise. Default 0. |

**One vector, not a direction and a speed.** Two parameters can disagree — a
stale unit vector beside a speed that moved is a wind blowing the wrong way,
with nothing in a frame to report it. A velocity cannot be inconsistent with
itself, and it makes "calm" exactly one value rather than a special case someone
has to remember.

**It is a velocity: it points where the air is going.** Meteorology names winds
by where they come *from*, so a 6 m/s "northerly" is `WindVectorMS = (-6, 0)`.
Waves travel along `+WindVectorMS`. This is the classic 180° bug and it is the
one thing in this table that must be confirmed rather than assumed.

**Reference height is the standard 10 m (U10).** Every growth relation below is
written for U10 and the defaults are calibrated against it. If the field
publishes a surface-level or boundary-layer wind instead, the calibration moves;
it does not silently absorb.

**Why `MPC_VoxelSky` and not a new collection.** The far-field lake sheet has no
dynamic material instance, so an MPC is the only CPU→material channel reaching
both water draw paths. The water material already binds six parameters there. A
second collection would be a second asset that can be regenerated out from under
a dependent — a failure this project paid for on 2026-08-10.

**It runs today without any of this.** If the collection has no wind parameters,
the builder logs and falls back to two material parameters
(`WindFallbackSpeedMS` 5.0, `WindFallbackDirDeg` 238.7) whose defaults reproduce
the shipped look. The fallback is also the runtime path whenever
`WindFieldValid` is 0, so a session with weather disabled gets today's lake
rather than a glassy one.

---

## The model

### Wind sizes the waves — fetch-limited, not fully developed

Pierson–Moskowitz is the famous relation and it is the wrong one here. It
describes a *fully developed* sea and gives, at 5 m/s, a 0.61 m wave 20.8 m
long: taller than the shallows and longer than a tenth of the basin.

The right family is fetch-limited JONSWAP, which includes the distance the wind
has blown over water:

```
H_s   = 1.6e-3 · U · sqrt(F/g)
f_p   = 3.5 · (g/U) · (gF/U²)^(-0.33)
lam_p = g / (2π f_p²)
```

At U = 5 m/s over F = 500 m: **H_s = 5.7 cm**, lam_p = 1.1 m. That is a lake.

**The shipped water already was one.** `WaveAmplitudeM = 0.25` was set by eye
against a slope measurement with no reference to wind. Put through the standard
H_s = 4σ it comes out at **6.0 cm** — 5% from the JONSWAP figure. Inverting the
relation says the shipped lake is *a 5 m/s breeze over 552 m of fetch*. That is
why `WindRefSpeedMS = 5.0`: it is not a chosen pivot, it is what the water
already is, measured after the fact.

Fetch is not available in the material (the baked field gives distance to the
*nearest* shore, not the distance *upwind*, and a per-pixel ray march is not
affordable), so fetch folds into that calibration and only the scaling with wind
speed is kept:

- amplitude ∝ U^1.00 (`WindAmpExponent`)
- wavelength ∝ U^0.68 (`WindLenExponent`)
- angular frequency ∝ 1/sqrt(λ), so crest speed ∝ sqrt(λ) — deep-water
  dispersion, free, and exactly 1.0 at the reference speed

**One honest caveat.** The amplitude matches the physics; the *wavelengths do
not*. JONSWAP puts the spectral peak at 1.15 m for that wind and fetch; the
shipped base octave is 5.0 m, about 4× too long. That was an artistic choice and
it is kept — this module supplies the scaling, not a re-tuning. It does not look
wrong because the *speed* is right: the shipped crest speed is 1.59 m/s and the
physically correct speed for a 1.15 m wave is 1.34 m/s. The lake moves at lake
speed even though its crests are spaced further apart than a real one's.

### Wind steers the waves — a cone, not a line and not a circle

The shipped field spreads eight octaves over the whole circle at the golden
angle. That was the right fix for corduroy, but as a wind sea it is wrong in a
specific way: it is a *confused* sea, with as much energy travelling upwind as
down.

Octave *i* is now placed at

```
angle_i = baseAngle + lerp(i·DirIncrementDeg,
                           WindSpreadDeg · SPREAD_SHAPE[i],
                           WindDirectionAuthority)
```

`SPREAD_SHAPE` is a golden-ratio low-discrepancy sequence in [−1, +1], sampled
at `(i + 0.5)·φ` so that **no octave lands exactly on the wind axis**, scaled by
a width that ramps 0.5× → 1.5× from the longest octave to the shortest. The ramp
is Mitsuyasu's observation that directional spread is narrow at the spectral
peak and broad in the tail — long heavy octaves line up with the wind, short
ones fan out — and it is also the insurance against eight nearly parallel
octaves being corduroy again.

At `WindSpreadDeg = 35` the offsets are

```
-6.7, +19.2, +2.5, -21.9, +21.1, -8.6, -45.9, +14.2   degrees
```

`WindDirectionAuthority` blends whole-circle (0) against the wind cone (1). It
is a lerp rather than a switch so it doubles as the A/B knob on a material
instance.

**The waves travel downwind, and the sign is spelled out in the code.** In
`x = dot(d,pos)·freq + T·tmul` a crest travels along **−d**, so the wind target
angle is the wind bearing plus 180°, applied as `atan2(-w.x, -w.y)`.

### Calm and choppy patches

Already shipped: two diagonal sines at ~180 m and ~232 m, non-commensurate with
each other and with every octave. Kept verbatim. One thing added — the patch
field is **advected by the wind**, so gust cells drift downwind instead of
breathing in place. One multiply-add. `WindPatchDriftFrac` defaults to 0.5; at
1.0 a 10 m/s wind sweeps the whole patch structure past a fixed viewpoint in
about 20 s, which reads as the lake sliding rather than the wind gusting.

---

## Breaking

**The criterion** is McCowan's solitary-wave breaker index, H/d = 0.78, i.e. the
wave breaks once depth falls below **1.28 × H** (`BreakDepthRatio`). H is the
significant wave height, H_s = 4σ of the field. Nothing here integrates a
shoaling wave properly and nothing needs to.

### The collision with the existing shore damping, and two wrong answers

The material already damps the wave to zero over the first 0.6 m of depth. The
break depth for the shipped wave is 7.7 cm. The two are a **factor of eight**
apart, and the damping wins: a wave arrives where it should break with **16%** of
its height left. There is almost nothing there to break.

1. **Collapse them into one band** (`dSurf = max(1.28·H_s, 0.6)`, breaking as
   the complement). Elegant, wrong twice: at ordinary wave heights the band is
   set by the 0.6 m floor, so it foams the whole 0.6 m band whenever there is
   any wave — measured, a 3 m/s breeze foamed out to 46 cm of depth, the
   permanent white ring the gates exist to prevent. And it foams water the wave
   is not breaking in.
2. **Nest them**, foam off the inner band only. Fixes the ring, leaves the
   original defect untouched. Peak crest height inside the foamed region, 2-D
   shore, four phases:

   | `dSurf` floor | 5 m/s | 12 m/s | amplitude at the break line |
   |---|---|---|---|
   | 0.6 m (this attempt) | 1.22 cm | 5.62 cm | 16% / 47% of offshore |
   | 0.15 m (shipped) | 4.88 cm | 12.95 cm | 65% / 100% of offshore |

   A correct-looking foam mask over water with a quarter of its wave in it.

**The damping band was the wrong size.** A real surf zone runs *from* the break
point *to* the waterline; the wave keeps its height until it breaks, then
dissipates. So they are the same band:

```
dBreak = BreakDepthRatio · H_s               the wave breaks here
dSurf  = max(dBreak, BreakSurfFloorM)        the wave dies over this

shore damping = saturate(depth / dSurf)
breaking      = 1 − saturate(depth / dBreak)
```

`BreakSurfFloorM` (0.15 m, 1.5 voxels) is a floor for tiny waves only, so a
near-flat lake still fades in rather than stopping dead at the shoreline. Above
about 12 cm of wave the two coincide and breaking is the literal complement of
the damping.

**The band is proportional to the wave**, which is what makes it behave at both
ends. Zero wind → H_s exactly 0 → no surf zone at all, no ring. 20 m/s → H_s
24 cm, surf reaching 31 cm of depth, which on a 1-in-20 beach is a six-metre
band of white. The surf zone widens with the wind, free.

### It hands back a guarantee that used to be a guess

Inside the surf zone a linear ramp on a wave-proportional band makes the wave
**depth-limited** at any wind speed — which is what a real surf zone does, and
why a beach looks the same in a gale as in a breeze except wider. Crest height
as a fraction of local depth, measured over a 0.8 m → 0 m shore at four phases:

| | 2 m/s | 5 m/s | 10 m/s | 20 m/s |
|---|---|---|---|---|
| p99 | 0.17 | 0.50 | 0.75 | 0.75 |
| max | 0.28 | 0.75 | 1.22 | 1.22 |
| **geometry** (×0.25 WPO) | 0.07 | 0.19 | **0.30** | **0.30** |

The vertex displacement never exceeds about a third of the water it is standing
in, at any wind speed, and stops growing above 10 m/s because the depth limit
takes over. That is the concern that pinned `WaveWpoFraction` at 0.25 in the
first place, answered with a bound instead of a small number. **Not raised
here** — that is a look change, and the owner judges those from captures.

The shading field's rarest crests do exceed the local depth (max 1.22). That is
a normal and a colour, not geometry; it cannot clip anything, and clamping it
would flatten exactly the crests a breaking wave should have.

### Three gates on the foam

A permanent white ring around a still lake is the most recognisable tell of a
tutorial water shader, and this is one line away from drawing one.

1. **Size** — `saturate(H_s / BreakMinWaveHeightM)`, 5 cm. An absolute floor
   under the proportional band; belt and braces.
2. **Crest** — foam rides the crests of the field, not the whole band, so it
   *travels with them*. The threshold relaxes to nothing at the waterline, where
   swash really is solid foam.
3. **Validity** — multiplied by the bathymetry field's validity, like every
   other consumer. No baked depth, no breaking, and the water looks exactly as
   it does today.

### The peaked shape

A shoaling wave pitches forward: the crest sharpens and rises, the trough stays
broad. Applied as `f(H) = H + k·max(H,0)²/peak` with `k = BreakPeakGain ·
breaking`, which lifts crests and leaves troughs alone — and has an **analytic
derivative**, `f'(H) = 1 + 2k·max(H,0)/peak`, applied to the gradient in the same
breath so the normal and the displacement stay two outputs of one field.

Measured inside the surf zone, 2-D shore averaged over four phases: crest height
**+9 to +11%**, and gradient magnitude **×1.50 to ×1.83** — up to an 83% steeper
crest face, which is where the effect is actually seen.

The exact gradient would also carry `dk/dp`, the spatial variation of the
breaking signal. It is omitted, for the same reason the patch field's derivative
already is: breaking varies over metres, the wave over tens of centimetres.

---

## Measured numbers

Offline numpy transcription of both the shipped HLSL and this one, at the
world's real ~84 km offset. Not a frame.

### Regression against the shipped field

With `WindDirectionAuthority = 0`, wind speed = `WindRefSpeedMS`,
`BreakShoalGain = BreakFoamGain = BreakPeakGain = 0`, `BreakSurfFloorM = 0.6`
(this is `LEGACY_RECIPE` in the module):

- max |ΔH| = **5.1e-11 m**, max |ΔG| = **8.6e-10** — float64 rounding from
  re-associating the angle accumulation into a lerp. The same field.
- tilt p50/p95/p99/max identical to two decimals: 2.91 / 5.98 / 7.36 / 11.24°.

Any future change shows up as a diff of the *wave*, against a baseline that is a
parameter set rather than a git revision of a 2,964-line generator.

### The wind ladder

Deep water, `WavePatchContrast` held at 0 so the rungs are comparable (the wind
advects the patch field, so at the shipped 0.55 each rung samples a different
patch and the amplitude swings ±55% on top of these). Averaged over four phases,
120 m × 120 m at 900².

| U (m/s) | λ₀ (m) | H_s (cm) | crest (cm) | tilt p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| 0 | — | 0.0 | 0.00 | 0.00° | 0.00° | 0.00° | 0.00° |
| 1 | 1.67 | 1.2 | 1.24 | 1.59° | 3.71° | 4.70° | 7.59° |
| 2 | 2.68 | 2.4 | 2.46 | 1.98° | 4.62° | 5.87° | 9.50° |
| **5** | **5.00** | **6.0** | **6.19** | **2.65°** | **6.19°** | **7.84°** | **12.59°** |
| 8 | 6.88 | 9.6 | 9.75 | 3.08° | 7.18° | 9.10° | 14.80° |
| 12 | 9.07 | 14.3 | 14.82 | 3.51° | 8.16° | 10.33° | 16.37° |
| 20 | 12.83 | 23.9 | 24.32 | 4.13° | 9.59° | 12.12° | 19.03° |

The 5 m/s row is the shipped field by construction. `H_s` from the model and
`H_s` measured as 4σ agree to the printed precision at every rung, which is the
check that the amplitude scaling is doing what it claims.

Tilt grows more slowly than amplitude because wavelength grows too — steepness
goes as U^0.32, which is the fetch-limited answer and is why a storm lake is
*bigger* rather than merely *steeper*.

Vertex displacement (`WaveWpoFraction` 0.25, patch 0; multiply by up to 1.55 in
the choppiest patch):

| U (m/s) | max | p99 |
|---|---|---|
| 0 | 0.00 cm | 0.00 cm |
| 5 | 1.53 cm | 0.91 cm |
| 12 | 3.59 cm | 2.18 cm |
| 20 | 5.93 cm | 3.61 cm |

### Octave directions

| case | nearest world axis |
|---|---|
| shipped / legacy (authority 0) | 16.2° |
| wind at 238.7° (the fallback default) | 10.2° |
| wind due east or due north | **2.5°** |

The last row is the honest cost of steering by wind: when the wind is
axis-aligned, the wavefronts are too. That is *correct* — real waves align to
real wind — but it means the anti-axis property the shipped field bought with
`WaveDirBaseDeg = 58.7` is now a property of the weather, not of the material.
The mitigation is that no octave sits exactly on the wind axis (the +0.5 offset
in the spread sequence), so the worst case is 2.5° rather than 0°.

### Anti-tiling

Best self-similarity of the gradient's X component over a 400 m slice, lags
0.5–200 m (higher is more tile-like):

| | best | at lag |
|---|---|---|
| shipped / legacy | 0.681 | 17.1 m |
| wind 238.7°, 5 m/s | 0.647 | 11.2 m |
| wind 238.7°, 15 m/s | 0.736 | 23.7 m |

The wind cone does not make the field more repetitive at moderate wind and is
mildly worse at 15 m/s, where the wavelengths are longer and a 400 m slice holds
fewer periods. Note this metric is computed differently from the 0.58-at-9.2 m
figure quoted in `create_water_voxel_material.py`; the three rows here are
comparable to each other and not to that one.

### Breaking

2-D shore, 16 m across x (depth 0.8 m → 0) by 40 m along y, 320×800, averaged
over four phases. `WavePatchContrast` is left at its shipped 0.55 here and the
wind advects the patch field, so the offshore H_s below is **not** the ladder's
(which forces contrast to 0). That is the patch field working, not a
disagreement between the two sections.

| U (m/s) | H_s offshore | break depth | foam >0.5 | crest +peak | crest −peak | grad gain |
|---|---|---|---|---|---|---|
| 0 | 0.0 cm | 0.001 m | 0.0% | 0.00 cm | 0.00 cm | ×1.00 |
| 2 | 2.9 cm | 0.037 m | 0.6% | 0.42 cm | 0.38 cm | ×1.50 (+9%) |
| 5 | 7.5 cm | 0.096 m | 3.6% | 4.88 cm | 4.41 cm | ×1.70 (+11%) |
| 10 | 16.2 cm | 0.207 m | 7.8% | 9.65 cm | 8.69 cm | ×1.83 (+11%) |
| 20 | 32.3 cm | 0.414 m | 15.2% | 18.29 cm | 16.60 cm | ×1.78 (+10%) |

The two bands, confirming the design: floored by `BreakSurfFloorM` at 0/2/5 m/s
(0.150 m against break depths of 0.001/0.033/0.098 m) and **coinciding exactly**
at 12 m/s (0.282/0.282) and 20 m/s (0.440/0.440).

### The probe defect that reported a working effect as dead

Recorded because it nearly shipped as "the peaking does nothing", and because the
failure mode is silent.

The first version of the breaking section sampled the shore as a single **1-D
line** — 601 points, depth 3 m → 0 across 30 m, y fixed. At 12 m/s that puts the
whole surf zone in 54 samples spanning 2.70 m of x, which is **0.30 of one
9.07 m wavelength**: a third of a wave at whatever phase the line happened to
cross. The `breaking > 0.2` mask then selects the samples nearest the
*waterline*, where the swash term takes foam to 1 whatever the crest is doing —
and in that strip every one of them was trough, H from −1.34 to −0.00 cm. So it
computed crest amplification over a set containing **no crests** and printed
×1.00 and a crest of negative zero. The fifteen samples that did carry a crest
sat further out, where breaking is 0.000–0.076, *below the mask*.

Two lessons, both cheap:

- **The fix was phases, not resolution.** A 1-D transect through a 2-D random
  field cannot resolve a phase-conditioned statistic however finely it is
  sampled. The 2-D shore samples every depth at ~800 phases instead of one.
- **It contaminated a recorded failure.** "Wrong answer 2 measured +0% peaking"
  came from this same broken sampling and was wrong twice over — the peaking
  ratio is amplitude-independent and measures ×1.70 under *both* configurations.
  The real defect there was the absolute crest, four-fold. The conclusion held;
  the evidence for it did not, and the wrong quantity would have sent the next
  person into the peaking maths instead of the band width. Both files are
  corrected.

---

## Cost

**Estimated, not measured**, and the distinction is not a formality: this was
authored while the editor was held by another job, so nothing here has been
through a shader compiler. The estimate is an operation count of the emitted
HLSL, which is what the compiler starts from and not what it finishes with.

| | instructions |
|---|---|
| shipped `WaveField` node | ~150 |
| + wind sizing (length, normalize, 2 pow, sqrt) | ~12 |
| + wind steering (atan2, round, 8 × lerp+mul) | ~30 |
| + patch advection | ~4 |
| + surf band and shoaling | ~12 |
| + breaking signal | ~14 |
| + crest peaking | ~8 |
| **estimated total** | **~230** |

Two things about that number. It is a pixel *and* vertex cost on water surfaces
only — the node feeds Normal and World Position Offset, so it runs once per
water vertex and once per water pixel, never for the submerged volume and never
for a basin off screen. And roughly 80 of the added instructions are **uniform
across the draw**: they depend only on material and collection parameters, not
on position, so a compiler that hoists uniform expressions should move most of
them out. Whether this one does is exactly the sort of claim this project does
not make without a measurement.

---

## Composing with the interactive ripple field

`ue-project/Tools/ripple_field_graph.py` is being built in parallel and produces
the same shape of thing: `dH/dx`, `dH/dy`, `H` in metres, from a camera-following
render target, for rings spreading from objects entering the water.

**The two compose by addition and nothing else is needed.** Surface gradients of
independent height fields sum — that is exactly why the wave field accumulates
gradients rather than blending normals, and the argument is already written down
at the normal assembly in `create_water_voxel_material.py`. So:

```
gradient = wave.gradient + ripple.gradient
height   = wave.height_m + ripple.height_m
```

before the `(-dH/dx, -dH/dy, 1)` normal assembly and before the WPO scale. Both
integrations land at the same two sites, so whoever applies the second one
should apply both adds at once rather than threading one through the other.

One thing to check when they meet: the ripple field is a 51.2 m camera window
and the wave field is unbounded, so the ripple must fade to zero at its window
edge or the sum will show the window. That is the ripple module's business, not
this one's, and its docstring says it is aware of it.

---

## Deliberately not done

- **No gust term.** `WindVectorMS.B` is reserved for it and unread.
- **No fetch query.** Folded into the calibration constant.
- **No per-octave distance fade.** Still the Phase 5 item it was.
- **No true dispersion in the per-octave time progression.** The 1.07 multiplier
  stays; the dispersion-correct value is 1.19. That is a look change and should
  be judged on its own capture.
- **No re-tuning of the wavelengths** to the physical spectral peak, though the
  measurement above says they are ~4× long.
- **No foam texture.** The breaking signal is a scalar for the material to route
  into the existing foam chain.
- **No change to the simulation**, and no tide. The hook is named at the top.
- **`WaveWpoFraction` not raised**, though the depth-limit bound now justifies
  considering it.

---

## Integration patch note for `create_water_voxel_material.py`

Line numbers are against the file as of this writing (2,964 lines). **The wave
field must be built EARLIER than it is today** — before the foam section —
because the breaking signal is now an input to foam and foam is composited at
line 1396, twelve hundred lines above where the wave node is created. Everything
else stays where it is.

### 1. Import (near line 441, beside the other Tools imports)

```python
from bathy_field_graph import build_slant_depth, sample_bathy_field  # noqa: E402
import water_wave_graph  # noqa: E402                                  <-- ADD
```

### 2. Move the wave field's *inputs and node* up, to just after line 808

Insert immediately after `bathy_weight = bathy_b.mul(bathy_authority, bathy["validity"])`.
This is the earliest point where `bathy` exists, and it is above the foam
section. The block below is **moved**, not duplicated — see step 4 for the
deletions.

```python
    # --- THE WAVE FIELD, BUILT EARLY BECAUSE FOAM READS IT -----------------
    #
    # It used to live down with the normal and the WPO. It has to be here now:
    # water_wave_graph's node also returns a BREAKING signal, and the foam
    # composite at the shoreline-foam section below is the consumer. Nothing
    # about the field changed by moving it -- it is a pure function of world
    # position and time, and its own outputs are still read where they always
    # were.
    #
    # ONE Time node still drives the whole field, so VOXEL_WATER_FREEZE_TIME is
    # still a single-node substitution -- and it now freezes the breaking foam
    # as well, by construction.
    if FREEZE_RIPPLE_TIME:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionConstant, -900, 950)
        ripple_time.set_editor_property("r", 0.0)
    else:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionTime, -900, 950)

    world_pos_abs = mel.create_material_expression(
        material, unreal.MaterialExpressionWorldPosition, -1300, 700)
    world_pos_abs.set_editor_property(
        "world_position_shader_offset",
        unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)

    world_xy = mel.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -1120, 700)
    world_xy.set_editor_property("r", True)
    world_xy.set_editor_property("g", True)
    world_xy.set_editor_property("b", False)
    world_xy.set_editor_property("a", False)
    if not mel.connect_material_expressions(world_pos_abs, "", world_xy, ""):
        raise RuntimeError("connect world_pos_abs -> world_xy failed")

    uu_to_m = mel.create_material_expression(
        material, unreal.MaterialExpressionConstant, -1120, 780)
    uu_to_m.set_editor_property("r", 0.01)
    wave_pos_m = mel.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -960, 720)
    if not mel.connect_material_expressions(world_xy, "", wave_pos_m, "A"):
        raise RuntimeError("connect world_xy -> wave_pos_m.A failed")
    if not mel.connect_material_expressions(uu_to_m, "", wave_pos_m, "B"):
        raise RuntimeError("connect uu_to_m -> wave_pos_m.B failed")

    wave_field = water_wave_graph.build_wave_field(
        bathy_b, wave_pos_m, ripple_time, bathy, log=unreal.log_warning)
    wave_grad_raw = wave_field["gradient"]     # float2 dH/dx, dH/dy
    wave_height_m = wave_field["height_m"]     # float, metres
    wave_breaking = wave_field["breaking"]     # float 0..1, foam
```

### 3. Feed the breaking signal into foam — one line, at line 1396

```python
    foam_raw = bathy_b.maximum(foam_raw, shore_foam)
    # SIGNAL 4: BREAKING WAVES. Same MAX as the other three and for the same
    # reason -- these describe one physical thing from four directions, and a
    # breaking wave over a foamy shore should be fully white rather than 2x
    # white and clipped. It is already gated three ways inside the wave node
    # (wave size, crest phase, bathymetry validity), so a settled or windless
    # lake contributes exactly zero here.
    foam_raw = bathy_b.maximum(foam_raw, wave_breaking)          # <-- ADD
```

### 4. Delete, in the old wave section

| lines | what goes |
|---|---|
| 2326–2354 | the Time / WorldPosition / mask / `uu_to_m` / `wave_pos_m` block — **moved** to step 2 |
| 2356–2478 | the knob block: `wave_amp`, `wave_base_len`, `wave_dir_inc`, `wave_dir_base`, `wave_quant`, `wave_patch` and their comments. `water_wave_graph` authors these parameters now, under the same names and the same defaults, so a material instance that overrides them keeps working. **KEEP `wave_wpo_fraction` (line 2399)** — the displacement fraction is applied outside the node and stays here. |
| 2480–2590 | the `WAVE_CODE` string and its comment block |
| 2592–2597 | the `wave = custom_node(...)` call |
| 2599–2620 | the Phase 3 shore-damping block (`wave_damp_depth`, `wave_damp_raw`, `wave_damp`, `wave_amp = bathy_b.mul(...)`). The node owns depth response now — see the design note for why the 0.6 m band was the wrong size. |
| 2621–2626 | the `for src, pin in (...)` wiring loop |
| 2645–2651 | the `wave_grad` mask nodes and their connect |
| 2701–2707 | the `wave_height_m` mask nodes and their connect |

Keep every comment in 2246–2325 (the "what this replaces and why" history) and
in 2628–2644 / 2652–2700 / 2708 onward (the normal assembly and the WPO). Those
are still accurate and they are the reasoning for what remains.

### 5. Repoint two references in the surviving code

`wave_grad` and `wave_height_m` were local node variables; they are now the
expressions returned in step 2.

```python
    # was: if not mel.connect_material_expressions(wave_grad, "", normal_xy_raw, "A")
    if not mel.connect_material_expressions(wave_grad_raw, "", normal_xy_raw, "A"):
        raise RuntimeError("connect wave_grad -> normal_xy_raw.A failed")
```

`wave_height_m` keeps its name, so `wave_height_uu`'s connect at line ~2712 is
unchanged.

### 6. Log the wind model (after line 588, beside `water_optics.summary_lines()`)

```python
    for _line in water_wave_graph.summary_lines(wave_field["wind_source"]):
        unreal.log("M_WaterVoxel " + _line)
```

Note this must move below the wave build, or pass the source separately —
`summary_lines()` takes `None` and omits that one line if the build has not run
yet.

### 7. Module docstring

Add a pointer beside the existing "THE REPEATING TILE (2026-08-11, Phase 5)"
section saying the wave field now lives in `Tools/water_wave_graph.py` and that
`docs/water-wind-waves.md` is its design note. The Phase 5 reasoning stays where
it is — it is still the reasoning for the octave count, the frequency ratio, the
quantisation knob and the drag warp, all of which are unchanged.

### 8. The MPC parameters (`create_sky_material.py`)

Not part of this change and **not applied** — the weather subsystem owns the
timing. When it lands, add to that file's tables:

```python
SCALAR_PARAMS += [("WindFieldValid", 0.0)]
VECTOR_PARAMS += [("WindVectorMS", 0.0, 0.0, 0.0, 0.0)]
```

Defaults of zero, for exactly the reason `MoonLightFraction` and
`BathyFieldValid` default to zero in that file: an unbound or undriven
collection must fail to the OLD behaviour visibly, not to a plausible-looking
wrong one. Until they exist, `build_wind_input` logs a warning and uses the
material fallback, and the water still generates.

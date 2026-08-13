# Weather system v0 — the world gets wind

Status: **implemented, never compiled, never run in the editor.** Written
2026-08-12 while a 90-minute automated chain held the editor, so nothing here
has been built or seen. The verification that *was* possible is described in
[§9](#9-what-has-actually-been-verified); read that before trusting a number.

---

## 1. What this is for

The owner wants wind-driven waves on every lake and on the ocean. Before waves
can be driven by wind, the world has to have wind — a speed and a direction
that differ from place to place and change as time passes. That is all v0
builds. It does not touch the water.

Concretely, after this change:

- Anywhere in the world, at any moment on the game clock, you can ask "which
  way is the air moving here and how fast" and get an answer.
- Two places a couple of kilometres apart get **different** answers at the
  same instant — a median 1.5 m/s and 9 degrees apart.
- The answer changes as the clock runs: at one spot, over three hours of game
  clock, the wind runs between about 1.8 and 11.8 m/s and swings right round
  the compass.
- The same seed and the same clock always give exactly the same answer, on any
  machine, so a pinned screenshot is reproducible.
- The current wind at the camera is published to `MPC_VoxelSky` every frame,
  where a material can read it.

Nothing consumes it yet. The water material's wave field still runs on its own
constants. Wiring those two together is [§10](#10-integration-edits-that-were-not-applied),
written up as a patch rather than applied, because that file belongs to the
asset regeneration chain.

---

## 2. Where the pieces live

| File | What it is |
|---|---|
| `voxel-core/include/voxelcore/weather.h` | **The field.** A pure integer function of (seed, x, y, t). No engine, no state, no allocation. |
| `voxel-core/bench/windprobe.cpp` → `vxc_windprobe` | Prints the field. Runs headless in about a second. |
| `voxel-core/tests/test_weather.cpp` | Nine tests, in CI on gcc/clang/MSVC. |
| `ue-project/Source/VoxelEarth/VoxelWeatherSubsystem.h/.cpp` | The adapter: finds the four arguments, converts units, publishes to the MPC, owns the console variables. |

### Why the field is in `voxel-core` and not beside the sky

The sky lives in `ue-project` on purpose, and wind is the same *kind* of thing
— presentation, outside the determinism boundary. Three things moved it:

1. **It can be inspected without the editor.** `voxel-core` has a bench and a
   test harness that run from a command line. `ue-project` has neither. A wind
   field nobody can print is a wind field nobody can argue about.
2. **The bake may want it.** `terrain-service` already carries an orographic
   rain-shadow wind (`diffusion.py:553`, a single fixed bearing in a coordinate
   frame `docs/world-generation-architecture.md:379-400` refuses to vouch for).
   If those two winds are ever to be reconciled, they need to be able to reach
   the same function.
3. **Integer maths makes the determinism gate free.**
   `docs/lighting-weather-plan.md`'s Part B gate is "two clients at the same
   (seed, time, position) sample identical weather". In integers that is true
   by construction on every compiler forever. In floating point it is a promise
   that has to be re-tested whenever anything changes.

The price is the float ban (`ci.yml:77-104` — no `float`/`double` in
`voxel-core/include` or `/src`). Everything is fixed point, and the two places
that would normally want trigonometry are handled the way this codebase already
handles them: a small compile-time table (as `detail_rill.h:191-198` does) and
the quintic fade from `hash.h`.

---

## 3. The model: four bands

Wind is built as **a direction plus a speed**, not as two velocity components.
Every knob anyone will ever want to turn is a knob on one or the other — "make
it windier", "the wind should come off the sea" — and a decomposition where the
knobs are nameable is worth more than one where they are not.

Four bands contribute, at four scales. Each is a thing a player could name.

| Band | Varies with | Timescale | Space scale | Contributes |
|---|---|---|---|---|
| **Regime** | time only | ~3 h of clock | — | ±180° of bearing |
| **Prevail** | time only | ~20 min | — | ±55° of bearing |
| **Synoptic** | position **and** time | ~10 min | **2048 m** | ±25° of bearing, **all** of the speed variation (×0.35 … ×1.65) |
| **Gust** | position and time | ~6 s | 180 m | ±9° of bearing, ±25% of the sustained speed |

Plus a fixed base bearing (default 240°, west-south-west) that the whole thing
is centred on.

**Regime** is what lets the wind visit the whole compass instead of fidgeting
about one quarter. "This week it is out of the north." It has no dependence on
position, because a regime that differed between two ends of the map would not
be a regime.

**Prevail** is the within-session wander. "It has backed round to the west this
afternoon." At the default day length, its 20-minute lattice is about a third
of a game day — long enough that a capture leg does not see it move, short
enough that a session does.

**Synoptic** is the band that answers the actual requirement: different parts
of the map having different wind at the same moment. Its 2048 m cell is the one
number in this whole system that is not a guess —
`docs/lighting-weather-plan.md:400` already fixed the weather cell at ~2 km so
that a front takes minutes of play to cross the visible world. This band also
**advects** (§4), which is what makes weather arrive from upwind rather than
fading in on the spot.

**Gust** is the fast local eddy. Its speed contribution is a **fraction of the
sustained speed**, not an absolute amount, which does three things at once: a
calm day has small gusts and a rough one has large ones (the real behaviour);
the gust factor stays at a realistic 1.25 whatever the wind is doing; and
`speed = sustained + gust` cannot go negative, so there is no clamp to put a
kink in the curve. The gust does **not** advect — an eddy belongs to the ground
you are standing on, not to the system passing overhead.

### Why four named bands instead of N octaves of fBm

Summing a few octaves of value noise is the standard cheap answer and would
produce a field that looks similar. It was rejected because every octave in it
is a number with no meaning, so "the wind changes too fast" has no single knob
and no single owner. Here it does: that complaint is `gustLatticeMs`, and
"the wind is too samey across the map" is `synopticLatticeMm`, and each has a
console variable and a paragraph explaining what it costs.

### Base bearing 240° is not arbitrary

It is chosen against the same evidence `voxel.Sky.OriginLatitudeDeg`'s 52.0 was
(`VoxelSkySubsystem.cpp:252-262`): `VoxelClimateProbe` measures this world's
climate window as cool-temperate maritime, which is the northern-European
westerly belt. A world whose biomes say maritime while its wind comes off the
east reads as a worldgen fault even though neither half is wrong alone.

Note the base is the **centre of a distribution, not the wind**. The regime
band alone swings ±180°, so on any given afternoon the wind will not be from
240°. Over a long sweep the compass rose is lopsided towards the west, which is
the intended shape: the rarest quarter (opposite the prevailing) still gets
about 1% of samples.

---

## 4. Advection, and a bug worth recording

The synoptic pattern is sampled at a point that slides upwind as the clock
runs, so weather approaches from the direction the wind comes from.
`docs/lighting-weather-plan.md:393` writes this as

```
p = pressureNoise(x - windU·t, y - windV·t, t·ε)
```

with `windU/windV` from a slow noise — i.e. with a **varying** advection
direction.

**That formula must not be implemented as written.** `direction(t) · speed · t`
is not the path the air took; it is the whole journey re-aimed along wherever
the wind happens to point right now. After a day of play the journey is about
1000 km long, so a perfectly ordinary wobble in the prevailing bearing — 0.07°
between one frame and the next — swings the far end of that lever by 1.2 km,
which is over half a synoptic cell.

This was measured, not reasoned about. At seed 20260719 and t = 84,895,298 ms
the wind's bearing jumped **20.4° inside one 16 ms frame**, and the speed with
it. It reads as the weather teleporting.

The fix: **advect along the fixed base bearing**, which has no wobble to
amplify. After the fix the worst frame-to-frame motion over the same sweep is
0.023 m/s and 0.213°.

What that costs, stated honestly: weather systems always travel the same way
even when the surface wind has backed round. Wrong in detail, invisible in
practice — you cannot see the direction a pattern arrives from when the pattern
is 2 km across and you can see 4. The correct fix is to integrate the velocity
along the path, which needs either accumulated state (destroying the pure
function that the whole determinism argument rests on) or an analytic integral
of the noise. Both are v1 problems.

`voxel-core/tests/test_weather.cpp`'s `wind_is_smooth_in_time` is this bug's
headstone. Its bounds are about 10× the measured worst case: loose enough that
ordinary re-tuning does not trip them, tight enough that the lever arm cannot
come back.

---

## 5. Sampling granularity — the decision the brief asked about

The request was that "tiles/chunks have different wind speed and wind direction
values at points in time". There are three ways to read that, and they are
materially different.

### It is a continuous function, not a per-chunk value

**A per-chunk constant was rejected.** It is the simplest thing that satisfies
the literal words and it would put a visible discontinuity at every chunk
border on a continuous water surface — a lake spanning four chunks would have
four different wave directions meeting at hard lines. That is precisely the
class of seam this project has spent enormous effort deleting: it is why the
bathymetry window refuses a toroidal wrap (`VoxelBathyField.h:50-66`), why the
water wave field was re-keyed onto absolute world XY so near-field voxels and
far-field sheets share one field (`create_water_voxel_material.py:280-290`),
and why the terrain detail ladder uses a quintic fade instead of bilinear
interpolation (`hash.h:158-172`).

So the field is a **continuous function of world position**, evaluated
analytically wherever it is needed. Where a genuinely per-chunk answer is
wanted later — a chunk-granular gameplay query, a per-chunk foliage sway
constant — the right thing is to *sample the same function at the chunk centre*
and treat that as a view of the field, never as the authority. The function is
the authority. That distinction is worth keeping because the moment something
caches a per-chunk value, two neighbouring chunks can disagree about a
continuous quantity and the seam is back.

The field is continuous but not infinitely so: position is quantised to
1 millimetre and time to 1 millisecond. Against the finest band (a 180 m, 6 s
gust) those are 1 part in 180,000 and 1 in 6,000. Verified across three lattice
boundaries in each axis at single-unit resolution — nothing steps.

### v0 publishes ONE value to materials, and that is a real compromise

The subsystem samples the field **at the camera** and publishes that single
vector to every water surface on screen. It does not publish a texture.

Here is what that costs, measured — the difference in wind between two points a
given distance apart, at one instant, over 4000 pairs:

| Separation | median Δspeed | p95 Δspeed | median Δbearing |
|---|---|---|---|
| 25.6 m (near-field water reach) | 0.07 m/s | 0.37 m/s | 0.5° |
| 400 m (a far lake sheet) | 0.63 m/s | 2.05 m/s | 4.1° |
| 2048 m (one synoptic cell) | 1.46 m/s | 4.42 m/s | 9.3° |
| 4096 m (the drawn world's edge) | 1.54 m/s | 4.67 m/s | 9.8° |

Read that as: for the near-field meshed water the published value is *right*.
For a lake sheet a few hundred metres out it is about 10% off in speed and 4°
off in direction. At the edge of the drawn world it can be 25% and 10° off.

**Why that is acceptable for v0 and not for v1.** Wave amplitude scales with
wind speed, so a 25% speed error is a 25% wave-height error on water 4 km away,
viewed at a grazing angle, at a scale where the wave field itself is below a
pixel. It is not visible. What *would* be visible is a discontinuity, and a
single global value has none by definition. The honest summary is that v0 is
not *wrong*, it is *less rich than the field it is built on* — every water
surface gets a plausible wind that varies correctly in time and correctly as
the player travels, and only the variation *across* one view is missing.

**The v1 upgrade is already designed, twice.** `docs/lighting-weather-plan.md`
:404-407 specifies a 256², ~50 km, camera-centred render target refreshed at a
few Hz; and this codebase already ships that exact pattern in
`UVoxelBathyFieldSubsystem` — a camera-centred world-space window, an origin
and inverse-size pair in the MPC, a validity scalar, quantised origin to stop
shimmer. Building the wind version is filling `FillWindow` with `sampleWind`
calls instead of tile lookups. Nothing in v0 has to change for it: the
subsystem's query API (`SampleWindAtWorldUU`) is already the thing that fill
loop would call, and `WindFieldValid` is already the flag consumers gate on.

---

## 6. How it reaches a shader

Five parameters, written into `/Game/Voxel/MPC_VoxelSky` every frame:

| Parameter | Kind | Meaning |
|---|---|---|
| `WindFlowDirection` | vector | unit XY of the direction the air **travels**, Z = 0 |
| `WindSpeedMps` | scalar | instantaneous speed, m/s |
| `WindSustainedMps` | scalar | the same minus the gust |
| `WindGustMps` | scalar | the gust alone, signed |
| `WindFieldValid` | scalar | 1 while publishing, 0 otherwise |

`WindFieldValid` is written **last**, after the values it vouches for, so a
consumer can never see the flag raised over stale numbers.

### Why not a new Material Parameter Collection

A second collection was the obvious answer and is against a standing decision.
`docs/water-architecture.md:207-215`: `create_sky_material.py` **deletes and
recreates** `MPC_VoxelSky` on every run, and any material still holding a
binding to the old object compiles to UE's **default material** while the log
reports success. That is the 2026-08-10 failure where all water in the world
drew with the default material and nothing said so. A second collection is a
second instance of that hazard, and the standing rule is not to create one
until something needs a runtime CPU→material channel the sky collection cannot
carry. Wind does not; it is five numbers a frame.

What that costs is real: adding these names means editing
`create_sky_material.py` and re-running the whole chain in order via
`tools/voxel-water-star-regen.ps1`. See [§10](#10-integration-edits-that-were-not-applied).

### The guard that makes the silent failure loud

Writing an MPC parameter that does not exist **does not fail**. No return
value, no warning, no log line. On the material side the mirror failure is
worse: an unresolved `CollectionParameter` compiles to a *constant*
(`MaterialExpressions.cpp:17179-17193`). So the visible result of having added
these five names to C++ but not yet to `create_sky_material.py` would be: a
wind system whose logs are perfect, water that never responds, and no evidence
anywhere.

`UVoxelWeatherSubsystem::ResolveCollection` therefore scans the collection's
`ScalarParameters` and `VectorParameters` arrays once and logs an **Error**
naming every missing parameter, the script to fix it, the script to re-run the
chain, and the failure that happens if you re-run it in the wrong order. It
still publishes afterwards — the writes are harmless no-ops and stopping would
only hide that the rest of the subsystem works.

This guard is new. The sky's own MPC writes have no equivalent; adding one
there is a reasonable follow-up.

---

## 7. Determinism and pinning

The wind is a **pure function of (seed, x, y, t)**. No state, no cache, no
first-call initialisation, no accumulation. Two clients that agree on the seed
and the clock compute identical wind independently — which is better than
replicating it, since it costs no bandwidth and cannot desync.

**The clock is the sky's clock and there is no other.** The time argument is
`UVoxelSkySubsystem::GetSkyState().EpochSeconds`, rounded to whole
milliseconds. This is the single most important constraint in the whole design:
the capture harness works by pinning that clock (`-VoxelTimeOfDay`,
`voxel.Sky.TimeScale 0`) so two screenshots can be differenced, and a wind on
its own accumulator would drift between the settle wait and the shutter and
quietly invalidate every appearance A/B this project takes.

**So a pinned clock already pins the wind, exactly.** Nothing extra is needed
for a reproducible capture. Rounding to milliseconds is what makes it exact
rather than nearly-exact: two runs pinned to the same `-VoxelTimeOfDay` resolve
the same epoch, hence the same integer, hence bit-identical wind.

One surprise, logged once when it happens: **with `voxel.Sky.Enabled 0` the
wind freezes at the t=0 field**, not at what was on screen. The sky subsystem
zeroes its whole state struct when switched off, so `EpochSeconds` genuinely
reads 0. Deterministic and fine — just not what "freeze" looks like from
outside.

### The console variables

All live; all read every frame.

| Variable | Default | What it does |
|---|---|---|
| `voxel.Weather.Enabled` | 1 | 0 costs nothing per frame and drops `WindFieldValid` to 0 so consumers fall back. A clean A/B on "how much of the water is the wind". |
| `voxel.Weather.BaseWindFromDeg` | 240 | Prevailing quarter the wind comes **from**. Also sets which way weather travels. |
| `voxel.Weather.BaseWindMps` | 6.0 | Mean sustained speed. Beaufort 4. |
| `voxel.Weather.WindScale` | 1.0 | Global gain. 0 is a dead calm — the control arm of every wind-driven-waves comparison. |
| `voxel.Weather.GustScale` | 1.0 | Gust band only. 0 gives a steady wind that still varies in space and over the slow bands. |
| `voxel.Weather.FieldScaleM` | 1.0 | Multiplies the **spatial** lattices only. Clamped 0.05…64. |
| `voxel.Weather.PinFromDeg` | −1000 | Pin the direction. Sentinel is −1000, not −1, because −1 is a legal bearing. |
| `voxel.Weather.PinMps` | −1 | Pin the speed. 0 is a legal pin. Removes the gust. |
| `voxel.Weather.LogIntervalSeconds` | 0 | Periodic "wind is currently…" line, reporting what was *used*. |

Command line, parsed in `Initialize` and never via `-ExecCmds` (which lands
*after* subsystem initialisation — a lesson this module has learned three times
and written down twice):

```
-VoxelWindFromDeg=<deg>    -VoxelWindMps=<m/s>
```

`Enabled`, `WindScale` and `FieldScaleM` reuse the names
`docs/lighting-weather-plan.md:965-966` pre-registered.
**`voxel.Weather.Override` is deliberately left unimplemented** — the plan
specified it as a preset selector (clear/overcast/rain/snow/storm), v0 has no
presets, and quietly repurposing a reserved name to mean "pin the wind" is how
a plan and a build stop describing the same thing.

### Seeding, and why the wind is not in the hash channel registry

`hash.h:16-122` is emphatic that the channel id is the only domain separator
and that reusing one is world-breaking. The obvious move was to allocate
`CH_WIND_*` ids at 62… and register them.

It is the wrong move, for a reason about scope rather than hygiene. Those ids
are the namespace of **world derivation**: everything in it feeds terrain,
feeds the edit-log digest, and is pinned by `kWorldGenVersion`. Wind feeds a
material. Putting it there would make a change to the gust timescale look, to
every tool that reads the registry, exactly like a change to the cave lattice —
and would invite the next person to bump `kWorldGenVersion` for it, which would
invalidate every saved edit log in the project in order to make the water
choppier.

So the wind derives its own seed, `splitmix64(worldSeed ^ "WIND_V0\0")`, and
numbers its channels privately from 0 inside that space. Two different seeds
make the fields independent whatever the ids are.

**The condition under which this reverses**, recorded so it is not
re-litigated: if wind ever becomes an *input* to world derivation — aeolian
deposition, wind-driven erosion, a windward/leeward term baked into tiles —
then it belongs in the registry, needs a `kWorldGenVersion` bump, and the salt
has to go.

---

## 8. What v0 deliberately does not do

Everything in this list is a placeholder or an absence, not an oversight.

**No terrain.** A ridge does not accelerate the wind and a valley does not
shelter it. **This is the single most visible missing effect for the thing wind
was asked for** — a lake in a steep valley should be glassy while one on a
plateau is choppy, and that contrast is more of what "wind-driven water" looks
like than the wind speed itself. It is out because it needs the carrier's
elevation and gradient at the sample point, which means an `Amplifier` and a
tile source, which turns a pure four-argument function into something that
needs the streaming layer and can fail. The hook: a `windTerrainFactor(exposure,
shelter)` multiplying `sustained`, taking two scalars a caller supplies, so the
field itself never learns about tiles. **This is the first thing to build next.**

**No climate coupling.** `climate.h`'s wire format carries exactly four
channels (`bio_1`, `bio_4`, `bio_12`, `bio_15`) and none is wind. Adding one
rolls `provider_id` and re-bakes every tile in the world. Climate is also
biome-scale and static, so it is the right thing to *modulate* wind by later
(windier in maritime places, gustier where precipitation seasonality is high),
not a source of wind now.

**No reconciliation with the bake's wind.** `terrain-service` has an
orographic rain-shadow wind at a fixed 270°, and
`docs/world-generation-architecture.md:379-400` explicitly refuses to say which
compass direction its dry side faces, because two coordinate swaps sit between
it and the rendered world. Until that is resolved, the runtime default of 240°
and the bake's 270° are two unrelated numbers that happen to be close. **Do not
"fix" one to match the other before resolving the orientation.**

**No vertical component and no variation with altitude.** Wind is a horizontal
2D vector. Water surfaces are horizontal.

**No texture, no per-view spatial variation.** §5.

**Nothing simulated is pushed by it.** Standing decision at
`docs/lighting-weather-plan.md:1074-1078`: no physics, no debris, no
projectiles.

**No presets, no fronts as objects, no precipitation, no cloud, no temperature,
no humidity, no fog response, no lightning.** All of those are specified in
`docs/lighting-weather-plan.md` §5.1 as outputs of the *same* field sample, and
the header is named `weather.h` rather than `wind.h` so they can be added
without a rename.

**Every parameter is a starting guess except one.** The synoptic cell size
(2048 m) comes from the plan. Everything else — 6 m/s, ±180°, 6 s, 0.25, ×2.0 —
is a plausible number nobody has yet judged against water. When somebody does,
`voxel.Weather.BaseWindMps` and `voxel.Weather.GustScale` are the first two
knobs.

### Growth path, in the order it makes sense

1. **Wire the water material to the MPC** — §10. Nothing about wind is visible
   until this happens.
2. **Terrain shelter.** The biggest visible win and the biggest missing piece.
3. **A wind info texture** on the `VoxelBathyField` pattern, so distant water
   gets its own wind. §5.
4. **Temperature and humidity** from the same sample, modulated by the climate
   channels — the plan's §5.1 formula, which needs a tile lookup and therefore
   the same plumbing as (2).
5. **Precipitation and cloud**, which are what temperature and humidity are for.
6. **Presets** (`voxel.Weather.Override=storm`) once there is more than one
   quantity for a preset to set.
7. **Proper path-integrated advection**, if fronts ever need to arrive from the
   direction the wind is actually blowing rather than from the base bearing.

---

## 9. What has actually been verified

**Nothing has been compiled and nothing has been run in the editor.** Be
precise about what that leaves.

**Verified, and how:**

- *The field's arithmetic.* An independent integer mirror of the algorithm was
  written in Python, modelling C++'s truncating division and int64 width
  exactly, and every intermediate was range-checked on every evaluation. The
  widest value anywhere in the call is 1.37 × 10¹⁷ against int64's 9.22 × 10¹⁸
  — a 67× margin. Sweeps covered ±4000 km of position and 28 hours of clock,
  plus extremes at ±10⁹ km and 10⁸ s.
- *Every number quoted in this document.* All from that mirror.
- *The direction rose.* Enumerated over all 360,000 whole-milli-degree
  bearings: table entries are unit to within 0.0012%, the interpolated vector's
  length runs 0.99514…1.00001 of unit, and the bearing that comes back out is
  within 0.0094° of the one that went in.
- *Seam continuity.* Walked across three time-slice boundaries and three
  spatial lattice boundaries at 1 ms / 1 mm resolution. Nothing steps.
- *The advection bug and its fix.* §4.
- *The float ban.* `voxelcore/weather.h` passes the exact CI check.
  (Unrelated finding: **`voxel-core/include/voxelcore/fluidoccupancy.h`
  currently fails it**, at 16 lines. That is pre-existing and not touched here,
  but the `float-ban` CI job must be red.)

**Not verified, and what could be wrong:**

- *That any of the C++ compiles.* The header uses C++20 constexpr features
  (an immediately-invoked lambda inside a `static_assert`, `constexpr`
  indexing of a `const char*` table) that are legal but that I could not
  exercise. The `-Wall -Wextra -Werror` bench and test builds are the most
  likely place for a first failure.
- *That the C++ agrees with the Python mirror.* This is the one risk that is
  handled: `test_weather.cpp` pins five exact samples produced by the mirror.
  If the two disagree, that test fails loudly with both numbers. **If it does,
  work out which side is wrong — do not regenerate the expectation.**
- *That the UE subsystem compiles or ticks.* Unverified UE API surface:
  `FCollectionScalarParameter::ParameterName` / `FCollectionVectorParameter::
  ParameterName` (used for the missing-parameter scan),
  `TAutoConsoleVariable::AsVariable()`, and the `FVTableHelper` constructor
  shape. All three are copied from working code in this module, but copied is
  not compiled.
- *That the wind looks right.* Nobody has seen it. Per this project's standing
  rule, that is the owner's call on a screenshot, not mine — and there is
  nothing to screenshot until §10 is applied.

**No golden digest in the test.** The convention here is a swept
`vxc::Digest` pinned as `// GOLDEN(name)`. It is absent because the digest
could not be computed without a compiler and a fabricated one is worse than
none. Whoever first builds this should add a sweep digest and run
`tools/regen-goldens.ps1`.

### Running the probe

```
cmake --build build --target vxc_windprobe
build/bench/Release/vxc_windprobe 20260719
build/bench/Release/vxc_windprobe 20260719 --hours 12 --step 900 --quiet
build/bench/Release/vxc_windprobe 20260719 --pin-from 270 --pin-speed 9
```

It prints its config, a time series, a spatial census, the by-distance error
table from §5, and a verdict line beginning `vxc_windprobe: RAN.` — and it
**refuses with exit 1** if the field turns out to be constant in space, or in
time, or both. A constant wind and a wind field that never ran print the same
page of plausible numbers, and this project has burned a session on exactly
that class of mistake before (`docs/water-architecture.md:143`).

---

## 10. Integration edits that were **not** applied

These three are needed to make the wind visible. None was applied: the first
two are owned by the asset regeneration chain, which was running.

### 10a. `ue-project/Tools/create_sky_material.py` — declare the parameters

Add to `SCALAR_PARAMS` (near line 448):

```python
    # --- wind (UVoxelWeatherSubsystem, docs/weather-system-v0.md) ------------
    # DEFAULTS CHOSEN TO FAIL VISIBLY, NOT PLAUSIBLY, which is this table's
    # standing rule and the same reason MoonLightFraction and BathyFieldValid
    # default to 0: an undriven wind parked at a plausible 6 m/s is the single
    # most convincing way to make water look correct while proving nothing.
    # WindFieldValid at 0 makes every consumer fall back to its own constant.
    ("WindSpeedMps",     0.0),
    ("WindSustainedMps", 0.0),
    ("WindGustMps",      0.0),
    ("WindFieldValid",   0.0),
```

Add to `VECTOR_PARAMS` (near line 530):

```python
    # Unit XY of the direction the air TRAVELS (not where it comes from), Z=0.
    # Defaulted to zero rather than to a unit vector for the same reason as
    # above: a material that ignores WindFieldValid gets a zero vector and
    # looks broken, which is the correct outcome.
    ("WindFlowDirection", (0.0, 0.0, 0.0, 0.0)),
```

Then re-run the **full** chain in order —
`tools/voxel-water-star-regen.ps1` without `-WaterOnly`. Running the water
alone leaves it bound to a collection that no longer exists in the form it
expected; that is the 2026-08-10 all-water-drew-with-the-default-material
failure and it cost a night.

`UVoxelWeatherSubsystem` logs an Error listing exactly which of the five names
are missing until this is done, so the state is diagnosable rather than
invisible.

### 10b. `ue-project/Tools/create_water_voxel_material.py` — consume them

Today the wave field's direction and height are static material scalars with no
CPU driver:

| Parameter | Default | Line |
|---|---|---|
| `WaveDirBaseDeg` | 58.7 | ~2453 |
| `WaveAmplitudeM` | 0.25 | ~2383 |

The minimal wiring, keeping both as fallbacks:

- **Direction.** Convert `WindFlowDirection` (a unit XY vector) to the angle
  the wave loop wants, or better, feed the vector straight in and skip the
  angle — the loop rotates a direction per octave from a base, and a base
  *vector* is one fewer sin/cos in the shader than a base *degree*. Blend
  against the existing `WaveDirBaseDeg` with `WindFieldValid` so a run without
  the subsystem is unchanged.
- **Amplitude.** Wave height grows with wind speed. A defensible v0 curve is
  `WaveAmplitudeM * saturate(WindSpeedMps / 10)`, again lerped by
  `WindFieldValid` so the current look is exactly what you get at
  `WindSpeedMps = 10`. This keeps every existing capture comparable.
- **Consider** driving foam or streak intensity from `WindGustMps` rather than
  from the total, so gusts read as gusts.

Two things to check while doing it, both already written down in that file:

- Its `collection_param()` helper re-checks name membership against the
  collection as read back and raises. That check is the safety net, not a
  formality — if 10a has not been run, this script should fail loudly.
- `create_water_voxel_material.py:2599` already cites Valheim's
  `lerp(0, wind, depth)` shore damping. Wind arriving is the missing half of
  that note.

**Do not tune the wave response and the wind field in the same session.** They
move the same pixel, and this project's own rule is one at a time.

### 10c. `docs/lighting-weather-plan.md` — status

`P2` ("Weather field, sampling API, debug visualisation, fog/atmosphere
response, NO particles") is listed as *Not started*. It is now partly done:
the field, the sampling API and a headless debug readout exist; the fog and
atmosphere response do not, and neither do the non-wind outputs. Worth
amending so the plan and the tree agree.

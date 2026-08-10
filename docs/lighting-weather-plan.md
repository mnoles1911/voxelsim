# Day/night, weather and lighting — implementation plan

Written 2026-07-29. Scoped in one session against the code as it stands after
PR #182 (`claude/phase3-integration`). Design only — nothing here is
implemented yet. `file:line` references were current at the time of writing and
are worth re-checking before leaning on one.

**Corrected 2026-07-29, same day, against the code.** The first draft asserted a
critical path that does not exist: it claimed a moving sun forces continuous
brick re-solves, made "the encoding split" the spine of the plan, and ordered
every phase behind it. That is wrong — sun direction is not an input to the
solve at all, and the day/night cycle needs no GI work whatsoever. §2.2 now
records what was believed and why it was wrong; §2.3 states the composition
contract that makes it true; §6 is reordered. Numbers in §5.2, the shadow
strategy in §5.3, the night-darkness mechanism in §5.4 and the execution route
in §5.7 were all corrected in the same pass. Anything not marked corrected was
re-checked and stands.

This plan closes the item the implementation plan has carried unanswered since
July: §6 *"Weather & seasons (climate channels support it)"*.

---

## 0. Read this before starting

The executing session starts cold. Read, in order:

| Document | Why |
|---|---|
| `docs/voxel-earth-implementation-plan.md` §2 (doctrine), §3.1, §4 | The determinism doctrine this plan must not violate, and where M4 sits |
| `docs/gpu-gi-volume-design.md` | The GI volume's design. Its header was stale and has been corrected — see §2.1 |
| `ue-project/Source/VoxelEarth/VoxelLightField.h` | The ambient-cube formulation, in full, with its own rationale |
| **§2.3 of this document** | The composition contract. Read it before writing a line of shading code — getting it wrong produces a plausible image with a breathing brightness ring |
| `docs/backlog.md` §0 | Where the frame time actually goes. Everything visual lands on the full side |
| `docs/lessons-2026-07-27-s0-s1.md` | The measurement discipline this plan's gates inherit |

Critical files this plan touches:

```
ue-project/Source/VoxelEarth/VoxelEarthGameMode.cpp:100-137   the light rig, spawned from code (server-only, see §8 risk 9)
ue-project/Source/VoxelEarth/VoxelLightField.h/.cpp           the ambient cube; SolveBrickInternal at .cpp:654-805
ue-project/Source/VoxelEarth/VoxelGI.cpp                      budgets, cvars, the ingest hooks, the fade clamp at :970-986
ue-project/Source/VoxelEarthShaders/Public/VoxelGIVolume.h    the GPU volume's uniform buffer
ue-project/Source/VoxelEarthShaders/Private/VoxelGIVolume.cpp the volume itself; VolumeDim default at :38-43
ue-project/Shaders/VoxelQuadVertexFactory.ush:445-600         where GI is recombined per pixel (:573-579)
ue-project/Source/VoxelEarth/VoxelClimateProbe.h              temperature/precipitation, already calibrated
ue-project/Source/VoxelEarth/VoxelClipmapActor.cpp:311-384    the exposure trap, already documented and already fought once (§5.8)
ue-project/Tools/terrain_material_common.py:285-312           the snow term, already parameterised — both terrain materials share it
ue-project/Tools/create_voxel_material.py                     M_VoxelTerrain is a GENERATED artifact; edit this, not the .uasset
ue-project/Tools/create_clipmap_material.py                   same, for M_VoxelClipmap
```

New files this plan adds: `VoxelSkySubsystem.*`, `VoxelEphemeris.*`,
`VoxelWeather.*`, `VoxelWeatherField.*`. (`VoxelLightInjection.*` was on this
list in the first draft. It is not, today, a file anyone can usefully write —
see §5.4: there is no additive path from a volume to the pixel.)

---

## 1. Decisions, locked

Scoped with the project owner on 2026-07-29. These are settled inputs, not
options to revisit mid-execution.

| # | Decision | Reason |
|---|---|---|
| 1 | **Weather is a deterministic function of (seed, worldTime, position).** Only a world clock is replicated | Doctrine #1 and #2 verbatim. No weather state on the wire, works offline unchanged, and two players in the same place necessarily see the same sky |
| 2 | **Cosmetic and shading only.** No world-state effects | Weather never authors an edit-log entry, never sources the water CA, never touches deterministic physics. This is what keeps the whole system outside the determinism boundary — see §1.1 |
| 3 | **Full solar geometry and seasons on a long calendar** | Real declination/hour-angle from latitude and day-of-year. Polar summers, seasonal sun paths and a moving snow line fall out of one function |
| 4 | **Advected weather field over the climate channels** | Fronts drift across the world and arrive from the horizon, modulated by the per-tile precipitation/temperature the terrain already carries |
| 5 | **Sky gets everything**: volumetric clouds, night sky with moon phases, aerial perspective + height fog, and the set pieces (lightning, god rays, rainbows, aurora) | Owner's call, all four selected |
| 6 | ~~**Sun shadows: cascades near, volume-traced far**~~ → **REVISED: CSM only, cadence-tuned. Far shadows deferred** | The original reason was "cheap traced shadows out to the 50 km vista where cascades cannot reach". That is geometrically impossible with this structure — the volume ends at 38 m, *inside* cascade 0, and occluders only exist inside the 70 m build radius. See §5.3 |
| 7 | **Night is genuinely dark, with emissive voxels and placed lights** | Otherwise the cycle only recolours the sky. **The mechanism is dimming the lights plus an exposure policy — NOT crushing the GI term**, which multiplies albedo and would leave a torch lighting nothing (§5.4) |
| 8 | **Precipitation**: camera-anchored particles + wetness/snow as voxel shading + rain ripples on water. **No screen-space lens effects** | Explicitly not selected — the look stays world-anchored, not camera-anchored. Note the project has **zero** particle infrastructure today (§5.6) |
| 9 | **Target: this desktop (RX 7800 XT) at 2K, with scalability tiers** | Matches how the rest of the project is built. Every heavy feature is cvar-gated |
| 10 | **Perf-blind design, measured after** | Owner's call. The per-phase measurement legs in §6 are the safety net, and they are *recording*, not gating |

### 1.1 Why none of this needs an ADR

The doctrine that would normally bite — *"everything below the 30m tiles is
bit-deterministic integer math"* — applies to world **state**. This system
produces none. It reads the clock and the climate channels and writes pixels.
That is the same boundary `VoxelLightField.h` already argues for the GI field:

> *CLIENT-SIDE RENDERING ONLY... Two clients may legitimately converge to
> slightly different irradiance; they must never disagree about world state, and
> nothing here can make them.*

So the ephemeris and the weather field may use doubles freely. Clients agree to
within float precision, which is sufficient because nothing downstream writes
state. **The moment weather sources the water CA or deposits snow voxels, that
stops being true and an ADR is required.** Decision #2 exists to keep that door
shut, and §8 lists what walking through it would cost.

The one replicated quantity is the clock: an epoch and a rate, a handful of
bytes, sent on join and on correction.

---

## 2. Five findings that shape the work

### 2.1 The GPU GI volume is BUILT. The design doc said it wasn't

`docs/gpu-gi-volume-design.md` opened with *"Design only; nothing here is
implemented yet"*. That was true on 2026-07-25 and is false now. **That header
has since been corrected** — this section is what prompted it.
`VoxelGIVolume.h/.cpp` exists, `VoxelQuadVertexFactory.ush` carries a `GIUVW`
interpolant and samples the volume per pixel with multi-step probe fallback
(`:481-590`), and the volume shipped as **Scheme A** — two RGBA8 3D textures,
`(Vis[+X],+Y,+Z,v)` and `(Vis[−X],−Y,−Z,v)` — not the Scheme B the doc
recommends. The header records why: Scheme B *"missed the design's own RMS bar
by 2.6x, worst on exactly the cave walls the feature exists for."*

**Consequence: the single largest item in this plan was already paid for.** The
day/night work is not "build a GI volume"; it is "drive the light rig from a
clock." The volume needs no work at all for the cycle to happen — see §2.2.

### 2.2 The field does NOT depend on the sun. The first draft said it did — that was wrong

**What the first draft claimed.** That `VoxelLightField.cpp:771` fuses a
time-varying radiance into the stored byte:

```cpp
ConeIrr[D] = FMath::Clamp(Vis * FMath::Min(Params.SkyIntensity, 1.f) + Bounce, 0.f, 1.f);
```

and therefore that a moving sun forces **every resident brick to be re-solved
continuously** — ~2,000 bricks against a budget of 8 solves per frame, a cycle
that takes minutes to catch up and saturates the solve budget forever. On that
premise, separating visibility from radiance ("the encoding split") was the
spine of the plan and every other phase queued behind it.

**Why it is wrong.** Three facts, each checkable in under a minute:

1. **Sun direction appears nowhere in the solve.** `SolveBrickInternal`
   (`VoxelLightField.cpp:654-805`) marches the 14-cone basis over
   `TraceDirTable`, a fixed geometric basis. It reads opacity and the previous
   pass's `AvgIrr`. There is no sun, no light direction, no time. Grep the
   function for anything solar and it returns nothing.
2. **`SkyIntensity` is a constant that is never assigned.** It is declared
   `float SkyIntensity = 1.0f;` at `VoxelLightField.h:210` and that is the ONLY
   assignment anywhere in the repository — no cvar writes it, no caller sets it.
   At the point of use it is additionally clamped to `min(x, 1.f)`
   (`VoxelLightField.cpp:771`). The header says what the 1.0 is for: *"an
   unoccluded cell solves to exactly 1.0, which is what keeps open terrain
   looking identical with GI on and off."*
3. So the multiplication by `SkyIntensity` is multiplication by one. **The field
   stores sky visibility under an isotropic unit sky.** Its correct contents do
   not change when the sun moves.

**⇒ Zero re-solves are needed. Today. With no encoding split, no new volume, and
no code.** The premise that made the split urgent does not exist.

**Where absolute light actually comes from.** UE's deferred path. The rig at
`VoxelEarthGameMode.cpp:100-137` spawns a `DirectionalLight` (intensity 8,
`SetAtmosphereSunLight(true)`) and a `SkyLight` with
`SetRealTimeCaptureEnabled(true)`, which tracks the atmosphere automatically.
Those are the lights. The GI term is not a light — it is a **relative modulation
of albedo**: `M_VoxelTerrain` computes `BaseColor = albedo * VertexColor.G`, and
the factory folds the sampled visibility into `.g` at
`VoxelQuadVertexFactory.ush:573-579`. `VoxelGI.h:49-57` says so outright in its
"HONEST SCOPE NOTE": *"it modulates albedo rather than the ambient term
specifically."*

**So the cycle already works through the engine.** Rotate the directional light
and dim it, and everything darkens together — including cave interiors, because
a cave mouth's *incident* light is dimming at the source while the visibility
term that shapes it stays put. The shaping is geometry; the brightness is the
light. They were never fused.

> **Consequence, stated plainly: the day/night cycle needs NO GI work at all.**
> It is a clock, an ephemeris, the light rig that already exists driven from it,
> and an exposure policy (§5.8). That is the whole visible feature.

The encoding split is still a real refinement — per-direction sky *colour* (a
blue zenith over a red horizon at dusk) cannot come out of a scalar visibility
byte, and neither can a lightning flash that reddens only the cave mouths facing
it. But it buys a second-order colour effect, not the cycle, and it is worth far
less than the first draft claimed. **It is not the critical path.** §6 demotes it
accordingly, behind two phases that are nearly free and visible immediately.

### 2.3 The composition contract — read this before writing shading code

This is the single most load-bearing sentence in the document:

> **The GI volume term is RELATIVE, not radiometric. An unoccluded point reads
> 1.0 at every time of day, forever. Absolute intensity comes from the engine
> lights and nowhere else.**

It is a *shadowing/occlusion* factor multiplying albedo, not sky radiance in any
unit. `VoxelGI.h:49-57` and `VoxelLightField.h:206-210` both say this in their
own words; the shader's `lerp(AmbientFloor, 1.0, saturate(Irr))` at
`VoxelQuadVertexFactory.ush:573` is the arithmetic that makes it true.

**The failure this prevents.** Suppose an executing agent reads §2.2's dismissal
as licence to make the volume absolute — to scale the stored term by a
time-varying sky radiance so that night reads 0.05 and noon reads 1.0. Then:

- Open terrain **inside** the volume darkens **twice** at night: once because
  the directional light and SkyLight dimmed, and again because the GI term
  dropped. Multiplicatively.
- Open terrain **past the volume's reach** darkens **once** — the fade at
  `VoxelGI.cpp:970-986` has already blended the GI term out to 1.0 there, so
  only the engine lights apply.
- The boundary between the two is the fade band, which is **camera-relative**
  (`FadeCentrePoolUU` is the camera, `VoxelGIVolume.h:53-60`). It moves with the
  player.

The result is a ring of "correct" brightness sliding across the world around the
camera, breathing brighter and dimmer as the sun sets and rises. It is exactly
the class of bug `gpu-gi-volume-design.md` risk 8 warns about — *"a plausible
image with a hard lighting ring — the hardest kind of wrong to notice"* — with
the added misery that it only appears at some times of day.

**The rule that follows:** any time-varying quantity this plan introduces
belongs on a light, on the atmosphere, or on the exposure. Nothing that varies
with time of day may be multiplied into `VertexColor.g`. If a later phase does
introduce per-direction sky colour into the volume (§5.2), it must be normalised
so that the unoccluded case still evaluates to 1.0 — the colour term reshapes,
it does not scale.

### 2.4 All four vertex-colour channels are spoken for

`VoxelQuadVertexFactory.ush:298-305`:

| Channel | Terrain | Water |
|---|---|---|
| R | biome tint | corner height |
| G | AO, then GI folded in at `:579` | AO |
| B | climate.x (temperature) | top boundary |
| A | climate.y (precipitation) | — |

Nothing is free, and `BaseColor = albedo * VertexColor.G` can only **darken** —
snow needs to whiten, and the long-tail light injection of §5.4 needs to *add*,
which the factory also cannot do. So neither can be done inside the factory
alone.

Two corrections the first draft got wrong here:

- **Snow does not need any of this.** Both terrain materials already carry a
  parameterised snow term with its own colour, snow line and roughness
  (`terrain_material_common.py:285-312`). It is driven by an existing scalar, not
  by a new interpolant. See §5.7.
- **Nothing in this plan needs the editor.** All four materials are generated by
  checked-in Python run headlessly via `-run=pythonscript`. A graph change is a
  script edit and a regeneration — a reviewable text diff, not an editor session
  (§5.7).

What genuinely remains channel-constrained is **wetness**, and `TEXCOORD1` — the
obvious place to put it — is already taken by `GIUVW` and arrives as zero on the
component path. §5.7 has the details; it is a design decision, not a step.

### 2.5 The perf shape, and what it implies about where to put things

From `docs/backlog.md` §0: render thread 13.5 ms *is* the frame; the game thread
spends 10.1 ms **waiting**; the GPU frame at 16.8–18.4 ms is the real ceiling.

⇒ **The clock, the ephemeris, the weather field and the light-source bookkeeping
are effectively free** — they are game-thread work, and the game thread is idle
75% of the frame.

⇒ **Volumetric clouds, volumetric fog (god rays) and the extra volume samples
are not free** — they are GPU, and the GPU is the ceiling. These are the three
items that need tiers and honest measurement.

Per `voxelsim-draw-path-2k`'s rule, no cost estimate in this document should be
believed until a leg measures the frame. None are given.

---

## 3. Architecture

Five parts. Each is independently useful and independently verifiable.

```
  ┌──────────────────────────────────────────────────────────────┐
  │  A. Clock + ephemeris    (replicated epoch; pure functions)   │
  │     worldTime, lat/long → sun dir, moon dir+phase, season     │
  └────────────┬──────────────────────────────┬──────────────────┘
               │                              │
  ┌────────────▼─────────────┐   ┌────────────▼──────────────────┐
  │ B. Weather field         │   │ D. Sky & atmosphere           │
  │  advected, deterministic │──▶│  SkyAtmosphere, clouds, fog,  │
  │  cloud/precip/wind/humid │   │  night sky, set pieces        │
  └────────────┬─────────────┘   └────────────┬──────────────────┘
               │                              │  sky/sun radiance
               │                 ┌────────────▼──────────────────┐
               │                 │ C. Lighting                   │
               │                 │  engine lights carry INTENSITY│
               │                 │  Vis volume (exists) SHAPES it│
               │                 │  + exposure policy            │
               │                 └────────────┬──────────────────┘
               │                              │  sky visibility
  ┌────────────▼──────────────────────────────▼──────────────────┐
  │ E. Precipitation & surface response                          │
  │    particles occluded BY the Vis volume; snow gated BY it     │
  └──────────────────────────────────────────────────────────────┘
```

**The load-bearing reuse:** the sky-visibility volume answers "can the sky see
this point?" — which is simultaneously the GI question, the *does rain reach
here* question, the *does snow settle here* question, and the *is this cave dark*
question. One asset, four consumers, guaranteed consistent. Rain will not fall
inside a tunnel because the same data that darkens the tunnel kills the
particles.

**And note what the volume is NOT in this diagram.** It carries no time and no
absolute brightness; it does not sit between the sun and the pixel. Part D's
lights are what get brighter and dimmer, and part C's volume only shapes where
that light can reach (§2.2, §2.3). The arrow from D to C is *"which way is the
sun pointing and how bright is it"* — consumed by the engine's deferred path, not
written into the volume.

---

## 4. Part A — clock, calendar and ephemeris

**What it buys:** the sun rises in the right place at the right time for where
you are standing and what month it is. Everything else in this plan is a
function of it.

`UVoxelSkySubsystem` (UWorldSubsystem, game thread):

- **Clock.** `double WorldEpochSeconds`, advanced by `DeltaTime × TimeScale`.
  Server-authoritative, replicated as (epoch, scale) on join and on a slow
  correction cadence. Persisted next to the edit log so time survives a restart —
  consistent with the implementation plan's *"time freezes offline"*.

  > **A `UWorldSubsystem` does not replicate, and the light rig is server-only.**
  > Put the two replicated scalars on the **existing** `AVoxelEditRelay`
  > (`VoxelEditRelay.h:55-60` is the established pattern) and give rig ownership
  > to the client, or a dedicated-server client renders an unlit or stale sky.
  > This is a silent failure in the one configuration nobody routinely runs —
  > see §8 risk 8 for the full argument.
- **Calendar.** `voxel.Sky.DayLengthSeconds` (default 2400 = a 40-minute day;
  doubled from 1200 on 2026-08-09 because the sky read as moving too fast),
  `voxel.Sky.DaysPerYear` (default 48 — a season passes in roughly eight hours of
  play at that day length), `voxel.Sky.AxialTiltDeg` (23.44).
- **Geographic mapping — new concept, does not exist today.** A grep for
  latitude/longitude across the whole repo returns nothing: the diffusion world
  is an unlabelled infinite plane. This plan defines the mapping:

  ```
  latitude  = OriginLatitudeDeg + (WorldY_m / 111_320.0)
  longitude = OriginLongitudeDeg + (WorldX_m / (111_320.0 * cos(latitude)))
  ```

  with `voxel.Sky.OriginLatitudeDeg` defaulting to **52.0**. That default is not
  arbitrary: `VoxelClimateProbe.h` measures this world's climate window at
  −8.6…+19.3 °C and 659…1506 mm/yr and calls it *"cool-temperate maritime"* —
  which is where 52°N sits. Pick a latitude that contradicts the terrain and the
  snow line will disagree with the biomes.

- **Solar position.** Standard low-precision solar equations (Meeus ch. 25 /
  NOAA): day-of-year → declination and equation of time; longitude + clock →
  hour angle; declination + hour angle + latitude → altitude and azimuth. Double
  precision, once per frame, ~nothing.
- **Lunar position and phase.** Truncated ELP2000 (Meeus ch. 47) is more than
  accurate enough. Phase comes from the sun–moon elongation, which gives both the
  rendered moon disc and the moonlight intensity — a new moon really is dark.
- **Sidereal rotation** for the star field.

Outputs, published once per frame as a plain POD other systems read:

```cpp
struct FVoxelSkyState {
    FVector SunDirection, MoonDirection;   // world space, unit
    float   SunAltitudeDeg, MoonPhase;     // phase 0..1
    float   DayFraction, YearFraction;
    double  LatitudeDeg, LongitudeDeg;
    float   SeasonalTempOffsetC;           // feeds the snow line and the weather field
};
```

**Gate (P0):** sun altitude/azimuth within **0.5°** of a published solar
calculator at four checkpoints — equinox and both solstices at 52°N, plus one
polar case at 70°N to prove the midnight sun. Unit-testable with no renderer.

---

## 5. Parts B–E — the systems

### 5.1 The weather field (Part B)

**What it buys:** you can watch a storm come in over the ridge, and the rain you
are standing in matches the clouds overhead and the climate of the place.

A coarse, seed-deterministic scalar field advected by a slow synoptic wind:

```
sample(x, y, t):
    windU, windV = slowNoise(t)                       // synoptic drift, changes over ~hours
    p  = pressureNoise(x - windU·t, y - windV·t, t·ε) // the front pattern, drifting
    h  = humidityNoise(...) modulated by tile precipitation channel
    T  = tileTemperature + lapseRate·altitude + SeasonalTempOffsetC + diurnal(t)
    → CloudCover, PrecipIntensity, PrecipIsSnow = (T < 0), WindVector,
      Humidity, FogDensity, LightningRate
```

Cell size ~2 km, so a front takes minutes of play to cross the visible world.
Pure function of `(seed, t, x, y)` — decision #1 satisfied by construction,
nothing cached that could diverge.

Two consumers at two rates: **a point sample at the camera** each frame for fog,
precipitation and audio cues; and **a small render target** (256², ~50 km,
camera-centred, refreshed at a few Hz) that becomes the VolumetricCloud weather
map, so the clouds you see are literally the same field as the rain you feel.

`voxel.Weather.Override=<preset>` forces clear/overcast/rain/snow/storm for
testing and for screenshots.

### 5.2 The encoding split (Part C — a refinement, NOT the spine; see §2.2)

**What it buys:** per-direction sky *colour*. A blue zenith over a red horizon at
dusk, reaching into a cave mouth with the right hue per opening direction; a
lightning flash that brightens only the openings facing it. It does **not** buy
the day/night cycle — §2.2 — and it is not a prerequisite for anything else here.

**The numbers the first draft got wrong.** It quoted N=256 and 134 MB. Neither is
what ships. Corrected, against `VoxelGIVolume.cpp:38-43` and `VoxelGI.h:254-261`:

| | first draft said | actually |
|---|---|---|
| `voxel.GI.VolumeDim` default | 256 | **192** (`ECVF_ReadOnly`, clamped to [16,256], rounded **down** to a multiple of 8) |
| existing volumes | 134 MB | **56.6 MB** VRAM — 2 × RGBA8 at 192³ |
| CPU cost | not mentioned | **an equal-size CPU mirror**: `VolumeShadow` + `VolumeShadowNeg`, another 56.6 MB of system RAM. **~113 MB total** |
| coverage | ±5120 UU = ±51.2 m | **±3840 UU = ±38.4 m** |
| usable reach | assumed = coverage | **~19–35 m.** See below |

**The usable reach is much smaller than the half-extent**, and this is the number
that matters. `VoxelGI.cpp:970-986` clamps the fade band so it finishes inside
the volume face, and it does so by **sliding the whole band down**, not by
truncating it (truncating would collapse a 1600 UU fade into a 40 UU one, which
is the hard ring risk 8 exists to prevent). At Dim 192:

```
HalfExtent   = 0.5 * 192 * 40                = 3840 UU
MaxFadeEnd   = max(2*320, 3840 - 320)        = 3520 UU
Shift        = FadeEnd(6400) - 3520          = 2880 UU
→ FadeEnd    = 3520 UU        (35.2 m)
→ FadeStart  = 4800 - 2880    = 1920 UU  (19.2 m)
```

So GI is at full strength only to **19.2 m**, fading out entirely by **35.2 m**,
against a nominal 38.4 m box. Any reasoning about what the volume can reach must
use those two numbers. `VoxelGI.cpp:1008-1009` logs the requested-vs-granted fade
whenever it moves, precisely so a fired clamp is greppable — check it before
believing a reach figure.

Volumes after this work, at the shipping Dim 192, 40 UU cells:

| Volume | Content | Changes when | Size |
|---|---|---|---|
| `VolumePos`, `VolumeNeg` | **Vis[±XYZ] and validity** — unchanged in content, since the stored value is already pure visibility (§2.2) | geometry (a dig) | 56.6 MB VRAM + 56.6 MB CPU mirror (existing) |
| `VolumeBounce` (new, R8) | scalar indirect term | geometry | **+7.1 MB** VRAM, and it needs a CPU mirror too |

`SkyRadiance[6]` moves into the `VoxelGIVol` uniform buffer, rebuilt per frame —
it is already rebuilt whenever any input changes (`VoxelGIVolume.h:23-32`), so
this adds no new machinery. It must be normalised per §2.3: an unoccluded cell
still evaluates to 1.0.

Any new volume added at Dim 192 costs **~7.1 MB each** as R8, not the 16.8 MB the
first draft quoted (that figure was an R8 volume at 256³). Add the same again for
its CPU mirror, which the first draft omitted entirely and which the
`VolumeCheck` harness depends on existing.

**Bounce stays scalar deliberately.** Directional bounce would be another 56.6 MB
for a low-frequency term. If the split shows bounce is visibly directional, that
is a follow-up, not a starting point.

**VRAM is a much smaller risk than the first draft made it.** ~64 MB against
today's ~57 MB, not 168 against 134. The design doc's escalation ladder (N=192,
then a two-level clipmap) is largely moot — **192 is already the shipping
value**, taken for coverage reasons, not memory ones.

**Parity gate — the first draft's gate cannot pass.** It asked for
**pixel-identical** output with radiance pinned. Splitting one quantised byte
into two separately-quantised bytes recombined in a shader introduces
**±1–2/255 by construction**; there is no way to write the code such that the
gate passes. Specify a numeric bar instead:

> Extend `voxel.GI.VolumeCheck` (`VoxelGI.cpp:177-189`) to compare the split
> encoding against the field, and require **RMS < 1/255** over solved cells with
> the sky radiance pinned to the constant 1.0. That harness already exists, is
> already the primary evidence for the current encoding
> (`gpu-gi-volume-design.md` step 2), and already reports mean/max absolute error
> in irradiance bytes. Pixel diffs are the secondary check, quoted against the
> **1.81% between-session noise floor** (§7.1), not against zero.

### 5.3 Sun shadows: CSM only, cadence-tuned (Part C)

> **Decision #6 was revised here.** The first draft specified *"cascades near,
> volume-traced far"*, with the light field's opacity pyramid marched toward the
> sun into a new `VolumeSunVis`, cross-faded with CSM at a blend band. **That is
> deleted. It is geometrically impossible**, for reasons worth recording so
> nobody proposes it again:
>
> - **There is no "far" region for the volume to serve.** The volume's usable
>   reach is 19–35 m (§5.2) and its hard face is 38.4 m. Cascade 0 alone covers
>   more than that; the cascade set reaches hundreds of metres to kilometres. The
>   volume ends *inside* cascade 0. The proposed blend band would sit at 35 m,
>   handing "far" shadows to a structure that stops before the first cascade does.
> - **Raising `VolumeDim` cannot fix it.** `VoxelGIVolume::GetDim` clamps to 256
>   (`VoxelGIVolume.cpp:66`), which is ±51.2 m. Still inside cascade 0.
> - **The occluders do not exist out there anyway.** Bricks are only built inside
>   `voxel.GI.RadiusUU` = 7000 UU (`VoxelGI.cpp:64-65`), i.e. **70 m**. Past that
>   the field is empty by construction. Distant terrain can never shadow anything
>   through this structure at any volume size, because there is nothing in the
>   structure to shadow with. A mountain 3 km away casting a shadow at dawn is not
>   reachable by this mechanism at all.
> - **The cone march is not reusable machinery.** The first draft said the sun
>   solve *"is the same cone march the field already runs"*. It is not, twice
>   over. The march is **inlined and private** inside `SolveBrickInternal`
>   (`VoxelLightField.cpp:716-772`) — there is no function to call — and
>   `SampleOpacity` is private too (`VoxelLightField.h:388,395`). And a sun ray
>   has ~zero aperture, so `Radius = T * ConeTanHalfAperture` stays ~0, the mip
>   selection at `VoxelLightField.cpp:730-732` pins to level 0, and the march
>   steps **40 UU** the whole way instead of widening with distance. That is
>   roughly **10× the step count** of the 90° cones the field's ~12-step budget
>   was designed around.
>
> Recording this because the idea is intuitively appealing and the numbers are
> the only thing that kills it.

**What ships instead: UE cascades, kept, with the update cadence tuned.** The
pooled proxy already feeds them. Three things a moving sun changes: cascade
transforms re-render as the sun rotates (cap the update cadence — the sun moves
~0.3°/s at a 20-minute day, so a fixed update rate is invisible); grazing-angle
acne on voxel faces at dawn (normal-offset bias, tuned per face — voxel normals
are axis-aligned, which makes this easier than usual); and low-sun cascade range.

> Set cascade count on the **light component** (`DynamicShadowCascades`), not via
> `r.Shadow.CSM.MaxCascades`. `VoxelEarthGameMode.cpp:109-127` records that the
> cvar route was tried and **moved nothing** — the gather census stayed at
> 5/frame. `-VoxelShadowCascades=<N>` is the honest control and already exists.

**The measurement that cleared cascades does not transfer to a moving sun.**
`docs/backlog.md` §0 reports `shadowGather=0` and ~1.03 gathers/frame and
concludes *"the pool re-gathered 4–5× for shadows hypothesis is dead."* That was
measured with **the sun frozen at `(-45°, 30°)` since spawn** — the rig sets the
rotation once in `BeginPlay` (`VoxelEarthGameMode.cpp:103`) and nothing ever
moves it. A static movable directional light lets the engine cache whole-scene
shadow depths across frames; a sun that rotates every frame invalidates that
cache and re-renders the pool into every cascade, every frame. **The number that
cleared cascades was measured under exactly the condition this plan removes.**

⇒ **This was the largest unmeasured GPU risk in the plan** (§8 risk 8). **Measured
and answered 2026-07-29 — see P1.5 in §6 and `docs/status.md`'s 2026-07-29
entry.** `tools/voxel-sun-arms.ps1` alternated FROZEN vs MOVING arms on real
terrain: p50 15.18 ms identical, p95 25.31 vs 25.32 ms (0.01 ms apart), at
`voxel.Sky.ShadowUpdateHz=10` (the shipped default, not the uncapped case). A
moving sun costs nothing measurable under that cadence cap. The mitigation
this section proposed — cap the cadence rather than pay per frame — is
therefore not a fallback, it is the configuration that was actually measured;
an uncapped rotating light (`voxel.Sky.ShadowUpdateHz=0`) remains unmeasured.

**Far shadows are deferred, not solved.** The 50 km vista gets no cast shadows
under this plan.

> **Option not taken, recorded for whoever revisits it.** The only mechanism that
> can reach the vista is an **analytic horizon term in the clipmap material**:
> `M_VoxelClipmap` already samples the heightfield it renders, so it can compute
> a cheap horizon angle in the sun's azimuth and shade the terrain that lies
> below it. No new data structure, no new volume, works at any distance, and it
> costs a few instructions in a material that is already generated by a script
> (`Tools/create_clipmap_material.py`). It gives soft terrain self-shadowing at
> dawn and dusk — which is the only distance shadow anyone will notice — and
> nothing else. It is not in scope here, but it is the right answer if the vista
> looks flat at low sun.

### 5.4 Local lights and emissive voxels (Part C)

**What it buys:** night and caves are worth carrying a torch through, instead of
being an inconvenience the ambient floor papers over.

**Hero lights (≤8 nearby): ships. Everything else here is blocked.**

- **Hero lights (≤8 nearby):** ordinary UE point/spot lights. The pooled proxy is
  a normal primitive, so this works today with no new code. Player torch,
  headlamp, the explosive flash. This is the whole of what P6 in §6 delivers.
- **The long tail — BLOCKED, and the first draft did not say so.** It specified
  *"an additive RGB injection volume (~64³ at 160 UU) splatted from a light
  list, occluded by sampling the opacity pyramid."* Every word of that is about
  producing the value. **There is no path from that value to a pixel.** Neither
  `M_VoxelTerrain` nor `M_VoxelClipmap` has an emissive or additive input on the
  shipping path — `terrain_material_common.py` connects `MP_EMISSIVE_COLOR` only
  under `VOXEL_MATERIAL_DEBUG` (`:344-378`), and that path also flips the shading
  model to unlit. And the vertex factory cannot substitute: it owns
  `VertexColor`, so the only thing it can do to shading is **scale `.g`**
  (`VoxelQuadVertexFactory.ush:579`), which darkens. Adding light is not
  expressible. This work is blocked on **designing an additive path** — a real
  material change with a real graph decision behind it, not plumbing. Do not
  schedule it as if it were.
- **Emissive voxels** are harvested at mesh time through the hook the GI ingest
  already uses — `ApplyMeshResult` → `NotifyPooledChunkMeshUpdated`
  (`VoxelGI.h:43-47`). Material id → emissive colour, so lava lights its own
  cavern with no authoring. Same blocker: the harvest is easy, the delivery is
  not.

**`voxel.GI.AmbientFloor` must NOT become time-varying. The first draft was
dangerous here.** It said the floor *"becomes time-varying — that is what makes
night dark rather than dim... the floor at night should approach zero."*

Follow that through the arithmetic. `AmbientFloor` (0.06 today,
`VoxelGI.cpp:51-52`) is the low end of
`Ambient = lerp(AmbientFloor, 1.0, saturate(Irr))`
(`VoxelQuadVertexFactory.ush:573`), which multiplies `VertexColor.g`, which
multiplies **albedo** (§2.3). A floor near zero makes `BaseColor` near black.

⇒ **A torch in a "genuinely dark" cave would light nothing.** A torch is a
deferred point light; its contribution to the pixel is proportional to
`BaseColor`. Multiply the albedo to zero and the torch illuminates a black
surface and returns black. The player would carry a light source that visibly
glows and casts no light on anything — and it would look like a bug in the
torch, not in the ambient floor, which is what makes this worth writing down.

The same trap in the other direction is already on record: `VoxelClipmapActor.cpp:335-340`
notes that *"every previous attempt to fix the veil by making it DARKER could not
have worked"*, because darkening albedo and fighting exposure are different
problems.

**Correct mechanism for night darkness:**

1. **Dim the lights.** Sun intensity → 0 below the horizon; moon as a second
   atmosphere light scaled by phase; SkyLight follows the atmosphere on its own.
   These are absolute quantities and they are the right place for time to live.
2. **Set the exposure** (§5.8). This is what decides whether "dark" reads as dark
   or gets normalised back to mid-grey.
3. **Leave `AmbientFloor` alone.** It exists to keep unlit geometry readable
   rather than pure black, which is a legibility floor, not a brightness control.
   If it must move at all, it moves *between environments* (cave vs surface), not
   with the clock — and even then only after checking a torch still works.

### 5.5 Sky and atmosphere (Part D)

Drive the existing rig at `VoxelEarthGameMode.cpp:100-137` from the clock. Per
§2.2 **this is the day/night cycle** — not a presentation layer on top of one:

- **Sun** — `DirectionalLight`, `SetAtmosphereSunLight(true)`, rotation driven by
  the ephemeris; intensity and colour temperature from altitude (warm and dim at
  the horizon, cool and bright at noon).
- **Moon** — a *second* directional light at `AtmosphereSunLightIndex 1`, which
  UE supports natively. Intensity scales with phase, so a new moon is genuinely
  black and a full moon is navigable.
- **SkyAtmosphere** — already correctly configured for LWC
  (`PlanetTopAtComponentTransform`, with a documented origin fix). Keep it
  exactly as is; only the sun direction changes. Aerial perspective is what sells
  the 50 km vista and it comes free with the component.
- **SkyLight** — already `SetRealTimeCaptureEnabled(true)`, so it tracks the
  atmosphere automatically. **Measured in P1.5, 2026-07-29**, alongside the CSM
  cost (§5.3, `docs/status.md`'s 2026-07-29 entry): frozen vs moving sun on the
  same terrain leg came back p50 15.18 ms identical, p95 0.01 ms apart — a
  moving sun's continuous SkyLight re-capture is folded into that same GPU
  frame number and does not move it measurably.
- **ExponentialHeightFog** with volumetric fog — density and colour from the
  weather field's humidity and precipitation. Volumetric fog is also the god-ray
  mechanism, and it is a real GPU cost: tier it.
- **VolumetricCloud** — coverage/density/type from the weather-map render target
  (§5.1). Tiers: `0` off, `1` a cheap 2D layer in the sky material, `2` full
  volumetric. **The cheapest route to tier 2 is Epic's `Volumetrics` content
  plugin** (cloud noise textures and cloud material templates), present in the
  engine at `D:\UE_5.8\Engine\Plugins\Experimental\Volumetrics` and **not
  currently enabled** — `VoxelEarth.uproject` enables three plugins and that is
  not one of them (§5.6). Enabling it is a one-line `.uproject` edit when this
  phase arrives; authoring cloud noise from scratch instead is weeks.
- **Night sky** — procedural stars in the sky material rotated by sidereal time
  and latitude (so the celestial pole sits at the right altitude), Milky Way
  band, moon disc with the phase terminator computed from the sun–moon geometry.
- **Set pieces** — lightning (a two-frame spike on the **sky light and a spawned
  directional/point light**, plus a bolt and thunder delayed by
  distance/343 m·s⁻¹. The first draft routed this through a `SkyRadiance` term in
  the GI volume; per §2.3 that would scale albedo rather than add light, so it
  goes on a light like everything else. The GI volume still does its job here —
  it is what stops the flash reaching sealed rock — but it shapes the flash, it
  does not carry it); rainbows (screen-aligned at 42°
  from the antisolar point, gated on sun-behind-camera and active rain); aurora
  gated on `|latitude| > 60` — which is a real payoff of having defined latitude
  at all, since it means flying north eventually gets you one.

### 5.6 Precipitation (Part E)

Camera-anchored Niagara systems for rain and snow, drifted by the field's wind
vector. **Occlusion comes from the sky-visibility volume** — bind `VolumePos` as
a Niagara user parameter and kill particles where the +Z visibility is near zero.
This is why no rain falls in tunnels, and it needs no distance field (the project
has none) and no collision queries.

Splashes and ripples on impact; on water surfaces, drive a ripple normal in
`M_WaterVoxel`/`M_Ocean` from precipitation intensity.

**Particles are a from-scratch subsystem, not a dependency to tick.** The first
draft said *"`VoxelEarth.uproject` lists only five plugins and Niagara is not
among them"* and treated it as a build-file line. Corrected:

- `VoxelEarth.uproject` enables **exactly three** plugins: `PythonScriptPlugin`,
  `ModelContextProtocol`, `AllToolsets`. Not five.
- Niagara is not enabled and is not a module dependency in `VoxelEarth.Build.cs`.
- The project has **zero particle infrastructure of any kind**. The only
  references to particles anywhere in `Source/` are two comments in
  `VoxelExplosive.cpp:22,201` saying *"no particles yet"*. There is no emitter,
  no system asset, no spawning code, no pooling, no lifetime management.

⇒ Enabling the plugin is the trivial part. This phase means introducing an
authoring surface the project has never had, and the `.uasset` files it produces
are **not** generated by a checked-in script the way the materials are (§5.7), so
they are the first genuinely unmergeable binary assets in the repo. That is a
reason to keep this late in the order, which §6 does.

### 5.7 Wetness and snow cover as shading (Part E)

**What it buys:** weather you can see on the world, not just in the air. Surfaces
darken and gloss when it rains; snow whitens up-facing rock. No voxel data
changes, nothing is diggable, nothing is persisted.

#### The snow system already exists. Do not build a second one

The first draft proposed a new interpolant, a material graph change and an
editor session, for a seasonal snow line. **`M_VoxelTerrain` and `M_VoxelClipmap`
both already have a fully parameterised snow term**, generated from one shared
graph (`Tools/terrain_material_common.py:285-312`, called by both
`create_voxel_material.py` and `create_clipmap_material.py:89,126`):

| Parameter | Type | Default | What it does |
|---|---|---|---|
| `SnowTempMax` | Scalar | 0.16 | temperature threshold in remapped units (≈ −4.1 °C) |
| `SnowTempFeather` | Scalar | 0.10 | width of the temperature blend |
| `SnowlineLowMeters` | Scalar | 2700 | elevation where snow starts |
| `SnowlineHighMeters` | Scalar | 2900 | elevation where snow is full |
| `SnowColor` | Vector | (0.90, 0.925, 0.96) | the snow albedo |
| `RoughnessBase` / `RoughnessSnow` | Scalar | 0.90 / 0.55 | snow smooths the surface |

`snow_w = saturate(max(snow_from_temp, snow_from_z))` — so **a seasonal snow line
is one animated scalar**, `SnowlineLowMeters` (and its High partner) driven from
`SeasonalTempOffsetC`. Not a subsystem. Not a graph change. Not an interpolant.
Both materials move together automatically because they share the graph, which
also disposes of the clipmap-seam worry for snow specifically.

> **Constraint: do NOT drive it with per-chunk `UMaterialInstanceDynamic`s.**
> That is explicit anti-doctrine. `VoxelChunkComponent.h:140-159` records that
> `ChunkMID` exists only for the always-on ring-fade params and the debug tint
> layer, that a `SetVectorParameterValue` is *"a real, non-coalescing
> render-thread command"*, and that the code goes out of its way to skip it.
> `terrain_material_common.py:336-339` says the same from the other side: *"there
> is no per-chunk MID plumbing to drive one from the command line anyway."*
> A per-chunk parameter write, on thousands of chunks, every time the snow line
> moves, is a render-thread command storm.
>
> Use a **Material Parameter Collection** (none exists in `Content/` yet — it
> would be a new generated asset, authored the same scriptable way as the
> materials) or the **single pool primitive's own MID**. One write, not N.

#### Wetness still needs a route, and TexCoord1 is a trap

Wetness has no existing parameter and does need a new per-pixel term. The first
draft's route was:

1. the factory computes `Snow`/`Wetness` in `GetMaterialPixelParameters`, gated
   on the sky-visibility volume it is already sampling one line earlier (that
   reuse is genuinely free — the sample is already in flight);
2. write to `Result.TexCoords[1]`;
3. the material reads `TexCoord(1)`.

**Two landmines in step 2, both silent:**

- **`TEXCOORD1` is already occupied.** `VoxelQuadVertexFactory.ush:50-55` carries
  `float3 GIUVW : TEXCOORD1` — the GI volume lookup. The comment there says
  *"TEXCOORD1 is free"*, which was true when it was written and is what claimed
  the slot. Taking it for wetness deletes GI.
- **The component path delivers TexCoord1 as ZERO.** This is measured, not
  assumed: `Tools/probe_texcoord1.py` exists specifically to prove it, by wiring
  `EmissiveColor = abs(TexCoord1) * 0.05` and checking the frame stays black.
  Since `voxel.Stream.GPUMaxLevel` puts both renderers in one frame, wetness via
  TexCoord1 would work on pooled rings and **silently vanish on
  component-rendered rings** — a seam that only appears when it rains, which is
  the worst possible time for it to first be noticed.

⇒ Whoever executes this picks a different channel and states which, or accepts
the component-path seam explicitly and gates wetness off there. It is a real
design decision, not a step.

#### Material changes are scriptable and headless. The MCP server cannot do them

The first draft said *"Step 3 needs the editor... the project hosts a native MCP
server — use it rather than hand-editing binary assets."* Wrong in both
directions:

- **The MCP server cannot edit materials.** `VoxelEarth.uproject` enables
  `ModelContextProtocol` + `AllToolsets`; `AllToolsets` aggregates 21 toolsets
  (`D:\UE_5.8\Engine\Plugins\Experimental\Toolsets\AllToolsets`) and **there is
  no material toolset, no asset-import tool, and no Python-exec tool** among
  them. Its useful capabilities here are **PIE control, viewport capture and log
  reading** — genuinely valuable for looking at a change, useless for making one.
- **But materials are fully generated by checked-in Python, run headlessly.**
  `Tools/create_voxel_material.py`, `create_clipmap_material.py`,
  `create_ocean_material.py`, `create_water_voxel_material.py` and the shared
  `terrain_material_common.py`, all invoked as
  `UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<file> -unattended -nop4 -nosplash`.
  So material changes **are** scriptable, headless, reviewable as a text diff, and
  automatable — through Python, not through MCP.

⇒ **Edit the script, regenerate the asset.** Never hand-edit a `.uasset` and
never treat the binary as the source of truth; it is a build artifact that
happens to be committed.

Snow coverage is driven by `PrecipIsSnow` accumulated against a **snow line**
from temperature + altitude + `SeasonalTempOffsetC`, so the line visibly climbs
and falls across the year. It is a shading term with a time constant, not stored
state — a fresh client computes the same value from the clock.

### 5.8 Exposure — the policy without which none of this is verifiable

**What it buys:** that "night" actually looks like night on a screen, and that a
screenshot gate can tell the difference.

The first draft did not mention exposure once. That is the single most likely way
this feature ships broken-but-passing, and the trap is already documented in this
repository, at length, with three measured captures behind it.

`VoxelClipmapActor.cpp:311-348` records the underground investigation. Its
conclusion:

> *"A vs B is the whole story: nothing underground is over-lit in absolute terms
> (the sun is 8 lux and the ambient is a fraction of that). What is wrong is the
> EXPOSURE. UE's histogram eye adaptation defaults to a −10..+20 EV100 clamp,
> i.e. effectively unbounded, and a lightless cave has no 18%-grey subject
> anywhere in frame — so adaptation walks the whole image up until something is
> grey."*

Apply that to a day/night cycle and the failure is exact:

- Dim the sun to zero. Auto-exposure lifts the frame until something averages 18%
  grey. **Night renders mid-grey.**
- Every screenshot gate in §6 passes anyway, because the frames *do* differ
  between times of day — the sky colour changed, the shadows moved. The diff is
  non-zero. The gate is satisfied. The feature is broken.
- "The night is too bright" then reads as a lighting bug and sends the next
  session to tune light intensities, which cannot work — the same dead end
  `VoxelClipmapActor.cpp:335-340` records for the veil.

**So the policy is part of the feature, not a polish pass:**

1. **Time-of-day drives exposure explicitly.** A curve from sun altitude to a
   target EV100, applied as `AEM_Manual` — not as histogram min/max clamps.
   `VoxelClipmapActor.cpp:372-379` records why: the clamp fields are interpreted
   through `r.EyeAdaptation.ExposureFormat`, *"so '0' is not unambiguously
   'EV100 0'"*, and the first attempt at it produced a frame that was **100.0%
   pure white**. `AEM_Manual` has no such ambiguity.
2. **Allow a bounded adaptation range around the target**, so moving from
   sunlight into a cave still adapts. Bounded, so it cannot walk night to grey.
3. **Freeze it in every headless leg.** A leg with unpinned exposure is as void
   as a leg with an unpinned clock (§7.1) — the adaptation state depends on where
   the camera has been.

> **Two unbound post-process volumes will fight, silently.** `CaveExposurePP`
> (`VoxelClipmapActor.cpp:356-384`) already exists, is already `bUnbound = true`,
> already sets `Priority = 100`, and already overrides
> `AutoExposureMethod` + `AutoExposureBias`. Its own comment says *"Above every
> default-priority volume, but the project ships none, so this is future-proofing
> rather than a fight anyone is having today."* **This plan ships one**, and the
> fight begins. Whichever volume loses is simply ignored, with no log line and no
> visual clue beyond "the exposure looks wrong sometimes."
>
> Resolve it by ownership, not by priority numbers: either the sky subsystem
> *drives* `CaveExposurePP`'s existing settings (preferred — one volume, one
> owner, the cave rig keeps its measured EV100 0 as the underground target), or
> the two are explicitly ordered and the ordering is logged whenever it changes.
> Do not add a second unbound volume and hope.

---

## 6. Phases and gates

Each phase is independently shippable and independently verifiable. Every one
carries a measurement leg; per decision #10 the legs **record** rather than gate,
except where a gate is marked hard.

**This table was reordered in the 2026-07-29 correction pass.** The first draft
put the encoding split at P2, called it *"the critical path"*, and claimed
*"P3–P8 all depend on it."* Per §2.2 nothing depends on it and it delivers no
part of the visible cycle. The order below front-loads what is visible and
cheap, and defers what is expensive, blocked, or merely refining.

| P | Phase | Gate | Status |
|---|---|---|---|
| **P0** | Clock, calendar, ephemeris, lat/long mapping, cvars, HUD readout, headless switches. **No visual change** | **Hard:** sun position within 0.5° at 4 reference checkpoints (§4). Frame time unchanged — it is game-thread only | **DONE 2026-07-29.** `VoxelEarth.Sky.SolarPosition` passes all four checkpoints; noon altitude measured 60.94° against theoretical 60.96° at 52.48°N, day-of-year 171 |
| **P1** | **The visible day/night cycle.** Drive the *existing* rig (§5.5) from the clock: sun rotation + intensity + colour temperature, moon as second atmosphere light, SkyLight, basic fog — **plus the exposure policy (§5.8)**. No GI work of any kind | Screenshot ladder at 8 times of day, **captured as N frames in ONE process** (§7.1); sunrise/sunset times correct for latitude and season. **Hard:** night is measurably darker than noon *after* exposure — a mean-luminance ladder, not eyeballs. Exposure and lighting ship together or the gate is meaningless | **DONE 2026-07-29**, with three defects found and fixed by the ladder itself (twilight rendering pure black, the moon rendering warmer than the sun, deep night rendering brighter than a moonlit midsummer midnight — see `docs/status.md`'s 2026-07-29 entry for each mechanism). Two 8-rung ladders measured (summer 06-21, winter 12-21); the exposure curve has since been refitted from those 16 points (sin(alt)^0.80, not the ^1.15 the first anchors assumed) but **that refit is itself not yet verified by a re-run** |
| **P1.5** | **Measure the moving sun.** CSM cost and SkyLight real-time-capture cost, moving vs frozen, same binary (§5.3, §8 risk 3, risk 8) | Recording, not gating — but it is the input to every later decision. GPU frame and gather census, two runs, byte-identical arguments apart from `voxel.Sky.TimeScale` | **DONE 2026-07-29 — risk retired.** `tools/voxel-sun-arms.ps1`, real terrain, alternating arms: frozen p50/p95 15.18/25.31 ms vs moving 15.18/25.32 ms — **no measurable cost**, at `voxel.Sky.ShadowUpdateHz=10` (the shipped default). That cap is the load-bearing part of the result, not an incidental detail — an uncapped rotating light was not measured |
| **P2** | **Weather field**, sampling API, debug visualisation, fog/atmosphere response. **No particles.** Promoted: it is game-thread work and the game thread is idle 75% of the frame (§2.5) | Two clients at the same (seed, time, position) sample identical weather; a front visibly crosses the world over minutes; frame time unmoved | Not started |
| **P3** | **Seasonal snow line** through the *existing* material parameters (§5.7). One animated scalar via an MPC or the pool MID — **not** per-chunk MIDs. Promoted: nearly free | The snow line visibly climbs and falls across a simulated year; near field and clipmap agree at the seam (they share the graph, so this is a check, not a build); **no new `SetScalarParameterValue` per chunk per frame** | Not started |
| **P4** | **The encoding split** (§5.2). Per-direction sky *colour* in the uniform buffer. Demoted: a second-order refinement, not the cycle | **Hard, numeric:** extend `voxel.GI.VolumeCheck` (`VoxelGI.cpp:177-189`); **RMS < 1/255** against the field with radiance pinned to 1.0. Pixel-identical is *impossible* and is not the bar (§5.2). Plus: unoccluded cells still read 1.0 at every time of day (§2.3) | Not started |
| **P5** | **Hero lights only:** player torch, headlamp, explosive flash as ordinary UE point lights (§5.4). Emissive-voxel *harvest* may land here; emissive *delivery* may not | A torch lights a cave; `MaxBricks` budget unmoved. **The long tail and the injection volume are NOT in this phase** — they are blocked on designing an additive path to the pixel (§5.4) | Not started |
| **P6** | Volumetric clouds + weather-map render target + tiers. Enable Epic's `Volumetrics` content plugin (§5.5) | Cloud cover matches sampled weather at the camera; per-tier frame cost recorded at 2K | Not started. Sky-asset fetch/import/material scripts exist (`tools/fetch-sky-assets.ps1`, `ue-project/Tools/import_sky_textures.py`, `create_sky_material.py`) but have not been run through the editor — no sky-dome actor or `M_NightSky` asset exists yet |
| **P7** | Precipitation: particle infrastructure from scratch (§5.6), occluded by the Vis volume; wetness shading once a channel is chosen (§5.7); ripples and splashes | **Hard:** no rain inside caves. Wetness present on **both** renderer paths, or explicitly gated off on the component path with a log line | Not started |
| **P8** | Set pieces: lightning + thunder, god rays, rainbows, aurora | Visual ladder. Lightning demonstrably lights a cave mouth and not sealed rock | Not started |
| **P9** | Scalability tiers, final measurement leg, backlog and status entries | Full leg at 2K against the P0 baseline; tiers 0/1/2 each measured | Not started |

**P0 → P1 is the whole day/night cycle**, and it is the shippable slice if time
is short. P1.5 is the only thing that can invalidate the plan's cost assumptions,
so it comes immediately after and before anything is built on top of it.
Nothing after P1.5 is a prerequisite for anything else, which is deliberate — the
order past that point is by value per unit of work, and it can be resequenced
without breaking anything.

---

## 7. Harness, cvars and a hard rule about time

### 7.1 Freeze time in every headless leg — non-negotiable

`voxelsim-headless-leg-harness` records that the A/B screenshot noise floor is
already **mean ~45/255** and *"does NOT shrink with settle time (water animation
+ temporal accumulation)"*. A running clock makes every capture
non-reproducible: two runs seconds apart have different sun angles, different
cloud cover, different everything.

⇒ **`voxel.Sky.TimeScale 0` and an explicit `-VoxelTimeOfDay=` in every perf and
screenshot leg.** Default the perf harness to a fixed noon so existing baselines
stay comparable. A leg that does not pin the time is void — treat this exactly
like the spawn-coordinate rule.

**And one addition this plan forces: a time-of-day ladder must be N frames from
ONE process, never N separate runs.** The screenshot noise floor is *bimodal*:
`docs/gpu-waves-plan.md:439-443` and `docs/gpu-g-compaction.md:801` both measure
**0.00% within a session and ~1.81% between sessions**, identical in magnitude
across every cross pair — a per-session latch, most likely eye adaptation
settling to a slightly different point each launch.

⇒ A ladder captured as eight separate runs puts a 1.81% floor under every rung,
which is the same order as the difference between adjacent times of day. The
ladder becomes unreadable, and — worse — it reads as *noise that happens to look
like a signal*. **`-VoxelSkyLadder=<N>` (§7.2) exists for exactly this reason:
one process, N frames, 0.00% between rungs, so any difference is real.** The same
rule applies to the P4 encoding-split comparison and to the P1 night-vs-noon
luminance gate.

### 7.2 Command line

```
-VoxelTimeOfDay=HH:MM     pin the clock
-VoxelDate=MM-DD          pin the calendar day (drives declination and season)
-VoxelTimeScale=<f>       0 to freeze; default from the cvar
-VoxelLatitude=<deg>      override the origin latitude
-VoxelWeather=<preset>    clear | overcast | rain | snow | storm
-VoxelSkyLadder=<N>       capture N screenshots evenly across one day, then quit
```

`-VoxelSkyLadder` is the workhorse: one run produces the whole day in comparable
frames, which is the only way to review a cycle without a person watching it —
and, per §7.1, the only way to get a **0.00%** floor between the rungs instead of
1.81%. It must capture from a single process; splitting it into N launches
defeats its entire purpose.

### 7.3 Cvars

```
voxel.Sky.Enabled / TimeScale / DayLengthSeconds / DaysPerYear
voxel.Sky.OriginLatitudeDeg / OriginLongitudeDeg / AxialTiltDeg
voxel.Sky.MoonEnabled / StarsEnabled / AuroraEnabled
voxel.Sky.CloudTier            0 off | 1 2D layer | 2 volumetric
voxel.Sky.VolumetricFog        god rays; off on low tiers
voxel.Sky.ExposureMode         0 engine default | 1 driven by time of day (§5.8)
voxel.Sky.ExposureEV100Curve   target EV100 vs sun altitude; the night-darkness control
voxel.Sky.ShadowUpdateHz       CSM/light rotation cadence — the P1.5 mitigation (§5.3)
voxel.Sky.SnowlineOffsetM      seasonal offset applied to the EXISTING SnowlineLowMeters/High (§5.7)
voxel.Weather.Enabled / Override / FieldScaleM / WindScale
voxel.Weather.PrecipTier       0 off | 1 particles | 2 particles + surface response
voxel.GI.SkyRadianceDir        per-direction sky COLOUR for the P4 split; normalised so
                               an unoccluded cell still reads 1.0 (§2.3)
```

Three cvars from the first draft are **deleted**, not renamed:
`voxel.GI.SunVisEnabled` / `SunVisSolvesPerFrame` / `SunVisMaxDistUU` (there is
no `VolumeSunVis` — §5.3), `voxel.GI.LightInjection` / `MaxInjectedLights` (no
additive path to the pixel — §5.4), and `voxel.GI.SkyRadianceScale` (described
as *"the successor to the fused SkyIntensity"*; nothing was fused, so there is
nothing to succeed — §2.2. A *scalar* on the GI term is precisely the §2.3
double-darkening bug, which is why the replacement above is directional and
normalised).

Every heavy feature is off-able independently, so a regression can be bisected by
cvar rather than by rebuild.

---

## 8. Risks and open questions

Ordered roughly by how much is unknown, not by how much would hurt.

1. ~~**Moving-sun CSM cost is the largest unmeasured GPU unknown in this plan.**~~
   **MEASURED AND RETIRED, 2026-07-29 (P1.5).** `docs/backlog.md` §0's
   `shadowGather=0` / ~1.03 gathers/frame number was measured with a sun frozen
   at `(-45°, 30°)` since spawn (`VoxelEarthGameMode.cpp:103`, pre-W4) and did
   not transfer by inspection alone — a rotating light busts UE's whole-scene
   shadow cache. `tools/voxel-sun-arms.ps1` alternated frozen vs moving arms on
   real terrain and found **no measurable difference**: p50 15.18 ms identical
   both arms, p95 25.31 vs 25.32 ms. The mitigation named below
   (`voxel.Sky.ShadowUpdateHz`, cadence-capped at 10 Hz by default) is what was
   actually measured — this result does not extend to
   `voxel.Sky.ShadowUpdateHz=0` (uncapped), which remains unmeasured. Mitigation
   if the uncapped case ever bites: the same cadence cap; at 0.3°/s nobody can
   see the difference.
2. **Exposure will silently defeat every visual gate** unless §5.8's policy ships
   *with* P1 rather than after it. Auto-exposure lifts a dark frame toward 18%
   grey, so night ships "dark" and renders mid-grey, and the screenshot ladder
   passes regardless because the sky colour still changed. This is not
   hypothetical — `VoxelClipmapActor.cpp:311-348` is a full write-up of the same
   mechanism costing a debug cycle underground.
3. **Two unbound post-process volumes will fight, silently** (§5.8).
   `CaveExposurePP` already exists at `Priority = 100`, `bUnbound = true`. Its own
   comment assumes *"the project ships none"* others. This plan ships one.
4. ~~**Real-time SkyLight capture with a moving sun** is an unmeasured cost~~
   **MEASURED 2026-07-29 (§5.5, P1.5), alongside risk 1, from the same pair of
   runs: no measurable cost.**
5. **Volumetric clouds and volumetric fog are the two heaviest items**, and both
   land on the GPU, which `docs/backlog.md` §0 identifies as the ceiling. The
   perf-blind decision means these may need to come back down after P6/P8. Tiers
   exist precisely so that retreat is a cvar, not a revert.
6. **The clipmap seam.** The 50 km vista uses `M_VoxelClipmap` and does not
   sample the GI volume. As lighting becomes time-varying, the near field and the
   vista will drift apart unless the clipmap tracks the same sun. *Open:* is a
   cheap analytic sky term enough out there? P1 will show it. Note the vista
   shares the snow graph already (§5.7), so snow is not part of this seam — only
   lighting is. The analytic horizon term of §5.3 would land here too.
7. **Snow line vs biome LUT.** `VoxelClimateProbe.h` warns at length that four
   independent climate calibrations drifting apart is what made the entire world
   classify as desert. The snow line is a **fifth** consumer of the same
   temperature window — and it is *already wired* (`SnowTempMax` reads
   `VertexColor.B`, the temperature channel). Animate the offset; never re-derive
   the calibration, and never hand-tune a byte literal.
8. **Multiplayer: a dedicated-server client would get a stale or unlit sky, and
   nobody tests that configuration.** Two separate reasons, both silent:
   - The light rig is spawned in `AVoxelEarthGameMode::BeginPlay`
     (`VoxelEarthGameMode.cpp:91-137`). `AGameMode` exists **only on the server**.
     A dedicated-server client never runs it and therefore has no sun, no
     SkyLight and no SkyAtmosphere unless something else provides them.
   - `UWorldSubsystem`s do not replicate. `UVoxelSkySubsystem` will tick happily
     on the client with whatever epoch it started from, and drift.

   **Fix, and specifically not by adding an actor:** move rig ownership
   client-side (spawn it from the local player controller or a client-safe
   subsystem, so every instance has one), and replicate the clock as **two
   `UPROPERTY(Replicated)` fields on the existing `AVoxelEditRelay`** — epoch and
   rate. That actor already exists, already replicates to every client, and
   already carries exactly this shape of handshake data (`ServerSeed`,
   `ServerWorldGenVersion`, `ServerProbeDigest` at `VoxelEditRelay.h:55-60`,
   which arrive before the client's own `BeginPlay` by standard actor-channel
   ordering — precisely the timing a clock needs). A second replicated actor for
   two scalars would be a new failure surface for no gain.
9. **VRAM (§5.2).** Much smaller than the first draft claimed: **+7.1 MB on a
   ~57 MB base**, not +33.6 on 134. Plus an equal CPU-side mirror, which the
   first draft omitted entirely. *Open:* nobody has measured the actual headroom
   on the 7800 XT against the 112 MB geometry pool — but at this size it is
   unlikely to be the binding constraint.
10. **Time persistence across a server restart** interacts with the edit log's
    save format, which the implementation plan lists as not-yet-designed. P0 can
    store the epoch in a sidecar; a real save format supersedes it later.

> **A risk the first draft carried that has now largely evaporated.** It listed
> *"material asset changes are binary and do not merge — do them in one sitting,
> on a branch, via the MCP editor server."* All four materials are **generated
> artifacts of checked-in Python** (`Tools/create_*_material.py`), regenerated
> headlessly with `-run=pythonscript` (§5.7). The mergeable source is the script;
> the `.uasset` is a build output that happens to be committed. So the real rule
> is much weaker and much more useful: **edit the script, regenerate, and never
> hand-edit the binary.** The one place the original warning still applies is
> particle assets (§5.6), which have no generator and would be the first genuinely
> unmergeable binaries in the tree.

### Explicitly out of scope

Named so a later session does not assume they were forgotten. Each was
considered and declined:

- **Snow accumulating as real, diggable voxels** — would make weather an
  edit-log author and a persistence problem. Needs an ADR.
- **Rain feeding the water CA** — would make weather load-bearing for flooding
  and put it inside the determinism boundary. Needs an ADR.
- **Wind affecting physics, debris or projectiles** — same reason.
- **Screen-space rain-on-lens effects** — considered and rejected; the look stays
  world-anchored.
- **Seasonal vegetation** (leaf colour, bare trees) — belongs with M4 vegetation,
  which does not exist yet. The calendar this plan adds is the input it will use.
- **Audio propagation of thunder through voxel space** — thunder gets a distance
  delay only. Audio propagation is its own open question in the implementation
  plan §6.

Added by the 2026-07-29 correction pass — these were *in* the first draft and are
now out, with reasons, so nobody re-derives them:

- **Volume-traced far sun shadows (`VolumeSunVis`)** — geometrically impossible.
  The volume stops at 38 m, inside cascade 0, and occluders only exist inside the
  70 m build radius. §5.3 has the full argument and names the analytic horizon
  term as the only mechanism that reaches the vista.
- **The additive light-injection volume and the long tail of local lights** —
  **blocked, not declined.** There is no additive or emissive path from any
  volume to the pixel on the shipping materials, and the vertex factory can only
  scale `.g`. Reopen it by designing that path first (§5.4).
- **Time-varying `voxel.GI.AmbientFloor`** — would crush albedo toward black and
  leave a torch lighting nothing. Night darkness comes from the lights and the
  exposure (§5.4, §5.8).
- **A new `TexCoord1` interpolant for snow** — unnecessary; the snow term already
  exists and is parameterised on both terrain materials (§5.7). And `TEXCOORD1`
  is taken by `GIUVW` in any case.

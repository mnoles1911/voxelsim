# Day/night, weather and lighting — implementation plan

Written 2026-07-29. Scoped in one session against the code as it stands after
PR #182 (`claude/phase3-integration`). Design only — nothing here is
implemented yet. `file:line` references were current at the time of writing and
are worth re-checking before leaning on one.

This plan closes the item the implementation plan has carried unanswered since
July: §6 *"Weather & seasons (climate channels support it)"*.

---

## 0. Read this before starting

The executing session starts cold. Read, in order:

| Document | Why |
|---|---|
| `docs/voxel-earth-implementation-plan.md` §2 (doctrine), §3.1, §4 | The determinism doctrine this plan must not violate, and where M4 sits |
| `docs/gpu-gi-volume-design.md` | The GI volume's design. **Partly stale — see §2.1** |
| `ue-project/Source/VoxelEarth/VoxelLightField.h` | The ambient-cube formulation, in full, with its own rationale |
| `docs/backlog.md` §0 | Where the frame time actually goes. Everything visual lands on the full side |
| `docs/lessons-2026-07-27-s0-s1.md` | The measurement discipline this plan's gates inherit |

Critical files this plan touches:

```
ue-project/Source/VoxelEarth/VoxelEarthGameMode.cpp:100-170   the light rig, spawned from code
ue-project/Source/VoxelEarth/VoxelLightField.h/.cpp           the ambient cube; the solve at .cpp:771
ue-project/Source/VoxelEarth/VoxelGI.cpp                      budgets, cvars, the ingest hooks
ue-project/Source/VoxelEarthShaders/Public/VoxelGIVolume.h    the GPU volume's uniform buffer
ue-project/Source/VoxelEarthShaders/Private/VoxelGIVolume.cpp the volume itself
ue-project/Shaders/VoxelQuadVertexFactory.ush:445-600         where GI is recombined per pixel
ue-project/Source/VoxelEarth/VoxelClimateProbe.h              temperature/precipitation, already calibrated
ue-project/Content/Voxel/M_VoxelTerrain.uasset                needs a graph change in P7 (see §5.7)
```

New files this plan adds: `VoxelSkySubsystem.*`, `VoxelEphemeris.*`,
`VoxelWeather.*`, `VoxelWeatherField.*`, `VoxelLightInjection.*`.

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
| 6 | **Sun shadows: cascades near, volume-traced far** | Crisp contact shadows where the eye checks them, cheap traced shadows out to the 50 km vista where cascades cannot reach |
| 7 | **Night is genuinely dark, with emissive voxels and placed lights** | Otherwise the cycle only recolours the sky. Requires light injection, which the field has none of today |
| 8 | **Precipitation**: camera-anchored Niagara + wetness/snow as voxel shading + rain ripples on water. **No screen-space lens effects** | Explicitly not selected — the look stays world-anchored, not camera-anchored |
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

## 2. Three findings that shape the work

### 2.1 The GPU GI volume is BUILT. The design doc says it isn't

`docs/gpu-gi-volume-design.md` opens with *"Design only; nothing here is
implemented yet"*. That was true on 2026-07-25 and is false now.
`VoxelGIVolume.h/.cpp` exists, `VoxelQuadVertexFactory.ush` carries a `GIUVW`
interpolant and samples the volume per pixel with multi-step probe fallback
(`:481-590`), and the volume shipped as **Scheme A** — two RGBA8 3D textures,
`(Vis[+X],+Y,+Z,v)` and `(Vis[−X],−Y,−Z,v)` — not the Scheme B the doc
recommends. The header records why: Scheme B *"missed the design's own RMS bar
by 2.6x, worst on exactly the cave walls the feature exists for."*

**Consequence: the single largest item in this plan was already paid for.** The
day/night work is not "build a GI volume"; it is "make the volume that exists
respond to time." That is a much smaller job, and it changes the phase order —
the encoding split (P2) can start almost immediately.

**First action for the executing session: correct the stale header of
`gpu-gi-volume-design.md`** so the next cold reader is not misled the same way.

### 2.2 Visibility and radiance are fused at solve time. This is the crux

`VoxelLightField.cpp:771`:

```cpp
ConeIrr[D] = FMath::Clamp(Vis * FMath::Min(Params.SkyIntensity, 1.f) + Bounce, 0.f, 1.f);
```

One byte per direction holding *visibility × a scalar sky intensity, plus
bounce*. Geometry-dependent and time-dependent terms, multiplied together and
thrown away as one number.

With the sun frozen at `(-45°, 30°)` that is free. With a sun that moves, it
means **every brick must be re-solved continuously** — ~2,000 resident bricks
against a budget of 8 solves per frame. The cycle would take minutes to catch
up and would saturate the solve budget forever.

**The fix is to stop multiplying.** Store visibility, which changes only when
someone digs, and supply radiance per frame as a uniform. The shader recombines:

```
Irr(N) = Σ_slots w(N,D) · Vis[D] · SkyRadiance[D](t)      // sky, per-direction colour
       + SunVis · SunRadiance(t) · max(0, N·SunDir)        // direct sun, volume-traced
       + Bounce · SkyRadianceAvg(t)                        // indirect
```

`SkyRadiance[D]` is **six float3s in the existing `VoxelGIVol` uniform buffer**,
computed on the game thread. So:

> **The entire day/night response — dawn reddening, dusk, moonlight, a lightning
> flash lighting a cave through its opening — costs one uniform-buffer update per
> frame and zero re-solves.**

That is the plan's central idea. Everything else is built on it.

It also means a lightning flash is *correct for free*: bump `SkyRadiance` for
two frames and every cave mouth, overhang and window in the volume responds with
the right directional falloff, because the visibility that describes them is
already stored.

### 2.3 All four vertex-colour channels are spoken for

`VoxelQuadVertexFactory.ush:298-305`:

| Channel | Terrain | Water |
|---|---|---|
| R | biome tint | corner height |
| G | AO, then GI folded in at `:579` | AO |
| B | climate.x (temperature) | top boundary |
| A | climate.y (precipitation) | — |

Nothing is free, and `BaseColor = albedo * VertexColor.G` can only **darken** —
snow needs to whiten. So wetness and snow cover cannot ride vertex colour and
cannot be done inside the factory alone. They need a new interpolant plus a
material graph change (§5.7). That is the only part of this plan that touches a
`.uasset`, and it is the only part that needs the editor.

### 2.4 The perf shape, and what it implies about where to put things

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
               │                 │  Vis volume (exists) + SunVis │
               │                 │  + light injection            │
               │                 └────────────┬──────────────────┘
               │                              │  sky visibility
  ┌────────────▼──────────────────────────────▼──────────────────┐
  │ E. Precipitation & surface response                          │
  │    Niagara occluded BY the Vis volume; snow placed BY it      │
  └──────────────────────────────────────────────────────────────┘
```

**The load-bearing reuse:** the sky-visibility volume answers "can the sky see
this point?" — which is simultaneously the GI question, the *does rain reach
here* question, the *does snow settle here* question, and the *is this cave dark*
question. One asset, four consumers, guaranteed consistent. Rain will not fall
inside a tunnel because the same data that darkens the tunnel kills the
particles.

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
- **Calendar.** `voxel.Sky.DayLengthSeconds` (default 1200 = a 20-minute day),
  `voxel.Sky.DaysPerYear` (default 48 — a season passes in roughly four hours of
  play), `voxel.Sky.AxialTiltDeg` (23.44).
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

### 5.2 The encoding split (Part C, the spine — see §2.2)

Volumes after this work, at the current N=256, 40 UU cells:

| Volume | Content | Changes when | Size |
|---|---|---|---|
| `VolumePos`, `VolumeNeg` | **Vis[±XYZ] and validity** — sky visibility, radiance removed | geometry (a dig) | 134 MB (existing) |
| `VolumeBounce` (new, R8) | scalar indirect term | geometry | +16.8 MB |
| `VolumeSunVis` (new, R8) | visibility toward the sun | geometry, and slowly as the sun moves | +16.8 MB |

`SkyRadiance[6]`, `SunRadiance`, `SunDirection` move into the `VoxelGIVol`
uniform buffer, rebuilt per frame — it is already rebuilt whenever the origin
moves (`VoxelGIVolume.h`), so this adds no new machinery.

**Bounce stays scalar deliberately.** Directional bounce would be another
134 MB for a low-frequency term. If the split shows bounce is visibly
directional, that is a follow-up, not a starting point.

**VRAM is the real risk here: ~168 MB against today's 134 MB.** The design doc
already names the escalation ladder — N=192 (a smaller reach), then a two-level
clipmap (*half* the memory and 2.5× the reach, but needs an irradiance
downsample the field does not have). Do not pre-emptively take either; measure
first, and take N=192 before the clipmap.

**Parity gate:** with `SkyRadiance` pinned to the old constant `SkyIntensity` and
the sun frozen at `(-45°, 30°)`, the split build must render **pixel-identical**
to the current build. This is a pure refactor until that passes. The existing
`-VoxelGIConverge` energy harness (`VoxelGI.cpp:275-323`) is the right tool.

### 5.3 Sun shadows: cascades near, traced far (Part C)

**Near** — UE cascades, kept. `docs/backlog.md` already cleared them as a cost
(`shadowGather=0`, ~1.03 gathers/frame) and the pooled proxy already feeds them.
Three things a moving sun changes, none fatal: cascade transforms re-render as
the sun rotates (cap the update cadence — the sun moves ~0.3°/s at a 20-minute
day, so a fixed update rate is invisible); grazing-angle acne on voxel faces at
dawn (normal-offset bias, tuned per face — voxel normals are axis-aligned, which
makes this easier than usual); and low-sun cascade range.

> Set cascade count on the **light component** (`DynamicShadowCascades`), not via
> `r.Shadow.CSM.MaxCascades`. `VoxelEarthGameMode.cpp:110-126` records that the
> cvar route was tried and **moved nothing** — the gather census stayed at 5/frame.

**Far** — march the opacity pyramid the light field already maintains toward the
sun, writing `VolumeSunVis`. Beyond the last cascade this costs no shadow maps at
all and matches the voxel world exactly. Re-solve amortized: the sun moves slowly
enough that a rolling budget (`voxel.GI.SunVisSolvesPerFrame`) keeps up, and the
solve is the same cone march the field already runs.

> **Carry the MAX-aggregation invariant into the sun march.** `VoxelLightField.h`
> chose MAX over average precisely because *"VCT's classic failure mode is light
> LEAKING through thin walls once they blur out at coarse mips; for a game about
> digging tunnels, erring toward 'too dark' is the correct side to be wrong on."*
> A sun ray that leaks through a coarse mip puts a sunbeam inside solid rock.

**The seam** is a blend band around the last cascade distance — cross-fade CSM
into traced visibility over a few hundred UU. Verify by walking the band, not by
screenshotting either side of it.

### 5.4 Local lights and emissive voxels (Part C)

**What it buys:** night and caves are worth carrying a torch through, instead of
being an inconvenience the ambient floor papers over.

Same doctrine as the sun — crisp near, cheap far:

- **Hero lights (≤8 nearby):** ordinary UE point/spot lights. The pooled proxy is
  a normal primitive, so this works today with no new code. Player torch,
  headlamp, the explosive flash.
- **The long tail:** an additive RGB injection volume (~64³ at 160 UU, covering
  the same reach) splatted from a light list, occluded by sampling the opacity
  pyramid. Soft, cheap, and correct through cave mouths. Hundreds of torches,
  lava glow, campfires.
- **Emissive voxels** are harvested at mesh time through the hook the GI ingest
  already uses — `ApplyMeshResult` → `NotifyPooledChunkMeshUpdated`
  (`VoxelGI.h`). Material id → emissive colour, so lava lights its own cavern
  with no authoring.

`voxel.GI.AmbientFloor` (0.06 today) becomes time-varying — that is what makes
night *dark* rather than dim. Decision #7 says genuinely dark; the floor at night
should approach zero and let the moon do the work.

### 5.5 Sky and atmosphere (Part D)

Replace the frozen rig at `VoxelEarthGameMode.cpp:100-170`:

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
  atmosphere automatically. **Measure it**: real-time capture with a *moving* sun
  re-captures continuously, which is not the same cost as with a static one. An
  arm for this already exists in the repo's history (`C-noskylight.stdout`).
- **ExponentialHeightFog** with volumetric fog — density and colour from the
  weather field's humidity and precipitation. Volumetric fog is also the god-ray
  mechanism, and it is a real GPU cost: tier it.
- **VolumetricCloud** — coverage/density/type from the weather-map render target
  (§5.1). Tiers: `0` off, `1` a cheap 2D layer in the sky material, `2` full
  volumetric.
- **Night sky** — procedural stars in the sky material rotated by sidereal time
  and latitude (so the celestial pole sits at the right altitude), Milky Way
  band, moon disc with the phase terminator computed from the sun–moon geometry.
- **Set pieces** — lightning (§2.2: a two-frame `SkyRadiance` spike, plus a bolt
  and thunder delayed by distance/343 m·s⁻¹); rainbows (screen-aligned at 42°
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

**Confirm Niagara is enabled.** `VoxelEarth.uproject` lists only five plugins and
Niagara is not among them; it is an engine default, but the module dependency in
`VoxelEarth.Build.cs` will need adding.

### 5.7 Wetness and snow cover as shading (Part E)

**What it buys:** weather you can see on the world, not just in the air. Surfaces
darken and gloss when it rains; snow whitens up-facing rock. No voxel data
changes, nothing is diggable, nothing is persisted.

This is the one part that needs a `.uasset` change (§2.3). Route:

1. The vertex factory computes `Snow` and `Wetness` in
   `GetMaterialPixelParameters` from world position, face normal (up-facing only)
   and the weather uniform buffer — **and gates snow on the sky-visibility volume
   it is already sampling one line earlier**, so snow does not appear under
   overhangs or inside caves. That reuse is free; the sample is already in flight.
2. It writes them to `Result.TexCoords[1]`, alongside the existing
   `TexCoords[0]`. `NUM_MATERIAL_TEXCOORDS` must be ≥ 2.
3. `M_VoxelTerrain` reads `TexCoord(1)`, lerps albedo toward a snow albedo and
   modulates roughness/specular for wetness.

Step 3 needs the editor. The project hosts a native MCP server at
`http://127.0.0.1:8000/mcp` (auto-start, `.mcp.json` at repo root) — use it
rather than hand-editing binary assets. `M_VoxelClipmap` needs the same treatment
or the 50 km vista will disagree with the near field at the seam — the same
failure mode `VoxelClimateProbe.h` was written to prevent for climate.

Snow coverage is driven by `PrecipIsSnow` accumulated against a **snow line**
from temperature + altitude + `SeasonalTempOffsetC`, so the line visibly climbs
and falls across the year. It is a shading term with a time constant, not stored
state — a fresh client computes the same value from the clock.

---

## 6. Phases and gates

Each phase is independently shippable and independently verifiable. Every one
carries a measurement leg; per decision #10 the legs **record** rather than gate,
except where a gate is marked hard.

| P | Phase | Gate |
|---|---|---|
| **P0** | Clock, calendar, ephemeris, lat/long mapping, cvars, HUD readout, headless switches. **No visual change** | **Hard:** sun position within 0.5° at 4 reference checkpoints (§4). Frame time unchanged — it is game-thread only |
| **P1** | Sky follows the sun: directional light, moon as second atmosphere light, SkyAtmosphere, SkyLight, basic fog | Screenshot ladder at 8 times of day; sunrise/sunset times correct for latitude and season; SkyLight real-time-capture cost measured against a static-sun control |
| **P2** | **The encoding split** (§5.2). Vis and radiance separated; sky radiance in the uniform buffer | **Hard:** pixel-identical to the current build with radiance pinned and sun frozen. Then: a full day/night cycle with **zero** brick re-solves |
| **P3** | `VolumeSunVis`, traced far shadows, CSM near, the blend band | **Hard:** no light leak into sealed caves at any sun angle. Seam invisible walking through the blend band |
| **P4** | Local lights: emissive harvest, injection volume, hero UE lights, time-varying ambient floor | A torch lights a cave; the same cave without one is genuinely dark; `MaxBricks` budget unmoved |
| **P5** | Weather field, sampling API, debug visualisation, fog/atmosphere response. **No particles yet** | Two clients at the same (seed, time, position) sample identical weather; a front visibly crosses the world over minutes |
| **P6** | Volumetric clouds + weather-map render target + tiers | Cloud cover matches sampled weather at the camera; per-tier frame cost recorded at 2K |
| **P7** | Precipitation: Niagara occluded by the Vis volume; wetness/snow shading (factory + both materials); ripples and splashes | **Hard:** no rain inside caves. Snow only on up-facing, sky-visible surfaces. Near field and clipmap agree at the seam |
| **P8** | Set pieces: lightning + thunder, god rays, rainbows, aurora | Visual ladder. Lightning demonstrably lights a cave mouth through the volume, not by a global flash |
| **P9** | Scalability tiers, final measurement leg, backlog and status entries | Full leg at 2K against the P0 baseline; tiers 0/1/2 each measured |

**P2 is the critical path.** P1 is presentable without it but will lag visibly.
P3–P8 all depend on it. If time is short, P0→P2 alone is a coherent, shippable
day/night cycle.

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
frames, which is the only way to review a cycle without a person watching it.

### 7.3 Cvars

```
voxel.Sky.Enabled / TimeScale / DayLengthSeconds / DaysPerYear
voxel.Sky.OriginLatitudeDeg / OriginLongitudeDeg / AxialTiltDeg
voxel.Sky.MoonEnabled / StarsEnabled / AuroraEnabled
voxel.Sky.CloudTier            0 off | 1 2D layer | 2 volumetric
voxel.Sky.VolumetricFog        god rays; off on low tiers
voxel.Weather.Enabled / Override / FieldScaleM / WindScale
voxel.Weather.PrecipTier       0 off | 1 particles | 2 particles + surface response
voxel.GI.SunVisEnabled / SunVisSolvesPerFrame / SunVisMaxDistUU
voxel.GI.LightInjection / MaxInjectedLights
voxel.GI.SkyRadianceScale      the successor to the fused SkyIntensity
```

Every heavy feature is off-able independently, so a regression can be bisected by
cvar rather than by rebuild.

---

## 8. Risks and open questions

1. **VRAM (§5.2).** +33.6 MB on a 134 MB base. If it does not fit, take N=192
   before the clipmap. *Open:* nobody has measured the actual headroom on the
   7800 XT against the 112 MB geometry pool.
2. **Volumetric clouds and volumetric fog are the two heaviest items**, and both
   land on the GPU, which `docs/backlog.md` §0 identifies as the ceiling. The
   perf-blind decision means these may need to come back down after P6/P8. Tiers
   exist precisely so that retreat is a cvar, not a revert.
3. **Real-time SkyLight capture with a moving sun** is an unmeasured cost
   (§5.5). Cheap to measure in P1; do it there rather than discovering it in P9.
4. **Material asset changes** (§5.7) are binary and do not merge. Do them in one
   sitting, on a branch, via the MCP editor server, and land them before any
   parallel work touches the same assets.
5. **The clipmap seam.** The 50 km vista uses `M_VoxelClipmap` and does not
   sample the GI volume. As lighting becomes time-varying, the near field and the
   vista will drift apart unless the clipmap gets at least the sky-radiance term.
   *Open:* is a cheap analytic sky term enough out there, or does the vista need
   real visibility? P1 will show it.
6. **Snow line vs biome LUT.** `VoxelClimateProbe.h` warns at length that four
   independent climate calibrations drifting apart is what made the entire world
   classify as desert. The snow line is a **fifth** consumer of the same
   temperature window. Derive it from `voxelcore/climate.h` like the others —
   never from a hand-tuned byte literal.
7. **Time persistence across a server restart** interacts with the edit log's
   save format, which the implementation plan lists as not-yet-designed. P0 can
   store the epoch in a sidecar; a real save format supersedes it later.

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

# Vintage-Story-class lighting for VoxelEarth — implementation plan

Owner directive (2026-09-06, in session): "Write a plan to implement the rest
of Vintage Story's lighting and illumination approach in our own game...
Use GPU where possible for better game performance and optimization over
Vintage Story system."

Foundation: docs/vintage-story-lighting-research-2026-09-05.md (the VS system,
source-linked) and the shipped L1 below. Doctrine constraints that bind every
phase: ADR-0006 invariants 3-5 (lighting is DISPLAY ONLY — no gameplay system
may ever read the light volume; it is per-client cosmetic state per the F7
boundary doc), the house verification rules (byte-identical off arms,
engagement counters that can fail, image-before-timing, owner judges frames),
and the perf envelope (owner config 65% internal res, ~9-11 ms frame; the
whole lighting stack gets a stated budget below).

## What VS does, in one paragraph (the target)

Per-voxel sunlight (32 levels) + block light (32 levels, hued), CPU
flood-filled with per-block absorption, written per-vertex with corner AO so
faces carry gradients; face response bounded by a hard floor + half-Lambert
wrap + up-face sky boost (max ~3:1 daylit contrast); shadows only ever remove
up to half the light. The FEEL comes from light propagating into shade and
the bounded contrast — not from shadow maps.

## Phase L1 — bounded face response. SHIPPED 2026-09-05, owner judging.

The VS wrap/floor/sky-boost formula composed as an emissive deficit in the
marcher (converges exactly to UE's sun on lit faces; byte-identical off arm).
Cvars: voxel.March.VSLighting (1), SunWrapFloor (0.34), SkyBoost (0.95),
SunWrapGain (6.0), ShadowFloor (0.4). Remains in this plan only as the
directional/colour shaping layer the later phases feed.

## Phase L2 — day-night light colour (VS's SunLightLevels[] ramp)

The sky rig paints day-night with GPU atmosphere transmittance and exposes no
CPU-readable colour (research doc, rec-2 skip note). Build the publisher:
UVoxelSkySubsystem samples its own sun colour/intensity ramp at the current
epoch (a small authored table, VS-style: warm dawn/dusk, neutral noon, cool
moonlight floor) and publishes SunColour + AmbientColour + a moon fraction to
the marcher uniforms (the VoxelMarchPublishSunDirection precedent) and to
MPC_VoxelSky for materials. L1's wrap and ambient tint by these instead of
neutral white. GATES: colour ramp table test (pure function of epoch);
off arm = white (byte-identical); dawn/noon/dusk/night owner captures.

## Phase L3 — THE HEADLINE: GPU-propagated sunlight volume

VS's flood-fill, moved to where our data already lives.

- **Storage**: a light volume covering the near field — target cascade rings
  R0-R2 (~200 m) at 0.5-1.0 m cells — as a 3D texture / structured buffer
  beside the brick pool the marcher already binds. 8 bits/cell v1: 6-bit sun
  level + 2 spare (L4 claims them). Camera-following with ring recentre like
  the GI volume; far field falls back to L1+L2 (the bounded response is the
  distance-independent floor, so the seam is soft by construction).
- **Propagation, GPU compute, amortized**:
  1. SEED pass: top-down column sweep sets full sun on sky-visible cells
     (the brick occupancy the pool already holds answers solid/air).
  2. RELAX passes: iterative 6-neighbour max-minus-absorption diffusion
     (VS semantics), N iterations/frame budgeted (dirty-region prioritised;
     a fresh recentre floods over several frames — light "settling in" is
     acceptable and VS chunks pop the same way).
  3. DIRTY tracking rides the existing brick upload/edit paths: a brick
     upload or voxel edit marks its cells + a halo; the relax queue drains
     worst-first. Engagement counters: cellsRelaxed/frame, dirtyQueue depth,
     seedColumns/frame — a leg greps them and zero-on-a-moving-leg FAILS.
- **Consumption**: the marcher samples the volume trilinearly at the hit
  point (+ small normal offset) and the sampled sun level SCALES the L1
  response's ambient/wrap share (sky light wraps into overhangs — the VS
  look); the flat AmbientIntensity becomes the volume's out-of-range
  fallback. Water/underwater materials read the same volume via a small MPC
  window contract later if wanted (stretch, stated not promised).
- **Budget**: <= 1.0 ms/frame amortized GPU for relax+seed at v1 scale,
  measured by leg before/after; the volume upload rides the existing pool
  residency machinery. Escape hatch: a settings row (Lighting Quality) maps
  volume reach 0/100/200 m.
- **Doctrine**: display-only, per-client (F7 boundary doc gets an entry);
  absent volume == today's picture (off arm byte-identical, the standing
  rule); no HLSL worldgen mirror implications (this reads bricks, never
  generates them — kWorldGenVersion untouched).

## Phase L4 — block light, hued (VS's second channel)

Same volume's spare bits (v1: 2-bit intensity + a small palette, honest about
coarseness; v2 widens to a second texture if owner wants full hue). Seeds =
emissive voxels/materials (lava, future torches) injected in the seed pass;
max-not-sum mixing with sunlight per VS. Warm block light against cool night
sky = the VS night look. Gated behind L3 shipping + owner appetite.

## Phase L5 — integration polish

Corner-AO strength re-tune against the new floors (AO supplies the "earned"
darkness); AmbientIntensity retirement or re-doc (its 1.5-era help text is
stale by its own admission); clipmap far-field ambient follows via the
already-authored MarchAmbient* MPC params so the 8.2 km seam tracks every
retune; contrast-bound QA pass at the two dossier harshness poses + a night
pose; final owner judgment session sets defaults.

## Execution

Opus sub-agent lane (owner-directed): L2 first (small, self-contained,
publisher + tint), then L3 scaffolding (volume resource + seed pass + relax
kernel + marcher sample behind voxel.Light.Propagated, default 0 until the
owner judges). File ownership: Shaders/VoxelMarch*.usf, VoxelEarthShaders
module, VoxelSkySubsystem (publisher only), new VoxelLightVolume files.
DO NOT touch: VoxelBoat/VoxelGlider/VoxelWaterSubsystem/water Tools
generators (live owner-testing lanes). No UBT/editor runs while the owner
holds the box — author compile-clean, the main session batches builds.
Gates run per phase; the owner judges L2 and L3 frames before defaults flip.

## Execution log

**2026-09-06, Opus implementation lane.** L2 complete; L3 scaffolded, compile-clean
by inspection, DEFAULT OFF. NOTHING WAS BUILT OR RUN — the owner holds the box and
the main session batches builds; every number below that is not a memory figure or
a dispatch shape is an *expectation*, not a measurement, and is labelled as such.

### Phase L2 — day-night light colour. SHIPPED, default on (rides `voxel.March.VSLighting`).

Files: `VoxelSkySubsystem.h/.cpp`, `VoxelSkyTests.cpp`, `VoxelMarchRenderer.h/.cpp`.

- **The ramp.** `VoxelSky::SampleLightColours(SunAltitudeDeg, MoonFraction)` —
  a pure function, six authored rows keyed on **apparent sun elevation** (−18 night,
  −6 civil twilight, 0 horizon, +10 low, +30 mid, +60 noon), linearly interpolated,
  clamped (not extrapolated) at both ends. Each row comments its intent. The moon
  term applies to the **ambient only** and only below −6° (weight ramps to 0 at the
  horizon), lifting the night ambient ×1.30 and pulling it 0.35 toward neutral at a
  full moon.
- **The control-row property, and it is load-bearing.** The noon row's ambient tint
  is *exactly* `1.00/1.04/1.12` — `MakeMarchAmbient`'s shipped constant — and its sun
  tint is exactly white. A frozen-noon capture with L2 live is byte-identical to one
  taken before it, so **the noon plate is a control** in the owner's dawn/noon/dusk
  comparison rather than a fourth variable. `SampleLightColours` returns endpoint rows
  bit-exactly (an explicit `Mix()` with `t<=0` / `t>=1` branches, because
  `FMath::Lerp(A,B,1.0f)` is `A + (B-A)` and that is not guaranteed to reproduce `B`).
- **Uniform contract (extends the `VoxelMarchPublishSunDirection` precedent).**
  New: `VoxelMarchPublishSunColour(const FLinearColor& SunColour, const FLinearColor&
  AmbientColour, float MoonFraction)` — game thread, atomics, published-flag last.
  A non-finite/negative/absurd (>8) component makes the call **refuse whole** rather
  than sanitise. Consumers: `MakeMarchVSColours()` (one derivation, gated on the
  `voxel.March.VSLighting` master), which feeds
  `MarchVSWrapColorAndSkyBoost.rgb = Gain × SunTint` and
  `MarchAmbientSkyAndGround.rgb = AmbientTint × AmbientIntensity`. The ambient tint
  **replaces** the hardcoded triple rather than multiplying it (a product would square
  the coolness at exactly the hour every archived capture was taken at). Never
  published, or master 0 ⇒ white / the shipped constant ⇒ byte-identical off arm.
  `MoonFraction` is published but not yet consumed by the compose; it is there so
  L4 does not have to change the contract again.
- **MPC_VoxelSky: NO NEW PARAMETERS, deliberately.** No existing scalar or vector
  fits `SunColour`/`AmbientColour`, and adding them forces a full
  `Tools/create_sky_material.py` chain regeneration (that script deletes and recreates
  the collection — an `-Only` run cannot do it) for something no material reads yet.
  The one existing parameter that fits any part of this contract, `MoonLightFraction`,
  **is already written every tick** by `ApplySkyMaterialParams`, and the marcher
  publish now rides the same block so the two provably carry one derivation of it.
- **Gates.** `VoxelEarth.Sky.LightColourRamp` (new automation test, in the module's
  existing `VoxelEarth.Sky` slot) pins: the noon control row exactly; zenith clamps to
  it; warm sun / cool ambient at the horizon; night blue, dimmer than noon but ≥ half
  its mean; a full moon brightens and neutralises the night while a daylight moon
  changes nothing; and continuity + finiteness across −90…+90° at a quarter-degree
  stride (worst step bound 0.05, derived: the steepest authored segment is 0.0229).
  Plus a one-time greppable engagement line, **`"VoxelSky light ramp"`**, printing the
  ramp sampled at horizon/noon/civil/night(new+full moon) *and* this frame's own
  sample — fixed elevations, because a line printing only "the colour right now"
  cannot distinguish a working table from one that returns row 0 for every input.

### Phase L3 — GPU-propagated sunlight volume. SCAFFOLDED, `voxel.Light.Propagated` DEFAULT 0.

Files: `Public/VoxelLightVolume.h`, `Private/VoxelLightVolume.cpp`,
`Shaders/VoxelLightVolume.usf` (new); hooks in `VoxelMarchRenderer.cpp`,
`VoxelMarchChunkIndex.cpp`, `Shaders/VoxelMarch.usf`.

- **Doctrine.** Display-only is stated at the top of the header, in the ADR-0006
  invariant-3–5 terms: no gameplay system may read the volume, it is per-client
  cosmetic state, and the enforcement is structural (it lives in VoxelEarthShaders,
  there is no CPU-side sampler, the only readback is a census counter).
- **Memory and geometry.** 128 × 128 × 64 cells at 100 UU (1.0 m) ⇒ a
  **128 × 128 × 64 m** box around the camera. `PF_R8G8B8A8`, **ping-ponged**:
  **4.0 MiB per copy, 8.0 MiB resident.** Channels: R = sky level 0…1,
  G = air mask (seed-cached solidity), B/A reserved for L4.
  **Deviation from the plan's "8 bits/cell", with its reason:** hardware trilinear
  filtering is the entire argument for a texture over a buffer (it *is* VS's
  per-vertex smoothing), and a filter blends every bit of a channel — so a packed
  "6-bit level + 2 spare" byte cannot be filtered at all. The level gets a whole
  channel; L4 gets B/A, which is the plan's own v2 shape arriving early.
- **Dispatch shapes.** SEED: `[numthreads(64,1,1)]`, one thread per **column**,
  serial 64-cell top-down walk (sky visibility is a prefix property, so one walk
  answers 64 cells; thread-per-cell would be O(n²) for the same answer). Dispatched
  over a dirty-column list, `ceil(N/64)` groups; a full reseed is 16,384 columns
  = 256 groups and 64 KB of upload. RELAX: `[numthreads(8,8,4)]` = 256 threads,
  **16 × 16 × 16 = 4,096 groups** over 1,048,576 cells, `Dst = airMask ? max(Src,
  max6(neighbours) − absorption) : 0`, ping-pong (never in place — in-place would be
  Gauss-Seidel in an undefined order).
- **Budget (≤ 1 ms amortized) — the structural argument, NOT a measurement.** Per
  relax iteration: 7 texel reads + 1 write per cell, but neighbour reads are shared,
  so real traffic ≈ the 4 MiB working set + 4 MiB written. At ~620 GB/s that is
  ~0.03 ms for two iterations; the honest cost is latency- and dispatch-dominated,
  which is exactly why it must be measured. **The amortization comes from the gate,
  not the kernel:** relax runs only while something is dirty, so a settled camera in
  a streamed-in world dispatches *zero* and costs its memory alone.
- **Dirty tracking — reuses the pool's own channel.** `FVoxelBrickIndexDelta` (what
  `FVoxelBrickPool::Flush` already publishes) is forwarded from inside
  `FVoxelMarchChunkIndex::ApplyDelta` via
  `VoxelLightVolumeNoteBrickIndexDelta_GameThread`. Not a second sink on the pool:
  `SetIndexSink` holds exactly one and the chunk index owns it, and chaining would
  make the pool's "delivered after the pool's own render command" ordering guarantee
  depend on subscriber order. Level-0 entries only (the volume is 64 m of radius,
  inside ring 0 by construction); other levels are **counted as dropped**, never
  silently ignored. Queue cap 4,096 ⇒ overflow forces a full reseed (the safe and,
  at cold fill, the cheap direction) and is counted.
- **Consumption.** `VoxelMarch.usf`, in the emissive block: one trilinear
  `Texture3DSampleLevel` at `HitTWS + N × halfCell`, range-checked; `G > 0.5`
  gates on "this sample is mostly about air"; the level is floored by
  `voxel.Light.Floor` (VS's MINBRIGHT, "Light up all caves" — also what stops a
  half-settled volume flashing black) and lerped by `voxel.Light.Strength`. The
  resulting scale multiplies **both** the hemisphere ambient **and** the L1 wrap
  deficit — scaling only the ambient would give a sealed cave a full orientation
  term, i.e. a cave lit from nowhere. Out of range ⇒ 1.0 ⇒ today's flat ambient
  (the designed fallback: L1+L2's response is distance-independent, so the seam is a
  change of gradient, not of level). `Enabled == 0` ⇒ scale exactly 1.0 ⇒ multiply by
  one ⇒ byte-identical.
- **The transition nobody can ask for, and why there is a no-op pass.** The volume is
  an external texture written through a UAV and read by the emit through a **uniform
  buffer**, which RDG cannot see. A `VoxelLightVolume.ToSRV` pass declares the texture
  as an SRV and does nothing, which is how you tell RDG that something outside the
  graph is about to read it. Binding it to the emit as an RDG texture instead would
  mean a loose `Texture3D` in the shader shadowing the uniform-buffer member — the
  silent-zeros trap three headers in this module already warn about.
- **Hook placement, and it is a correctness constraint.** The update is called from
  `VoxelMarchRenderer.cpp` **after `VoxelMarchBindPool`** and once per frame, and it
  is **handed the already-registered chunk-index SRV** rather than registering it
  itself: `FVoxelMarchChunkIndex::Register` *consumes* the frame's staged upload, so a
  second call would take this frame's index for the light volume and leave the marcher
  on last frame's pooled buffer — a permanently one-frame-stale index that would be
  blamed on streaming.

### Cvar and uniform contracts (new)

| cvar | default | what it does |
|---|---|---|
| `voxel.Light.Propagated` | **0** | master; 0 = nothing allocated, no pass, no sample, byte-identical |
| `voxel.Light.RelaxIterations` | 2 | relax sweeps per dirty frame; 0 = seed-only bisection state; clamp [0,16] |
| `voxel.Light.AbsorptionCells` | 12 | sky reach around a corner, in 1 m cells (absorption = 1/this); clamp [1,64] |
| `voxel.Light.Strength` | 1.0 | how much the sampled level scales the ambient/wrap share; 0 = built+bound+sampled, no pixel changes |
| `voxel.Light.Floor` | 0.25 | VS's MINBRIGHT: lowest fraction of today's flat ambient |
| `voxel.Light.RecentreCells` | 16 | camera dead zone in cells; clamped to ¼ of the shortest axis |
| `voxel.Light.SeedColumnBudget` | 4096 | columns seeded per frame (a full reseed may exceed it) |
| `voxel.Light.Census` | 1 | print the 1 Hz census line |

Shader-side uniform buffer `VoxelLightVol` (never null; `GBlackVolumeTexture` +
`Enabled = 0` when off): `Volume`, `VolumeSampler`, `OriginRelCameraUU`, `InvSizeUU`,
`CellSizeUU`, `Strength`, `Floor`, `Enabled`.

### The census line format (grep: `[voxel-light] census`)

```
[voxel-light] census <win>s frames=<n> | cellsRelaxed=<f>/frame seedColumns=<f>/frame
  | dirtyDepth=<n> dirtyChunks=<n> dropped=<n> overflow=<n>
  | recentres=<n> fullReseeds=<n> iters=<n> absorb=1/<f> cells
  | origin=(x,y,z) voxels dims=128x128x64 @ 100 UU
  | declined pool=<n> index=<n> volume=<n> | <VERDICT>
```

`VERDICT` is one of `PROPAGATING`, `SETTLED` (a legitimate zero — but **a moving
camera reading SETTLED is a dead dirty wire and the leg should FAIL**), or
`INERT-DECLINED` (armed and every frame refused — read the decline counters, not the
zeros). Counters are **monotonic**; the printer and the exported
`VoxelLightVolumeGetAndResetCensus()` keep independent baselines, so neither steals
the other's window. `cellsRelaxed`/`seedColumns` are **dispatched** counts, not
GPU-confirmed — stated in the header, because there is deliberately no readback ring
for a pass that ships off.

### What the main session must do

1. **Build.** Nothing here has been compiled. New translation unit
   (`VoxelLightVolume.cpp`), new global shaders (two entry points in
   `VoxelLightVolume.usf`), a new automation test, and edits to
   `VoxelMarchRenderer.h/.cpp`, `VoxelMarchChunkIndex.cpp`, `VoxelMarch.usf`,
   `VoxelSkySubsystem.h/.cpp`, `VoxelSkyTests.cpp`.
2. **Run the L2 gate:** `Automation RunTests VoxelEarth.Sky` (the ramp test is
   `VoxelEarth.Sky.LightColourRamp`), and grep any leg's log for
   `"VoxelSky light ramp"` to prove the publisher engaged.
3. **L2 owner captures:** dawn / noon / dusk / night at a fixed pose, with
   `voxel.March.VSLighting 1`. The **noon plate is the control** and should be
   pixel-identical to the pre-L2 archive; if it is not, the ramp's noon row has
   drifted and the test above will already have failed.
4. **L3 engagement leg:** a documented **overhang pose** (a cliff base or cave mouth
   with sky above and shade beneath — the pose the feature exists for), moving
   camera, `voxel.Light.Propagated 1`. Grep `[voxel-light] census`; the leg **FAILS**
   on `SETTLED` with a moving camera, on `INERT-DECLINED`, or on `cellsRelaxed == 0`.
   Then an A/B still pair at 0 and 1 for the owner, plus `voxel.Light.Strength 0`
   as the third arm that separates "costs time" from "changes the picture".
5. **L3 cost:** `-csvGpuStats` + `CsvProfile FRAMES=N`, read `GPU/VoxelLightVolume`
   (its own column — the volume's stat scope nests inside `VoxelMarch`, so its time
   is subtracted from `GPU/VoxelMarch` rather than hidden in it). `marchMs` from
   `voxel.March.Stats` **includes** these passes and a leg quoting it across the two
   arms is quoting a sum; say so.
6. **Defaults stay as shipped** until the owner has judged frames. Nothing in this
   lane flips `voxel.Light.Propagated`.

## L3 STATUS 2026-09-06: scaffolding built + compiled, ENGAGEMENT NOT YET FIRING

Build green; L2 SHIPPED and gated (VoxelEarth.Sky.LightColourRamp PASS after a
float-precision fix to the control-row assert -- double(1.04f), not 1.04; the
publisher engaged live in light-l3-engage2.log: horizon warm / noon neutral /
night cool, noon row == marcher's 1.00/1.04/1.12).

**L3 leg (light-l3-engage2.log) FAILED its engagement check, and it is a real
bug, not a tuning gap.** With `voxel.Light.Propagated 1` set at startup (logged
`voxel.Light.Propagated = "1"`) and the marcher demonstrably running (raster +
[voxel-march] output present), the light-volume category emitted NOTHING over
220 s -- not the 1 Hz `[voxel-light] census`, and not even the one-time
`[voxel-light] allocated` line from EnsureAllocated. The category is not
suppressed (LogVoxelGI/Shadow lines are present in the same log). So
`VoxelLightVolumeUpdate_RenderThread` never enters its armed body: either its
call site in the `-game` emit path (VoxelMarchRenderer.cpp:~10805, inside the
per-view march loop after VoxelMarchBindPool) is not reached in this
configuration, or `CVarVoxelLightPropagated.GetValueOnRenderThread()` reads 0
on the render thread despite the game-thread set (ECVF_RenderThreadSafe is set,
so this would be a sync/duplicate-registration surprise, not a missing flag).
NEXT (lighting lane, own pass -- do NOT block the commit): add an unconditional
one-line trace at the update entry to disambiguate "not called" from "called,
reads 0", then fix whichever it is. L3 remains default-0 and byte-identical-off,
so the shipped game and the owner's L2 judgment are unaffected.

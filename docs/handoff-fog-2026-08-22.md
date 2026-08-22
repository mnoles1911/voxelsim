# Handoff: fog session -> next session (2026-08-22)

For a fresh Claude Code session in D:\voxelsim continuing the fog work. Read
this top to bottom before touching anything; the working tree currently
interleaves TWO sessions' uncommitted work, and the single most important rule
is knowing which files are yours to touch.

---

## 1. What this session was doing, and where it stopped

Task: implement the approved fog plan (Appendix A, verbatim) -- SkyAtmosphere
aerial perspective for the horizon (already live), a new
ExponentialHeightFogComponent for mid-distance depth cueing, and volumetric
fog behind a cvar tier, all in the deferred screen-space passes and never in
material graphs (hard constraint from the parallel ray-marching session).

**Slice 1 is fully authored. Nothing about it has compiled, run, or been
captured yet.** The build is blocked by the other session (see section 3).
Every line of fog code is written and symbol-sane but unverified.

Owner's standing constraint, verbatim: "Implement and build what you can but
do not interrupt my other sessions work." The other session is implementing
GPU ray marching for all rendering (docs/ray-marching-plan-2026-08-19.md,
docs/marcher-to-primary-plan-2026-08-21.md) and may hold the editor --
**one editor per box; check with Get-Process, never tasklist via Bash.**

## 2. The fog work itself -- four files, all uncommitted

| File | What the fog session added |
|---|---|
| `ue-project/Source/VoxelEarth/VoxelSkySubsystem.h` | `SkyHeightFog` member, `UnderwaterFogSuppression`, `AppliedVolumetricTier`, `ApplyFogFromState()`, public `SetUnderwaterFogSuppression(float)` |
| `ue-project/Source/VoxelEarth/VoxelSkySubsystem.cpp` | 4 cvars (~:505-547); EHF spawn in the sky-rig block (~:2394) -- BLACK inscattering colours, `SetRelativeLocation(0,0,SpawnGroundZUU)` from `GetSurfaceHeightUU` (the altitude trap: play band is ~1650 m up and falloff measures from component Z); ran-flag log + stale-preset Warning (~:2419-2427); static-pose hide (~:2561); `ApplyFogFromState()` (~:3227) with push-on-change, tier sink (GridPixelSize 16/8, GridSizeZ 64/128 at SetByConsole), `// WEATHER HOOK` comment; NaN-guarded suppression setter (~:3217) |
| `ue-project/Source/VoxelEarth/VoxelOceanActor.cpp` | Pushes `Sky->SetUnderwaterFogSuppression(UnderwaterBlendWeight)` every tick after the blend ramp (ocean pushes to sky, never the reverse); transition log gained `fogSuppression=%.2f` |
| `ue-project/Config/DefaultEngine.ini` | Fog block appended to `[ConsoleVariables]`: `r.VolumetricFog=1`, `r.ViewDistanceScale=1.0`, `r.SkyAtmosphere.*` lifted verbatim from BaseScalability `[EffectsQuality@3]`; new `[/Script/Engine.RendererSettings]` with `r.SupportSkyAtmosphereAffectsHeightFog=True` (spelling is "Affects", verified in RendererSettings.h:1036 -- the docs' "Affecting" is wrong). **This flag triggers a shader recompile: expect a slow first launch.** |

Cvars: `voxel.Sky.Fog` (1), `voxel.Sky.FogDensity` (0.008),
`voxel.Sky.FogHeightFalloff` (0.05), `voxel.Sky.VolumetricFog` (tier 0/1/2,
default 1).

Ran-flags to check on first run: grep the log for `SkyHeightFog` /
`volumetric tier=` -- the spawn line prints ground-ref Z, density, falloff,
tier, and the LIVE `r.VolumetricFog` value, and warns if that reads 0 while
tier >= 1 (the stale Saved/ scalability preset symptom; the [ConsoleVariables]
block is the fix and outranks it).

**Also uncommitted from this session's earlier water/weather work:**
`VoxelWeatherSubsystem.cpp` -- the final wave-speed fix (WaveResponseSeconds
45 -> 8 -> 600 s with the full reasoning in comments, either-pin bypass). This
is the build the owner still needs to judge. Do not lose it.

## 3. The blocker

Build.bat exits 6. **Every error is in the OTHER session's in-progress
files** -- `VoxelEarthShaders/Public/VoxelMarchRenderer.h(584): C2065
'kMaxHvfSamples': undeclared identifier` and a C2039 in
`VoxelMarchRenderer.cpp(1541)`. VoxelMarchRenderer.h is *untracked* (their
work in progress); zero errors reference the fog files (the one grep hit in
Saved/build-fog.log is the unity-exclusion list line).

**Do NOT fix, edit, or even reformat anything under
`ue-project/Source/VoxelEarthShaders/` or the other march/GI/shadow files.**
The blocker clears when that session finishes -- detectable by their files
compiling (try a build), a new commit landing on top of `8d884de`, or the
owner saying so.

## 4. Whose uncommitted work is whose (read before ANY git operation)

As of 2026-08-22 the tree has ~35 modified + ~40 untracked files from both
sessions mixed together:

- **Fog session's files**: the four in section 2 plus `VoxelWeatherSubsystem.cpp`.
- **Other session's**: everything march/brick/GI/shadow/karst/fluid --
  `VoxelEarthShaders/*`, `VoxelMarchSpike.usf`, `VoxelGIMarch.usf`,
  `VoxelBrickPack.usf`, voxel-core brickpack/covervolume files,
  `VoxelPerfRunSubsystem.*`, `VoxelEarthGameMode.cpp` (their -VoxelGIOn/Off
  arms), most of the docs/ and tools/ additions.
- **MIXED FILE -- `DefaultEngine.ini`**: one uncommitted diff holds BOTH their
  shadow-restoration block (r.ShadowQuality=5 etc., dated 2026-08-19) AND the
  fog block. Committing the fog config requires `git add -p` to stage only the
  fog hunks, or coordination with the owner. Never `git add -A`, never
  `git checkout --` anything, never `git stash` -- any blanket git operation
  destroys or entangles the other session's work.

## 5. Resume sequence once the build is unblocked

1. Build (the fog code has never compiled -- expect possible small fixups;
   past slips here were of the FString-vs-FText `*Def->DisplayName.ToString()`
   variety).
2. First launch: shader recompile (slow), then confirm the spawn ran-flag and
   that `r.VolumetricFog` reads 1 at runtime.
3. Captures, pinned poses via tools/voxel-capture.ps1, `-VoxelExecCmds` is
   COMMA-separated (a `|` chain silently runs only the first command):
   - Long vista `7680,7680 -SpawnAltM 2000 -SpawnPitch -25`: dome-seam gate +
     density ladder 0.004 / 0.008 / 0.015 (ladder alt-cvar mechanism;
     `-VoxelSkyLadderAltCvar=voxel.Sky.Fog` works for on/off A-B too).
   - Pond shore `-65102,-51084`: mid-distance cueing + enter/exit-water pair
     proving the cross-fade never double-darkens.
   - Dusk (sun 0..-6 deg) god rays; night moon volumetrics (expect subtle).
   - Wait for terrain to settle before every capture (blank = unloaded, not a
     bug); present captures + conditions to the owner, NEVER a verdict.
4. **New baseline perf leg is mandatory before any fog perf claim** -- the
   config block un-forced the r.VolumetricFog=0 and r.ViewDistanceScale=0.4
   that silently shaped EVERY historical number. Protocol:
   tools/voxel-run-flight-leg.ps1 per docs/final-comparison-method.md; deltas
   resolve at >= max(0.18 ms, 2x within-arm spread), p50 only. Arms: new
   baseline (fog off) -> fog-only -> tier 1 -> tier 2. Tier 1 must fit the
   ~1.5 ms headroom at 1440p or volumetrics ship default-off (the owner's "on
   by default" was conditioned on measurement).
5. Ray-march follow-up gate (once their session lands): same poses,
   `voxel.March` on vs off -- fog must be near-identical on both paths.

## 6. Other open items inherited by this handoff

**Owner-in-editor checks still outstanding (from the water/weather pass):**
- Waves at the current (uncommitted) 600 s + either-pin build -- judge speed
  and inertia. The frozen-wind look of `voxel.weather.enabled 0` is the
  reference the owner liked.
- Ripples at raised defaults: `voxel.Water.Ripple.DropHere 1.5 0.3 40` -- the
  trailing `40` (sim steps) is essential; without it the probe reads zero.
- Throwable: press key 5 in PIE (hotbar is 5-9; 1-4 are taken), then
  `voxel.Throwable.Stat`.

**Backlog:** docs/backlog.md section 9.1 -- destroying a voxel underwater
crashed perf and made nearby water unload / turn transparent. A diagnosis
path is written in the backlog entry; not yet investigated.

**Deliberately deferred fixes (each needs its own change + before/after):**
- `ue-project/Tools/water_wave_graph.py` fallback direction builds
  (sin,cos) = (east,north); should be (cos,sin). The tuned 238.7 deg fallback
  absorbed the swap, so fixing it ROTATES the approved look -- do not slip it
  into another change.
- `tools/water-playtest.ps1` still joins exec commands with `|`; must be `,`.
- Weather-driven FogDensity: wire the weather field into the `// WEATHER HOOK`
  seam in ApplyFogFromState() -- the owner decided static-first, hook later.

**House rules that bit this session** (full versions in auto-memory):
comma-split -ExecCmds; -VoxelExecAfter for post-world console commands (this
session added it); the instrument must run the engine's binding; the owner
judges screenshots; verify screenshot sites before shooting; one editor per
box via Get-Process.

---

## Appendix A: the approved fog plan (verbatim, owner-approved 2026-08-21)

Decisions already taken by the owner: static art-directed density first with a
documented weather hook later; volumetric fog on by default, conservatively
tuned, cvar-scalable, measured before the defaults are trusted.

### Context

The owner wants three things, in his words: "atmospheric fog to blend/blur far
off horizon, general distance fog and volumetric fog", using "built in UE5
features where possible", for a procedural open world whose real vista is
**65.5 km on-axis / 92.7 km at the corner** (the clipmap -- docs saying 50 km
are stale).

Exploration (2026-08-20) established facts that reshape the obvious plan:

1. **A third of the ask already ships.** A real `USkyAtmosphereComponent` runs
   today (VoxelSkySubsystem.cpp:2278-2292) and its aerial perspective is strong
   enough at 2 km to have broken the underground veil once
   (VoxelClipmapActor.cpp:92-103). The horizon layer is a tuning job, not a
   build job.
2. **Volumetric fog was forced OFF on every run this project has ever
   measured** -- a stale `Saved/Config` scalability preset (`sg.ShadowQuality=0`
   et al.) sets `r.VolumetricFog:0`, `r.ViewDistanceScale:0.4`, and LOW-quality
   atmosphere LUTs at boot. Every historical perf number predates volumetric
   fog existing. DefaultEngine.ini's [ConsoleVariables] override precedent
   applies (SetByConsoleVariablesIni 0x0A outranks SetByScalability 0x02).
3. **There is no ExponentialHeightFog anywhere, deliberately.** It was deleted
   from VoxelOceanActor with a written policy (VoxelOceanActor.cpp:133-164):
   the crime was double-counted extinction against the underwater Beer-Lambert
   post-process, not fog itself. Any new height fog must be suppressed while
   submerged, keyed off the same state that drives the underwater blend.
4. **The forward-light slot was pre-tuned for volumetric fog** -- the sun/moon
   handover sits at -6 deg specifically so twilight god rays keep the sun
   (VoxelSkySubsystem.cpp:1066-1090). The lighting/weather plan pre-registered
   `voxel.Sky.VolumetricFog` and named ExponentialHeightFog + volumetrics as
   the intended mechanism (docs/lighting-weather-plan.md:647-657, :958-961),
   with FogDensity already in the weather field's designed output signature.
5. **A parallel session is implementing GPU ray marching for all rendering**
   (docs/ray-marching-plan-2026-08-19.md, default-off behind `voxel.March`).
   Its verified integration emits SV_Depth + full GBuffer + SceneColor at
   `PostRenderBasePassDeferred` -- before lighting and the fog passes -- so
   ray-marched pixels receive deferred fog identically to rasterized ones.
   This decides an architectural question outright: **fog must live in the
   deferred screen-space passes (EHF, aerial perspective, volumetrics), never
   in the terrain/clipmap material graphs** -- a material-level fog term would
   not exist for ray-marched pixels, and the two paths would fog differently,
   which is exactly the two-renderers-disagreeing failure this project keeps
   paying for. The lighting plan's 5.3 "analytic horizon term in the clipmap
   material" option is therefore rejected for anything long-lived.

Web research (UE 5.8 docs, practitioner consensus) fixes the layering and the
one classic trap: **fog contributions are additive**, so when SkyAtmosphere and
ExponentialHeightFog are combined, the EHF's `FogInscatteringColor` and
`DirectionalInscatteringColor` must be BLACK with "Support Sky Atmosphere
Affecting Height Fog" enabled, letting the atmosphere supply the colour -- the
standard failure is authored fog colour added on top of aerial perspective.
Volumetric fog cost in the literature: ~1-3 ms GPU (PS4 High 1 ms, 970GTX Epic
3 ms), controlled by `r.VolumetricFog.GridPixelSize` (default 8) and the
scalability tie to `sg.ShadowQuality`.

### The architecture -- three built-in deferred layers, no material fog terms

| Layer | System | Owns | Cost |
|---|---|---|---|
| Horizon / vista (2-92 km) | **SkyAtmosphere aerial perspective** (already live) | ALL fog **colour** at distance | already paid (LUT) |
| Mid-distance depth cueing | **ExponentialHeightFogComponent** (new, on the sky rig) | density/falloff **shape**; colour sourced from atmosphere | ~0.1 ms |
| Near/mid lit fog + god rays | **Volumetric Fog checkbox** on that EHF | froxel-lit scattering | 1-3 ms GPU, tiered |
| Valley/pond accents (slice 3, optional) | **Local Fog Volumes** | per-area art-placed fog | cheap |

Pass ordering verified in the local 5.8 source, not assumed: the ray marcher's
GBuffer emit is at BasePassRendering.cpp:1152; `RenderFog` (height fog +
volumetric apply) is DeferredShadingRenderer.cpp:3822 -- strictly after. The
froxel compute at :2943 runs earlier but doesn't read scene depth; only the
apply does. So deferred fog treats rasterized and ray-marched pixels
identically, and TSR's `r.SeparateTranslucency=0` ghosting risk never arises
because nothing translucent is added.

### The double-count rules (stated in code comments)

1. **EHF vs aerial perspective**: `FogInscatteringLuminance` and
   `DirectionalInscatteringLuminance` both **BLACK**;
   `bSkyAtmosphereAffectsHeightFog = true` + project setting
   `r.SupportSkyAtmosphereAffectsHeightFog=True`. Contributions are additive;
   the atmosphere is the *only* colour authority. A non-black EHF colour is
   the same two-disagreeing-colour-tables failure as the deleted underwater
   fog.
2. **EHF vs underwater Beer-Lambert**: never both. EHF density scales by
   `(1 - UnderwaterBlendWeight)` -- the exact weight that fades `M_Underwater`
   in, so the two extinction models cross-fade and never sum. This satisfies
   the written policy at VoxelOceanActor.cpp:133-164.
3. **EHF vs water surface tint**: fogging the water *surface* at distance is
   air, not water -- correct, no change. Don't add haze terms to the water
   material.
4. Want a thicker vista? Turn `AerialPerspectiveViewDistanceScale` on the
   existing SkyAtmosphere -- never fake it with EHF density.
5. **Dome seam**: the IsSky dome takes no height fog; with rule 1, fogged
   terrain converges to atmosphere colour at distance so no seam should exist.
   The long-vista capture is the gate; the fix, if needed, is density tuning,
   never a dome material change.

### Implementation

**1. VoxelSkySubsystem.cpp / .h** -- the bulk of the work:
- Spawn `UExponentialHeightFogComponent` ("SkyHeightFog") in the existing
  sky-rig block (:2274-2398) beside `SkyExposurePP`; new member next to it.
- **The altitude trap**: the rig root sits at Z=0 (planet-top transform, must
  not move), but EHF falloff measures from the *component's* Z and the play
  area is ~1,650 m up -- default falloff would make fog vanish there. Fix:
  `SetRelativeLocation(0,0,GroundZ)` with GroundZ from
  `GetSurfaceHeightUU(spawn column)` -- same discipline as the rest of the rig.
- New `ApplyFogFromState()` called from Tick beside `ApplyExposureFromState()`:
  applies cvar density x (1 - underwater suppression), volumetric tier,
  pushes only on change. Marked `// WEATHER HOOK:` where the weather field's
  FogDensity sample will multiply in later (one seam, deferred by owner
  decision).
- `ApplyStaticRigPose()` must hide the fog when `voxel.Sky.Enabled 0` --
  "off" stays byte-identical to the pre-feature frame.
- New cvars beside the existing table: `voxel.Sky.Fog` (1),
  `voxel.Sky.FogDensity` (0.008 -- bracket 0.004/0.008/0.015 by capture; UE's
  0.02 default is too thick for a 92 km vista), `voxel.Sky.FogHeightFalloff`
  (0.05 -- bracket 0.02-0.1), and the pre-registered `voxel.Sky.VolumetricFog`
  tier: 0 = off, 1 = conservative (GridPixelSize 16, GridSizeZ 64), 2 = engine
  default (8/128). Exposure fields untouched -- no new PP volume, so the
  two-volume invariant needs no edit.

**2. VoxelOceanActor.cpp** -- in `UpdateUnderwaterState`: after the BlendWeight
ramp, push `Sky->SetUnderwaterFogSuppression(UnderwaterBlendWeight)`. Ocean
pushes to sky, never the reverse (matches the exposure-ownership lifetime
rule). Density-scale, not SetVisibility -- a visibility toggle pops, the
shared weight cross-fades. Append `fogSuppression=%.2f` to the existing
transition log line.

**3. Config/DefaultEngine.ini** -- append to `[ConsoleVariables]` using the
documented precedent (defeats the stale Saved/ preset that has forced
`r.VolumetricFog:0` and `r.ViewDistanceScale:0.4` on every run ever measured):
- `r.VolumetricFog=1` (the component flag stays the real runtime gate)
- `r.ViewDistanceScale=1.0` (judging horizon fog through a 0.4 cull is judging
  wrong)
- `r.SkyAtmosphere.*` quality lifted verbatim from BaseScalability.ini
  `[EffectsQuality@3]` -- copy Epic's values, don't invent
- New `[/Script/Engine.RendererSettings]`:
  `r.SupportSkyAtmosphereAffectsHeightFog=True`
  (**triggers a shader recompile on next launch -- expect a slow first boot**)

**Untouched by design**: `create_sky_material.py` and the MPC (no new params ->
no sky-chain regeneration), `create_clipmap_material.py` (stays a clean slate --
material fog is rejected permanently per the ray-marching constraint), the
-6 deg sun/moon handover (already tuned for volumetric fog), both exposure
volumes.

### Ran-flags

- Spawn: one log line with ground-ref Z, density, falloff, tier, and the live
  `r.VolumetricFog` value -- plus a **Warning** if that reads 0 while tier >= 1
  (the stale-preset symptom, named so the next person doesn't rediscover it).
- Tier changes: one line each. Ocean transition line gains the suppression
  field.

### Verification

**Captures** (pinned pose, `-VoxelExecCmds` comma-separated, one editor per
box):
1. Long vista -- `7680,7680 -SpawnAltM 2000 -SpawnPitch -25` -- horizon blend /
   dome-seam gate, and the density bracket (0.004/0.008/0.015) shot as a
   ladder.
2. Pond shore -- `-65102,-51084` -- mid-distance cueing at altitude, plus an
   enter/exit-water pair proving the cross-fade never double-darkens.
3. Dusk (sun 0..-6 deg, clock pinned) -- god rays, and no fog pop at the
   handover.
4. Night -- moon-lit volumetrics; expect subtle.
- A/B pairs inside one process via the existing ladder alt-cvar mechanism:
  `voxel.Sky.Fog` 0/1, then `voxel.Sky.VolumetricFog` 0/1.
- **Ray-march follow-up gate** (once that session lands, not a blocker): same
  poses, `voxel.March` on vs off -- fog must be near-identical; residual
  should be geometry, not fog.

**Perf** (`voxel-run-flight-leg.ps1`, protocol per final-comparison-method.md):
new baseline first (new config, fog off) -- **every historical number predates
these config changes and is not comparable** -- then arms: fog-only, tier 1,
tier 2. Deltas resolve at >= max(0.18 ms, 2x within-arm spread), p50 only.
Tier 1 must fit the ~1.5 ms headroom at 1440p or volumetrics ship default-off
pending tuning -- the owner's "on by default" was conditioned on measurement.

### Phasing

1. **Slice 1 (first visible result)**: config + EHF + cvars + suppression +
   captures 1-2 + the new baseline leg. *(Status at handoff: authored in
   full, blocked at the build step.)*
2. **Slice 2**: volumetric tiering, default ON after the perf arms pass,
   dusk/night captures.
3. **Slice 3 (optional, later)**: Local Fog Volumes for valley/morning fog.

**Not in scope**: VDBs, fog cards, weather-driven density (hook only),
god-ray set pieces (P8 in the lighting plan), any material-graph fog anywhere.

**Flagged uncertainties**: exact falloff unit constant and
`VolumetricFogDistance` default (read `ExponentialHeightFogComponent.h` in
D:\UE_5.8 when implementing); whether GridPixelSize 16 looks acceptable at
1440p (capture decides).

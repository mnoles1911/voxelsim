# Why the lake does not read as water — mechanism-level diagnosis and a ranked plan

2026-09-06. Written against the owner's report: the lake "seems off and not
realistic... I cannot nail down exactly why but I know it is... it seems like our
Single Layer Water in UE5 needs more work to look realistic."

This is an ANALYSIS. Nothing in `ue-project/Tools/` or `ue-project/Source/` was
edited, and nothing here was run (the owner has the machine). Every claim below
is a file:line read, an engine shader read, or arithmetic on numbers that are in
the tree. Where I am guessing, the sentence says so.

**Standing constraint honoured:** the interactive ripple/wake foam is known
broken and under active debugging elsewhere. This document theorises about the
foam path only where a foam mechanism is load-bearing for a DIFFERENT
observation (Opacity, and the contour lines), and proposes no fix for it.

---

## 0. THE HEADLINE, IN ONE PARAGRAPH

Observations 1, 2 and 3 — far water goes dark instead of silver, no specular or
sun glitter, and nothing reflects in it — are **one root cause, and it is not in
the water material.** The engine's Single Layer Water shading *deletes the
water's own colour at grazing angles on purpose*, because it assumes a specular
reflection is arriving to replace it:

    SingleLayerWaterShading.ush:234
        ScatteredLuminance *= bCameraIsUnderWater ? 1.0 : (1.0 - EnvBrdf);

`EnvBrdf` is `EnvBRDF(SpecularColor, Roughness, max(0, dot(N,V)))`
(`BasePassPixelShader.usf:1707-1710`). Our Specular is 0.5 (F0 = 0.04) and our
Roughness is 0.08 (`create_water_voxel_material.py:2341`, `:2355`), so as the
view goes grazing `NoV -> 0`, `EnvBrdf -> ~1`, and **`(1 - EnvBrdf) -> 0`: the
in-scattered body colour, which is the only thing making our lake blue, is
multiplied out to nothing exactly where the owner says the water goes dark.**

The energy is supposed to come back as `Reflection * EnvBrdf`
(`SingleLayerWaterComposite.usf:247`). On this project it does not, because:

* `Config/DefaultEngine.ini:205` sets `r.Water.SingleLayer.Reflection=2`, which
  in `SingleLayerWaterRendering.cpp:56-70` means "reflection captures and
  skylight cubemaps only". SSR runs only on modes 1 and 3
  (`SingleLayerWaterRendering.cpp:337-342`) and Lumen only on mode 1
  (`:367-372`, "It can't be forced with r.Water.SingleLayer.Reflection").
* **There are zero reflection capture actors in the project.** No
  `ASphereReflectionCapture`, `ABoxReflectionCapture` or `APlanarReflection` is
  spawned or placed anywhere in `Source` or `Tools`, and there is no `.umap` in
  `Content` at all — the world is built at runtime, from
  `GameDefaultMap=/Engine/Maps/Entry.Entry` (`DefaultEngine.ini:11`).
  `docs/sky-and-local-light-plan.md:242-245` already says so in as many words:
  "This project ships no reflection capture actors."
* So the entire reflection budget of every water surface in the game is **one
  real-time SkyLight cubemap** (`VoxelSkySubsystem.cpp:3006-3032`) — and that
  capture is taken from `z = 0`, sea level, which at the shipped seed is
  **2187.6 m below the player's ground, inside fog 1960x denser than the fog the
  player stands in** (`VoxelSkySubsystem.cpp:3064-3091`;
  `voxel.Sky.SkyLightAtGroundZ` default 0 at `:785-787`).
* And the material's own analytic Fresnel sky reflection — the term that used to
  supply the grazing rise — **ships multiplied by zero, twice over**:
  `LegacySkyReflectGain` default `0.0` (`water_sky_reflection_graph.py:255-256`,
  retired by the owner 2026-08-12) and `SurfacePresence` default `0.0`
  (`:258-266`, rejected by the owner 2026-09-05).

**Net: the shipped M_WaterVoxel emissive is the sun glint plus the moon glint and
nothing else.** There is no Fresnel-weighted term of any magnitude in the
material (both Fresnel nodes exist and both feed a chain that ends in a `x 0.0`),
and the engine-side replacement is a fog-washed cubemap integrated from two
kilometres underground. That is a surface with no reflectance and no mirror,
whose own colour the renderer removes precisely where reflectance should be
taking over. It cannot read as water at distance, by construction.

Observations 4, 5 and 6 are a second, separate story: the optics are tuned so the
bed dies within ~3 m of *vertical* depth, the 2026-09-05 secant slant multiplies
that depth by up to 20x at exactly the angles in the owner's screenshot, and the
one term that is never routed through the smooth baked depth field — scattering —
still reads the 10 cm voxel staircase, which is what draws the contour lines.

And one separate, hard finding that answers the owner's caustics question
outright: **the Phase F1 caustic term is wired into `M_VoxelTerrain` and
`M_VoxelClipmap`, and neither of those materials shades a single near-field
terrain pixel in the shipped configuration.** `voxel.Terrain.RetireQuads` defaults
to 1 (`VoxelBrickPool.cpp:668-675`), so terrain quads are not produced at all;
the clipmap's inner hole is 8,192 m (`VoxelClipmapActor.cpp:791-813`); the lake
bed is drawn by the ray marcher, and the marcher has **zero** occurrences of the
string `caustic` in any of its C++ or HLSL. Caustics at `voxel.Water.Caustics
0.5` producing nothing is not a tuning problem — the light is being added to a
material that does not draw the lake floor. Full evidence at observation 4d.

---

## 1. WHAT THE SHIPPED MATERIAL ACTUALLY IS

For reference throughout. `M_WaterVoxel`, built by
`ue-project/Tools/create_water_voxel_material.py`:

| Pin | Wired to | Site |
|---|---|---|
| ShadingModel | `MSM_SINGLE_LAYER_WATER` (read back off the saved package) | `:1524-1525`, `:2857-2867` |
| BlendMode | `BLEND_MASKED`, clip 0.5, two-sided | `:831`, `:842`, `:852` |
| BaseColor | `lerp(black, (0.82,0.90,0.94), foam) * VertexColor.G` | `:1936-1975` |
| EmissiveColor | `build_sky_reflection(...)["emissive"]` + disturbance foam | `:2066-2105` |
| Opacity | `saturate(foam)` = `BaseMaterialCoverageOverWater` | `:2162-2168` |
| OpacityMask | shore-SDF step (default OFF, `WaterShoreClipEnabled` 0.0) x hull mask | `:2236-2314` |
| Roughness | `lerp(0.08, 0.62, foam)` | `:2340-2352` |
| Specular | constant `0.5` | `:2354-2357` |
| Normal | `(-dHdx, -dHdy, 1) * VertexColor.B`, unnormalised | `:2663-2695` |
| WPO | fill-drop + wave/ripple height x `WaveWpoFraction` x distance fade | `:2760-2807` |
| SLW output | Absorption `(1-0.85*valid)*a`, Scattering `s` (FULL), PhaseG 0.35, ColorScaleBehindWater `exp(-a * 0.85*valid * slant)` | `:1501-1516` |

Optics (`ue-project/Tools/water_optics.py:119-157`, current, and note these have
MOVED since the prose in `create_water_voxel_material.py` was written — that file
quotes the superseded 3.5 m / (0,0.30,0.38) state):

    ABSORPTION_DISTANCE_M = 5.5
    ABSORPTION_COLOR      = (0.0, 0.59, 0.74)
    SCATTERING_PER_M      = (0.010, 0.067, 0.120)
    PHASE_G               = 0.35

    => absorption/m  (0.711, 0.292, 0.185)
       extinction/m  (0.721, 0.359, 0.305)
       deep albedo   (0.014, 0.187, 0.394)     <- the colour of deep water

The shipped bed-visibility table is at `water_optics.py:111-114`, quoted for
**vertical** depth:

    bed visible through...   0.5 m        1 m          3 m          6 m
      SHIPPED             .70/.84/.86  .49/.70/.74  .12/.34/.40  .01/.12/.16

Emissive chain (`ue-project/Tools/water_sky_reflection_graph.py`), unrolled with
the shipped defaults:

    emissive = ( sun_glint + moon_glint
               + (sky_day + sky_night + stars) * Fresnel(0.02, 5) * 0.0   <- LegacySkyReflectGain
               + (sky_day + sky_night + stars) * Fresnel(0.00, 8) * 0.0   <- SurfacePresence
               ) * (1 - foam) * top_face_mask

    i.e.  emissive = (sun_glint + moon_glint) * (1 - foam) * top_face_mask

The star arm rides `LegacySkyReflectGain` too (`:980-989`), so the reflected
stars are also dead — stated because the arm's build-time A/B
(`VOXEL_WATER_STAR_REFLECT`) implies it is live and it is not.

---

## 2. MECHANISM-LEVEL DIAGNOSIS, OBSERVATION BY OBSERVATION

### Observation 1 — the far water gets DARKER with distance instead of going silver

**Three mechanisms stacking, in order of contribution.**

**1a. The engine deletes the water's own colour at grazing angles.**
`SingleLayerWaterShading.ush:231-238`:

    SeparatedWaterMainDirLightScatteredLuminance += ... * (bCameraIsUnderWater ? 1.0 : (1.0 - EnvBrdf)) * WaterVisibility;
    ScatteredLuminance *= bCameraIsUnderWater ? 1.0 : (1.0 - EnvBrdf);
    Transmittance      *= bCameraIsUnderWater ? (1.0 - EnvBrdf) : 1.0;
    ...
    Output.Luminance = WaterVisibility * (ScatteredLuminance + Transmittance * (BehindWaterSceneLuminance * ColorScaleBehindWater));

Above water only `ScatteredLuminance` is faded; `Transmittance` is left at 1.0.
So at grazing the water keeps its (fully absorbed, therefore black) transmitted
bed and loses its in-scatter. Our in-scatter IS the lake's colour — the deep
albedo (0.014, 0.187, 0.394) above. The measurement in
`water_sky_reflection_graph.py:930-945` already recorded what the volume alone
reads at a *non*-grazing pose: `0.000 / 0.008 / 0.005`. At NoV 0.5 that number
was already almost nothing; at NoV 0.1 it is multiplied by another ~0.2-0.0.

**1b. Nothing arrives to replace it.** `SingleLayerWaterComposite.usf:237-251`:

    float3 EnvBrdfValue = EnvBRDF(GBuffer.SpecularColor, GBuffer.Roughness, NoV);
    ...
    Reflection *= EnvBrdfValue;
    const float ShoreOpacity = saturate(DeltaDepth * 0.02f);
    Reflection.rgb *= ShoreOpacity;

`Reflection` is built from `CompositeReflectionCapturesAndSkylightTWS(...)` at
`:202-229`, plus an SSR layer at `:232-233` that is not running in mode 2.
With `NumCulledReflectionCaptures == 0` (the literal fallback at `:206` when no
capture exists) the cubemap half is the skylight alone. That skylight is the
`z = 0` real-time capture, so what the water mirrors at grazing is the sky as
integrated from 2.2 km below the terrain, through 15.7 density-units of height
fog against the player's 0.008 (`VoxelSkySubsystem.cpp:3076-3083`). A
fog-saturated cubemap is low-contrast, low-frequency and dim — the *opposite* of
the bright horizon band a grazing water surface should mirror.
`water_sky_reflection_graph.py:140-147` predicted this exact failure without
naming the capture position: "a cubemap that under-represents the bright horizon
sky a grazing water surface actually mirrors".

Note the second multiply: `ShoreOpacity = saturate(DeltaDepth * 0.02)` kills the
reflection entirely wherever the water column is under 50 cm — so shallow water
gets neither reflection nor (via 1a) its own colour.

**1c. The two material terms that could produce the silver ship at zero.**
`LegacySkyReflectGain = 0.0` and `SurfacePresence = 0.0`
(`water_sky_reflection_graph.py:255-266`). Both were owner decisions and both are
correct decisions *against the alternatives that were on the table at the time*
— see section 4. But the consequence is that the shipped material contains no
Fresnel response at all, and the shipped renderer contains no reflection worth
trading for. **The graph as it stands genuinely cannot produce "far water goes
silver". There is no term in it whose magnitude rises with grazing angle.**

### Observation 2 — no specular highlight, no sun glitter anywhere

There are two things a real lake shows and we have neither.

**2a. Broad environment specular: absent for the same reason as observation 1.**
`GBuffer.SpecularColor` survives the water coverage fade — the engine keeps it
deliberately (`BasePassPixelShader.usf:1380-1384`, "We also keep the
SpecularColor for sun/water interactions") — but with roughness 0.08 the
environment specular is a near-mirror sample of a cubemap that has no sun in it
and no contrast. There is nothing bright to mirror.

**2b. The analytic sun glint is real but is a needle, not a path.**
`water_sky_reflection_graph.py:336-351` (GLINT_CODE) is a representative-point
area light with a **0.55 degree** flat top (`SunGlintAngularRadiusDeg`, `:249`)
and a `pow(d, 900)` skirt (`GlintSpecularExponent`, `:247`). It is non-zero only
where the mirror ray about the pixel's own wave normal lands within roughly two
degrees of the sun. The width of the sun path on real water is set by the
**wave slope distribution**, and the module says so at `:435-437`: "The long
streak itself comes from the wave slope distribution... the representative point
only fixes the CORE of the highlight."

Our slope distribution is small, and gets smaller with distance:

* `WaveAmplitudeM = 0.25` on an ~8.6 cm field (`water_wave_graph.py:685`,
  `create_water_voxel_material.py:416-418`).
* The wave amplitude is scaled by `u^WindAmpExponent` with
  `u = |Wind| / WindRefSpeedMS` and exponent 1.0
  (`water_wave_graph.py:695`) — **at published-calm wind the field goes to
  glass**, and `create_water_voxel_material.py:1109-1154` documents that this is
  deliberate and unfloored. If the weather subsystem was publishing a light wind
  when the screenshot was taken, the whole slope distribution is scaled down
  linearly with it, and with it the glitter.
* At distance the normal is filtered by mip/TSR/`r.ScreenPercentage=65` toward
  the mean (flat), while **Roughness stays pinned at 0.08**. That is the classic
  specular-aliasing-to-nothing failure: the correct filtered roughness of a pixel
  covering many wave facets is far higher than 0.08, and keeping it at 0.08 with
  an averaged normal produces a mirror of nothing rather than a glitter field.

So: no broad glint (nothing to mirror), and the narrow glint only fires in a
small screen region and only if the wind is up. If the sun was not in the mirror
direction anywhere in the framed region, the frame contains literally zero
specular water response. That matches the report exactly.

### Observation 3 — no reflections of the sky, shoreline, dunes or canoe

**This is a hard architectural fact, not a tuning issue.** Reflecting *scene
geometry* in water needs one of: screen-space reflections, a planar reflection, a
reflection capture that contains the geometry, or ray-traced/Lumen reflections.
The project has none of them on water:

* SSR on water: only under `r.Water.SingleLayer.Reflection` modes 1 or 3
  (`SingleLayerWaterRendering.cpp:337-342`). We ship 2 (`DefaultEngine.ini:205`).
  Note the asymmetry recorded in the config's own comment at `:517-519`: opaque
  terrain runs SSR (`r.ReflectionMethod=2`, `:573`) while water opted out.
* Lumen on water: mode 1 only, and cannot be forced
  (`SingleLayerWaterRendering.cpp:367-372`). Also unavailable regardless — voxel
  terrain has no mesh distance fields (`DefaultEngine.ini:197-204`).
* Planar reflections: SLW has never supported them (`DefaultEngine.ini:184-185`).
* Reflection captures: none exist (section 0).
* The SkyLight cubemap: **contains the sky dome and the fog and no terrain
  whatsoever.** The marcher declines every capture view —
  `VoxelMarchRenderer.cpp:9555`,
  `if (InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection) { ...DeclinedNonPrimary++; return; }`,
  with the reasoning at `:9549-9551`: "the marcher draws near-field terrain into a
  probe nobody reads terrain out of". The fluid renderer does the same
  (`VoxelFluidRender.cpp:439`). Since terrain quads are retired
  (`VoxelBrickPool.cpp:668`) and the clipmap starts at 8 km
  (`VoxelClipmapActor.cpp:791-813`), **a capture taken anywhere near a lake sees
  no ground at all.** This is decisive for R4 below: adding a reflection capture
  cannot put the beach into the water, because the beach is not in any capture.

**Conclusion: there is currently no mechanism in the build by which the beach,
the dunes or the canoe could appear in the water. The observation is not a bug —
it is the absence of a feature that was never added.**

### Observation 4 — the lakebed is completely invisible, and no caustics

Four independent contributors; any one of them alone would be enough at some
depths.

**4a. The 2026-09-05 secant slant multiplies the effective depth by up to 20x.**
`bathy_field_graph.py:139-214`, `build_slant_depth`:

    cos_eff = sqrt(1 - (1 - vz^2) * BathyRefractInvN2)
    slant   = depth / max(cos_eff, 0.05)

with `BathyRefractInvN2 = 1.0` (`:208`) this degenerates to
`slant = depth / max(|NoV|, 0.05)`. The docstring is explicit about the 20x cap
at `:185-191`. A low aerial view of a lake has `|vz|` around 0.3-0.4 across most
of the water and far less near the far shore.

Applying that to the shipped table above (which is quoted for *vertical* depth):

    vertical depth   NoV 1.0 (straight down)   NoV 0.35 (a low aerial view)
      0.5 m          .70 / .84 / .86           1.43 m -> .36 / .60 / .65
      1.0 m          .49 / .70 / .74           2.86 m -> .13 / .36 / .42
      2.0 m          .24 / .49 / .54           5.71 m -> .02 / .13 / .18
      3.0 m          .12 / .34 / .40           8.57 m -> .00 / .05 / .07

`docs/lake-floor-ladder-2026-08-29.md:15-20` gives the median lake depth in this
world at the shipped 0.5 m floor as **1.17 m**, with the pre-existing population
at 1.93 m. So a typical lake, seen from the owner's camera angle, transmits
under 15% of its bed in the best channel and effectively none in red. **The
"somewhere in between" clarity midpoint the owner approved on 2026-08-12
(`water_optics.py:90-118`) was calibrated as a vertical-depth table, and the
2026-09-05 secant change multiplied it by 1/NoV at every non-vertical angle
without re-laddering that verdict.** The lake pose check that accompanied the
secant approval was "00686 looks pretty much the same as 00722. No preference"
(plan `:875`) — i.e. it confirmed the *close-up* look was preserved, which is
precisely the band where the secant and Snell curves are identical. The band it
changed was never re-judged against a bed-visibility criterion.

**4b. `(1-EnvBrdf)` does not fade the transmitted bed, but the engine's own
absorption does.** `Transmittance` above water is left at 1.0
(`SingleLayerWaterShading.ush:235`), so the bed is not deleted by Fresnel —
it is deleted by `exp(-ExtinctionCoeff * WaterVolumeDepth)`
(`:221-222`) where `WaterVolumeDepth = max(0, BehindWaterSceneDepth -
WaterSurfaceSceneDepth)` (`:160`) measured **along the view ray, with no upper
clamp**. That is the *same* 1/NoV elongation as 4a, applied a second time to the
engine's 15% share of the absorption and to 100% of the scattering.

**4c. Caustics cannot be on screen in this shot even if everything else worked.**
`water_caustics_graph.py:405-422` fades the term to zero between
`CausticFadeStartM = 40 m` and `CausticFadeEndM = 64 m` (`:180-181`) measured
from the camera. A low aerial lake shot is mostly beyond 64 m. Inside 64 m the
term is further gated by `saturate((sunZ - 0.10)/0.15)` (`:184-185`) and by
`saturate(depth/0.10)`, then attenuated down the sun ray by the same absorption
coefficients. **Caustics at `voxel.Water.Caustics 0.5` producing nothing visible
is the expected result of the shipped fade window, not evidence of a broken
caustic path.**

**4d. THE CAUSTICS ARE WIRED INTO TWO MATERIALS THAT DO NOT SHADE ANY NEAR-FIELD
TERRAIN. This is a hard finding, not a suspicion, and it is the whole answer to
"caustics are at 0.5 and I see none".**

`build_caustics` has exactly three consumers: `create_voxel_material.py:287-291`
(M_VoxelTerrain, emissive), `create_clipmap_material.py:368-372` (M_VoxelClipmap,
emissive) and `create_underwater_material.py:826-830` (the post-process, only
when submerged). The premise is stated at `create_voxel_material.py:69-70`: "The
lake and sea FLOORS are this material, so the caustics land here." **That premise
is false in the shipped configuration.**

* `voxel.March` **defaults to 1** — `VoxelMarchRenderer.cpp:66-67`. (The help
  string at `:73` and the header at `VoxelMarchRenderer.h:186` both still say the
  default is 0; both are stale, flipped by commit 8d884de.)
* `voxel.Terrain.RetireQuads` **defaults to 1** — `VoxelBrickPool.cpp:668-675`,
  "terrain is marched now". This retires terrain quad **production** on both the
  CPU worker and the GPU fork, so `M_VoxelTerrain` is still loaded and still
  assigned to chunk components and has **no quads to shade**.
* The clipmap's inner hole is exactly the ring cascade's outer edge —
  `VoxelClipmapActor.cpp:791-813` with `HoleHalfIndex = 16`
  (`VoxelClipmapActor.h:162`) and R7 outer = 8192 m
  (`VoxelWorldSubsystem.h:197`). **M_VoxelClipmap draws nothing inside 8 km.**
* A lake bed at 50-300 m is therefore drawn by the marcher, in rings R0-R3
  (`VoxelWorldSubsystem.h:188-192`; reach at
  `VoxelMarchRenderer.cpp:10388-10391`).
* And the marcher has **no caustic term at all**. A case-insensitive grep for
  `caustic` returns **zero hits** across `Source/VoxelEarthShaders/**` and
  `Shaders/**` (including `VoxelMarch.usf`). Its emissive is hand-written at
  `VoxelMarch.usf:3495` and `:3550-3552` and reads no MPC parameter and no
  bathymetry.

**So the Phase F1 caustic term lands on exactly nothing the player can stand next
to.** It is not a fade problem (4c is real but secondary), not an intensity
problem, and not a bug in `water_caustics_graph.py` — the field is correct and it
is connected to a material that does not draw the lake floor. See R7.

**4f. The bed IS reachable through the water, which is the good news.** The
marcher writes the real `SceneDepth` in `PreRenderBasePass_RenderThread`
(`VoxelMarchRenderer.cpp:9663`, emit at `:11447`, `:11461-11464`, RDG event
`VoxelMarch.DepthPreEmit`; the reason it is there rather than later is quoted at
`:11419-11427` — "a raster pass at PostRenderBasePassDeferred lands its colour
and DROPS its depth") and writes GBuffer + SceneColor in
`PostRenderBasePassDeferred_RenderThread` (`:11854`, targets at `:12005-12019`,
bound at `:12364-12387`). Both run inside `RenderBasePass`, long before
`RenderSingleLayerWater`. **So `SceneColorWithoutSingleLayerWater` and
`BehindWaterSceneDepth` do contain the marched lake bed, and `WaterVolumeDepth`
is computed against real bed depth.** The bed is invisible because of 4a/4b —
the optics — and not because the water has nothing behind it.

**4e. And the thing there is to see is flat.** ADR-0008 (accepted 2026-08-11)
fixes every voxel face at one flat colour with no texture and no sub-voxel
detail (`docs/adr/0008-flat-per-voxel-material-colour.md:46-50`). So "no sand
texture, no rocks" is doctrine, not a defect — what a visible bed would give you
here is the per-voxel jitter and the slow patch term of invariant 3, plus AO.
That is still a big improvement on nothing, but it is worth being honest that the
bed will not look like a photograph of sand when it comes back.

### Observation 5 — the depth gradient reads like a bathymetric map

This is the shipped design working as specified, at a strength nobody has judged
at this camera angle.

* `ColorScaleBehindWater` is `exp(-absorb_per_cm * 0.85 * validity * slant_uu)`
  (`create_water_voxel_material.py:1477-1485`) — a smooth, wide-band exponential
  ramp keyed on the **bilinearly-filtered baked depth** (1.875 m per texel,
  `bathy_field_graph.py:5-17`). That produces exactly the smooth iso-depth bands
  in the screenshot.
* The bands are highly saturated because that is what the numbers say: red is
  absorbed 3.85x faster than blue (0.711 vs 0.185 per m), the deep-water albedo
  is `(0.014, 0.187, 0.394)`, and the only term that would desaturate it toward
  sky — a Fresnel reflection — is at zero. On a real lake the postcard-cyan
  shallows are *diluted* by the sky reflected off the surface above them; ours
  are not diluted by anything.
* The bands are wide and hit "navy" quickly because the whole ramp is spent
  inside about 3 m of vertical depth (4a's table), so an entire lake's depth
  range is compressed into two or three visible steps.

The "tropical postcard" read is therefore the *combination* of a correct
absorption ramp with a missing reflection, not a bad absorption ramp on its own.

### Observation 6 — thin dark stair-stepped contour lines all over the surface

**The material's own comment names this artefact and names the reason it
survives.** `create_water_voxel_material.py:1001-1010`:

    THE TWO DEFECTS, precisely, because "baked is better" is not an argument:
      * the bed is a 10 cm voxel staircase, so a scene-depth difference steps
        with it and draws CONTOUR RINGS in the water colour;

`BathyDepthAuthority = 0.85` (`:1011`) was added to fix that — by moving 85% of
the **absorption** onto the smooth baked field. But two lines up, at `:995-999`:

    SCATTERING IS NEVER SPLIT and always stays with the engine, deliberately

So **100% of the in-scatter still reads `WaterVolumeDepth`, which is
`BehindWaterSceneDepth - WaterSurfaceSceneDepth` off the 10 cm voxel staircase**
(`SingleLayerWaterShading.ush:160`), and so does the engine's remaining 15% of
the absorption (`create_water_voxel_material.py:1510`). Every voxel step in the
bed is a step in the water's colour.

Two things make the steps read as thin dark *lines* rather than as broad bands:

* At a grazing view a 10 cm vertical riser in the bed is a *large* jump in
  view-ray depth (the same 1/NoV amplification as 4a), so the step edge is a
  sharp discontinuity even though the bed step is small.
* The direction of the jump is darker: extra depth removes more transmitted bed
  immediately, while the in-scatter that should compensate is already near its
  saturation value and barely moves. Net: a dark line on the deep side of every
  riser, following iso-depth by construction.

They are stair-stepped because they are literally the voxel grid, and they cover
the whole lake because the entire bed is voxels. **The current graph cannot
remove them: the SLW node has no hook that lets the material supply its own depth
to the scattering integral.** See R7 for what can be done instead.

*Second candidate, listed and not preferred:* the wave field's breaking band is
also keyed on baked depth (`BreakSurfFloorM = 0.15`, `BreakDepthRatio = 1.28`,
`water_wave_graph.py:720-726`) and would draw iso-depth features. It is ruled out
as the primary cause because that band produces WHITE foam riding crests, not
1-pixel dark stair-steps, and because it is confined to shoaling water rather
than "all over the lake". The A/B in R7 separates them cleanly.

### Observation 7 — blocky stair-stepped waterline, no foam, no wet sand

Four contributors:

* **The sheet's extent mask is quantised to 1.875 m and deliberately
  over-covers** (`create_water_voxel_material.py:2194-2201`, citing
  `water-architecture.md:231-242`). The shore-SDF clip that cuts it back
  (`:2236-2263`) is a hard `step()` against a field baked at 1.875 m, so the cut
  line is a 1.875 m staircase softened only by bilinear filtering.
* **The clip ships OFF by default on the material** (`WaterShoreClipEnabled`
  default `0.0`, `:2236`, with the reasoning at `:2225-2235`) and is turned on
  per-consumer through the sheet's MID. So whether the shot has the clip at all
  depends on which draw path drew the waterline.
* **Shoreline foam is the one foam signal that fires on a settled lake**
  (`BathyFoamGain = 0.55`, `:1748`), and every foam signal lands on BaseColor and
  on Opacity. With `Opacity = saturate(foam)`, `BasePassPixelShader.usf:1383-1384`
  multiplies `GBuffer.DiffuseColor` by that same coverage — see section 3 — so a
  weak foam term is doubly weak: low coverage AND low albedo through it. A 0.55
  gain over a 1.6 m band on a 1:20 shelf is a faint grey-white, not a foam line.
* **Wet-sand darkening is the terrain material's, not this one's**
  (`:960`, "wet shores are the TERRAIN material's"), behind `VOXEL_SHORE_FX` in
  `terrain_material_common.py`. It has never been confirmed in a capture — the
  generator says so itself at `:657-661`: "Both effects were built, wired and
  shipped, and NEITHER has ever been confirmed in a screenshot."

The 2026-09-05 owner verdict already records the residual: "Black band is mostly
gone but there is still black/dark shading and slight band around the pond"
(plan `:858`), with the slack ladder measured and **owner judgment still
pending** (`:860`).

### Observation 8 — the canoe sits ON the water with no interaction

* **No reflection of the boat is possible** — see observation 3. Nothing in the
  build can put a scene object into the water.
* **No contact shadow / darkening / meniscus exists.** There is no term anywhere
  in `create_water_voxel_material.py` keyed on proximity to an object. The one
  object-aware term is `water_hull_mask_graph.build_hull_mask`
  (`:2292-2307`), and it is a *discard* — it removes water inside the hull
  volume. Its correct behaviour is to be invisible.
* **The wake/ripple foam is the known-broken path** and is excluded from this
  document by instruction. Noted only so the list is complete.

---

## 3. THE FRESNEL / REFLECTION QUESTION, ANSWERED SPECIFICALLY

The brief asks: what does SLW actually do with our BaseColor / Emissive /
Opacity / Normal / Roughness, are we getting any real Fresnel-weighted
reflection, and what would it take to get one.

### 3.1 What each pin does on this shading model

**Opacity is `BaseMaterialCoverageOverWater`, and it gates BaseColor.**
`BasePassPixelShader.usf:1140-1141`:

    const float BaseMaterialCoverageOverWater = Opacity;
    const float WaterVisibility = 1.0 - BaseMaterialCoverageOverWater;

and `:1380-1384`:

    // Fade out diffuse as this will be handled by the single scattering lighting in water material.
    // We also keep the SpecularColor for sun/water interactions.
    GBuffer.DiffuseColor *= BaseMaterialCoverageOverWater;
    DiffuseColor *= BaseMaterialCoverageOverWater;

Our `Opacity = saturate(foam)`. On a settled lake foam is 0, so coverage is 0,
so **BaseColor is multiplied by zero.** This is worth stating plainly because
the project currently carries a stronger conclusion than the evidence supports.
`create_water_voxel_material.py:1940-1954` records a 2026-08-30 experiment that
rewired BaseColor to a constant and measured a byte-identical null, concluding
"Whatever SLW does with BaseColor here, it is not what that reading assumed", and
the session-9 note at `:2077-2091` escalates that to "EVERY foam signal lands
only on BaseColor... The art has been painted onto a dead channel all along."

**The mechanism is not mysterious and BaseColor is not dead — it is
coverage-gated, by our own wiring.** Note also that the saturated control arm in
that experiment (`BathyFoamWidthM 400 + BathyFoamGain 5`) is tautological: where
foam >= 1 the shipped `lerp(black, foam_tint, foam)` already returns exactly
`foam_tint`, so replacing the lerp with a constant `foam_tint` is a no-op by
construction, and a null result there proves nothing. I am **not** proposing a
change to the foam path (that is the owner's live debugging), only correcting the
mechanism on the record, because Opacity's behaviour is load-bearing for
observations 4 and 7 and for any future reflectance work.

**Emissive is outside the engine's energy split entirely.** It is added after
everything, which is why `water_sky_reflection_graph.py:955-964` says the retired
term "did not merely brighten the water, it BURIED the volume". Anything put
there is unconditioned by Fresnel unless the material supplies the Fresnel
itself.

**Normal feeds everything.** `MaterialParameters.WorldNormal` is what
`EnvBRDF(..., max(0,dot(N,V)))` reads (`BasePassPixelShader.usf:1707-1710`) and
what the material's own `ReflectionVectorWS` / `Fresnel` nodes default to
(`water_sky_reflection_graph.py:69-78`). Our normal is
`(-dHdx, -dHdy, 1) * VertexColor.B`, deliberately unnormalised so octaves sum as
gradients (`create_water_voxel_material.py:2645-2653`).

**One thing to verify in the far field:** the normal is masked by
`VertexColor.B`. On the far-field lake sheet that is fine — every sheet vertex is
written `FColor(255, 255, 255, 0)` at `VoxelWaterSheetActor.cpp:80-88`, so B = 1
and A = 0 across the entire sheet, which means the wave normal DOES run on the
sheet and vertex-activity foam is identically zero out there. On the near-field
pooled path B is conditional per corner
(`VoxelWaterChunkComponent.cpp:243-244`) and A carries real activity. So the two
draw paths differ in exactly one visible way on a calm lake: only the near field
can ever show activity foam.

**Roughness (0.08) and Specular (0.5, F0 = 0.04)** are the two inputs to
`EnvBrdf`, and therefore they are the knob that decides how much of the water's
own colour the engine throws away at grazing. **This is the least-used lever in
the whole material and the one with the most leverage.** See R4.

### 3.2 Are we getting any real Fresnel-weighted reflection?

**Material side: no.** Both Fresnel nodes are built
(`water_sky_reflection_graph.py:905-907` front Fresnel, base 0.02 exponent 5;
`:1024-1026` grazing Fresnel, base 0.0 exponent 8) and both terminate in a
multiply by a scalar that ships at 0.0. Zero contribution, at every angle, day
and night, including the reflected stars.

**Engine side: yes, in the sense that `EnvBrdf` is a real Fresnel-weighted split
— but it is currently a one-way valve.** It removes the volume at grazing
(`SingleLayerWaterShading.ush:234`) and multiplies a reflection into the frame
(`SingleLayerWaterComposite.usf:247`) that has almost nothing in it. The measured
evidence that the engine reflection is *not* zero is in the tree:
`water_sky_reflection_graph.py:930-945`, at camera 12 m / pitch -30 / noon, the
engine term alone read `0.106 / 0.203 / 0.287` display-linear. That is a real
number, at NoV ~0.5. What nobody has measured is what it reads at NoV 0.1, which
is the band the owner is complaining about, and where `ShoreOpacity` and the
capture's fog-washed horizon both bite hardest.

### 3.3 What the project settings are, exactly

Complete, from `ue-project/Config/` (there is no `DefaultScalability.ini`, no
platform override directory, and no `r.*` in `Saved/Config`):

| Setting | Value | Where |
|---|---|---|
| `r.Water.SingleLayer.Reflection` | **2** (captures + skylight only) | `DefaultEngine.ini:205` |
| `r.ReflectionMethod` | **2** (SSR — opaque only) | `DefaultEngine.ini:573` |
| `r.SSR.Quality` / `r.SSR.HalfResSceneColor` | 3 / 0 (full-res) via `sg.ReflectionQuality=3` | `DefaultEngine.ini:548-551` |
| `r.DynamicGlobalIlluminationMethod` | ABSENT -> engine default 0 = None | noted `DefaultEngine.ini:555` |
| every `r.Lumen.*` | **ABSENT** | — |
| every `r.ReflectionCapture*` / `r.SkyLight*` | **ABSENT** | — |
| `r.Substrate` | **ABSENT** -> off | — |
| `r.ForwardShading` | **ABSENT** -> deferred | — |
| `r.SeparateTranslucency` | 0 | `DefaultEngine.ini:176` |
| `r.ScreenPercentage` | 65 | `DefaultEngine.ini:142` |
| `r.AntiAliasingMethod` | ABSENT -> TSR (UE5 default) | — |
| `r.VolumetricFog` | 1 | `DefaultEngine.ini:475` |
| exposure | driven from C++, `AEM_Manual` on the ladder path | `VoxelSkySubsystem.cpp:4166-4187` |

Actors: **one** `ASkyLight`, spawned at runtime, Movable, real-time capture on,
placed at `(SpawnColumnXUU, SpawnColumnYUU, 0.0)`
(`VoxelSkySubsystem.cpp:3006-3097`). **Zero** reflection captures. **Zero**
planar reflections. **Zero** `.umap` files.

### 3.4 So: does SLW need something to produce a sky reflection at all?

Yes, and specifically one of these four, in ascending order of cost:

1. **Move the SkyLight capture out of the fog.** `voxel.Sky.SkyLightAtGroundZ 1`
   (`VoxelSkySubsystem.cpp:785-787`) plus `voxel.Sky.FogInSkyCapture 0`. Live
   cvars, no rebuild, no regen. This does not create a reflection mechanism; it
   makes the one we have contain a sky.
2. **Add a reflection capture — and this is now known to be nearly worthless.**
   A capture placed near a lake would contain the sky dome and the fog and no
   terrain (`VoxelMarchRenderer.cpp:9555`), i.e. the same content the SkyLight
   already has, at a different position. It can only ever duplicate option 1.
   Ranked and kept only so the reasoning is on the record — see R4.
3. **`r.Water.SingleLayer.Reflection 1`.** Restores the engine default and turns
   SSR on for water (SSR is already on and at full res for opaque). This is the
   only mechanism in the build that can put the beach, the dunes and the canoe
   into the water. It is a one-cvar A/B with no rebuild and no regen.
4. **Re-arm a material-side Fresnel term.** `SurfacePresence` 0.15-0.3 is the
   documented next ladder (`water_sky_reflection_graph.py:206-211`) and needs no
   regeneration. It must be judged AFTER 1-3, or it re-creates the 2026-08-12
   double-count.

---

## 4. THINGS HERE THAT CONTRADICT AN EXISTING OWNER VERDICT

Flagged explicitly, per the brief.

**(a) Re-proposing a grazing sky sheen contradicts the 2026-09-05 rejection.**
Verdict, verbatim (plan `:873`): REJECTED — "00698 honestly looked better with
its green coloring. But however still look too transparent. 00714 looks very
transparent at a distance"; "00686 looks better than 00716". The reading recorded
at `water_sky_reflection_graph.py:198-205`: "at 1.0 it washed the deep teal
toward pale sky-blue, which the owner reads as MORE transparent — his model of
'reads as water' is the water's own saturated body colour beating the bed, not a
mirror."

**How R9 differs.** Four ways, and all four matter:
* **Gain.** 0.15-0.3, not 1.0. This is not a new idea — it is the next rung the
  module itself parks at `:206-211` ("a small gain (0.15-0.3) riding on top of
  the slant path is the expected NEXT ladder").
* **What it now rides on.** When 1.0 was judged, the slant path was not shipped;
  the choice presented was *mirror-sky* OR *body colour*. Both now exist. The
  ladder the module prescribes is sheen ON TOP OF the approved slant default.
* **The complaint has changed.** The rejected sheen was answering "too
  transparent". The current complaint is the opposite end: the water is now dark
  and matte and reflects nothing. A sheen judged against "is the bed too visible"
  is a different question from one judged against "does the far water go silver".
* **And it should be judged LAST.** If R1/R2 give the engine a real reflection,
  an emissive stand-in double-counts and buries the volume — the exact defect
  measured on 2026-08-12 (`water_sky_reflection_graph.py:930-945`).

**(b) Re-laddering `BathyRefractInvN2` contradicts the 2026-09-05 approval.**
Verdict (plan `:875`): APPROVED — "00720 looks much better"; lake pose "00686
looks pretty much the same as 00722. No preference. Proceed."

**How R5 differs.** The approval was taken at a *tidal-flats* pose, against the
sheen as the alternative, and the lake-pose half of it confirmed only that the
CLOSE-UP look was unchanged — which is guaranteed by construction, because at
`vz = 1` the secant and Snell curves are identical (`bathy_field_graph.py:178`).
The band the change actually moved is the grazing band, and the frame the owner
is complaining about now is squarely in it. R5 does not propose reverting to
Snell; it proposes laddering the **elongation cap** (the `max(cos_r, 0.05)` floor
at `:213`, currently 20x) which is a knob nobody has ever moved, and which
bounds the damage without touching the curve's shape at moderate angles.

**(c) Nothing here re-opens `LegacySkyReflectGain`.** That term is a
*constant-sky* stand-in added on Emissive, outside the energy split, and its
retirement measurement (`:930-945`) stands. If a Fresnel term comes back it
should be the `SurfacePresence` branch (zero base, exponent 8), which is
angle-separated by design, not the front-Fresnel branch.

**(d) Nothing here re-opens the black-band mechanisms.**
`docs/lake-sheet-black-band-2026-08-29.md` records four refuted mechanisms and an
engagement-proven null. None is re-proposed. Note, though, that this analysis
supplies a *fifth* mechanism the document did not have — `ScatteredLuminance *=
(1 - EnvBrdf)` combined with a near-empty `Reflection` — which is a better fit
for "a dark rim wherever the water column is thin" than any of the four, because
`SingleLayerWaterComposite.usf:249-251` kills the reflection under 50 cm of water
while `:234` is simultaneously killing the volume. That is offered as an
observation, not as a re-diagnosis.

---

## 5. THE RANKED PLAN

Every item states: mechanism, expected visual difference in the owner's terms,
implementation, cost, risk, and the A/B. House rule honoured throughout: an
appearance change ships behind a scalar or cvar whose OFF arm is byte-identical,
and the owner judges frames.

**Rank is by realism-per-effort, not by size.**

---

### R1 — Turn SSR on for water: `r.Water.SingleLayer.Reflection` 2 -> 1
**No rebuild. No regen. One cvar. Run this first.**

* **Mechanism.** Mode 1 enables SSR on the water pass
  (`SingleLayerWaterRendering.cpp:337-342`), which is the only thing in the build
  that can reflect scene geometry into water. SSR is already ON and at full
  resolution for opaque surfaces (`r.ReflectionMethod=2`,
  `sg.ReflectionQuality=3` -> `r.SSR.Quality=3`, `r.SSR.HalfResSceneColor=0`), so
  the technique is already paid for elsewhere in the frame. Mode 1 also layers
  SSR over the capture/skylight base rather than replacing it
  (`SingleLayerWaterComposite.usf:232-233`:
  `Reflection = SSR.rgb + Reflection * (1 - SSR.a)`), so where SSR misses,
  today's picture is what remains.
* **Expected visual difference, in the owner's words.** "The beach and the dunes
  should appear upside-down in the water below them, and the canoe should sit in
  its own reflection instead of on a painted surface. The far water should stop
  being flat navy where the shoreline meets it."
* **Implementation.** `-ExecCmds="r.Water.SingleLayer.Reflection 1"` for the
  trial; if the owner keeps it, one line in `DefaultEngine.ini` replacing `:205`,
  with the existing comment block at `:178-204` amended rather than deleted (it
  is the record of why 2 was chosen, and the reasons — SSR is a scene-colour
  read, it is noisy, the frame is render-thread bound — are still true and now
  have to be weighed against a picture).
* **Cost.** GPU. Unmeasured on water. `DefaultEngine.ini:194` names the two
  companion cvars (`r.Water.SingleLayer.Reflection.Denoising`, `.SSRTAA`) that
  may be needed. `.DownsampleFactor` (`SingleLayerWaterRendering.cpp:73-76`) is
  the cheap dial if it is too expensive at full rate. **Measure, do not predict.**
* **Risk. HIGH, and it is a NAMED, ALREADY-DOCUMENTED debt — read this before
  shooting the pair.** The marcher's own header, `VoxelMarchRenderer.h:169-175`:

      (d) THE HZB IS BUILT BEFORE OUR DEPTH LANDS (DeferredShadingRenderer.cpp:
      2752/2784), so Lumen screen traces and SSR accelerate against an HZB with
      no terrain in it and will overshoot. This is the seam's one genuine
      quality debt.

  So SSR on water will trace against a hierarchical depth buffer that does not
  contain the beach, the dunes or the terrain the reflection is supposed to find.
  The expected failure is overshoot: rays sail past the shoreline and either miss
  (falling back to the cubemap, i.e. today's picture) or hit something far behind
  it. `docs/marcher-to-primary-plan-2026-08-21.md:140-141` records the same thing
  and names the falsification (`r.Lumen.ScreenProbeGather.ScreenTraces 0/1`).
  **This does not make the experiment worthless** — SSR also traces the full-res
  depth buffer, which DOES contain the marched bed
  (`VoxelMarchRenderer.cpp:11447`), and the HZB is an acceleration structure, so
  the failure is "misses and streaks", not "traces nothing". But if the pair comes
  back with streaks that follow terrain silhouettes, the fix is upstream in the
  marcher's HZB ordering and is a much bigger job than a cvar. Budget for that
  answer.
  Secondary risk: SSR's ordinary disocclusion smearing is worst at grazing
  angles, which is exactly the band we care about.
* **A/B.** Two captures, same settled pose, same `-TimeScale 0`,
  `-ExecCmds="r.Water.SingleLayer.Reflection 2"` vs `... 1`. Cvar override is
  `SetByCommandline`, which outranks the ini (`DefaultEngine.ini:566`). The OFF
  arm is byte-identical by construction — it is the shipped value.
  **Engagement proof:** the frames must differ in the water and nowhere else;
  if a land patch moves, the exposure moved and the pair is void.

---

### R2 — Move the SkyLight capture out of the fog
**No rebuild. No regen. Two live cvars.**

* **Mechanism.** The only cubemap the water can mirror under mode 2 is the
  real-time SkyLight, and its capture is taken from `z = 0` — 2187.6 m below the
  player's ground and inside 15.7 fog-density units against the player's 0.008
  (`VoxelSkySubsystem.cpp:3064-3091`). `voxel.Sky.SkyLightAtGroundZ 1` moves it
  to ground; `voxel.Sky.FogInSkyCapture 0` takes the fog out of it. Both are
  applied live (`ApplySkyLightPlacement`, `VoxelSkySubsystem.h:448-456`).
* **Expected visual difference.** "The water should stop mirroring grey soup and
  start mirroring an actual sky — brighter and bluer toward the far shore instead
  of darker."
* **Implementation.** `-ExecCmds`, then (if kept) flip the two cvar defaults.
* **Cost.** Zero GPU. It changes the whole world's ambient term, which is a
  whole-frame appearance change and is exactly why the default was left at 0
  (`VoxelSkySubsystem.cpp:3085-3091`).
* **Risk.** Real: the night exposure rungs were calibrated against today's
  ambient (`VoxelSkySubsystem.cpp:773-784`), so this can move night frames. Judge
  a day pair and a night pair.
* **THE MEASURED NULL THAT DOES NOT APPLY HERE, stated so it is not misread.**
  `VoxelMarchRenderer.cpp:1016-1021` records that both arms and their combination
  moved marched shaded faces from `(60.6, 80.8, 109.8)` to `(60.1, 81.0, 110.3)`
  — nothing — and says "Do not re-run that sweep." That measurement is about the
  **marcher's own hemisphere ambient**, which is an emissive term injected from
  the marcher's pass and does not read the SkyLight at all (`:1023-1028`,
  and the marcher explicitly does not read sky colour back from the SkyLight —
  `:1279`, `:1313`). **The water's specular reflection reads the SkyLight cubemap
  directly, through a completely different path.** The null does not transfer,
  and the sweep is worth re-running on a WATER pose, which the original never
  was.
* **A/B.** Four captures at one pose: baseline; `SkyLightAtGroundZ 1`;
  `FogInSkyCapture 0`; both. OFF arm = the shipped defaults, byte-identical.

#### MEASURED 2026-09-07 (session: R2/R3/R5). VERDICT PENDING OWNER.

**No code change was made and none is needed to run this.** R2 is written up
above as C++ work; it is not. Both cvars already exist, both are already applied
live every frame, and both say in their own help text that they are live
*specifically so this sweep could be shot* through `-ExecCmds`
(`VoxelSkySubsystem.cpp:786-811`, `ApplySkyLightPlacement` at `:4232`,
`ApplyFogFromState`'s capture flag at `:4486-4492`). The shipped defaults are
untouched: flipping them is an appearance change and belongs to the owner, on
these frames.

Four arms, one settled pose (`-65102,-51084`, +6 m, pitch -18, yaw 45, 75 s
settle, frozen 12:00 03-20), all four shot against **one identical water
material** (md5 `C316CBC8…`, verified unchanged across every arm and verified
free of the other session's debug arms via the regen log's `SHORE FX ARM: ON` /
no-debug-marker check).

| arm | frame | far water dRGB | mid water dRGB | near water dRGB | sky ctrl | land ctrl |
|---|---|---|---|---|---|---|
| baseline | `VoxelVerify00874.png` | — | — | — | — | — |
| `SkyLightAtGroundZ 1` | `VoxelVerify00884.png` | +5.95/+5.97/+5.61 | +18.6/+21.8/+25.1 | -14.0/-10.8/-10.3 | 0.08 | 0.32 |
| `FogInSkyCapture 0` | `VoxelVerify00888.png` | +7.90/+7.37/+6.43 | +30.3/+32.0/+32.9 | -5.5/-2.6/-1.0 | 0.05 | 0.20 |
| both | `VoxelVerify00890.png` | +6.07/+6.09/+5.79 | +19.2/+22.5/+26.6 | -13.7/-10.4/-9.1 | 0.08 | 0.31 |

(Frames in `ue-project/Saved/Screenshots/WindowsEditor/`; logs
`Saved/capture-r2-*.log`. dRGB is the mean 0-255 change against the baseline.)

**The engagement proof passed, and it is a read-back, not an echo.**
`SkyLightAtGroundZ 1` logs the capture point moving `z=0.0 m -> z=1644.2 m` read
back off the actor; `FogInSkyCapture 0` logs `inRealTimeSkyCapture=1 -> 0` read
back off the component. The two controls did not move: the sky band by 0.05-0.08
and the land band by 0.20-0.32 out of 255, i.e. nothing. **The exposure did not
move, so the pair is valid** — and the slab breakdown puts the whole difference
in the water (top slab, sky and dunes, mean |diff| 0.86; the water slabs 12-45).

**The null of `VoxelMarchRenderer.cpp:1016-1021` genuinely does not transfer, as
predicted.** That sweep found nothing on marched faces. On a water pose the same
two cvars move the mid-water band by up to **32/255**. R2's reasoning for
re-running it was correct.

**Direction: it does what the section predicted, and more than predicted.** Far
and mid water get **brighter and bluer** — in the mid band the blue channel
gains most (+25.1 against red's +18.6 for `SkyLightAtGroundZ`, +26.6 vs +19.2
for both), which is exactly "stop mirroring grey soup and start mirroring an
actual sky". The near field gets **darker** (-9 to -14), which is the same
mechanism read the other way: the near water is looking at a steeper part of the
cubemap and no longer picks up the fog's flat grey lift.

**The single biggest arm is `FogInSkyCapture 0` ALONE, not both together, and
that is not a measurement error.** `nofog` moves the mid band +31.7 mean; `both`
moves it +22.8. The arms are strongly non-additive because they are two ways of
fixing the *same* defect: once the capture is lifted to ground z the fog around
it is thin anyway, so removing the fog from it adds almost nothing. `both` lands
essentially on top of `groundz` alone (mid +22.8 vs +21.8), confirming this.
If the owner wants one cvar rather than two, `FogInSkyCapture 0` is the one
carrying the picture.

**A correction to this section's arithmetic, from the run's own log.** The
"2187.6 m below the player / 15.7 fog units / 1960x" figures are for the seed
this document was written against. At the pose actually shot the spawn column's
ground is **1644.2 m**, and the fog APPLIED line reports the effective density
at the capture point as **2.387** against the player's **0.0078** — a factor of
**305x**, not 1960x. The mechanism and the direction are unchanged; the
magnitude is a third of the quoted one. Anyone re-deriving should read the log
line rather than this paragraph.

**UNVERIFIED, and R2 asks for it explicitly: the NIGHT pair was not shot.** The
risk this section names — that the night exposure rungs were calibrated against
today's ambient — is untested here. Every frame above is frozen noon. A night
pair is the remaining work before this can be judged safe to default.

---

### R3 — Distance/variance-driven roughness on the water
**Regen. One new scalar. Default 0.0 = byte-identical.**

* **Mechanism.** Two problems, one fix. (i) `EnvBrdf` at Roughness 0.08 rises
  almost to 1 at grazing and deletes the entire volume term
  (`SingleLayerWaterShading.ush:234`); a higher roughness there keeps more of the
  water's own colour. (ii) A far pixel covers hundreds of wave facets whose
  normals average to flat, and shading a many-facet pixel with a 0.08 mirror
  roughness is the textbook way to get no glitter at all. The correct filtered
  roughness rises with the sub-pixel normal variance — Toksvig / LEAN, or the
  cheap version: ramp roughness with distance.
* **Expected visual difference.** "The far water should stop being a dead flat
  navy sheet — it should keep more of its own colour AND start to sparkle, so
  there is a broad glitter path under the sun instead of nothing."
* **Implementation.** In `create_water_voxel_material.py` at the roughness block
  (`:2340-2352`), lerp the calm roughness from 0.08 toward
  `WaterRoughnessFar` (suggest 0.30) over a `WaterRoughnessFadeStartM` ->
  `...EndM` camera distance ramp (60 m -> 400 m as a first guess), gated by a new
  scalar `WaterRoughnessFarGain` **default 0.0**. At 0.0 the material is
  byte-identical to today. The distance expression is already available in the
  same shape `water_caustics_graph.py:405-422` uses
  (`WorldPosition` vs `CameraPositionWS`, `Distance`, ramp) — reuse that idiom,
  do not invent a second one. The foam lerp stays downstream and unchanged.
  *Better version, if the effort is available:* derive the roughness from the
  wave field's own gradient magnitude (the field already returns `dH/dx, dH/dy`)
  combined with `ddx/ddy` of it, which is a real variance estimate rather than a
  distance proxy — but the distance ramp is the one-hour version and gets most of
  the read.
* **Cost.** A handful of ALU on the water pass. Regen: this is
  `create_water_voxel_material.py`, so a **water-only** regen (`-Only`) suffices —
  no new MPC parameter, so the sky collection is untouched and a full chain is not
  required (`tools/voxel-sky-chain-regen.ps1`, and the plan's rule at `:255-258`
  that a new MPC name forces a FULL chain — this change adds none).
* **Risk.** Low. Two things to watch: raising roughness also blurs the analytic
  glint's *environment* companion (it does not touch the analytic glint itself,
  which is roughness-free by design — `water_sky_reflection_graph.py:80-84`), and
  a roughness ramp is a visible "the water goes matte out there" ring if the fade
  is too short. Ramp over hundreds of metres, not tens.
* **A/B.** `-VoxelWaterMatScalar=WaterRoughnessFarGain:0` vs `:1` at one pose.
  **Caveat that must be stated when the pair is handed over:** that probe is
  **sheets-only** (`VoxelWaterSheetActor.cpp:310-311` — "set ANY scalar parameter
  on the sheet material, sheets only"). Near-field pooled water binds
  `/Game/Voxel/M_WaterVoxel` with no MID, so in the ON arm the near field and the
  far field will disagree. For a clean whole-water arm, either regenerate both
  ways or shoot a pose where only sheet water is visible.

---

### R4 — A reflection capture. **RANKED AND NOT RECOMMENDED. Kept so nobody spends a day on it.**

* **The idea.** If R1 (SSR) is rejected on cost or on the HZB debt, the obvious
  fallback under mode 2 is one large `ASphereReflectionCapture` over the play
  area, giving the capture path something with a horizon in it.
* **Why it does not work here.** The marcher declines every capture view
  (`VoxelMarchRenderer.cpp:9555`, reasoning at `:9549-9551`), terrain quads are
  retired (`VoxelBrickPool.cpp:668`) and the clipmap starts at 8 km
  (`VoxelClipmapActor.cpp:791-813`). **A reflection capture at a lake would
  contain the sky dome and the height fog and nothing else** — which is exactly
  what the SkyLight's real-time cubemap already contains, at a worse position.
  A sphere capture is therefore a second copy of R2's content, and R2 is free.
* **The only version of this worth building** is not a capture at all: it is
  removing the marcher's capture decline for the SkyLight's cube faces
  specifically, so the ambient/reflection cubemap actually contains ground. That
  is a real change to a hot path, it was added to fix an RDG crash (`:9538-9540`),
  and marching six 128x128 cube faces every capture is exactly the waste the
  comment refuses. **Do not attempt it as part of a water look pass.**
* **If it is attempted anyway:** spawn behind `voxel.Sky.ReflectionCapture`
  default 0 in `UVoxelSkySubsystem::SpawnRig` (`VoxelSkySubsystem.cpp:3005-3098`)
  so the OFF arm spawns no actor and is byte-identical.

---

### R5 — Take the contour lines out of the depth grading
**Regen. Contradicts a stated design principle in the generator — see below.**

* **Mechanism (observation 6).** 100% of the in-scatter and 15% of the absorption
  read the engine's `WaterVolumeDepth`, which steps on the 10 cm voxel bed
  (`SingleLayerWaterShading.ush:160`; the artefact is named at
  `create_water_voxel_material.py:1001-1005`). The baked field exists precisely to
  give a smooth depth, and it is already wired to absorption at 0.85 authority —
  but `create_water_voxel_material.py:995-999` states SCATTERING IS NEVER SPLIT.
* **Step 1 is a MEASUREMENT, not a change.** Before touching anything, shoot
  `-VoxelWaterMatScalar=BathyDepthAuthority:1.0` against the default 0.85 at a
  pose that shows the contour lines. At 1.0 the engine's absorption share is
  zero and only the scattering still reads the staircase.
  * If the lines survive at 1.0 -> it is the scattering, and the fix below
    applies.
  * If the lines vanish at 1.0 -> it was the 15% absorption residue, and the fix
    is one number, not a redesign.
  * If they neither vanish nor survive but MOVE -> it is the wave field's
    depth-keyed breaking band, and the fix is upstream in
    `water_wave_graph.py`'s `BreakSurfFloorM`.
  (Note the ladder is sheets-only, per R3's caveat.)
* **The fix, if it is the scattering.** Drop `ScatteringCoefficients` toward zero
  and re-supply the deep-water colour as a smooth baked in-scatter on EMISSIVE:
  `albedo * (1 - exp(-extinction * slant_baked)) * skylight`, keyed on the same
  bilinear bathy depth the absorption already uses. **This directly contradicts
  the "SCATTERING IS NEVER SPLIT" reasoning**, and that reasoning is not wrong —
  the engine's integral gives us `MeanTransmittanceToLightSources` (the light
  path down to the bed, attenuated separately) and the PhaseG sun band, and a
  baked term has neither. The honest framing: we would be trading a physically
  better in-scatter that is drawn on a staircase for a physically cruder one that
  is smooth, and the owner judges which he would rather look at.
* **Expected visual difference.** "The map contour lines all over the lake should
  go away and the depth shading should be smooth."
* **Cost.** A water-only regen. Runtime: roughly neutral (one exp() replaces the
  engine's integral, which still runs).
* **Risk.** High-ish. It changes the deep-water colour's response to the sun and
  loses the grazing-sun brightening. Gate on a new `BakedScatterGain` scalar,
  default 0.0 = today.
* **A/B.** `BathyDepthAuthority` ladder first (free, no regen). Then, if built,
  `BakedScatterGain 0` vs `1`.

#### STEP 1 MEASURED 2026-09-07 (session: R2/R3/R5). ANSWER: **IT IS THE SCATTERING.**

R5's step 1 is a measurement and this is it, run exactly as specified and before
touching anything: `-VoxelWaterMatScalar=BathyDepthAuthority:1.0` against the
shipped 0.85, same settled lake pose and same material as R2's sweep above.

* baseline `VoxelVerify00874.png`, authority 1.0 `VoxelVerify00894.png`
* engagement proof: `Lake sheets: material scalar 'BathyDepthAuthority' set to
  1.0000 (diagnostic override).` in `Saved/capture-r5-auth100.log`
* controls: sky **0.00**, land **0.01** out of 255. This is the cleanest pair in
  the session — a sheets-only scalar moves nothing but the sheets.

The metric is a **vertical band-pass on luma** (mean `|I - 9-tap vertical box
blur|`), which is the right instrument for this artefact and not an impression:
the contour rings are thin, near-horizontal, dark lines sitting on a smooth
top-to-bottom depth gradient, so a vertical high-pass keeps the rings and
discards the gradient they ride on.

| band | contour energy, authority 0.85 | at 1.0 | change |
|---|---|---|---|
| near | 0.3264 | 0.3590 | **+0.033** |
| mid | 0.3749 | 0.4511 | **+0.076** |
| far | 12.02 | 9.95 | -2.08 (see below) |

**The lines SURVIVE at authority 1.0 — they do not vanish, and they do not
move. In the near and mid bands the contour energy goes slightly UP.** By this
section's own decision ladder that is the first branch: *"If the lines survive
at 1.0 -> it is the scattering, and the fix below applies."* The 15% absorption
residue is exonerated: taking the engine's absorption share to zero did not
remove the staircase, so the staircase is being drawn by the term that still
reads `WaterVolumeDepth`, which is the in-scatter. The third branch (the lines
MOVE, i.e. the wave field's `BreakSurfFloorM`) is also excluded — the rings are
in the same place in both frames.

The far band's -2.08 is **not** evidence about contour lines. That band is the
bright near-horizon strip where the metric is dominated by SSR streaks and wave
texture (its absolute value, ~12, is thirty times the mid band's); it moves
because raising the baked authority brightens the far water overall. Read the
near and mid rows, which is where the owner's "map contour lines" actually are.

**So the R5 fix as written is the indicated change**, with everything this
section already says about it standing: it contradicts `SCATTERING IS NEVER
SPLIT` at `create_water_voxel_material.py:995-999`, that reasoning is not wrong,
and it trades a physically better in-scatter drawn on a staircase for a cruder
one that is smooth. **NOT BUILT IN THIS SESSION** — the owner is being asked to
judge that trade on R2's and R3's frames first, and the build is gated behind
`BakedScatterGain` default 0.0 when it happens.

---

### R6 — Make the bed visible again: ladder the slant elongation cap
**Free measurement first; contradicts an owner approval — see section 4(b).**

* **Mechanism.** Observation 4a: `slant = depth / max(cos_eff, 0.05)` with
  `BathyRefractInvN2 = 1.0` is a 20x elongation cap
  (`bathy_field_graph.py:202-214`). At the owner's camera angle it is multiplying
  the approved vertical-depth clarity table by roughly 3x, and near the far shore
  by up to 20x.
* **The ladder, all free, all sheets-only, no regen:**
  `-VoxelWaterMatScalar=BathyRefractInvN2:1.0` (shipped) / `0.8` / `0.565`
  (Snell, the pre-2026-09-05 default). Four captures at one settled aerial pose.
  This is the cheapest experiment in the document and it directly answers "why
  can I not see the bottom".
* **Expected visual difference.** "In the shallows near the boat I should be able
  to see the sand and the rocks under the water, and the shallow water should
  read as shallow instead of as more blue paint."
* **The prerequisite is CLEARED — the bed is genuinely there to be seen.**
  The lake bed at 50-300 m is drawn by the ray marcher (observation 4d), and the
  marcher writes real `SceneDepth` before the base pass
  (`VoxelMarchRenderer.cpp:9663`, `:11447`, `:11461-11464`) and real
  `SceneColor` + GBuffer at the end of it (`:11854`, `:12364-12387`), both long
  before `RenderSingleLayerWater`. So `BehindWaterSceneDepth` is the real bed
  depth and `SceneColorWithoutSingleLayerWater` holds the real bed colour. The
  bed is invisible because of the optics, and only because of the optics. This
  ladder is the right lever.
* **Cost / risk.** The ladder is free. Reverting the secant would re-open the
  "water too transparent" complaint the secant was shipped to answer — which is
  why this is ranked below R1-R3 and why the recommendation is to move the CAP
  (0.05 -> 0.20, i.e. 20x -> 5x) rather than the curve, and to run it AFTER the
  reflection work, because a surface that reflects a sky hides its own bed for
  free and physically.
* **A/B.** The three-rung ladder above, one pose, byte-identical OFF arm = 1.0.

#### R6 MEASURED 2026-09-07 — four rungs, one pose. VERDICT PENDING OWNER.

**THE POSE HAD TO MOVE FIRST, and the reason is worth writing down because it
would have produced a confident null.** The lake harness at **+2 m / -25 deg is
SUBMERGED**: `capture-r6-invn2-100.log` reads *"Ocean: camera entered water
(camera z=164691.4 UU, worldgen ground z=164421.8 UU, submerged depth 3.32 m,
treatment=M_Underwater)"*. The lake surface at that column stands 5.32 m above
the ground the spawn measures from, so everything in the frame is drawn by the
underwater post-process and `BathyRefractInvN2` — a parameter on the
ABOVE-water sheet material — moves nothing in it. **This also settles
`VoxelVerify00840`: that frame is not debug paint, it is the underwater view**,
and the flat blue terraces in it are the lakebed. The ladder therefore ran at
**+6 m / -18 deg**, which is above the surface, is the shipping baseline, and is
the grazing framing the owner complained about — where the secant bites hardest.

**The metric is the BED'S OWN STRUCTURE showing through the water.** ADR-0008
fixes every voxel face at one flat colour, so what a visible bed contributes is
the 10 cm contour terrace, the per-voxel jitter and AO. The water's own grading
is a smooth wide-band exponential (observation 5) and contributes almost none of
it. `det31` below is the standard deviation of the luma high-pass at the terrace
scale (a 31 px box residual; the terraces are 50-100 px wide at this pose),
measured over three horizontal bands of the water, band 5 nearest.

    BathyRefractInvN2   det31 band3   band4   band5     G      B    Rshare  frame
      1.0  (shipped)      0.00190   0.00267  0.00276  .3190  .4405  .1890   00870
      0.8                 0.00335   0.00372  0.00374  .3283  .4520  .1845   00872
      0.565 (Snell)       0.00554   0.00555  0.00498  .3398  .4638  .1797   00876
      0.4                 0.00740   0.00686  0.00588  .3478  .4708  .1768   00878

    frames: ue-project/Saved/Screenshots/WindowsEditor/VoxelVerify0087{0,2,6,8}.png
    logs:   Saved/capture-r6b-invn2-{100,080,0565,040}.log
    arms:   -VoxelWaterMatScalar=BathyRefractInvN2:<v>, one pose (+6 m / -18 deg,
            yaw 45, settle 75 s, frozen noon), voxel.March.Caustics 0 on all four
            so the ladder moves ONE lever. Each log carries its own
            "Lake sheets: material scalar 'BathyRefractInvN2' set to N.NNNN"
            line -- the override is proved per rung, not assumed.
            LADDER CLEAN: one water material (regen 04:24:32) across all frames.

**Monotone in every band and every column.** Reverting the secant to Snell
(0.565) multiplies the visible bed structure by **2.9x** in the mid-water band
and 1.8x nearest; 0.4 takes it to 3.9x. **Red does not move** (.1770 -> .1758)
and that is the physics, not a null: at 1.118 per metre red is gone at every
path length on this ladder, so the bed can only come back in green and blue,
which is exactly what the G and B columns do (+9.0% and +6.9% at Snell).

**And it is visible, not just measurable.** At 1.0 the lower half of the frame
is featureless dark blue-grey; by 0.4 the bed's stair-stepped contours are
legible through it and the far shore grows a distinct pale-cyan shallow band
that the shipped arm does not have. That is the owner's own ask -- "the shallow
water should read as shallow instead of as more blue paint".

**What this does NOT settle**, and it is the trade R6 was ranked below R1-R3
for: reverting the secant re-opens the "water is too transparent" complaint the
secant shipped to answer on 2026-09-05. The ladder says what the lever buys; it
does not say the owner wants to spend it. The recommendation in this section --
move the CAP (0.05 -> 0.20) rather than the curve -- was **not** measured,
because the cap is a bare `b.const(0.05)` inside `bathy_field_graph.
build_slant_depth` and moving it needs a water-material regeneration, which is
another session's file. If the owner likes the 0.565 look but not at
steep angles, the cap is the next experiment and it is one line plus a regen.

---

### R7 — Caustics: move the term into the marcher, because that is what draws the floor
**A real code change, not a tuning pass. Only worth doing after R6 makes the bed
visible — caustics on an invisible bed are invisible caustics.**

* **Mechanism.** Phase F1's caustic light is wired into `M_VoxelTerrain` and
  `M_VoxelClipmap` emissive, and neither shades a near-field terrain pixel in the
  shipped config: `voxel.March = 1` (`VoxelMarchRenderer.cpp:66-67`),
  `voxel.Terrain.RetireQuads = 1` (`VoxelBrickPool.cpp:668-675`), clipmap inner
  hole 8192 m (`VoxelClipmapActor.cpp:791-813`). The marcher's emissive
  (`VoxelMarch.usf:3495`, `:3550-3552`) has no caustic input and no MPC read.
  Full evidence at observation 4d.
* **Expected visual difference.** "There should be dancing bright light on the
  lake bottom in the shallows, moving with the surface."
* **Implementation.** Reimplement the caustic field inside the marcher's emit —
  `CausticIntensity` and the bathy depth as uniforms on `FVoxelMarchEmitPS`,
  the field itself transcribed from `water_caustics_graph.CAUSTIC_CODE`
  (`water_caustics_graph.py:200`). **Do not copy it.** This project has a four-times-repeated rule that two copies of one
  derivation drift silently (`water_sky_reflection_graph.py:22-31`), and a
  caustic pattern that differs between the marcher's floor and
  `M_Underwater`'s view of the same floor is exactly that defect in the one frame
  guaranteed to show both (surfacing at a shoreline). The honest shape is a
  single `.ush` holding the HLSL, `#include`d by the marcher and emitted verbatim
  into the Custom node by the Python module — the same "one derivation, two
  consumers" split the wave field and the optics already have.
* **Also fix the fade window while you are there.** `CausticFadeStartM 40` /
  `CausticFadeEndM 64` (`water_caustics_graph.py:180-181`, `:405-422`) makes the
  term exactly zero past 64 m of camera distance, which is most of any lake shot.
  Both are parameters by design (`:177-179`), so this half is an instance
  override, not a regeneration.
* **Cost.** A shader change to the marcher's emit — the hottest pass in the
  frame. The plan already names the caustic cost as a risk (`:359-360`) and **no
  measurement exists for it anywhere**. Measure before and after.
* **Risk.** Medium-high: it touches the marcher. The whole term must be behind
  the existing `voxel.Water.Caustics` global (default 0.5,
  `VoxelWaterSubsystem.cpp:256-258`), whose 0 arm is already documented as
  pixel-identical (`water_caustics_graph.py:50-59`).
* **A/B.** `voxel.Water.Caustics 0` vs `0.5`, one pose, shallow water in frame.
  Once the term is in the marcher this is a live cvar with no regen.
* **Bookkeeping.** `create_voxel_material.py:69-70`'s premise ("The lake and sea
  FLOORS are this material") and the F1 rows in
  `docs/water-ocean-tides-plan-2026-09-04.md` are wrong as written and should be
  corrected at the same time. Note also that the 2026-09-05 owner verdict on
  F1 — "00734 looks good" (plan `:898`) — was a verdict on a frame in which the
  caustic term contributed **nothing**, so it is not evidence that caustics work.

#### R7 IMPLEMENTED 2026-09-07 — the marcher half. VERDICT PENDING OWNER.

**What changed.** Five files; the field itself is written once and shared.

* **`ue-project/Shaders/VoxelCaustics.ush` (new).** The caustic derivation as
  HLSL: `VoxelCausticField()` (three panned, mutually-warped interference
  layers) and `VoxelCausticLight()` (the sun-altitude gate, the submersion
  gate, the distance fade, and the sun-ray absorption). The field's body sits
  between two marker lines and is **byte-identical** to
  `water_caustics_graph.CAUSTIC_CODE`.
* **`ue-project/Shaders/VoxelMarch.usf`.** `#include`s it, declares the seven
  new loose parameters beside `MarchVis` under `#if VOXEL_MARCH_CAUSTICS`, and
  adds the term to `Emissive` in `VoxelMarchEmitPS` immediately before
  `OutSceneColor` is written. Additive on emissive, not folded into BaseColor —
  it is a light arriving at the floor, and BaseColor would multiply it by AO
  and the diffuse response.
* **`VoxelMarchRenderer.cpp/.h`.** Seven parameters on
  `FVoxelMarchEmitParameters`; `MakeMarchCaustics()` derives them; six cvars
  (below); and `VoxelMarchPublishBathyField()`, the game→render wire.
* **`VoxelBathyField.cpp`.** Calls that wire from `PublishWindow` (same tick as
  the pixel upload, carrying the origin in **double** rather than through the
  float the MPC can hold) and from `PublishInvalid`.

**The knobs.** `voxel.Water.Caustics` (unchanged, default 0.5) still gates
everything; `voxel.March.Caustics` (new, default 1.0) is the marcher's own half
of the switch, and 0 on either makes the emit byte-identical to the pre-R7 pass
— the shader skips the whole block. The fade window is `voxel.March.
CausticFadeStartM` 150 / `CausticFadeEndM` 400, **not** the material's 40/64
(R7's "fix the fade window"): the bound that matters is the bathy window's
±480 m, outside which there is no depth to read, and the fade end is clamped
host-side to 0.9 of the window half-extent so no cvar setting can draw a hard
square edge around the camera. `voxel.March.CausticScaleM` / `Speed` /
`Sharpness` mirror `water_caustics_graph.DEFAULTS`.

**Engagement is logged, because armed-and-inert is the failure this would
otherwise present as.** Five named declines and one engagement line, each said
once:

    Caustics (R7): ENGAGED. intensity 0.500 (march 1.00 x water 0.50),
    sin(sunAlt) 0.617, fade 150-400 m, field scale 2.60 m speed 0.55
    sharpness 6.0, absorb/m (1.118 0.458 0.291), window origin rel camera
    (-49324 -48600) UU size 96000 UU.

`absorb/m` is **derived** in C++ from `ABSORPTION_DISTANCE_M` and
`ABSORPTION_COLOR` exactly as `water_optics.absorption_per_m` derives it, not
pasted — the numbers above are the arithmetic, checkable by hand.

**THE HAND-OFF THAT IS NOT DONE, stated rather than discovered later.**
`water_caustics_graph.py` still carries its own copy of the field as
`CAUSTIC_CODE`. It is byte-identical to the `.ush` block today and there is
nothing to see in a frame, but that is exactly the drift this project has a
four-times-repeated rule about. The `.ush` header names the one-function hook
that closes it (`CAUSTIC_CODE = _read_field_body("VoxelCaustics.ush")`, slicing
between the marker lines, raising if either marker is missing). It was left
undone deliberately: the water material generators were owned by another
session on the day this landed, and the marcher half neither needs it nor may
wait for it. **Whoever owns those generators should take it.** Two smaller items
travel with it: the material's `CausticFadeStartM`/`EndM` should be overridden
to match the marcher's window (an instance override, not a regeneration), and
`create_voxel_material.py:69-70`'s premise plus the F1 rows in
`docs/water-ocean-tides-plan-2026-09-04.md` are still wrong as written.

**KNOWN BOUND, inherited not invented.** A bathy texel is a COLUMN: it says how
deep the water at this XY is, not whether *this* point is under it. A boulder
standing proud in a lake gets caustics on its top. That is the same
approximation `create_voxel_material.py` makes with the same channel; closing it
needs a water-surface Z the marcher does not have.

#### R7 MEASURED 2026-09-07 — the term is LIVE and it is SUB-VISIBLE at this lake.

**Cost first, because R7 asked for it and no figure existed anywhere.** The emit
pass's own GPU time, from `voxel.March.Stats` fired at t+70 s by
`-VoxelExecAfter` (the only way to get it out of a headless run — the census is
an on-demand console command and `-ExecCmds` lands at t=0 when `emitFrames` is
still 0):

    arm                                        emitMs   marchMs
    caustics 0, submerged  (00868)              0.214    1.871
    caustics 0, +6 m/-18   (00870)              0.228    4.021
    caustics 0, +6 m/-18   (00876, invN2 .565)  0.223    3.848
    caustics 1, submerged  (00880)              0.269    1.864
    caustics 1, +6 m/-18   (00882, invN2 .565)  0.273    3.852

**+0.05 ms, i.e. +22% of the emit pass and about 0.5% of the frame.** That is
the whole cost of the term and it is now on the record.

**The A/B, and it is an honest disappointment.** `voxel.March.Caustics` 0 vs 1
at the submerged pose — the frame where the bed is fully visible because there
is no water column between the camera and it, and no wave surface to add
run-to-run noise:

    00868 (off) vs 00880 (on):  mean delta RGB +0.00038 +0.00043 +0.00046
                                3.91% of pixels move >1/255, 0.02% >4/255,
                                NONE >12/255.

**Below the visible threshold.** The above-water pair (00876 vs 00882) cannot
even be read: the wave surface's phase differs between two launches and swamps
the term at +/-0.40 luma.

**So an ENGAGEMENT PROOF was shot instead, at 8x gain** (`voxel.March.Caustics
8`, i.e. intensity 4.0), `VoxelVerify00886.png`, delta image at
`Saved/r7-caustic-delta-x8.png`:

    mean delta RGB   +0.00003  +0.00041  +0.00141     <- R:G:B ~ 1:14:47
    p99 blue delta   +0.0196 (5/255) ; max luma +0.043
    structure ratio (stddev/mean) in the near band: 3.71

Three things follow, and together they are conclusive. The delta is **strongly
blue-weighted in exactly the ratio the sun-ray absorption predicts** (red 1.118,
green 0.458, blue 0.291 per metre). It is **structured, not a flat lift** — a
stddev 3.7x its own mean is a filament pattern, which is what a caustic field
is and what a uniform brightening is not. And it **scales with the gain**. The
term is wired, bound, engaged (its own log line says so on every run) and
drawing the right thing.

**WHY IT IS INVISIBLE, and this is the finding, not the bug.** The chain
`water_caustics_graph` specifies attenuates the caustic down the SUN ray by
`exp(-absorb * depth / sunZ)`. This lake is **5.3 m deep at the harness column**;
at noon (sin alt 0.617) that is an 8.1 m sun path, and the survival fractions
are 0.0001 / 0.024 / 0.094 RGB. **The term is being divided by roughly 500
before it reaches the floor.** It is designed for shallows — at 0.5 m the same
factors are 0.40 / 0.69 / 0.79 — and the harness lake is not shallow. So the
caustic light is now demonstrably ON the floor the player stands beside, which
is what R7 set out to fix and what observation 4d proved had never happened; it
is not yet *seen*, because at 5 m of this water nothing is.

**The three levers, all the owner's call, none of them a code fix:**

1. **A shallower pose.** The one thing that would settle it in one frame is a
   shoreline camera over 0.3-1 m of water. **One attempt was made and missed:**
   `-SpawnAt '-64961,-50943'` (200 m along yaw 45 toward the shore) found ground
   2.9 m higher — so the basin does shallow that way — but still put the camera
   under the surface (`VoxelVerify00892.png`, a clean underwater frame with the
   bed and the surface both in shot, shot at 8x gain). Finding a genuine 0.3-1 m
   waterline column needs the bathy field consulted rather than a guess, and it
   is the obvious next capture.
2. **Intensity.** `voxel.Water.Caustics` is a live cvar, no rebuild, no regen.
   At 5 m it would take roughly 8-16x to read.
3. **The absorption itself.** Real caustics are clearly visible on a 5 m bottom
   in clear water; ours is calibrated to `ABSORPTION_DISTANCE_M = 3.5` — the
   "somewhere in between" midpoint the owner approved 2026-08-12 as a
   *vertical-depth* table. Whether the caustic term should carry that full
   coefficient down the sun ray, or a fraction of it, is a look decision and
   `water_optics.py` is its single authority. **Do not fix it by typing a second
   coefficient in the marcher.**

**Also worth saying plainly, because it is what the owner asked about:** at the
shipped `voxel.Water.Caustics 0.5` nothing in the picture changes that a person
would notice. The pre-R7 answer was "the light lands on a material that draws
nothing"; the post-R7 answer is "the light lands on the floor and the water eats
it". Those are different problems with different fixes, and only the second one
is now a tuning question.

---

### R8 — The waterline: finish the pending slack ladder, and confirm wet shores exist
**No new work — two items are already measured and waiting on the owner.**

* `BathyShoreClipSlackM` ladder is measured and **pending owner judgment**
  (plan `:860`): dark px at `+0.9` (default) = 225, `0.0` = 73, `-0.5` = 22,
  `-2.0` = 0, monotonic, with wet-shore darkening exonerated. The tradeoff the
  owner has to rule on is band-vs-dry-rim.
* `VOXEL_SHORE_FX` shoreline foam + wet-sand darkening have **never been
  confirmed in a capture** (`create_water_voxel_material.py:657-661`). One
  regen-pair at a beach pose settles whether they are visible at all. Note the
  OFF arm here is a generation-time arm, so it is two regens, not a cvar.
* Observation 7's stair-stepping itself is the 1.875 m bake raster meeting a hard
  `step()`. Softening the clip is explicitly refused at `:2255-2258` ("a smooth
  clip against a 0.5 mask threshold moves the waterline instead of placing it"),
  and that reasoning stands. The honest fix for a jagged waterline is foam over
  it, which is the item the owner is debugging.

---

### R9 — The parked `SurfacePresence` ladder, at 0.15-0.3
**LAST, deliberately. No regen. Contradicts an owner verdict — see section 4(a).**

* **Mechanism.** The grazing-only sheen already in the graph:
  `sky_reflected * Fresnel(base 0.0, exponent 8) * SurfacePresence`
  (`water_sky_reflection_graph.py:1011-1049`), 0.0039 at NoV 0.5 and 0.43 at
  NoV 0.1. It is the only term in the material whose magnitude rises with grazing
  angle, and it ships at gain 0.0.
* **Expected visual difference.** "The far water should go silver toward the
  horizon" — which is literally what this term does and nothing else in the
  material does.
* **Implementation.** None. It is a material scalar with a default of 0.0; the
  ladder is `-VoxelWaterMatScalar=SurfacePresence:0.15` / `:0.3`, sheets-only
  (R3's caveat applies). Shipping a non-zero default is a one-line change to
  `water_sky_reflection_graph.DEFAULTS` (`:266`) plus a water-only regen.
* **Cost.** Zero — the nodes are already in the shipped shader.
* **Risk. HIGH, and it is a verdict risk rather than a technical one.** The owner
  rejected this exact term at 1.0 on 2026-09-05 because it washed the body colour
  toward sky. Section 4(a) is the argument for why 0.15-0.3 on top of the
  now-shipped slant path is a different proposition; that argument may still lose.
* **WHY IT IS RANKED LAST AND NOT FIRST, despite being free.** If R1 or R2 give
  the engine a real reflection, this term double-counts it — added on Emissive,
  outside the engine's `EnvBrdf` energy split, which is precisely the mechanism
  that retired `LegacySkyReflectGain` in August
  (`water_sky_reflection_graph.py:955-964`, measured at `:930-945`). Running this
  ladder before the reflection question is settled would produce a frame that
  looks better for the wrong reason and would have to be re-run afterwards.
* **A/B.** `SurfacePresence` 0.0 / 0.15 / 0.3 at one grazing pose, **moved
  alone**, against the shipped `BathyRefractInvN2 = 1.0` — the module's own
  instruction at `:210-211`: "move ONLY this, against the slant default, or the
  frames measure a mixture."

---

## 6. WHAT I WOULD DO FIRST

Two experiments and one build, sized for a single owner judging session. Neither
of the first two needs a regen, a rebuild, or the editor — they are cvars on the
command line.

**First: shoot the reflection pair. One pose, four frames, no rebuild.**

    A  (shipped)     -ExecCmds="r.Water.SingleLayer.Reflection 2"
    B                -ExecCmds="r.Water.SingleLayer.Reflection 1"
    C                -ExecCmds="r.Water.SingleLayer.Reflection 2 | voxel.Sky.SkyLightAtGroundZ 1 | voxel.Sky.FogInSkyCapture 0"
    D                -ExecCmds="r.Water.SingleLayer.Reflection 1 | voxel.Sky.SkyLightAtGroundZ 1 | voxel.Sky.FogInSkyCapture 0"

Same settled pose as the screenshot (low aerial, water filling the frame,
shoreline in shot), `-TimeScale 0`, exposure pinned. **This is the single
highest-information hour available.** It tests, from one binary and one build,
whether observations 1, 2 and 3 are one root cause: A is today; B says whether
SSR gives the lake the beach, the dunes and the canoe; C says whether the
existing skylight is merely in the wrong place; D is both. If B or D looks like
water and A does not, the diagnosis in section 0 is confirmed and everything else
on the list becomes secondary tuning.

**Read B and D against R1's named risk.** The HZB is built before the marcher's
depth lands (`VoxelMarchRenderer.h:169-175`), so SSR's acceleration structure has
no terrain in it. If B/D come back with the sky reflected but the shoreline
streaking or missing, that is the expected signature and the finding is "SSR on
water needs the HZB ordering fixed first" — which is a real answer worth an hour,
not a failed experiment.

**Second, in the same session because it costs one more launch: the free ladders.**

    -VoxelWaterMatScalar=BathyRefractInvN2:0.8      (vs shipped 1.0)
    -VoxelWaterMatScalar=BathyDepthAuthority:1.0    (vs shipped 0.85)

The first answers "why can I not see the bottom" (R6). The second answers "what
draws the contour lines" (R5 step 1) and is a pure diagnostic — it is not a
proposal to ship 1.0. Both are sheets-only; note it when handing the frames over.

**Third, the one build worth queuing before the frames come back: R3, the
distance-driven roughness**, default `WaterRoughnessFarGain = 0.0` so the shipped
asset is unchanged and the arm is one `-VoxelWaterMatScalar` away. It is the only
item on the list that improves the far water WITHOUT depending on the reflection
question going one way or the other: at high roughness the engine keeps more of
the water's own colour (which is what this owner has twice said reads as water)
AND the wave facets start to sparkle. If the reflection pair comes back "SSR is
too expensive / too smeary", R3 is the fallback that still moves the picture.

**And one thing that needs no experiment at all.** The caustics question is
already answered: the term is on a material that does not draw the lake floor
(observation 4d). That is worth telling the owner in the same session, because it
converts "caustics look weak" into "caustics have never been on screen", and it
means the F1 verdict of 2026-09-05 ("00734 looks good", plan `:898`) was passed on
a frame with no caustics in it. R7 is the fix and it is a marcher change, not a
knob.

**What I would NOT do first.** Re-arming `SurfacePresence`. It is the obvious
move and it is the one the owner has already rejected once, and if R1/R2 land it
will double-count the engine's reflection exactly as `LegacySkyReflectGain` did
in August. It belongs after the reflection question is settled, at 0.15-0.3, on
its own ladder, moved alone — R9, ranked last on purpose despite being the
cheapest thing on the list.

---

## 7. OPEN QUESTIONS THIS DOCUMENT COULD NOT CLOSE

1. **CLOSED during this analysis, recorded here because it was the biggest
   unknown:** the lake bed at 50-300 m is drawn by the ray marcher, and marched
   terrain IS present in scene depth and scene colour before the SLW pass
   (observation 4f). The bed is reachable; the optics are hiding it.
   *Static evidence only.* `voxel.March` and `voxel.Terrain.RetireQuads` were
   read from their declarations and confirmed to have no override in any `.ini`,
   `.ps1` or `.py` in the tree — but the `unreal-editor` MCP server refused
   connection this session, so nothing was read back from a live editor. Typing
   `voxel.March` and `voxel.Terrain.RetireQuads` with no argument into the
   console closes that last mile in two seconds.
2. **What the engine's reflection actually reads at NoV ~0.1.** The 2026-08-12
   table (`water_sky_reflection_graph.py:930-945`) is the only measurement and it
   was taken at pitch -30, NoV ~0.5. The complaint band has never been measured.
3. **What the wind was doing** in the screenshot. `WindAmpExponent = 1.0` and no
   floor means a calm publish takes the whole wave field — and with it every
   slope-driven glitter and foam term — linearly to zero
   (`create_water_voxel_material.py:1109-1154`). `voxel.Weather.PinMps 5` in the
   console at the same pose separates "the water is matte" from "there was no
   wind".
4. **Whether `r.Water.SingleLayer.Reflection 1` costs anything here.** No SSR-on-
   water figure exists. The config's own note at `DefaultEngine.ini:540-541` is
   explicit that no SSR cost figure exists for the opaque case either.
5. **The reference pin is stale.** `VoxelVerify00534` (2026-08-12) is what
   `tools/voxel-sky-chain-regen.ps1` diffs every regen against, and the
   2026-09-05 chain run measured 99.42% diffuse difference and did not re-pin
   (plan `:838`). Until the owner re-signs a frame, every full-chain regen fails
   its most valuable check by design — which matters for R3 and R5, both of which
   need a regen.

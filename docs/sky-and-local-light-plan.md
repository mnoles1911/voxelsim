# Sky radiance to the ground, and an additive path to the pixel

Written 2026-07-29, after the day/night cycle and night sky shipped (PR #187).
Design converged with a deep architectural pass; every engine claim below was
verified against `D:\UE_5.8` source rather than assumed, and the `file:line`
citations are worth re-checking before leaning on one.

Two problems, and the headline is that they are **not** the same problem.

---

## 0. Read this first

| Document | Why |
|---|---|
| `docs/lighting-weather-plan.md` §2.2, §2.3, §5.4 | The composition contract. §2.3 is the load-bearing one |
| `docs/measurements/sky-daynight-2026-07-29.txt` | Every number the gates below are stated against |
| `ue-project/Shaders/VoxelQuadVertexFactory.ush` :445-600 | The crux file for both problems |
| `docs/backlog.md` §0 | Render-thread bound, game thread idle 75% |

**Not in scope, because it already ships.** Sun and moon are `ADirectionalLight`s
driven from `VoxelEphemeris`, the moon is atmosphere light 1, both cast CSM
shadows, and a moving sun measured free at `voxel.Sky.ShadowUpdateHz=10`
(p50 15.18 both arms; p95 25.31 vs 25.32). Nothing here changes the rig, the
ephemeris, the shadow cadence, or the fitted exposure curve.

---

## 1. P2 — the sky's own art lighting the world

### 1.1 Verified: the star dome contributes exactly zero ambient today

`FScene::AllocateAndCaptureFrameSkyEnvMap`
(`Renderer/Private/ReflectionEnvironmentRealTimeCapture.cpp:343`) renders
**four** things into the SkyLight's cubemap: the SkyAtmosphere raymarch,
volumetric clouds (`:494-547`), height fog (`:921`), and meshes whose material
has **`bIsSky`** set. Nothing else — the translucency pass is never run for the
capture view.

A mesh reaches `View.SkyMeshBatches` only via `ViewRelevance.bUsesSkyMaterial`
(`SceneVisibility.cpp:1970-1977`), which comes from the material's `IsSky()`
(`SkyPassRendering.cpp:29`). `M_NightSky` sets `is_sky = False` **deliberately**
(`Tools/create_sky_material.py:983`, rationale at `:39-60`). So
`SkyMeshBatches` is empty and the capture takes the `else` branch at
`ReflectionEnvironmentRealTimeCapture.cpp:743-746`, whose engine comment says it
outright: *"If there are any mesh tagged as IsSky then we render them only,
otherwise we simply render the sky atmosphere itself."*

Consistent with measurement: the `no dome` and `gain 0.15` ladder columns agree
bit-for-bit on ground-dominant day rungs — the dome only ever touched sky pixels.

### 1.2 The trap that kills the cheap fix

The obvious shortcut is a second, capture-only `IsSky` dome
(`bRenderInMainPass=false`, `bVisibleInRealTimeSkyCaptures=true`). **It breaks
the visible sky.** The moment *any* primitive uses a sky material,
`View.bSceneHasSkyMaterial` goes true (`SceneVisibility.cpp:2096`) and the main
view's atmosphere pass stops painting sky pixels
(`SkyAtmosphereRendering.cpp:2214`: `bRenderSkyPixel = !View.bSceneHasSkyMaterial`).
The engine ships an editor warning for exactly this configuration
(`ReflectionEnvironmentRealTimeCapture.cpp:313-316`).

⇒ **Once one IsSky mesh exists, an IsSky mesh must paint the main-view sky.**

### 1.3 The design: a canonical IsSky dome the capture can see

One new material `M_SkyAtmosphereDome`, authored by a new
`Tools/create_sky_atmosphere_dome_material.py` sharing the star-UV graph and MPC
conventions with `create_sky_material.py`:

```
Unlit, Opaque, bIsSky = TRUE, TwoSided
EmissiveColor = SkyAtmosphereViewLuminance                    // the atmosphere's own sky
              + SkyAtmosphereLightDiskLuminance(light 0)      // the sun disc
              + ReflectionCapturePassSwitch(
                    Default    = 0,                           // main view: atmosphere only
                    Reflection = StarMap(StarRotation) * StarBrightness * StarAmbientGain)
```

Three engine expressions do the work, all authorable headlessly:
`UMaterialExpressionSkyAtmosphereViewLuminance` (the capture wires its LUT
deliberately — `ReflectionEnvironmentRealTimeCapture.cpp:590-597`),
`UMaterialExpressionSkyAtmosphereLightDiskLuminance`, and
`UMaterialExpressionReflectionCapturePassSwitch`, which compiles to
`View.RenderingReflectionCaptureMask > 0` (`Common.ush:2306-2309`) and is set to
1 by the sky capture (`:570`). **That switch is the load-bearing node** — it is
what keeps stars out of the main view while feeding them to the capture.

It renders on a **second sphere component** on `AVoxelSkyDomeActor` — same mesh,
same camera-follow (`VoxelSkyDomeActor.cpp:245`), same 2e7 UU radius. In the main
view it draws in `EMeshPass::SkyPass` with depth writes masked, so sky depth stays
at the far plane and height fog, aerial perspective and TSR behave as today.

**The existing additive dome is untouched.** It keeps owning the *visible* stars,
their measured 0.15 gain, the sunrise fade, the textured phased moon, and
depth-tested occlusion by terrain.

What the capture then integrates, on the time-slicing it already does: the
atmosphere **plus the star map at the correct sidereal rotation**, because the
dome reads the same `MPC_VoxelSky.StarRotation` the subsystem writes every frame.
The Milky Way becomes a real anisotropy in the SkyLight's irradiance SH, so a band
overhead brightens up-facing terrain more than walls — **at every distance and on
all three geometry paths** (pooled, component, clipmap), because SkyLight ambient
is deferred. No 38 m ring is possible by construction.

### 1.4 Three routes explicitly rejected

- **`SLS_SpecifiedCubemap`.** Mutually exclusive with real-time capture —
  `ReflectionEnvironmentCapture.cpp:2899-2901` asserts a real-time-capture sky
  light is never scheduled for a cubemap update. Switching deletes
  atmosphere-follows-sun ambient, which the rig's own comment calls *"HOW NIGHT
  GETS DARK HERE"* (`VoxelSkySubsystem.cpp:1614-1620`).
- **A capture-only IsSky dome without converting main-view painting.** §1.2.
- **The encoding split as the starlight carrier.** See below — this is the
  finding that removes scope.

### 1.5 The encoding split does NOT earn its place here

The plan once over-claimed the split as necessary for a moving sun (it was not),
then demoted it. The natural guess was that P2 is where it finally pays. **The
arithmetic says no.** The split multiplies per-direction radiance into
per-direction visibility, but it stops at 35.2 m (the slid fade band,
`VoxelGI.cpp:970-986`), cannot reach the clipmap at 92.7 km, and its unoccluded
case must still normalise to 1.0 (§2.3) — so it can *tint*, never *add*.

Its residual unique value is second-order: a cave mouth *facing* the Milky Way
reading brighter than one facing away, since SkyLight SH gives
direction-of-normal, not direction-of-opening. **Stays demoted, possibly forever.**

---

## 2. P1 — an additive path to the pixel

### 2.1 The routes

1. **UE point/spot lights (hero).** Work today, zero code. Defect: contribution ∝
   BaseColor, and in a cave `G ≈ AO × lerp(0.06, 1, ~0) ≈ 0.06`, so a torch lights
   a surface at 6% of its albedo. **Keep for direct light; fix the 6%.**
2. **Material emissive input.** Necessary, not sufficient —
   `terrain_material_common.py:344-378` connects `MP_EMISSIVE_COLOR` only under
   `VOXEL_MATERIAL_DEBUG`. An emissive input needs a per-pixel spatial signal,
   which only the factory can supply.
3. **An additive injection volume sampled in the factory.** The pick.
4. **Emissive-material geometry.** Glows, illuminates nothing without Lumen GI.
   Fine for a lava surface later; not an illumination path.

### 2.2 The pick: `VolumeLocal`

A new RGBA8 3D texture in `FVoxelGIVolume` covering **the same box** as
`VolumePos/Neg` — same `OriginPoolUU`, same normalised UVW. That identity is the
trick: the factory reuses `Interpolants.GIUVW` with **zero new interpolants**, and
the texel dimension is free to differ. Start `voxel.GI.LocalVolumeDim=96` (80 UU
texels, **3.4 MB VRAM + 3.4 MB CPU mirror**); escalate to 192 only if the
wall-leak gate fails.

**Channel contract — this is the double-counting firewall:**
- **A** = un-crush scalar, max over *all* local lights: "how lit is this cell by
  local sources, 0..1".
- **RGB** = additive radiance from **tail lights only**. Hero lights are already
  deferred lights; giving them RGB too would count them twice.

Factory change, inside the existing GI block so it costs nothing outside the
volume and nothing when disabled:

```hlsl
if (VoxelGIVol.LocalEnabled != 0u) {
    const float4 L = Texture3DSampleLevel(VoxelGIVol.VolumeLocal,
                                          VoxelGIVol.VolumeSampler, Interpolants.GIUVW, 0);
    Ambient  = max(Ambient, L.a);                    // un-crush, for deferred hero lights
    LocalRGB = L.rgb * Interpolants.Color.g;         // tail radiance x RAW AO
}
VertexColorWithGI.g *= (half)lerp(1.0f, Ambient, Weight);   // line :579 unchanged
```

`LocalRGB` multiplies the **raw AO**, not the folded `G` — torch light must not be
attenuated by *sky* visibility, and 0.06 in a sealed cave is exactly where it
matters. The un-crush stays a relative modulation ≤ 1 with no time-of-day input,
so §2.3 holds; outside the volume `Ambient` is already 1 and nothing changes.

Then `Result.TexCoords[1]/[2]` carry `LocalRGB` (computed at pixel rate, so no
VS→PS interpolant is added), and the regenerated material graph reads
`TexCoord(1)/(2)` into `EmissiveColor`. `BaseColor`, roughness and mask are
untouched. On the component path `TexCoord1` arrives as measured zero
(`Tools/probe_texcoord1.py`), so emissive is a silent no-op there — and the
shipped config is fully pooled anyway.

**Splat on the game thread** — the thread idle 75% of the frame. Inverse-square
falloff × a short line-march through the light field's level-0 `Opacity`, as a new
method *inside* `FVoxelLightField` (`SampleOpacity` is private and should stay
so). A 10 m light is ~12³ texels ≈ 7 KB of upload. Budgeted like solves, uploaded
through the same `UpdateTexture3D` idiom, and **restaged by the same
`RestageVolumeZRange` rows** so it can never be addressed by a different origin
than its siblings.

---

## 3. Why these must NOT share a mechanism

**Sky radiance is a boundary condition; local lights are sources.**

Starlight is hemispheric, applies to every pixel out to 92.7 km, and already has
an engine-wide delivery channel — the SkyLight — that the existing visibility term
correctly shapes. Local light is compact, lives inside the 35 m volume where the
player is, and needs occlusion at voxel resolution.

Forcing one mechanism gives either starlight that stops at 38 m (a visible ring)
or a world-sized injection volume (absurd). The shared *insight* — the volume
stores visibility, radiance rides on top — resolves into two carriers: the
SkyLight carries sky radiance globally, `VolumeLocal` carries source radiance
locally. Both leave `Vis` and the composition contract untouched, so **P1 and P2
are independent and can land in either order.**

---

## 4. Phases and gates

| P | Work | Gate | Risk |
|---|---|---|---|
| **S0** | Probes: `r.DynamicGlobalIlluminationMethod` readout; confirm `SkyMeshBatches` empty; re-run `probe_texcoord1.py` | Log lines exist; probe frame black | known |
| **S1** | `M_SkyAtmosphereDome` (atmosphere + sun disc, **star branch gained to 0**), second sphere, `voxel.Sky.AtmosphereDome` toggling visibility **live** | **Hard:** ladder with alternating dome-on/off rungs in ONE process, \|Δ luma\| < 2/255 at every rung. Twilight rungs specifically — they found three defects last time | **regression** risk, not research: it swaps who paints the sky |
| **S2** | Enable the capture-branch stars, `voxel.Sky.StarAmbientGain` | **Hard:** winter 18h/21h ground luma rises from 0.02 into an *ordered band* — above 0.5, strictly below the moonlit 17.4–25.5, which stay below midsummer 44.85. Day rungs bit-unchanged. Band-at-zenith vs band-at-horizon must differ on up-facing ground | minor research |
| **S3** | Flight leg, dome arm vs baseline | Recording: p50/p95 within noise of 15.18/25.31 | known |
| **L1** | `VolumeLocal` A channel: allocate, splat, un-crush, torch console command | **Hard:** cave torch-on vs torch-off in one process, near-wall luma ratio ≥ 4×. `voxel.GI.VolumeCheck` still passes, extended with a `VolumeLocal` arm. Checkerboard rung | splat occlusion quality is **the** research risk |
| **L2** | RGB channel + material regeneration + TexCoords route | **Hard:** emitter behind 40 cm of rock — far side unchanged (this decides 96³ vs 192³). Noon open terrain with feature on and zero lights: bit-unchanged | known machinery, new graph |
| **L3** | Emissive-voxel harvest | **Blocked** on voxel-core emitting an emissive material id. Do not schedule | — |

---

## 5. Silent failure modes

This renderer fails silently; each of these produces a plausible image, not an
error.

1. **`bSceneHasSkyMaterial` flips and the dome material is wrong → the sky goes
   black and reads as "night".** Highest consequence, and why the star branch
   ships gained to zero first. Caught by S1's live A/B ladder.
2. **MPC typo in the new dome material → `Compile` emits a constant, stars frozen
   at the default rotation in the capture, no diagnostic.** Caught by the
   generator's name-membership check against the read-back collection.
3. **Stars leak into the main-view branch → doubled stars.** Caught by S1's
   bit-unchanged day rungs.
4. **`ReflectionCapturePassSwitch` fires for ordinary reflection captures too.**
   True — it reads `RenderingReflectionCaptureMask`, not a sky-specific flag. This
   project ships no reflection capture actors; note it in the script header for
   whoever adds the first one.
5. **New uniform members omitted from `FVoxelGIVolumeSettings::operator==` → the
   diff says "unchanged" and `LocalEnabled` latches stale forever.** Extend the
   operator; the checkerboard rung proves reachability.
6. **A loose `Texture3D VolumeLocal;` global in the `.ush` → compiles, reads zeros
   forever.** The trap `VoxelQuadVertexFactory.ush:21-26` documents. Checkerboard
   rung again.
7. **`VolumeLocal` staged against the staging origin while the shader holds the
   committed one → torch light displaced up to 2560 UU during a re-centre.**
   Prevented by restaging in the same rows; caught by the `VolumeCheck` extension.
8. **Bilinear smear through a thin wall → glow on the far side of 40 cm of rock.**
   L2's wall-leak gate. Mitigation ladder: hard zeros into solid cells → 192³ →
   premultiplied-validity divide like Scheme A.
9. **Double-counting a hero light → over-bright halo that reads as "torch too
   strong" and gets mistuned.** Prevented by the channel contract; caught by an
   L1 arm with the deferred light disabled — the wall must go *dark*.
10. **`StarAmbientGain` × the +13.6 EV deep-night lift washes night to grey, and
    every "brighter" gate passes.** Why S2's gate is two-sided.
11. **The moon disc entering the capture and double-counting against directional
    light 1.** Excluded by construction: the textured moon stays on the
    non-captured `M_NightSky`, and the atmosphere's own disc is already black.

---

## 6. Not building

- The encoding split as the starlight carrier (§1.5).
- `SLS_SpecifiedCubemap` or any hand-rolled SkyLight source (§1.4).
- The analytic route (offline SH of the EXR + MPC vectors + per-material ambient
  terms): three delivery points, its own exposure coupling, and every failure mode
  silent. This is S1's fallback, not a starting point.
- Deferred point lights for the long tail: unshadowed leaks through voxel walls,
  per-light shadow maps unaffordable.
- A time-varying `AmbientFloor`, or any time-of-day term multiplied into
  `VertexColor.g`. Already adjudicated (§2.3, §5.4).
- Per-chunk MIDs for anything here.
- Any change to the sun/moon rig, ephemeris, shadow cadence, or exposure curve.

## 7. Uncertainties and their probes

Each is a one-process capture — hours, not days.

1. **GI method** — Lumen vs SH ambient. One console readout (S0). The design
   survives both; gate magnitudes differ.
2. **Does the LUT-sampled dome match the raymarched full-screen sky to <2/255 at
   twilight?** That is S1's gate, run *before* anything is built on it.
3. **SH-level Milky Way directionality magnitude** — S2's two-date capture.
4. **Splat occlusion quality at 80 UU** — L1/L2's wall-leak arm.

# Making the lakes look like water

**Status:** plan, not yet built. Written 2026-08-11 from four research passes plus a
measurement pass on the current build. The owner's words that started it: *"the
lake surface static texture looks like a repeating tile"*, *"the near water ...
looks even worse for being uneven and lack of depth and 3d shape"*, *"our water
volume seems very simplistic and not realistic"*, and a target of **realistic but
slightly stylised, for a voxel world**.

---

## The one-sentence answer

**We do not need a new water shader.** The surface material already has the right
bones — thickness from scene depth, Fresnel sky reflection, sun and moon glints,
per-pixel translucent lighting, panning ripples. What is missing is one physical
term, one geometry decision, and a depth source that does not move when the
camera moves. Those three are about three days of work and they address every
complaint above.

---

## What we have today

Read this first, because most of it is fine and a rewrite would throw it away.

| Piece | State |
|---|---|
| Along-ray thickness from scene depth | Present, drives a colour tint and opacity |
| Fresnel-weighted sky reflection | Present, constant-sky |
| Sun glint / moon glint | Present, analytic, moon added 2026-08-11 |
| Translucent lighting | Surface per-pixel (not the engine default) |
| Ripples | Two-scale panning procedural normals + a small vertex wave, **no texture asset** |
| Foam | A lerp layer |
| Lake bed shading | Nothing depth-aware |
| Underwater | **Nothing for lakes.** The only underwater handling is on the ocean actor: a constant `SceneColorTint` + vignette keyed to sea level z=0 (`VoxelOceanActor.cpp:51-58`). Lakes sit at ~1650 m, so none of it applies |
| Baked bathymetry | **Exists and is unused.** Every basin ships a floor elevation, a surface elevation and a true extent mask |

---

## The three fundamental problems

### 1. We absorb light but never scatter it back

Real water does two things to light. It **absorbs** what passes through it — we do
this. And it **scatters** light back toward the eye out of the water itself — we
do not.

That second term is additive and it *saturates* with distance: past a certain
depth it stops growing. The consequence is the whole depth cue. Distant things
converge on the water's own colour and lose contrast; near things stay crisp.
Absorption on its own just darkens everything uniformly, which is exactly the
flat wash the owner saw underwater, and the "coloured film" read from above.

Both major engines ship the identical two-term structure. UE 5.8's own
`SingleLayerWaterShading.ush`:

```
Luminance = ScatteredLuminance                          <- additive, saturates
          + Transmittance * BehindWaterSceneLuminance   <- what survives
```

**Contrast loss with distance is not a separate feature to implement. It falls
out for free the moment the in-scatter term exists.** This is the single
highest-value change in this document.

We can have it without touching the scene-colour ban: UE ships a depth-only
variant of exactly this path for mobile
(`SINGLE_LAYER_WATER_SHADING_QUALITY_MOBILE_WITH_DEPTH_TEXTURE`), which drops the
`Transmittance * BehindWater` term to zero, emits the scattered luminance as
colour, and hands transmittance to hardware alpha blending.

### 2. The near-field lake surface is a 10 cm staircase

A still lake is flat by definition. Ours is not: within ~25.6 m of the camera the
lake stops being a sheet and becomes actual voxel geometry whose top surface
steps in 10 cm risers.

This one defect produces three separate symptoms:

- **The flicker.** Root-caused 2026-08-11: TSR's per-frame sub-pixel jitter
  cannot stabilise 1–3 pixel high-contrast risers. The terrain half was fixed
  with `r.TSR.ThinGeometryDetection`; water only improved 14% because
  translucency is composited after TSR's anti-flicker machinery runs.
- **"Uneven, lack of depth and 3d shape."** A faceted staircase wearing a
  material designed for a flat plane.
- **The harsh seam** where the voxel disc meets the far-field flat sheet — two
  representations that look nothing alike, joined at 25.6 m.

**Flattening the near-field water surface to the basin datum is expected to
address all three at once**, and it is geometry work, not shader work.

### 3. Depth is measured in a way that moves when the camera moves

Thickness today is `scene depth - pixel depth`: a view-space Z difference. Three
defects follow. It changes as the camera moves, so nothing is stable. It goes
wrong at grazing angles. And on a 10 cm staircase bed it *inherits the
staircase*, drawing contour rings on the lake floor.

We already know the true vertical depth offline, per basin, and are not using it.

---

## Getting the colour right — the finding that would have burned us

Every tutorial says *red absorbs first*. **That is true for ocean water only.**
Akkaynak et al. (CVPR 2017) state it directly: the notion "only holds for oceanic
water types."

Inland lakes are dominated by CDOM — the tea-coloured dissolved organic matter
from leaf litter and peat — which absorbs blue ferociously. Worked from measured
coefficients (Pope & Fry 1997 plus a modest CDOM load of a(440) = 0.5 m⁻¹):

| absorption, per metre | R (611 nm) | G (549 nm) | B (464 nm) |
|---|---|---|---|
| pure water | 0.265 | 0.055 | 0.010 |
| **realistic lake** | 0.311 | **0.164** | **0.367** |

Blue goes from least-absorbed to *most*-absorbed, and green becomes the survivor.
**That is the green-lake signature, and it is derived rather than guessed.** Had
we used ocean coefficients our lakes would have been confidently, physically
wrong.

Two more facts worth carrying:

- **Extinction must be a float3, not a scalar.** The per-channel differential
  *is* the depth cue. Crest's parameter is `_DepthFogDensity.xyz`, UE's is
  `float3 ExtinctionCoeff`, Veloren's is `vec3(0.6, 0.04, 0.01)`. All vectors,
  deliberately.
- **Murky water is a bright medium, not a dark one.** Petzold's measurements:
  going from clear ocean to turbid harbour moves scattering ~50× but absorption
  only ~3×. Model murk as absorption and you get mud; real murk is mostly
  scattering, which is why silty water looks milky rather than black.

**Unit trap, and it is a 100× error waiting to happen.** Epic's public docs say
absorption is in reciprocal *metres*. The engine header
(`MaterialExpressionSingleLayerWaterMaterialOutput.h`) says **"Unit is 1/cm"**,
and the shader multiplies a depth in centimetres. The header is right. Multiply
every per-metre figure above by 0.01.

**Authoring interface — copy Unity's.** Nobody hands an artist a coefficient in
inverse metres. HDRP exposes an **Absorption Distance** (the depth at which 2% of
light survives) plus a colour picker, and derives the coefficient. That is the
knob to build.

---

## Our bans are narrower than we have been treating them

The no-scene-colour rule exists because a **translucent material** that grabs
scene colour breaks our single translucent sort key. It does **not** apply to a
**post-process** material, where the colour buffer is the declared input. Epic's
own underwater effect is a post-process material for exactly this reason.

**So the entire submerged look is available to us**, including screen warp — and
that is the owner's second complaint answered. Similarly,
`r.Water.SingleLayer.Reflection 2` ("reflection captures and skylight only") is a
first-class supported mode in Epic's source, not a degraded fallback: reflection
captures and skylight were never planar reflections or SSR.

Worth knowing for expectations: at normal viewing angles reflection is only
2–6% of the signal (Fresnel at water's IOR is 2.0% straight down, 6.0% at 60°),
so absorption and scattering carry the look. And deep-water brightness is driven
by **skylight / GI**, not by reflection — if deep water reads too dark, that is
where to look.

---

## Order of work

Items 1–3 are the three days that answer the owner's words. Item 4 is what makes
them stable. Item 5 is the underwater complaint.

| # | Work | Effort | GPU cost | Ban-safe |
|---|---|---|---|---|
| 1 | In-scatter term; opacity from transmittance | 1 d | ~10 ALU | yes |
| 2 | float3 extinction, lake coefficients, 1/cm units, Absorption-Distance knob | 0.5 d | free | yes |
| 3 | Depth darkening applied to the **lake bed** (opaque) rather than the surface | 1 d | ~5 ALU on bed | yes |
| 4 | **Flatten the near-field water surface** to the basin datum | ? | likely negative | yes |
| 5 | Bake per-basin vertical depth + distance-to-shore; slant conversion | 2–4 d | 1 fetch | yes |
| 6 | Underwater as a post-process: extinction, halved coefficients, near-field particulates, meniscus | 3–5 d | 1 fullscreen pass | yes |
| 7 | Shore foam from world height + bed slope + noise; wet-edge darkening | 2 d | 1–2 fetches | yes |
| 8 | Caustics in the bed material, two layers, `min()` combine, depth+facing fade | 1–2 d | 2 fetches | yes |

**Why item 3 is better than it sounds.** Darkening the *bed* instead of the
*surface* means the work happens on opaque geometry: per-pixel exact, free when
the bed is off-screen, never touches translucent sort order, and it composes
correctly with the surface's additive in-scatter. This is 117HD's structure and
it is the single most transferable idea for a voxel game.

**Two cheats every shipped game uses, and we should too.** Halve the extinction
when the camera is submerged (Photon does exactly this) or the player cannot see.
And keep the depth *gradient* while cheating the *floor* — Subnautica
deliberately broke physics twice and said so, because "being 150 meters down in
the ocean isn't especially interesting."

---

## What would be wasted effort here

Stated explicitly so nobody spends a month on it:

- **FFT ocean simulation and Jacobian-folding whitecaps.** No folding on a flat
  plane. Sea of Thieves: "calm water will only show foam generation around
  intersecting objects."
- **Ray-traced or photon caustics.** 0.5–2 ms on RTX for one light; a tiling
  texture gets ~90% of the read for two fetches.
- **Ray-marching the lake volume.** `exp(-σd)` is *exact* for a homogeneous
  medium. Marching a constant-density lake buys literally nothing. Only go
  volumetric for light shafts.
- **Subsurface scattering as a separate feature.** It is driven by wave peaks; we
  have no waves. For still water the in-scatter term of item 1 *is* the correct
  SSS.
- **Photoreal pure-water coefficients.** Physically right, visually invisible at
  the 2–20 m depths we ship. Every shipped preset is 3–10× more absorbing.
- **Chasing UE backbuffer dithering.** 5.8 already applies correct
  triangular-PDF dither at the final write. Banding still visible in water is
  upstream — ramp texel precision, filter-weight precision (D3D guarantees only
  8 bits of sub-texel fraction, so a 256-entry ramp has ≤256 blend positions
  *regardless of its bit depth*), or a low-precision intermediate.
- **Refraction.** Worth saying plainly given the ban: Complementary — the
  most-installed Minecraft shader pack — ships with refraction **off by
  default**. Vanilla Minecraft has no refraction, no depth model, and one alpha
  constant, and still reads as water. **We are not missing much by banning
  refraction. We are missing a lot by not having in-scatter.**

---

## Open questions for the owner

1. **How murky?** "Clear alpine tarn" and "peat-stained lowland lake" are
   different coefficient sets, and the honest authoring knob is *how far down can
   you see* (a Secchi depth), which converts directly to a coefficient. This is
   an art call.
2. **How stylised?** The physical model above lands on "believable"; pushing
   toward stylised means saturating the scattering colour and shortening the
   absorption distance rather than changing the model.
3. Item 4 (flattening the near-field surface) changes what the water *is* within
   25.6 m. It should make the lake read as a lake, but it removes the voxel
   character of the surface at close range. Worth one A/B before committing.

---

## Sources

The full cited surveys live in the session transcript. Primary anchors: UE 5.8
`SingleLayerWaterShading.ush` and `MaterialExpressionSingleLayerWaterMaterialOutput.h`;
Unity HDRP `WaterUtilities.hlsl` and its shipped presets; Crest Ocean System
(SIGGRAPH 2019 Advances); 117HD/RLHD; 0 A.D.; Dagor Engine; Sea of Thieves
(SIGGRAPH 2018 Talks); Horizon Forbidden West (SIGGRAPH 2022 Advances);
Akkaynak et al. (CVPR 2017); Pope & Fry (1997); Smith & Baker (1981);
Lagarde on wet surfaces; Gjøl, *Banding in Games*.

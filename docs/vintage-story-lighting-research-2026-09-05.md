# Vintage Story lighting research — what it does, why it reads soft, and how to get the look in our marcher

2026-09-05. Owner directive, verbatim: "research the lighting and shadow system used
in the game vintage story. It's lighting and illumination system seems quite good and
our current game feels very harsh with very dark and very bright lighting depending
on relative position to sun."

Research only — no code was changed. Every claim about Vintage Story (VS) is cited.
VS's engine is closed-source, but its API (vsapi) is on GitHub, its GLSL shaders ship
as plain-text game assets (quoted here from a public mirror), and Tyron (Anego
Studios) has documented the lighting engine in devlogs since 2016. Our-side facts are
cited to files in this repo.

---

## 1. How Vintage Story actually lights its world

### 1.1 The data model: light is a per-voxel FIELD, not a per-frame computation

Every voxel stores a light record, maintained by CPU flood-fill and only recomputed
when blocks change — render cost per frame is just reading it:

- **32 sunlight levels + 32 block-light levels + a hue byte, per voxel.** From the
  May 2016 devlog: "Added data fields for each chunk to be able to store 32 block
  player light, 32 block sun light as well as about one more byte for storing color
  values" — the stated motivation for in-memory chunk compression, because this
  doubled chunk memory. ([Terrain and Light devlog](https://www.vintagestory.at/forums/topic/17-terrain-and-light/))
- **Packed layout** (from the API's own decoder, `ColorUtil.LightUtil.ToRgba`,
  [Math/ColorUtil.cs](https://github.com/anegostudios/vsapi/blob/master/Math/ColorUtil.cs)):
  a `ushort` per voxel — bits 0–4 sun level (0–31), bits 5–9 block-light level
  (0–31), bits 10–15 hue index — plus a separate saturation nibble (`lightSat`,
  see `IWorldChunk.Unpack_AndReadLight(index, out int lightSat)`,
  [Common/API/IWorldChunk.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IWorldChunk.cs)).
  Chunks expose `IChunkLight.GetSunlight/SetSunlight/GetBlocklight/...` per 3D index.
- **`GetLightLevel(pos, EnumLightLevelType)` returns 0..32**, and `OnlySunLight` is
  documented as "just the sun light **unaffected by the day/night cycle**"
  ([Common/API/IBlockAccessor.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IBlockAccessor.cs))
  — the stored sun level is time-of-day-independent; the day/night ramp is applied at
  render time (§1.4). `GetLightRGBs` returns "XYZ component = block light rgb,
  W component = sun light brightness" (same file) — the exact per-voxel quantity the
  renderer consumes.

### 1.2 Propagation: flood fill with per-block absorption

- Sunlight enters from the sky and is "calculated and recalculated for the chunk
  where the block changed, and for its neighbors below it"; block light spreads
  outward from sources "like a liquid", attenuating per block
  ([forum: Algorithm for determining light level](https://www.vintagestory.at/forums/topic/5500-algorithm-for-determining-light-level/),
  [mod-db discussions of the vanilla spread model](https://mods.vintagestory.at/lumos)).
- Blocks declare `lightAbsorption` (how many levels they eat) and light sources
  declare `lightHsv: [hue, saturation, value]` with value up to the low 30s (the
  engine hard-caps near 32) in their JSON
  ([Modding: Block Json Properties](https://wiki.vintagestory.at/index.php/Modding:Block_Json_Properties),
  [forum: what do these numbers represent](https://www.vintagestory.at/forums/topic/3265-what-do-these-numbers-represent/)).
  Since 1.22, absorption is computed "down to the voxel level" for chiselled
  blocks/slabs and light can round corners more spherically
  ([1.22 light-leak issue, anegostudios/VintageStory-Issues #9772](https://github.com/anegostudios/VintageStory-Issues/issues/9772)).
- In-game source values for scale: torch 14, lit firepit 16, lantern 18–20,
  chandelier up to 24; full sky sunlight sits at 22 of the 0–31 range
  ([wiki: Light sources](https://wiki.vintagestory.at/Light_sources),
  [forum algorithm thread](https://www.vintagestory.at/forums/topic/5500-algorithm-for-determining-light-level/)).
- **Level → brightness is a lookup table, not a formula**: the server configures
  `float[] SunLightLevels` and `float[] BlockLightLevels` ("the currently configured
  sun light brightness levels") and `int SunBrightness` ("the currently configured
  max sun light level") — an artist-shaped, nonlinear perceptual ramp
  ([Common/API/IWorldAccessor.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IWorldAccessor.cs)).
  `LightUtil` bakes these tables to bytes and converts (level, hue, sat) → RGBA with
  block light in RGB and sun level in A
  ([Math/ColorUtil.cs](https://github.com/anegostudios/vsapi/blob/master/Math/ColorUtil.cs)).

### 1.3 Meshing: per-VERTEX light + corner AO ("AO+Smoothlight")

The chunk tesselator writes the flood-filled light into each vertex:
`layout(location = 2) in vec4 rgbaLightIn; // rgb = block light, a=sun light level`
(chunkopaque.vsh — VS's shaders are plain-text game assets; quoted from a
[public mirror of the asset folder](https://github.com/TheDarkLord777/vintagestory/tree/master/assets/game/shaders)).
Because the light value is per-vertex and the GPU interpolates it, every cube face
carries a smooth gradient rather than one flat value. The user-facing switch is
**"AO+Smoothlight — Produce smooth shadow transitions and block-level ambient
occlusion. Should be left on by default."**
([wiki: Settings](https://wiki.vintagestory.at/Settings)) — i.e. Minecraft-style
vertex smoothing of the 4 adjacent cells plus corner-darkening AO, on by default.
Sub-block-scale AO is a separate SSAO pass: "Adds ambient occlusion to fine geometry
at sub-block level" (same wiki page); since 1.14 new installs default to "High
quality preset with SSAO and low shadows on"
([v1.14.0 stable release notes](https://www.vintagestory.at/blog.html/news/stable-steel-age-and-character-customization-v1140-r270/)).

### 1.4 Shading: the exact math (from the shipped GLSL)

**Combining sun and block light** (`applyLight`, fogandlight.vsh, mirror above):

```glsl
vec3 blockLightColor = lightColor.rgb;                 // flood-filled, colored
vec3 sunLightColor   = lightColor.a * ambientColor.rgb; // sun level x day/night sun color
// 1. Mix colors according to their brightness (very bright light has more influence)
vec4 rgba = (2*bSun*sunColor + bBlock*blockColor) / (2*bSun + bBlock);
// 2. Fix brightness
rgba *= max(bGlow, max(bSun, bBlock)) / ((rgba.r+rgba.g+rgba.b)/3);
```

Two deliberate choices: sun gets 2x weight in the hue mix, and the final brightness
is the **max** of the contributors, renormalized — light sources never sum to
blow-out, and a torch's warm hue visibly tints a sunlit area without brightening it.
`bBlock = max(MINBRIGHT, bBlock)` ("Light up all caves") floors the darkest possible
pixel. The day/night cycle rides `ambientColor` (uniform per frame) and the
calendar's `SunColor` / `DayLightStrength` (0 = no sunlight, 1 = full) /
`SunsetMod` ("Creates a greater variety of sunsets")
([Common/API/IGameCalendar.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IGameCalendar.cs)).

**The face-orientation term** — this is the one to stare at (chunkopaque.vsh /
fogandlight.fsh `getBrightnessFromNormal`):

```glsl
float intensity = 0.34 + (1 - shadowIntensity)/8.0;   // 0.45 when shadows are off
nb = max(max(intensity, 0.5 + 0.5 * dot(normal, lightPosition)), normal.y * 0.95);
```

Three mechanisms in one line:
1. **Half-Lambert wrap**: `0.5 + 0.5·N·L`, not `saturate(N·L)`. A face at 90° to the
   sun gets 0.5, and even a face pointing dead away gets 0 only asymptotically.
2. **A hard floor** (~0.34 by day, 0.45 with shadows off): no face orientation can
   darken a surface below roughly a third of full. The comment in the fsh version is
   explicit about the intent: "makes semi sunfacing block sides pretty dark" is the
   failure mode they tuned against.
3. **A sky boost for up-facing surfaces**: `max(nb, normal.y * 0.95)` — their comment:
   "diffuse light from the sky provides an additional brightness boost for up facing
   stuff because the top side of blocks being darker than the sides is uncanny o__O".

And `nb` is a bounded MULTIPLIER on the already-propagated light
(`mix(1, nb, normalShadeIntensity)` in the general helper), never the whole lighting
answer.

**Cast shadows** (`getBrightnessFromShadowMap`, fogandlight.fsh): two shadow-map
cascades (near + far), each sampled 3x3 PCF through `sampler2DShadow`, and — the
important part — each cascade can remove at most `shadowIntensity * 0.5` of
brightness:

```glsl
float b = 1.0 - shadowIntensity * totalFar * shadowCoordsFar.w * 0.5;   // far cascade
b -=       shadowIntensity * totalNear * shadowCoordsNear.w * 0.5;      // near cascade
b = clamp(b + blockBrightness, 0, 1);                                    // torches punch through
```

`blockBrightness = clamp(max(bGlow, bBlock) - bSun/2, 0, 1)` is computed in the
vertex shader and ADDED BACK after the shadow term — a torch-lit surface cannot be
darkened by a sun shadow. The fragment then takes `min(shadowB, nb)` (shadow and
face-shade don't stack multiplicatively) and multiplies the textured, vertex-lit
color once. `shadowIntensity` is a uniform the engine drives down at low sun angles
and in weather, so shadows fade out at dusk instead of becoming long black knives.

**Shadow history and perf story**: shadows arrived in v1.4.8 ("Creatures and terrain
now drops shadows onto the terrain/creatures, depending on the sun or moon position…
Turning the shadow quality to very high might turn your machine into an egg cooker.
Won't work on low-end computers, sorry!" —
[Shadows v1.4.8 devlog](https://www.vintagestory.at/blog.html/news/shadows-v148-r109/)),
were "much improved" in 1.10
([v1.10 devlog](https://tyronx.itch.io/vintage-story/devlog/100454/version-110-released)),
and remain a 5-step quality slider (off/low/medium/high/very high, "High FPS cost",
[wiki: Settings](https://wiki.vintagestory.at/Settings)). The default cascades cover
only the near field; a community mod (Coriaender Shaders) exists specifically to
"add cascading shadow maps to extend shadows to the full draw distance"
([mod page](https://mods.vintagestory.at/coriaendershaders)) — i.e. the base game
deliberately ships short-range shadows and leans on the propagated field + face term
for everything beyond. **The whole vanilla game is playable with shadows OFF** and
still looks like Vintage Story, because shadow maps contribute at most a bounded
0.5x modulation on top of a lighting model that is complete without them.

---

## 2. Why it FEELS good — the perceptual mechanisms

Ranked by how directly each one kills our reported symptom ("near-black unlit faces
beside full-sun faces"):

1. **Bounded contrast ratio between lit and shaded faces.** In VS the directional
   term can never take a daylit face below ~0.34 of full (floor), a side face sits
   at ~0.5 (half-Lambert), and a shadow-mapped pixel keeps ≥50% per cascade.
   Worst-case lit:shaded face ratio in open daylight is about **3:1**. Ours,
   measured: near-field shaded snow at **22.5%** of the sunlit snow beside it before
   the ambient existed, i.e. 4.4:1 — and that number was itself flattered by haze;
   the true surface term for an anti-sun face under `saturate(N·L)` is **zero**
   (VoxelMarchRenderer.cpp:1002–1007, the 2026-08-30 measurement block). *This is
   the single mechanism that kills black-beside-white:* VS refuses, structurally, to
   let orientation alone produce more than ~3x contrast. Everything else is polish
   on top of that guarantee.

2. **Light arrives by propagation, so it wraps into shade.** A face the sun misses
   under an overhang still sits in air whose sun level is 20-something — the flood
   fill carried skylight around the corner with -1/block attenuation. Shaded ground
   next to open ground is bright; the back of a 10 m-deep overhang is dim; a cave is
   black. Our flat `voxel.March.AmbientIntensity` gives every shaded face the same
   lift whether it's 10 cm into a crevice or on an open hillside — the cvar's own
   comment names the risk: "it will lift the inside of a cave exactly as much as the
   outside of a mountain" (VoxelMarchRenderer.cpp:1048–1050, VoxelMarch.usf:3398–3403).
   Propagation is what makes shade *graded and situational* instead of uniform.

3. **Per-vertex interpolation removes the per-face step.** VS light varies WITHIN a
   face (vertex gradient) and BETWEEN faces smoothly (adjacent faces share corner
   samples). Two coplanar faces never butt a bright quad against a dark quad unless
   the field itself steps. Our per-face constant normal + per-face constant ambient
   makes every cube edge a visible lighting discontinuity.

4. **Corner AO supplies the darkness that feels "earned".** With contrast bounded,
   the scene needs *some* local darkening to avoid flatness; VS puts it exactly
   where geometry justifies it (block-level corner AO + sub-block SSAO), so creases
   read dark while open faces stay lifted. We already have the corner AO
   (VoxelMarch.usf:3345–3363, byte-identical to `vxc::detail::aoCorner`).

5. **Shadows attenuate only the sun, are soft, and are bounded.** 3x3 PCF, ≤0.5
   per cascade, `shadowIntensity` fading with sun height, block light added back.
   Shadowed ground in VS is "less sun", never "no light". Composition order matters
   as much as the shadow tech itself.

6. **Warm/cool separation.** Sun/ambient color ramps warm at low sun
   (`SunColor`, `SunsetMod`); block light carries its own hue (torch orange, mushroom
   blue) and the brightness-weighted mix keeps hue without additive blow-out. Shaded
   areas therefore drift cool (sky-toned) while lit areas are warm — contrast is
   expressed partly as COLOR instead of purely as luminance, which reads gentler at
   the same measured ratio. (Our ambient is already slightly cool, 1.00/1.04/1.12 —
   VoxelMarchRenderer.cpp:1076–1078 — the right instinct, currently the only place
   it exists.)

7. **The level→brightness ramp is authored, not physical.** `SunLightLevels[]` /
   `BlockLightLevels[]` are hand-shaped tables; VS never argues with a radiometric
   formula about how dark level 12 should look. Our project already works this way
   (every appearance knob is owner-tuned by eye), so this is a philosophical match.

---

## 3. Mapping to our renderer, mechanism by mechanism

Our relevant machinery: (a) the GPU brick volume the marcher reads; (b) the
camera-centred GI volume — `voxel.GI.VolumeDim` 192 at 40 UU cells = ±38.4 m box,
directional Pos/Neg visibility, faded to neutral 1.0 beyond
(VoxelGIVolume.cpp:39–66, VoxelMarch.usf:3295–3343); (c) per-face normals + the
emissive hemisphere ambient `voxel.March.AmbientIntensity` (default 1.5,
VoxelMarchRenderer.cpp:1031–1079, VoxelMarch.usf:3378–3412); (d) the sun-ray shadow
march `voxel.Shadow.March` (default ON since 2026-09-05 for owner evaluation),
which composes as a light-function mask on the sun directional light — so it already
attenuates ONLY the direct sun term, exactly VS's layering
(VoxelShadowMarch.cpp:99–105, 1619–1628). The sun N·L itself is UE's deferred
directional light against our GBuffer normal; the marcher writes DefaultLit +
`SKIP_PRECSHADOW` (VoxelMarch.usf:3419–3453).

| VS mechanism | Verdict | Shape of the adaptation |
|---|---|---|
| Half-Lambert wrap + floor + up-boost (`nb` line) | **Applicable as-is, cheapest thing on this list** | We can't rewrite UE's directional-light N·L, but we don't need to: the marcher already owns an emissive term. Make the hemisphere ambient sun-aware: `Ambient *= (Floor + WrapGain * (0.5 + 0.5*dot(N, SunDir)))`, keeping the existing ground-mix. SunDir is already known CPU-side (the shadow march uses it); one more float4 uniform, a handful of ALU in a ray-count-bound shader. Result: anti-sun faces lift toward a tunable fraction of sun-facing ones — VS's bounded-contrast guarantee — while UE's real N·L still provides the directional modelling on top. Tune with a screenshot ladder exactly like the 2026-08-30 AmbientIntensity sweep. |
| Propagated per-voxel sunlight field | **Adaptable — the headline item, and the natural fit for (a)** | Propagate a 5–6-bit sun level through the brick volume's cells (flood from open sky, -1 per cell laterally/down through air, absorption for solids), computed when a chunk uploads/changes — VS proves the cost model: propagation is paid on EDIT, rendering pays one sample. Store per brick cell (a second channel next to occupancy/material, or a half-res sibling volume; at 0.5–1 m propagation cells it matches VS's granularity — propagating at 25 mm would be waste). Marcher samples it at the hit point and uses it AS the ambient scale, replacing the flat AmbientIntensity: `Ambient = AmbientColor * SunLevelTable[level]` — skylight wraps into overhangs, dims in ravines, dies in caves, **with no shadow maps at all**. This is the VS look. Fallback where the field isn't resident (far rings): today's flat constant — the seam is a fade, same pattern as the GI volume's FadeStart/End. |
| Per-vertex smoothing across faces | **Adaptable, and cheaper for us than for VS** | We have no vertices, but the propagated field lives in a 3D texture: a single trilinear `SampleLevel` at the hit position IS the smoothing — hardware interpolation gives the within-face gradient VS builds by hand at vertices. (The GI volume sampling code at VoxelMarch.usf:3298–3341 is the exact template, push-out offsets and all.) No per-face steps in the ambient once the field is trilinear. |
| Block-level corner AO | **Already shipped** | `VoxelMarchCornerAO` is byte-identical to the raster path's rule. Tuning note from VS: AO composes against a FLOORED light term, so corners read dark-vs-lit rather than black-vs-black. Once the wrap floor exists, today's AO will automatically read more like VS. |
| Bounded, sun-only, soft shadows | **Applicable as tuning of what shipped this week** | The light-function composition is already VS-correct (sun only; ambient untouched). Two VS lessons to port: (1) the mask should bottom out at a floor (VS: ≥0.5 per cascade) rather than 0 — bake `lerp(ShadowFloor, 1, mask)` into the LF material or the mask write; (2) scale shadow strength down with sun elevation/weather (`shadowIntensity` analog) so dawn/dusk doesn't produce long black shadows. Perf: keep `voxel.Shadow.MarchRayReachM` short (96 m default) — VS ships near-field-only cascades for the same reason our own cvar comment gives: "occluders hundreds of metres away contribute almost nothing at a high sun" (VoxelShadowMarch.cpp:168–181; the August coupling mistake, 0.4 ms → 10.5 ms at 512 m rays, is the local proof). |
| Colored block light (hue/sat channels) | **Adaptable later, needs the field first** | VS budgets 6-bit hue + a sat nibble on top of the block-light level. For us this is a second propagated channel (torches, lava, glowshrooms) sampled the same way and mixed VS-style: brightness = max, hue = brightness-weighted — not additive. Depends on having placed light sources; not part of the harshness fix. |
| Day/night warm/cool ramp | **Applicable as-is** | Drive the ambient color (and the wrap term's sun tint) from the existing sky subsystem (VoxelSkySubsystem / MPC_VoxelSky) instead of the hard-coded 1.00/1.04/1.12: warm at low sun, cool blue at night, `SunsetMod`-style variety optional. Pure uniform plumbing. |
| SSAO (sub-block) | **Not applicable / already covered** | VS needs SSAO because its blocks contain sub-block geometry. Our sub-voxel detail IS voxels — corner AO covers it. UE-side SSAO/GTAO over the GBuffer is available if ever wanted; not part of this track. |
| The GI volume (b) | **Keep, reposition** | The Pos/Neg visibility volume is a near-field *bounce/occlusion* refinement, not a skylight model — beyond ±38 m it's neutral 1.0, which is exactly where the harshness lives. Once the world-space propagated field exists, the GI volume's role shrinks honestly to near-camera directional bounce (and local lights via VolumeLocal); don't try to stretch it to do the skylight job — its camera-box + fade design is wrong for that, and the brick-resident field is right. |

---

## 4. Recommended incremental path (cheapest first)

Each step is independently shippable, owner-judgeable from a screenshot pair, and
follows the standing discipline (default-off cvar, engagement proof, ladder, verdict).

**Step 1 — Sun-wrapped ambient + shadow floor (hours-to-a-day, ALU only).**
Add `voxel.March.AmbientWrapGain` (and a floor knob) so the emissive ambient becomes
`AmbientColor * lerp(Floor, 1, 0.5 + 0.5*dot(N, SunDir))` composed with the existing
ground mix; floor the shadow mask at ~0.4–0.5 and scale it by sun elevation.
*Expected visual*: the black-beside-white face pairs collapse to roughly 3:1, shaded
snow reads as snow-in-shade, shadows read soft; the terrain keeps its modelling
because UE's real N·L still rides on top. This is VS mechanisms #1, #5 and most of
#6, with zero new data structures. Risk: overall frame brightens (the AmbientIntensity
ladder showed the trade); the wrap gain is the dial.

**Step 2 — Ambient color from the sky subsystem (a day).** Warm/cool day-night ramp
into the same uniform (mechanism #6 completed). Cheap, independent, screenshot-judged.

**Step 3 — Propagated sunlight field in the brick volume (1–2 weeks, the headline).**
5–6-bit sun level at 0.5–1 m cells, flood-filled at chunk build/edit (CPU alongside
worldgen, or a few GPU relax iterations in the existing upload chain), resident next
to the bricks; marcher samples trilinearly at the hit and uses it as the ambient's
spatial scale (Steps 1–2 keep their roles as the directional/color shaping of that
ambient). *Expected visual*: overhangs and gullies darken with depth, cave mouths
fade to black over metres, cliff bases sit in graded sky-shade — the specific
"light wraps into shade" quality that makes VS read well — with no per-frame
propagation cost and no dependence on the shadow march. This also finally gives the
"lift the cave as much as the mountain" ambient an honest occlusion signal.
(Underground polish beyond that belongs to the parked underground rework.)

**Step 4 — later, on demand.** Colored block-light channel for placed lights
(VS hue/sat model, max-not-sum mixing); shadow-march softening (jittered/multi-tap
mask filtering) if the owner wants softer penumbrae than the LF mask gives; GI
volume re-scoped to near-field bounce only.

A useful framing for the owner: **Vintage Story without shadow maps already looks
like Vintage Story.** Steps 1–3 reproduce that shadow-map-free core; our shadow
march (already on for evaluation) then plays the same optional, bounded,
sun-only role VS's shadow slider plays.

---

## 5. Sources

Vintage Story (all public):
- [Terrain and Light — Tyron devlog, 2016](https://www.vintagestory.at/forums/topic/17-terrain-and-light/) — 32+32 light levels + color byte, chunk-compression cost.
- [vsapi IBlockAccessor.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IBlockAccessor.cs) — 0..32 levels, `EnumLightLevelType`, `GetLightRGBs` layout.
- [vsapi IWorldChunk.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IWorldChunk.cs) — `IChunkLight`, packed light + `lightSat`.
- [vsapi ColorUtil.cs (LightUtil)](https://github.com/anegostudios/vsapi/blob/master/Math/ColorUtil.cs) — bit layout, brightness tables, HSV→RGBA.
- [vsapi IWorldAccessor.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IWorldAccessor.cs) — `SunLightLevels`/`BlockLightLevels`/`SunBrightness`.
- [vsapi IGameCalendar.cs](https://github.com/anegostudios/vsapi/blob/master/Common/API/IGameCalendar.cs) — `SunColor`, `DayLightStrength`, `SunsetMod`.
- VS game shaders `chunkopaque.vsh/.fsh`, `fogandlight.vsh/.fsh`, `vertexflagbits.ash` — plain-text game assets, quoted from a [public mirror](https://github.com/TheDarkLord777/vintagestory/tree/master/assets/game/shaders) (verify against a local install's `assets/game/shaders` if exactness matters).
- [wiki: Settings](https://wiki.vintagestory.at/Settings) — "AO+Smoothlight", SSAO, shadow-quality slider text.
- [wiki: Light sources](https://wiki.vintagestory.at/Light_sources) — source levels.
- [Shadows (v1.4.8) devlog](https://www.vintagestory.at/blog.html/news/shadows-v148-r109/) and [v1.10 devlog](https://tyronx.itch.io/vintage-story/devlog/100454/version-110-released) — shadow-map introduction and perf framing.
- [v1.14.0 stable notes](https://www.vintagestory.at/blog.html/news/stable-steel-age-and-character-customization-v1140-r270/) — SSAO in the default preset.
- [forum: Algorithm for determining light level](https://www.vintagestory.at/forums/topic/5500-algorithm-for-determining-light-level/) — sun peak 22, block/sun query split.
- [Modding: Block Json Properties](https://wiki.vintagestory.at/index.php/Modding:Block_Json_Properties) and [forum: lightHsv numbers](https://www.vintagestory.at/forums/topic/3265-what-do-these-numbers-represent/) — `lightHsv`, engine cap near 32.
- [VintageStory-Issues #9772](https://github.com/anegostudios/VintageStory-Issues/issues/9772) — 1.22 voxel-level absorption / spherical spread.
- [Coriaender Shaders mod](https://mods.vintagestory.at/coriaendershaders) — community CSM extension, evidence base-game shadows are near-field by design.

This repo:
- `ue-project/Source/VoxelEarthShaders/Private/VoxelMarchRenderer.cpp` :999–1123 — ambient cvars, 22.5% measurement, SkyLight null result.
- `ue-project/Shaders/VoxelMarch.usf` :3275–3453 — GI sample, corner AO, hemisphere ambient, GBuffer writes.
- `ue-project/Source/VoxelEarthShaders/Private/VoxelShadowMarch.cpp` :99–200, :1546–1634 — shadow march cvars, reach/cost history, light-function composition.
- `ue-project/Source/VoxelEarthShaders/Private/VoxelGIVolume.cpp` :39–66 — GI volume extent/cells.
- `docs/perf-redesign-2026-09-03.md` — current frame budget (marcher 69% of steady GPU; p95 12.3 ms shipped stack) constraining any new per-frame cost.

<!--
PROVENANCE. Verbatim copy of design/UE5_ART_ASSETS.md from the Mira-Thal /
Voxelmark Godot checkout, github.com/mnoles1911/Test @ main, retrieved
2026-08-24. Written 2026-06-16 as research for the UE5 port -- so unlike the
audio docs, this one was already aimed at Unreal and its advice mostly lands.

It is the closest thing to a VFX asset list that exists. There is no list of
gameplay effects to port, because in the Godot build those were code-built
GPUParticles3D nodes (BloodBurst, BloodDrip, DustBurst) and that repo's
asset-pipeline doc says so outright: "Particle systems / VFX -- these are
runtime, not asset pipeline. Don't try to generate blood particles." What this
doc lists is the SOURCE MATERIAL those effects sample: CC0 sprite sheets,
HDRIs and detail normals, every entry licence-checked, with direct download
URLs and the recommendation that sky/cloud/fog/water stay procedural.

FOUR CORRECTIONS FOR THIS REPO, since it was written before any of it existed
here:

1. THE SKY HALF IS ALREADY DONE. tools/fetch-sky-assets.ps1 fetches the star
   map and moon textures, recorded with source page and sha256 in
   ue-project/Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md. Do not
   re-fetch, and note the NASA attribution obligation recorded there.

2. THE HDRI RECOMMENDATION IS PARTLY OVERTAKEN. voxelsim renders its sky
   through M_SkyAtmosphereDome / M_NightSky with MPC_VoxelSky driving them, and
   regenerating that MPC breaks dependents (see docs and the sky-chain regen
   script). Treat the three Poly Haven skies as candidate SkyLight sources to
   evaluate, not as a shopping list to act on.

3. THE WATER MATERIAL ADVICE IS SUPERSEDED. This doc recommends Single Layer
   Water; voxelsim already ported SLW and rebuilt the water rendering in
   August 2026. Its normal-map suggestions may still be useful; its
   architecture section is history.

4. THE DOWNLOAD RULE. Everything here is third-party and mostly large. It goes
   through a fetch script into a gitignored directory with a credits file, the
   pattern SKY_ASSET_CREDITS.md sets -- never committed. The Kenney particle
   and smoke packs (150 sprites between them) are the genuinely new material,
   and they are what a first Niagara emitter would sample.

Authored effects live in ue-project/Content/VFX/; see that directory's README.
-->

# UE5 Atmosphere Art Assets — license-vetted, ship-safe (CC0/PD/MIT)

**Status:** RESEARCH / PLAN (2026-06-16). Companion to `design/UE5_TECH_STACK.md` (canonical stack),
`design/UE5_RENDERING_STRATEGY.md`, `design/WEATHER_AND_ENVIRONMENT.md`, `design/WATER_SHADER_V3_PLAN.md`.

> **Scope note:** these HDRIs / normals / VFX sprites are for **atmosphere** — sky, lighting, fog,
> water, weather particles. They do **not** texture the voxel terrain: voxel surfaces are **per-face
> solid color** baked into vertex color (no atlas), see `UE5_RENDERING_STRATEGY.md` + `UE5_TECH_STACK.md` §6.

**Goal:** Elevate atmosphere (sky, lighting, fog, weather, water) for the UE5 medieval
voxel RPG. Mood target: **Veloren + Skyrim** — soft volumetric skies, dramatic weather
states, readable wet/snow surfaces.

**Hard rule on licensing:** Only **CC0 / public-domain / MIT** assets qualify to ship.
Anything CC-BY is flagged **"needs attribution"** and should be avoided unless we commit
to maintaining a credits file. When in doubt, prefer CC0.

> **No binaries were downloaded.** Every entry below is a verified asset *page* plus its
> direct-download URL pattern, so a developer can pull exactly what they decide to use.

---

## TL;DR — the recommendation is a HYBRID

1. **Get the sky, lighting, fog, and weather particles from UE5 built-ins** (procedural,
   zero downloads). This is already most of the atmosphere.
2. **Download only a handful of high-value CC0 textures/HDRIs** where a real-world capture
   beats procedural: HDRI skies for the SkyLight/reflection capture, and a water
   normal + ground/rock detail normals.
3. **Author the water *material* procedurally** (Single Layer Water shading model) but feed
   it one good CC0 water normal map — that one texture is the highest-leverage download here.

This mirrors the existing Godot weather system (six-state machine, camera-following rain/snow,
fog override, wind-driven water), so the UE5 build is a re-implementation, not a redesign.

---

## 1. UE5 built-ins first — what needs NO download

Before downloading anything, the designer should know these atmosphere systems are fully
**procedural** in UE5 and already (or nearly) in the level. Downloading assets for these adds
little or nothing.

| Atmosphere system | UE5 built-in (no download) | What a download *would* add |
| --- | --- | --- |
| **Sky / horizon glow** | `SkyAtmosphere` actor — physically-based Rayleigh/Mie scattering, sun-driven. Already in level. | Nothing for the *sky itself*. (An HDRI is for the **SkyLight**, not the visible sky — see below.) |
| **Clouds** | `VolumetricCloud` actor + the default `m_SimpleVolumetricCloud` material. Already in level. Animate coverage/density via a Material Parameter Collection. | Nothing required. Optional cloud noise textures only if you want a custom cloud material. |
| **Lighting / GI / reflections** | **Lumen** (GI + reflections) — already on. `DirectionalLight` (sun) + `SkyLight`. | Nothing. Lumen is the renderer; assets don't replace it. |
| **Ambient light color + reflections** | `SkyLight` set to **SLS Captured Scene** captures the SkyAtmosphere automatically. | An **HDRI** as the SkyLight source gives richer, art-directed ambient + reflection per weather state. **This is the one place HDRIs earn their keep.** |
| **Fog (height + volumetric)** | `ExponentialHeightFog` with **Volumetric Fog** checkbox on. Drives god-rays, depth, weather murk. | Nothing. 100% procedural. Tune `FogDensity`, `FogHeightFalloff`, scattering color per weather state. |
| **Rain / snow / storm particles** | **Niagara** — GPU sprite/ribbon emitters, camera-following. | CC0 sprite textures (droplet, splash, snowflake, smoke) so the particles look good. Niagara needs *art* even though it's "built-in." |
| **Water surface** | **Single Layer Water** shading model — a *custom material* you author. The Water plugin's `Water Body` actors give surface/spline tooling. | A CC0 **water normal map** (and optionally a flowmap) — meaningfully improves ripple/wave detail over hand-authored noise. Highest-value texture download. |
| **Wet surfaces after rain** | Material logic: lerp roughness/specular by a "wetness" scalar in a Material Parameter Collection (matches the Godot wet-terrain sheen). | A CC0 detail normal helps wet rock/ground read correctly. |

**Bottom line:** Sky, clouds, Lumen, and fog need **zero** downloads. We download for exactly
three jobs: **(a)** HDRI skies for the SkyLight/reflection, **(b)** a water normal, **(c)** a
few Niagara VFX sprites + ground/rock detail normals.

---

## 2. Curated CC0 downloads

All entries verified **CC0 / public-domain** on 2026-06-16. Resolutions/formats listed are
what to grab for a game (4K is plenty; 2K for distant/detail). Download URL patterns are the
real CDN paths.

### 2a. The table

| Asset | Source page | License | Res / Format | Feeds which UE5 system |
| --- | --- | --- | --- | --- |
| **Kloofendal 48d Partly Cloudy (Pure Sky)** | [polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) | **CC0** | 4K EXR | **SkyLight source + reflection capture** for the **clear/partly-cloudy day** weather state |
| **Kloppenheim 06** | [polyhaven.com/a/kloppenheim_06](https://polyhaven.com/a/kloppenheim_06) | **CC0** | 4K EXR/HDR | **SkyLight + reflection** for an **overcast / soft-light** state (low-contrast, diffuse) |
| **Belfast Sunset (Pure Sky)** | [polyhaven.com/a/belfast_sunset_puresky](https://polyhaven.com/a/belfast_sunset_puresky) | **CC0** | 4K EXR | **SkyLight + reflection** for **dusk / dramatic golden-hour** state (Skyrim sunset mood) |
| **Rocky Terrain 02** (normal map) | [polyhaven.com/a/rocky_terrain_02](https://polyhaven.com/a/rocky_terrain_02) | **CC0** | 4K JPG (`nor_gl`) | **Detail normal** for wet rock / cliffs + reused as a coarse **water surface normal** layer |
| **Ground 037** | [ambientcg.com/view?id=Ground037](https://ambientcg.com/view?id=Ground037) | **CC0** | 2K–4K PNG (PBR set) | **Ground detail normal/roughness** — damp moss/earth, reads well under rain wetness |
| **Rock 023** | [ambientcg.com/view?id=Rock023](https://ambientcg.com/view?id=Rock023) | **CC0** | 2K–4K PNG (PBR set) | **Cliff/stone detail normal** for voxel rock faces; wet-surface sheen |
| **Kenney Particle Pack** (80 sprites) | [kenney.nl/assets/particle-pack](https://www.kenney.nl/assets/particle-pack) | **CC0** | 512×512 PNG | **Niagara** — splash, spark, magic, glow sprites (rain splash, embers) |
| **Kenney Smoke Particles** (70 sprites) | [kenney.nl/assets/smoke-particles](https://www.kenney.nl/assets/smoke-particles) | **CC0** | PNG | **Niagara** — smoke/fog wisps, chimney smoke, dust, storm haze |

> **Water normal note:** ambientCG has **no dedicated flat "Water" PBR surface** (its water
> tag returns Ice, wet-Ground, and SurfaceImperfection droplet stains — verified). For a true
> animated water normal, the best CC0 route is **(1)** Poly Haven's `rocky_terrain_02` normal
> as a base wave-detail layer panned at two speeds, or **(2)** author a procedural Gerstner/normal
> setup in the material and use the CC0 normal only for fine ripple. Avoid CC-BY water packs.

### 2b. Direct-download URL patterns (verified)

**Poly Haven HDRIs** — `https://dl.polyhaven.org/file/ph-assets/HDRIs/[exr|hdr]/[res]/[name]_[res].[ext]`
```
https://dl.polyhaven.org/file/ph-assets/HDRIs/exr/4k/kloofendal_48d_partly_cloudy_puresky_4k.exr
https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/4k/kloppenheim_06_4k.hdr
https://dl.polyhaven.org/file/ph-assets/HDRIs/exr/4k/belfast_sunset_puresky_4k.exr
```

**Poly Haven textures** — `https://dl.polyhaven.org/file/ph-assets/Textures/[jpg|png]/[res]/[name]/[name]_[map]_[res].[ext]`
```
https://dl.polyhaven.org/file/ph-assets/Textures/jpg/4k/rocky_terrain_02/rocky_terrain_02_nor_gl_4k.jpg
```
(`nor_gl` = OpenGL-convention normal — **the right one for UE5**. Do *not* use `nor_dx`.)

**ambientCG materials** — `https://ambientcg.com/get?file=[AssetID]_[Res]-[Format].zip`
```
https://ambientcg.com/get?file=Ground037_2K-PNG.zip
https://ambientcg.com/get?file=Rock023_2K-PNG.zip
```
(Swap `2K`→`4K` for higher res; `PNG`→`JPG` for smaller files.)

**Kenney packs** — direct zip on their CDN:
```
https://www.kenney.nl/media/pages/assets/particle-pack/f8fe0f8cb8-1677578741/kenney_particle-pack.zip
https://www.kenney.nl/media/pages/assets/smoke-particles/23249a0d35-1677695171/kenney_smoke-particles.zip
```

### 2c. License verification notes

- **Poly Haven** — entire library is **CC0**, no signup, no attribution required. Verified on
  each asset page above. (FAQ: "use them for absolutely any purpose, including commercial.")
- **ambientCG** — entire library is **CC0 1.0 Universal**. Verified on `Ground037` and `Rock023`
  pages ("free to use without attribution — even in commercial circumstances").
- **Kenney** — both packs explicitly tagged **Creative Commons CC0** on their pages.
- **OpenGameArt** — **NOT used here.** Its assets are a mix of CC0 / CC-BY / GPL per-upload and
  must be checked individually. Skipped in favor of the three CC0-guaranteed sources above. If a
  specific OGA asset is ever wanted, confirm the per-asset license reads **CC0** and record the
  author/URL before shipping. Treat anything CC-BY as **needs attribution**.

### 2d. UE5 import notes (so this is actionable tomorrow)

- **HDRIs (.exr/.hdr):** Import as Texture → set **Texture Group: HDRI**, **Compression: HDR**,
  **sRGB OFF**. Assign to a `SkyLight` (Source Type = **Specified Cubemap**) per weather state,
  OR keep SkyLight on **Captured Scene** and use the HDRIs only for offline reflection-capture
  reference. Recommended: one SkyLight whose cubemap you swap/blend per state.
- **Normal maps:** Import with **sRGB OFF**, **Compression: Normalmap (BC5)**, **Flip Green OFF**
  when using Poly Haven `nor_gl` (already OpenGL/UE convention). ambientCG normals are DirectX
  convention by default — **flip green** (or grab their GL variant if offered).
- **Kenney sprites:** Import PNGs with alpha; in Niagara use a **Sprite Renderer** with a
  translucent/additive material. Pack multiple into a flipbook/sub-UV where useful (splashes).

---

## 3. Dynamic weather / sky architecture (UE5 best practice)

These three short sections are the developer-facing architecture. They re-implement the existing
Godot six-state weather machine (`scripts/WeatherManager.gd`) on UE5 primitives.

### (a) Time-of-day + weather state machine driving Sky/Sun/Clouds/Fog

The cleanest modern approach (UE5.5+) is the built-in **Day Sequence** plugin
([Day Sequence Time of Day Plugin](https://dev.epicgames.com/documentation/en-us/unreal-engine/day-sequence-time-of-day-plugin-for-unreal-engine)).
A `DaySequenceActor` (use `ASunMoonDaySequenceActor` or the **Celestial Vault** preset for a
drop-in 24h cycle) drives the `DirectionalLight` sun/moon, `SkyAtmosphere`, `VolumetricCloud`,
and `SkyLight` from **one controller**, instead of disconnected Blueprint logic fighting each
other — this single-controller principle is the explicitly recommended best practice. **Weather**
sits *on top* as a separate state machine (Clear / Cloudy / Fog / Rain / Snow / Storm — same six
states as the Godot build). Drive all weather-varying parameters through a **Material Parameter
Collection** + a few scalar/vector blueprint variables (cloud coverage, fog density, fog scattering
color, sun intensity tint, wetness, wind vector), and **lerp** them over a ~20–30s transition on
state change (matching the Godot 30s tweens). The DaySequenceActor exposes `GetTimeOfDay`/day
duration so weather scheduling can be time-aware (e.g. fog at dawn). If you prefer no plugin
dependency, hand-roll the same: one "AtmosphereController" actor that on Tick writes sun rotation
+ all MPC params; weather state machine only sets the *targets* it lerps toward.

### (b) Lumen-friendly water material (Single Layer Water shading model)

Use the **Single Layer Water** shading model — a cost-effective, physically-based water surface
that does proper scattering/absorption/reflection in one depth layer
([official docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)).
Material setup: **Blend Mode = Opaque** (or Masked), **Shading Model = Single Layer Water**; the
node's four inputs are **Scattering Coefficients**, **Absorption Coefficients**, **PhaseG**
(forward/back light directionality), and **Color Scale Behind Water**. Surface look comes from
the standard **Normal** (feed the CC0 water normal panned at two speeds/scales for moving ripples,
plus optional Gerstner waves), **Roughness** (low for sharp reflections), and **Specular**.
**Lumen reflections DO support Single Layer Water, but they are forced to mirror reflections**
(verified — a known constraint; a planar reflection or screen-space fallback is the workaround if
mirror-only looks wrong on a large lake). Tie **Roughness/Specular and a wetness param to the
weather MPC** so the surface dulls in fog and sharpens after rain, and animate the normal pan
speed off the same **wind vector** the Godot system already uses. Keep it Opaque + single-layer
for performance on the voxel world; reserve full translucency for small/close water only.

### (c) Niagara rain/snow that follows the camera

Spawn precipitation from a Niagara system **attached to the player/camera**, not the whole map —
a single emitter parented to the camera (or to a pawn-following actor) with a **Box/Sphere
location module** sized to a volume *above and ahead* of the camera, so particles only ever exist
where they're visible. This is the standard community pattern and matches the Godot
camera-following `GPUParticles3D` already shipping. Use a **GPU sprite emitter** for volume (snow
especially — thousands of cheap particles), drive **spawn rate from the weather state** (0 in Clear
→ heavy in Storm, lerped over the transition), and add wind by feeding the shared wind vector into
a **Curl Noise / Vector Force**. For impact polish, enable **Niagara collision** (Analytical or
GPU depth-buffer collision) so raindrops **die on contact and spawn a splash sub-emitter** using a
Kenney splash sprite, and let snow accumulate logic hook into the same surface-wetness/snow channel
the Godot design deferred. Keep rain as **ribbon or stretched-sprite** for streak motion; snow as
soft round sprites with slow fall + lateral drift. Cull aggressively with the bounding volume so
cost stays flat regardless of world size.

**Docs / sources:**
- [Single Layer Water Shading Model — UE5 docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)
- [Lumen GI & Reflections — UE5 docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)
- [Day Sequence Time of Day Plugin — UE5 docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/day-sequence-time-of-day-plugin-for-unreal-engine)
- [Celestial Vault Plugin — UE5 docs](https://dev.epicgames.com/documentation/unreal-engine/celestial-vault-plugin-for-unreal-engine)

---

## 4. Suggested first download set (minimal, high-value)

If picking the smallest set that moves the needle, grab these six files:

1. `kloofendal_48d_partly_cloudy_puresky_4k.exr` — clear-day SkyLight (Poly Haven, CC0)
2. `kloppenheim_06_4k.hdr` — overcast SkyLight (Poly Haven, CC0)
3. `belfast_sunset_puresky_4k.exr` — dusk SkyLight (Poly Haven, CC0)
4. `rocky_terrain_02_nor_gl_4k.jpg` — water ripple + wet-rock detail normal (Poly Haven, CC0)
5. `kenney_particle-pack.zip` — rain splash / spark sprites for Niagara (Kenney, CC0)
6. `Rock023_2K-PNG.zip` — cliff/stone wet detail normal (ambientCG, CC0)

Everything else (sky, clouds, GI, fog, water material, particle simulation) is **procedural UE5**.

---

*All license claims verified against the live source pages on 2026-06-16. Re-check the asset page
license badge before shipping if revisiting later — these three sources are CC0-stable, but always
confirm.*

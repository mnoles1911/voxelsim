# Oblivion Remastered water as a reference for ours — what is public, what transfers, what to try

Date: 2026-09-08. Research agent, read-only. Owner directive, verbatim: *"i want our
river and lake water + surface effects and ripples to all look as close to
possible as Elder Scrolls Oblivion IV Remastered. Do open source internet
research and see if there is anything worth learning or calibrating our own
config values after."*

**The one-paragraph answer.** The remaster's water *numbers* (absorption,
scattering, roughness, normal tiling, wave size, foam thresholds, ripple
timing) are not public anywhere I could reach: nobody has posted a dump of its
water material's parameters, and the mod pages that could carry them need a
Nexus login for the files. What IS public, and solid, is the *architecture*:
the remaster renders water with Unreal's Single Layer Water shading model (the
same one we use), gets the look people praise from **Lumen reflections**
(software Lumen on consoles, hardware Lumen on PC — the latter is what puts
the trees, bridges and towers into the river), layers optional screen-space
reflections on top (the same SSR we run, and the thing its community turns
off because of edge artifacts), has the Water plugin loaded for swimming and
buoyancy, and draws its visible water on static-mesh planes with material
instances of a base material named `M_Water_River`. Its surface splashes are
particle effects, not a fluid simulation. So the honest calibration is not a
row of borrowed numbers; it is (a) three reflection-side settings we can flip
live, (b) the ripple scale the owner has already asked for, and (c) a short
list of mechanisms they have that we lack, each of which needs a generator or
C++ edit. Nothing here is a verdict on how anything looks; the owner judges.

Every claim below carries its URL and one of four evidence classes:
**DEV** = developer statement; **MEASURED** = a technical analysis with
captures; **SHIPPED** = a dump of data that ships with the game; **FOLK** =
community observation or mod-author wording. Pages I could not open are marked
**UNREACHABLE** and only their search-index snippet is used, labelled as such.

---

## 1. Sources

### 1.1 Shipped data (the strongest evidence available)

| # | URL | Type | Claim |
|---|---|---|---|
| S1 | https://github.com/nathtest/UProjOblivionRemastered | SHIPPED | UE 5.3.2 project reconstructed from the game's UE4SS reflection dump plus its cooked `Config/*.ini`. Everything in S2-S9 is read from this repository's raw files. |
| S2 | S1 `Config/DefaultEngine.ini` (lines 4-6, 30, 36, 37) | SHIPPED | `r.DynamicGlobalIlluminationMethod=1` (Lumen GI), `r.ReflectionMethod=1` (Lumen reflections), `r.Shadow.Virtual.Enable=1` (virtual shadow maps), `r.Lumen.TraceMeshSDFs=0`, `r.RayTracing=False`, `r.TSR.History.SeparateTranslucency=1`. (`r.RayTracing=False` sits oddly beside the in-game Hardware Lumen option; the runtime toggle must set it elsewhere — not resolved here, and not needed for water.) |
| S3 | S1 `Config/DefaultEngine.ini` (lines 253, 298-299, 518-519) | SHIPPED | Collision profile `WaterBodyCollision` with help text "Default Water Collision Profile (Created by Water Plugin)"; custom channels `VOWaterVolume` (overlap) and `VTWaterInteraction` (trace); a `[/Script/Water.WaterRuntimeSettings]` section. The Water plugin is live in the shipped game. |
| S4 | S1 `OblivionRemastered.uproject` | SHIPPED | Plugins enabled include `Water`, `NiagaraFluids`, `HoudiniNiagara`. |
| S5 | S1 `Config/DefaultScalability.ini` (lines 73, 77, 81, 106-158) | SHIPPED | `r.Water.SingleLayer.VSMFiltering=1` at Shadow quality 2, 3 and Cine; `sg.ReflectionQuality` 0/1/2/3/4 across the five presets. `VSMFiltering` only affects Single Layer Water surfaces, so this is the shipped-data proof that the water IS Single Layer Water. |
| S6 | S1 `Config/DefaultAltar.ini` (lines 109-114, 242, 251) | SHIPPED | `EnteringSplashSoundMinVelocityThreshold=650`, `ExitingSplashSoundMinVelocityThreshold=1000` (Unreal units/s), `ExitingFixedSplashDepth=0.15`, `WaterFollowerInnerRadius=100`, `WaterSoundFollowerOuterRadius=500`, `WaterPhysicalMaterial=/Game/Materials/PM_Water`, and `+NoLumenWaterReflectionMapsXSS=L_XPVitharn02` — a per-map opt-out list for **Lumen water reflections**, i.e. Lumen water reflection is the default and one Shivering Isles cell turns it off. |
| S7 | S1 `Source/Altar/Public/VWater.h` | SHIPPED | `AVWater : AVActor` carries one `UStaticMeshComponent`. The legacy cell water is a static-mesh plane actor, not a Water-plugin spline body. |
| S8 | S1 `Source/Altar/Public/TESWaterForm.h`, `WaterShaderData.h`, `BSShaderType.h` | SHIPPED | The Gamebryo water form survives (`Texture`, `ALPHA`, `Flags`, `FWaterShaderData WaterData`, `FString MaterialID`); `FWaterShaderData` is opaque in the dump (no reflected fields, so no values to read); `BSShaderType::SHADER_WATER = 17`. The water's colour/optics are addressed per water form through a `MaterialID` string, i.e. a UE material instance per legacy water type. |
| S9 | S1 `Source/Altar/Public/VBuoyancyComponent.h`, `VPhysicsControllerComponent.h` (lines 18, 61-64, 102, 123, 126) | SHIPPED | `UVBuoyancyComponent : UBuoyancyComponent` (the Water plugin's); the physics controller holds `TWeakObjectPtr<AWaterBody> InteractingWaterBody`, `float WaterLevel`, `RetrieveWaterBodyInfo(OverlappedActor, OtherActor)`, `FindWaterInteractiveComponents()`, `ClearWaterBodyInfo(...)`. Water-plugin `AWaterBody` actors exist in the world and are found by overlap; that is the swim/float side. |
| S10 | Local engine, `D:/UE_5.8/Engine/Source/Runtime/Renderer/Private/SingleLayerWaterRendering.cpp` lines 69-155, 367-373 | SHIPPED (engine) | `r.Water.SingleLayer.Reflection`: "0: Disabled, 1: Enabled (same as rest of scene), 2: Force Reflection Captures and Sky, 3: Force SSR". `ShouldRenderLumenReflectionsWater()` "only returns true if using the default reflections method and having Lumen enabled in the scene. It can't be forced with r.Water.SingleLayer.Reflection." Also `r.Water.SingleLayer.Reflection.Denoising` (default 0: "Adds some cost and makes reflections softer, but removes noise and flickering"), `r.Water.SingleLayer.SSRTAA` (default 1), `r.Water.SingleLayer.TiledComposite` (1), `r.Water.SingleLayer.VSMFiltering` (0). 5.8 is one major version past the remaster's 5.3, but these cvars and their meanings are unchanged. |
| S11 | Local engine, `D:/UE_5.8/Engine/Plugins/Experimental/Water/Source/Runtime/Public/GerstnerWaterWaves.h` lines 104-143, `WaterSplineMetadata.h` lines 38-41 | SHIPPED (engine) | Water-plugin Gerstner defaults: 16 waves, wavelength 521-6000 cm (falloff 2), amplitude 4-80 cm (falloff 2), spread 1325 deg, steepness 0.4 small / 0.2 large. River spline defaults: depth 150 cm, velocity 128 cm/s. **These are engine defaults, not the remaster's values**, and section 2.1 argues the remaster's visible surface probably does not use them. |
| S12 | https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine | DEV (Epic) | Pins: Scattering Coefficients ("rate at which light scatters on particles within a medium"), Absorption Coefficients ("how easily light penetrates the volume"), PhaseG, Color Scale Behind Water ("multiplies the luminance of the surfaces below the water"), Water Opacity. Reflections: "an indirect draw screen space reflections (SSR) pass" then a pass to "composite reflection captures, sky, and newly computed screen space reflections on top of the water's surface." |

### 1.2 Measured analyses

| # | URL | Type | Claim |
|---|---|---|---|
| M1 | https://www.eurogamer.net/digitalfoundry-2025-oblivion-remastered-is-one-of-the-worst-performing-pc-games-weve-ever-tested (read through the r.jina.ai text proxy; eurogamer.net itself refuses the fetcher) | MEASURED (Digital Foundry, 26 Apr 2025) | Turning off hardware-RT Lumen: "Water reflections are visibly poorer, while ambient shadowing and lighting is heavily downgraded." "software Lumen can look more like a screen-space effect." "stick with hardware Lumen, because the lighting it produces on vegetation is a lot better." Software Lumen "as used on the consoles" is ~35% faster. |
| M2 | https://wccftech.com/oblivion-remastered-is-an-impressive-remaster-with-dire-performance-problems-says-digital-foundry/ | MEASURED (second-hand DF) — **UNREACHABLE** (403), search snippet only | Same water-reflection quote as M1; adds that the game "uses screen-space reflections as a fallback, which creates artifacts where objects like weapons appear reflected in water despite being far away from it." |
| M3 | https://x.com/GeForce_JacobF/status/1914836994766897471 | MEASURED (NVIDIA staff comparison captures) — **UNREACHABLE** (402), title/snippet only | "Software Lumen RT vs Hardware Lumen RT in Elder Scrolls IV: Oblivion Remastered. Here you can see much more accurate water reflections with Hardware Lumen RT (many are missing with Software.)" |
| M4 | https://en.gamegpu.com/test-gpu/rpgrolevye/the-elder-scrolls-4-oblivion-remastered-obzor-i-sravnenie-graficheskikh-nastroek | MEASURED (settings comparison) — **UNREACHABLE** (403), snippet only | Hardware Lumen's "impact is most visible in water and reflections - Low performs efficiently, while Medium, High, and Ultra deliver similar results." |

### 1.3 What modders change (mod-author wording; files not downloadable without a login)

| # | URL | Type | Claim |
|---|---|---|---|
| X1 | https://www.nexusmods.com/oblivionremastered/mods/1761 (Enable Fog In Reflections) | FOLK (mod author) | "allows volumetric fog to be rendered in Lumen reflections - especially visible in water ... should apply to both software and hardware Lumen reflections. The effect may be less noticeable when using screen space reflections. Requires UE4SS, and ideally turning the screen space reflections setting off." The description does not name the cvar; the engine's fog-in-Lumen switch is `r.Lumen.HeightFog` (verified to exist in `LumenTracingUtils.cpp:24` locally). |
| X2 | https://www.nexusmods.com/oblivionremastered/mods/1842 (Accurate Reflections) | FOLK (mod author) | "enhances Lumen's ray-traced reflections. It extends ray tracing's culling distance, enables reflections for volumetric fog, and optionally adds Nanite support to all ray tracing ... REQUIRES Hardware Lumen to be enabled and screen space reflections disabled. It will not function with software Lumen." Presets "View Distance + Fog (2.5x)" and "Cinematic (2.5x)". Search snippet calls it a replacement for "the awful Screen Space Reflections option". |
| X3 | https://www.nexusmods.com/oblivionremastered/mods/1528 (Shaders Revised) | FOLK (mod author) | "Enabled Lumen reflections on transparent surfaces to fix broken reflections. Enabled fog in Lumen reflections to fix issues on water surfaces." Plus Lumen skylight and colour-grading changes. No cvar names in the description. |
| X4 | https://www.nexusmods.com/oblivionremastered/mods/35 (Ultimate Engine Tweaks) | FOLK (mod author) | Changelog lines: "Fixed occasional visual glitches near water"; "try changing r.Lumen.Reflections.Temporal=1 including its duplicate in [ConsoleVariables]"; "Reduced/Fixed Lumen 'boiling' artifacts and improved reflections when using DLSS 4.5". The search index for this and the Steam threads also attributes `r.Water.SingleLayer.Reflection=0` ("disables water reflections") to the tweak community; I could not find that line in the page text, so treat it as **snippet-only**. |
| X5 | https://www.nexusmods.com/oblivionremastered/mods/754 (Yet Another Engine.ini) | FOLK (mod author) | "Screen Space Reflections are on for me, personal prefference." No water cvars listed in the description. |
| X6 | https://www.nexusmods.com/oblivionremastered/mods/5572 (Ray Reconstruction Enable) — snippet only | FOLK | Recommends `r.Lumen.Reflections.DownsampleFactor=1` and warns other Engine.ini mods can cause "white dots in the ocean or wells". |
| X7 | https://www.nexusmods.com/oblivionremastered/mods/4874 (Placeable Water — modder's resource) | FOLK (mod author, working from the unpacked assets) | Ships "Large Waterfall Mesh — a vertically animated cascade", "Calm Water Planes", "Flowing Water Plane (Square) — adds directional current animation", "Waterfall Transition Plane". "Each mesh is animated, translucent, and designed to work with Material Instances that use the base material M_Water_River." The base material name is the only piece of the shipped water material anyone has published. |
| X8 | https://www.nexusmods.com/oblivionremastered/mods/5389 (Niben Flow) | FOLK (mod author) | "because Oblivion Remastered uses Unreal Engine 5 and is not like Oldblivion, we can't edit the landscape to make the river connect" — the Niben is two disconnected water bodies at Leyawiin. Rivers are placed pieces, not a routed network. |
| X9 | https://www.nexusmods.com/oblivionremastered/mods/47 (usmap for FModel), https://www.nexusmods.com/oblivionremastered/mods/3918 (retoc + UAssetGUI) | FOLK (tooling) | The route by which someone with the game installed could read the actual `M_Water_River` instance values: FModel + this usmap to browse, retoc to extract, UAssetGUI to read the scalar/vector parameters. Nobody has published the result. |

### 1.4 Community observations

| # | URL | Type | Claim |
|---|---|---|---|
| C1 | https://steamcommunity.com/app/2623190/discussions/0/604154270792549020 | FOLK | Weapon "reflected in water no matter how far away I am, even looking at the ocean from miles away"; replies: "This is a basic limitation of screen-space reflections"; workaround: disable SSR or use hardware Lumen. |
| C2 | https://steamcommunity.com/app/2623190/discussions/0/604154270792564648 | FOLK | Same artefact ("the shadow of the blade" in the lake). A reply posts `r.RayTracing.Nanite.Mode=1`, `r.RayTracing.Reflections.SamplesPerPixel=4`, `r.RayTracing.Shadows.RayCount=16`. |
| C3 | https://steamcommunity.com/app/2623190/discussions/0/604154444533234220/ | FOLK | "disable screen space reflections if you're using RT Lumen. It makes any movement or objects in front of reflective surfaces not show any artifacts." One reply: "I'm using RT Lumen and I am not getting any reflections when disabling SSR." |
| C4 | https://www.resetera.com/threads/the-elder-scrolls-iv-oblivion-remastered-pc-performance-thread.1170327/page-14 — **UNREACHABLE** (403), snippet only | FOLK | "water reflections seem broken, working in a cone spreading out from the middle of the screen, with the bottom left and right screen quadrants having no water reflections." |
| C5 | https://steamcommunity.com/app/2623190/discussions/0/604155112791766760/ | FOLK | Hardware Lumen "impact is most visible in water and reflections"; costs "about 10 FPS" over software Lumen. |
| C6 | https://steamcommunity.com/app/2623190/discussions/0/604154270792585002/ | FOLK | After swimming, "water droplets hover around you nonstop" until a reload — the swim splash is a particle effect attached to the character that sometimes fails to stop. |
| C7 | Steam thread snippets via search (e.g. https://steamcommunity.com/app/2623190/discussions/0/604154270792591322) | FOLK | "Puddles form when it starts raining." |

### 1.5 Not found / unreachable

- No Unreal Fest, GDC, Epic spotlight or interview in which Virtuos or Bethesda describes the water. The "behind the scenes" piece at https://www.tildee.com/behind-the-scenes-how-400-developers-from-virtuos-revamped-the-elder-scrolls-iv-oblivion-remastered/ carries no developer quotes on rendering (fetched and checked).
- No published dump of `M_Water_River` parameter values, wave amplitudes, foam thresholds or ripple timings. Search terms tried: `M_Water_River`, `MI_Water`, `/Game/Art/Water`, `WaterBodyRiver`, `Gerstner`, `SingleLayerWater`, `Niagara Fluids`, `FluidNinja`, `ripples`, `rain`, `water walking`.
- The original game's `Oblivion.ini [Water]` section (ripple scale/alpha/timing the Gamebryo half may still carry): https://stepmodifications.org/wiki/Guide:Oblivion_INI/Water and https://en.uesp.net/wiki/Oblivion:Oblivion.ini both **UNREACHABLE** (403). Not quoted from memory.
- The UE4SS dump has **zero** hits for `Ripple`, `Gerstner`, `Puddle`, `Wetness`, `Waterfall` in class or file names (GitHub code search over S1). Whatever draws ripples is Blueprint/Niagara/material content, which the reflection dump does not describe.

---

## 2. The three water classes, what is known about each

### 2.1 Rivers (flow direction, current)

- **What they are.** Placed water planes (`AVWater`, one static mesh each, S7) with material instances of `M_Water_River` (X7). The "Flowing Water Plane" asset "adds directional current animation" (X7): the current is an animated material on a flat mesh, i.e. scrolling normals/flow in the shader, not moving geometry. Rivers do not connect (X8); there is no routed network and no per-reach discharge.
- **Water plugin?** Loaded (S3, S4) and used for swim/float (`AWaterBody` overlaps, `UBuoyancyComponent`, S9). Whether the rendered surface is a Water-plugin water mesh (which would enable Gerstner displacement) or the `AVWater` static plane is **not resolved by any public source**. Two facts lean to the static plane: the modder's resource speaks only of meshes and `M_Water_River` instances (X7), and the legacy water form still addresses the look through a `MaterialID` string (S8). Treat "no Gerstner geometry on rivers, normal-map current only" as the best-supported inference, not a fact.
- **Shading model.** Single Layer Water: shipped scalability toggles `r.Water.SingleLayer.VSMFiltering` (S5), the per-map `NoLumenWaterReflectionMaps` list exists (S6), and the SLW reflection pipeline is the one modders fight with (X1-X4, C1-C4).
- **Numbers.** None public (absorption, scattering, colour behind water, roughness, tiling, flow speed).

### 2.2 Lakes (still, deep colour, reflections)

- **The look is Lumen reflections.** `r.ReflectionMethod=1` shipped (S2); DF: water reflections "visibly poorer" without hardware Lumen (M1); NVIDIA's comparison: many water reflections "missing with Software" Lumen (M3); settings comparison: hardware Lumen's "impact is most visible in water" (M4, C5). Lumen reflections on SLW are only produced when the scene's reflection method is Lumen and cannot be forced by the water cvar (S10). The water gets scene-lit reflections of terrain, buildings and trees with Lumen's roughness-driven softness — this is the single largest visual difference from our water, which has no Lumen and no reflection source containing terrain (docs/water-realism-analysis-2026-09-06.md section 3.3).
- **SSR is an add-on layer** exposed as a user toggle; its artefacts are the things the community complains about (C1-C4) and the things modders switch off (X1, X2). One user reports losing reflections entirely with SSR off (C3) — consistent with software Lumen missing many water reflections (M3).
- **Fog in reflections** is the most common community "fix" for water surfaces (X1, X2, X3): with it off, distant reflected terrain in a foggy scene reads too dark/crisp against the fogged direct view.
- **Numbers.** None public. What Epic's SLW pins do (S12) is what ours do; the remaster's values behind them are unknown.

### 2.3 Surface effects and ripples (player, objects, rain, wake, shore foam)

- **Interaction is trace/overlap driven on the logic side**: a dedicated `VTWaterInteraction` trace channel and `VOWaterVolume` overlap channel (S3), `FindWaterInteractiveComponents()` on the physics controller (S9), and splash thresholds in the shipped ini — entering splash sound above 650 uu/s (6.5 m/s), exiting above 1000 uu/s, a fixed 0.15 exit splash depth (S6).
- **The visible splash is a particle effect**: the persistent "water droplets hover around you" bug after swimming (C6) is a character-attached particle system that failed to stop. `NiagaraFluids` and `HoudiniNiagara` are enabled (S4). No public evidence of a height-field ripple simulation on the water surface, and no `Ripple` class in the dump (1.5). Best-supported inference: player/object ripples are Niagara/decal-style normal effects placed on the surface, with the original Gamebryo ripple logic deciding when. **Not public**: ring size, expansion speed, lifetime.
- **Rain**: "puddles form" (C7). No public evidence either way for rain rings on lake surfaces.
- **Wake**: no public evidence of a boat/swim wake beyond the splash particles.
- **Shore foam**: no public evidence of what it is keyed on. Since the surface is SLW on a plane over a heightmap, the only free signal is scene depth under the surface (depth fade); a distance-field or flow-map key is possible but unevidenced. **Not public.**

---

## 3. What does NOT transfer, stated before the table

| Their mechanism | Why it cannot be copied here |
|---|---|
| Lumen reflections on water (the headline look) | Lumen traces mesh distance fields / hardware RT geometry. Our terrain is voxels drawn by a ray marcher; it has no distance fields and no RT geometry (`ue-project/Config/DefaultEngine.ini:218-226` comment; realism doc section 3.3). `ShouldRenderLumenReflectionsWater` cannot be forced (S10). |
| `r.Water.SingleLayer.VSMFiltering=1` (S5) | Only does anything under Virtual Shadow Maps. The engine default is `r.Shadow.Virtual.Enable=0` (`VirtualShadowMapArray.cpp:161-162`) and our `DefaultEngine.ini` does not set it: we run classic shadow maps. |
| `r.Lumen.HeightFog` / fog in Lumen reflections (X1-X3) | Our SSR reflects fogged scene colour already; the sky term comes from the SkyLight capture whose fog handling is the R2 arm (realism doc). No Lumen, so the cvar is inert. |
| `r.TSR.History.SeparateTranslucency=1` (S2) | Only affects the separate-translucency history; we run `r.SeparateTranslucency=0`. |
| Water-plugin water bodies, Gerstner waves, river spline velocity (S11) | We have no water bodies: a baked bathymetry field, a lake sheet and an ocean sheet with our own 8-octave wave field (`water_wave_graph.py`). The plugin's numbers are engine defaults anyway, not the remaster's. |
| Static-mesh water planes, one material instance per legacy water type (S7, S8) | Our lake sheet is one material over a bathymetry field; there is no per-lake material instance. |
| Niagara splash/ripple particles (C6, S4) | Our ripples are a CPU height-field simulation at 10 cm texels in a 51.2 m window (`VoxelRippleField.cpp`), a physically different mechanism with different knobs. |
| SSR layered on top of Lumen | We have SSR only, so "turn SSR off" advice from their community is a non-starter here; it would leave sky + capture only (mode 2), which the owner has already seen. |

---

## 4. Calibration table

Columns: theirs = known value or best-supported inference (basis in brackets); ours = current default in the repo (file:line); proposal = an arm for the owner to judge, one line of reason. "not public" means exactly that.

### 4.1 Reflections (the class where their evidence is strongest)

| Theirs | Value / range | Our knob | Ours now | Proposal | Reason |
|---|---|---|---|---|---|
| Reflection method | Lumen (S2), hardware on PC = "more accurate water reflections" (M1, M3) | `r.ReflectionMethod` | 2 = SSR (`DefaultEngine.ini:665`) | keep 2 | Lumen impossible here (section 3). |
| Water reflection mode | 1 "same as scene" (engine default; S6 shows Lumen water reflection is on by default) | `r.Water.SingleLayer.Reflection` | 1 (`DefaultEngine.ini:226`) | keep 1 | Already matched. |
| Reflection softness | Lumen reflections are denoised and roughness-blurred; their SSR layer shows edge/cone artefacts (C3, C4) | `r.Water.SingleLayer.Reflection.Denoising` | 0 = engine default (not set by us) | **1** | The only live knob that softens SSR on SLW: "makes reflections softer, but removes noise and flickering" (S10). |
| SSR temporal stabilisation | `r.Lumen.Reflections.Temporal=1` is what a tweak mod restores (X4) | `r.Water.SingleLayer.SSRTAA` | 1 (engine default) | keep 1 | Already on. |
| Reflection quality preset | `sg.ReflectionQuality` 0-4 by preset (S5) | `sg.ReflectionQuality` | 3 (`DefaultEngine.ini:548-551`) | keep 3 | Same scale. |
| Reflection blur with distance | Lumen: roughness-driven; not public as a number | `WaterRoughnessFarGain` / `WaterRoughnessFar` / `FadeStartM` / `FadeEndM` | 0 (ramp OFF) / 0.30 / 60 m / 400 m (`create_water_voxel_material.py:2638-2641`) | **FarGain 1** | Turns on the built R3 ramp so distant SSR blurs instead of staying mirror-sharp at near roughness 0.08; the arm exists and has never been owner-judged on this water (realism doc R3). |
| Near-field roughness | not public | near constant | 0.08 (`create_water_voxel_material.py:2695-2696`) | no change | No basis to move it. |
| Specular | not public (SLW default F0 = specular 0.5 -> 4% at normal incidence, the physical value for water) | specular constant | 0.5 (`:2718-2719`) | no change | Already physical. |
| Fog in reflections | modders enable it for water (X1-X3) | none (SSR already fogged) | — | — | Does not transfer. |

### 4.2 Body colour and depth (lakes and rivers)

| Theirs | Value / range | Our knob | Ours now | Proposal | Reason |
|---|---|---|---|---|---|
| Absorption coefficients | **not public** (SLW pin, S12) | `water_optics.ABSORPTION_DISTANCE_M` 3.5, `ABSORPTION_COLOR` (0, 0.59, 0.74), `ABSORPTION_CHANNEL_SCALE` (2.0, 1.0, 1.5) => 2.236 / 0.458 / 0.437 per m; live `WaterAbsorbScaleR/G/B` | as listed (`water_optics.py:150-206`, `create_water_voxel_material.py:1454-1456`) | none from this research | Tonight's owner-directed ladder already covers this (tides plan, 2026-09-08 section); nothing public from the remaster to anchor a different number. |
| Scattering coefficients | **not public** | `SCATTERING_PER_M` (0.010, 0.067, 0.120); live `ShallowScatterBoost` 15, `ShallowTurbidityFloor` 0.7, `ShallowTurbidityDepthM` 1.5 | as listed (`water_optics.py:183`, `create_water_voxel_material.py:1567-1569`) | none from this research | Same. |
| Colour scale behind water | **not public** | `ColorScaleBehindWater` | (1, 1, 1) vector (`create_water_voxel_material.py:1503`) | none live (vector, not a `-VoxelWaterMatScalar` target) | Generator edit if ever wanted (section 6). |
| Phase G | **not public** | `PHASE_G` | 0.35 (`water_optics.py:211`) | no change | — |
| Per-lake water types | one material instance per legacy water form via `MaterialID` (S8) | none | one lake material | — | Section 6 (generator/C++). |

### 4.3 Waves (ambient surface motion)

| Theirs | Value / range | Our knob | Ours now | Proposal | Reason |
|---|---|---|---|---|---|
| Wave geometry | inference: none on rivers/lakes (flat animated planes, X7, S7); Gerstner engine defaults (S11) are 16 waves, 5.2-60 m, 4-80 cm — almost certainly not what a still lake plane uses | `WaveAmplitudeM` 0.25 (scaler; shipped Hs 6 cm), `WaveBaseWavelengthM` 5.0, 8 octaves at x1.42 down to 0.43 m, `WindRefSpeedMS` 5, `WaveWpoFraction` 0.25 | as listed (`water_wave_graph.py:657, 683-690`, `create_water_voxel_material.py:2884`) | no change from this research | Their wave numbers are not public; ours were owner-tuned on 2026-09-07 (`WaveQuantPerVoxel` 0 accepted). |
| River current | "directional current animation" in the material (X7); speed not public; plugin river default 1.28 m/s is an engine default (S11) | none | no flow term in the lake material | — | Section 6: a flow-map scroll keyed on carried discharge is a generator edit. |
| Whitecaps | not public | `FoamV2SlopeThresh` 0.08 / `Full` 0.16, `FoamV2WindMinMS` 3 / `FullMS` 9 | as listed (`water_wave_graph.py:573-600`) | no change | — |

### 4.4 Surface effects and ripples

| Theirs | Value / range | Our knob | Ours now | Proposal | Reason |
|---|---|---|---|---|---|
| Ripple mechanism | particle/decal normal effects on a plane, trace-triggered (S3, S9, C6); no height-field sim in evidence | CPU height-field sim, 10 cm texels, 512 x 512 window (51.2 m), fixed dt, Courant-limited (`VoxelRippleField.cpp`) | — | keep ours | Different mechanism; the visual targets below are what can be matched. |
| Ring expansion speed | **not public** | `voxel.Water.Ripple.SpeedMPS` | 1.6 (`VoxelRippleField.cpp:154`) | **0.9** | Finer rings travel slower: deep-water phase speed c = sqrt(g*lambda/2pi) is 0.88 m/s at a 0.5 m wavelength and 0.40 m/s at 0.1 m; 1.6 m/s is the speed of a 1.6 m wave. Matches the owner's "much smaller and finer" directive. Stay above ~0.8 or the wake outruns the field's damping window. |
| Ring lifetime | **not public** | `voxel.Water.Ripple.HalfLifeSec` | 5.0 (`:163`) | **2.0** | Shorter life = more, smaller, overlapping rings ("fine ripples in high quantity") instead of a few large rings to the horizon. Owner directive only; no remaster number. |
| Ripple visual gain | **not public** | `voxel.Water.Ripple.Gain` | 2.5 (`:114`) | hold, revisit after Speed/HalfLife | The gain multiplies whatever height survives; change it last. |
| Player entry ring | **not public** | `voxel.Water.Ripple.PlayerStrengthM` / `PlayerRadiusM` | 0.22 m / 0.9 m (`:215, :222`) | **0.12 / 0.5** | Same finer-ring direction for the player as for the boat; 0.22 m is 2.5x the ambient crest (`:217-219`). |
| Object entry ring | not public | `voxel.Water.Ripple.ObjectStrengthM` | 0.18 m (`:228`) | 0.10 | Same reason. |
| Boat wake | no evidence they have one | `voxel.Boat.WakeGain` / `WakeWidthScale`; authored `BowWakeWidthM` 0.55, `BowWakeStrengthM` 0.030, `TransomWakeWidthM` 0.95 | 3.0 / 1.0 (`VoxelBoat.cpp:68, 74`; `VoxelMovementTuning.h:387-389`) | **WakeWidthScale 0.5** | Halves the splat radius (0.55 -> 0.275 m bow) so the field carries sub-half-metre rings — the exact lever the cvar text was added for on 2026-09-08. |
| Wake foam | no evidence they have any | `DisturbanceFoamThreshold` / `Gain` / `HeightWeight` | 0.2 / 3 / 0 (regenerated tonight, `ripple_field_graph.py:359-382`) | 0.35 (already on tonight's ladder) | Crests only. No remaster basis. |
| Rain rings | no evidence either way (C7 is puddles on ground) | none exist | — | — | Section 6 (C++ + generator). |
| Shore foam key | **not public**; depth-fade is the only free signal on their setup | `BathyFoamWidthM` 6, `BathyFoamNoiseM` 0.9, `BathyFoamShelfLo/Hi` 0.05/0.25, `BathyFoamGain` 0.55, `ShallowFoamDepthM` 0.6, `ShallowFoamGain` (live 1, default 0), `FoamBreakupGain` (live 1, default 0) / `ScaleM` 0.6 / `DriftMPS` 0.15 / `Contrast` 2.2 | as listed (`create_water_voxel_material.py:1783-1940`) | no change | Owner accepted tonight's shore foam; nothing public to move it toward. |
| Splash thresholds | entering 6.5 m/s, exiting 10 m/s, fixed exit depth 0.15 (sound side, S6) | `VoxelRipple::kFullImpactSpeedMPS` (header constant) | not a cvar | — | Their number is for the splash *sound*; ours scales ripple height; not the same quantity. Recorded so nobody borrows it by mistake. |

---

## 5. Try-first ladder (live, no regen, no build; one change per arm; owner judges)

Baseline = tonight's regenerated defaults. Each line is the exact thing to set. `r.*` cvars go on the launch line as `-dpcvars=` or in the console; `voxel.*` are live console cvars; `-VoxelWaterMatScalar=` pairs are launch-time and echo `Lake sheets: material scalar '<Name>' set to`.

1. `r.Water.SingleLayer.Reflection.Denoising 1` — softer, non-flickering SSR on the sheet; the closest live approximation to the softness of their Lumen reflections (S10; C3/C4 for what raw SSR does at edges). Cost: "adds some cost" per the engine text; watch `renderMs`.
2. `-VoxelWaterMatScalar=WaterRoughnessFarGain:1` — enables the built distance-roughness ramp (0.08 near -> 0.30 at 60-400 m), blurring far reflections the way a roughness-driven reflector does; falsifier: `WaterRoughnessFarGain:0` is bit-identical to today.
3. `voxel.Boat.WakeWidthScale 0.5` — finer wake rings (owner directive; lever built for it).
4. `voxel.Water.Ripple.SpeedMPS 0.9` — finer rings travel slower (dispersion relation); to be judged WITH 3, since a halved ring at the old speed still spreads to the old size.
5. `voxel.Water.Ripple.HalfLifeSec 2.0` — rings die in seconds, so the surface carries many small ones rather than a few large ones (owner: "fine ripples in high quantity").
6. `voxel.Water.Ripple.PlayerStrengthM 0.12` + `voxel.Water.Ripple.PlayerRadiusM 0.5` — the player's entry ring on the same scale as the boat's.

Bounds worth knowing before an arm runs: the ripple sim's step is Courant-limited (`VoxelRippleField.cpp:666-669` prints `speed*dt/texel` against `kCourantLimit` at start-up), so lowering `SpeedMPS` is always safe; raising it is what can trip the limit. `Denoising` is a scalability-flagged cvar and may be reset by a saved scalability preset — confirm it took with `r.Water.SingleLayer.Reflection.Denoising` echoed in the console, per the flag-trap rule.

---

## 6. Changes that need a generator or C++ edit (not for tonight; listed so the owner can rank them)

1. **A reflection source that contains terrain.** This is the entire gap between their lake and ours, and no cvar closes it: Lumen is unavailable (section 3). The remaining routes are a planar reflection of the marcher (a second marcher pass, C++ + material), or a low-res cubemap re-rendered from the marcher near the camera (C++ + `water_sky_reflection_graph.py`). Both are real projects; neither has been costed.
2. **River current in the material.** Their rivers show a directional current animation (X7). Ours has none. A flow vector from the carried-discharge field scrolling the wave normal (`water_wave_graph.py` + a bathy-field channel in `bathy_field_graph.py`) is the generator route; the direction data already exists per the carried-discharge work.
3. **Rain rings.** Drop injection into the ripple field when the weather system reports rain (C++: `VoxelRippleField` already has a `Drop` path at `:255`), plus nothing on the material side. Unevidenced for the remaster; owner taste.
4. **Per-water-body colour.** They address a material instance per legacy water type (S8). Ours would be a bathy-field channel or a per-basin scalar set (generator + C++), only worth it if the owner wants swamp/mountain-lake/river to read differently.
5. **`ColorScaleBehindWater` as three live scalars.** Currently a vector constant (`create_water_voxel_material.py:1503`); exposing it is a two-line generator change and gives a live darkening of the bed that is independent of the extinction arms.
6. **Wake and ripple gradient into roughness.** Their reflections show ripples because Lumen reflects a perturbed normal; ours feed the ripple normal to `MP_NORMAL` only. Adding |ripple gradient| into roughness (`ripple_field_graph.py` + the roughness lerp at `:2697-2715`) would make the wake visible in the SSR term. Generator only, no C++.

---

## 7. Direct answers to the four questions

1. **Water plugin water bodies and Gerstner, or custom meshes?** Both halves exist: the Water plugin is enabled and its `AWaterBody` actors are what the character overlaps and floats in (S3, S4, S9). The visible water in the modding evidence is static-mesh planes (`AVWater`, S7) carrying `M_Water_River` material instances (X7), with the current as material animation. Whether the rendered surface is the plugin's water mesh with Gerstner displacement is not settled by any public source; the best-supported reading is flat planes with animated normals, no Gerstner geometry.
2. **Reflections.** Lumen reflections (`r.ReflectionMethod=1`, S2), hardware on PC and software on consoles; DF and NVIDIA both attribute the water's reflection quality to hardware Lumen (M1, M3). SSR is a separate user toggle layered on top; the community disables it under hardware Lumen because of screen-edge and foreground artefacts (C1-C4). Modders improve water by enabling fog in Lumen reflections, extending the RT culling radius, adding Nanite to RT, and restoring `r.Lumen.Reflections.Temporal` (X1-X4). No planar reflections in evidence.
3. **Player ripples / rain rings.** Trace- and overlap-triggered on the logic side (S3, S6, S9); the visible effect is a particle system attached to the character (C6), with `NiagaraFluids` available (S4). No public evidence of a fluid or height-field surface simulation, and no `Ripple` class in the dump. Rain produces ground puddles (C7); rain rings on water are unevidenced either way.
4. **Shore foam key.** Not public. On their setup (SLW plane over a heightmap) scene-depth fade is the only signal that comes for free; anything else is a guess.

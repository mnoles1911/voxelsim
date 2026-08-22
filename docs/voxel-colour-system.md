# The voxel colour system

What decides the colour of every voxel face in the game, where each piece lives,
and what is checked by what. Written 2026-08-22, replacing the open question at
the end of `docs/colour-system-handoff.md` §8.

Read [ADR-0008](adr/0008-flat-per-voxel-material-colour.md) for why every face
is one flat colour, and [ADR-0009](adr/0009-voxel-colour-system.md) for the
decisions this document describes. This is the map; those are the reasoning.

---

## 1. The pipeline

```
   ┌─ 1. BASE ───────────────────────────────────────────────────────┐
   │  kMaterialPalette[mat].face[faceClass]              sRGB → linear│
   │  materialpalette.h · 47 materials × 3 face classes               │
   └──────────────────────────────┬───────────────────────────────────┘
                                  │
   ┌─ 2. PLACE ───────────────────▼───────────────────────────────────┐
   │  lerp(base, climate, biomeTint[mat] × nearSurfaceFlag)            │
   │  climate = T_VoxelBiomeLUT sampled at (precipitation, temperature)│
   └──────────────────────────────┬───────────────────────────────────┘
                                  │
   ┌─ 3. VARIATION ───────────────▼───────────────────────────────────┐
   │  × (1 + light)   then  r × (1 + hue),  b × (1 − hue)              │
   │  light = jitter·hash(voxel) + patchStrength·noise(world/λ)        │
   │  materialcolor.h · vxc::voxelTint, vxc::applyTintQ16              │
   └──────────────────────────────┬───────────────────────────────────┘
                                  │
   ┌─ 4. SURFACE ─────────────────▼───────────────────────────────────┐
   │  wet-shore darkening, roughness      (bathy field, ADR-0008 era)  │
   └──────────────────────────────┬───────────────────────────────────┘
                                  │
   ┌─ 5. LIGHT ───────────────────▼───────────────────────────────────┐
   │  × corner AO × sun × sky × GI            the renderer's, not here │
   └───────────────────────────────────────────────────────────────────┘
```

**The order of 2 and 3 is load-bearing.** Applying the variation before the
climate blend lets the blend average it away, and every outdoor surface hands
190–235/255 of its colour to the climate. That is the difference between a
mottled hillside and a flat one, and it is why the base colour and the variation
travel to the material graph as separate interpolants instead of one finished
colour.

**Stage 5 is not in the table**, per ADR-0008 invariant 4. Baking a
top-is-brighter bias into the palette double-counts the renderer's own terms and
goes wrong the moment the sun moves.

---

## 2. Where each piece lives

| Piece | File | Kind |
|---|---|---|
| The table | `voxel-core/include/voxelcore/materialpalette.h` | **authored** |
| The evaluation | `voxel-core/include/voxelcore/materialcolor.h` | **authored** |
| The asset boundary | `core.h` `kFirstAssetMaterial` + the banner | authored, cross-checked |
| Shader copy | `ue-project/Shaders/VoxelMaterialPalette.ush` | generated |
| UE table copy | `ue-project/Tools/terrain_palette.py` | generated |
| Forge copy | `asset-forge/forge/palette.py` | generated |
| Shader → graph route | `VoxelQuadVertexFactory.ush` | authored |
| The composition, in nodes | `Tools/terrain_material_common.py` | authored |
| Detail-lattice cover | `VoxelDetailAssetSubsystem.cpp` | calls `voxelTint` |
| Forge preview | `asset-forge/forge/render.py` | mirrors `voxelTint` |

Two generators: `ue-project/Tools/gen_material_palette_ush.py` (the first two
copies) and `asset-forge/tools/gen_palette.py` (the third). **Never transcribe
the table. There is no fourth copy and there must not be one.**

---

## 3. The five columns, and what each is for

```c
Rgb     face[3];        // top / side / bottom, sRGB
uint8_t voxelJitter;    // near-field dither, 1/255ths
uint8_t voxelHue;       // warm/cool tilt, same units
uint8_t patchStrength;  // far-field mottle, same units
uint8_t patchScaleDm;   // its wavelength, DECIMETRES OF WORLD
uint8_t biomeTint;      // how much the climate owns, 1/255ths
```

- **Face class is for MATERIAL difference, not shading.** Grass is green on top
  and soil underneath; a cut trunk shows heartwood on its ends. Most materials
  use all three for the same colour and that is fine.
- **Jitter and hue are separate draws** from one hash, because correlated they
  would read as a single stronger jitter instead of as "uneven light" and "a mix
  of stuff".
- **Both frequencies are required.** Jitter alone averages back to its own mean
  once voxels fall below a pixel; the patch term is what survives. `--far` on
  the contact sheet is that averaging, drawn.
- **`patchScaleDm` is WORLD**, not voxels. Band-limited to two rendered cubes by
  `vxc::patchWavelengthMm`.
- **`biomeTint` is appearance, not policy.** See ADR-0009 §3.

---

## 4. What is checked, by what, and where it runs

| Check | Catches | Runs on |
|---|---|---|
| `static_assert` × 4 in `materialpalette.h` | a missing row; a tinted asset row; a varied watermark; patch strength with no wavelength | any build |
| `test_materialcolor.cpp` (15 tests) | bias, amplitude, coherence, band limit, salt, composition order, two materials collapsing onto each other | `ctest` |
| `gen_material_palette_ush.py --check` | a generated copy drifting from the header | anywhere |
| `check-palette-parity.py` | the shader's or the forge's **evaluation** drifting from `voxelTint`; the composition; the face-class mapping | Linux CI, no GPU |
| `check-terrain-graph.py` | the material graph failing to build at all | Linux CI |
| `palette-sheet.py` | nothing — it is how a human judges the numbers | Linux CI (artifact) |
| `compile-shaders.ps1` | the shader being ill-formed or undefined on either ADR-0001 target | the Windows box |
| a capture | everything above put together | the editor box |

### Two checks that were checking themselves

Worth knowing about, because both looked fine and neither was.

**The parity checker was a Python mirror of the shader.** It re-implemented the
`.ush`'s arithmetic and compared that against the C++. It passed a deliberately
corrupted hash constant and a deliberately reverted patch wavelength, because
the Python said what the shader was *supposed* to say. It now compiles the real
shader text as C++ against `tools/hlsl_cpp_shim.h`.

**The band-limit test inferred the limit from statistics** and set its bound at
two independent draws apart — which is what the *unlimited* version produces, so
removing the limit passed it. The limit is now a named function
(`patchWavelengthMm`) and is tested as itself.

Seventeen deliberate breakages were run against these guards when they were
written. Four passed and the guard was fixed. **If you add a guard here, break
it on purpose and show the failure.**

---

## 5. Where a change actually lands

Lopsided, and easy to get wrong.

- **40 materials own their colour outright** (`biomeTint` 0): every cave wall,
  every trunk, every leaf, every animal. A retune of these is what the player
  sees today.
- **7 climate-led surfaces** hand 190–235/255 to the biome LUT, so only 7–25% of
  a retune survives outdoors.

That is not the design's preference — it is the surface classifier. On real
diffusion tiles it labels very nearly every land surface voxel `MAT_SAND`
(`VoxelClimateProbe.h` has the measurement), so material id carries almost no
information outdoors. **Fixing the classifier is the single highest-value thing
anyone can do to this colour system**, and when it lands, `biomeTint` on those
seven rows is the first number to lower.

---

## 6. Boundaries — what this system does NOT colour

- **Water.** `M_VoxelWater` and the fluid renderer; depth-graded transmission,
  not albedo. A water quad's `mat` byte is a CA fill fraction, not a material id
  — the vertex factory passes 0 to the palette for exactly that reason.
- **Sky and the sun.** `M_VoxelSky`, the atmosphere dome, the starfield.
- **The 50 km clipmap vista.** It shares `build_terrain_base_color` but passes
  `palette=None`: a clipmap vertex is a heightmap sample with no material id to
  look one up with, so it stays climate-only. The two must not diverge at their
  seam, which is why the graph is shared code and not two copies.
- **UI.** `prepare_ui_assets.py` and the front end.

---

## 7. The traps, still true

Inherited from `docs/colour-system-handoff.md` §7 and still worth reading before
touching anything:

- **State which colour space you are in, every time.** The header is sRGB, the
  shader is linear, textures are imported with `srgb=True`. A tint is a
  reflectance scale and is meaningful in linear only.
- **The pooled path is what draws.** `voxel.Stream.GPU` is on by default. A
  change made only in `UVoxelChunkComponent`'s vertex builder is invisible —
  that mistake cost a whole session once.
- **One editor per box is a hard rule.** Two capture sessions on one machine
  killed each other's frames for hours and read exactly like a slow
  configuration.
- **Silent success is this project's signature failure.** Whatever you build,
  prove the check fires by breaking the thing it guards.

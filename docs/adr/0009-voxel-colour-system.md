# ADR-0009: The voxel colour system — one evaluation, and terrain joins it

- **Status:** accepted
- **Date:** 2026-08-22
- **Amends:** [ADR-0008](0008-flat-per-voxel-material-colour.md), which stands.
  Its four invariants are unchanged and this adds a fifth.
- **Doctrine sections affected:** none — additive. No new float in voxel-core
  (`materialcolor.h` is integer Q16 throughout), no change to `Material`
  ordering, no change to any on-disk format, no change to any worldgen hash.
- **Human sign-off:** Matt, 2026-08-22 — chose to retune the art values as well
  as the system, to weaken rather than remove the detail texture, and to land
  the material-graph half unrun.

## Context

ADR-0008 decided *what* a voxel face looks like and made
`vxc::kMaterialPalette` the one definition. A year of work later the table was
still single and two things around it were not.

**The evaluation had forked three ways.** ADR-0008 wrote its arithmetic down as
pseudocode in the "In-game implementation" section and left each consumer to
implement it:

| Consumer | jitter | hue | patch |
|---|---|---|---|
| `VoxelMaterialPalette.ush` | yes | yes | yes |
| `VoxelDetailAssetSubsystem.cpp` | at `0.35` of authored | **no** | **no** |
| `asset-forge/forge/render.py` | yes | at `0.6` strength | one median wavelength per grid |

None of the three looked wrong. A fern approved in the forge, the same fern
drawn as ground cover and the same species baked into terrain were three
different pictures of one row. The detail path is the one that mattered: it is
85% of all placements in the world (grass, ferns, flowers, reeds, small rocks —
266 species), and dropping the patch term removes the half of invariant 3 that
survives distance, so a hillside of cover flattened to one grey at exactly the
range where the variation was doing the most work.

**Terrain never got a colour at all.** The renderer wiring ADR-0008 called for
landed as `BaseColor = lerp(biomeAlbedo, paletteRGB, isAsset)`, deliberately
one-sided: `isAsset` is 0 for all sixteen terrain materials, so terrain kept the
climate path, which was the only appearance path that then worked. That was the
right first step and it was never taken further. What it cost:

- Every cave wall was `SubsurfaceColor`, **one flat constant** standing in for
  bedrock, rock, gravel, subsoil, mud and clay alike.
- Terrain had **no per-voxel variation anywhere**. The only thing varying a
  hillside was `T_VoxelDetail`, an 8 m-tiled fBm — eighty times coarser than a
  voxel and unrelated to the lattice under it, which is why hillsides read as
  soft blotches rather than as cubes.

The reference art (Lay of the Land) is built on exactly the two things missing:
per-voxel dither on *everything* including bare stone, and materials that read
as themselves.

## Decision

### 1. One evaluation: `voxelcore/materialcolor.h`

`vxc::voxelTint` and `vxc::applyTintQ16` are the definition. Every other
implementation is a transcription of them and is **checked against them**, not
trusted: `tools/check-palette-parity.py` compiles the shipped `.ush` as C++ and
imports asset-forge's mirror, and compares both to the C++ on the same inputs.
It needs no GPU and no editor, and CI runs it.

Integer Q16 throughout. Doctrine forces that (voxel-core has no floats), and the
result is worth having anyway: the tint is bit-identical on every compiler, so a
test can pin exact numbers rather than assert a tolerance and hope.

The colour hash is **not** a worldgen hash and must not become one. It is a
32-bit mixer with no `HashChannel` id, because colour is presentation — it
changes nothing about the solid set and no edit log depends on it — and because
a 64-bit splitmix chain per pixel is not free on a GPU.

### 2. The composition, in order

```
1. BASE       kMaterialPalette[mat].face[faceClass]         what it is
2. PLACE      lerp(base, climate, biomeTint/255)            where it is
3. VARIATION  x (1 + light);  r x (1+hue);  b x (1-hue)     which one it is
4. LIGHT      x corner AO x sun x sky x GI                  the renderer's
```

**Stage 3 comes after stage 2**, and that ordering is the whole reason the
renderer carries the base colour and the variation to the material graph
*separately* instead of handing over one finished colour. Applied before the
blend, the dither lands on the material's share only — and every outdoor surface
hands 190–235/255 of its colour to the climate, so it would be averaged away on
exactly the surfaces the player spends the whole game looking at. That is the
difference between a mottled hillside and a flat one.

Stage 4 stays out of the table, per ADR-0008 invariant 4.

### 3. `biomeTint`: a per-material weight, in the engine header

How much of a material's colour the climate owns, 0–255.

It was a boolean `BIOME_TINT` column in `ue-project/Tools/terrain_palette.py`,
defended there as UE-side policy with no counterpart in the engine. **That
argument is rejected.** Rock is the same grey in a rainforest and a tundra
because it is rock; grass is a different green in each because grass is a plant
that responds to climate. The difference is what the material *is*, and every
consumer needs the same answer — asset-forge's preview included, which had no
way to know and drew terrain materials at a saturation the game never shows.

A **weight** rather than a boolean is the point. A boolean forces the choice
between one flat tone underground and a hillside that ignores its climate. A
weight is the third option: subsurface strata at 0 keep their own colour, so a
cave wall gets its strata back, while surfaces stay climate-led.

The surface weights are high (190–235) and that is **not taste, it is the
classifier**. On real diffusion tiles voxel-core labels very nearly every
outdoor surface voxel `MAT_SAND` (`VoxelClimateProbe.h` records the
measurement), so material id carries almost no information outdoors and climate
carries all of it. A low weight now would not make the world more varied; it
would paint the landmass sand. This column is where the world gets its material
identity back when the classifier is fixed, and lowering it is a data change.

Evidence that this is a faithful promotion rather than a new set of opinions:
all 47 generated booleans came out identical to the hand-authored ones.

### 4. Invariant 5 — the patch wavelength is WORLD metric

`patchScaleVox` becomes `patchScaleDm`, in decimetres of world. The numbers are
unchanged (a level-0 voxel is 1 dm) and the meaning is not.

Counted in *rendered cubes*, one hillside's mottle had a 2 m wavelength in the
near streaming ring and a 64 m one two rings out, and the two met at the ring
boundary as a visible step **in the very term that exists to survive distance**.
The same bug made a 5 cm detail-lattice tuft carry a mottle at half the scale of
the terrain behind it.

`vxc::patchWavelengthMm` band-limits it to at least two rendered cubes: below
that a "patch" is one independent value per cube — a second jitter with none of
the coherence the term exists for, and one that aliases as the camera moves.
Stretched rather than dropped, so a coarse ring stays continuous with the fine
one it abuts.

### 5. Terrain reads the palette

`M_VoxelTerrain` composes stages 1–3 from `TexCoords[3]/[4]/[5]`.
`TexCoords[5]` costs no interpolant: Unreal packs customised UVs two per
`float4`, so the `float4` holding `[4]` already reserves the lanes `[5]` uses.

Zero had to stop being ambiguous. `M_VoxelTerrain` is also the material on the
component path, where no fourth or fifth UV is supplied and they arrive as zero
— and a raw `biomeTint` of 0 legitimately means "the material owns its colour",
which with an equally-unwritten base of `(0,0,0)` renders the world black. The
weight lane reserves 0 for ABSENT and sends real weights in its upper half, so
that path and `M_VoxelClipmap` (a heightmap sample with no material id) both
fall through to exactly the climate-only graph that shipped before.

`T_VoxelDetail` drops from 0.30/0.22 to 0.05/0.04. Most of what it was doing is
now done properly and the rest fights it — two variation systems at unrelated
scales read as noise, not as detail. The nodes stay, so "too clean" is a slider
rather than a material regeneration on a contended box.

## Consequences

**Easier.** Retuning the whole world is still one table, and now it reaches the
whole world rather than the assets only. A cave has strata. A hillside has
cubes. The forge shows what the game draws.

**Harder.** Three implementations of one formula have to be kept in step, which
is why the parity check exists and why it compiles the real shader text rather
than a mirror of it — the first version of that checker was a Python mirror and
**passed two of three deliberate breakages**, because it was checking itself.

**Unverified, and stated plainly.** The `M_VoxelTerrain` graph is a generated
`.uasset` and running the generator needs the editor box. The graph change is
written, executed against a recording stub (`tools/check-terrain-graph.py`), and
**not run**. What a capture must show is enumerated at the call site in
`create_voxel_material.py`.

**Must be revisited if:** the surface classifier is fixed, at which point the
190–235 surface weights are the first thing to lower; or if a material class
turns out to need pattern to be legible, which is ADR-0008's own trigger and
unchanged.

The colour values remain data, not doctrine. Changing them needs no ADR — it
needs `tools/palette-sheet.py` and somebody's eye.

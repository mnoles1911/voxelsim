# ADR-0008: One flat colour per voxel face, varied per voxel, from a single palette

- **Status:** accepted
- **Date:** 2026-08-11
- **Doctrine sections affected:** none — additive. No new float in voxel-core
  (`MaterialAppearance` is all `uint8_t`), no change to `Material` ordering, no
  change to any on-disk format.
- **Human sign-off:** Matt, 2026-08-11 (chose flat colour over the texture-array
  and procedural alternatives, with a reference screenshot as the spec).

## Context

Materials had no appearance defined anywhere in code. Colour was resolved in
the UE material graph from the `Mat` byte the quad decoder hands the shader, so
there was no table to point at and nothing that could be reviewed, tested or
kept in step.

Two things forced the decision now.

**The material set doubled.** ADR-adjacent work appended ten asset materials
(bark, heartwood, deadwood, six leaf types, pale bark), taking `kMaterialCount`
from 16 to 26. Every one of them arrived without an appearance.

**The renderer is moving to GPU ray marching for terrain AND assets, in one
volume.** That removes the constraint that would otherwise have driven this
decision. Greedy meshing merges up to eight voxels into one quad, which makes
per-voxel texturing awkward; a ray marcher hands back the exact voxel, face and
fractional hit position, so per-voxel addressing is free. It also removes the
second appearance path — there is one renderer reading one volume, so a bark
voxel and a cliff voxel are shaded by identical code.

Three options were considered:

1. **Flat colour per voxel**, varied between voxels.
2. **Texture array**, one tile per material per face class, tiled per voxel.
   Rejected on look, not cost: it puts a pattern *inside* a face, and at 5–10 cm
   a face is a few pixels across, so the pattern reads as noise. A texture
   array was still the right container over a packed 2D atlas — manual LOD in a
   ray marcher plus atlas gutters is the combination that bleeds between tiles
   at distance — but the container question is moot once no texture is sampled.
3. **World-space procedural evaluation** at the hit point. Rejected because it
   places detail at a *finer* scale than a voxel, which works against the cubic
   read the art direction is built on.

## Decision

**Every voxel face is a single flat colour. There is no texture and no
sub-voxel detail.** Colour comes from `vxc::kMaterialPalette`
(`voxel-core/include/voxelcore/materialpalette.h`), which is the one definition
of what a material looks like, for every consumer.

Four invariants future code can be checked against:

1. **One entry per `vxc::Material`.** Enforced by `static_assert` on the table
   size. A material added without an appearance is a build failure, not a black
   voxel found in a screenshot weeks later. This mirrors the tripwire that
   caught the material-ID gap, applied to colour before it can drift.

2. **The tint is keyed to the VOXEL, not the face.** All six faces of one cube
   share the hash of its integer position. Hashing per face gives a cube a
   different colour on its top than its side, which reads as six unrelated
   squares instead of one solid object — the opposite of what a cubic world
   trades on.

3. **Variation has two frequencies, and both are required.** Per-voxel jitter
   (`voxelJitter`, `voxelHue`) supplies the near-field dither. A slow patch term
   (`patchStrength`, `patchScaleVox`) supplies what survives distance. Per-voxel
   jitter alone is not enough: once voxels fall below a pixel it averages back
   to its own mean, so a varied hillside flattens to grey at exactly the range
   where variation is doing the most work. Anything that removes the patch term
   as a simplification is removing the half that carries.

4. **Face class is for MATERIAL difference, not shading.** `kFaceTop` /
   `kFaceSide` / `kFaceBottom` exist so grass can be green on top and soil
   underneath, and so a cut trunk shows heartwood on its ends and bark on its
   sides. Light direction and ambient occlusion are the renderer's business and
   are applied on top. Baking a top-is-brighter bias into the table would
   double-count them and go wrong the moment the sun moves.

## In-game implementation

Per hit, in the ray marcher:

```
mat        = volume material at the hit voxel        // uint8
faceClass  = faceClassOf(crossedAxis, normalPositive)
base       = kMaterialPalette[mat].face[faceClass]

h          = hash(voxelPositionInt)                  // NOT the face
tint       = lightness(h) * voxelJitter + hue(h) * voxelHue
patch      = noise(voxelPosition / patchScaleVox) * patchStrength

colour     = base * (1 + tint + patch)
colour    *= ambientOcclusion * lighting             // renderer's job, not the table's
```

The table is small enough (26 × ~15 bytes) to live in a constant buffer and be
uploaded once. Nothing per-frame reads it from CPU.

## Consequences

**Easier.** Retuning the whole world is editing one table — no re-export, no
texture authoring, no atlas repack. Zero texture memory and zero sampler cost
per hit. No mip selection problem, which a ray marcher would otherwise have to
solve by hand because hardware derivatives are discontinuous across voxel
edges. Adding a material is one row.

**Harder.** Anything wanting an authored, recognisable pattern on a surface is
now out of reach without revisiting this. Materials must be distinguishable by
colour and variation alone, which puts real pressure on the palette when two
materials are naturally similar (rock vs gravel, the six leaf types).

**Must be revisited if:** the voxel size grows enough that a face covers many
pixels, at which point flat faces will start to read as flat; or if a material
class turns out to need pattern to be legible. Foliage is the one to watch —
flat colour is most at risk there of reading as a solid green mass. It carries
the highest jitter in the table for that reason, and it depends on the air
voxels that asset-forge's thinning pass leaves inside every clump, so the
marcher sees daylight through a canopy. That was done for silhouette reasons
and happens to be what this renderer needs; it should be verified, not assumed.

**Open, and not decided here:** the palette must become the source for
asset-forge's preview colours (`forge/materials.py`), which is currently an
independent list. Two lists is the arrangement that produced the material-ID
drift. Until that generation step exists, what a designer approves in the forge
and what the game shows are not guaranteed to match.

The colour values themselves are a first pass aimed at naturalistic rather than
stylised, and are the part most likely to be wrong. They are data, not
doctrine — changing them needs no ADR.

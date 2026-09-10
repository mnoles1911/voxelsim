# Temperate tree appearance proposal — review required

Prepared 2026-09-07. This is an implementation proposal, not a shipped appearance update.

## Decisions received

- Leaf-only cutouts are permitted (user decision this session).
- Spring colors should be natural but clearly distinguishable (user decision).
- Keep 100 mm tree voxels and solid-color bark faces. No seasonal system.
- Once the appearance update is approved, apply it to all existing tree variants.
  Preserve geometry seeds, endorsement decisions and rejection decisions.

## What the project actually contains

ADR-0008 establishes one flat color per voxel face, position-keyed tint shared
between faces, both voxel jitter and slower patches, and a single engine palette.
The tint should not bake illumination into albedo. Leaf-only cutouts are an explicit
exception now permitted by the user; document that narrow exception when implemented.

The current header and generated Forge/HLSL palettes already have jitter, hue and
patch parameters. The older August handoff is historical: its claim that the UE
asset palette is missing must not be treated as current without checking source.
A census of all 49 seed-7 tree records found broadleaf green in 36 profiles,
needle green in 13, pale bark in only 3, blossom in 1, and generic bark in all 49.
The forest generator has a few pale/shaggy/fluted bark substitutions and pink
clusters, but no comprehensive species appearance profiles. Current birch and
beech shaded pilots were inspected again for this proposal.

Display parity is incomplete. Forge's interactive viewer normally maps material
IDs directly to a single RGB color and opaque cube shader; its optional authored
RGB trailer is an imported-appearance path. Offline thumbnails apply jitter/hue/
patches, but their noise/hue implementation differs from the HLSL version. UE detail
and prototype mesh palette helpers currently retain face color plus jitter, while
the terrain palette function supports hue and patches too. Matching base palettes
is therefore insufficient to claim matching appearance.

A September 6 cutout/wind prototype already exists in vegetation_material_common.py.
It uses binary discarded holes, two-sided surfaces, 4 cm mask cells grouped into
8 cm gaps for trees, and filtering at distance. It is an outer-shell approximation,
not a canopy filled with individual translucent leaves. I inspected its saved
opaque/cutout oak captures: openings increase, but the green shell remains visible.
These are historical demonstration geometry, not today's new 49-profile collection.
The implementation document explicitly excludes ordinary terrain-stamped trees.
Current production ownership work is still a separate integration dependency.

## Recommended design

### A species appearance catalog, with shared swatches

Use one versioned catalog entry per species: bark family and color range, mature
limb and twig roles, foliage family and base/fresh-growth colors, feature scale in
millimeters, deterministic appearance seed, cutout profile and approved revision.
The user's atlas is a reviewable swatch/pattern catalog; it need not be a bitmap
color texture on each cube face. Species share a bounded palette of useful colors,
but use different proportions and patterns. Do not allocate one gameplay material
per tree species, or infer physical behavior from its RGB.

For the first pilot, evaluate a bounded addition of approximately 24–40 shared
bark/foliage appearance swatches against the existing byte material budget. Bake
coherent swatch selection per voxel for the current terrain route. Explicitly map
each new ID to wood/leaf gameplay behavior and audit every hardcoded material-range
consumer (wind, shadows, mining, fire, collision, mip selection and export). Never
renumber old IDs or repurpose animal colors to disguise missing plant materials.

This is the lowest format-disruption candidate, not an unlimited long-term atlas.
If that palette budget cannot produce the intended art or leaves insufficient room
for understory, return a versioned appearance-channel proposal before changing the
VXA/world formats. A species shader uniform alone cannot identify trees after their
voxels have been flattened into the terrain material field.

### Pattern hierarchy

1. Small deterministic whole-tree variation within that species' range. Starting
   artistic bound: roughly 3–5% lightness, restrained warm/cool shift.
2. Coherent biological regions: older trunk vs thinner limbs/twigs; fresh foliage
   grouped by shoot, not randomly scattered pixels. Starting fresh-growth share
   10–20%, tuned by species, not interpreted as shadow or season simulation.
3. Bark features in branch-local coordinates: vertical fibers, broken plates,
   sparse horizontal lenticels, mottled smooth bark. Features follow branch axes;
   no global horizontal stripes through every limb. Minimum resolved marking is
   one 100 mm voxel; thin twigs cannot carry realistic tiny bark details.
4. Gentle cell variation on top, with multi-voxel patches that survive distance.
   Noise is stable in asset-local space and seeded independently of geometry.
   Moving, rotating, reloading or chopping a tree must not repaint surviving cells.

All visible faces stay flat-colored. Do not apply end-grain merely because a bark
voxel has an upward-facing staircase: actual cuts need explicit cut-surface semantics.
Nor does the underside of a leaf voxel necessarily represent a botanical leaf underside.

### Leaf-only visibility

Retain true holes between shoot groups for crown-scale openness. Add a binary
cutout mask to leaf surfaces for local detail; visible parts stay opaque. Start
with 15%, 25% and 35% mask-opening comparisons, and 40/80 mm clustered features,
then tune broadleaf, needles and sprays independently. These are test settings,
not promised screen-coverage percentages. Preserve dense needles where appropriate.
Avoid white-noise speckles, translucent green glass, or one universal perforation.

Match the visibility rule in color, depth, shadows and relevant reflections.
Reveal wood behind leaf surfaces; do not cull it as if its leaf neighbor were solid.
Do not indiscriminately emit every internal leaf face. Validate outer-shell artifacts
and several canopy layers against the sky. At distance, compare coverage-aware
filtering with the existing close-to-opaque fallback; avoid sudden canopy thickening,
LOD flicker and harsh shadow changes. Visual holes do not automatically alter
collision, mining selection or authoritative voxel occupancy.

Ordinary terrain-tree support is a release gate. Prefer the existing environment
render-ownership work if ready, with one visible owner per tree and consistent edits.
If that route is not ready, a terrain ray-traversal cutout pilot is a separate,
explicitly benchmarked alternative; never assume a mesh alpha-discard is sufficient
inside a volume marcher. No production rollout until this is resolved.

## Concrete pilot to approve

Eight species × seeds 1, 4 and 7 = 24 fixed models. Compare baseline, species color
only, and species color plus cutout in matched lighting/camera. No geometry reroll.
Include a mixed stand and 1.8 m pawn. Trees remain 100 mm.

| Species | Bark base | Foliage base / fresh | Pattern target |
|---|---|---|---|
| Temperate oak | `#70634F` | `#526F35` / `#81954C` | Irregular gray-brown ridges; grouped fresh tips |
| American beech | `#94968B` | `#638345` / `#96AC67` | Smooth cool-gray bark; quiet mottling |
| Birch | `#D0CDBB` | `#74934B` / `#A5B865` | Ivory bark with sparse horizontal dark scars |
| Sugar maple | `#797465` | `#587B39` / `#90A650` | Gray-brown plates; warm fresh greens |
| Scots pine | `#755740` | `#486D51` / `#7F965F` | Gray-brown base, warm upper bark; cool needles |
| Western red cedar | `#805C48` | `#416447` / `#728450` | Vertical reddish fibers; dark green sprays |
| Weeping willow | `#797762` | `#809351` / `#A8B573` | Muted olive-gray ridges; pale spring shoots |
| Cherry blossom | `#69574F` | `#668342` / `#94A55C` | Horizontal bark marks; pale pink blossom clusters |

These hex colors are provisional sRGB art-direction swatches, not calibrated color
measurements from photographs. Cherry includes a separate pale pink/ivory blossom
range. The pilot tests whether these differences remain natural in a mixed stand.
After visual approval, author and reference-check all 49 species profiles rather
than blindly applying these eight palettes to every species.

## Work sequence and acceptance gates

1. Unify appearance evaluation and color-space handling across Forge interactive,
   offline previews and game consumers. Add a neutral-light diagnostic comparison;
   identical lighting is needed before judging preview/game color differences.
2. Build the 24-model pilot in an isolated review location with versioned appearance
   manifests. Preserve geometry seeds, existing saved bytes and user decisions.
3. Record close (1–3 m), canopy (10–25 m), stand (50–100 m), and LOD-boundary views;
   daylight, overcast and backlit sky; static and moving camera; wind off first.
4. Measure render-thread/GPU time, memory, draw calls/overdraw or traversal steps,
   shadow cost and temporal stability on a fixed dense scene. Proposed pilot budget:
   no more than 10% additional GPU frame time versus the same opaque scene, subject
   to user's performance target. No performance claim until measured.
5. User approves or rejects color and cutout comparisons separately. Revise failing
   families, then author the remaining 41 profiles and audit all 49 in mixed stands.
6. Recolor existing variant geometry deterministically; no paid model calls for each
   seed and no geometry regeneration merely to alter color. Store appearance revision,
   source references, seed, palette/mask hashes and decision in the species library.
7. Apply the chosen endorsement migration policy, preserve rejected IDs, stage an
   exact versioned collection release, verify restart/export behavior, then publish
   only after the production rendering gate is met. The user has authorized applying
   the approved appearance update to existing variants; preserve rejected status and
   endorsed status while recording the migrated appearance revision.

This is appearance work, not a seasonal system, new wind design, or unrestricted
texture/normal-map rollout. It must not alter existing voxel/gameplay authority.

## Research and engineering basis

- [Minecraft Bedrock material instances](https://learn.microsoft.com/en-us/minecraft/creator/reference/content/blockreference/examples/blockcomponents/minecraftblock_material_instances?view=minecraft-bedrock-stable): alpha testing supports fully
  opaque or discarded pixels; its documented leaf mode becomes opaque far away.
  Separate foliage tint methods demonstrate a useful split between shape mask and
  color. This is Bedrock documentation, not a claim about every Java renderer.
- [Vintage Story render passes](https://apidocs.vintagestory.at/api/Vintagestory.API.Client.EnumChunkRenderPass): alpha-discard/no-blend rendering is distinct
  from its half-transparent OIT pass. That distinction supports testing cutouts
  instead of glass-like leaves.
- [Vintage Story block schema](https://apidocs.vintagestory.at/json-docs/jsondocs/Vintagestory.ServerMods.NoObf.BlockType.html): climate and seasonal color maps,
  alpha-render settings and side-occlusion behavior are separate controls. Borrow
  that separation; keep season mapping disabled for this spring-only slice.

Local sources: docs/adr/0008-flat-per-voxel-material-colour.md;
docs/colour-system-handoff.md; docs/vegetation-wind-and-cutout.md;
docs/HANDOVER-environment-2026-09-07.md; voxel-core/include/voxelcore/materialpalette.h;
ue-project/Shaders/VoxelMaterialPalette.ush; ue-project/Tools/vegetation_material_common.py;
ue-project/Source/VoxelEarth/VoxelDetailAssetSubsystem.cpp;
asset-forge/forge/render.py; asset-forge/forge/forest.py;
asset-forge/web/src/lib/viewer.ts.

## Approval requested

Approve or decline the 24-model appearance pilot above. Leaf cutouts and natural,
clearly distinguishable colors are already agreed directions. This approval would
authorize pilot implementation and measurement, not an unreviewed production-world migration. Once the resulting appearance update
is approved, applying it across existing variants is already authorized; no additional
blanket migration permission is needed. Preserve their approval/rejection decisions.

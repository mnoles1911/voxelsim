# Vegetation wind and cutout rendering

Implementation date: 2026-09-06. Source implementation, C++ build, revised
material generation and prototype gameplay capture checks are complete.
The production HISM ground-cover path compiled, but a populated ground-cover
field has not been visually verified. Ordinary terrain trees remain outside
this first pass. Offline numerical checks are not GPU performance tests.

## Renderer scope

The production `VoxelDetailAssetSubsystem` HISM renderer now supplies plant
motion metadata for grass, reeds, ferns, flowers and other detail vegetation.
The opt-in `VoxelEnvironmentLODPrototype` renderer supplies the same metadata
for the oak, bush and flower demonstration actors, across their existing LODs.
Stone materials remain rigid and opaque. No VXA contents, voxel resolutions,
placement rules, collision grids or gameplay material IDs change.

**Production trees embedded in the terrain lattice do not gain this motion.**
They pass through terrain rendering, not these instance/material paths. Their
follow-up needs separately rendered plant instances, root/branch metadata and
an explicit relationship to the authoritative terrain/edit lattice. Neither
the terrain renderer nor every production tree has been migrated here.

## One weather source, separate responses

Both plant materials read the existing `/Game/Voxel/MPC_VoxelSky` collection:

| Parameter | Interpretation |
| --- | --- |
| `WindVectorMS.xy` | Sustained air velocity in world X/Y, m/s; direction the air travels |
| `WindVectorMS.z` | Signed gust contribution, m/s |
| `WindFieldValid` | Published field validity |

These are the same wind fields used by water. The implementation does not
create another provider or rebuild the sky collection. Weather defaults to
enabled, with a 6 m/s base wind; this is a moderate breeze, not a guarantee of
constant light wind. Existing water-response filtering smooths the published
XY field over 600 seconds by default; the gust channel remains immediate.
This first integration inherits that prevailing direction and uses the gust
channel to adjust plant response. A later refinement can expose raw air wind
and water-filtered wind as two responses to the same weather source.

Oscillation uses `MaterialExpressionTime`, equivalent to pause-aware
`UWorld::GetTimeSeconds()`, as water does. It does not use the sky/calendar
clock, an independently accumulated clock, or water-specific `WaveTimeScale`.
Freezing the sun/calendar therefore does not freeze vegetation motion.

## Plant appearance and motion

Vertex alpha encodes material behavior, not opacity: 1 is inert, .75 wood,
.5 leaves, and .25 herbaceous material/petal appearance colors. UV1 contains
normalized height above the asset's root plane and asset height in cm. Vertex
RGB retains the existing sRGB packing and single decode in the material.

The shader applies coherent height-dependent bending and a smaller crosswind
flutter for foliage and soft stems. The root plane at local Z=0 stays fixed.
Direction and phase use world coordinates, so yawed instances still follow
the same wind. This is visual deformation; physics and mining selection still
use the undeformed authoritative lattice.

Leaves use masked, two-sided pixel-shaped perforations. Pixels are fully
opaque or discarded, not glass-like translucency. The pattern is anchored in
asset-local planar metres across edit sections and LODs; subpixel holes fade
to solid coverage to avoid distant shimmer. Existing LOD screen-door fading
multiplies this mask.

The first 1 cm pattern became solid under filtering in the 21 m oak capture,
making the opaque and cutout views almost identical. The revised mask uses
4 cm cells grouped into 8 cm gaps on trees, with a 36% clustered gap rate and
a small additional detail pattern. Small flowers scale from 1.25 cm cells;
asset height determines this scale, so changing voxel LOD does not re-seed it.
Filtering first removes the smaller speckle, then closes the larger gaps only
when those too become subpixel. The revised result retains visible openings in
the fixed-camera 21 m oak capture. Other viewing distances still need broader
evaluation. The mask changes shading only, with no added faces.

The mesh remains a **cutout outer shell**. Internal leaf-to-leaf faces remain
culled to avoid multiplying geometry and overdraw. Wood faces touching leaves
are restored so leaf holes can reveal enclosed limbs. The view can also reveal
the back shell; this is not individually modeled leaves filling the canopy.
Materials are two-sided, which also increases potential pixel work. Actual GPU
cost must be measured rather than inferred from the modest face-count change.

The shader's maximum displacement is sqrt(24² + 3²) = 24.19 cm. Prototype PMC
sections use `UVoxelPlantMeshComponent` to pad bounds by 30 cm independent of
section size. Runtime static meshes receive positive/negative 30 cm extensions
and recalculate their bounds before HISM creation.

Detached timber and the held stone axe explicitly set `WindEnabled=0` on their
material instances. Root-based deformation must not continue after transfer to
a moving physics body. Wind forces on detached bodies are a separate future
physics feature. A/B tooling should not override this exclusion globally.

## Controls and generation

`WindEnabled` and `FoliageCutout` are scalar material parameters, both default
to 1 and saturated by the shader. Set either to 0 for controlled comparison;
these names are material parameters, not standalone console commands.

Run `ue-project/Tools/create_detail_asset_material.py` and
`ue-project/Tools/create_environment_lod_material.py` in the appropriate UE
Python session. They share `vegetation_material_common.py` and update material
assets in place. Their existing sky collection bindings are checked by name.
Do not regenerate the sky collection to install this feature.
The detail generator explicitly enables `used_with_instanced_static_meshes`
so its HISM shader permutation is prepared before runtime.

## Validation and remaining coverage

`Saved/vegetation-geometry-audit.json` compares visible faces in the existing
prototype VXA files before/after restoring wood faces adjacent to foliage:

| Asset | Previous faces | New faces | Increase |
| --- | ---: | ---: | ---: |
| Oak, 50 mm | 468,732 | 488,807 | 4.28% |
| Oak, 100 mm | 117,144 | 121,871 | 4.04% |
| Bramble, 25 mm | 35,616 | 37,246 | 4.58% |
| Bramble, 50 mm | 8,260 | 8,796 | 6.49% |
| Bramble, 100 mm | 1,748 | 2,001 | 14.47% |
| Granite and daisy | unchanged | unchanged | 0% |

`Saved/vegetation-wind-numeric-audit.json` records an independent CPU check,
expressing directional displacement with complex vectors. It samples 200,000
combinations of 1–5,000 cm heights, 0–60 m/s wind, signed gusts from −30 to
30 m/s, height weights, world positions and times. All offsets were finite;
the largest sampled magnitude was 23.503 cm. Root displacement and calm-wind
displacement were exactly zero, including calm XY with a nonzero gust channel.
Opposite headings reversed the displacement exactly when compared at the same
phase anchor (world XY=0). At arbitrary locations, heading reversal also changes
the traveling-wave phase, so exact instantaneous negation is not expected.
The analytic envelope above covers states not reached by random sampling.

The final C++/UHT build passed (`Saved/build-vegetation.log`). Both revised
materials were generated and their saved shader maps validated
(`Saved/vegetation-materials.log`). The commandlet still returned exit code 1
with pre-existing project configuration errors; Python completion and material
validation markers passed. The runtime log contains no failed material compile.

The final fixed-camera run captured 20 images across oak, bramble, daisy and
granite: opaque/still, cutout/still, two eastward-wind frames and westward wind.
`tools/verify-vegetation-capture.py` passed, recording results in
`Saved/vegetation-capture-validation.json`. Each shot logged a valid shared wind
field with the expected 0 or ±6 m/s value. The run restored the previous weather
pins and returned control to the player. Images are in
`ue-project/Saved/Screenshots/Vegetation/`.

The revised cutout reduced green-pixel coverage by approximately 8.5% for the
oak and 8.9% for the bramble relative to their opaque controls. These are image
coverage measurements, not the mask's raw gap percentage. Visual inspection
confirmed canopy openings; the image checks also detected displacement between
opposite wind directions. Temporal antialiasing can affect those image metrics.
Root anchoring, calm response and the displacement envelope were checked
numerically as described above, rather than inferred from screenshots.

Launch the fixture with `tools/voxel-vegetation-prototype.ps1`; add `-Capture`
for the controlled comparison run. `ue-project/Tools/create_vegetation_materials.py`
generates and validates both materials in a UE Python session.

Remaining coverage includes populated production HISM ground cover, animated
LOD transitions, offscreen bounds behavior, chopping/falling regression with
these materials and GPU profiling. This is prototype acceptance, not validation
of every plant throughout the generated world.

## Primary references

- [Minecraft material instances](https://learn.microsoft.com/en-us/minecraft/creator/reference/content/blockreference/examples/blockcomponents/minecraftblock_material_instances?view=minecraft-bedrock-stable)
  documents `alpha_test` as fully opaque/transparent with backface culling
  disabled and includes a palm-leaf alpha-test example. This motivates masked
  foliage rather than translucent glass. No Minecraft artwork is copied.
- [Vintage Story wind modes](https://apidocs.vintagestory.at/api/Vintagestory.API.Common.EnumWindBitModeMask.html)
  describes separate leaf, weak-wind and tall-plant behavior, including wiggle
  and bending based on ground distance. This motivates per-material behavior
  and root-height weighting; this implementation is not a copy of its shader.

## Follow-up: ordinary terrain-tree migration

Simply adding an instance mesh would **draw the tree twice**. Current terrain
generation and rendering already include it at near and distant ring levels.
Filtering leaf/wood material IDs globally is also unsafe: it can erase placed
wood, fail to reveal terrain hidden behind the old tree, and lose edit intent.
Changing `terrainLattice` to false would remove the existing tree from
authoritative collision/digging as well as rendering; it is not a visual switch.

The current path is explicit:

- `voxel-core/include/voxelcore/generator.h`: `GeneratedWorld::makeBrick` and
  `materialAt` compose terrain-lattice assets into terrain AIR cells.
- `voxel-core/include/voxelcore/assetfield.h`: `instancesForRect(terrainOnly)`
  filters placement layers; `resolveForCompose` and `materialOfInstance` require
  terrain-lattice grids. `materialAtResolved` preserves first-non-air ownership
  among overlapping instances. Its resolved record currently drops species/bank
  identity, retaining grid pointer, anchor, yaw and layer.
- `voxel-core/include/voxelcore/world.h`: `World::materialAt` reads an edited
  overlay brick first, otherwise the composed generator. Full materialized
  overlay bricks also contain unchanged generated tree cells.
- `ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp`: near/coarse CPU
  samplers, `VoxelResolveTerrainInstances`, `ResolvedAssetsForFootprint`, GPU
  stamp dispatch and worklist composition all consume this tree data. Relevant
  GPU kernels are `ue-project/Shaders/VoxelAssetStamp.usf` and
  `VoxelWorklistAssetStamp.usf`. Terrain volume paths also feed marchers and
  their shadows, so changing only the visible triangle mesher is incomplete.
- `VoxelWorldSubsystem.cpp`: `TryDig`, grouped edits and `IsSolidAtVoxel` use
  the authoritative world. `VoxelDetailAssetSubsystem.cpp` currently refuses
  terrain-lattice layers/grids, so removing only that guard creates duplication.

Recommended staged implementation:

1. **Preserve instance identity.** Build a streamed tree presentation registry
   from the existing placement resolver and fine-tile residency gates. Use a
   deterministic identity containing world seed, placement/species identity,
   anchor and asset version; do not persist raw pointers or mutable table
   indices alone. Carry that identity through resolved composition. Reuse
   species/seed geometry and LOD meshes; instantiate actors only for edits or
   physics, not one actor per pristine forest tree.
2. **Separate render ownership from material authority.** Add an explicit
   presentation-composition policy with a pilot species/region gate. Keep
   `World::materialAt` unchanged for digging and collision. CPU render samplers,
   GPU asset-stamp/worklist paths and any visible/shadow volume must agree about
   which instance supplies geometry. A visual terrain query must compose base
   terrain, unmanaged assets and explicit edits while excluding only managed
   instance contributions. This requires edit/provenance handling; subtracting
   matching material IDs from an already composed overlay brick is insufficient.
3. **Make the swap atomic.** Retain the old terrain tree until its replacement
   meshes and the affected terrain chunks/volume updates are ready for the same
   revision. Then publish one ownership change. Reverse the process on eviction.
   Validate overlap, buried roots, ring boundaries, GPU/CPU parity and missing
   bank fallback. Never expose duplicate trees or a frame with neither owner.
4. **Drive edits from one authority.** Add an affected-instance notification to
   the existing grouped-edit publication path in `VoxelWorldSubsystem.cpp`.
   Translate authoritative edits into the instance's local occupancy and dirty
   only intersecting mesh sections plus their LOD parents, following the current
   prototype's incremental approach. New placed blocks remain terrain edits.
   Save/load, replica edits and detached-island promotion need the same revision
   and ownership transaction, so a felled tree cannot reappear after streaming.
5. **Use shared meshes for untouched trees and mutable meshes for changed ones.**
   Extract reusable LOD/section code from `VoxelEnvironmentLODPrototype.cpp`
   rather than promoting the debug actor wholesale. Align authored 25/50 mm
   visuals with the 100 mm gameplay lattice and define how a gameplay edit
   removes corresponding fine cells. Keep root-height metadata and wind/cutout
   materials from this implementation. Bound active fine meshes and uploads;
   the current oak prototype is roughly 489,000 faces at 50 mm and cannot be
   multiplied over an entire forest indiscriminately.
6. **Transfer severed ownership to physics.** Connect the instance edit state to
   felling/fracture, remove detached cells from authoritative rooted occupancy,
   disable root-based shader wind, and publish the detached body once. Apply the
   agreed sleep/despawn lifecycle separately. Validate collision/raycast
   agreement and persistence before expanding the species gate.

The first acceptance case should be one ordinary generated oak: identical
placement before/after, exactly one visible owner at every distance, unchanged
dig/collision results, persistent cuts after unload/reload, no duplicate
shadows, bounded transition/upload time, and correct felling ownership. These
steps are architecture recommendations; they have not been implemented here.

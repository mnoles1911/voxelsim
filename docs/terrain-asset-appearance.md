# Approved appearance in the terrain renderer

Status (September 9, 2026 UTC): sparse approved appearance is wired into the default terrain brick-page producers and marcher, with native tests for fine/coarse lookup, surface shading, edit provenance, GPU publication and detached debris. Full production traversal, quad-renderer parity and representative forest visual/performance acceptance remain incomplete. Unpublished assets still use ordinary material shading.

## Why attach appearance to pages

The desired result is approved species appearance on editable world voxels. It does not require replacing every stationary tree with an actor. A separate appearance sidecar on each existing terrain page preserves occupancy, material IDs, canonical placement, collision and the current edit stream. Geometry and appearance then share one page publication instead of a terrain-to-actor visibility transition. The inactive ownership transaction and composed actor importer remain useful for future moving-object handoffs.

## Data and publication contract

Canonical terrain takes precedence. Walk the complete ordered asset list and record the first non-air winner, even when that winner has no approved appearance. Do not fall through an unapproved winner to a later approved tree. Source handle zero means ordinary material shading. Static identity maps to a verified original VXA/VAC1 digest; keep anchor, original source frame and quarter yaw so face variation and cutout UVs match Asset Forge.

Prepare a sparse sidecar alongside the geometry for a newly reserved resident slot. Publish their matching allocation/generation together and retire them together. Chunk coordinates alone do not protect against stale remeshes or slot reuse. No spare bits in existing cells, brick descriptors or packed quads have been established as a safe source-identity extension.

A candidate representation is a brick directory, per-affected-brick occupancy masks and compact page-local source handles, plus an instance mapping table. VAC1 resources can be shared across placements and uploaded as sparse source-brick masks with packed material/RGB records. Exact memory limits and GPU packing remain to be finalized and tested; do not silently truncate winners.

## Rendering acceptance

- CPU and GPU production must create identical source ownership, including terrain precedence, overlaps, negative coordinates and canonical yaw.
- The main brick marcher must evaluate approved coverage at the actual source face and continue traversal when a leaf cutout rejects that face. Discarding the first opaque hit is insufficient.
- Shadow and GI traversals need matching foliage coverage; existing solid-brick shortcuts must not bypass it.
- The quad renderer needs per-pixel source lookup or equivalent merge constraints because material/AO greedy merging crosses source identities.
- Coarse pages need a tested representative/source-child rule consistent with their actual material selection.

## Edits, persistence and moving pieces

The edit log and its compactor preserve actually touched cells, which can distinguish untouched generated cells inside edited bricks. Explicit player replacement cells should not accidentally inherit the original tree's source appearance just because their material ID matches. Digging removes occupancy. Craft projection requires special care to preserve appearance on source cells that the fine edit did not touch; a whole edited-brick exclusion would be wrong.

Static source identity can be reconstructed from the authoritative world/catalog and actual edit state. Detached pieces need their source appearance supplied during extraction. The existing packed timber mesh already persists UV0, UV1 and color; verify the generic extraction path and network/save restoration rather than assuming those fields are populated.

## Current implementation work

A bounded core prerequisite is being implemented for canonical per-page appearance winners, with approved-source IDs and an explicit touched-cell predicate. This is not engine activation. The next steps are a verified sparse resource layout, paired page allocation/publication, CPU/GPU producer parity, main/shadow/GI/quad rendering, and edit/coarse/detach persistence checks. Full forest visual and performance acceptance remains required.


## Verified sparse lookup (2026-09-08)

The core canonical page generator and word packer now pass focused tests. The page payload uses a 16-word header, 64-entry brick directory, 17-word blocks (handle start plus 512-bit mask), eight words per page-local instance, and packed 16-bit handles. It carries the page key and generation; the pool still needs to publish/retire its allocation with geometry. It is not an on-disk format.

Verified VAC1 packets can now produce a cached sparse source resource: a 20-word header, sorted source-brick directory, occupancy masks and packed material/RGB records. Output and temporary allocation budgets are enforced. Source MD5, dimensions, origin, pitch and needle flag remain attached. No dense 3D appearance texture is allocated.

`Voxel.Appearance.SparseSource` and `Voxel.Appearance.SparseGpu` passed on D3D12 SM6 / RX 7800 XT. The GPU test checks every cell of a 32-cubed page under all four yaws, two overlapping source instances, terrain precedence, explicit touched cells, nonzero resource-buffer offset, source RGB/material/coordinates and truncated-range refusal. Report: `asset-forge/out/tree-runtime-appearance-v1/sparse-appearance-tests/index.json`, two successes and no warnings/failures. This verifies lookup, not full renderer shading or page publication.

Shared GPU face variation, source UVs and foliage coverage are the next implementation step. Ordinary world rendering remains unchanged until allocation, production and traversal integration are complete.


## Integration checkpoint — September 9, 2026 UTC

Earlier sections record the implementation sequence; the following supersedes their pending allocation and coarse-format statements. Geometry and appearance now publish together with key, level and generation validation. GPU state bounds page/source/range/slot storage, deduplicates sources and clears stale descriptors before replacement. Catalog entries require explicit endorsement and exact accepted bank bytes.

Level-zero pages retain header version 1. Levels 1–7 use version 2 and the same representative voxel selected by coarse geometry (`coarseCell * scale + scale / 2`). Anchors remain in finest terrain coordinates. Coarse shading preserves source-space color coordinates and physical leaf UV scale; existing footprint filtering fades fine cutouts with distance. Cover level 8 is excluded. Default CPU and GPU producers, including edited coarse pages, prepare this source ownership. The alternate recursive CPU coarse fallback remains unfinished.

Exact touched-cell tracking reads terrain and craft logs using the active world's 12.5 mm craft conversion. It suppresses appearance only at actual representative writes, preserving untouched neighbors in edited bricks. Generic detached pieces capture the original ordered source winner before removal and preserve colors, source coordinates, rotation and mask policy through restoration. Detail meshes support source pitches of 12.5, 25 and 100 mm. Generic packets remain opaque unless their approved policy explicitly enables foliage masks.

The consolidated `coarse-surface-build` succeeded and its second build reported up to date. `coarse-surface-tests/index.json` records six successes, zero failures and zero warnings: CoarseTerrainSurfaceGpu, TouchedPage, TerrainPagePreparation, TerrainGpuState, WorldDebrisCapture and CoarseSparseGpu. The coarse surface probe covers all seven coarse levels and four yaws; it exercises the integrated surface helper, not the full production main/shadow/GI traversal entry points.

Still required: quad renderer source lookup and coverage, production traversal acceptance, the alternate coarse fallback, remaining creature/cover renderer paths, and fresh full-world captures and performance measurements after these coarse changes. The earlier integration2 scene predates them and must not be cited as their acceptance. No unendorsed library assets were published by this work.


## Recursive and pooled rendering checkpoint — September 9, 2026 UTC

The pooled quad renderer now binds the same approved source pages and applies source color and cutout coverage through the actual terrain vertex factory. Add, update, removal, reuse and proxy recreation retain paired geometry/appearance ownership. The actual material is masked. GPU surface checks cover all eight levels, all quarter rotations, opaque and foliage policies, derivative coverage, source-coordinate offsets, and independently calculated pattern coordinates.

Recursive mip appearance is now wired into both pure worker generation and edited remeshing. The shared material reducer reports its contributing child without changing material or occupancy rules. The native builder revalidates cached parent material against its eight children and follows the selected child to a finest voxel. Stale edit epochs or inconsistent cached material refuse provenance. Version 3 appearance pages carry a signed XYZ offset per occupied record; versions 1/2 remain unchanged when offsets are zero. Native admission and GPU decoding validate the bounded layout. Edited recursive pages query sparse exact finest-cell write provenance, including projected craft writes, instead of only the center sampling lattice.

Evidence: `recursive-complete-tests/index.json` passes RecursiveMipTrace, QuadSurfaceGpu and actual QuadShadowPixels (the latter has pool diagnostic warnings). `recursive-majority-tests/index.json` passes the actual recursive builder with the alternative majority policy. `recursive-trace-tests/index.json` passes selected-child page preparation and exact touched-cell provenance. These reports live under `asset-forge/out/tree-runtime-appearance-v1/`. Raster RGB/depth acceptance is still pending a corrected capture-channel check; Unreal base-color capture writes alpha zero, which must not be compared to source-color alpha one.

The four-pass `approved-appearance-sequence1` full-game capture completed all edits and passed strict runtime and input-integrity gates. Visible-after GPU median is 16.496 ms versus 11.018 ms for the hidden-stand control; visible frame median is 19.271 ms. This is a 16-tree stand on terrain at 1280×720 output / 832×468 internal resolution, not isolated shader cost or a complete populated-biome acceptance. Its central approved stand is coherent; legacy background trees and sparse ground cover remain visible. This capture precedes the recursive producer and lazy terrain-preparation changes and does not validate their performance.

Remaining acceptance includes actual raster RGB/depth, production brick traversal coverage across main/shadow/GI, and an updated current-path gameplay capture. No pending variants were automatically endorsed or published.


Actual raster acceptance: `raster-rgb-tests/index.json` now passes QuadRasterPixels with pool diagnostic warnings and no errors. Bark: 4,096 samples, zero RGB/depth mismatches. Each of four leaf rotations: 2,592 samples, 590 covered and 2,002 open, zero RGB/depth mismatches. The test checks RGB at the original tolerance and separately asserts Unreal's base-color capture alpha zero. The source material is complete without fallback. Root inspected the resulting leaf image; red backstop is visible through open areas.

Read-only traversal audit confirms main hit shading resolves approved RGB and flat/hierarchical brick DDA both continue through cutout openings. The uniform-solid shortcut is disabled for potentially masked approved foliage. Shadow and sky-visibility GI call these same DDA functions. A direct production-DDA regression is being added to validate those calls with explicit expected hit voxels, rather than treating the call-chain audit as sufficient execution evidence.


Production traversal execution: `brick-traversal-tests/index.json` passes BrickTraversalGpu with no warnings or failures. Its 44 cases use actual pool admission and binding, then the production flat and hierarchical DDA functions. They check exact hit voxel, material, face and distance across mixed/uniform leaf geometry, generic opaque materials, missing appearance fallback, four rotations and coarse selected-child pages. Open rays reach a backstop in another brick. The expected occupied cells are explicit; mask evaluation uses the separately verified surface function. This closes the DDA execution gap, without claiming a full-frame GI-quality evaluation.

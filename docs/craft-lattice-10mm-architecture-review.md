# Review: extending world crafting from 100 mm to 10 mm

> Current pitch policy (6 September 2026): binary 100 / 50 / 25 / 12.5 mm; 12.5 mm is reserved for craftables and creatures. See [implementation notes](binary-voxel-pitches.md). Earlier minimum-pitch proposals below are historical.


6 September 2026. Source review and proposed architecture; no runtime implementation changed and no C++/UE tests run for this review.

**Recommendation:** retain 100 mm terrain and add an explicitly addressed 10 mm craft domain in locally promoted regions. Preserve per-entity asset pitch. Separate physical pitch, storage page size, terrain ownership, and rendering LOD rather than extending the current binary level number to mean all four.

Matt's request supersedes the old 25 mm floor for world crafting. It does not require reducing the terrain base grid to 10 mm or converting all existing assets. The new handheld set remains at 10 mm; the approved raft can retain its authored pitch.

## What is implemented today

- Base terrain is 100 mm: [core.h](../voxel-core/include/voxelcore/core.h:433). UE agrees: `VoxelSizeUU = 10.0` in [VoxelCoords.h](../ue-project/Source/VoxelEarth/VoxelCoords.h:23).
- Core world crafting is a **fixed 25 mm** overlay, not a general configurable hierarchy: [craftlattice.h](../voxel-core/include/voxelcore/craftlattice.h:80). It promotes one 800 mm terrain brick into 32³ cells, stored in 64 bricks of 8³ cells. Terrain projection uses two fixed 2× reductions.
- Core editing and persistence exist: [world.h](../voxel-core/include/voxelcore/world.h:193), [editlog.h](../voxel-core/include/voxelcore/editlog.h:85), and `test_craftlattice.cpp`, `test_craftpersist.cpp`, `test_craftcost.cpp`. Craft logs stamp their pitch; compaction preserves it.
- **The inspected UE source does not wire up this craft domain.** Searches found no `craftLattice`, `applyCraftEdit`, or `produceCraftChunk` integration there. Current chunk-index mapping exposes terrain rings plus cover, not a craft grid. [VoxelMarchChunkIndex.h](../ue-project/Source/VoxelEarthShaders/Public/VoxelMarchChunkIndex.h:260)
- The older architecture document is stale about GPU slot numbers: cover is now **level 8**, after terrain rings 0–7; no current `kCraftLevel` definition was found. Do not assign craft to slot 8 based on the old document.
- Spawned assets have a separate path. `VoxelAssetBody` reads `Grid.voxelSizeMm()` and converts to UE units: [VoxelAssetBody.cpp](../ue-project/Source/VoxelEarth/VoxelAssetBody.cpp:498). This supports the architectural choice of 10 mm item geometry without world-grid composition; it is not evidence that handheld equip/collision behavior is already finished.
- “Fine” in several UE names means elevation-tile streaming. Continue calling this system **craft detail** to avoid confusing it with terrain-height residency.

## Main findings

### 1. Changing `kCraftPitchMm` to 10 cannot work by itself

The current assertions require four craft cells per terrain voxel and exactly two binary reductions. At 10 mm the ratio is ten, which is not a power of two. An 800 mm ownership region becomes **80³**, not 32³, cells.

`kVoxelsPerCraftBrick = 8 / kCraftCellsPerVoxel` becomes **zero** under integer division at ratio ten. `expandCraftBrick()` would therefore need new addressing even if the assertions were removed. A fine brick may straddle a coarse-cell boundary; sample each source coordinate using `floor((localBrickOrigin + localCell) / ratio)`.

The relationship “one craft chunk = one terrain brick” also fails: a 32-cell GPU page at 10 mm spans 320 mm, while a terrain brick spans 800 mm. Changing a pool level ID cannot fix that geometry.

### 2. Terrain edits must be split across fine bricks

[world.h](../voxel-core/include/voxelcore/world.h:317) computes one destination craft-brick key from the terrain voxel's minimum corner, then puts every child cell into that bucket using modulo indexing. This is valid for the current aligned 4³ expansion.

At ratio ten, a terrain voxel spans 10 cells and can cross 8-cell brick boundaries on all three axes. Retaining the existing loop would wrap coordinates into the wrong brick and create duplicate cell indices. Compute the destination brick **for every affected child range**, group deterministically, and commit the entire edit as one transaction. A single terrain voxel can touch up to eight fine bricks with this aligned 10:8 mapping.

### 3. Rendering needs a craft context and ownership protocol

[VoxelBrickTraverse.ush](../ue-project/Shaders/VoxelBrickTraverse.ush:2275) uses shifts for terrain rings and a dedicated doubled origin for cover. There is no inspected craft equivalent. A 10 mm context needs integer address conversion and a camera/region-relative floating-point ray transform with explicit pitch.

Keep absolute addressing in integer millimetres or integer cell coordinates. Compute local coordinates before converting to float. Use floor division for negative positions. Compare hits in one shared world-distance parameter; audit boundary nudges, ray epsilons, depth output and the visibility-coordinate packing range at the smaller pitch. A nonbinary scale is feasible, but the old binary exactness argument no longer establishes correctness.

Promoted-region ownership must become visible only with a complete matching page generation. Preserve explicit all-air authority: a fully carved-away region is different from missing data. Publish its ownership record even when it needs no solid payload. On eviction, atomically switch to an intentional coarse fallback policy; do not accidentally reveal the pre-carve terrain. Avoid mixed generations between terrain projection, ownership mask and craft pages.

### 4. Coarse projection is insufficient for close interaction

[world.h](../voxel-core/include/voxelcore/world.h:399) sends collision, pathfinding, water and other coarse consumers through the projected terrain overlay. A majority reduction can erase a thin 10 mm object or fill a narrow opening. That is a coarse approximation, not exact collision truth.

Add a query layer: coarse terrain for broad phase, craft-cell DDA/shape queries for promoted regions near an interaction. Chisel selection must return domain, pitch, cell, face and generation. Character contact needs a deliberate fine-region collision strategy; rigid assets keep their own colliders. Navigation and fluid sealing should have separately specified approximation policies. Do not globally make every fluid or navigation cell 10 mm.

### 5. Existing 25 mm saves cannot be reinterpreted as 10 mm

`replayCraft()` rejects a log with the wrong pitch, correctly. Keep this guard. A 25 mm boundary is not generally on a 10 mm boundary, so conversion is not an exact subdivision. The two grids align every 50 mm, not at every cell boundary.

Preserve existing 25 mm regions as versioned legacy domains initially. Give each ownership region exactly one authoritative pitch. New regions may use 10 mm. If conversion is requested later, use a deterministic overlap-based migration with an explicit topology/volume policy, retain the original save, and record the conversion. Never silently round away old carvings. A temporary mathematical 5 mm common unit for overlap calculations does not require a stored 5 mm voxel tier.

## Proposed architecture

### Separate four concepts

| Concept | Suggested representation | Purpose |
|---|---|---|
| Address domain | Stable domain ID, integer pitch, origin, format/reducer version | Interpret edits and coordinates |
| Ownership region | Terrain-brick key, authoritative domain, generation | Decide which representation owns space |
| Storage | Sparse 8³ fine bricks with homogeneous compression | Reuse existing brick codec and limit allocation |
| Rendering | Page descriptors, valid bounds, index slot, LOD selection | Upload and draw without assuming page = ownership region |

An implementation might introduce `CraftDomainDesc`, `CraftRegion`, `CraftPageKey` and a shared integer coordinate helper. Names are illustrative. Retain 64-bit global coordinates and explicitly bound narrower GPU-relative coordinates. Include pitch/domain and reducer version in deterministic digests and handshakes.

### Keep local promotion, decouple packing

Retain the 800 mm terrain brick as the initial ownership unit. At 10 mm it contains 10³ fine bricks of 8³ cells. Avoid eager dense cell arrays: homogeneous bricks or a captured immutable base plus explicit deltas are suitable. If using a captured base, never fall back to the **mutable projected terrain**; that would create the circular dependency the current implementation correctly avoids.

For a first GPU implementation, reuse canonical 32³ page packing with **region-local pages**: an 80³ region needs 3³ pages, with valid extents 32, 32 and 16 along each axis. Clip traversal to those extents and the ownership region. Page keys include owner and local page coordinate, so adjacent owners never overlap accidentally. The last page's unused cells are padding, not world air.

This deliberately favors reuse and verifiability. It reserves up to 96³ page cell positions for 80³ valid cells: **72.8% extra slot capacity**, not necessarily 72.8% extra resident bytes because empty/uniform storage compresses. Measure it. A later variable-size page or independent brick index can remove this overhead; do not rewrite the proven material codec merely to introduce 10 mm pitch.

### Define reduction explicitly

Use a versioned deterministic **direct 10³-to-one reduction** for the 100 mm projection of new 10 mm regions. Preserve the old two-stage reduction for legacy 25 mm regions. Specify solid threshold, material tie-breaking and treatment of thin geometry separately from the collision query.

If a 50 mm visual proxy is useful, it is a 5³ reduction from 10 mm. Do not assume reducing 10 → 50 → 100 produces the same result as direct 10 → 100: intermediate majority votes discard information. Derived 50 mm data need not be another editable authority.

Exact 100, 50, 25 and 10 mm sizes cannot form one nested binary pyramid. Support them as explicit domains/derived views, rather than claiming every rung is half the preceding one. There is no need to expose every intermediate integer pitch as a user-facing edit mode.

### Version persistence and batch edits

The existing v3 log already carries a single pitch. It can remain useful for a single-domain stream; multiple authoritative pitches require a domain registry plus separate streams, or a new record format that identifies the domain. Specify ownership/promotion records, transaction boundaries, ordering and snapshot compaction. Do not merely add a pitch field that already exists.

Batch changes, project once per touched region, and enqueue only changed render pages. Propagate missing-brick and projection failures as failed transactions instead of publishing partially updated state. Maintain the existing invariants: terrain then craft replay, no coarse-log double counting, deterministic ordering, and explicit refusal for missing authoritative data.

## Cost and reach

| Per 800 mm ownership region | Existing 25 mm | Proposed 10 mm |
|---|---:|---:|
| Cells per axis | 32 | 80 |
| Logical cells | 32,768 | 512,000 |
| 8³ fine bricks | 64 | 1,000 |
| Uncompressed one-byte material cells | 32 KiB | 500 KiB |

This is **15.625× logical volume** for equal world extent; equally sampled surface area scales by 6.25×. These are geometric estimates, not measured frame time or packed memory. Preserve bounded promotion, byte budgets, batch edits, dirty-page uploads and distance-based proxies. Do not put a planet-wide 10 mm volume behind the feature.

## Implementation order and acceptance gates

1. **Domain contract and CPU addressing.** Add explicit pitch/ownership/page mapping. Test negative coordinates, 800 mm boundaries, 8-cell boundaries, first promotion, all-air authority and coarse edits that cross fine bricks.
2. **Projection and save compatibility.** Add the new reducer without changing old terrain mips. Test replay, compaction, mixed legacy/new regions, wrong-pitch refusal, failure atomicity, stable hashes and save upgrades. Add thin sheets, tunnels and mixed materials as adversarial cases.
3. **UE bridge and CPU-produced pages.** Wire authority edits, dirty regions, page generations and residency into the existing pool. Derive index IDs from current code, not historical slot numbers. Validate packed CPU pages against independent cell queries before GPU production.
4. **Traversal and interactions.** Add the craft context, ownership-aware terrain suppression, picking and collision refinement. Compare independent CPU/GPU rays at negative coordinates, page/region seams, long world offsets and grazing faces. Test complete carving, eviction, stale uploads and recreation.
5. **Performance and optional GPU production.** Extend the existing craft-cost tests and census to 10 mm, including dense mixed materials, stairs, thin walls, repeated edits and settlement-scale residency. Measure frame time, bytes, upload churn and query cost before choosing budgets or replacing canonical pages.

No full engine build or runtime benchmark was performed in this review. The recommendations are based on the checked source and existing tests, not a claim that the 10 mm world path is working today.

# Binary voxel pitches

6 September 2026. Supersedes the proposed 10 mm minimum.

## Authoring policy

| Category | Supported finest authored pitches |
|---|---|
| Craftables, creatures | 100, 50, 25, 12.5 mm |
| Environment detail | 100, 50, 25 mm |
| Terrain-stamped environment trees and rocks | 100 mm |

Terrain remains 100 mm. Terrain-stamped assets must match that lattice because
the current compositor places cells directly. Their category may be explicitly
changed to craftable for independently rendered items. Display LOD reductions
can be coarser than 100 mm; they do not create additional authoring tiers.

Asset Forge applies one shared eligibility policy to spec validation, generation
overrides, preview tiers, UI choices and imports. Unsupported legacy authoring
settings normalize to the next coarser supported tier with a warning. Imports
reject incompatible voxel grids instead of relabeling their physical size.

337 existing creature specs were migrated: 226 from 10 to 12.5 mm and 111 from
20 to 25 mm. Originals and the affected kept raven model are backed up under
`asset-forge/.backup/binary-pitches-2026-09-06/`; its migration report lists each
change. The kept raven was regenerated. All 11 kept VXA models use supported
pitches. The eight starter tool/component specs were rebuilt at 12.5 mm and
remain drafts pending visual approval. The approved bamboo raft remains 25 mm.

## Exact export scale

VXA v3 retains whole-millimetre pitches. VXA v4 uses the same header layout with
the pitch field in integer micrometres. VXM v2 retains whole-millimetre species
pitches; VXM v3 uses micrometres for every species record in that manifest.
Python and C++ readers accept both versions. Thus 12.5 mm is stored as 12500,
without rounding to 12 or 13. Existing rig joint positions retain their original
integer-millimetre representation. Consumers of the new files must use updated
readers; older readers will reject the new version.

## Core craft lattice

`World<8>` now selects three binary refinements, with 8 craft cells per terrain
cell. An owned 0.8 m terrain brick contains 64³ craft cells, represented by 512
8³ bricks. Promotion materializes the whole region, including authoritative air.
Projection performs three applications of the existing material downsampler.

The producer emits eight canonical 32³ pages for each owned region and refuses
an incomplete region as a whole. A renderer must publish those pages and their
terrain supersede state atomically. Legacy 25 mm APIs remain available through
explicit refinement 2 for compatibility tests and migration.

Edit-log v4 records micrometre pitch. Native replay and compaction preserve it.
Legacy 25 mm craft edits expand each authored cell to 2³ children when replayed
into a 12.5 mm world. This preserves geometry exactly. Coarse terrain edits route
to all 8³ craft children of the affected cell within promoted regions.

## Validation and remaining integration

Verified 6 September: all 832 C++ tests passed; the web production build,
pitchprobe, survivalprobe, category export audit and 840-spec hash audit passed.
The live browser editor showed all four tiers for craftables and 100 mm for
terrain-stamped trees. A fresh server with these changes runs locally on port
8732; the pre-existing server on 8731 was left untouched.

`asset-forge/tools/pitchprobe.py` checks category eligibility, preview and override
policy, all saved specs, kept VXA pitches, exact creature manifest scale and the
binary LOD ladder. `tools/survivalprobe.py` verifies deterministic generation,
connectivity, no repair bridges, health and exact VXA round trips for all eight
starter assets. The web production build verifies the TypeScript controls.

Core regression tests cover projection identity, negative coordinates, all-air
authority, all eight decoded pages, missing-page refusal, migration, compaction,
coarse edits and Python-generated VXA/VXM fixtures at exactly 12.5 mm.

This is the core and asset-format implementation, not completed world-renderer
integration. UE craft-page residency, atomic supersede publication, craft-specific
shader traversal and fine picking/collision still require integration. The asset
body loader now preserves fractional pitch, but its changes have not been
validated with a full Unreal build or in-game play test. Existing coarse collision
projection should not be presented as exact interaction with 12.5 mm features.

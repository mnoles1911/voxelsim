# Creature model refresh — 2026-09-06

382 wildlife species regenerated, plus five goblin mob models. All 387 are
saved in `library/<species>/<species>-<seed>/`, with VOX, VXA, thumbnails,
specifications, realized individuals and measurements. Wildlife uses seed 7;
goblins use seed 1. The existing saved raven variant was preserved as the
parameter source and refreshed. Original specifications and pre-existing
library files are backed up under `out/creature-refresh/originals/`.

## Review

- [Searchable gallery](../out/creature-refresh/index.html)
- [Five goblins](../out/goblin-review/goblin-lineup.png)
- [Machine report](creature-refresh-2026-09-06.json)
- [Goblin design and integration](goblin-mobs-2026-09-06.md)

## Changes

Wildlife retains its species-specific forms, palettes and physical dimensions.
The generators now draw eyes on the actual curved surface, with size following
head proportions. Quadrupeds gain inset ear color, nostrils and closed lip lines;
striped coats gain anatomical curvature and taper instead of perfectly straight
bars. Folded bird wings taper at shoulder and rump and gain subtle feather relief.
Fish gain closed-mouth and gill-cover detail, with separate slit patterns for
sharks and no side gills on cetaceans or rays. Targeted palette corrections cover
the raven, wolf, golden eagle and rainbow trout. Review thumbnails use a neutral
studio background to make dark coats visible.

Three discovered rig defects were repaired through voxel part ownership:
fennec fox ear roots had severed the skull's attachment, and tiny neck remnants
on wood thrush/yellowhammer did not touch their parent. All three now export real
face-contact joints. Their geometry and materials were preserved by that repair.

Goblins: raider (axe/shield), spearhunter (spear), stalker (paired blades), hexer
(staff), brute (heavy weapon/shield). Each uses 12.5 mm voxels and carries
articulated body, arm, hand, leg, foot and equipment parts.

## Resolution

368 wildlife models and all five goblins use the finest supported 12.5 mm tier.
The remaining 14 wildlife models use the finest supported tier whose realized
export envelope fits 16 million cells. This bounds the dense generator's large
temporary fields; it is a batch resource policy, not an engine restriction.
A pitch change can also change the seeded individual under the current hash
contract, so the budget measures each candidate pitch with its actual seed.

| Wildlife exception | Authored/export pitch |
|---|---|
| african-bush-elephant | 25 mm |
| basking-shark | 25 mm |
| blue-whale | 100 mm |
| fin-whale | 50 mm |
| great-white-shark | 25 mm |
| grey-whale | 50 mm |
| humpback-whale | 100 mm |
| long-finned-pilot-whale | 25 mm |
| minke-whale | 25 mm |
| north-atlantic-right-whale | 100 mm |
| orca | 25 mm |
| sei-whale | 50 mm |
| sperm-whale | 50 mm |
| whale-shark | 50 mm |

## Validation and limits

- All 387 saved assets pass scale, voxel count, material ID, complete part-tag,
  acyclic joint hierarchy, VOX chunk-size and VXA checks.
- 17 representative wildlife/goblin exports were rebuilt and compared exactly
  against saved material and part arrays.
- A curved-surface eye regression verifies symmetry and surface-only painting.
- All five goblins pass deterministic rebuilds, face connectivity and joint tests.
- The three repaired wildlife rigs also pass at seeds 1, 7 and 19.
- The web TypeScript/Vite build passes. The running service was restarted and
  its goblin controls and all 387 creature library records were verified.
- `export_categories.py --check` passes, with 387 creatures in the catalog.
- The general `selftest --quick` still fails on four unrelated existing plant
  tip-radius warnings: black-coral-tree, bramble-thicket, carnation-soft-coral,
  cold-water-coral. Its full-library environment build pass was not run.

These are voxel models with improved anatomy, not smooth photorealistic meshes.
Very small animals still have only a few cells across their identifying details.
The work supplies model assets and rig tags; runtime spawning/combat behavior
and animation clips are not implemented here. The existing creature/rendering
boundary is preserved. No curation verdicts were changed by the batch.

## Reproduce

From `asset-forge`:

```powershell
python tools/refresh_creatures.py --workers 2
python tools/goblinprobe.py --write
python tools/export_categories.py
python tools/creatureprobe.py
python tools/creature_review.py
```

The wildlife refresh resumes by specification hash plus generator source revision;
`--force` rebuilds every selected output. `--only <species> ...` limits the batch.

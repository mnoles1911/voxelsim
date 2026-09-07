# Environment voxel LOD pilot

Four isolated, opt-in game actors test finer source voxels while retaining the 100 mm terrain lattice. They are a temperate oak, granite boulder, bramble bush, and meadow flower from the current Asset Forge generators. These are experimental placements, not replacements for biome generation or the production asset bank.

| Asset | Near → far pitch | Solid voxels, finest → coarsest | Exposed faces, finest → coarsest |
|---|---|---:|---:|
| Oak | 50 → 100 mm | 977,547 → 152,585 | 468,732 → 117,144 |
| Granite | 50 → 100 mm | 14,928 → 2,146 | 5,110 → 1,344 |
| Bramble | 25 → 50 → 100 mm | 35,224 → 1,364 | 35,616 → 1,748 |
| Meadow flower | 25 → 50 → 100 mm | 660 → 52 | 1,468 → 128 |

Coarser grids derive from the finest source with aligned 2× reduction. Distance changes use hysteresis and a 0.45-second complementary masked transition. Initial thresholds are oak 25 m, granite 12 m, bramble 8 m, flower 3 m; these are experimental values, not tuned production settings.

## Gameplay contract

Oak and granite use one stable 100 mm collision grid in the game's actual custom character collision sampler. Bush and flower remain pass-through. Fine source-grid raycasts support carving; each edit regenerates all visual levels and the coarse collision grid. The source is independent of the visible level, so changing distance cannot restore removed voxels. Terrain in front of an actor occludes its interaction ray.

The mining selection defaults to a cube **300 mm on each side**, comprising 27 terrain cells. Keys **2 / 3 / 4** select **100 / 200 / 300 mm**, respectively. The gold outline displays the grid-aligned affected volume before clicking, including hidden edges. Terrain preview and editing use the same camera raycast and cube-anchor helper; the prototype preview and carving use the same actor hit selection and local grid anchoring. Existing placement shares the controller's selected size. This does not implement pickaxe animation, swing timing, tool durability, hardness, or resource drops.

## Running

After building VoxelEarthEditor, run `tools/voxel-environment-lod-prototype.ps1 -Capture -KeepOpen` to launch a visible standalone game, collect comparisons and interaction checks, and leave the player in the scene. The launcher refuses to start while another editor or compiler is active. Omit `-Capture` for manual exploration. Console `voxel.EnvironmentLOD.Reset` restores prototype source grids; `voxel.EnvironmentLOD.Spawn` creates another set ahead of the player.

Source assets and generation report: `asset-forge/out/environment-lod-prototype/`. Actual game captures: `ue-project/Saved/Screenshots/EnvironmentLOD/`. The test location is snowy mountain terrain; the species are temperate test objects and this is not a biome-placement approval.

## Findings and production limits

The follow-up game pass verified all four actor raycasts and carving through the world subsystem: removed source voxels were 154 oak, 77 granite, 1,044 bramble, and 89 flower. Every derived level stayed empty at the edited sample. Oak/granite collision changed from solid to empty there; bush/flower stayed pass-through. Oak walking stopped at X = −60.10 cm relative to the actor in both LODs. Granite walking ended at X = 9.90 cm and Z ≈ 90.1 cm in both LODs; this verifies identical behavior but is not evidence of a frontal collision stop. The overhead interaction search resolved the initial granite/flower probe misses.

The capture run then exposed a fixture cleanup crash: removing the timer destroyed the capture state before the free-exploration handoff finished reading it. Cleanup now happens after the handoff. The rebuilt follow-up completed all four interaction checks, logged `FREE EXPLORATION`, and remained running for direct UI and mining-size checks. Direct player left-click carving removed another 92 oak voxels with all LODs and collision empty at the edited sample.

Finer pitch improves silhouettes and small features, but does not fix generator art: the oak crown remains rounded and layered, and the current meadow flower needs more species-specific modeling before being described as an accurate daisy.

The first game pass exposed synchronous whole-asset meshing: approximately **3.5–4.2 seconds for the oak** on creation or carving. The [mining hitch fix](environment-mining-hitch-fix-2026-09-06.md) replaces per-strike full rebuilding with local procedural mesh sections and incremental coarse-grid updates; repeated oak edit samples dropped to roughly 1–2 ms in the validation run. Initial loading is still synchronous, and production population streaming needs cached assets and asynchronous initialization. Per-frame sample logs are not a forest-scale benchmark; CPU geometry byte estimates are not GPU memory measurements.

Prototype edits are transient and standalone only. Persistence, replication, felling, support/island simulation, gathering drops, streaming residency, and integration into the normal tree/rock population remain outside this pilot. These results support selective finer environment LODs, not bulk regeneration of all environment assets yet.

## Captured comparisons

Each pair uses the same camera; coarse is forced to 100 mm for comparison.

| Asset | Finest | 100 mm |
|---|---|---|
| Oak | [50 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/temperate-oak-fine.png) | [100 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/temperate-oak-coarse.png) |
| Granite | [50 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/granite-boulder-fine.png) | [100 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/granite-boulder-coarse.png) |
| Bramble | [25 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/bramble-thicket-fine.png) | [100 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/bramble-thicket-coarse.png) |
| Flower | [25 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/meadow-daisy-fine.png) | [100 mm](../ue-project/Saved/Screenshots/EnvironmentLOD/meadow-daisy-coarse.png) |

[Direct 300 mm mining result](../ue-project/Saved/Screenshots/EnvironmentLOD/mining-300mm-notch.png): a player click cut the visible trunk notch; the log recorded 78 removed 50 mm voxels, with every derived level and collision empty at the hit. The cube includes air outside the rounded trunk, so fewer than the maximum 216 fine cells were occupied.

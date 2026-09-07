# Local environment mining updates

The initial environment pilot rebuilt a complete oak StaticMesh and every coarser grid for each strike. This took roughly 3.5–4.2 seconds. Mining now updates only the affected source cells, their coarser parents, and intersecting mesh sections plus a one-cell neighbor border.

## Implementation

- Source voxel grids remain authoritative. Coarse-grid coordinates retain floor division and negative-origin alignment from the full reduction.
- Mesh sections read neighbors in the complete grid, so internal section boundaries do not create extra faces. Carving includes a one-cell border to expose faces in neighboring sections.
- Procedural vertex/index buffers replace per-strike StaticMesh/MeshDescription construction. Sections span 32 cells at 50/100 mm and 16 cells at 25 mm. These are render partitions, not a change in voxel size.
- Unaffected sections retain their geometry. Empty sections are removed, and newly exposed sections are created when needed.
- All sections of a visual LOD share its existing fade material. Visibility changes propagate through the LOD root. Color packing uses the same sRGB convention as the former StaticMesh path.
- Collision continues to use the fixed 100 mm grid. It updates from the same source edit and remains independent of visual LOD.

## Validation

The opt-in `-VoxelEnvironmentLODValidate` fixture performs repeated 100–400 mm cuts. After each cut it compares every derived voxel grid against a full reduction and checks the total section triangle count against a full source-grid mesh. These checks run after the edit timing measurement and deliberately do expensive full scans; never enable this flag for interactive performance judgment.

Both the initial section implementation and the procedural implementation passed these checks for the tested oak, rock, bush and flower. The first section version still spent 7–20 ms on oak strikes and up to 84 ms on bush strikes because it retained StaticMesh construction. Procedural buffers reduced the repeated oak samples to 1.30–1.91 ms, rock to 1.77 ms, and flower to 0.61 ms in that validation run. The final normal-mode capture verifies the color correction and smaller 25 mm sections without the slow oracle.

Final normal-mode 300 mm cuts, from `Saved/environment-lod-game-v3.log`:

| Asset | CPU edit/mesh preparation | Sections rebuilt | Faces rebuilt |
|---|---:|---:|---:|
| Oak | 1.667 ms | 3 | 4,038 |
| Granite | 2.010 ms | 2 | 5,157 |
| Bramble | 5.018 ms | 9 | 13,718 |
| Flower | 0.699 ms | 4 | 1,665 |

All four reported successful digging and the expected collision result. The normal run completed and returned to free exploration. The final screenshot was visually checked against the prior oak for silhouette and color; the initially darker procedural-color regression is fixed. The complete asset contains 585,876 faces across oak LODs, while this oak strike rebuilt only 4,038. Reference-run evidence is retained in `Saved/environment-lod-procedural-validation.log`; earlier StaticMesh section measurements are in `Saved/environment-lod-static-sections-validation.log`.

The capture run still logs stalls around screenshot readbacks and startup/streaming. It is not a clean whole-frame benchmark, and these edit times do not imply all game hitches are eliminated.

Timing is CPU edit/mesh preparation time, not an assertion about worst-case total frame time or forest-scale performance. Initial source loading and full mesh creation still happen synchronously when the experimental actors spawn; they are no longer repeated during mining. The measured procedural oak's initial rebuild was about 694 ms versus roughly 3.5 seconds before. Production population streaming still needs cached meshes/asynchronous initialization and a separate many-tree draw-call budget.

The main four-asset fixture now uses the player's 300 mm default for each system-level mining check. The reference stress loop still covers all supported 100–400 mm subsystem brush sizes.

## Reproduce

Build `VoxelEarthEditor`. Use `tools/voxel-environment-lod-prototype.ps1 -Capture -Validate` for correctness, then run `-Capture -KeepOpen` without `-Validate` to judge actual interaction. The launcher guards other editor/compiler sessions. Logs contain `EnvironmentLOD incremental` durations and `reductionReferenceMatch` results. No production bank, inventory UI, world persistence, or terrain renderer was changed by this fix.

# Environment population readiness

Current checkpoint: 2026-09-07, merged PR248 (`8e0fe8073c73016f127481a9152bc600bacd8f67`). Procedural population works, but streaming performance and catalog-wide fine LODs are not accepted. Broader destruction ownership, joint World transactions and live async capture integration are frozen for this milestone.

## Observed population

The repaired, isolated `asset-forge/out/population` library contains 444 environment species in the manifest, 439 with banks and 1,730 VXA files. Five large hero assets intentionally receive zero spacing-folded weights. Sorted present seed files are valid; seed numbers need not be contiguous. The primary library was not modified.

Trees and rocks currently use 100 mm voxels. Smaller vegetation mostly uses 50 mm, with some 100 mm bushes and grasses. The separate fine LOD prototypes do not establish catalog-wide 25/50 mm support.

`tools/voxel-environment-population.ps1` runs the normal biome placer with private user data and `-VoxelNoLoad`. The elevated temperate run installed 438 placeable species and reached 17,913 detail instances, 171 species/seed meshes, and zero bank misses. This is one biome sample, not evidence that every species appeared.

Receipts:

- Log: `D:/voxelsim/Saved/environment-temperate-population-overview.log`.
- Two reviewed captures: `ue-project/Saved/EnvironmentPopulation/a7fc01a3b79c4e519a3350e0e0ce613b/Saved/Screenshots/WindowsEditor/`.
- Library repair hashes and provenance: `ue-project/Saved/environment-population-repair/repair-receipt.json`.

The forest rendered, but terrain was visibly still filling behind vegetation. At the recorded population sample, 2,798 groups remained pending. Cold-fill frame measurements are not settled frame-rate acceptance. The first run recorded a 9.146-second outer-ring scan; nested fine tile decoding contributes to that cost.

## Critical path

1. Validate paired terrain bounds and bounded asset-resolution caching. Both are applied locally as of September 8. The core targets compile and both CTest targets pass: 852 individual checks, including the three bounds-parity cases and three cache cases (329.39 seconds total). Unreal compilation and runtime validation remain pending. Independent cache review found and corrected rectangle field names and inclusive high-edge admission counting. Core float, unity and frontend switch lints pass. Paired bounds removes duplicate raster traversal; it does not promise to eliminate all cold loading stalls.
2. Repeat the same population fixture, then a moving-camera run. Compare terrain completion, cold and settled timings, queue drainage and memory. Do not infer FPS improvement from isolated resolver timings.
3. Validate representative biome densities, slopes and all environment categories.
4. Extend the tested fine LOD pipeline beyond the four prototypes, with a catalog-wide memory and streaming budget.
5. Enable ordinary new-world population with explicit generation compatibility. Existing save headers do not pin asset enabled-state or catalog/bank content. Manifest-only identity is insufficient when bank bytes change. Keep current probes isolated until this is resolved.

The shared Unreal compiler/runtime slot belongs to the persistence task until explicit release. Avoid source changes during its compilation and preserve other editor sessions. Publication-blocked persistence descendants must not be incorporated through this task.

The population harness now accepts `-AssetResolveCacheOnly` for a matched comparison against the unchanged default. This switch launches no warm jobs. Re-run both configurations with the same camera and inputs; the September 7 baseline alone cannot isolate changes in machine load or disk cache state.

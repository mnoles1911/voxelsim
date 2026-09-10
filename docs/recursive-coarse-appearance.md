# Recursive coarse appearance checkpoint

The normal direct coarse path and the `VoxelCoarseGrid=0` brick-cache fallback both use `GeneratedWorld::makeCoarseBrick`: terrain uses the configured coarse surface rule and assets use the finest voxel at the coarse cell centre. The fallback now prepares the same approved appearance page, compares every eligible source material against the actual brick sampler, and refuses mismatches. Geometry, job generation, edit epoch, and existing publication remain unchanged.

The `VoxelCoarseMinLevel` bypass is different. `FCachedMipBuilder` recursively calls `downsampleBricks`, with a solidity threshold and either material-majority/lowest-ID tie breaking or topmost-solid-child selection. Its material is not generally the centre voxel's material. No approved appearance is attached in that arm; it remains incomplete. A same-material centre match alone would not establish which species supplied the recursively selected material.

Completing that arm requires:

- A deterministic reducer provenance rule identifying the contributing finest cell for the selected material, including ties, terrain winners, and unapproved earlier assets. The geometry reducer and appearance provenance must share that rule through every recursive level.
- A page format capable of per-cell source coordinates or an explicit offset from the current representative. Current versions1/2 retain an instance handle and recover only the centre representative; they cannot encode arbitrary selected child coordinates.
- Matching CPU/GPU decoding, original-source face hash and physical UV mapping, immutable geometry/page generation publication, and edited-cell provenance under the active craft lattice.
- Regressions for majority versus surface-preserve selection, same-material overlapping species, negative coordinates, all levels/yaws, edited children, and save/rebuild identity.

Source checkpoint: `VoxelDirectCoarseAppearance.h/.cpp`, the alternate CPU arm in `VoxelWorldSubsystem.cpp`, and native test `Voxel.Appearance.DirectCoarseFallback`. The test invokes actual `makeCoarseBrick` in both surface-preserve modes, compares appearance bytes to the canonical representative preparation, checks geometry unchanged, and rejects deliberately mismatched material selection. Build/run is coordinated by the parent task.

Correction after native regression: `makeCoarseBrick` is terrain-only. The direct fallback now wraps it in terrain-first ordered asset composition at each coarse representative; this restores missing asset geometry in that alternate path. Strict appearance/material validation remains intact, and a terrain-only sampler is explicitly refused by the test. Recursive reduction still requires its own provenance contract.

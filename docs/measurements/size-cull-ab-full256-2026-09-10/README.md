# Understory size-cull A/B, full 256 m ring, 2026-09-10 (captures 26 OFF / 27 ON)

> **CORRECTION, 2026-09-12.** Text below that calls 48 m "the shipping default"
> or "the default ring" is WRONG. `kDefaultRingMeters` is **256.0** in
> `VoxelDetailAssetSubsystem.cpp:512`, with no config override, and its own
> comment records that it was raised deliberately after the owner reported the
> slopes looking bare from the vista. 48 m was the *validation harness*
> parameter default, which every capture then passed on the command line — so
> runs at 48 m were measuring a configuration the game does not use. The
> harnesses now default to 256 to match. What this changes: the 256 m figures
> throughout are the SHIPPING configuration, not an experiment.



Replaces the invalidated 24/25 pair (runtime module changed during the ON run). Both
captures here carry passing fail-closed receipts (`run-validation.json`): same three
module hashes at start and end, same detail cache manifest, no competing UE/compiler
process, eight movement checks passed. `compare_size_culling_ecological_walks.py`
validated both receipts before comparing.

Binary: PR #254 head (merged main + repairs), coherent `voxel-build.ps1 -Verify`.
Cache: `temperate-authored-lods-full-2` (manifest 603c973e...), the first immutable
bake on the merged builder identity and regenerated M_VoxelDetailAsset; its geometry is
identical to `-full-1` (0 changed LOD models, same 4,157,030 last-LOD triangles).
Scene: private `full-forest-low-cover-1`, spawn -154740,-81476, 123,005 initial
understory instances, LOD on, predictive off, 1280x720 offscreen at internal 832x468.

Medians (OFF -> ON):

| phase       | GPUTime ms      | FrameTime ms    | GameThreadTime ms |
|-------------|-----------------|-----------------|-------------------|
| WalkForward | 42.4 -> 21.6    | 43.4 -> 38.5    | 40.2 -> 39.0      |
| SprintGate  | 39.2 -> 20.4    | 51.9 -> 54.5    | 51.3 -> 53.7      |

Reading: size culling roughly halves GPU time at 256 m (p95 and max move the same way).
Frame time does not follow because both arms are game-thread bound (GT median 40-54 ms,
p95 300+ ms from streaming hitches); ON is within noise on the game thread. Scope is one
matched pair, draw-distance policy only: not residency savings, not popping acceptance,
not the 48 m default ring.

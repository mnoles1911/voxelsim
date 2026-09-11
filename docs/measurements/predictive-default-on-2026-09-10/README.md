# Predictive prewarm made the default for ecology worlds, 2026-09-10 (captures 32 / 33)

Owner decision after the flag-only A/B in `../predictive-resolve-ab-2026-09-10/`.
`PredictiveAssetResolveEnabled()` now follows `AsyncAssetResolveEnabled()`'s shape: on
whenever a non-empty `-VoxelEcologyConfig=` is supplied, off for legacy worlds, with
`-VoxelNoPredictiveAssetResolve` as the control arm's off switch. `-VoxelPredictiveAssetResolve`
still forces it on.

**This pair exists to prove the default engages and that the off switch really turns it off.**
A flag that does nothing while producing plausible numbers is this project's recurring failure,
so both directions are pinned, not just the one we want. Both receipts passed with identical
module hashes and the same cache (`temperate-authored-lods-full-4`, identity re-verified against
source before the run).

| | capture 32, no flag at all | capture 33, `-VoxelNoPredictiveAssetResolve` |
|---|---|---|
| predictive windows | 38 | 0 |
| warm resolves launched / landed | 6,310 / 6,266 | 0 / 0 |
| admission calls, final window | 18, of which 16 hits, 2 cold | 173, of which 0 hits, 173 cold |
| admission inline time, final window | 13.6 ms | 600.3 ms |
| R0 entry total (16 entries) | 591 ms | 1,685 ms |
| R0 Resolve total | 490 ms | 1,638 ms |

The mechanism is unambiguous and now measured twice (30/31 and 32/33): prewarming moves the
per-footprint asset resolve off the game thread, and the cold resolve that dominated the R0 entry
profile drops by about 70%.

**Where the numbers are noisy, stated plainly.** Frame-level phase figures are one pair each and
move both ways. Walking p95 frame time is much better with prewarm on (114 ms against 273 ms), as
is jump-tap p95 (76 against 143) and sprint p95 (264 against 343); but sprint medians are ~4 ms
*worse* with it on and jump-apex p95 is worse (98 against 63). Medians overall barely move, because
the ~40 ms GPU floor at this ring is untouched by a game-thread change. There is also a real small
cost while parked: stream tick median 2.15 ms with prewarm on against 0.07 ms off, which is the warm
queue draining with nothing to do. The decision rests on the resolve mechanism and the walking
hitch tails, not on the noisier per-phase figures.

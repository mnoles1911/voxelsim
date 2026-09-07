# Asset composition diagnostic startup hitch

The September 7 real-terrain candidate run (`Saved/environment-real-candidate.log`)
spent approximately 177 seconds inside an automatic, synchronous asset diagnostic.
The species histogram logged at 06:07:45.421 UTC, the column scan finished at
06:10:34.539, and the crown scan finished at 06:10:42.408. The measured subsystem
tick was 177,041 ms. This aligns with the diagnostic's 169.118-second column scan
and subsequent 7.869-second crown scan. Derived-data-cache maintenance overlapped
the stall; the log does not establish it as the cause.

`FVoxelWorldImpl::MaybeLogCounters` previously ran this probe automatically after
asset bank traffic appeared. It synchronously sampled 1,089 columns at 250 heights
(272,250 canonical `materialAt` queries), then scanned crown reach. Those calls
belong to a composition investigation, not routine gameplay telemetry.

The probe now defaults off. Cheap bank, species, and streaming counters remain
enabled. To explicitly run the same diagnostic, set `voxel.Assets.CompositionProbe 1`
in the console. It runs once per process at the next periodic asset-stat log with
bank traffic, and reports its start and elapsed seconds. **It can freeze the game
for minutes**; use it only to investigate solid voxels, internal air gaps, and crown
extent, never during performance captures. Setting it back to zero before it
starts prevents the scan; it cannot interrupt an already-running synchronous scan.

This change removes the demonstrated automatic diagnostic stall. It does not
change canonical asset placement, source identity, terrain generation, rendering,
or candidate preparation, and does not establish that other rendering/streaming
hitches are fixed. The original run never reached settled gameplay and its queued
prepare and quit commands fired together after the stall, so it was not a
successful candidate-preparation test.

The rebuilt retry `Saved/environment-real-candidate-fixed.log` completed with
normal telemetry enabled and no `assets PROBE` execution. The same real catalog,
seed and site reached `PREPARED HIDDEN` for `rhododendron-thicket` in 443ms across
ticks (06:27:25.707–06:27:26.150 UTC), and exited0 normally. Ordinary frame
hitches remain visible in the log; this proves removal of the automatic
diagnostic and successful preparation, not overall frame-rate performance.

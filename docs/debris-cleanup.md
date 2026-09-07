# Detached debris cleanup

> Integration update (2026-09-06): stable-ID v4 saves, asynchronous object loading, bounded actor residency, and server-authoritative object replication are now implemented for the current detached actors and environment prototypes. See [detached-object-integration.md](detached-object-integration.md) for current scope, measurements, tests, and remaining scaling limits. Earlier standalone persistence details below describe the v1–v3 stage.


Implemented 2026-09-06 in `UVoxelDebrisLifecycle`, a reusable actor component.

| Category | Rule |
| --- | --- |
| Cosmetic particles/chips | Destroy owning effect/chip actor after 10 gameplay seconds |
| Ordinary harvestable fragment | Destroy after 900 unprotected inactive gameplay seconds |
| Substantial fallen wood/rock | No expiry; allow physics sleep |
| Retained/player-owned | No expiry |

Timers follow game time, including pause/time dilation. Physics sleep does not
reset or pause cleanup. Harvestable protection pauses the countdown if any
player is within 20 m of the object's bounds, or the object intersects a
conservative 120-degree viewing cone within 120 m. The cone deliberately avoids
terrain/occlusion traces and may protect objects behind an obstruction. Bounds
include object size. Checks run twice per second only on timed harvestables;
cosmetics use a deadline timer and persistent categories have no cleanup timer.
Resource destruction is authority-only; cosmetic actors may expire locally.
`NotifyInteraction()` resets the harvestable timer; `Retain()` cancels expiry.

## Current integrations

- `AVoxelDebris` classifies islands of at most eight 100 mm voxels, and at most
  300 mm on their longest side, as cosmetic chips. Larger islands remain
  substantial; a long sparse branch cannot be misclassified solely by count.
  Its actor tick stops at settlement, eliminating idle terrain-raycast polling.
- `AVoxelFallingTimber` remains substantial, including the two large impact
  fragments. Sleeping non-breakable pieces stop prototype diagnostic ticks
  after the existing 35-second motion-observation window;
  Chaos retains their bodies and can wake them on contact.
- No particle emitter or ordinary collectible-fragment spawn system currently
  exists in these paths. Those future owners must attach/configure this component
  (or use a matching 10-second pooled-effect lifetime), and harvesting must call
  `NotifyInteraction`. Large prototype timber is not yet a harvestable inventory
  resource merely because its cleanup category preserves it.

## Persistence

Standalone saves now include a compressed detached-object snapshot beside the
terrain log. `AVoxelDebris` records retain island coordinates, transform, motion,
settlement, category and remaining lifetime. `AVoxelFallingTimber` records retain
actual mesh sections, cut caps, transform, velocities, sleep/hinge state and timer
state. Prototype environment actors retain their edited finest grid, collision
policy, transform and severed state; their coarse LODs rebuild on restoration.
This preserves the cut stump along with the fallen timber, preventing an intact
prototype tree from reappearing beside its restored fallen pieces.

The file name is `<terrain log>.detached-<hash of exact terrain bytes>.bin`.
The snapshot is atomically published before the terrain log. An interrupted
terrain-log replacement leaves the old log's corresponding snapshot available.
Existing terrain-only saves remain readable. Copy the complete save directory,
including its sidecar files; the terrain log alone cannot restore detached objects.
Unused older snapshots are currently retained rather than risking an incomplete
save pair. A future save-generation pruning policy should reclaim them.

Container version 3 uses bulk 60-byte timber vertices and contiguous indices,
zlib compression, and a CRC over the uncompressed payload. Coordinates must
roundtrip exactly through the renderer's float representation or capture refuses
them; color/material bytes are preserved. Versions 1 and 2 remain readable.
Counts and payload sizes are
bounded. Failed validation quarantines both the affected snapshot path and its
terrain-log path against overwrite; original bytes remain on disk. A failed
actor capture refuses saving rather than silently omitting a resource. Snapshot
state is captured at world teardown before any source actor destroys promoted
mesh components. Shutdown autosaving consumes those cached records instead of
trying to serialize already-ended actors. Explicit destruction removes its entry.

This path is currently standalone only. It does not add multiplayer replication,
a streamed detached-object registry, or inventory harvesting. Engine-native
snapshot field layout changes require a container version change/migration.
Lifetime state stores remaining game time, so reopening a saved snapshot preserves
its remaining timeout and time spent outside the game does not consume it.

Existing per-edit debris spawn budgets remain in place. A regional residency and
active-body budget needs the detached-object registry/persistence work; valuable
objects are not silently deleted to enforce a cosmetic cap.

## Verification

`Voxel.DebrisCleanup.Policy` is a UE automation test covering exact expiry,
protected expiry, permanent categories, proximity/view boundaries, interaction
reset and a SaveGame archive roundtrip of remaining time.
`voxel.DebrisCleanup.Probe` creates invisible game-world fixtures and checks after
12 gameplay seconds that a 10-second chip and accelerated unprotected resource
were destroyed, while substantial and retained fixtures survived. It destroys
its surviving fixtures afterward. Check for `DebrisCleanup PROBE PASS` in logs.

The editor build and `Voxel.DebrisCleanup.Policy` passed. The game-world probe
also passed with both timed actors removed and both exempt actors preserved
(`Saved/cleanup-automation.log`, `Saved/tree-felling-game.log`).

The final combined capture passed `tools/verify-tree-felling.py`: five partial
hits before release, gravity-driven acceleration, two impact fragments, and
sustained rest with both bodies still present at completion. The fixture now
uses spawn `-61472,-61504`, where the slope fitter accepts all four assets.
Reproduce with `tools/voxel-tree-felling-prototype.ps1 -Capture -KeepOpen -CleanupProbe`.

Add `-PersistenceProbe` to save through the real terrain-save writer after the
capture, destroy/reload the actors, and verify exact prototype voxel data,
actor counts/transforms, and a resource with 123 seconds remaining. The probe
also writes a deliberately damaged test snapshot and verifies refusal without
changing live actors. Test files live under `ue-project/Saved/Tests/`.
Fresh-process verification uses `-VoxelDetachedRestoreProbe` and console command
`voxel.DetachedPersistence.CheckRestored`; it checks the saved stump, two timber
pieces, and the resource's continuing timeout after 30 gameplay seconds.

## Cost and remaining work

The first uncompressed six-actor pilot snapshot was 250,985,123 bytes. This
motivated compression before adoption. Capture/rebuild still processes the full
prototype geometry synchronously: compression reduces disk size, not mesh memory
or all save/load stalls. Production should store reusable asset references and
sparse edits/fractures, with bounded asynchronous IO and mesh uploads. This is
not a forest-scale residency implementation. The compressed seven-actor fixture
occupies 26,924,645 bytes (250,985,477 bytes uncompressed), about 89% less disk
space. This measurement does not imply that saving or restoring is hitch-free.

`tools/verify-detached-persistence.py` verifies the roundtrip and restart logs,
reads the compressed snapshots independently, checks their CRC and actor counts,
and verifies that the graceful-shutdown snapshot preserves the remaining resource
timer. `tools/voxel-detached-restore-probe.ps1` opens the isolated restart fixture
and exits normally after checking it. Its shutdown save is directed to
`Saved/SaveGames/detached-restore-probe/` inside the UE project, not a player save.

Final verification passed: build, felling regression, compressed seven-actor
roundtrip, corruption refusal, fresh-process restoration, and graceful-shutdown
autosave. The independent reader verified both on-disk CRCs and all seven actor
records. The resource's remaining time was 123.0 seconds in the original snapshot
and 92.993 seconds after restart and normal shutdown; it neither reset to 900
seconds nor disappeared. Results: `Saved/detached-persistence-validation.json`.

## Asynchronous named saves (2026-09-07 UTC)

`voxel.SaveGame` now calls `VoxelSave::WriteAsync`. Its return value means admitted,
not completed. The game-thread callback runs after the worker has published the
detached snapshot, terrain log and metadata. The HUD reports Saving, then Saved
or failure. Only a successful completion changes the active save slot.

`VoxelSaveJobs` allows one active job. Overlapping requests are rejected before
capturing another large snapshot; save deletion is refused while the writer is active.
Only immutable byte arrays and strings cross to the worker. Actor access and
the matching terrain/debris snapshot capture remain on the game thread.
The worker performs compression and atomic file writes. Metadata is published
last. Snapshot reconstruction on load remains synchronous.

Synchronous APIs retain their completed-save contract for shutdown and explicit
verification tools. They drain any in-flight job first. The world teardown hook
also drains before capturing the newest actor state; an older background job
therefore cannot overwrite the later shutdown snapshot. The test confirmed a
pending job completes and updates its save slot before shutdown autosaving.

For the same seven-actor fixture, initial field-by-field capture took 788.744 ms.
Bulk mesh capture reduced that to 209.531 ms. The worker took 992.921 ms while
the game advanced 87 frames. The compact snapshot was 148,069,365 bytes before
compression and approximately 22.4 MB on disk. These are individual measured
runs, not a guaranteed latency bound: a 210 ms capture hitch is still noticeable.
Removing it requires immutable geometry snapshots/shared asset references and
bounded capture work, rather than more compression tuning.

Validation passed: editor build; `Voxel.Save.PackedTimber` automation test;
background progress; duplicate admission rejection; real worker IO failure;
old-format load; compact-format reload; and exit during an active save.
`tools/verify-async-saves.py` independently verifies both resulting containers,
all seven actor records, exact prototype-grid hashes and continuing timers.
Results: `Saved/async-save-validation.json`.

Run `tools/voxel-detached-restore-probe.ps1 -Mode async`, then `-Mode async-exit`.
These fixtures use the `async_save_verification` and `async_exit_verification`
save folders. After verification, their test metadata was moved to
`ue-project/Saved/Tests/<slug>.meta.json` so they do not appear in the player's
save list; the verifier also supports that metadata location.

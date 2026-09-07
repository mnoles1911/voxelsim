# Detached objects: saves, streaming, and multiplayer

The detached-object integration uses one game-thread registry for stable IDs, geometry revisions, dynamic state, lifetime, ownership, residency, and deletion tombstones. It covers the current debris islands, falling timber, and four environment prototypes. Ordinary terrain-embedded forest instances are still owned by the terrain renderer.

## Saving and loading

Named saves capture the terrain edit log and object metadata in one game-thread operation. Geometry is held through thread-safe immutable shared buffers. The worker assembles the v4 object payload, compresses it, and writes the matching terrain-addressed sidecar before publishing the terrain file and metadata. Edits after capture create a new geometry allocation; the pending save retains the previous allocation. Busy saves remain bounded to one active job. Shutdown drains pending work.

The loader accepts detached container versions 1–4. Startup disk reading and decompression run on a worker. Version 4 metadata validation also runs there. Nearby v4 actors materialize through the residency driver after the records are installed. Legacy versions retain their validated actor reconstruction path. Failed loads quarantine the save path; they cannot silently become an empty replacement save.

## Residency

Objects load within 160 m of any player and unload beyond 200 m, measured against their bounds. Active physics bodies stay resident. A pass inspects at most 128 records and attempts at most two transitions. Remaining harvestable lifetime advances while dormant with the same nearby/view protection policy. Retained and substantial objects remain in the registry; tombstones prevent stale updates from resurrecting deleted objects. Timber owns transferred components, so the source stump can unload independently.

Dormant records retain compact geometry buffers in RAM. Region grouping uses 256 m cells. This is actor/physics residency, not a complete disk-backed regional memory cache. One large actor restore can still exceed a frame budget because Unreal mesh/component creation remains synchronous. Source geometry caches also add memory, including old buffers retained by pending jobs.

## Networking

The authority creates detached objects, validates chop requests, runs physics/fracture, and owns cleanup. Each connection receives relevant stable IDs, revisions, compressed geometry fragments, dynamic state and deletions. Clients construct visual replicas and disable independent dynamic-body physics. Late join uses the same snapshot path. Geometry encoding/compression and decoding/decompression run on workers; byte counts, sequence, epoch, revision and checksum are bounded/validated. Geometry transfer has a paced reliable channel; motion has a separate unreliable update path.

The existing prototype axe is the current entitlement check; the game's inventory does not yet provide authoritative tool ownership. Motion interpolation and full inventory harvesting transactions are separate gameplay work. Large mesh-based assets have significantly larger network payloads than future asset-reference plus sparse-edit representations.

## Validation

Use `tools/verify-object-persistence.ps1` for the `Voxel.Objects` automation group. It exercises real isolated debris streaming, v4 disk reload, corruption rejection, ID and revision rules, snapshots, and network packet assembly. Tests require the regular renderer because unrelated water texture initialization reports errors under `-nullrhi`.

Use the async save fixture and multiplayer smoke launcher for cross-process validation. Runtime measurements and exact smoke results are recorded in the task's completion report and `Saved` logs.

## Measured verification (2026-09-06)

- `Saved/build-object-integration.log`: Unreal editor build succeeded.
- `Saved/object-persistence-final-tests.log`: all six `Voxel.Objects` automation tests passed (geometry envelope, identity, integrated residency/persistence, network assembly, registry, snapshot).
- `Saved/async-save-validation.json`: independent old/new-format and byte-level validation passed. The tree fixture captured in **0.464 ms**, versus approximately **210 ms** before immutable geometry snapshots. Its worker took **1124.511 ms**, while **152 game frames** advanced. Busy admission and forced disk-write failure tests passed.
- Fresh v4 startup installed seven records and materialized the six nearby actors in three bounded passes; the distant resource remained a data record. Shutdown drained its pending save. Stable IDs, geometry bytes, all four plant grids, and remaining resource lifetime survived restart.

The multiplayer launcher deliberately uses `-VoxelSyntheticTerrain` with a unique seed and normal DX12 clients. This isolates transport from baked-terrain availability and preserves ordinary game settings. It tests real dedicated-server/two-client RPCs and actor replicas, including a delayed join; it is not a full-tree network performance benchmark. NullRHI clients currently enter unrelated terrain GPU work and are unsuitable for this smoke test.
- `Saved/Tests/detached-net-20260906-214932-3bf04369/result.json`: dedicated server plus two DX12 clients passed authoritative identity, movement, retention, late join, and deletion checks; all three processes exited normally. The resource kept ID `E65ECA5747750E140BDEBC9BE2B3EC4C`; all peers agreed on revision 4 while retained and revision 5 after deletion.

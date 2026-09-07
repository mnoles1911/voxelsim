# Detached objects: saves, streaming, and multiplayer

The detached-object integration uses one game-thread registry for stable IDs, geometry revisions, dynamic state, lifetime, ownership, residency, and deletion tombstones. It covers debris islands, falling timber, and generic environment grid assets; the four original prototypes remain compatible fixtures. Ordinary terrain-embedded forest instances are still owned by the terrain renderer.

## Saving and loading

Named saves capture the terrain edit log and object metadata in one game-thread operation. Geometry is held through thread-safe immutable shared buffers. The worker assembles the v4 object payload, compresses it, and writes the matching terrain-addressed sidecar before publishing the terrain file and metadata. Edits after capture create a new geometry allocation; the pending save retains the previous allocation. Busy saves remain bounded to one active job. Shutdown drains pending work.

The loader accepts detached container versions 1–4. Startup disk reading and decompression run on a worker. Version 4 metadata validation also runs there. Nearby v4 actors materialize through the residency driver after the records are installed. Legacy versions retain their validated actor reconstruction path. Failed loads quarantine the save path; they cannot silently become an empty replacement save.

## Residency

Objects load within 160 m of any player and unload beyond 200 m, measured against their bounds. Active physics bodies stay resident. A pass inspects at most 128 records and attempts at most two transitions. Remaining harvestable lifetime advances while dormant with the same nearby/view protection policy. Retained and substantial objects remain in the registry; tombstones prevent stale updates from resurrecting deleted objects. Timber owns transferred components, so the source stump can unload independently.

Distant dormant records now page immutable geometry to compressed, content-addressed files grouped into 256 m regions. At most two page jobs run per world. The registry releases its RAM buffer only after the published file passes hash and CRC verification. Nearby page-only records hydrate on a worker; stale edit/delete completions are discarded. Saves and network snapshots resolve page references on their workers and embed the bytes, so saved worlds do not depend on the temporary page cache. Published cache files currently persist after world teardown; disk-cache garbage collection remains separate work.

Version 4 timber and environment restoration decode geometry and build mesh arrays on workers. A global queue admits four actors and advances at most two staging steps within a soft 2 ms budget per frame. Actors remain hidden until all sections are ready and their current identity/revision is accepted. Cancellation cannot resurrect a tombstone. A single component upload or collision cook remains indivisible and can exceed the soft limit. Legacy restoration and debris reconstruction retain their synchronous implementations. Source geometry caches also add memory, including old buffers retained by pending jobs.

## Networking

The authority creates detached objects, validates chop requests, runs physics/fracture, and owns cleanup. Each connection receives relevant stable IDs, revisions, compressed geometry fragments, dynamic state and deletions. Clients construct visual replicas and disable independent dynamic-body physics. Late join uses the same snapshot path. Geometry encoding/compression and decoding/decompression run on workers; final geometry acknowledgements wait for staged actor publication, and incoming motion is preserved while the mesh builds; byte counts, sequence, epoch, revision and checksum are bounded/validated. Geometry transfer has a paced reliable channel; motion has a separate unreliable update path.

The existing prototype axe is the current entitlement check; the game's inventory does not yet provide authoritative tool ownership. Full inventory harvesting transactions remain separate gameplay work. Large mesh-based assets have significantly larger network payloads than future asset-reference plus sparse-edit representations.

Detached client replicas blend translation and rotation from their displayed pose to each accepted server pose over 100 ms. The registry immediately retains the latest authoritative state; interpolation is presentation only and does not predict physics. Missing packets hold the final pose. Corrections over 10 m, scale changes and standing environment transforms snap. Standing assets must retain exact quarter-yaw lattice alignment. Replaced, evicted and deleted actors discard old blends, as does a connection epoch reset. Both reliable and unreliable motion paths preserve residency instead of incorrectly resetting live records to dormant. The multiplayer smoke checks settled actor positions in addition to registry positions and requires live residency.

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

## Ordinary forest migration

Stable placement provenance, immutable render-ownership transactions, and a guarded UE adapter are implemented. Ordinary forest instances still use terrain rendering. Activating their shared-object rendering requires coordinated hidden CPU/GPU page publication and composition-correct bank-to-object placement. Arbitrary environment identity and VXA initialization are now implemented. The current GPU path can publish before the game-thread result check, so a two-site asset filter cannot provide a safe ownership swap. See `production-environment-ownership.md` for the exact renderer bridge and acceptance criteria.

## Current verification pass

- `Saved/build-object-paging.log`: editor build succeeded.
- `Saved/object-paging-tests.log`: all nine `Voxel.Objects` tests passed, including paging, portable page-backed encoding, staged restoration/cancellation, production ownership prerequisites, and retry fairness.
- `Saved/asset-ownership-core-tests.log`: four core ownership tests passed.
- `Saved/staged-restore-game.log`: all six nearby full-size fixture actors restored; the seventh distant resource remained a record and produced a verified page file. Maximum measured individual restore steps ranged from 0.533 to 12.665 ms. Ready/publication elapsed times included terrain startup, worker work and hundreds of staging frames; these measurements do not imply an overall 2 ms frame guarantee.
- That fixture's save captured in 0.554 ms, its worker took 1698.846 ms, and 224 game frames advanced. Busy admission and real write-failure probes passed.

Timber publication waits for terrain collision support. Ground preparation samples only a resident footprint, spreads sampling and small collision tiles across budgeted steps, and caches up to 32 proxies. Admission can reclaim an unused distant proxy; protected proxies stay resident. A world with 32 simultaneously protected proxies defers further ground-dependent restores. Terrain edits invalidating an existing ground proxy and larger-range production physics still need a shared terrain collision provider.

`Saved/staged-save-validation.json` passed independent format/CRC/EOF, ID, geometry, owner and lifetime checks across fresh startup and shutdown. This repeated fixture had only 0.334 seconds remaining on its distant resource; the exit save correctly kept that ID as a newer tombstone with zero lifetime and released payloads. All six surviving objects kept identical geometry bytes. The validator's explicit `--expect-resource-expiry` mode requires this near-expired input and exact tombstone transition; ordinary timer-continuity validation remains the default. One timber actor was still pending at teardown; its data survived while the partial actor was cancelled. Exit-save capture was 0.336 ms and shutdown drained the worker successfully.

`Saved/staged-restore-measurements.json` independently verifies both runtime page files' sizes, decompression boundaries, CRCs and content hashes.

`Saved/Tests/detached-net-20260906-223855-c8347fec/result.json` passed the dedicated-server/two-DX12-client test with the final queue implementation. All peers agreed on ID `0940C1CD4CA42B4D27D76A906811EF0E`, retained revision 4 and deleted revision 5. The early client received X=215 before movement; the delayed client installed X=315. Both used the restore queue and all three processes exited normally. This remains a small resource transport test; full-tree geometry bandwidth and network fracture performance are not established by it.

The launcher uses Unreal's supported `-Multiprocess` flag so three test processes avoid repeated SDK-validation builds. It also detects UnrealBuildTool between compiler actions and leaves concurrent builds untouched. Earlier attempts `223055-f31875de` and `223513-fe709673` were interrupted by SDK startup waiting on another checkout's build; their logs are retained. The earlier dated measurements above describe the preceding revision.

## General environment support (2026-09-06)

The common actor accepts arbitrary VXA bytes through `InitializeAssetFromVxa`, with a bounded source descriptor instead of a four-name save enum. Source ID, generator kind, environment category, VXA content hash, spec/catalog/provider fingerprints, seed index and explicit felling capability survive persistence and staged restoration. Legacy identities retain their original bytes. LOD switching derives distances from actual dimensions. Felling eligibility, restored collision preparation and terrain seating use capabilities/kind metadata; the old fixture names remain only in compatibility mapping and fixture commands.

`Saved/build-general-environment.log` succeeded. `Saved/general-environment-tests.log` passed all ten object tests. `Voxel.Objects.GeneralEnvironment` exercises 18 combinations of six families and three pitches, with arbitrary names, snapshot/staged round trips, hash/metadata retention, LODs, voxel queries and capability checks. It refuses 12.5 mm environment sources, category mismatches and wrong source hashes.

`Saved/environment-contract-audit.json` covers 446 specs and 1,742 existing bank files representing 439 species. Current bank headers fit the source limits. Seven hero species are unbaked; hypothetical finer rebakes of the largest trees can exceed the dense-grid limit and require a chunked/sparse implementation. This audit does not enable production terrain ownership migration or establish every species' gameplay physics. See `environment-asset-framework-contract.md` for required profiles and precise scaling limits.

# Session persistence handover — 2026-09-07

## Immediate instruction

The current user instruction is to finish every remaining planned phase without waiting for feedback. Work is active. This document preserves an earlier handover plus the verified continuation below; it is not a completion claim. Continue from `docs/session-persistence-execution.md` in `.scratch/persistence-integration`, branch `codex/session-persistence-integration`. Preserve the original dirty parent worktree.

## Verified continuation through secure transport

- `326b45e` adds world-local slots, periodic saves, agent/player/actor adapters and selected-seed travel. `0aeba2c` merges main through PR234.
- The subsequent secure batch adds per-client AES-GCM invites, isolated persistent client profiles, dedicated-server initialization, Windows long-path atomic publication, hidden-sky simulation, offline vehicle destruction handling, capture relationship validation and one deferred manual save.
- Full160-action Unreal build passed (`Saved/persistence-lifecycle-queue-build.log`). Five persistence/object tests passed with zero errors (`Saved/persistence-lifecycle-queue-report/index.json`). Python checkpoint11 and transport3 tests pass; unity-collision lint passes.
- Dedicated server + two DX12 clients passed a complete encrypted process restart, preserving world/player IDs, inventory, selection, position and flight mode (`Saved/Tests/session-net-8bec4b53653943de974a303ff291dce9/result.json`). All six processes exited0.
- Rendered asynchronous queue + real worker failure + pre-teardown save passed (`Saved/persistence-manual-queue-runtime.log`), process exit0. Two small captures were1.046/5.942ms; this is not a large-world benchmark.
- Generated credentials are private runtime files. Never print or commit them. Use the invite generator and documented harness options in `docs/session-transport.md`.
- Remaining gates include shared domain join baseline/ack, streamed safe spawn, live vehicle physics/network controls, actual selected-seed travel, five-generation retention/read leases, river rearm/transaction fences, crash/lifetime matrices and16-player/large-world budgets. Do not mark P0�P6 complete.

The sections below describe the earlier checkpoint and are historical where they differ from this continuation.

This task worked in `C:/Users/Matt Noles/.codex/worktrees/5aaf/voxelsim`, initially detached at `1d1005a`. The shared primary checkout is `D:/voxelsim`. Other tasks have concurrently evolved environment/object/core and Asset Forge code there. The persistence commit deliberately excludes unrelated asset/model changes. Its integration depends on the shared primary checkpoint's existing object/geometry/streaming implementation, which was uncommitted at this task's start. Read the final Git history for actual merge IDs.

## Implemented and previously verified

- Immutable checkpoint generations under `<logical-world-path>.checkpoints/`, hash/size manifest validation, unique flushed commit marker, whole-generation fallback, no fallback to legacy after committed data loss, no terrain-only downgrade over a simulation checkpoint.
- Sync, async, named, world and pre-teardown captures include terrain, detached objects, CA water, basin ledger and simulation clock. Water/sky wait for restore and object load; failed sessions cannot capture a different save. The menu reports failed loading without its cosmetic timeout opening gameplay.
- Water BeginPlay seed autoload and independent water Deinitialize save are removed. SWE volume and outstanding spill transfers are normalized before capture. Pre-teardown save occurs while water/sky still exist, after object/save work drains.
- Legacy default-slot seed water import; original files retained. Legacy named water association remains ambiguous and currently initializes fresh water/time with a warning (a real explicit import-choice UI is still required).
- Exact epoch and CA fixed-step remainder restoration without offline catch-up.

Evidence before the interrupted extension: `Saved/session-runtime-verified-report/index.json` reports `Voxel.Persistence.SessionDomains` Success. Storage and object companion tests passed in `Saved/session-runtime-final-report/index.json`. The runtime fixture expects exactly three literal NullRHI bathymetry resource diagnostics, one per transient world, and suppresses no persistence errors. Independent reader/tests are `tools/checkpoint_store.py` and `tools/test_checkpoint_store.py`.

## Latest interrupted extension — do not confuse with completed phases

- Water hydrology container v2 adds original river region, suspended graph bytes, remaining routing tick delay and enabled state. Disarming retains graph state. Restore validates graph against its recorded region before installation, and rearm failure stops the session rather than discarding dams/water. Legacy graph blobs without region identity still refuse.
- Restored implicit-ocean mode is pinned per world. Sky clock rate/calendar now use per-world restored values rather than rejecting a different process-global configuration. Console-cvar override behavior and clock-disabled behavior need follow-up validation.
- Store manifest v3 optionally requires `gameplay.json`, and refuses later downgrade. Runtime gameplay capture is NOT connected yet, so current coordinator still emits v2. Update the independent Python resolver for v3 before enabling it; add required-domain/size/fault tests.
- Inventory now has owner-only replicated slots/selection/initialized flag, server-only mutation and validated snapshot restore. Player identity/admission and server hotbar-use RPC are NOT implemented. Server BeginPlay seeding is temporarily retained so this checkpoint does not start every inventory empty. Replace it with restore-or-seed when the durable player store is connected. Remote item-use flow is incomplete; do not call multiplayer inventory complete.
- No player record store, credential exchange, agent adapter or generic entity adapter was created. The proposed identity choice was server-issued high-entropy bearer credentials (only hashes on the server) plus a persistent local host profile. No credential files or secrets have been generated. Authentication/transport and duplicate-login behavior still require implementation and tests.

## Next work, in dependency order

1. Finish validating river v2/clock changes, including saved graph rearm with no pawn, toggles, same-seed branches, bounds overflow/trailing bytes and timer continuation. Update legacy compatibility policy and tests.
2. P0 classify every enabled state owner; P2 add agents, boats/gliders, thrown inventory items, explosives and remaining gameplay timers or explicit safe transaction completion. Craft runtime path was not found in the world subsystem despite core serializers existing; do not invent a live craft state silently.
3. Add a bounded required gameplay payload with world identity, all connected/offline player records, agents and entity relationships. All semantic validation must precede live publication. Keep material/item/content identities validated.
4. P3 selected-seed construction/travel, fresh epochs, recoverable failed-load menu, worker/registry invalidation, collision-safe spawn, all mutation/RPC admission. Current failure screen requires a restart and the active slug remains process-global.
5. P4 durable identity, empty-inventory restoration, owner-only inventory RPC flows, logout capture, duplicate identity rejection and player position/velocity/pawn restoration. Do not use display names or unverified client GUIDs as identity.
6. P5 world-local slots, periodic dirty autosave/coalescing/manual queue/completion/retry, admin-only rollback, shared terrain/water/object/player join baseline and acknowledgement.
7. P6 migrations, bounded catalog/reader work, safe generation GC/read leases, crash and multi-process matrices, 16-player and large-world measured budgets. Existing whole-world capture has not met the proposed 5 ms p95/16 ms maximum targets.

## Build/test procedure and cautions

Engine `D:/UE_5.8`; bundled Python `C:/Users/Matt Noles/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`. Normalize duplicate PATH/Path before MSBuild/UBT. Core: `cmake --build build/voxel-core-msvc --config Release --target voxelcore -j 4`. Unreal: `Build.bat VoxelEarthEditor Win64 Development -project=<absolute project> -WaitMutex -NoHotReloadFromIDE -DisableUnity -MaxParallelActions=4`. UBT has repeatedly rebuilt all ~143 actions because of its dependency cache, taking about four minutes.

Tests: `UnrealEditor-Cmd.exe <project> -unattended -nop4 -nosplash -nullrhi -nosound -ExecCmds="Automation RunTests Voxel.Persistence.SessionDomains+Voxel.Persistence.CheckpointTransaction+Voxel.Objects.Pages+Voxel.Objects.Snapshot" -TestExit="Automation Test Queue Empty" -ReportExportPath=<unique directory> -abslog=<unique log>`. Inspect report states, not only process exit (UE has exited 0 even with a failed test). Existing ProjectID/GameFeatureData startup errors and absent render assets are separate from domain assertions.

Generated response files can rebuild a corrected translation unit and relink, with working directory `D:/UE_5.8/Engine/Source`; their output paths point into this worktree. Link both the gameplay import library/DLL and UI DLL after exported API changes. Do not overwrite a DLL in use by another session. Do not terminate the original editor or other tasks' builds.

Rendered gameplay/shutdown and multiplayer admission were not verified by the passing scalar runtime fixture. Old compiled binaries do not include the latest interrupted extension until the handover build completes. See final verification below.

## Final verification / integration

Checkpoint commit: `c3b7951`; integrated with accumulated gameplay main `fa56da9` by `a46bfed`. Compile corrections are `93fcd2f`, merged into the integration branch by `1a8ac0d`. Local main first fast-forwarded to `a46bfed`; the environment session is subsequently integrating its separate environment/creature checkpoint and the compile corrections. Do not reset main to this older persistence integration branch.

The initial full 143-action build in `Saved/persistence-handover-build.log` found two compile errors: the inventory replication macro requires the output parameter name `OutLifetimeProps`, and the implicit-ocean pin guard had been inserted into the hot-reload constructor instead of `MaybeRelatchImplicitOcean`. Both were corrected. Recompiling both affected translation units using the generated response files, rebuilding the gameplay import library, and linking both gameplay and UI DLLs completed successfully (exit 0). The original full-build log retains the earlier errors; it is not evidence of a second full successful UBT invocation.

Final runtime evidence: `Saved/session-handover-final-report/index.json` reports all four tests Success with zero errors: `Voxel.Persistence.SessionDomains`, `Voxel.Persistence.CheckpointTransaction`, `Voxel.Objects.Pages`, and `Voxel.Objects.Snapshot`. The transaction fault test emitted six warnings. `tools/test_checkpoint_store.py` passed all six tests. These tests exercised the corrected binaries, including the latest clock/hydrology extension; they do not replace the outstanding river graph, multiplayer, rendered gameplay, or scale validation listed above.

The user authorized publishing and merging a PR after finishing this session checkpoint. Use the repository PR history for the final remote merge commit. The broader P0-P6 implementation remains incomplete and is explicitly handed over above; no unattended continuation or scheduling is active.

## Remote checkpoint CI

PR #232: https://github.com/mnoles1911/voxelsim/pull/232. CI run `34082724414` is not green. This PR includes previously unpublished ancestor work, so these are real integration follow-ups even though the persistence diff against `fa56da9` does not change their files:

- GCC and Clang: `voxel-core/bench/riverribbonprobe.cpp` uses `std::sqrt` without its own `<cmath>` include.
- Asset Forge: selftest uses a 50 mm environment tree where resolution policy requires 100 mm; generated palette is out of sync.
- Terrain service: missing SciPy/Numba bake dependencies and a stale bake-fingerprint expectation (13 failures, 518 passes, 125 skips).
- Unity lint: DetailAssetSubsystem and EnvironmentLODPrototype collide on `FMeshGeometry`, `FPaletteLinear`, `PaletteLinear`, and `kMaxGridCells`.
- Front-end switch classification has unclassified switches; float-ban reports asset pitch/slope and fluid helpers; shader vendor lint reports brickpack bounds and signed arithmetic in karst/worldgen.

Shader compilation, Docker build, SFX parity and Python syntax/perf-gate checks passed. Unreal CI is skipped; use the local build/runtime evidence above. Do not describe this checkpoint as having passed the complete repository CI suite. Coordinate fixes with the environment session, which is integrating newer versions of several affected files.

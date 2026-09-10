# Session persistence execution ledger

User authorization: implement every remaining phase without waiting for feedback (2026-09-07). Work remains active until the acceptance gates below are satisfied or an external dependency makes further progress impossible. Existing unrelated changes are preserved.

## Decisions

- No offline catch-up; online empty servers continue simulation.
- Manual saves are retained. Autosaves default to five gameplay minutes and five generations per world.
- Unsupported required content is a load error, never silent deletion.
- Authentication must prove possession of a server-issued credential or a verified provider identity; display names/client-supplied player GUIDs are not identities.
- Legacy named-water ambiguity needs an explicit import policy represented in the UI; original files remain untouched.
- Physics internals and rendering caches are rebuilt; transforms, velocities, gameplay state and remaining timers persist.

## Acceptance ledger

- [x] P1 immutable generation commit, complete-generation recovery, fault-stage tests.
- [x] P2 terrain/object/CA/basin/clock initial runtime round trip.
- [ ] P0 complete enabled-state inventory and required-domain/content contract.
- [ ] P2 river bounds/routing phase; agents and enabled gameplay entity adapters; transaction fence.
- [ ] P3 selected-seed construction/travel, staged failure recovery, epoch invalidation, usable failed-load UI.
- [ ] P4 persistent player records, identity, owner-only authoritative inventory, disconnect/rejoin.
- [ ] P5 world-local slots/autosave/coalescing, manual completion/retry, shared join baseline and admin controls.
- [ ] P6 bounded readers, safe retention/GC, migrations, crash/process/lifetime tests and measured limits.

## Evidence carried forward

Storage and object regression tests pass. Initial runtime domain test passes in `Saved/session-runtime-verified-report/index.json`; its three transient worlds explicitly expect NullRHI bathymetry resource diagnostics. The latest source compiled and both gameplay/UI modules linked after a targeted TEXT-wrapper correction. Rendered shutdown and multiplayer acceptance remain open.

## Active continuation after PR 232

Implementation is continuing in `.scratch/persistence-integration` on `codex/session-persistence-integration`. The original dirty worktree is preserved. PR 232 merged the earlier checkpoint; it did not complete P0–P6.

The current batch adds world-scoped slot bindings, unique New Game slots, periodic gameplay-time saves, strict version-3 readers, persisted agent records, player inventories and motion records, and restore-before-seed admission. Remote registration requires an encrypted connection and a durable client credential acknowledgement. Transport configuration and remote-process acceptance remain open; the default plaintext connection cannot admit persistent remote players.

Verified continuation evidence:

- `tools/test_checkpoint_store.py -v`: 11 tests passed, repeated under Python `-O`. Run the script directly so its local `checkpoint_store` import resolves.
- `Saved/persistence-continuation-report/index.json`: four persistence/object tests passed before the connected player/identity batch. This is not evidence for the later player changes.
- `Saved/persistence-gameplay-build.log`: full Unreal build passed before the latest player admission/motion changes.
- `Saved/persistence-player-verified-build.log`: full Unreal build passed. The first runtime fixture correctly refused a synthetic controller without a local player; after fixing that setup, it exposed an admitted-controller capture gap. Capture now iterates the persistence bindings, including controllers outside the world iterator during lifecycle transitions.
- `Saved/persistence-player-bound-report/index.json`: `Voxel.Persistence.SessionDomains` passed with zero errors, including empty host inventory, selected slot and agent round trip. The other three storage/object tests passed in `Saved/persistence-player-verified-report/index.json`.

The next source batch connects boat/glider/item/explosive adapters and player relationships, adds per-game-instance world replacement for selected-seed loads and a failed-load return button. `Saved/persistence-actors-travel-build.log` passed all 157 actions. `Saved/persistence-actors-travel-report/index.json` reports all five tests successful with zero errors: SessionDomains, GameplayActors, CheckpointTransaction, Objects.Pages and Objects.Snapshot. The actor test validates the codec; live vehicle/physics restoration and actual travel still require process tests. A per-client AES-GCM invite handshake is being added separately and is not included in that build.

See [the state inventory](session-state-inventory.md) for remaining entity adapters and acceptance gates. Build success alone does not close those gates.

Checkpoint `326b45e` contains the tested player/actor/travel batch. Merge `0aeba2c` incorporates main through PR 234 (`c54de6e`). The next uncommitted batch enables per-client AES-GCM transport invites and distinct local profiles, fixes hidden-sky clock publication to water/weather, and adds live pawn/projectile and multi-process restart fixtures. Full builds passed in `Saved/persistence-secure-live-build.log` (159 actions) and `Saved/persistence-network-probe-build.log` (160 actions).

The first live-actor fixture correctly hit the fine-tier gate because transient worlds had no streamed production tiles. The fixture now explicitly constructs synthetic terrain; the production gate remains enabled. That corrected runtime run and encrypted restart acceptance are pending. `tools/test_session_transport.py -v` passed three invite-generator tests; generated keys are never printed or committed.

`Saved/persistence-live-synthetic-report/index.json` subsequently passed both SessionDomains and GameplayActors with zero errors. This covers real pawn motion/stance/selection and in-flight item/explosive records in synthetic transient worlds.

The real encrypted fixture has not passed yet. Its first NullRHI clients hit an existing raster-atlas rendering assertion, so the harness now defaults to hidden DX12 clients with a headless server. The next attempts found two runtime bugs that unit fixtures did not expose: the dedicated-server rendering early return skipped session restoration, and Windows atomic profile publication failed with a 282-character temporary path. Both fixes compile and link. The shared publication helper now uses extended Windows paths, and profile publication refuses to overwrite a different credential under a process lock. A long-path checkpoint regression has been added; its runtime result and the next encrypted restart result remain pending.

The encrypted restart acceptance now passes in `Saved/Tests/session-net-8bec4b53653943de974a303ff291dce9/result.json`: dedicated server plus two DX12 clients, encrypted admission, distinct durable IDs, empty/nondefault inventory slots, selected slot, position and flight mode persisted through complete process restart. All six owned processes exited successfully. The server was headless and terrain synthetic; this does not establish streamed collision readiness, shared domain baseline completion, vehicle physics or 16-player capacity. The long-path CheckpointTransaction regression passed in `Saved/persistence-longpath-report/index.json` with zero errors and the expected fault-injection warnings.

The following source batch closes destroyed offline vehicle references and checks player/actor relationships during capture as well as restore. A bounded deferred manual-save request captures only after the prior worker finishes and refuses expired/finalized worlds. These changes are building in `Saved/persistence-lifecycle-queue-build.log`; runtime validation is pending.

`Saved/persistence-lifecycle-queue-build.log` passed all 160 build actions. `Saved/persistence-lifecycle-queue-report/index.json` passed SessionDomains, GameplayActors, CheckpointTransaction, Objects.Pages and Objects.Snapshot with zero errors. SessionDomains now includes a reserved offline vehicle destruction regression. The manual queue probe was then updated for one admitted deferred request and refusal of a third request, compiled and linked; its rendered process verification is running.

`Saved/persistence-manual-queue-runtime.log` reports both queued writes successful, `SaveAsync PROBE PASS exit=0 busyRejected=1`, `SaveAsync FAILURE_PROBE PASS`, and final pre-teardown save success. Rendered process exited0. The two small capture measurements were1.046/5.942ms, workers91.565/90.752ms, advancing24/25frames respectively. Large-world performance remains unmeasured.

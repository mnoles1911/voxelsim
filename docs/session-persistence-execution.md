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

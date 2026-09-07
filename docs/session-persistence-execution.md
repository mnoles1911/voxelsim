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

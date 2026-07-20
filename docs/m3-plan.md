# M3 — Multiplayer (working plan)

Gate (plan §4): two clients dig the same hole. Do this BEFORE the world gets
rich. Doctrine §2.2/§2.4 are the whole design: never replicate voxels; one
authority path (the edit log) for every world change.

## Decisions (binding)

| Topic | Decision |
|---|---|
| Server model | UE dedicated-server target (`VoxelEarthServer.Target.cs`) + listen-server both supported. Server owns THE authoritative `vxc::World` + edit log. |
| Terrain source | `ITerrainSource` split: **Local** (in-process generation — current path, used by server and single-player) vs **Remote** (client): v1 clients DERIVE tiles locally from the replicated seed (synthetic tiles are deterministic below the doctrine boundary), so no tile bytes move. When diffusion tiles arrive, Remote gains tile fetch (server/CDN) — the seam is isolated here by design. |
| What replicates | (a) seed + worldgen version handshake (join-time, versions must match or client is refused); (b) edit-log entries: seq-stamped brick diffs, reliable, ordered, streamed on join (full log replay → later: compacted snapshot via `vxc_editlog` machinery) then live appends; (c) pawns/actors via standard UE replication. NOTHING else. |
| Authority flow | Client sends edit INTENT (dig/place/carve params — the same few ints the local path uses today) → server validates (range from pawn, rate cap, size caps) → applies to authoritative World (assigning seq) → broadcasts applied entries to all clients → every client applies entries to its overlay through the SAME code path local edits use today. |
| Prediction | Client applies its own intent locally at send time (instant feel, exactly today's behavior) and remembers pending intents. Server entries arriving that match a pending intent confirm it (no-op — deterministic derivation means byte-identical bricks). Divergence (rejected/reordered) → brick-granularity reconcile: re-apply server log state for affected bricks, re-mesh dirty chunks (machinery exists). |
| Transport | UE replication (a replicated `AVoxelEditRelay` actor: server->client reliable RPCs carrying batched serialized entries via the existing `ByteWriter` wire format; client->server intent RPC). No custom sockets in v1. |
| Determinism guard | Handshake exchanges the CPU digest of a fixed probe region (seed-derived); mismatch = version/platform drift → refuse join loudly. This weaponizes the M0 work as a live compatibility check. |

## Waves

1. **Skeleton**: server target builds; subsystem role split (authoritative
   vs replica — replica never generates edits locally except via prediction);
   `AVoxelEditRelay` + intent RPC + entry broadcast + join-time log sync;
   PIE 2-player verification (editor multiplayer PIE), then dedicated server
   + 2 headless clients with log-verified matching editedDigest → the gate.
2. **Prediction/reconcile** polish + join-time compacted-snapshot sync +
   validation hardening (rate/range/size caps, server cvars).
3. **Water/NPC readiness hooks** (W2 wants networking from birth): the relay
   generalizes to non-edit authoritative streams later — do not build it
   narrower than "seq-stamped opaque entry stream".

## Non-goals (v1)
Interest management (all edits to all clients — KB/s class per plan §3.4),
encryption/auth, server browser, persistence across restarts (M3 wave 2+
via edit-log save/load + `vxc_editlog` compaction).

## Wave 1 — as-built (landed 2026-07-20)

Gate PASSED — see docs/status.md's M3 section for the full writeup and the
three matching digest lines. Summary of what shipped vs. this doc's
original framing:

- **Server target**: `Source/VoxelEarthServer.Target.cs` exists and is
  UBT-valid, but cannot be COMPILED on this dev machine — `D:\UE_5.8` is an
  Epic Games Launcher Installed Build, and Installed Builds refuse to build
  `TargetType.Server` (requires a source-built engine; UBT's own message:
  "Server targets are not currently supported from this engine
  distribution"). The gate instead ran the dedicated-server role via
  `UnrealEditor-Cmd.exe <uproject> -server -log` (uncooked headless server
  mode on the already-built Editor target) — functionally exercises the
  exact same `NM_DedicatedServer` code path this doc's role-split decisions
  are about, just without a separately-cooked binary. Building the actual
  `VoxelEarthServer` target is deferred until a source-built engine is
  available (wave 2+ or infra work, not a code blocker).
- **Relay RPC surface**: `AVoxelEditRelay` carries server->everyone traffic
  only (replicated handshake fields + `MulticastAppliedEntries`). Client->
  server traffic (`ServerSubmitDigIntent`/`ServerSubmitPlaceIntent`/
  `ServerSubmitCarveIntent`/`ServerRequestJoinSync`/
  `ClientReceiveJoinSyncChunk`) lives on `AVoxelEarthPlayerController`
  instead of the relay actor this doc's decisions table implied — a single
  shared, unowned relay actor structurally cannot receive client-called
  Server RPCs (UE requires the calling connection to own the target actor).
  The relay stays the generic "seq-stamped opaque entry stream" broadcaster
  the wave-3 note asks for; only the request/response half moved.
- **voxel-core untouched**: no `applyReplicated(const EditEntry&)` or any
  other voxel-core change was needed. Replicated entries replay through the
  existing public `World::applyEdit` (one call per brick, exactly the local
  path), fed by a UE-side wire format built from `vxc::ByteWriter`/
  `ByteReader` — see `FVoxelWorldImpl::ApplyReplicatedEntries` /
  `SerializeEntries`/`ParseEntries` in `VoxelWorldSubsystem.cpp`.
- **A dedicated-server perf fix fell out of the gate work**: the render-
  chunk streaming/meshing pipeline was running full-tilt on a headless
  `-server` process with zero viewport ever to consume it, stalling
  `FTimerManager` timers under concurrent multi-process load. Fixed by
  skipping `ChunkOwner`/`ChunkRoot` spawn entirely on `NM_DedicatedServer`
  in `UVoxelWorldSubsystem::OnWorldBeginPlay` — a real standing
  optimization, not just a test-timing workaround.
- PIE 2-player verification was not run this wave (the dedicated-server +
  2-headless-clients gate superseded it); worth a follow-up smoke test.

## Wave 2 — persistence + validation hardening (landed 2026-07-20)

**Save/load.** `voxel.SaveWorld` console command + autosave-on-shutdown
(`UVoxelWorldSubsystem::Deinitialize`) serialize `Impl->Voxels.log()`
(`vxc::EditLog::serialize`, the same format `vxc_editlog`'s `stats`/
`compact`/`verify` commands already read) to
`Saved/VoxelWorlds/<seed>.vxlog`, atomic tmp+rename write (`FFileHelper` +
`IFileManager::Move`). Authority only (server/listen/standalone) — a no-op
with a logged warning on `NM_Client`. Compacts the OUTGOING copy
(`vxc::compactLog`) when the raw log has more than 2x its compacted entry
count; the live in-memory log itself is never mutated (append-only
doctrine). `-VoxelSaveWorldAfter=<s>` mirrors `-VoxelDumpDigestAfter` for
headless verification (save, log entries+digest, self-quit). On startup,
`OnWorldBeginPlay` loads `Saved/VoxelWorlds/<seed>.vxlog` (if present) via
`vxc::World::replay` before any streaming/chunk work touches `Impl->Voxels`
— authority only, gated on `NetMode != NM_Client` (a client gets its state
from join-sync instead). `-VoxelNoLoad` bypasses the load entirely.
**Bug found and fixed while getting this verified**: `-game`/`-server`
launches also construct a `UVoxelWorldSubsystem` instance for the
transient `/Engine/Maps/Entry` loading world (`WorldType::Game`, so
`DoesSupportWorldType` doesn't filter it out) — that phantom instance's
`Impl` is a freshly-constructed EMPTY world that never reaches
`OnWorldBeginPlay` before being torn down, and an unconditional autosave in
`Deinitialize` was overwriting the real save file with a 0-entry log before
the actual game world even got a chance to load it (caught by the very
first verification run: `SaveWorld: wrote 0 entries` appeared BEFORE the
first `LoadWorld:` line). Fixed with a `bWorldBegunPlay` flag, set only once
`OnWorldBeginPlay`'s game-world/Impl-present body actually runs, checked
before `Deinitialize` calls `SaveWorld()`.

**Join-sync compaction.** `UVoxelWorldSubsystem::SerializeCompactedLogEntries`
(new) sends `vxc::compactLog(Impl->Voxels.log())` through the existing flat
wire format instead of the raw log; `AVoxelEarthPlayerController::
ServerRequestJoinSync_Implementation` now calls it in place of
`SerializeLogEntriesFrom(0, ...)`. The server's live log is never mutated —
only this outgoing copy is compacted. Verified live in the networked rerun
below: `SerializeCompactedLogEntries (join-sync): 8 raw entries -> 4
compacted entries (112 bytes)`.

**Validation hardening.** Per-player token-bucket rate cap
(`AVoxelEarthPlayerController::TryConsumeIntentToken`, continuous refill,
starts full so a client's first post-join intent isn't throttled) gates
every `ServerSubmit*Intent_Implementation`, capacity =
`voxel.Server.MaxIntentsPerSec` (default 10). Size-cap enforcement
(dig/place `SizeVoxels > UVoxelWorldSubsystem::MaxCubeSizeVoxels` rejected)
and carve-radius enforcement (`RadiusUU > voxel.Server.MaxCarveRadiusUU`,
default 400 UU, rejected) both live in `_Implementation` (logged reject, no
edit applied) rather than `_Validate` (which stays a loose sanity bound —
`_Validate` failing disconnects the connection, too harsh for a tunable
game-rule cap that a legitimate-but-outdated client might exceed). The
existing camera/pawn-distance range check was verified unchanged. New
cvars: `voxel.Server.MaxIntentsPerSec` (10), `voxel.Server.MaxCarveRadiusUU`
(400.0) — `VoxelDebug.h`/`.cpp`, same `TAutoConsoleVariable` pattern as
every other `voxel.*` cvar in this module. `MaxCubeSizeVoxels` itself stays
a compile-time constant (shared with client-side prediction clamping)
rather than a separate cvar, to avoid client/server drift.

**Verification.**

*Single-process standalone* (`-VoxelSeed=20260719`): run 1 —
`-VoxelHeadlessDigTest=3 -VoxelSaveWorldAfter=6` carved near spawn, then
saved: `SaveWorld: wrote 7863 entries (4024895 bytes) to
.../Saved/VoxelWorlds/20260719.vxlog -- editedDigest=0x9EA22D63D98BE8CD`
(no compaction triggered — a single carve already writes at most one entry
per touched brick, so raw==compacted here; the >2x gate correctly declined
to compact). Quit. Run 2 (relaunch, same seed, no dig) —
`LoadWorld: restored 7863 entries from .../20260719.vxlog --
editedDigest=0x9EA22D63D98BE8CD` — **exact digest match** against run 1's
pre-quit save line, plus two more independent `VoxelDigestDump` lines
(`role=Client` from the player controller, `role=Server` from GameMode —
both fire off the same `-VoxelDumpDigestAfter` switch in a standalone
process) also reading `0x9EA22D63D98BE8CD`. Zero ensures/fatal in either
run, clean shutdown both times.

*Networked rerun* (wave 1's gate scenario, seed 20260719, dedicated server +
2 headless clients via `UnrealEditor-Cmd.exe <uproject> -server` / `127.0.0.1
-game`, same fixed-column `-VoxelAutoDigAfter` probe): both clients dig,
server applies+broadcasts, all three dump matching digests —
```
VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client1)
VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client2)
VoxelDigestDump: role=Server seed=20260719 editedDigest=0x2451E40F5C935D2C   (server)
```
(identical to wave 1's own gate digest — same seed, same deterministic
probe). Server then saves (`SaveWorld: wrote 8 entries (261 bytes) ...
editedDigest=0x2451E40F5C935D2C`) and quits. **Server relaunched** (fresh
process, same seed) — `LoadWorld: restored 8 entries from
.../20260719.vxlog -- editedDigest=0x2451E40F5C935D2C` (matches the saved
digest) — then a **fresh third client joins** (no dig, join-sync only) and
requests join-sync; the server's `SerializeCompactedLogEntries (join-sync):
8 raw entries -> 4 compacted entries (112 bytes)` line proves compaction
ran on the outgoing copy, and both dump matching digests:
```
VoxelDigestDump: role=Server seed=20260719 editedDigest=0x2451E40F5C935D2C   (relaunched server)
VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (fresh client, joined via compacted sync)
```
Zero ensures/fatal across all five process logs (2 standalone + 3
networked). Every launched `UnrealEditor-Cmd.exe` process was waited on
(self-quit via its own `-VoxelDumpDigestAfter`/`-VoxelSaveWorldAfter`
timer) and confirmed exited; none needed to be force-killed, and a
post-run process sweep found no stragglers.

**Gates.** `voxelcore.lib` rebuilt clean in this worktree (`vxc_tests`
70/70 pass, `vxc_editlog selftest` PASS — both untouched by this wave, no
voxel-core source files were modified). `VoxelEarthEditor` Win64
Development builds clean via `Build.bat ... -WaitMutex -NoHotReloadFromIDE`
(zero warnings from any file touched this wave — `VoxelDebug.cpp`,
`VoxelEarthGameMode.cpp`, `VoxelEarthPlayerController.cpp`,
`VoxelWorldSubsystem.cpp`, plus header-only changes; same pre-existing
engine-header deprecation baseline as prior waves). Standalone behavior
without the new switches/commands is unchanged (load/save are additive:
`LoadWorld: no saved world ... starting fresh` is the observed behavior
whenever no save file exists yet, byte-identical to pre-wave-2 startup).

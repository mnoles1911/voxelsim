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

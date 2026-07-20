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

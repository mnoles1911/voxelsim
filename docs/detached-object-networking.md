# Detached object networking

The server owns detached-object chopping, geometry, fracture, motion, retention and deletion. Each connection receives registry records identified by stable GUID and monotonic revision; clients render replicas and never run authoritative cleanup or physics.

`UVoxelDetachedReplication` automatically attaches to game worlds. It enumerates remote player controllers, refreshes immutable registry snapshots at 10 Hz, and sends relevant objects within 250 m of the owning pawn. New connections receive an epoch reset followed by current geometry; reconnecting creates a new connection stream. Departing the interest area evicts the visual without creating a gameplay tombstone. Returning restores it. Authoritative tombstones cannot be resurrected by an older snapshot.

Geometry encoding and zlib compression run on a worker. Only one encode job is active at once; raw snapshots are capped at 512 MiB and compressed transfers at 64 MiB. Each reliable RPC carries at most 16 KiB of compressed payload. One application-acknowledged packet may be outstanding per connection, preventing an unbounded reliable queue. `voxel.Objects.NetMiBPerSecond` defaults to 2 MiB/s; actual throughput is also limited by frame cadence and acknowledgement round-trip time. This is a safety bound, not a measured delivery rate. Oversized or failed revisions are logged and withheld instead of retrying indefinitely.

Client staging checks protocol, epoch, sequence, object identity, revision, offsets, sizes and CRC. Decompression and snapshot decode run on a worker. The final acknowledgement waits until the game thread installs the actor successfully. Actor/material/mesh creation still runs on the game thread and can hitch for large objects; worker decoding does not remove that cost. The compressed transfer and raw decode allocations can coexist, so these caps are not a total memory budget.

An independent unreliable path carries up to 32 motion/removal records per connection every 100 ms, allowing existing objects to move while another geometry transfer is underway. Per-object revisions discard stale updates; reliable state delivery later supplies the final state after packet loss. Motion currently applies authoritative transforms directly, without interpolation. This can look stepped under latency.

The prototype axe routes strikes through an owned PlayerController RPC. The server validates finite direction, size 1–3, camera proximity to its pawn, a 650 ms cooldown and the shared intent rate budget. Tool entitlement still uses the existing world prototype axe flag: this is not a production server-authoritative inventory system. Terrain networking remains its existing separate protocol.

## Secure harness transport

The automated harness requires the session transport implementation and
`tools/create-session-transport.py`. It discovers `python` on PATH or accepts
`-Python <absolute-path-to-python.exe>`. Before launching, it creates two distinct
invites in its new ignored `Saved/Tests/<run>/private` directory. The server receives
only the server-key-file path; each client receives its own invite-file path and
a distinct profile name stable for that run. The connection URL contains only
the public invite ID as `EncryptionToken`, never a key or persistent credential.
Invite generation failure aborts the run; there is no plaintext fallback.

Keep the private directory and generated client profile credentials out of source
control and shared test artifacts. Existing replication assertions and the early,
ready, and completion barriers remain unchanged. The historical successful run
below predates this transport adaptation; a new encrypted runtime run is required
after merging and building the session implementation.

## Verification

`Voxel.Objects.NetworkAssembly` checks missing, duplicate, overlapping, oversized, cross-object and cross-revision fragments, byte identity and corrupt checksums. The registry test checks permanent tombstones and revision rejection. These unit tests do not substitute for a multi-process UE session.

Opt-in server command `voxel.Objects.NetFixture` waits for a player and creates a nine-cell harvestable resource. It moves 100 UU after 2 seconds, becomes retained after 4 seconds, and is deleted after 60 seconds. Join a second client between 4 and 60 seconds. `voxel.Objects.NetDump` prints stable IDs, revisions, geometry revisions, actor residency, retention and transform in each process for comparison. Confirm both clients receive the same resource, the early client sees motion, the late client sees its latest retained location, and both receive the tombstone. Reconnect and confirm the resource does not reappear. Fixture startup is explicit; ordinary sessions create no test resource.

`tools/verify-detached-network.ps1` automates an uncooked dedicated server and two hidden DX12 clients on port 17879, checks that no editor/compiler is already running, and uses a fresh seed without modifying existing player saves. The server waits for the harness to observe the first client's initial installed geometry before moving. The second client launches after the server has moved and retained the object. Deletion waits until both clients have reported live retained geometry and converged displayed poses; server exit waits until both have reported the real tombstone. Checkpoint labels 8/20/70 name observed states rather than assuming cold-client startup times. Unique local marker files control these fixture phases; all object state still travels over the real network. Gated observers have a 300-second wall-clock watchdog; manual ungated console behavior remains unchanged.

The harness asserts stable IDs, geometry revisions, movement, retained late-join state, displayed positions and deletion, then writes `result.json` beside preserved logs. Only processes created by that invocation are eligible for watchdog termination. Optional `-AllowOtherProjectEditors` permits explicitly identified different-project editors while retaining compiler, same-project and unknown-process guards. Such a run is functional validation, not a performance measurement.

Verified 2026-09-07 with `-TimeoutSeconds 480 -AllowOtherProjectEditors`: all three processes exited normally, authoritative identity/motion/retention/late-join/deletion agreed, and both client displayed-position errors were zero. Evidence: isolated checkout `Saved/Tests/detached-net-20260907-015619-9198275d/result.json` and its three process logs. This nine-cell transport fixture does not establish large fracture-chain or production ownership synchronization acceptance.

Also verify a real chopped tree/fracture chain, client intent rejection, a client leaving and returning beyond 250 m, packet loss, and a large compressed snapshot before treating this as production multiplayer validation. The fixture is intentionally small and does not measure forest-scale replication performance.

Encrypted integration verification: all three processes exited0 in `Saved/Tests/detached-net-20260907-031142-700ebdab/result.json`. Stable ID `0234303A40D0489CB60BF2BC9C253C73`, live retained geometry, early movement, late join and deletion agreed. Both clients used distinct invites and isolated profiles. All three observed-state barriers were preserved. This remains a nine-cell functional fixture, not a scale or production terrain-ownership benchmark.

# Persistent multiplayer transport

Implementation status: the dedicated-server/two-client encrypted restart fixture passed on 2026-09-07; see the execution ledger for the exact report and scope. Listen-server, duplicate-login and capacity checks remain open.

The session uses Unreal's AES-GCM packet handler with a distinct 256-bit key for each invited client. An invite proves possession of that transport key; display names and client-supplied player GUIDs do not prove identity. The server subsequently issues a separate per-world persistent-player credential over the encrypted connection. The client durably stores that credential before player admission.

Generate a new private transport directory with:

```text
python tools/create-session-transport.py <private-output-directory> --clients 2
```

The generator refuses an existing output directory and does not print keys. Keep `server.json` on the server. Give each player only their own `invite-N.json` through an authenticated private channel; do not put generated files in source control. These are reusable transport credentials, so a compromised invite should be removed from the server key index and replaced. A shared invite is not a distinct transport identity.

The server reads `-VoxelTransportKeys=<path-to-server.json>` or `Saved/Transport/server.json`. Each client reads `-VoxelTransportInvite=<path-to-its-invite.json>` or `Saved/Transport/invite.json`. The connection URL carries only `?EncryptionToken=<invite-id>`, the non-secret `id` field of the invite. It must never carry the key or the persistent-player credential. Malformed, missing and unknown keys fail closed; there is no plaintext credential fallback.

Offline single-player requires no transport invite. Online account-provider integration, automated private invite delivery and host-to-dedicated profile mapping are separate from this local invite mechanism. The per-world identity index and client profile files remain private runtime data and must be preserved independently of a gameplay rollback.

Multiple local clients can select distinct profile directories with `-VoxelPlayerProfile=<name>`. The name is validated as a safe slot name; omitting it preserves the default profile location. The encrypted restart fixture supplies a unique profile name per client and reuses it after the server restart:

```text
python tools/verify-session-network.py --players 2
```

The fixture creates its own synthetic seed and private invite files, and closes only the processes it starts. Its report checks encrypted admissions, distinct durable IDs, inventory counts, selection, pawn position and fly mode across restart. A successful fixture run must be recorded separately; implementation of the harness is not evidence that the network gate passed.

Use `--server-mode listen` to exercise a rendered listen host with the same remote-client restart checks. The host runs with its persistent local host identity; these remote-client assertions do not themselves verify the host inventory.

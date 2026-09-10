# Session state ownership

This inventory tracks the enabled runtime owners found during persistence implementation. A completed adapter must validate its entire input before admission and participate in the same checkpoint generation. GPU buffers, meshes, navigation paths and replication channels are reconstruction work.

| Owner | Authority that must survive | Current adapter |
| --- | --- | --- |
| VoxelWorldSubsystem | Seed/provider identity, terrain overlay and detached-object transaction results | Terrain log and object registry checkpoint |
| Detached persistence / object registry | IDs, revisions, geometry, transforms, velocities, residency, cleanup policy/timers and tombstones | Registry snapshot and immutable geometry pages |
| Water subsystem | CA fills, mobilization, basin ledger, river graph/region, routing remainder, implicit ocean selection | Water + hydrology + clock domains |
| Sky subsystem | Epoch, rate, calendar | Clock domain; advances while sky rendering is disabled |
| Agent subsystem | Persistent IDs, position, velocity, tier, standoff state, mining cooldown | Bounded agent records; paths and render instances rebuild |
| Player controller / inventory | Persistent ID, slots including empty slots, selection, offline records, transform | Player records; host admission and encrypted remote credential handshake |
| Fly pawn / custom character mover | Walk/fly mode, actual movement velocity, stance, speed settings, jump buffer/coyote timing | Motion record, rather than generic PawnMovement velocity |
| Boat | Authored hull identity, transform, physics velocities, sleep/ground state, scripted throttle timer, pilot relationship | Connected actor adapter; live reconstruction tests pending |
| Glider | Authored wing identity, transform, integrated velocity/attitude, parked state and pilot relationship | Connected actor adapter; live reconstruction tests pending |
| Thrown item | Item/count, transform/velocity, settled/flight/return timers, persistent owner | Connected actor adapter; timeout can credit an offline player record |
| Explosive | Transform/velocity, landed state, already-rolled blast radius and remaining fuse | Connected actor adapter; fuse resumes from remaining gameplay time |
| Craft volume / ownership | Promoted geometry and material ownership if instantiated | No `replayCraft`, craft log or craft ownership call found in the active WorldSubsystem; core-only codec fixtures are not a runtime domain |
| Weather / fluid presentation | Wind is sampled deterministically from seed, position and sky epoch; wave inertia is visual | Authority-changing weather controls and disabled-sky state publication still need compatibility coverage |

The game-thread end-of-world-tick autosave callback captures one pending request per world. Manual saves still capture synchronously on the game thread before their immutable worker submission. All physics and delayed authority callbacks must be audited against that boundary before declaring the transaction fence complete.

## Identity and transport

Remote profiles use server-issued random bearer credentials. Only BLAKE3 credential digests are stored server-side. A separate atomically published per-world identity index survives rollback of gameplay generations. The client profile is scoped to server endpoint and persistent world ID. A new identity is durably indexed before its credential is issued, and admission waits for the client's durable profile acknowledgement. Duplicate active/pending identities are refused. Unadmitted connections have no pawn or inventory mutation authority and cannot block autosaves.

Credential exchange refuses an unencrypted Unreal connection. Configuring an authenticated encryption handshake/transport and exercising encrypted multi-process login remain required validation work; no fallback sends credentials over the default plaintext connection. Tests must not log or commit generated profile credentials.

## Open acceptance gates

Actor adapters and relationship restore; world recreation for selected-seed load/retry; shared join baseline/acknowledgement; bounded retention with reader leases; real shutdown/crash recovery; secure remote transport configuration; 16-player and large-world budgets. These are not implied complete by individual codec or build successes.

The connected actor format refuses placeholder hulls and pins authored hull bytes by BLAKE3. It currently limits one checkpoint to 8,192 gameplay actors, 16 MiB of actor records and 128 MiB per source asset. These are enforced development limits, not measured capacity claims. Rendering instances, wake history and camera offsets rebuild; they are not player inventory or vehicle ownership.

# Bounded stationary authority core foundation

`StationaryAssetAuthority<B>` in `voxelcore/assetauthority.h` is an explicit,
immutable captured-domain query object. It is not installed into GeneratedWorld,
World, UE collision/mining, or the visual pilot. Existing game behavior is unchanged.
The unsafe seed-only overloads in the earlier ignored draft were not applied.

Capture validates the actual World's amplifier/field seed and edit-log provider
identity, canonical source provenance (bank, seed index, yaw, layer, anchor),
provider/worldgen fingerprint, catalog/content fingerprint, source hash, matching
projection transform and nonzero equal revisions. `IAssetAuthorityIdentity` is a
trusted host service: it must return the registered catalog identity for the actual
loaded field, and delegate content hashing to the existing canonical MD5
`GridContentHash`. Unknown fields must return empty. The core tests use a clearly
marked fixture digest service; there is no production adapter or claim that the
core can reconstruct a manifest identity from an arbitrary field by itself.

The capture enumerates the complete brick-aligned domain, including one neighboring
brick on each side of the source. `assetAuthorityInstances` preflights terrain-layer
site ranges before reserving a vector or calling column providers. It skips detail
layers before site generation, preserving original terrain first-winner order.
Worst-case visits are capped at8192, source dimensions64x64x128, captured cells1Mi,
resolved instances64, and composition64Mi probes. Source and current projection
are validated before dense allocation. An existing terrain overlay or craft
promotion anywhere in the captured domain refuses capture. Bank resolution and
digest services remain trusted synchronous dependencies; their internal cache/
allocation policies are outside this per-capture budget.

After capture there are no World, provider, bank, grid or UObject pointers. Private
material arrays retain filtered terrain, original composition-clipped source and
current projection. Every sample stays inside one immutable generation. Queries
outside the domain return false and never fall back to another same-seed World.
Terrain samples and makeBrick exclude the active source's original winner without
revealing a later overlapping source. Base amplifier terrain is preserved. Gameplay
samples separately add the current object projection and return its stable owner ID,
generation and object revision.

`carve(expectedGeneration, ...)` and `editTerrain(expectedGeneration, ...)` produce
new immutable views; old readers retain the old generation. Terrain edits carry an
explicit per-cell present bit, so air is an override only at the edited cell. A
materialized terrain brick's unrelated air does not erase the object projection.
Projection edits never enter terrain materialization. A stale expected generation,
out-of-domain coordinate or invalid carve refuses without modifying old data.

Material storage is at most five bytes per captured cell with today's one-byte
MaterialId/two-byte override; `retainedMaterialBytes()` uses actual sizeof values.
New revisions share immutable terrain/source arrays and copy only changed projection
or overrides. Accounting excludes vector/control-block overhead and shared storage
may be counted more than once between views. This is a per-view/capture bound, not a
global retained-generation quota. A production coordinator must bound retained old
views, reserve replacement memory, and revalidate World edit/provider/catalog epochs
before publication. Capture requires a frozen World/field/service during its
synchronous construction; it is not a synchronization primitive.

Validation: focused `vxc_authority_tests` covers real World/AssetField source capture,
carve followed by adjacent terrain edit, terrain point/brick agreement versus
aggregate object samples, explicit air overrides, old generation consistency, source
lifetime, same-seed/different-provider refusal, unknown catalog binding, invalid
revision/yaw/extreme coordinates, preexisting overlay refusal, preallocation site
refusal before any column callbacks, detail-layer exclusion, and actual overlapping
source captures with earlier/later ownership and base terrain preservation.

Remaining production work: real canonical identity adapter, global admission and
generation leases, integration of terrain-domain generation/edit/replay and aggregate
raycast/collision/mining APIs, explicit edit-log provenance/serialization, observers,
restore/demotion transactions, moving/fine projections and multiplayer. This core
foundation does not activate or save ownership in the game.

## Retained-generation coordinator

`StationaryAssetAuthorityCoordinator<B>` is the separate owner/publication layer for
this bounded foundation. It reserves a conservative maximum full-generation credit
before initial capture or any projection/override array copy. At most64 generations
and512MiB configured storage credit are accepted; smaller caller limits apply.
Shared arrays are conservatively charged to each generation, and a64KiB per-view
margin covers known small state/ticket metadata. This accounting controls material
storage and known metadata, not arbitrary host identity/bank cache allocations or
allocator bookkeeping. Initial construction still has its separate bounded site
and callback work contract.

Each facade holds its own credit. Arrays are destroyed before that credit releases;
worker-retained old views keep admission closed until their last reference drops.
Editing a coordinated view directly also reserves a new credit. Cancelling or
consuming a prepared ticket clears its held base/candidate references, so retaining
spent ticket handles cannot retain uncharged material arrays. Read views and credits
outlive a destroyed coordinator safely; its closed ledger refuses subsequent edits.
No external raw owner pointer participates in retirement.

Prepared tickets bind an exact base Ref, coordinator identity and publication epoch.
Publication validates all three before assigning the visible immutable view. A
competing publication invalidates other tickets; stale/cross-coordinator tickets
cannot publish. Coordinator mutation APIs are single-owner-thread; only ledger
reservation/release and immutable view lifetime are worker-safe. Prepared publication
is a core pointer handoff, not the game's renderer/collision/save transaction.

The coordinator fixture covers budget/count exhaustion, direct-edit admission,
release/retry including last-view release on a worker thread, stale expected base,
competing prepared publications, cross-coordinator rejection, failed-capture credit
rollback, repeatedly retained cancelled tickets, and coordinator destruction with
an outstanding immutable reader. Production still needs to bind this coordinator
to actual world epochs, canonical identity adapter, edit/replay and render publication.

## UE identity adapter prerequisite (source written; UE validation pending)

`VoxelEnvironmentAuthority::FIdentity` registers one actual session field against
its loaded manifest bytes. It independently parses the manifest, derives the
placement species table and tightened layers, and compares explicit member values
(no padding-dependent struct hash). Unknown field addresses, changed seed/tables,
invalid manifests and bounded-size failures return no identity. Catalog identity
is the exact loaded manifest MD5 used by the existing production candidate path;
source identity delegates the existing canonical GridContentHash MD5, with terrain
format/size admission before hashing. Registration is immutable and owned by World
after its field, so destruction precedes field teardown. Calls require a frozen
field; this adapter does not implement concurrent hot-reload synchronization.

World now stamps terrain/craft edit logs from the actual created fine streamer's
ProviderId accessor. A configured ID whose streamer failed to construct does not
become a false authority identity. The production provider string is derived from
that same World stamp. Terrain replay passes the current stamp explicitly: legacy
unstamped logs remain accepted; stamped mismatches are refused and preserved by
the existing save guard. Existing seed/worldgen parser checks remain intact.
Without a live fine streamer, the World stamp stays empty and the authority capture
continues to refuse. Coarse-only provider identity is not invented by this patch.

Two source-only UE tests are EnvironmentAuthorityIdentity (including independent
known canonical MD5 and compression-insensitive hash) and EnvironmentAuthorityProviderReplay
(legacy accepted, wrong provider refused without mutation, matching accepted,
seed/worldgen mismatch refused). No UE build/runtime has run for these additions.
The adapter is registered but no authority coordinator/capture is created in game.
Production promotion, epoch binding, replay of ownership and publication remain off.

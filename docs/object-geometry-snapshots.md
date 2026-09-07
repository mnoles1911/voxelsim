# Immutable geometry snapshots for world objects

Date: 2026-09-07. Actor-side implementation; world registry, worker scheduling,
streaming and network orchestration are integrated by the parent task.

Both `AVoxelEnvironmentLODPrototype` and `AVoxelFallingTimber` expose:

```cpp
bool CaptureObjectState(FVoxelImmutableGeometry& Geometry, TArray<uint8>& Dynamic);
bool RestoreObjectState(const TArray<uint8>& Geometry, const TArray<uint8>& Dynamic);
```

`FVoxelImmutableGeometry` is a thread-safe shared pointer to a **const** byte
array. Capture and restore must run on the game thread. Once capture returns,
a worker may retain/read those immutable bytes without touching the actor.
The actor's cache member is itself game-thread-only; thread-safe reference
counting does not make concurrent UObject access legal.

## Save cost and invalidation

Environment geometry stores the finest occupancy buffer. Timber stores packed
section vertices, indices, invariant relative transforms and fracture-cap
visibility/tags. Derived environment LODs are rebuilt after restore.

Geometry is serialized at initialization/rebuild and after actual environment
carves or detachment, not at every save. Legacy restore also initializes the
cache. Fracture children build independent caches after receiving their mesh
sections. Rigid motion changes only the small dynamic header and does not
invalidate geometry. Capture returns false if initialization/serialization did
not establish a valid cache; it does not silently serialize a large object in
the save call as a fallback.

Dynamic data preserves the existing actor header: environment transform and
grid metadata, or timber transform, velocity, hinge state, age, cleanup state
and associated small fields. Subsequent captures perform O(metadata) work and
share the existing geometry allocation. A pending save keeps the old immutable
allocation alive when a later edit replaces the actor cache.

The initial cache build and post-edit refresh are still synchronous and copy
the full finest buffer or pack mesh sections. This moves cost away from repeat
saves; it does not eliminate mutation-time cost or make enormous actors cheap.
Verbose `ObjectGeometry CACHE` logs expose serialized byte size and CPU time.
Chunked/copy-on-write source storage is a future optimization if edit latency
requires it. Save/network workers must avoid concatenating those blobs back on
the game thread; only the restore compatibility bridge does that once.

## Compatibility

Geometry and dynamic arrays each begin with uint32 format version 1. After
removing those prefixes, `Dynamic + Geometry` is the existing actor record.
Timber geometry uses the packed mesh custom version introduced in legacy v3.
Restore reconstructs that record once and routes through the existing validated
reader, including its grid/mesh bounds checks. The original `PersistentState`
entry point and v1–v3 reading behavior remain available for legacy sidecars.

The envelope rejects unknown versions, truncated prefixes, dynamic headers
over 4 KiB and combined data over 512 MiB. Complete legacy-reader consumption
is required. The parent registry records the object kind and geometry format;
format 0 remains an unsplit legacy record.

## Mesh lifetime during streaming

Promoting a section to falling timber now transfers its UObject outer using
`UActorComponent::Rename`, as well as attaching it to the new body. Unreal's
`PostRename` updates the old and new actors' owned-component lists. Attachment
alone was insufficient: the old stump would still own the section and could
destroy it on unloading. Ownership is transferred again when the initial
timber breaks into two child actors.

## Checks

`Voxel.Objects.GeometryEnvelope` validates record reconstruction,
prefix removal, version refusal, truncation and the dynamic payload bound.
Compilation and actor-level save/reload/streaming checks are coordinated with
the parent task; this document does not claim they passed before that run.

## Staged restoration

The actors now also expose `BeginStagedObjectRestore`,
`AdvanceStagedObjectRestore`, `CancelStagedObjectRestore`, and
`PublishStagedObjectRestore`. Begin takes the immutable geometry pointer,
dynamic bytes and a game-thread completion callback. The bridge supplies a
fresh unpublished actor and retains its pending identity/revision.

Packed timber decode and validation run on a worker. Environment decoding,
wood counts, LOD reductions and all section mesh generation also run on a
worker. Workers do not dereference UObjects. Decoded arrays stop changing at
the queued game-thread handoff; the game thread then exclusively adopts them,
avoiding large source-grid copies. The original serialized geometry allocation
is retained as the actor's immutable cache without reserialization.

Advance publishes at most one original spatial/packed mesh component, or a
small setup/finalization step. It has no per-actor timer: the bridge enforces a
global step count and soft CPU budget across jobs. An individual component
upload is indivisible, so this is not a hard millisecond guarantee. The
original mesh-section boundaries and fracture caps are preserved.

Completion(true) means the geometry is ready, still hidden and without physics
or collision publication. Only Publish makes the object visible after the
bridge's final revision check. Timber physics, hinge, velocity and lifetime
are restored on authority; clients remain non-simulating. Capturing an
unpublished actor is refused. Cancellation is checked between decode sections
and environment mesh blocks, destroys partially created components, and fires
the pending completion once with false. World/actor teardown cancels work;
late worker completion is ignored through weak-actor and job-identity checks.

`Voxel.Objects.StagedRestore` is focused latent automation covering tiny real
actor snapshots, hidden/frozen readiness, explicit publication, cancellation
after partial setup, geometry allocation reuse and restored source collision.
Its execution results belong to the parent task's build/test run.

## Ground support before physics publication

`VoxelTreeFelling::AdvanceRestoreGround` incrementally prepares the temporary
terrain collision bridge used by timber. It checks the full fine-tier
footprint is resident before sampling, does not request missing tiles, samples
at most 64 points with a soft 1 ms allowance per call, and creates at most one
16 by 16 cell collision tile. Independent PMCs avoid repeatedly cooking all
previously added collision sections. One complete 32 m proxy uses 100 tiles at
200 mm sampling. Existing complete proxies can be reused only when their
bounds cover at least 14 m around the requested point. Partial proxies are
excluded from both the staged and legacy reuse checks.

The bridge must keep physics frozen until this call returns true. This avoids
both the previous monolithic ground rebuild and bodies falling through a
world whose production terrain still has no general Chaos representation.
`EnsureAxe` restores the tool independently of ground preparation.

Staged ground work is cleared on world teardown and capped at 32 cached proxies
per world. At capacity, one far unused completed or abandoned partial proxy can
be reclaimed per call, with new admission deferred to the following step.
Reclamation protects proxies near players (100 m), live/pending oak prototypes
(35 m), the requested location (64 m), and any overlapping live/pending timber
bounds. Pending timber receives 14 m of bounds padding, and the bridge sets
its intended transform before dispatch. If all cached proxies are protected,
the new body remains pending rather than removing required ground. Ground is
still a sampled prototype collision bridge; it does not automatically rebuild
for later terrain edits. These limitations are separate from successful staged
mesh restoration.

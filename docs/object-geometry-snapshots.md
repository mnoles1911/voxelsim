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

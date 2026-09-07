# Detached object registry and residency

`VoxelObjectRegistry` stores stable GUID identities and increasing object and geometry revisions independently of actor lifetimes. An immutable, shared byte array owns geometry; snapshots copy the handle and small dynamic state. Replacing geometry leaves an in-flight save's old data intact. A destroyed object becomes a tombstone, and imports cannot resurrect that identity during the world session.

The registry is game-thread owned. Its snapshot geometry handles may be passed to workers; registry methods and weak actor pointers may not. Import requires strictly newer revisions. A world/session change must discard the old registry with `Forget` before importing the new world's state. Network implementations must additionally validate the authoritative world epoch.

Residency uses 160 m loading and 200 m unloading distances from the object's bounds, with the closest of all player views determining residency. Trees can remain upright at their existing placement; movement between regions uses floor division into 256 m regions, including negative coordinates. `RegionFor` provides the common spatial key for regional save/replication grouping. The current scheduler rotates through at most 128 entries per call and performs at most two restoration/eviction attempts, including failed attempts. This bounds actor transition count, not the cost of a single large actor reconstruction.

Active physics and pinned ownership dependencies cannot be evicted. Root integration supplies capture, restore, and eviction callbacks. Capture must populate current geometry, dynamic state, transform, bounds, velocities, lifetime, and physics activity. During an eviction callback the entry is explicitly `Evicting`; actor EndPlay must preserve that entry rather than tombstone it. The callback may destroy the actor only once all dependent component ownership has been resolved. Retained objects may become dormant but cannot expire.

Dormant harvestable objects preserve their remaining gameplay lifetime and continue counting it down while abandoned. Player proximity and the same conservative view-cone protection as live debris pause cleanup. Large objects and retained objects do not expire. Dormant timer sampling accounts for the complete elapsed gameplay interval between scheduler visits. Wall-clock/offline time does not count. A zero-player session conservatively retains live actors; dormant timers still advance when the caller advances gameplay time.

`Voxel.Objects.Registry` automation checks stable identity, strict revision ordering, immutable geometry replacement, negative-region boundaries, load/unload hysteresis, active-body/pin exclusions, timer preservation and expiry, permanent tombstones, stale restore callbacks, and bounded inspections/restores.

This registry does not by itself implement disk paging, asset-library deduplication, arbitrary ordinary terrain-stamped tree ownership migration, or bounded mesh reconstruction within a single restore. Integration layers own those capabilities; shared geometry handles do not make a monolithic actor rebuild asynchronous.

## Integrated paging and restoration

`VoxelObjectPages` now supplies bounded worker IO and verified disk references. `VoxelObjectRestore` supplies the four-job global staged actor queue, with a two-step/soft-2-ms frame budget. The registry's optional `BeginRestore` callback keeps a record `Restoring`; `CompleteRestore` accepts only the matching geometry revision, and cancellation returns it to dormancy. Lifetime changes during restoration do not invalidate the geometry job. The scheduler preserves its cursor when transition attempts exhaust the budget, so repeatedly failing entries cannot starve later ones.

See `detached-object-integration.md` for measured verification and remaining production renderer/physics limits.

# ADR-0005: Persisting water — the CA fill is irreducible state, and the mobilized set cannot be saved without it

> **STATUS 2026-08-09: [CURRENT].** Shipped CA persistence design (`WaterState`).
> Stays load-bearing for the frozen CA (bucket pours, cave floods). The
> re-architecture's scalar hydrology (basin ledgers, routing graph) needs its
> own persistence, on the same "irreducible state, not re-derivable by replay"
> argument this ADR made first — read it before designing that. See
> `docs/water-architecture.md`.

- **Status:** **ACCEPTED** (Matt, 2026-07-22) — implemented as `WaterState`
  (`voxel-core/include/voxelcore/waterca.h`).
- **Date:** 2026-07-21
- **Doctrine sections affected:** **§2.1** (never replicate voxels; seed +
  edit-log diffs only). This ADR argues that §2.1 is a *replication* rule and
  that persistence is a different axis, but the distinction is load-bearing
  enough that it should be Matt's call rather than mine.
- **Human sign-off:** **GRANTED by Matt, 2026-07-22.** He took the
  recommendation below: persist water state, on the reading that §2.1
  constrains the wire rather than the disk. The counter-argument stands on
  record — this is the first simulation state we persist, and the SWE grid
  and collapse scheduler will invoke the precedent. Those remain out of
  scope and need their own decision; adopting this one does not pre-approve
  them.

## Context

The backlog carried an item reading "`mobilizedBricks` savegame serializer" —
apparently a small, mechanical piece of work: write out a set of brick keys,
read it back, call `markMobilized` for each. `waterca.h` even names that path
explicitly, and `markMobilized`'s doc comment says it is "the savegame-load and
client-replication path".

**Built literally, that serializer silently destroys every underground lake in
the world on the first reload.** This ADR exists because the small task is not
small, and the reason it is not small is a genuine gap rather than an oversight.

## The trap

Underground water is a static implicit field (`WaterMobilizer`) that costs zero
storage until a player disturbs it, at which point the affected brick
*mobilizes* and the CA takes ownership. Two code paths make ownership total:

```cpp
// waterca.cpp:1195 — once mobilized, the implicit field contributes nothing
uint8_t WaterMobilizer::implicitFillAt(int64_t vx, int64_t vy, int64_t vz) const {
    if (mobilized_.count(waterKeyForVoxel(vx, vy, vz)) != 0) return 0;
    return sourceFillAt(vx, vy, vz);
}

// waterca.cpp:1209 — and an unmobilized cell reads SOLID to the CA
if (mobilized_.count(waterKeyForVoxel(vx, vy, vz)) != 0) return MAT_AIR;
return implicit_(vx, vy, vz) != 0 ? MAT_ROCK : MAT_AIR;
```

That design is *correct* and is the thing that makes double-ownership
structurally impossible — an unmobilized lake is a wall, so the CA physically
cannot write into water the implicit field still owns.

But it means mobilization is a **one-way surrender of the only remaining record
of where the water was**. Restore the mobilized set without restoring the CA
fill and every mobilized brick reports: implicit field → 0 units, CA → 0 units,
solidity → `MAT_AIR`. The lake is not merely empty, it is *gone*, and the cave
it filled now reads as open air.

`markMobilized`'s comment is not wrong about this — it says the units "are
already present in the loaded/replicated CA fill". That is a **precondition**,
and on the persistence path nothing satisfies it: **there is no CA fill
serializer anywhere in voxel-core.** `setReplicatedFill` is an inbound network
API with no on-disk counterpart. The replication path has its other half; the
persistence path never got one.

## Why the water cannot simply be re-derived

The obvious doctrine-preserving answer is to persist nothing and re-derive, as
we do for terrain. It does not work, in both directions:

**Persist nothing.** On load the mobilized set is empty, so every brick reverts
to the implicit field — which is a pure function of the seed and is therefore
*full*. Note the edit log does not save us here: `sourceFillAt` gates the
implicit field on terrain being `MAT_AIR`, and draining a lake leaves the cave
air, not rock. So a player who spends an evening cutting a drain shaft and
emptying a cavern logs back in to a full cavern. **The single most satisfying
thing a player can do to underground water is also the only thing the save
format would refuse to remember.**

**Replay to re-derive.** Water state *is* deterministic given seed + edit log +
tick history, so it is in principle reproducible. But it is reproducible only by
replaying every tick since world creation — unbounded work that grows without
limit, and which would have to be re-run before the first frame. Terrain is
re-derivable in O(1) per column; water is not re-derivable in bounded time.

**Conclusion: CA water fill is irreducible simulation state.** It is not a
cache, and unlike terrain it cannot be recomputed from the seed. Either it is
persisted or the behaviour it represents is not persisted.

## The doctrine question

§2.1 says: never replicate voxels; ship seed + edit-log diffs. Persisting an
array of per-cell water fill looks uncomfortably like persisting voxels, which
is why this is an ADR and not a commit.

The argument that it does not deviate:

1. **§2.1 constrains the wire, not the disk.** Its rationale is bandwidth and
   authority — a client must not be fed a voxel dump it could derive. Nothing in
   it is about what the authority writes to its own save file. The savegame
   already stores something non-derivable (the edit log); this adds a second
   non-derivable thing.
2. **Water fill is not a voxel material.** It is not in the material palette,
   the mesher does not read it as terrain, and it never enters the edit log. It
   is solver state, closer to the SWE grid than to a block.
3. **It is bounded by player activity, not by world size.** Only mobilized
   bricks are stored, and bricks mobilize only where players disturb water.
   An untouched world stores zero bytes — the same property that made the
   implicit field worth building.

The honest counter-argument: this is the first time simulation state gets
persisted at all, and once the precedent exists, the SWE grid, the collapse
scheduler, and the region graph will each want the same exemption. If Matt wants
that line held, the alternative is to accept that underground lakes reset on
reload, which I think is the wrong trade but it is a legitimate one.

## Recommendation

**ADOPT** persistence of water state, as a blob separate from the edit log:

- Contents: (a) mobilized brick keys, (b) per-brick CA fill for every brick with
  any nonzero fill. Both are sparse and both are already `BrickKeyLess`-ordered,
  so the format is deterministic without extra sorting.
- Versioned under **`kWaterCAVersion`**, not `EditLog::kFormatVersion` — water
  state must be invalidatable independently of terrain edits, exactly as
  ADR-0004 put the SWE golden under an independent `kSweVersion` so SWE changes
  could never invalidate a water golden.
- Load order: **fills first, then `markMobilized`.** The final state is
  order-independent, but filling first means the world is never momentarily in
  the both-report-zero state described above — which matters if load is ever
  interrupted or made incremental.
- The round trip must be provable, not assumed: `WaterMobilizer::digest` already
  digests the mobilized set in sorted key order; the test should assert that
  save → load → digest is **byte-identical**, and that total volume is
  conserved across the round trip. A save/load cycle that silently loses water
  is precisely the failure this ADR was written to catch, so it gets a test
  that fails loudly rather than a comment.

**Explicitly out of scope** until Matt rules: persisting the SWE grid (ADR-0004
deferred enabling the coupler to M3 anyway, so there is no live SWE state to
lose), and persisting the collapse scheduler.

## Status of the work

This ADR was written *instead of* the backlog item, because implementing the
item as written would have shipped a silent lake-destroying bug behind a green
test suite — the mobilized-set round trip would have passed perfectly while the
water it referred to vanished.

**Implemented** (voxel-core, `WaterState` in `waterca.h` / `waterca.cpp`,
tests in `test_waterca.cpp`):

- The serializer, carrying CA fill + the active set + mobilized keys as a
  standalone blob with its own magic and its own `kWaterCAVersion` stamp, kept
  deliberately separate from the edit log so water is discardable without
  discarding terrain edits. Container format is versioned independently of the
  CA version so the byte layout can evolve without a world-breaking bump.
- Load applies **fills first**, then the active set, then `markMobilized`.
  Parse decodes and validates the whole blob before applying anything, so a
  truncated or tampered save fails cleanly and never half-loads; `totalVolume`
  is stored redundantly and cross-checked against the fills actually decoded,
  so a lossy cycle is loud in production and not only under test.
- The round-trip test asserts both digests are byte-identical and that
  implicit + CA volume is conserved, plus a mid-flow save that must resume to
  the same end state as an uninterrupted run (which is what persisting the
  active set buys). `waterca_state_mobilized_keys_alone_would_destroy_the_lake`
  keeps the trap itself executable: it demonstrates the naive serializer
  zeroing the lake *while its mobilized-set digest round-trips perfectly*.
- Payload encoding reuses the edit log's `kSparse`/`kRle` split rather than
  inventing a third scheme, and the choice was **measured** on four real
  scenarios, not assumed. `kRle` carries almost everything (11-21% of
  dense-only); `kSparse` earns its place on thin vertical water (a drain
  shaft — 8 B vs 17 B) and *not* on water in motion, which turned out to still
  be sheets; `kDense` wins none of the four and is kept only as the
  never-worse-than-raw floor.

**Ships inert.** Nothing in voxel-core writes a file by itself — the blob is
produced into and parsed out of a caller-owned buffer, exactly like `EditLog`.

**Still open:** the UE-side hook that writes the blob alongside the edit log on
save and feeds it back on load, including the decision of what to do when the
water blob is missing, stale (`kWaterCAVersion` moved), or refused (fall back
to a fully implicit world — coherent, but every drained cavern refills).

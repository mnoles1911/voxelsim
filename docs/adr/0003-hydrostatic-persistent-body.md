# ADR-0003: Hydrostatic pass — cross-tick terrain-solidity memo, and deferral of the persistent per-water-body structure

- **Status:** proposed
- **Date:** 2026-07-21
- **Doctrine sections affected:** none by the code that ships with this ADR
  (the memo is OFF by default and byte-identical when on). Two things in here
  DO need a decision: (1) enabling the memo on the live UE water subsystem,
  which imposes a new obligation on `UVoxelWorldSubsystem`'s edit paths;
  (2) the "should the overflow cap count air?" question, which would be a
  world-breaking `kWaterCAVersion` bump.
- **Human sign-off:** PENDING — see "Decision" below.

## Context

`WaterCA::hydrostaticPass` (Phase C, `voxel-core/src/waterca.cpp`) is the
connected-volume level equalization that makes U-bends and communicating
vessels work. It rediscovers its components by flood fill EVERY tick. Water-side
reach is deliberately unbounded (`waterca.h`, "Phase C" step 1) — a settled body
must stay in the volume accounting even after it stops changing — so one touched
brick anywhere on a body drags the whole body back into the flood.

Two prior perf passes are already banked (docs/status.md, W2): the two-phase
read/apply rewrite, and a ~3x byte-identical access-cost rewrite of the flood
itself (per-brick cache, deferred writes, no-op-write skip, raw `solid_`). That
second pass profiled the residual and found **97-98% of flood cell-pops are AIR**
(~900K/tick on the 441-column bench), with the dominant cost being the ~1us
terrain `solid_` query over that air shell — not the flood machinery. It then
filed the "real" fix (a persistent/incremental per-water-body structure) as
blocked on byte-identical grounds, needing a design pass rather than a silent
perf tweak. This ADR is that design pass.

### Verifying the two previously-filed blockers

Both were re-derived against the code, not taken on faith. **Both are real, and
both are less fatal than filed** — but a third, sharper blocker replaces them.

1. **"The 65536-cell overflow cap counts AIR."** CONFIRMED: the flood pushes air
   cells into `cells` and trips `overflowed` on `cells.size()`, so a component's
   level-vs-skip decision genuinely depends on this tick's `touched`. But this
   does not block a persistent structure, because the count DECOMPOSES: it is
   (persistent water-component size, maintainable incrementally) + (air cells
   found this tick). It only blocks the naive shortcut of skipping untouched
   bodies outright.

2. **"Air bridges water."** CONFIRMED: a full (255) cell can step into gated air
   and back down into another region's water, making them one component; so
   component identity is not water connectivity, and persistent union-find over
   water alone under-merges. But note gate (c): an air cell is explorable ONLY
   if its owning brick is in `touched`. **Every air bridge therefore lies inside
   the touched region**, which is exactly the region we are already paying to
   walk. A persistent water union-find, unioned per tick with the bridges found
   by the touched-confined air walk, reproduces the exact partition. Also not
   fatal.

3. **The blocker that actually bites (new).** Two things, neither previously
   filed:
   - **Splits.** Union-find supports merges, not deletions. A water cell
     draining to 0 can split a body, and detecting/repairing that correctly
     costs a re-flood — so the WORST case of a persistent structure is today's
     cost plus guard overhead, not better.
   - **The visited obligation.** Even a component we can PROVE is unchanged must
     still have its cells marked visited, or a later seed inside it starts its
     own partial sub-component and emits different writes. Restoring visited is
     O(cells) unless per-brick visited masks are cached, which is O(bricks)
     (a 64-byte memcpy per brick) — real, but it means the persistent structure
     must carry per-body, per-brick mask state, not just a cell count and a
     volume.

### What actually costs the time (measured, this pass)

`vxc_waterca_bench` gained `--solid-cache`, `--count-queries`, and a new
`--lake` mode (a fully settled walled pool disturbed by one unit of water per
tick — the persistent-structure motivating case, which the 441-column pour does
not represent). `--lake-solid-spin` emulates an expensive terrain query, because
the basin predicate costs nanoseconds while the live engine's SolidFn
(`IsSolidAtVoxel` -> `World::materialAt` -> Amplifier) is ~1us.

| Scenario | memo off | memo ON |
|---|---|---|
| 441-column pour, real Amplifier terrain (full-run avg) | 329-345 ms/tick | **111-121 ms/tick** |
| terrain `solid_` calls, same run | 40.8M | 4.7M |
| settled 63x63 lake, spin 3300 (~1us/query) | 9.2 ms/tick | 6.5 ms/tick |
| settled 127x127 lake, spin 3300 | 10.6 ms/tick | 6.8 ms/tick |
| settled 63x63 lake, cheap query (spin 0) | 2.8 ms/tick | 2.6 ms/tick |

Two conclusions, and they point in the same direction:

- **The terrain query is the whole story in the active case.** The memo takes the
  pour bench 2.8x faster, to *below* the pre-Phase-C v2 baseline (~140-155 ms) —
  i.e. Phase C is now cheaper than the pass it was a regression against.
- **The settled-lake case is NOT expensive, and most of what it does cost is the
  terrain query the memo already removes.** A settled body barely explores air at
  all: once it settles, `touched` shrinks to the disturbed corner, and gate (c)
  confines air exploration to `touched`. The residual ~6.5 ms/tick is water
  traversal — precisely the persistent structure's target, and it grows only
  sub-linearly (2.8 -> 6.3 ms as lake area grows 38x, because large bodies trip
  the overflow cap and get skipped).

## Options considered

- **A. Cross-tick terrain-solidity memo (SHIPPED, off by default).** Memoize
  `solid_` per voxel in per-brick 512-bit known/solid masks, persisting across
  ticks, with explicit invalidation. Memoizing a pure function cannot change its
  answers, so it is byte-identical for as long as the memo agrees with `solid_`.
  Cheap (~830KB of masks on the pour bench), simple, and it attacks the measured
  dominant cost. Risk is entirely in the caller contract (below).
- **B. Persistent per-water-body structure (DEFERRED, designed below).** The
  theoretically-correct O(1)-settled fix. Real, and byte-identical in principle
  via the replay-skip invariant below — but a genuine redesign carrying per-body
  footprints, per-brick visited masks and content stamps, whose measured prize is
  the ~6.5 ms/tick residual, not the ~230 ms/tick the memo already banked.
- **C. Make the overflow cap count only WATER.** Would make the cap a property of
  the water body and simplify option B considerably. Rejected for now: it is a
  deliberate world-breaking change (digest + `kWaterCAVersion` bump), and it
  should not be spent to enable an optimization we are deferring anyway.
- **D. Seed only from CHANGED cells / skip "clean" bodies / truncate the
  post-overflow air walk.** Each diverges on a constructible case (a settled body
  made to rise by a third party newly touching adjacent below-surface air; an
  air-bridged sub-body under the cap that would then be leveled instead of
  skipped). Rejected — these are the silent behavior changes doctrine forbids.

### The replay-skip invariant (option B's correctness core, for the record)

Stated so a future implementation has something to be checked against.

> For a component C discovered at tick T, let its **dependency footprint** F(C)
> be every brick the flood READ while discovering C — including bricks it
> queried and rejected. The flood's output on C is a pure function of, over
> F(C): (i) every cell's fill, (ii) each brick's `touched` membership, and
> (iii) `solid_` over those cells. Therefore if none of (i), (ii), (iii) changes
> over F(C) between T and T+1, the flood from the same seed at T+1 yields the
> identical cell set, identical `totalVol`, identical overflow decision, and
> identical targets. If additionally every target at T was a no-op (the settled
> case — the pass already suppresses equal-valued writes), then every target at
> T+1 is a no-op too, and C may be skipped entirely PROVIDED its per-brick
> visited masks are restored so that later seeds behave identically.

Merges, splits, and appearing/disappearing air bridges are all covered rather
than special-cased: each of them necessarily changes a fill in F(C) or a
`touched` membership in F(C), so the guard fails and the component is re-flooded
the ordinary way. Checking the guard is O(|F(C)| bricks) given per-brick
content-version stamps. Note that (iii) needs the SAME terrain-edit signal the
memo needs — option B does not avoid the caller contract, it inherits it.

## Decision

**PENDING — requires Matt's sign-off on items 2 and 3.**

1. **Merged now, no sign-off needed (byte-identical, off by default).** WaterCA
   gains an opt-in cross-tick solidity memo — `setSolidCacheEnabled`,
   `invalidateSolidAt`, `invalidateSolidRegion`, `invalidateSolidCache`,
   `solidCacheBrickCount`. Default OFF, which is bit-for-bit today's code path.
   `kWaterCAVersion` is NOT bumped. Both pinned goldens
   (`0x3D2224BE4A253404`, `0x56BC18914355A205`) re-derive unchanged with the memo
   ON, and every other existing test is byte-identical with the memo forced on.

2. **Needs sign-off: enable the memo on the live UE water subsystem.** This is
   the only part with observable risk, and it is a CALLER obligation, not a
   voxel-core one. `FVoxelWaterImpl::CA`'s SolidFn is overlay-aware
   (`UVoxelWorldSubsystem::IsSolidAtVoxel` -> `World::materialAt`), so digging
   and explosives change solidity under settled water at runtime. Before
   `setSolidCacheEnabled(true)` may be called there, `VoxelWaterSubsystem.cpp`
   must invalidate on EVERY solidity change:
   - `TryDig` and `CarveSphere` already deliver an exact cleared-voxel list into
     `NotifyTerrainVoxelsCleared`, but that hook currently consumes it only for
     below-sea-level breach seeding and discards `Z >= 0` outright.
   - `TryPlace` has **no** water notification at all today — and air->solid
     matters to the memo exactly as much as solid->air.
   - `ExtractClearedVoxelCoords` filters to `MAT_AIR`, so non-air edits are
     invisible to water.
   **Observable behavior if this is done wrong:** water flows through terrain
   that was dug away, or sits inside terrain that was placed, at the specific
   edited voxels — and, because Phase C redistributes a conserved total across a
   component, a wrong component boundary moves water levels body-wide, not just
   at the bad voxel. It is a determinism divergence, so it also desyncs
   multiplayer (the client mirror applies replicated fills, but the authority's
   own sim would be wrong). This is why the default is OFF and why this is not
   being flipped on unilaterally by a perf pass. Mitigation if approved: land the
   invalidation calls first, then flip the flag as a separate commit, and keep
   `invalidateSolidCache()` (full drop) as the coarse fallback for any edit path
   we are unsure about — over-invalidation only ever costs a re-query.
   **Win if approved:** on the pour bench the memo is worth 329-345 -> 111-121
   ms/tick (~2.8-3.0x) and 40.8M -> 4.7M terrain queries; on a settled lake with an
   engine-realistic query cost, ~9-11 -> ~6.5-6.8 ms/tick.

3. **Needs sign-off (or explicit deferral): the persistent per-water-body
   structure is DEFERRED, not adopted.** Recommendation is to defer on the
   evidence above — its measured prize (~6.5 ms/tick per large settled body) is
   an order of magnitude smaller than the memo's, while its complexity (per-body
   footprints, per-brick visited masks, content stamps, split repair) is an
   order of magnitude larger, and it carries far more determinism risk. Revisit
   when BOTH hold: (a) M3+ actually has multiple large persistent bodies
   (ocean/lakes) resident at once, and (b) item 2 is live, so the terrain-query
   cost is out of the way and the water-traversal residual is genuinely the top
   of the profile. If it is picked up, the replay-skip invariant above is the
   thing to implement and test against, and option C (cap counts water only)
   should be reconsidered at the same time as a deliberate versioned change.

## Consequences

- Phase C stops being a perf regression against v2 the moment item 2 lands: the
  pour bench goes from ~2.7x over the pre-Phase-C baseline to below it.
- voxel-core now carries a cache whose correctness depends on a caller promise.
  That is a new kind of contract for this codebase and the reason it is
  default-off, documented at length in `waterca.h`, and covered by
  `waterca_solid_cache_invalidation_tracks_terrain_edit` — an executable spec of
  what a caller owes WaterCA. Turning the default ON would be a doctrine
  deviation requiring its own ADR.
- The "persistent per-body structure" follow-up in docs/status.md is now
  answered rather than open: designed, costed, and deliberately deferred, with
  the invariant written down so it does not have to be re-derived.
- If a future caller cannot honour the invalidation contract (e.g. terrain that
  changes without any edit event, like a streaming LOD swap that alters
  materialAt), that caller must simply leave the memo off; it loses perf, never
  correctness.

## Item 2 resolution (docs/status.md "Water edit-notification completeness +
## memo enablement" has the full writeup; this is a pointer, not a restatement)

The three notification gaps this item called out (`TryPlace` sending nothing,
`ExtractClearedVoxelCoords`'s `MAT_AIR`-only filter hiding non-air edits, and
M5 collapse/island removal never notifying water at all) are fixed in
`VoxelWorldSubsystem.cpp`/`.h` and `VoxelWaterSubsystem.cpp`/`.h`: every edit
path now calls a new material-agnostic `NotifyTerrainRegionEdited` hook
(batched per edit, routed to `invalidateSolidRegion`), in addition to the
existing solid->air-only `NotifyTerrainVoxelsCleared` reservoir/breach path.

The memo was then proven safe via an in-engine cross-process A/B
(`-VoxelWaterMemoTest`, `VoxelEarthGameMode.cpp`): the full dig/place/carve/
M5-collapse edit vocabulary run beneath and around a settled basin pool, once
with `voxel.Water.SolidCacheEnabled` forced 0 and once forced 1 (same seed,
same edits) -- every logged checkpoint digest matched byte-for-byte, not just
the final one. `voxel.Water.SolidCacheEnabled` now defaults to **true** on
that strength (a console var, per this ADR's own item-4-equivalent ask, so it
can still be forced off in the field with no relaunch if a future edit path
is ever suspected of a missed invalidation).

This does NOT change the PENDING/proposed status or human-sign-off line
above -- that decision is Matt's to make; this note only records what was
built and proven so it need not be re-derived. One real, separate gap
surfaced during the proof and is NOT fixed here (see docs/status.md for the
full reasoning): a fully-settled water body does not reactivate itself when
nearby terrain changes at all (memo on or off) -- no public voxel-core API
exists to wake a brick without also injecting real volume via `addWater`, so
"dig beneath a long-settled pond above sea level and see it react" is not
currently true in shipped gameplay. Flagged as a follow-up needing either a
new voxel-core API or a deliberate, metered production nudge policy -- not
something this item's scope (caller-side invalidation wiring) covers.

# Three quarters of the game thread is one mistake, made three times

> **CORRECTION, 2026-09-12.** Text below that calls 48 m "the shipping default"
> or "the default ring" is WRONG. `kDefaultRingMeters` is **256.0** in
> `VoxelDetailAssetSubsystem.cpp:512`, with no config override, and its own
> comment records that it was raised deliberately after the owner reported the
> slopes looking bare from the vista. 48 m was the *validation harness*
> parameter default, which every capture then passed on the command line — so
> runs at 48 m were measuring a configuration the game does not use. The
> harnesses now default to 256 to match. What this changes: the 256 m figures
> throughout are the SHIPPING configuration, not an experiment.



From `walk-capture-43-full256-tickscopes`, quiet stretch only (frames 0–199, no
streaming, one frame in two hundred over 33.3 ms), so none of this is hitch
noise. Game thread median 21.55 ms.

| item | ms | what it recomputes every frame | what the answer depends on |
|---|---|---|---|
| Ocean tick | 4.12 | a follow transform and an "is the camera underwater?" worldgen query | a sea 75.8 m below the camera |
| Ripple tick | 4.13 | a 512×512 wave field stepped at a fixed 60 Hz | water within 25.6 m; there is none |
| Collision prepare | 3.79 | an asset shortlist over the pawn's swept region | the pawn's position, which moves ~0.1 m per frame |
| Roof probe | 2.83 | up to 128 column samples | whether the camera is in a cave |
| **total** | **14.87** | | |

**69% of the quiet game thread is expensive queries rebuilt every frame to
answer questions whose answers barely change.** None of it scales with anything;
all of it is a flat tax paid whether the player moves or stands still.

## Collision prepare: the cache is destroyed by construction

This one is the cleanest, because the fix is visible in the code that defeats
it.

`WorldQueryBatch::prepare` already has a containment cache with a 32-voxel
(3.2 m) margin: if the requested rectangle sits inside the covered one, it
returns the existing query and does no work. Within a frame that works exactly
as designed — this capture measures **1.00 preparations against 10.67 sweep
calls**, so ten of eleven sweeps are free.

It never works *across* frames, because the batch does not survive one:

```
void UVoxelWorldSubsystem::BeginMovementCollisionQueries()
{
    check(!Impl->MovementCollisionQueries);
    Impl->MovementCollisionQueries = MakeUnique<vxc::WorldQueryBatch<...>>(Impl->Voxels);
}
void UVoxelWorldSubsystem::EndMovementCollisionQueries()
{
    if (Impl) Impl->MovementCollisionQueries.Reset();
}
```

Created at the top of every movement tick, destroyed at the bottom. The header
says so plainly: *"Never retained across ticks."* So the 3.79 ms is paid once
per frame unconditionally, and the 3.2 m margin can never earn anything back.

**This refutes the plan's Phase 3 hypothesis**, which was that consecutive
sweeps re-prepare overlapping regions. They do not — the batch already shares
one prepare across all of them. The cost is one prepare, and one prepare is
3.79 milliseconds.

What a prepare does: `WorldQuery`'s constructor calls
`field->instancesForRect(...)` with a callback that runs
`world.amplifier().columnCached(x, y)` **per column** of the padded rectangle,
then resolves every returned instance for composition. It is a worldgen sweep
over the pawn's neighbourhood, from scratch, sixty times a second.

**Why retaining it across frames looks viable, and the one question that
decides it.** Edited voxels are not the obstacle: `materialAt` consults
`editedBricks()` live on every single query, ahead of the shortlist, so a
retained shortlist cannot serve a stale edit. What the shortlist actually
caches is the set of *asset* instances (trees and the like) overlapping the
rectangle. So the correctness question is narrow and answerable: **when can the
asset field change under a retained batch?** Streaming assets in or out is the
obvious case, and it has an invalidation hook already. Answer that and the
3.79 ms becomes an occasional cost instead of a per-frame one.

## Roof probe: 2.83 ms for one boolean, with a latch that already exists

`CountUndergroundRoofSamples` walks up to 128 samples up a column to decide
whether the camera is under rock, so that an underground veil can be shown. It
is effectively the entire clipmap tick — 2.836 of 2.836 ms at median.

The caller already has hysteresis: two solid samples latch the veil on, zero
latch it off, and one holds the previous state. So the system already accepts
that this answer is sticky. What it does not have is any gate on *asking*. The
probe runs every frame regardless of whether the camera moved far enough
vertically or horizontally to change a cave/no-cave answer.

## Ocean and ripple

Covered in `docs/measurements/gamethread-attribution-2026-09-11/`. Same shape:
the ocean asks a worldgen surface query every frame to decide whether a camera
75.8 m above sea level is underwater, and the ripple field steps a 512×512 wave
simulation at a fixed 60 Hz with no test for water being anywhere near. The
ripple cvar ships enabled with no config override.

Sub-scopes are in the source to split each of those two into its parts; the
capture that reads them is queued.

## Why this matters more than it looks

The GPU floor at the 256 m ring is 40.3 ms standing still, so the game thread's
21.6 ms is currently hidden behind it. **Size culling removes 21 ms of that GPU
floor**, and the moment it does, this 14.87 ms becomes the binding constraint —
at the shipping 48 m ring it already is (game thread 21.1 ms against a 12.7 ms
GPU).

None of the four is a trade. Nothing is lost by not recomputing an answer that
has not changed. That distinguishes this whole group from the velocity pass and
from size culling itself, both of which cost something visible.

# Half the game thread is one asset resolve, paid three times

Four captures this afternoon, each narrowing the last, ending at a single line of
code. Temperate forest site, quiet stretch of each capture (frames 0–199), all
receipts passed.

## The chain, followed all the way down

    IsUnderwaterAtWorld
      -> GetWaterFillAtWorld          8.179 of 8.194 ms
        -> CA.fillAt                  0.001 ms        the cellular automaton: free
        -> Mob.implicitFillAt         8.175 ms        all of it
          -> sourceFillAt
            -> terrain_()  =  UVoxelWorldSubsystem::IsSolidAtVoxel

Each step was measured, not inferred, and two hypotheses died on the way:

- **The worldgen ground sample is not the cost.** `UnderwaterSurfaceMs` is
  0.013 ms across all twelve calls a frame. The fast path built to skip it
  engaged on 12 of 12 calls and moved the game thread 0.093 ms. Retired.
- **There is no cache to warm.** A probe that repeats each implicit query
  immediately measured the repeat at **103.5%** of the first — 8.553 against
  8.264 ms. So the cost is not a cold amplifier column that a memo would fix.

## The line

`UVoxelWorldSubsystem::IsSolidAtVoxel` has two paths, and its own comment names
them:

> *Synchronous water/ground probes inside movement share its prepared asset
> shortlist. Worker queries and calls outside that scope keep the exact
> standalone path.*

Inside the movement tick's `Begin`/`End` pair it uses a prepared shortlist. The
movement component makes **twelve such calls a frame for 0.021 ms between them**.

Outside that scope it takes the standalone path — a full per-point asset resolve
— and **one call costs about 4 ms**. The ocean actor and the ripple field's
watcher both tick outside it.

| item | ms | what it is |
|---|---|---|
| CollisionPrepare | 3.79 | the shortlist, built once per frame |
| OceanUnderwater | 4.12 | **one** standalone call |
| RippleAutoWatch | 4.01 | **one** standalone call |
| **total** | **11.92** | of a 21.6 ms game thread — **55%** |

Same function, same frame, same column. The only difference is whether a
shortlist had been prepared.

That also explains the 200× spread between callers that looked so strange when
it first appeared, and it explains why exactly two of twelve calls carried the
cost: those two are the two that tick outside the movement scope.

## The change

One shortlist keyed to the frame counter, shared by everyone in the frame
instead of only by the movement tick.

It **cannot outlive a frame**, so it carries exactly the staleness guarantee the
movement scope already had. And edits stay live either way, which is the part
that looked like a blocker and is not: `WorldQuery::materialAt` consults the
edited-brick overlay *before* it ever reaches the asset entries. A felled tree
reads as air from the overlay whatever the shortlist remembers. What the
shortlist caches is only which asset instances overlap the rectangle.

`voxel.Collision.FrameAssetShortlist 0` restores the previous behaviour exactly
and is the control arm.

**Prediction, written before the run:** about 8 ms off the game thread,
`CollisionPreparations` still 1 per frame, nothing on the GPU moves. If the game
thread does not move, the shortlist is not reaching the callers that pay, and
`FrameShortlistPointCalls` will say so.

## Why this is the right target now

Size culling took the GPU from 34.8 to 20.0 ms and left the game thread
untouched at 28.9, so the slice is game-thread bound. This is 55% of the quiet
game thread, and none of it is a visual trade — the answer is identical either
way, it is only computed once instead of three times.

## The method note worth keeping

Two of the four captures were nulls, and both were cheap because the instrument
shipped in the same build as the change. The fast path proved it engaged before
it proved it was worthless, so a null was still an answer rather than a dead
end. The memo probe was written to make its two outcomes far apart on purpose —
near-zero or full price, nothing in between to argue about.

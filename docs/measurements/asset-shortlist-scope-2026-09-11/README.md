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

## The result: −5.16 ms, and the counters say where the rest went

Walk 50 against walk 51, one binary, a cvar apart, both receipts passed.

| scope (median ms) | shortlist OFF | shortlist ON | change |
|---|---|---|---|
| **GameThreadTime** | **21.497** | **16.334** | **−5.163** |
| UnderwaterQuery | 8.232 | 3.330 | −4.902 |
| └ WaterFillImplicit | 8.214 | 3.312 | −4.903 |
| **RippleAutoWatch** | **3.976** | **0.019** | **−3.957** |
| OceanUnderwater | 4.226 | 3.306 | −0.920 |
| CollisionPrepare | 3.742 | **7.091** | **+3.349** |
| CollisionPreparations | **1** | **2** | **+1** |
| FrameShortlistPointCalls | — | 2 | |
| GPUTime | 40.452 | 40.410 | −0.042 |
| PawnTick | 4.316 | 4.361 | +0.045 |

**A quarter of the game thread, and the prediction held in direction if not in
size.** Nothing on the GPU or the render thread moved, which is what a pure
game-thread change should look like.

**The ripple watcher went to nothing** — 3.976 to 0.019 ms. It now finds a
shortlist already prepared and its call is free.

**The ocean only fell by 0.92 ms**, and the reason is in the two counters at the
bottom: `CollisionPreparations` went from 1 per frame to **2**, and
`CollisionPrepareMs` from 3.742 to 7.091. The ocean's call stopped being a
standalone resolve and became *the second prepare*, which costs about the same
thing. The first cut left the movement tick on its own `Begin`/`End` batch, and
**two batch objects cannot share a prepare however close their rectangles are.**

## The second cut: put the movement tick on the same batch

`FindFirstSolidVoxelSlice` is the caller that asks for the **widest** rectangle —
a whole sweep slab rather than a point — so moving it onto the shared batch is
what makes every other query of the frame fall inside an already-covered region.
With one batch object the containment check does the rest: whoever asks widest
pays once, everyone inside is free.

Expected: one preparation per frame instead of two, and the ocean's remaining
3.3 ms collapsing with it.

### It did, and the prediction held on all three counts

Walk 52 against walk 53, one binary, a cvar apart, both receipts passed with
their eight movement checks.

| scope (median ms) | shortlist OFF | shortlist ON | change |
|---|---|---|---|
| **GameThreadTime** | **21.583** | **13.301** | **−8.283** |
| **UnderwaterQuery** | **8.096** | **0.027** | **−8.069** |
| └ WaterFillImplicit | 8.079 | 0.011 | −8.068 |
| OceanUnderwater | 4.111 | 0.004 | −4.108 |
| RippleAutoWatch | 3.953 | 0.018 | −3.935 |
| CollisionPrepare | 3.697 | 3.692 | −0.005 |
| **CollisionPreparations** | **1** | **1** | **0** |
| FrameShortlistPointCalls | — | 12 | |
| GPUTime | 40.488 | 40.479 | −0.009 |
| RenderThreadTime | 41.684 | 41.640 | −0.044 |
| PawnTick | 4.286 | 4.258 | −0.028 |

**−8.28 ms, 38% of the game thread.** The prediction written before the run was
"about 8 ms off the game thread, `CollisionPreparations` still 1 per frame,
nothing on the GPU moves." All three held.

The 8.1 ms underwater query is **gone** — 0.027 ms. One prepare a frame now
serves the movement sweep, the ocean's camera test and the ripple watcher
together, and the prepare itself did not get more expensive for covering them:
3.697 to 3.692 ms.

`FrameTime` does not move in this pair, because these arms run with size culling
off and the GPU is still the 40.5 ms wall. The two changes are independent and
compose: size culling takes the GPU to 20.0, this takes the game thread to 13.3.

## Where the game thread stands now

| item | ms | note |
|---|---|---|
| GameThreadTime | **13.30** | was 21.58 |
| PawnTick | 4.26 | ⊃ MovementTick 4.25 ⊃ **CollisionPrepare 3.69** |
| ClipmapTick | 2.96 | ⊃ RoofProbe, a cave boolean recomputed every frame |
| OceanTick | 0.006 | was 4.11 |
| RippleTick | 0.088 | was 4.02 |

**The single prepare is now the largest item on the thread.** It is the same
3.69 ms it always was; what changed is that it is no longer paid three times.
Reducing it means retaining the shortlist *across* frames, which is a different
and more delicate change — the asset field carries a configuration revision that
would serve as the invalidation signal, and the edited-brick overlay already
protects against stale edits.

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

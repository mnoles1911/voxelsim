# The ~11-13 ms is the game thread BLOCKED — and which thread owns the ceiling changes with speed

2026-08-24. Answers "name the ~11-13 ms outside `tickMs`" and the open p50/p95
question. **No new leg and no new code were needed: mode 3 already ran on
M20/M30/M0 and the answer was on disk.**

Windows read: the eight mid-flight windows of each leg with
`seg=SETTLED-MOVING n>=100` (M20 n=170-193, M30 n=137-156). Named, because the
transition windows say something different and the linger windows say nothing.

---

## 1. Engine semantics, checked before claiming anything

`UnrealClient.cpp:1831-1846` — "Calculate gamethread time (excluding idle time)":

    ThreadTime          = CurrentTime - Lastimestamp       // full game-thread frame period
    GGameThreadTime     = ThreadTime - GameThread.Waits    // BUSY, idle EXCLUDED
    GGameThreadWaitTime = GameThread.Waits                 // IDLE

**Disjoint, complementary, and they sum to the frame period.** So
`frame = tick + gameOther + gameWait + residual` is a real reconciliation and not
four overlapping totals — which is exactly what the existing `FrameAttribution`
line could never be.

## 2. The answer

M20, representative mid-flight window (`n=178`):

    frameMs=27.48 = tick 14.14 (51%) + gameOther 0.00 (0%) + gameWait 10.38 (38%)
                                     + RESIDUAL 2.95 (11%)
                    || render=19.50 (71% of frame)  renderWait=4.90  rhi=8.12

`gameOther` reads **0.00 on 14 of 16 mid-flight rows across both legs**.

**The ~11-13 ms outside the streaming tick is not game-thread work. It is the
game thread BLOCKED (`gameWait` 10.4 ms) plus an unattributed residual
(2.9 ms).** Proxy creation, RDG setup and brick-pool flushes are not hiding in
it — there is no game-thread bucket left for them to hide in. The brick-pool
flush is already inside `tickMs` as `brickFlush=`, and on the marcher path there
are no components to create proxies for.

## 3. Which thread owns the ceiling — and it differs by speed

Both stages are single-thread wall time and directly comparable; the frame
cannot be shorter than the longer of them.

| leg | game busy | render busy | longer stage | floor |
|---|---|---|---|---|
| **M20** (23.5 m/s) | **<= 14.14** | **19.50** | **render** | **51 fps** |
| **M30** (34.3 m/s) | **<= 21.84** | **16.32** | **streaming tick** | **46 fps** |

**At 20 m/s the render thread is the longer stage. At 30 m/s the streaming tick
has grown past it and the game thread is.** The ceiling changes hands with
speed, which is why a single leg would have produced a confident wrong answer
either way.

What that does to the recompute arithmetic **at 20 m/s**: deleting **100%** of
recompute takes game busy from ~14.1 to ~7.5 ms, but the frame is floored by the
render thread at **19.50 ms — 51 fps**. Not 62. **The gate is unreachable at
20 m/s by game-thread work alone.**

At 30 m/s the same fix does bite, because there the tick *is* the longer stage.
Both are true; neither generalises, and that is the point.

`gameBusy` is an upper bound (`<=`) because the old `Max(0, ...)` clamp on
`gameOther` hid how far below the tick `GGameThreadTime` actually landed. That
clamp is removed in this commit — see §5(b).

## 4. p50/p95: the distribution is splitting, and it is not terrain

Per-window, mid-flight:

| | M20 | M30 |
|---|---|---|
| meanMs | 23.6-24.6 | 23.8-26.4 |
| p50Ms | 20.9-24.5 | **17.8-20.9** |
| p95Ms | 47-52 | **57-68** |
| p99Ms | 57-71 | **77-106** |
| maxMs | 73-90 | **99-120** |
| **stutters** (>=20 ms) | **116-138 (51-66%)** | **91-108 (48-51%)** |
| **hitches** (>=33.3 ms) | **38-49** | **50-63** |

**`stutterPct` goes DOWN with speed while the hitch count goes UP.** Mass left
the 20-33 ms band in both directions at once: more frames below 20 ms (p50
falls) and more above 33 ms (p99, max rise). The mean barely moves because the
two migrations cancel.

**That is a bimodal split, not harder terrain.** Terrain difficulty moves mean
and median together and would not move the stutter and hitch counts in opposite
directions. The tail is **a minority of very expensive frames**, not a uniform
slowdown.

The actionable half: **spreading the work across frames beats making it
cheaper.** A per-frame budget on the bursty term turns one 100 ms frame into
five 20 ms frames and moves p99 far more than any constant-factor win on the
same work.

## 5. Three changes, two of them defects in my own instrument

**(a) `DELTA tag=MOVE` was structurally blind.** 51 of 53 windows per leg printed
`VERDICT=NOT-COMPUTED`. Not a bug — it is the delta's own "a window without both
populations says nothing" rule doing its job: a window in which the camera flew
the *whole* window contains no parked frames. The only two that computed were
transition windows (`n=196/21`, `n=46/482`) — describing takeoff, not flight.

Fixed with **cumulative** `SMovingTotal` / `SParkedTotal` buckets, never reset,
and a `DELTA tag=MOVE-LEG` comparing every settled-moving frame in the leg
against every settled-parked one. The per-window `tag=MOVE` is kept — a
transition window is a genuine within-5-seconds comparison, immune to drift
across a leg — but it can no longer be mistaken for the answer.

**(b) `gameOther` could not go negative.** The `Max(0, ...)` clamp fired on 14 of
16 flying rows and printed `gameOther=0.00`, which reads as "there is no other
game-thread work" and is **indistinguishable from** "`GGameThreadTime` came in
below the tick and the instrument swallowed the difference". Different findings,
same line. Unclamped now.

*A bucket that cannot go negative is not a measurement, for the same reason a
residual that cannot stay large is not a residual: it silently absorbs its own
error.*

**(c) New `PIPELINE` line**, stating thread ownership instead of leaving it to be
inferred from two numbers on different lines:

    Voxel frame phase PIPELINE seg=SETTLED-MOVING-LEG gameBusyMs=14.10
        renderBusyMs=19.50 frameMs=27.48 longerStageMs=19.50 floorFps=51
        gapPct=28 bound=RENDERBOUND

`bound=` is `GAMEBOUND` / `RENDERBOUND` / `BALANCED` (within 15%) /
`UNKNOWN-A-STAGE-READS-ZERO`. `floorFps` is what the frame would be if the
shorter stage vanished entirely — the floor any single-thread fix works against.

**H3 remains reachable.** The residual is still `frame` minus the three named
buckets with no clamp anywhere, and at 30 m/s two windows already show it at
8.1-9.7 ms (26-29%). It has not been distributed into whatever happened to be
measured.

---

## 6. THE HOOK — nothing to change

`NoteFrame` and `NoteSettled` signatures are unchanged. Everything here is
inside `VoxelFramePhase.{h,cpp}`. Re-run any mode-3 leg to get the new lines.

## 7. Legs and gates

Re-run **M20** and **M30** unchanged (`-VoxelFramePhase=3`).

- **G9 (the delta fires).** `DELTA tag=MOVE-LEG` computes on mid-flight windows
  with both `n` in the thousands. Still `NOT-COMPUTED` = §5(a) did not ship.
- **G10 (thread ownership, stated).** `PIPELINE bound=` on the
  `SETTLED-MOVING-LEG` row of both legs. **Prediction from this analysis: M20
  `RENDERBOUND`, M30 `GAMEBOUND`.** If M20 returns `GAMEBOUND`, §3 is wrong and
  the render-thread conclusion must be withdrawn.
- **G11 (the clamp mattered).** `gameOtherMs` on flying rows is now negative or
  small-positive rather than exactly 0.00. Still exactly 0.00 everywhere = the
  unclamp did not ship.
- **G12 (H3 stays alive).** `residualPct` reported and not silently small. Above
  25% the reconciliation is not tight enough to name the term and **no thread
  verdict may be published from that leg.**

## 8. What I am not claiming

That the render thread's 19.50 ms is *terrain*. It is the render thread's whole
frame — the marcher, the sky, the water, the UI and everything else — and
nothing here splits it. If G10 confirms `RENDERBOUND` at 20 m/s, the next
question is which render pass, and that needs an RDG-level instrument this file
does not have and should not grow.

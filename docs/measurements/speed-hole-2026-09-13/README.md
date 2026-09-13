# Above about 5 m/s the world stops being built, and the player flies over a hole

Found by the owner, in the first moving captures this project has been able to
take. Four flight legs on one path at the temperate forest spawn, 100 m altitude,
pitch −10°, identical in everything but speed.

## The ladder

| speed | FPS | chunks loaded/s | chunks unloaded/s | what the picture shows |
|---|---|---|---|---|
| 5 m/s | **30.1** | **2,319.6** | 1,042.2 | intact — trees, undergrowth, a pond |
| 10 m/s | 7.0 | 452.9 | 1,018.9 | partial void, plants floating over nothing |
| 20 m/s | 2.9 | 178.1 | 746.0 | a crater from the camera to the horizon |

**Production collapses 13x across the ladder while eviction barely moves** (1,042
→ 746 per second). This is not eviction outrunning admission. It is admission
nearly stopping.

The shape of the defect: distant terrain is drawn by the clipmap, a separate
path, and it stays. The near field is the marcher's, drawn from streamed chunks,
and those never arrive — so everything inside the clipmap's inner edge is simply
absent. The crater wall is that boundary. The water at the bottom is the ocean,
visible because no ground stands between the camera and it.

At 10 m/s the understory gives the mechanism away by itself: the plants are
placed and drawn by their own component path, so they hang in mid-air over ground
the marcher never built.

## It is not caused by anything shipped on 2026-09-13

The same leg with **every** change from that day reverted by cvar — the prewarm
caps, the coarse warming, the site cap, the cache pending limit, the across-frames
shortlist, the roof-probe shortlist and the cold-resolve cap — produces the
identical crater at the identical 2.9 FPS. Screenshot in this directory.

## A counter I misread, recorded so nobody repeats it

The HUD's `Rings: R0 0/638 R1 0/460 …` line reads zero on the left at EVERY
speed, including the 5 m/s leg where the world is perfect. **It is not
resident-versus-desired.** I read it as "nothing is resident" and said so before
the 5 m/s leg refuted it. The load/unload rates on the `Streaming:` line are the
counters that actually separate these arms.

## Why no earlier capture caught it

Every previous capture settles before it shoots — the route harness parks the pawn
and waits 2 seconds, the static harness waits 150. Streaming catches up in that
time, so the world is always whole by the time the shutter fires. The moving
capability only started working on 2026-09-13, and it needed a harness fix of its
own before it produced comparable ground (see the flight-leg checkpoint bug).

## What this changes about the target

The standing target is p95 under 10 ms at 20 m/s. At 20 m/s there is no world
within about a kilometre of the player, so frame time was never the binding
problem at that speed — **the engine cannot build ground fast enough to be
looked at.**

The on-foot dial tops out at 9.5 m/s ("Mad dash"), which is ABOVE this knee. That
does not automatically mean a sprinting player sees this: the fly pawn sits 100 m
up and demands a far larger area than a ground-level camera does, and the 9.5 m/s
route captures looked normal — but those were settled checkpoint shots and cannot
answer it. **A moving capture with the WALKING pawn is the missing test**, and
the moving harness currently drives the fly pawn only.

## The first thing to measure next

Throughput is roughly `admitted-per-tick × tick rate`, and the streaming tick runs
about once every two frames. At 2.9 FPS that is under two ticks a second. If
per-tick admission caps are what bound the 178/s, then the collapse is a feedback
loop — slow frames give fewer ticks, fewer ticks admit less ground, and the
ground that is missing is not what makes the frame slow. The counters to settle
it are the admission window's own, read against tick rate at each speed on this
ladder.

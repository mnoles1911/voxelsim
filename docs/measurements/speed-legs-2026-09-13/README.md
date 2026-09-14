# The slice measured at the speed its target is written for, and it fails there

Phase 5. Every slice number before tonight came from a pawn walking at 2.2 m/s
or standing still; the Goal 3 bar is written for **20 m/s**. Two legs, because
20 m/s is not one speed the slice can be asked for — it is two different pawns.

## On foot, as fast as a player can go

Same authored detour route, same fixture, same binary, one flag apart —
`-VoxelEcologyRouteSpeedTier=7`, which the route driver now accepts. Sustained
speed is real and measured, not requested: **median 2.20 m/s against 9.09 m/s**
over the moving samples.

| | tier 2 (2.2 m/s) | tier 7 (9.5 m/s) |
|---|---|---|
| frames | 3,466 | 793 |
| frame p50 | 15.38 | **19.72** |
| frame p95 | 28.40 | **176.06** |
| frame p99 | 310.04 | 495.33 |
| frames over 33.3 ms | 4.24% | **20.55%** |
| frames over 100 ms | 2.22% | **8.07%** |
| game thread p99 | 267.28 | 382.03 |
| **GPU p50** | **13.42** | **13.84** |
| SubmitMs p99 | 224.00 | 345.02 |

**The GPU does not care how fast the player moves. Everything else does.** At
4.1x the speed the GPU median moves 3%, while one frame in five crosses 33 ms and
p95 goes up more than six-fold. The cost is arrival rate: new ground per second,
paid on the game thread, and `SubmitMs` — the cold asset resolve of
`docs/measurements/submit-cold-resolve-2026-09-13/` — rises with it.

> **CORRECTION, 2026-09-13 (later the same day). The 20 m/s leg below was NOT at
> the forest site.** `tools/voxel-run-flight-leg.ps1` cleared only top-level
> `*.vxlog` and left the session CHECKPOINT store, which holds the pawn pose. So
> the leg restored the previous leg's position and `-VoxelSpawnAt` lost to it
> silently. Four consecutive legs asking for the same spawn logged path centres
> of −6144000, −5904260, −5664702 and −5185007 UU — each exactly the previous
> leg's travel distance further on, and all about **100 km from the forest**,
> whose pawn sits at −15,473,834 UU.
>
> The walk and route harnesses are unaffected: they pass a per-capture `-UserDir`,
> so they never shared the checkpoint. **Every on-foot number in this document
> stands.** What does not stand is the claim that the 20 m/s row describes the
> temperate forest fixture: it describes terrain with the ecology system active,
> somewhere else.
>
> The harness now clears the checkpoint store, mirroring the block
> `tools/voxel-capture.ps1` grew for this same bug on 2026-09-07. Confirmed by
> the number that was wrong: a re-run lands at (−15,473,834, −8,147,600) UU, the
> requested spawn, at 100 m altitude rather than 1,986 m.

## At 20 m/s, which needs the fly pawn

`tools/voxel-run-flight-leg.ps1 -Flight line -VoxelPerfSpeed=20` at the same
spawn, with the ecology fixture and the same detail cache passed through, 2560x1440
output at an internal 1664x936. The driver's own line confirms the speed
(`flight=line, depth=60m, speed=20m/s`) and the understory is live in the leg
(size-cull bounds logged against the 256 m ring).

The engine's own frame-phase gate, on its SETTLED-MOVING segment:

    n=498 hitches=475 stutters=481 stutterPct=96.59
    meanMs=242.70 p50Ms=246.00 p50Fps=4 p95Ms=471.22 maxMs=471.22
    gateP95=FAIL gateSteady=FAIL gateSpeed=PASS gate=GOAL3-FAIL

**Four frames per second.** The speed gate passes — the leg really is moving —
and every other gate fails. Chunk throughput reads 2,000–3,400 chunks/s against a
standing 50k/s target.

**Two things must be said with that number.** A `line` flight never re-treads
ground, so it is continuous cold fill and the harshest streaming case there is;
and the fly pawn is not the walking pawn, so this row and the route rows above
are not one experiment. It is still the condition the target names.

## What this settles about the plan

The slice plan's Phase 4 says the marcher alone spends the whole 10 ms budget
before a plant is drawn, and treats the GPU as the wall. **At speed that is not
where the wall is.** The GPU is flat from 2.2 to 9.5 m/s; the tail is streaming,
on the game thread, and it is the same cold-resolve path that the p99 work
attacked tonight.

That reframes what "100 FPS at 20 m/s" would require: not a faster marcher, but a
streaming path that can admit, resolve and submit new ground at four times the
rate this one does — or a slower admission that accepts coarser ground for longer
while the fine ground catches up. The second is the cheaper idea and it already
has machinery in the tree.

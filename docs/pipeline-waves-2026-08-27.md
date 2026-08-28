# The plan: marcher + streaming, 2026-08-27

Owner-accepted. Build against this. Short on purpose.

> **REVISED LATE 2026-08-27; NUMBERS AND OUTCOMES RE-REVISED 2026-08-28 after the
> buried-skip flip shipped.** The premise below ("only Wave 2 moves the goal") was
> written when the tail was believed to be one thing. It is two. Read this box first;
> the waves below are kept because their measurements stand, but their framing is
> superseded.
>
> **WHERE THE GOAL ACTUALLY STANDS** (2026-08-28 legs, shipping default, 2560x1440
> TSR-upscaled from `view=1552x873`, line flight 23.4 m/s, spawn `-61440,-61440`):
>
>     p50   8.47 ms = 118.0 fps
>     p95  11.95 ms =  83.7 fps
>     p99  14.29 ms =  70.0 fps     <- the owner's stated gate is 50 fps 1% low
>
> **The 1% low >= 50 fps gate is being MET, by 20 fps.** What still fails is the older
> ">100 fps steady at 20 m/s" target: p50 passes it, p95 and p99 do not. Those are
> different bars and the difference should be a deliberate decision, not an accident of
> which doc was open. (The 2026-08-27 pooled figures this box previously carried —
> p50 9.10 / p95 13.67 / p99 17.57 over 3,059,356 frames on five control legs — are
> superseded by the flip below.)
>
> **THE TAIL HAS TWO REGIMES AND THEY HAVE DIFFERENT OWNERS.**
>
>     FAST -> SLOW (p95)   gpu +5.48   game +4.07     GPU-LED
>     SLOW -> TAIL (p99)   gpu +0.91   game +5.41     GAME THREAD ONLY
>
> The GPU saturates near 13 ms and stops. So GPU work buys p95 and cannot buy p99, and
> a p99-framed GPU claim is attributing a step the GPU does not take.
> Full tables: `docs/p99-game-thread-split.md`, `docs/gpu-tail-split-2026-08-27.md`.
> Reproduce with `tools/attribution-pool.sh`.
>
> **THE p95 STEP IS FULLY ATTRIBUTED, AND THE BAND IS NOW GONE.** Nothing in the GPU
> frame is unnamed any more: streaming 73%, marcher 20%, unaccounted 5%, draw path
> NEGATIVE. The largest single block was **the band: +2.51 ms, 43% of the whole GPU
> rise**, spread across three terms (`RgBand` + `RgColumn` + `RgVoxelize`) kept for one
> reason -- to buy the buried-chunk skip (`because: quads 0, band 31671, noPack 0`).
> **The A/B (`tools/voxel-buriedskip-ab.ps1`) came out: `BuriedSkipEnabled()` default
> flipped 1 -> 0 and SHIPPED 2026-08-28 (commit 178b1a8)** -- p99 56.9 -> 70.0 fps,
> verified role-reversed, image confirmed against a measured noise floor, and the band
> is removed: `[gpu-lean] kept=0`.
>
> **THE p99 STEP IS 77% THE VOXEL TICK**, and inside it: dispatch +3.09 of which
> `submit` is 86%, apply +0.23, remesh 0.00, unload +0.15 -- leaving **+3.35 ms of
> `other` inside the tick that is now measured and still unnamed**. `submit` itself has
> a six-way split already implemented as window counters (`Voxel gpu submit split`),
> dominated by `reqHdr` at ~65%; carrying that split per-frame is the open instrument.
>
> **RETIRED TONIGHT, with proof, do not re-open:** async raster-atlas fill (moves 85%
> of fill off the game thread and is WORSE -- it spreads the stall: hitch time 3957 ->
> 5834 ms, frames >=200 ms 1 -> 4); seed-only band (`dup=0 redundant=0` on BOTH arms --
> the legacy path already emitted the seed-only population); brick-pool publication as
> a tail term (<=1.2 ms per 2 s WINDOW).
>
> **A COUNTER TRAP WORTH THE LINE:** `wlcols conv=483,014 fb=387` reads as 99.9%
> converted, but 31,636 kept graphs are called with no column feed, so 6.5% of jobs
> still run the classic path -- that is the +1.05 ms. The counter is honest about
> worklist RECORDS and was read as if it covered CALLS.

## The one thing to know before picking work

**Only Wave 2 moves the frame-rate goal.** Everything else buys headroom.

    PARKED   p50 8.40 (119 fps)  p95 9.10 (110)  p99 9.50 (105)   <- already beats >100 fps
    MOVING   p50 9.10 (110 fps)  p95 15.20 (66)  p99 19.60 (51)   <- beats it at the MEDIAN only

The gap is **the tail**: +6.10 ms at p95, +10.10 ms at p99. And the marcher is **provably not in
it** -- halving ray count (the half-res A/B) moved the moving p99 delta by **0%**.

So: Waves 1, 3 and 4 make the pipeline faster and lower the floor. Wave 2 is the goal.

---

## WAVE 1 -- cut ray cost in EVERY direction  *(in flight)*

The marcher is 54% of the GPU frame (3.169 of 5.842 ms parked) and its cost tracks EMPTY SPACE
CROSSED:

    looking down (pitch -90)   1.108 ms
    horizon      (pitch   0)   4.450 ms
    sky          (pitch +30)   5.638 ms

**1a. Height pyramid.** `H(x,y)` = sound upper bound on terrain, crown-inclusive, built from the
finest level and **max-reduced upward** (direct coarse query = 188.83 m slack, 3.2 m leaf =
3.84 m -- **49x**). A ray is provably in air wherever `p.z > H(p.x,p.y)`. Marched as a 2D DDA over
the pyramid, ONE test pays in all three directions:

    UP          terminate once above and rising
    HORIZONTAL  skip forward in coarse steps while above local H   <- THE HEADLINE
    DOWN        tStart: jump to first contact

Go/no-go re-measured at the real spawn `-61440,-61440`: **R/S = 357** against a threshold of 4.
Design: `docs/marcher-height-pyramid-design.md`.

**Why not just fix ZCut.** A Z slab is a horizontal sandwich a level ray never leaves --
**0.00% of 3.3e9 decisions at pitch -10**, 21% at sky, 93% at altitude. It cannot reach the
horizon, and the horizon is where the frame is spent. The pyramid supersedes it.

**1b. `anySolid` index bit.** `VoxelBrickTraverse.ush:2284` reads it and early-outs -- "the
cheapest possible skip" -- and has **never executed**, because all writers hardcode it to 1. Of
lookups that pay for the record fetch, **35% / 69% / 73%** (down/horizon/sky) are thrown away as
air one step later. Zero added loads, zero added tests, pays in every direction.
**Expect ~0.2-0.4 ms, not 73%** -- the 15.7 MB table fits the 64 MB Infinity Cache.

**1c. `tools/voxel-march-direction-sweep.ps1`.** No per-direction harness exists as a script.
Alternated A,B,A,B, hard invalidators, reads the marcher's own `view=`.

---

## WAVE 2 -- the moving tail  *(cause found, fix not started)*  **THE GOAL**

    game-thread busy   FAST 2.84 ms -> p95 7.25 -> p99 12.35   (4.3x)
    hideable slack on a fast frame: 8.25 ms

**On p99 frames the game thread stops being idle and becomes the critical path.** That is a
different regime from p50, and it is why the MEAN reads RENDERBOUND while the tail is not.

**Strongest correlate: chunk publication volume, 19 -> 78 chunks/frame.** And the cost is NOT in
the bracket named for it -- `applyMs` is 0.11 ms; the batch publication lands in a counter called
`unloadMs`.

Two blockers, both honest:
- The attribution instrument has a **sampling bug** -- `tick` from the current tick, the rest from
  the previous, producing a part larger than its whole. Fixed in code, unbuilt. Until it lands, no
  `tick` figure may be read.
- **~4 ms of the slow frame sits on no clock.** There is **no per-frame GPU timer in this
  project** (zero `RHIGetGPUFrameCycles` call sites), and `renderBusyMs` is not clean CPU time
  (TaskGraph.cpp:739-745 books every dependent wait as busy; :754 exempts the RHI thread, so
  `rhiMs` IS clean). Naming that residual needs `FRHIGPUFrameTimeHistory` wired in -- one field,
  and then it is either GPU or a real CPU stall.

Refuted, do not re-open: per-chunk cost (HEAVY-APPLY frames average 175.5 chunks and 9.90 ms,
**5.30 ms BELOW p95**); GPU backpressure and the marcher (half-res moved p99 delta 0%);
RDG pass count (23x more passes cost 0.6 ms); `submitMs` (dead three times, last on the right
population: +0.5 ms into p95 against 5.40 ms of slack).

---

## WAVE 3 -- cold start

The raster atlas spends **~1,464 ms of game thread inside a 6.1 s settle** -- ~24% of it --
against a standing **<5 s** target. `submitMs` peaks 117 ms per frame, but **entirely before the
settle boundary**: it is a cold-start cost, not a tail cost. Both budget levers are measured dead
ends (`capHit=0` lifetime, so the cap never binds; async was measured and refuted).

---

## WAVE 4 -- leftovers

- `PACK_BIAS` tripwire: `FVoxelMarchDepthOnlyVS/PS` compile `VOXEL_MARCH_RINGS 0` while the march
  packs with bias 4096. Harmless only because that pass never reads `V.LocalVoxel`.
  **Taken by the crafting session with P0.**
- `GVoxelMarchZCutRanZMin/Max` literal `[8]`s that the `static_assert(kGridSlots == 8)` does not
  name. **Also taken with P0.**
- `[voxel-march] emit halfres:` is one-shot per process and printed `full res` forever while the
  marcher ran half-res. Give it a change-set like the march's own line.
- The marcher **does not cast shadows at all**; the quad path's terrain shadows cost 15.8 ms.
- `voxel.March.VerifyDepth` has **never run** (`compared=0`, three attempts).

---

## Rules for every wave

- **One agent owns the build lane.** Building against a live editor corrupts the DLL pair; legs
  serialize. Code work parallelises, build and measure do not.
- **Image before timing** on anything that changes what is drawn. This project shipped a "-7.6%
  win" that was the marcher deleting a mountain; the timing INVERTED to +3.1% once the image was
  honest.
- **Red arm before green.** A confirmation that cannot come out the other way is not one.
- **Proof of engagement, every arm.** Eleven arms in one night reported success and did nothing.
- **Read the `++` site, never a counter's name.** Four names lied tonight alone: `promoteExit
  cap=`, `sveBlockedMs`, `unloadMs`, and the one-shot `emit halfres:`.
- **A number taken on a different binary is not a control.** This programme's own premise was
  stale by 10x.
- Spawn `-84480,53760` is **fatal** -- 38 km outside the tile set, every elevation query answered
  with sea level. Use `-61440,-61440`. `view=1552x873` is CORRECT at 2560x1440.
- `-Cvars` separator is **COMMA**. A `|` silently drops everything after it.

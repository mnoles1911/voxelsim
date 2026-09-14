# What the night of 2026-09-13 did

Sixteen commits, twenty-two captures. The slice at the temperate forest site,
256 m ring, as it stood at the start of 2026-09-12 against now:

| | 2026-09-10 | now |
|---|---|---|
| frame p50 | 41.7 ms (24 fps) | **13.9 ms (72 fps)** |
| GPU p50 | 40.5 ms | **12.6 ms** |
| game thread p50 | 21.6 ms | **6.2 ms** |
| frame p99 | ~350 ms | **~102 ms** |

## The six planned steps

1. **Nanite default verified and committed.** Frame 20.594 → 13.935 ms, GPU
   −34.6%, 48.6 → 71.8 fps, with the marcher unmoved as the control and **no flag
   on the command line** — the cache identity accepting the `nanite=1` bake is
   what proves the default flipped.
2. **The p99 hitch named.** `SubAssetsMs` is 98.8% of the submit bracket;
   `SubRasterMs` is 0.059 ms. The raster-atlas hypothesis, written down first,
   lost.
3. **Roof probe and retained shortlist measured.** Game thread 13.03 → 6.17 ms,
   with the prediction (`CollisionPreparations` stays at 1) holding.
4. **Height pyramid timed at last — it costs 1.2%.** Passes its hole gate and its
   image gate, then loses on the clock. Skipping air does not remove segment
   entries.
5. **Measured at the speed the target names.** The GPU is flat from 2.2 to
   9.5 m/s while one frame in five crosses 33 ms; at 20 m/s the engine's own gate
   reads GOAL3-FAIL at 4 fps. 20 m/s is unreachable on foot — the dial stops at
   9.5.
6. **Nanite's open risk recorded**: only the baked path can be Nanite, so bake
   coverage is now load-bearing.

## The unplanned half, which was the larger one

Chasing the p99 hitch found **three caps in series**, each hiding the next:

1. The prewarm queue held level-0 keys only — so raising every cap moved cold
   misses 7,563 → 8,247, i.e. nothing.
2. `footprintResolveSitesBound` refused *speculative* warming at 8,192 sites while
   the inline path had no bound at all — the cap never prevented the work, it
   guaranteed the game thread did it.
3. `FootprintResolveCache` issued **eight** warm tokens, whatever the launch cap
   said. That was the one that mattered.

Fixed: p99 146.90 → 109.92 ms, outside run-to-run spread, throughput unmoved, and
the 448 MiB of extra cache tried along the way was shown to buy nothing and is not
spent.

Then the part prediction cannot reach — a resolve first asked for in the frame it
is needed — got a bound: `ColdResolveCapPerTick`, then a millisecond budget when
the count proved the wrong shape, then charging the estimate **before** the work
when the budget proved it could only bound everything after the first resolve. At
9.5 m/s that last version reads p50 −16%, p95 −25%, frames over 100 ms −23%.

All three are **off by default**: deferral means coarser ground held longer, which
is a visual trade and the owner's verdict under the settings-panel policy.

## Four things I got wrong, and how they were caught

- **Ranked hitches by `FrameTime`.** It is stamped one row after the stall, so
  every thread timer looked innocent. The render thread's `EventWait` on the
  previous row is the tell.
- **Predicted the raster atlas.** It is 0.02% of the bracket.
- **Predicted raising the prewarm caps would help.** It moved nothing; the gate I
  had written — "coldMisses, not launched=" — is what caught it.
- **Explained a 20 ms budget overshooting to 80 ms as several ticks per frame.**
  The counters say the tick runs once per *two* frames. The real cause was the
  unbounded first resolve.

## What is open

- The cold-resolve bound wants an owner verdict on pictures, and a per-level cost
  estimate (28.91 ms is charged for every coarse level; a level-7 footprint is far
  wider than a level-1).
- The height pyramid at a high-relief site is unmeasured — the walk harness
  refuses there for want of ecology, so it needs a terrain-only harness.
- Frames over 300 ms at 20 m/s remain the gap between this slice and its target,
  and the target is a streaming problem, not a marcher one.

# The black ring at 20 m/s, and the ring-normalised priority that removes it

Terrain-only world, fly pawn, 20 m/s, spawn `-154740,-81476`, heading 0, a shot
every 128 m from 1024 m to 4992 m (32 frames per arm), 90 s preflight, sun
frozen 12:00 03-20. Harness: `tools/voxel-moving-capture.ps1`. Scorer:
`tools/voxel-black-pixels.py` — pixels with max(R,G,B) ≤ 55 in the lower 60 %
of the frame, HUD box excluded; a clean frame reads 0.00 %, terrain in shadow
does not trip it (calibrated on the parked frames; the dark chunk slots read
40–70).

## What the owner saw

Flying forward, the world stayed intact for a while and then opened a ring of
missing ground between the near field and the horizon: square dark slots at the
ring's walls, a smooth black band between them, healing after a few seconds.
`dense20` first caught it on a still frame: **32.4 % of the frame black at
1664 m**, in an episode from 1408 m to 2176 m that coincides with a hill (pawn
altitude 93 m → 144 m).

## What it was

Rings are annuli — R0 0–64 m, R1 64–128, R2 128–256, R3 256–512, R4 512–1024,
R5 1–2 km, R6 2–4 km, R7 4–8 km — with hierarchical coverage on, so each coarser
ring also carries "stand-in" chunks under the finer rings for the marcher to fall
through to. Two priority rules decide what gets meshed first:

1. the cross-ring dispatch pick is **strictly nearest-first in metres** across
   the eight ring heads (`DispatchJobs`, the `HeadDistSq` loop);
2. an interior coarse stand-in's key is clamped **up to its ring's inner
   radius** (`BiasedSortKeySq`), written so that at a stationary fill a coarse
   chunk under the camera never queues ahead of the fine chunk beside it.

While moving, every ring's new work sits at its own OUTER edge, so the heads
are at ~64 / 128 / 256 / 512 … m, and R3 is served only when R0, R1 and R2 are
all empty. On the hill the near rings alone consume the ~4,000 chunks/s the
pipeline can do (R0 admission alone rose to 7,890 records per 2 s window), and
everything keyed beyond ~200 m — R2's outer half, all of R3, R3's stand-ins at
"256 m", R4's at "512 m", … — gets 1–2 dispatches per window against 4–5
thousand pending:

```
Voxel ring STARVED: R3 pending=4541 dispCpu=0 dispGpu=1 ... (floor slot held by GPU round trip)
Voxel rings: R0 pending=431 | R1 1545 | R2 5411 | R3 4914 | R4 4260 | R5 3015 | R6 1409 | R7 787
```

Nothing covers that band: R2's front is behind, R3+ never arrive, and the
preflight's R5/R6/R7 only reach the horizon. That is the annulus. It heals on
the flat because R0/R1 demand drops, their queues empty, and R3 finally gets a
turn (`R3 disp=1481` in the window it recovers).

What it was NOT, each ruled out on its own leg: the velocity lead-horizon floor
(`-VoxelLeadHorizonSec=3`, `denseFloor3`: 33.14 % at 1664 m); the ring slot
floors being held by a GPU round trip (`-VoxelRingFloorCpuOnly=1`,
`denseCpuFloor`: 29.97 %, R3–R7 still starve — a one-slot floor is a trickle
either way); the admission cutoff (`cutoffM=-1`, `rejected=0` throughout);
dispatch-ahead latency (cap 1024 changed nothing, earlier record).

## The fix

`voxel.Stream.RingNormalizedKey` = p multiplies each level's key by
(Outer_0 / Outer_L)^(2p), so at p = 1 rings compete on **distance as a fraction
of their own radius**. An R3 stand-in at 256 m (0.5 of 512) then sorts with an
R0 chunk at 32 m: the coarse stand-in loads BEFORE the fine front it stands in
for, and under a deficit every ring's front recedes a little instead of one
ring going black. `voxel.Stream.RingNormalizedKeyMinSpeed` fades p in from 0 at
rest to full at that speed, so a stationary cold start keeps the near-first
order. Per-level cutoffs, the lead-horizon floor and the GPU residency compare
are kept on the same scale (rescaled in place when the scale changes). Both
have command-line overrides (`-VoxelRingNormalizedKey=`,
`-VoxelRingNormalizedKeyMinSpeed=`) so the preflight's first recomputes run the
arm; `keyP=` on the `Voxel admission (window)` line is the proof of engagement.

## Results (same flight, same 32 frames)

| arm | flags | black pixels, all frames | worst frame | STARVED lines R3/R4/R5/R6/R7 |
|---|---|---|---|---|
| control | `-VoxelLeadHorizonSec=3` (inert) | 1,206,488 | 33.14 % @ 1664 m | 17 / 48 / 83 / 108 / 123 |
| floor CPU-only | `-VoxelRingFloorCpuOnly=1` | 1,230,004 | 29.97 % @ 1664 m | 22 / 53 / 85 / 109 / 123 |
| p = 1, always | `-VoxelRingNormalizedKey=1` | **5,204** | 0.09 % @ 4864 m | 0 / 1 / 5 / 14 / 9 |
| **p = 1, fade to 5 m/s (SHIPPED)** | `-VoxelRingNormalizedKey=1 -VoxelRingNormalizedKeyMinSpeed=5` | **8,752** | 0.10 % @ 2304 m | — |

Per-frame, control vs shipped: 1408 m 4.51 → 0.00 %, 1536 m 3.02 → 0.00 %,
1664 m 33.14 → 0.00 %, 1792 m 9.47 → 0.00 %, 2176 m 2.92 → 0.00 %. The 0.08 %
at 4864 m is on every arm (a shadowed cliff face).

Dispatch share, mean per 2 s window over shots 1–12 (the hill):

| ring | R0 | R1 | R2 | R3 | R4 | R5 | R6 | R7 |
|---|---|---|---|---|---|---|---|---|
| control | 4073 | 2137 | 985 | 441 | 200 | 46 | 1 | 1 |
| p = 1 | 3342 | 2116 | 1052 | 533 | 288 | 164 | 94 | 45 |

Throughput is unchanged — 4,054 / 4,165 / 3,940 chunks/s (control / floor /
p = 1) — the deficit is the same size, it just lands where it is invisible.
R0's queue is what grows instead (to 5,684 on the hill), and R1/R2 stand-ins
cover it; the near ground in the shipped frames is at full detail.

Cold start, seconds until each ring's pending queue first reads 0 (from the
preflights, spawn at rest):

| arm | R0 | R1 | R2 | R3 | R4 | R5 | R6 | R7 |
|---|---|---|---|---|---|---|---|---|
| control | 5.7 | 7.7 | 7.7 | 9.7 | 13.8 | 15.8 | 22.8 | 26.6 |
| p = 1, always | 21.9 | 21.9 | 21.9 | 21.9 | 21.9 | 21.9 | 21.9 | 21.9 |
| p = 1, fade to 5 m/s | 6.2 | 8.2 | 8.2 | 10.2 | 14.2 | 16.2 | 23.3 | 30.2 |

Always-on drains every ring together (the whole world is covered sooner, the
ground underfoot is at 10 cm later); the fade keeps the shipped order. That is
why the default is p = 1 with the 5 m/s fade.

## Pictures for the owner

`Saved/Screenshots/WindowsEditor/VoxelMove_denseFloor3-d01408_00000.png` and
`-d01664_00000.png` (control) against `VoxelMove_denseNormS5-d01408_00000.png`
and `-d01664_00000.png` (shipped). Same pose to within 0.3 m. The one thing to
judge: with coarse stand-ins loading first, pale coarse patches at mid distance
are what stands where the black ring was — whether they read worse than before
is the picture call, not a counter's.

## Open

- Ecology worlds at speed are a separate, pre-existing collapse (asset resolve
  is 96 % of submit at every speed; 2.9 fps at 20 m/s). Untouched here.
- The trees-only 5 m/s crater at 256 m (`kindsTreeOnly`) is plausibly the same
  mechanism at a lower throughput (asset chunks dispatch ~13x slower), which
  this change would also serve — not re-run.
- No Settings row yet (policy: every visual trade is a row); the cvars are the
  levers meanwhile.
- The stuck-chunk dump's `dist=` prints the scaled key for coarse rings; the
  admission lines print metres.

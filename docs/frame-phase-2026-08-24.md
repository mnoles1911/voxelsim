# The 94 ms outside `tickMs`: the lead is falsified, and here is what can answer it

2026-08-24. Follows the lifted-cap result (14,099 chunks/s, game thread 97.9%,
tick rate 50 Hz -> 3 Hz, cold settle regressing 23.5 s -> 35.5 s).

---

## 1. `renderWaitMs=9962` is frame 1. It is not a measurement.

The hitch line that opened this investigation:

    Hitch frame: frameMs=400.00 | subsystemTickMs=17.36 elsewhereMs=382.64
                 renderMs=1505.40 renderWaitMs=9962.46 rhiMs=0.00 gameWaitMs=0.00

In `Saved/ahead-on.log`, **exactly two of 717 hitch lines** carry
`renderWaitMs` above 200:

    [2026.08.23-22.25.56:262][  1]  renderWait=9773.87  rhi=0.00  gameWait=0.00
    [2026.08.23-22.25.56:284][  2]  renderWait=9773.87  rhi=0.00  gameWait=0.00

**Frames 1 and 2, with the identical value on both** — a global that was not
rewritten between them. The process's first log line is at 22.25.47, so these
are the first rendered frames after ~9 s of module load, RHI init and shader
setup, and `GRenderThreadWaitTime` is reporting that *startup* accumulation
once, before the per-frame cadence begins. `rhiMs=0.00` and `gameWaitMs=0.00`
on the same line are the tell: two of the four timers had never been written at
all.

**The median over the 715 non-startup hitch frames of the same log is
`renderWait = 18.89 ms`.** That is the real number.

This is the third time tonight the first frames of a cold leg have produced a
mechanism that was not there.

### What the honest hitch population does show

Medians over non-startup hitch frames (startup rows dropped by the rule "any
per-thread timer above 3x its own frame is describing history, not this frame"):

| leg | caps | n | frame | tick | elsewhere | render | renderWait | rhi | gameWait |
|---|---|---|---|---|---|---|---|---|---|
| `q-ctl` | default | 781 | 40.32 | 21.36 | 23.04 | 20.58 | 19.41 | 11.93 | 0.00 |
| `q-a1024` | 1024/24 | 1144 | 43.81 | 28.88 | 19.09 | 25.29 | 23.62 | 14.46 | 0.00 |
| `q-L2` | **1024/24** | 797 | 38.42 | 20.59 | 24.78 | 24.48 | 12.66 | 12.74 | 7.75 |
| `q-L3` | 192/6 | 595 | 38.72 | 18.86 | 27.53 | 21.84 | 15.12 | 10.99 | 7.13 |
| `q-L4ship` | 192/6 | 639 | 38.25 | 17.35 | 27.98 | 22.16 | 10.75 | 11.39 | 6.99 |
| `ahead-on` | default | 715 | 39.57 | 20.85 | 22.32 | 20.94 | 18.89 | 11.52 | 0.00 |

Two observations, both weak on purpose:

- **`elsewhereMs` is very close to `renderMs` in every arm** (19-28 vs 20-25).
  On hitch frames the non-tick part of the frame is about the size of the render
  thread's frame time — consistent with the game thread pipelining against a
  render thread that is the slower of the two. **A hypothesis, not a finding.**
- **`gameWaitMs` is 0.00 in the older builds and 7-8 ms in the newer ones** —
  and `q-L3`/`q-L4ship` ran at *default* caps and also show ~7 ms. That is a
  build difference, not a cap effect. Anyone comparing `q-a1024` to `q-L2` on
  `gameWait` is comparing two binaries.

### Why none of this settles it

`docs/lessons-2026-07-27-s0-s1.md` lesson 17 already records the trap I would be
walking into: **every figure grepped out of the Hitch line is a median of frames
that had already exceeded 33.3 ms.** That is why all six rows above sit at
38-44 ms — by construction. The hitch population cannot describe a typical frame
at 1024/24, and it is exactly the typical frame that collapsed from 50 Hz to
3 Hz.

---

## 2. The existing instrument is the right shape and still cannot answer it

`voxel.Stream.FrameAttribution` already samples **every** frame and splits FAST
(<= p50) from SLOW (>= p95) with a DELTA line. It was **never armed on a
lifted-cap leg** — `q-L2.log` contains zero attribution lines.

But arming it is not enough, and the reason is structural rather than a bug:

> `render` / `renderWait` / `rhi` / `gameWait` are **per-thread totals that
> overlap each other and the frame.** They do not sum to `frameMs`, nothing
> checks that they do, and so the instrument can say "render rose by X" but can
> never say "and 94 ms is still unaccounted for."

The unaccounted part is the entire question.

---

## 3. What shipped: a reconciliation with an explicit residual

`ue-project/Source/VoxelEarth/VoxelFramePhase.{h,cpp}`. One hook, one switch,
off byte-identical.

    frameMs  =  voxelTickMs + gameOtherMs + gameWaitMs + RESIDUALMs

| bucket | source |
|---|---|
| `voxelTickMs` | passed in by the caller (already computed) |
| `gameOtherMs` | `GGameThreadTime - voxelTickMs` — everything else the game thread did |
| `gameWaitMs` | `GGameThreadWaitTime` — the game thread **blocked**, which in `-game` is the frame-end sync on the render thread |
| **`residualMs`** | `frameMs` minus the three. **The point of the file.** |

and beside it, the render side, which is what decides thread ownership:
`renderMs` (render thread busy), `renderWaitMs` (render thread idle on RHI/GPU).

### The split is by WORK, not by slowness

`HEAVY` = a frame that applied >= `-VoxelFramePhaseHeavy` (default **512**)
chunks; `LIGHT` = applied < 64. **Both populations come from one leg**, so
leg-to-leg variance cannot enter — the failure mode that already cost this
project a retraction when a contended box was read as a slow configuration.
512 is chosen so no default-cap frame (ceiling 192) can land in HEAVY by
accident.

This also answers the question *directly*: does applying 1,024 chunks in a frame
create render-thread work? Compare frames that did against frames that did not,
inside the same run.

### Registered disproof — written before the leg

| | hypothesis | disproved if |
|---|---|---|
| **H1** | the ceiling is the render thread | `d(render) < 20%` of `d(frame)` **AND** `d(gameWait) < 20%` |
| **H2** | the ceiling is the game thread | `d(voxelTick + gameOther) < 50%` of `d(frame)` |
| **H3** | neither; unattributed | **confirmed** if `d(residual) > 50%` of `d(frame)` |

**H1 and H2 can both be disproved.** That is the outcome the residual exists for
and the instrument prints it as a first-class verdict
(`H1 AND H2 BOTH DISPROVED -- read the residual`) rather than resolving it by
preference.

Every share is printed **beside its own absolute millisecond value**, because
`waitShare=99%` was true and meaningless tonight. And only single-thread wall
times are reported — no worker-pool totals, because wait spread across N
parallel threads is not wall time and this file must not offer a number that
invites that mistake again.

### Failing readings, both ways

| reading | verdict |
|---|---|
| no `Voxel frame phase` line with `-VoxelFramePhase=1` | the hook was not applied. **Nothing ran.** |
| `frames=0` | hook present, never called |
| **`heavyN=0`** | **the leg never entered the regime being asked about.** It says nothing about lifted caps and must not be read as evidence about them |
| `gameThreadMs=0.00` on every frame | **hard zero.** `GGameThreadTime` is not populated here and the game-thread half of the reconciliation is dead. Must *not* be read as "the game thread is idle" — the same mistake as reading `params=0.00ms` as "the sampler is free". The residual absorbs it and will be enormous |
| `residual` persistently **negative** | the globals describe a different frame than `frameMs` (one-frame lag). Past a few ms the reconciliation is invalid and **no verdict may be drawn** |
| heavy and light frame times **equal** | apply volume does not move frame time; the ceiling is somewhere this file cannot see. Say that; do not pick a thread |

Each window prints its own `peakAppl/frame`, so a reader can name **which
regime** a line describes. **Read the peak window and say which one you read.**

---

## 4. THE HOOK — one line, `VoxelWorldSubsystem.cpp` @ `54cc8f5`

**Include**, beside the existing block near line 14:

```cpp
#include "VoxelFramePhase.h" // frame reconciliation with an explicit residual, behind -VoxelFramePhase=
```

**Hook — immediately after line 9515** (`AccumTickMs += TickMsSoFar;`), insert:

```cpp
	VoxelFramePhase::NoteFrame(double(TickMsSoFar), ThisFrameAppliesFromWorker);
```

Both values are already computed and in scope at that point: `TickMsSoFar` on
line 9510, and `ThisFrameAppliesFromWorker` is set by `DrainResults` earlier in
the same tick. With the switch absent this is one branch on a latched bool.

---

## 5. The legs

The measurement is **within one leg**, so a single lifted-cap run answers it.

| leg | log | `-ExtraArgs` |
|---|---|---|
| **F1** the question | `fp-1024.log` | `-VoxelFramePhase=1`, `-VoxelApplyCap=1024`, `-VoxelApplyBudgetMs=24` |
| **F2** default-cap control | `fp-ctl.log` | `-VoxelFramePhase=1` |
| **F3** cross-check | `fp-1024-attr.log` | as F1, plus `-ExecCmds="voxel.Stream.FrameAttribution 1"` |

All with `-ClearEditLog -BudgetSec 300`.

F2 exists to prove `heavyN=0` there — if the default-cap leg reports heavy
frames, the 512 threshold is wrong and F1's split is not what it claims. F3
arms the existing attribution alongside, so the two instruments can be checked
against each other on one run; they measure the same globals and should agree on
`render`/`renderWait` while only the new one carries a residual.

### Gates

- **G1 (traffic).** F1 prints `Voxel frame phase` with `heavyN > 0` and
  `peakAppl/frame` near 1024. `heavyN=0` on F1 = the leg was not in the regime;
  the run says nothing and must be re-taken.
- **G2 (validity).** `zeroGameThreadFrames = 0` and `negResidualFrames` under
  5% of `frames`. Either one large voids the reconciliation and **no verdict may
  be published from that leg**.
- **G3 (the answer).** The `VERDICT` line on the **peak** window, quoted with the
  window it came from and its `peakAppl/frame`.
- **G4 (control identity).** F2 reports `heavyN=0` and the verdict
  `NOT COMPUTED`.

---

## 6. What I am NOT claiming

I have not measured the lifted-cap regime. I falsified the lead that pointed at
the render thread, showed that the hitch population cannot answer the question
either way, and built the instrument that can.

The one directional signal I have — `elsewhereMs` tracking `renderMs` on hitch
frames in **every** arm, including the default-cap ones where no collapse
happens — is as consistent with "the game thread pipelines against the render
thread at all throughputs" as with "the render thread is the ceiling at high
throughput". It does not separate them. That is why the instrument splits by
apply volume within a run instead.

**Counter-evidence to keep on the table:** the `[gpu-jobcost]` instrument
reported `enqDisp/enqPoll ~ 0` — no render-thread backpressure — but at default
caps, in the regime where the collapse does not happen. It does not speak to
1024/24, and F1 is what would make it speak.

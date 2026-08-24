# GOAL 3: the settled-only frame distribution, and one candidate eliminated for free

2026-08-24. Implements the "WE CANNOT CURRENTLY MEASURE THE GOAL" requirement in
`docs/50k-budget-2026-08-23.md`. Companion to `docs/frame-phase-2026-08-24.md`,
which falsified the `renderWaitMs=9962` lead.

---

## 1. The free result: the GPU job manager is not the 94 ms

I was told to run the collapse leg and read the always-on `[gpu-jobcost]`
brackets rather than write new code. Those legs already exist. From
`Saved/q-a1024.log`, **caps 1024/24, the peak window** (`submits=25744`,
`ticks=233`, `promoted=25967`):

    chargedMs=12.3 = enqDisp 0.3 + enqPoll 0.1 + enqFetch 0.0 + enqRel 0.1 + deliver 11.8
    perTickUs=52.9   perDeliverUs=0.4   delivered=26512

**12.3 ms of a 5,000 ms window — 0.25%.** The render-thread enqueue brackets
specifically (`enqDisp + enqPoll`) are **0.4 ms per window**, in the collapse
regime, at 25,744 submits. `q-L2.log` agrees at its own scale (`chargedMs=1.1`,
`enqDisp 0.0`, `enqPoll 0.0`).

**What this eliminates, precisely:** the GPU job manager's own render-thread
enqueue path is not a meaningful consumer of anything, at lifted caps, and the
`enqDisp/enqPoll ~ 0` reading now holds *in the regime where the collapse
happens* rather than only at default caps.

**What it does not eliminate:** the render thread. The brackets live inside
`Tick()`, i.e. inside `tickMs`, and the 94 ms is outside `tickMs` — so by
construction they cannot see proxy creation, brick-pool upload flushes, RDG
setup, or the frame-end sync. One candidate is gone; the thread is not cleared.

Also visible on the same `q-L2` line and worth handing to whoever owns pool
capacity: `poolReplaced=203 (11.1%)` and `CEILING=39175/s`.

---

## 2. Why no arm can currently claim >100 FPS

The only frame statistic in the tree is cumulative:

    VoxelPerfRun post-warmup (t>=10s): frames=19425 p50=9.84ms p95=30.62ms
                                       max=115.28ms hitches=564

On `q-repro-main` that statistic opens at **t=10 s** and the world settles at
**t=21.3 s** (`Voxel cold settle: SETTLED t=21.3s`). So **eleven seconds of
cold-fill storm — the regime where hitches are explicitly authorised — are
averaged into the same p95 as the ~270 s of settled play the goal is about.**

Two lines exist for the whole 300 s leg. One statistic cannot serve two regimes
with opposite rules.

---

## 3. What shipped

`ue-project/Source/VoxelEarth/VoxelFramePhase.{h,cpp}` (the file from the
frame-phase wave, extended). The switch is now a bitmask:

| `-VoxelFramePhase=` | effect |
|---|---|
| `0` | **off, default, byte-identical** — both hooks fold to one branch on a latched int |
| `1` | **frame distribution, segmented at the cold-settle boundary.** This is what GOAL 3 requires |
| `2` | phase reconciliation (`frame = tick + gameOther + gameWait + residual`) for the 94 ms question |
| `3` | both |

Split because they answer different questions and cost different amounts: the
distribution is one histogram increment per frame and is cheap enough to leave
on permanently; the reconciliation reads five engine globals and keeps six
running sums per bucket.

**Recommendation for the coordinator: put `-VoxelFramePhase=1` in the leg
driver's defaults.** A gated instrument nobody arms is this project's
eleven-inert-features failure, and GOAL 3 says *every* leg from here reports the
settled segment.

### What it prints, per 5 s window

    Voxel frame dist (5.0s window): segment=SETTLED settleT=21.3s | gate: p95 < 10.00ms == 100 fps
    Voxel frame dist SETTLED window: n=498 mean=9.91ms | p50=9.60ms (104 fps) p95=30.40ms (33 fps)
        p99=48.00ms (21 fps) max=115.28ms | hitches=22 (>33.3ms, 4.42%) | bin=0.10/1.00ms | GOAL3 FAIL (p95 >= 10.00ms)
    Voxel frame dist FILL    total : n=1102 ...  | gate n/a -- fill is exempt (hitches authorised during the load storm)
    Voxel frame dist SETTLED total : n=18323 ... | GOAL3 PASS/FAIL

- **The cumulative (`total`) row is what the goal is judged on**, and it is
  printed on *every* window precisely so `grep | tail -1` — which has produced
  four retractions on this project by landing on the post-flight linger — cannot
  pick up a three-frame window and call it the answer. A cumulative row is
  monotone and carries its own `n`.
- The gate verdict is stated, not left to the reader, and **only on the settled
  segment** — the fill is exempt by the owner's standing directive.
- FPS is printed beside every millisecond so nobody has to divide.

### Design choices worth defending

**A histogram, not a sample buffer.** 0.10 ms bins to 32 ms, 1.00 ms bins to
256 ms, one overflow bin, exact max tracked outside it. 4.4 KB per segment
regardless of leg length, so a 3,000 s leg cannot silently start dropping
samples. **The bin width is printed beside every percentile** (`bin=0.10/1.00ms`)
because p95 is compared against a 10.00 ms gate and a reader must be able to see
that 0.10 ms is 1% at the gate rather than assume exactness. Quantiles return
the **upper** bin edge — a gate is a "must be under" test, so the pessimistic
edge is the honest one.

**The boundary is told, not derived.** Hook 2 fires from the cold-settle line
itself. A boundary guessed from apply volume would mislabel the tail of the fill
as settled, which is exactly the blend GOAL 3 exists to undo — and "derived, not
verified, detaches" has produced five bugs here in three days.

**The distribution is fed above the stale-globals guard.** That guard rejects
frames whose *engine globals* are describing history (the `renderWaitMs=9962`
artifact). The frame *time* is not one of those globals — it comes from this
function's own clock — and dropping a real 400 ms frame out of the tail
statistic because an unrelated global was stale would corrupt the exact number
the gate is judged on, in the flattering direction.

### Failing readings, both ways

| reading | verdict |
|---|---|
| no `Voxel frame dist` line with `-VoxelFramePhase=1` | hook 1 not applied. **Nothing ran.** |
| `SETTLED total: n=0` | **the leg never settled, or hook 2 was not applied.** The instrument says so explicitly and **no >100 FPS claim may be made from that leg either way** — never the fill numbers under a settled heading |
| `FILL total: n=0` | hook 2 fired before any frame — the boundary is wrong |
| `settleT=NOT SETTLED` on the last window | the leg was cut short; the settled rows describe nothing |
| `p95` exactly at a bin edge repeatedly | resolution artifact; `bin=` is printed so this is visible rather than inferred |
| settled `n` far below `frames × settledSeconds` | frames are being lost before hook 1; the distribution is not the whole population |

---

## 4. THE HOOKS — `VoxelWorldSubsystem.cpp` @ `c80f112`

**Include**, beside the existing block near line 14:

```cpp
#include "VoxelFramePhase.h" // segmented frame distribution + frame reconciliation, behind -VoxelFramePhase=
```

**Hook 1 — immediately after `AccumTickMs += TickMsSoFar;` (line 9515):**

```cpp
	VoxelFramePhase::NoteFrame(double(TickMsSoFar), ThisFrameAppliesFromWorker);
```

Both values are already in scope: `TickMsSoFar` at line 9510,
`ThisFrameAppliesFromWorker` set by `DrainResults` earlier in the same tick.

**Hook 2 — in the cold-settle SETTLED branch, immediately after
`const double SettleT = LastActivity - ColdStartSeconds;` (line 10002):**

```cpp
		VoxelFramePhase::NoteSettled(SettleT);
```

That branch fires exactly once per run. `NoteSettled` is idempotent anyway — a
second call returns without moving the boundary.

---

## 5. The legs

| leg | log | `-ExtraArgs` |
|---|---|---|
| **S1** the GOAL 3 baseline | `fps-main.log` | `-VoxelFramePhase=1` |
| **S2** the collapse regime | `fps-1024.log` | `-VoxelFramePhase=3`, `-VoxelApplyCap=1024`, `-VoxelApplyBudgetMs=24` |
| **S3** default-cap control for S2 | `fps-ctl.log` | `-VoxelFramePhase=3` |

All with `-ClearEditLog -BudgetSec 300`. S2 and S3 answer the 94 ms question
(mode 3 gives both instruments on one run); S1 establishes the honest GOAL 3
baseline that `p50=9.84 / p95=30.62` cannot.

### Gates

- **G1 (traffic).** `Voxel frame dist` present; `SETTLED total: n > 0`;
  `settleT` matches the `Voxel cold settle` line. Any of these missing = the
  leg cannot speak to GOAL 3.
- **G2 (the baseline).** S1's **settled cumulative** `p50/p95/p99/max/hitches`,
  quoted with its `n` and its window. This is the number every later arm is
  compared against — the current `p95=30.62` is not it, because that figure has
  11 s of fill in it.
- **G3 (the conflict, and this is the point).** S2 reports **both** regimes:
  fill settle-seconds *and* settled p95. **An arm that improves settle and
  wrecks settled p95 is not a win.** If S2's settled p95 is worse than S3's, the
  cap lift is not shippable as-is and that must be said plainly in the same
  breath as the 14,099 chunks/s.
- **G4 (control identity).** With no switch, no `Voxel frame dist` line at all.
```

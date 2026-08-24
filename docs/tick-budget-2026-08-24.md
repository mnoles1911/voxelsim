# A per-frame work budget for the streaming tick — redistribution, not throttling

2026-08-24. Builds the thing the bimodal finding pointed at
(`docs/flight-tail-named-2026-08-24.md` §4).

---

## 1. Why only redistribution reaches the gate

The steadiness target is `stutters` **31.4% → 0.10%**: a **300x reduction in
tail frames**. No constant-factor win on any term reaches that. If one frame in
twenty does 100 ms of work, halving the work leaves one frame in twenty at 50 ms
and it still fails. Spreading the *same* work over five frames at 20 ms passes.

That follows from the tail being **bimodal** — `stutterPct` fell 51-66% → 48-51%
going 20→30 m/s while the hitch count rose 38-49 → 50-63, and mass left the
20-33 ms band in both directions. Terrain difficulty moves mean and median
together and could not produce that.

It may also be the one lever on **both** halves of the gate: if the render lane's
prior confirms that per-frame render commands scale with streaming and execute
outside the scene renderer, spreading streaming work spreads render-thread work
with it. The render thread is `RENDERBOUND` at every speed, so a game-thread
budgeter cannot reach 100 fps alone — but **steadiness is a separate pass/fail**
and is where a bimodal tail lives.

## 2. Design question 1 — the unit of deferral

**A whole stage, at its own existing loop boundary.** Not a cell range, not a
level, and nothing new.

This codebase's worst failures are silent wrong terrain, not crashes, and the
recorded example is exact: `DrainResults` once popped a result off the MPSC
queue and broke out of the body before using it, stranding the chunk with
`bJobInFlight` pinned and no counter moving. A novel mid-scan resumption point
would be new torn state by construction.

So this defers only where the code **already** stops early and is **already**
known to resume cleanly:

| stage | existing early exit | where the deferred work lives |
|---|---|---|
| `DrainResults` | `MaxAppliesPerFrame` / `ApplyBudgetMs` | `ResultsQueue` |
| `DrainGameThreadMesh` | `MaxRemeshesPerFrame` | `PendingGameThreadKeys` |
| `DrainUnloads` | `MaxUnloadsPerFrame` | the unload list |
| `DispatchJobs` | in-flight caps | the pending job queue |
| `RecomputeDesiredSet` | **cannot be interrupted** → deferred by *declining to start*, exactly as `-VoxelRecomputeDutyPct` already does | `bRecomputeWanted`, recomputed from the anchor every tick |

**The deferred work is carried by the queue that already owns it** — which is why
no new carry structure appears anywhere in this change. A new carry structure
would be a second place for work to be lost.

## 3. Design question 2 — sustained overload

The recorded failure is `VoxelWorldSubsystem.cpp:3571-3579`: **R3/R4 at 0 loaded
chunks for 90 s** from an ordering change. A budget that always spent itself on
the earliest stage would reproduce it. Three guards:

1. **Urgent is never deferred.** `bRecomputeUrgent` (first recompute, underground
   transition) passes through untouched — the same exemption the existing rate
   bound already makes, for the same reason.
2. **A reserve for late stages.** `-VoxelTickBudgetReserveMs` (default 2.0) is
   held back from every stage but the last, so apply cannot consume the whole
   tick and leave unloads never running — residency then grows without bound and
   the symptom is memory, not a stall.
3. **Escalation after N consecutive deferrals.** After
   `-VoxelTickBudgetMaxDefer` declines (default 4) the stage runs **regardless**.
   This bounds worst-case latency to N ticks *by construction* — at ~50 Hz that
   is 80 ms — however long the overload lasts. The check runs **before** the
   budget test, so a stage that has waited its turn runs even when the tick is
   already deep over budget.

**A budget that cannot escalate is a throttle.** The entire difference between
redistribution and throttling is whether deferred work is guaranteed to run.

**Check per-ring residency, never aggregate throughput.** Aggregate chunks/s
stayed healthy through the recorded R3/R4 stall. Read per-ring `loaded=`; any
ring at 0 for more than 10 s is a revert whatever the tail did.

## 4. Design question 3 — the apply budgets

| | |
|---|---|
| `ApplyBudgetMs` | **becomes tick-aware**, through the interception that already exists. `VoxelApplyFast::ApplyBudgetSeconds()` is already the single place `DrainResults` gets its wall budget, so it now returns `min(its own value, what the tick has left − reserve)`. **No new hook.** One clock, one owner. |
| `MaxAppliesPerFrame` | **left alone, deliberately.** It is a *count*, and the apply-ceiling census established it is a safety ceiling rather than a throttle. Making a count cap tick-aware would put two currencies on one clock and make the exit attribution unreadable — and that census is exactly how anyone tells this budget apart from the ones underneath it. |

**The existing exit census is this feature's proof of traffic.** Under a binding
tick budget, `wallClock` must **rise** and `countCap` must **fall**, because the
loop is now stopping on a clock it did not previously have. If neither moves, the
budget is not reaching `DrainResults` at all.

## 5. Registered disproof — before the leg

| reading | verdict |
|---|---|
| **`stutterPct` falls while cold start rises by more than `deferredMs` explains** | **THROTTLING, not redistribution. REVERT, do not tune.** Work genuinely carried costs cold start *at most* the amount deferred, and less in practice because it overlaps. `deferredMs` is summed per window so this is a comparison, not a judgement. |
| `escalations >= deferrals` | not deferring — a branch that then does the work anyway. Traffic without effect. The instrument says this inline. |
| `deferrals = 0` and `stageClamps = 0` with the switch on | the budget never bound; this arm is **behaviourally identical to control**. Lower `-VoxelTickBudgetMs` before reading anything into the tail. Said inline. |
| **`hookedTicks = 0`** | hook A was never applied. Latched, linked, and **completely inert**. Said inline, as a Warning. |
| any per-ring `loaded=` at 0 for >10 s | the R3/R4 stall reproduced. **REVERT** regardless of the tail. |
| `p50` improves while `p99`/`max`/`stutterPct` do not | the budget moved the median and left the tail — the exact opposite of the point. |

**The reading that decides this feature is `p99`, `max` and `stutterPct` on
`SETTLED-MOVING` — not `p50` — with cold start reported beside it**, because an
arm that fixes the tail and costs cold start is a trade the owner must see, not
one this file makes quietly.

## 6. THE HOOKS — `VoxelWorldSubsystem.cpp`

**Include**, beside the existing block near line 14:

```cpp
#include "VoxelTickBudget.h" // per-frame work budget for the streaming tick, behind -VoxelTickBudgetMs=
```

**Hook A — `FVoxelWorldImpl::TickStreaming`, immediately after
`const double TickStartSeconds = FPlatformTime::Seconds();` (line 8659):**

```cpp
	VoxelTickBudget::BeginTick(TickStartSeconds);
```

Passing the tick's own timestamp rather than taking a new one keeps this on the
same clock the tick is already measured against.

**Hook B — the recompute gate, line 9203.** Replace:

```cpp
	if (bRecomputeWanted && (bRecomputeUrgent || AllowRateBoundedRecompute()))
```

with:

```cpp
	if (bRecomputeWanted && (bRecomputeUrgent || AllowRateBoundedRecompute()) &&
	    VoxelTickBudget::MayStartRecompute(bRecomputeUrgent))
```

`bRecomputeUrgent` is passed through so the budget honours the same exemption the
existing rate bound does. Note the ordering: `AllowRateBoundedRecompute()` has a
side effect (it stamps `RecomputeHeldSinceSeconds` and increments
`RecomputeDeferralsSinceLog`), so the tick budget is tested **after** it — the
duty-cycle instrument keeps reading exactly what it read before, and the two
bounds stay separately attributable.

**Hook C (optional)** — `VoxelTickBudget::FlushStats(true);` in
`MaybeLogCounters`, for exact window alignment. It self-clocks at 5 s otherwise.

## 7. The legs

| leg | log | `-ExtraArgs` (all with `-VoxelFramePhase=3`, flight at 20 m/s) |
|---|---|---|
| **B0** control | `tb-ctl.log` | *(none)* |
| **B12** | `tb-12.log` | `-VoxelTickBudgetMs=12` |
| **B8** | `tb-8.log` | `-VoxelTickBudgetMs=8` |
| **B8-nodefer** | `tb-8-nd.log` | `-VoxelTickBudgetMs=8 -VoxelTickBudgetMaxDefer=1` |

**B8-nodefer is the throttle control**, and it is the leg that makes the claim
falsifiable: with `MaxDefer=1` the budget escalates almost immediately, so it
should behave close to B0. If B8 and B8-nodefer produce the *same* tail, the
deferral is not what improved it.

Budgets chosen against the measurement: M20's flying tick is 8.98-11.49 ms with
recompute at 4.90-6.84 ms of it, so 12 ms should rarely bind and 8 ms should bind
on exactly the recompute ticks — the bursty term the bimodal finding identified.

### Gates

- **G13 (traffic).** `hookedTicks > 0`, and `deferrals > 0` on B8. Either zero =
  the arm says nothing.
- **G14 (the answer).** `stutterPct`, `p99`, `max` on the **`SETTLED-MOVING-LEG`**
  row, quoted with `n` and `meanSpeedMps`. Target direction: `stutterPct` down
  with `p99` and `max` down with it.
- **G15 (the trade, stated).** Cold start from the same legs, beside G14. If
  cold start regresses by more than the window-summed `deferredMs`, **revert**.
- **G16 (starvation).** Per-ring `loaded=` never 0 for >10 s on any arm.
- **G17 (census consistency).** On B8, `wallClock` up and `countCap` down on the
  `Voxel apply stages` line — the independent confirmation that the tick budget
  reached `DrainResults` at all.
- **G18 (control identity).** B0 prints no `Voxel tick budget` line.

## 8. What I have not done

Deferral is wired for **recompute** (the bursty term the tail analysis points at)
and for **apply** (via the existing budget subordination). `DrainUnloads`,
`DrainGameThreadMesh` and `DispatchJobs` are *not* budgeted yet — they each
already have a count cap, and adding three more clocks before knowing whether one
works would be three more ceilings nobody can attribute. If B8 moves the tail,
the same `RemainingSecondsForStage()` extends to them one at a time.

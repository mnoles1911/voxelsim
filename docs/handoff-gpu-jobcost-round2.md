# ROUND 2 — after the coordinator's readings came back

Continues `docs/handoff-gpu-jobcost-2026-08-23.md`. Same lane
(`VoxelGpuMeshJobManager.{h,cpp}`), same rule: **authored, NOT COMPILED AND NOT
RUN** — `af110c78e87d6ff18` holds the build lane.

---

## 6. What round 1 got right and wrong

**Wrong: the denominator challenge.** `AccumGpuManagerTickMs` already separated
the two populations and reads **12-47 ms against a 1,445 ms loop, 1-3%.** The
manager tick is not the cost; the per-chunk framing stands. H1 is hygiene now,
not a decision.

**Right, and it earned its keep:** `[gpu-jobcost]` closed three theories in one
window —

* `subUs=0.32-0.44` — this manager's submit half is trivial;
* `enqDisp/enqPoll ~ 0` — **no render-thread backpressure.** That was my own
  leading hypothesis in round 1, falsified by my own instrument. That is what
  the bracket was built to be able to do;
* `rasterB=1954` in one window — **the raster atlas declines intermittently**,
  so "the atlas fixed it" is void for those windows.

`assetB=0` on this leg, so half one (the asset-table move) is credited with
nothing.

The localisation is `submit=1436.9` of `loopMs=1444.6` — **97-99%** — and inside
it `reqHdr` at 1,302.9 ms (`SampleChunkParamsForPool`: four amplifier columns
per chunk on the game thread, each taking the fine-tier sampler lock
exclusively), now fixed by `-VoxelApplyFast`.

---

## 7. The arithmetic that says STOP optimising this file's Submit

At 50,000/s a 5 s window carries 250,000 chunks against a 5,000 ms game thread.

    this manager's whole Submit half:  0.44 us x 250,000  =  110 ms  = 2.2%
    its single largest term
      (MakeShared<FJob>: one malloc of ~720 B plus ~30 member
       constructions; estimated 40-60% of subUs):
                                       0.26 us x 250,000  =   65 ms  = 1.3%

**Deleting every allocation in `Submit()` buys at most ~1.3% of the window.** An
FJob free-list means a hand-maintained `ResetForReuse()` whose one missed field
is a stale value leaking into the next chunk — the exact silent class this
project keeps paying for. **The arithmetic refuses the trade.** Not shipped,
deliberately, and recorded here so nobody re-derives it from scratch.

Half three (a lean job skipping its mesh-region validate + graph-size) is on the
same scale: ~0.05 us/chunk, **~12 ms/window, 0.25%.** It ships because it is
three lines and removes a term outright, **not** because it is a lever.

> **Nothing left in this file's per-chunk path is worth more than ~2% of the
> window. The 7.1x is not here** — except for the two things below, which are
> not per-chunk costs at all.

---

## 8. `MeshBatchCap` is a 3,840 chunks/s wall, and its justification is gone

    BatchCap (-VoxelGpuPrimary default)   =    64 promotions/tick
    x 60 fps                              = 3,840 chunks/s
    50,000/s needs                        =   834 promotions/tick

**One constant, a 13x shortfall.** The measured GPU fork (~2,078/s) sits at 54%
of it — the regime in which a cap does not yet *look* binding and is about to be.

BatchCap's own comment is explicit that it is **not** a capacity limit: it is a
per-tick burst limiter protecting **render-thread pass setup** ("~7 compute
passes + 3-4 copy passes each"), and the sweep behind it measured 32/64 giving
367 hitches against 4/8's 8. Every one of those hitches was pass setup scaling
with promoted chunks.

**Worklist stage 6 took a claim-fed chunk's brick passes to ZERO** — "a
claim-fed job adds ZERO brick passes to the batch graph ... production is fully
inside the flush graph". For those chunks the entire reason to charge them a
slice of BatchCap has been removed. **This file already accepts that exact
argument once**, for Tier B.1 stack siblings:

> *"a sibling rides its head's allowance instead of consuming one — the
> per-chunk reason for the cap is exactly what the fusion removes."*

Same argument, different mechanism.

### Half four

A predicted pass-free job draws on **its own allowance**, bounded by the
resource that actually bounds it: the worklist's **per-flush consume budget**
(`-VoxelGpuWorklistCellBudget`, default 256). Promoting more pass-free chunks in
a tick than the flush can consume does not make them pass-free — the surplus
falls back to the classic chain at ~15 passes each, which is precisely the hitch
BatchCap exists to prevent.

    default cell budget 256 x 60 fps  = 15,360 chunks/s   (4.0x on the quota)
    Write-triple ceiling 442 x 60 fps = 26,520 chunks/s   (the claim stage's own wall)

**26,520/s is a hard ceiling on the claim stage** (65,535 D3D groups / 148 per
record). That is a finding for the **worklist lane**, not this one: at 50k it
must either split the Write dispatch or raise groups-per-record.

The head is **peeked, not popped**, so strict FIFO is untouched and low-priority
still gets its turn exactly as it does today when demand exhausts BatchCap. With
the switch off `PassFreeCap` is 0, the pass-free branch is unreachable, and the
loop is byte-identical.

**The refutation is `passFreeOverCap=`** — promotions that happened while
`DemandPromoted` was *already* at BatchCap, i.e. that the shipped gate would
have blocked. Zero means this bought nothing, and **no throughput difference
between arms is attributable until it is non-zero.**

---

## 9. `poolReplaced=` — the `revalRan=24,345` finding, given a mechanism

Owned as assigned. `revalRan` said "the identity failed on batches totalling
24,345 job-slots"; it could not say how many chunks. `poolReplaced` does,
exactly and for free — it is the same two deltas the revalidation skip is
already gated on, kept instead of discarded:

    shellsTaken   = the pool's ChunksAdded delta across the shell loop
    poolReplaced  = shellsTaken - (resident-count delta)
                  = chunks that LEFT the resident map to make room

**On a cold fill nothing should be resident yet.** With `evictions=0` the only
mechanism left is `AllocateForChunk`'s same-key replacement, i.e. **a chunk
generated over itself**: a wasted submit, a wasted shell, a wasted graph, a
wasted claim and a wasted delivery. **Whole chunks, not microseconds.**
`poolReplaced/shellsTaken` is the redundancy rate and it multiplies straight
into the 7.1x cold start needs.

A `Warning` fires whenever it is non-zero and names the `evictions=` cross-check
that must be read with it: with evictions > 0 this is churn, and only with
evictions = 0 is same-key replacement the sole mechanism left.

> **This is the only lead in these two files that DELETES work rather than
> shaving it, and it is measured with two integer reads per batch — flat in N.**

---

## 10. `FLOW` — the ceiling, and which of three regimes we are in

Nothing has ever computed this manager's throughput ceiling, and it has been
argued from both ends without one:

* the **2026-07-27 depth sweep falsified** the depth hypothesis — 590 chunks/s
  at cap 1024 against 602 at 256, with the fork idling at ~11 in flight;
* the **2026-08-23 cold fill** shows a five-second dead window at
  `inFlight=0 pending=512` — the opposite failure, a **starved** promoter.

Both cannot be the regime, and only a measurement says which.

    CEILING = MaxInFlight / mean residency in ticks x tick rate

All three terms are now measured in-window, alongside
`promoteExit cap=/quota=/empty=` — the manager's own version of the
`exitCap=/exitEmpty=` pair the streaming side already prints:

| reading | meaning | what to do |
|---|---|---|
| `quota=` dominant | promotion allowance is the limit | direct confirmation of §8 |
| `empty=` dominant | starved; the producer upstream is the limit | nothing here is the fix |
| `cap=` dominant | depth-bound | **check `drained` first** — the 90,000-deep backlog is what makes a cap reading a trap |
| all three ~0 | the promote loop did not run | broken instrument, not a healthy manager |

---

## 11. Hooks — still none required

Everything above prints from the manager's own `Tick` and is armed from the
command line. **H1 is now hygiene only** (`AccumGpuManagerTickMs` already
answered the question it was for); take it when convenient — exact text in §4 of
the round-1 doc.

**H3 (new, and for the worklist lane rather than the subsystem):** if
`passFreeOverCap` is large and the fork rate moves, the next wall is
`-VoxelGpuWorklistCellBudget` at 256 (15,360/s), and behind that the Write
triple's 442-record ceiling (26,520/s). Neither is in this file.

---

## 12. Leg — one arm added

    # ARM C -- the promotion-quota collapse. REQUIRES the claim stage armed.
    pwsh tools/voxel-run-flight-leg.ps1 -LogName jobcost-passfree
         -ExtraArgs @('<STANDING>', '-VoxelGpuWorklistClaim=1', '-VoxelGpuJobLean=1')

Read in this order and **stop at the first one that fails**:

1. **`passFreeOverCap=` non-zero.** Zero = the switch converted nothing; arm C
   measured nothing and no rate comparison is valid.
2. **`promoteExit quota=` must fall**, ARM A vs ARM C. If it does not, the cap
   was not the wall and §8's arithmetic is wrong about this leg.
3. **`wlclaim conv=` growing** (the worklist line). Pass-free jobs must actually
   *be* claim-fed — otherwise they are classic jobs promoted past a cap that
   existed to stop exactly that, and the hitch profile will say so.
4. **`CEILING=` and `residTicks=`** — what replaced the quota as the limit.
5. **`poolReplaced=` on every arm** — the redundancy rate, independent of all of
   the above, and the one number here that can delete work.

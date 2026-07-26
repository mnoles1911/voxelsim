# Handoff: G2 closed and verified, G3 is next (2026-07-25)

Supersedes the earlier 2026-07-25 handoff. All work is on
**`claude/terrain-holes-wip`**. Build is clean; everything below is in the binary
and verified headless.

## State

| Item | Status |
|---|---|
| Concentric rings of holes | ✅ Root-caused and fixed (ring-seam admission, `749ad7b`) |
| ADR-0006 | ✅ Accepted, signed, diagnosis measured not asserted |
| **G1 — GPU greedy mesher** | ✅ Complete. Gate green, bit-exact with the CPU mesher |
| **G2a — kernels in-engine via RDG** | ✅ Complete. Cross-toolchain digest `f3c48a4df3e20e9a` |
| **G2 — GPU geometry pool + custom draw** | ✅ **Complete.** 256 chunks / 876k quads, ONE primitive, ONE draw |
| **G2 incremental upload** | ✅ **Verified.** Partial writes match a full rebuild exactly |
| **G3 — drive the pool from the cascade** | 🟨 **Wired and rendering, not yet correct.** 2893 chunks / 2.4M quads / ONE draw. Coarse rings render as flat slabs. |
| G4 parity | ⬜ Checklist done; two real blockers, see below |
| G5 — flip the default, retire per-chunk components | ⬜ |

## Start here

G3 is wired and the pooled cascade renders. **One bug is left, and it is
precisely located: the R0 ring stops populating.**

Same 30 s headless run, same spawn, only `voxel.Stream.GPU` differing:

| Ring | CPU path | GPU pool |
|---|---|---|
| **R0** | **1947** loaded, 0 pending | **255** loaded, 2 pending |
| R1 | 1319 | 1319 |
| R2 | 1602 | 1602 |
| R3 | 1853 | 1853 |
| R4 | 1675 | 1675 |
| R5 | 1426 | 1426 |

Every coarser ring is byte-identical. Only R0 differs, and `pending` is ~0, so
it is **not** a throughput stall -- the cascade simply stops *asking* for R0
chunks. That makes this a desired-set / admission bookkeeping difference, not a
rendering one, and it is why the picture still looks wrong: the near-field
detail never arrives, leaving the coarse rings' exposed interiors on screen as
large flat slabs. Chasing the slabs is chasing the symptom.

Already ruled out:

- **Coverage/retention** (`ReplacementCovered`) keys on `bMeshSettled`, not on
  the component, so it is already path-agnostic.
- **The three load-bearing `Component.IsValid()` sites** now ask
  `FChunkRecord::HoldsGeometry()` instead (retention gate, unload budget, and
  the record-drop check -- that last one would have leaked pool allocations).
- **Throughput.** The ~170 MB-per-update copy bug that did throttle streaming is
  fixed; it took the pool from 2893 to 8130 chunks and R0 pending from 66 to 2.
  R0's *loaded* count barely moved, which is what shows the remaining problem is
  admission, not speed.

Sharper still, from the ring-dispatch line (`total` = chunks that produced a
result at all, loaded + zero-quad):

```
CPU:  R0 total=3398 load=1947 zq=1451
GPU:  R0 total= 408 load= 255 zq= 152
```

So R0 is not failing to *draw* 3,000 chunks -- it never *meshes* them. `tracked`
is correspondingly lower (11,323 vs 16,417), and `bandCache` is 171 vs 1,288.
Records are going missing upstream of any geometry handoff.

It is specifically R0-being-pooled that costs R0. With
`voxel.Stream.GPUMaxLevel 0` -- only R0 pooled, R1-R5 left on components --
R0 is `total=408 load=255`, **identical** to pooling every level, and R1-R5 are
untouched. So the trigger is a level-0 chunk taking the pooled branch, and the
loss does not depend on pool size, chunk count, or the coarse rings at all.
That an 8x smaller pool reproduces the number exactly is the strongest clue
available: this is a per-chunk logic difference, not a resource or throughput
effect.

Things checked and NOT the cause: `DropFarthestOverCap`'s admission guard (a
never-meshed chunk has neither a component nor a pool slot, so `HoldsGeometry()`
is false there on both paths, exactly as before); `ReplacementCovered`; the
per-update copy throttle (fixed, and it moved `pending`, not `total`).

### Traced: this is the bounded-admission straggler, amplified

It is not in the pooled branch at all. The anchor is byte-identical between the
two runs (`-8448000, 5376000, 55054`), and `tracked` is almost exactly the sum
of the ring totals, so R0 genuinely only ever gets ~408 *records* --
`RecomputeDesiredSet` stops admitting.

R0 is admitted a slice at a time: `AdmissionsThisLevel >= Cap / 4` rejects the
rest of a pass and arms `bAdmissionDeferredWork[0]`. A later pass refills, but
only when that ring's pending queue is **empty**
(`GetPerLevelRefillEnabled` branch, ~2970). And at the end of every
`RecomputeDesiredSet` (~4967):

```cpp
if (bLevelScannedThisCall[Level] && LevelCandidatesRejectedThisCall[Level] == 0)
    bAdmissionDeferredWork[Level] = false;
```

So the refill trigger is disarmed by any pass that happens to enumerate without
rejecting, and re-armed only by a pass that rejects. That is a race against how
fast the ring's queue drains -- and the pooled path drains R0 much faster,
because applying a chunk no longer costs an `FScene::AddPrimitive`. Same code,
same flags, different timing: `scans R0=1` under the pool versus `11` on the
component path.

This is the **"bounded-admission straggler"** already in the backlog below
("a few chunks never load until the player moves. Diagnosed to the refill path,
not fixed"). ADR-0006 turned "a few chunks" into ~3000, because it removed the
very cost that was accidentally keeping the queue non-empty long enough for the
trigger to stay armed.

Two consequences worth stating plainly:

1. **Fix it in the refill path, not in the pool.** The pooled path is behaving
   correctly; it is exposing a latent bug. Likely shape: arm the trigger from
   "this ring still has undesired-but-wanted footprints" rather than from
   "this pass rejected something", so it does not depend on timing at all.
2. **It is a pre-existing CPU-path bug too**, and fixing it should be validated
   on both paths. Anything that makes the component path faster -- which is the
   whole point of the remaining G phases -- will start surfacing it there as
   well.

Two tools for narrowing it:

- `voxel.Stream.GPUMaxLevel N` pools only rings <= N and leaves the rest on the
  component path. Both renderers coexist per chunk, so R0 can be isolated
  directly.
- `voxel.Stream.GPUMaxChunks N` caps pool admission, so the streamed path can be
  run at the small scale `voxel.GPU.SpawnPool` is known good at. That is what
  proved the earlier blank screen was never a scale problem.

Two things G3 uses that already exist and are easy to miss:

- **`UpdateChunk(Handle, Quads)`** re-meshes in place when the new quad count
  fits the existing slot, and only reallocates when a chunk outgrows it. Use it
  for digs. Implementing a re-mesh as Remove+Add fragments the pool hardest on
  exactly the chunks that re-mesh most.
- **`RingPresets`** (`VoxelWorldSubsystem.h:86`) is `static constexpr` and must
  become a runtime accessor before R0 can move to 128 m. Fold that into G3;
  do not flip R0 before G3 lands.

## Verification recipes

Build (close the editor first — it locks the DLL, and a failed link DELETES it):

```
"D:/UE_5.8/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development \
  -Project="D:/voxelsim/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE
```

**Pass the project path as an ABSOLUTE path when launching headless.** A relative
`VoxelEarth.uproject` fails with exit code 1, no log, and no crash report, which
reads exactly like a broken build and is not one.

Cross-toolchain gate, ~30 s:

```
UnrealEditor-Cmd.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -nosplash \
  -unattended -sm6 -ExecCmds="voxel.GPU.VerifyRegion, quit"
```
Both legs must print `f3c48a4df3e20e9a` (bench: `build/voxel-core-msvc/bench/vxc_gpu.exe --radius 64`).

Pool, three modes — the third is the one that matters:

```
UnrealEditor.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -windowed \
  -resx=1280 -resy=720 -nosplash -unattended -sm6 "-VoxelSpawnAt=-84480,53760" \
  -ExecCmds="voxel.GPU.SpawnPool 64 churnlive shot"
```
`shot` writes `Saved/Screenshots/WindowsEditor/ScreenShot00000.png` at 10 s.

- `voxel.GPU.SpawnPool 64 shot` — plain pool.
- `... 64 churn shot` — edits **before** the proxy exists. One full rebuild.
  Does **not** test the incremental path.
- `... 64 churnlive shot` — edits **after** the proxy is up. This is the path
  streaming uses. Pass = the log shows `Live churn`, the `upload:` line count
  does **not** increase after it, and the image matches the `churn` image.

## Four traps that cost real time here — do not rediscover

1. **These buffers must be `EBufferUsageFlags::Static`, never `Dynamic`.**
   Only the static lock path honours a lock offset: it stages exactly the locked
   size and `CopyBufferRegion`s it to that offset (`D3D12Buffer.cpp:750, :801,
   :818`). The dynamic path ignores the offset and returns the buffer's base
   address (`:659`), then renames the whole buffer on every lock after the first
   (`:667, :697`), leaving everything outside the dirty range uninitialised.
   "We write it every frame, so mark it Dynamic" is the intuitive and wrong call.
2. **Screenshot the pool at 10 s, not 3 s.** The pool spawns into the live
   streamed world. A shot taken before the CPU cascade fills shows coarse LOD
   slabs around the pool that read exactly like corrupted pool geometry. That
   cost an hour of chasing a bug that was not there.
3. **`SetRootComponent()` on a fresh `NewObject`ed component installs an
   identity transform**, redefining the actor's location as the world origin —
   8,448 km away, i.e. invisible. Call `SetWorldLocation()` after
   `RegisterComponent()`.
4. **The pool must be the ROOT component of its own actor.** Attached as a
   child of `ChunkRoot` the primitive never enters the visible set at all --
   `GetDynamicMeshElements` is never called, with a live proxy and valid
   bounds. Same trap as (3): `SetWorldLocation` after `RegisterComponent`.
5. **Bounds do not reach the renderer by themselves.** On the incremental path
   the proxy is created once and never rebuilt, and `UpdateBounds()` only
   updates the component -- the scene keeps the bounds it got on frame 1. That
   culled a 6.4 m box at the player's feet while 2.4M quads sat in the buffer.
   `MarkRenderTransformDirty()`, never `MarkRenderStateDirty()` (which rebuilds
   every buffer and defeats the incremental path).
6. **The chunk table is float32 and this world is 84 km wide.** At ~8.4M UU the
   ULP is 1.0 UU against a 10 UU voxel, so chunk origins are stored relative to
   a rebase origin the component carries in its double-precision transform.
7. **`VoxelEarthShaders` must stay `PostConfigInit`**, and `/VoxelCore` is a
   directory *mapping*, not a copy. Copying `worldgen.ush` into the project would
   void the cross-toolchain digest the moment the copies drift.

## G4 parity: the two genuine blockers

Everything else on the checklist is computable in-shader. These two are not:

- **Voxel GI light field** must become a GPU-readable volume. Today the CPU path
  bakes it into vertex colour G.
- **Per-chunk debug tints and ring fade** need a per-chunk buffer. Pooling
  collapses the per-primitive material instances they use today — the chunk
  table is the natural place, and it already carries origin + climate.

Water surface is a separate, near-identical task to the terrain pool.

## Open, not blocking G3

- **Deep-column waste:** 44.6% of R0 worker time meshes buried chunks emitting
  zero quads. The proposed fix — move the band skip from dispatch-time to
  admission-time — was built, measured and **does not reach it**; see "The
  admission-time band skip: built, measured, left off" below. The downward
  escape hatch it needed was built anyway and shipped, because the hole it
  closes turned out to be a live bug rather than a prerequisite.
- **Residual perf cost of the seam fix:** p50 14.92 → 17.52 ms, 968 → 703
  chunks/s. Real extra chunks where holes used to be. Exactly what ADR-0006
  removes.
- **Bounded-admission straggler:** a few chunks never load until the player
  moves. Diagnosed to the refill path, not fixed.
- **Min-spec-proxy M1 gate re-run** still owed; current numbers are
  default-quality and cannot be read against the historical gate rows.
- **`status.md` determinism row is stale** — goldens predate worldgen v6.
- **NVIDIA CI leg** for the G1 gate.

## Two methodology rules earned the hard way

1. **Discard the first run after any build.** Cold PSO state costs ~20%
   throughput and reads exactly like a regression.
2. **Never A/B this renderer through scalability cvars.** `r.ShadowQuality 0`
   desyncs the BeginPlay PSO precache and measures precache invalidation instead.
   Change one primitive flag, or use `r.ScreenPercentage`.

## R0 admission: one hypothesis tested and disproven (2026-07-25)

`voxel.Stream.LogAdmission 1` now prints the loop state per level per
`RecomputeDesiredSet` call. Under `voxel.Stream.GPU` it shows:

```
R0[scan=1 rej=4582 q=512 def=1]
R0[scan=1 rej=4070 q=512 def=1]
R0[scan=1 rej=3558 q=512 def=1]    ... -512 per pass, ending def=0 q=0
```

Read that carefully, because it kills the two readings that came before it:

- **The refill trigger is not stuck.** R0 re-enumerates on nearly every call
  (a level-0 chunk is 3.2 m, so the anchor crosses one constantly), takes
  exactly its `Cap/4` = 512 pass budget, and correctly re-arms `def=1`. My
  earlier "the deferred flag is cleared too eagerly" reading was wrong.
- **`DropFarthestOverCap` is not eating the records either.** `kRingCapShare[0]`
  is 0.25 and `Cap` is 2048, so R0's queue cap is *also* 512 -- the same number
  as the pass budget. I hypothesised admission was filling a full queue and the
  trim was deleting what it had just created, and added a guard refusing to
  admit into a ring queue already at its cap. **It changed nothing**: R0 stayed
  at `total=408` and the CPU path stayed at `3398`, byte-identical. The queue
  goes 0 -> 512 *within* a pass, so it never exceeds the cap and the trim never
  fires. Guard reverted rather than left in shared admission code unproven.

So the live contradiction, stated precisely for whoever picks this up:

> Across ~9 passes R0 rejects 512 fewer candidates each time, which can only
> mean 512 more of them now hold records. That implies ~4,600 R0 records exist.
> But `tracked` (11,323) minus the R1-R5 totals (10,800) leaves ~520 -- about
> ONE pass worth. So R0 records are being created and then removed somewhere
> between admission and dispatch, and the rejection count still falls as though
> they had survived.

Resolve that first; it is the whole bug. The two counters to reconcile are
`RecordsAddedSinceLog` against `RecordsDroppedSinceLog` / `RecordsEvictedSinceLog`,
per level -- neither is currently broken out by level, and that is the next
instrumentation to add rather than the next fix to guess at.

Reminder that this is a **CPU-path bug too**: the component path only looks
healthy because the loop spins slower there. Any fix wants a measured
before/after on both paths.

### The R0 freeze is located: two undispatchable queue entries (2026-07-25)

Per-level record accounting settles what earlier readings got wrong:

```
R0[+512  -0    ev0]      <- and +0 on every window after
R1[+3019 -760  ev0]
R3[+2560 -1025 ev0]
```

R0 admits **exactly one pass worth of records, once**, and never admits again.
Nothing removes them (0 dropped, 0 evicted). 512 records -> 408 results
(255 drawn + 152 legitimately zero-quad), which is the entire shortfall.

The admission log says why:

```
R0[scan=0 rej=0 q=2 def=1 cut=-1m]     ... every pass, from the first
```

- `def=1` -- the refill trigger is armed.
- `cut=-1m` -- no distance cutoff; it would admit anything.
- `scan=0` -- but it never re-enumerates.
- `q=2` -- **because refill requires the ring's queue to be EMPTY, and two
  entries never leave it.**

A ring re-enumerates only when the anchor crosses one of its chunks or when its
refill trigger fires. On a stationary anchor the first never happens, and the
second is gated on `PendingJobKeysByLevel[Level].IsEmpty()`. Two stuck entries
therefore starve the entire ring, permanently, with its trigger armed the whole
time. This is `docs/status.md`'s **"bounded-admission straggler"** -- "a few
chunks never load until the player moves" is the same bug seen from a moving
anchor, where crossing a chunk boundary rescans the ring and hides it.

Four hypotheses tested and killed by measurement, all reverted, none of them it:

1. Refill trigger cleared too eagerly -- no, `def=1` throughout.
2. Admission filling a queue that `DropFarthestOverCap` then trims -- guard
   added, **byte-identical results on both paths**; the queue never exceeds cap.
3. The `HoldsGeometry()` guard in `DropFarthestOverCap` blocking drops -- swapped
   back to `Component.IsValid()`, **no change**.
4. Dead queue entries (no record) blocking `IsEmpty()` -- pruned them in
   `SortPendingQueues`, **no change**, so the two entries DO hold records.

So: two entries with live records sit in R0's queue and never dispatch. The next
question is the only one left -- why does `DispatchJobs` skip them every pass
without removing them? Prime suspects are the buried/sky pre-dispatch skip sites,
which clear `bJobInFlight` and `continue`; if they do not also pop the key, it
loops forever.

Two design notes for whoever fixes it:

- **`IsEmpty()` is the wrong predicate for the refill gate** regardless of why
  those two are stuck. "This ring has no dispatchable work" is the property
  actually wanted, and it should not be defeatable by a bounded number of
  permanently-stuck entries.
- **This is a CPU-path bug.** The component path shows `pending=0` only because
  it is slow enough that R0 keeps rescanning for other reasons. Fix it with a
  measured before/after on both paths.

### The full chain, and the one link still missing (2026-07-25)

Dumping the stuck entries names them exactly:

```
stuck R0 (-26401,16786,169) rec=1 inFlight=0 settled=0 quads=0 overlay=0 dist=44m
stuck R0 (-26401,16786,170) rec=1 inFlight=0 settled=0 quads=0 overlay=0 dist=44m
```

Two chunks, one XY column, adjacent Z, 44 m out, records live and idle. They are
deferred every dispatch pass by the **cold-band throttle**
(`VoxelWorldSubsystem.cpp:5306`): a level-0 chunk whose column is absent from
`FootprintBandCache` but present in `FootprintBlindJobInFlight` is pushed onto
`DeferredColdBand` and put back, forever.

So the chain is:

```
column (-26401,16786) stuck in FootprintBlindJobInFlight
  -> its 2 R0 chunks deferred on every dispatch pass, never dispatched
  -> R0's queue never reaches empty
  -> the refill trigger, though armed (def=1) and unconstrained (cut=-1m),
     never fires because it is gated on IsEmpty()
  -> R0 never re-enumerates on a stationary anchor (scan=0)
  -> R0 frozen at the 512 records of its single initial scan
  -> 255 chunks drawn against ~1950 desired
```

**This is not the pooled path.** The mark is cleared in `DrainResults`
(`:6156`) for EVERY level-0 result, before the stale-result discard and before
`ApplyMeshResult` is reached, so it is independent of which renderer runs. The
GPU path only changes how fast the loop spins.

The missing link is why that column's seeding job result never arrives to clear
the mark (`:5666` adds it at the launch site). Note the comment at `:5300`
documents a previous instance of exactly this failure -- "the whole column stayed
deferred forever (observed as blindInFlight pinned at 92)" -- fixed then by moving
where the mark is set. This is a second path to the same state.

Two fixes are wanted, and they are independent:

1. **The leak**: ensure the blind-job mark is always released -- either by
   guaranteeing a result for every launch, or by ageing the mark out. The
   `:5300` comment shows the "guarantee a result" approach has already been
   attempted once and has a history of missed exit paths.
2. **The amplifier**: `IsEmpty()` is the wrong refill predicate regardless. A
   ring should refill when it has no *dispatchable* work, so a bounded number of
   permanently-deferred entries cannot starve it. Fixing only (1) leaves the
   next stuck entry free to do this again.

## The admission-time band skip: built, measured, left off (2026-07-25)

The "deep-column waste" item above proposed moving the buried-chunk band skip
from dispatch-time to admission-time. It is built, it is correct, and it is
**default-off** (`voxel.Stream.AdmissionBandSkip`, 0/1/2) because measurement
says it cannot reach the waste it was aimed at. The escape hatch it needed is
ON, and is the part that mattered.

### Why it cannot reach the waste

The band is derived from a level-0 job's own 34x34 column grid and lands in
`FootprintBandCache` in `DrainResults`. So it **does not exist until some
level-0 job in that footprint has completed AND drained.** Admission runs
before that, and `AddCandidate` admits a footprint's ENTIRE
`ChunkZMin..ChunkZMax` column in the single pass that first scans the
footprint. By the time the band arrives, every chunk it could refuse is
already a record, and `AddCandidate`'s `ChunkRecords.Contains` guard means it
is never reconsidered.

Mode 2 (compute the verdict, admit anyway) over one full stationary fill at
`-VoxelSpawnAt=-84480,53760`:

```
Voxel band skip at admission:  warm=7325 cold=8131 skipped=89
Voxel buried skip (dispatch, same run):        skipped=2186
```

**89 against 2186** -- ~4% of the opportunity. The `warm` scans are almost all
re-scans of footprints whose chunks are already records, so they have nothing
to refuse. Running mode 1 for real removes 61 records (`tracked` 16417 ->
16356) and **zero quads**.

**That 4% is a property of the stationary fill, not of the lever, and reading
it as the general case would be wrong.** On `-VoxelPerfFlight=surface` the
same census reads `warm=46258 cold=0 skipped=5443` per 5 s window against the
dispatch-time skip's `skipped=5359` in the same window -- essentially **1:1**.
A moving anchor evicts footprints behind it and re-enters them later, and
`FootprintBandCache` is pruned only at twice level 0's unload ring, so a
re-entering footprint arrives with its band already warm. `cold` collapses to
0 about 40 s into the flight. So the predicate IS available under movement,
which is the case the 44.6% figure came from. What follows is why that still
did not turn into throughput.

And the 61 it does remove cost no worker time to begin with: the dispatch-time
skip already refuses to mesh them. Moving the same predicate earlier saves a
record, a queue slot and an admission slot -- not a job. The 44.6% of R0
worker time in the item above is the chunks the band *declines* to skip (`R0
total=3396 zq=1449`, 42.7% zero-quad on this spawn), and evaluating the same
predicate one stage earlier cannot skip more of them.

### Throughput A/B: it measured as a large REGRESSION, unexplained

Interleaved, one binary, `-VoxelPerfRun=60 -VoxelPerfFlight=surface`, all four
runs shown, first-after-build discarded:

| run | chunks/s | postWarmup p50 | hitches |
|---|---|---|---|
| off 1 | 595.7 | 21.01 | 129 |
| on 1 | **309.3** | 33.85 | 745 |
| off 2 | 695.9 | 16.45 | 11 |
| on 2 | **374.3** | 25.62 | 558 |

Both on-runs lose to both off-runs with no overlap. **-46% chunks/s.**

Inside the subsystem the change does exactly what it was designed to do:
`tracked` 15,003 -> 5,256 (**-65%**), streaming tick 4.03% -> 1.51% of wall,
exit scan 1.13 -> 0.54 ms. The cost is somewhere else entirely -- ticks per
5 s window fall **279 -> 89** (~56 fps -> ~18 fps), and everything else on the
streaming side follows from that: with a third of the passes covering the same
20 m/s flight, each pass sees far more ground, `candidatesRejected` goes
31,872 -> 78,138, and the outer rings starve (R1 loaded 878 -> 59, R2 221 ->
32, R3 339 -> 78).

**I could not isolate what makes the frame slower, and I am not going to guess
at it.** The two off-runs differ from each other by 100 chunks/s and 11 vs 129
hitches, which is the signature of external CPU contention -- this box was
shared with two other agents running editors throughout. Two pairs is not
enough to separate "the skip does this" from "the machine was busy", and the
one causal story that fits (fewer tracked records -> `ReplacementCovered`
releases retained stand-ins sooner -> more re-meshing) was not tested.

What is certain either way: the lever's whole claimed benefit was worker time,
it demonstrably saves none, and enabling it measured 46% worse. Default 0.
Anyone re-opening this needs a quiet box and at least three pairs.

### What would actually reach it

Not this. The residual waste is (a) blind seeds on cold footprints and (b)
chunks the band is not tight enough to prove empty. Neither is an admission
problem. The shape that WOULD work is to defer the sub-surface part of a cold
footprint's column by one pass so the band is warm when it is scanned -- i.e.
move the **cold-band throttle** to admission, not the skip. That is a
different mechanism with a real latency risk and it was not attempted here.

### The downward escape hatch, which shipped

Intended as a prerequisite; turned out to close a live hole. `ChunkZMin` is
worldgen's floor (lowest corner surface, -1 chunk, minus the 12-chunk depth
skirt = ~41.6 m at level 0 near band). A player digging past it leaves the
bottom of the shaft outside the desired set, and the anchor-relative deep box
cannot cover for it because that box only exists once the anchor is itself
underground. `-VoxelDigDownTest`, 60 m shaft, pawn stationary on the surface:

| | tracked | deepestTracked | deepestGeometry |
|---|---|---|---|
| `-VoxelNoEditFloorHatch` | 13/25 | 41.6 m | 41.6 m |
| hatch on | 19/25 | **60.8 m** | **60.8 m** |

**Two things the one-line description of this fix does not cover, and both
were found by running it, not by reading it:**

1. **The widening alone does nothing.** `RecomputeDesiredSet`'s entry scan is
   gated on the anchor having crossed a level-L chunk. A player standing still
   and digging down widens `EditedFootprintMinZ` and no scan ever reads it --
   the first build of this fix produced a result byte-identical to no fix at
   all. `PropagateEditToMips` now clears `bHasRecomputedLevel[Level]` when a
   footprint's recorded edit range actually changes (at most once per 3.2 m of
   new depth per footprint; measured 152 clears for a whole 60 m shaft).
2. **A saved world came back with the hatch dead.** `World::replay` writes the
   overlay directly, so `EditedFootprintMinZ/MaxZ` and `EditedAncestorChunks`
   were all empty after a load and the hole returned on reload -- which also
   means the pre-existing **sky-band** hatch has been losing saved structures
   above the trimmed band. `RebuildEditedFootprintsFromOverlay()` rebuilds all
   three from the restored bricks at load. Verified: a fresh process loading
   the saved shaft streams it to 60.8 m.

### Correctness

Stationary spawn, fully settled (`jobsInFlight=0 pendingJobs=0`), skip off vs
on: `loaded=9822 quads=8813242` **both sides**, per-ring loaded identical at
every level. Only `tracked` moves (16417 -> 16364).

### Trap for whoever runs this next

A git worktree of this repo has no `tile-cache/`, so the amplifier silently
falls back to the **synthetic** sampler and every number moves. Junction it in
before measuring anything (`New-Item -ItemType Junction`). With it in place
this box reproduces the reference spawn at `loaded=9822 quads=8813242
tracked=16417` against the recorded `9819 / 8809945 / 16417`.

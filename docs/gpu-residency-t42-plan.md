# T4-2: GPU-resident residency — design + shadow implementation

**Date:** 2026-08-23
**Status:** SHADOW ARM IMPLEMENTED (mode 1). LIVE ARM (mode 2) WIRED
2026-08-23, per §7 exactly (its prerequisite, the AddCandidate extraction,
landed as `AdmitCandidateCommit` + `AdmitCandidateEvaluate`): mode 2 consumes
the delta and skips both CPU walks; edits / cold / underground / first-scan /
starvation lanes stay CPU and are counted on the `[gpu-resid] live:` line.
Neither arm has completed a validation leg yet.
**Switch:** `-VoxelGpuResidency=1` or `=2` (command-line latched, for
`-VoxelPendingJobCap`'s reason: an `-ExecCmds` cvar lands after streaming has
built its desired set). Absent = mode 0 = the control arm, byte-identical.
**Prior design:** `docs/deep-review-streaming-perf-2026-07-27.md` T4-2 (§359-364),
downgraded then to "a bet, not a prerequisite" at a 3.6% exit scan. The premise
changed: 271,549 tracked records, `Dispatch exits: cap 0 / empty 89 ->
admission-limited`, and `Saved/phase1-split.log` splits recompute as admission
~65% / eviction walk ~30% / sort ~2% / fine tick ~1%.
**Files:** `ue-project/Shaders/VoxelResidencyScan.usf`,
`ue-project/Source/VoxelEarthShaders/{Public/VoxelResidencyGpu.h,
Private/VoxelResidencyGpu.cpp}`, plus the seam in `VoxelWorldSubsystem.cpp` (§8).

---

## 1. What is actually on main vs what the docs claim (checked 2026-08-23)

`docs/gpu-streaming-architecture.md` is accurate, and current to today:

| claim | verdict |
|---|---|
| P1 GPU pool suballocator merged, default off, latched at pool Init | **Real.** `VoxelBrickPool.cpp:509-` (`voxel.GPU.PoolAlloc`, latch warning text present), layout + three allocator buffers in `VoxelBrickPool.h:360-441`, kernels in `VoxelBrickPoolAlloc.usf`. Has its own gate counters (`doubleGrant`/`badFree`/xcheck/`unwritten`/`claimFail`). |
| P2 GPU-written march index merged, default off | **Real.** `voxel.March.IndexGpuResident` (`VoxelMarchChunkIndex.cpp:170`), publish kernel + ordering argument at `:1398-1470`, verify ladder present. |
| Neither P1 nor P2 has ever executed | **True** per the doc's own status section; being validated on the editor tonight. |
| GPU fork residual cost is game-thread submission (dispatch 23→496 ms) | Matches the measured table in the doc; nothing in the code contradicts it. |

So the streaming *generation* fork is real and gated; what did **not** exist
anywhere is any GPU participation in the **desired set** — `RecomputeDesiredSet`
was 100% CPU. That is the gap this phase fills.

## 2. The constraint that shapes the design

Fine-tile residency **cannot be queried off the game thread**, and all
streaming state is game-thread-only. So the GPU may *propose*, and the CPU must
*adjudicate* — and the adjudication must be O(delta), or the walk has merely
moved (the exact failure Aokana's "decouple, don't relocate" warns about; their
chunk selection is CPU-driven and the GPU is never waited on — the transferable
principle is the missing synchronization, not the relocation).

What makes O(delta) possible: both walks are **pure geometry over the anchor**
plus exactly two pieces of mirrorable state —

1. **Which chunks are tracked** (`ChunkRecords` keys + pendingUnload + the
   deep-box flag). Mirrored as a per-level toroidal grid (`ShadowGrid`,
   128³/level, wrap-tagged cells so the full coordinate is reconstructible —
   the march index's rolling-window shape, 8 MiB/level).
2. **Each footprint's Z range** (the `FootprintZRangeCache` memo — amplifier
   is CPU-only). Mirrored as per-level 128² texels (`ZRangeGrid`), uploaded as
   deltas from the memo's own two exit points, inheriting the memo's
   resident-tiles-only rule verbatim. Skirt is applied in-kernel (it is
   anchor-dependent; the memo never held it).

**The mirror is never an authority.** Every cell transition is a CPU-confirmed
feedback op staged at the exact sites that mutate `ChunkRecords` /
`PendingUnloadSet` (two adds, two removes, mark, clear — the complete set; the
grep is in §8). Aliasing collisions (two live records 128 chunks apart on one
slot — unload-drain stragglers under long flight) eject the older key to an
**orphan lane**; the CPU keeps those on a residual ledger it owns. The mirror
refuses to silently own what it cannot represent.

### The pass (per recompute, ~9 Hz at 30 m/s, one RDG graph)

FeedbackCS (ordered, single-lane) → ZRangeCS (dedup'd scatter) →
**ExitScanCS** (thread per cell, all levels: the eviction walk's three tests,
verbatim) → **EntryScanCS** per gated level (the annulus XY tests including the
seam-parent exception and both pads, verbatim; Z loop against the texel;
tracked cells subtracted by the mirror; pendingUnload cells become RESURRECT
proposals; cold texels emit the footprint to the CPU lane) → AuditCS every 32
dispatches (XOR-reduce of the whole mirror vs the CPU's incremental ledger) →
readbacks. Nothing waits: deltas are consumed whenever they arrive.

### The O(delta) argument for the live arm

Per consumed delta, the CPU does: per **admit** proposal — `ChunkRecords.Find`,
the fine-tile gate, parked-adoption check, cap/budget, queue push: O(1) each,
list bounded by the GPU-applied per-ring cutoff plus a hard 64k cap (overflow
truncates; unconfirmed proposals re-propose next scan by construction). Per
**evict** proposal — one double recheck + retention stamp + unload queue: O(1).
Feedback staging: O(mutations). Residual walk: O(stragglers), reported, and
its growth is a failing reading. Edited footprints: O(edits in annulus),
CPU-enumerated (edits stay on the CPU — the owner's split). **No O(records)
and no O(annulus) term remains on the game thread.** The one deliberate
exception: underground recomputes stay fully CPU (deep box + solid-skip proofs
are amplifier-bound), counted per skip — surface flight is the measured
limiter, and the split stays lopsided at the producer level with ONE
adjudicator, per the owner's decision.

## 3. Modes

- **0 (default):** one latched-int check per seam site. Control arm
  byte-identical.
- **1 SHADOW (implemented):** dispatch at the top of `RecomputeDesiredSet`,
  *before* the CPU walks, from the pre-walk mirror state — so the pass
  predicts exactly the decisions the call is about to make. The mutation notes
  double as the per-recompute decision ledger; when the delta arrives the
  comparator holds the two against each other. Streaming decisions unchanged.
  Adds cost (notes, dispatch, compare) — this arm buys *validation*, mode 2
  buys the win.
- **2 LIVE (wired 2026-08-23):** consumes the delta, skips the CPU walks;
  adjudication and every fallback lane per §7. The `[gpu-resid] live:` line
  carries its counters and their failing readings (documented at the log
  site in VoxelResidencyGpu.cpp).

## 4. The gate, and what FAILURE looks like (stated in advance)

The `[gpu-resid]` 5 s line. A dead kernel **cannot read as a pass**: the
dispatch runs before the CPU walks, so a pass that appends nothing maximises
`adMISS-SURFACE` while the CPU admits thousands.

| reading | meaning |
|---|---|
| `adMISS-SURFACE > 0` (samples logged) | **FAIL.** CPU admitted a surface chunk with a warm texel, away from every boundary, that the pass did not propose. The kernel is wrong for it. |
| `evMISS`/`evEXTRA > 0` (non-boundary) | **FAIL.** Exit kernel and CPU walk disagree on a tracked record. |
| audit mismatch streak ≥ 3 | **FAIL** (logged as such); mirror drifted; auto-resync fires and the run's compare numbers are tainted. |
| `admit=0` across a moving leg | **FAIL** (the traffic reading; also surfaces as adMISS-SURFACE). |
| `cold` not decaying with warm texels | the Z-texel lane never warms; the pass is handing everything back. |
| `adBnd`/`evBnd` small and stable | expected float-vs-double boundary jitter (classifier re-runs mismatches in double; ε=1e-4 relative on squared quantities, ~500× float's worst case here). NOT failure. |
| `extra` (proposedExtra) nonzero | expected — the CPU's own gates (fine tier, caps, budget, band-skip) reject proposals. Trend matters, not zero. |
| `adDeep`, `adHatch`, `adCold`, `adOvf`, `skipUG` | classified-excusable lanes, each counted separately so they cannot hide inside a pass/fail. |

Known comparator blind spot, recorded now: an **edited** (overlay-aware) chunk
bypasses the admission cutoff on the CPU but not in the kernel; if one lies
beyond the cutoff it reads as `adMISS-SURFACE`. If samples land on edited
footprints, check `EditedFootprint*` before blaming the kernel.

## 5. Dependency on P1/P2, and the fallback if they fail

**This pass requires neither.** The mirror is its own buffer — not the march
index (whose residency bit means "bricks resident", not "record tracked", and
whose P2 publish kernel clears cells this design must not fight), not the
brick pool. If P1 fails tonight: streaming keeps the CPU allocator; T4-2 still
removes the recompute limiter; the 50k/s *end-to-end* target stays blocked by
the readback fence — that is P1's problem, not this phase's. If P2 fails: the
index stays CPU-written; no interaction (the admission site's HoleStats-2
annotation call sits in CPU code on both arms and is untouched). What P1+P2
*do* decide is how much the live arm is ultimately worth: with them, admission
is the last O(big) game-thread stage; without them, dispatch/apply costs
return to the top before admission does (the architecture doc's finding #3).

## 6. The matched A/B leg (mode 1 vs mode 0) — for the main session to run

Standard harness, standard two-leg minimum, quiet box:

- **Control:** the standard streaming leg, no new flags.
- **Shadow:** identical plus `-VoxelGpuResidency=1`.
- Read `[gpu-resid]` via `tools/leg-summary.sh` conventions (never
  `grep | tail -1`). Gate: traffic nonzero while flying; `adMISS-SURFACE=0`,
  `evMISS=0`, `evEXTRA=0`, audit `fail=0` with `pass>0`; `cold` decaying;
  boundary buckets small/stable. Also confirm the control-arm invariant: mode
  0's streaming counters match historical legs (the seam compiles to a latched
  check).
- Cost readings (shadow arm only, informational): recompute stage timings
  should be UNCHANGED (the walks still run); the notes/dispatch overhead shows
  up in `tickMs` minus parts.
- **Incompatibilities to avoid in the same leg:** none known. Compatible with
  `voxel.March.HoleStats 2` and with P1/P2 validation flags — but do not stack
  first-run validations of three features in one leg; attribute one at a time.

## 7. The LIVE arm wiring plan (IMPLEMENTED 2026-08-23)

Implemented as written below, with two additions: a STARVATION fallback (8
consume-eligible recomputes with no delta run the full CPU walks that call,
counted as cpuFallback -- a dead GPU path degrades to the CPU arm and reads
as that counter climbing, never as a working GPU path), and per-level scan
STAMPING on consume (a consumed delta that scanned level L stamps
`LastAnchorChunkPerLevel`/`LastEntryScanAnchorXY` from its own dispatch
params and clears the refill/edit cause flags, which is what keeps the
refill/stale-scan/view-rescan triggers and the dispatch gate working with no
CPU sweep in the loop). The original plan, for reference:

1. Extract `AddCandidate` (the lambda in the entry loop) into a member
   callable with explicit (footprint, key, sortKey) — mechanical, one site,
   but it sits in the middle of the incremental-admission agent's ground.
2. `ConsumeResidencyDelta()`: iterate arrived admits → extracted AddCandidate
   (fine gate, parks, caps, overlay routing all intact); arrived evicts →
   double recheck + today's retention/queue block; resurrects → today's
   resurrection block; cold footprints → CPU-enumerate that footprint (Z memo
   + hatches + AddCandidate) and let the texel note warm the mirror; orphans →
   residual ledger, walked with today's evict math, O(residual).
3. Guard the two walks with `if (mode < 2)`; keep sort/truncate/queue-filter
   (2%) CPU; edited footprints in annulus enumerated CPU-side each consume.
4. Confirmation flow is ALREADY the note calls — no new sync needed.
5. Gate for the live leg: recompute window-sum collapses toward
   sort+fine-tick; `uncovered` (the owner's metric) not regressed; admission
   latency (cross-boundary → first admit) bounded by delta latency ~1-3
   frames; and the mode-1 comparator numbers stay clean when re-armed in a
   spot-check leg.

## 8. Conflict surface in `VoxelWorldSubsystem.cpp` (complete list)

All additive; line numbers as of this commit's base (`d86bfd1`):

1. `#include "VoxelResidencyGpu.h"` — top of file (after the
   `VoxelMarchChunkIndex.h` include, line ~16).
2. `FootprintChunkZRangeCached` — one Note line in the memo-hit branch, one
   after the memo Add (~10697-10760). Untouched by the other waves.
3. **Dispatch block** (~95 lines incl. comments) inserted between the cutoff
   relaxation and the `// 1. Hysteresis exit` comment (~11566). Reads members
   only; writes none. *Sits between the eviction agent's and admission
   agent's territory.*
4. `NoteUnloadQueued` — 4 lines after `EvictedThisCall.Add(LevelKey)` in the
   exit walk (~11760). **Inside the bucketed-eviction agent's ground.**
5. `NoteUnloadCancelled` — 4 lines in the resurrection branch of
   `AddCandidate` (~12310). **Inside the incremental-admission agent's ground.**
6. `NoteRecordAdded` ×2 — after both `ChunkRecords.Add(LevelKey)` sites
   (adoption ~12370, admission ~12580). **Ditto.**
7. `OnRecomputeEnd` — 3 lines after `FlushAbsentMarks()` (~12820).
8. `NoteRecordRemoved` — after `ChunkRecords.Remove(Entry.Key)` in
   `DropFarthestOverCap` (~12850) and after `ChunkRecords.Remove(Key)` in
   DrainUnloads (~18310).
9. `FVoxelResidencyGpu::Get().Reset()` — next to `Index.Detach()` /
   `Pool.Reset()` in `Deinitialize` (~20620).

**Merge rule for the other agents:** if a `ChunkRecords.Add/Remove` or
`PendingUnloadSet.Add/Remove` site moves or a new one appears (bucketed
eviction is the likely one), the matching Note call must move with it — a
missed note is exactly what the audit gate exists to catch (mismatch → logged
FAIL → resync), so the failure is loud, not silent. New LOD rings: the manager
sizes for 8 levels and takes `NumLevels` per dispatch; nothing hardcodes 6.

---

## 9. The first live leg, read correctly (2026-08-23, `Saved/t42-live.log`)

The leg was scored as "dead path": `adOK=0` with `cons=427`, throughput
2,401 -> 2,007/s (-16%). **That reading is wrong, and it violated §6's own
rule ("never `grep | tail -1`").** The quoted window
(`cons=427 sup=0 empty=0 noDelta=105 ... ms ev=0.02 ad=0.70`) is the log's
FINAL window — minutes after the flight ended, anchor parked, records frozen
at 239,815, `fb staged=0`. A converged, parked world proposing nothing is the
pass working. The flight windows just above it show the pass proposing
heavily:

| window (08:1x) | ad prop | adOK | rejBud | ev prop / q | cold prop→enum | records |
|---|---|---|---|---|---|---|
| 10:39 | 1,595,376 | 35,840 | 1,526,218 | 0 / 0 | 16,384 → 2,048 | 22,457 |
| 10:49 | 1,972,226 | 71,619 | 1,866,563 | 0 / 0 | 0 | 70,240 |
| 11:29 | 637,451 | 28,160 | 597,841 | 0 / 0 | 0 | 159,475 |
| 11:39–12:04 | 0 | 0 | 0 | 0 / 0 | 0 | 168,147 (frozen: parked) |
| 12:09 | 71,539 | 42,929 | 96 | 19,276 / 7,320 | 10,143 → 10,143 | 199,380 |

Also healthy across the whole leg: `audit pass=2..14 fail=0` with cpu==gpu
counts at every audit, `VETO=0`, `cpuFallback=0`, `firstScans=0` after
warm-up, `fb dropped=0 collide=0 residual=0`.

### The real defect the log shows: a tick-rate recompute storm

`disp=427/5s cons=427 empty=0` **while parked** is a genuine bug — the
converged world never goes quiet. Chain, each link verified in code:

1. Budget/cutoff/fine rejections arm `bAdmissionDeferredWork[L]`
   (VoxelWorldSubsystem.cpp:12639/12908/12943/13016). In the flight phase
   rejBud runs to millions per window, so every level arms.
2. The ONLY clear (`:15173`) requires `bLevelScannedThisCall[L]`, whose only
   writer was the CPU entry sweep (`:14107`) — **which mode 2 skips**. In
   live mode the flag was a one-way latch.
3. Queue drains (converged) → deferred-refill trigger (`:7639`) fires EVERY
   TICK: `bLevelWantsRefill` → gate cleared + recompute forced → dispatch
   with `bScanThisDispatch=true` → the scan of a fully-tracked annulus
   returns zero proposals but a scanned level → the delta is STAGED (it is
   stampable), never empty-retired → `HasActionableDelta()` (`:7804`) forces
   the next tick's recompute. ~106 recomputes/s, ~85 dispatches/s, forever.
4. While flying, the same loop multiplies recompute cadence 3–7x over the
   crossing gates, so the capped admit list (65,536, `ovf` bit set in the
   early windows) is re-adjudicated ~70x/5s: `ad ms` 260–637 per 5 s window
   (5–13% of the game thread), plus a full `SortPendingQueues` +
   `TruncatePendingJobQueue` per forced recompute. **That is the -16%.**

### The fix (this commit)

One line in the live stamp block (`VoxelWorldSubsystem.cpp` ~14880):
`bLevelScannedThisCall[Level] = true;` — the GPU scan of level L is this
call's scan of level L, so it earns the sixth stamp alongside the five the
block already writes. The deferral clear's other guards (zero rejections
this call, not held back, not clamped) still hold the flag armed whenever
work is genuinely waiting, so the refill trigger's purpose is intact.
Post-fix parked sequence: one final consume clears the deferral, the next
dispatch has `ScanMask=0`, its delta is empty-retired, `HasActionableDelta`
goes false, silence. Control arm and mode 1 untouched (the line is inside
the mode-2-only stamp block).

Plus instrumentation in `VoxelResidencyGpu.cpp`: the live line now prints
`move=` (XY anchor displacement across the log window), so an all-zero
window is readable alone — `prop=0, move~0` is convergence; `prop=0, move >>
chunk edge` is the dead path.

### Re-run gate for the next live leg

- Parked/converged: `disp` per window collapses to ~0 (was 427) and `empty`
  ticks once per re-arm, not never.
- Flying: `cons` per window near the readback-latency bound (~20/s), not
  tick rate; `ad ms` per 5 s window under ~100 ms and decaying as rings
  fill; `adOK`/`ev q` tracking the control arm's admission/eviction volume;
  `audit fail=0`; `cpuFallback=0`; per-ring residency tracking control.
- Throughput judged only against those, per §6.

### Known remaining risk (not addressed tonight)

Per-consume adjudication walks the full admit proposal list to feed
nearest-first commit — O(annulus backlog) bounded by the 65,536 cap, not
O(accepted delta). It self-decays as rings fill (prop fell 2.4M → 0.36M/5s
across the leg) and at natural consume cadence costs ~1–2% game thread in
the worst window, but if the next leg still shows `ad ms` high with the
storm gone, the fix is a cheap distance-key partial select before
`AdmitCandidateEvaluate`, keeping nearest-first semantics — not a kernel
change.

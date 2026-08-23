# The 50:1 reject ratio: cause, and the GPU admission budget

2026-08-23. Files: `ue-project/Shaders/VoxelResidencyScan.usf`,
`ue-project/Source/VoxelEarthShaders/{Private/VoxelResidencyGpu.cpp,Public/VoxelResidencyGpu.h}`,
`tools/lint-residency-globals.py`, `tools/voxel-check-residency-shader.ps1`.

Nothing here was built or flown. Everything below that is a NUMBER comes from a
log already on disk; everything that is a PROJECTION is labelled as one.

---

## 1. Where the 485,000 rejected candidates/s come from

The per-level, per-reason split added earlier on 2026-08-23
(`VoxelWorldSubsystem.cpp:11129`, token `Voxel admission detail`) had never been
read. Aggregated over every window of three matched cold-start legs:

| leg | rejBudget | rejCutoff | rejFine | rejNearest | total |
|---|---|---|---|---|---|
| `gp-ctl2` | 22,269,674 (**99.13%**) | 195,889 (0.87%) | **0** | **0** | 22,465,563 |
| `gp-ctl`  | 36,940,946 (**99.47%**) | 198,005 (0.53%) | **0** | **0** | 37,138,951 |
| `pool-pri`| 36,314,438 (**99.99%**) |   2,245 (0.01%) | **0** | **0** | 36,316,683 |

Per ring on `gp-ctl2`: R0 15,987,750 · R1 1,108,885 · R2 1,124,784 ·
R3 1,165,986 · R4 1,341,823 · R5 1,540,446. **Level 0 alone is 71% of the leg's
rejections**; on `gp-ctl` it is 83%.

Three findings, and the first one overturns the received diagnosis.

**(a) It is not the cutoff.** The standing theory was "level 5's whole annulus
is rejected against an 84 m cutoff". `rejCutoff` is 0.01-0.87% of the total,
occurs at **level 0 only**, and only in the first ~3 windows of the fill. Every
coarse ring rejects on cutoff exactly zero times. The 84 m the aggregate line
printed was `WidestAdmissionCutoffM()` — the *widest* ring's cutoff — while the
ring actually capping was a fine one. The per-ring `cut=` field says so
directly: `R2[cut=318]`, `R3[cut=925]`, `R4[cut=1606]`, and `cut=-1` (DBL_MAX,
not capping) everywhere else.

**(b) It is the per-call budget, gate (a).** `VoxelWorldSubsystem.cpp:14673`:

```cpp
if (!bNearestAdmit && !bOverlayAware && Cap > 0 && AdmissionsThisLevel >= Cap / 4)
```

A pure counter test, independent of distance, applied in **row-major scan
order**. Once a level has admitted `EffectivePendingJobCap/4` chunks, every
remaining cell in that level's annulus is still fully enumerated — footprint
Z-range memo consulted, Z loop entered, `ChunkRecords.Find` and
`ParkedGeometry.Find` per Z cell — and then thrown away. The sweep stops
admitting; it does not stop enumerating. That is the 50:1.

**(c) The fine-tile gate rejects nothing.** `rejFine = 0` in every window of
every leg. The constraint the whole T4-2 design is built around — residency
cannot be queried off the game thread — is real and must be respected, but on
these legs it is not what is throwing candidates away. `rejNearest = 0` too:
`-VoxelNearestAdmit` was off in all three legs, so gate (a) rather than the
sorted commit tail did the rejecting.

An early `break` out of the sweep is **not** the fix. Row-major order means the
first `Cap/4` cells are the top-left corner of the scan box, not the nearest
chunks; breaking early would admit the wrong ones. That is exactly why
`-VoxelNearestAdmit` exists (collect all, sort, commit the budget). Fixing the
waste requires the enumeration to be ordered by distance *before* the budget
bites — which is what the GPU can do for free and the game thread cannot.

---

## 2. What `-VoxelGpuResidency=2` does today

Mode 2 is wired and it works. The eviction half is a complete success and the
admission half is a complete failure, and the same log line says both.

From `Saved/t42-live.log`, flight windows (the tail windows are a parked,
converged world and must not be read — that misreading is recorded in
`VoxelResidencyGpu.h`):

```
live: cons=248 sup=55 empty=4 noDelta=98 | ad: prop=2393682 adOK=65453 adopt=0
      res=0 stale=44855 rejFine=0 rejBud=2283374 rejCut=0 | ev: prop=0 q=0
      VETO=0 stale=0 resid=0 | ... | ms ev=0.02 ad=636.87
```

* **The exit walk is gone.** `ms ev=0.02` per 5 s window, replacing an
  O(tracked-records) walk that was ~30% of a 246-266 ms recompute (~74 ms). The
  design works.
* **The entry walk was RELOCATED, not removed.** `ms ad=636.87` per window —
  **2.4x worse than the entire CPU recompute it replaces.** The GPU proposes
  2,393,682 admits per window; the game thread runs one
  `AdmitCandidateEvaluate` on each; 2,283,374 of them (95.4%) die on the same
  gate (a) that was rejecting them before. The 50:1 moved from the CPU sweep
  into the readback.

This is precisely the failure `VoxelResidencyScan.usf`'s own header names —
Aokana's "decouple, don't relocate" — arriving through the one door the design
left open: **nothing bounded the size of the delta.** `EntryScanCS` appends
every untracked, in-cutoff cell in the annulus, capped only by `kAdmitCap`
(65,536). At the measured 248 deltas/window that is ~9,652 proposals per delta,
and it grows as Span² as the rings widen.

Also established, and worth recording because it corrects the brief:

* **The loose-global trap has never actually bitten in a committed state.**
  `tools/lint-residency-globals.py` (new) was run against every commit that has
  ever touched the `.usf` — `2fbfbde` and `78ecd3f` — and both PASS. The only
  kernel that reads `MaxRingLevel` is `EntryScanCS`, and it always declared it;
  the four `MaxRingLevel` entries added to the other structs are harmless
  no-ops. The one genuine instance of the shape was `ScanLevelMask`: declared in
  the `.usf`, read by no kernel, one edit away from being the real trap. It is
  now deleted (the level gate is a CPU-side dispatch skip, never a shader test).
* **Four GPU counters have been written on the hot path and read by nothing**
  since the first commit: `candVisited`, `zVisited`, `cutoffRej`,
  `exitVisited`. Four `InterlockedAdd`s per candidate feeding an instrument that
  did not exist. They are now on the log line.
* **Two mode-2 lanes fall back to the FULL CPU walks and reported nothing.**
  Anchor-underground (`VoxelWorldSubsystem.cpp:15417`, `!bAnchorUnderground`)
  and mirror-resync (`!bResidResyncedThisCall`) both skip the block that builds
  `LiveOutcome`, so those calls printed an all-zero live line — the same shape
  as a dead GPU path. Both are now counted as `ug=` and `resync=`, and both
  join the log's quiet-window test so an underground flight can no longer print
  no line at all.

---

## 3. What moved to the GPU: `-VoxelResidencyAdmitBudget=N`

**Default 0 = OFF, and off is byte-identical**: no histogram pass, no resolve
pass, `EmitPhase` is 1, and `EntryScanCS` compiles and runs exactly as before.

The CPU's budget is a *count*, so make the GPU emit the nearest that many. Per
scanned level, when the switch is on:

1. **`EntryScanCS`, `EmitPhase=0`** — identical enumeration, appends nothing,
   `InterlockedAdd`s each surviving candidate into a per-level 1024-bin
   histogram over **squared** sort-key distance. Squared keeps the bins
   equal-*area*, so a uniform annulus fills them evenly.
2. **`BudgetResolveCS`** (new kernel, one thread per level) — prefix-sums that
   level's bins, finds the first bin whose running total exceeds the level's
   budget, and publishes that bin's **upper** edge as `BudgetThresholdSq[L]`.
3. **`EntryScanCS`, `EmitPhase=1`** — enumerates again and appends only
   `SortKeySq < BudgetThresholdSq[L]`, counting the rest as `budRej`.

Two enumeration passes instead of one. The GPU enumeration is 128×128 threads
per level and is not the cost; the readback and the game-thread adjudication
are.

**Every degenerate case resolves to +infinity (emit everything), never to
zero.** The threshold buffer is cleared to `0x7F7FFFFF` before the resolve, a
budget the level never exceeds yields +inf, and a budget only exceeded in the
last (catch-all) bin yields +inf. Overshoot costs a few CPU rejects; undershoot
starves admission and stalls streaming, so the safe direction is deliberately
chosen everywhere.

Budgets are **per level**, packed `int4 LevelAdmitBudget[2]`, because gate (a)
is per level: a single number would throttle a wide coarse ring by a fine
ring's allowance. `FVoxelResidencyLevelParams::AdmitBudget` defaults to 0
meaning "use the command-line number", so the switch works with **no subsystem
change at all**; hook A below makes it track the CPU's real cap.

### The per-tick arithmetic

Measured, mode 2 today (`t42-live.log`, peak flight window):

| | value |
|---|---|
| deltas consumed / 5 s window | 248 |
| admit proposals / window | 2,393,682 |
| proposals per delta | ~9,652 |
| CPU bytes parsed per delta | 9,652 × 16 B = **154 KB** |
| CPU bytes parsed per window | **38.3 MB** |
| `AdmitCandidateEvaluate` calls / window | **2,393,682** |
| game thread, admit lane | **636.87 ms / 5 s window** |
| cost per proposal | 636.87 ms / 2,393,682 = **0.266 µs** |
| admissions actually made / window | 65,453 (264 per delta) |

Projected with the budget on, 6 active rings, budget `B` per level. Proposals
per delta become `Σ_L B_L` plus at most one bin's occupancy per level — a
constant, **independent of ring span and of record count**:

| B per level | proposals/delta | bytes/delta | evaluations/window | admit lane (at 0.266 µs) |
|---|---|---|---|---|
| 512 | ~3,080 | 49 KB | 764 k | ~203 ms |
| 256 | ~1,540 | 25 KB | 382 k | ~102 ms |
| 128 | ~770 | 12 KB | 191 k | ~51 ms |
| 64 | ~390 | 6 KB | 97 k | ~26 ms |

The observed admission rate is **264 per delta**, so even `B=64` (384 per
delta) leaves headroom above what the pipeline actually absorbs. Start at 256
and bracket down.

**The property that decides 50k** is the flatness, not the size. Before, a
delta carries `|untracked ∩ annulus|`, which grows as Span². The owner wants
rings out to 8-10 km against today's ~4 km reach (`cut=` peaks at 1,606 m on
R4), which multiplies the outer annulus area several-fold and multiplies `prop`
with it. After, a delta carries `Σ_L B_L` whatever the rings do.

The exit side needs no change: it is already 0.02 ms.

---

## 4. Counters, and the failing reading at each one

The rule this project pays most for: **never gate on a statistic that cannot
come out the other way.** Traffic before timing, every time.

New on the `[gpu-resid]` traffic line:

```
enum: candV= zV= cutRej= budRej= histTot= exitV=
```

| reading | means |
|---|---|
| `candV`/`zV` = 0 with `disp` > 0 | the entry kernel is not running at all. No timing on that leg means anything. |
| `zV` enormous against `admit` | the annulus is producing candidates the CPU cannot use — the GPU-side form of `candidatesRejected`. |
| `exitV` = 0 while `records~` > 0 | the mirror is empty; evict proposals cannot appear. |
| `budRej` = 0, `histTot` = 0 | **the switch is OFF or never latched.** |
| `budRej` = 0, `histTot` > 0 | **the switch is on and the threshold never bound** — every level fits inside its budget, or the resolve fell into its last-bin catch-all. "Fired but did nothing", and it is a *different* fault from the line above. The two are only distinguishable because `histTot` is printed next to `budRej`. |
| `admit + budRej != histTot` | the histogram phase and the emit phase enumerated different sets. An **exact identity across two passes**, not a statistic — it can fail in both directions. |
| `budRej` climbing WITH `adOK` holding | **the win.** |
| `budRej` climbing WITH `adOK` falling | **over-throttled.** The budget is below what the level absorbs and streaming will starve. This is the reading that makes the switch refutable instead of "the numbers got smaller". |

New on the `live:` line: `ug=` and `resync=`. Either climbing means the
recompute took a lane that runs the full CPU walks and never built a live
outcome, so every other number on that line is 0 for that call. Before these
existed, an underground flight and a dead GPU path printed the same thing.

### The mutation test — required before believing any of it

A verify that reads 0.00% different is not evidence unless it has also been
seen non-zero when mutated. Two mutations, opposite directions, same binary:

* `-VoxelResidencyAdmitBudget=1` → `prop` must collapse to a handful per scan
  and `budRej` must approach `histTot`. Streaming should visibly starve. If
  `prop` does not move, **the feature never fired** and no other leg counts.
* `-VoxelResidencyAdmitBudget=100000000` → `budRej` must be 0 and
  `admit == histTot`, and the leg must match a `=0` control. If it does not,
  the emit phase is dropping candidates the histogram phase counted.

Only after both have been seen is a `=256` leg's timing worth reading.

### Checks that run without the build lane

Both are new, both run in seconds, neither needs a build or an editor — which
is the point, because this work happens in a lane that has neither.

```
python tools/lint-residency-globals.py            # PASS (all 6 kernels)
pwsh tools/voxel-check-residency-shader.ps1       # all 6 kernels compile
```

Both were confirmed to **fail** when mutated: deleting `HistBins` from
`EntryScanCS`'s `FParameters` gives `FAIL EntryScanCS reads but does NOT
declare: HistBins` and exit 1; dropping a semicolon in `BudgetResolveCS` fails
all six DXC compiles and exits 1.

---

## 5. Hooks into `VoxelWorldSubsystem.cpp`

**This file was not edited.** These are exact, and each is optional — the
switch is fully functional without any of them.

### Hook A — make the GPU budget track the CPU's real gate (a). 1 line.

`RecomputeDesiredSet`, the residency params fill loop. Immediately **after**
line 15373:

```cpp
			LP.CutoffSortKeySq = LevelAdmissionCutoffDistSq[ResidLevel];
```

insert:

```cpp
			// T4-2 admission budget: this ring's own gate-(a) allowance, so the
			// GPU stops proposing exactly what the CPU is about to reject. The
			// SAME expression as gate (a) (:14673) and the nearest-admit commit
			// budget (:14767) -- if that expression ever changes, change it
			// here in the same commit or the GPU will budget to a stale rule.
			LP.AdmitBudget = (EffectivePendingJobCap > 0) ? EffectivePendingJobCap / 4 : 0;
```

Before landing it, confirm `UpdateEffectivePendingJobCap()` has already run for
this tick at line 15340 — the whole point of `EffectivePendingJobCap` is that
every admission site sees the same cap within one tick, and reading it before
it is refreshed would budget to the previous tick's number.

With hook A the command-line value becomes a fallback for rings the subsystem
leaves at 0, and `-VoxelResidencyAdmitBudget=1` still works as the "did it
fire" mutation because the shader gates on the scalar being non-zero.

### Hook B — none needed for the admit lane

Once the delta is budget-bounded, `LiveOutcome.AdmitRejBudget` (`rejBud=` on
the live line) should fall to near zero on its own. If it does not, the GPU
budget is above the CPU's and hook A has not landed or is reading a stale cap.
No code change; it is a cross-check that already prints.

### Hook C — not mine, but it bounds mode 2 and nobody owns it

The edited-footprint lane, `VoxelWorldSubsystem.cpp:16695-16730`, is
**O(edit-map size), unbudgeted, and runs on every live call** — including
no-delta calls, since it sits outside any delta test — and re-allocates a
per-level `TSet<FIntPoint>` each level. Every other live lane is O(delta) or
explicitly capped (cold is capped at `kLiveColdFootprintBudget = 2048` per
call). In a heavily edited world this lane, not the proposals, is mode 2's
bound. It needs the same treatment the cold lane already has: a per-call
budget with a deferral counter.

---

## 6. The leg that measures it

`t42-live.log` is a **flight** leg, so it must be launched directly and left to
`UVoxelPerfRunSubsystem`'s own clock — `tools/voxel-run-leg.ps1` is a cold-fill
driver and would truncate it (it refuses the combination now). Same flags as
`t42-live` plus the switch:

```powershell
$P = 'D:\voxelsim\ue-project\VoxelEarth.uproject'
$E = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach ($b in 0, 1, 256, 100000000) {
  & $E "$P" -game -nosplash -unattended -sm6 -dx12 `
     -abslog="D:\voxelsim\Saved\budget-$b.log" `
     -ResX=1600 -ResY=900 -WinX=0 -WinY=0 -VoxelSpawnAt=-61440,-61440 `
     -VoxelTimeOfDay=12:00 -VoxelDate=03-20 -VoxelTimeScale=0 `
     -VoxelPerfRun=120 -VoxelPerfFlight=line -VoxelPerfPreflightSec=90 `
     -VoxelPerfLingerSec=60 -VoxelPerfLogInterval=2 -ExecCmds="voxel.Debug 0" `
     -VoxelGpuResidency=2 -VoxelResidencyAdmitBudget=$b `
     -ini:EditorPerProjectUserSettings:[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]:bAutoStartServer=False
  Wait-Process -Name UnrealEditor-Cmd -Timeout 400 -ErrorAction SilentlyContinue
}
```

`b=0` is the control and must be **byte-identical** to a plain
`-VoxelGpuResidency=2` leg. `b=1` and `b=100000000` are the two mutations from
§4. `b=256` is the measurement.

Read the flight windows, never the tail — `move=` on the live line says which
is which. Read with:

```
grep "gpu-resid" Saved/budget-256.log | grep -v "move=0uu"
grep "Voxel admission detail" Saved/budget-256.log
tools/leg-summary.sh budget-0 budget-256
```

Three numbers decide it, and all three must be read together:

1. `ms ad=` on the live line — the thing being fixed (636.87 today).
2. `adOK=` — must **hold**. Falling with `budRej` climbing is over-throttling,
   not a win.
3. `dispatched=`/`drained=` on the job-flow line — the pipeline must stay
   balanced. A smaller delta that starves the queue is a regression wearing a
   better number, and this is the counter that says so.

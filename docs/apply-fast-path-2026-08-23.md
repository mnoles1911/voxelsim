# Apply's 54.5 us/chunk: what it is, and the hooks that remove it

2026-08-23. Companion to `docs/handoff-50k-2026-08-23-2200.md` ("SECOND blocker,
same arithmetic, nobody has looked at it").

**This document exists because `VoxelWorldSubsystem.cpp` is held by another
agent and must not be edited by two people at once.** Everything the change
needs lives in new files (`VoxelApplyBatch.{h,cpp}`); the four lines that have
to go *into* that file are written out below, exactly, for the lane holder to
paste.

---

## 1. What apply actually spends the time on

### The arithmetic, restated

Control arm (`gp-ctl`), matched cold-start leg:

    apply = 692 ms per 5 s window over ~12,700 drains
          = 54.5 us of GAME THREAD per chunk
          = 2.7 seconds of game thread per second of gameplay at 50,000/s

`apply=` on the `Voxel tick budget` line brackets **`DrainResults` exactly**
(`VoxelWorldSubsystem.cpp:9364`, `ThisFrameApplyMs = float((T2 - T1) * 1000.0)`
with `DrainResults` between T1 and T2). So the question is what one iteration of
that loop costs, not what `ApplyMeshResult` costs.

### It is NOT ApplyMeshResult

On the marcher path every drained chunk is `zeroQuad` (confirmed on both arms:
`zeroQuad == drained` in every window), and `voxel.Terrain.RetireQuads` defaults
to **1**, so this is the only path. `ApplyMeshResult`'s `NumQuads == 0` branch
(`:21545`-`:21590`) is:

| statement | cost on a marcher chunk |
| --- | --- |
| `NotifyFluidTerrainDirtyChunk` | level 0 only, and only with a fluid listener registered |
| `ResidentQuads -= Rec.LastQuadCount` | an integer subtract; `LastQuadCount` is already 0 |
| `GetOrCreateGpuPool(...)` | early-returns on `GpuPool.Get()` — one weak-pointer serial check |
| `ReleaseChunkGeometry(Rec)` | two weak-pointer gets over a record holding neither a component nor a pool slot |
| `Rec.bMeshSettled = true` | a bool |

Nothing there is 54 microseconds. **The comment at `:7673` calling a zero-quad
apply "buried: real work, no component" is describing the *worker's* work, not
the game thread's.** No UObject is touched, nothing is allocated, no render
state is marked dirty. The shape the brief expected to find is *already* the
shape of the code.

### It IS the line after the apply call

`DrainResults`, the tail of the loop body, `VoxelWorldSubsystem.cpp:22406`:

```cpp
VoxelBrickCpuArm::Publish(
    Result.Key, Result.BrickPack,
    ShadingFromChunkParams(SampleChunkParamsForPool(
        Root, VoxelCoords::ChunkOriginWorldForLevel(Result.Key.Key, Result.Key.Level),
        Result.Key.Level)));
```

`SampleChunkParamsForPool` (`:20688`) is **four calls to
`UVoxelWorldSubsystem::GetSurfaceHeightUU` plus one climate sample**, per
drained result. Each `GetSurfaceHeightUU` (`:26123`) is:

1. `FVoxelFineTileStreamer::RequestFootprint` — takes the sampler's lock
   exclusively and may do disk I/O; and
2. `Impl->Voxels.amplifier().column(Vx, Vy)` — a **full worldgen column**, cave
   lattice and cavern passes included,

on the **game thread**. The function's own comment prices the single-sample
version at 0.002-0.004 ms/apply; task #44 took it to four corners, so
0.008-0.016 ms/drain — and that measurement predates both the fine tier and the
30 m → 10 cm amplification redesign.

Two independent defects sit on top of that:

**(a) It is computed whether or not anything consumes it.** C++ evaluates
arguments before the call, and `VoxelBrickCpuArm::Publish` (`:2301`) opens with

```cpp
if (!Pack.IsValid() || !VoxelGpuBrickPackResidentEnabled()) { return; }
```

`Result.BrickPack` is null on **every GPU-fork result** — that arm publishes its
bricks at completion, and the call site's own comment says so ("Null on every
GPU-fork result … and whenever the CPU arm is gated off"). On the GPU-primary
arm the four amplifier columns are therefore computed and discarded ~12,700
times per 5 s window.

**(b) It is recomputed per chunk for a quantity that is per column.** The four
corner heights, the climate bytes and the fitted gradients are functions of
`(X, Y, Level)` only. The chunk's Z enters exactly once, at the end, as
`SurfaceZRelUU = BaseZUU - ChunkWorldOrigin.Z`. A level-0 band is 20-40 chunks
deep, so the same four columns are computed 20-40 times for one footprint.

### What the counters already in the tree say — and why they could not have found this

Two instruments already exist near this cost. Neither can see it, and one of
them reads as an all-clear.

**`LevelZeroQuadMs` / `LevelQuadMs` (`:7802`) are worker-side, not apply-side.**
They sum `Result.JobMs`, CPU results only, in the census block *above* the
stale-result discard. Their own comment says what they are for: the honest
ceiling on a pre-dispatch buried-chunk skip, `zeroQuadMs / (zeroQuadMs +
quadMs)`. They price what the *worker* spent producing a zero-quad chunk. They
say nothing whatever about the game thread, and `LevelZeroQuadTotal` is a count.
They are not evidence about apply in either direction.

**`Voxel apply stages` (`:11193`) is named for exactly this cost and is
structurally blind to it.** `ApplyStageParamsMs`'s declaration
(`:7716`) reads "SampleChunkParamsForPool: a full Amplifier::column on the game
thread", and its comment at `:7711` says that if this bucket is large, T1-3's
column cache moves up the queue. But `ApplyStageParamsMs` and
`AppliesTimedSinceLog` are incremented **only inside `ApplyMeshResult`'s pooled
branch** — and on the marcher path `NumQuads == 0` returns *before* that branch,
for every chunk. So on any leg with `voxel.Terrain.RetireQuads` on (the default):

    Voxel apply stages (5s window): ... timedApplies=0 pack=0.00ms params=0.00ms
                                        poolAdd=0.00ms | per-apply params=0.000

**`params=0.00ms` is not "the sampler is free". It is "the branch this
instrument watches was never taken."** The sampler that *does* run is the one in
`DrainResults`, which this instrument does not wrap. That is the whole reason
0.054 ms/chunk had no explanation: the counter that would have named it was
measuring a different call site, and it was reading a clean zero.

This is also why the first leg is `-VoxelApplyFast=4`. `sampleUs/call` from the
new line is the first number in this project that prices the sampler at the call
site that actually runs it.

### The same sampler runs on the DISPATCH path too — read this, dispatch owner

`SubmitGpuMeshJob`, `VoxelWorldSubsystem.cpp:17841`:

```cpp
if (const USceneComponent* PoolRootForShading = GpuPoolRoot.Get())
{
    Req.BrickShading = ShadingFromChunkParams(SampleChunkParamsForPool(
        *PoolRootForShading,
        VoxelCoords::ChunkOriginWorldForLevel(LevelKey.Key, LevelKey.Level),
        LevelKey.Level));
}
```

Four more amplifier columns, on the game thread, **per GPU submission** — and it
lands inside the `reqHdr` bucket of the six-bucket breakdown that shipped as
`fd77973` and has not yet been read (`const double SubT1 = …; // reqHdr:
footprint/seed/shading/skirt`, `:17849`). That is the open 1,547 ms/window
dispatch blocker, and this is a named, priced candidate for part of it.

**`reqHdr` is the first number to read off the next `-VoxelGpuPrimary` leg.**
The same column cache serves this site unchanged — see optional hook 4.

---

## 2. What changed (new files only)

`ue-project/Source/VoxelEarth/VoxelApplyBatch.{h,cpp}` — namespace
`VoxelApplyFast`. Nothing else in the tree is modified except
`tools/frontend-switch-classification.txt` (three switch registrations).

### The switch

`-VoxelApplyFast=N`, latched from the command line (not a cvar: `-ExecCmds`
lands after streaming has begun, so a cvar would flip apply half way through a
cold fill and make the leg a blend of two behaviours). **Default 0 = off.**

| bit | name | effect |
| --- | --- | --- |
| 0 | — | **OFF.** `ShadingForPublish` is `FORCEINLINE` and folds to exactly the expression it replaces. No branch survives, no counter moves, no clock is read. |
| 1 | guard | Skip the sample entirely when the pack will not be published (defect **a**). |
| 2 | cache | Memoise the per-`(X,Y,Level)` part of the sample (defect **b**). |
| 4 | measure | **Time the sampler and change nothing else.** Implied by 1 and 2. |

`3` = the shipping candidate. `4` = the non-invasive measurement leg, and it is
the **first** leg to run.

Sizing: `-VoxelApplyColumnCache=N` (default 8192 slots, direct-mapped, rounded
up to a power of two, 40 B/slot). Cross-check: `-VoxelApplyColumnCacheAudit=N`
recomputes and compares 1 hit in N, logs an Error on any mismatch, and
**returns the fresh value**, so an audited leg can never publish worse than
control.

### Counters and their failing readings

Printed on its own 5 s clock as `Voxel apply fast (5s window): …`. The full list
lives in the header; the readings that condemn the change:

| reading | verdict |
| --- | --- |
| no `Voxel apply fast` line at all, with `-VoxelApplyFast=3` on the command line | the hook was never applied, or the module is not linked. **Nothing in this change ran.** |
| line present, `calls=0` | linked and latched, but nothing calls it — the hook is in a branch the marcher path never reaches |
| `calls>0`, `avoided=0` | both arms inert. **FAIL, not a null result**, whatever `apply=` says |
| `cacheHit=0` with `cacheMiss ≈ calls`, `cacheEvict ≈ cacheMiss` | table thrashing — raise `-VoxelApplyColumnCache` |
| `cacheHit=0` with `cacheMiss ≈ calls`, `cacheEvict ≈ 0` | the **key** is wrong (one entry per chunk, not per column). Pure loss |
| `mismatch>0` under `-VoxelApplyColumnCacheAudit=1` | taken but **wrong**. `maxMismatchUU` separates the float32 reconstruction tail (< 0.01 UU) from a real wrong-ground event (metres) |
| `sentinel>0` | hard zero. The sampler found no world/subsystem: every brick that window has its surface gate disabled |
| `offThread>0` | hard zero. Data race; symptom is wrong terrain, not a crash |
| `rootFlush` climbing every window | the terrain root is moving; the cache arm of the leg is void (not wrong — flushing is what keeps it correct) |
| **`avoided ≈ calls` but `apply=` UNCHANGED** | **the diagnosis in §1 is wrong. Revert; do not tune.** |

If the world looks wrong, bisect with the **mode**, not a counter: mode=2
(cache only, guard off) publishes exactly what control publishes, so
"mode=2 right, mode=3 wrong" isolates the guard and the reverse isolates the
cache.

---

## 3. THE HOOKS — for the lane holder of `VoxelWorldSubsystem.cpp`

All line numbers are against `e64e4f2`.

### Hook 1 (required) — the include

Beside the existing block at the top of the file (after line 13,
`#include "VoxelBrickCpuPackFromCore.h"`), add:

```cpp
#include "VoxelApplyBatch.h" // apply's per-chunk worldgen sampling, behind -VoxelApplyFast=
```

### Hook 2 (required) — `DrainResults`, lines 22414-22416

Replace these three lines:

```cpp
			ShadingFromChunkParams(SampleChunkParamsForPool(
				Root, VoxelCoords::ChunkOriginWorldForLevel(Result.Key.Key, Result.Key.Level),
				Result.Key.Level)));
```

with these three:

```cpp
			VoxelApplyFast::ShadingForPublish(Result.Key, Result.BrickPack, Root,
			                                  &SampleChunkParamsForPool, &ShadingFromChunkParams));
```

(The two file-statics are **passed by address, not re-implemented.**
`ShadingFromChunkParams` is a byte-exact round trip of a wire format shared with
`VoxelQuadVertexFactory.ush`, and a second transcription of a byte format is the
defect shape this project has paid for most often. One definition, two callers.
Both are already forward-declared at `:17792`/`:17795`, so their addresses are
available at this site.)

The surrounding comment at `:22408-22413` ("ChunkOriginWorldForLevel WITH NO
REBASE …") still applies and should stay — `ShadingForPublish` composes the
origin the same way, from the same key, in the same frame.

### Hook 3 (recommended) — `DrainGameThreadMesh`, lines 22601-22603

The edit re-mesh path has the identical expression. It is far lower traffic
(≤ `voxel.Stream.MaxRemeshesPerFrame` = 4/frame vs up to 64 applies), so this is
about keeping one behaviour rather than about the 50k number:

```cpp
		VoxelBrickCpuArm::Publish(
			LevelKey, BrickPack,
			VoxelApplyFast::ShadingForPublish(LevelKey, BrickPack, Root,
			                                  &SampleChunkParamsForPool, &ShadingFromChunkParams));
```

### Hook 4 (optional, and it belongs to the DISPATCH owner) — `SubmitGpuMeshJob`, lines 17841-17844

Same sampler, per GPU submission, inside the `reqHdr` bucket of the open
1,547 ms/window dispatch blocker. The guard arm does not apply here (the
shading *is* consumed), so pass an always-valid pack — or, more honestly, call
the cache directly. **Read `reqHdr` off a leg first.** Do not apply this hook
on the strength of this document.

### Hook 5 (optional) — one predicate instead of two

`VoxelApplyFast::WillPublish` is character-for-character the test
`VoxelBrickCpuArm::Publish` opens with, and nothing in the new module can
observe the two drifting apart. The one-line fix is to make Publish's early-out
(`:2305`) read:

```cpp
		if (!VoxelApplyFast::WillPublish(Pack))
		{
			return;
		}
```

leaving one definition and two callers.

### Hook 6 (optional) — exact window alignment

`ShadingForPublishSlow` flushes its own 5 s window every 256 calls, so it
reports with no second hook. If you want its window edges to line up exactly
with `Voxel tick budget` and `Voxel job flow` — worth doing before publishing a
per-chunk number, since `apply-us/chunk` is `apply=` from one line divided by
`drained=` from another — add to `MaybeLogCounters`:

```cpp
	VoxelApplyFast::FlushStats(/*bForce*/ true);
```

---

## 4. Gates — stated as numbers, before the legs

Baseline: **54.5 us/chunk** of game thread in apply (692 ms / ~12,700 drains,
`gp-ctl`, 2026-08-23).

**G0 — is the diagnosis right? (`-VoxelApplyFast=4`, behaviour unchanged.)**
Read `sampleUs/call`.

- ≥ 20 us/call → the sampler is ≥ 37% of apply. Ship modes 1-3.
- 5-20 us/call → real but partial. Ship, and re-bracket the remainder before
  claiming apply is solved.
- < 5 us/call → **§1 is wrong.** Do not ship modes 1-3; find the real term.

**G1 — traffic (`-VoxelApplyFast=3`).** `avoided / calls ≥ 0.90` in every
settled window. `avoided == 0` is a **FAIL**, not a null result, and a flat
total with the fast path never firing is a FAIL regardless of what `apply=`
does.

**G2 — headline.** `apply= × 1000 / drained` ≤ **30 us/chunk** (a ≥ 45% cut
from 54.5). Stretch: ≤ 15 us/chunk. Both numbers must come from the **same 5 s
window** — see hook 6.

**G3 — correctness.** Over a full leg with
`-VoxelApplyFast=3 -VoxelApplyColumnCacheAudit=1`:
`mismatch=0`, `sentinel=0`, `offThread=0`, `holes=0`, and the brick pool's
resident chunk count within 0.1% of the matched control leg.

**G4 — control identity.** With the switch absent: **no `Voxel apply fast` line
is printed at all**, and `apply=` reproduces the control within 10%.

---

## 5. The legs

All through the sanctioned driver (`tools/voxel-run-leg.ps1`, a cold-fill
driver — do not pass `-VoxelPerfFlight` to it), read with
`tools/leg-summary.sh`, never `grep | tail -1`.

```powershell
# G0 -- measurement only, behaviour unchanged. RUN THIS FIRST.
pwsh -File tools/voxel-run-leg.ps1 `
     -LogPath D:\voxelsim\Saved\apply-m4.log -ClearEditLog -BudgetSec 300 `
     -ExtraArgs @('-VoxelApplyFast=4')

# G1/G2 -- the shipping candidate.
pwsh -File tools/voxel-run-leg.ps1 `
     -LogPath D:\voxelsim\Saved\apply-m3.log -ClearEditLog -BudgetSec 300 `
     -ExtraArgs @('-VoxelApplyFast=3')

# G3 -- correctness, audit every hit. Slower by construction; judge only
# mismatch/sentinel/offThread/holes off this leg, never a timing.
pwsh -File tools/voxel-run-leg.ps1 `
     -LogPath D:\voxelsim\Saved\apply-m3-audit.log -ClearEditLog -BudgetSec 420 `
     -ExtraArgs @('-VoxelApplyFast=3','-VoxelApplyColumnCacheAudit=1')

# G4 -- control. Same binary, no switch.
pwsh -File tools/voxel-run-leg.ps1 `
     -LogPath D:\voxelsim\Saved\apply-ctl.log -ClearEditLog -BudgetSec 300
```

`voxel.Terrain.RetireQuads` defaults to 1, so a bare leg is already the marcher
path and `zeroQuad == drained` — no extra flag is needed to reach the population
this change is about.

Read together, never alone (the handoff's rule, burned twice):

```
Voxel tick budget  -> apply=            game-thread ms this window
Voxel job flow     -> drained= zeroQuad= population for the divide
Voxel apply fast   -> calls= avoided=   proof the code ran at all
```

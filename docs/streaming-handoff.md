# Handoff: G4 parity closed and measured; G5 is the flip (2026-07-25, evening)

Supersedes the earlier 2026-07-25 handoff, which described G3 as "wired and
rendering, not yet correct". That is resolved. This document is the current
state.

## State

| Item | Status |
|---|---|
| **G1** — GPU greedy mesher | ✅ Gate green, bit-exact with the CPU mesher |
| **G2a** — kernels in-engine via RDG | ✅ Cross-toolchain digest `f3c48a4df3e20e9a` |
| **G2** — GPU geometry pool + custom draw | ✅ ONE primitive, ONE draw |
| **G2** incremental upload | ✅ Partial writes match a full rebuild exactly |
| **G3** — drive the pool from the cascade | ✅ 9,822 chunks / 8.8M quads, ONE draw, matches the CPU path ring for ring |
| **G4** — parity | ✅ **Closed and measured.** See below |
| **G5** — flip the default | 🟨 Justified by measurement; the flip itself is the remaining step |

## What G4 actually turned out to be

The G4 checklist and the first version of `gpu-g4-parity-plan.md` were wrong
about which items mattered. The corrected picture is in that document; the short
version:

- **The "material gate" was mostly not real.** The pooled vertex factory owns
  both ends of the vertex-colour pipe, so anything expressible as a vertex colour
  channel is already an interpolant the material graph reads. That covers the
  biome tint and — per `gpu-gi-volume-design.md` — the GI term too.
  `M_VoxelTerrain.uasset` has not been touched and does not need to be for
  anything on the critical path.
- **Ring cross-fade was listed first and matters least.** It is off by default
  (`-VoxelRingCrossFade`) and was disabled because it was the dominant cause of
  the see-through-ring bug. The pooled path already matches it by construction:
  the C++ inert sentinels are the same values as the material's own parameter
  defaults, so a pooled chunk with no material instance behaves identically to a
  component chunk with an inert one.
- **The item that did affect normal play was on no list** — vertex colour R, the
  biome tint. The pooled factory had neither the side-face value (140/255, not 0)
  nor the surface-proximity gate. Every vertical riser in the world rendered
  pink-tan against the CPU path's blended turf. Fixed and measured: pixels
  differing by more than 8/255 fell from **17.4% to 4.3%**, against a same-path
  repeat-run noise floor of 1.1%. The residual is the documented per-chunk
  (rather than per-quad) climate sampling.
- Also fixed: `bVelocityRelevance` (pooled terrain contributed nothing to the
  velocity pass, so TSR reprojected it from depth alone), `bUsesLightingChannels`,
  `bRenderCustomDepth`, and editor Wireframe view mode.

## What the flip buys — measured

60 s scripted surface flight at 20 m/s, same spawn, post-warmup, one cvar
differing. `-VoxelPerfRun=60 -VoxelPerfFlight=surface`, JSON summary in
`Saved/PerfRuns/`.

Two pairs, the second run in the reverse order (pool first) so that any drift in
background load over the session works *against* the result rather than for it:

| | cpu #1 | cpu #2 | pool #1 | pool #2 | mean delta |
|---|---|---|---|---|---|
| p50 frame ms | 17.78 | 17.21 | 17.57 | 16.75 | −1.9% |
| **p95 frame ms** | 29.17 | 51.83 | 24.34 | 21.77 | **−43.1%** |
| **worst frame ms** | 333.1 | 109.7 | 66.5 | 86.7 | **−65.4%** |
| **hitches (>33.3 ms)** | 35 | 373 | 11 | 6 | **−95.8%** |
| chunks/s | 678 | 537 | 839 | 797 | **+34.6%** |

**The median frame is not the point and never was.** ADR-0006 removes an
`FScene::AddPrimitive` per chunk from the streaming path; that cost lands in the
tail, and the tail is what moved.

Read these as *direction and rough magnitude*, not as gate numbers. All four
legs ran while several other headless instances were active on the same machine,
and the spread between `cpu #1` and `cpu #2` (35 hitches versus 373) is mostly
that contention — which is itself informative, since the component path degrades
far worse under load than the pool does. What survives the noise is that **the
pool wins on p95, worst frame, hitch count and throughput in every one of the
four runs**, which is not something contention produces by accident.

Re-run both legs on a quiet machine before quoting anything here as an official
gate row.

## What is still component-path-only

Flipping `voxel.Stream.GPU` on by default does not require any of these. Deleting
the component path does, and each would become a silent no-op:

- Voxel GI (`voxel.GI.*`). Worse than "not implemented": under
  `voxel.Stream.GPU` the light field is never even populated, because
  `NotifyChunkMeshUpdated` is called only from
  `UVoxelChunkComponent::SetChunkQuads`. See `gpu-gi-volume-design.md` step 0.
- Chunk-state and ring debug tints (the tint loops skip records with no
  `Component`).
- Ring cross-fade (`-VoxelRingCrossFade`).
- `voxel.Render.CastShadow` A/B — the pool hardcodes `true`.
- Static-relevance A/B (`-VoxelStaticRelevance`).
- Mesh-time diagnostics: `-VoxelMatHistogram`, `-VoxelGIColorCheck`,
  `-VoxelWindingCheck`, `voxel.GI.DebugVis`.
- Per-quad (rather than per-chunk) climate sampling.
- **`voxel.Stream.GPUMaxLevel` and `GPUMaxChunks` presuppose the component path
  as the fallback**, and they are the two sharpest debugging tools this renderer
  has.

**Recommendation: flip the default, keep the path.** The cvar is the revert, and
the bisection tools are worth more than the code deleted by retiring one
renderer.

## Verification recipes

Build (close the editor first — it locks the DLL, and a failed link DELETES it):

```
"D:/UE_5.8/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development \
  -Project="D:/voxelsim/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE
```
~60 s incremental.

**Pass the project path as an ABSOLUTE path when launching headless.** A relative
`VoxelEarth.uproject` fails with exit code 1, no log, and no crash report, which
reads exactly like a broken build and is not one. Launching also needs the
sandbox disabled.

Cross-toolchain gate, ~30 s — both legs must print `f3c48a4df3e20e9a`
(bench: `build/voxel-core-msvc/bench/vxc_gpu.exe --radius 64`):

```
UnrealEditor-Cmd.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -nosplash \
  -unattended -sm6 -ExecCmds="voxel.GPU.VerifyRegion, quit"
```

Visual A/B, one cvar differing. Nothing exits on its own — launch it, wait, kill
it:

```
UnrealEditor.exe "D:/voxelsim/ue-project/VoxelEarth.uproject" -game -windowed \
  -resx=1280 -resy=720 -nosplash -unattended -sm6 "-VoxelSpawnAt=-84480,53760" \
  -ExecCmds="voxel.Stream.GPU 1, voxel.Debug.ShotIn 30"
```
Screenshot lands in `Saved/Screenshots/WindowsEditor/`. **Shoot at 25–30 s, never
earlier** — an under-filled cascade reads exactly like a rendering bug.

Diff screenshots numerically rather than by eye. Python + Pillow are present; the
useful statistic is *percentage of pixels differing by more than 8/255*, always
quoted against a same-path repeat-run noise floor, which is 0.02–1.1% here. Two
images that "look the same" routinely differ by 17%.

Perf A/B — the JSON summary is the deliverable:

```
UnrealEditor.exe <uproject> -game -windowed -resx=1280 -resy=720 -nosplash \
  -unattended -sm6 "-VoxelSpawnAt=-84480,53760" \
  -VoxelPerfRun=60 -VoxelPerfFlight=surface -ExecCmds="voxel.Stream.GPU 1"
```
Written to `Saved/PerfRuns/perf_<timestamp>.json`. Read
`postWarmupP95FrameMs`, `postWarmupMaxFrameMs` and `postWarmupHitchCount` —
`p50` will barely move and is the wrong metric for this change.

Pool test harness, three modes — the third is the one that matters:

```
voxel.GPU.SpawnPool 64 shot         — plain pool
voxel.GPU.SpawnPool 64 churn shot   — edits BEFORE the proxy exists. One full
                                      rebuild; does NOT test the incremental path
voxel.GPU.SpawnPool 64 churnlive shot — edits AFTER the proxy is up. This is the
                                      path streaming uses
```
`churnlive` passes when the log shows `Live churn`, the `upload:` line count does
**not** increase after it, and the image matches the `churn` image.

## Traps that cost real time — do not rediscover

1. **These buffers must be `EBufferUsageFlags::Static`, never `Dynamic`.** Only
   the static lock path honours a lock offset. The dynamic path ignores the
   offset and returns the buffer's base address, then renames the whole buffer on
   every lock after the first, leaving everything outside the dirty range
   uninitialised. "We write it every frame, so mark it Dynamic" is the intuitive
   and wrong call. **This does NOT apply to volume textures** —
   `RHIUpdateTexture3D` honours destination offsets; its trap is a 256-byte
   staged row pitch instead (`gpu-gi-volume-design.md` §3).
2. **Screenshot at 25–30 s, not 3 s.** A shot before the cascade fills shows
   coarse LOD slabs that read exactly like corrupted pool geometry. One hour lost
   to a bug that was not there.
3. **`SetRootComponent()` on a fresh `NewObject`ed component installs an identity
   transform**, redefining the actor's location as the world origin — 8,448 km
   away, i.e. invisible. Call `SetWorldLocation()` after `RegisterComponent()`.
4. **The pool must be the ROOT component of its own actor.** As a child of
   `ChunkRoot` the primitive never enters the visible set —
   `GetDynamicMeshElements` is never called, with a live proxy and valid bounds.
5. **Bounds do not reach the renderer by themselves.** `UpdateBounds()` updates
   the component; the scene keeps the bounds the proxy got on frame 1. Push them
   with `MarkRenderTransformDirty()`, never `MarkRenderStateDirty()` (which
   rebuilds every buffer and defeats the incremental path).
6. **The chunk table is float32 and this world is 84 km wide.** At ~8.4M UU the
   ULP is 1.0 UU against a 10 UU voxel, so chunk origins are stored relative to a
   rebase origin the component carries in its double-precision transform. The
   same applies to anything else absolute you put in that table — the surface
   height added for the biome gate is stored chunk-relative for exactly this
   reason.
7. **`VoxelEarthShaders` must stay `PostConfigInit`**, and `/VoxelCore` is a
   directory *mapping*, not a copy.
8. **Discard the first run after any build.** Cold PSO state costs ~20%
   throughput and reads exactly like a regression. A shader change also forces a
   recompile on the first run after it.
9. **Never A/B this renderer through scalability cvars.** `r.ShadowQuality 0`
   desyncs the BeginPlay PSO precache and measures precache invalidation instead.
10. **Prefer a control experiment to a bisect.** Four hypotheses died to
    measurement on the R0 freeze alone; each looked airtight on paper.

## Streaming bugs: what was actually wrong

The R0 freeze chain, now closed, is worth keeping because the shape recurs:

```
a result is dropped by DrainResults
  -> FootprintBandCache.Add and FootprintBlindJobInFlight.Remove both skipped
  -> the column is absent from the band cache AND present in the blind-job set
  -> the cold-band throttle defers its level-0 chunks on every dispatch pass
  -> R0's pending queue never reaches empty
  -> the refill trigger, though armed and unconstrained, never fires because it
     is gated on IsEmpty()
  -> R0 never re-enumerates on a stationary anchor
  -> R0 frozen at the 512 records of its single initial scan
```

Two fixes, both landed, and they are independent:

1. **The cause.** `DrainResults` had `ResultsQueue.Dequeue(Result)` in the `while`
   condition with the wall-clock budget check as the first statement of the body,
   so an over-budget frame popped a result and then dropped it on the floor —
   silently, before the drain counter incremented. Testing the budget *before*
   the dequeue fixes it. Note this also stranded the chunk itself
   (`bJobInFlight` pinned, never re-dispatched) on both paths, independently of
   the blind-job mark.
2. **The amplifier.** `IsEmpty()` is the wrong refill predicate regardless — "this
   ring has no *dispatchable* work" is the property wanted, so a bounded number of
   permanently-deferred entries cannot starve a ring. Plus a 5 s age-out on the
   blind-job mark as a backstop.

Fix (1) has NOT been reproduced under test — the 30 s spawn runs show
`markTimeouts=0`, i.e. they never overran the budget long enough. Forcing it
needs a low `voxel.Stream.ApplyBudgetMs`. Worth doing if the symptom recurs.

**Both were pre-existing CPU-path bugs that the pool merely exposed**, by removing
the very cost that was accidentally keeping the queue non-empty. If a "the pool
broke X" symptom appears, suspect a latent race the pool has surfaced before
suspecting the pool.

## Open, not blocking

- **Deep-column waste:** 44.6% of R0 worker time meshes buried chunks emitting
  zero quads. The proposed fix -- move the band skip from dispatch-time to
  admission-time -- was built, measured, and **does not reach it**; see the
  section below. Left default-off with its census in. The downward escape hatch
  it was supposed to need shipped anyway, because the hole it closes turned out
  to be a live bug rather than a prerequisite.
- **R0 target 128 m:** unblocked -- `RingPresets` is now a runtime accessor
  (`GetRingPresets()`), overridable with `-VoxelRingInnerMeters=` /
  `-VoxelRingOuterMeters=`. Moving R0 itself is still an open decision.
- **GI light field as a GPU volume:** the largest remaining GPU piece and the
  only one that is worth more than parity — it decouples lighting from
  re-meshing, which a dig currently forces. Full design in
  `gpu-gi-volume-design.md`; its step 0 is the empty-field bug above.
- **Water surface pool:** a separate, near-identical instance of the terrain
  pool.
- **Per-chunk debug tints:** storage is already solved (`ChunkParams.w` is free),
  but this is the one remaining item that genuinely needs the material graph to
  read an interpolant instead of a named parameter.
- **Min-spec-proxy M1 gate re-run** still owed; `status.md`'s determinism row is
  stale (goldens predate worldgen v6); NVIDIA CI leg for the G1 gate.

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

And the 61 it does remove cost no worker time to begin with: the dispatch-time
skip already refuses to mesh them. Moving the same predicate earlier saves a
record, a queue slot and an admission slot -- not a job. The 44.6% of R0
worker time in the item above is the chunks the band *declines* to skip (`R0
total=3396 zq=1449`, 42.7% zero-quad on this spawn), and evaluating the same
predicate one stage earlier cannot skip more of them.

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

# Marcher plan — addendum, 2026-08-22

This does not replace `docs/marcher-to-primary-plan-2026-08-21.md`. It records
what a day of measurement changed about that plan, and proposes a re-ordering.
Every number here is in
`docs/measurements/armA-drawpath-ceiling-2026-08-19.txt` with its conditions.

---

## 1. The headline changed twice, and the final one is better

**Matched draw-path comparison** — two legs per arm, no fluids in either arm,
`r.ShadowQuality 0` in both, static flight at `-61440,-61440`, 2560×1440:

| post-warmup | p50 | p95 | max |
|---|---|---|---|
| quad path | 19.78 / 19.78 ms | 22.68 / 22.74 ms | 264 / 293 ms |
| **marcher** | **7.27 / 7.25 ms** | **9.60 / 9.23 ms** | 306 / 300 ms |

**p50 2.72×, p95 2.41×** (50.6 → 137.7 fps). Quad p50 repeats to 0.00%.

The 2.43× on record was right by accident: two large confounds nearly
cancelled.

- The **quad arm paid a 15.78 ms shadow pass** the marcher arm did not.
- The **marcher arm paid a ~7.45 ms p50 / ~33.8 ms p95 fluid sim**.

**Condition that must always travel with 2.72×: neither arm casts sun shadows.**
The quad path *with* its shadows is 35.56 ms at the same pose.

**Wave 4.1 is answered.** The plan called the p95 tail "the largest
unattributed frame-time item in the project". It was the water simulation:
marcher p95 goes 43.42 → 9.60 ms when the 40,000-particle spawn is removed.
That spawn was in every prior leg only because the marcher's hookup required it
— the coupling Wave 1.1 removed. **Max frames (~265–306 ms) did not move with
either confound** and are now the only genuinely unattributed item; that
residue is streaming/apply.

---

## 1b. THE FLIP HAS TWO SILENT VISUAL REGRESSIONS, NOT ONE

**Voxel GI has exactly one consumer, and it is the quad vertex factory.**
Verified by grep, not inferred: `VoxelQuadVertexFactory.ush` is the only shader
in the tree that reads `VoxelGIVol.`, and `VoxelQuadVertexFactory.cpp:225` is
the only site that binds the GI uniform buffer for rendering. `VoxelMarch.usf`
writes `EncodeIndirectIrradiance(1.0f)` — the neutral value. `voxel.GI.Enabled`
defaults to **1**.

**So flipping to the marcher silently removes voxel GI from terrain.** Same
shape as the shadow finding, hiding in the same place: a feature the quad path
provides, that the marcher's frame-time advantage is partly funded by not
providing, with no error and no counter.

| gate | status |
|---|---|
| sun shadows | measured — 42.0M primitives / 15.8 ms on quads, **zero** on the marcher. S2 exists (0.335 ms) but reaches only 64 m. |
| voxel GI | **terrain gets none under the marcher.** Unmeasured; needs a lit-cave A/B. |

**And the GI origin is already wrong for water**, which reframes "replace the
anchor" entirely. `FVoxelGIVolume` is a `TGlobalResource` — one instance — whose
`OriginPoolUU` is computed from the **terrain** pool's location and then bound
identically to every quad draw, including all of water's buckets, each with its
own unrelated rebase. No per-primitive origin term exists in either struct. So
water's GI lookup is off by kilometres and **water silently receives no GI
today**. `VoxelGIVolume.h:69-74` states the assumption in the singular — true
when written, before water was split into buckets.

So the task is **not** "find a terrain origin so the empty stand-in pool can be
deleted". It is **"make the GI origin per-consumer, because terrain's pool was
never the right space for the consumer that survives."** Falsifiable cheaply: a
lake captured with `voxel.GI.Enabled` 0 vs 1 should, by this reading, be
*identical* — that is both the falsifier and the acceptance test.

**Also corrected:** section 5's "up to five pool components" (repeated from
`VoxelGI.cpp:440-450`) is stale. Water alone can hold **twelve**
(`kMaxWaterPoolBuckets`, `VoxelWaterSubsystem.cpp:1954`); the true live maximum
is 16.

---

## 2. Wave 3 is no longer optional or independent

Retiring quads empties the terrain pool, so nothing is gathered for any
cascade: `ShadowDepths` draws **0 primitives** in every marcher leg and **42.0M**
in every quad leg. Terrain self-shadowing is simply absent in the marcher arm.

That is a feature being switched off, not a rendering efficiency. So:

- **Wave 3 moves ahead of Wave 4.**
- **The Wave 4 flip gate must require both arms to cast**, or the flip ships a
  visible regression with a flattering frame-time number attached.
- Wave 3's own "measure first" step is **done**: the answer is that terrain
  casts today, by default, at 15.8 ms, and that is the bar.

---

## 3. Wave 1.2 — the plan's site mapping was wrong (implemented differently)

The plan said to add `RemoveChunk` inside `ReleaseChunkGeometry`, covering its
four callers. That would have freed **nothing** in the shipping config:
`ReleaseChunkGeometry` sits behind `FChunkRecord::HoldsGeometry()`, and under
quad retirement a terrain record holds neither a component nor a pool slot.
Two of the other callers would have been actively wrong — the buried/sky skips
never published a brick, and the `ApplyMeshResult` zero-quad case is what a
**buried solid chunk** looks like, whose bricks are exactly what the marcher
needs.

**Implemented instead:** a separate `ReleaseChunkBricks(Key)` at the one line
that means "this chunk has left residency" — immediately before
`ChunkRecords.Remove(Key)` in `DrainUnloads`, outside the geometry gate — plus
`EvictParkedKey`. Parked geometry is skipped at unload because adoption
re-admits with no mesh job. Verified that `DrainUnloads` is the *only* leak
point: the other `ChunkRecords.Remove` is already guarded by `HoldsTerrain`.

**Status: BUILT AND VERIFIED.** `released` is 0 while the camera is pinned and
~26,500 per 5 s window under motion (structurally zero before this wave),
`resident0` falls 26,316 → ~14,000, `evictions=0`, `writesDropped=0`, and
`indexEntries` tracks the pool down in step. The `released:absent` ratio runs
about 20:1, which is why `absent` is counted rather than treated as an error.

---

## 4. Wave 2 — there is no free half

The plan lists the biome tint face term as needing no plumbing. Both premises
are true and the conclusion is not:

- The marcher **already** shades from `VoxelPaletteFace[material * 3 + faceClass]`
  — the palette is face-class indexed and `MAT_GRASS` already carries green on
  top, brown underneath. Adding `BiomeTintForFace` would apply the same
  distinction twice, against ADR-0008 invariant 4.
- What the quad path actually has that the marcher lacks is the **gate**, not
  the term: `TerrainTintR = bNearSurface ? FaceTint : 0`, where `bNearSurface`
  tests the hit against the chunk's fitted surface plane. Without it, the face
  term is not a partial win — it is wrong underground.

So the surface-proximity gate, temperature and precipitation all depend on the
same per-chunk `ChunkParams` float4, and **the plumbing is the whole wave**.
Sizing corrected: temp + precip + `SurfaceZRelUU` + packed gradients is ~9
bytes, so spare dword 7 cannot carry it and the 32-byte record must grow either
way.

**Not written**, deliberately: it is a format change with three writers that
`voxel.GPU.VerifyBrickPack` byte-compares, plus a hardcoded 8-dword clear
kernel and a `kChunkRecordDwords * 4 == 32` static_assert. Writing that with no
compiler available is how a silently-wrong format lands.

---

## 5. A ~16 ms target that is on no wave — and a wrong theory about it

The quad path's shadow pass costs 15.8 ms and draws 42M primitives against a
camera pass drawing 21.8M. That much is measured and stands.

**My first explanation for it was wrong, and the correction matters more than
the theory did.** I saw the pool's cull fall back to submitting the *entire*
pool on empty gathers (`VoxelGpuPoolComponent.cpp:2234`, "the conservative
answer") — 100 times on shadow gathers against 48 on camera gathers — and
blamed the fallback. But **all 100 fall inside a 1.1-second window at boot**,
about 2.5 minutes before the measured period. In steady state the shadow cull
works and reports `visibleQuads` of 611,836 (1.3%) up to 5,722,657 (12.2%) of a
46.9M-quad pool. 5.72M quads is ~11.4M triangles for *one* cascade, so four
cascades over 400 m reach 42M honestly. The counting was right; joining it to
the ProfileGPU number without checking *when* either happened was not.

**The real levers, both already instrumented:**

- **Cascade distance.** `-VoxelShadowDistanceM=200` against the 400 m default
  takes the pass 16.646 → 8.337 ms. Measured; costs shadow reach and nothing
  else.
- **`voxel.Stream.GPUShadowMaxLevel` is NOT a lever, and I checked before
  recommending it.** I first named it as the obvious candidate. The per-level
  breakdown on the same log line says otherwise: `visiblePreCap: L0=5,122,276
  L1=600,381 L2..L5=0`. Shadow-visible geometry is essentially all level 0 — the
  coarse rings are not casting at all, so capping them removes nothing. The only
  other way down is **fewer level-0 quads**, which is what the marcher achieves
  by having none.

This is independent of the marcher and pays off even if the marcher never
ships.

---

## 5b. What the flip looks like — capture set for the owner

Three captures, identical shutter pose `(-6143981, -6143981, 222831)`,
pitch −20 / yaw 45, sun **frozen at 08:00** (chosen low so shadow presence is
visible; the standard 12:00 pose casts short shadows and understates it), all
fully settled.

| | | unique colours | mean RGB |
|---|---|---|---|
| **A** `VoxelVerify00115` | quad + shadows (today's default) | 10,180 | 39.8 / 43.4 / 52.5 |
| **C** `VoxelVerify00119` | quad, `r.ShadowQuality 0` | 38,239 | 125.8 / 111.4 / 96.6 |
| **B** `VoxelVerify00117` | **marcher** (casts nothing) | 51,869 | 90.4 / 79.3 / 67.9 |

`imgdiff` at 8/255:

- **A vs C — shadows alone:** 98.38% of pixels, mean delta **86.01/255**. The
  single largest visual change measured.
- **C vs B — renderer alone:** 89.79%, mean delta 45.58/255. This is Wave 2 in
  a picture.
- Both diffuse (concentration 0.10–0.11), so neither is a localised artefact and
  neither can be diagnosed further from the images alone.

Worth noting without drawing a conclusion: **the marcher renders the same
unshadowed scene meaningfully darker than the quad path** (mean 90.4 vs 125.8,
same lighting, neither casting). And its 51,869 colours against 38,239 is
consistent with ADR-0008's per-voxel jitter being present and working.

The look is the owner's call.

---

## 5c. Wave 1.2b — load-before-unload had never run for the marcher

Found by the owner watching a leg, not by an instrument: *"the moving camera
quickly outpaces the terrain streaming … chunks are missing."*

The coverage gate PR #160 added to fix ring-gap holes was **dead code under
retirement**: `held=0 covRelSettled=0 covRelAbsent=0 capRel=0` across every
marcher leg ever taken, with `retentionMs=10000` configured. **Two** gates both
asked `HoldsGeometry()`, and fixing the obvious one changed nothing because a
second gate upstream (`RecomputeDesiredSet`, where `RetainReplaceDir` is
*stamped*) had already decided the answer. Both fixed; counters now read
`held=1024 covRelSettled=620 covRelAbsent=1842`.

**It is not the fix for what the owner saw, and must not be presented as one.**
Matched window-for-window it buys 5–10% of holes, leaves the plateau unchanged
(~19–20k of 23,900 columns), and costs p95 72.76 → 90.71 ms. The retention
site's own comment says why: *"retention can only delay a hole, never
manufacture a chunk."* Holes sit at 6–14 while the camera is pinned and climb
monotonically the instant it moves. **What the owner saw is the ~600 chunks/s
generation ceiling.**

I first reported "a third of the holes gone" by comparing one leg's early-climb
windows against the other's late plateau — the fourth unlike-for-unlike
comparison in a day. Retracted in the measurements log.

**Also uncovered:** the coverage verifier itself reads `HoldsGeometry()` for its
"a coarser stand-in is on screen" test, so **it over-reports holes on the
marcher**. Before/after comparisons are sound (same bias); absolute rates are
not.

---

## 5d. Wave 3 is a leg sequence, not a build

S2 light-function injection is **already built and in the binary**, and the
material asset exists — verified by reading `VoxelShadowMarch.cpp:1424-1502`,
not by trusting the doc. What remained was the leg order: **I0** (mutation arm,
must show half-dark everywhere — proves the chain can fail), **Cal-A/Cal-B**
(the raster's own replicate noise, as the bar), then **V1** (the look).

The `voxel.Shadow.March` cvar help said *"2 = … NOT BUILT"* long after mode 2
shipped. Anyone planning this wave from the cvar help — the reasonable thing to
do — would have concluded a whole renderer needed writing. Fixed.

---

## 5e. Wave 3 measured: marched shadows cost 0.41 ms against the raster's 15.8

The full leg sequence ran.

- **I0 (mutation) — PASS.** Mask forced to 0.5; frame mean 90.4 → 63.8 at a fixed
  pose. Census: `INJECTION LIVE`, `copies=0 mutated=599 declinedNoTarget=0`.
  The chain is proven able to fail, so later captures are trustworthy.
- **Cal-A/Cal-B — the bar is 0.37%** of pixels, mean delta 1.58/255. The raster
  path reproduces itself almost exactly at a pinned pose. This retroactively
  settles every comparison in this document: shadows alone (98.38%) are ~266× the
  noise floor.
- **V1 — works.** Marched shadows move **34.42%** of pixels vs no shadows, mean
  delta 24.69, concentration **0.28** — spatially structured, not a global tone
  shift, which is what a shadow should look like.

**The number that matters: `gpuMs mean=0.335–0.409` against the raster shadow
pass's 15.8 ms — ~38×.** Every decline counter zero, `exhausted/f=0.00`,
`steps/marched mean=3.4` against a 2048 budget, so it is not cheap because it is
refusing work. This is a **like-for-like feature comparison** and is a stronger
argument for the marcher than the draw-path ratio.

**The limitation is reach, and it is structural.** At the standard pose
`far=840,288` — **62% of the frame is beyond the 64 m reach and reads lit**; only
18% is marched. Reach cannot simply be raised: 64 m "keeps the box inside R0's
level-0 residency (128 m radius), because this walk is level-0-only until the
hierarchy lands". **Taking the hierarchy is the real remaining work in Wave 3.**

---

## 5f. Wave 2 under way — format first, gate first

Following the sequencing plan, not writing shading yet.

- **Step 0 — convert the verify gate, on the OLD format, and run it. PASS.**
  `voxel.GPU.VerifyBrickPack` did not compare the record; it compared five named
  fields and never read dword 7. Now it builds `Want[]` through
  `BuildChunkRecord` and compares every dword, so every field added later is
  gated for free.
- **Step 1 — stride 8 → 16 dwords. PASS.** First run **FAILED** (`recordFail=6
  guardFail=1`) and that was the step working: it caught **two hardcoded 32-byte
  strides in the readback** (`VoxelGpuMeshAsyncVerify.cpp:1744, :1806`). Those
  three literals — the two copies and the old gate's indexing — all agreed with
  each other, so nothing could detect them until one became a constant. After
  the fix: gate byte-identical **and the render 0.00% different** from the
  8-dword binary, against a 0.37% replicate bar.
- **Step 2 — the shading fields exist, all producers neutral. PASS**,
  byte-identical. `FVoxelBrickChunkShading` is a **required** parameter, never
  defaulted, so no producer can silently ship flat climate.
- **Step 3 — real values, CPU arm then GPU fork, deliberately in that order.**
  After the CPU arm: `neutralShading = 3220` and `brickFromGpu = 3220`,
  **identical to the chunk** — the counter named exactly which producer was
  still unplumbed. After the fork: **`neutralShading = 0`**. Landing both at
  once would have taken the counter 0 → 0, which proves nothing.
  Also fixed the plan's second blocker: `GpuPoolRoot` is now assigned every tick
  rather than only inside the quad pool's constructor, so the fork's climate
  sampling no longer depends on a component Phase 5 intends to delete.

**Three safeguards now stand between this format and a silent divergence:** the
whole-record byte compare (the two *writers* agree), the bound stride
cross-check that empties the world on mismatch (the *reader* agrees), and
`neutralShading` on the 5-second line (no *producer* was forgotten). Each
catches something the other two cannot.

- **Step 4 — the surface-proximity gate. The first pixel change.** *In progress.*

**What was actually missing was the gate, not the tint.** `BiomeTintForFace`
ignores the material id — it is a purely geometric top/side/bottom rule, and the
marcher's palette is *already* indexed `[material*3 + faceClass]`. Applying the
tint too would double-apply it, against ADR-0008 invariant 4. What the quad path
has and the marcher does not is `bNearSurface ? FaceTint : 0`: without it, a +Z
face two hundred metres down a cave is tinted as turf. **Not a missing
refinement — a wrong answer underground.**

So the gate changes the **face-class choice**, not the pixel's brightness: below
the band a Z face is a cave roof, not sky-facing, so it stops selecting the TOP
palette entry rather than being darkened. A chunk with `kNoSurfaceGate` is always
surface, so the gate can never darken terrain it has no information about.

**Status: wired, fires, NOT yet shown to fire broadly.** The correct build
measures 0.00% against the ungated one — which is what a *correct* gate produces
at a pose with no sub-surface +Z faces in view. **That reading is not evidence
on its own**: inverting the comparison also gave 0.00%, revealing the gate was
never firing at all (I had used `V.LocalVoxel` where the lookup needs the
absolute chunk coordinate — the third coordinate-frame defect of the night).
After the fix the mutation moves 0.39% with a mean shift in all three channels,
so the mechanism is real; but 0.39% sits almost on the 0.37% replicate bar, and
an inverted gate ought to repaint far more. **The next instrument is a counter —
pixels where the lookup succeeded, and pixels where the face class actually
changed — not another capture.** Shipped as-is because it is fail-open and
cannot regress the picture.

Three failures on the way, all instructive:
- **Compile error** — the whole lookup API is inside `#if
  VOXEL_MARCH_HAS_BRICKPOOL`, and the emit shader's source-0 permutation has no
  chunk records at all. An unguarded call *fails to compile* rather than reading
  garbage. On this project, that is the good outcome.
- **`MarchChunkIndex was not set`** — `Register` hands the staged buffer to the
  march pass and the pooled copy isn't valid until that graph executes, so the
  emit's call returns null. Invisible while the emit never referenced the index.
  Fixed **in the emit pass only**: a zero index there just misses the lookup and
  leaves the appearance unchanged, whereas putting the fallback in the shared
  binder would have turned the march's loud fatal into a silent empty world.

Doing step 0 before step 1 is what made that failure readable. Landed together,
it would have been ambiguous between "the format is wrong" and "the gate is
wrong" — and the instinct on an ambiguous gate failure is to distrust the gate.

---

## 6. Proposed order

1. ~~**Wave 1.2**~~ — **DONE and verified.** `released` 0 while pinned →
   26,557 per 5 s under motion, `resident0` falls, `evictions=0`. Wave 1.2b
   (retention for brick records) was tried, measured, and **reverted** — see 5c.
2. **Wave 3** — **S2 measured and working** (5e). Remaining: take the hierarchy
   so reach can exceed 64 m; penumbra; non-voxel receivers.
3. **Wave 2** — in progress (5f). Steps 0 and 1 landing; 2–4 next.
   **The cascade depth gate was never broken** — `voxel.March.Stats` is an
   `FAutoConsoleCommand`, and I had put it in `-ExecCmds`, so it printed at
   **frame 0** with `frames=0 emitFrames=0 indexEntries=0`. The readback path is
   intact end to end. Fixed two ways: schedule it with `voxel.DeferExec` inside
   the run window (as `voxel-final-comparison.ps1` already does), and the gate
   now **self-reports** when its accumulation window closes, so it no longer
   depends on anyone remembering to ask.
   **`voxel.March.VerifySource` is a genuine and different defect**: the
   dispatch is never armed, because `bVolumeSettled` ANDs a term counted in
   render frames with one counted in 5-second perf ticks — needing ~5 minutes of
   idle streaming. Three further conditions on that path have no decline log at
   all.
4. **Wave 4** — flip. Gate now includes "both arms cast".
5. **Wave 5** — water off the quad pool. Its "measure first" leg was attempted
   and produced **no measurement**: at the river trunk `-160632,-85613` on a
   `surface` flight, **zero water pool components were ever created**, despite
   the water subsystem being armed and the fine tier fully resident. Water only
   meshes within ~±25.6 m of the camera, so "near a river on the map" is not
   "water in the frame". **Prerequisite nobody had written down: verify the
   candidate pose actually yields a water-pool cull line — with one capture,
   not a 3-minute perf leg — before spending the A/B.**

Also open, unchanged: `voxel.March.VerifySource` readback never lands;
`voxel.Cover.VerifyStore` standalone-`FRDGBuilder` crash; the GI volume anchor
still points at the deliberately-empty terrain pool (confirmed live in the
marcher leg's own log), which blocks the Wave 4.3 deletion.

---

## 7. Where this stands at the end of the night

**Wave 1 — done.** Fluid decoupling (1.1) and brick-pool lifetime (1.2) shipped
and verified: `released` 0 while pinned → ~26,500 per 5 s under motion,
`resident0` falls, `evictions=0`. Wave 1.2b (retention for brick records) was
built, measured, and **reverted** — it violated 1.2's own gate.

**Wave 2 — steps 0–3 shipped and verified, step 4 landing.** The chunk record is
16 dwords carrying real per-chunk climate and a fitted surface plane from all
three producers, with three independent safeguards. Step 4 (the surface-proximity
gate) is built and being mutation-proved.

**Wave 3 — S2 measured and working.** Marched sun shadows at **0.335–0.409 ms**
against the raster path's **15.8 ms**. Remaining: take the hierarchy so reach can
exceed 64 m, penumbra, non-voxel receivers.

**Wave 4 — blocked on Wave 3 by evidence, not by plan order.** The flip would
otherwise ship terrain that casts no sun shadows.

**Wave 5 — still no measurement.** The candidate pose produced zero water pools.

### The dispatch stage is now bracketed, and the obvious fix is falsified

`DispatchJobs` had one timing number and is ~44% of a tick that is 73–84% of
wall clock. Split into three:

```
dispatchMs=21.05 = airProofMs=0.51 + bandMs=0.00 + submitMs=1.56 + other=18.98
```

- **The air proof is ~2% of dispatch in steady state.** Memoising
  `FootprintSurfaceUpperBoundMm` — the change identified as highest-value — would
  buy roughly that. It *does* spike (two frames at 192 ms and 204 ms against a
  0.4 ms median), so the memo would flatten a real tail; it is just not the
  sustained cost.
Four more suspects then died the same way — the overlay scan
(`NeedsOverlayAwarePath`, 0.00 ms), the band consult (0.00), the ring-quota scan
and pop (0.02), and the speculative half (0.0, answered by a sub-total that
**already existed**). At that point five brackets covering the loop's named calls
accounted for ~2 ms of 21.

So the next instrument was a **bisector, not a sixth suspect** — one bracket
around the whole loop asking *"is it even in here"*:

```
dispatchMs=20.41 | loopMs=2.20 gpuMgrTickMs=18.21     (2.20 + 18.21 = 20.41)
dispatchMs=19.28 | loopMs=0.47 gpuMgrTickMs=18.81     (0.47 + 18.81 = 19.28)
```

**`dispatchMs` is mislabelled.** It is `(T1−T0) + (T3b−T3)`, and `T0..T1` spans
`DispatchJobs()` **and `GpuMeshJobs->Tick()`** — the GPU mesh job manager's whole
tick, which promotes jobs, builds one RDG graph for the batch, polls two readback
phases, publishes bricks and runs its own flush. Its placement there is correct
for ordering; it was simply never carved out of the timing.

### FOUND AND FIXED: the ceiling was a hash for a disabled diagnostic

Chasing it four levels down —
`tick → dispatch bucket → GpuMeshJobs->Tick → FVoxelBrickPool::Flush →
FVoxelMarchChunkIndex::ApplyDelta → MarkDirtyAndUpload` — the answer is an
**FNV-1a hash over the whole 4 MiB chunk-index grid, on the game thread, once per
flush, unconditionally**. ~190 million hash steps per 5-second window.

**Its only consumer is `voxel.March.VerifySource`**, which defaults to 0 *and*
never arms today. The most expensive thing in the streaming tick existed to feed
a disabled diagnostic.

Gated behind `SetContentHashEnabled`, switched on by the comparator that reads
it. Same flight, same config:

| | before | after |
|---|---|---|
| `uploadMs` | 3,146–3,190 | 155–880 |
| `brickFlush` | 1,627–1,806 | 108–407 |
| `dispatch` | 1,985–2,311 | 670–680 |
| `tickMs` | 3,564–4,223 | 930–1,602 |
| **% of wall clock** | **71–84%** | **18–32%** |

**~2.5 seconds of game-thread time per 5 seconds, returned.**

**Throughput rose ~25%** — `brickPacks/s` 1,743–2,001 → **2,274–2,435** in steady
state, meeting the pre-registered gate.

**But p50 frame time rose, 8.01 → 15.27 ms** (p95 63.78 → 72.83; max unchanged).
The consistent reading is that the game thread was previously hashing rather than
generating — short frames because little was happening — and now carries 25% more
real streaming work per frame. **Plausible, not yet measured.** Do not quote this
as "25% faster streaming" until the frame-time move has its own attribution: two
numbers moved in opposite directions and only one is the one the owner feels.

`removedMs = 0.0 (n=0)` — **Wave 1.2 is exonerated.** Brick removal was never the
cost, despite being the obvious suspect since `Removed` was empty until it landed.

### How it was found (the part worth reusing)



The sum closes to the hundredth on every frame, so the bucket is fully accounted
for by exactly two things and one is 90% of it. **Every fix discussed — the
memo, batching the submit, trimming the quota scan — targets the 10%.** The 90%
is a GPU-pipeline question. It is also where brick publication happens, so it may
overlap the `brickFlush` figure (32–41% of the tick) already on the same line —
check that before sizing anything.

**Not optimised**, deliberately: the next step is to bracket *inside* that tick
(promote / graph build / readback poll / publish) and only then choose. Five
reasonable guesses died getting here; the bisector was worth more than all of
them and should have been first.

### The largest open item is not on any wave

Terrain generation tops out at **1,629–1,756 chunks/s** (not the ~600 this plan
quoted), and it is **game-thread bound**: the streaming tick is 73–84% of wall
clock. Throughput is exactly `MaxJobsInFlight × frame rate`, so the tick cost
*is* the ceiling. **This is what the owner sees as "the camera outpaces
streaming", and no eviction policy will close it.**

**Now attributed** (see above): the tick's largest item is
`FVoxelGpuMeshJobManager::Tick()` at ~18–19 ms, not the dispatch loop at
0.5–3.3 ms. The "0.21 ms per dispatched chunk" figure I quoted earlier was real
but attributed to the wrong function.

### Six corrections this document exists to carry

1. The 2.43× headline had two confounds; matched it is **2.72× / 2.41×**.
2. Terrain shadows cost **15.8 ms** and the marcher casts none.
3. The p95 tail was the **fluid sim**, not the draw path.
4. Load-before-unload was **dead code** for the marcher — but switching it on was
   a net regression.
5. The depth gate was never broken; **I read it at frame 0**.
6. "~600 chunks/s" and `chunksPerSec` are both **stale or dead** on this arm.

---

## 8. PROTOTYPE BUILD — 2026-08-22, approach changed at the owner's direction

The owner's call, and it is the right one for where this is: stop measuring,
build the remaining planned work as a prototype, and iterate live in the editor
with a human designer. Everything below is BUILT AND COMPILING, and none of it
is measured. That is deliberate, not an omission.

**Every piece is on a cvar** so it can be toggled live in the editor rather than
rebuilt. That is the one measurement-shaped thing kept, and only because three
separate features this week ran and did nothing while looking perfectly healthy
— a prototype that silently no-ops costs a designer a session, not a leg.

### What is in the build

| feature | switch | default |
|---|---|---|
| ray-marched terrain | `voxel.March` | **1** (was 0) |
| brick-pool source | `voxel.March.Source` | **1** (was 0) |
| ring cascade | `voxel.March.Rings` | **1** (was 0) |
| skip levels / step budget | `voxel.March.SkipLevels` / `.StepBudget` | **2 / 3328** |
| brick production | `voxel.GPU.BrickPack` | **1** (was 0) |
| quad retirement | `voxel.Terrain.RetireQuads` | **1** (was 0) |
| marched sun shadows | `voxel.Shadow.March` | **2** (was 0) |
| shadow reach | `voxel.Shadow.MarchReachM` | **512 m** (was 64 m) |
| shadow ring cascade | `voxel.Shadow.MarchRings` | **1** (new) |
| voxel GI in the marcher | `voxel.GI.Enabled` | 1 (now actually reaches the marcher) |
| climate tint | `voxel.March.ClimateStrength` | **1.0** (new) |
| index content hash | `voxel.March.IndexContentHash` | 0 (the ~61%-of-tick cost, off) |

**Opening the editor now gives marched terrain with sun shadows, voxel GI and
climate tint, with no command-line arguments.** Previously all of it needed
`-VoxelRetireQuads=1 -VoxelBrickPack=1 -VoxelBrickPackOnCpu=1` plus five cvars.

### Three things built this session that were previously blockers

1. **Voxel GI now reaches the marcher.** It had exactly one consumer — the quad
   vertex factory — so the flip silently deleted terrain GI. The marcher wrote
   the neutral irradiance value. Ported the three-probe sample (0.6 / 1.25 / 2.0
   cells, matching `FVoxelLightField` term for term) using a new
   **camera-relative volume origin**, because the marcher has no pool primitive
   to express `OriginPoolUU` in. That new origin is also the groundwork for
   fixing water, whose GI lookup is currently offset by kilometres.
2. **Shadow reach 64 m → 512 m.** Two prerequisites had to land first: the march
   box is now **centred on the camera rather than cornered** (cornered at 2×reach
   the float32 ulp reaches the traversal's 0.01 UU nudge at ~419 m, so the
   geometry capped the reach before any ring logic did), and the walk now picks
   **one ring level per ray from the receiver's camera distance**.
3. **Climate tint.** The chunk record has carried temperature and precipitation
   since Wave 2 step 3 and nothing consumed them.

### What is honestly NOT done

- **The shadow level selection is per-RAY, not per-SEGMENT.** Correct wherever a
  ray stays inside its own annulus; where it leaves, the chunk record's level
  check rejects the foreign chunk and the ray reads **LIT** — under-shadowed,
  which is this file's existing fail-lit convention rather than a wrong picture.
  The fully correct form re-partitions the ray into camera-radial intervals,
  because a shadow ray's `t` is not distance-from-camera. That is the follow-up.
- **Wave 5 (water off the quad pool) is not started.** It is a second renderer,
  not a switch, and nothing about it is prototypable in an evening.
- **None of the above is measured.** No legs, no captures, no A/Bs.
- The GI origin audit logs once per run and will say whether water's GI is
  offset — that is a log line, not a fix.

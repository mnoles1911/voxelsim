# Build GPU waves A–F, then push R0 to 128 m

## Context

ADR-0006 recorded G0–G5 complete. They are not. G2/G3 built the **destination** — a
GPU-resident geometry pool drawn as one primitive in one draw call — and never
switched the **producer**. Chunk geometry is still meshed on CPU worker threads by
`MeshChunkBricks` (`ue-project/Source/VoxelEarth/VoxelChunkMesher.h:70`), dispatched
from `VoxelWorldSubsystem.cpp:5952`. The ADR's invariant 1 is unbuilt.

`docs/gpu-roadmap-remaining.md` (committed on `claude/gpu-roadmap`) sorts what is
left into waves A–F. This plan executes all of them in order, and finishes with
the change they exist to enable.

**The destination is a recorded decision.** `docs/gpu-g0-sizing.md:117-141`:

> **Decision (Matt, 2026-07-24): target R0 = 128 m** … It cannot ship on the CPU
> path, and the arithmetic is not close … Shipping this before generation moves to
> the GPU would take the exact symptom being chased and multiply it by four.

| | R0 radius | R0 chunks | cascade total | cascade edge |
|---|---|---|---|---|
| today | 64 m | 2,035 | 10,503 | 2 km |
| target | **128 m** | **~8,100** | **~16,600** | **4 km** |

Ring counts are flat per level (`gpu-g0-sizing.md:50-54`), so R0 = {0,128} shifts
every ring outward one slot and the world edge doubles for free. **This is why the
waves run in order A→F and R0 moves last:** moving it first quadruples the near
field on a CPU path that already cannot keep up.

**Not in scope:** the reported ring-gap symptom. The owner will investigate it
manually in a later session using `docs/manual-verification-checklist.md` item 1.

---

## Ground rules for every wave

These are the traps this programme has actually fallen into. They are not
boilerplate.

1. **Never conclude from a single run.** Two identical configurations have
   repeatedly differed more than the effect being measured (43 fps vs 103 fps on
   the same settled scene; 21.0 vs 47.4 ms p50 on an idle box). Every performance
   claim needs ≥2 legs per config and a stated noise floor. A published set of
   percentile improvements was retracted this way once already.
2. **Pin the camera.** `-VoxelPerfFlight=static` with `-VoxelPerfYaw=`/`-VoxelPerfPitch=`
   pins position *and* rotation and logs the pose. Unpinned poses swamp the signal.
3. **Rebuild voxel-core before believing any terrain.** `VoxelEarth.Build.cs:96-143`
   now warns on a stale `voxelcore.lib`. That staleness has already sent two
   investigations into the wrong subsystem and produced one wrong conclusion
   reported to the owner. If the warning fires, stop.
4. **A pooled primitive fails silently.** It renders *something* whatever you do.
   Screenshot-diff against a measured same-path noise floor; never accept
   "looks fine". The biome-tint result (17.4% → 4.3% against a 1.1% floor) is the
   standard.
5. **Let the world load before capturing.** A screenshot taken as the session opens
   shows an empty world and reads as a rendering bug. **Wait for the cascade to
   settle** — the full 2 km cascade takes ~10–30 s — and confirm it from the log
   (`jobsInFlight=0 pendingJobs=0`, `pending` 0 on every ring) rather than by
   counting seconds. Every capture must state the settle evidence next to it. This
   also means every screenshot is a *settled, stationary* scene, which structurally
   cannot show a movement-induced gap — so never offer one as evidence about
   streaming under motion.
6. **Never resolve a merge conflict with `git checkout --ours`.** It replaces the
   whole file with HEAD's version and still compiles — it silently deleted 22
   references to a feature earlier in this programme.
7. Branch per wave, PR per wave, CI green before the next wave starts. **CI does
   not build the UE project** — it covers voxel-core only (gcc/clang/msvc
   determinism digests, the float ban, and the shader lint). Every UE-side compile
   and every measurement in this plan is local, so "CI green" is necessary and
   nowhere near sufficient.
8. **Every claim in a PR body must name how it was measured**, or be marked as
   unverified. This programme has now retracted one set of numbers and corrected
   **two** root-cause diagnoses (the determinism gate twice); all three would have
   been caught by this rule.
9. **Shader source is RUNTIME-LOADED. Never edit a `.ush` while runs are in
   flight.** `ue-project/Shaders/*.ush` is read from disk when the engine
   compiles a permutation, **not** compiled into the module DLL. So editing one
   mid-batch does not take effect at the next build — it takes effect at the next
   *run*, including runs that are already queued against the old C++. If the
   shader references a uniform-buffer member the built C++ does not declare yet,
   every subsequent leg dies at
   `ShaderCompiler.cpp:2298 Fatal error` with no mention of your file. Measured
   the hard way in Wave B: seven legs of a measurement batch were lost this way
   and the crash reads as a GPU or driver fault, not as an edit. **Finish the
   batch, then edit.** The corollary is that a `.ush` edit and its matching
   `.h`/`.cpp` edit must land in the same build before any run, because the two
   halves are versioned independently by the filesystem.
10. **Line numbers in this plan drift.** `VoxelWorldSubsystem.cpp` is ~10,600 lines
   and moves under every PR. Anchor on symbol names — `DispatchJobs`,
   `DrainResults`, `MeshChunkBricks`, `EnsureVolumeOrigin` — and treat the numbers
   as hints. Several quoted here were already stale when this plan was written.

## How to use this document

This is the execution plan other sessions should follow. It is written **from the
code**, not from summaries — which is why it contradicts
`docs/gpu-roadmap-remaining.md` in four places, all listed at the bottom under
"Corrections owed".

**Each wave's PR updates its own section here with what was actually measured**, so
this stays a record rather than an intention. If a wave's stated cause turns out
wrong, correct it in place and say so — do not delete the wrong version, the same
way `backlog.md` §6a keeps its superseded diagnosis visible.

## Order of execution

```
A (cull) ─┐
B (GI)   ─┼─ parallel, disjoint files ──▶ C green ──▶ D1–D4 ──▶ D5 ──▶ F (R0=128m)
C (gate) ─┤
E (G4)   ─┘
```

A, B, C and E run concurrently as sub-agents. D cannot start until C is green (it
writes GPU-generated voxel state) and until A's buffer work has landed. D5 starts
only after D1–D4 are measured. F is last, because it is the change every earlier
wave exists to afford.

---

## Wave A — per-chunk frustum culling for the pool

**In-game:** frame cost stops scaling with *resident* geometry and starts scaling
with *visible* geometry. This is the precondition for `voxel.Stream.GPU` being
defensible as a default, and for R0 = 128 m not making frames worse.

**The measured defect** (pinned camera, settled scene, identical geometry both
paths, `loaded=9822 quads=8813242`, three legs each):

| camera | component | pooled | delta |
|---|---|---|---|
| horizon p50 | 15.12 ms | 18.58 ms | **+23%** |
| straight down p50 | 5.39 ms | 19.05 ms | **+253%** |

The control is the second row: looking at almost nothing makes the component path
64% cheaper and leaves the pool unchanged. A renderer whose cost does not depend
on what is on screen is not culling.

**Already built and building** in `VoxelEarthShaders/Private/VoxelGpuPoolComponent.cpp`:
`FChunkRun` + `BuildChunkRuns()` (declared `VoxelGpuPoolComponent.h:49-54,211`),
`BuildCulledRanges()`, the `bRunsDirty` fix for the parallel-GDME race
(`VoxelGpuPoolComponent.h:207`), and cvars `voxel.Stream.GPUCull`,
`GPUCullMergeGap`, `GPUCullMaxRanges`, plus three isolation diagnostics already in
the tree — `GPUCullDebugAllVisible`, `GPUCullDebugSplit`, `GPUCullDebugInvert`
(`:50,66,78`). **It still drops geometry**, so it ships disabled (`GEnabled = 0`,
`:15`).

*Two things to fix on the way past, both of which mislead a reader:*
`voxel.Stream.GPUCull`'s help text says **"1 = on (default)"** while the default is
0 (`:15-21`) — that is how someone concludes the cull is active when it is not.
And **`voxel.Stream.GPUCullMergeGap` is dead**: declared at `:29-34`, referenced
only in a comment at `:689`; the merge actually uses a threshold computed from
`GPUCullMaxRanges`. Tuning it does nothing. Either wire it up or delete it — do not
leave a knob that silently ignores you, and do not report a "tuned merge gap"
measurement until it is real.

**The CPU range list is not the problem.** `BuildCulledRanges`
(`VoxelGpuPoolComponent.cpp:477-715`) only ever *adds* quads when it merges and
never drops a range; runs are sorted by pool offset (`:1121`). So geometry is lost
between `Ranges` and the rasteriser — and because runs are pool-ordered, "near
quad 0 draws, far in does not" is diagnostic rather than incidental.

### A1 — one screenshot that separates three hypotheses

`GPUCullDebugSplit 2` (`VoxelGpuPoolComponent.cpp:482-508`) tiles the pool into two
exact contiguous ranges — the same quads as the single full draw, with the frustum
test bypassed. The three candidate faults predict **visibly different** pictures:

| outcome | means |
|---|---|
| identical to the full draw | `SV_VertexID` is fine; the fault is in the frustum test |
| **first half renders twice, second half missing** | `SV_VertexID` does **not** include the base vertex |
| second half missing, first half renders **once** | the draw itself is being rejected |

*This is the experiment the earlier control failed to be. That control was already
retracted in `52c8a73` — "a single range spanning first-visible..last-visible
covers 98% of the pool no matter whose runs are in the array. The experiment could
not fail." Treat `FirstIndex = FirstQuad * 6` as **untested**, not proven.*

### A2 — the likely fault, and why the roadmap's stated one is weak

**Discard the zero-stride theory.** `GNullColorVertexBuffer` is 16 bytes with
**stride 0**, so the D3D12 fetch address is `Base + StartVertexLocation * 0` —
always in bounds no matter how large the start vertex is. It deserves one
debug-layer run, not a plan built on it.

**The real lead is in the engine's own header.**
`D:\UE_5.8\...\RHI\Public\DataDrivenShaderPlatformInfo.h:1195-1201`:

```cpp
// Returns true if SV_VertexID contains BaseVertexIndex passed to the draw call,
// false if shaders must manually construct an absolute VertexID.
inline bool RHISupportsAbsoluteVertexID(const FStaticShaderPlatform P)
{
    return IsVulkanPlatform(P) || IsVulkanMobilePlatform(P);
}
```

**D3D12 is not in that set**, and `FLocalVertexFactory` compensates by threading the
base through a uniform buffer and adding it in the shader (`LocalVertexFactory.cpp:128`,
`LocalVertexFactory.ush:730`, `VF_VertexOffset`). The comment now sitting at
`VoxelGpuPoolComponent.cpp:336-337` — *"SV_VertexID includes the draw's start vertex
for a non-indexed draw"* — is an assumption **the engine explicitly declines to
make**. If it is wrong, every range draws pool quads `[0, Count)` instead of
`[First, First+Count)`, which reads on screen exactly as the reported symptom and
also explains why the 98% control passed.

*Honest caveat: every in-engine call site of that helper is an **indexed** draw, so
it does not strictly settle the non-indexed case. A1 settles it.*

### A3 — the fix: explicit `BaseQuad` through a uniform buffer

Correct under either hypothesis, and it matches the engine's own `VF_VertexOffset`
idiom: every draw starts at vertex 0 and the shader computes
`QuadIndex = BaseQuad + VertexId / 6`.

**The constraint that decides the implementation:** loose per-element
`FShaderParameter`s **do not bind in this project** — measured, and the reason the
current design uses SRVs in a uniform buffer. From `docs/gpu-g2-draw-path.md`:

> `FShaderParameter::IsBound()` returns false for both `VoxelChunkOriginUU` and
> `VoxelChunkLevelScale` … Because `ShaderBindings.Add()` on an unbound parameter
> is a **silent no-op**, `LevelScale` arrives as 0, every vertex collapses to a
> point, and the chunk vanishes with nothing in the log.

So route `BaseQuad` through a **second small uniform buffer**, which is exactly the
shape already working for `FVoxelGIVolumeParameters` in the same hook
(`VoxelQuadVertexFactory.cpp:161-165`):

1. `BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelQuadRangeParameters, )` with one
   `SHADER_PARAMETER(uint32, BaseQuad)`, created per range with
   `CreateUniformBufferImmediate(..., UniformBuffer_SingleFrame)`.
2. Store it in `Element.UserData` allocated via
   **`Collector.AllocateOneFrameResource<T>()`** — a stack local will not survive to
   draw submission. (Do **not** copy `VoxelGpuChunkComponent.cpp:185`, which points
   `UserData` at a proxy member and is dead code nothing reads.)
3. `GetElementShaderBindings` already exists and already runs
   (`VoxelQuadVertexFactory.cpp:97-179`); today it reads nothing from
   `BatchElement`. Its own comment says *"G3 will need per-chunk ranges and will
   move to UserData"* — this is that change. Landscape's
   `LandscapeRender.cpp:3607-3613` is the in-engine reference.
4. Shader change is **two lines**: `QuadIndex` is derived in exactly one place
   (`VoxelQuadVertexFactory.ush:96`) and everything downstream is already expressed
   in terms of it. Reach it as `VoxelRange.BaseQuad` through the struct, never as a
   loose global — `:21-26` explains that loose globals compile fine and silently
   read zeros.
5. **Keep `BaseQuad = 0` on the uncull path** so the single-draw case stays
   byte-identical, and verify that before enabling the cull.

*Clean-ups while in here:* delete the two vestigial `LAYOUT_FIELD(FShaderParameter)`
bindings at `VoxelQuadVertexFactory.cpp:101-102` (neither name exists in the `.ush`
any more), and the dead `FVoxelChunkDrawData`. Also `GPUCullMaxRanges` defaults to
**256** while the renderer's element loop is `(1ull << BatchElementIndex) &
BatchElementMask` — undefined for index ≥ 64. Cap it.

### A4 — measure

`-VoxelPerfFlight=static` at both poses (horizon, straight down), ≥2 legs per
config, three configs: component / pooled-uncull / pooled-cull. **Success is the
pool tracking the component path's visibility scaling** — the straight-down case
getting dramatically cheaper — not merely getting faster at the horizon. Log range
count and redrawn quads. Only add a merge-gap sweep as a fourth config **after**
the dead cvar is wired up.

**Falsified if:** culled and unculled frames differ visually at all, or the range
count is high enough that draw-call overhead eats the saving.

**Then:** default `voxel.Stream.GPUCull` on, and re-flip `voxel.Stream.GPU` on
**only if A3 shows the pool at or below the component path**. That flip was
reverted once for exactly this reason; do not repeat it on hope.

**Deferred, not dropped — and there is a real tension to record.** The ADR says
twice that the pool **must compact into one contiguous draw range per frame**,
because `RHIMultiDrawIndexedPrimitiveIndirect` is never called with
`MaxDrawArguments > 1` and D3D12 implements the single-draw entry point on top of
it. N draw ranges is the thing the design says not to do, and multi-element batches
additionally foreclose the static-relevance upgrade path (static-mesh command
caching requires exactly one `FMeshBatchElement`, `PrimitiveSceneProxy.cpp:164`).

The end state is therefore a compute cull/compaction pass writing
`FRHIDrawIndexedIndirectParameters`, with `NumPrimitives = 0` as the documented
signal to use `IndirectArgsBuffer` (`MeshBatch.h:272`). **The project has zero
indirect draws today** — no `DrawPrimitiveIndirect`, no indirect args anywhere in
`ue-project/`. Land the CPU cull first as the measurable, correct step; revisit
after Wave D, when the pool is already GPU-written and the chunk table already
lives on the GPU. Record the multi-range version explicitly as an interim.

---

## Wave B — GI volume, steps 3–5

**In-game:** lighting stops being baked into geometry, so an edit updates lighting
by texel upload instead of by re-shading and re-uploading vertex colours. Cave and
dig lighting become correct at range instead of only within 12.8 m of spawn.

Steps 0–2 landed and are verified: the pooled path feeds the light field (it
previously never did — `voxel.GI.Enabled 1` was a silent no-op under
`voxel.Stream.GPU`), the volume is sampled **per pixel** via a `GIUVW : TEXCOORD1`
interpolant folded into vertex colour `.g` in `GetMaterialPixelParameters`, and
the ±Z encode matches the CPU sampler at **0.000 mean error** against a half-cell
shifted control.

> **RESULT (2026-07-26).** All measurements below are from
> `.claude/worktrees/wave-b-gi`, `-VoxelSpawnAt=-84480,53760`, settled
> (`jobsInFlight=0 pendingJobs=0`), 2,212 resident bricks, unless stated.
>
> - **B1 — closed, by reversing the expected answer.** Scheme B never passed the
>   design's own bar (an **RMS**, fed a mean-abs for three transcripts running);
>   Scheme A is built and measures **0.000 on every direction class**.
> - **B3, B4, B5, B6 — landed.**
> - **B2 — one claim retracted and re-measured**; the transient is real
>   (69.3% of occupied texels, reaching the camera's own row). Ships only on the
>   corrected claim, or defaults off.
> - **B7 — target changed** to `voxel.GI.Enabled 1` / `voxel.GI.Volume 0`, once
>   the coupling to Wave A was found. The volume is correct-and-off.
> - **B-M — a new finding, not root-caused**: the light field is effectively
>   **empty under motion** (0–12 bricks against 2,212 settled). It qualifies the
>   B7 recommendation and is recorded rather than fixed.

### The correction this wave owes the plan (and the plan owed the roadmap)

The roadmap said step 5 was "stop re-meshing a chunk to refresh its lighting on
the pooled path". This plan corrected that to "the retirable cost is the
**component** path's 5×5×5 re-shade plus the `voxel.GI.MaxQuadSpanVoxels`
subdivision". **That correction is also wrong, and in the same direction.**

Read `ApplyMeshResult`: under `voxel.Stream.GPU 1` with the default
`GPUMaxLevel -1`, the pooled branch returns before any `UVoxelChunkComponent` is
constructed, for **every** level-0 chunk. `BuildChunkVertexData` gates all of its
GI work on `bGIEnabled = VoxelGI::IsEnabled() && ChunkLevel == 0`, and GI is
level-0-only. So on the pooled path there is **no component to re-shade and no
quad to subdivide** — both costs are already exactly zero, and were before this
wave started.

What *was* real, and is now fixed: every solved pooled brick was still being
pushed onto `RefreshQueue`, where it could only ever be popped and discarded
(pooled bricks have no `BrickComponents` entry, by design). That is what forced
the 8× pop cap added in step 0. Bricks with no component no longer go on the
queue at all.

**So Wave B's prize is not a cost saving. It is a capability**: on the pooled
renderer, per-vertex baked GI does not exist at all, and the volume is the only
thing that can light a dig or a cave there. Worth stating plainly, because
"retire the baked path's cost" set an expectation this wave cannot meet and does
not need to.

### B1 (step 3) — the encoding: **the stated bar is failed, and it was the wrong statistic**

Expected outcome was "keep Scheme B and record why". Scheme B stays — but not
because it passed.

`voxel.GI.VolumeCheck 20000`, `VolumeDim 192`, two legs on one build:

| | leg A | leg B |
|---|---|---|
| ±Z mean / RMS / max | **0.000 / 0.000 / 0.000** | **0.000 / 0.000 / 0.000** |
| horizontal mean | **9.629** | **9.463** |
| horizontal RMS | 21.051 | 20.405 |
| horizontal p50 / p90 / p95 / p99 / max | 0.013 / 37.4 / 53.0 / 91.6 / 105.0 | 0.229 / 36.8 / 51.1 / 81.8 / 105.0 |
| fraction over the 8/255 bar | **27.68%** | **27.69%** |
| control (half-cell X shift) | 1.576 | 1.662 |
| signal (±Z irradiance) mean / sd | 19.7 / 40.7 | 20.3 / 41.0 |

Three findings, in order of how much they change the decision.

**1. The two in-tree transcripts are reconciled, and neither was wrong.** The
design doc records `5.950 / control 0.591`; `backlog.md:301` and the roadmap
record `6.165 / control 0.565`. My two legs agree with *each other* to **±0.17
bytes**, so the harness is precise. What moves the number is **the field's
composition when it fires** — the design-doc run measured 1,947 resident bricks,
mine 2,212 (the settled figure step 0 itself records). Different resident sets,
different mix of open sky and enclosed geometry, different horizontal error.
**They are three measurements of three field states, not three attempts at one
number.** Anyone re-running this must quote the brick count beside the error or
the figure means nothing.

**2. At the settled state the design's bar is failed.**
`gpu-gi-volume-design.md:103` says "under ~8/255 means Scheme B is free quality".
At settle it is **9.5**. The two passing readings on record were taken on a
less-settled field.

**3. The mean was the wrong statistic anyway, and this is the finding that
matters.** The distribution is **bimodal**: p50 ≈ 0.02–0.23 (essentially exact)
while p95 ≈ 52 and max = 105. Scheme B stores `B = mean(±X, ±Y)`, so it is exact
wherever the four horizontal directions agree — open ground, uniformly-lit
interiors — and wrong by up to 41% of full scale wherever they disagree, which is
precisely **a side face next to a vertical occluder**, i.e. most of a cave wall.
A mean of 6 or 9 averages those two populations and describes neither. §2 asked
for an RMS and got a mean-abs; the harness now reports both, plus percentiles and
the fraction over the bar.

**4. On the statistic §2 actually asked for, Scheme B fails by 2.6× — and always
did.** §2's words are *"report `RMS(Vis[±X], Vis[±Y] − their mean)` in irradiance
bytes. Under ~8/255 means Scheme B is free quality."* **The bar is an RMS.** The
harness reported a mean-abs and it was compared against the RMS threshold as
though the two were interchangeable. Measured RMS is **20.4–21.1**. So the
5.950/6.165 readings on record never passed §2's bar; they were a different,
smaller statistic that happened to land just under the number.

**Decision: ship Scheme A.** My first conclusion here was "keep Scheme B", and it
was wrong. Two errors in it, both mine:

- I compared against the mean-abs bar rather than §2's RMS bar, so I recorded a
  marginal failure (9.5 vs 8) where the real one is 2.6×.
- I asserted Scheme A "doubles a sample the depth pass also pays". **It does not.**
  Scheme A stores `(+X,+Y,+Z,v)` and `(−X,−Y,−Z,v)` in two textures, and §2 itself
  establishes that *every greedy-mesh normal is axis-aligned, so exactly one slot
  is selected*. A given face therefore needs exactly **one** of the two textures —
  branch on the sign of the normal, sample one, take one component. §2's own "two
  samples" is the cost for a general normal, which this mesh never produces.
  **Scheme A costs memory and nothing else.**

With the sample-count objection gone, the trade is memory against wall accuracy,
and the design's sizing table does not show the configuration that settles it:

| config | VRAM | CPU shadow | reach | walls |
|---|---|---|---|---|
| Scheme B, N=256 (design's recommendation) | 67.1 MB | 64 MB | ±51.2 m | **approximate** |
| **Scheme A, N=192** | **56.6 MB** | 54 MB | ±38.4 m | **exact** |
| Scheme A, N=256 | 134.2 MB | 128 MB | ±51.2 m | exact |

**Scheme A at N=192 costs less VRAM and less RAM than the configuration the design
already recommends**, and buys exactness on every face. What it gives up is 13 m
of reach. For a game whose GI exists for caves and digging, that is the cheaper
thing to give up — and reach is the axis the fade already has to hand back on
anyway (risk 8), whereas a wall lit by the mean of four directions is wrong in a
way no amount of fading fixes.

Two costs of Scheme A the table does not show, recorded so the decision is not
taken on VRAM alone: the CPU shadow doubles (it is a full mirror of the staged
bytes), and B2's staged re-centre moves `Dim^3 * 8` rather than `Dim^3 * 4` bytes, so
re-centre upload traffic doubles too (~54 MB per re-centre at N=192, ~42 MB/s at
20 m/s across the default dead zone).

**Still owed, and it is the thing that would overturn this:** a screenshot. p95 of
52/255 is 20% of range, and on a *dark* wall the relative error is larger still —
an irradiance error of 0.20 swings `lerp(AmbientFloor, 1, Irr)` from 0.06 to 0.35
where `Irr` is small, a factor of ~2 in brightness. That is an arithmetic
projection, not an observation. The `-VoxelGITest` wall-and-roof fixture puts a
side face under a roof, which is exactly the geometry the error concentrates on,
and is the capture that settles whether this reads as wrong lighting or as
nothing.

**Scheme A is built and measured.** Same harness, same scene, same 20,000
samples, Dim 192, settled at 2,212 resident bricks / 933 in volume:

| statistic | Scheme B (2 legs) | **Scheme A** |
|---|---|---|
| ±Z mean / RMS / max | 0.000 / 0.000 / 0.000 | 0.000 / 0.000 / 0.000 |
| horizontal mean | 9.629 / 9.463 | **0.000** |
| horizontal RMS | 21.051 / 20.405 | **0.000** |
| horizontal p50 / p90 / p95 / p99 | 0.013 / 37.4 / 53.0 / 91.6 | **0 / 0 / 0 / 0** |
| horizontal max | 105.0 | **0.000** |
| fraction over the 8/255 bar | 27.68% / 27.69% | **0.0000** |
| control (half-cell X shift) | 1.576 / 1.662 | 1.525 (max 58.8) |
| verdict | PASS (±Z only) | **PASS (both classes)** |

**The control is what makes the zeros load-bearing.** A harness comparing a thing
against itself also reports 0.000; this one still reports 1.525 mean and 58.8 max
on the deliberately mis-addressed probe, so it is still measuring something.

**Measured memory: 54.0 MiB** for both volumes at Dim 192. The design doc's table
says 56.6 MB for Scheme A at N=192 — **these are the same quantity**, 56.6 × 10⁶
bytes = 54.0 × 2²⁰ bytes. Flagged because a future reader comparing the log line
against the table will otherwise go looking for a 5% discrepancy that does not
exist.

**Implementation note that is the whole reason this was affordable:** a face picks
the volume by the **sign** of its normal and the channel by the **axis** — one
sample, one channel. The second volume is never read.


### B2 (step 4) — camera-following re-centring

`EnsureVolumeOrigin` latched on `bVolumeOriginSet` and never re-entered, so the
volume sat wherever the camera was at the first upload and `DrainVolumeUploads`
silently dropped every brick outside it. Now: dead zone
(`voxel.GI.VolumeRecentreCells`, default 64 cells = 2560 UU) + staged re-upload +
a one-frame origin swap, exactly as §4 decided. Toroidal addressing and
double-buffering stay rejected.

**The gap in §4's design, which the implementation cannot remove and which anyone
reading that section should know about.** §4 says the dead zone "guarantees the
old volume still covers the camera throughout", and reads as if that made the
staged re-upload invisible. Coverage is not the issue. The issue is that the
texture has no per-region origin: while the staged upload is in flight it holds a
**mix** of old-addressed and new-addressed bytes, and the shader is reading all
of it with the old origin. Every row already restaged is therefore returning
irradiance from a point one re-centre shift away until the swap lands. §4 does
not mention this, and it is not avoidable without the two options §4 rejected.

Two things bound it, both deliberate:

- rows are restaged **from both ends inward**, so the stale region is furthest
  from the camera — where the distance fade is already attenuating GI — and the
  camera's own row is restaged in the **committing frame**, whose upload and
  origin swap are enqueued into the same render frame;
- the swap is a single uniform-buffer publish, so no frame ever mixes two
  origins.

`voxel.GI.Debug 2` prints `VOLUMERECENTRE transient:` with the peak stale
occupied-texel count and how close the nearest stale row got to the camera, so
this is a number rather than an argument.

> **RETRACTED (2026-07-26), original left visible above.** The two bullets above
> claimed the transient "is bounded" by the both-ends-inward ordering, and
> specifically that *"the camera's own row is restaged in the committing frame,
> whose upload and origin swap are enqueued into the same render frame"*. **The
> first claim holds. The second is false.** Both are now measured rather than
> argued.

**MEASURED.** A forced re-centre over a *populated* field
(`voxel.GI.VolumeRecentreTest`, 163 bricks restaged, shift 176×48×−160 cells,
Dim 192, `voxel.GI.Debug 2`):

```
VOLUMERECENTRE transient: peakStaleTexels=14645 of 21122 occupied (69.3%),
                          nearestStaleRowToCamera=0 UU
```

Two things wrong with what was written:

1. **69.3% of occupied texels are misaddressed in the frame before the swap.**
   That is inherent to staging over ~8 frames — by the penultimate frame 7/8 of
   the volume has been rewritten — and **no ordering can reduce it**. Ordering
   controls *where* the stale region is, not *how much* of it there is. The
   original wording implied the quantity was small. It is most of the volume.
2. **`nearestStaleRowToCamera = 0 UU`: the stale region reached the camera's own
   row one frame before the commit**, so there is a potential one-frame artifact
   in exactly the place the original claim said there could not be one. Cause:
   the step loop takes **two** rows off the low end and **one** off the high end
   per frame, so the meeting point drifts toward the high end instead of staying
   on the camera. The camera's row lands in frame 7 of 8, not frame 8.

**Why the scripted flight did not catch it, and why that is its own finding.**
The first B2 verification ran the 20 m/s scripted flight and reported seven
re-centres, each exactly 8 frames with a one-frame origin commit — and every one
of them reported `0 of 0 occupied`. The transient measurement was vacuous because
**the field was empty**. See finding B-M below; it is a bigger problem than this
one.

**Status: the ordering fix (order rows by descending distance from the camera's
row) is owed, followed by a re-measurement.** B2 ships only on whichever of these
the re-measurement supports:

- *"A transient exists, bounded to ≤8 frames, confined to the far field, camera's
  row last"* — ship with exactly that claim, not rounded up to "cannot pop"; or
- re-centring **defaults off** and is recorded as not ready.

### B3 (step 5) — see "the correction this wave owes the plan" above

Retired: the pooled path's dead `RefreshQueue` traffic. Kept, as required:
`NotifyPooledChunkMeshUpdated`'s quad hand-off — the field must still be fed.
Not retired, because it does not exist on this path: the 5×5×5 re-shade and the
quad subdivision.

### B4 — the two shade formulas, reconciled

All four divergences were live, and all four would have surfaced as a lighting
seam at the `voxel.Stream.GPUMaxLevel` boundary where both renderers draw into
one frame.

| # | was | now |
|---|---|---|
| 1 | `Strength`/`AmbientFloor` **hardcoded** 1.0/0.06, ignoring `voxel.GI.Strength` / `voxel.GI.AmbientFloor` | supplied by the subsystem through `FVoxelGIVolumeSettings` (VoxelEarthShaders may not depend on VoxelEarth, so they cannot be read there) |
| 2 | fade radii derived from the volume extent (896/1152 UU at Dim 64) vs the CPU's 4800/6400 — design risk 8, live | the same cvars feed both paths, clamped to the half-extent |
| 3 | `DistUU` measured to the **volume centre** | measured to the **camera**, which is what the CPU measures; the dead zone makes those differ by up to 2560 UU |
| 4 | **one** probe offset (0.6 cells) vs the CPU's three | the same three, `{0.6, 1.25, 2.0}`; later samples only execute where the first missed |

On (2), the clamp **slides the band rather than truncating it**. Clamping
`FadeEnd` alone would have left `FadeStart` where it was and collapsed a 1600 UU
fade into a **40 UU** one at `VolumeDim 192` — which is the hard lighting ring
risk 8 describes, reached from the other direction. A fired clamp is now logged
(`VoxelGI volume params:` prints both the effective fade and the one the cvars
asked for), because risk 8's whole difficulty is that it produces a plausible
image.

On (4), the CPU walks three offsets specifically because a 20 cm roof slab is
half a light-field cell thick. Shipping one offset GPU-side had quietly
reintroduced the roof-underside defect this module already fixed once.

**A cost fix found while doing (2), worth its own line.** The shader now skips
the volume sample entirely when the probe UVW is outside `[0,1]`. That is not an
edge case: the volume covers ±38.4 m of a **2 km** cascade, so most pixels on
screen are outside it; the sampler is `AM_Clamp`, so an outside lookup returns the
face texel (step 1 saw exactly this as "large smeared bands" and nearly diagnosed
it as an addressing bug); and the material is masked, so the **depth pass pays it
too**. Skipping is exact, not approximate — the fade is clamped to end inside the
half-extent, so `Weight` is already 0 wherever the branch fires.

### B5 — the two correct-by-construction claims, now measurable

`voxel.GI.VolumeDigTest [radiusUU]` carves through the real edit path
(`CarveSphere`), **suppresses the round-robin re-solve** for the duration — the
round-robin delivers bricks in `TMap` iteration order, which is exactly what makes
the steady-state 1.4 bricks/run figure uninformative about the dig case — and
then reads its answers out of `VolumeShadow`, the actual staged bytes rather than
a re-encode.

It reports three things: bricks-per-run on a contiguous neighbourhood,
zero-on-revoxelize (dug bricks reach the volume as `A = 0` **before** the solve
lands, so a tunnel cannot keep its pre-dig lighting), and zero-on-evict, driven
through the real `EvictFarBricks` rather than waited for.

RESULT_B5_PLACEHOLDER

### B6 — the live-toggle claim is now true

`voxel.GI.Volume`'s help text said it was read per frame. It was not:
`UpdateParameters_RenderThread` had three callers, none per-frame, so `Enabled`
and `DebugVis` were latched for the session. There is now a real per-frame driver
(`TickVolume`) that re-publishes the uniform buffer whenever any input actually
changes — the same hook B2 needed, as predicted.

**Also fixed while in here:** the volume texture was allocated in `InitRHI`. It is
a `TGlobalResource`, so that charged **every** session for it whether or not GI
was ever enabled — 1 MB at the bring-up `Dim 64`, but **67 MB at the recommended
shipping `Dim 256`**, handed to players who never turn GI on. It is now allocated
on first enable, and zeroed on creation (undefined bytes in the validity channel
read as "there is data here", i.e. arbitrary irradiance on every surface until the
first upload covers that texel).

### B7 — ship it on

**Target, as it now stands: `voxel.GI.Enabled 1` and `voxel.GI.Volume 0`.**

The original wording was "Wave B is complete when `voxel.GI.Enabled 1` **and**
`voxel.GI.Volume 1` are the defaults". That conflated two things, and the
conflation only became visible once the coupling below was found.

**The coupling the plan missed: `voxel.GI.Volume 1` has no consumer under
`voxel.Stream.GPU 0`, which is the default.** Only the pooled vertex factory
samples the volume; the per-chunk component path bakes GI into vertex colours
and never reads the texture. So shipping the volume on by default is not a Wave B
decision at all — it is gated on Wave A's `voxel.Stream.GPU` flip, which Wave A
has measured as not ready. The plan's "Order of execution" treats A and B as
independent and disjoint. They are disjoint in *files*; they are not independent
in *shipping*.

**What that leaves is better, not worse: GI can ship today, on the renderer that
is actually the default, with no new work.** The component path's CPU-baked
per-vertex GI already works. `voxel.GI.Enabled 1` with `voxel.GI.Volume 0` is a
shipping configuration right now, and it satisfies the owner's actual request —
"ship GI on by default" — rather than the narrower reading.

**The volume is correct-and-off, not unfinished.** Its encoding is exact
(B1: 0.000 on every direction class, against a live control), its shade formula
matches the CPU's term for term (B4), its parameters are live (B6), and its
uploads are measured (B5). What it lacks is a renderer that reads it.

RESULT_B7_COST_PLACEHOLDER

### Considered and rejected: making the COMPONENT path sample the volume

The obvious way to decouple B7 from Wave A. **Do not build it.** Recorded with
reasons so it is not reached for again:

- It needs a **material-graph change** to `M_VoxelTerrain` — the single thing
  §0 of the design doc establishes this whole feature does *not* need, and the
  reason the vertex-colour fold was chosen in the first place.
- The volume is a raw `FRHITexture` in a global uniform buffer that nothing binds
  for component draws. Reaching it from a material needs either a
  `UVolumeTexture` wrapper set on a material instance, or a custom vertex factory
  for the component path. Material Parameter Collections cannot carry textures.
- The UVW cannot be computed from world position in the material without hitting
  §6's precision rule (float32 ULP is 1.0 UU at 8.4 M UU against a 40 UU cell).
  The workable route is per-chunk **custom primitive data** carrying
  `ChunkOrigin − VolumeOrigin` — which then has to be **rewritten on every one of
  ~2,000 components at every re-centre**, i.e. roughly the per-component touch
  the volume exists to delete.
- Worst: it would make the material graph a **third** copy of the shade formula.
  B4 found the existing **two** copies had silently diverged in **four** places.
  A third copy, in a graph where a divergence cannot be grepped for, is the wrong
  direction.

### B-M — **the light field is effectively empty under motion. NOT ROOT-CAUSED.**

Found while trying to verify B2, and it is a bigger question than B2: it asks
whether voxel GI works at all in play, as opposed to in a stationary capture.

**Measured**, same build, same seed, same spawn (`-VoxelSpawnAt=-84480,53760`):

| scene | resident light-field bricks |
|---|---|
| settled, camera stationary | **2,212** |
| during `-VoxelPerfFlight=surface` (~20 m/s), whole 90 s run | **0–12** |

Throughout the flight the GI tick reports `pendingVox=0(+0 pooled)` — the ingest
queue is not backed up, it is **empty** — while the streaming system is loading
normally around it (`R0 loaded=3131`, `loaded=53855` cumulative, pool active).
So chunks are being meshed and pooled, but almost none of them are reaching
`NotifyPooledChunkMeshUpdated` → `PendingPooledVoxelize` → `VoxelizeChunk`, or
they are being dropped immediately after.

**Why it matters, and where it lands.** A player is moving most of the time. If
GI is present only when standing still, then `voxel.GI.Enabled 1` as a default
buys much less than the cost legs suggest, and every screenshot ever taken of
this feature — all of which are settled, stationary scenes by house style — has
been measuring the one case where it works. It also means the B2 re-centring
verification cannot be done on the scripted flight at all, which is how this was
found.

**Explicitly not root-caused.** Candidates not yet separated: the build-radius
test in the voxelize drain (`Dist(Origin, FieldCentreUU) > voxel.GI.RadiusUU`)
rejecting chunks whose camera moved on between enqueue and drain;
`EvictFarBricks` at twice a second outrunning a 16-chunk-per-frame voxelize
budget; or the ingest hook not firing for most pooled applies. These have very
different fixes and the evidence so far does not choose between them.

**The two cheap legs that would split it** (owed, not run): field brick count
during the flight, and again ~5 s after coming to rest. Recovering on stop means
a throughput/priority problem; not recovering means bricks are never requested —
the same fork that splits the owner's ring-gap symptom in
`docs/manual-verification-checklist.md` §1, which makes it interesting beyond GI.

**Deliberately not chased in this wave.** Recorded so it survives, and carried as
a caveat on the B7 recommendation rather than fixed under it.

## Wave C — the determinism gate (blocks Wave D on correctness)

**In-game:** nothing directly. But while this is red, GPU-generated voxel state
cannot be trusted, and Wave D writes exactly that.

Same HLSL, same GPU, same CPU reference:

| leg | toolchain | result | digest |
|---|---|---|---|
| `bench/vxc_gpu.exe` | DXC → SPIR-V → Vulkan | **PASS**, bit-exact | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE → DXIL → D3D12 | **FAIL** | `046b4a9f9c5e49b7` |

Localised (`docs/backlog.md:219-243`): all 4,096 columns match, quad decode matches
exactly over 20,544 vertices, **cells differ on material ids only** and in one
direction — the DXIL build's soil column sits one layer shallower
(`cpu=2 gpu=5`, `cpu=5 gpu=12`).

### The recorded diagnosis is wrong. Do not chase it.

`docs/backlog.md:238-242`, `docs/gpu-roadmap-remaining.md:74-84` and the C1 entry of
an earlier draft of this plan all name **floating-point contraction** as the prime
suspect. **That hypothesis does not survive reading the shader.**

- `voxel-core/shaders/worldgen.ush` contains **no floating-point arithmetic at
  all** — every operand from raster to material id is `int64_t`/`uint64_t`, which
  the file's own header states (`worldgen.ush:6-13`). The only `float`/`precise`
  hits in 1,660 lines are inside comments. **There is no `<` for an FMA to flip by
  one ULP, because there is no float to hold a ULP.**
- Neither leg passes any FP flag. And on the D3D12 path in UE 5.8,
  `CFLAG_NoFastMath` **is never translated** — `TranslateCompilerFlagD3D11`
  (`D3DShaderCompiler.cpp:52-61`) maps only `PreferFlowControl`,
  `AvoidFlowControl` and `WarningsAsErrors`; everything else returns 0, and the DXC
  path has no `-Gis` / `D3DCOMPILE_IEEE_STRICTNESS` equivalent. **"Force strict
  IEEE for `VoxelizeMain`" is not a fix that can be written.**

This is the third wrong root-cause on this one failure. Record the correction in
`backlog.md` §6a and the roadmap alongside the existing one, rather than deleting
it — same reasoning the last correction used.

### What the evidence actually supports

The real compile-flag deltas between the legs:

| | bench (PASS) | Unreal (FAIL) |
|---|---|---|
| **target profile** | **`cs_6_0`** | **`cs_6_6` / `cs_6_8`** |
| **parameter binding** | `cbuffer WorldGenParams : register(b0)` | loose `$Globals` scalars (`VXC_UE`) |
| backend | `-spirv`, vulkan1.1 | DXIL, `-auto-binding-space 0` |
| optimisation | `-O3` | `-O3` (identical) |
| FP flags | none | none |

- **C1a — the `$Globals` packing hypothesis (test first).** Under `VXC_UE`,
  `worldgen.ush:396-406` declares loose globals instead of the bench's explicit
  `cbuffer` (`:408-422`). A packing mismatch on `BrickZMin`/`BricksZ` would produce
  **exactly** the observed signature: all 4,096 columns match, quad decode matches,
  and cells are wrong by a consistent vertical shift of one layer. Cheapest
  possible test: log the `BrickZMin` the shader actually receives (write it to an
  unused output slot) and compare to what the CPU passed. If they differ, this is
  the whole bug and no compiler theory is needed.
- **C1b — int64 codegen across shader models.** This file has *already* produced one
  cross-vendor divergence of exactly this class: the M0 AMD-vs-NVIDIA failure was
  64-bit `OpSDiv`/`OpSRem` with mixed-sign operands, fixed by routing through
  `absToU64` + `OpUDiv` (`worldgen.ush:146-199`). If C1a is clean, force UE to
  `cs_6_0` (or the bench to 6.6) and see which way the digest moves — that isolates
  shader model from backend in one run.
- **C0 — source/bytecode skew (ten-minute elimination).** The bench runs **committed
  SPIR-V**, not a fresh compile. Confirmed in sync — `worldgen.ush` and
  `worldgen.VoxelizeMain.spv` were last touched by the same commit (`3fbf3f7`) — so
  this is already eliminated on the record and needs re-checking only if either
  file moves.

Any edit to `worldgen.ush` passes CI's shader-lint job (signed `/` and `%` outside
the approved `floorDiv`/`truncDiv` helpers, wave-width assumptions, unclamped
writes) **and** requires a matching SPIR-V respin via `tools/compile-shaders.ps1`,
or the skew C0 eliminated is reintroduced deliberately.
- **C2 — NVIDIA determinism leg.** Never run; the cross-vendor claim rests on one
  AMD card. ~$5, ~20 min on a rented box. Only meaningful once C1 is green.
- **C3 — min-spec-proxy M1 gate re-run.** Owed, and now plausibly passable.

**Do not re-baseline the Unreal digest.** `046b4a9f9c5e49b7` is the output of a
build that disagrees with the CPU; recording it would turn a loud failure into a
silent one. Re-baselining the *bench* value is legitimate whenever wanted.

**Falsified if:** the flags are already identical — in which case the divergence is
in DXIL codegen itself and the fix is a targeted `precise` qualifier on the layer
comparison inputs, mirrored into the SPIR-V leg and re-gated.

---

## Wave D — GPU meshing into the streaming path

**In-game:** chunk geometry stops being produced by ~24 saturated CPU worker
threads. This is the change that makes R0 = 128 m affordable: `gpu-g0-sizing.md`
measures ~8,100 R0 chunks at **~0.09 s** on the GPU against **~11.5 s** on the CPU.

Gated on Wave C being green.

### D1 — make the pool GPU-writable (`VoxelGpuPoolComponent.cpp:117-147`)

- Add `EBufferUsageFlags::UnorderedAccess` to the quad and chunk-id buffers; create
  UAVs alongside the existing SRVs.
- Bring both into RDG via `ConvertToExternalBuffer` / `RegisterExternalBuffer` — the
  pattern `docs/gpu-g2-draw-path.md:89-93` prescribes and the shipped code did not
  follow.
- **Buffer identity must stay stable.** The factory bakes these SRVs into a uniform
  buffer built once in `InitRHI`. In-place UAV writes are fine; reallocation is not.
- **Neutralise the CPU shadow clobber — the highest-severity silent-corruption risk
  in this wave.** `PooledQuads`/`QuadChunkIds` (`VoxelGpuPoolComponent.h:148-152`)
  are a full CPU mirror, and `CreateSceneProxy` re-uploads all of it. Any
  `MarkRenderStateDirty` path would overwrite GPU-written quads with stale CPU
  content — presenting as **terrain reverting to older geometry after an unrelated
  event**. Mark GPU-owned ranges and skip them on rebuild, or drive rebuilds from a
  re-dispatch.

### D2 — gated `MeshEmit` variant emitting pool-ready quads

`voxel-core/shaders/worldgen.ush` emit block, `VoxelGpuWorldGen.cpp:455-468`.
Today's quads are brick-local (`slice/u0/v0` are 0..7 inside one 8³ brick). Bake in
(a) the brick→chunk offset, giving chunk-local 0..31 exactly as `MeshChunkBricks`
produces; and (b) a **pool base offset** so the shader writes straight into the
chunk's allocated range — it already writes at `InQuadOffsets[maskIndex] + local`,
so this is an added uniform, not a restructure.

**Must be a gated permutation, not an edit in place:** the digest gate hashes quad
fields, so changing emitted bytes changes the digest even though quads are
display-only.

### D3 — allocation without a big readback

Upper-bound reservation does not work (static bound ≈98k quads/chunk against a
typical ~900, and the draw's vertex count comes from `Pool.GetHighWaterMark()`, so
over-allocation directly inflates the thing Wave A just proved the renderer is
bound by). **Read back only the 4-byte scan total per chunk**, which the scan
already computes. Latency, not bandwidth:

1. frame N: dispatch into a **persistent** scratch buffer (external, not an RDG
   transient — today's `Voxel.Quads` dies at `GraphBuilder.Execute()`), enqueue the
   count readback;
2. frame N+k: count lands → `Pool.Alloc(exact)` → GPU→GPU compaction copy into the
   allocated range;
3. CPU writes the chunk-table entry as it does today.

### D4 — wire the async runner into dispatch

**Most of this already exists and is unwired.** `FVoxelGpuMeshJobManager`
(`VoxelEarthShaders/{Public,Private}/VoxelGpuMeshJobManager.*`) is an async
per-request state machine with `Submit`/`Tick`/`CancelAll`, it calls
`GraphBuilder.Execute()` **without** `SubmitAndBlockUntilGPUIdle`
(`.cpp:380-383`), and its header already states the exactly-one-outcome invariant
(`.h:22-29`) that mirrors the CPU path's. Its own scope note says: *"Nothing here
is wired into DispatchJobs or the streaming path, and the brick-local → chunk-local
rebase still happens on the CPU after readback."*

So D4 is: delete the three `AddEnqueueCopyPass` calls at
`VoxelGpuMeshJobManager.cpp:370-375` and the poll/`Lock`/rebase tail, point the
emit at the pool UAV from D1/D2, and wire `Submit` into `DispatchJobs`
(`VoxelWorldSubsystem.cpp:5807`) behind a cvar, **level 0, non-edited chunks only**.
Do **not** build on `RunRegionBlocking` — it captures its outputs **by reference**
into the render command (`VoxelGpuWorldGen.cpp:493-502`), which is safe only
because of the flush at `:544`/`:587`.

**The invariant that must not break**, quoted from `VoxelWorldSubsystem.cpp:7183-7188`:

> every job DispatchJobs launches produces exactly one result, live or stale, so
> this is the matching decrement and must happen BEFORE the stale `continue` below
> or a ring would leak slots against its floor.

Note the two counters are decremented in **different places**, and a GPU path has to
respect both: `JobsInFlightCounter` is decremented on the **worker thread** at
enqueue (`:6532-6535`), while `LevelJobsInFlight[]` is decremented on the **game
thread** in `DrainResults` (`:7188`). `FootprintBandCache` is read in `DispatchJobs`
(`:5931`, `:5994`, `:5324`) but written only in `DrainResults` (`:7101-7104`),
deliberately *before* the stale discard. A dropped result strands a whole (X,Y)
column via the cold-band throttle — a bug already diagnosed and fixed once here.

Decide explicitly whether "in flight" means dispatched or completed; with GPU
latency those differ by frames. Watch `StaleResultsDiscarded` (`:7191-7202`):
multi-frame latency makes stale results *more* common, and `GenerationId` is
snapshotted at `:6115` and compared at `:7191`.

**Batch across chunks.** A region dispatch is a brick-aligned slab with a 1-brick
halo, so meshing one 32³ chunk alone dispatches 48³ voxels — 3.4× waste. Batch a
footprint's column or a tile of neighbours and slice the quad stream by
`meshBrickIndex`. Cap: `MaskCount ≤ 65,536` (single-workgroup scan) ⇒ ≤1,365
interior bricks per dispatch.

### D5 — coarse-level GPU generation (levels 1–5) — **in scope (owner's call)**

`voxel-core/shaders/worldgen.ush` has **no level parameter**. Levels ≥1 are ~80% of
resident chunks and, under motion, ~75% of meshing work. This is the largest and
most determinism-risky piece in the programme: mirroring coarse generation
bit-exactly in HLSL and re-passing the Wave C gate **at every level**.

**Sequencing, not scope:** start D5 only after D1–D4 are landed and measured. The
plumbing (UAV pool, async state machine, exactly-one-result invariant, batching)
is shared, and debugging it against a novel generation path at the same time
doubles the search space for any failure.

Approach:
1. Add the level parameter to `worldgen.ush` and mirror `MakeCoarseLevelSampler`'s
   B×B amplifier evaluation (`VoxelWorldSubsystem.cpp:274-278`). Note the CPU cost
   is already flat in level, so this is a correctness port, not an optimisation.
2. **Gate each level independently.** Extend `voxel.GPU.VerifyRegion` to take a
   level and require bit-exactness at that level before enabling it in
   `DispatchJobs`. Ship level by level behind `voxel.Stream.GPUMaxLevel`, which
   already puts both producers in one frame.
3. The **ring skirt** has no GPU equivalent. `ComputeRingSkirtMask`
   (`VoxelWorldSubsystem.cpp:5397-5406`) returns 0 for level 0 — which is why D1–D4
   can ignore it — but coarse rings need the inward retaining wall or the cascade
   seam reopens as see-through edges. Mirror `RingSkirtMask` into the kernel as a
   uniform that forces the apron to `MAT_AIR` on flagged faces, exactly as
   `MeshChunkBricks`'s sampler does (`VoxelChunkMesher.h:119-134`).

**Falsified if:** any level cannot be made bit-exact against the CPU. Stop at the
last level that passes and ship that as `GPUMaxLevel`; do not relax the gate.

### What stays on the CPU

- **The 34×34 column grid and band reduction.** `FootprintBandCache` is reduced via
  `ColumnDeepestAirVoxel`, which needs `Col.cave.segs[]`, `shaftMarginSq`, `cavern`,
  `bedrockDepthMm` — fields `FVoxelGpuColumnSample` does not carry. The band feeds
  two admission/dispatch skips *and* the cold-band throttle, which deadlocks a whole
  (X,Y) column if bands stop arriving. So GPU meshing removes at most the ~55% of
  level-0 job time that is meshing, until the GPU column struct grows.
- **Edited chunks.** `NeedsOverlayAwarePath` already routes them to a game-thread
  path; the GPU never sees them. A carve-out, not a blocker.

### Verification, in this order

1. **Bit-exactness before any rendering claim.** Mesh the same chunk both ways,
   compare packed quads byte for byte. Extend `voxel.GPU.SpawnPool`'s existing CPU
   round trip into an A/B assert rather than inventing a harness.
2. **Screenshot diff** of GPU-meshed vs CPU-meshed level 0 against a measured
   same-path noise floor.
3. **Motion measurement** — per-ring dispatch rate × per-ring p50, before and after.
4. `voxel.Stream.GPUMaxLevel` keeps both renderers in one frame; it is the sharpest
   tool here.

**Falsified if:** GPU quads differ from CPU quads for any unedited level-0 chunk; or
`markTimeouts`/stranded columns appear; or the stale-result rate climbs enough to
negate the throughput gain.

---

## Wave E — remaining G4 parity items

- **E1 — per-chunk debug tints.** The last G4 item and the only one still needing
  `M_VoxelTerrain.uasset` edited. Storage is solved (`ChunkParams.w` is free); route
  is a `float4` `TexCoords` interpolant with the tint in `.zw`. **Encode identity as
  ZERO, not white** — the component path supplies only a `float2`, so `TexCoord0.zw`
  arrives as zero there regardless of the graph, and a naive unpack treating 0 as
  black would render every component-path chunk black the moment the material is
  regenerated. Debug-only in play.
- **E2 — water pool parity number.** `voxel.Water.GPU` is landed and renders
  correctly (2,231 cavern bricks / 28,862 quads in one primitive) but its comparison
  is honestly missing: same-path repeats differed 20–88%. Needs a scene whose only
  variable is the water (`-VoxelFloodTest=70`). Also record the constraint: one
  primitive means one translucent sort key for all water — harmless for the current
  constant-opacity material, but W5 fill-fraction shading, foam, caustics or
  refraction would each break it.
- **E3 — ring cross-fade: CLOSED, do not build.** Re-tested after the seam fix gave
  the annuli their overlap band; it still produces see-through patches at ring
  boundaries. It is the one item on the G0 checklist that should not be built.

---

## Wave F — R0 = 128 m

The recorded target, and the reason A–D exist. **Land last**, after Wave D is
measured green.

- **F1 — shift the cascade.** `kDefaultRingPresets`
  (`ue-project/Source/VoxelEarth/VoxelWorldSubsystem.h:92-99`) becomes
  `{0,128},{128,256},{256,512},{512,1024},{1024,2048},{2048,4096}`. **A whole-cascade
  re-layout is forced, not optional:** the annuli must abut exactly
  (`Outer[L] == Inner[L+1]`, asserted as an invariant at
  `VoxelWorldSubsystem.cpp:5082`), so R0 = 128 m cannot be changed alone without
  making R1 degenerate. Ring counts are flat per level, so every ring shifts one
  slot outward and **the world edge doubles to 4 km**.
  **No code change is needed to test it** — the shipped binary takes
  `-VoxelRingInnerMeters=0,128,256,512,1024,2048 -VoxelRingOuterMeters=128,256,512,1024,2048,4096`
  (parsing at `:2208-2231`). Prototype and measure before touching the default.
- **F2 — resize the pool.** `kPoolCapacityQuads = 14,000,000`
  (`VoxelWorldSubsystem.cpp:6701`) against 8.81M today. R0 alone goes 2,035 → ~8,100
  chunks at ~1,532 quads/chunk, adding **~9.3 M quads** — so 14 M is roughly 60%
  short and the target is ~22–24 M (≈180 MB). Size from the measured high-water mark
  of the F1 prototype, not from that estimate.
  Also: **the terrain pool never calls `SetChunkTableCapacity` at all.** It relies on
  `MaxChunks = Max(InOrigins.Num() * 4, ChunkTableCapacity)` with a default of 1024
  (`VoxelGpuPoolComponent.h:231`, `.cpp:117`) — the water subsystem is the only
  caller (`VoxelWaterSubsystem.cpp:526`). At ~16,600 chunks give it an explicit
  floor, or pay a full render-state rebuild each time it is crossed. Re-check
  `GetLargestFreeRun`/`GetFreeRunCount`, which the header flags as the early warning
  that allocations are about to fail on a pool that still looks half empty.
- **F3 — re-tune the ring budgets.** `kRingCapShare` and `kRingSlotFloorDefault`
  (`VoxelWorldSubsystem.cpp:1996,2038`) were sized for the current radii. Note the
  measured catastrophe on record: floors `{0,2,3,4,4}` reserved 13 of 24 slots and
  collapsed throughput from 49,179 chunks to 558. Change these one at a time, with
  measurement.
- **F4 — check `AVoxelClipmapActor`.** It derives its **entire** vertex spacing from
  `GetRingPresets()[GetMaxRingLevel()].OuterMeters` (`VoxelClipmapActor.cpp:529`), so
  doubling the cascade edge changes the 30 km heightmap's inner hole and spacing.
  Verify no seam opens between the voxel edge and the clipmap.
- **F5 — LWC/precision re-check.** The world is 84 km wide and float32 ULP is 1.0 UU
  at 8.4M UU; the rebase already happens in double precision, but a 4 km cascade
  puts more geometry further from the rebase origin. Confirm no vertex jitter at the
  new edge.

**Falsified if:** the F1 prototype's cold fill or motion residency is worse than
today's — in which case Wave D did not deliver and R0 must stay at 64 m.

---

## Verification summary

| wave | the measurement that decides it |
|---|---|
| A | `-VoxelPerfFlight=static` at two poses, ≥2 legs × 4 configs; pool must track the component path's visibility scaling |
| B | existing GI equivalence harness (the 0.000 / 0.565 control pair) + a dig-and-hold PIE capture |
| C | `voxel.GPU.VerifyRegion` bit-exact **and** `bench/vxc_gpu.exe` still bit-exact — both legs, or it is not fixed |
| D | packed-quad byte equality first; then screenshot diff vs noise floor; then per-ring dispatch-rate × p50 under motion |
| E | E1 screenshot on both render paths; E2 a water-only-variable scene, ≥2 legs |
| F | cold-fill time, motion residency per ring, pool high-water mark, clipmap seam screenshot |

Everything a human must confirm in the editor is appended to
`docs/manual-verification-checklist.md` as it arises, so the owner's PIE session at
the end has one list. `docs/gpu-roadmap-remaining.md` and `docs/backlog.md` §6a/§6b
are updated as each wave lands.

## Corrections owed to `docs/gpu-roadmap-remaining.md` (and `backlog.md` §6a/§6b)

Four, all found while planning this from the source:

1. **The determinism gate's "prime suspect" is impossible.** Both docs name
   floating-point contraction; `worldgen.ush` has no floating point in it, and
   UE 5.8's D3D12 backend cannot express the proposed fix anyway. Record the
   correction beside the existing one rather than replacing it — same reasoning the
   last correction used, and this is now the second wrong diagnosis on this one
   failure.
2. **Wave A's stated cause is weak and its stated fix would not have worked.** The
   zero-stride buffer has stride 0, so a large `StartVertexLocation` is harmless;
   and "pass BaseQuad as per-element shader data" via a loose `FShaderParameter`
   would have hit the already-measured non-binding wall and failed **silently**.
   Both corrected in Wave A above.
3. **Wave B's stated purpose describes work that does not exist.** Pooled bricks are
   never re-shaded, so there is no lighting-driven pooled re-mesh to retire. The
   retirable cost is on the component path, plus the quad subdivision.
   **— CORRECTED AGAIN (2026-07-26, Wave B), same direction.** The component
   path does not pay it either, under the configuration that has a volume to
   sample: `ApplyMeshResult`'s pooled branch returns before constructing a
   `UVoxelChunkComponent` for every level-0 chunk, and `BuildChunkVertexData`
   gates all GI work on `ChunkLevel == 0`. Both costs are already zero on that
   path. Wave B's prize is a **capability**, not a saving — see Wave B above.
   Third statement of this item, second correction; the pattern is that each
   version was written one level of indirection away from the code.
4. **The horizontal-error figure disagrees across the tree** — `6.165 / control
   0.565` in the roadmap and backlog, `5.950 / control 0.591` in the design doc's
   own transcript. Reconcile to one run, or record that they are two.
   **— RESOLVED (2026-07-26, Wave B): they are two, and a third.** Two legs on one
   build agree to ±0.17 bytes, so the harness is precise; the figure tracks the
   **field composition** at the moment it fires (1,947 vs 2,212 resident bricks).
   At the settled 2,212-brick state it is **9.5**, which **fails** the design
   doc's own 8/255 bar. Quote the brick count beside the error or the number
   means nothing. Full working in Wave B, B1.

That is three of six waves whose published root cause or fix was wrong. Worth
noting plainly in the doc: **the roadmap was written from summaries, and this plan
was written from the code.**

## Parallelism

Waves A, B, C and E touch disjoint files and run concurrently as sub-agents:

- **A** — `VoxelGpuPoolComponent.cpp`, `VoxelQuadVertexFactory.*`, `.ush`
- **B** — `VoxelGIVolume.*`, GI paths in `VoxelWorldSubsystem.cpp`
- **C** — `worldgen.ush`, `VoxelEarthShaders` compilation environment, `VoxelGpuVerify.cpp`
- **E** — the material asset, `VoxelWaterSubsystem.cpp`

**D and F are serial and must not be parallelised**: D depends on C and on A's
buffer work; F depends on D's measurement. D5 depends on D1–D4's measured result.

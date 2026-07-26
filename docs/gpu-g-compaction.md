# Wave G — GPU-driven compaction for the voxel pool

Written 2026-07-26 **from the 5.8 engine source and the current tree**, before any
code was changed, because `docs/gpu-waves-plan.md`'s ground rule "cost before
build" has caught four wrong premises in Wave D alone.

This document's first job is to answer one question the owner asked directly:
**is `docs/gpu-g2-draw-path.md`'s indirect-draw design still accurate against the
code, and what does it understate?**

Short answer: **every positive claim it makes is true and I verified all of them.
What it omits is where the compute pass goes, and that omission is most of the
wave.**

---

## 1. What the ADR gets right (verified, do not re-derive)

| claim | verified at |
|---|---|
| `NumPrimitives == 0` is the signal to use `IndirectArgsBuffer` | `MeshBatch.h:272`, and it is **enforced both ways** — see below |
| non-indexed indirect draws exist and are reachable | `MeshPassProcessor.cpp:1369-1385` → `RHICmdList.DrawPrimitiveIndirect` |
| D3D12 implements it | `D3D12Commands.cpp:1227` — `ExecuteIndirect` on `GetDrawIndirectCommandSignature()`, `MaxCommandCount = 1` |
| persistent buffers are `TRefCountPtr<FRDGPooledBuffer>` via `ConvertToExternalBuffer` | still the pattern; `RenderGraphUtils.h:1352` |

The args struct is `FRHIDrawIndirectParameters` (`RHI.h:561`):

```cpp
uint32 VertexCountPerInstance;   // = CompactedQuadCount * 6
uint32 InstanceCount;            // = 1
uint32 StartVertexLocation;      // = 0 forever (see §5)
uint32 StartInstanceLocation;    // = 0
```

**Stronger than "documented": it is `checkf`'d in both directions.**
`FMeshDrawCommand::SetDrawParametersAndFinalize` (`MeshPassProcessor.cpp:952-963`)
fails a check if `NumPrimitives > 0` while an args buffer is set, *and* fails a
check if `NumPrimitives == 0` while one is not. There is no silent-wrong-geometry
mode here, which is a relief on a renderer whose characteristic failure is
exactly that.

**One correction to the brief.** `SubmitDrawIndirectEnd` (`MeshPassProcessor.cpp:1408`)
does branch on `NumPrimitives == 0` as advertised — but it has **zero callers in
the entire Renderer module**. Only the declaration (`MeshPassProcessor.h:1468`)
and the definition exist. The function that actually submits a base-pass draw is
**`SubmitDrawEnd`** (`:1332`), which handles the indirect case through the same
test at `:1371`. Same outcome; it matters only because anyone bringing this up by
breakpointing `SubmitDrawIndirectEnd` will never hit it and will read that as
"the indirect path is not being taken".

**A genuine piece of luck, worth stating because it removes a whole failure
mode.** `SubmitDrawEnd` can *override* a command's own args from
`SceneArgs.IndirectArgsBuffer`, but only when
`MeshDrawCommand.PrimitiveIdStreamIndex >= 0` (`:1334`). This factory
deliberately omits `SupportsPrimitiveIdStream` (`VoxelQuadVertexFactory.cpp:210-214`),
so that index is `INDEX_NONE` and instance culling cannot hijack our args buffer.
*(Confirm during bring-up rather than trusting this paragraph — it is read from
the flag list, not measured.)*

---

## 2. What it understates, #1: **there is nowhere in the current architecture to put the compute pass**

The ADR describes the compaction pass as a step. It is the wave.

Constraint 5 of `gpu-g2-draw-path.md` says RDG inserts the compute→draw barrier
automatically "as long as both the writing pass and the draw declare the same
`FRDGBufferRef`". **The draw cannot declare anything.** The cull decision lives in
`FVoxelGpuPoolSceneProxy::GetDynamicMeshElements`, and `FMeshElementCollector`
exposes `FRHICommandList& GetRHICommandList()` (`MeshElementCollector.h:68`) and
**no `FRDGBuilder` at all**. So the prescribed pattern is unavailable at the only
place that currently knows which chunks are visible.

**The one in-engine precedent for this exact shape** — a compute cull writing
indirect args consumed by an `FMeshBatchElement` — is Landscape, and the piece
that makes it work is named nowhere in our design:

```cpp
// LandscapeCulling.cpp:767
DispatchIntermediates.IndirectArgsBuffer =
    ConvertToExternalAccessBuffer(GraphBuilder, IndirectArgsRDG, ERHIAccess::IndirectArgs);
FRHIBuffer* IndirectArgsBufferRHI = DispatchIntermediates.IndirectArgsBuffer->GetRHI();
...
Args.IndirectArgsBuffer = IndirectArgsBufferRHI;          // raw FRHIBuffer* onto the element
// LandscapeRender.cpp:3038
ViewDependentMeshBatch.Elements[0].IndirectArgsBuffer = CullingArgs.IndirectArgsBuffer;
```

`ConvertToExternalAccessBuffer(..., ERHIAccess::IndirectArgs)` is **load-bearing,
not tidiness**. `FD3D12CommandContext::SetupIndirectArgument`
(`D3D12Commands.cpp:108-118`) does *not* transition the buffer — it flushes
pending barriers and updates residency, nothing else. The buffer must already be
in the indirect-args state by the time the draw is submitted, and nothing on the
mesh-pass path will put it there. Get this wrong and the symptom is a D3D12
validation error at best and undefined draw arguments at worst.

And Landscape schedules that work from
**`FLandscapeSceneViewExtension::PreInitViews_RenderThread(FRDGBuilder&)`**
(`LandscapeRender.cpp:1328`), a `FSceneViewExtension` override, called from
`SceneVisibility.cpp:5127` before visibility runs — i.e. before
`GetDynamicMeshElements` gathers.

> **`ue-project/Source` contains zero matches for `SceneViewExtension`.** Wave G
> has to introduce one: registration, render-thread lifetime, and per-view state
> keyed by `FSceneView*`. That is subsystem-scale plumbing and it appears nowhere
> in the ADR's scope.

The alternative — dispatching the compute shader straight onto the collector's
`FRHICommandList` from inside `GetDynamicMeshElements` — is possible
(`FComputeShaderUtils::Dispatch` has an RHI overload) but that method is called
**concurrently across views and shadow cascades** (`bSupportsParallelGDME`, the
reason Wave A had to move the cull's working set off the proxy). Issuing a
dispatch whose result a later pass reads, from a parallel gather task, with no
RDG dependency to order it, is the kind of thing that works on one vendor and
one driver. Not the first thing to try.

---

## 3. What it understates, #2: **shadow gathers, and what they do to the headline number**

`GetDynamicMeshElements` is called once per shadow cascade as well as once per
view — Wave A's cull handles this explicitly, taking
`View.GetDynamicMeshElementsShadowCullFrustum()` and backing out the
pre-shadow-translation from every plane (`VoxelGpuPoolComponent.cpp:567-591`).
The pool sets `CastShadow = true` in its constructor.

Shadow frusta **do not exist** at `PreInitViews_RenderThread` — they are built in
`InitDynamicShadows`, afterwards. So a compaction pass scheduled the way
Landscape schedules its own can serve **only the main view(s)**, and every shadow
gather keeps either Wave A's range path or the full draw.

**This is not fatal, but it resizes the claim, and the resizing must happen
before the numbers are quoted rather than after.** Wave A's
~1.19 µs / 1000 quads model was fitted against **whole-frame p50**, so whatever
the shadow depth passes cost for this pool is already inside that slope. The
`drawnQuads` figure the model was fed is logged per gather and the periodic log
does not separate main-view gathers from shadow gathers. Therefore:

> **The predicted 4.7 ms at the horizon is an upper bound on what main-view
> compaction can recover, not an estimate of it.**

### The first experiment, and it can fail

Split the existing `drawnQuads` / `visibleQuads` counters by gather type — the
proxy already computes `bShadowGather` two lines away — and log the two totals
separately over a settled pinned pose.

- It is a **count**, not a frame time: immune to the 400 ms clamp and to GPU
  contention, which makes it the safest evidence available.
- It costs one leg per pose and **needs no compaction, no indirect draw, and no
  view extension** to run.
- **It can fail, and failure is informative.** If most of the horizon's drawn
  quads come from shadow gathers, then main-view compaction cannot deliver
  anything near 4.7 ms and the wave's headline is wrong before a line of it is
  written. That would redirect Wave G toward the shadow path — or toward a
  cheaper answer entirely — for the price of one measurement.

This is deliberately the opposite shape from Wave A's first cull control, which
"covered 98% of the pool regardless of contents and certified nothing".

---

## 4. What it understates, #3: `BaseQuad` does not disappear, it forks

The plan says the `.ush` becomes `QuadIndex = CompactedIds[VertexId / 6]` and the
per-range `BaseQuad` Wave A added "becomes 0 forever". True **of the compacted
path**. Still needing Wave A's expression, on the same day the compacted path
ships:

- `voxel.Stream.GPUCull 0` — the uncull control, which every measurement in Wave
  A is read against and which must stay byte-identical to survive as a control.
- The single-chunk `UVoxelGpuChunkComponent` (`PoolMode == 0`).
- Every shadow gather, per §3.
- `GPUCullDebugSplit` / `DebugInvert` / `DebugAllVisible`, all three of which are
  range-shaped by construction.

So `VoxelQuadVertexFactory.ush:110` becomes a **branch**, not a substitution —
one flag in a uniform buffer, or one permutation. It is still exactly one place
in the file, which is the property that matters; it is not one line.

## 4a. Smaller traps, recorded so they are not rediscovered

- **`MaxVertexIndex` is still read when `NumPrimitives == 0`.**
  `SetDrawParametersAndFinalize` skips `VertexParams` on the indirect branch, but
  `MESH_DRAW_COMMAND_STATS` (`:965-969`) reads
  `MaxVertexIndex - MinVertexIndex + 1` unconditionally. Only a stat, but leave
  the element's Min/Max sane rather than zeroed.
- **A capacity rebuild invalidates every GPU-written range.** Flagged by Wave D
  for its own buffers; it applies to a compaction output buffer identically. The
  compacted list and the args buffer must be re-derived, not carried, across any
  path that goes through `MarkRenderStateDirty`.
- **`kMaxBatchElements = 64` stops being a constraint on the compacted path** —
  one element, one draw — but the fallback paths above still live under it, so the
  constant and its reasoning stay.

---

## 5. Why compaction, and not "indirect draw plus a base"

Recorded in `gpu-waves-plan.md` Wave A and re-confirmed here: an indirect draw
supplies `StartVertexLocation` from the GPU buffer, and D3D12 does **not** put
`StartVertexLocation` into `SV_VertexID`
(`RHISupportsAbsoluteVertexID` is Vulkan-only). So an indirect draw cannot
express "start at pool quad F" either. Anything drawing a sub-range of the pool
needs the base in-shader regardless of how the draw is issued.

Compaction sidesteps it: the compacted list *is* the indirection, `StartVertexLocation`
is 0, and the 64-range element cap — which is what forces the horizon's 2.72×
over-draw — stops existing.

---

## 6. Plan, staged so each layer is proven before the next is stacked

The project has **zero indirect draws today**. Every stage below is separately
falsifiable, and the early ones deliberately have a known-exact correct output.

### G0 — the gather split (no code on the draw path)
Per §3. Counts only. Decides whether the wave's stated value is real.

### G1 — an indirect draw that reproduces the current full draw, bit for bit
The minimum machinery that can prove the draw mechanism:

- a persistent buffer with `EBufferUsageFlags::DrawIndirect` holding
  `{ NumQuads * 6, 1, 0, 0 }`, written CPU-side on the render thread;
- `NumPrimitives = 0`, `IndirectArgsBuffer` set, `IndexBuffer = nullptr`;
- **no compute pass, no view extension, no RDG, no shader change.**

Correct output is known exactly: pixel-identical to the current uncull full draw
against a same-session floor. **A failure here is unambiguously the draw
mechanism** — the state transition, the command signature, the `checkf` pair, or
the factory's lack of a primitive-id stream — and not compaction logic, because
there is none yet.

*If G1 cannot be made to render, the ADR's design does not work in this project
and that is worth knowing on day one rather than after the compute pass exists.*

### G2 — the compaction kernel, in its own `.usf`
`ue-project/Shaders/VoxelQuadCompact.usf`, following `VoxelQuadScan.usf`'s
precedent and its stated reasons: outside `worldgen.ush`, outside the determinism
digest's blast radius, no SPIR-V respin, clear of ground rule 10.

Verified against a CPU reference first — the same compacted id list the render
thread can compute from `Runs` — so the kernel is proven before it is ever the
thing driving a picture.

### G3 — schedule it: the `FSceneViewExtension` and the external-access conversion
Per §2. This is where the real bring-up risk sits, and by this point both the
draw and the kernel are already known good.

### G4 — the `.ush` branch, and the A/B
Only then does `VoxelQuadVertexFactory.ush:110` fork. **That file is shared and is
coordinated through the owner before it is touched.**

### Ordering against Wave D
G1's args buffer is proxy-owned and touches no pool buffer, so it does not
conflict with `claude/wave-d-pool-uav`. **G2's kernel writes into buffers Wave D
is moving to component ownership** — that is D1's foundation being used as
intended, and G2's landing is gated on D1 merging.

---

## 7. Verification order (from the owner's brief, unchanged)

1. **Pixel identity first** — against a same-session floor, because the floor is
   bimodal (two clusters, 0.00% within, ~1.81% between; a fixed threshold reads
   the per-session latch as signal).
2. **Then the mechanism** — `drawnQuads` should fall to `visibleQuads`. A count,
   not a time: immune to the clamp and to contention.
3. **Then the pinned-pose A/B**, ≥2 legs, both poses, against Wave A's board
   (component 9.42/9.89 horizon, 2.25/2.30 down; pooled-cull 9.34/9.31 and
   2.42/2.43).
4. **Then, only if the numbers support it**, the `voxel.Stream.GPU` default flip —
   criterion written down before the deciding legs run, as Wave A did.

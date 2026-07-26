# GPU (ADR-0006) — what remains, and what each wave buys the game

Written 2026-07-26, after G0–G5 were recorded complete. They were not: G2/G3 built
the *destination* (a GPU-resident geometry pool drawn in one call) but never
switched the *producer*. Chunk geometry is still meshed on CPU worker threads.

Each wave below says what it changes **in the game**, not just in the code, and
carries an honest value verdict. Two waves are marked questionable; one item is
closed as "do not build".

> **This document was written from summaries. `docs/gpu-waves-plan.md` was written
> from the code, and it corrects four things below.** Read that one before
> starting work; it is the execution plan. The corrections are marked inline as
> **CORRECTED** and the reasoning is in the plan.
>
> Two scope decisions have also been taken since (Matt, 2026-07-26): **Wave D is
> in scope including coarse levels**, and **Wave B ships GI on by default** —
> which changes the verdicts recorded below for both.

---

## Wave A — Make the pooled renderer worth having (frustum culling)

**What the pool already does:** draws ~9,822 chunks / 8.8M quads as **one
primitive, one draw call**, instead of 9,822 scene primitives. Streaming a chunk
in or out writes into a buffer instead of calling `FScene::AddPrimitive`.

**What is wrong with it:** it has **no per-chunk frustum culling**, so it pays for
every resident quad every frame regardless of where you look. Measured on a
settled scene with a pinned camera: **+23% frame time at p50 vs the component
path, +253% when little is in frustum.** The control is decisive — point the
camera at almost nothing and the component path gets 64% cheaper while the pool
does not move (18.58 → 19.05 ms). A renderer whose cost is invariant to what is
on screen is not culling.

| | |
|---|---|
| **In-game effect** | Frame cost stops scaling with *resident* geometry and starts scaling with *visible* geometry. This is what makes `voxel.Stream.GPU` shippable as a default at all. |
| **Status** | CPU-side cull written; the parallel-`GetDynamicMeshElements` race that corrupted its working set is **fixed** (that also fixed a crash). It still drops geometry, so it stays `voxel.Stream.GPUCull 0`. |
| **Next lead** | ~~The zero-stride vertex stream.~~ **CORRECTED** — `GNullColorVertexBuffer` is 16 bytes at **stride 0**, so the fetch address is `Base + StartVertexLocation * 0` and is always in bounds. The real lead is `RHISupportsAbsoluteVertexID`, which returns **false on D3D12**: `SV_VertexID` may not include the draw's base vertex, so every range would draw quads `[0, Count)`. See `gpu-waves-plan.md` Wave A2. |
| **Real fix if confirmed** | Pass each range's base quad down so every draw starts at vertex 0 and the shader computes `QuadIndex = BaseQuad + VertexId/6`. **CORRECTED** — this must go through a **uniform buffer**, not a loose per-element `FShaderParameter`: those are measured **not to bind** in this project (`gpu-g2-draw-path.md`), and `ShaderBindings.Add()` on an unbound parameter is a **silent** no-op. |
| **End state** | GPU-driven culling — a compute pass over the chunk table emitting an **indirect draw**, which keeps *one* draw call rather than one per visible run. This is the ADR's own design (`gpu-g2-draw-path.md`), and the docs state twice that the pool must compact into one contiguous range per frame. |
| **Verdict** | **Worth doing.** Without it the pool is a measured regression and cannot be the default. With it, one draw call for the whole world is a genuine architectural win. |

---

## Wave B — Voxel GI as a GPU volume (steps 3–5)

**The point is not throughput.** Today GI is baked into vertex colours at mesh
time, so a dig re-shades a chunk partly just to refresh its lighting. Sampling a
volume texture by world position decouples the two: lighting updates become texel
uploads, geometry updates stay geometry updates.

> **CORRECTED.** An earlier version of this section said the re-mesh being retired
> was on the **pooled** path. It is not: `BrickComponents` has no entry for pooled
> bricks (`VoxelGI.h:154-161`), so pooled chunks are never re-shaded at all, and
> `CollectDirtyChunks` already expands an edit only to the 1-voxel mesher apron,
> never to a lighting radius. The 5×5×5 re-shade that is genuinely retirable is on
> the **component** path, and the larger cost is the `voxel.GI.MaxQuadSpanVoxels`
> quad subdivision. See `gpu-waves-plan.md` Wave B.

Steps 0–2 are landed and verified: the pooled path feeds the light field (it
previously never did — `voxel.GI.Enabled 1` was a silent no-op under
`voxel.Stream.GPU`), the volume is sampled per pixel, and the encode matches the
CPU sampler at **0.000 mean error**, with a deliberate half-cell-shifted control
at 0.565 proving the harness is not comparing a thing against itself.

| step | what it does | in-game effect |
|---|---|---|
| **3** | Decide Scheme A vs B from the measured horizontal error (6.165 bytes) | Whether side-facing surfaces get exact or averaged irradiance. Affects cave wall lighting quality. |
| **4** | Camera-following re-centring (the origin is static today) | At the shipped default only **36 of ~1,950** resident bricks fall inside the volume. Without this, GI simply is not there for most of the world. |
| **5** | Retire baked GI on the pooled path | **The actual prize.** Digging updates lighting immediately, without re-meshing the chunk. Removes a whole class of re-mesh work from edits. |

Also untested from step 2: the X-run upload merge on a dig's contiguous
neighbourhood (measured at 1.4 bricks/run in steady state, but the dig case is
what it was built for), and zero-on-revoxelize / zero-on-evict, which are correct
by construction only.

**Verdict: worth doing, and the most clearly valuable GPU work left.** It stands
regardless of where meshing runs, and it improves something a player sees (cave
lighting responding to digging).

---

## Wave C — Correctness gates (blockers, not features)

Nothing here is visible in normal play, but C1 blocks any GPU-generated voxel
state from being trusted.

**C1 — The Unreal leg of the cross-toolchain determinism gate is FAILING.**
Same HLSL, same GPU, same CPU reference: the standalone bench passes bit-exact
(`6e893ab3679a8c81`), while in-engine `voxel.GPU.VerifyRegion` fails
(`046b4a9f9c5e49b7`). The two toolchains disagree with each other — exactly the
fault class a two-leg gate exists to catch. Localised: columns match, quad decode
matches exactly, **cells differ on material ids only** and in a consistent
direction (the DXIL build's soil column sits one layer shallower).

> **CORRECTED — the floating-point contraction theory is impossible.**
> `worldgen.ush` contains **no floating-point arithmetic at all**; every operand
> from raster to material id is `int64_t`/`uint64_t`, which the file's own header
> states. There is no `<` for an FMA to flip by one ULP because there is no float
> to hold a ULP. And the proposed fix cannot be written: on UE 5.8's D3D12 path
> `CFLAG_NoFastMath` is never translated (`TranslateCompilerFlagD3D11` maps three
> flags and returns 0 for the rest), and there is no `-Gis` equivalent.
>
> The real compile deltas are **`cs_6_0` vs `cs_6_6`/`cs_6_8`** and **`cbuffer` vs
> loose `$Globals` binding**. A `$Globals` packing mismatch on `BrickZMin` would
> produce exactly this signature — columns match, cells shift vertically by one
> layer. This is the *second* wrong diagnosis on this one failure; both are kept
> visible. Hypotheses and the cheapest test: `gpu-waves-plan.md` Wave C.

**C2 — NVIDIA determinism leg.** Never run. The cross-vendor claim rests on one
vendor (AMD RX 7800 XT). ~$5 and ~20 minutes on a rented box. Cannot be
meaningfully run until C1 is green.

**C3 — Min-spec-proxy M1 gate re-run.** Still owed. Now worth doing, because for
the first time there may be a configuration that passes it.

**Verdict: C1 is a hard blocker for Wave D and should be fixed regardless** — a
red determinism gate is the project's most important invariant failing quietly.

---

## Wave D — GPU meshing in the streaming path — **QUESTIONABLE VALUE**

This is the never-built headline of ADR-0006 invariant 1: *"a compute pipeline …
replaces the CPU worker mesh + per-chunk vertex upload for streaming."*

**What it was designed to accomplish:** move chunk geometry generation off the CPU
entirely, freeing the ~24 saturated worker threads for physics, NPCs and water,
and removing meshing as a limit on view distance.

| piece | what it needs |
|---|---|
| **D1 — no-readback GPU→pool** | UAV on the pool's quad buffer, `ConvertToExternalBuffer` to bring it into RDG, and a **gated** `MeshEmit` variant baking the brick→chunk rebase and a pool base offset so the shader writes straight into the chunk's allocated range. Must be gated: editing the emitted bytes in place changes the digest. |
| **D2 — coarse-level generation** | `worldgen.ush` has **no level parameter**. R1–R5 have no GPU path at all, and they are ~80% of resident chunks. Adding it means mirroring new generation logic bit-exactly in HLSL — the most expensive and determinism-risky work in the programme. |
| **D3 — edited chunks** | Stay on the CPU. `NeedsOverlayAwarePath` already routes them to a game-thread path; the GPU never sees them. A carve-out, not a blocker. |

**Why the value is questionable now, on evidence rather than opinion:**

1. **Meshing throughput is not the current bottleneck.** Making meshing 39%
   cheaper did **not** improve the reported ring-gap symptom — the near rings
   absorbed the slack.
2. **It accelerates the cheapest 20%.** GPU support is level-0 only; coarse rings
   are the majority of the work and have no path.
3. **Blocked on C1.** GPU-generated cells currently disagree with the CPU
   in-engine.
4. The current implementation measures **188 chunks/s against the CPU's 765–944
   single-threaded** across ~24 workers — but note that figure measures "GPU **plus
   a 768 KB readback plus a once-per-tick poll**", which D1 removes by
   construction. **It is not the GPU's ceiling and should not be used to condemn
   the approach.**

**What would change the verdict:** the recorded **R0 = 128 m** goal, which
quadruples level-0 work and which `gpu-g0-sizing.md` says "cannot ship on the CPU
path". That is the regime where CPU meshing stops being adequate. Also: sustained
CPU core pressure from physics/NPCs competing with 24 meshing workers.

**Verdict: do not build now. Revisit when R0 = 128 m is on the table.**

---

## Wave E — Remaining G4 parity items

**E1 — Per-chunk debug tints.** The last G4 item, and the **only** one that still
needs `M_VoxelTerrain.uasset` edited. Storage is solved (`ChunkParams.w` is free);
the route is a `float4` `TexCoords` interpolant with the tint packed in `.zw`.
**The decision that makes it safe: encode identity as ZERO, not white** — the
component path supplies only a `float2`, so `TexCoord0.zw` arrives as zero there
regardless of the graph, and a naive unpack treating 0 as black would render
every component-path chunk black the moment the material is regenerated.
*In-game effect: debug-only. Invisible in normal play.* Low priority.

**E2 — Water pool.** Landed, `voxel.Water.GPU 0`. Renders correctly (2,231 cavern
bricks / 28,862 quads in one primitive). **Its parity number is honestly missing**
— same-path repeat runs differed 20–88%, so the comparison said nothing. Needs a
scene whose only variable is the water. Also note: one primitive means one
translucent sort key for all water; harmless for the current constant-opacity
material, but W5 fill-fraction shading, foam, caustics or refraction would each
break it.

**E3 — Ring cross-fade: CLOSED. Do not build.** Re-tested after the seam fix gave
the annuli their overlap band; it still produces see-through patches at ring
boundaries. The G0 checklist listed it *first*; it is the one item on that list
that should not be built. Spending the material-graph change on it would be
porting a defect.

---

## Wave F — Adjacent streaming items

**R0 to 128 m** — now unblocked (`RingPresets` became a runtime accessor,
overridable via `-VoxelRingInnerMeters=` / `-VoxelRingOuterMeters=`). Moving it is
still an open call, and the `+9.2%` resident-chunk cost of the seam-padding fix is
what to weigh it against. *This is also the decision that would revive Wave D.*

**`voxel.Stream.AdmissionBandSkip` — off, and should stay off.** It reaches ~4% of
the waste it was aimed at (89 skips against 2,186), its edit veto differs from the
dispatch site's in a way argued but never measured, and frame rate collapsed with
it on for unexplained reasons.

**The reported ring gaps while flying — NO ROOT CAUSE.** Not reproducible in the
20 m/s scripted flight on current main; coarse rings measure 110–142% of at-rest
residency (over-resident from eviction lag, not starved), and raising the ring
slot floors changes residency by +1%. See `manual-verification-checklist.md` item
1 — the question that splits the space is whether a gap fills in after you stop
moving.

---

## Suggested order

1. **C1** — the determinism gate is red; that is the project's core invariant.
2. **The ring gaps** — the actual reported symptom, still unexplained.
3. **Wave A** — makes the pooled renderer a win instead of a regression.
4. **Wave B** — the clearest remaining value, and player-visible.
5. **E1/E2** — small, low priority.
6. **Wave D** — only if R0 = 128 m goes ahead.

# ADR-0006: GPU-resident voxel streaming (display geometry off the render-thread apply funnel)

- **Status:** accepted
- **Date:** 2026-07-24
- **Doctrine sections affected:** additive. Implements ADR-0001's GPU-compute
  posture at *runtime* (not just CI), and **clarifies** the §2 determinism
  boundary (plan §2 item 3): the bit-deterministic set is voxel *state*
  (amplifier → voxelization → water CA), **not** display geometry. Needs
  human sign-off because it interprets that boundary and redefines the M1 gate.
- **Human sign-off:** **GIVEN — Matt Noles, 2026-07-24 (in session).** Covers
  the §2 determinism-boundary interpretation (invariant 3: display geometry is
  not required to be cross-vendor bit-exact), invariant 3's fourth bullet
  ("no gameplay system may read display geometry", added the same day), and the
  M1 gate redefinition at G3.
- **Empirical basis (added 2026-07-24):** this ADR originally *asserted* the
  per-chunk apply funnel was the ceiling. It is now measured — see
  `docs/status.md`, sections "M1 gate re-run", "Terrain sun shadows are NOT the
  render cost", and "`renderMs` is NOT pixel work". Summary: a same-binary A/B
  showed the streaming throttles buy 2.84x fill for 2.58x frame time (the trade
  this ADR removes); `renderMs` is the frame (43.12 of 43.92 ms) while voxel
  game-thread work is 0.26 ms; and both cheap alternative explanations for
  `renderMs` are eliminated — terrain shadow-casting costs nothing measurable,
  and quartering the pixel count via `r.ScreenPercentage 50` changes nothing.
  What remains in `renderMs` is per-primitive culling, draw-command generation
  and `FScene` mutation across ~24,700 records: exactly what invariants 1 and 2
  collapse to O(1).

## Context

Profiling the "streaming is too slow / holes while moving" reports (2026-07-24)
located the bottleneck precisely, and it is **not compute**:

- Chunk worldgen + greedy meshing already run 24-wide on CPU worker threads and
  produce a movement delta in a fraction of a second. Meshing is not the limit.
- The limit is the **per-chunk render-thread apply funnel**: each finished chunk
  is a `UVoxelChunkComponent` whose mesh is uploaded CPU→GPU and registered with
  `FScene::AddPrimitive` (and `RemovePrimitive` on unload). That render-thread
  scene mutation is expensive enough that `voxel.Stream.MaxAppliesPerFrame` had
  to be pinned at **3/frame** to hold the M1 zero-hitch gate. At 3/frame the
  cascade fills in minutes and every LOD upgrade lags 1–2 min.

A same-day CPU pass (adaptive time-budgeted applies + load-before-unload
retention) lifts fill to ~tens of seconds and bridges the moving-holes, but it
is working *around* an architecture whose ceiling is "one scene primitive per
chunk, mutated on the render thread." The stated target — **blindingly fast,
silky, high quality, very far view distance at 10 cm voxels** — is not reachable
by tuning that funnel; it needs the geometry to live on the GPU and be drawn
without per-chunk scene mutation.

What already exists to build on:

- **ADR-0001**: GPU compute is integer HLSL, dual-hosted (UE RDG + a Vulkan
  bench harness), no CUDA / no wave-size assumptions. The AMD desktop is the
  canonical determinism leg.
- **Worldgen voxelize kernel** (`voxel-core/shaders/worldgen.ush`): done and
  AMD-verified (git: "Merge GPU voxelize kernel, AMD PASS"), CPU↔GPU bit-exact.
  Currently wired only into CI/bench, not live streaming.
- **`docs/gpu-mesher-design.md`**: the deterministic GPU greedy-mesher kernel
  (count → scan → emit, one thread per face-mask) is fully speced but unbuilt.

The missing piece is the **runtime streaming/rendering integration** — turning
GPU-generated geometry into on-screen chunks without the funnel. That is what
this ADR decides.

## Decision

State these as invariants the implementation is checked against.

1. **Voxel-cascade display geometry is GPU-generated and GPU-resident.** A
   compute pipeline — voxelize (`worldgen.ush`) → greedy mesh
   (`gpu-mesher-design.md`) → into a **persistent GPU geometry pool** (a large
   suballocated vertex/index buffer, ring-managed) — replaces the CPU worker
   mesh + per-chunk vertex upload for streaming. Streaming a chunk in or out is a
   **pool region (de)allocation + a draw-argument update**, never an
   `FScene::AddPrimitive`/`RemovePrimitive` per chunk.

2. **Chunks are drawn via a small fixed set of persistent primitives using
   GPU-driven / indirect draws.** The scene sees O(1) primitives for the whole
   cascade (e.g. one per active level, or one global), not one per chunk. Per-
   chunk visibility/LOD is resolved on the GPU (indirect draw args / culling
   compute), so the render thread does no per-chunk work as the player moves.

3. **The runtime mesh is DISPLAY-ONLY and is explicitly NOT required to be
   cross-vendor bit-exact.** Authority — voxel materials (tiles + edit-log),
   collision/raycast, dig/place, water CA, and all replication — stays entirely
   on the CPU integer-deterministic `materialAt` path. Two clients agree because
   they share voxel **state**, never vertex buffers; no client compares meshes.
   Therefore:
   - The §2 determinism gate is **unchanged**: it still validates the CPU
     sampler and the deterministic bench (columns → voxelize → mesh digest
     parity). That digest parity is retained as **kernel-correctness insurance**
     (how we trust the GPU kernels), *not* a per-frame shipping constraint.
   - Meshing is display, downstream of the bit-deterministic state set the plan
     enumerates (amplifier, voxelization, water CA). Classifying it display-only
     is consistent with that enumeration, but because it *interprets* the
     boundary it is called out here for explicit sign-off.
   - **No gameplay system may read display geometry.** Collision, raycast,
     dig/place, water and any future interaction query resolve against
     `materialAt`, never against the pool, the draw args, or anything derived
     from them. This is the condition the whole carve-out rests on, and it was
     unstated in the first draft: the safety argument is "clients agree because
     they share state, never vertex buffers", which holds only while nothing
     gameplay-facing consults a vertex buffer. **This world is networked**
     (Matt, 2026-07-24), so a client-side read of GPU geometry is not merely an
     inconsistency — it is a desync vector, and a silent one. Stated as a rule so
     it can be violated loudly rather than quietly.

4. **One authority path is preserved (§2 item 4).** Edits mutate the
   authoritative CPU voxel state exactly as today; affected chunks are re-meshed
   on the GPU and their pool regions updated. The GPU path is a **consumer** of
   edit-log-derived state, never a second authority. Collision and digging never
   read GPU geometry.

5. **Vendor-neutral per ADR-0001.** Integer HLSL, RDG in UE + Vulkan bench, DXC
   → DXIL/SPIR-V from one source. No CUDA, no vendor intrinsics, no wave-size
   assumptions.

6. **The CPU streaming path is kept behind a cvar as a fallback** until the GPU
   path meets or beats it on fill speed, frame time, and visual parity. The GPU
   path ships only when it wins on all three; no big-bang cutover.

## Consequences

**Easier / unlocked**
- Streaming a chunk becomes ~O(GPU buffer copy) with **no render-thread scene
  mutation** → the apply funnel disappears → dramatically more resident chunks,
  much longer view distance at full 10 cm res, no per-chunk hitch on movement.
- The CPU worker fleet is freed from meshing; those cores go to worldgen,
  physics, NPCs.
- Per-chunk `AddPrimitive`/`RemovePrimitive`, the component pool, and the
  `MaxAppliesPerFrame`/`MaxUnloadsPerFrame` cvars become **legacy** for the
  cascade (retained only for the fallback path).

**Harder / new work**
- A GPU geometry-pool allocator (suballocation, fragmentation handling,
  eviction) and a **custom UE mesh-drawing path** — either a
  `FPrimitiveSceneProxy` fed by GPU-resident buffers with indirect draws, or a
  GPUScene / Mesh Draw Command integration. This is the bulk of the risk.
- Re-establishing **shadow, GI, and material** interaction against pooled
  geometry (CastShadow / cave-veil / voxel-GI all currently key off the
  per-chunk component; PR #95's shadow flag, the M4 GI vertex-color path).
- **Edit latency** now includes a GPU round-trip for the re-mesh (mitigated:
  edits touch tiny regions; do them async and keep the CPU mesh for the single
  just-edited chunk if the round-trip is visible).
- **Debug tooling** (chunk-state tints, ring overlay, `voxel.Debug.ChunkStates`)
  needs GPU-side equivalents.
- GPU **memory budgeting** becomes a first-class streaming constraint (the pool
  size caps resident voxel geometry; view distance trades against VRAM).

**Must be revisited / when**
- **The M1 perf gate is redefined**: cost moves off the render thread onto the
  GPU, so the gate becomes GPU frame time + streaming throughput (chunks/s) +
  zero-hitch, not game-thread p95 of `DrainResults`. New numbers set at G3.
- The `view-distance vs VRAM vs hitch-budget` trade is deferred to the execution
  plan's G0 sizing study — Matt decides with real numbers, per his ADR call.
- If GPU-side greedy meshing ever profiles worse than expected, ADR-0001's
  32-bit-hash-under-version-bump escape hatch applies to worldgen; the mesher
  has no determinism-version surface because it is display-only (invariant 3).

**Risk posture**
- This is the largest architecture change since M0. It ships **phased behind a
  cvar with the CPU path intact** (invariant 6); each milestone is independently
  verifiable (bench digest parity, then visual A/B, then perf), so a stall at any
  stage leaves a working game on the CPU path.

See `docs/gpu-streaming-plan.md` for the milestone-by-milestone execution plan.

## Delivered (2026-07-25)

G0–G5 are complete and merged. `voxel.Stream.GPU` **defaults to true**; the
CPU component path is intact behind `voxel.Stream.GPU 0`, exactly as the risk
posture above requires.

The pooled cascade renders **9,822 chunks / 8,813,242 quads in ONE primitive and
ONE draw call**, and matches the component path ring for ring.

**The redefined M1 gate, measured.** 60 s scripted surface flight at 20 m/s, same
spawn, post-warmup, this cvar the only difference; two pairs, second run in
reverse order:

| | component | pool | |
|---|---|---|---|
| p50 frame | 17.50 ms | 17.16 ms | −1.9% |
| **p95 frame** | 40.50 ms | 23.05 ms | **−43.1%** |
| **worst frame** | 221.4 ms | 76.6 ms | **−65.4%** |
| **hitches (>33.3 ms)** | 204 | 8.5 | **−95.8%** |
| chunks/s | 608 | 818 | **+34.6%** |

This is the ADR's thesis measured directly. The empirical basis above records
that the streaming throttles buy 2.84x fill for 2.58x frame time — **that trade
is now gone**: the pool delivers *more* fill (+34.6%) at *fewer* hitches (−95.8%)
simultaneously. The median frame is unchanged, which is correct — removing a
per-chunk `FScene::AddPrimitive` was never going to move the median.

**These are not yet an official gate row.** They are not min-spec-proxy protocol,
and all four legs ran on a contended machine. The direction is unanimous across
all four runs; the controlled re-run is still owed, and is now worth doing
because for the first time there is a configuration that might pass.

**What invariant 6 turned out to be worth.** Keeping the CPU path was written
down as a risk-posture concession. It was not: `voxel.Stream.GPUMaxLevel` and
`GPUMaxChunks` — which put both renderers in one frame and bisect between them —
were the two tools that located every hard bug in G3 and G4, including proving
that the R0 freeze was a pre-existing CPU-path bug the pool merely exposed. The
component path is therefore **retained after G5, not retired**, and
`docs/gpu-g4-parity-plan.md` records what still depends on it.

**One correction to the plan this ADR pointed at.** The G4 checklist held that
every remaining parity item required editing `M_VoxelTerrain.uasset`. That was
wrong for the two that mattered: the pooled vertex factory owns both ends of the
vertex-colour pipe, so anything expressible as a vertex colour channel is already
an interpolant the material graph reads. The asset has not been modified, and
nothing on the critical path required it. Only the debug tints still do.

### CORRECTION to the table above (2026-07-25, same day)

**The frame-time row of the "Delivered" table is retracted.** Twelve 60 s legs
of the scripted flight, taken across contended and idle machines, show the
component and pooled ranges overlapping almost completely, with the component
path holding the better *median* on p95 and hitches. Two identical pooled legs on
an idle box gave p50 21.0 and 47.4 ms — a 77% spread, larger than the effect
being measured. The four legs that produced the table were a coin landing the
same way four times.

Full write-up in `docs/streaming-handoff.md`, "CORRECTION: the G5 frame-time
numbers were noise".

This does **not** disturb the ADR's own empirical basis, which is a different and
earlier measurement: `renderMs` is 43.12 of a 43.92 ms frame while voxel work is
a rounding error, and a same-binary A/B showed the streaming throttles buying
2.84x fill for 2.58x frame time. That argument stands. What tonight failed to do
is confirm it end-to-end on this hardware with this harness.

What *is* verified, and is structural rather than statistical: the pooled path
draws 9,822 chunks / 8,813,242 quads as **one primitive and one draw call**
against 9,822 primitives, on every run, independent of the machine. That is the
mechanism the ADR predicted. Whether it converts into frame time here is
unmeasured, and the way to settle it is to measure the mechanism — primitive
count, draw calls, render-thread time in `FScene::AddPrimitive` — rather than
wall-clock percentiles over a flight whose run-to-run spread swamps the effect.

**Method note worth carrying into the next ADR.** The cheap control was two
consecutive identical runs. It was not taken until after the numbers had been
written into a commit message, a PR, this file and a cvar comment. The screenshot
work in the same session did quote every delta against a measured same-path noise
floor and was correct as a result; the timing work did not and was not.

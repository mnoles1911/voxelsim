# ADR-0006: GPU-resident voxel streaming (display geometry off the render-thread apply funnel)

- **Status:** proposed
- **Date:** 2026-07-24
- **Doctrine sections affected:** additive. Implements ADR-0001's GPU-compute
  posture at *runtime* (not just CI), and **clarifies** the §2 determinism
  boundary (plan §2 item 3): the bit-deterministic set is voxel *state*
  (amplifier → voxelization → water CA), **not** display geometry. Needs
  human sign-off because it interprets that boundary and redefines the M1 gate.
- **Human sign-off:** pending — Matt Noles

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
- **Worldgen voxelize kernel** (`voxel-core/shaders/worldgen.hlsl`): done and
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
   compute pipeline — voxelize (`worldgen.hlsl`) → greedy mesh
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

# GPU-resident voxel streaming — execution plan (ADR-0006)

Phased delivery of the GPU streaming path decided in ADR-0006. **The CPU path
stays the shipping path until G5 flips the cvar.** Every milestone is
independently verifiable and leaves a working game if the next stalls.

Vendor-neutral throughout (ADR-0001): integer HLSL, RDG in UE + Vulkan bench,
AMD desktop is the canonical leg. "Digest parity" = CPU reference vs GPU output,
byte-identical, in the `voxel-core/bench` harness.

---

## G0 — Sizing study + kernel wiring proof *(no rendering yet; decision gate)*

**Goal:** put real numbers in front of Matt for the view-distance ⇄ VRAM ⇄
throughput trade (ADR-0006 says he decides here), and prove the two existing
kernels chain end-to-end at runtime resolution.

- Wire `worldgen.hlsl` voxelize into an RDG pass inside UE (not just the bench),
  producing a chunk's cell buffer on the GPU. Reuse the AMD-verified kernel.
- Measure: voxelize + (CPU-side, for now) mesh throughput at 10 cm for one
  chunk, per ring level; VRAM per chunk of packed geometry (2×uint32/quad per
  `gpu-mesher-design.md`); quads/chunk distribution from live tiles.
- **Deliverable:** a one-page table — for candidate view distances (2 km, 4 km,
  8 km…) the resident chunk count, packed-geometry VRAM, and generate-throughput,
  with a recommended target. **Matt picks the target here.**
- **Evaluate engine-native GPU-driven paths BEFORE committing to a hand-rolled
  one** (added 2026-07-24). G2 below proposes a private geometry pool plus a
  custom `FPrimitiveSceneProxy` with indirect draws — that is where all of this
  plan's schedule risk is concentrated, and it is close to re-implementing
  machinery UE already ships. Spend part of G0 answering: can GPUScene /
  Mesh Draw Commands be fed GPU-resident buffers directly? Does Nanite apply to
  the outer rings, where geometry is effectively static between digs (note the
  sibling Mira-Thal project already runs a working Nanite bake path over 10 cm
  voxel terrain)? Building a private version of an engine feature is an
  expensive way to discover the engine had one. **Deliverable:** a paragraph per
  option — viable / not viable and why.
- **Gate:** numbers produced; no code on the hot path yet. Cheap to abort.

## G1 — GPU greedy mesher kernel (`gpu-mesher-design.md`) + digest parity ✅ COMPLETE

**Status: DONE (kernels landed 2026-07-21; gate re-verified 2026-07-25).**

- `MeshCountMain` → scan → `MeshEmitMain` implemented exactly as speced, one
  thread per face-mask. The scan went **straight to the GPU**
  (`ScanBlocksMain`/`ScanSumsMain`/`ScanAddMain`) — the "CPU scan for v1"
  fallback was never needed.
- **Gate: GREEN.** `vxc_gpu.exe --radius 64` compares 3,058,001 of 3,058,001
  quads over 319/319 tiles, byte-identical to the CPU mesher. Combined digest
  (columns + cells + quads) `591c7602bb9b0e62`, seed 20260719, worldgen v6.
  End-to-end 0.147 s against the <1 s M0 target. AMD leg green.
- **NVIDIA CI leg is still owed** — the only unmet part of the original gate.

**Carried into G2:** the kernels exist only in the standalone Vulkan bench,
compiled by `dxc` outside the engine. They have never run inside Unreal. Porting
the HLSL to UE global shaders + RDG is the first task of G2, not a G1 remnant.

## G2a — The kernels, in-engine, through RDG *(prerequisite; added 2026-07-25)*

**Goal:** run the G1 chain inside Unreal and prove it still produces the same
bytes. Nothing renders. This is the bridge between a green standalone bench and
any of the rendering work below, and it is the cheapest place to discover that
engine-compiled HLSL diverges from `dxc`-compiled HLSL.

- Register a shader directory for the `VoxelEarth` module; get `worldgen.hlsl`
  compiling as UE global compute shaders (D3D12/SM6) with whatever `#ifdef`
  work the Vulkan-specific syntax needs. **The HLSL must stay one source of
  truth** — the bench and the engine compile the same file, or the digest gate
  stops meaning anything.
- Dispatch `ColumnMain` → `VoxelizeMain` → `MeshCount` → scan → `MeshEmit` as
  RDG passes for a single chunk; read the quad buffer back.
- **Gate:** a console command (`voxel.GPU.VerifyChunk`) meshes the chunk under
  the player on both paths and logs a byte-comparison PASS/FAIL plus the quad
  digest. Must match the CPU mesher exactly.
- **Why this is its own milestone:** it is verifiable *headlessly* — no PIE
  flying, no visual judgement, no designer time. If engine-compiled output
  diverges, we find out here, against a known-good reference, instead of inside
  a half-built renderer where a wrong quad looks like a pool-allocator bug.

## G2 — Persistent GPU geometry pool + custom draw (one static chunk on screen)

**Goal:** the hard part — draw GPU-resident geometry with **no per-chunk
`AddPrimitive`**. Prove it with a single hand-placed chunk before any streaming.

- Geometry pool: one large suballocated vertex/index buffer; a region allocator
  (alloc on chunk-in, free on chunk-out, fragmentation handling).
- Custom draw path: a `FPrimitiveSceneProxy` (or GPUScene/Mesh Draw Command
  integration) that issues **indirect draws** over the pool. Start with ONE
  primitive drawing ONE chunk's region.
- **Gate:** a single chunk renders identically to the CPU-meshed component
  (visual A/B, same material). Toggling it in/out touches only draw args +
  pool, not `FScene` primitives (verified in a render-thread trace).

## G3 — GPU-driven streaming of the full cascade + redefined perf gate

**Goal:** replace the CPU stream path for the cascade behind a cvar
(`voxel.Stream.GPU 1`), still authoritative-state-driven by the CPU desired-set.

- Desired-set / ring logic (CPU, unchanged) drives pool allocations instead of
  component applies. Per-chunk visibility + LOD selection resolved on the GPU
  (indirect draw args / culling compute) so movement does no per-chunk render-
  thread work.
- Port ring skirts / load-before-unload semantics into the pool lifecycle
  (retain a region until its replacement is resident — cheaper here: it's a
  draw-arg flip, not a component).
- **Gate (new M1 per ADR-0006):** GPU frame time under budget, streaming
  throughput ≥ CPU path, **0 hitches**, at the G0 target view distance. Fill
  time and moving-holes measured against the CPU baseline from today's pass.

## G4 — Edits, shadows, GI, water, debug against pooled geometry

**Goal:** feature parity so the GPU path is a true drop-in.

- Edits: mutate CPU state (unchanged authority), re-mesh the affected chunk on
  GPU, update its pool region async. If the round-trip is visible, keep the CPU
  mesh for the single just-edited chunk as a one-frame bridge.
- Shadows (PR #95 CastShadow), cave veil, and M4 voxel-GI vertex colors
  re-expressed against pooled geometry.
- Water surface + debug tints/overlays (`voxel.Debug.ChunkStates`, ring tint)
  get GPU-side equivalents.
- **Gate:** dig/place, water drain/refill, shadows, GI, and debug overlays all
  visually match the CPU path (A/B per feature).

## G5 — Flip default + retire the funnel

**Goal:** GPU path becomes the shipping default once it wins on all three:
fill speed, frame time, visual parity.

- `voxel.Stream.GPU` defaults on; CPU path stays behind the cvar as fallback for
  one milestone, then the per-chunk component/apply machinery is marked legacy.
- Update `docs/status.md` M1 gate row to the G3 definition; update memory.

---

## Non-goals (this plan)
- The distant heightfield **clipmap** (ADR-0002) is untouched — it already uses
  GPU sampling and sits beyond the voxel cascade.
- No cross-vendor bit-exactness for display geometry (ADR-0006 invariant 3); the
  only digest gate is the G1 kernel-correctness one.
- No change to the authority/edit-log path, collision, or replication.

## Fallback / abort safety
Each gate is a clean stopping point on the CPU path: G0 aborts free; G1 leaves a
tested kernel unused; G2–G4 live behind `voxel.Stream.GPU 0`. The game is always
shippable on the CPU streaming path (today's adaptive-apply + load-before-unload
work), which is why that work was worth doing even with this plan pending.

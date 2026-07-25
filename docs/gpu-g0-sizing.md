# G0 sizing study — view distance vs VRAM vs throughput

**Milestone:** ADR-0006 G0 (`docs/gpu-streaming-plan.md`). This is the decision
table. **Matt picks the target view distance from these numbers.**

**Measured 2026-07-24** on this box: AMD Radeon RX 7800 XT, seed 20260719,
worldgen v6. GPU numbers from `build/voxel-core-msvc/bench/vxc_gpu.exe --radius N`
(the M0 gate harness: ColumnMain → VoxelizeMain → MeshCount → GPU scan →
MeshEmit, one command buffer per flight of 8 tiles). CPU comparison numbers from
`-VoxelPerfRun` in the live game.

---

## 1. Generation throughput — GPU vs the CPU worker fleet

| radius | GPU gate time | interior bricks | → 32³ chunks | **chunks/sec** |
|---|---|---|---|---|
| 64 m | 138.4 ms | 307,720 | 4,808 | **34,700** |
| 128 m | 182.9 ms | 1,077,412 | 16,834 | **92,000** |

(÷64 bricks per 32³ render chunk. Gate time excludes the CPU reference pass and
the verification compare, which exist only for the digest gate.)

**Live CPU path, measured in-engine the same day: 740–968 chunks/sec.**

> **The GPU generation path is 36–95× the CPU worker fleet.**

Throughput *improves* with radius (34.7k → 92k chunks/s) because per-flight
setup amortises: at 64 m the run is short enough that buffer growth and
submission overhead still dominate. 92k chunks/s is the better estimate of
steady state.

For scale: the entire live 2 km cascade is 10,503 chunks. At 92k chunks/sec that
is **~0.11 s to generate the whole world from cold.** The CPU path takes ~11–14 s
for the same set, which is what the 10–20 s catch-up on fast flight actually is.

## 2. VRAM — and why view distance is nearly free

Packed geometry is **8 bytes per quad** (2 × uint32; `docs/gpu-mesher-design.md`,
confirmed in `gpu_harness.cpp` — the GPU quad stream is `uint64_t` per quad).

Live cascade, read off the in-game HUD at the 2 km configuration:

```
components 10503   quads 9,441,170   →  75.5 MB packed
```

Per-ring resident counts, same reading:

| ring | R0 | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|
| chunks | 2,035 | 1,405 | 1,712 | 1,988 | 1,796 | 1,567 |

**The counts are flat, and that is the whole story.** Each ring doubles its
radius while its chunk edge also doubles, so every annulus holds roughly the same
number of chunks (~1,400–2,000) regardless of how far out it sits. Doubling view
distance therefore costs **one more ring**, not four times the geometry:

| view distance | rings | resident chunks | **packed VRAM** |
|---|---|---|---|
| 2 km (today) | 6 | 10,503 | **75.5 MB** |
| 4 km | 7 | ~12,100 | **~87 MB** |
| 8 km | 8 | ~13,700 | **~99 MB** |
| 16 km | 9 | ~15,300 | **~110 MB** |

**VRAM is not the constraint.** On a card with 16 GB, an 8 km cascade of
GPU-resident voxel geometry is well under 1% of it. The ADR anticipated VRAM
being the thing that caps view distance; on these numbers it is not close.

## 3. What that reframes

The real cost driver is **not** total view distance — it is the **R0 radius**,
i.e. how far out you want true 10 cm voxels. R0 is the only ring whose count
grows with its own radius at full resolution, and it is where 44.6% of CPU worker
time currently goes.

So the question to answer is not "2, 4 or 8 km?" — take 8 km, it costs ~24 MB
more than today. The question is **"how far out do you want 10 cm?"**

## 4. Per-level chunk cost (CPU reference, for the outer rings)

`vxc_bench --mips --reps 3`, one 32³ chunk at the surface, min of 3:

| level | chunk edge | fine path | **coarse path** | quads/chunk |
|---|---|---|---|---|
| 0 | 3.2 m | 3.1 ms | 3.6 ms | 1,532 |
| 1 | 6.4 m | 13.1 ms | **2.6 ms** | ~1,500 |
| 2 | 12.8 m | 74.8 ms | **2.9 ms** | ~640 |
| 3 | 25.6 m | 617.3 ms | **3.0 ms** | ~1,300 |
| 4 | 51.2 m | 4,119.8 ms | **2.9 ms** | ~2,350 |

The coarse path is flat at ~3 ms/chunk at every level — which is why the outer
rings are affordable at all today. Quads per chunk stay in the 600–2,400 band
across every level, so the VRAM extrapolation above holds per-ring.

## 5. Caveats on these numbers

- **`--radius N` is not a cascade.** It generates 10 cm everywhere out to N
  metres, so §1 is an upper bound on work and a *lower* bound on chunks/sec for a
  real cascade (whose outer rings are far cheaper per unit area).
- **GPU path is level-0 only.** `worldgen.ush` has no level parameter; coarse
  generation is CPU-only today. Outer-ring GPU throughput is not measured and is
  not required for G0 — but G1/G3 will need it, or the outer rings stay on CPU
  (which §4 shows is cheap).
- **One-shot timing.** The harness has no `--reps`; at small radii first-touch
  allocation is a large share (127 buffer reallocations, front-loaded). The 128 m
  figure is the more trustworthy one.
- **Digest moved and that is expected.** Radius 64 now prints
  `591c7602bb9b0e62`, against `e1db29a9b6874012` recorded in `docs/status.md` on
  2026-07-20. The bench reports **worldgen v6**; the goldens predate that bump.
  Not a determinism regression, but the status.md determinism row should be
  re-baselined against v6 so the next person does not read it as one.
- **VRAM figure is geometry only.** It excludes the index/draw-arg buffers, pool
  fragmentation headroom, and any double-buffering the pool allocator needs.
  Budget ~1.5–2× the raw figure in practice — still trivial at these scales.

## 5b. R0 = 128 m — Matt's call, and why it must wait for G1

**Decision (Matt, 2026-07-24): target R0 = 128 m**, i.e. true 10 cm voxels out to
128 m instead of today's 64 m. Recorded here as the G3 target.

**It cannot ship on the CPU path, and the arithmetic is not close.** R0 is the one
ring whose chunk count scales with the square of its own radius at full
resolution, because its chunk edge stays at 3.2 m:

| | R0 radius | R0 chunks (10 cm) | cascade total |
|---|---|---|---|
| today | 64 m | 2,035 | 10,503 |
| target | 128 m | **~8,100** | **~16,600** |

Four times the level-0 work. At the current measured 703 chunks/sec, filling R0
alone goes from ~2.9 s to **~11.5 s**, and that is the near field — the terrain
directly under the player, the part whose absence reads as holes. Shipping this
before generation moves to the GPU would take the exact symptom being chased and
multiply it by four.

Against the G0 GPU throughput of ~92,000 chunks/sec, ~8,100 R0 chunks is **~0.09 s**.

So R0 = 128 m is not a tuning change, it is a **reason to finish G1/G3**. It is
the concrete payoff: the thing that is unaffordable today and nearly free after.

Sequence: G1 (GPU mesher + digest parity) → G3 (GPU-driven cascade streaming) →
flip R0 to 128 m and re-measure. Not before.

The ring-preset table (`VoxelWorldSubsystem.h:86`) is `static constexpr`, so this
also needs `RingPresets` to become a runtime accessor before it can be A/B'd on
one binary — worth doing as part of G3 rather than as a standalone change now.

## 6. Recommendation into G1/G2

1. **Take 8 km.** ~99 MB packed. VRAM is not the trade the ADR expected it to be.
2. **Spend the freed budget on R0 radius instead**, which is the number that
   actually governs how the world looks at 10 cm and where the CPU is drowning.
3. **G2 must compact into ONE indirect draw**, not one per chunk —
   `FMeshDrawCommand::SubmitDrawIndirectEnd` issues exactly one indirect draw per
   command, and `RHIMultiDrawIndexedPrimitiveIndirect` is not wired into the mesh
   pass system. This determines the pool layout and the mesher's output format
   and is expensive to discover later. (From the G0 engine-path evaluation.)
4. **Build on the Landscape pattern, do not hand-roll a renderer.** Static
   relevance + `EnableGPUSceneSupportFlags` + `bViewDependentArguments` +
   `ApplyViewDependentMeshArguments`, with culling in a `FSceneViewExtension` in
   the game module. That is a shipping precedent for ADR-0006 invariant 2 with no
   engine fork. What is genuinely left to write: the pool suballocator, the GPU
   mesher (G1), and one vertex factory doing manual vertex fetch from the pool.
5. **Nanite is ruled out.** `NaniteBuilder` is an editor-only module; clusters
   cannot be built at runtime in 5.8, so "static between digs" does not rescue it
   for runtime-generated terrain.

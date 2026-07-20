# Milestone gate status

Updated whenever gate-relevant work lands. See the implementation plan §4 for
gate definitions.

## M0 — Core + proof of numbers (IN PROGRESS)

| Gate | Status | Notes |
|---|---|---|
| Amplify+mesh 128m radius < 1s on RTX 3060 | 🟨 half-open | **AMD leg measured (2026-07-19)**: `vxc_gpu --radius <m>` (voxel-core/bench/gpu_harness.cpp) now runs the FULL GPU pipeline (ColumnMain→VoxelizeMain→MeshCount/EmitMain) over every surface-shell brick in a horizontal radius, tiled into 128×128-column dispatches with a 1-brick shared halo (14×14-brick/112-column interior per tile — partitions the target square with no gaps, no double-meshing). On this desktop's AMD Radeon RX 7800 XT: **radius 64m = 0.678s** (PASS, under target) and **radius 128m = 2.388s** (OVER target) — end-to-end gate time excludes per-tile CPU column setup and CPU-reference comparison, both timed and reported separately. At 128m the host prefix-scan step (CPU reads GPU-mapped mesh-mask counts, writes back scan offsets, between the count and emit dispatches) is the single largest bucket (1.20s of 2.39s) — larger than all four GPU dispatch stages combined (1.07s) — pointing at CPU↔GPU-mapped-memory round-trip cost in the count→scan→emit chain, not raw compute throughput, as the next optimization target (gpu-mesher-design.md's mesher lists moving the scan to GPU as exactly this contingency). **Host scan replaced with a GPU scan (2026-07-20)**: worldgen.hlsl gained `ScanBlocksMain`/`ScanSumsMain`/`ScanAddMain` (fixed-order shared-memory Hillis-Steele, per-256-block scan + single-workgroup block-sum scan + add-back — deterministic by construction, so quad order is byte-for-byte unchanged), and `gpu_harness.cpp`'s `runMeshChain()` now chains MeshCountMain→ScanBlocksMain→ScanSumsMain→ScanAddMain→MeshEmitMain in ONE command buffer (COMPUTE→COMPUTE buffer barriers write→read between every stage, one trailing COMPUTE→HOST barrier, ONE fence per tile/region) instead of the old two-submission count/emit split with a CPU-side scan in between. The emitted quads buffer is upper-bound sized (32 quads/mask max × maskCount, grow-only reused) *before* the chain is recorded, since MeshEmitMain now has no readback point to learn the exact total first; the true total (`counts[maskCount-1] + offsets[maskCount-1]`) is read back after the single fence. Per-stage GPU timing survives the merge to one fence via 6 `vkCmdWriteTimestamp` queries bracketing the 5 dispatches. maskCount is asserted ≤65,536 per dispatch (ScanSumsMain's single-workgroup limit) — never triggered by the 128×128 tile layout in practice. Result: **radius 64m = 0.529s** (PASS, was 0.678s) and **radius 128m = 1.134s** (still OVER target, was 2.388s — a 52.5% reduction). The host scan bucket (was 1.20s at 128m) is now a 6.98ms GPU-scan bucket (blocks 3.66ms + sums 2.22ms + add 1.10ms) — the scan is no longer a bottleneck at any radius tested. The four original GPU dispatch stages (columns+voxelize+meshcount+meshemit) are ~925ms at 128m, about the same as before (1.07s); marshalling overhead rose modestly (118ms→201ms — three descriptor sets now rewritten per tile instead of ≤2, plus the upper-bound quads allocation) — mesh count/emit dispatch cost (283ms+286ms=569ms) and columns/voxelize (224ms+132ms=356ms) are now the dominant buckets and the next optimization target, not the scan. Digests at both radii are byte-identical to the pre-scan run (see the determinism row below), confirming the GPU scan changes nothing about output, only how it's computed. NVIDIA leg still needs to run the same `vxc_gpu --radius 64/128` and compare digests. CPU reference baseline (single-threaded, container hardware) unchanged below. |
| Bit-identical amplifier output NVIDIA vs AMD | 🟨 half-open | **AMD leg PASSING** (2026-07-19): `vxc_gpu` (voxel-core/bench/gpu_harness.cpp, ADR-0001) dispatches the SPIR-V worldgen kernel (build/shaders/worldgen.ColumnMain.spv) on this desktop's AMD Radeon RX 7800 XT via a headless Vulkan 1.1 harness and byte-compares every field of every column against `vxc::Amplifier::column` — bit-exact over 32,768 columns across two dispatch regions (near-origin and a far/negative-coordinate region), digest `be28ce960bd5bcf6`. **`--radius` gate mode PASSING too** (2026-07-19, seed 20260719): full columns+cells+quads comparison at radius 64m (144/144 tiles, 100% verified: 2,359,296 columns / 87,687,168 cells / 1,129,674 quads, digest `e1db29a9b6874012`) and a deterministic every-8th-tile sample at radius 128m (67/529 tiles, 12.7% verified: 1,097,728 columns / 39,583,744 cells / 497,132 quads, digest `583e91d62cefb8a9`) — zero mismatches in both, and the digest covers ALL GPU output (not just the verified sample) so it's comparable across differently-sampled runs. **Re-verified after the GPU-scan wiring (2026-07-20)**: same seed, same two digests exactly (`e1db29a9b6874012` at radius 64m, `583e91d62cefb8a9` at radius 128m) and the default column-only regions mode also stays bit-exact — confirms the GPU scan is a pure re-implementation of the exclusive-scan step with no observable output change. NVIDIA leg still open: needs a rented/CI Linux+NVIDIA runner producing the same `vxc_gpu` digests (ADR-0001 gate = identical digest on both legs, for both the column-only regions AND the `--radius` gate digests). Interim cross-*compiler* proxy (gcc/clang/MSVC digests) still green as a secondary signal; see `determinism-cross-compiler` in CI. |

### Early 8³ vs 16³ data (CPU ref, will re-decide after GPU port)

At 128m radius: 8³ = 129.7k surface-shell bricks, 36.5M solid voxels, 4.53M
quads, 10.6s total; 16³ = 32.5k bricks, 69.9M solid voxels (taller bricks
capture more buried volume), 4.35M quads, 14.9s (meshing 16³ slices costs
~2× despite 4× fewer bricks). CPU ref favors 8³ on generation cost; final
call needs GPU meshing + render/memory numbers (M1).

### M0 task checklist (plan §5)

- [x] Repo scaffold (monorepo layout, docs, CI)
- [x] Tile API: `GET /tile?seed&x&y&scale` → elevation int16 + climate uint8[4]; disk cache; golden-tile regression test (synthetic provider; real terrain-diffusion worker pending GPU machine)
- [x] Brick storage + palette + bitmask; property tests; 8³ vs 16³ benchmark
- [x] Amplifier v0: column stratigraphy + integer-hash fractal detail (fixed-point only, hash documented in docs/determinism.md). CPU reference first; GPU port pending
- [x] Greedy mesher (CPU ref); bricks/sec + 128m-radius wall-clock in bench
- [x] Edit overlay + append-only log format (versioned, RLE brick diffs) + replay test
- [ ] terrain-diffusion worker running (GPU machine)
- [x] GPU compute port of amplifier (worldgen.hlsl ColumnMain, Vulkan harness verified bit-exact on AMD leg); mesher GPU port (MeshCount/EmitMain) also landed and verified bit-exact
- [x] `vxc_gpu --radius <m>` M0 gate driver: tiled full pipeline (columns→voxelize→mesh) over every surface-shell brick in a radius; AMD leg measured, see gate row above
- [ ] Cross-vendor (NV vs AMD) determinism CI (AMD leg passing locally, both column-only and `--radius` gate digests recorded; NVIDIA leg needs a rented/CI runner)

## M1 — Walkable world in UE5 (IN PROGRESS, stages 1–2 verified on screen)

Working plan + binding decisions: docs/m1-plan.md. UE 5.8.0 (retargeted
2026-07-19), native editor MCP enabled.

- [x] Stage 1 — voxels on screen: custom scene proxy (FLocalVertexFactory),
  vertex-color AO material, verified by screenshot 2026-07-19
- [x] Stage 2 — streaming + dig/place: lock-free worker split, budgets,
  hysteresis, DDA raycast digs through the edit-log authority path
- [ ] Stage 3 — walkable + LWC: streaming perf (worker column caching —
  measured ~5 chunks/s, needs ~10×), DDA box-sweep character collision,
  origin rebasing, 60fps gate run
- Gate (walk & dig at 60fps min-spec): ⬜ open — blocked on stage 3

## M0 GPU track (ADR-0001)

- [x] Worldgen HLSL kernel (ColumnMain, VoxelizeMain, MeshCountMain,
  MeshEmitMain) mirrors CPU reference; compiles to DXIL + SPIR-V from one
  source (pinned DXC 1.9, tools/compile-shaders.ps1)
- [x] GPU exclusive scan (ScanBlocksMain, ScanSumsMain, ScanAddMain) replaces
  the CPU-mapped-memory host scan between mesh count and emit; chained with
  count/emit in one command buffer/one fence (`runMeshChain()`,
  gpu_harness.cpp) — see the M0 gate row above for before/after numbers
- [x] Vulkan headless harness: dispatch + byte-compare vs CPU reference on
  the AMD leg (this desktop) — both the column-only regions mode and the
  `--radius` full-pipeline gate mode. Still needs a cloud NVIDIA leg
  producing matching digests → closes both M0 gates

## Water track — W1 first slice landed (2026-07-19)

Implicit ocean (z<0, zero voxel data): `AVoxelOceanActor` (camera-following
40km `/Engine/BasicShapes/Plane`, `M_Ocean` via `Tools/create_ocean_material.py`),
underwater fog/post-process tint toggled by camera depth (log-verified
transition, no screenshot), swim-mode placeholder in `AVoxelEarthFlyPawn`
(gravity off, fly-style 300 UU/s below sea level). No pressure CA, reservoirs,
buoyancy, or currents yet -- those are W2-W4.

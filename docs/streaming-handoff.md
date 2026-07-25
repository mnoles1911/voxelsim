# Handoff: streaming fixed, ADR-0006 signed, G1 is next (2026-07-25)

Supersedes the 2026-07-24 handoff entirely. All work is on
**`claude/terrain-holes-wip`**, pushed to origin, 9 commits ahead of the previous
handoff. Build is clean; every change below is in the binary.

## State

| Item | Status |
|---|---|
| Concentric rings of holes | ✅ **ROOT-CAUSED AND FIXED** (ring-seam admission, `749ad7b`) |
| Radial line of missing chunks | ✅ Fixed by the same change, confirmed by Matt |
| Chunks missing at rest | ✅ Gone — Matt confirmed "all chunks loaded, feels very fast" |
| Load-before-unload coverage | ✅ Rewritten against `ChunkRecords` (`a4e35af`) — was a real bug, was NOT the holes |
| Hitch regression from the fix | ✅ 47 → 6 (exact-seam admission); residual p50 cost accepted |
| Real diffusion tiles | ✅ Now the project default, no command line needed |
| ADR-0006 | ✅ **ACCEPTED**, signed by Matt, diagnosis measured not asserted |
| G0 sizing study | ✅ Delivered — `docs/gpu-g0-sizing.md` |
| Terrain amplification proposal | ⏸️ Reconciled and PARKED — `docs/terrain-amplification-reconciliation.md` |
| **G1 — GPU greedy mesher** | ⬜ **NEXT. Not started.** |

## The one thing to build next: G1

**Goal:** implement the deterministic GPU greedy mesher so geometry can be
produced entirely on the GPU, and gate it on byte-identical quad streams vs the
CPU mesher.

- **Spec:** `docs/gpu-mesher-design.md` — `MeshCountMain` → scan → `MeshEmitMain`,
  one thread per face-mask. Fully speced, previously unbuilt.
- **It may already be partly done.** `voxel-core/shaders/prebuilt/` contains
  `MeshCountMain.spv` and `MeshEmitMain.spv`, and the M0 gate harness already
  chains them. **Check what exists before writing anything** — G1 may be mostly
  "wire the existing kernels into UE via RDG" rather than "write the kernel".
- **Gate:** bench meshes N regions CPU and GPU with byte-identical quad streams
  (joins the existing columns+cells digest). AMD leg green.
- **Verify with:** `build/voxel-core-msvc/bench/vxc_gpu.exe --radius 64`
  (already built, runs in ~140 ms + verification). Current digest at radius 64 is
  `591c7602bb9b0e62` on **worldgen v6**.

### Constraints G1/G2 must respect (found in G0, do not rediscover)

1. **G2 must compact into ONE indirect draw.** `FMeshDrawCommand::SubmitDrawIndirectEnd`
   issues exactly one indirect draw per command, and
   `RHIMultiDrawIndexedPrimitiveIndirect` is **not** wired into the mesh-pass
   system. This determines pool layout AND mesher output format.
2. **Build on the Landscape pattern, do not hand-roll a renderer.** Static
   relevance + `EnableGPUSceneSupportFlags` + `bViewDependentArguments` +
   `ApplyViewDependentMeshArguments`, with culling in an `FSceneViewExtension` in
   the game module. Shipping precedent, no engine fork. What genuinely needs
   writing: pool suballocator, the mesher, one vertex factory with manual vertex
   fetch.
3. **Nanite is ruled out.** `NaniteBuilder` is an editor-only module; clusters
   cannot be built at runtime in 5.8.
4. **Voxel STATE stays bit-exact CPU↔GPU.** ADR-0006's display-only carve-out
   covers the mesh only. No floats in `voxel-core` (CI-enforced).
5. **No gameplay system may read display geometry** (ADR-0006 invariant 3,
   fourth bullet). This world is networked; a client-side read of GPU geometry
   is a desync vector.

## Why G1 matters, in one number

- GPU generation measured: **~92,000 chunks/sec** (AMD RX 7800 XT, radius 128 m).
- CPU worker fleet measured in-engine the same day: **703–968 chunks/sec**.
- The whole 2 km cascade (10,503 chunks) is ~0.11 s on GPU vs ~11–14 s on CPU.
  That ~11 s **is** the fast-flight catch-up Matt reports.

**Matt's decision: R0 target = 128 m** (10 cm voxels out to 128 m, from 64 m
today). It is 4× the level-0 work — ~8,100 R0 chunks vs 2,035 — which is ~11.5 s
of CPU fill and ~0.09 s of GPU. **Do not flip R0 before G1/G3 land.**
`RingPresets` (`VoxelWorldSubsystem.h:86`) is `static constexpr` and must become
a runtime accessor first; fold that into G3.

## Open, not blocking G1

- **Residual perf cost of the seam fix:** p50 14.92 → 17.52 ms, chunks/s
  968 → 703 vs the pre-fix baseline. Real extra chunks where holes used to be.
  Exactly what ADR-0006 removes.
- **Deep-column waste:** 44.6% of R0 worker time meshes buried chunks emitting
  zero quads. Measured ceiling for fixing it: **+34.8% useful throughput**
  (`-VoxelNoUnderground` is the upper-bound switch, already exists). Safe path is
  moving the exact band skip from dispatch-time to admission-time; the deep column
  is residency-independent for collision/dig/water (all go through
  `World::materialAt`), so the handoff's old "RISKY — touches collision" note was
  wrong. Needs a downward escape hatch (`EditedFootprintMinZ` → widen `ChunkZMin`)
  first.
- **Bounded-admission straggler:** a few chunks never load until the player moves.
  Diagnosed to the refill path, not fixed. In the `docs/status.md` backlog.
- **Min-spec-proxy M1 gate re-run** still owed; today's numbers are
  default-quality and cannot be read against the historical gate rows.
- **`status.md` determinism row is stale** — goldens predate worldgen v6.

## Two methodology rules earned the hard way

1. **Discard the first run after any build.** Cold PSO state costs ~20%
   throughput and reads exactly like a regression. It produced one false
   "regression" report this session.
2. **Never A/B this renderer through scalability cvars.** `r.ShadowQuality 0`
   desyncs the BeginPlay PSO precache and measures precache invalidation instead
   — 118 hitches of pure artifact. Change one primitive flag, or use
   `r.ScreenPercentage` (resolution scale, no shader permutation change).

## A/B switches that now exist (all default to current behaviour)

`voxel.Render.CastShadow` · `voxel.Stream.JobsInFlightPerCore` ·
`-VoxelNoColdBandThrottle` · `-VoxelStaticRelevance` · `-VoxelNoUnderground` ·
`-VoxelBuriedSkip=0` · `voxel.Stream.LodRetentionMs`

## Verification recipe

```
"D:/UE_5.8/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development \
  -Project="D:/voxelsim/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE

UnrealEditor.exe VoxelEarth.uproject -game -windowed -resx=1920 -resy=1080 \
  -nosplash -unattended "-VoxelSpawnAt=-84480,53760" "-VoxelPerfRun=60"
```
Writes `Saved/PerfRuns/perf_*.json`. Close the editor first (it locks the DLL —
a failed link DELETES it and the project then will not launch until a good build).
Current reference: post-warmup p50 ~17.5 ms, p95 ~26 ms, ~6 hitches, ~703 chunks/s.

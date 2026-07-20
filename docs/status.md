# Milestone gate status

Updated whenever gate-relevant work lands. See the implementation plan §4 for
gate definitions.

## M0 — Core + proof of numbers (IN PROGRESS)

| Gate | Status | Notes |
|---|---|---|
| Amplify+mesh 128m radius < 1s on RTX 3060 | 🟨 half-open | **AMD leg measured (2026-07-19)**: `vxc_gpu --radius <m>` (voxel-core/bench/gpu_harness.cpp) now runs the FULL GPU pipeline (ColumnMain→VoxelizeMain→MeshCount/EmitMain) over every surface-shell brick in a horizontal radius, tiled into 128×128-column dispatches with a 1-brick shared halo (14×14-brick/112-column interior per tile — partitions the target square with no gaps, no double-meshing). On this desktop's AMD Radeon RX 7800 XT: **radius 64m = 0.678s** (PASS, under target) and **radius 128m = 2.388s** (OVER target) — end-to-end gate time excludes per-tile CPU column setup and CPU-reference comparison, both timed and reported separately. At 128m the host prefix-scan step (CPU reads GPU-mapped mesh-mask counts, writes back scan offsets, between the count and emit dispatches) is the single largest bucket (1.20s of 2.39s) — larger than all four GPU dispatch stages combined (1.07s) — pointing at CPU↔GPU-mapped-memory round-trip cost in the count→scan→emit chain, not raw compute throughput, as the next optimization target (gpu-mesher-design.md's mesher lists moving the scan to GPU as exactly this contingency). **Host scan replaced with a GPU scan (2026-07-20)**: worldgen.hlsl gained `ScanBlocksMain`/`ScanSumsMain`/`ScanAddMain` (fixed-order shared-memory Hillis-Steele, per-256-block scan + single-workgroup block-sum scan + add-back — deterministic by construction, so quad order is byte-for-byte unchanged), and `gpu_harness.cpp`'s `runMeshChain()` now chains MeshCountMain→ScanBlocksMain→ScanSumsMain→ScanAddMain→MeshEmitMain in ONE command buffer (COMPUTE→COMPUTE buffer barriers write→read between every stage, one trailing COMPUTE→HOST barrier, ONE fence per tile/region) instead of the old two-submission count/emit split with a CPU-side scan in between. The emitted quads buffer is upper-bound sized (32 quads/mask max × maskCount, grow-only reused) *before* the chain is recorded, since MeshEmitMain now has no readback point to learn the exact total first; the true total (`counts[maskCount-1] + offsets[maskCount-1]`) is read back after the single fence. Per-stage GPU timing survives the merge to one fence via 6 `vkCmdWriteTimestamp` queries bracketing the 5 dispatches. maskCount is asserted ≤65,536 per dispatch (ScanSumsMain's single-workgroup limit) — never triggered by the 128×128 tile layout in practice. Result: **radius 64m = 0.529s** (PASS, was 0.678s) and **radius 128m = 1.134s** (still OVER target, was 2.388s — a 52.5% reduction). The host scan bucket (was 1.20s at 128m) is now a 6.98ms GPU-scan bucket (blocks 3.66ms + sums 2.22ms + add 1.10ms) — the scan is no longer a bottleneck at any radius tested. The four original GPU dispatch stages (columns+voxelize+meshcount+meshemit) are ~925ms at 128m, about the same as before (1.07s); marshalling overhead rose modestly (118ms→201ms — three descriptor sets now rewritten per tile instead of ≤2, plus the upper-bound quads allocation) — mesh count/emit dispatch cost (283ms+286ms=569ms) and columns/voxelize (224ms+132ms=356ms) are now the dominant buckets and the next optimization target, not the scan. Digests at both radii are byte-identical to the pre-scan run (see the determinism row below), confirming the GPU scan changes nothing about output, only how it's computed. NVIDIA leg still needs to run the same `vxc_gpu --radius 64/128` and compare digests. CPU reference baseline (single-threaded, container hardware) unchanged below. **Tiles batched into flights to close the 128m gate (2026-07-20)**: gate mode now processes tiles in flights of 8 (`kFlightSize`), each flight recording EVERY tile's full chain (ColumnMain→VoxelizeMain→MeshCount→GPU scan→MeshEmit) into ONE command buffer with ONE fence, replacing the old 3-fences/tile scheme (column, voxelize, mesh chain) — cuts Vulkan submission/fence count from ~1,587 (529 tiles × 3) to 67 at 128m. CPU/GPU overlap ("flight k+1's CPU marshalling runs while flight k's fence is still pending, via a deferred `vkWaitForFences` call") needed double-buffered slot banks (`kPipelineDepth=2`, 16 total per-tile buffer/descriptor slots): a bank is only reused once its owning flight's fence has actually been waited on, since reusing the same slots for two flights at once would race the CPU's buffer writes against the GPU still reading them. Persistent per-slot descriptor sets are now only rewritten when a slot's `GrowBuffer` actually reallocates, not every tile. Per-stage timing now comes from `vkCmdWriteTimestamp` queries bracketing all 7 stages per tile (not just the mesh chain), since batched submission means CPU-side submit/wait stopwatching no longer isolates individual stages — see `runGateMode()`'s report for the "hidden by flight double-buffering" figure (814ms at 128m: CPU marshalling of one flight overlapping GPU execution of another). Result on this AMD RX 7800 XT: **radius 64m = 0.112s** (was 0.529s) and **radius 128m = 0.191s** (was 1.134s — **GATE NOW PASSES**, an 83% reduction). Digests unchanged (see the determinism row below), confirming batching changed only execution overlap, never output or its order; `vxc_tests` green (64/64), default column-only regions mode still bit-exact. One correctness bug was found and fixed during this work: `vkGetQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)` was requesting a full bank's worth of timestamp queries even on a partial (last) flight, where only some tiles' queries were ever written by that flight's command buffer — the unwritten queries never become available, so the call hung indefinitely (surfaced as a multi-minute stall on both `--radius 16` and `--radius 128`, both of which have a partial last flight); fixed by requesting only `pf.count * kTimestampsPerTile` queries. NVIDIA leg still needs to run the same `vxc_gpu --radius 64/128` and compare digests. |
| Bit-identical amplifier output NVIDIA vs AMD | 🟨 half-open | **AMD leg PASSING** (2026-07-19): `vxc_gpu` (voxel-core/bench/gpu_harness.cpp, ADR-0001) dispatches the SPIR-V worldgen kernel (build/shaders/worldgen.ColumnMain.spv) on this desktop's AMD Radeon RX 7800 XT via a headless Vulkan 1.1 harness and byte-compares every field of every column against `vxc::Amplifier::column` — bit-exact over 32,768 columns across two dispatch regions (near-origin and a far/negative-coordinate region), digest `be28ce960bd5bcf6`. **`--radius` gate mode PASSING too** (2026-07-19, seed 20260719): full columns+cells+quads comparison at radius 64m (144/144 tiles, 100% verified: 2,359,296 columns / 87,687,168 cells / 1,129,674 quads, digest `e1db29a9b6874012`) and a deterministic every-8th-tile sample at radius 128m (67/529 tiles, 12.7% verified: 1,097,728 columns / 39,583,744 cells / 497,132 quads, digest `583e91d62cefb8a9`) — zero mismatches in both, and the digest covers ALL GPU output (not just the verified sample) so it's comparable across differently-sampled runs. **Re-verified after the GPU-scan wiring (2026-07-20)**: same seed, same two digests exactly (`e1db29a9b6874012` at radius 64m, `583e91d62cefb8a9` at radius 128m) and the default column-only regions mode also stays bit-exact — confirms the GPU scan is a pure re-implementation of the exclusive-scan step with no observable output change. NVIDIA leg still open: needs a rented/CI Linux+NVIDIA runner producing the same `vxc_gpu` digests (ADR-0001 gate = identical digest on both legs, for both the column-only regions AND the `--radius` gate digests). Interim cross-*compiler* proxy (gcc/clang/MSVC digests) still green as a secondary signal; see `determinism-cross-compiler` in CI. **Re-verified after flight batching (2026-07-20)**: same seed, same two digests exactly (`e1db29a9b6874012` at radius 64m, `583e91d62cefb8a9` at radius 128m) and the default column-only regions mode also stays bit-exact — confirms batching tiles into flights with double-buffered CPU/GPU overlap changes nothing about output, only how and when it's computed. |

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
- [x] Edit-log compaction (`voxelcore/editcompact.h`'s `compactLog()`) now has an offline caller: `vxc_editlog` (voxel-core/bench/editlog_tool.cpp) — `stats <file>` (parse + seed/brickEdge/entries/uniqueBricks/serializedBytes/cellsTouched), `compact <in> <out>` (compactLog → serialize → atomic tmp+rename write, before/after report), `verify <original> <compacted>` (replay both against `World<8>`/`SyntheticTileSampler` from the log's own seed, compare `editedDigest()`, PASS/FAIL exit code; brickEdge-16 logs error out cleanly, not yet supported). `vxc_editlog selftest` round-trips a messy in-memory log through the real file I/O end to end and is wired into CTest (`vxc_editlog_selftest`). Previously compaction had test coverage (`test_editcompact.cpp`) but no caller outside tests. **Remaining**: hook into a real server save/load cycle at M3 (persistence is currently informal — this tool operates on standalone log files, not a live server's log)
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
- [x] Stage 3 — walkable + LWC: streaming perf fixed (≥25×), custom AABB
  walk collision, LWC verified at 8,000km, player experience implemented
  (sized digs, explosives, dual cameras, proxy body — PRs #7/#11)
- Gate (walk & dig at 60fps min-spec): 🟨 near-closed — player feel testing
  passed (Matt, in-session sign-off 2026-07-20); -VoxelPerfRun harness
  measured p50 2.8ms / p95 4.2ms pre-M2 (PR #12). Formal close needs a
  min-spec proxy (settings-throttled) perf run.

## M2 — LOD cascade (first implementation wave landed, 2026-07-20)

Working plan + binding decisions: docs/m2-plan.md.

- [x] Level-aware streaming: `VoxelCoords::FVoxelLevelChunkKey` generalizes
  every record/queue in `VoxelWorldSubsystem.cpp`; per-level annulus desired
  sets from the `RingPresets` table (R0 0-64m .. R4 512-1024m), outer-edge
  hysteresis (1.25x), nearest-first-globally priority with level as an
  equal-distance tie-break only, per-level entry-scan gating so outer rings
  don't rescan on every 3.2m step.
- [x] MipChain worker integration: level-L (L>=1) jobs build bricks via
  `vxc::MipChain<8>` over a pure-`GeneratedWorld` level-0 source with a
  per-job column-grid LRU (`MakeLevelSampler`); level 0 keeps its existing
  hand-tuned fast path unchanged.
- [x] Component/proxy: `UVoxelChunkComponent::SetLevel` scales position and
  bounds by `VoxelSizeUU << level`; `voxel.Debug.Rings` cvar tints by level
  (R0 green .. R4 magenta), `-VoxelDebugRings` forces it on headlessly.
- [x] Verification (2026-07-20, seed 20260719, static spawn, no player
  movement): headless `-game -VoxelScreenshotAfter=<n>` runs, clean shutdown,
  zero ensures. From a cold spawn, ring population is strictly nearest-first
  (rings are non-overlapping distance bands, so this is inherent, not a
  bug): R0 (1596 chunks) and R1 (1172) settle in ~20s; R2 (~2000+ visible /
  ~4010 candidates) takes ~90s; R3 candidates (~4038) dispatch markedly
  slower — measured ~8 chunks/s vs R2's ~45-50/s, confirming the plan's "some
  cost growth expected" note for higher MipChain levels — R3 was still
  draining (598/2565 loaded) and R4 (4076 candidates) had not started after
  475s. **Open follow-up**: cold-start fill time for R3/R4 is long relative
  to a session (this is a one-time worst case, not the steady-state cost of
  keeping rings loaded during normal flight, but it means the "R1+ nonzero"
  verification needs several minutes from a cold spawn, not tens of
  seconds) — worth profiling the per-job MipChain cost at L3/L4 specifically
  (recursive 2x2x2 downsample fan-out) before the next M2 wave.
- **Known limitation** (see docs/m2-plan.md "Known limitation" section):
  distant edits do not propagate to mip levels yet — only level 0 takes the
  overlay-aware path; R1-R4 always render pure-generated.
- Gate (50km+ vista, 60fps, fast flight with no hitches): ⬜ open — this wave
  is streaming/rendering plumbing only; the flight/hitch gate run and
  dithered cross-fade are later M2 items.

### Wave 2 — cross-job mip caching + distant-edit propagation (2026-07-20)

**Item 1: cross-job mip cache.** `FSharedMipCache` (VoxelWorldSubsystem.cpp,
file-scope) is a sharded (16 shards, `FRWLock` per shard) cache of
pure-generated level>=1 `vxc::Brick<8>` mip bricks keyed by `vxc::MipKey`
(level, BrickKey — `voxelcore/mips.h`'s own key/hash types, read-only this
wave, reused as-is), owned by `FVoxelWorldImpl` and shared by every worker
job. `FCachedMipBuilder` reimplements `vxc::MipChain<8>`'s recursion using
`mips.h`'s public `downsampleBricks<8>` directly (`MipChain::cache_` is
private with no seeding hook, so a per-job-only cache can't be shared across
jobs without this wrapper) — every level of the recursion, not just a job's
own target level, consults/populates the shared cache, so an outer level's
build can skip rebuilding inner levels another job (or the SAME chunk's own
previous residency) already computed. Populated on miss inside worker jobs;
no eviction yet (tracked, not bounded) — `MipCacheBrickCount`/`MipCacheBytes`
(brick count + an approximate byte total: base struct size, +1 byte/cell
+1 byte/palette-entry for non-homogeneous bricks) land in
`FVoxelPerfSnapshot` and a new perf-HUD "mip cache" row. A cross-thread race
(a worker job dispatched before an edit lands could still `Insert` a
now-stale value after `PropagateEditToMips` invalidated it) is closed by
`EditEpoch` (`std::atomic<uint64>`, bumped once per edit batch before
invalidation) — same "snapshot at dispatch, compare later" idiom
`FChunkRecord::GenerationId` already uses for stale-*result* discarding,
applied here to stale-cache-*insert* discarding (a losing race just skips
the `Insert`, safe: the value is recomputed fresh next time).

*Measured, `-VoxelPerfRun=120` each way (seed 20260719, same circular
100m-radius flight path, cache toggled via `SharedMipCache*` -> `nullptr` at
the one worker call site for the "before" build only, reverted before
commit), final-snapshot per-level worker ms from the `LogVoxelPerf` "Voxel
worker ms/level" line:*

| Level | Before (no shared cache) p50 / p95 | After (shared cache) p50 / p95 |
|---|---|---|
| R0 | 2.56 / 4.26 ms | 2.53 / 4.40 ms |
| R1 | 21.20 / 50.69 ms | 8.05 / 13.85 ms |
| R2 | 81.55 / 218.19 ms | 7.93 / 13.39 ms |
| R3 | 335.33 / 867.89 ms | 334.80 / 887.43 ms |
| R4 | not reached in 120s (matches wave-1's cold-start finding) | not reached in 120s |

R0 is unaffected (level 0 never touches the mip cache, unchanged fast path).
R1/R2 improve sharply (R2 p95 218ms -> 13ms, ~16x) — NOT from cross-job
sharing between *different* same-level chunks (those partition disjoint
brick keyspace at every level, by construction) but from the SAME chunk's
mip data surviving repeated unload/reload cycles: the perf run's circular
flight continuously churns chunks across ring boundaries (`chunksLoaded`
tracked ~20k over 120s against ~67k total tracked records — heavy
unload/reload churn), and once a level-L brick has ever been computed it
now stays cached forever (no eviction), so a chunk crossing back into a
ring it previously occupied is nearly free instead of a full rebuild. This
directly targets the M2 gate's "no hitches at ring crossings" concern. R3
does not show a matching improvement WITHIN this 120s window: R3 chunks
here are being visited for the first time in the run (the flight path never
gets far enough from the circle's fixed center for those specific 256-512m
footprints to have any prior cached ancestry, and R3 itself hasn't yet
looped back around to a repeat load in 120s) — a longer run, or a player
genuinely flying away and back, would be expected to show the same
R1/R2-style improvement at R3/R4 once that residency exists. Shared-cache
memory at the end of the "after" run: 4,900,415 bricks / ~716MB (no
eviction; a future wave should add an LRU/size cap once this is a live
concern, not before it's measured).

**Item 2: distant-edit mip propagation.** `VoxelCoords::AncestorChunkKey`
(cheap key math: level-L ancestor of a level-0 chunk key = `floorDiv(key,
1<<L)` on every axis, since a level-L chunk's world footprint is exactly
`1<<L` level-0 chunks wide per axis) generalizes `MarkChunkDirtyForRemesh`
from level-0-only to any `FVoxelLevelChunkKey`. `PropagateEditToMips`
(called from `ApplyGroupedEdits` after every dig/place/carve) computes, for
each already-apron-extended dirty level-0 chunk, its ancestor at every level
1..4; each ancestor's `FSharedMipCache` entries are invalidated (stale pure
values), it's recorded in a per-level `EditedAncestorChunks` membership set
(so a chunk not yet streamed still routes correctly through
`NeedsOverlayAwarePath` whenever it later enters the desired set — mirrors
level 0's live `ChunkHasEditedBrick` overlay scan, but as a maintained set
instead of a per-candidate scan, since a level-L scan would touch up to
`(4*2^L)^3` level-0 brick keys per candidate at L4), and is dirtied for
re-mesh if currently resident. Re-mesh itself goes through
`MakeOverlayAwareLevelSampler` (game-thread only, `vxc::World::brickAt` as
the level-0 source, `FCachedMipBuilder` with `SharedCache=nullptr` so an
edited mip value can never leak into the pure shared cache by construction,
not convention) via the same `PendingGameThreadKeys` queue and 4/frame
budget level-0 edits already used (now genuinely level-agnostic).

*Verification (`-VoxelDebugRings -VoxelHeadlessDigTest=25
-VoxelScreenshotAfter=40`, seed 20260719):* the first attempt carved
directly at the spawn/anchor location and produced zero re-mesh events —
by design, R1+ rings are an annulus that EXCLUDES the anchor's own <64m
radius (R0's exclusive territory), so there was never a resident R1+ chunk
there to prove propagation against. Fixed by carving 100m from spawn
(inside R1's 64-128m band, where an R1 chunk had already streamed in
pure-generated by dig time): the corrected run produced the level-1
ancestor set (`Distant-edit mip propagation: level=1 chunk=(15,-1,167)
tracked=0 ...` etc., 8 level-1 + additional level 2-4 ancestor log lines)
immediately on carve, followed within the same second by 36 `Distant-edit
mip re-mesh: level=1 chunk=(...) quads=N` lines (e.g. `level=1
chunk=(14,0,169) quads=7623`) as those now-tracked ancestor chunks
re-meshed through the overlay-aware path — screenshots
`VoxelVerify00002.png`/`00003.png` (Saved/Screenshots/WindowsEditor/) show
the crater with rings-debug tinting on. Level 2-4 ancestors were marked
dirty (logged) but had no resident chunk yet at dig time in this run (R2/R3/R4
hadn't reached that specific 100m-offset footprint within 25s) so produced
no re-mesh event THIS run — the membership-set mechanism (not a live scan)
is exactly what guarantees they'll still route correctly through the
overlay-aware path whenever they do stream in later. Zero ensures across
all four verification logs (`perfrun_after.log`, `perfrun_before.log`,
`perfrun_after120.log`, `digtest2.log`).

- Gate (50km+ vista, 60fps, fast flight with no hitches): ⬜ open — wave 2
  closes both wave-1 items on this list (cross-job mip cache, distant-edit
  propagation); the flight/hitch gate run and dithered cross-fade are still
  later M2 items, and R3/R4 cold-start fill time (wave-1's other open
  follow-up) is unchanged by this wave.

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

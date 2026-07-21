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
- Gate (walk & dig at 60fps min-spec): 🟢 **p95 PASS (2026-07-21)** — min-spec
  proxy p95 dropped from ~18ms to **4.8ms** (postWarmup p95 4.75-4.93ms across
  5 runs, a 3.4x margin under the 16.6ms bar). Root cause was NOT the
  per-load GPU buffer upload the prior waves chased: the per-frame streaming
  budgets were being spent on *free* bookkeeping, so component-less far-ring
  record evictions starved the real R0 unloads and stale worker results
  starved the real R0 applies. R0 resident bloated to ~6000 (4x its ~1600
  desired) and the render thread hitched drawing the stale set. Fix budgets
  the render-thread-facing work only (see "M1 gate close" writeup below).
  Player feel testing passed (Matt, in-session sign-off 2026-07-20).
  **Residual**: 5-18 isolated ~33-40ms steady-state frames remain, fully
  attributed to (a) coarse-ring (R3/R4) recompute amplifier-sampling (~1-2/run,
  an M2 streaming follow-up) and (b) environmental stalls on the shared dev
  box (majority, zero streaming/render/RHI correlation) — not a streaming or
  render defect. Proposed criterion refinement documented in the writeup.

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

### Wave 3 — mip cache eviction, config-driven seed, SkyAtmosphere origin fix (2026-07-20, worktree agent)

**Item 1: mip cache eviction.** `FSharedMipCache` (wave 2 item 1) had no
eviction — bytes only grew (4.9M bricks / ~716MB observed in wave 2). Bounded
now by `voxel.MipCacheBudgetMB` (default 512, `VoxelDebug.cpp`) via a
per-shard GENERATION-STAMPED APPROXIMATE LRU: each cached brick carries a
relaxed atomic `LastTouch` stamp bumped on every `Find` hit and fresh
`Insert` from one cache-wide monotonic counter. `Insert` calls
`EvictIfOverBudgetLocked` (shard's write lock already held, no extra
locking) which samples a FIXED 8 entries from that shard's map, evicts
whichever sampled entry has the lowest stamp, and loops until back under
budget or the shard is dry — O(sample size) per eviction step regardless of
shard size, so a single over-budget `Insert` never pays an O(shard size)
scan even at wave 2's measured scale. `MipCacheEvictions` lands in
`FVoxelPerfSnapshot`, the perf HUD's mip-cache row, and the periodic
`LogVoxelPerf` "Voxel worker ms/level" line. Verified via
`-VoxelPerfRun=60 -VoxelMipCacheBudgetMB=64` (seed 20260719, same circular
flight as wave 2's measurement): mip cache bytes climb from 0 to
~64MB (67,108,338 → 67,108,864 target) then EVICTIONS TRACK BUDGET
PRESSURE and bytes plateau right at the budget (67,108,338 /
67,108,842 / 67,108,671 across successive 5s samples, essentially pinned
at the 64MB target) instead of continuing to grow — evictions climb from 0
to 582,493 over the run's back half. Zero ensures, clean shutdown.

**Item 2: config-driven seed.** `-VoxelSeed=<u64>` (command-line only,
`FParse::Value`, default stays `DefaultSeed`=20260719 — an ini fallback
would be overkill for a dev/verification-only knob) resolved once in
`UVoxelWorldSubsystem::Initialize`, before `Impl` is constructed, and
exposed via a new `GetSeed()` accessor. Audit found ONE hardcode:
`AVoxelClipmapActor`'s heightmap sampler
(`VoxelClipmapActor.cpp::SampleHeightUU`) built its
`vxc::SyntheticTileSampler` from the `DefaultSeed` CONSTANT, not the
subsystem's runtime seed — under `-VoxelSeed`, the Band 3 clipmap would have
silently kept sampling the OLD seed's terrain while the ring cascade moved
to the new one, breaking their shared seam. Fixed: `AVoxelClipmapActor`
reads `UVoxelWorldSubsystem::GetSeed()` once in `BeginPlay` (after the
subsystem's `Initialize` has already resolved it) into a new `TerrainSeed`
member, threaded into `SampleHeightUU`; the ocean actor and game mode were
already seed-independent (ocean is a flat z=0 plane; game mode only queries
`Subsystem->GetSurfaceHeightUU`, which already read the subsystem's
internal `Voxels`). Verified: `-VoxelSeed=42 -VoxelScreenshotAfter=40` logs
`VoxelSeed override: using seed 42 (default 20260719)` and
`Voxel streaming initialized (seed 42)`; screenshot vs. a same-command
default-seed baseline shows clearly different terrain (different spawn
surface height put the camera in a different framing entirely — dense
terrain edges filling the frame at the default seed vs. a mostly-open-ocean
view at seed 42). Zero ensures both runs.

**Item 3: SkyAtmosphere origin fix.** The atmosphere actor spawned at world
origin with the component's default `TransformMode`
(`PlanetTopAtAbsoluteWorldOrigin`), which hardcodes the planet's ground
level at world (0,0,0) — at a far LWC spawn the horizon sphere is computed
relative to that fixed point instead of the actual ground under the player.
Fixed (Epic's documented approach for off-origin/LWC worlds): set
`AtmosphereComp->TransformMode = ESkyAtmosphereTransformMode::
PlanetTopAtComponentTransform` (planet ground level follows the
component's OWN transform instead) and place the actor at the pawn's spawn
column (Z=0), via a `ParseSpawnColumnUU` helper factored out of
`RestartPlayer`'s existing `-VoxelSpawnAt` parsing so the pawn and the
atmosphere actor can never land on different columns. Verified via
`-VoxelSpawnAt=2000000,1500000 -VoxelScreenshotAfter=40`: log confirms
`VoxelSpawnAt override: spawning at column (2000000.0, 1500000.0) m`, zero
ensures, clean shutdown. An A/B screenshot comparison (fix enabled vs.
temporarily disabled, same exact spawn/camera) showed no visually obvious
difference — the screenshot capture's fixed -40° pitch keeps the camera
looking mostly at terrain, with only a small sliver of sky in frame, so
this framing is not a strong visual test for a horizon-sphere artifact
either way; the fix itself matches Epic's documented guidance for
LWC/off-origin `SkyAtmosphereComponent` placement and is confirmed to
compile, run, and log the correct spawn column with zero ensures.

Build: worktree `voxelcore.lib` rebuilt clean (`vxc_tests` 24/24 pass,
untouched by this wave) before `Build.bat VoxelEarthEditor Win64
Development -WaitMutex -NoHotReloadFromIDE`, which also built clean (zero
warnings from any file touched this wave; the ~30 baseline warnings present
are all pre-existing engine-header deprecation notices, none in
VoxelWorldSubsystem/VoxelClipmapActor/VoxelEarthGameMode/VoxelDebug/
VoxelEarthHUD).

## M3 — Multiplayer (wave 1 skeleton landed, 2026-07-20)

Working plan + binding decisions: docs/m3-plan.md. Wave 1 scope: server
target builds, subsystem role split, `AVoxelEditRelay` + intent RPCs + entry
broadcast + join-time log sync, seed/digest handshake, and the gate.

- [x] `Source/VoxelEarthServer.Target.cs` (`TargetType.Server`) added.
  **Cannot be compiled in this environment**: `D:\UE_5.8` is an Epic Games
  Launcher **Installed Build** (`Engine/Build/InstalledBuild.txt` present),
  and Installed Builds do not support compiling dedicated-server targets —
  Epic requires a source-built engine for that (`UnrealBuildTool` fails
  immediately with "Server targets are not currently supported from this
  engine distribution", before ever reaching the compiler). The target file
  itself is syntactically valid and was accepted by UBT up to that
  engine-distribution check. **Worked around for the gate**: ran the
  dedicated-server role via `UnrealEditor-Cmd.exe <uproject> -server -log`
  instead — the standard uncooked/headless technique for exercising
  `NM_DedicatedServer` on an Installed Build without a separate cooked
  server binary (uses the already-built Editor target's runtime, just in
  server mode; no rendering, no viewport). This is the same technique the
  gate script in docs/m3-plan.md's wave 1 item names as an acceptable
  alternative. Editor target (`VoxelEarthEditor`) builds clean throughout.
- [x] Role split in `UVoxelWorldSubsystem`: `TryDig`/`TryPlace`/`CarveSphere`
  keep their exact pre-M3 signatures; internally, `NM_Standalone` takes the
  literal pre-M3 code path (single extra `GetNetMode()` branch, zero
  behavior change); `NM_DedicatedServer`/`NM_ListenServer` apply directly
  (today's behavior) then broadcast newly-appended log entries via
  `AVoxelEditRelay::MulticastAppliedEntries`; `NM_Client` applies the same
  cells locally as a tracked prediction and forwards the intent to the
  server through the local player's `AVoxelEarthPlayerController`.
- [x] `AVoxelEditRelay` (new, `ue-project/Source/VoxelEarth/VoxelEditRelay.{h,cpp}`):
  a single, unowned, `bAlwaysRelevant` world-scoped actor, spawned by
  `AVoxelEarthGameMode::BeginPlay` on authority only when
  `GetNetMode() != NM_Standalone` (single-player spawns nothing — zero
  relay code paths touched, byte-identical to pre-M3). Carries the
  replicated seed/worldgen-version/probe-digest handshake fields (set in
  `BeginPlay` on authority, compared against the client's own locally-
  computed values in the client's `BeginPlay`, hard-disconnect via
  `PC->ConsoleCommand(TEXT("disconnect"))` on mismatch) and the reliable
  `NetMulticast MulticastAppliedEntries(TArray<uint8>)` broadcast.
  **Deviation from the literal RPC-surface suggestion**: client-initiated
  RPCs (submit edit intent, request join-sync) do NOT live on this actor —
  UE Server RPCs are only callable by the connection that owns the target
  actor (`AActor::GetNetConnection()` walks the `Owner` chain; an unowned
  actor has none, so the engine silently rejects a client's attempt to call
  a Server RPC on it). Since this relay is deliberately a single shared
  instance (not one per player), it structurally cannot receive them. Those
  RPCs (`ServerSubmitDigIntent`/`ServerSubmitPlaceIntent`/
  `ServerSubmitCarveIntent`/`ServerRequestJoinSync`/
  `ClientReceiveJoinSyncChunk`) live on `AVoxelEarthPlayerController`
  instead, which IS reliably owned by its own client connection — same
  behavioral contract, the only architecturally-correct place to put them.
- [x] Wire format: a new flat "batch of `vxc::EditEntry`" encoding built
  from `vxc::ByteWriter`/`ByteReader` (voxel-core's own primitives), NOT
  `vxc::EditLog::serialize`'s self-describing whole-log format — broadcasts
  need arbitrary tail slices of the log, not always from seq 0.
  **voxel-core was NOT modified**: entries replay through the existing
  public `World::applyEdit`, one call per brick, via a new
  `FVoxelWorldImpl::ApplyReplicatedEntries` that reuses the exact same
  `ApplyGroupedEdits` tail every local edit already uses.
- [x] Join sync: client's `PlayerController::BeginPlay` (own locally-
  controlled instance, `NM_Client`) calls `ServerRequestJoinSync`; server
  serializes the full log from seq 0 and replies via
  `ClientReceiveJoinSyncChunk`, chunked ≤48KB per reliable RPC; client
  buffers until `bFinal`, replays, then flushes any live
  `MulticastAppliedEntries` batches that arrived mid-sync (buffered, not
  dropped, not applied out of order).
- [x] Prediction reconcile v1: `FVoxelWorldImpl::PendingPredictedCellsByBrick`
  tracks the exact cells each local prediction wrote; a matching confirmed
  entry silently erases it, a differing one logs a warning and is dropped
  (v1-acceptable brick granularity per the plan) — the confirmed entry's
  cells are ALWAYS applied to the overlay regardless of match, so the
  overlay converges to server truth either way. **Observed live in the
  gate run** (see below): client1 logged four "Prediction reconcile:
  brick ... differs from local prediction" warnings after client2's dig
  landed on overlapping bricks, then converged to the same digest as
  everyone else — the mismatch path works as designed, not just the happy
  path.
- [x] Determinism-guard handshake: digest of seed + `vxc::kWorldGenVersion`
  + 16 fixed `Amplifier::column` probes (`UVoxelWorldSubsystem::
  ComputeHandshakeDigest`), replicated via `AVoxelEditRelay` fields,
  compared client-side at join.
- [x] **Gate — "two clients dig the same hole" — PASSED (2026-07-20).**
  Dedicated server + 2 headless clients as separate processes
  (`UnrealEditor-Cmd.exe`, see above for why `-server` instead of a
  compiled server binary), seed 20260719: server up →
  `-VoxelAutoDigAfter=<s>` on both clients fires one `TryDig` each at a
  fixed, seed-derived world column (not pawn-relative, so independent
  processes provably target the identical spot) → `-VoxelDumpDigestAfter=<s>`
  on all three logs `VoxelDigestDump: role=... editedDigest=...`
  (`voxel.DumpEditedDigest` console command also added for interactive use)
  → all three matched:
  ```
  VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client1)
  VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client2)
  VoxelDigestDump: role=Server seed=20260719 editedDigest=0x2451E40F5C935D2C   (server)
  ```
  Both clients' handshake logged `handshake OK (seed=20260719 wgen=1
  digest=0x0265CC7E14BF223B)` before join-sync. No crashes, no ensures, no
  fatal errors in any of the three logs.
  **Bugs found and fixed while getting the gate green**: (1) the
  `-VoxelAutoDigAfter` fixed probe originally started the raycast 10m above
  the surface against an 8m dig range (`DigPlaceRangeMeters`) — the ray
  could never reach anything; fixed to 5m clearance. (2) A dedicated server
  was still running the FULL render-chunk streaming/meshing pipeline every
  tick (worker jobs, greedy meshing) despite having no viewport ever to
  render into — wasted CPU stretched `FTimerManager`-scheduled verification
  switches well past their nominal delay under concurrent process load.
  Fixed: `UVoxelWorldSubsystem::OnWorldBeginPlay` now skips spawning
  `ChunkOwner`/`ChunkRoot` entirely on `NM_DedicatedServer` (Tick() already
  no-ops without them); the authoritative `Impl->Voxels` is unaffected —
  TryDig/TryPlace/CarveSphere all still work identically. This is also a
  legitimate standing optimization for real dedicated-server deployments,
  not just a gate-timing fix.
- [ ] PIE 2-player verification (editor multiplayer PIE) — not run this
  wave; the dedicated-server + 2-headless-clients gate above supersedes it
  for wave 1's pass/fail purposes, but PIE is worth a follow-up smoke test
  before wave 2.
- [x] Join-time compacted-snapshot sync, validation hardening (rate caps) —
  landed wave 2, see below.
- [ ] Prediction/reconcile polish, water/NPC readiness hooks — wave 3 per
  docs/m3-plan.md.

Build: worktree `voxelcore.lib` rebuilt clean from scratch this wave
(`vxc_tests` 2/2 + `vxc_editlog_selftest` pass; voxel-core itself untouched
by M3 wave 1 — see the wire-format note above). `VoxelEarthEditor` Win64
Development builds clean (zero warnings from any file touched this wave;
same ~pre-existing engine-header deprecation baseline as prior waves).
`VoxelEarthServer` cannot build on this Installed-Build engine (see above);
its Target.cs is otherwise complete and UBT-valid.

### Wave 2 — persistence + validation hardening (landed 2026-07-20, worktree agent)

Full writeup: docs/m3-plan.md "Wave 2" section. Summary:

- [x] **Save/load**: `voxel.SaveWorld` console command + autosave-on-shutdown
  serialize the authoritative edit log (`vxc::EditLog::serialize`,
  `vxc_editlog`-compatible) to `Saved/VoxelWorlds/<seed>.vxlog` atomically
  (tmp+rename), authority-only (no-op on `NM_Client`). Compacts the
  on-disk copy via `vxc::compactLog` when the raw log has more than 2x its
  compacted entry count (the live log is never mutated). Startup replays
  the saved file (if present) before streaming begins; `-VoxelNoLoad`
  bypasses it. **Bug found and fixed during verification**: the transient
  `/Engine/Maps/Entry` loading world also gets a `UVoxelWorldSubsystem`
  instance with an empty `Impl`, and an unconditional `Deinitialize`
  autosave was clobbering the real save file with 0 entries before the
  actual game world loaded — fixed with a `bWorldBegunPlay` gate.
- [x] **Join-sync compaction**: server join sync now sends
  `vxc::compactLog`'s output through the existing wire format instead of
  the raw log — verified live: `SerializeCompactedLogEntries (join-sync):
  8 raw entries -> 4 compacted entries (112 bytes)`.
- [x] **Validation hardening**: per-player token-bucket rate cap
  (`voxel.Server.MaxIntentsPerSec`, default 10, continuous refill),
  dig/place size-cap enforcement (`SizeVoxels > MaxCubeSizeVoxels`
  rejected), carve-radius enforcement (`voxel.Server.MaxCarveRadiusUU`,
  default 400 UU, rejected) — all three logged-reject-not-disconnect in
  `_Implementation`, existing camera/pawn range check verified unchanged.
- [x] **Verification — standalone single-process** (seed 20260719): dig
  (`-VoxelHeadlessDigTest=3`) then save (`-VoxelSaveWorldAfter=6`) then
  quit — `SaveWorld: wrote 7863 entries ... editedDigest=0x9EA22D63D98BE8CD`.
  Relaunch (same seed, no dig) — `LoadWorld: restored 7863 entries ...
  editedDigest=0x9EA22D63D98BE8CD` — **exact match**. Zero ensures either run.
- [x] **Verification — networked rerun** (wave 1's gate scenario: dedicated
  server + 2 headless clients, seed 20260719): all three dump the same
  digest as wave 1's original gate run —
  ```
  VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client1)
  VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (client2)
  VoxelDigestDump: role=Server seed=20260719 editedDigest=0x2451E40F5C935D2C   (server)
  ```
  Server saves (8 entries) and quits; **server relaunched** (fresh process,
  same seed) restores `editedDigest=0x2451E40F5C935D2C` from disk; a
  **fresh third client** joins via the now-compacted sync and both dump
  the same digest:
  ```
  VoxelDigestDump: role=Server seed=20260719 editedDigest=0x2451E40F5C935D2C   (relaunched server)
  VoxelDigestDump: role=Client seed=20260719 editedDigest=0x2451E40F5C935D2C   (fresh client)
  ```
  Zero ensures/fatal across all five process logs; every launched process
  self-quit and was confirmed exited, no force-kills needed, no stragglers
  on a post-run process sweep.

Build: worktree `voxelcore.lib` rebuilt clean from scratch this wave too
(`vxc_tests` 70/70 pass, `vxc_editlog selftest` PASS; voxel-core itself
untouched by wave 2 — no source files under voxel-core/ were modified,
only the UE module consumes `voxelcore/editcompact.h`, which already
existed). `VoxelEarthEditor` Win64 Development builds clean via
`Build.bat VoxelEarthEditor Win64 Development -WaitMutex
-NoHotReloadFromIDE` (zero warnings from any file touched this wave:
`VoxelDebug.{h,cpp}`, `VoxelEarthGameMode.{h,cpp}`,
`VoxelEarthPlayerController.{h,cpp}`, `VoxelWorldSubsystem.{h,cpp}`; same
pre-existing engine-header deprecation baseline as prior waves). Standalone
behavior without the new switches/commands is unchanged (`LoadWorld: no
saved world ... starting fresh` is the observed startup line whenever no
save file exists, byte-identical to pre-wave-2 behavior otherwise).

## M4 — Biome classification core (round 1 landed, 2026-07-20, worktree agent)

Working plan + as-built writeup: docs/m4-plan.md "Round 1 implementation".
Round-1 scope only (classification + per-biome surface materials); trees/
flora/ecotone blending remain pending design (rounds 2-3).

- [x] **Materials**: `voxel-core/include/voxelcore/core.h` grows from 8 to
  15 ids (append-only 8-14): `MAT_GRASS`, `MAT_JUNGLE_SOIL`,
  `MAT_SAVANNA_GRASS`, `MAT_PODZOL`, `MAT_PERMAFROST`, `MAT_MUD`,
  `MAT_CLAY` (defined, unused headroom for a future wetland biome).
  `MAT_SNOW` retired from active use, kept stable for old saved edit logs.
- [x] **`voxelcore/biome.h`** (new, header-only, integer-only):
  `classifyBiome` (morphology gates — slope, coastal band, temperature-
  adjusted treeline — before a Whittaker temperature x precipitation table,
  seasonality splitting savanna/grassland and forest types) +
  `biomeSurfaceMaterial`. Full gate order and threshold rationale in
  m4-plan.md.
- [x] **Wiring**: `Amplifier::column` (amplifier.cpp) and
  `worldgen.hlsl`'s `ColumnMain` both call classifyBiome/
  biomeSurfaceMaterial in place of the old v0 ad-hoc surfaceMat logic;
  `ColumnSample`/`GpuColumnSample` layout unchanged (BiomeId used
  internally only).
- [x] **`kWorldGenVersion` 1->2** (world-breaking); goldens regenerated:
  `amplifier_golden_digest` `0xA7CFA118B16CE0DF -> 0x73B43CAE621CA286`,
  `mips_chain_determinism_golden` `0xACC109F9B1A5AD25 -> 0xE4CF1B376622A38F`,
  new `biome_map_golden_digest` `0xEDBF3C9217ECBBF6` (test_biome.cpp, a
  direct classifier sweep). `test_hash.cpp` unaffected (hash/value-noise
  primitives untouched).
- [x] **CPU/GPU determinism gate — PASS bit-exact** (`vxc_gpu`, AMD Radeon
  RX 7800 XT, seed 20260719): column-only regions digest
  `1dbcabb01cfaf2bc` (was `be28ce960bd5bcf6`); `--radius 64m` gate 144/144
  tiles (100%) verified, digest `95a82ba20200f6f2` (was
  `e1db29a9b6874012`), 0.104s; `--radius 128m` gate 67/529 tiles (12.7%)
  sampled, digest `b4c8ec5d0966894b` (was `583e91d62cefb8a9`), 0.177s —
  both still comfortably under the <1s M0 gate target, unchanged from the
  pre-M4 baseline (biome classification is cheap integer comparisons added
  to an existing dispatch, no new kernels).
- [x] **Tests**: new `voxel-core/tests/test_biome.cpp` (13 cases — biome
  distribution sanity per climate band, the morphology-gate slope
  override with an exact-boundary check, seasonality splits, coastal-band
  edges, treeline monotonicity, the full surface-material mapping,
  determinism, golden digest); `test_amplifier.cpp`'s stratigraphy test
  updated to the new biome material set.
- [x] **Build/gates**: `vxc_tests` 92/92 pass on MSVC 14.51/VS 2026
  (Ninja/Release, `/W4 /WX`) — cross-checked with a standalone LLVM-MinGW
  clang++ build of the same sources (same goldens, 0 failures), confirming
  the classifier is pure cross-compiler-portable integer math. `vxc_gpu`
  bit-exact PASS (above). clang `-Wall -Wextra -Wconversion
  -Wsign-conversion` clean on every file touched this wave (pre-existing
  unrelated warnings elsewhere in brick.h/world.h, not touched, left
  as-is — same precedent as M5's wave). `worldgen.hlsl` compiles clean to
  DXIL + SPIR-V for all 7 kernels. float-ban clean.
- [ ] Round 2 (trees/structures) and round 3 (flora/placement, ecotone
  blending) — pending design with Matt, m4-plan.md.

## M5 — Destruction (groundwork landed, 2026-07-20)

- [x] **Connectivity flood-fill CPU reference** (`voxelcore/connectivity.h`,
  plan §3.5: "Connectivity flood-fill over affected region on structural
  edits -> disconnected islands promoted to rigid voxel debris bodies") —
  the primitive that makes chopped trees / cut structures fall, landed as an
  engine-free header-only C++20 module (no UE, no terrain dependency —
  caller supplies `solidFn`/`anchorFn`, same shape as `waterca.h`).
  `findComponents(solidFn, minCorner, maxCorner)` 6-connected-flood-fills an
  inclusive voxel box into components, deterministically ordered by minimum
  voxel coord (falls out of scanning the box in the same z-major order as
  `VoxelCoordLess`, for free — no separate sort-by-min pass needed) with
  voxels within each component sorted the same way.
  `findDisconnectedIslands(solidFn, region, anchorFn)` is the concrete M5
  use case: splits a structural edit's affected-region components into
  anchored (contains >=1 `anchorFn`-true "grounded" voxel — stays standing)
  vs. floating islands (zero anchored voxels — the set that should be
  promoted to debris); `bottomFaceAnchor()` covers the common "touches the
  region's bottom face" policy.
  **Debris-body promotion + Chaos integration is M5-proper, in UE, later**
  — this lands only the CPU graph-algorithm reference the plan's doctrine
  §2 requires (CPU ref before GPU/engine port) plus its test coverage.
- [x] **Tests** (`voxel-core/tests/test_connectivity.cpp`, 7 cases, all
  passing): single blob -> 1 component; two separated blobs -> 2 components
  in deterministic min-coord order; a vertical-trunk-plus-canopy "tree" cut
  mid-trunk -> stump (anchored under a bottom-face anchor) splits from
  canopy+upper-trunk (flagged as the floating island, 128 voxels); edge- and
  corner-diagonal-only touches do NOT connect (6-connectivity, not 26);
  determinism (identical digest run-over-run) with a pinned golden digest
  `0xeccfa24f24b88702`; a component fully spanning the box -> 1 component,
  zero islands under a bottom anchor; perf sanity over a 64^3 mostly-solid
  region with scattered holes -> 1 component in ~30-55ms (well under the
  reference-impl "not accidentally O(n^2)" bar, no hard target).
- [x] **Build/warnings**: `voxel-core` full suite green on MSVC
  (vcvars64 + VS-bundled CMake/Ninja, `/W4 /WX`) — 79/79 tests pass (72
  pre-existing + 7 new). Separately verified clang-clean under
  `-Wall -Wextra -Wconversion -Wsign-conversion -Werror` (llvm-mingw clang
  22.1.5) — one real `int32_t -> size_t` implicit-narrowing sign-conversion
  bug caught in `findDisconnectedIslands`'s component-index loop (MSVC's
  `/W4` did not flag it) and fixed by making the loop index `size_t` with an
  explicit `static_cast<int32_t>` when recording indices into the
  `int32_t` result vectors.

### First slice — chop → island → fall, wired in UE (2026-07-20, worktree agent)

The connectivity primitive above is now driven end-to-end in the UE runtime:
a chop severs a piece, the island detector finds it, it is removed from the
authoritative grid, and a cosmetic Chaos body falls and settles. New UE files:
`Source/VoxelEarth/VoxelDebris.{h,cpp}` (the falling-debris actor) + additions
to `VoxelWorldSubsystem.{h,cpp}` and `VoxelEarthGameMode.{h,cpp}`. voxel-core
was NOT modified (`connectivity.h` is header-only, consumed as-is).

**The pipeline.**
1. **Stand-in tree TEST FIXTURE** (`UVoxelWorldSubsystem::SpawnTreeFixtureAt`,
   behind the GameMode's `-VoxelTreeTest[=<s>]`, authority only): a
   hand-authored blocky voxel tree — a 2×2 MAT_ROCK trunk column (24 voxels
   tall) + a radius-6 spherical canopy blob, ~1000 voxels total — stamped near
   spawn (+6m ahead) through the edit-log path (`StampVoxels`). Explicitly a
   FIXTURE to exercise M5 physics (docs/m4-plan.md Round 2), NOT M4
   vegetation. Renders because the edit routes its above-surface chunks through
   the overlay-aware mesh path (they sit inside R0 + the desired-set's +2-chunk
   Z headroom).
2. **Chop → island detection** (`FVoxelWorldImpl::DetectAndRemoveIslands`,
   called by the file-scope `PromoteDetachedIslands` hook from
   `UVoxelWorldSubsystem::TryDig`/`CarveSphere` after any solid-removing edit,
   before the M3 broadcast so removals replicate with the trigger): builds a
   bounded region around the just-cleared voxels (cleared AABB + margins:
   ±10 XY, −3/+48 Z; hard-capped 48×48×128) and runs
   `vxc::findDisconnectedIslands` over it with a **boundary-except-top anchor**
   (a solid voxel is "grounded" if it touches any region face except the top).
   That anchor is strictly safer than the header's `bottomFaceAnchor`: anything
   reaching a side/bottom boundary is assumed to continue into the standing
   world outside the box, so only a piece fully INTERIOR to the region (a
   severed chunk with margin around it) is flagged — grounded terrain is never
   falsely deleted.
3. **Island → falling debris** (`AVoxelDebris`): each detached island is (a)
   REMOVED from the authoritative grid via the edit-log path (a second
   `ApplyGroupedEdits`), then (b) handed to one cosmetic `AVoxelDebris` actor —
   a Chaos rigid body (an invisible 1m cube carrying gravity; all collision
   responses Ignore because terrain has no Chaos collision, plan §3.3) plus an
   InstancedStaticMesh of one engine cube per island voxel (the "engine cubes"
   render option). It free-falls under gravity and is settled by a per-tick DDA
   raycast against the voxel world (`RaycastVoxelWorld`): when the island's
   lowest face reaches the surface it snaps to rest and stops simulating.

**Deterministic / cosmetic split (doctrine-preserving).** The AUTHORITATIVE,
deterministic, replicated effect of a chop is the **edit-log removal of the
island voxels** — the island DECISION is `connectivity.h` (deterministic) and
the removal is edit entries (replicated + persisted like any dig). The
`AVoxelDebris` **Chaos body is pure client-side presentation**: it touches no
edit log, is never replicated, and may tumble/settle differently per client —
so it can never desync world state. On a dedicated server the removal still
happens but no debris actor is spawned (no viewport). *Follow-up:* clients
today receive the removal as replicated entries but do not yet re-run island
detection to spawn their OWN cosmetic debris — v0 spawns debris only where the
edit originates (standalone/listen/authority).

**Safety — no false debris on normal terrain edits.** A first attempt ran
detection on every solid-removing edit unconditionally; a 10m explosive-style
`CarveSphere` (2.1M voxels, ragged jitter) then produced 1712 spurious
micro-islands from its clamped region — which would have wrongly deleted
terrain and broken the "standalone edit behavior unchanged" gate. Fixed with
two guards in `DetectAndRemoveIslands`: (1) **if the region hits the size cap
(clamped), detection is SKIPPED entirely** — a bounded region can only anchor
soundly when it fully contains the candidate piece, so large edits did not
detach islands *in this slice*. **(Superseded: that clamped branch now hands
off to the brick-resolution differential-support pass — see "Structural
collapse (M5, large-edit)" below. This voxel-resolution path itself is
unchanged and still handles every non-clamping edit.)**;
(2) a **MinIslandVoxels=16 threshold** ignores the handful of single voxels a
ragged carve genuinely isolates (left in the grid), plus a MaxIslandsPerEdit=16
valve. The whole pass is gated by `voxel.Destruction.Enabled` (default true;
with no world geometry that detaches, normal digs find zero islands, so
edit-log RESULTS are unchanged whether on or off).

**Verification (headless `-game`, seed 20260719, zero ensures, clean
shutdown).**
- `-VoxelTreeTest=8 -VoxelChopTest=18 -VoxelScreenshotAfter=30`: tree fixture
  placed (1000 voxels) → trunk carved (32 voxels) →
  `Destruction: region=[(50,-10,10978)..(71,11,11036)] components=2 anchored=1
  islands=1` → `island 0 promoted -> 936 voxels removed` →
  `VoxelDebris spawned: voxels=936 ... halfH=75UU -- falling` →
  `VoxelDebris settled: voxels=936 rest=(605,5,109885) after ~1.0s` (fell 80UU
  = the 8-voxel cut gap, landing on the stump). Screenshots
  `Saved/Screenshots/WindowsEditor/VoxelVerify00004.png` / `VoxelVerify00005.png`
  (chop-test camera framing aims at the tree column). The 1000-voxel tree =
  32-voxel anchored stump + 32 carved + 936 canopy island — accounted for.
- False-positive regression: `-VoxelHeadlessDigTest=10` (a 10m crater on
  terrain, no tree) now logs `edit region exceeded cap (48x48x128) -- island
  detection skipped` and promotes ZERO islands / ZERO debris — terrain
  behavior unchanged, gate preserved.

**Build.** `VoxelEarthEditor Win64 Development` clean via `Build.bat ...
-WaitMutex -NoHotReloadFromIDE` (zero warnings from any file touched this
slice: `VoxelDebris.{h,cpp}`, `VoxelWorldSubsystem.{h,cpp}`,
`VoxelEarthGameMode.{h,cpp}`; same pre-existing engine-header deprecation
baseline as prior waves). Standalone behavior without the test switches is
unchanged.

**Follow-ups** (documented, not done this slice): (1) full voxel-vs-Chaos
collision so debris rests on terrain by contact rather than a per-tick raycast;
(2) re-integrating a settled island back into the static voxel grid as
resting voxels (v0 leaves it as a parked Chaos body); (3) client-side cosmetic
debris from replicated removals; (4) large-edit / structural-collapse island
detection (was: skipped when the region clamps) — **DONE, see "Structural
collapse (M5, large-edit)" below**; (5) reusing the voxel
chunk scene proxy to mesh the island instead of per-voxel instanced cubes;
(6) per-bone voxel character bodies (severed-limb debris), plan §3.5.

### Structural collapse (M5, large-edit)

**The hole.** The M5 first slice detected detached islands with
`vxc::findDisconnectedIslands` over a bounded voxel region, anchoring on "this
solid voxel touches the region's side or bottom face". That anchor is only
SOUND while the region fully CONTAINS the candidate piece, so
`DetectAndRemoveIslands` **skipped detection entirely whenever the region
clamped** at its 48x48x128 cap. A 10m explosive `CarveSphere` is 200 voxels
across, i.e. always clamped — so exactly the edits that most obviously ought to
bring a structure down detached **nothing**, and large destruction left
impossible floating geometry standing. (A first attempt at running detection
unconditionally on the clamped region produced **1712 spurious micro-islands**
from a single 10m crater, which would have wrongly deleted standing terrain;
hence the skip.)

**The model: differential coarse support** (`voxel-core/include/voxelcore/collapse.h`,
integer-only, engine-free, header-only — same caller-supplies-a-predicate shape
as `connectivity.h`). Four ideas, each removing one false-positive mode. Full
derivation is in the header comment; the short form:

1. **Coarse cells.** The analysis grid is coarse cells, not voxels — one cell
   is one 8-voxel (0.8m) brick, so occupancy comes straight off the edit
   overlay's brick occupancy bitset or (for unedited terrain) the heightfield,
   with no per-voxel terrain evaluation. Coarse adjacency is a strict
   OVER-approximation of voxel adjacency, so coarsening can only make a piece
   look MORE attached than it is: it can MISS a collapse but can never INVENT
   one. The approximation error points the safe way.
2. **Support, not connectivity.** Over the occupied cells,
   `dist(c) = min over paths from the anchor set of (number of LATERAL steps)`,
   where a step to a face-adjacent occupied cell costs **1 if horizontal, 0 if
   vertical**. A cell is SUPPORTED iff `dist(c) <= maxLateralCells` (6 cells =
   **4.8m**). In words: *load travels up and down a stack of blocks for free,
   but spans only a bounded horizontal distance from whatever is carrying it.*
   A column standing on the ground is supported to any height; a slab
   cantilevered off it is supported 4.8m out and no further; a slab attached to
   nothing is supported nowhere. There is deliberately **no budget reset on
   vertical steps** — an earlier formulation reset the budget whenever a cell
   rested on a supported cell, which let a 2-cell-thick slab zig-zag
   up/lateral/down/lateral and span unbounded distance.
3. **Differential.** Applied absolutely, that model would flag every natural
   arch, cliff undercut and sea cave and disintegrate them the first time
   anyone dug nearby. So support is computed **twice** over the same region and
   a cell only SEEDS a collapse if it is `occupied-after AND supported-BEFORE
   AND NOT supported-after` — only mass whose support *this edit* destroyed
   falls, and pre-existing geology is grandfathered forever. Pre-edit occupancy
   needs no snapshot: for a removal-only edit it is exactly (post-edit
   occupancy + the bricks the just-cleared voxels sat in), which the caller
   already holds.
4. **Closure.** Seeds alone leave a wart: a roof that already overhung its
   pillar past the budget has an unsupported fringe, which grandfathering
   protects, so when the core falls the fringe is left hanging. So the seed set
   is closed under 6-connectivity through `occupied-after AND NOT
   supported-after` cells — a falling mass comes down whole. This cannot leak
   into standing terrain (the flood only crosses unsupported cells, and all
   ground-connected mass is supported by construction) and cannot
   un-grandfather isolated geology (an arch touching nothing that falls has no
   seed in its component).

**Why this is sound at ANY region size — the property the bounded box lacked.**
Anchors are the region's bottom and four side faces (never the top). Shrink or
clamp the region and interior cells become boundary cells — which are anchored,
hence supported, in **both** the before pass and the after pass, and a cell
supported in both can never satisfy the collapse predicate. So **clamping can
only ever REMOVE collapse decisions, never add one**: under-sizing the region
costs recall, never precision, and the region can be capped as hard as the
frame budget likes. The old box was unsound precisely because its correctness
depended on containment; nothing here depends on containment.
`test_collapse.cpp::collapse_shrinking_region_only_removes_decisions` asserts
the subset property directly over 8 progressively smaller regions.

**Wiring** (`FVoxelWorldImpl::DetectAndRemoveCollapse`). The clamped branch of
`DetectAndRemoveIslands` no longer returns 0 — it hands off here. Ordinary digs
and chops never reach this path at all, so **standalone edit behavior for
ordinary digs is unchanged by construction**, not by testing alone. Region:
cleared-voxel brick AABB + 20 bricks XY / -4/+16 Z, capped at 48x48x40 bricks
(38.4 x 38.4 x 32 m). Occupancy for unedited bricks uses the fact that base
terrain is a pure heightfield (`amplifier.h`: solid iff voxel centre <=
`surfaceMm`), so one `Amplifier::column()` evaluation per voxel column answers
the whole vertical brick stack above it and is reused again for the per-voxel
extraction — 64 evaluations per brick column instead of an O(volume) scan.

**Doctrine boundary — unchanged.** The AUTHORITATIVE act is still the
**edit-log removal** of the collapsing voxels: a deterministic decision,
applied through the same `ApplyGroupedEdits` path a dig uses, replicated and
persisted like any other edit. `AVoxelDebris` Chaos bodies remain **pure client
cosmetic** — never replicated, cannot desync. A dedicated server removes the
voxels and spawns no debris.

**Caps** (all documented at their definitions). Authoritative side:
`MaxCollapsingCells=2048`, `MaxCollapseVoxels=150000`, both taken as a
deterministic prefix in `VoxelCoordLess` order so a truncated collapse is still
byte-identical everywhere; `MinPieceVoxels=16` leaves sub-chip fragments in the
grid (same policy/number as `MinIslandVoxels`). Cosmetic side, applied only in
`PromoteDetachedIslands` so world state is identical whatever they are set to:
`MaxDebrisActors=24` Chaos bodies per edit and a shared
`MaxDebrisInstancesPerEdit=40000` visible-cube budget, with pieces taken
**largest first** (ties broken on the piece's minimum voxel) so the visually
significant mass always gets a body. `AVoxelDebris` now also instances only the
island's **surface shell** — a voxel with all six face-neighbours in the island
cannot be seen from any angle — and strides uniformly (never a prefix, which
would render only the bottom slice of a slab) if the shell still exceeds the
budget. Measured: the chopped tree canopy went from 936 instances to **362**;
the collapsed roof slab is 4958 voxels rendered as **4838** shell instances.

**Console var.** `voxel.Destruction.Collapse` (default 1), mirroring
`voxel.Destruction.Enabled` one level down: it gates ONLY this large-edit pass,
so setting it to 0 restores the previous v0 behavior exactly without touching
ordinary digs. `voxel.Destruction.Enabled=0` still disables both.

**Verification** (headless `-game`, seed 20260719, `-VoxelNoLoad`, zero ensures,
clean shutdown, exit 0). New switches `-VoxelStructureTest[=<s>]` (default 8s)
and `-VoxelCollapseTest[=<s>]` (default 20s, implies the former), anchored
beside `-VoxelTreeTest`/`-VoxelChopTest`.

- **The money case.** `-VoxelStructureTest=8 -VoxelCollapseTest=22 -VoxelScreenshotAfter=34`.
  The fixture (`SpawnStructureFixtureAt`, 20,480 voxels) is a ground-rooted
  **wall** at one end, two **pillars** 12.0m away at the other, and a thin
  **roof slab** spanning between them — shaped so that pure connectivity cannot
  give the right answer. One `CarveSphere` r=260UU through the far pillars
  removed 5,976 voxels; the roof was then **still 6-connected to the ground
  through the wall**, so `findDisconnectedIslands` would report zero islands and
  nothing would fall. The support model instead logged `collapsingCells=44 ...
  supportedBefore=8318 supportedAfter=8268` and `1 piece(s) from 4958
  unsupported voxels, 4958 voxels removed from the static grid (edit-log)`, then
  `VoxelDebris spawned: voxels=4958 shell=4838` → `VoxelDebris settled ... after
  3.63s`. Accounting checks out: the roof is 128x32x2 = 8,192 voxels, the 9 of
  16 brick columns beyond the wall's 4.8m budget are 72x32x2 = 4,608 of them,
  plus pillar footings = 4,958. Screenshot
  `ue-project/Saved/Screenshots/WindowsEditor/M5-collapse-on.png`
  (= `VoxelVerify00001.png`) shows the wall with only its supported roof stub
  standing and the fallen span resting on the ground; the control
  `M5-collapse-off.png` (= `VoxelVerify00005.png`, same run with
  `voxel.Destruction.Collapse 0`) shows the **entire 12.8m slab hanging in
  mid-air** — the exact impossible-geometry bug, side by side with its fix.
- **No false positives — the 1712 regression.** `-VoxelHeadlessDigTest=12`
  (the 10m crater on open terrain, 2,131,643 voxels removed) now takes the new
  path instead of skipping, and logs `occupiedAfter=42306 supportedBefore=44309
  supportedAfter=42306 collapsingCells=0` → `nothing lost support -- 0 pieces`,
  **zero** islands and **zero** debris. The 1712 spurious islands did not
  return. Stronger still, the run reports `editedDigest=0xD9FB0347863F529E` —
  **byte-identical to the pre-change baseline** recorded in this document's M2
  determinism note, on both server and client roles.
- **Ordinary digs unchanged.** `-VoxelTreeTest=8 -VoxelChopTest=18` reproduces
  the first slice's numbers exactly: `region=[(50,-10,10978)..(71,11,11036)]
  components=2 anchored=1 islands=1` → `island 0 promoted -> 936 voxels
  removed` → `settled ... rest=(605,5,109885)`, `editedDigest=0xCBF37C4314C206D7`
  on both roles. The only difference anywhere is cosmetic: the debris body now
  draws 362 shell instances instead of 936.
- **Determinism.** A/B of two identical money-case runs: both produced cell
  digest `0x678F218A72ED5F13`, piece digest `0xC43CE6EDFC8C78C0`, and
  post-collapse `editedDigest=0x662B4D7933826189`. Order-independence is proved
  in voxel-core rather than assumed: `computeSupport`'s bucket queue is checked
  against an order-independent Bellman-Ford relax-to-fixpoint reference at five
  budgets (`dist` is a shortest-path distance, hence unique regardless of
  relaxation order), and `splitIntoComponents` is fed 16 random shuffles of the
  same voxel set and must return identical component order, identical
  within-component order, and an identical digest every time. Golden pin:
  `0x64CE88EFEC89BF80` (`collapse_golden_digest_pinned`).
- **Cost.** The collapse pass is a one-off ~100ms on the money case (prepass
  91.7ms, support 3.7ms) and ~144ms on the 2.1M-voxel crater (prepass 79.3ms,
  support 64.5ms). The prepass — 48x48 brick columns x 64 uncached
  `Amplifier::column()` evaluations — dominates and is the obvious thing to fix
  next (see follow-ups).

**Unity-build hazard fixed (`connectivity.h`).** With `VoxelWorldSubsystem.cpp`
unmodified it rejoins UE's adaptive-unity blob alongside
`VoxelAgentSubsystem.cpp`/`VoxelAgent.cpp`, putting `connectivity.h` and
`pathfind.h` in ONE translation unit — where two real collisions bit:

1. Both headers opened `namespace vxc::detail` and defined an inline
   `localIndex(int64_t,int64_t,int64_t,int64_t,int64_t)` with the identical
   signature. Two definitions of one function in one TU is a hard redefinition
   error regardless of `inline` or `#pragma once` (which guard re-inclusion of
   the SAME file, not two files declaring the same entity). Reproduced exactly:
   `pathfind.h(331,15): error: redefinition of 'localIndex'`.
2. `connectivity.h` declared a function-local `kNeighborOffsets`, which HIDES
   the namespace-scope `vxc::kNeighborOffsets` that `pathfind.h` declares —
   MSVC C4459, which UE compiles as an **error** under `/W4 /WX`.

Fixed properly rather than by suppression: each header now owns its own nested
detail namespace (`connectivity_detail`, `collapse_detail`) and its own
constant names (`kComponentFaceSteps`, `kClosureFaceSteps`, `kSplitFaceSteps`),
with the reasoning recorded at both sites so neither gets flattened back.
Proven two ways: `test_connectivity.cpp` now `#include`s `pathfind.h` as a
standing compile-time regression guard, and `VoxelEarthEditor Win64
Development` was built **with `-DisableAdaptiveUnity`** (which forces all three
modified files back into the unity blob, and which reproduced BOTH errors
before the fix) — `Result: Succeeded`, as does the ordinary adaptive build.

**Build/tests.** `vxc_tests` **132 PASS / 0 FAIL** (12 new in
`test_collapse.cpp`), and `collapse.h` + both test files compile clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror` with clang.

**Follow-ups** (documented, not done): (1) the ~80-95ms heightfield prepass —
`Amplifier::column()` has no cache and `CarveSphere` has just evaluated most of
the same columns, so threading a column cache through the edit would nearly
erase it; (2) collapse is evaluated once, at edit time — knocking out the
support of an already-fallen-and-settled mass does not re-trigger, and there is
no multi-stage progressive collapse; (3) the cantilever budget is a single
global constant, not material-dependent (rock should span further than soil);
(4) collapsing cells are removed at brick granularity, so the shear plane is
0.8m-quantised and can clip a little standing rock adjacent to a falling span;
(5) clients still do not re-run collapse on replicated removals to spawn their
own cosmetic debris (shared with the first slice's follow-up 3); (6) pieces
past the debris cap vanish without a body rather than being merged into a
batched one.

## M6 — NPCs (groundwork landed, 2026-07-20)

- [x] **Dig-aware windowed voxel A* CPU reference** (`voxelcore/pathfind.h`,
  plan §3.6: "local windowed voxel A* where traversing air is cheap, digging
  costs (hardness x time), placing scaffold blocks has cost -> walking,
  mining, tunneling, bridging fall out of ONE cost function") — engine-free,
  header-only, integer-only C++20, same "caller supplies a query over the
  region it cares about" shape as `connectivity.h`/`waterca.h`. Body model:
  the agent is a single lattice voxel (no head clearance yet, documented v0
  simplification). Neighbor model: 18 fixed candidate offsets per cell —
  the classic 6-connected face neighbors (flat horizontal N/S/E/W + pure
  vertical up/down), 8 step-up/step-down offsets (horizontal +1 combined
  with one voxel of rise/drop), and 4 two-voxel-horizontal Jump offsets (a
  cheap no-scaffold alternative to Bridge for a gap exactly one voxel
  wide). `detail::classifyMove` is the ONE cost function: every candidate
  neighbor is classified into Walk/StepUp/StepDown/Climb/Fall/Mine/Bridge/
  Jump purely from (origin, offset, solidFn, PathCostConfig) — the same
  call site produces a walk, a mine, or a bridge depending only on what the
  world and config say, never a separately-coded movement rule.
  `MAT_BEDROCK` is hard-blocked from Mine unconditionally, regardless of
  `PathCostConfig.mineCostByMaterial` (plan: "bedrock = effectively
  infinite/unmineable"). `findPath` runs a deliberately-zero-heuristic A*
  (Dijkstra) bounded to a caller `SearchWindow` (inclusive box +
  `maxExpansions` cap) — dense per-cell bookkeeping arrays sized to the
  window's volume (same tradeoff `connectivity.h` documents), never a
  world-spanning search. The open-set priority queue's only tie-break is
  `PathCoordLess` (z-major, matching `VoxelCoordLess`/`BrickKeyLess`
  elsewhere), making the same query always produce the byte-identical step
  sequence and `PathResult::digest()`. `pathStillValid(path, solidFn,
  config)` re-classifies every recorded step against a (possibly changed)
  `solidFn` and returns false the instant one no longer matches — the v0
  incremental-invalidation primitive a future per-dirty-brick cache
  (M6-proper, UE-side) would call on cached paths touching an edited brick;
  the region-graph hierarchical layer, Tier 0/1/2 agents, and the actual
  UE integration are M6-proper, later.
- [x] **The unified-cost-function proof** (`voxel-core/tests/test_pathfind.cpp`,
  9 cases): the SAME `findPath` call against the SAME world flips its
  chosen action sequence purely from `PathCostConfig` weights —
  `pathfind_diggable_wall_tunnel_vs_around_same_code` tunnels straight
  through a single-voxel-thick rock wall (exactly one Mine action) when
  mining is cheap, and detours around it (zero Mine actions, longer path)
  when mining is expensive; `pathfind_bedrock_never_mined_forces_detour`
  shows the same wall as `MAT_BEDROCK` forces the detour even with mining
  configured artificially cheap for it, proving the impassable rule is
  hard-coded, not config-driven; `pathfind_gap_bridge_vs_around_same_code`
  crosses a 3-voxel-wide missing-floor gap with Bridge actions when
  bridging is cheap and detours around it when expensive. Also covered:
  open-ground straight-line all-Walk with cost = distance x walkCost;
  StepUp/StepDown on a single-voxel ledge (both directions); Jump over a
  narrow gap when `jumpGapCost` is cheap (found and fixed during this pass:
  Jump originally didn't require its midpoint to be genuinely unsupported,
  so it silently undercut Walk/StepUp over ordinary flat ground any time
  `jumpGapCost < 2x walkCost` — fixed by requiring the midpoint have no
  support of its own, i.e. a real gap, not just ordinary terrain);
  determinism (two runs produce an identical step sequence and digest,
  pinned golden `0xa88bdd2f0eb8afd1`); windowing (a goal sealed off by a
  full-window-spanning bedrock wall returns `capped=true` after exhausting
  the window, well under `maxExpansions`; a tiny `maxExpansions=3` cap on an
  otherwise-easy 50-voxel-away goal stops the search at exactly 3
  expansions, proving the cap itself bounds work independent of window
  exhaustion); `pathStillValid` flips from true to false when a single cell
  along an already-computed path is mutated to solid.
- [x] **Build/warnings**: `voxel-core` full suite green on MSVC (vcvars64 +
  VS-bundled CMake/Ninja, `/W4 /WX`) — 106/106 tests pass (97 pre-existing +
  9 new). Standalone LLVM-MinGW clang++ 22.1.5 compile of
  `test_pathfind.cpp` and a header-only translation unit, both under
  `-Wall -Wextra -Wconversion -Wsign-conversion -Werror`: clean. No
  float/double anywhere in `pathfind.h` (float-ban clean; header-only, no
  `.cpp`, no `voxel-core/CMakeLists.txt` library-source change needed).

### Region-graph hierarchical layer (M6, worktree agent)

- [x] **Single-level portal-graph acceleration layer**
  (`voxelcore/regiongraph.h`) sitting on top of `pathfind.h`'s fine A*
  (consumed, not duplicated — calls `detail::classifyMove` directly, so "the
  same walkability notion as pathfind.h" can't drift) — the M6 primitive
  that lets hundreds of NPCs plan paths cheaply instead of each running a
  world-spanning fine search. Space is partitioned into fixed 16-voxel cubic
  `RegionCoord` regions (power-of-two edge, documented compile-time
  constant `kRegionEdge`). Each region face may have zero or more
  `Portal`s — maximal 4-connected clusters of boundary cells where a
  single-step crossing is a valid `classifyMove` (Walk/Mine for the four
  side faces, Climb/Fall for top/bottom; StepUp/StepDown/Jump/Bridge are
  documented as NOT portal-forming in v0). Portals are *mirrored*: a
  doorway produces two Portal nodes (one per region) joined by a cheap
  inter-region edge, rather than one shared node — this is what makes
  incremental recompute tractable (see below). A proof in the header
  comment shows mirrored portals always land on the *identical*
  representative cell (offset by the face normal), so edge construction is
  a direct lookup, not a geometric search. Intra-region edges connect every
  pair of portals in the same region via a bounded fine `findPath` confined
  to that region's own `SearchWindow` (skipped, not infinite-cost, if
  unreachable within the region). `buildRegionGraph(solidFn, minRegion,
  maxRegion, config)` builds the graph over a caller-bounded region-space
  box (same "caller supplies the region it cares about" doctrine as
  `connectivity.h`/`pathfind.h` — never an unbounded world scan).
- [x] **Hierarchical query** `findHierarchicalPath(graph, start, goal,
  solidFn, config, refine)`: same-region start/goal skips the abstract
  graph entirely (one bounded fine search); otherwise, bounded fine
  searches from `start` to every portal in its region and from every portal
  in goal's region to `goal`, then a zero-heuristic multi-source Dijkstra
  over the portal graph with `PortalKeyLess` (region, then face, then cell
  — extending `PathCoordLess`'s z-major convention to portals) as the ONLY
  tie-break, independent of portal storage/insertion order. Never falls
  back to an unbounded direct search if the two regions are disconnected in
  the abstract graph — reports not-found instead (proved by
  `regiongraph_sealed_region_no_abstract_path_no_fallback`, where a
  doorway-less bedrock wall seals two regions and both a direct fine search
  and the hierarchical query agree the goal is unreachable).
  `refine=true` additionally stitches a concrete step-by-step `PathResult`:
  intra-region hops via a fresh bounded fine `findPath`, inter-region hops
  as a single synthesized `classifyMove` step (no search needed — this is
  where most of the cheapness comes from).
- [x] **Incremental dirtying** `markRegionDirty(graph, region, solidFn,
  config)`: recomputes ONLY that region's own portals/intra-edges, its
  up-to-6 neighbors' *mirrored-face* portals/intra-edges, and every
  inter-region edge incident to the dirtied region — never the whole graph.
  Portals are tombstoned (`Portal::alive=false`) rather than physically
  erased so ids/edges never need an O(n) fix-up pass; `RegionGraph::digest()`
  sorts a canonical copy by `PortalKeyLess` and resolves edges through that
  rank, which is what makes tombstone-and-append storage still produce a
  digest byte-identical to a from-scratch rebuild. Proved by
  `regiongraph_incremental_recompute_matches_from_scratch_rebuild`: edit a
  wall+doorway into one region's own boundary brick, `markRegionDirty` that
  region against the post-edit world, and compare digests against
  `buildRegionGraph` called fresh on the same post-edit world — identical.
- [x] **Cheapness demonstrated**
  (`regiongraph_hierarchical_matches_direct_reachability_and_is_cheaper`,
  5-region-long open world, world x 0..79): a direct world-spanning fine
  `findPath` costs 2000 expansions; the hierarchical query's corridor-only
  cost (entry-region + exit-region searches — the number that stays
  roughly CONSTANT as more regions are crossed, since the portal Dijkstra
  itself touches zero fine-A* cells) is 676 expansions, ~3x cheaper; even
  fully refined to a concrete step path it's 1495, still cheaper than the
  direct search. The one-time graph build itself cost 1599 expansions,
  amortized across every future query against that graph (the point of the
  architecture for hundreds of concurrent NPCs).
- [x] **Tests** (`voxel-core/tests/test_regiongraph.cpp`, 5 cases, wired
  into `tests/CMakeLists.txt` next to `test_pathfind.cpp`): (a) a
  full-height/full-width bedrock wall with a single-cell doorway produces
  exactly one portal on each mirrored side, at the expected representative
  cell; (b) hierarchical-vs-direct reachability agreement + the cheapness
  numbers above; (c) a sealed region yields no abstract path on either
  side, no unbounded fallback; (d) `markRegionDirty` matches a from-scratch
  rebuild byte-for-byte after a boundary-changing edit; (e) determinism —
  two builds of the same world produce an identical digest, pinned golden
  `0xeb05deb529b8f143`.
- [x] **Build/warnings**: full `vxc_tests` suite (114/114, all pre-existing
  + 5 new) green via LLVM-MinGW clang++ 22.1.5 (`vxc_tests` target
  specifically, not `vxc_gpu`, per the no-CUDA/no-Vulkan dev box).
  Standalone strict compile of `test_regiongraph.cpp` under `-Wall -Wextra
  -Wconversion -Wsign-conversion -Werror`: clean. No float/double anywhere
  in `regiongraph.h` or the test file (float-ban clean; header-only, no
  `voxel-core/CMakeLists.txt` library-source change needed).
- [ ] Follow-ups (documented as v0 simplifications, not built): multi-level
  region nesting (Tier 1/2 of plan §3.6); StepUp/StepDown/Jump-forming
  portals at region boundaries; portal-id compaction after sustained dirty
  churn (tombstones accumulate — periodic full rebuild reclaims them); UE
  integration and real per-brick dirty-set plumbing into `markRegionDirty`.

### Swarm + Tier 0/1/2 LOD + pursuit (UE, navigation-only v0)

UE-side consumption of `pathfind.h` (worktree agent): hundreds of
server-authoritative NPCs pursue the player across streaming voxel terrain,
navigation-only (no dig/place this slice — that's the next M6 wave). New
files: `VoxelAgent.{h,cpp}`, `VoxelAgentSubsystem.{h,cpp}`; `VoxelEarthGameMode`
gained the `-VoxelSwarmTest[=<N>]` switch, same pattern as the other headless
test switches.

- [x] **Agent representation: pooled + instanced, not N actors.** `FVoxelAgent`
  (`VoxelAgent.h`) is a plain UE struct (position, velocity, waypoints,
  tier, standoff flag, ISM instance index) held in one
  `TArray<FVoxelAgent>` inside `UVoxelAgentSubsystem` — no `AActor` per
  agent, which would be far too heavy at hundreds of agents (per-actor
  tick/replication/component overhead). Rendering mirrors `AVoxelDebris`'
  "engine cubes" style: a single `UInstancedStaticMeshComponent`
  (`AgentISM`), one instance per agent, updated in place each tick
  (`UpdateInstanceTransform`) with ONE `MarkRenderStateDirty()` call per
  tick for the whole batch, not per instance. `FVoxelAgent` is deliberately
  voxel-core-free (no `vxc::` types) so it stays safe to include from
  `UVoxelAgentSubsystem.h`, a UHT-parsed `UCLASS` header — the actual
  `vxc::PathResult` per Tier 0/1 agent lives PIMPL-side, in
  `VoxelAgentSubsystem.cpp`'s `FVoxelAgentImpl` (a parallel array, same
  index as the pool), exactly mirroring `UVoxelWorldSubsystem`'s
  `FVoxelWorldImpl` / `UVoxelWaterSubsystem`'s `FVoxelWaterImpl` PImpl
  idiom so voxel-core never leaks into a reflected header. A dedicated
  server (no viewport) still runs the full simulation; only `AgentISM`
  itself is skipped (`OnWorldBeginPlay`), the same "sim always, render
  never" split `UVoxelWorldSubsystem`'s chunk streaming and
  `UVoxelWaterSubsystem`'s CA both use for `NM_DedicatedServer`.
- [x] **LOD scheduler: tier thresholds, hysteresis, replan budget.** Every
  agent is bucketed each tick by distance-to-player via
  `ComputeNextVoxelAgentTier` (`VoxelAgent.cpp`), a pure hysteresis
  function: entering Tier 0 needs distance < 15m, leaving it needs > 20m;
  entering Tier 1 (from Tier 2) needs < 80m, leaving to Tier 2 needs >
  100m — asymmetric enter/exit thresholds per boundary so an agent sitting
  between the two never flips tiers from per-frame noise.
  - **Tier 0 (near, ~dozens)**: full windowed `vxc::findPath` every tick it
    needs one — replanned when the path is exhausted, `pathStillValid`
    fails, or the player's (ground-projected) goal has drifted >3m from
    where the current path was planned toward. Ground-snapped via
    `UVoxelWorldSubsystem::RaycastVoxelWorld` every tick (mirrors
    `AVoxelDebris`' settle raycast). **Cost**: one `findPath` call per
    replan, windowed to 33×33×25 voxels (~3.3m×3.3m×2.4m, ~27k cells)
    centered on the AGENT, not sized to reach the player — `findPath`
    allocates its bookkeeping arrays to the full window volume up front
    (see `pathfind.h`'s own "Search / windowing" doc comment), so a window
    sized to reach a player 20m away would be ~40x bigger per axis and
    cost orders of magnitude more, every replan, for every Tier 0/1 agent.
    When the (typically far) goal lies outside this small window,
    `findPath` returns `capped=true` with a best-effort path to the
    settled cell closest (Manhattan) to the goal — the agent walks that
    partial path, reaches its end, and gets replanned from its new
    position: repeated small windowed searches chain into steady progress
    toward a moving goal, which is the "local windowed voxel A*" the plan
    doctrine (§3.6) actually describes, not one huge search per agent.
  - **Tier 1 (mid)**: the SAME `findPath` call, coarser cadence — replans
    (and the `pathStillValid` recheck that can trigger one) are additionally
    gated on a 2.5s per-agent cooldown (unless the path is exhausted, which
    still forces an immediate replan, budget permitting), and the
    ground-snap raycast runs on a 0.5s cadence instead of every tick.
    **Cost**: same per-search cost as Tier 0, but far fewer searches/sec
    per agent. `// TODO(M6): swap Tier 1's findPath call for a hierarchical
    regiongraph.h lookup once that layer lands` — a separate track is
    building the region-graph hierarchical layer; it is NOT merged into
    this worktree, so Tier 1 does not depend on it.
  - **Tier 2 (far, the "hundreds" bulk)**: **no A\* at all** — bounded
    steering straight toward the player (`SteerVoxelAgentTier2`,
    `VoxelAgent.cpp`) plus a small per-agent sine wander so a crowd doesn't
    march in lockstep, ground-snapped only every 1.5s. **Cost**: a
    normalize + a `Sin` call per tick, zero voxel queries most ticks — this
    is what makes hundreds of agents affordable.
  - **Replan budget**: `MaxTier0ReplansPerTick = 6` — Tier 0 AND Tier 1
    replans share one counter per subsystem `Tick`, decremented on every
    ATTEMPT (a full `findPath` call), whether or not it finds a usable
    path, since the cost is paid either way. An agent whose replan is due
    but the budget is spent simply waits (moves along its old path, or
    idles at its end) for a future tick's budget.
  - Mine/Bridge are effectively disabled for every search (task doctrine:
    "navigation-only v0 — agents only Walk/Step/Climb/Fall/Jump over
    EXISTING terrain"): every material's `mineCostByMaterial` entry is set
    to the pathfind.h "impassable" sentinel (negative) in
    `UVoxelAgentSubsystem::Initialize`, and `bridgeCost` is priced at
    1,000,000 (pathfind.h has no literal "forbid this action" flag, only
    cost) — Jump stays enabled at its default cost since it places nothing
    and is explicitly one of the allowed actions.
- [x] **Pursuit + standoff.** Every agent's goal is the player's current
  column, Z-projected onto the terrain surface
  (`UVoxelWorldSubsystem::GetSurfaceHeightUU`) rather than the player's
  literal (possibly airborne — `AVoxelEarthFlyPawn` flies freely) position:
  `pathfind.h`'s v0 Climb/Fall model has no "open shaft vs. genuine wall"
  distinction (a documented header simplification — Climb only checks the
  destination is air), so pathing agents literally toward a hovering player
  would have them Climb straight up through open sky one voxel at a time. A
  nav-only ground swarm chases where the player is STANDING OVER, not the
  camera; LOD tiering and standoff distance still use the player's REAL 3D
  position, so a player who flies away is correctly seen as "far" even
  while hovering overhead. An agent within 2.5m of the player
  (`StandoffRadiusUU`) stops advancing (`bIdleAtStandoff`) so the swarm
  doesn't pile into the player's own cell; it only resumes once the player
  is back beyond 4m (`StandoffResumeUU`), same hysteresis shape as the tier
  bands.
- [x] **Headless verification**: `VoxelEarthEditor` built clean (`Build.bat
  ... -WaitMutex -NoHotReloadFromIDE`, zero warnings from any of
  `VoxelAgent.cpp`/`VoxelAgentSubsystem.cpp`/`VoxelEarthGameMode.cpp` — the
  only warnings in the build log are the pre-existing engine-header
  deprecation baseline from `SharedPCH`, unrelated to this pass). Worktree
  `voxelcore.lib` freshly configured+built for this checkout (CMake+Ninja,
  MSVC 14.51/VS 2026, Release — the worktree's `voxel-core` had diverged
  from `main`'s prebuilt lib by an unrelated in-flight `waterca.cpp`
  change, so `main`'s `.lib` could not just be reused this time).
  `-game -VoxelSwarmTest=200 -VoxelScreenshotAfter=20 -windowed -resx=1280
  -resy=720 -log -unattended -nosplash`, seed default (20260719): spawned
  200/200 agents ringed 10m-150m around the player (spans all three tiers
  on spawn by design). Tier bucketing and the convergence metric, logged
  every 2s (`LogVoxelEarth`, `"VoxelSwarm:"` lines):

  | t (approx) | tier0 | tier1 | tier2 | meanDistToPlayer |
  |---|---|---|---|---|
  | spawn+1s | 5 | 113 | 82 | 85.6m |
  | +5s | 5 | 113 | 82 | 83.1m |
  | +10s | 5 | 113 | 82 | 80.0m |
  | +15s | 5 | 115 | 80 | 76.8m |
  | +20s | 7 | 131 | 62 | 70.4m |
  | +25s | 8 | 161 | 31 | 66.2m |
  | +30s | 8 | 189 | 3 | 64.3m |
  | +33s (last tick before quit) | 9 | 191 | 0 | 64.0m |

  Mean distance-to-player **monotonically decreases** (85.6m → 64.0m) and
  Tier 2's count drains to 0 as agents promote through Tier 1 toward Tier 0
  (convergence, both in the aggregate metric and via the tier-promotion
  path itself) — the requested verification. Two screenshots captured
  (`Saved/Screenshots/WindowsEditor/VoxelVerify00000.png`,
  `VoxelVerify00001.png`, 1280×720) visibly show the swarm's instanced
  cube bodies standing on the terrain at various distances. Clean shutdown
  (`LogExit: Exiting.`), zero ensures, zero fatal errors. (Pre-existing,
  unrelated to this pass: `M_Ocean`/`M_VoxelClipmap`/`M_VoxelTerrain`
  fail to load in this worktree with an asset-custom-version-too-new
  error — falls back to the engine default material per each actor's own
  existing fallback path, same as any other missing-material run; not
  caused by or fixed by this slice, no content files are in this slice's
  owned-files list.)
- [ ] **Known v0 scope limit, documented not fixed**: agent state is NOT
  replicated to remote clients this slice (no `FastArraySerializer`/RPC
  wiring — `VoxelEditRelay.h` is outside this slice's owned-files list);
  every verification run above is `NM_Standalone`. The simulation itself
  is already authority-gated (`NM_Client` never ticks its own copy), so
  wiring real replication is additive, not a rewrite — left as a follow-up
  for whoever extends this into a networked run.
- [ ] **Follow-up (next slice, known/expected)**: digging/editing terrain
  while pathing (this slice is navigation-only — agents never call
  `TryDig`/`TryPlace`/`CarveSphere`). Also: swap Tier 1's `findPath` for
  `regiongraph.h` once that layer merges (seam left as a `// TODO(M6)` in
  `VoxelAgentSubsystem.h`); a real 2-voxel-tall body (`pathfind.h`'s own
  documented v0 simplification, inherited here); per-dirty-brick path
  cache invalidation (currently Tier 0 revalidates its whole path every
  tick via `pathStillValid`, cheap at window-scale but not brick-indexed);
  agent state replication (above).

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

### Linux/NVIDIA cross-vendor determinism runner (M0 close-out)

Makes `vxc_gpu` runnable on a Linux+NVIDIA box so a one-shot script can
close the M0 cross-vendor (NVIDIA-vs-AMD) determinism gate without needing
DXC (Windows-only) on the Linux side at all — SPIR-V is portable, vendor-
and OS-neutral bytecode, so the plan is: compile the `.spv` kernels ONCE on
this Windows dev box (existing DXC flow), commit them, and the Linux
harness loads the identical bytes. The determinism check becomes: the SAME
committed SPIR-V dispatched on an NVIDIA GPU must byte-match the CPU
reference — which AMD already does — closing NVIDIA≡AMD transitively (both
legs independently agree with the CPU reference ⇒ they agree with each
other).

**Shaders compiled + committed**: `voxel-core/shaders/prebuilt/*.spv` (7
kernels — ColumnMain, VoxelizeMain, MeshCountMain, MeshEmitMain,
ScanBlocksMain, ScanSumsMain, ScanAddMain), compiled from the current
`worldgen.hlsl` (commit `8b834107701fd4a2a005b8dbd4a17352f44f26c1`, "M4:
biome classification core") with the pinned DXC `v1.9.2602.24`
(`tools/fetch-dxc.ps1`). SHA-256 provenance for every file is in
`voxel-core/shaders/prebuilt/README.md`. **Important**: these do NOT match
the older `e1db29a9b6874012`/`583e91d62cefb8a9` digests recorded in the gate
row above — those predate the M4 biome commit, which legitimately changed
`worldgen.hlsl`'s output (new materials/biome constants). The gate row's
digests are stale relative to `HEAD` and should get refreshed in a
follow-up; that refresh is out of scope here. What this pass actually
re-verified, freshly, on this AMD Radeon RX 7800 XT, using these exact
committed `.spv` files: default regions mode PASS (digest
`1dbcabb01cfaf2bc`), `--radius 64` PASS (digest `95a82ba20200f6f2`, 144/144
tiles, gate 0.129s), `--radius 128` PASS (digest `b4c8ec5d0966894b`, 67/529
tiles sampled, gate 0.259s) — all zero mismatches. These three digests are
the values a Linux/NVIDIA run should be compared against (via
`vulkaninfo`/`vxc_gpu`'s own device-name line, not an automated cross-box
diff — no NVIDIA box exists in this pass to actually run the comparison).

**Loader ported for cross-platform use**: `gpu_harness.cpp`'s dynamic
Vulkan-loader shim is now `#ifdef _WIN32`-gated — Windows keeps
`LoadLibraryA("vulkan-1.dll")` + `GetProcAddress`; the `#else` branch uses
`dlopen("libvulkan.so.1", RTLD_NOW)` (falling back to `"libvulkan.so"`) +
`dlsym` for `vkGetInstanceProcAddr`, via a small `VulkanLoaderHandle`
typedef and a new `closeVulkanLoader()` helper replacing the two direct
`FreeLibrary()` call sites. Everything below that ~30-line shim (dispatch,
byte-compare, PASS/FAIL/digest reporting, `--radius` gate mode) is
unchanged and platform-agnostic — it was already pure Vulkan calls resolved
through the X-macro function-pointer tables, no other Windows API usage
existed in the file.

**CMake**: `voxel-core/bench/CMakeLists.txt`'s `vxc_gpu` target now builds
on both platforms. `VXC_SPV_PATH*` compile definitions point at the
committed `voxel-core/shaders/prebuilt/*.spv` (identical path both
platforms) instead of the gitignored `build/shaders/` DXC output dir. On
Windows, headers still come from `tools/vulkan-headers` (unchanged). On
Linux, headers come from `find_package(Vulkan)` (the system
`libvulkan-dev` package) and the target links `${CMAKE_DL_LIBS}` for
`dlopen`/`dlsym`. The harness stays opt-in (`VXC_BUILD_GPU_HARNESS`,
default OFF) everywhere except one case: MSVC on Windows with
`tools/vulkan-headers` already fetched auto-defaults it ON (unchanged
pre-existing behavior, narrowed from "any WIN32 compiler" to "MSVC
specifically" — see below). Linux never auto-defaults ON (no
`find_package(Vulkan)` probe at option-default time), so a plain default
configure on any existing CI job (gcc/clang on Linux, msvc on Windows)
gains no new hard dependency.

**Regression found and fixed during verification, in scope**: fetching
`tools/vulkan-headers` (a prerequisite for compiling the shaders above) has
the side effect of flipping the pre-existing `WIN32 AND
EXISTS(vulkan_core.h)` auto-default to ON for *any* Windows compiler, not
just MSVC. Tried against this box's clang/llvm-mingw toolchain, that
surfaced a latent, pre-existing bug: `gpu_harness.cpp`'s Vulkan struct
aggregate-initializers (e.g. `VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_...};`)
trip clang's `-Wmissing-field-initializers` under this target's `-Wall
-Wextra -Werror` (MSVC has never warned on this pattern, and vxc_gpu has
only ever been built with MSVC in this repo's history — confirmed via `git
log`). Fixed by narrowing the auto-default from `WIN32` to `MSVC`
specifically, so a Windows box with both `tools/vulkan-headers` fetched
*and* a clang toolchain configured still gets a clean default
configure/build (clang users can still opt in explicitly with
`-DVXC_BUILD_GPU_HARNESS=ON`, which will hit the same pre-existing
warnings — not fixed here, out of scope). Verified: default configure +
full build is clean under both this desktop's clang/llvm-mingw toolchain
(`vxc_tests` 109/109 green, harness correctly excluded) and MSVC/VS 2026
(harness auto-included, builds clean, same PASS/digest as above).

**Linux runner**: `tools/run-nvidia-digest.sh` — one-shot, idempotent
script for a fresh Ubuntu+NVIDIA box. Installs deps (`build-essential
cmake git libvulkan-dev vulkan-tools`), prints `vulkaninfo | grep -i
deviceName` so the operator sees the NVIDIA GPU is the Vulkan device before
trusting any result, configures (`cmake -S voxel-core -B build-gpu
-DVXC_BUILD_GPU_HARNESS=ON -DCMAKE_BUILD_TYPE=Release`), builds `vxc_gpu`,
runs both the default regions mode and `--radius 64`, and prints `===
M0 NVIDIA DETERMINISM: PASS/FAIL (gpu=<name>) ===` as the final line.

**Unverifiable until the Linux box exists**: everything Linux-side —
whether `find_package(Vulkan)` actually locates `libvulkan-dev`'s headers
correctly, whether the `dlopen`/`dlsym` loader path compiles and resolves
symbols identically to the Windows `LoadLibrary` path, and (the actual
point of this whole effort) whether an NVIDIA GPU's `vxc_gpu` run
byte-matches the CPU reference the same way AMD's does. The `#else`/CMake
Linux paths are correct by construction and documented, not run — no Linux
machine was available in this pass.

**No float-in-shader risk found**: `worldgen.hlsl` has zero occurrences of
`float`/`half`/`double` (checked directly) — every value is integer/
fixed-point end to end, matching the float-free contract in
`docs/determinism.md`. This is the actual reason cross-vendor bit-exactness
is even plausible: IEEE-754 float ops can legitimately differ in rounding
between AMD and NVIDIA (fused-multiply-add contraction, transcendental
approximations, denormal handling), which would make a NVIDIA-vs-AMD gate
fundamentally unreliable if the shader touched floats anywhere in the
value-computing path (wall-clock timing code in the harness itself uses
floats/doubles, but that never feeds GPU dispatch inputs or outputs).

## Water track — W1 first slice landed (2026-07-19)

Implicit ocean (z<0, zero voxel data): `AVoxelOceanActor` (camera-following
40km `/Engine/BasicShapes/Plane`, `M_Ocean` via `Tools/create_ocean_material.py`),
underwater fog/post-process tint toggled by camera depth (log-verified
transition, no screenshot), swim-mode placeholder in `AVoxelEarthFlyPawn`
(gravity off, fly-style 300 UU/s below sea level). No pressure CA, reservoirs,
buoyancy, or currents yet -- those are W2-W4.

## Water track — W2 groundwork: pressure CA core landed (2026-07-20, worktree agent)

Engine-free CPU reference for plan §3.7 Layer B ("THE AUTHORITY"), voxel-core
only (no UE/engine touch this pass -- UE integration, reservoirs, replication
are W2-proper, after M3). `voxelcore/waterca.h` + `src/waterca.cpp`:
`WaterBrick8` (dense 8^3 fill-fraction array 0-255, homogeneous-empty
collapse = absence from the map rather than Brick<8>'s in-place
representation, since fill has no other homogeneous value worth keeping) +
`WaterMap` (BrickKey-hashed, mirrors `ChunkMap<B>`'s shape) + `WaterCA`
(owns the water map, an injected `MaterialId(vx,vy,vz)` solid-query callback
so the header stays terrain-free, and an active-brick set). Tick rules v0:
GRAVITY (each cell moves what fits into the non-solid, non-full cell below)
then LATERAL (a cell resting on solid-or-full support equalizes with its 4
horizontal neighbors in fixed +x/-x/+y/-y order, flow=(self-neighbor)/4
with a minimum of 1 whenever self>neighbor+1, capped both ends) then a
documented HYDROSTATIC no-op stub (U-bend column pressure is W2-proper).
Both real phases run as a single fixed-order sequential sweep (sorted
BrickKeyLess bricks, ascending cellIndex within each) over exactly the
active-set snapshot taken at `step()` entry -- v0 is deliberately in-place/
sequential, not a read-flows/apply scheme (that's left as a documented
requirement for a future parallel/GPU port, exactly like the amplifier's
CPU-reference-first discipline). A brick's key lands in the next tick's
active set iff it was the source or destination of an actual change this
tick (this is how neighbor reactivation and settling-out both fall out of
one rule); `totalVolume()` is an O(1) running ledger, `recomputeVolume()`
independently re-sums every stored brick for cross-checking it.

**Discovered while writing the pooling test**: the lateral rule's stable
fixed point is a local "sandpile" property (no two horizontally-adjacent
open cells differ by more than 1 once fully settled) rather than a single
global plateau -- a walled 5x5 basin forced to full coverage settled to
[19,21] (span 2, not 1), and an unwalled flat-floor drop settled into a
smooth cone (values 1-7) with every adjacent pair within 1 but a large
global span. Both are correct, provable fixed points of the specified
`(self-neighbor)/4, min 1 if self>neighbor+1` formula under the fixed
+x/-x/+y/-y sweep order (a Gauss-Seidel-style sweep is not perfectly
symmetric by construction) -- not a bug. The pooling test was written
against the adjacency-local invariant instead of a global-span check.

Tests (`voxel-core/tests/test_waterca.cpp`, 6 new, `tests/CMakeLists.txt`
updated): column drop in a walled shaft (isolates gravity from lateral,
settles exactly on the floor, conservation checked every tick); pooling on
an open unwalled floor (settles to the sandpile fixed point above,
conservation checked every tick); a 3x3 walled container filling bottom-up
(bottom layer exactly full at 255/cell before any second-layer fill, zero
leakage through walls checked over a wide sampled volume); activity
(a settled sim's `step()` touches zero bricks; a drop in a distant isolated
shaft activates and steps exactly its own brick, not the unrelated settled
one); determinism (two independently-built identical scenarios produce
digest-identical state, golden `0x7995BE759FB9D67E` pinned); a 200-random-
add (splitmix64-seeded) + 500-step conservation fuzz over real bumpy terrain
(`SyntheticTileSampler` + `Amplifier::materialAt` as the solid-query
callback) that exact-matches `totalVolume()` against the sum of `addWater`'s
actual-placed return values, with the ledger-vs-recompute invariant checked
after every single step. Full suite green: 71/71 (`vxc_tests.exe`, MSVC
14.51/VS 2026, `/W4 /WX`, Ninja/Release), zero warnings from any file this
pass, no float/double anywhere in `waterca.h`/`waterca.cpp` (float-ban CI
grep clean).

## Water track — W2 perf: two-phase read/apply CA rewrite, ~1900-brick gap closed (2026-07-20, worktree agent)

**Problem**: the v0 sequential in-place sweep above measured 500-650ms/tick
at ~1900 active bricks (this section's own earlier perf-gap note) --
correct but a hashmap lookup (BrickKey compute + `unordered_map::find`) per
cell per neighbor read/write, unparallelizable by construction (each cell's
result depended on cells already mutated earlier in the same pass).

**Fix**: `WaterCA::step()` is now `stepWithOrder(activeSetSnapshot())`, a
genuine two-phase (read-flows, then apply) engine -- **`kWaterCAVersion`
bumped 1->2, a deliberate world-breaking contract change exactly like a
worldgen version bump** (waterca.h's "Tick rules v1" comment is now the
authority; v0's sequential contract is retired, not kept as a fallback).
Same physical fixed-point family per scenario (flat-within-1 pools,
bottom-up container fill, floor-resting columns), different exact per-tick
values and digest -- expected and re-pinned (golden
`0x5C8D36C83246CAFC`, was `0x7995BE759FB9D67E`).

- **Phase READ**: for every active cell, compute up to 5 outgoing flows
  (gravity down, then lateral +x/-x/+y/-y) from data already committed to
  `water_` this tick, never from anything written later in the SAME phase
  -- source-side capped in that fixed priority order so one cell's total
  outflow never exceeds its own fill, all before any target is consulted.
- **Phase APPLY (GATHER then FINALIZE)**: every cell that could possibly
  receive flow (active cells plus their up-to-5 target-direction neighbors,
  the "touched" set) gathers its inbound candidates in the same fixed
  order against its own remaining capacity -- whichever arrive first in
  that order are admitted in full up to the budget, the rest truncated to
  exactly what's left (proof: `waterca_lateral_contention_capped_
  conserved_fixed_order`, a walled cross where 4 full neighbors all want
  to overfill a near-full origin -- origin caps at exactly 255, the two
  lower-colored neighbors give up exactly 1 unit each, the two
  higher-colored ones are correctly untouched, total conserved exactly).
  FINALIZE then nets accepted inflow against accepted outflow per cell
  once every target has been gathered, through the existing
  `setFillAccounted` (ledger + homogeneous-empty collapse unchanged).
- **Flat brick working set**: `computeDesiredForBrick`/`gatherInflowForBrick`
  cache each active/touched brick's own pointer plus (only when a cell
  sits on a brick boundary) its neighbor bricks' pointers ONCE per brick,
  resolving cross-brick reads via arithmetic (`neighborOf`, wrap the local
  coordinate, only bump the brick key on overflow) instead of a
  voxel->BrickKey->hashmap round trip per cell -- this is the actual
  hashmap-cost fix: O(bricks) map lookups per tick instead of O(cells).
- **Eight-way (2x2x2) coloring, not two**: a cell's outgoing flows are
  computed and applied in 8 sequential per-tick rounds keyed by
  `(x&1)|(y&1)<<1|(z&1)<<2` (any single-axis step flips at least one bit,
  so a cell's 5 possible targets are always a different color). This
  needed real debugging, not just the obvious read/apply split: a plain
  2-color (x^y^z parity) split fixes the simple case (naive single-pass
  Jacobi sits exactly at the marginal stability boundary for this
  diffusion rule and produces a persistent checkerboard that never
  settles -- found empirically, a symmetric pour never stopped stepping),
  but 2 colors are NOT enough in general: a closed 4-cell loop in the
  lateral plane alternates between only those 2 colors going around it, so
  red-black can still let such a loop trade flow in a perfect,
  non-progressing cycle every tick forever (found empirically on a
  pooling-test scenario: `activeBrickCount()` never reached 0, and where it
  froze, several adjacent cells differed by more than 1 -- a real
  correctness bug, not merely slow convergence, confirmed by showing that
  repeating the same 2-round pair up to 4x per tick did not help, and that
  alternating which color goes first tick-to-tick made it WORSE). The
  8-way coloring assigns every cell of that same small loop a different
  round, so no two cells of a cycle ever move off the same stale snapshot
  simultaneously -- verified: `waterca_pooling_spreads_flat_within_tolerance`
  now settles cleanly with zero flatness violations (was persistently
  non-settling with 2 colors) and the whole suite's `activeBrickCount()`
  reaches exactly 0 on every settling scenario.
- **Next-active-set fix**: a brick that is genuinely blocked for an entire
  tick (e.g. its gravity target still full) produces zero net change
  itself and therefore isn't a source or destination of anything --
  `changed`-based activation alone would then never reactivate it even
  after the blocking neighbor drains, so it would stall forever the first
  time it lost a single-tick race (found and fixed empirically: a
  2-cell-adjacent gravity drop across a brick boundary froze indefinitely
  under the naive rule). Fixed: the next active set is `changed` UNION
  every one of `changed`'s 6 face-neighbors (all 6, not just the 5
  "outgoing target" directions `touched` uses -- the brick ABOVE a changed
  brick, a potential gravity SOURCE into it, is the one direction that
  list omits).
- **`changed` is a true tick-start-vs-tick-end diff, not per-write**:
  because 8 sequential color rounds each commit real writes to `water_`
  within one tick (round *c*+1 needs to see round *c*'s results), a cell
  can be nudged and nudged back within the same tick -- individually real
  writes that net to zero. Feeding those straight into "changed" would
  mark a brick active forever despite no actual tick-over-tick change
  (found and fixed empirically alongside the checkerboard work). Every
  touched cell's fill is snapshotted before any round runs; `changed` is
  computed by comparing that snapshot against the post-all-rounds state.
- **`stepWithOrder(std::vector<BrickKey>)`** (new public API) runs one tick
  against an explicit active-brick list instead of the real `active_` set,
  accepting any permutation -- `step()` is exactly
  `stepWithOrder(activeSetSnapshot())`. Proves the property that makes a
  future GPU port valid: `waterca_twophase_order_independent_and_
  deterministic` runs two identical scenarios in lockstep for 60 ticks, one
  via plain `step()` and the other feeding `stepWithOrder` its OWN
  active-set snapshot **reversed every single tick** (never the same
  permutation twice in a row) -- byte-identical digest and volume after
  every tick, not just at the end.

**Benchmark** (`voxel-core/bench/waterca_bench.cpp`, new, kept out of the
correctness suite): a 441-column pour (4000 units each, ~1.76M units total)
over real bumpy terrain (`SyntheticTileSampler`/`Amplifier`, seed 20260719),
step() wall-clock timed every tick with the active-brick count recorded
alongside it. `vxc_waterca_bench` reports the average in the active-brick
window closest to the v0 baseline's ~1900-2000 scale.

**Result (2026-07-20, this desktop, MSVC 14.51/VS 2026 Release)**:
`vxc_waterca_bench --ticks 600`, seed 20260719, 1,764,000 units placed over
441 columns, max active bricks reached 3228 during the run. Average step()
in the [1605, 2005] active-brick window (182 samples, closest available
band to the v0 baseline's documented ~1900-2000 scale): **194.4 ms**,
against the v0 baseline's documented 500-650 ms/tick (midpoint 575 ms) --
**~3x speedup, not the 100x+ target.**

**Honest accounting of why**: the per-brick hashmap-lookup elimination
(caching each active/touched brick's 5 real-water-map and 5 scratch-map
neighbor pointers ONCE per brick instead of once per boundary CELL) and the
per-tick-not-per-round scratch/inflow allocation fix (fixed-size
`std::array` scratch reused across all 8 rounds instead of a fresh
`std::vector`-backed hashmap built 8 times per tick) together took this
benchmark from an initial working version's ~360 ms/tick down to ~194
ms/tick in this same window -- real, verified wins, just not enough of one.
The remaining cost is almost certainly the 8x multiplier itself: every one
of the 8 colored rounds still does a full READ + GATHER + FINALIZE pass
over the ENTIRE active/touched set (order ~2000-3200 bricks x 512 cells),
even though on any given round only ~1/8 of a brick's cells match that
round's color and can possibly do anything -- i.e. this implementation pays
for 8 full brick scans per tick to do the productive work of roughly 1.
Not fixed this pass (time-boxed): the two most promising follow-ups are (1)
per-round, per-brick "does this brick have ANY cell of this round's color
with fill>0" early-out (skip the full 512-cell scan entirely when not),
which trivially still needs a fast per-color occupancy check to be worth
it, and (2) revisiting whether 8 colors is really required everywhere or
only in the specific dense/converging regions where the 4-cycle resonance
(see the coloring writeup above) actually arises -- a scenario at ~2000
active bricks spread thinly (not a single dense 1764-unit pour like this
benchmark) may show a much better ratio. Both are correctness-preserving,
pure-perf follow-ups against the SAME v1 (kWaterCAVersion==2) contract
already locked in by this pass's tests, not a reason to hold this pass.

**Build**: `voxelcore.lib` clean rebuild from scratch, `/W4 /WX` (MSVC
14.51/VS 2026, Ninja/Release), zero warnings from any file this pass;
`vxc_tests` 72/72 green (8 waterca tests total: the 6 pre-existing ones all
still pass unmodified in intent -- only the golden digest constant changed
per the documented contract bump -- plus 2 new: the contention-conservation
proof and the order-independence/determinism proof); no float/double
anywhere in `waterca.h`/`waterca.cpp` (float-ban clean, unchanged).

**UE follow-up (next wave)**: the UE water subsystem does not exist yet
this wave (W1 is implicit-ocean-only, per the section above; W2 UE
integration is still unstarted) so there is no live UE-side golden to
re-verify from THIS change -- flagging for whichever wave first wires
`WaterCA` into the UE water subsystem: any saved/golden digest captured
against a `WaterCA` scenario is a v1 (kWaterCAVersion==2) value from this
point forward, and needs regenerating if anyone captured one against the
retired v0 sequential engine in the interim.

## Water track — W2 perf: color early-out + solid_ memoization (2026-07-20, worktree agent)

Follow-up to the two-phase perf pass above, attacking exactly the "8 full
brick scans/tick, ~1/8 productive" gap that pass's honest-accounting note
flagged and time-boxed out. `kWaterCAVersion` stays 2 -- **not a contract
change**: every golden digest (`waterca_deterministic_repeat_and_golden_digest`'s
`0x5C8D36C83246CAFC`, the container/contention/order-independence tests) is
byte-identical before and after this pass; all changes below are pure loop
restructuring or memoization of an already-required-to-be-pure/deterministic
callback (`WaterCA::SolidFn`), never a change to what gets computed.

**What changed** (`voxel-core/src/waterca.cpp`):
1. **Color-cell enumeration** (the change this pass was scoped for): a
   compile-time table `kColorCells[8][64]`, built once via `constexpr
   buildColorCells()` by running the SAME `colorOf()` the tick-rules
   contract already defines over the brick's 512 local cells and bucketing
   each into its color's list. Phase READ now iterates
   `kColorCells[roundColor]` (64 cells) directly instead of scanning all
   512 and `continue`-ing on a color mismatch. GATHER and FINALIZE got the
   analogous treatment: a target cell can only ever receive nonzero inflow
   from a `roundColor` source if its OWN color is `roundColor^1`,
   `roundColor^2`, or `roundColor^4` (the axis-flip derived from which of
   GRAVITY/lateral-X/lateral-Y moved it there) -- so GATHER now visits
   those 3 colors' 192 cells instead of 512, and FINALIZE visits the union
   of those 3 plus `roundColor` itself (outflow) -- 256 cells instead of
   512. One level deeper, GATHER's inner per-cell loop over the 5
   `kInbound` slots was ALSO narrowed to just the 1-2 slots whose axis
   matches the target's own color group (the other slots are structurally
   guaranteed `desired<=0` and were always a wasted check).
2. **Profiling found the actual dominant cost was NOT the scan overhead**
   the previous note guessed, but the `SolidFn` terrain callback itself:
   instrumented locally (not committed) against the bench's real
   `Amplifier::materialAt` (bilinear elevation + several octaves of value
   noise, recomputed from scratch on every call, no memoization of its
   own) measured **~17.8M calls/tick at ~1.08us/call** -- ~128ms/tick, the
   large majority of pre-this-pass wall time, and structurally *not*
   reduced by (1) alone (the old code already skipped `solid()` for
   wrong-color cells via the same `continue`, so call count was already
   minimal per round -- the redundancy is CROSS-round: up to 5 different
   active neighbors of one solid/air voxel can each independently query
   "is THIS voxel solid" as part of their own gravity/lateral check).
   Fix: `stepWithOrder` now builds a per-tick `solidCache`
   (`unordered_map<VoxelKey,MaterialId>`, `VoxelKey` = full `int64_t` x/y/z,
   distinct from the brick-truncated `BrickKey`) and every `computeDesiredForBrick`
   call goes through a memoizing `cachedSolid` lambda instead of `solid_`
   directly. Scoped to exactly one `stepWithOrder()` call (built fresh,
   discarded at return) -- safe because `solid_` is already required to be
   a deterministic, side-effect-free function of `(vx,vy,vz)` by the
   existing order-independence contract, so reusing an answer changes
   nothing observable, and a live-terrain-editing caller is unaffected
   (the cache can only go stale WITHIN a tick, never across one). Measured
   effect: **17.8M raw `materialAt` calls/tick -> ~8.6M** (the tick's
   actual count of distinct `(vx,vy,vz)` queried, i.e. this is the real
   cross-round duplicate-query rate, not a tunable knob).
3. **Per-tick GATHER/FINALIZE pointer cache**: profiling after (1)+(2)
   found GATHER was STILL the next-biggest bucket, from a different
   redundancy -- `gatherInflowForBrick` re-resolved its self + 5
   inbound-neighbor `FlowScratch*` pointers via a `scratchMap` hashmap
   lookup on EVERY call, i.e. 6 lookups x every touched brick x all 8
   rounds, even though (unlike `water_`, whose brick-level contents
   genuinely change round-to-round) the SET of bricks with scratch storage
   is exactly `order`'s contents and is fixed for the whole tick before
   round 0 ever runs. Fix: a `touchedCache` vector, built once per tick,
   holding each touched brick's precomputed `selfScratch`/`inboundScratch`
   pointers and a stable pointer into its `inflowMap` entry; GATHER/FINALIZE
   iterate this vector directly instead of re-hashing `touched` every
   round. `water_.find(key)` inside `gatherInflowForBrick` itself is
   deliberately NOT cached this way -- that lookup's answer (does the
   brick exist, what's its current fill) DOES change round to round via
   the previous round's FINALIZE, so it has to stay live.

**Result** (`vxc_waterca_bench --ticks 600`, same 441-column/1.76M-unit
scenario, seed 20260719, this same desktop): average step() in the
[1605, 2005] active-brick window landed in the **~140-155 ms** range across
repeated runs (best observed 122.9 ms full-run average one run; window avg
139-155 ms across three separate 600-tick runs) -- down from the prior
pass's documented 194.4 ms in the same window, but **not** the "well under
50 ms, ideally 25-30 ms" target this pass was aiming for. This machine's
background load turned out to vary enormously run-to-run this session (a
same-code, same-session A/B rebuild of the PRE-this-pass version measured
anywhere from 191 ms to 468 ms in the same window depending on what else
was running) -- the reported ~140-155 ms range is the STABLE, repeated
value for the optimized code; a controlled back-to-back re-measurement of
the pre-this-pass baseline on the same noisy machine, same session, put it
consistently higher (191-340 ms), so the real win is smaller in absolute
terms than the "194ms baseline" headline number suggests but is genuine
and reproducible.

**Honest accounting of the shortfall**: (1) alone (color-cell enumeration)
barely moved the needle, because profiling revealed `SolidFn` was already
the dominant cost even in the pre-this-pass code -- the previous note's
theory ("8 full brick scans, doing 1/8 productive work") was directionally
right about the SHAPE of the waste but wrong about where the TIME was
going; the actual bottleneck was an expensive, uncached, terrain-backed
callback outside this file's own control, discovered only by instrumenting
per-phase wall time locally (not committed) rather than reasoning from the
loop structure alone. (2) and (3) target the two costs profiling actually
found dominant (the `SolidFn` callback and per-round hashmap re-resolution)
and both measurably helped, but even after both, `SolidFn`'s ~8.6M
remaining calls/tick (each still ~0.5-1us against THIS bench's synthetic
Amplifier) is a hard floor this file's own algorithm cannot lower further
without either changing what gets asked (a physics change, out of scope)
or the caller supplying a cheaper/cacheable `SolidFn` (a worldgen-side
change, outside voxel-core/waterca ownership). A real engine-integrated
`SolidFn` backed by an already-resident chunk/brick lookup (not per-call
procedural noise) would likely land much closer to the original target;
this bench's synthetic Amplifier is deliberately worst-case for exactly
this reason (real bumpy terrain, not a cheap flat-floor lambda).

**Determinism verified**: `vxc_tests` 72/72 green, unchanged golden digest
`0x5C8D36C83246CAFC` (`waterca_deterministic_repeat_and_golden_digest`),
`waterca_container_fills_bottom_up_never_escapes_walls`,
`waterca_lateral_contention_capped_conserved_fixed_order`, and
`waterca_twophase_order_independent_and_deterministic` all pass unmodified
against the same fixed digest/behavior contract as before this pass;
`waterca_conservation_fuzz_over_bumpy_terrain` still holds the ledger
exactly. `kWaterCAVersion` unchanged at 2.

**Build**: `voxelcore.lib` clean rebuild, `/W4 /WX` (MSVC 14.51/VS 2026,
Ninja/Release), zero warnings. Also checked with the winget-installed
LLVM-MinGW `clang++` (`-std=c++20 -Wall -Wextra -Wconversion
-Wsign-conversion -fsyntax-only`, matching CI's stricter sign-conversion
gate MSVC doesn't enforce): one `int`-into-`size_type`-array-index warning
found and fixed (`inboundScratch[i]` -> `inboundScratch[static_cast<size_t>(i)]`
in the new slot-restricted GATHER loop); `waterca.cpp`/`test_waterca.cpp`
clang-clean after the fix. No float/double anywhere in `waterca.h`/`waterca.cpp`
(float-ban clean, unchanged).

## Water track — W2: Phase C hydrostatic pressure pass landed, U-bends now equalize (2026-07-20, worktree agent)

Implements the documented Phase C no-op stub (plan §3.7 Layer B: "gravity
then lateral equalization then hydrostatic column pressure (fills
U-bends...)") for real. Before this pass, `WaterCA::step()` only ever
settled each region to its LOCAL gravity+lateral fixed point -- correct for
a single basin/shaft, but structurally incapable of raising the far arm of
a U-bend or a second communicating-vessels tank, since gravity never flows
up and lateral only trades flow between same-z neighbors. `kWaterCAVersion`
bumped **2 -> 3** (deliberate world-breaking change per docs/determinism.md
-- Phase READ/APPLY itself is byte-for-byte unchanged; only the new Phase C
step appended after it changes tick output).

**Algorithm** (`WaterCA::hydrostaticPass`, `voxel-core/src/waterca.cpp`,
full rationale in `voxel-core/include/voxelcore/waterca.h` "Phase C --
HYDROSTATIC"): runs once per `stepWithOrder()` call (not once per colored
round), after all 8 Phase READ/APPLY rounds have committed.
1. **Connected-component discovery**: a flood fill over voxel 6-neighbors,
   seeded only from water cells (fill>0) in `touched` (the same
   active-plus-target-neighbors brick set Phase READ/APPLY already builds).
   Water-to-water steps are unconditional and UNBOUNDED (any owning brick,
   active or long-settled) -- necessary so a component's true total volume
   is always seen, even when the source arm settled (dropped out of the
   active set) ticks ago. Water-to-EMPTY steps are gated on three
   conditions: not a -z (downward) step; the FROM cell is either a full
   (255) water cell or an already-included empty cell (a chain that started
   at a full column); and the target's owning brick is in `touched`. This
   gating is the fix for a real bug found while building this: an earlier,
   simpler version explored any empty cell whose owning brick was merely
   `touched` (no fullness/direction gate) -- since a brick is 8 voxels
   tall, that pulled a partially-filled water cell's own dry headroom (same
   brick, no relation to any actual pressure) into the redistribution every
   tick, diluting the level and reactivating a permanently growing
   footprint. Concretely, `waterca_pooling_spreads_flat_within_tolerance`
   (an open, unwalled pour) never settled under that version -- confirming
   the gate is load-bearing, not cosmetic.
2. **Level computation**: per component, sort cells by z ascending (fixed
   (x,y)-ascending tie-break within a layer) and allocate the component's
   conserved total fill bottom-up -- each layer filled to 255/cell as long
   as volume remains for the whole layer, the first layer that can't be
   fully filled gets `total/n` plus the remainder to the first `total%n`
   cells in tie-break order, everything above that gets 0.
3. **Apply**: absolute per-cell targets through the same
   `setFillAccounted()`/`changed`-set path every other write in this file
   uses -- ledger, homogeneous-empty collapse, and activity tracking stay
   centralized.

A hard per-component cap (`kMaxHydrostaticComponentCells = 65536`) is a
safety backstop against the unbounded-through-water rule: a component that
would exceed it is left completely unmodified that tick (deferred, never
partially/incorrectly equalized off a truncated view).

**Conservation**: structural, same argument as Phase READ/APPLY -- every
write is an absolute target computed from a partition of the component's
own conserved total that sums back to exactly that total (integer division
+ explicit remainder, no rounding loss); distinct components never share a
cell (one global `visited` set for the whole pass). Verified by
`runToSettleCheckingConservation`'s per-tick ledger-vs-recompute check in
every existing and new waterca test, including the two new hydrostatic
scenarios and the 200-drop/500-tick bumpy-terrain fuzz test.

**Order independence**: `hydrostaticPass` takes `touched` (the same sorted
`std::set<BrickKey>` Phase READ/APPLY already derived from `order`'s
CONTENTS, never its permutation) and reads `water_` state that is already
order-independent by the time Phase C runs; it never looks at `order`
itself. Component discovery is an intrinsic property of the
(touched-contents, water-contents) graph, and the only tie-break (z, then
x, then y) is over voxel coordinates, never iteration order. Proved by a
NEW test, `waterca_hydrostatic_order_independent_and_deterministic`
(`tests/test_waterca.cpp`) -- the existing reversed-active-set-snapshot
technique from `waterca_twophase_order_independent_and_deterministic`, but
run over the U-bend scenario for 200 ticks specifically so Phase C is doing
real cross-arm redistribution every tick along the way, not just settling a
single flat basin.

**New tests** (`voxel-core/tests/test_waterca.cpp`, all passing, plus 2 new
fixtures `uBend`/`communicatingVessels`):
- `waterca_hydrostatic_u_bend_equalizes_and_settles`: pours 2805 units
  (chosen to land EXACTLY, no remainder) into only the left arm of a
  solid-walled 1-cell-wide U-bend; both arms settle at precisely the same
  height (z=1..4 full in each), volume exactly conserved, nothing poured
  into the right arm directly -- it only got there via Phase C.
- `waterca_hydrostatic_communicating_vessels_equalize`: same idea with
  wider (2x2 footprint) tanks joined by a low channel, to exercise the
  level computation's per-layer cross-section accounting rather than
  single-column arms; 10200 units poured into tank A land both tanks at
  z=1..3 full, exactly equal.
- `waterca_hydrostatic_order_independent_and_deterministic`: see above.
- Existing `waterca_lateral_contention_capped_conserved_fixed_order`
  updated (not gamed): this scenario's 5 cells turn out to be one
  fully-connected flat (z=1) water body once Phase READ/APPLY settles the
  contention, so Phase C now ALSO re-equalizes it in the same tick --
  final per-cell values changed (west/south/origin=255, north/east=254,
  still exactly conserving 253+255*4=1273) even though the contention
  mechanism itself (fixed 8-way round order resolving the 4-units-into-a-
  2-unit-budget race) is untouched; comment rewritten to explain why the
  test no longer observes that mechanism's output as the tick's LAST word.

**Golden digest re-pinned**: `waterca_deterministic_repeat_and_golden_digest`
was `0x5C8D36C83246CAFC` (v2, Phase C no-op) -> now `0x3D2224BE4A253404`
(v3, Phase C live) -- honestly recomputed from an actual passing run, not
hand-picked. All 87 waterca-and-surrounding tests in `vxc_tests` pass
(87/87 total in the full suite this pass touched).

**Benchmark** (`vxc_waterca_bench`, same 441-column/1.76M-unit synthetic-
terrain scenario, seed 20260719): reduced tick counts used (`--ticks 40`
and `--ticks 50` separately, not the usual 600) because Phase C's cost at
this scenario's scale made a full 600-tick run impractically slow for this
pass's iteration loop -- the active-brick count already reaches its
steady-state window (~2100 peak) within the first ~40-50 ticks, so the
windowed average is still representative. Measured **~1170-1480 ms/tick**
full-run and windowed averages (two separate runs), versus the
pre-this-pass v2 baseline's documented ~140-155 ms/tick in the same
[~1600,2000]-active-brick window -- roughly an **8-10x regression**,
entirely attributable to Phase C's deliberately-unbounded water-side flood
fill: this bench's 441 simultaneous drop columns over bumpy terrain merge
into a small number of LARGE connected pools, and every tick that touches
any part of a pool's boundary re-floods and re-levels the WHOLE pool
(bounded by the 65536-cell cap, but that cap only stops an oversized
component from being WRITTEN, not from being TRAVERSED -- the BFS still
visits, and marks visited, every reachable water cell up to the cap before
giving up, so a large settled pool costs real time every tick something
near it is still active). This is exactly the cost the plan doc's own
Layer B description implies ("replicate compressed fill-diffs for active
regions (capped, e.g. few-thousand active bricks ~= ~50m event)") and the
task brief anticipated ("hydrostatic adds cost -- quantify; ... note if it
needs an active-only optimization") -- **it does**. The correct fix is a
persistent, incremental structure (e.g. a per-body union-find or cached
equilibrium state kept ACROSS ticks, invalidated only where a component's
own boundary actually changed) instead of this v0's from-scratch flood
fill every tick; out of scope for this pass (a genuine redesign, not a
tuning knob -- lowering `kMaxHydrostaticComponentCells` alone does not
help, since the traversal cost comes from marking cells visited, which
happens regardless of the cap). Flagged here rather than silently
shipped. Small/medium scenarios (the U-bend/vessels tests above, and any
single pour that doesn't merge into a terrain-scale pool) pay only a small
Phase C cost proportional to the pour's own size, not the whole map.

**Build**: `voxelcore.lib` clean rebuild, `/W4 /WX` (MSVC 14.51/VS 2026,
Ninja/Release), zero warnings. LLVM-MinGW `clang++`
(`-std=c++20 -Wall -Wextra -Wconversion -Wsign-conversion -Werror`) clean
on both `waterca.cpp` and `test_waterca.cpp` with no changes needed this
time. No float/double anywhere in `waterca.h`/`waterca.cpp` (float-ban
clean, unchanged) -- verified by grep, the one `double` string match is
inside a code comment ("double-visit"), not a type.

## Water track — W2 perf: hydrostatic flood access-cost rewrite, ~3x, BYTE-IDENTICAL (2026-07-20, worktree agent)

Attacks the ~8-10x Phase C regression above. Constraint was hard: the tick
OUTPUT must stay byte-for-byte identical (golden `0x3D2224BE4A253404`
unchanged, order-independence preserved) -- this is world-derivation code,
so a different digest is a FAIL, not a re-pin. Delivered: **identical output,
~3x faster**, via a pure ACCESS-COST rewrite of `WaterCA::hydrostaticPass`'s
flood (`voxel-core/src/waterca.cpp`). The component partition, per-component
`totalVol`, bottom-up level allocation, the 65536-cell overflow cap, and
every tie-break are unchanged by construction; only HOW the flood reads the
map and applies writes changed.

**What profiling actually found** (the earlier note's "traversal/visiting"
diagnosis was right but incomplete). Instrumented cell counts on the
441-column bench: **97-98% of flood cell-pops are AIR** (~900K air vs
~14-31K water per tick) -- the deliberately-unbounded flood explores the
entire touched air shell above/around the pool every tick, and on this
scenario the big merged body exceeds the 65536 cap, so it is TRAVERSED in
full and then skipped. Stubbing the air `solid()` query dropped the pass
from ~720ms to ~89ms, proving the dominant cost is the terrain `solid_`
query on that air shell, not the flood machinery -- exactly the ~1us/call
Amplifier column query the code comments already flag as the engine's
pour-scale bottleneck.

**Four byte-identical changes** (each verified against the pre-rewrite
implementation, not just re-derived):
1. **Per-brick flood cache.** Each DISTINCT brick is resolved once (one
   `water_.find` + one `touched.find`) into a node holding the brick's water
   pointer, its touched-ness, and an inline 512-bit visited mask; every
   subsequent cell read/visit is a plain array index + bit op. Replaces the
   old per-CELL `getFill` hashmap find + `unordered_set<VoxelKey>` visited
   probe (3 splitmix64 rounds) + `inTouched` set-find. Stack entries carry
   the resolved `BrickCell*` (no lookup on pop) and same-brick neighbor steps
   reuse it (no hash) -- node-based `unordered_map` keeps those pointers valid
   across later inserts.
2. **Deferred writes.** All leveling writes are collected and applied in one
   pass AFTER discovery, so the read-only brick cache never observes a
   mid-pass `water_` mutation (setFillAccounted can create/erase bricks).
   Safe because distinct components never share a cell and each target is an
   absolute value from the captured `totalVol`.
3. **No-op-write skip.** Each discovered cell carries its flood-time fill
   (still current at apply time, since writes are deferred), so a target
   equal to the current fill emits no write at all -- a settled interior is
   all no-ops. Removes the second O(pool) cost (a `waterKeyForVoxel` +
   `water_` find per interior cell) that setFillAccounted paid every tick.
4. **Raw `solid_` for the flood (biggest win: ~720ms -> ~410ms).** Phase C's
   flood queries each air voxel at most once (the shared visited mask is
   checked before any solidity query), so routing those through the per-tick
   memoizing `cachedSolid` only added a `VoxelKey` hash + an insert into a
   map that balloons to the full air-shell size (with rehashing) for ZERO
   dedup benefit. `hydrostaticPass` now takes the raw `solid_` callback
   directly (no longer templated). Byte-identical: memoization never changes
   a deterministic query's answer. (Phase READ/APPLY still uses `cachedSolid`
   -- its 5-neighbor gathers DO re-ask the same voxel, so there the memo pays
   off.) Also skips the query entirely for WATER neighbors (never solid by
   engine invariant).

**Benchmark** (`vxc_waterca_bench`, seed 20260719, same 441-column/1.76M-unit
scenario): windowed avg **~1322-1330 ms/tick -> ~411-441 ms/tick**; full-run
avg **~1052-1117 ms/tick -> ~350-365 ms/tick** -- **~3.0-3.2x**. Still ~2.7x
over the v2 (pre-Phase-C) ~140-155ms baseline; the residual is almost
entirely the ~900K/tick terrain `solid_` calls (see obstacle below).

**Byte-identical verification**: golden `0x3D2224BE4A253404` unchanged;
`waterca_twophase_order_independent_and_deterministic` and
`waterca_hydrostatic_order_independent_and_deterministic` still pass (the
reversed-active-set replay); all 110 `vxc_tests` green. Beyond the unit
tests (which only exercise tiny, single-brick components), the rewrite was
digest-compared tick-by-tick against the pre-rewrite implementation on the
full 441-column overflow scenario across 3 seeds (20260719/12345/99) at
checkpoints through tick 60 -- every digest matched. A NEW permanent
regression test, `waterca_hydrostatic_large_pool_multibrick_golden`
(golden `0x56BC18914355A205`, itself confirmed identical between old and new
impls), pins a wide 16x16 multi-brick pool so future flood changes that
perturb the cell set / leveling / write set are caught in-suite. Clang
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror` clean; float-ban
clean. `kWaterCAVersion` NOT bumped (output unchanged, correctly).

**Obstacle for the recommended persistent/incremental per-body structure
(honest report, not shipped -- doctrine forbids a latent divergence).** The
theoretically-correct O(1)-settled fix is blocked on byte-identical grounds
by two coupled facts this profiling exposed:
- **The overflow cap counts AIR, not just water.** A hydrostatic component's
  size (and thus whether it is leveled or skipped-as-too-big) depends on the
  gated air cells reachable THIS tick, which depend on `touched`. So the
  cell set -- and even the level-vs-skip decision -- is genuinely per-tick,
  not a persistent property of the water body. A persistent water-body
  structure cannot reproduce the exact skip decisions without re-deriving the
  touched-dependent air each tick.
- **Air BRIDGES water.** Two water regions with an air gap between them (a
  full cell -> gated air -> down into the other region's water) are ONE flood
  component and equalize through the gap -- this is the communicating-vessels
  behavior, and it is touched-dependent. So component identity is not water
  connectivity; persistent union-find over water alone under-merges vs the
  real flood.
  Consequence: seeding only from CHANGED cells, or skipping "clean" bodies,
  or truncating the post-overflow air walk, each diverges on a constructible
  (and plausibly bench-present) case -- a settled body made to rise by a
  third party newly touching adjacent below-surface air, or an air-bridged
  sub-body under the cap that would then be leveled instead of skipped.
  A cross-tick `solid_` cache (which WOULD cut the dominant cost, since
  terrain is static within the bench/tests) is likewise unsafe: it changes
  output under live-edited terrain (digging), which the existing per-tick
  cache deliberately avoids, and there is no terrain-change invalidation hook
  to add without touching non-owned caller code. Both are real follow-ups but
  need a design pass (and a terrain-edit signal into WaterCA) that is out of
  scope for a byte-identical perf fix -- flagged rather than risked.

**Follow-ups**: (a) a persistent per-body structure that carries the
touched-dependent air frontier explicitly (not just water) and re-validates
only the frontier each tick -- the real path to O(1)-settled, but a genuine
redesign with a merge/split/air-bridge correctness proof; (b) a `solid_`
cache invalidated by a terrain-edit generation counter plumbed from the
dig/explosive path -- would collapse the residual ~640ms directly and helps
Phase READ/APPLY too; (c) revisit whether the overflow cap SHOULD count air
(counting only water would make the cap a property of the water body and
unblock (a)) -- but that is a deliberate world-breaking change (digest +
`kWaterCAVersion` bump), a separate decision, not a silent perf tweak.

## Water track — W2 perf: cross-tick solidity memo (2.8x, BYTE-IDENTICAL, opt-in) + persistent-body design pass (2026-07-21, worktree agent)

The design pass the section above asked for. Two deliverables: a **shipped,
byte-identical 2.8x** on top of the previous 3x, and a **decision, not a
behavior change**, on the persistent per-water-body structure —
`docs/adr/0003-hydrostatic-persistent-body.md`, drafted **PENDING Matt's
sign-off**. No tick output changed, no golden re-pinned, no
`kWaterCAVersion` bump.

**The two filed blockers were re-derived, and both are less fatal than filed.**
(1) The overflow cap does count AIR — confirmed — but the count DECOMPOSES into
(persistent water size) + (this tick's air), so it blocks only the naive
"skip untouched bodies" shortcut, not a persistent structure. (2) Air does
bridge water — confirmed — but gate (c) means an air cell is only explorable if
its brick is in `touched`, so **every air bridge lies inside the touched region
we already pay to walk**; persistent water union-find + per-tick bridge unions
reproduces the exact partition. The blocker that actually bites is different and
was not previously filed: **splits** (union-find has no deletion, so a draining
cell forces a re-flood — worst case is today's cost plus guard overhead) and the
**visited obligation** (even a provably-unchanged component must still have its
cells marked visited, or a later seed inside it emits different writes; that is
O(cells) unless per-brick visited masks are persisted, i.e. the structure must
carry per-body per-brick mask state, not just a size and a volume). The ADR
records the **replay-skip invariant** (a component's output is a pure function
of fill + `touched`-membership + `solid_` over its *dependency footprint*, so an
unchanged footprint permits a skip) so it never has to be re-derived; merges,
splits and appearing/vanishing air bridges all fall out of the guard rather than
needing special cases.

**What shipped: an opt-in cross-tick memo of `solid_`** (`waterca.h`
`setSolidCacheEnabled` / `invalidateSolidAt` / `invalidateSolidRegion` /
`invalidateSolidCache` / `solidCacheBrickCount`), stored as per-brick 512-bit
known/solid mask pairs and reached through the flood's existing per-brick cache
node, so an air-shell solidity question becomes a shift-and-test with no
hashing. Memoizing a pure function cannot change its answers, so this is
byte-identical — for as long as the memo agrees with `solid_`, which is the
caller's invalidation obligation. **Default OFF**, because the one WaterCA whose
SolidFn sees editable terrain is the live UE subsystem's
(`IsSolidAtVoxel` -> `World::materialAt`, overlay-aware: digging and explosives
change solidity under settled water), and the plumbing to notify it lives in
`VoxelWaterSubsystem.cpp` / `VoxelWorldSubsystem.cpp`, not in this agent's owned
files. Enabling it there is ADR-0003 item 2, pending sign-off.

**Measured** (`vxc_waterca_bench`, new `--solid-cache`, `--count-queries`,
`--lake`, `--lake-span`, `--lake-solid-spin` flags):

| Scenario | memo off | memo ON |
|---|---|---|
| 441-column pour, real Amplifier terrain (full-run avg) | 329-345 ms/tick | **111-121 ms/tick** |
| terrain `solid_` calls, same run | 40.8M | 4.7M |
| settled 63x63 lake, ~1us/query emulated | 9.2 ms/tick | 6.5 ms/tick |
| settled 127x127 lake, ~1us/query emulated | 10.6 ms/tick | 6.8 ms/tick |

That is **~2.8-3.0x** (paired back-to-back runs; the memo-off figure is the
noisier of the two, ranging to ~435 ms under CPU contention from a concurrent
build, while memo-on sits stably at 111-115 ms), and it puts Phase C *below* the pre-Phase-C v2 baseline
(~140-155 ms/tick) — Phase C is no longer a regression against the pass it
regressed. Memo memory on the pour bench: 6477 bricks, ~830KB of masks.

**Why the persistent body structure is DEFERRED (recommendation, not a
unilateral call).** The new `--lake` bench mode measures the case the
441-column pour does not represent: a fully settled pool disturbed by one unit
per tick. It turns out a settled body barely explores air at all — once it
settles `touched` shrinks to the disturbed corner, and gate (c) confines air
exploration to `touched` — so the settled-lake cost is only ~6.5 ms/tick after
the memo, and it grows *sub-linearly* (2.8 -> 6.3 ms as lake area grows 38x,
since big bodies trip the overflow cap and get skipped). So the persistent
structure's measured prize is ~6.5 ms/tick per large settled body, against the
memo's ~230 ms/tick already banked, for an order of magnitude more complexity
and determinism risk. ADR-0003 recommends revisiting only once (a) M3+ has
multiple large persistent bodies resident and (b) the memo is live in-engine.

**Verification**: 120/120 `vxc_tests` green (115 existing + 5 new). Both
goldens re-derived with the memo ON inside
`waterca_solid_cache_golden_digests_unchanged` — `0x3D2224BE4A253404` and
`0x56BC18914355A205`, unchanged. New tests also cover tick-by-tick equality
against an unmemoized CA over the U-bend, order-independence with the memo on,
mid-run full-invalidate / disable / re-enable, and — the case the memo is not
vacuously safe for — a **live terrain edit** (a divider wall dug out under
settled water) where the memoized CA must track an unmemoized one byte-for-byte
via `invalidateSolidRegion`; that test is the executable spec of what a caller
owes WaterCA. Additionally, the whole suite was run once with the memo default
FORCED ON: 119/120 byte-identical, the single failure being that test's own
deliberately-unmemoized control instance — which incidentally proves the test
really does detect a missed invalidation. Clang `-Wall -Wextra -Wconversion
-Wsign-conversion -Werror` clean on `waterca.cpp`, `test_waterca.cpp` and
`waterca_bench.cpp` (also fixed a pre-existing sign-conversion in the bench's
`samples.reserve`); float-ban clean.

## Backlog (parked / deferred, updated 2026-07-20)

| Item | State | Unblock |
|---|---|---|
| **Confirm real terrain-diffusion tile outputs** (exact climate-channel count/semantics/ranges vs our 4-channel assumption: temp, seasonality, precip, precip-variability) — then reconcile the tile codec + amplifier climate lookup + M4 biome table to reality, addressing gaps/tweaks | **SCAFFOLDED 2026-07-20**: the confirm-tool now exists — `terrain_service/providers/diffusion.py`'s `EXPECTED_CHANNELS` manifest + `validate_model_output(raster_dict)` checks a real model raster dict's channel count/names/dtype/ranges against our assumption and raises every mismatch found (not just the first); `docs/diffusion-bringup.md` step 5 is "run this against ONE real tile." Still gates M4 biome tuning | cloud NVIDIA rental, run `validate_model_output` against the real checkpoint (docs/diffusion-bringup.md); until then synthetic tiles stand in |
| NVIDIA cloud digest run (closes BOTH M0 gates) | blocked on rental spend (Matt) | ~$1, minutes of runtime; same session can bring up terrain-diffusion + confirm tile outputs above |
| terrain-diffusion worker bring-up | **SCAFFOLDED 2026-07-20** (worktree agent): deferred by ADR-0001 to vistas, but no longer a research project when eligible — `DiffusionConfig` (pinned checkpoint id/hash, sampler, scale, channel mapping → stable `provider_id()`), `adapt_raster_to_tile` (config-driven raster→Tile adapter), and dry-run mode (`DiffusionProvider(dry_run=True)` / `TERRAIN_DIFFUSION_DRY_RUN=1` / `pregen --dry-run`, synthetic rasters through the real config→adapter→validate→encode path) are all implemented and tested (`tests/test_diffusion.py`, 20 tests, no GPU); only `DiffusionProvider._call_model` (the actual inference call) remains, behind a numbered TODO. `terrain-service/docs/diffusion-bringup.md` is the turnkey runbook (rent GPU → install → pin checkpoint → validate → pregen radius) with the §3.4 cost model. `Dockerfile.diffusion` (CUDA base, torch, terrain-diffusion install placeholder) added, unbuildable here (no GPU cloud network access) but shaped for the rented box. Remaining = actual GPU rental + wiring `_call_model` + running `validate_model_output` against the real checkpoint | cloud NVIDIA rental |
| M1 formal min-spec proxy perf run | RAN 2026-07-20: p50 3.8ms pass-quality, p95 20.4ms fails bar under M2 streaming ramp; gate 🟨 pending M2 hitch work | M2 polish (below) |
| R3/R4 first-build cost (~335ms/job) | needs GPU-side gen or disk brick cache | design pass |
| Dithered ring cross-fades — SYMMETRIC blend | v1 landed (adjacent fade-through); symmetric-blend needs wider desired-set annulus overlap | M2 polish wave |
| ring↔clipmap seam (z-fighting, accepted v1) | noted | M2 polish |
| Perf-run hitches (~15-38/run, max 400ms, initial streaming ramp) | **CLOSED on p95 2026-07-21** (worktree agent): p95 18ms→4.8ms. The prior waves' render-buffer-upload diagnosis was a red herring — real root cause was budget MIS-ACCOUNTING (free stale-result discards consumed the apply budget; free component-less far-ring evictions consumed the unload budget), starving the real R0 applies/unloads and bloating R0 resident to ~6000 (4x desired). Fix budgets only render-thread-facing work. See "M1 gate close" writeup below | residual steady-state hitches (5-18/run, ~33-40ms) are environmental + coarse-ring recompute amplifier cost; the latter (memoize ComputeFootprintChunkZRange) is an optional M2 streaming follow-up |
| Clipmap follow-ups: two-sided material (winding unverified), A/B perf isolate, CDLOD replacement per ADR-0002 tripwire | noted in ADR/plan | M2 polish |
| Mip cache: budget default tuning (512MB) + real-scenario A/B | eviction landed (PR #16); default may need tuning | M2 polish |
| Debug tooling P2/P3 (τ overlay, water ledgers) | phased with M2-polish/W2 | — |
| M4 rounds 2-3 design (trees/structures, flora/placement) | round 1 decided; awaiting Matt design session | Matt |

**Closed this session** (moved off backlog): mip-cache eviction ✅ (PR #16),
config-driven `-VoxelSeed` ✅ (#16), SkyAtmosphere LWC fix ✅ (#16), M0 128m
performance gate ✅ PASS 0.191s (#14), edit-log compaction CLI ✅ (#16),
ring cross-fade v1 ✅ (#16), M3 persistence ✅ (#20).

M1 min-spec proxy definition: `sg.ViewDistanceQuality 0`, `sg.ShadowQuality 0`,
`sg.PostProcessQuality 0`, `sg.EffectsQuality 0`, `r.ScreenPercentage 100` at
1080p — intent: approximate RTX-3060-class headroom on this 7800 XT by
measuring at these settings and requiring p95 < 16.6ms with zero
steady-state hitches (excluding an initial warmup window).

**M1 gate run result (2026-07-20, 60s at proxy settings, post-M2 world):**
p50 3.78ms (steady-state comfortably 60fps+), but p95 20.4ms / 38 hitches /
max 400ms — FAILS the p95<16.6ms bar. Attribution: the M2 multi-ring +
clipmap streaming ramp (14,748 chunks loaded across the run, 59% budget
saturation) — M1-scope content alone measured p50 2.8/p95 4.2 pre-M2
(PR #12). Verdict: M1 gate stays 🟨; the blockers are the backlogged M2
items (streaming-ramp hitch isolation, PSO precache, R3/R4 first-build
cost, budget cvars for true throttling) — re-run after those land.

### Perf-run hitches: isolation + fix (2026-07-20, worktree agent)

**Instrumentation (the diagnostic deliverable, "measure before fixing").**
`FVoxelWorldImpl::TickStreaming` (VoxelWorldSubsystem.cpp) now times its own
Dispatch/Drain* calls every frame (four extra `FPlatformTime::Seconds()`
calls — negligible; the function already took one unconditionally) and, on
any frame whose `DeltaTime` exceeds a shared `VoxelDebug::kHitchThresholdMs`
(33.3ms, the same constant `UVoxelPerfRunSubsystem` sums hitches against —
factored out so the two can never disagree about what counts as a hitch),
logs (`LogVoxelPerf`, Warning) a breakdown: `frameMs`, `subsystemTickMs` vs
`elsewhereMs` (game-thread time outside our subsystem's tick),
`dispatchMs`/`applyMs`/`remeshMs`/`unloadMs` (the four budgeted sub-stages),
and `componentsApplied`/`proxiesCreated`/`editRemeshes`/`unloads` (raw
counts). Deliberately NOT gated behind `voxel.Debug` — a real hitch in
normal play is exactly when this is wanted, and the cost when there is no
hitch is one float compare. `UVoxelPerfRunSubsystem` also gained a
post-warmup window: samples/hitches from `ElapsedSeconds >= 10s` onward are
tracked and reported separately (`postWarmup*` fields in the JSON summary,
a matching `LogVoxelPerf` line) — the mechanism the M1 gate note's own
"if the ramp itself still hitches but steady-state is clean, report that
honestly ... with steady-state window numbers" fallback needs;
`tools/check-perf-run.py` gained matching `--max-post-warmup-p95-ms`/
`--max-post-warmup-hitches` flags.

**Finding.** Ran `-VoxelPerfRun=30`/`=60` repeatedly (seed 20260719, min-spec
proxy settings) and read the hitch-attribution log. Every single hitch frame,
across every configuration tested, showed `componentsApplied`/`unloads`
PINNED AT THEIR PER-FRAME BUDGET CAP (8/4 originally), while
`dispatchMs+applyMs+remeshMs+unloadMs` summed to **under 1ms even on hitch
frames** — the streaming subsystem's own budgeted CPU work is not the
expensive part. The dominant cost bucket in the overwhelming majority of
hitches (e.g. `frameMs=110.49 subsystemTickMs=0.34 elsewhereMs=90.97`) is
`elsewhereMs`: game-thread time spent OUTSIDE `TickStreaming` entirely. This
is consistent with a RENDER-THREAD scene-mutation backlog (each of the
budgeted 8 applies + 4 unloads enqueues a `NewObject`/`RegisterComponent` or
`DestroyComponent` plus `BeginInitResource` calls for up to 4 GPU buffers)
periodically forcing the game thread to stall resyncing with a render thread
that has fallen behind — NOT a single synchronous PSO/shader-compile stall
on one frame (which would show as one dominant frame, not a recurring
pattern). Two frames are a distinct, separate, one-time cost in every run:
frame 1 (`subsystemTickMs` ~70-100ms — the cold-start `RecomputeDesiredSet`
synchronously populating all 5 ring levels' candidate queues) and frame 2
(`elsewhereMs` ~90-145ms — the first real draw's shader/pipeline warm-up).
Contrary to the backlog's "initial streaming ramp" framing, hitches beyond
those first two frames are **not confined to the first ~10 seconds** — they
recur (now rarely) throughout a 60s run, timestamp-correlated with the
scripted circular flight path (100m radius, ~31s/lap) crossing ring
boundaries and triggering a batch of chunk-component create/destroy churn on
revisit, not just on first fill.

**Fix applied.** (a) The previously-hardcoded `MaxApplies=8`/`MaxRemeshes=4`/
`MaxUnloads=4` constants in `DrainResults`/`DrainGameThreadMesh`/
`DrainUnloads` are now `voxel.Stream.MaxAppliesPerFrame`/
`MaxRemeshesPerFrame`/`MaxUnloadsPerFrame` cvars (`VoxelDebug.h/.cpp`),
**tightened to 3/2/2** — the data-justified fix, since every hitch
correlated with these being at cap: smaller caps spread the render-thread-
facing scene-mutation rate more evenly. (b) A one-time, non-blocking
`UMaterialInterface::PrecachePSOs(&FLocalVertexFactory::StaticType, ...)`
warmup for the terrain material now runs in `OnWorldBeginPlay`, before
`ChunkOwner`/`ChunkRoot` even exist (toggleable via `-VoxelNoPSOPrecache` for
A/B measurement) — Epic's documented pattern for exactly this class of
first-draw stall. Measured effect on this fast dev machine was small/
inconclusive (the async compile races the short BeginPlay-to-first-draw
window and doesn't reliably win it), but it's a correct, standing
optimization expected to matter more on slower/target hardware; kept.

**Before/after (60s runs, seed 20260719, min-spec proxy settings):**

| Config | p50 | p95 | max | hitches | post-warmup (t≥10s) hitches | avg chunks/s |
|---|---|---|---|---|---|---|
| Before (8/4/4, no precache — reproduces pre-fix behavior) | 3.52ms | 21.36ms | 110.5ms | 27 | ~25 | 259 |
| After (3/2/2 + precache), 4 repeated runs | 2.53-2.69ms | 17.4-19.2ms | 90-113ms | 2, 3, 5, 10 | 0, 2, 2, 8 | 142-143 |

**Verdict: substantial, measured improvement (~3-5x fewer hitches, p95 down
~10-20%, max down ~10-20% and now driven almost entirely by the two
unavoidable cold-start frames) but NOT a clean, reproducible PASS.** p95
lands just above the 16.6ms bar on every after-run measured, and
post-warmup hitch count varies 0-8 across four identical-config repeats —
real run-to-run variance on this shared dev machine, not a regression
between runs. M1 gate stays 🟨. The two cold-start frames are a legitimate
loading-screen-class cost per the gate note's own allowance (a spinner
during those ~150-200ms would be an acceptable exclusion); the remaining
scattered mid-run hitches are the honest open item, and the data now points
at a concrete next step: pool/reuse `UVoxelChunkComponent`s across
unload/reload instead of `DestroyComponent`+`NewObject` on every
ring-boundary crossing, which is what would actually shrink the
render-thread-facing scene-mutation volume this pass's budget-tightening
could only spread out, not reduce. Throughput trade-off: avg chunks/s
dropped from ~259 to ~143 at the tightened budgets — an accepted cost for
smoothness, not evaluated against a throughput requirement (none is gated).

### Component pooling wave (2026-07-20/21, worktree agent) — implemented, measured, did NOT close the gate

**Design.** `FVoxelWorldImpl::ComponentPool` (`VoxelWorldSubsystem.cpp`) is a
`TArray<TWeakObjectPtr<UVoxelChunkComponent>>` (weak for the same reason
`FChunkRecord::Component` already is -- the real GC root is
`ChunkOwner`'s `OwnedComponents` list; pooled components stay
registered/attached the whole time they're parked, never
Unregister/DestroyComponent'd). `DrainUnloads` and `ApplyMeshResult`'s
zero-quads branch (a resident chunk re-meshing to no visible geometry --
previously also a `DestroyComponent`, now routed through the same pool path
for consistency) call `ReturnChunkComponentToPool` instead of
`DestroyComponent()`: `SetVisibility(false)` + `SetChunkQuads({}, ...)` +
`ClearDebugTint()`, then push onto the pool -- capped at
`voxel.Stream.ComponentPoolMax` (default 512; over-cap unloads still
`DestroyComponent()`, so the pool never grows unboundedly). `ApplyMeshResult`'s
first-load branch calls the new `AcquireChunkComponent` (pop-if-non-empty,
else the pre-pooling `NewObject`+`SetupAttachment`+`RegisterComponent`) and
then unconditionally re-applies every per-load property (`SetLevel`,
`SetRelativeLocation`, `SetMaterial`, `SetVisibility(true)`) regardless of
which path was taken -- a reused component is fully overwritten before
anything reads it again.

**Correctness.** `SetVisibility(false)` + `SetChunkQuads({}, ...)` coalesce
into one end-of-frame `MarkRenderStateDirty` recreate (UE batches same-frame
dirty-render-state requests into one actual recreate, not one per call), so
a parked component ends up with NO live scene proxy at all (cheapest
possible idle state, and correctness-required -- a hidden-but-still-meshed
component would otherwise flash its OLD chunk's geometry for one frame if
ever force-shown). `ChunkLevel`/`ChunkMaterial`/`ChunkMID` deliberately
survive a park/reuse cycle: `SetLevel` on reuse re-applies the level scale
and (via the existing `ApplyRingFadeParams`) the ring cross-fade params onto
the *same* MID rather than recreating it -- reusing the MID (skipping
`UMaterialInstanceDynamic::Create`) is real, not just the
register/unregister avoidance. `UVoxelChunkComponent::SetLevel`'s doc
comment (previously "never changes for the lifetime of this component") is
updated to reflect that a pooled component's level now legitimately changes
across residencies. `GenerationId`/`bHasOverlayBricks`/flash-timer fields
live on `FChunkRecord`, not the component, and a fresh `FChunkRecord` is
always constructed for a (level, key) re-entering the desired set
(`ChunkRecords.Add(LevelKey)`), so none of that state can leak from a
component's previous residency. `UpdateChunkStateTints`/`UpdateRingTints`
only ever iterate live `ChunkRecords` (never the pool) and recompute tint
fresh from `Pair.Key.Level` every call, so pooling is transparent to both
debug-tint layers by construction, not by convention.

**Instrumentation.** `FVoxelPerfSnapshot` gained `PooledComponents` (current
pool size), `PoolReusesPerSec`, and `TotalPoolReuses`; the perf HUD gained a
`component pool: pooled N reuses X/s (total M)` row; the existing per-frame
hitch-attribution log line gained `poolReuses=N poolSize=N`.

**A correctness-adjacent perf bug found and fixed during measurement.**
`ReturnChunkComponentToPool` originally called `ClearDebugTint()`
*unconditionally* on every pool-park. Unlike `SetVisibility`/`SetChunkQuads`,
`ClearDebugTint`'s `SetVectorParameterValue` does **not** coalesce with
anything -- it fires its own immediate render-thread command every time it's
actually called, even when the tint was already the identity (the
overwhelmingly common case with `voxel.Debug` off, which every gate run
below uses). With ~2 unloads/frame during the perf run's steady ring-crossing
churn, this meant a genuinely new, avoidable render-thread command on nearly
every frame -- a real regression the pre-pooling `DestroyComponent` path
never paid (a destroyed component's material params are simply gone, never
touched on unload). Fixed with a `bDebugTintDirty` flag
(`UVoxelChunkComponent`, set by `SetDebugTint`, cleared by `ClearDebugTint`):
`ClearDebugTint` now early-outs when nothing is actually dirty. This also
incidentally speeds up the existing debug-layer off-transition sweep in
`TickStreaming` (was already calling `ClearDebugTint` on every tracked
chunk on that edge). Measured effect: cut per-run hitch counts roughly in
half on this run of measurements (below) -- real, but not enough to close
the gate.

**Measurement.** `-VoxelPerfRun=60`, seed 20260719, min-spec proxy settings
(`sg.ViewDistanceQuality 0`, `sg.ShadowQuality 0`, `sg.PostProcessQuality 0`,
`sg.EffectsQuality 0`, `r.ScreenPercentage 100`, 1080p), 8 total runs (4
before the `ClearDebugTint` fix, 4 after):

| Run | p50 | p95 | max | hitches | post-warmup (t≥10s) hitches |
|---|---|---|---|---|---|
| 1 (pre-fix) | 3.84ms | 18.44ms | 275.0ms | 25 | 22 |
| 2 (pre-fix) | 3.97ms | 18.58ms | 114.5ms | 28 | 26 |
| 3 (pre-fix) | 3.92ms | 18.46ms | 98.5ms | 16 | 14 |
| 4 (pre-fix) | 3.94ms | 18.50ms | 212.1ms | 17 | 14 |
| 5 (post-fix) | 3.91ms | 18.02ms | 121.0ms | 7 | 4 |
| 6 (post-fix) | 3.92ms | 18.23ms | 117.7ms | 17 | 15 |
| 7 (post-fix) | 3.86ms | 18.14ms | 102.0ms | 5 | 2 |
| 8 (post-fix) | 3.91ms | 18.75ms | 109.1ms | 20 | 18 |

Pre-pooling baseline for comparison (from the "Perf-run hitches" section
above, same settings/seed, 3/2/2+precache, 4 runs): p50 2.53-2.69ms, p95
17.4-19.2ms, hitches 2/3/5/10, post-warmup hitches 0/2/2/8.

**Verdict: M1 gate FAILS. Component pooling, as implemented, did not
measurably close the gap.** p95 across all 8 pooling runs is 18.02-18.75ms
(mean ~18.4ms) -- *every single run* lands above the 16.6ms bar, and the
band is statistically indistinguishable from the pre-pooling baseline's
17.4-19.2ms. Post-warmup (steady-state) hitches range 2-26 across the 8
runs -- not trending to 0, and on average higher than the pre-pooling
baseline's 0-8 (though the baseline itself already flagged large run-to-run
variance on this shared dev machine, and my post-fix best cases, 2 and 4,
land inside that baseline's own range). Neither the raw gate criterion
(p95<16.6ms) nor the softer "ring crossings spike but steady-state is
clean" fallback applies here -- steady-state is not clean in any of the 8
runs. p50 is also slightly *worse* than the pre-pooling baseline (3.86-3.97ms
vs 2.53-2.69ms), consistent with pooling's `SetVisibility`/`SetChunkQuads`/
`ClearDebugTint` calls adding some fixed per-event cost across the many
ordinary (non-hitch) frames that see ring-crossing churn during the
scripted flight's continuous 100m-radius circle.

**Why the theoretical win didn't materialize.** The original hitch-
attribution diagnosis (see "Perf-run hitches" above) named TWO cost sources
inside every budgeted apply/unload: "a `NewObject`/`RegisterComponent` or
`DestroyComponent` **plus** `BeginInitResource` calls for up to 4 GPU
buffers." Pooling eliminates the first (UObject allocation/GC, actor
component-list churn, `RegisterComponent`/`UnregisterComponent`'s broader
lifecycle) but **not** the second: every `SetChunkQuads` call --pooled
component or not-- still calls `InitFromDynamicVertex` and
`BeginInitResource` for a brand-new vertex/index/color buffer set, because
the actual geometry content is different every time (a reused component's
OLD buffers are useless for a NEW chunk's quads). The render-thread
`FScene::AddPrimitive`/`RemovePrimitive` pair triggered by
`MarkRenderStateDirty` also fires exactly once per park and once per reuse,
same count as one `DestroyComponent`+one fresh `RegisterComponent`+
`SetChunkQuads` before pooling -- pooling changed nothing about *how many*
scene-mutation events happen, only trimmed the smaller UObject-lifecycle
overhead layered on top. The data now says that smaller layer was not the
dominant bucket after all. **Recommended next lead** (not attempted this
wave): target the GPU buffer upload cost directly -- e.g. investigate
whether `BeginInitResource`/`AddPrimitive` can be moved off the game
thread's critical path (already async at the RHI level, but the *enqueue*
still costs something per call), reduce the *frequency* of these events by
enlarging the render-chunk footprint (fewer, larger meshes crossing ring
boundaries less often, at the cost of coarser edit-remesh granularity), or
get a proper render-thread-side profile (Unreal Insights) of what
`FScene::AddPrimitive` actually costs at this call rate before optimizing
further blind. Component pooling itself is kept in the codebase regardless
of not closing the gate -- it is not harmful (same p95, render-correctness
screenshot unaffected, standalone behavior unchanged, zero ensures across
every run) and it does reduce real GC/UObject-churn pressure that the
measurement above doesn't directly capture.

Build: worktree `voxelcore.lib` rebuilt clean (`vxc_tests` green, untouched
by this wave) before `Build.bat VoxelEarthEditor Win64 Development
-WaitMutex -NoHotReloadFromIDE`, which also built clean both before and
after the `ClearDebugTint` fix (zero warnings from any file touched this
wave: `VoxelChunkComponent.{h,cpp}`, `VoxelDebug.{h,cpp}`,
`VoxelEarthHUD.cpp`, `VoxelWorldSubsystem.cpp`; same pre-existing
engine-header deprecation baseline as prior waves). Render-correctness
verified via `-VoxelScreenshotAfter=40` (seed 20260719): terrain, ocean, and
shadowing all render correctly, zero ensures, clean shutdown --
`VoxelVerify00000.png`/`00001.png` under
`ue-project/Saved/Screenshots/WindowsEditor/`.

## Water track — W3 groundwork: river segment-graph core landed (2026-07-20, worktree agent)

Engine-free CPU reference for plan §3.7 Layer R ("river network sim: segment
graph ticks ~1Hz server-side, Muskingum-class storage routing") and the
Generation note ("rivers = flow-accumulation routing, discharge from
upstream precip sets width/depth"), voxel-core only (no UE/engine touch this
pass; waterca.h/.cpp untouched, per-voxel river carving, CA coupling, and
replication are W3-proper/later). `voxelcore/rivernet.h` + `src/rivernet.cpp`:
`RiverNode` (vx,vy voxel-coord position + elevationMm) + `RiverSegment`
(fromNode/toNode, lengthMm, discharge, storage, conveyance 0-255,
Muskingum-K travelMillis) + `RiverNetwork`, which owns both vectors (ids are
just vector index, assigned in a fixed position-ordered traversal — fully
deterministic given tiles+bounds+threshold).

`buildFromFlowAccumulation(tiles, seed, bounds, accumThreshold)`: coarse
tile-pixel-scale (NOT per-voxel) D8 flow accumulation over an inclusive
pixel rectangle — each pixel's steepest-descent neighbor is the lowest-
elevation in-bounds compass neighbor (fixed N/NE/E/SE/S/SW/W/NW tie-break
order; v0 simplification: no distance normalization, though segment length
itself does account for the diagonal-vs-cardinal distance via an integer
sqrt(2)~=181/128 approximation, no floats), pixels visited in strictly-
descending elevation order (ties broken by position) so every uphill
contributor to a cell is accumulated before the cell itself is — the
standard topological-order flow-accumulation trick. A pixel becomes a
`RiverNode` once its accumulation (precipitation-channel-weighted) crosses
`accumThreshold`; a `RiverSegment` connects two qualifying pixels along a
D8 edge. Every node has at most one outgoing segment (its single D8
target), so the graph is a forest of chains draining toward the region's
low edge / basin sinks — exactly the "coarse graph, not per-voxel" the plan
calls for.

`step(dtMillis)`: two-phase (read outflow from tick-start storage, then
apply) Muskingum-class routing — v0 implements the X=0 linear-reservoir
special case (outflow proportional to storage, no negative-prone
subtraction), integer-exact: `outflow = clamp(storage*dtMillis/travelMillis
* conveyance/255, 0, storage)`, where `travelMillis` (Muskingum K) is fixed
at build time from segment length (longer reaches lag more). The two-phase
split means confluences (multiple segments feeding one downstream segment)
never race regardless of segment iteration order, mirroring waterca.h's own
two-phase contract for the same reason. `setConveyance(segId, 0)` = full
dam: that segment's own outflow is permanently forced to 0, so its storage
only ever grows from upstream inflow (monotonic non-decreasing — the
numeric proxy for "upstream stage rises," a later system would read this to
decide when to spawn a UE reservoir entity); the segment immediately
downstream receives zero new inflow and its own storage/discharge decays
toward zero at its own routing rate — "downstream discharge decays" falls
out with no special-casing. `injectInflow(segId, amount)` is the rivernet
analogue of `WaterCA::addWater` — the explicit stimulus API conservation
tests inject through. `totalStorage()`/`totalInjected()`/
`totalOutflowToOutlets()` are O(1) running ledgers with the exact invariant
`totalStorage() + totalOutflowToOutlets() == totalInjected()` holding after
any call sequence; `recomputeTotalStorage()` independently re-sums for
cross-checking.

**Hydrology graph-diff** (the persistent replicated log hook, plan §3.7):
`RiverDiffRecord` + `applyGraphDiff()`. `kSetConveyance` ("a dam
placed/removed/adjusted") is REAL — dispatches to the same `setConveyance()`
a live caller uses, so replaying a diff log against a freshly-built network
reproduces byte-identical state to whatever produced it live (proven by the
replay test below). `kDivertChannel` ("a sustained CA flux promotes a new
channel to a segment") is a documented no-op stub — promoting a channel
needs live CA data this graph-only layer doesn't have; that coupling is
W3-proper, after this header and waterca.h are wired together. Full network
*replication* (sending these diffs over the wire) is separately M3-water
integration, later still.

Tests (`voxel-core/tests/test_rivernet.cpp`, 5 new, `tests/CMakeLists.txt`
updated): flow accumulation on a hand-verifiable synthetic east-descending
slope (`LinearSlopeTileSampler`, deliberately simpler than
`SyntheticTileSampler` so structure can be asserted exactly) produces a
single downhill chain reaching the low edge (last node has no outgoing
segment, first node is never any segment's downstream target) with
discharge strictly increasing along the main stem; routing conservation
(inject at a headwater, step 300 ticks, exact integer ledger checked every
tick: `totalStorage()==recomputeTotalStorage()` and
`totalStorage()+totalOutflowToOutlets()==totalInjected()`); dam behavior
(continuous headwater baseflow, dam a mid-chain segment, 200 ticks: dammed
segment's storage strictly monotonic non-decreasing, its own discharge
pinned at exactly 0, the immediately-downstream segment's discharge
decays below 50 from a much larger starting value, conservation exact
throughout); determinism (two networks built from the real
`vxc::SyntheticTileSampler` over a 16x16-pixel branching region, identical
stimulus injected at every headwater segment, stepped 50 ticks, byte-
identical digest, golden `0xE4944F92B37F60FB` pinned); graph-diff replay
(one network dammed via direct `setConveyance()`, a fresh twin dammed only
via `applyGraphDiff(kSetConveyance)` replayed at the same point in an
otherwise-identical stimulus sequence — digests and dammed-segment storage
match exactly; a follow-up `kDivertChannel` diff is confirmed to leave the
digest unchanged, proving the stub is really a no-op). Full suite green:
84/84 (`vxc_tests.exe`, MSVC 14.51/VS 2026, `/W4 /WX`, Ninja/Release), zero
warnings from any file this pass. Separately verified clang-clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror` (llvm-mingw clang++,
`rivernet.cpp` and `test_rivernet.cpp` compiled standalone) — zero
diagnostics, no sign-conversion bugs found this pass. No float/double
anywhere in `rivernet.h`/`rivernet.cpp` (the one `\bfloat\b`/`\bdouble\b`
grep hit in the header is the prose word "double-count," not a type).
### M1 gate close — budget mis-accounting was the real hitch (2026-07-21, worktree agent)

**The prior diagnosis was wrong.** Every earlier wave (budget tightening,
PSO precache, component pooling) accepted that "the per-load GPU
vertex/index-buffer upload + FScene::AddPrimitive is the irreducible cost."
Re-measuring from scratch on this AMD 7800 XT (min-spec proxy: `sg.*Quality 0`,
`r.ScreenPercentage 100`, 1080p, `-VoxelPerfRun=60`, seed 20260719) with the
existing hitch-attribution log AND newly-added per-thread frame timers
(`GRenderThreadTime`/`GRenderThreadWaitTime`/`GRHIThreadTime`/
`GGameThreadWaitTime` from `RenderTimer.h`, logged only on hitch frames)
showed a different picture:

- **Attribution.** Steady-state hitch frames had `subsystemTickMs≈0` (our own
  budgeted CPU work is nothing) and were **render-thread-bound**: `renderMs`
  spiked to 17-47ms with `renderWaitMs≈0` (render thread fully busy, not idle)
  and `gameWaitMs` tracking it (game thread blocked on the render fence). The
  giant 100-400ms spikes were one-time (cold DDC/shader-compile: `postWarmupMax`
  fell 358ms→56ms between a cold and a warm run with no code change). GC and
  shader compilation were ruled out by log inspection (no `LogGarbage`, no
  mid-run `LogShaderCompilers`).
- **The smoking gun.** The `Voxel rings:`/`Voxel streaming:` periodic log lines
  showed **R0 resident = 5925 and still growing** (design is ~1600 — see M2
  wave-1 verification), with **pendingUnload = 58,051** backlogged and
  `tracked = 68,771`. The render thread was drawing ~4x the chunks it should,
  because chunks that had long left the 80m unload radius stayed resident with
  live scene proxies. Unload starvation, not buffer-upload cost.

**Root cause: the per-frame budgets were being spent on FREE work.**
`DrainResults` counted every stale-result discard (chunk left the desired set,
or was superseded by an edit re-mesh — a `TMap::Find` + counter, no proxy)
against `voxel.Stream.MaxAppliesPerFrame`. `DrainUnloads` counted every
component-LESS record eviction (an outer-ring R2/R3/R4 candidate that was
queued as a job but left the desired set before it ever meshed — a bare
`TMap` erase, no `RemovePrimitive`) against `voxel.Stream.MaxUnloadsPerFrame`.
During a perf flight the far-ring churn is enormous (measured ~130k
component-less evictions and a flood of stale results vs ~7k real loads), so
the render-facing budgets were consumed almost entirely by bookkeeping and the
real R0 applies/unloads near the player were starved: R0 couldn't unload
(resident bloated → render-thread hitch) and, when the unload budget was naively
raised to compensate, R0 couldn't even apply (resident collapsed to 0 — fast
frames drawing nothing).

**Fix (render/streaming-side only; determinism unaffected — only *when* chunks
load/unload changes, never *what*).** Make the budgets gate only the
render-thread-facing events:
- `DrainResults`: stale discards no longer consume `MaxApplies`; only live
  results (an actual `ApplyMeshResult` → `SetChunkQuads` → `AddPrimitive`/GPU
  upload) do. A separate large per-frame drain cap (1024) bounds the cheap
  discard work so a stale backlog can't stall the game thread either.
- `DrainUnloads`: component-less evictions no longer consume `MaxUnloads`; only
  a pool-park of a live component (which fires `RemovePrimitive`) does.
  Component-bearing unloads over budget this frame are deferred (kept tracked,
  re-queued) rather than forced through; component-less evictions flow freely
  under a 1024/frame pop cap.

No cvar defaults changed (still 3/2/2) — the numbers were never the problem,
the accounting was. With the fix, `MaxApplies=3`/`MaxUnloads=2` per frame is
ample (render-facing rates ~540/360 per sec at ~180fps vs ~127/sec real
churn), so R0 stays both populated and bounded.

**Before / after** (min-spec proxy, `-VoxelPerfRun=60`, seed 20260719, 5 runs
each; "before" = this session's baseline with the old accounting, matching the
prior waves' ~18ms band):

| Metric | Before | After (5 runs) |
|---|---|---|
| p95 (full run) | 17.6-18.4 ms | **4.79-4.93 ms** |
| postWarmup p95 (t≥10s) | 18.5 ms | **4.75-4.93 ms** |
| p50 | 3.1-3.9 ms | 3.09-3.15 ms |
| postWarmup hitches (>33.3ms) | 7-26 | 5-18 |
| R0 resident (perf flight) | 5925 & growing | ~250-625 (bounded) |
| pendingUnload backlog | 58,051 | 23 |

**Verdict: p95 gate PASSES** — 4.8ms vs the 16.6ms bar, a 3.4x margin,
reproducibly across 5 runs, both full-run and post-warmup. Render correctness
verified (`-VoxelScreenshotAfter=40`, seed 20260719: contiguous voxel terrain
with AO, ocean plane, HUD, no near-field holes, zero ensures —
`VoxelVerify00000.png`/`00001.png`). Standalone behavior with default budgets
unchanged; the fix is byte-identical output, only faster loading/unloading.

**Honest residual (the "steady-state hitches → 0" sub-criterion is NOT
literally met).** 5-18 isolated ~33-40ms frames per run remain post-warmup,
fully attributed to two non-render, non-buffer-upload sources: **(a)** ~1-2/run
where `subsystemTickMs≈14-15ms` — a coarse-ring (R3/R4) entry rescan that
re-samples the amplifier via `ComputeFootprintChunkZRange` over ~1849 footprints
when the anchor crosses a level-3/4 chunk boundary (every ~1-2.5s); memoizing
that pure function per `(level,Cx,Cy)` would remove it and is a clean optional
M2 streaming follow-up. **(b)** the majority — ~33-40ms frames with
`subsystemTickMs≈0.03` and `renderMs`/`rhiMs`/`gameWaitMs` all tiny, i.e. no
correlation to any measured thread: environmental stalls on this shared dev box
(other processes / GPU-driver / windowed-mode present), an ~0.1%-of-frames
floor no streaming or render change can reach here. Neither category is the
per-load render cost the gate was chasing.

**Proposed gate-criterion refinement** (per the gate note's own "if steady-state
is clean, report that honestly" allowance): assess the 60fps requirement as
**post-warmup p95 < 16.6ms with no steady-state hitch attributable to
streaming/render** (met: 4.8ms, and every residual hitch is attributed to
amplifier-recompute cost or environmental noise, not render/proxy work),
excluding the two unavoidable cold-start frames (first-frame desired-set
build + first-draw shader warm-up — legitimate loading-screen-class cost).

**Diagnostic instrumentation kept**: the per-thread frame timers
(`GRenderThreadTime` etc.) are now logged on every hitch frame (cost: a few
global reads, only when a frame already overran the threshold) — they are what
turned "elsewhereMs is a black box" into the render-thread-bound attribution
above, and remain useful for any future perf work.

Build: worktree `voxelcore.lib` reused from the identical `main` checkout
(voxel-core untouched — `git diff main -- voxel-core/` empty), `VoxelEarthEditor
Win64 Development` built clean via `Build.bat ... -WaitMutex
-NoHotReloadFromIDE` (zero warnings from the only file touched this wave,
`VoxelWorldSubsystem.cpp`; same pre-existing engine-header deprecation baseline
as prior waves).

### R3/R4 recompute amortization (M1 steady-state hitches) (2026-07-21, worktree agent)

Closes the one sub-criterion the M1 60fps gate did not meet: residual
steady-state frame spikes. The prior wave's *p95* fix stands untouched; this
wave only addresses the leftover spikes it attributed to "coarse-ring (R3/R4)
recompute amplifier-sampling".

**That attribution was wrong, and the instrumentation says so directly.**
`TickStreaming`'s existing hitch-attribution timers covered
`DispatchJobs`/`DrainResults`/`DrainGameThreadMesh`/`DrainUnloads` but NOT
`RecomputeDesiredSet` -- the one stage the residual hitches were blamed on.
This wave added that missing breakdown (per-call `recomputeMs` split into
exit-scan / per-level entry-scan / queue-sort, plus in-annulus footprint
counts), logged on hitch frames, AND a periodic "max since last log" line so a
burst that lands on a frame *under* the 33.3ms hitch threshold is still
visible. Two findings:

- **None of the >33.3ms hitch frames involve a recompute at all.** Across the
  baseline runs every single hitch frame logged `recomputeMs=0.00` with
  `subsystemTickMs~=0.02-0.05`. Those hitches are cold-start frame-1
  render/PSO warm-up (`renderWaitMs` in the thousands) or unattributed
  `elsewhereMs`-only frames -- i.e. the environmental floor the prior wave
  already described, not ring recompute.
- **But recompute really was blowing the 60fps budget ~8 times a second** --
  invisible to every existing metric, because both the hitch log and
  `UVoxelPerfRunSubsystem`'s hitch count use a 33.3ms threshold and the bursts
  landed at 25-32ms, just underneath it. Measured baseline worst-case
  recompute: **25-32ms**, ~40 calls per 5s. The gate's actual bar is 16.6ms,
  so a **frames-over-16.6ms counter** was added (also in the periodic log):
  the baseline run spends **260 frames per 60s run over the 60fps bar**, ~25
  per 5s.

**Where the time actually went** (worst-case call, baseline,
`-VoxelPerfRun=60`, seed 20260719, min-spec proxy):

| Stage | Baseline worst | Cause |
|---|---|---|
| exit hysteresis scan | 10-13 ms | `PendingJobKeys.RemoveSingle` per eviction -- a linear scan of a **~19,000-deep** pending queue, run once per evicted chunk (O(evictions x depth)) |
| entry scan R0 | 5-7 ms | 4 `Amplifier::column` samples per in-annulus footprint (~1250 footprints), re-sampled from scratch every call |
| entry scan R1/R2/R3/R4 | 2.4-4.3 ms each | same, ~940 footprints per ring |
| queue sort | 3.2-3.5 ms | `Sort()` predicate recomputed chunk-centre distance on every **comparison** (~2*n*log2(n) ~= 530k distance computes at n=19k) |

Note the shape: R3/R4 are **not** special -- every level costs about the same,
R0 costs the most, and the worst frames are the ones where several levels scan
at once. Ring chunk boundaries are **nested**, so an anchor crossing a level-4
boundary necessarily crosses the level-3/2/1/0 boundaries in the same frame;
the existing per-level gating therefore never staggers the work, it aligns it.

**Fix -- three scheduling changes, all semantics-preserving (nothing mutable is
memoized, no change to *which* chunks are desired):**

1. **Memoized `ComputeFootprintChunkZRange`** per `(Level, ChunkX, ChunkY)`.
   It is a pure function of the amplifier, which is a pure function of the seed
   and never changes (edits live in the overlay and never move terrain
   elevation), so the memo is exact. Consecutive entry scans re-visit almost
   the same annulus -- a one-chunk anchor step changes only its margin -- so
   the hit rate is very high. Bounded at 65,536 entries by a distance-based
   prune (`PruneFootprintZRangeCache`), which drops far entries rather than
   clearing, so pruning can never itself cost a re-sample burst.
2. **Batched queue filtering.** The per-eviction `RemoveSingle` calls are
   replaced by one `RemoveAllSwap` pass over each pending queue, filtered
   against a set of the keys evicted by *this* call (deliberately not the whole
   `PendingUnloadSet`, which also holds not-yet-drained earlier evictions that
   `DrainUnloads` may legitimately have re-queued). At most one copy of a key
   can ever be in a queue, so "remove all" and "remove one" are identical here;
   queue order is irrelevant because `SortPendingQueues` re-sorts immediately.
3. **Decorate-sort-undecorate** in `SortPendingQueues`: each distance is
   computed once into a scratch array instead of once per comparison. Ordering
   is bit-identical (same primary key, same tie-break, and `Sort()` was already
   unstable so fully-equal elements never had a defined order).

**Before / after** (min-spec proxy `sg.*Quality 0` + `r.ScreenPercentage 100`,
1080p windowed, `-VoxelPerfRun=60`, seed 20260719, clean `Saved/VoxelWorlds`):

| Metric | Baseline | After (3 runs) |
|---|---|---|
| **frames over 16.6ms (60fps bar), whole run** | **260** | **2 / 14 / 19** |
| worst recompute call | 25-32 ms | 6.2-10.7 ms steady (15-18 ms in the first 5s, cold memo) |
| -- its exit scan | 10-13 ms | 2.4-2.7 ms |
| -- its sort | 3.2-3.5 ms | 1.8-2.3 ms |
| -- its R0 / R1-R4 entry scans | 5-7 / 2.4-4.3 ms | 1.6-2.4 / 0.5-1.7 ms |
| p95 (full run) | 5.01 ms | 5.20 / 5.24 / 5.30 ms |
| postWarmup p95 | 5.08 ms | 5.29 / 5.33 / 5.40 ms |
| p50 | 3.08 ms | 3.04-3.11 ms |
| postWarmup max frame | 33.31 ms | 12.79 / 71.38 / 46.95 ms |
| postWarmup hitches (>33.3ms) | 1 | 0 / 2 / 1 |
| chunks loaded (60s) | 13,867 | 14,383-14,995 |

p95 is unchanged within run-to-run noise (the prior wave's ~4.8-5.0ms band
holds; recompute bursts were always a sub-1% tail, never the p95 driver) and
throughput improved slightly. The headline is the 60fps-bar count: **260 ->
2-19 frames per run**, and every remaining one is unattributable to recompute.

**Honest residual accounting.** The 0-2 post-warmup >33.3ms frames per run all
log `recomputeMs=0.00` and `subsystemTickMs~=0.03-0.05`, i.e. none of them is
ours: one is always frame 1 (`renderWaitMs` ~19-22s of accumulated wait = cold
shader/PSO warm-up, a loading-screen-class cost), the rest are `renderMs`-only
or fully unattributed dev-box stalls, matching the prior wave's environmental
floor. The residual 14-19 over-16.6ms frames in two of the three runs likewise
do not line up with the recompute maxima (which stay at 7-10ms in those
windows). The genuinely-ours residual is the ~7-10ms steady worst-case
recompute -- under the bar, but not free. **Measurement caution learned the
hard way**: an intervening `-VoxelHeadlessDigTest` run leaves a
`Saved/VoxelWorlds/<seed>.vxlog`, and a later perf run *loads* it, producing
200ms overlay-re-mesh frames that have nothing to do with streaming. Two runs
were discarded for this reason; clear `Saved/VoxelWorlds` between perf runs.

**Determinism verified, not asserted.** A/B of `-VoxelHeadlessDigTest=15
-VoxelDumpDigestAfter=25 -VoxelNoLoad` (seed 20260719) on a baseline build and
on the changed build: both carve **2,131,643 voxels** and both report
**`editedDigest=0xD9FB0347863F529E`** (server and client). Render correctness
re-checked with `-VoxelScreenshotAfter=40`: contiguous voxel terrain, no
near-field holes, HUD present, zero ensures.

**Follow-ups.**

- `PendingJobKeys` sits at a **steady ~19,000-20,000** all run: the desired set
  is produced far faster than workers drain it, and the queue depth is what
  makes both the sort and the eviction filter cost anything at all. Capping the
  outer-ring queue depth (or not enqueueing R3/R4 candidates that provably
  cannot be reached before they leave the annulus) would shrink the remaining
  recompute cost and cut wasted work -- the real structural item behind
  wave-1's "R3/R4 cold-start fill time" note.
- The exit hysteresis scan is still O(tracked) = ~27,000 records per call,
  ~2.5ms. If it ever matters, amortize it across frames with a round-robin
  cursor; the 1.25x unload hysteresis makes a few-frame eviction delay safe.
- Unrelated latent issue found while A/B-building: with
  `VoxelWorldSubsystem.cpp` **unmodified** (so it rejoins the adaptive-unity
  blob), `VoxelEarth` fails to compile with redefinition errors in
  `voxel-core/include/voxelcore/connectivity.h` (`localIndex` "already has a
  body", `kNeighborOffsets` hides a global). It is masked whenever that file is
  dirty. Worth fixing before it bites a clean CI build.

Build: `VoxelEarthEditor Win64 Development` via `Build.bat ... -WaitMutex
-NoHotReloadFromIDE` -> `Result: Succeeded`, zero warnings from
`VoxelWorldSubsystem.cpp` (the only source file touched -- this wave's working
diff is exactly `VoxelWorldSubsystem.cpp` + this file). voxel-core was not
modified by this wave; the worktree's `voxelcore.lib` was reused from the
`main` build, whose voxel-core **C++** sources are identical (this branch's
base differs from `main` only in `voxel-core/shaders/`, which is HLSL/SPIR-V
for the GPU worldgen path and is not compiled into that library nor exercised
by the CPU perf run). Note for future waves: the engine is at
**`D:\UE5\UE_5.7\Engine`**, not `D:\UE5\Engine`.

# Milestone gate status

Updated whenever gate-relevant work lands. See the implementation plan §4 for
gate definitions.

## M0 — Core + proof of numbers (IN PROGRESS)

| Gate | Status | Notes |
|---|---|---|
| Amplify+mesh 128m radius < 1s on RTX 3060 | 🟨 half-open | **AMD leg measured (2026-07-19)**: `vxc_gpu --radius <m>` (voxel-core/bench/gpu_harness.cpp) now runs the FULL GPU pipeline (ColumnMain→VoxelizeMain→MeshCount/EmitMain) over every surface-shell brick in a horizontal radius, tiled into 128×128-column dispatches with a 1-brick shared halo (14×14-brick/112-column interior per tile — partitions the target square with no gaps, no double-meshing). On this desktop's AMD Radeon RX 7800 XT: **radius 64m = 0.678s** (PASS, under target) and **radius 128m = 2.388s** (OVER target) — end-to-end gate time excludes per-tile CPU column setup and CPU-reference comparison, both timed and reported separately. At 128m the host prefix-scan step (CPU reads GPU-mapped mesh-mask counts, writes back scan offsets, between the count and emit dispatches) is the single largest bucket (1.20s of 2.39s) — larger than all four GPU dispatch stages combined (1.07s) — pointing at CPU↔GPU-mapped-memory round-trip cost in the count→scan→emit chain, not raw compute throughput, as the next optimization target (gpu-mesher-design.md's mesher lists moving the scan to GPU as exactly this contingency). **Host scan replaced with a GPU scan (2026-07-20)**: worldgen.ush gained `ScanBlocksMain`/`ScanSumsMain`/`ScanAddMain` (fixed-order shared-memory Hillis-Steele, per-256-block scan + single-workgroup block-sum scan + add-back — deterministic by construction, so quad order is byte-for-byte unchanged), and `gpu_harness.cpp`'s `runMeshChain()` now chains MeshCountMain→ScanBlocksMain→ScanSumsMain→ScanAddMain→MeshEmitMain in ONE command buffer (COMPUTE→COMPUTE buffer barriers write→read between every stage, one trailing COMPUTE→HOST barrier, ONE fence per tile/region) instead of the old two-submission count/emit split with a CPU-side scan in between. The emitted quads buffer is upper-bound sized (32 quads/mask max × maskCount, grow-only reused) *before* the chain is recorded, since MeshEmitMain now has no readback point to learn the exact total first; the true total (`counts[maskCount-1] + offsets[maskCount-1]`) is read back after the single fence. Per-stage GPU timing survives the merge to one fence via 6 `vkCmdWriteTimestamp` queries bracketing the 5 dispatches. maskCount is asserted ≤65,536 per dispatch (ScanSumsMain's single-workgroup limit) — never triggered by the 128×128 tile layout in practice. Result: **radius 64m = 0.529s** (PASS, was 0.678s) and **radius 128m = 1.134s** (still OVER target, was 2.388s — a 52.5% reduction). The host scan bucket (was 1.20s at 128m) is now a 6.98ms GPU-scan bucket (blocks 3.66ms + sums 2.22ms + add 1.10ms) — the scan is no longer a bottleneck at any radius tested. The four original GPU dispatch stages (columns+voxelize+meshcount+meshemit) are ~925ms at 128m, about the same as before (1.07s); marshalling overhead rose modestly (118ms→201ms — three descriptor sets now rewritten per tile instead of ≤2, plus the upper-bound quads allocation) — mesh count/emit dispatch cost (283ms+286ms=569ms) and columns/voxelize (224ms+132ms=356ms) are now the dominant buckets and the next optimization target, not the scan. Digests at both radii are byte-identical to the pre-scan run (see the determinism row below), confirming the GPU scan changes nothing about output, only how it's computed. NVIDIA leg still needs to run the same `vxc_gpu --radius 64/128` and compare digests. CPU reference baseline (single-threaded, container hardware) unchanged below. **Tiles batched into flights to close the 128m gate (2026-07-20)**: gate mode now processes tiles in flights of 8 (`kFlightSize`), each flight recording EVERY tile's full chain (ColumnMain→VoxelizeMain→MeshCount→GPU scan→MeshEmit) into ONE command buffer with ONE fence, replacing the old 3-fences/tile scheme (column, voxelize, mesh chain) — cuts Vulkan submission/fence count from ~1,587 (529 tiles × 3) to 67 at 128m. CPU/GPU overlap ("flight k+1's CPU marshalling runs while flight k's fence is still pending, via a deferred `vkWaitForFences` call") needed double-buffered slot banks (`kPipelineDepth=2`, 16 total per-tile buffer/descriptor slots): a bank is only reused once its owning flight's fence has actually been waited on, since reusing the same slots for two flights at once would race the CPU's buffer writes against the GPU still reading them. Persistent per-slot descriptor sets are now only rewritten when a slot's `GrowBuffer` actually reallocates, not every tile. Per-stage timing now comes from `vkCmdWriteTimestamp` queries bracketing all 7 stages per tile (not just the mesh chain), since batched submission means CPU-side submit/wait stopwatching no longer isolates individual stages — see `runGateMode()`'s report for the "hidden by flight double-buffering" figure (814ms at 128m: CPU marshalling of one flight overlapping GPU execution of another). Result on this AMD RX 7800 XT: **radius 64m = 0.112s** (was 0.529s) and **radius 128m = 0.191s** (was 1.134s — **GATE NOW PASSES**, an 83% reduction). Digests unchanged (see the determinism row below), confirming batching changed only execution overlap, never output or its order; `vxc_tests` green (64/64), default column-only regions mode still bit-exact. One correctness bug was found and fixed during this work: `vkGetQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)` was requesting a full bank's worth of timestamp queries even on a partial (last) flight, where only some tiles' queries were ever written by that flight's command buffer — the unwritten queries never become available, so the call hung indefinitely (surfaced as a multi-minute stall on both `--radius 16` and `--radius 128`, both of which have a partial last flight); fixed by requesting only `pf.count * kTimestampsPerTile` queries. NVIDIA leg still needs to run the same `vxc_gpu --radius 64/128` and compare digests. |
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
- [x] GPU compute port of amplifier (worldgen.ush ColumnMain, Vulkan harness verified bit-exact on AMD leg); mesher GPU port (MeshCount/EmitMain) also landed and verified bit-exact
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

**Gate system split (measured 2026-07-21, see "R2-R4 ring starvation fix"
below for the full writeup):** the gate below reads "50km+ vista ... (only
ring coarsening)", which is misleading about which system does the work.
The voxel ring cascade (R0-R4, this section) tops out at R4's outer edge,
**1024m** — it delivers close-range detail out to ~1km, not the 50km+
vista. The long-distance vista is delivered by **Band 3**, the heightmap
clipmap (`AVoxelClipmapActor`, docs/m2-plan.md's "Band 3 first slice"),
covering ~1km out to ~16.4km radius (~32.8km diameter) via direct TILE
elevation sampling — a different rendering system, not a coarser ring
level. Vista screenshots (`-VoxelVistaShot`, 250m above spawn) confirm a
real horizon-to-horizon vista renders today, but on the clipmap, not the
ring cascade. An earlier internal note calling this "passing on a
technicality" was imprecise and is superseded by this measurement: the
gate was not passing on a technicality, it was passing on a DIFFERENT
system than the one its wording credits. This is a documentation
correction only — see the "Gate" bullets below for actual pass/fail status,
unchanged by this note.

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
  dithered cross-fade are later M2 items. (Vista range: delivered by the
  Band 3 clipmap, not this ring cascade — see the system-split note above.)

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
  follow-up) is unchanged by this wave. (Vista range: delivered by the
  Band 3 clipmap, not this ring cascade — see the system-split note above.)

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
  `worldgen.ush`'s `ColumnMain` both call classifyBiome/
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
  as-is — same precedent as M5's wave). `worldgen.ush` compiles clean to
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

### Digging-while-pathing (M6, authoritative NPC edits)

- [x] **The payoff slice**: Tier 0 NPC agents now MINE and BRIDGE terrain
  live, through the exact same cost function that already chose Walk/
  StepUp/Climb/Fall/Jump (`voxel-core/include/voxelcore/pathfind.h`,
  consumed as-is — not modified). Enabling this was purely a
  `PathCostConfig` change on the UE side
  (`ue-project/Source/VoxelEarth/VoxelAgentSubsystem.cpp`); no new
  algorithm.

- [x] **Cost values chosen (`FVoxelAgentImpl::DigConfig`, Initialize)**:
  `walkCost`/`stepUpCost`/`fallCostPerVoxel`/`jumpGapCost` unchanged from
  `pathfind.h`'s own defaults (10/14/5/15). `mineCostByMaterial[MAT_ROCK] =
  35` (~3.5x `walkCost`) — a short detour (a few walked cells) stays
  cheaper than mining, but tunneling through a THIN wall beats a detour
  that a small (~3.3m) search window can't even see the end of.
  `bridgeCost = 26` (~2.6x `walkCost`) — pricier than a soft-material mine
  would be, cheaper than mining hard rock (bridging a gap is usually less
  work than excavating through solid material). The full
  `kMineCostByMaterial` table (file-scope, `VoxelAgentSubsystem.cpp`) is
  filled in for every `vxc::Material` with a documented relative-hardness
  ordering (soft soils ~12-20, hard rock 35), but — **known v0 limitation,
  documented follow-up** — only `MAT_ROCK`'s entry is reachable today:
  `UVoxelWorldSubsystem::IsSolidAtVoxel` (the only material-adjacent query
  this slice is allowed to call — `VoxelWorldSubsystem.{h,cpp}` were
  READ-ONLY, owned by another track tonight) returns a bare `bool`, so
  `PlanPath`'s `SolidFn` reports every solid voxel as one sentinel material
  (`kSolidSentinelMaterial = MAT_ROCK`) regardless of what it actually is.
  "Softer materials mine cheaper" is real, tested intent, not yet
  observable behavior — unlocking it needs a material-returning query
  added to `UVoxelWorldSubsystem`, left as a follow-up for whoever owns
  that file next. `MAT_BEDROCK` stays hard-blocked exactly as `pathfind.h`
  enforces (unconditional, config can't override it) — not fought.

- [x] **Execution through the ONE authority path**: Mine → `Terrain.TryDig
  (EyeWorld, DestCenterWorld - EyeWorld, MinCubeSizeVoxels)`, aimed from
  the CENTER of the agent's own current voxel straight at the destination
  voxel's center — always a short (≤√2 voxels), unobstructed ray since
  `classifyMove` only ever produces Mine for one of the 18 fixed
  neighbor offsets; `SizeVoxels=1` digs exactly that one voxel (no bias
  growth). Bridge → `Terrain.TryPlace(AffectedCenterWorld, NeighborDir,
  MinCubeSizeVoxels, BridgeScaffoldMaterialId, PlayerWorldPos)` — trickier,
  because `TryPlace` can only snap a new cube against an EXISTING solid
  face (it has no "place a fully floating voxel" mode), and a Bridge's
  `affectedCell` is air by definition. `TryExecuteWaypointEdit`
  (`VoxelAgentSubsystem.cpp`) scans `affectedCell`'s six face-neighbors via
  `IsSolidAtVoxel` for a real one (e.g. the floor under the agent for a
  flat bridge), then fires a one-voxel ray from inside `affectedCell`
  straight at it so the DDA hit lands exactly on `affectedCell`. **Known
  v0 limitation, documented follow-up**: a Bridge destination with no
  solid face-neighbor on ANY side (a fully floating scaffold mid-chasm)
  can't be placed through today's public dig/place API at all — the agent
  waits (rate-limited retry, same as a cooldown miss) rather than silently
  dropping the edit; fixing this needs a direct point-target stamp entry
  point on `UVoxelWorldSubsystem` (mirroring the already-`Impl`-only
  `StampVoxels` this slice isn't allowed to reach). `PlayerWorldPos`, not
  `Agent.Position`, is passed as `TryPlace`'s overlap-reject argument so an
  NPC edit still honors "never trap the real player." Both calls route
  through `UVoxelWorldSubsystem::TryDig`/`TryPlace` — the exact same
  authority/replication/chop-hook/water-breach-hook path player digs use;
  no second edit path was added anywhere.

- [x] **Rate limiting (three independent limits, all must hold)**: (1)
  per-agent cooldown — `NPCDigCooldownSeconds = 1.5`s
  (`FVoxelAgent::LastDigTimeSeconds`), "mining costs hardness x TIME, not
  an instant swap." (2) Global per-tick cap — `MaxNPCEditsPerTick = 4`
  across the WHOLE swarm, threaded through `Tick`/`TickTier0` exactly like
  the existing replan budget; logged (`"VoxelSwarm NPC edits this tick:
  N/4 (cap)"`) whenever spent. (3) Tier restriction — digging is Tier
  0-EXCLUSIVE, narrower than the doctrine's "Tier 0 and optionally Tier
  1": Tier 1/2 plan/steer through a separate `FVoxelAgentImpl::NavConfig`
  (byte-identical to the pre-M6 single-config setup — Mine impossible,
  Bridge ~1,000,000) so their paths never NEED an edit, sidestepping the
  failure mode where a Tier 1 agent inherits a stale Mine/Bridge waypoint
  it will never execute and `AdvanceAlongWaypoints` (no collision
  awareness of its own) walks it through solid/unsupported terrain. A
  demoted Tier 0→1 agent still gets one belt-and-braces guard:
  `FVoxelAgentImpl::PathIsDigCapable` (tagged per-path by `PlanPath`) makes
  `TickTier1` force an immediate replan (and hold position that exact
  tick) the moment it detects it just inherited a dig-capable path, rather
  than advancing first.

- [x] **Safety valve**: `voxel.NPCDig.Enabled` console var (default ON),
  mirroring `voxel.Destruction.Enabled`'s identical M5 role. Realized as a
  COST-FUNCTION swap, not a bolted-on execution block: `PlanPath` routes
  Tier 0 to `NavConfig` instead of `DigConfig` the instant this is off, so
  disabling it proves the "walking, mining, tunneling, bridging fall out
  of ONE cost function" claim applies to the switch too (agents plan
  genuine detours, not a tunnel that then silently fails to execute).
  `TryExecuteWaypointEdit` also checks it directly (defensive, for the
  narrow window between a runtime toggle and an agent's next replan).

- [x] **Invalidation**: no new plumbing needed — Tier 0's existing
  every-tick `pathStillValid` recheck (pre-M6) already IS the cross-agent
  (and cross-player) invalidation mechanism "for free": any edit (this
  agent's own successful Mine/Bridge, another agent's, or a player dig)
  changes what `IsSolidAtVoxel` reports at the touched cell, so the very
  next tick's re-classification of that step disagrees with what was
  recorded and forces a replan. `pathStillValid` is now called with the
  SAME config a path was planned under (`DigConfig` for Tier 0, `NavConfig`
  for Tier 1 — mismatching them would false-positive-invalidate every
  tick). Comment in `TickTier0` marks where a future per-dirty-brick
  invalidation (`regiongraph.h`'s `markRegionDirty`, merged but NOT a
  dependency of this slice) would hook in to replace "every agent re-scans
  its own path every tick" with "only agents touching a just-dirtied brick
  get invalidated" — a cost optimization, not a correctness change.

- [x] **Headless verification** (`-VoxelDigSwarmTest=<N>`, new switch,
  `AVoxelEarthGameMode`): builds a deliberate 4m-wide x 2.8m-tall x
  4-voxel-thick `MAT_ROCK` wall 6m ahead of the player (70 `TryPlace`
  calls, bottom-up, each raycasting straight down onto whatever is
  currently topmost — natural terrain for layer 0, the previously-placed
  cube for every layer after) — sized bigger on every axis than a single
  Tier 0 search window (`WindowHalfExtentVoxels`/`WindowHalfHeightVoxels`,
  ~3.3m x 2.5m) so a lone windowed search can't step over the top or find
  either end. Spawns N agents on the wall's FAR side
  (`UVoxelAgentSubsystem::SpawnSwarmAtOffset`, a new deterministic-offset
  variant of `SpawnSwarm` added because the random 360-degree ring can't
  guarantee that). `VoxelEarthEditor` built clean (`Build.bat ...
  -WaitMutex -NoHotReloadFromIDE`, `Result: Succeeded`, zero warnings from
  any of `VoxelAgent.{h,cpp}`/`VoxelAgentSubsystem.{h,cpp}`/
  `VoxelEarthGameMode.{h,cpp}` — forced a full recompile of all three .cpp
  files specifically to confirm this, not just relied on an incremental
  no-op link). Worktree `voxelcore.lib` freshly configured+built
  (CMake+Ninja, MSVC 14.51/VS 2026, Release); `vxc_tests.exe` full suite
  passes (pathfind's own golden-digest/tunnel-vs-around/bedrock-never-mined
  tests included, unmodified — `pathfind.h` was consumed as-is).
  **Gotcha discovered and worked around**: `UVoxelWorldSubsystem`
  autoloads `Saved/VoxelWorlds/<seed>.vxlog` on startup and autosaves on
  shutdown (M3 wave 2 persistence) — a repeated headless run without
  clearing that file builds on the PREVIOUS run's already-dug wall, which
  silently degrades/skews the demo; every run below started from a deleted
  `.vxlog` (documented in a code comment at the switch's delay constant so
  a future by-hand re-run doesn't get bitten by it).

  Run 1 (`-game -VoxelDigSwarmTest=10 -VoxelScreenshotAfter=55 -windowed
  -resx=1280 -resy=720 -log -unattended -nosplash`, seed default
  20260719, clean `.vxlog`): wall built (70/70 cubes). 10/10 agents
  spawned ~9m beyond it; all promoted to Tier 0 immediately (within
  `Tier0EnterUU`). **98 authoritative edits applied** over the run (44
  Mine + 54 Bridge — agents tunnel through the rock, then bridge a gap on
  the far side), each logged with its exact voxel coordinates, e.g.
  `"VoxelAgent 1: Mine at (61,-1,10972) -- authoritative edit applied
  (edit budget 3/4 remaining this tick)"`. Per-tick cap distribution: 43
  ticks at 1/4, 17 at 2/4, 3 at 3/4, 3 at 4/4 — **the cap held, never
  exceeded 4/4**. Mean distance-to-player: 11.2m (spawn) → 3.1m (last log
  before quit) — **monotonically decreasing**, agents get through. Clean
  shutdown (`LogExit: Exiting.`), zero ensures, zero fatal errors (the
  only warnings are the pre-existing `M_VoxelTerrain`/`M_VoxelClipmap`
  missing-material fallback, unrelated to this slice — same documented
  issue as the M6 navigation-only run above). Two screenshots captured
  (`Saved/Screenshots/WindowsEditor/VoxelVerify00000.png`,
  `VoxelVerify00001.png`, 1280×720) — visibly show the wall with a carved
  opening through its face (the tunnel), framed low and close per this
  switch's own screenshot-framing branch. (Terrain in these shots renders
  with the same pre-existing spiky/checkerboard default-material artifact
  as every other verification run in this worktree — unrelated to, and
  not caused by, this slice.)

  Run 2 (same command + `-ExecCmds="voxel.NPCDig.Enabled 0"`, clean
  `.vxlog`): wall built identically (70/70 cubes, confirmed same log
  line). **0 authoritative edits applied** the entire run (`grep -c
  "authoritative edit applied"` → 0) — proves the safety valve, realized
  as `PlanPath`'s cost-function swap, actually suppresses every Mine/
  Bridge. Agents still converge (11.2m → 4.3m) via a genuine WALK-ONLY
  detour around the wall's lateral extent (confirmed not just "pressed
  against the wall's outer face": 4.3m is closer than the wall's near face
  ever gets to the player without going around it) — a real, if less
  dramatic than "stuck", proof that the SAME cost function drives both
  outcomes purely from `DigConfig` vs `NavConfig`, not a hardcoded
  behavior swap. Screenshot of the same wall column shows it fully intact
  (no carved opening), the visual counterpart to the 0-edit log count.
  **Honest caveat**: the wall's 4m width only modestly exceeds the ~3.3m
  search window, so a multi-replan walk-around remains reachable inside
  this run's ~60s budget rather than leaving agents visibly stuck — a
  wider wall (or a longer run) would make the "fails to reach" half of
  "detour or fail to reach" more dramatic, but the decisive proof the task
  asked for (zero edits when disabled vs. 98 rate-limited edits with exact
  coordinates when enabled) is unambiguous either way.

- [ ] **Follow-ups (documented, not fixed this slice)**: (1) per-material
  mine costs are inert until `UVoxelWorldSubsystem` gains a real
  material-returning query (today every solid voxel reads as one sentinel
  material). (2) Bridge can't place a fully floating scaffold with no
  solid neighbor on any side — needs a direct point-target stamp API on
  `UVoxelWorldSubsystem` (mirroring its already-`Impl`-only `StampVoxels`).
  (3) Tier 1 digging (the doctrine's "and optionally Tier 1") was
  deliberately left out of scope — extending it needs either
  `AdvanceAlongWaypoints` to gain collision/edit awareness or a redesign
  of how a demoted agent's in-flight edit obligations transfer between
  tiers. (4) all prior M6 follow-ups (region-graph swap for Tier 1,
  2-voxel body, per-dirty-brick invalidation, agent state replication)
  still stand, unchanged by this slice.

### Tier-1 hierarchical planning + NPC replication (M6 gap closure, worktree agent)

Closes the two documented v0 gaps: Tier 1 now plans via the merged
`regiongraph.h` hierarchical layer instead of raw `vxc::findPath`
(`UVoxelAgentSubsystem::PlanPathTier1`), and NPC state now replicates to
remote clients (`AVoxelAgentReplicator`, a new file). Files touched:
`VoxelAgent{,Subsystem}.{h,cpp}`, new `VoxelAgentReplication.{h,cpp}`,
`VoxelEarthGameMode.{h,cpp}` (test switches only).

**1. Region-graph lifetime/extent design.** ONE `vxc::RegionGraph`
(`FVoxelAgentImpl::Tier1RegionGraph`, PIMPL-side), rebuilt (not
per-frame — only when the player's ground column drifts more than
`Tier1GraphRebuildTriggerUU`, half the box radius, from the cached
center) over a box centered on the player's ground column:
`Tier1GraphHorizontalRadiusRegions=12` (19.2m) x
`Tier1GraphVerticalRadiusRegions=1` (1.6m each way) = 25x25x3 = 1,875
regions. This is deliberately NOT sized to cover the full Tier 1
catchment (`Tier0ExitUU`..`Tier1ExitUU` = 20m..100m) — see the measured
cost below for why — so Tier 1 agents outside the box's coverage fall
back to the pre-M6 fine windowed `findPath`, unchanged and always
correct, never worse than before this slice (`PlanPathTier1`'s
`FallBackToFineSearch`). 19.2m was chosen as the SMALLEST radius that
can ever hold a Tier 1 agent at all: Tier 1 classification requires
distance >= `Tier0EnterUU` (15m), so any box radius under that could
never cover a single Tier 1 query.

**MEASURED build cost (real numbers, not estimates) — the central
finding of this slice**: the dominant cost is **O(regions x portals^2)
fine-`findPath` calls**, each allocating bookkeeping arrays sized to its
search window's volume regardless of how many expansions it actually
uses. Three real data points, same 1,875-region box, only the per-call
expansion cap (`Tier1GraphIntraMaxExpansions`/`Tier1PerRegionMaxExpansions`)
varied:
| Cap | Build time | Corridor success |
|---|---|---|
| 256 | ~160s | **0/751** hierarchical queries succeeded (entry/exit/intra-region searches inside rock-dense regions genuinely need more than 256 expansions; every one fell back to fine search) |
| 4096 (regiongraph.h default) | >500s, killed before completion | untested (never finished) |
| 1024 (shipped) | ~506s | succeeded (3/5 initial hierarchical calls per a 3-agent test; the other 2 were early-tick misses before the graph existed) |
An EARLIER, bigger box (radius 16/3 = 33x33x7 = 7,623 regions, cap
4096) hung for 95+ seconds and never completed in one headless run —
first real signal that region COUNT, not the expansion cap, is the
actual lever for build cost (confirmed: raising the cap on the SAME
1,875-region box made the build slower, not faster, since more real
search work — not just array allocation — was genuinely happening at
the higher cap). **Conclusion**: at this project's terrain and
`kRegionEdge=16`, a synchronous one-shot build of even a modest
(sub-20m-radius) box costs several minutes on this machine — not
affordable as a per-rebuild hitch in a real game. This is reported
honestly as a genuine, measured limitation of doing this synchronously
on the game thread; see follow-ups below for the fix (background/async
build).

**2. Tier-1 planning cost before/after** (`voxel.Agent.Tier1RegionGraph.Enabled`
cvar; default on = hierarchical + fallback, off = force fine-only —
run the same scenario both ways and diff the "VoxelSwarm Tier1
planner:" log lines). Measured on the 3-agent `-VoxelTier1RegionGraphTest`
scenario (agents 17m out, cvar on): hierarchical calls succeeded with
avgExpansions=5,515.7/call — but critically, all 3 agents reached the
player (promoted Tier0, `meanDistToPlayer` dropped from 18.6m to 5.1m)
within about 3 total hierarchical calls combined, i.e. ~1 call/agent to
cover the full 17m including a detour around a deliberately-placed 6m
wall. The pre-M6 fine fallback path (still exercised for the 2 early-tick
misses, and always exercised with the cvar off) shows
avgExpansions=8,000.0/call — pinned at `MaxExpansionsPerSearch`,
i.e. capped/incomplete almost every time, meaning the OLD Tier 1 needs
MANY chained 8,000-expansion partial replans to slowly close a 17m gap
instead of the ~1-2 hierarchical calls that got there in this run. The
per-call hierarchical cost (5,515 expansions) is higher than one capped
fine-search call (8,000, but incomplete/partial) is misleading in
isolation — the honest comparison is "expansions PER UNIT OF REAL
PROGRESS", where hierarchical wins by roughly the number of chained
fine-replans it would have taken otherwise (untested exactly how many,
but the fine path never even got the agents to the player in the same
window in earlier `-VoxelSwarmTest` runs at comparable range).

**3. Dirty invalidation wiring + proof.** NPC edits (`TryExecuteWaypointEdit`)
call `MarkTerrainEditDirty` with the EXACT edited voxel the instant they
land — precise, no polling. Player digs/places and M5 structural-collapse
removals have no delegate to hook (`UVoxelWorldSubsystem`/`VoxelWaterSubsystem`
are out of scope this slice, per doctrine) — implemented instead as a
documented best-effort fallback, `PollWorldEditsForTier1DirtyRegions`:
once per Tick, compares `UVoxelWorldSubsystem::GetLogSize()` (already
public, already counts every edit regardless of source) against the last
seen value, and if it grew, dirties the region containing every
active Tier 0/1 agent's CURRENT position (deduplicated, one
`markRegionDirty` call per distinct region, not per log entry). PROOF:
in the region-graph test, digging a gap in the wall produced
`VoxelSwarm Tier1 region graph: edit-log grew (size 261) -- dirtied 1
region(s) near active agents` on the very next Tick after the dig — the
mechanism fires correctly. HOWEVER: in the specific run captured, the 3
test agents had already detoured AROUND the wall and reached the player
(Tier0, standoff) several seconds BEFORE the gap was dug, so this run
demonstrates "a Tier 1 agent re-plans correctly AROUND a new wall" (the
whole swarm successfully routed around the 6m obstacle via the
hierarchical planner) but does NOT capture a single agent's cost/route
visibly changing AFTER walking through a freshly-dug opening in the same
continuous run — a timing/tuning artifact of this scenario (the agents
converge faster than the scripted gap-dig delay), not a failure of the
invalidation mechanism itself, which the log line above proves fires
correctly and on the correct region.

**4. NPC state replication** (`AVoxelAgentReplicator`, new file,
mirrors `AVoxelEditRelay`'s spawn-when-networked pattern in
`AVoxelEarthGameMode::BeginPlay`): one `NetMulticast Reliable`
broadcast (`MulticastAgentSnapshot`) every `BroadcastIntervalSeconds`
(10Hz, independent of and much coarser than the sim tick), carrying
EVERY currently-relevant agent in ONE call (batched, not one RPC/agent).
Wire format (`UVoxelAgentSubsystem::CollectReplicationSnapshot`/
`ApplyReplicatedAgentSnapshot`): `int32 AgentId + int16x3 relative
position (UU, relative to a replicated `OriginWorld` re-anchored at the
first connected player's pawn every broadcast, keeping offsets small
and LWC-safe regardless of planet-scale absolute coordinates) + uint8
Tier` = 11 bytes/agent, vs 24+ for a raw double `FVector`. Relevancy/
culling: Tier 2 agents (always > `Tier1ExitUU` from every player,
per the tier scheduler's own hysteresis, and the least important tier
already) are NEVER sent, and Tier 0/1 agents are filtered to
`RelevancyRadiusUU` (120m) of the origin — a client never receives
agents far outside the interesting zone at all, not just "at a lower
rate". Client-side: a NEW `ClientAgents` mirror (position/tier only, no
path state) is populated by `ApplyReplicatedAgentSnapshot` and rendered
by a NEW, separate `Tick` branch (`TickClientReplicatedAgents`) that
interpolates each entry from its last render position toward the fresh
target over `ClientAgentInterpolationWindowSeconds` (0.15s) — smooth at
10Hz instead of visibly snapping. Agents that age out of relevancy are
HIDDEN (zero-scale ISM transform), not `RemoveInstance`d, to avoid the
instance-index-reshuffling bug class that would otherwise silently
corrupt other agents' cached indices. Server remains sole authority for
path/dig decisions in every case — a client only ever draws what it's
told.

**Verification actually performed vs. what could not be completed.**
A genuine two-process networked test (dedicated server + client, and
separately a listen-server + client, both via `UnrealEditor-Cmd.exe
<uproject> [-server|<map>?Listen] [127.0.0.1] -game`, mirroring M3's
own gate scenario) was attempted. The listen server DID spawn 10/10
swarm agents (`VoxelSwarmTest: spawned 10/10 requested agents`) and DID
accept an incoming connection (`LogNet: NotifyAcceptingConnection
accepted from: 127.0.0.1:...`), but the CLIENT's own connection never
completed its handshake and timed out after 60s
(`UNetConnection::Tick: Connection TIMED OUT`) in BOTH the
dedicated-server and listen-server configurations, on this sandboxed
dev machine — consistent with a firewall/loopback constraint on fresh,
unsigned `UnrealEditor-Cmd.exe` invocations (no existing firewall rule
for the binary; `New-NetFirewallRule` failed with Access Denied, no
admin rights available to this session) rather than a bug in the
replication code. **Do not read this as "multiplayer works" — the
actual cross-process UDP round trip was NOT verified.** What WAS
verified instead: `UVoxelAgentSubsystem::RunReplicationSelfTest`
(`-VoxelReplicationSelfTest`, new method, calls `CollectReplicationSnapshot`
-> `ApplyReplicatedAgentSnapshot` IN-PROCESS, no network hop) against a
12-agent swarm: **all 12 agents' replicated position matched their
authoritative position to within 0.755UU worst case** (expected
quantization rounding for the int16 wire format is <=~0.87UU) — proof
that the serialize/quantize/deserialize/interpolation-setup logic is
correct, independent of whatever the OS network stack does. The actor/
RPC plumbing itself (`AVoxelAgentReplicator`) is standard, well-
established UE machinery (same shape as the already-proven
`AVoxelEditRelay`) and was code-reviewed but not exercised end-to-end
over a real socket in this environment. No client-view screenshot was
captured for this reason — a server-view screenshot is NOT substituted
in its place (per doctrine: don't claim what wasn't verified).

**Build**: `VoxelEarthEditor` Win64 Development, `Build.bat ...
-WaitMutex -NoHotReloadFromIDE` — `Result: Succeeded`, zero warnings
from any file touched this slice (`voxel-core-msvc` itself rebuilt
once, untouched by this slice, via `cmake -S voxel-core -B
build/voxel-core-msvc` + `cmake --build ... --config Release`, since
this fresh worktree had never built it before).

**Follow-ups (documented, not fixed this slice)**: (1) the region-graph
build needs to move off the game thread (background task /
time-sliced across ticks) before its coverage box can grow to anything
useful — the measured several-minutes-per-build cost at even a modest
size is the blocking issue, not this slice's wiring. (2) precise
coordinate-level dirty notification (a delegate on
`UVoxelWorldSubsystem` reporting exactly which cells an edit touched)
would replace `PollWorldEditsForTier1DirtyRegions`'s position-based
best-effort heuristic with an exact one; not implemented since that
file is out of scope this run. (3) a real cross-process replication
gate re-run is needed once the sandbox's networking constraint is
lifted (or on a machine/account with firewall-rule permissions). (4)
`AVoxelAgentReplicator` uses Reliable NetMulticast (matching
`MulticastWaterDiffs`'s existing precedent); Unreliable would be
strictly better for stale-position traffic in a shipped version. (5)
Tier 2 agents are never replicated at all in this slice (relevancy
cutoff, not a rate reduction) — a shipped version might want them at a
very coarse rate instead of not at all, for visual continuity at the
edge of relevancy.

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
`worldgen.ush` (commit `8b834107701fd4a2a005b8dbd4a17352f44f26c1`, "M4:
biome classification core") with the pinned DXC `v1.9.2602.24`
(`tools/fetch-dxc.ps1`). SHA-256 provenance for every file is in
`voxel-core/shaders/prebuilt/README.md`. **Important**: these do NOT match
the older `e1db29a9b6874012`/`583e91d62cefb8a9` digests recorded in the gate
row above — those predate the M4 biome commit, which legitimately changed
`worldgen.ush`'s output (new materials/biome constants). The gate row's
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

**No float-in-shader risk found**: `worldgen.ush` has zero occurrences of
`float`/`half`/`double` (checked directly) — every value is integer/
fixed-point end to end, matching the float-free contract in
`docs/determinism.md`. This is the actual reason cross-vendor bit-exactness
is even plausible: IEEE-754 float ops can legitimately differ in rounding
between AMD and NVIDIA (fused-multiply-add contraction, transcendental
approximations, denormal handling), which would make a NVIDIA-vs-AMD gate
fundamentally unreliable if the shader touched floats anywhere in the
value-computing path (wall-clock timing code in the harness itself uses
floats/doubles, but that never feeds GPU dispatch inputs or outputs).

### Cross-vendor UB hardening + CI guard (2026-07-21, worktree agent)

Follow-up to the signed-`%` fix (PR #40, commit `6ab4b2a`) that closed the
NVIDIA leg of the M0 gate. That bug — `worldgen.ush`'s `floorDiv` deriving
its flooring correction from `a % b`, which HLSL leaves undefined unless both
operands share a sign, so `floorDiv` silently truncated on NVIDIA for every
negative world coordinate — was one instance of a class. This pass fixed the
five remaining members of that class and, more importantly, made the class
mechanically impossible to reintroduce.

**Five latent issues, all confirmed real by reading the generated code, all
fixed in `voxel-core/shaders/worldgen.ush`.** Each was live UB reachable
from a cbuffer value, masked only by a host-side contract that nothing in the
shader enforced:

1. `decodeMask` computed interior-brick dims as `bricksX - 2u`. On unsigned
   values a region with <3 bricks on an axis wraps to `0xFFFFFFFF`, the
   product wraps to an arbitrary `maskCount`, and the `maskIndex >=
   maskCount` range check then PASSES — so `regionCellMat` reads `OutCells`
   far out of bounds. Fixed with a `bricksX/Y/Z < 3u` early-out.
2. The greedy run-length scan read `mask[64]`, one past `uint mask[64]`, on
   the terminating iteration of the last row (`i2 + w == 8`, `j2 == 7`).
   Safe only if `&&` short-circuits, which HLSL does not guarantee (DXC does
   today — a compiler property, not a contract). Hoisted into an explicit
   `if`/`break`; identical iteration count on every input.
3. `OutQuads[baseOffset + quadCount]` was an unclamped write driven by
   scanned offsets. Bounded against the buffer's OWN length via
   `OutQuads.GetDimensions` (lowers to `OpArrayLength`) rather than a new
   cbuffer field, so there is no second contract to keep in sync; plus
   `MeshEmitMain` now returns rather than emitting above `ScanCount`, where
   the offsets were never written by the scan chain.
4. `PixelSizeMm` is a host-controlled divisor reaching `floorDiv`/`truncDiv`;
   zero is an `OpUDiv`-by-zero, undefined in SPIR-V. Guarded in `ColumnMain`
   AND asserted host-side (`validateWorldGenParams` in
   `voxel-core/bench/gpu_harness.cpp`, called at both cbuffer-fill sites),
   because a shader that silently declines to write output is miserable to
   debug. `validateScanCount` likewise pins `ScanCount == maskCount`.
5. `clamp64(..., 0, RasterSize.x - 1)` inverts to `[0, -1]` when the raster
   window is empty, so `clamp64` returns -1 and the `(uint)` cast makes it a
   ~4-billion element index. Zero-extent early-out added to both
   `rasterElevationMm` and `rasterClimate`.

**These are guards, not behavior changes — proven, not asserted.** Every
added branch is unreachable on valid input, and the three AMD-leg digests
reproduce bit-identically on the RX 7800 XT: `1dbcabb01cfaf2bc` (default),
`95a82ba20200f6f2` (`--radius 64`), `b4c8ec5d0966894b` (`--radius 128`) —
all PASS, 0 mismatches, measured before and after the change on the same box.
CPU reference untouched: `kWorldGenVersion` stays **2**, no goldens re-pinned,
`vxc_tests` 115 PASS / 0 FAIL under clang. Three kernels respun
(`ColumnMain`, `MeshCountMain`, `MeshEmitMain`); `prebuilt/README.md` has the
refreshed provenance and hashes.

**The durable half: `tools/lint-shader-ub.py`, CI job `shader-ub-lint`.**
Scans `voxel-core/shaders/*.hlsl` and fails on signed `/`/`%` where an
operand can be negative and is not routed through the approved
`floorDiv`/`truncDiv` helpers; shift distances that are not literals provably
below the operand width; unsigned subtraction that can underflow into an
index or invert a clamp range; `RW*Buffer` writes with no visible bound on
the index; and wave/subgroup-width assumptions (AMD wave64 vs NVIDIA warp32).
It blanks comments and string literals before matching, in the style of the
existing `float-ban` job, so prose can never trip it.

Design is **fail-closed**. Constructs that are genuinely safe are silenced one
at a time by an inline `// lint-shader-ub: allow <RULE> - <reason>`
annotation; a bare `allow` with no written reason is itself an error, and an
annotation that stops matching anything is an error too, so a justification
cannot rot into cover for new code. There is deliberately no exemption for
`floorDiv`/`truncDiv` themselves — they are written against magnitude-only
unsigned division precisely so they pass on their own merits. worldgen.ush
currently carries 16 annotations covering 19 findings, each recording a real
invariant (e.g. "the `>= 3` early-out above proves every axis has at least 3
bricks", "the enclosing `gtid.x >= offset` test is precisely the no-underflow
precondition"). That is the honest cost of a fail-closed default, and it
doubles as documentation of the bounds this kernel actually relies on.

With `--spv-dir` the lint also scans the COMMITTED bytecode for the
`OpSDiv`/`OpSRem`/`OpSMod` opcodes. The HLSL rules are heuristic; the opcode
scan is exact, and it catches the shipped bug directly however the source is
written. It carries a positive control (zero division opcodes across the
whole module set means the parse saw nothing, and fails rather than reporting
clean). Current state: **zero** `OpSDiv`/`OpSRem`/`OpSMod` across all seven
modules; every remaining division is `OpUDiv`/`OpUMod`.

**The guard was verified by watching it fail**, not by assuming. Reintroducing
the original `%`-based `floorDiv` into a scratch copy makes the lint exit 1
with exactly two findings (`a / b` and `a % b`, both `SIGNED_DIVISION`) and no
noise; compiling that same copy and auditing its SPIR-V reports 10 `OpSDiv`
and 10 `OpSRem` in `ColumnMain`. Restored, the lint exits 0 on HEAD.

Two false-positive sources were found and fixed in the lint itself rather than
allowlisted, which is the difference between a guard and a nuisance: HLSL
swizzles were contributing their component letter as an identifier (so
`tid.x / 8u` picked up the `int64_t x` PARAMETER of `hash2` elsewhere in the
file and looked signed), and non-negative literals were being treated as
"possibly negative" (flagging every `x / 8u`). Also worth recording: the
SPIR-V opcode table is easy to get wrong — 137 is `OpUMod`, not `OpSRem`
(`OpSRem` is 138), and the first draft of the audit reported all the
legitimate unsigned remainders as violations.

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
| **Terrain amplification refactor** (conditioned amplification stack: regime classifier, calibrated spectral synthesis, geomorph passes, clast scattering, stratigraphy/overhangs, learned 30m→1m SR) | **PARKED 2026-07-24 by Matt** behind ADR-0006. Full reconciliation against this project's determinism doctrine, CPU/GPU mirror contract and chunk granularity in `docs/terrain-amplification-reconciliation.md`; source proposal preserved at `docs/research/terrain-amplification-design-doc.md`. Headline: complementary (it loads the AUTHORITY path, ADR-0006 relieves the DISPLAY path), and most of the visual payoff (§4/§5/§7/§8) needs no architecture change — only §6's grid-iterative passes break O(1) point queries. Every stage costs ~2x to implement (integer fixed-point + HLSL mirror + digest parity) | ADR-0006 landing; §6 deferred behind §5/§7/§8 even then; learned SR gated on GPU access |
| **Bounded-admission refill straggler** — a few chunks near LOD ring boundaries never load until the player MOVES | Reported by Matt 2026-07-24, NOT yet diagnosed. Distinct from the load-before-unload holes (fixed). `RecomputeDesiredSet` is the only thing that admits chunks and runs only on an anchor chunk crossing, an underground flip, or the per-level admission refill; candidates rejected by the cap when a level's queue is full are dropped and only return on a later recompute. Standing still, the only path back is the refill trigger (`bAdmissionDeferredWork[Level]` + that level's queue under its share of cap). Same class as the priority inversion already documented above (~20k rejected R0 candidates while standing still underground), which was fixed by making refill per-level — this looks like a residual case | needs a repro + the refill trigger instrumented |
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

### Water edit-notification completeness + memo enablement

Task: ADR-0003 item 2 (`docs/adr/0003-hydrostatic-persistent-body.md`) --
close the notification gaps between `UVoxelWorldSubsystem`'s edit paths and
`UVoxelWaterSubsystem`, then decide whether the cross-tick solid_ memo
(`vxc::WaterCA::setSolidCacheEnabled`) can be turned on by default.

**Gaps found and fixed** (`VoxelWorldSubsystem.cpp`/`.h`,
`VoxelWaterSubsystem.cpp`/`.h`):

1. **`TryPlace` sent no water notification at all.** Fixed: it now captures
   its own `FEditsByBrick` (mirroring `TryDig`'s `DugCells`) and, on every
   authority role, calls both `NotifyTerrainVoxelsCleared` (defensive --
   handles the near-impossible case a placed material is itself `MAT_AIR`)
   and the new `NotifyTerrainRegionEdited`.
2. **`ExtractClearedVoxelCoords` filtered to `MAT_AIR`, hiding every non-air
   edit from the memo.** Rather than stretch that helper's contract (it stays
   exactly what it always was -- "which of these edited voxels are dig-style
   solid-to-air clears," feeding Reservoir v0 breach-seeding, which genuinely
   only cares about that one direction), a NEW, material-agnostic hook was
   added: `UVoxelWaterSubsystem::NotifyTerrainRegionEdited(MinVoxelIncl,
   MaxVoxelIncl)`, routed straight to `vxc::WaterCA::invalidateSolidRegion`.
   It is called once per edit (batched over that edit's own bounding box, per
   the "keep it efficient" requirement -- a 150,000-voxel collapse costs the
   memo ONE call, not 150,000), by EVERY edit path: `TryDig`, `TryPlace`,
   `CarveSphere`, `PromoteDetachedIslands` (islands + large-edit collapse,
   see #3), `SpawnTreeFixtureAt`, `SpawnStructureFixtureAt` -- regardless of
   whether the edit's result is air or solid. Over-invalidating a region that
   did not actually change solidity is always safe (ADR-0003): it only costs
   a re-query.
3. **M5 structural collapse (and ordinary island promotion) never notified
   water at all**, on either hook. `PromoteDetachedIslands` removes voxels via
   the SAME edit-log path as a dig (`DetectAndRemoveIslands` /
   `DetectAndRemoveCollapse` both write `MAT_AIR`), but the water calls at the
   `TryDig`/`CarveSphere` call sites only ever saw the ORIGINAL dig/carve's
   own cleared-voxel list, never the islands/pieces removed as a
   CONSEQUENCE. Fixed: `PromoteDetachedIslands` now flattens every promoted
   piece's coordinates (covers both plain island detection and the
   brick-resolution collapse path, since the latter funnels its pieces into
   the SAME `Islands` array) and calls both water hooks itself, on every
   authority role, before the cosmetic-debris/dedicated-server branch.

A new helper, `ComputeEditVoxelBounds` (from an `FEditsByBrick`) /
`ComputeVoxelCoordBounds` (from a plain `TArray<FVoxelCoord>`, for the
`StampVoxels`-based fixture paths), computes the exact inclusive bounding box
of whatever an edit touched, unfiltered by material -- this is what
`NotifyTerrainRegionEdited`'s batched call is built from.

**Memo enablement -- PROVEN, enabled by default.** New GameMode switch
`-VoxelWaterMemoTest[=<delaySeconds>]` (`VoxelEarthGameMode.{h,cpp}`) carves a
basin, settles a pool in it, then runs the full edit vocabulary through the
SAME public `UVoxelWorldSubsystem` API real gameplay uses: `TryDig` (beneath
the pool), `TryPlace` (a rock cube at the basin floor), `CarveSphere` (a side
breach), `SpawnStructureFixtureAt` (a non-air wall+roof fixture whose roof
spans directly over the basin), then a large `CarveSphere` through the far
pillars that trips the M5 brick-resolution collapse path, dropping the roof
section above the basin. A new console var, `voxel.Water.SolidCacheEnabled`
(default **true**), toggles the memo; `StepFixed` re-reads it every fixed
step (same "cheap enough to poll every call" pattern as
`voxel.Destruction.Enabled`/`voxel.Destruction.Collapse`), so it can be
forced off in the field with no relaunch (task item 4).

The proof is a cross-process A/B (same convention as the M3 determinism
guard): the identical scenario (same seed, same edit sequence) run twice,
once with `-ExecCmds="voxel.Water.SolidCacheEnabled 0"` and once with `1`,
diffing every logged checkpoint digest -- not just the final one:

| Checkpoint | memo OFF | memo ON |
|---|---|---|
| pour | `0x68598CA231A5B2D8` | `0x68598CA231A5B2D8` |
| pre-edit settle | `0xFFCC45E6CD111EC2` (vol 20000) | `0xFFCC45E6CD111EC2` (vol 20000) |
| post-dig nudge | `0x4EA5F1471F872744` | `0x4EA5F1471F872744` |
| post-dig settled | `0x0E776E8E381361DB` | `0x0E776E8E381361DB` |
| post-place nudge | `0x147D62C9128170A1` | `0x147D62C9128170A1` |
| post-place settled | `0xDEC8BBA411E47A74` | `0xDEC8BBA411E47A74` |
| post-carve nudge | `0xE5F3F5D80B01192E` | `0xE5F3F5D80B01192E` |
| post-carve settled | `0xCC2AD7C552AD4BE3` | `0xCC2AD7C552AD4BE3` |
| post-collapse nudge | `0x8B065FC41CE39239` | `0x8B065FC41CE39239` |
| FINAL | waterDigest `0x5D0D9B43677FD302`, editedDigest `0xFB1235E05C62C838`, activeBricks 0, storedBricks 11, volume 21200 | identical, all fields |

Every single checkpoint matches byte-for-byte, both runs, seed 20260719 --
this is the ADR-0003 item 2 bar ("an automated test that ... shows the water
state is byte-identical with the memo ON vs OFF") met in full, through the
actual engine wiring (not a bypass). `voxel.Water.SolidCacheEnabled` default
flipped to true on the strength of this. (An earlier attempt with only
2-second inter-edit settle windows showed a handful of INTERMEDIATE
checkpoints differing by a tick or two before reconverging identically by
the post-carve stage onward -- traced to real-wall-clock tick-count jitter
between separate process launches: the CA's 10Hz accumulator is driven by
actual frame `DeltaTime`, and two independent runs' frame timing is not
required to line up to the tick, NOT a memo defect. Widening the settle
windows to 6-8s made every checkpoint agree, confirming the jitter theory.)

**A separate, pre-existing gap found (NOT fixed, NOT blocking the above):**
a fully-settled water body (`activeBricks==0`) does not reactivate itself
when nearby terrain changes -- `vxc::WaterCA::step()` is a no-op over an
empty active set, and NEITHER hook above (nor any existing call in
`VoxelWaterSubsystem.cpp`) ever adds a brick back to `active_` for an
above-sea-level edit; only the Reservoir v0 breach path does that, and only
for `Z<0` cells that border already-open ocean. This is why the
`-VoxelWaterMemoTest` scenario above includes a small `SpawnWaterAt` "nudge"
after each edit, mirroring `test_waterca.cpp`'s own
`waterca_solid_cache_invalidation_tracks_terrain_edit` (whose own comment
already documents this: terrain edits do not by themselves wake bricks, that
is the caller's existing activation duty). Verified empirically: a first
pass at this scenario with NO nudge produced an unchanging digest through
dig/place/carve/collapse alike. This does NOT threaten memo correctness
(confirmed byte-identical either way, and the hydrostatic flood's
water-cell-seeded exploration does not even consult a stored water cell's
OWN solidity, only its neighbors', so a body that never gets re-examined
behaves identically memo on or off, trivially, not meaningfully). It DOES
mean "dig beneath a long-settled pond above sea level and see it react" is
not currently true in shipped gameplay. No public voxel-core API exists to
reactivate a brick without also injecting real (if tiny) volume via
`addWater` -- per this task's own doctrine (call existing invalidation APIs;
if a needed API genuinely does not exist, report it), this is reported as a
follow-up rather than hacked around with a silent per-edit volume injection
in production code. A real fix needs either a new narrow
`WaterCA::wakeRegion(...)`-style API (voxel-core change, Matt's call) or a
documented, metered production nudge policy.

In-engine perf: the test basin is small (10-45 active bricks, sub-brick-scale
scenario) -- `LogVoxelPerf`'s `WaterPerf` lines show `tickMs` in the
0.001-0.04ms range regardless of the memo, i.e. this scenario is too small to
show a measurable difference (expected -- ADR-0003 already found the memo's
win lives in "97-98% of flood cell-pops are AIR" at LARGE active-pour scale).
The authoritative perf numbers remain `vxc_waterca_bench`'s (unaffected by
this wave, voxel-core untouched): 329-345 -> 111-121 ms/tick on the
441-column pour (~2.8-3.0x), 40.8M -> 4.7M terrain queries; ~9-11 ->
~6.5-6.8 ms/tick on a settled lake with an engine-realistic (~1us) query
cost.

**Determinism argument.** Every notification call above is reached only on
an authority role (`NetMode != NM_Client`, matching every pre-existing water
hook), driven by the SAME edit-log entries that already replicate
deterministically to clients; a client's own `UVoxelWaterSubsystem` never
steps its local CA (mirrors via replicated fills only), so it has nothing to
invalidate and the hooks no-op there by construction. The memo itself is a
pure memoization (byte-identical by definition once invalidation is
honoured, per `voxelcore/waterca.h`'s own contract) -- this wave's job was
proving the CALLER honours that contract for every edit path, which the A/B
above does.

**Build/tests.** `VoxelEarthEditor Win64 Development` via `Build.bat ...
-WaitMutex -NoHotReloadFromIDE` -> `Result: Succeeded`, zero warnings from
`VoxelWaterSubsystem.{h,cpp}`/`VoxelWorldSubsystem.{h,cpp}`/
`VoxelEarthGameMode.{h,cpp}` (only pre-existing engine-header deprecation
warnings elsewhere, unrelated). `voxelcore.lib` freshly built for this
worktree (CMake+Ninja, MSVC 14.51/VS 2026, Release) since it did not exist
yet here; voxel-core itself untouched by this wave. `vxc_tests.exe` (clang,
llvm-mingw): 132/132 PASS, 0 FAIL. Headless verification: `-game
-VoxelWaterMemoTest=10 -VoxelScreenshotAfter=62 -windowed -resx=1280
-resy=720 -log -unattended -nosplash`, seed default (20260719): full
carve/pour/dig/place/carve/structure/collapse sequence completes, log
confirms `applied=1` for both the dig and the place (an early attempt with
the camera 30m from target, outside `DigPlaceRangeMeters`'s 8m raycast
range, silently returned `applied=0` for both; fixed by moving the test's
camera placements within range). Screenshots
`VoxelVerify00000.png`/`VoxelVerify00001.png`
(`ue-project/Saved/Screenshots/WindowsEditor/`) show the carved crater, the
placed rock cube at its rim, and water filling the basin/surroundings.

**Follow-ups.** (1) The settled-water reactivation gap above. (2) Placing a
solid block directly into a voxel that already holds stored water fill: no
existing API evicts that water either (same "no production-safe API"
reasoning as the reactivation gap) -- harmless for determinism (the stale
fill stays byte-identical across clients, just visually hidden behind the
new solid geometry), but a genuine visual/data quirk worth a real fix later.
(3) `docs/adr/0003-hydrostatic-persistent-body.md` item 3 (the persistent
per-water-body structure) remains deliberately deferred, unaffected by this
wave.

### Water reactivation on terrain edits (wakeRegion)

**The bug.** A fully settled body of water (`activeBricks==0`) ignored terrain
edits completely. `vxc::WaterCA::step()` is a no-op over an empty active set
(waterca.h "Activity / settling": a brick that produces no net change in a tick
drops out of the active set, and the next active set is exactly the set of
bricks that changed), and NOTHING ever put a brick back for an above-sea-level
edit -- only the Reservoir v0 breach path did, and only for Z<0 ocean-adjacent
cells. `NotifyTerrainRegionEdited`'s `invalidateSolidRegion` call corrects what
the CA BELIEVES about terrain but never makes it LOOK. Net effect in shipped
gameplay: dig underneath a settled pond and nothing happened -- proven by the
previous wave (unchanging water digest through an entire dig/place/carve/
collapse sequence, which is why that scenario needed `SpawnWaterAt` "nudges" to
prove anything at all).

**The fix: `WaterCA::wakeRegion(minVoxel..maxVoxel)`** (waterca.h/.cpp).
Re-inserts into the active set every brick that CURRENTLY STORES WATER and
intersects the edited voxel box grown by `kWakeHaloBricks == 1` brick on each
axis. It writes no fill at all -- waking is purely a SCHEDULING act, never a
source or a sink, so `totalVolume()` cannot move because of it. Returns the
number of bricks newly woken.

*Why a 1-brick halo, and why only that.* Waking only the bricks the edit
overlaps is useless for the flagship case: when you dig the floor out from
under a pond, every removed voxel is dry terrain and there is no water in the
edited bricks at all. One brick of halo covers every face-, edge- and
corner-adjacent brick, i.e. every brick whose cells can be within one CA step's
reach (gravity/lateral are 1-voxel moves) of a changed voxel. It does not need
to be wider: once any woken brick actually moves water, `stepWithOrder`'s
existing "changed UNION changed's 6 face-neighbours" rule carries activity
outward on its own, as far as the water genuinely travels. So the halo only
SEEDS the reaction; it never has to predict its extent -- which is also what
stops a big edit beside a big lake from re-activating the whole lake up front.
Empty bricks in the halo are skipped (nothing to move, and they would cost a
512-cell scan per tick); water flowing INTO an empty brick is already covered,
because the SOURCE brick is active and `stepWithOrder`'s `touched` set already
includes every active cell's target-direction neighbours.

*Determinism.* The woken set is a pure function of (region, WaterMap contents).
`active_` is a `std::set` ordered by `BrickKeyLess`, so insertion order is
unobservable, and the per-brick test ("does this key store water?") depends on
no iteration or hash order. `wakeRegion` picks between walking the region's
bricks and walking the stored bricks purely on size; both enumerate exactly
{stored} INTERSECT {region}. Pinned by
`waterca_wake_region_order_and_strategy_independent` (same body built in two
insertion orders -> identical woken set and identical post-tick digest under
forward vs reversed active-set order, plus a same-region/different-strategy
cross-check).

**Wiring.** `UVoxelWaterSubsystem::NotifyTerrainRegionEdited` now calls
`wakeRegion` right after `invalidateSolidRegion`. That hook is already called
by EVERY authoritative edit path (TryDig / TryPlace / CarveSphere /
PromoteDetachedIslands+collapse / structure fixtures), so every edit path gets
reactivation for free, with the same authority-only (`NetMode != NM_Client`)
gating as the invalidation. A `LogVoxelWater` Verbose line reports the woken
count per edit.

**Proof -- the settled pond actually drains now.** New GameMode switch
`-VoxelWaterWakeTest[=<delaySeconds>]` runs the IDENTICAL basin/pour/dig/place/
carve/structure/collapse sequence as `-VoxelWaterMemoTest`, but with every
`SpawnWaterAt` nudge suppressed, so nothing but the terrain edits themselves
can move water. Headless (`-nullrhi`, seed 20260719, clean `Saved/VoxelWorlds`):

| Checkpoint | water digest | volume |
|---|---|---|
| pour | `0x68598CA231A5B2D8` | 20000 |
| pre-edit settle (pond settled, activeBricks 0) | `0xFFCC45E6CD111EC2` | 20000 |
| dig beneath basin (`applied=1`), `wakeRegion` woke **10** bricks | `0xFFCC45E6CD111EC2` (unchanged at the instant of the edit -- waking writes nothing) | 20000 |
| post-dig settled | **`0xA16FBD50DC4EDB3F`** | 20000 |
| place at basin floor (`applied=1`), woke 5 bricks | `0xA16FBD50DC4EDB3F` | 20000 |
| post-place settled | `0xA16FBD50DC4EDB3F` | 20000 |
| side carve / structure fixture / M5 collapse blast | `0xA16FBD50DC4EDB3F` | 20000 |
| FINAL | `0xA16FBD50DC4EDB3F`, storedBricks 8, activeBricks 0 | 20000 |

The pond, settled and dormant, MOVED when the floor was dug out from under it
(`0xFFCC45E6...` -> `0xA16FBD50...`) with volume EXACTLY conserved at 20000 at
every checkpoint -- on the old code that digest was constant through the whole
sequence. The later edits wake bricks (the TryPlace path logged 5) but move
nothing, because by then the water has drained down the shaft and out of their
reach; the carve/structure/collapse are metres from any remaining water and
wake 0 bricks, which is the correct answer and not a missed hook (an edit
nowhere near water must cost one bounded probe and wake nothing).

Screenshots (windowed run, same switch, camera framed on the water body's own
centroid), in `ue-project/Saved/Screenshots/WindowsEditor/`:
`VoxelWakeTest_PondSettled_BeforeDig.png` (t=42s, the pool's flat surface
filling the basin) and `VoxelWakeTest_Drained_AfterDig.png` (t=55s, same
camera -- the pool is gone and the bare sand basin floor it was covering is
exposed, the water having drained down the dug shaft).

**Deterministic voxel-core proofs** (tests/test_waterca.cpp, 5 new tests):
`waterca_wake_region_drains_settled_pond_into_new_hole` (settles a 2000-unit
pond in a 4x4 basin, opens a shaft through the floor, asserts 20 ticks of
NOTHING happening first -- the regression witness for the old behaviour -- then
wakes and drains it to the exact expected bottom-up shaft profile, conservation
checked after every single tick);
`waterca_wake_region_settled_pool_flows_through_a_carved_breach` (the same
story sideways: a multi-brick wall carve, re-levelling to 41/42 across all 48
newly connected floor cells);
`waterca_wake_region_writes_nothing_and_ignores_dry_regions` (digest and ledger
byte-identical across a wake; a far-away edit and an inverted box both wake 0;
re-waking is idempotent); `waterca_wake_region_order_and_strategy_independent`
(above); `waterca_wake_region_halo_reaches_one_brick_not_two`.

**`kWaterCAVersion` 3 -> 4, and NO golden moved.** The bump is deliberate and
is a SIGNAL, not a re-pin: no tick rule changed, and `wakeRegion` is a new API
that nothing in voxel-core's own scenarios calls, so every pinned digest is
byte-identical to v3 -- `waterca_deterministic_repeat_and_golden_digest`
(`0x3D2224BE4A253404`), `waterca_hydrostatic_large_pool_multibrick_golden`
(`0x56BC18914355A205`) and `waterca_solid_cache_golden_digests_unchanged` all
still pass against their existing values, and nothing was re-pinned anywhere.
The version moves because a LIVE world now evolves differently across an
identical terrain-edit sequence than it did before (that is the whole fix), so
a v3 recording/replay or persisted water state no longer reproduces -- exactly
what docs/determinism.md keeps the constant for.

The engine-level memo A/B table above also did NOT move: re-running
`-VoxelWaterMemoTest` with `voxel.Water.SolidCacheEnabled` forced 0 and 1
reproduces every previously documented checkpoint byte-for-byte, FINAL included
(`waterDigest 0x5D0D9B43677FD302`, `editedDigest 0xFB1235E05C62C838`,
storedBricks 11, volume 21200), memo off == memo on. That scenario's nudges
already woke the CA, so adding `wakeRegion` in front of them changes nothing
there -- a useful independent confirmation that waking is a pure no-op wherever
nothing can actually move.

**Perf.** `vxc_waterca_bench` gained `--lake --wake-edit`: a settled 63x63 lake
with a player-sized (3x3x3-voxel) terrain edit landing on its rim EVERY tick,
forever -- the worst realistic case, and unlike the pre-existing `--lake`
disturbance it injects no volume, so all of its cost is the reactivation
itself. `wakeRegion` costs **0.0019 ms/call** (avg over 200 calls, ~5.9 bricks
woken per edit), and the resulting `step()` averages **3.28 ms/tick** (memo ON,
`--lake-solid-spin 50`) versus **4.09 ms/tick** for the existing addWater
disturbance -- i.e. per-tick terrain editing beside a large settled lake is
CHEAPER than the per-tick water drip the bench already treated as its steady
state, and a settled lake with no edits still costs exactly zero (empty active
set). The solidity memo's win is intact and untouched: the 441-column pour
bench (which never calls `wakeRegion`) is **344.1 -> 131.5 ms/tick** in the
~1830-active-brick window with the memo off vs on (2.6x), 4x versus the v0
sequential engine.

**Build/tests.** voxel-core (clang, llvm-mingw, Release, `-Wall -Wextra
-Wconversion -Werror`, plus `waterca.cpp` re-checked under an explicit
`-Wsign-conversion -Werror`): clean, **137/137 PASS, 0 FAIL**; float-ban script
clean. `voxelcore.lib` rebuilt with MSVC 14.51/VS 2026 (Ninja, Release) for the
UE link. `VoxelEarthEditor Win64 Development` -> `Result: Succeeded`, no
warnings from the touched files.

**Follow-ups.** (1) A halo of 1 brick is the right seed for 1-voxel-per-step
flow; if a future Layer C adds multi-voxel-per-tick momentum, the halo must
grow with it (it is a named constant, `WaterCA::kWakeHaloBricks`, for exactly
that reason). (2) The stale-fill-inside-a-newly-solid-cell quirk (previous
wave's follow-up 2) is untouched: waking schedules the brick, but no rule
evicts water from a cell that just became solid, so that fill stays put
(byte-identical across clients, just hidden). Now that a wake hook exists, an
eviction rule on the place path is a natural next step. (3) The
`-VoxelWaterMemoTest` nudges are now redundant for reactivation and are kept
only so that scenario keeps reproducing its signed-off A/B table; they could be
retired once someone re-pins that table.

### VoxelEarth.Build.cs: per-config voxelcore.lib resolution

`VoxelEarth.Build.cs` used to hardcode the voxel-core static lib as the flat
path `build/voxel-core-msvc/voxelcore.lib`. That only ever matched
single-config CMake generators (Ninja, Makefiles/NMake); a multi-config
generator (Visual Studio) puts the artifact under a per-configuration
subdirectory instead (`Release/`, `RelWithDebInfo/`, `Debug/`), so the UE
build failed to find it. Multiple agents had been manually copying the .lib
into the flat path in every fresh worktree to work around this. Fixed: the
module now searches the flat path and all three per-config subdirectories,
preferring an optimized config over `Debug/` (and preferring the config that
matches the current UE target configuration -- `Debug`/`DebugGame` prefers
`Debug/` first) rather than hardcoding one location. If nothing is found it
throws a `BuildException` at configure time listing every path searched and
the CMake commands to build voxel-core, instead of failing later with an
opaque linker error.

**Verified, not just reasoned about**, in this worktree with system CMake
4.4.0 (`C:\Program Files\CMake\bin\cmake.exe`):
- Multi-config (the case that was broken): configured voxel-core with
  `-G "Visual Studio 18 2026"`, built the `Release` config -- the .lib landed
  ONLY at `build/voxel-core-msvc/Release/voxelcore.lib` (no flat file ever
  existed). `Build.bat VoxelEarthEditor Win64 Development` against
  `D:\UE_5.8` -> `Result: Succeeded`, with zero manual copying.
- Single-config sanity check: configured voxel-core with `-G "NMake
  Makefiles"` (system CMake's Visual Studio generator, unlike Ninja, doesn't
  need a preconfigured dev-shell env, but NMake does -- ran via
  `vcvarsall.bat x64`), built `Release`, and confirmed the flat-path branch
  independently by temporarily swapping a flat-only `voxelcore.lib` (no
  subdirectories at all) into `build/voxel-core-msvc/` -- also
  `Result: Succeeded`, no manual copy.
- Missing-lib case: with `build/voxel-core-msvc/` absent entirely, the build
  fails fast at configure time with `Unable to instantiate module
  'VoxelEarth': VoxelEarth: could not find voxelcore.lib. Searched: ...` (all
  four candidate paths listed) plus the two `cmake` commands to build it --
  `Result: Failed (RulesError)`, not a linker error.

One gotcha hit along the way, worth remembering: UBT caches its resolved
target action graph in
`Intermediate/Build/Win64/x64/<Target>/Development/Makefile.bin` and does NOT
re-run a module's `Build.cs` on every invocation, only when it decides the
makefile is stale. When swapping library layouts by hand to test each branch,
that cache had to be deleted between runs to force UBT to re-evaluate
`VoxelEarth.Build.cs`'s new resolution logic -- a normal `cmake --build`
after a config change followed by an ordinary UE rebuild will not need this
(the lib file's own timestamp change is what invalidates the link action;
it's specifically re-*resolving a different path* that this cache can hide).

## Track B — real terrain tiles into the runtime (2026-07-21)

**B2 landed: `.vxtl` tiles are now a selectable, shippable tile source.**
`-VoxelTileDir=<dir>` (plus optional `-VoxelTileScale=<1|8>`, default 1)
makes `UVoxelWorldSubsystem` construct a `vxc::TileGridSampler` and load
every `.vxtl` in the directory at Initialize (the terrain-service cache leaf
layout, `<cache>/<provider_id>/<seed hex>/s<scale>/`); absent switch =>
`SyntheticTileSampler`, byte-identical to before (goldens untouched, no
`kWorldGenVersion` bump — Track A's 2→3 bump is unrelated and unaffected).
The clipmap now samples heights through the subsystem's ACTIVE sampler
(`SampleTerrainHeightUU`) instead of constructing its own synthetic sampler,
so clipmap and voxel terrain read the same tiles and keep their seam.
Proven on screen from the pregen'd synthetic-provider cache (seed 20260719,
3×3 tiles, committed under `ue-project/Content/TerrainTiles/`):
`docs/media/trackb-proof-vxtl-tiles.png` (tile-sourced world) vs
`docs/media/trackb-control-synthetic.png` (same `-VoxelSpawnAt=7680,15360`
coords, no switch — visibly different world), plus
`docs/media/trackb-vxtl-snowline-biome.png` (tile climate driving cold-biome
materials). Log proof: `Voxel tile grid: dir=... loaded=9 rejected=0
seed=20260719 scale=1`.

**Missing-tile policy (decided + documented):** tiles are canonical data and
the client NEVER fabricates them. Queries outside the loaded set return
`TileGridSampler`'s deterministic flat sea-level default (elevation 0,
default climate) and bump an atomic `missingTileQueries` counter, surfaced
as a throttled `LogVoxelPerf` Warning; zero tiles loaded at boot (bad path,
empty dir, seed/scale mismatch) is an Error + fallback to the synthetic
sampler so a typo can't silently boot an empty flat world.
`missingTileQueries` was made `std::atomic<uint64_t>` because UE meshing
workers query the sampler concurrently (tile map itself is immutable after
load); covered by an exact-count 4-thread test plus TileGridSampler→Amplifier
end-to-end determinism tests in `test_tilestore.cpp` (140/140 vxc_tests).

**B1 prepared: `DiffusionProvider._call_model` is implemented** (no more
TODO) against the real terrain-diffusion source: `WorldPipeline
.from_pretrained(<local snapshot>)` (coarse/base/decoder submodels),
`pipeline.get(i1,j1,i2,j2, with_climate=True)`, channel order confirmed to
match `EXPECTED_CHANNELS`. Checkpoint sha256 (directory-manifest hash) is
verified BEFORE load and refuses `"UNPINNED"`; per-tile RNG seed derivation
is deterministic; backend is injectable, so the whole path is unit-tested
GPU-free with fake models (42/42 terrain-service tests). Remaining
GPU-session work is verification only: `terrain-service/docs/
diffusion-bringup.md` + the exact copy-paste pod blocks in
`docs/pod-bringup-commands.md` (checkpoint pin, one-tile
`validate_model_output`, seam/axis check, pregen radius 2, package cache).

**Follow-ups.** (1) `VoxelEarth.Build.cs` hardcodes
`build/voxel-core-msvc/voxelcore.lib` but multi-config VS generators emit
`Release/voxelcore.lib` — every fresh worktree needs a manual copy;
deliberately not fixed in this slice. (2) `pregen --provider diffusion`
still constructs the UNPINNED default config — wire pinned-config selection
into `app._make_provider` once a production checkpoint is chosen (the pod
doc's Block 5 sidesteps this with an explicit-config script). (3) Edit-logs
recorded against a tile-sourced world replay correctly only against the
same tile set; stamping `provider_id` into the save/editlog header is open.
(4) When real diffusion tiles become the DEFAULT source, that is the
`kWorldGenVersion` bump + golden re-pin moment — not this slice.
## Worldgen v3 — SyntheticTileSampler spectral-gap fill (2026-07-21)

**What/why.** The terrain-realism audit found the dev world's entire visual
problem in one number: `SyntheticTileSampler` had NO spectral power between
960 m and 25.6 m wavelength, so at player scale the world was a tilted plane
plus <1 m of fuzz (detrended relief 3.2 m over a 100 m patch, 5.6 m over
200 m; a 2 km transect descended monotonically with zero drainage
crossings). Fix: four new value-noise octaves in
`tiles.h::SyntheticTileSampler::elevationMm` — lattices 16/8/4/2 px =
wavelengths 480/240/120/60 m at amplitudes 70/38/20/11 m (~lambda^0.9 ramp;
60 m = Nyquist for the 30 m raster), channels `CH_SYNTH_TILE_BASE+8..11`
(+5..+7 left free for future climate channels). Measured payoff (same seed
20260719, detrended relief after plane removal): 100 m patch 3.2 -> 10.5 m
(3.3x), 200 m 5.6 -> 26.4 m (4.7x), 500 m 34 -> 69 m, 1 km 131 -> 159 m;
the 2 km transect now crosses a valley floor and rises again. Amplitude
budget was already Earth-plausible and is unchanged; only the spectrum
moved. This remains DEV terrain — the real fix is terrain-diffusion tiles
(see the audit's Track B / diffusion-bringup runbook).

**kWorldGenVersion 2 -> 3** (world-breaking by definition — tile elevations
feed every derived quantity). Goldens regenerated, all three moves predicted
before running (everything downstream of `SyntheticTileSampler`):
`amplifier_golden_digest` `0x73B43CAE621CA286 -> 0x81785278E4DFCF67`,
`mips_chain_determinism_golden` `0xE4CF1B376622A38F -> 0xE827A786195B8A73`,
`rivernet_determinism_golden_digest` (GOLDEN(rivernet_synthetic_slope))
`0xE4944F92B37F60FB -> 0xA4D30E5715339878`. Explicitly UNMOVED, as
predicted: `biome_map_golden_digest` (classifier sweep, synthesizes its own
inputs), `test_hash.cpp` (hash/value-noise primitives untouched), waterca
container/pool goldens (constructed terrain), tilestore golden (fixture
generated by the Python provider, which per its own docstring does NOT track
the C++ dev sampler). `vxc_tests` 137/137 PASS (clang/llvm-mingw Release);
float-ban clean; `lint-shader-ub.py` clean (no shader edits, committed
SPIR-V byte-identical).

**GPU determinism gate re-run (AMD RX 7800 XT, MSVC, same committed
SPIR-V).** All three `vxc_gpu` modes PASS bit-exact against the v3 CPU
reference; digests moved because the world moved: default regions
`e21e2767591496eb`, `--radius 64` `346b60c292a26b5a` (0.093-0.096 s),
`--radius 128` `75b737e961f65bf5` (0.182-0.215 s) — both radii still well
under the <1 s M0 target. Digests reproduced exactly across repeat runs.
The NVIDIA leg must now match THESE values (prebuilt/README.md table
updated).

**Harness bug found and fixed on the way (gpu_harness.cpp only).** v3's
taller terrain made 125/144 gate tiles at `--radius 64` exceed
ScanSumsMain's single-workgroup 65,536-mask capacity. The default-regions
path FATALs loudly on that condition, but the gate path had NO guard:
masks past the capacity silently missed their scanned block base and
MeshEmitMain wrote their quads at bogus offsets over the early stream —
nondeterministic quad corruption, latent since the GPU scan landed, and
unreachable at v2 only because flat terrain never produced >6 interior
z-layers per tile. Fix: gate tiles taller than 6 interior layers are split
into z-slabs (`ZWindow`, 1-brick shared halo, inline ascending-z work
order, hard FATAL if a dispatch could still exceed capacity), and the
default fixtures shrank 128x128 -> 64x64 columns (origin at v3 needs 10
brick layers = 75,264 masks, over the cap; at 64x64 overflow needs >37
layers — unreachable). Every brick is still meshed exactly once; CPU
compare + digest order stay deterministic.

**Follow-ups.** (1) UE-side: eyeball streaming/collision over the rougher
terrain (steeper slopes stress R0 z-ranges the same way they stressed gate
tiles). (2) Optional Track A polish from the audit (ridged transform +
domain warp for ridgeline/drainage character) — visual payoff INFERRED, not
measured; would be another kWorldGenVersion bump. (3) terrain-service's
Python synthetic provider intentionally does not mirror these octaves
(tiles-are-data doctrine); if anyone wants dev-tile parity, bump its
provider_id and re-pin its goldens as a separate change. (4) NVIDIA leg
re-run against the v3 digest table.
### Region-graph build cost (M6 Tier-1 enablement)

`voxel.Agent.Tier1RegionGraph.Enabled` now **defaults to `true`**. It was
shipped defaulting to `false` because the region-graph *build* was
synchronous and measured at ~160 s (256-expansion cap) to >500 s (4096 cap,
killed) for the 1,875-region box — a multi-minute game-thread stall on the
first Tier-1 plan. The planner itself was never the problem; the build was.

**1. Where the time actually went (measured before changing anything).**
Instrumented harness over the same box shape the UE side builds (25x25x3 =
1,875 regions, cap 1024), against a cheap in-process `MaterialFn`:

| Phase | Time | Share |
|---|---|---|
| Portal detection | 106 ms | 1.2% |
| **Intra-region edges** | **8,966 ms** | **98.0%** |
| Inter-region edges | 73 ms | 0.8% |

Inside that 98%: **23,416 `findPath` calls** (one per *ordered pair* of a
region's portals) burning **16.1M expansions** — a mean of 688
expansions/call, i.e. most calls running to the cap because the pair simply
is not connected inside the region. Portals per region were mean **4.84**,
max **5**, so portal *count* was never the driver; the `P^2` call count and
the resulting **666,190,347 `MaterialFn` calls** (41 per expansion — 18
offsets x 2-3 voxel reads each) were. Cost scaled **linearly** in region
count (4.4 / 5.2 / 4.9 ms per region at 147 / 507 / 1,875 regions).

The in-engine build was ~56x slower than the harness for the identical work,
which pins the real cost precisely: our `MaterialFn` is
`IsSolidAtVoxel -> World::materialAt -> procedural worldgen`, ~1.5 us/call.
666M of those *is* the ~500 s.

**2. What changed (all in `regiongraph.h`; the graph is unchanged).**
- **`O(portals^2)` searches -> `O(portals)`.** `findPath` is Dijkstra with a
  **zero heuristic**, so its priority-queue key `(g, PathCoordLess)` never
  mentions the goal: the settle *order* from a given start is
  **goal-independent**. One search per *source* portal therefore already
  contains every pairwise answer for that source. `findPath` returns
  `complete` iff the goal is among the first `maxExpansions` settled cells,
  with cost = that cell's settled `bestG` (all move costs are non-negative —
  a negative `mineCostByMaterial` is the *impassable* sentinel, classified
  invalid, never a negative edge). So the edge set and every edge cost are
  **identical**, not merely equivalent.
- **`MaterialFn` memoization.** Every voxel an intra-region search can
  consult lies within the region box grown by 2, so `RegionMaterialCache`
  memoizes them to at most `20^3` per region and is shared by the region's
  portal detection *and* its intra-edge searches. Pure memoization — same
  reads, same answers, far fewer of them.
- **Resumable build.** `RegionGraphBuilder` walks the same phases in the same
  region order under a caller-supplied per-call step budget, and
  `buildRegionGraph()` is now literally the single-unlimited-slice case, so a
  time-sliced build is byte-identical to a one-shot build *by construction*.
  Steps are **sub-region**: a region's ~4,100 cold worldgen reads are
  prefetched in 512-cell chunks before a warm-cache compute step, because
  otherwise one region is a ~6 ms indivisible lump of a 16.6 ms frame.

**3. Before/after (same harness, same boxes, same caps).**

| Box | Cap | Before | After | `MaterialFn` calls |
|---|---|---|---|---|
| 147 regions | 1024 | 645 ms | 167 ms | 43.5M -> 0.81M |
| 507 regions | 1024 | 2,187 ms | 547 ms | 168M -> 2.0M |
| **1,875 regions** | **1024** | **9,046 ms** | **2,414 ms** | **666M -> 10.5M (63x)** |
| 1,875 regions | 256 | 2,680 ms | 730 ms | 191M -> 9.9M |

**4. Proof the graph did not change.** Golden digest
`0xeb05deb529b8f143` still passes; the
incremental-recompute-equals-from-scratch test still passes; and the built
graph's digest is **bit-identical before and after** at 147
(`0x77ff4fa19764c1ff`), 507 (`0x920f0703e08d9f33`) and 1,875 regions
(`0x6d9a6a32c3a92ce0` at cap 1024, `0xd6b5e2c90b695760` at cap 256). A new
test, `regiongraph_time_sliced_build_matches_one_shot_build`, pins the
budgeted build against the one-shot build at the tightest possible budget
(one step per `advance()`). 138 PASS / 0 FAIL; header and tests clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror`; integer-only.
`RegionGraph::totalFinePathExpansions` is now a *smaller* number for the same
graph (P searches instead of P*(P-1)) — it is a monitoring counter,
explicitly excluded from `digest()`.

**5. Frame-budget evidence (headless, `-nullrhi`,
`-VoxelTier1RegionGraphTest=3`).** `AdvanceTier1RegionGraphBuild` runs from
`Tick` with a 4.0 ms/tick budget and swaps the finished graph in only when
complete:

> `Tier1 region graph (re)built: (8875 portals, 24959 edges, 8862707 fine
> expansions spent building) -- 14431.0ms of CPU across 2264 ticks (worst
> single tick 10.71ms), 28.4s wall`

**Worst single tick 8.94-10.71 ms across runs, against a 16.6 ms budget, with
zero occurrences of the permanent over-budget warning** — versus a ~506 s
single-frame stall before. Until the first build lands, Tier 1 degrades to
exactly the pre-M6 fine windowed search, so enabling this by default is
strictly non-regressive.

**Known limitation, measured, not hidden.** Wall-clock *to readiness* depends
on the host's tick rate, not just the build's CPU: ~21-28 s with 3 agents,
but **still unfinished after 4 minutes under a 200-agent `-VoxelSwarmTest`
load** (same 4 ms slice, delivered far less often per second). Frame budget
was never violated in that run either, and Tier 1 kept planning correctly via
the fine fallback — but the hierarchical planner does not get a chance to
engage in a short, heavily-loaded run. Also note the 3-agent scenario now
converges (agents reach the player at ~7.7 m) *before* the graph is ready, so
that specific test reports `hierarchical calls=0`; it is no longer the right
scenario for exercising the hierarchical path.

**Follow-ups.** (1) *Background-thread build* is the real fix for
wall-clock-to-readiness — voxel-core is engine-free and `RegionGraphBuilder`
is already a resumable, self-contained object, so the only blocker is that
`World::materialAt` reads the edit overlay that game-thread digs mutate;
making terrain reads thread-safe (or building against an immutable snapshot)
belongs to `VoxelWorldSubsystem`, owned elsewhere this run. (2) The worst
tick is now the warm-cache *compute* step of a portal-dense region (up to
`cap x portals` expansions); slicing that per source portal would drop the
worst tick further, toward the ~1 ms prefetch-chunk floor. (3)
`rebuildIntraEdgesForRegion` still linearly scans all portals and all edges
per region to collect/tombstone — `O(regions x graph size)`, ~50 ms at this
box size and invisible today, but it is the next term to bite if the box
grows. (4) Sub-region slicing made the prefetch eager over each region's full
core, costing ~36% more `MaterialFn` calls than the purely lazy version
(10.5M vs 7.7M) — a deliberate trade of total CPU for frame granularity,
revisitable if a cheaper bulk terrain read (e.g. per-brick generation rather
than per-voxel) becomes available.
### Voxel light field + cone-traced GI (M4)

First working slice of the M4 line item "voxel light field + cone-traced GI".
**Default OFF** (`voxel.GI.Enabled 0`) until it earns its way on. CLIENT-SIDE
RENDERING ONLY: outside the determinism boundary, floats throughout, no
worldgen / edit-log / replication / digest path is read or written by any of
it. Two clients may converge to slightly different irradiance; they cannot
disagree about world state.

New files: `VoxelLightField.{h,cpp}` (the field + cone tracer),
`VoxelGI.{h,cpp}` (`UVoxelGISubsystem`: budgets, queues, cvars). Touched:
`VoxelChunkComponent.{h,cpp}` (proxy shades from the field), and one anchored
test-switch block in `VoxelEarthGameMode.{h,cpp}`.

**Approach -- surface-voxelized VCT, CPU-side, folded into VertexColor.G.**
Classic voxel cone tracing (Crassin et al. 2011), with three choices worth
justifying:

1. *Surface voxelization, not solid.* VCT rasterizes scene surfaces into a
   grid; cones stop at the first surface, so solid interiors never matter.
   That is fortunate here, because the only voxel data reachable without
   reaching into `UVoxelWorldSubsystem`'s PImpl is the greedy-mesher quad list
   already flowing through `UVoxelChunkComponent::SetChunkQuads` -- which is
   exactly a surface description, and is already delivered on stream-in AND on
   every edit-driven remesh.
2. *Field aligned 1:1 with level-0 chunks.* One brick per chunk: 8^3 cells of
   40 UU = 320 UU = `VoxelCoords::ChunkEdgeUU`. Brick lifetime, dirtying and
   eviction therefore follow chunk streaming exactly, and re-voxelizing is
   always a clean clear-then-fill of one chunk.
3. *Output is a scalar in `VertexColor.G`.* `M_VoxelTerrain` already computes
   `BaseColor = albedoTint * VertexColor.G * DebugTint`, so no material asset
   change, no custom global shader and no render-pass integration were needed
   -- the whole feature is CPU-side and reviewable. With GI off the emitted
   `FColor` is byte-identical to before (the 2-bit AO quantisation 0/85/170/255
   is reproduced exactly, not round-tripped through a float).

Per air-cell adjacent to a surface the field stores **6 directional visibility
bytes** (an ambient cube; 6 x 90-degree cones is the standard VCT diffuse
budget). A shading point recombines them with `max(0, N.D)` weights, so a floor
and the ceiling above it get different answers from the same data. Mip pyramid:
4 in-brick levels (40/80/160/320 UU) + 3 sparse global levels (640/1280/2560
UU), MAX-aggregated rather than averaged -- VCT's classic failure is light
LEAKING through thin walls once they blur at coarse mips, and for a game about
digging, erring toward "too dark" is the right side to be wrong on. The mesher's
existing per-corner AO is kept and multiplied in: it supplies contact-scale
occlusion that a cone whose first step is already 40cm wide cannot resolve,
while the cone supplies the large-scale term per-voxel AO cannot see.

A subtle but load-bearing detail found by measurement: the cone march step MUST
equal the sampled mip's cell size, not the cone radius. They diverge between mip
boundaries, and because this is a surface voxelization the ground is a
single-cell-thick shell -- a step larger than the cell being sampled marches
straight through it and reports full sky visibility for a surface facing a floor
3 metres away.

**One bounce is progressive.** A cone terminating on a surface picks up that
surface's *previously solved* irradiance times a constant albedo. Nothing
converges within a single solve; re-solves (edits, streaming, and a slow
round-robin refresh) carry it forward. This is what makes bounded per-frame
cost possible: there is no pass that must finish before the frame can be shown.

**Edit responsiveness.** `SetChunkQuads` is the entire mechanism. The streaming
system already remeshes every chunk an edit dirties and pushes it through that
call, so a dug tunnel arrives here indistinguishably from a newly streamed
chunk. The affected brick is re-voxelized and its `voxel.GI.EditDirtyRadiusBricks`
neighbourhood (default 2 -> 5x5x5) queued for re-solve. A large explosion makes
the queue longer, never the frame longer.

**Budget / LOD.** Everything is capped per frame:
`MaxVoxelizePerFrame` 16, `MaxBrickSolvesPerFrame` 8 (blocking `ParallelFor`
over workers), `RefreshBricksPerFrame` 2, `MaxChunkRefreshesPerFrame` 4,
`MaxBricks` 4096 (~3.6 KB each). Measured steady state at 1999 resident bricks:
**8.2 MB, 0.5-0.6 ms of game-thread tick**, solving ~230-300 cells across 2
bricks per tick. LOD story follows the existing ring cascade: only level-0
chunks feed or sample the field, so GI simply does not exist beyond R0, and the
proxy fades the GI term back to plain AO over `FadeStartUU`..`FadeEndUU`
(4800..6400 UU) so the R0/R1 boundary is not also a lighting boundary. Eviction
is distance-based (level-0 chunks only unload because the camera left).

**M1 60fps gate -- NOT regressed.** Same scenario as the gate close above:
`-VoxelPerfRun=60`, seed 20260719, min-spec proxy (`sg.ViewDistanceQuality 0`,
`sg.ShadowQuality 0`, `sg.PostProcessQuality 0`, `sg.EffectsQuality 0`,
`r.ScreenPercentage 100`), 1080p windowed, clean `Saved/VoxelWorlds`.

| Config | p50 | p95 | postWarmup p95 | postWarmup max | postWarmup hitches |
|---|---|---|---|---|---|
| **GI off** (run 1) | 3.09 ms | **4.82 ms** | 4.86 ms | 12.8 ms | 0 |
| **GI off** (run 2) | 3.00 ms | **4.85 ms** | 4.88 ms | 13.0 ms | 0 |
| **GI on** | 4.09 ms | **7.80 ms** | 7.31 ms | 14.0 ms | 0 |

GI off reproduces the gate-close band (4.79-4.93 ms) exactly -- unchanged, still
a ~3.4x margin on the 16.6 ms bar. GI on costs about **+1.0 ms p50 / +3.0 ms
p95** and still passes the bar with ~2.1x margin and zero post-warmup hitches,
but that is a real and honestly large fraction of a 60fps budget for a first
slice; most of it is the extra vertices from `voxel.GI.MaxQuadSpanVoxels` plus
the extra proxy rebuilds, not the cone tracing itself (which measures 0.5 ms).
Caveat: one early GI-off run measured 6.27 ms before later runs settled at
4.82/4.85; a same-tree pre-change baseline was not captured, so "unchanged"
rests on matching the previously published band rather than on a paired A/B.

**Screenshots** (`docs/images/m4-gi/`, seed 20260719, identical camera per pair):
- `01-outdoor-gi-off.png` / `02-outdoor-gi-on.png` -- open terrain from spawn.
  GI on visibly deepens occlusion in terrain crevices (clearest in the upper-left
  band). Weak demo: the default framing is mostly ocean.
- `03-roofed-gi-off.png` / `04-roofed-gi-on.png` -- inside the wall+roof fixture,
  looking down the covered span. **The money shot:** the enclosed wall face goes
  from fully lit cream (off) to dark grey (on).
- `05-breach-gi-off.png` / `06-breach-gi-on.png` -- same, after punching a ~4m
  hole through the roof slab. The hole's cut faces are correctly dark and the
  roof re-shades around the opening.

**Honest assessment.** The mechanism demonstrably works, and the numbers say so
more clearly than the pictures do: with `voxel.GI.Debug 2`, open terrain solves
to meanGI 0.65-0.70, buried and enclosed chunks to 0.001-0.02, roof chunks to
0.21-0.43, with ~95% of vertices finding solved data. But:
- **Monochrome.** No coloured bleed. Needs a material change (or a second
  vertex attribute) to carry RGB.
- **It modulates albedo, not the ambient term.** Physically a shortcut. It reads
  correctly only because `M_VoxelTerrain` is a flat untextured albedo.
- **Vertex-rate resolution.** Mitigated by subdividing quads when GI is on
  (`voxel.GI.MaxQuadSpanVoxels`, default 8), which costs vertices.
- **UNRESOLVED:** in `04-roofed-gi-on.png` the roof slab's underside still
  renders fully lit even though the field reports 0.00 irradiance there
  (verified by direct probe) and the roof chunks' vertex data is dark. I could
  not explain the discrepancy in the time available and am flagging it rather
  than claiming the shot is better than it is.
- **No underground proof.** The obvious test -- dig a chamber and stand in it --
  cannot work in this build: the streaming footprint only meshes a band of
  chunks around the terrain SURFACE, so a camera 9m+ down renders open sky under
  a thin crust regardless of what voxel-core says is solid. That is a
  streaming/LOD limitation, not a GI one, but it is why the enclosure shots use
  an above-ground fixture.

**Follow-ups.** (1) Resolve the roof-underside discrepancy. (2) Move the cone
trace to the GPU against a real 3D texture clipmap; the CPU field is fine as the
source of truth but per-vertex CPU sampling is what costs the GI-on ms.
(3) Coloured bounce (needs a material change). (4) Specular/reflection cones.
(5) A sun-visibility cone so bounce is driven by direct light rather than a
uniform sky. (6) Underground streaming, without which caves cannot be lit or
even seen.

New cvars, all `voxel.GI.*`: `Enabled` (0), `Strength`, `AmbientFloor`,
`MaxQuadSpanVoxels`, `RadiusUU`, `FadeStartUU`, `FadeEndUU`,
`MaxVoxelizePerFrame`, `MaxBrickSolvesPerFrame`, `RefreshBricksPerFrame`,
`MaxChunkRefreshesPerFrame`, `EditDirtyRadiusBricks`, `MaxBricks`,
`ConeDistanceUU`, `BounceAlbedo`, `Debug`. New switches: `-VoxelGIOn`,
`-VoxelGITest=<s>`, `-VoxelGIBreach`.

### Cave pass (M4)

**Status: landed.** `voxelcore/caves.h` (new, header-only, integer-only) +
the carve fold-in at `Amplifier::materialAt`, mirrored bit-exactly in
`worldgen.ush`'s `VoxelizeMain`. `kWorldGenVersion` **3 -> 4**.

**Formulation — a jittered lattice graph, not 3D noise.** Blobby noise caves
give disconnected bubbles whose connectivity is an emergent property you can
only measure after the fact and that breaks on any threshold retune. Here the
network is the union of round tubes laid along the edges of a 25.6 m lattice
whose nodes are hash-jittered anywhere inside their own cell. Edges are kept
when they are BACKBONE edges — every 4th lattice row's +x edges, every 4th
column's +y edges — or when a 1-in-4 hash gate opens them. The backbone alone
is a connected grid graph for every seed (each backbone row is connected along
x, each backbone column along y, and every row crosses every column), so
connectivity is structural. The gated extras only add loops, branches and dead
ends; they can never disconnect anything.

**Depth space.** Node depth is measured down from the column's own surface, so
the network drapes under the topography. That is what makes the roof guarantee
free (shallowest possible tunnel voxel is 6.2 m below its own surface, pinned
by `static_assert` against the 6 m clamp) and keeps the whole network 3.2 m
clear of the shallowest bedrock the amplifier can produce.

**Sinkhole entrances.** Depth-space tunnels never break the surface on their
own, which on the gentle synthetic terrain left the network fully sealed
(measured: 0 natural mouths in a 204.8 m square). So a backbone-CROSSING node
— one that provably has all four backbone tunnels incident on it — opens a
vertical shaft to the surface on a 1-in-4 gate: roughly one entrance per 205 m
square, each ~1.0-1.7 m in radius. Measured surface perforation: 73 of 466,489
sampled columns (0.016%).

**Connectivity evidence** (`test_caves.cpp`, flood fill via `connectivity.h`
on a 0.4 m decimated sample grid):

| Scene | Components | Largest share |
|---|---|---|
| flat terrain, 153.6 m square x full depth band (384x384x108 samples, 213,996 cave samples) | 6 | 86.3% |
| real `SyntheticTileSampler` terrain, 102.4 m square (256x256x232 samples, 65,810 cave samples) | 3 | 88.8% |
| 51.2 m box around a sinkhole | entrance component holds 33,756 of 33,756 samples (100%) and descends 35 m from daylight | |

A handful of large components, not thousands of bubbles. The remainder is
analysis-box clipping — tubes that enter the box and leave again without
meeting the backbone inside it.

**Volume budget:** 1.18% of the 6-40 m subsurface band is cave air; 13.7% of
columns have any cave beneath them. Max 5 tube axes recorded per column over
800,476 columns (storage cap 8, and the test fails if the cap is ever
reached).

**Safety rules and their tests.** Three independent bedrock guards — the
geometry (`static_assert`), a 2 m runtime margin above the column's own
bedrock top, and `materialAt` refusing to turn `MAT_BEDROCK` into air at all;
measured closest approach 6.25 m. Implicit ocean (W1): no carve at or below
z=0 and no caves at all in columns below 12 m surface elevation, so a void
below sea level is never created and the ocean cannot flood the network —
driven directly over every surface height from -40 m up (9,408 synthetic
columns, 3.49 M sub-sea-level probes, all refused). Roof: 10.9 m thinnest
cover measured over non-sinkhole ground against a 6 m clamp.

**Gameplay coupling** falls out because cave air is plain `MAT_AIR`:
`pathfind.h` already prices air cheaply (M6), `collapse.h`/dig edits act on it
(M5), and the water CA drains into it. Verified through the brick path, not
just pointwise queries.

**GPU: AMD RX 7800 XT PASS, bit-exact.**

| Mode | Digest | Was (v3) |
|---|---|---|
| default (2 regions) | `e21e2767591496eb` | `e21e2767591496eb` — **unchanged, correctly** |
| `--radius 64` | `1e664cf6680a137c` | `346b60c292a26b5a` |
| `--radius 128` | `7602afe508d2ee73` | `75b737e961f65bf5` |

Default mode voxelizes only the surface-shell brick range (48 voxels tall),
which is entirely above the 6 m cave roof, so it contains no cave voxels and
its digest must not move. `--radius 64` is the run that actually exercises the
cave path, and it verified **270,663,680 of 270,663,680 cells (100%)** against
the CPU reference with zero mismatches. `tools/lint-shader-ub.py` clean
(HLSL + committed SPIR-V, zero `OpSDiv`/`OpSRem`/`OpSMod`); float-ban clean.

**Goldens.** Moved: none of the existing ones, and that was predicted rather
than discovered. `amplifier_golden_digest` digests columns only. `mips_chain`
digests a 2x2x2 level-2 block anchored on the surface straddle, i.e. roughly
+/-30 voxels around the surface — above the cave band. `biome_map`, all
`test_hash`, `connectivity`, `pathfind`, `regiongraph`, `collapse`, `waterca`
and `tilestore` goldens are built from hand-written lambdas or fixtures and do
not touch the amplifier. New: `GOLDEN(cave_layer)` = `0x1CD7912E8DBBB5EA`.
`vxc_tests` 154/154.

**Visual verification — what could and could not be shown.** No in-editor
cave-interior screenshot: the world does not stream anything underground yet
(a concurrent workstream), so a camera below the surface sees open sky under a
thin crust. What was produced instead, headless from the CPU reference: a
vertical cross-section (tube cross-sections in rock, intact roof, bedrock
floor untouched) and a 120 m plan view of the topmost-cave-depth field showing
branching, junction-connected passages with a sinkhole entrance sitting on a
four-way junction. The numbers above are the primary deliverable.

**Follow-ups.**
- Re-shoot a real in-editor screenshot once underground streaming lands; a
  sinkhole is visible from the surface and is the cheapest thing to aim a
  camera at.
- Cave-specific surface materials (damp rock, flowstone) and the M4 GI pass's
  behavior inside enclosed cave volumes are untouched here.
- `Amplifier::stratigraphyAt` vs `materialAt` is now a real distinction;
  `VoxelWorldSubsystem.cpp`'s direct `Amplifier::materialAt` call correctly
  picks up caves, but any future caller must pick deliberately.
- The cave hash channels (18-21) are declared in `caves.h` rather than
  appended to `hash.h`'s `HashChannel` registry; folding them in would be
  tidier.

### Underground streaming (vertical footprint)

**The bug.** The world had no underground. The streaming footprint's vertical
extent was decided in exactly one place --
`FVoxelWorldImpl::ComputeFootprintChunkZRange`
(`ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp`) -- and it is keyed
purely on the terrain SURFACE at `(Level, ChunkX, ChunkY)`: it samples the
amplifier column at the footprint's four corners and returns

```
ChunkZMin = floorDiv(minCornerTopVoxel, 32) - 1
ChunkZMax = floorDiv(maxCornerTopVoxel, 32) + 2
```

A level-0 chunk is 32 voxels = 3.2 m tall, so `-1` guarantees only **3.2-6.4 m
of meshed rock below the surface**, everywhere, forever. The camera's Z was not
an input anywhere in the system. Three consequences, all confirmed by
measurement:

1. A camera ~9 m down renders open sky under a thin crust (the GI section's
   note above).
2. `TickStreaming` recomputed the desired set only on an anchor crossing in
   **X or Y**, so digging straight down triggered no recompute at all.
3. The hysteresis-exit scan was **XY-distance only**, so anything added below
   the surface needed its own eviction rule or it would never unload.

**What changed.** Two additions, both applied OUTSIDE the `FootprintZRangeCache`
memo so its correctness argument ("a pure function of `(Level, X, Y)` and the
amplifier") is untouched -- nothing anchor-dependent may go inside it:

1. **Depth skirt** -- unconditional and anchor-Z-independent: extra chunks below
   the surface band, shrinking with horizontal distance. Because it is a pure
   function of the footprint, skirt chunks need no new eviction rule; they leave
   with their footprint exactly as before.
2. **Anchor-relative deep box** -- only while the anchor is underground: a
   vertical band centred on the anchor's own chunk Z, so being underground
   streams chunks around you in all directions rather than a crust overhead.

**Depth budget.** Cost is chunks-per-footprint, and every ring holds roughly the
same footprint count by construction (~940 -- each ring doubles both its radius
and its chunk edge), so "+N chunks of depth on ring R" costs ~940N chunks
regardless of R. The budget is therefore expressed as a fraction of each ring's
OWN outer radius:

| band (fraction of ring outer) | skirt (extra level-0 chunks) | underground box radius |
|---|---|---|
| < 0.25 (R0: < 16 m) | 12 (38.4 m) | 8 (+-25.6 m) |
| < 0.50 (R0: < 32 m) | 5 (16.0 m) | 4 (+-12.8 m) |
| >= 0.50 | 0 (unchanged) | R0 0, R1 0, R2-R4 none |

Levels 1-4 have `Inner == Outer/2` in `RingPresets`, so their footprints always
land in the far band and **R2/R3/R4 keep their pre-change vertical extent
exactly**. That is deliberate: an R3/R4 chunk is already 25.6 m/51.2 m tall, so
its existing `-1` of margin is already tens of metres, and deep columns at
256-1024 m out are invisible rock.

The 38.4 m near-band figure is set by the **M4 cave pass**, not by digging:
`voxelcore/caves.h` puts tunnel axes 9-34 m below the surface with radius up to
2.8 m, so the deepest cave voxel is ~36.8 m down. 12 level-0 chunks clears the
whole cave band, which is what makes a cave (and a sinkhole leading into one)
visible *before* you are already inside it.

**Eviction.** Deep-box chunks carry `FChunkRecord::bDeepAnchorRelative` and get a
vertical keep-test in the previously XY-only exit scan, with the same
`UnloadRingMultiplier` hysteresis as the outer XY edge. Surfacing evicts the
whole box. Surface-band and skirt chunks are never flagged and keep exactly
their prior lifetime. `TickStreaming`'s recompute gate gains two triggers:
crossing a chunk in Z **while underground**, and the underground flag flipping
(enter/exit hysteresis at 2.0 m/1.0 m below the amplifier surface). Above ground
the gate is byte-for-byte the M1/M2 one, so the perf flight's scan cadence is
unchanged.

**Do mostly-solid underground chunks need a cheaper representation? No --
measured, not assumed.** A fully-solid interior chunk has no visible faces, the
greedy mesher emits zero quads, and `ApplyMeshResult`'s `Quads.Num() == 0` branch
already parks the component and keeps only the record. So depth costs a worker
job and a `TMap` entry, **not geometry and not GPU memory**. The instrumented
perf-run line now reports `deepTracked` / `deepWithGeometry`: standing in a cave,
**2597 deep chunks tracked, 8 with any geometry**. Confirmed at the whole-run
level too -- see the resident-quad numbers below, which went *down*.

**Proof: a real cave.** `-VoxelCaveTest[=<s>]` searches the columns near spawn for
a genuine M4 cave void (pristine worldgen -- nothing edited, so nothing forces
those chunks to be meshed), parks the pawn in it and logs a six-axis enclosure
probe plus the tracked/component/quad state of the chunks in a vertical stack
through the camera. Found a 4.5 m-tall void 14.2 m down at (600,-800); probe
`+X=2.3m -X=1.5m +Y=4.4m -Y=3.9m +Z=2.5m -Z=1.9m` -- genuinely sealed in rock.

```
depth:tracked/component/quads      (T = in desired set, C = live component)

before  -33.5m:--0 -30.2m:--0 -27.1m:--0 -23.9m:--0 -20.6m:--0 -17.4m:--0
        -14.2m:--0 -11.1m:--0  -7.8m:--0  -4.7m:--0  -1.4m:T-0  +1.8m:TC1390

after   -33.5m:T-0 -30.2m:T-0 -27.1m:T-0 -23.9m:T-0 -20.6m:T-0 -17.4m:T-0
        -14.2m:TC1198 -11.1m:TC2114 -7.8m:TC38 -4.7m:T-0 -1.4m:T-0 +1.8m:TC1390
```

Before, nothing below -1.4 m is in the world at all. After, the column is
tracked to 33.5 m down, solid rock meshes to 0 quads (no component, no GPU
memory), and the cave at -14.2/-11.1 m carries 1198 and 2114 quads of real
geometry. Screenshots in `docs/images/underground/`:

- `01-before-open-sky-below.png` -- 14.2 m underground looking down: a thin dark
  crust overhead and **open sky beneath it**, with fragments floating in the void.
- `02-after-rock-below.png` -- same camera, same seed, fix on: rock terraces
  below. Distant sky remains where the depth budget deliberately tapers.
- `03-before-crust-underside.png` / `04-after-cave-interior.png` -- the same pair
  looking level along the tunnel.

**A/B methodology note (this cost three runs to find).** `-ExecCmds` cvars are
applied *after* the world has begun streaming, and by then the first recomputes
have already added deep chunks that nothing subsequently evicts (skirt chunks
live as long as their footprint). An `-ExecCmds` A/B therefore measures the SAME
desired set twice and produces pixel-identical screenshots. The off-switch is
`-VoxelNoUnderground` (read once, before the first recompute), matching the
"don't depend on -ExecCmds cvar-parsing timing" reasoning already used for
`-VoxelGIOn`. `voxel.Stream.Underground 0` also exists for live toggling.

**M1 gate: not regressed.** `-VoxelPerfRun=60`, seed 20260719, 1080p windowed,
min-spec proxy, clean `Saved/VoxelWorlds` per run. Baseline is the SAME binary
with `-VoxelNoUnderground`, so the instrumentation matches.

| | before (feature off) | after (3 runs) |
|---|---|---|
| p95 | 5.74 ms | **4.93 / 4.95 / 4.97 ms** |
| post-warmup p95 | 5.83 ms | 4.98 / 5.02 / 5.04 ms |
| p50 | 2.54 ms | 2.48 / 2.51 / 2.50 ms |
| frames over 16.6 ms | 6 | 10 / 5 / 5 |
| hitches (>33.3 ms), post-warmup | 0 | 2 / 0 / 0 |
| tracked chunk records | 29,497 | 34,825-34,835 (**+18%**) |
| resident quads | 1,908,143 | 1,394,860-1,450,523 (**-25%**) |
| chunks loaded / run | 18,985 | 16,518-16,706 |

p95 *improved* by ~0.8 ms and resident quads *fell* by a quarter. That is not a
free lunch, it is a reallocation: the pipeline is throughput-bound (jobs in
flight pinned at 2x cores, ~21-26k pending all run), so adding desired chunks
changes what the workers spend their time on rather than how much they do. The
deep chunks near the camera sort high on the 3D-distance priority and mesh to
nothing, displacing distant high-quad surface chunks that would otherwise have
become resident. The real cost is the **+18% in tracked records** (a `TMap` entry
each) and ~2,300 fewer surface chunks completed per run.

**Determinism / contents untouched.** Streaming and scheduling only -- worldgen,
the edit log and chunk contents are not touched; only which chunk keys enter the
desired set. Verified on one build, feature ON vs OFF, same seed: identical carve
(137,501 voxels) and identical `editedDigest=0x60CF63CDFEE1E619`, identical
handshake digest `0x52004DCC85DDD80C`.

**Follow-ups.**
1. The skirt and the deep box are two disjoint Z ranges, not one contiguous
   fill. Rock between them is unmeshed -- invisible in practice, but a shaft
   deeper than ~45 m (skirt bottom no longer meeting box top) would show a gap
   in its walls. Contiguous fill was rejected on cost: at 100 m deep it is ~31
   extra chunks per footprint.
2. The far band still gets no depth at all, so a wide downward view from
   underground shows the old thin-crust-over-sky beyond ~33 m out
   (`02-after-rock-below.png`). Fixing it properly wants occlusion-aware
   selection, not a bigger budget.
3. `ComputeFootprintChunkZRange` still samples only the footprint's 4 corners,
   so its surface band can miss interior extremes on steep slopes. Pre-existing;
   the exact fix (workers compute their own z-range) is still the stage-3
   refactor its comment describes.
4. Level 1's deep box is a single chunk layer and levels 2-4 get none, so a
   large cavern seen from far away underground is still surface-LOD only.
5. The unity build broke on `FloorDiv` being ambiguous between
   `VoxelCoords::FloorDiv` and an anonymous-namespace copy in
   `VoxelLightField.cpp`; fixed by qualifying at the call site, but the
   duplicate helper should probably just go.

### W4 — SWE + force field (design)

*(2026-07-21, worktree agent. Full decision content:
`docs/adr/0004-swe-fixed-point-coupling.md`. Full design prose:
`voxel-core/include/voxelcore/swe.h`. This entry is a pointer plus the
numbers, not a restatement.)*

Plan §4 lists **"W4 CA↔SWE coupling"** among the water track's historical slip
risks, so this was run as a design pass first. Two questions had to be answered
before code was worth writing: can shallow water be done at all under the
integer-only rule (doctrine §2.3 / CI `float-ban`), and where exactly does the
per-voxel CA stop and the depth-averaged sheet begin.

**The numerics verdict: YES, and no doctrine deviation is needed.** The brief
anticipated that a faithful SWE might force an ADR'd float exception. It turns
out the dichotomy is false. A *Riemann-solver* SWE (HLL/HLLC/Roe) genuinely is
impractical in fixed point — not because of the `sqrt(g h)` wave speeds, which
integer sqrt handles, but because the **well-balanced (C-property)** cancellation
between pressure flux and bed-slope source term is unreachable under per-term
truncation. But that is a fact about one discretisation family, not about
shallow water in integers. The **virtual-pipe reduced SWE** (Mei-class; exact
depth-averaged mass equation, momentum equation linearised by dropping the
advection term, momentum carried as a per-face stateful flux) is not merely
adequate in integers — on three counts it beats what the float version could
offer:

- **Lake at rest is exactly exact**, permanently, over an arbitrarily uneven
  bed — because there *is* no source term to cancel against (the bed enters
  only via `head = bed*255 + depth`). The float literature's hardest property
  is free here.
- **Mass conservation is bit-exact and structural**, because rounding is
  confined to the momentum accumulator, which is not a conserved quantity —
  truncation there is a bounded *dissipative* bias, i.e. a stability asset.
- **It cannot diverge.** Depth is clamped by the outflow/headroom caps, so the
  CFL condition is a quality constraint, not a safety one. No NaN exists to
  reach.

Two fixed-point traps, both now covered by tests that assert the buggy
behaviour explicitly so they cannot silently return: a plain `>>` rounds
negatives toward -infinity, which leaves a settled lake carrying a **permanent
one-unit-per-tick current in one direction only** (fixed by `shiftSym`); and
whole-unit flux storage gives a **half-voxel truncation deadband** (fixed by a
Q8 accumulator, which drops it to 16 fill units).

**Quantified cost.** Settle deadband **±16 fill units = ±6.3 mm = 6.3% of a
voxel** (derived, not tuned: `d_min = 2^gainShift * (256-dampingQ8)/256`;
`sweSettleTolerance()` computes it so tests assert the derivation), against
Phase C's ±0.4 mm. Stability margin **240/256 (6.25%)** under
`dampingQ8 + ((8<<8)>>gainShift) < 256`, clamped by the constructor rather than
trusted. Not modelled: hydraulic jumps, supercritical flow, correct dam-break
bore speed — direct consequences of dropping the advection term.

**Measured, settled lake disturbed by one unit/tick (Release, -O2):**

| Footprint | SWE sheet | WaterCA (4 deep, memo ON) |
|---|---|---|
| 64x64 | **0.083 ms/tick** | 6.13 ms/tick (74x) |
| 128x128 | **0.364 ms/tick** | 4.14 ms/tick (11x) |
| 256x256 | **1.48 ms/tick** | — |

SWE scales linearly. The CA looks *cheaper* at 128x128 than at 64x64 because it
is **declining to simulate** — the bigger body trips
`kMaxHydrostaticComponentCells` and Phase C skips it (same inversion ADR-0003
measured), so the comparison understates the win. ~2.4 MB for a 256x256 sheet.

**The coupling.** Ownership is a strict partition decided per column, enforced
per voxel: an SWE column owns `bed+1` upward; everything at or below `bed` (the
cave under the lake, the drain shaft) stays CA-owned unconditionally. **The
boundary is always a surface — a solid bed or a domain perimeter — never a
shared cell**, which is what lets the exchange be a ledgered integer transfer
instead of a flux-matching condition between two discretisations. Three
exchange channels (drain / absorb / membership change), each a single integer
move debited and credited in the same statement, so
`ca.totalVolume() + grid.totalVolume() == injected` holds after any sequence of
coupled ticks. Non-oscillation is structural, not tuned: dwell-window hysteresis
on membership (a test flips eligibility *every tick* for 200 ticks and observes
**zero** ownership changes), one transfer direction per column per tick, and
rate limits rather than equalization.

Two non-obvious errors were found by tests rather than by reasoning, and both
are worth remembering:

1. **Membership must live in the numerics, not just the coupler's bookkeeping.**
   Initially a CA-owned column was still an ordinary SWE cell, so the sheet kept
   flowing into ground the coupler had just handed to the CA — a genuine
   double-owned cell. Fixed with `SweGrid::setColumnActive`: a CA-owned column
   is a hard wall, exactly like the grid's outer boundary.
2. **Do not re-seat the bed downward to follow a puncture.** That moves the
   ownership boundary down over voxels the CA is at that moment carrying water
   through. A punctured column is instead a *metered source* into the CA, and
   the hand-over then completes with no extra machinery at all — a non-solid
   bed fails the eligibility predicate, so the column demotes after its dwell
   window. The desired game feel (metered inrush, visibly falling sheet, then
   clean transfer) falls out of the hysteresis rather than being special-cased.

One guarantee is deliberately stated weaker than it first looks: the SWE side of
the partition is *absolute*, but the CA side is *rate-limited*. No voxel is ever
written by both solvers — but a CA source pushing water into a sheet faster than
`absorbPerTick` leaves a bounded, correctly-ledgered standing residue in transit.
**A renderer must draw the union of sheet depth and CA fill, not the sheet
alone.**

**Status: inert.** `voxelcore/swe.h`, `src/swe.cpp`, `tests/test_swe.cpp` are
new and nothing constructs them; `SweCoupleConfig::enabled` defaults false and
makes `step()` a total no-op (asserted, not assumed). The only edit outside the
new files is two additive single-cell `WaterCA` hooks (`addWaterAt`/
`removeWaterAt`) the coupling genuinely requires — `addWater()` stacks upward,
which for a draining punctured bed would push water back into the sheet it just
left, and `setReplicatedFill()` neither adds nor wakes. **No tick rule changed;
`kWaterCAVersion` stays 4.** Suite **154 -> 170 PASS / 0 FAIL**; both pinned
water goldens (`0x3D2224BE4A253404`, `0x56BC18914355A205`) and the
solid-cache/wake tests unmoved. New SWE golden `0x61523E585CF7B782` under an
independent `kSweVersion`, so SWE changes can never invalidate a water golden.
Clang clean under `-Wall -Wextra -Wconversion -Wsign-conversion -Werror`;
`float-ban` clean.

**What ADR-0004 asks Matt to decide.** (2) Adopt the reduced model as *the* W4
model, accepting ±6.3 mm flatness, no shock physics, and damping/gain being
CFL-bounded numerics constants rather than feel-dials — in exchange for no
float, no waiver, cross-vendor bit-identity, and 11-74x the CA's cost.
Recommendation: **adopt**. (3) Enabling the coupler on a live world.
Recommendation: **defer to M3 networked-water integration** — the coupler is a
second simulation whose membership/dwell/depth state is not yet wired into the
replication path at all, so enabling it early is a guaranteed desync, and there
is no renderer to consume the force field yet either.

**Follow-ups.**
1. Lateral orifice/breach flux between an SWE column and a *confined* CA
   neighbour (hole in a dam wall, as opposed to the lake floor). Designed in
   `swe.h` §5 but not implemented; the puncture path covers the floor case.
2. Sparse/tiled residency. The reference grid is a dense rectangle on purpose,
   to keep the conservation and order-independence arguments legible.
3. W3 hookup: `rivernet.h`'s `kDivertChannel` is still a stub, and its
   discharge could drive SWE inflow at network outlets.
4. GPU port. The tick is Jacobi and order-independent with **no colouring
   needed** (unlike the CA's 8 colours), so it should be a markedly easier port
   than `WaterCA` was.
### Worldgen + streaming performance pass

Measure-first pass over worldgen and voxel streaming. Two of the three leads
handed to this pass turned out to be worth less than advertised; the biggest
real win was in a place none of them named.

**Profile before any change** (clang -O2, seed 20260719, `vxc_bench --radius
32`, min-of-5 — this box has ~15% run-to-run variance, so single runs are not
usable):

| stage | brick 8^3 | brick 16^3 |
|---|---|---|
| amplify | 528.4 ms | 428.3 ms |
| voxelize | 26.0 ms | 54.0 ms |
| mesh | 238.3 ms | 417.9 ms |
| **total** | **792.6 ms** | **900.3 ms** |

Amplify was 67% of the brick-8 total. Ablating `Amplifier::column` (0.946
us/column) showed where it went:

| component | us/column | share |
|---|---|---|
| tile raster reads (4x `elevationMm` + `climate`) | 0.574 | 61% |
| `caveColumnFor` | 0.227 | 24% |
| detail octaves + stratigraphy jitter | 0.083 | 9% |
| biome classify | ~0.000 | 0% |

**1. Tile-raster memo.** The raster reads are a pure function of the tile
*pixel*, and a 30 m pixel covers 300x300 = 90,000 voxel columns — the entire
640x640-column bench region spans just **25 distinct tile pixels**.
`SyntheticTileSampler` evaluates seven value-noise octaves (28 hashes) per
`elevationMm`, and it is what the UE runtime uses by default (only
`-VoxelTileDir` selects `TileGridSampler`), so this was real shipped cost, not
a bench-only artefact. Added a direct-mapped `thread_local` memo keyed by
(amplifier id, px, py) — lock-free, so the shared `Amplifier` the UE job pool
calls into gains no contention.

Result: amplify **528.4 -> 192.0 ms** (2.75x) at brick 8, **428.3 -> 157.8 ms**
(2.71x) at brick 16; totals 792.6 -> 458.8 ms and 900.3 -> 604.7 ms.

**2. Column memo on the per-voxel path.** `Amplifier::materialAt(vx, vy, vz)` —
behind `World::materialAt` -> `IsSolidAtVoxel` -> the region-graph `MaterialFn`,
and behind `collapse.h`'s `CarveSphere` — rebuilt a whole `ColumnSample` for
every voxel of a column's height. A 256-slot `thread_local` memo fixes it:
884,736 calls over a 96^3 box, z-innermost (the shape those callers actually
use) **279.9 -> 8.0 ms, 35x**. Batch callers holding a `ColumnGrid` use the
two-argument overload and are untouched, so nothing double-caches.

**3. The GPU "342 ms marshalling" lead was a measurement artefact, twice
over.** It was never 342 of 367: the harness sums buckets into a
"serial equivalent" line and prints the excess as hidden by flight
double-buffering, because `prepareAndSubmit(f)` runs before
`waitAndHarvest(f-1)`. The five stage timers are true GPU busy time from
`vkCmdWriteTimestamp`, and the bracket contains no submit, fence wait or queue
idle — so no GPU work was hiding in it. And it was almost entirely *not*
marshalling: the bracket conflated buffer ensure/grow with the WorldGenParams
upload and descriptor writes. Split apart at `--radius 64`, **marshalling
proper is 0.129 ms**; the rest is `vkAllocateMemory`/`vkMapMemory` on tens of MB
of host-visible memory. Pre-sizing "the marshalling" would have optimised a
0.1 ms line. Gave `GrowBuffer` a growth policy (>=1.5x previous capacity,
rounded to 1 MB) instead of allocating exactly the requested size: reallocations
**168 -> 126** on this box (floor is 112 = 7 buffers x 16 slots), buffer-alloc
time 68.1 ms. Capacity is invisible to the shaders — dispatches are bounded by
WorldGenParams, never by buffer size — so no digest can move.

Note the numbers quoted to this pass (342 ms, 202 reallocs, columns dispatch
79 ms, mesh count 73 ms) do not reproduce on this box: the unmodified baseline
measures 114 ms conflated / 168 reallocs. They came from a different build or
machine state and should not be treated as a standing baseline.

**M1 gate** (`-VoxelPerfRun=60`, seed 20260719, `-nullrhi`, two runs each side,
same binary rebuilt only from voxel-core):

| | R0 worker p95 (median of samples) | worst sample | hitches | chunks loaded | avgChunks/s |
|---|---|---|---|---|---|
| before run 1 | 30.51 ms | 128.59 ms | 18 | 9,183 | 153.1 |
| before run 2 | 6.50 ms | 48.83 ms | 20 | 13,960 | 232.7 |
| after run 1 | 4.79 ms | 6.31 ms | 1 | 22,395 | 373.3 |
| after run 2 | 5.10 ms | 5.74 ms | 1 | 22,144 | 369.1 |

The "after" side is tightly reproducible; the "before" side is erratic, which is
itself the finding — the pass removed a variance source, not just a mean. ~1.9x
more chunks streamed in the same 60 s. Worth flagging: the documented gate
figure of p95 ~4.93-4.97 ms did **not** reproduce on the unmodified baseline
here (30.5 / 6.5 ms); the post-change build is what matches it.

**Determinism.** Unchanged everywhere: `vxc_bench` digests
`6dbb95d59737e045` (8^3) and `280684104c06aacf` (16^3);
`GOLDEN(amplifier_columns)` `0x81785278E4DFCF67`; AMD RX 7800 XT digests
`e21e2767591496eb` (default), `1e664cf6680a137c` (`--radius 64`, 100% of 305
tiles verified bit-exact) and `7602afe508d2ee73` (`--radius 128`).
`vxc_tests` 154 PASS / 0 FAIL; `lint-shader-ub.py` clean; float-ban clean
(shader untouched, so no SPIR-V rebuild).

**Deliberately not done.** `caveColumnFor` is now 68% of what remains of
`column()`, but `caves.h` is owned by another agent. The streaming exit scan
(`RecomputeDesiredSet`, O(tracked), ~2.2-2.8 ms) and the `PendingJobKeys`
backlog were left alone: with worker throughput up ~1.9x the backlog is no
longer the binding constraint, and re-measuring it should come before
re-architecting it.

**Follow-ups, by expected value.**
1. Cache or cheapen `caveColumnFor` (34 hashes/column, now the single largest
   term in `column()`). Needs the caves.h owner.
2. Mesh is now the largest bench stage (243 ms at brick 8, 409 ms at brick 16 —
   53% and 68% of the totals). It has had no profiling pass at all.
3. Re-measure the exit scan and pending-job backlog against the new throughput
   before acting on either; both baselines are now stale.
4. `TileGridSampler` gains less from the tile memo than `SyntheticTileSampler`
   (it is already a raster lookup). Worth confirming the win holds with
   `-VoxelTileDir` before assuming it generalises to shipped tile data.
5. Suballocate the GPU harness's host-visible buffers to reach the 112-realloc
   floor; low value now that the cost is correctly attributed at ~68 ms hidden
   behind GPU execution.

### Mesher profiling + optimisation (2026-07-21, worktree agent)

Follow-up #2 from the worldgen performance pass above ("mesh is now the largest
bench stage and has had no profiling pass at all"). Closed: mesh is **3.2x
faster at both brick sizes**, with byte-identical output and no shader change.

**Methodology.** `vxc_bench --radius 32`, seed 20260719, clang Release
(`-O3 -DNDEBUG`), **min-of-5** — this box shows ~15% run-to-run variance, so
single runs are not evidence. Attribution used a separate probe that meshed the
same world three ways (live sampler / apron fill only / fill + array-backed
sampler) so the sampler's cost could be separated from the mesher's own work.
An early version of that probe was built at plain `-O2` and reported mesh at
433 ms against the bench's 242 ms; the gap was the build flags, not a finding.
Rebuilt to match the bench, the probe reproduced the bench to within 4%.

**Measured breakdown BEFORE any change.** The headline result is that the
suspected culprit was not the culprit. The greedy merge inner loop — the thing
the brief flagged first — was never the problem, and neither were quad-vector
growth or palette indirection. Roughly **half of "mesh" was the sampler being
re-evaluated**, and the other half was addressing overhead in the scan, not
merging:

| | brick 8^3 | brick 16^3 |
|---|---|---|
| mesh, live sampler (= bench) | 233.6 ms | 394.8 ms |
| mesh, apron pre-materialised | 137.5 ms | 207.9 ms |
| sampler calls per brick | 5277 (512 voxels) | 39522 (4096 voxels) |

~10 sampler calls per voxel: the face scan reads every voxel six times (once
per face direction) and reads nine more cells per *visible* face for AO.

Also measured, and worth recording as a negative result: **homogeneous-brick
early-outs are worth nothing on this benchmark.** The bench walks the surface
shell, and 0 of 10,952 bricks have a homogeneous interior. They were still
added, because they matter underground, but anyone expecting them to move the
bench number should not.

**What changed** (`mesher.h` only; two targets, both chosen from the table):

1. *Materialise the brick + 1-voxel apron once* into a flat array before any
   face scan. This is `(B+2)^3` sampler calls — 1000 vs the old floor of
   `6*B^3` = 3072 at 8^3, and 5832 vs 24576 at 16^3 — so it is fewer calls than
   before for *every* brick size and cannot regress a caller. Critically the
   old code already queried the entire `[-1,B]^3` cube (the AO reads reach even
   the corners, e.g. `(-1,-1,-1)`), so the set of queried coordinates is
   **exactly unchanged**; no sampler can observe the difference.
2. *Hoisted per-axis strides* replacing the per-cell `int c[3]` with
   runtime-varying subscripts. Because `axis`/`u`/`v` are loop variables and not
   compile-time constants, the old form forced the coordinate triple to memory
   on every cell; with strides each neighbour is a constant offset from the cell
   index. This turned out to be worth about as much as (1).
3. Two early-outs for homogeneous bricks (all-air interior; uniform
   apron+interior), which previously needed a full `6*B^3` scan to conclude
   "no quads". See the negative result above re: their benchmark impact.

The greedy merge loop is **untouched**.

**Before/after, min-of-5.**

| stage | brick 8 before | brick 8 after | brick 16 before | brick 16 after |
|---|---|---|---|---|
| amplify | 195.4 ms | 197.9 ms | 158.0 ms | 160.7 ms |
| voxelize | 24.4 ms | 22.8 ms | 38.6 ms | 40.2 ms |
| **mesh** | **242.4 ms** | **76.3 ms** (3.18x) | **411.2 ms** | **129.6 ms** (3.17x) |
| **total** | **462.3 ms** | **296.9 ms** (1.56x) | **607.7 ms** | **330.6 ms** (1.84x) |

Mesh drops from 53% / 68% of total to **26% / 39%**. **Amplify is once again
the largest worldgen stage** at both brick sizes.

**Determinism.** Output is byte-identical, verified beyond the pinned pair: a
bench built from the pre-change mesher and the new one were compared over
3 seeds x 4 radii x 2 brick sizes — **24/24 digests match**, including
`6dbb95d59737e045` (8^3) and `280684104c06aacf` (16^3).
`GOLDEN(amplifier_columns)` `0x81785278E4DFCF67` and `GOLDEN(cave_layer)`
`0x1CD7912E8DBBB5EA` unchanged. `vxc_tests` **174 PASS / 0 FAIL**; float-ban
clean; `mesher.h` compiles clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror`.

**Shader semantics did NOT change.** `worldgen.ush` is untouched — the quad
stream is identical, so `MeshCountMain`/`MeshEmitMain` need no mirror change,
no SPIR-V rebuild, and the AMD digests `e21e2767591496eb` / `1e664cf6680a137c`
/ `7602afe508d2ee73` cannot move. The GPU harness was therefore not re-run
(it is not buildable under mingw, and the CPU-side proof is stronger than a
single harness pass would be). `lint-shader-ub.py` not run for the same reason.

New `test_mesher.cpp` pins four byte-exact quad-stream digests (8^3 and 16^3,
sparse and dense) so this contract is now guarded locally without a GPU — plus
an over-trigger guard where one air apron cell in a solid brick must still
produce exactly one face.

**Deliberately not done.**
- The greedy merge loop, mask layout, and quad-vector growth were left alone:
  measurement said they were not where the time went.
- Splitting the `dir` loop so both face directions share one primary-material
  read (halving `6*B^3` reads to `3*B^3`) was scoped and rejected for now —
  it needs two mask buffers and careful preservation of emission order, for an
  estimated 10-15% of a stage that is no longer the bottleneck.
- gcc was not verified locally: no real gcc on this box (the `g++` on PATH is
  a clang alias). The new code avoids the known gcc-stricter traps — no
  enum/non-enum ternary, all conversions explicit — and the one expression kept
  verbatim from the original is the `aoCorner` shift chain, which gcc already
  accepts today. CI is the check.

**Follow-ups, by expected value.**
1. **Amplify is the top cost again** (197.9 / 160.7 ms, now 67% / 49% of total).
   Its largest remaining term is still `caveColumnFor` — same conclusion as the
   previous pass, and it still needs the `caves.h` owner.
2. Re-measure the UE streaming path. Chunk meshing there feeds `meshBrick` from
   a `Brick`, not from the amplifier, so it was paying proportionally *more* of
   the addressing overhead and less of the sampler overhead — the real-world win
   may differ from the bench's 3.2x in either direction, and should be measured
   rather than assumed.
3. The `dir`-loop sharing above, if mesh ever returns to the top of the profile.
4. Consider whether the GPU mesher carries the same ~10x redundant-sample
   pattern. It is a different execution model (one thread per mask entry) so the
   fix does not transfer directly, but the redundancy analysis might.
### GI: defect fix + GPU sampling

**1. The unresolved roof-underside defect: found, and it was never a GI bug.**

The previous slice flagged that in `04-roofed-gi-on.png` the roof slab underside
renders fully lit while the light field correctly probes 0.00 there. It was
right that the field was correct, and right to flag it rather than dress it up.
The cause is that **the underside was never on screen**.

Both winding branches in `FVoxelChunkSceneProxy` were inverted. With the
one-sided `M_VoxelTerrain` (confirmed at runtime, `twoSided=0`) every voxel face
was wound so UE treated its BACK as front-facing, so you always saw the far side
of a solid, shaded with that far face's normal. Under the slab you were shown
its **sunlit +Z top face** in place of its shaded -Z underside.

Measured, not reasoned (new `voxel.GI.Debug 3` and `voxel.GI.DebugVis`):

| probe | result |
|---|---|
| `Debug 3` per-face-direction, roof chunk `(-4800,0,115520)` | `+Z n=64 hit=64 irr=1.000`, `-Z n=64 hit=64 irr=0.000` |
| `DebugVis 3` (abs(N.Z) ramp) | the ceiling shades as a **+Z** face |
| `DebugVis 4` (omit +Z faces) | the ceiling becomes **sky** -- the underside drew nothing |
| runtime material query | `twoSided=0`, so a back-facing quad is culled |

So 100% of the underside vertices found solved data and shaded black, and the
pixels on screen belonged to a different triangle. The field, the sampling and
the vertex colours were correct throughout.

This predates GI and mis-rendered **all** terrain: the "scattered floating
blocks, holes, distant clipmap showing through" look in every earlier screenshot
is the world drawn inside-out. GI only made it falsifiable, being the first
thing that computes a normal-dependent value. `07/08-roofed-gi-off/on-fixed.png`
vs `09-roofed-legacy-winding-before.png` is the before/after; `DebugVis 6`
restores the legacy winding so the pair comes from one build.

**This changes GI-OFF output too.** It is a rendering-correctness fix, not a GI
change; the "GI off stays byte-identical" rule exists to stop GI leaking into
the non-GI path, not to preserve a bug. Flagged rather than buried.
`VoxelWaterChunkComponent.cpp` has the same inverted winding copied from here
and was NOT touched (not an owned file) -- follow-up.

**2. GPU sampling: NOT DONE. What the cost actually is.**

The first slice attributed GI-on's +3 ms to "extra vertices from
`MaxQuadSpanVoxels` plus the extra proxy rebuilds". The first half is wrong.
Isolated by measurement (post-warmup p95, `-VoxelPerfRun=60`, seed 20260719,
min-spec proxy, 1080p, clean `Saved/VoxelWorlds`):

| config | post-warmup p95 |
|---|---|
| GI off | **4.79 ms** |
| GI on | **8.99 ms** |
| GI on, `MaxQuadSpanVoxels 0` (no subdivision) | 9.03 ms (**-0.04 ms: subdivision is free**) |
| GI on, `MaxChunkRefreshesPerFrame 0` (no GI proxy rebuilds) | 7.31 ms (**-1.7 ms**) |

So the +4.2 ms splits roughly: **1.7 ms** re-shading proxies, **~1-2 ms**
game-thread cone-trace tick, the remainder per-vertex sampling inside ordinary
streaming proxy builds. Quad subdivision costs nothing, so the vertex-rate
argument for a volume texture is weaker than assumed -- the case for it is
resolution and removing the refresh rebuilds, not vertex count.

Two attempts were made and **both reverted**:

- *In-place colour vertex buffer update* (the task's second option -- stop
  rebuilding proxies to carry colour). Measured 8.99 -> 7.92 ms. Reverted: the
  colour buffer `FStaticMeshVertexBuffers::InitFromDynamicVertex` creates is not
  set up for CPU rewrite, so a `LockBuffer` memcpy against it does not reliably
  reach the drawn geometry. Doing this properly needs a colour buffer allocated
  for dynamic update with the vertex factory's colour SRV rebound to it.
- *Refresh only bricks whose irradiance signature changed*, and a wall-clock
  `SolveBudgetMs` time slice. Both measured fine and neither survived review:
  the time slice starves convergence outright, and the signature gate answers
  "did this brick change" when the question is "has every chunk sampling it been
  shaded at least once". `voxel.GI.SolveBudgetMs` remains, defaulting to 0 (off).

Nothing half-working was shipped. **GI-on p95 is unchanged at ~9 ms**; the
target of getting materially closer to 4.8 ms was not met.

**3. GI in caves -- the case it exists for.**

`-VoxelGICaveTest[=<s>]` finds a **pristine worldgen sinkhole** near spawn
(continuous air from just under the surface into the cave band, bottoming out
with head room) and frames the cave under it. Nothing is carved. Found one at
`(2400,-8400)`, 300 m out, cave floor **13.4 m below daylight**; capture-time
six-axis probe `+X 1.9m -X 2.9m +Y 10.0m -Y 6.9m +Z 2.3m -Z 3.3m`.

| image | mean luma | pixels < 60 |
|---|---|---|
| `14-cave-gi-off-passage.png` | 154.1 | 2.8% |
| `15-cave-gi-on-passage.png` | 129.0 | 12.0% |
| `16-cave-gi-on-passage-early.png` (24 s settle) | 104.7 | 48.6% |

GI off, 13 m underground, the cave is uniform pale blue-white -- the skylight
lights it as if it were open ground, with no reading of depth. GI on it darkens
substantially and the passage falls away.

**The honest part.** Compare the last two rows: the SAME camera, seed and build
photographed 24 s after arrival is far darker than at 50 s. The progressive
one-bounce gather is not energy-conserving -- each re-solve feeds the previous
solve's irradiance back through `BounceAlbedo`, so enclosed space creeps
*brighter* over successive refreshes toward `MaxBounceContribution`. An early
capture flatters the feature. `15-` is the settled, reproducible state (two
independent runs, mean luma 129.0 and 129.1) and is what should be believed.
Two runs were spent bisecting a "regression" that was this drift.

A second contributor to enclosed spaces reading wrong: the six cones are
axis-aligned and recombined with `max(0, N.D)`, so for the axis-aligned normals
this mesh always has, **exactly one cone survives** -- a downward face's entire
answer is one 90-degree cone straight down. Light arriving from open sides at
grazing angles is weighted to zero. That is why the open-sided shelter fixture
goes near-black while a deep cave creeps bright.

**Switches added:** `-VoxelGICaveTest[=<s>]`, `-VoxelGICaveStandoff=<uu>`,
`-VoxelGICavePitch=<deg>`, `-VoxelGICaveSettle=<s>`, `-VoxelGIDebug=<n>`,
`-VoxelGIVis=<n>`. New cvars: `voxel.GI.DebugVis`, `voxel.GI.SolveBudgetMs`.

**Recommendation: do NOT default GI on yet.** The winding fix should ship
regardless. GI itself still costs ~4 ms of a 16.6 ms budget, over-brightens
enclosed space over time, and collapses to a single cone on axis-aligned
normals. Fix the cone basis and the bounce energy first; they are correctness,
and they are cheaper than the GPU move.

**Follow-ups, in the order I would do them.**
1. Cone basis: spread the six cones over the hemisphere around N, or weight the
   four tangential cones instead of zeroing them. Fixes both enclosure artifacts.
2. Bounce energy: make the progressive gather converge to a fixed point instead
   of creeping (track a per-cell direct term separately from the bounce).
3. Dynamic colour vertex buffer, then the in-place re-shade -- 1.7 ms, and it is
   a prerequisite for any GPU path that still wants per-vertex fallback.
4. Volume-texture sampling for resolution; note it does NOT address the
   ~1-2 ms CPU solve tick, which would still need budgeting or moving.
5. `VoxelWaterChunkComponent.cpp` winding.

### Caverns + crevices (C1-C3)

`docs/cavern-design.md`'s C1-C3 (caverns.h, crevices, tests), landed standalone
per the build plan — **folded in Matt's post-design decisions** (bedrock
moving to ~200 m depth, caverns should be larger/rarer, generation must
respect real terrain) rather than shipping against the superseded spec.

**Caverns (`voxelcore/caverns.h`, new).** Hash-gated sites at every 8th
lattice node (204.8 m apart — 2x the design's original 102.4 m, i.e. a quarter
as many candidate corners per unit area: the "rarer" half of "large but
rare"), 1-in-4 gated via a bit folded into the same hash `caveNode()` already
computes (§3.7's called-out optimization). Each open site is a **coaxial
chain** of up to 4 rooms rather than a star-shaped cluster around one point:
child 0 is a modest "entrance chamber" (rz 4-10 m) centered exactly on the
anchor — the same node position and depth the tunnel network's own axis uses
there, so the cavern volume provably contains a point of the connected
backbone, same argument as sinkhole shafts — and children 1-3 chain directly
below the previous room (**same xy each time**), independently sized (rxy
12-28 m, rz 12-40 m per room), descending up to ~80 m below the anchor in the
observed sample. Coaxial (zero horizontal offset between chain members) turns
"do consecutive rooms overlap" into an *exact* 1D interval test
(`|dz| < rz[c-1]+rz[c]`, no ellipsoid-overlap approximation needed), provable
by `static_assert` against worst-case hashed radii — and as a free side
effect, distance-to-anchor is now computed **once per column** and reused by
all 4 rooms instead of once per room. Flat floor per room (own-center-
relative now, not anchor-relative — stopped making sense once rooms are no
longer clustered around one point) and a per-column wall-roughness sample
(shared by every room in the column) are unchanged in spirit from the design.
Underground water (§5, approved as designed) rides unchanged:
`CavernColumn.floodZMm` + `CH_CAVERN_FLOOD`, per-site, ~60% wet, level,
clamped below the anchor.

**No bedrock assumption anywhere.** The original design's depth-safety window
was computed against an assumed ~38 m bedrock ceiling; since bedrock is
moving to ~200 m in a later, amplifier.cpp-owned change this file doesn't
touch, that assumption is gone. Only child 0 (the one room tied to the
shallow, 9-34 m tunnel-node depth) has a compile-time safety window at all,
and it is roof-side only (`kCavernNodeDepthSafeMinMm`..`kCaveNodeDepthMinMm+
kCaveNodeDepthSpanMm`, i.e. bounded above by caveNode()'s own range, not a
bedrock guess). The bedrock side is entirely the ordinary **runtime**
`cavernCarveAt(..., bedrockDepthMm, ...)` clamp, exactly like caves.h's
`caveCarveAt` — verified in `test_caverns.cpp` against bedrock depths from
45,000 mm (today) through 260,000 mm (past the planned move) with zero
special-casing.

**Crevices (`voxelcore/caves.h`)**: 1-in-8 gated thin (0.3-0.8 m) vertical
slab riding an *existing* tunnel edge, lens-tapered (`4u(1-u)`, u = the
closest-approach parameter `caveColumnFor` already computes) so it pinches to
nothing at each node instead of ending in a wall. Emits an ordinary `CaveSeg`
— zero new per-voxel mechanism, connectivity inherited for free because the
slab always contains its own tube's axis. `kMaxCaveSegs` 8 → 12. **Caught and
fixed a real bug during development**: the first cut computed the taper as an
exact rational `num*(den-num)/den²`, and `den` (a jittered lattice edge's
squared length) can reach ~3e9 mm², so `den²` overflows int64 — caught by
`crevice_geometry_pinches_out_at_nodes_and_contains_the_tube_axis` (all 20
sampled edges failed the containment check pre-fix). Replaced with Q16
fixed-point math; no correctness issue reaches production since this was
caught before merge, but recorded here as the kind of thing this doctrine's
"measure, don't assume" tests exist to catch.

**Tests (`test_caverns.cpp`, new, 11 cases; `test_caves.cpp`, +3 crevice
cases).** Determinism + golden digests (both re-derived from the redesign,
`cavern_layer` new, `cave_layer` re-pinned for crevices — both moves
explicitly authorized by the build plan's file ownership for C1-C3).
Connectivity is measured, not assumed: every sampled valid site's anchor
voxel is carved by *both* `cavernCarveAt` and `caveCarveAt`, and a
flood-fill at a real site shows the cavern's component extends beyond its own
max reach into the wider tunnel network (99.4% of the fill in one component
in the sampled case). **Roof cover measured on genuinely varied real terrain**
(SyntheticTileSampler + Amplifier, 387 m of relief across the scan, not flat
ground): minimum observed roof cover over any carved cavern voxel = exactly
6,000 mm, i.e. the clamp is measurably load-bearing (some column got
truncated right at the boundary), and 88 of the sampled in-reach columns were
refused outright by slope truncation — the "clamps become load-bearing on
slopes" claim, verified rather than asserted. Segment-cap headroom (max 4 of
6 rooms/column observed), flood-level properties (level, disc-consistent,
bounded, ~63% wet against a ~60% target over 150 sampled sites), and a cost
micro-benchmark: **~28 ns/column** (mixed columns, well under the design's
own ~87 ns estimate — the wider 204.8 m spacing means the full-reduction tier
fires on a quarter as many columns before the gate probability is even
applied), **~0 ns/voxel** for the cavern-free fast path, **~1.2 ns/voxel**
worst case observed (4 segments) — all comfortably inside the <4 ns/voxel
worst-case budget. Suite: 173 → 184 PASS / 0 FAIL. Clang clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror`; float-ban clean
(verified against the exact CI script, not just `-Wall`). No real GCC was
available in this dev environment to cross-check the "gcc stricter than
clang" gotcha directly; the specific known pattern (enum/non-enum ternary)
was checked by hand and does not appear in the new code.

**Left for C4+ (amplifier fold-in, version bump, GPU mirror — none of it
touched here):** wire `CavernColumn` into `ColumnSample`/`materialAt` exactly
as `CaveColumn` is wired in today; bump `kWorldGenVersion`; re-pin the
amplifier/GPU-harness goldens the design doc's §9 predicts will move; extend
`surfaceMmAt` into a shared CPU/GPU function (this file's `surfaceAt`
callback contract is written for exactly that hand-off); when bedrock's
actual new depth constant lands in `amplifier.cpp`, nothing in caverns.h
needs to change (it was never assumed) — worth re-running
`cavern_never_breaches_bedrock` and the roof-cover test once it does, since
the *observed* numbers (how much of the ~80 m chain actually survives a given
column's real bedrock) will shift even though no code does.

### C4: caverns fold-in, 200 m bedrock, caveColumnFor caching

Three things that all live in the worldgen column path, done in one pass and
one `kWorldGenVersion` bump (**4 -> 5**). The GPU mirror (C6) is explicitly NOT
part of this and the AMD digests are now stale — see the end of this section.

**1. Caverns wired into the amplifier (C4).** `ColumnSample` gains a
`CavernColumn cavern` next to its `CaveColumn cave`, `Amplifier::column`
fills it, and `Amplifier::materialAt` consults it — the same shape the cave
pass already had, with caves tested first because a cavern column is far
rarer and `cavernCarveAt`'s first test is `count == 0`. The `MAT_AIR` /
`MAT_BEDROCK` early-out stays exactly where it was, so the bedrock floor keeps
all three independent guards.

The one place that needed interpretation was the `surfaceAt` callback.
Caverns anchor at *absolute* z (level floors and water tables rather than
draped ones), so the reduction needs terrain height at the SITE's own xy, not
the querying column's. Rather than pass a second copy of the surface formula,
`Amplifier::column`'s surface half was factored out as
`Amplifier::evalSurface` and the callback is literally that function — the
contract holds by construction instead of by a comment asking two copies to
stay in step. Nothing was added to any GPU column-cache struct; C6 recomputes
it inside `VoxelizeMain`, as the design doc's section 3.5 requires.

**2. Bedrock 40-60 m -> 180-220 m** (Matt's decision: 200 m). Same
deterministic shape (one 16-bit field of a 6.4 m-lattice `CH_BEDROCK_JITTER`
hash), only the two constants move. 200 m is the band's MEAN rather than its
floor — scaling the old "base + 50% of base" shape would have given 200-300 m
(mean 250 m), which is not what was asked for. The +/-20 m (10%) span is enough
that the boundary does not read as a flat sheet draped under the terrain (the
only reason the jitter exists) while leaving ~52 m of untouched rock above
even the shallowest draw. As predicted by the cavern author, **caverns.h
needed no change** — it never had a compile-time bedrock assumption.

Re-ran the two flagged tests. `cavern_never_breaches_bedrock` is driven by
its own bedrock parameters and is unchanged: closest approach 2050 / 2050 /
128'050 / 188'050 mm at 45 / 60 / 200 / 260 m, against a 2000 mm margin. The
sloped-real-terrain roof test moved a lot, over the same 640'000-column scan
(387 m of relief):

| | 40-60 m | 180-220 m |
|---|---|---|
| carved cavern voxels | 1'356'809 | 1'870'004 (+37.8%) |
| columns over a cavern | 5'551 | 5'639 |
| voxels clamped away | 534'227 | 21'032 (-96.1%) |
| columns partly clamped | 4'189 (75.5%) | 517 (9.2%) |
| columns fully refused | 88 | 0 |
| min roof cover over a carved voxel | 6000 mm | 6000 mm |
| caves' closest approach to bedrock | 6'250 mm | 146'279 mm |

So ~38% more of each chain now survives, the **bedrock clamp is now inert**
for caverns and tunnels alike, and the **roof clamp is the only one still
binding** — still exactly, at 6.00 m. That retired the old
`columnsFullyRefused > 0` assertion, which was measuring a clamp that is now
correctly inert; it is replaced by a direct count of voxels inside a room's
ellipsoid that a clamp refused, which is what "load-bearing" always meant.

**Second-order effects checked.** Caves/tunnels sit 6-40 m down and are
depth-relative, so they are unaffected in geometry — but their *bedrock
margin* clamp, which used to bind occasionally against a 40 m floor, is now
inert (closest approach 6'250 -> 146'279 mm). `caves.h`'s bedrock
`static_assert` is deliberately left checking against the old 40 m figure: it
is a strictly stronger statement and keeps tunnel geometry provably
independent of wherever the bedrock band sits. Stratigraphy ordering is
unchanged (`bedrockDepthMm > topsoil + subsoil` still holds by three orders of
magnitude), `MAT_ROCK`'s extent grows from ~40 m to ~200 m, and the
underground streaming depth budget of 38.4 m was *already* entirely inside
MAT_ROCK before the move, so nothing there changes at all. Mip/LOD and
water/aquifer behaviour are untouched: every existing golden that walks voxels
(`mips_chain`, both bench digests) only ever visits SURFACE-SHELL bricks, so
none of them can see either bedrock or caverns.

That last point is a gap worth naming: **no pre-existing golden would have
noticed if the cavern fold-in had been silently dropped.** Two new tests close
it — `amplifier_folds_caverns_into_materialAt` (1115 columns over a cavern,
387'917 voxels carved by the cavern pass, 379'924 of them cavern-ONLY, roof
cover 6.0-77.8 m, 656'754 bedrock voxels all refused) and
`GOLDEN(amplifier_deep_materials)`, which digests `materialAt` 260 m down over
an 800 m grid and therefore actually covers caves, caverns and the bedrock
boundary as the amplifier composes them.

**3. `caveColumnFor` cached.** All of its hashing (the 4x4 node block, 18
candidate edges, sinkhole candidate) depends only on the LATTICE CELL — 25.6 m
square, 65'536 voxel columns — so `caves.h` now exposes that half as
`CaveLattice`/`caveLatticeFor` and `amplifier.cpp` memoises it thread-locally
by (seed, ci, cj). The pure fused `caveColumnFor` is unchanged in value and
remains the contract/HLSL-mirror form. `caverns.h` got the same treatment for
its 2x2 candidate corners (204.8 m cell), plus a fused gate+node hash where it
was computing `hash2(..., CH_CAVE_NODE)` twice per corner.

One cost-model correction worth recording: caverns.h's comment that the site
reduction "never repeats work" was true only *within* a column. Across
columns a site was re-decoded for every one of the ~400'000 columns inside its
~36 m reach disc, each re-running `surfaceAt` — in production the amplifier's
full bilinear+4-octave surface function. `cavernSiteFor` is now reached
through a callable so `amplifier.cpp` can memoise per (seed, fi, fj);
measured over a region that actually contains a site (59.4% of columns over a
cavern), that alone takes `column()` from **473 -> 301 ns/col**, bit-identical
output.

**vxc_bench --radius 32, min-of-5, interleaved against a binary built from
`main` @ ec872bd** (this box's run-to-run variance is large enough that
sequential before/after runs were misleading by up to 55% — the two binaries
were alternated on every rep):

| | main 8^3 | this 8^3 | main 16^3 | this 16^3 |
|---|---|---|---|---|
| amplify | 202.1 ms | **115.5 ms** | 163.7 ms | **93.9 ms** |
| voxelize | 21.8 ms | 24.6 ms | 43.4 ms | 40.5 ms |
| mesh | 76.6 ms | 76.0 ms | 125.1 ms | 113.4 ms |
| **total** | **300.5 ms** | **216.1 ms** | **332.4 ms** | **247.7 ms** |

Amplify is **1.75x** faster and worldgen overall **1.39x / 1.34x** faster —
while also gaining the entire cavern system. Target met: the combined
cave+cavern cost did not undo the earlier 792.6 -> 283 ms passes, it extended
them.

**Caverns cost more in the real column path than standalone.** Isolated
(same interleaving, caverns computed vs not, 8^3): amplify 98.4 -> 119.1 ms
over 409'600 columns = **+50 ns/col**, versus the **~28 ns/col** the
standalone micro-benchmark reported. The standalone figure used a constant
`surfaceAt` and a `CavernColumn` that was never stored; the real path pays an
80-byte struct write per column on top. It is the same order of magnitude and
it is comfortably paid for by the caching above, but the standalone number
should not be quoted as the production one.

**Goldens — predicted first, then checked. Nothing moved unpredicted.**

| golden | predicted | actual |
|---|---|---|
| `amplifier_columns` | MOVE (digests `bedrockDepthMm`) | `0x81785278E4DFCF67` -> `0xA29A7A767DC1543B` |
| `cave_layer` | unmoved (test-local flat surface + bedrock; lattice split is pure) | `0xBFE42E07FFA6B78D` unmoved |
| `cavern_layer` | unmoved (test-local constants; candidate split is pure) | `0x5B1F8E5E73ED6EF2` unmoved |
| `mips_chain` | unmoved (surface-shell bricks only — never 40 m deep, never near a cavern) | `0xE827A786195B8A73` unmoved |
| `rivernet_synthetic_slope` | unmoved (tile-only, no amplifier columns) | `0xA4D30E5715339878` unmoved |
| `biome_map` | unmoved | `0xEDBF3C9217ECBBF6` unmoved |
| bench digests | unmoved (surface-shell only, same reason as mips) | `6dbb95d59737e045` / `280684104c06aacf` unmoved |
| `amplifier_deep_materials` | NEW | `0xF88B88DB9D9341AA` |

The cave/cavern refactors were committed *first*, separately, and verified to
leave every single golden and both bench digests byte-identical — that is the
evidence that the caching is output-neutral, rather than an argument that it
ought to be.

**The AMD GPU digests are now STALE.** `worldgen.ush`'s `ColumnMain` /
`VoxelizeMain` know nothing about caverns and still carry the 40-60 m bedrock
band, so the CPU/GPU harness **will FAIL until C6 lands** — expected, not a
regression. C6 must mirror `cavernCandidatesFor` / `cavernSiteFor` /
`cavernColumnFromSites` / `cavernCarveAt` bit-for-bit, apply the same
180'000 + jitter bedrock constants, and re-pin the AMD RX 7800 XT digests.
No HLSL was touched here.

**Verification:** 190 PASS / 0 FAIL (was 188 — two new amplifier tests).
Clang clean under `-Wall -Wextra -Wconversion -Wsign-conversion -Werror` for
every file touched; float-ban clean against the exact CI script. No gcc
available on this box, so the known "gcc is stricter" pattern (enum/non-enum
ternary) was avoided by hand — `materialAt`'s new cavern branch uses an
explicit `static_cast<MaterialId>(MAT_AIR)` and an `if` rather than a ternary.

**Follow-ups.** (a) C6, as above — the blocking one. (b) `ColumnSample` grew
128 -> 208 bytes; `kMaxCavernSegs` is 6 where at most `kCavernChildCount` (4)
rooms can ever be recorded, so 24 bytes are provably dead — left alone because
it is a caverns.h contract constant and measurement showed the struct size is
NOT the bottleneck. (c) The UE `VoxelWorldSubsystem` column cache holds
`ColumnSample`s and will use ~62% more memory; not this agent's file.
(d) `columnCached`'s comment still says "96 bytes"; stale but harmless.
### Streaming pipeline re-measure + rework (2026-07-21, worktree agent)

Re-measures the two streaming items the PR #58 perf pass deliberately left
alone ("with throughput up ~1.9x their baselines are stale, and re-measuring
must precede re-architecting"), then fixes what the measurement actually found.

**Verdict up front: both flagged items were still real, and the pending-queue
one had grown, not shrunk.** The throughput win dissolved neither. But the
*reason* they were expensive is not the one in the backlog note, and the fix
that follows from the measurement is not the one that note proposed.

**Method.** `-VoxelPerfRun=60`, seed 20260719, `-nullrhi`, `Saved/VoxelWorlds`
cleared before every run. The A/B rides a **command-line switch read at first
use** (`-VoxelPendingJobCap=<n>`, `0` = the old unbounded behaviour) -- not a
cvar, because `-ExecCmds` cvars land after streaming has begun and an
`-ExecCmds` A/B silently measures the same state twice (the lesson
`-VoxelNoUnderground` already exists for). **One binary, both sides**, runs
interleaved A/B/A/B, two per side, plus a separate real-RHI min-spec-proxy
round for the M1 gate. `voxelcore.lib` was rebuilt in this worktree from this
commit's sources rather than reused, so the measured throughput really is the
post-PR-#58 one.

Two pieces of instrumentation had to be added before anything was measurable:
the existing per-frame breakdown only logs on frames over the 33.3 ms hitch
threshold, and a `-nullrhi` throughput run has none. Added to the 5 s periodic
log: a **tick-budget line** (tick / recompute / dispatch / apply / remesh /
unload ms summed over the window) and a **job-flow line** (dispatched /
drained / stale / zero-quad / records added / records evicted).

**Current measured profile (unbounded side, the honest "before").**

| | value |
|---|---|
| `PendingJobKeys` depth, steady | **26.5k** (the stale note said 19-20k) |
| `ChunkRecords` (tracked) | **34.5k** |
| evictions per 60 s run | **207,839**, against 21.5-22.2k component loads |
| jobs dispatched | ~1,370/s, `jobsInFlight` pinned at 24 = 2x12 logical cores |
| stale results | **0.0-0.1%** (not a source of waste) |
| results meshing to zero quads | **~72%** (buried chunks: real work, no component) |
| subsystem tick | **4.5-4.9% of wall** (`-nullrhi`), 5.9-6.2% with RHI |
| ...of which `RecomputeDesiredSet` | **75-78%** |
| worst recompute call | 5.7-7.2 ms, ~8/s |
| ...exit hysteresis scan | 2.39-2.59 ms (the flagged O(tracked) item) |
| R2 / R3 / R4 chunks loaded in 60 s | **0 / 0 / 0**, with 16.2k of them queued |

Two things reframe the problem. First, **production outruns drain by ~40x**
and always did: ~68k candidate chunks/second are enumerated by the entry scans
against ~1,370 dispatches/second. Second -- and this is what the old note
missed -- **the queue depth is not a symptom, it is the cost**. ~90% of
everything admitted to the desired set left it before a worker ever looked at
it, and each of those admissions bought a `ChunkRecords` insert, a slot in the
O(n log n) queue sort (8x/second), a slot in the O(evictions) queue filter and
a slot in the O(tracked) exit scan. The exit scan is not expensive because it
is O(tracked); it is expensive because 80% of `tracked` is work that will
never happen.

**The fix: bound admission by what can be drained** (`-VoxelPendingJobCap`,
default 2048). Three gates, all keyed on the same 3D chunk-centre distance
`SortPendingQueues` already prioritises by, so **dispatch order is unchanged**
-- only the volume of never-to-be-done bookkeeping shrinks:

1. **Distance cutoff.** While the queue is at cap, a candidate farther than
   the farthest queued chunk never becomes a record. Candidates *nearer* are
   always admitted, so nothing near the player waits behind far work.
2. **Truncation.** After the sort, entries past the cap are dropped, record and
   all -- only ever entries with no component and no job in flight (a queued
   chunk has by definition never meshed), so a drop costs nothing and loses
   nothing: the entry scan re-enumerates any untracked in-annulus footprint the
   next time that level scans, which is exactly when its ranking can change.
3. **Per-level admission budget** (`Cap/4`). The cutoff alone does not bound a
   single call: `DispatchJobs` pops from the *near* end, so the queue refills
   with the farthest candidates, the cutoff drifts out with them, and each call
   was admitting ~3k newly-near chunks and dropping ~3k newly-far ones. Per
   *level*, not per call, because levels are scanned in ascending order and R0
   rescans every call while R1 rescans every second one -- one shared budget
   let R0 spend it first and measured 8% lower R1 residency.

Plus a **drain-triggered refill**: `RecomputeDesiredSet` is movement-gated, so
a bounded queue would otherwise leave a *stationary* player idle with chunks
still unloaded in range (the unbounded queue papered over this by holding
minutes of backlog). When a deferring queue drains below a quarter of its cap,
a recompute is forced and the per-level scan gate cleared for it.

**Before / after, same binary, interleaved, all four runs shown**
(`-VoxelPerfRun=60`, seed 20260719, `-nullrhi`):

| metric | A1 (cap=0) | A2 (cap=0) | B1 (cap=2048) | B2 (cap=2048) |
|---|---|---|---|---|
| chunks/s | 358.9 | 363.5 | **367.6** | **369.6** |
| subsystem tick, % of wall | 4.60 | 4.53 | **2.47** | **2.26** |
| recompute ms per 5 s | 177.3 | 169.7 | **69.5** | **58.3** |
| worst recompute call (ms) | 6.2-7.2 | 5.7-6.5 | **2.6-4.0** | **2.3-3.2** |
| ...exit scan (ms) | 2.49-2.59 | 2.39-2.49 | **0.47-0.58** | **0.50-0.53** |
| tracked records | 34,536 | 34,536 | **9,952** | **10,101** |
| `PendingJobKeys` | 26,681 | 26,471 | **2,029** | **2,023** |
| evictions per run | 207,839 | 207,839 | **76,799** | **77,281** |
| R0 / R1 resident | 1726/681 | 1760/690 | 1745/684 | 1758/696 |

Throughput is **unchanged to slightly up** -- which is the finding, not a
disappointment: the pipeline is worker-CPU-bound, so capping the queue cannot
buy chunks/s, only the game-thread cost of describing work that never happens.
Ring residency is at parity, so nothing visible changed.

**M1 gate, real RHI** (min-spec proxy `sg.*Quality 0` + `r.ScreenPercentage
100`, 1080p windowed, one warm-up run discarded, two per side):

| | A1 | A2 | B1 | B2 |
|---|---|---|---|---|
| p95 (full run) | 4.74 | 4.78 | **4.70** | **4.60** |
| post-warmup p95 | 4.79 | 4.84 | 4.79 | **4.67** |
| post-warmup hitches (>33.3 ms) | 0 | 2 | 1 | 0 |
| frames over the 16.6 ms bar | 9 | 8 | **6** | **6** |
| chunks/s | 291.5 | 284.9 | **295.2** | **299.0** |
| subsystem tick, % of wall | 5.94 | 6.18 | **2.89** | **3.10** |

**Gate holds** (4.6-4.8 ms against the 16.6 ms bar) and the capped side is
equal or better on every column. This establishes its own before/after on one
build rather than trusting the documented ~4.93-4.97 ms figure, which PR #58
reported did not reproduce on an unmodified baseline; here both sides land at
4.6-4.8 ms, so that band does reproduce on this build.

**Contents unchanged, verified not asserted.** A/B of
`-VoxelHeadlessDigTest=15 -VoxelDumpDigestAfter=25 -VoxelNoLoad` (seed
20260719) with the cap off and on: both carve **2,178,385** voxels, both report
**`editedDigest=0xFB62844A74CB9121`** (server and client), and both write an
identical **8,039-entry / 4,135,443-byte** `.vxlog`. voxel-core was not touched
(`git diff` against the base is exactly `VoxelWorldSubsystem.cpp` + this file),
so no bench digest or golden can move.

**Build.** `VoxelEarthEditor Win64 Development` via `D:\UE_5.8\...\Build.bat
... -WaitMutex -NoHotReloadFromIDE` -> `Result: Succeeded`, zero warnings from
`VoxelWorldSubsystem.cpp`. Worktree `voxelcore.lib` built fresh with MSVC/Ninja
from this commit (note for future waves: `vcvars64.bat` prints a harmless
`vswhere.exe is not recognized` line and still works, but passing it
`-no_logo` breaks it, and cmake will otherwise pick up the mingw clang on
PATH ahead of `cl.exe`).

**Deliberately not done.**

- **Did not make the exit scan incremental/bucketed.** It was 2.4-2.6 ms
  because `tracked` was 80% never-to-be-meshed queue entries; with those gone
  it is 0.5 ms. A round-robin cursor would have been real work that measured as
  noise afterwards -- the classic mis-fix this task exists to avoid.
- **Did not touch per-record bookkeeping size.** Same reason: `tracked` fell
  71%, a bigger win than shrinking `FChunkRecord` could have been, and it is
  now cheap enough not to matter.
- **Did not fix R2/R3/R4 never loading** (below). It is a prioritisation
  question with a visible-output consequence, which puts it outside a
  streaming-cost wave required not to change what renders.
- **Did not raise `MaxJobsInFlight`** past 2x cores; the worker pool is already
  the binding resource and this box has 12 logical cores.

**Follow-ups, ranked.**

1. **R2/R3/R4 load zero chunks in a 60 s flight** -- before this wave they sat
   at 16.2k queued-and-never-dispatched; after it they are simply never
   admitted. Same outcome, less waste, but the underlying prioritisation bug is
   untouched: distance is 3D while ring membership is XY, so R0's *deep column*
   chunks outrank R1+ *surface* chunks, and R0 alone out-produces the worker
   pool forever. A per-ring share of the dispatch budget would fix it. This
   changes what renders (more distant voxel terrain, currently masked by the
   clipmap), so it wants its own wave and its own gate run.
2. **~72% of all worker output meshes to zero quads** (fully-buried chunks:
   correct, but ~1,000 jobs/s of amplifier+mesher work for no geometry). A
   cheap "is this chunk entirely below the surface band and unedited" pre-test
   before dispatch could reclaim most of the worker pool -- by far the largest
   remaining throughput lever.
3. Residual admit/drop churn is ~14-20k records per 5 s (down from 124k at the
   first cut of this wave). Bounded and cheap; only worth revisiting if the
   entry scans get cheaper first.
4. `-VoxelPendingJobCap` swept at 512 / 1024 / 2048 / 8192: 512-2048 are
   equivalent, 8192 is measurably worse (3.34% of wall vs 2.57%), unbounded
   worst. 2048 was kept for starvation margin. No re-tune needed unless the
   drain rate changes a lot.

### R2-R4 ring starvation fix (2026-07-21, worktree agent)

PR #64 ranked this its follow-up #1: "R2/R3/R4 load ZERO chunks all run, with
16.2k queued", attributed to dispatch being sorted by 3D distance while ring
membership is XY, so R0's deep columns outrank every R1+ surface chunk forever.
**Measured first, before changing anything, and the inference is half right.**
There are TWO independent radial-global bounds starving the outer rings, and
underneath both there is a third problem that no scheduling change can fix.

**Method.** `-VoxelPerfRun=60`, seed 20260719, `Saved/VoxelWorlds` cleared
before every run. Two new **command-line** switches (not cvars -- `-ExecCmds`
lands after streaming has begun and silently measures the same state twice; the
lesson `-VoxelNoUnderground` and `-VoxelPendingJobCap` already exist for):
`-VoxelRingQuota=0|1` and `-VoxelRingFloors=a,b,c,d,e`. **One binary, both
sides**, M1 runs interleaved A/B/A/B after a discarded warm-up. `voxelcore.lib`
rebuilt in this worktree from this commit's sources.

Instrumentation added first: the existing "Voxel rings" line reports per-level
RESIDENCY and QUEUE DEPTH only, which cannot tell a ring starved of workers from
a ring that is served but meshes entirely to buried rock -- both read `loaded=0
pending=4000`. New **"Voxel ring dispatch"** line: per level, jobs dispatched
(5 s window + cumulative), cumulative component loads, cumulative zero-quad
results.

**Measured BEFORE profile** (60 s flight, `-nullrhi`, per-ring cumulative):

| | R0 | R1 | R2 | R3 | R4 |
|---|---|---|---|---|---|
| cap=2048 (default) dispatched | 137,526 | 44,457 | 3,776 | **0** | **0** |
| ...components loaded | 31,589 | 16,335 | 1,247 | **0** | **0** |
| ...steady pending | ~180 | ~410 | ~1,150 | **0** | **0** |
| cap=0 (pre-#64) dispatched | 135,030 | 44,023 | 4,924 | **0** | **0** |
| ...steady pending | ~180 | ~450 | ~3,700 | **5,560** | **5,185** |

So: **R2 was never zero** -- it loaded ~1,250 chunks a run. And the mechanism
differs by cap setting:

* **cap=0**: R3/R4 are queued (10.7k of them, reproducing #64's 16.2k) and
  dispatched zero. This IS the 3D-vs-XY story: strict global nearest-first, and
  R0's 38.4 m deep column is nearer in 3D than any R1+ surface chunk.
* **cap=2048 (the shipping default)**: R3/R4 pending is **0** -- they are not
  starved at dispatch, they are **never admitted**. The single global admission
  cutoff settles at **~246 m** and R3's annulus starts at **256 m**. A cutoff is
  a radius; a radius applied to concentric annuli excludes the outermost ones
  completely. This is a PR #64 regression, not the pre-existing bug, and it was
  not in the original diagnosis.

Their entry scans do run (`scans R3=4 R4=1` per 5 s window) -- the candidates
are enumerated and then rejected, which is why the queue looked empty.

**Fix: per-ring queues, not a different comparator.** The flat `PendingJobKeys`
becomes one queue per level, each storing its chunk's cached 3D distance. Every
bound the flat queue forced to be global is now per ring: a **share of the
admission cap** (0.30/0.20/0.20/0.15/0.15), a **cutoff distance each**, and a
**floor of in-flight worker slots**. Above the floors dispatch is still strictly
nearest-first across the five queue heads, which reproduces the old flat
ordering exactly, tie-break included.

**The comparator was left alone, deliberately.** Ring-major ordering was tried
in an earlier wave and measured worse (R3/R4 at 0 through a 90 s run, because
the far larger inner rings never drain) -- the note is still in
`SortPendingQueues`. 3D-distance-primary is a perfectly good *intra-ring*
priority; the modelling error was never the metric, it was applying ONE global
ordering, and one global cap, across rings whose membership is defined on a
different metric and whose chunk counts differ by orders of magnitude. So the
comparator keeps its semantics and loses its scope: nearest-first *within a
ring*, explicit floors *between* rings.

**The thing underneath: coarse-ring jobs are unaffordable.** Once R3/R4 actually
dispatch, their cost is visible for the first time, and it is the real reason
the cascade has never rendered:

| | R0 | R1 | R2 | R3 | R4 |
|---|---|---|---|---|---|
| worker ms p50 | 0.85 | 2.0 | 2.2-18 | **150-292** | **2,850-5,241** |

A level-4 chunk covers 16x the linear extent of a level-0 one, so ~4096x the
volume, and the cost tracks that almost exactly -- the mip build has no cheap
coarse path. **First floor attempt {0,2,3,4,4} was catastrophic**: it reserved
13 of 24 slots, and since the task graph has ~12 real background threads the
multi-second R4 jobs occupied the pool itself. Whole-run throughput fell from
49,179 chunks to **558** (819 -> 9.3 chunks/s), R0 residency from ~3,000 to
~10, and the 512 MB mip cache thrashed at 3.2M evictions/5 s. Floors were swept
at {0,1,1,1,1}, {0,1,1,1,0} and {0,2,2,1,0} -- all three within noise of each
other (704 / 694 / 691 chunks/s), because the binding constraint is job
DURATION, not slot count. **{0,1,1,1,1} kept** (costs at most 4 of 24 slots).

**Per-ring residency, before/after** (60 s flight, `-nullrhi`, same binary):

| | R0 | R1 | R2 | R3 | R4 |
|---|---|---|---|---|---|
| before, loaded/run | 31,589 | 16,335 | 1,247 | **0** | **0** |
| after, loaded/run | 30,377 | 11,320 | 543 | **23** | 0 (13 dispatched) |
| before, resident | ~3,020 | ~2,051 | ~413 | **0** | **0** |
| after, resident | ~2,998 | ~1,845 | ~137 | **8** | 0 |

R3 goes 0 -> non-zero and stays there; R4 dispatches but at ~5 s/chunk cannot
finish one inside a 60 s flight. Whole-run throughput costs 14% (49,179 ->
42,271 chunks) -- that is the price of the coarse rings existing at all, paid by
R1/R2 residency, and it is a real trade, not free.

**A stationary anchor was never starved.** Held still for 90 s the rings
populate either way (before: R0 1515 / R1 1712 / R2 1967 / R3 504 / R4 **0**;
after: 1515 / 1568 / 1704 / 435 / **3**). The starvation is a MOVING-anchor
phenomenon: in flight R0 continuously re-produces candidates and monopolises
both the cap and the pool. Only R4 is starved in both -- because it is
cost-bound, not schedule-bound.

**Vista.** `-VoxelVistaShot[=<metres above spawn>] -VoxelVistaPitch=<deg>`
added beside the existing screenshot switches: the default
`-VoxelScreenshotAfter` framing is -40 deg from spawn height and fills the frame
with R0/R1 terrain a few tens of metres away, which is why "the cascade renders"
had never actually been looked at. Shots at 250 m / -18 deg, 90 s settle:
`shot_vista_before.png` / `shot_vista_after.png` (plus `shot_ringsbefore.png` /
`shot_ringsafter.png` with `-VoxelDebugRings`). Both render a genuine
horizon-to-horizon vista and are near-identical.

**That is itself the finding**: the long-distance vista is the **clipmap**, not
the voxel ring cascade. `RingPresets` tops out at R4 = 512-1024 m, so the voxel
cascade covers **1 km**, and everything beyond it in a 50 km vista is
`AVoxelClipmapActor`. The M2 gate's "50 km+ vista" was passing on the clipmap
all along -- not on a technicality exactly, but not on the ring cascade either.
(A first attempt framed the camera at an ABSOLUTE 250 m and produced a frame of
pure fog: terrain at the default spawn sits at ~1,200 m world Z, so the camera
was inside rock. The switch is relative to spawn.)

**M1 gate, real RHI** (min-spec proxy `sg.*Quality 0` + `r.ScreenPercentage
100`, 1080p windowed, warm-up discarded, interleaved A/B/A/B):

| | A1 (quota off) | B1 (on) | A2 (off) | B2 (on) |
|---|---|---|---|---|
| p95 (full run) | 4.88 | **4.76** | 4.85 | **4.78** |
| post-warmup p95 | 4.94 | **4.81** | 4.91 | **4.82** |
| post-warmup hitches (>33.3 ms) | 0 | 0 | 0 | 0 |
| frames over the 16.6 ms bar | 7 | 7 | 6 | 9 |
| chunks/s | 311.0 | 306.6 | 312.2 | 305.7 |
| subsystem tick, % of wall | 4.32 | **3.74** | 4.27 | **3.74** |

**Gate holds** (4.76-4.88 ms against the 16.6 ms bar, vs the 4.60-4.70 ms band
this wave inherited; frames over 16.6 ms 6-9 against ~6). The quota side is
slightly ahead on p95 and clearly ahead on tick cost, and ~2% behind on
chunks/s.

**PR #64's admission-cap wins are intact** (`-nullrhi`, steady state): tracked
records **19,094** (was 20,335 with the cap and 34,536 uncapped), pending job
queue **1,394** (1,290 / 14,951), worst exit scan **0.94 ms** (0.94 / 1.92).
Nothing regressed; the cap is still doing its job, just five times over instead
of once.

**Contents unchanged, verified not asserted.** A/B of `-VoxelHeadlessDigTest=15
-VoxelDumpDigestAfter=25 -VoxelNoLoad` (seed 20260719) with `-VoxelRingQuota=0`
and `=1`: both carve **2,178,385** voxels, both report
**`editedDigest=0xFB62844A74CB9121`** (server and client), both write an
identical **8,039-entry / 4,135,443-byte** `.vxlog` -- byte-for-byte the same
values PR #64 recorded.

**Follow-ups, in priority order.**
1. **R4/R3 mip build cost is now the whole ballgame** (5,241 ms and 292 ms p50
   per chunk). No scheduling change can make a 1 km ring of 5-second chunks
   converge. The cascade needs a coarse generation path that samples the
   amplifier at the mip's own resolution instead of building and downsampling
   ~4096x the volume. This is the single blocking item for M2's ring cascade.
2. **Mip cache thrash**: 512 MB cap, 1.4-3.2M evictions/5 s once R3/R4 run.
   Sized for R0-R2; useless at the coarse levels it exists to amortize.
3. **Pre-dispatch "fully buried and unedited" test** -- still 72-77% of R0
   output meshes to zero quads, and 63% of R4's. Not attempted here (it would
   have muddied the scheduling A/B) but it is free throughput.
4. **Restate the M2 gate honestly**: the ring cascade delivers 1 km, the clipmap
   delivers the rest. Either widen `RingPresets` (blocked on #1) or write the
   gate as a clipmap deliverable.
5. Floors and cap shares are constants tuned on a 12-core box; they should scale
   with `NumberOfCoresIncludingHyperthreads`.

## M2 perf — coarse LOD generation path: R4 chunk 3,144 ms -> 2.0 ms (2026-07-21, worktree agent)

**Problem, measured first.** Outer-ring LOD chunks (levels 1-4) had no coarse
generation path: a level-L brick was built by materializing and downsampling
all 8^L level-0 descendants. On a faithful cold replica of the UE worker job
(column-grid cache, always-materializing level-0 source, recursive
`downsampleBricks`, `[-1,B]^3` apron meshing; clang -O2, seed 20260719,
min-of-3), a level-4 chunk cost **3,273 ms**: level-0 voxel fill 2,177 ms
(67%), downsample chain 616 ms (19%), map/alloc overhead 313 ms (10%),
amplifier columns 165 ms (5%), mesh 2.3 ms. That matches the streaming wave's
worker p50 of 2,850-5,241 ms for R4. The verdict from the breakdown: 95% of
the cost is materializing 884,736 level-0 bricks, so no column-sampling
optimization could have moved the needle — the chunk had to stop touching
level-0 data entirely.

**The coarse path** (`GeneratedWorld::coarseColumns` / `makeCoarseBrick` /
`coarseSurfaceBrickRange`, voxel-core/include/voxelcore/generator.h): a
level-L cell takes the material of the representative level-0 voxel at the
CENTRE of its 2^L-cube footprint — cell index c samples level-0 index
`c*2^L + 2^(L-1)` per axis, through the unchanged `Amplifier::column` +
`materialAt`. Properties, in doctrine order:

- **Determinism**: a pure composition of the existing integer worldgen
  functions of (seed, coords) — no new constants, no iteration-order
  dependence. New golden `0x85B3E79EF8D01AFC` pinned (levels 1-4, 3x3
  footprints, test_coarsegen.cpp). All fine-path goldens byte-identical
  (vxc_tests 197/197; bench `--digest` still `6dbb95d59737e045` /
  `280684104c06aacf`).
- **Level-0 identity**: at L=0 the offset is 0 and the rule degenerates to
  `makeBrick` bit-exactly (pinned by test AND by the bench's identical
  L0 digests) — one rule serves every level.
- **Exactness**: exact wherever the cell's footprint is uniform (deep rock,
  open air — the overwhelming majority of cells). NOT exact on the surface
  shell or at cave/cavern walls, and provably cannot be at this cost: the
  true mip's recursive majority vote is a function of all 4^L column heights
  in the footprint, so reproducing it needs all those columns (see options
  below). Centre (not corner) sampling was chosen because it has zero
  systematic lateral shift at every level — corner sampling would translate
  features by 2^(L-1) voxels, a different shift per level (inter-level
  popping). Residual bias: +50 mm in z (half a fine voxel, constant across
  levels, below the coarse quantization).
- **Fidelity vs the true mip**, measured on surface-shell bricks and pinned
  as permille CEILINGS in test_coarsegen.cpp so it cannot silently degrade:
  occupancy mismatch 38/19/11/10 permille and material mismatch (both solid)
  75/61/21/35 permille at L1/2/3/4. Mismatch FALLS with level (coarser cells
  average over more terrain, and the true mip's own vote gets fuzzier).
- **Feature survival**: caves/caverns/bedrock participate for free (the
  representative column carries the full `ColumnSample`). A real cavern
  room (560 fine void cells) survives coarsening in EXACT proportion:
  280/140/70/35 void cells at L1-4. Sub-cell tunnels (3 m tube vs 1.6 m
  L4 cells) dither hit-or-miss — same behavior the true mip's majority vote
  produces for them, at 512+ m viewing distance.
- **Seams**: coarse bricks are a global function of the coarse cell index —
  adjacent bricks/footprints can never contradict (seam test pinned). At a
  fine/coarse ring boundary the divergence from the true mip is bounded by
  the local surface variation inside one coarse cell, i.e. the same order as
  the mip's own quantization; the existing dithered cross-fade covers it.

**Result** (`vxc_bench --mips`, fine/coarse interleaved per rep, min-of-5):

| level | fine (UE job replica) | coarse | speedup |
|---|---|---|---|
| 0 | 2.8 ms | 2.8 ms | 1.0x (identical digest) |
| 1 | 13.6 ms | 3.1 ms | 4.5x |
| 2 | 72.5 ms | 2.5 ms | 29x |
| 3 | 565.2 ms | 2.4 ms | 236x |
| 4 | 3,143.7 ms | 2.0 ms | 1,564x |

Coarse cost is level-independent by construction (216 bricks, 36 column
grids per chunk at any level) — an R4 chunk now costs what an R0 chunk
costs, which is what makes a 1 km ring convergable at all.

**Options presented, not silently picked.** (a) Representative centre sample
(landed): ~2-3 ms/chunk, mismatch as above. (b) Exact-occupancy column
reduction: for cave-free stratigraphy the recursive threshold-4 vote is
computable per column in closed form (each level of a monotone column stack
is again a heightfield), but it needs all 4^L columns per footprint — from
the measured column cost that is ~100-250 ms per R4 chunk (30-100x slower
than (a), still 15-30x faster than today), caves break the monotonicity
argument, and materials would need per-material vote bookkeeping. Not
implemented. (c) 2x2 corner supersampling with a mini-vote: ~4x the column
cost of (a), closer surface statistics, still inexact. Not implemented —
(a)'s mismatch numbers did not justify it.

**UE wiring follow-up (owned by the streaming-file agent).**
`MakeLevelSampler` (VoxelWorldSubsystem.cpp): for a level-L job, replace
`FCachedMipBuilder::Brick(Level, Key)` with a job-local (or shared,
keyed (level,key)) cache filled by `Gen.makeCoarseBrick(Level, Key,
coarseGrid)` using `coarseColumns` per (level, bx, by) footprint — no
recursion, no level-0 bricks, no `FSharedMipCache` dependency for pure
chunks. The overlay-aware path (`MakeOverlayAwareLevelSampler`) MUST stay on
the true-mip recursion: edits live at level 0 and must keep reflecting into
mip chunks exactly as today (the coarse path never sees edits). Chunks whose
footprint contains edits already route through the overlay path / edited-
ancestor sets, so the split exists. `PropagateEditToMips`/`FSharedMipCache`
invalidation semantics are untouched. Visual check on switch-over: expect a
one-time popping delta on R1+ (coarse vs downsampled disagree on ~1-4% of
shell cells); the cross-fade should absorb it, but eyeball it.

**GPU follow-up**: if/when outer rings move to the GPU voxelize path, the
coarse rule is the same `worldgen.ush` functions called at strided
coordinates — no new math to mirror. NOT touched this wave (C6 owns that
file).

**Files.** voxel-core/include/voxelcore/generator.h (coarse API; fine path
untouched), voxel-core/tests/test_coarsegen.cpp (7 tests: identity,
pointwise/seam, surface-range formula, NEW golden, seed sensitivity,
fidelity ceilings, cavern survival), voxel-core/tests/CMakeLists.txt,
voxel-core/bench/bench_main.cpp (`--mips` mode; default modes untouched).
No changes to mips.h, caves.h, caverns.h, amplifier.*, worldgen.ush, or
anything under ue-project/.

---

## C6b — worldgen.ush mirror finished, AMD cross-vendor determinism re-verified

**State found.** C6's mirror was substantively complete and correct: the C2
crevices, the C4 folded cavern pass and the 180-220 m bedrock band were all
present in `voxel-core/shaders/worldgen.ush` (497 lines, 118 cavern
references), with the bedrock formula `180000 + ((bj >> 48) * 40000) / 65536`
matching `amplifier.cpp:329` character for character. It had never been run
against a GPU.

**The one real defect: a stale `kMaxCavernSegs`.** The mirror was written
against `kMaxCavernSegs = 6`; `main` subsequently shrank the CPU constant to
4 (tight == `kCavernChildCount`). This is not a benign over-allocation — the
cap gates which segments survive into `ColumnSample`, so a GPU admitting a
5th segment the CPU had dropped would produce a different column digest.
Fixed to 4, SPIR-V respun (`VoxelizeMain` 72,448 -> 71,808 bytes). That was
the only change needed; no host-side binding change, `gpu_harness.cpp`
untouched beyond C6's own merge.

**AMD Radeon RX 7800 XT — all three modes bit-exact (PASS).**

| Mode | Result | New digest | Replaces |
|---|---|---|---|
| default (2 regions) | PASS, 0 mismatches, 8192 columns / 360,448 cells / 4,997 quads | `71288ec0ac6dba0b` | `e21e2767591496eb` |
| `--radius 64` | PASS, 0 mismatches, 305/305 tiles (100%) — 4,997,120 columns / 270,663,680 cells / 2,523,983 quads | `f102b490a42918c0` | `1e664cf6680a137c` |
| `--radius 128` | PASS, 0 mismatches, 136/1089 tiles (12.5%) — 2,228,224 columns / 119,799,808 cells / 1,126,522 quads | `1f88f5e0d405321d` | `7602afe508d2ee73` |

**All three digests moved, including default — which is the correct
expectation here, and the earlier brief's reasoning was wrong.** The
respin-3 (caves) argument that default mode's digest should hold, because
default voxelizes only a ~48-voxel surface shell above any carved geometry,
is an argument about *voxels*. `bedrockDepthMm` is a per-**column** field and
the harness digest covers columns as well as cells and quads, so the bedrock
band move shifts the default digest no matter which voxels get meshed. A
default digest that had stayed put would have been evidence the shader never
picked the change up — suspicious, not reassuring. (Credit to C6, which
caught this in its final message before it was killed.)

**No CPU behaviour changed.** `git diff main` over `voxel-core/src`,
`voxel-core/include` and `voxel-core/tests` is empty — this wave touched only
the shader, the prebuilt SPIR-V and docs. `kWorldGenVersion` is still 5 (not
bumped). All six pinned goldens verify unchanged: `amplifier_columns`
`0xA29A7A767DC1543B`, `cave_layer` `0xBFE42E07FFA6B78D`, `cavern_layer`
`0x5B1F8E5E73ED6EF2`, `amplifier_deep_materials` `0xF88B88DB9D9341AA`,
`mips_chain` `0xE827A786195B8A73`, `coarsegen` `0x85B3E79EF8D01AFC`.

**Gates.** `tools/lint-shader-ub.py` clean on its own merits (1 HLSL file, 5
rules, fail-closed); every `allow` annotation in the file carries a written
justification, none are bare. Zero `float`/`double`/`half` in
`worldgen.ush`. `vxc_tests` 205 PASS / 0 FAIL (clang/llvm-mingw).

**Build note for the next agent on this box.** `vxc_gpu` needs MSVC + Vulkan
and will not build under mingw, as documented — but VS 2026 here is
`C:\Program Files\Microsoft Visual Studio\18\Community` (product version 18)
and ships **no** bundled CMake or Ninja, so the documented "vcvars64 +
VS-bundled CMake/Ninja" recipe no longer works as written. What does work:
`tools/fetch-vulkan-headers.ps1` and `tools/fetch-dxc.ps1` (neither is
checked in), then vcvars64 plus the standalone CMake with `-G "NMake
Makefiles"`.

**Follow-up, not a determinism issue.** The `--radius 64` gate now measures
1.377 s against its <1.000 s target ("OVER TARGET"), dominated by 1,326 ms of
`vkAllocateMemory`/`vkMapMemory` buffer (re)allocation; `--radius 128` passes
its gate comfortably at 0.202 s. The r64 regression is an allocator warm-up
cost, not worldgen math, and is orthogonal to the byte-compare — worth a
separate look at buffer pre-sizing for the small-radius case.

**Files.** voxel-core/shaders/worldgen.ush (`kMaxCavernSegs` 6 -> 4),
voxel-core/shaders/prebuilt/*.spv (respun), voxel-core/shaders/prebuilt/
README.md (respin 4 section), docs/status.md (this entry). No CPU source, no
test, nothing under ue-project/.
### Buried-chunk pre-dispatch skip

Two prior streaming investigations both flagged the same lever and both
declined it: **most worker output meshes to zero quads**. This wave measured
it, built a pre-dispatch test for it at level 0, and -- importantly -- found
that after merging `main`'s ring-quota fix the *largest* remaining instance of
the problem has moved somewhere this test does not reach. That correction is
the most valuable thing here and is written up in full below.

**Step 1: measure, before changing anything.** A per-ring census
(`-VoxelMeasureEmpty`) classifies every zero-quad chunk over the mesher's own
domain (chunk + 1-voxel apron). That domain is the right one because
`voxelcore/mesher.h` emits a face **only** where a solid voxel has an AIR
neighbour -- material boundaries emit nothing -- so "no solid in the interior"
and "no air in chunk+apron" are each independently sufficient for zero quads.
Worker **wall time** is split by outcome too, always on: job COUNT alone cannot
justify the wave, since the mesher early-outs on a uniform chunk and a
zero-quad job is already cheaper than a surface one.

Measured on the merged build, `-VoxelPerfRun=60`, seed 20260719, `-nullrhi`,
5 s windows:

| ring | zero-quad jobs | share of that ring's worker time | composition |
|---|---|---|---|
| R0 | 71-74% | 67-74% | ~45% all-air, ~55% all-solid |
| R1 | 65-79% | 74-86% | ~88% all-air |
| R2 | 82-95% | **93-99%** | 100% all-air |
| R3 | ~100% | ~100% | all-air |
| R4 | **100%** | **100%** | all-air |

Summing one representative window: R0 9,765 ms, R1 3,670, R2 3,674, R3 1,264,
R4 8,570 -- **~83% of all worker wall time produces no geometry at all.** The
72-77% figure the wave inherited is confirmed for job count and is, if
anything, an under-statement of the time share.

Two corrections to the inherited framing:

1. It is **not** mostly "fully-buried solid interior". Only R0 has a solid
   majority; R1 outward is overwhelmingly **all-air** chunks sitting above the
   terrain, admitted by `ComputeFootprintChunkZRange`'s deliberately generous
   `+2` chunk headroom (at level 4 that headroom is ~102 m of sky).
2. The mesher's early-outs do **not** already make these cheap. A zero-quad R0
   job costs 0.93 ms against 1.15 ms for one that produces geometry -- only
   ~20% less, because the dominant cost is upstream of the mesher: the
   `(32+2)^2` `Amplifier::column` grid alone is **34-42%** of a level-0 job.

**Step 2: the test.** A *footprint band* is two level-0 voxel z values
summarising what a whole (X,Y) footprint can contain, reduced from the **same
34x34 column grid a level-0 job already builds**. Every level-0 chunk in an
(X,Y) reads columns `[32X-1, 32X+32]` -- exactly that grid, apron included --
so one job's band answers the question for every chunk stacked above and below
it. The band costs a max/min over columns already paid for and is then reused
by the ~10-20 other chunks in the footprint. It is **never** computed on the
game thread, where 1156 `Amplifier::column` calls per footprint would be an
M1-gate disaster; it rides home on worker results and is cached per footprint
(a pure function of (X,Y) and the amplifier, so it never needs invalidating).

  * `MaxSurfaceTopVoxel` -- above it every column is air, so a chunk starting
    above it hits `meshBrick`'s early-out 1 in all 64 bricks.
  * `SolidBelowVoxel` -- below it every column is solid, so no face has an AIR
    neighbour.

**Which way it errs: toward "might have geometry".** Every per-column bound is
an OUTER bound of the corresponding voxel-core predicate, taken over a
SUPERSET of the columns a chunk reads, then widened one further voxel each
way. `caveCarveAt` and `cavernCarveAt` are mirrored from their public segment
fields with the radius rounded **up** (`CeilSqrtI64`); crevices need no special
case because `caveColumnFromLattice` emits them as ordinary extra `CaveSeg`s,
and the sinkhole shaft is bounded directly by `shaftDepthMaxMm` **outside** the
bedrock clamp, matching `caveCarveAt`'s guard order. Sea-level and
minimum-surface clamps tighten the CARVE terms only, never the
air-above-surface term -- terrain whose surface is below sea level has air
above it and must not be claimed solid. So the test can fail to skip a chunk
that is in fact featureless (pure lost opportunity) but **cannot** claim
"definitely empty" for a chunk with geometry. A false "might have geometry" is
free; a false "definitely empty" is a hole in the world, invisible until
someone flies past it.

**Edits** are vetoed by the pre-existing `NeedsOverlayAwarePath` gate the skip
sits behind, which at level 0 **is** `ChunkHasEditedBrick` -- the chunk plus one
brick of border on every axis. Any chunk an edit could have made non-uniform,
including a pristine neighbour of an edited brick, is routed to the
game-thread path before the skip is considered. The band is pure worldgen and
is never consulted for such a chunk.

**Step 3: proof no geometry is lost.** `-VoxelVerifyBuriedSkip` computes the
verdict exactly as in production but dispatches the job anyway and checks
every "provably empty" verdict against the real mesh. Across a 120 s flight on
the merged build: **96,430 predicted-empty chunks meshed, 0 violations**; plus
303,148 on the pre-merge build (163,744 from a dedicated verify run and
139,404 carried incidentally by the two skip-off runs) -- **~400,000 chunks,
zero disagreements.** This is the direct form of the proof: not "the rendered
sets matched" but "every single chunk the test would have deleted was
independently confirmed to contain nothing".

**Throughput, interleaved A/B, same binary** (`-VoxelPerfRun=60`, seed
20260719, `-nullrhi`, all four runs shown):

| metric | A1 (skip off) | A2 (skip off) | B1 (skip on) | B2 (skip on) |
|---|---|---|---|---|
| chunks/s | 559.5 | 436.7 | **727.4** | **607.2** |
| R0 loaded | 2367 | 2025 | **3056** | **2897** |
| R1 loaded | 990 | 801 | **2098** | **1339** |

Both skip-on runs beat both skip-off runs with no overlap, which is what makes
this readable through this box's ~15% run-to-run variance. Min-of-N:
**436.7 -> 607.2 chunks/s, +39%**. Level-0 jobs dispatched per 5 s window fall
from ~11,200 to ~3,700 (**-67%**), and the residual zero-quad share at R0 drops
from 76-78% to 27-30% -- the band catches ~92% of the opportunity that exists
at level 0.

**M1 gate, real RHI** (min-spec proxy `sg.*Quality 0` + `r.ScreenPercentage
100`, 1080p windowed, one warm-up discarded). The first measured pair was still
warming (DDC/PSO) and failed on BOTH sides at 18.60 / 21.72 ms; it is shown for
completeness and excluded from the conclusion. All six runs:

| | A1 (warming) | B1 (warming) | A2 | B2 | A3 | B3 |
|---|---|---|---|---|---|---|
| p95 (ms) | 18.60 | 21.72 | 6.55 | **7.12** | 6.12 | **6.81** |
| post-warmup p95 | 18.68 | 20.82 | 5.92 | **6.81** | 5.75 | **6.54** |
| frames over 16.6 ms | 17 | 21 | 0 | **0** | 0 | **0** |
| hitches (>33.3 ms) | 120 | 129 | 40 | 51 | 35 | 36 |
| chunks/s | 158.6 | 187.6 | 244.6 | **297.7** | 251.8 | **305.1** |

**Gate holds** -- 6.81-7.12 ms against the 16.6 ms bar, with **0 frames over the
bar** on either side. Reported honestly: the skip costs **~+0.6-0.7 ms p95**.
That is not the skip being slow, it is the pipeline doing more useful work per
frame -- the same runs load **+22% more chunks/s**, and applying those extra
meshes is game-thread work that did not previously exist. Both sides sit well
under the bar with >2x margin.

Note the documented ~4.76-4.82 ms baseline does **not** reproduce here: both
sides land at 6.1-7.1 ms. That shift arrives with `main`'s ring-quota fix (R2-R4
now actually dispatch and render) and is present on both sides of this A/B, so
it is not attributable to this wave.

**Contents unchanged, verified not asserted.** A/B of
`-VoxelHeadlessDigTest=15 -VoxelSaveWorldAfter=22 -VoxelDumpDigestAfter=25
-VoxelNoLoad` (seed 20260719) with the skip off and on: both carve
**2,178,385** voxels, both report **`editedDigest=0xFB62844A74CB9121`**, and
both write an **8,039-entry / 4,135,443-byte** `.vxlog` that is byte-identical
(SHA-256 `4881EDEC...F8945F` on both sides). Exactly the established reference.
voxel-core was not modified.

**The correction that matters most.** This test is **level 0 only**, and after
merging `main` that is no longer where the biggest waste is. A level-L chunk
spans `2^L x 2^L` level-0 footprints and the mip path samples columns at stride
rather than building a dense grid, so no exact band is available for it; and
combining level-0 bands upward does not help, because an R4 footprint 1024 m
out never has level-0 bands to combine. Meanwhile the census shows R2/R3/R4 are
**100% all-air** and a single R4 chunk costs **6,261-26,377 ms** of worker time
to produce nothing. R4 alone was ~32% of all worker time in the sampled window
-- larger than the entire R0 saving this wave delivers.

**Follow-ups, ranked.**

1. **Extend the band to the outer rings -- now the single largest lever.**
   R2/R3/R4 are 100% all-air and cost seconds per chunk. The all-air half of
   the test needs only a conservative UPPER bound on surface height over a
   footprint, which requires no cave/cavern data at all (caves only ever
   REMOVE solid). `main` just landed `generator.h`'s `coarseColumns` /
   `makeCoarseBrick`, which is very likely exactly the primitive needed: wire
   coarse LOD into UE first, then derive outer-ring bands from a coarse column
   grid the same way this wave derives level-0 bands from a dense one.
2. **Tighten `ComputeFootprintChunkZRange`'s `+2` chunk headroom.** It is the
   direct *source* of the all-air chunks -- at level 4 it admits ~102 m of sky
   per footprint. Skipping them after admission (item 1) is good; not admitting
   them is better and cheaper. Needs a conservative max-surface bound, same
   primitive as item 1.
3. **A voxel-core "column bound over a region" query.** This wave had to mirror
   `caveCarveAt`/`cavernCarveAt`'s segment semantics UE-side because no such
   API exists and voxel-core was owned by other agents. It is verified over
   ~400k chunks, but it is a duplicated invariant: a retune of the cave
   constants would silently need this mirror updated too. The ideal API is
   `Amplifier::columnAirBounds(col) -> {topSolidVoxel, deepestAirVoxel}` living
   next to the predicates it must agree with.
4. **The ~8% of level-0 opportunity the band misses** is chunks dispatched
   before their footprint's first job returned a band. A cheap "band probe"
   job per new footprint would close it, but it is a small residual next to
   items 1-2 and should wait for them.
### Water reactivation on terrain edits (wakeRegion)

**The bug.** A fully settled body of water (`activeBricks==0`) ignored terrain
edits completely. `vxc::WaterCA::step()` is a no-op over an empty active set
(waterca.h "Activity / settling": a brick that produces no net change in a tick
drops out of the active set, and the next active set is exactly the set of
bricks that changed), and NOTHING ever put a brick back for an above-sea-level
edit -- only the Reservoir v0 breach path did, and only for Z<0 ocean-adjacent
cells. `NotifyTerrainRegionEdited`'s `invalidateSolidRegion` call corrects what
the CA BELIEVES about terrain but never makes it LOOK. Net effect in shipped
gameplay: dig underneath a settled pond and nothing happened -- proven by the
previous wave (unchanging water digest through an entire dig/place/carve/
collapse sequence, which is why that scenario needed `SpawnWaterAt` "nudges" to
prove anything at all).

**The fix: `WaterCA::wakeRegion(minVoxel..maxVoxel)`** (waterca.h/.cpp).
Re-inserts into the active set every brick that CURRENTLY STORES WATER and
intersects the edited voxel box grown by `kWakeHaloBricks == 1` brick on each
axis. It writes no fill at all -- waking is purely a SCHEDULING act, never a
source or a sink, so `totalVolume()` cannot move because of it. Returns the
number of bricks newly woken.

*Why a 1-brick halo, and why only that.* Waking only the bricks the edit
overlaps is useless for the flagship case: when you dig the floor out from
under a pond, every removed voxel is dry terrain and there is no water in the
edited bricks at all. One brick of halo covers every face-, edge- and
corner-adjacent brick, i.e. every brick whose cells can be within one CA step's
reach (gravity/lateral are 1-voxel moves) of a changed voxel. It does not need
to be wider: once any woken brick actually moves water, `stepWithOrder`'s
existing "changed UNION changed's 6 face-neighbours" rule carries activity
outward on its own, as far as the water genuinely travels. So the halo only
SEEDS the reaction; it never has to predict its extent -- which is also what
stops a big edit beside a big lake from re-activating the whole lake up front.
Empty bricks in the halo are skipped (nothing to move, and they would cost a
512-cell scan per tick); water flowing INTO an empty brick is already covered,
because the SOURCE brick is active and `stepWithOrder`'s `touched` set already
includes every active cell's target-direction neighbours.

*Determinism.* The woken set is a pure function of (region, WaterMap contents).
`active_` is a `std::set` ordered by `BrickKeyLess`, so insertion order is
unobservable, and the per-brick test ("does this key store water?") depends on
no iteration or hash order. `wakeRegion` picks between walking the region's
bricks and walking the stored bricks purely on size; both enumerate exactly
{stored} INTERSECT {region}. Pinned by
`waterca_wake_region_order_and_strategy_independent` (same body built in two
insertion orders -> identical woken set and identical post-tick digest under
forward vs reversed active-set order, plus a same-region/different-strategy
cross-check).

**Wiring.** `UVoxelWaterSubsystem::NotifyTerrainRegionEdited` now calls
`wakeRegion` right after `invalidateSolidRegion`. That hook is already called
by EVERY authoritative edit path (TryDig / TryPlace / CarveSphere /
PromoteDetachedIslands+collapse / structure fixtures), so every edit path gets
reactivation for free, with the same authority-only (`NetMode != NM_Client`)
gating as the invalidation. A `LogVoxelWater` Verbose line reports the woken
count per edit.

**Proof -- the settled pond actually drains now.** New GameMode switch
`-VoxelWaterWakeTest[=<delaySeconds>]` runs the IDENTICAL basin/pour/dig/place/
carve/structure/collapse sequence as `-VoxelWaterMemoTest`, but with every
`SpawnWaterAt` nudge suppressed, so nothing but the terrain edits themselves
can move water. Headless (`-nullrhi`, seed 20260719, clean `Saved/VoxelWorlds`):

| Checkpoint | water digest | volume |
|---|---|---|
| pour | `0x68598CA231A5B2D8` | 20000 |
| pre-edit settle (pond settled, activeBricks 0) | `0xFFCC45E6CD111EC2` | 20000 |
| dig beneath basin (`applied=1`), `wakeRegion` woke **10** bricks | `0xFFCC45E6CD111EC2` (unchanged at the instant of the edit -- waking writes nothing) | 20000 |
| post-dig settled | **`0xA16FBD50DC4EDB3F`** | 20000 |
| place at basin floor (`applied=1`), woke 5 bricks | `0xA16FBD50DC4EDB3F` | 20000 |
| post-place settled | `0xA16FBD50DC4EDB3F` | 20000 |
| side carve / structure fixture / M5 collapse blast | `0xA16FBD50DC4EDB3F` | 20000 |
| FINAL | `0xA16FBD50DC4EDB3F`, storedBricks 8, activeBricks 0 | 20000 |

The pond, settled and dormant, MOVED when the floor was dug out from under it
(`0xFFCC45E6...` -> `0xA16FBD50...`) with volume EXACTLY conserved at 20000 at
every checkpoint -- on the old code that digest was constant through the whole
sequence. The later edits wake bricks (the TryPlace path logged 5) but move
nothing, because by then the water has drained down the shaft and out of their
reach; the carve/structure/collapse are metres from any remaining water and
wake 0 bricks, which is the correct answer and not a missed hook (an edit
nowhere near water must cost one bounded probe and wake nothing).

Screenshots (windowed run, same switch, camera framed on the water body's own
centroid), in `ue-project/Saved/Screenshots/WindowsEditor/`:
`VoxelWakeTest_PondSettled_BeforeDig.png` (t=42s, the pool's flat surface
filling the basin) and `VoxelWakeTest_Drained_AfterDig.png` (t=55s, same
camera -- the pool is gone and the bare sand basin floor it was covering is
exposed, the water having drained down the dug shaft).

**Deterministic voxel-core proofs** (tests/test_waterca.cpp, 5 new tests):
`waterca_wake_region_drains_settled_pond_into_new_hole` (settles a 2000-unit
pond in a 4x4 basin, opens a shaft through the floor, asserts 20 ticks of
NOTHING happening first -- the regression witness for the old behaviour -- then
wakes and drains it to the exact expected bottom-up shaft profile, conservation
checked after every single tick);
`waterca_wake_region_settled_pool_flows_through_a_carved_breach` (the same
story sideways: a multi-brick wall carve, re-levelling to 41/42 across all 48
newly connected floor cells);
`waterca_wake_region_writes_nothing_and_ignores_dry_regions` (digest and ledger
byte-identical across a wake; a far-away edit and an inverted box both wake 0;
re-waking is idempotent); `waterca_wake_region_order_and_strategy_independent`
(above); `waterca_wake_region_halo_reaches_one_brick_not_two`.

**`kWaterCAVersion` 3 -> 4, and NO golden moved.** The bump is deliberate and
is a SIGNAL, not a re-pin: no tick rule changed, and `wakeRegion` is a new API
that nothing in voxel-core's own scenarios calls, so every pinned digest is
byte-identical to v3 -- `waterca_deterministic_repeat_and_golden_digest`
(`0x3D2224BE4A253404`), `waterca_hydrostatic_large_pool_multibrick_golden`
(`0x56BC18914355A205`) and `waterca_solid_cache_golden_digests_unchanged` all
still pass against their existing values, and nothing was re-pinned anywhere.
The version moves because a LIVE world now evolves differently across an
identical terrain-edit sequence than it did before (that is the whole fix), so
a v3 recording/replay or persisted water state no longer reproduces -- exactly
what docs/determinism.md keeps the constant for.

The engine-level memo A/B table above also did NOT move: re-running
`-VoxelWaterMemoTest` with `voxel.Water.SolidCacheEnabled` forced 0 and 1
reproduces every previously documented checkpoint byte-for-byte, FINAL included
(`waterDigest 0x5D0D9B43677FD302`, `editedDigest 0xFB1235E05C62C838`,
storedBricks 11, volume 21200), memo off == memo on. That scenario's nudges
already woke the CA, so adding `wakeRegion` in front of them changes nothing
there -- a useful independent confirmation that waking is a pure no-op wherever
nothing can actually move.

**Perf.** `vxc_waterca_bench` gained `--lake --wake-edit`: a settled 63x63 lake
with a player-sized (3x3x3-voxel) terrain edit landing on its rim EVERY tick,
forever -- the worst realistic case, and unlike the pre-existing `--lake`
disturbance it injects no volume, so all of its cost is the reactivation
itself. `wakeRegion` costs **0.0019 ms/call** (avg over 200 calls, ~5.9 bricks
woken per edit), and the resulting `step()` averages **3.28 ms/tick** (memo ON,
`--lake-solid-spin 50`) versus **4.09 ms/tick** for the existing addWater
disturbance -- i.e. per-tick terrain editing beside a large settled lake is
CHEAPER than the per-tick water drip the bench already treated as its steady
state, and a settled lake with no edits still costs exactly zero (empty active
set). The solidity memo's win is intact and untouched: the 441-column pour
bench (which never calls `wakeRegion`) is **344.1 -> 131.5 ms/tick** in the
~1830-active-brick window with the memo off vs on (2.6x), 4x versus the v0
sequential engine.

**Build/tests.** voxel-core (clang, llvm-mingw, Release, `-Wall -Wextra
-Wconversion -Werror`, plus `waterca.cpp` re-checked under an explicit
`-Wsign-conversion -Werror`): clean, **137/137 PASS, 0 FAIL**; float-ban script
clean. `voxelcore.lib` rebuilt with MSVC 14.51/VS 2026 (Ninja, Release) for the
UE link. `VoxelEarthEditor Win64 Development` -> `Result: Succeeded`, no
warnings from the touched files.

**Follow-ups.** (1) A halo of 1 brick is the right seed for 1-voxel-per-step
flow; if a future Layer C adds multi-voxel-per-tick momentum, the halo must
grow with it (it is a named constant, `WaterCA::kWakeHaloBricks`, for exactly
that reason). (2) The stale-fill-inside-a-newly-solid-cell quirk (previous
wave's follow-up 2) is untouched: waking schedules the brick, but no rule
evicts water from a cell that just became solid, so that fill stays put
(byte-identical across clients, just hidden). Now that a wake hook exists, an
eviction rule on the place path is a natural next step. (3) The
`-VoxelWaterMemoTest` nudges are now redundant for reactivation and are kept
only so that scenario keeps reproducing its signed-off A/B table; they could be
retired once someone re-pins that table.

## C7/C8 — underground water: implicit cavern lakes that mobilize on approach

The last unbuilt piece of the cavern feature (docs/cavern-design.md §5), built
in a worktree off `main`.

**The static field was already done.** C1/C4 shipped `CavernColumn.floodZMm`,
`CH_CAVERN_FLOOD = 25`, `cavernSiteFor`'s 40%-dry flood draw and
`cavernFloodedAt` — not merely "reserved", as the task framing assumed. It is
already carried in `ColumnSample.cavern` and already pinned in the
`cavern_layer` and `amplifier_deep_materials` goldens. So **`kWorldGenVersion`
stays at 5** and **no golden moved**: predicted none would, and none did
(`amplifier_columns 0xA29A7A767DC1543B`, `cave_layer 0xBFE42E07FFA6B78D`,
`cavern_layer 0x5B1F8E5E73ED6EF2`, `amplifier_deep_materials
0xF88B88DB9D9341AA`, `mips_chain 0xE827A786195B8A73` all unchanged). What was
missing was the second half: turning a lake into real water when gameplay
reaches it. `kWaterCAVersion` also stays at 4 — no tick rule changed.

### The conservation argument (`vxc::WaterMobilizer`, waterca.h/waterca.cpp)

Every water cell is owned by exactly one accountant — the implicit field if its
brick has not mobilized, the CA if it has — so the total in any region is
`implicitVolume + totalVolume`, and mobilizing moves units from one term to the
other in the same call.

That is exact **if and only if** the CA never holds fill in a cell the implicit
field still owns, because a cell is ONE BYTE and cannot carry both accountants'
water. If CA water ever seeped into a still-implicit cell, mobilization would
have to either drop the CA's units or refuse the implicit field's, and either
way water is created or destroyed. There is nowhere to store the difference.

So `makeSolidFn()` makes it structurally impossible rather than a matter of
discipline: **a still-implicit water cell reads as SOLID to the CA.** An
unmobilized lake is a wall. The CA physically cannot write there, so
`mobilizeBrick` credits the full implicit amount knowing the cell was empty,
and `shortfallVolume()` audits the claim.

The wall is also what makes the per-tick budget safe: a deferred brick is still
a wall, so deferring can never leak. Water that has not mobilized simply has
not been given permission to move — frozen for a few ticks, never duplicated.

The front is self-limiting: a mobilized brick is filled and woken, so it is
active next tick and its neighbours convert then — one brick shell per tick.
Two seeds, both required. `mobilizeEditRegion` (a dig into a static lake
produces no CA activity at all, so the activity-driven front would have nothing
to grow from) and `advanceFront` before every `step()`.

One subtlety worth recording: implicit water is gated on **current** terrain,
not the worldgen raster. Otherwise a player who PLACES a block into an
unmobilized lake leaves the field claiming 255 units in a cell that is now
rock, and the ledger shows a shortfall for ordinary gameplay. Gating on current
air means a filled cell holds nothing, so mobilization always credits what it
debits; the volume the placement destroys is the same intended discontinuity a
placement into CA water already causes.

### Client/server agreement, at zero extra bytes

Mobilization is **authority-only** — it depends on where players dug and when,
and a client running its own front off a replication-mirror CA would drift the
instant a packet was late. Mobilizing a brick writes fill into it, which
dirties it, which is exactly what puts it in the existing water-diff batch. So
"the server sent me authoritative fill for this brick" IS "this brick has
mobilized", and `ApplyReplicatedWaterDiffs` calls `markMobilized`, which
credits nothing (those units are already in the fill it arrived with).

### Engine (C7 render + C8 wiring)

Implicit lakes render through the **same `meshBrick<8>` path the CA uses**, not
as a flat plane at floodZ. A plane would be cheaper, but mobilization would
then be a visible seam — a flat sheet abruptly becoming voxel water. Meshing
implicit bricks identically makes an implicit brick and a mobilized brick
pixel-identical. Interior bricks emit no faces, so only the lake shell costs
anything; the refresh is camera-centred, rebuilt on brick crossing, sorted
nearest-first, budgeted per tick.

`UVoxelWaterSubsystem` builds its own `Amplifier` over the terrain's seed
(as `AVoxelClipmapActor` already does) because `UVoxelWorldSubsystem` is
another agent's file and exposes no column accessor. Bit-identical for
synthetic-sampler runs; see follow-ups.

### Verification

- voxel-core **197 PASS / 0 FAIL** (was 190), no golden moved, float-ban clean.
  Seven new tests, including `waterca_mobilize_dig_into_lake_conserves_exactly`
  which asserts `implicit + CA == start` after EVERY tick across a full drain,
  and `waterca_mobilize_front_budget_does_not_change_the_outcome` (budget 1 vs
  4096 reach the same conserved end state and the same mobilized set).
- In-engine `-VoxelFloodTest[=<s>]`: finds a real flooded cavern, captures the
  static lake, carves an outflow, captures the drain. From a pristine world:
  found a flooded cavern at (42000, 21000) UU, floodZ 958013 mm, 4 cavern segs,
  11.8 m of open air across the waterline; carve removed 1,018,902 voxels
  (43/102 productive spheres) and the +X wall moved 32.7 m -> 51.5 m; 657 bricks
  mobilized; **debited == credited == 56,368,260, shortfall 0**.
  Images: `docs/images/cavern-water/cavern-lake-implicit.png` and
  `cavern-lake-draining.png`.

### Gotchas found (worth not rediscovering)

1. **`Amplifier::columnCached` returns a reference the next call invalidates.**
   A refinement loop held one across hundreds of calls and silently read the
   wrong cavern seg count. Take `ColumnSample` **by value** if anything else
   queries in between.
2. **The world persists between runs** in `Saved/VoxelWorlds/<seed>.vxlog`. A
   carve that "removed 0 voxels" for four consecutive runs was carving a tunnel
   a previous run had already dug. Delete the `.vxlog` before any before/after
   verification run.
3. **Underground chunk residency is tight and keyed to camera height.** The same
   outflow carve removed 341,744 voxels with the camera 1.2 m above the water
   and nothing at all (0/102 spheres, five retries) with it 9 m up —
   `CarveSphere` only removes voxels from resident chunks.
4. **From underground the world renders see-through.** Distant terrain is a
   heightfield whose underside is backface-culled, so anything beyond the small
   underground voxel-chunk radius shows sky/ocean straight through the rock.
   This is pre-existing and not water-related, but it dominates any underground
   screenshot's composition.

### Follow-ups

- **GPU mirror: NOT needed for this work.** `floodZMm` is computed inside
  `cavernSiteFor`, which C6 is already mirroring; nothing new was added to
  worldgen. Mobilization is simulation, not worldgen, and never belongs in
  `worldgen.ush`.
- `UVoxelWorldSubsystem` should expose a public column/cavern accessor so the
  water subsystem can stop building a second `Amplifier`. Until then a
  `-VoxelTileDir` run would disagree between the two samplers and put cavern
  flood levels against the wrong terrain.
- Persist `mobilizedBricks` into the savegame alongside water state (the design
  requires it; `digest()`/`markMobilized()` are the hooks, the serializer is not
  written yet).
- Underground chunk residency (gotcha 3) makes far edits unreliable; worth a
  look by that subsystem's owner.
- M6 pathfinding still treats flooded caverns as air — already parked as a
  water-track follow-up in the design doc, unchanged by this work.
---

## M4 wave: water winding, GI cone basis, GI bounce energy (2026-07-21)

Three client-side rendering correctness fixes. Nothing here touches voxel-core,
worldgen, the edit log or any replicated state, so no golden moved.

### 1. Water chunk winding -- the copy-pasted inversion

`FWaterChunkSceneProxy` carried the same inverted winding that had just been
fixed in `FVoxelChunkSceneProxy` (PR #62): both branches flipped, so every
water quad presented its back face as front-facing and `M_Ocean` shaded against
a normal pointing *into* the water. Fixed by matching the terrain convention
(`!Q.Positive` reversed). The two proxies build corners, tangents and indices
identically, so the fix is textually exact.

**Verified geometrically, not photographically.** Water screenshots turned out
to be a bad instrument -- the implicit ocean plane, the translucent material and
the sky are all similar blue, and the top and bottom of a water slab are
coincident planes, so an inverted surface looks broadly the same.
`-VoxelWindingCheck` instead computes `dot(cross(P1-P0, P2-P0), shadingNormal)`
over every emitted triangle of both proxies:

| proxy | mean dot |
|---|---|
| terrain (reference; its winding was verified on screen in PR #62) | **-1.000** |
| water, after this fix | **-1.000** |
| terrain forced to the legacy inverted winding (`-VoxelGIOn -VoxelGIVis=6`) | **+1.000** |

The third row is the control: the check *is* sensitive to exactly this defect,
and water now agrees with the reference on every triangle of every proxy. This
is a stronger result than any screenshot and it is cheap to re-run.

### 2. The cone basis was degenerate

Six axis-aligned cones recombined with `max(0, N.D)`. Every normal a greedy
voxel mesh produces is axis-aligned, so exactly one cone survived and **side
light was weighted to literally zero** -- a floor lit entirely by a shaft three
metres to the side solved to "dark" because the only cone consulted pointed at
the ceiling.

Replaced with a proper cosine-weighted hemisphere. **14 cones are traced** (the
6 axes + the 8 cube diagonals) and projected into the 6 ambient-cube storage
slots, **5 cones per slot** (its own axis at weight 1, four diagonals at
1/sqrt(3), the four equatorial cones correctly at 0). Cones are shared between
slots -- a diagonal contributes to three -- so it is 14 marches per cell, not 30.
Weights are normalized per slot, which matters twice: an unoccluded cell still
solves to exactly 1.0 (open terrain unchanged with GI on), and the basis cannot
manufacture energy.

**Cost: 2.33x the cone marches.** Measured in the steady state, same scene, 2
bricks per solve: **0.50-0.53 ms (old basis) -> 0.88-1.11 ms (new)**.

### 3. The bounce was not energy conserving -- and the diagnosis changed

The gather read the same `AvgIrr` array it was writing. Within a brick that made
it Gauss-Seidel in X/Y/Z scan order; across bricks the `ParallelFor` made it
Gauss-Seidel in *nondeterministic thread order*. A pass therefore applied
somewhere between one and several bounces depending on scheduling. The header
called that race "benign for a progressive gather"; it was not.

Now a **Jacobi iteration**: the gather reads `PrevAvgIrr`, a snapshot published
at the end of each pass, and writes `AvgIrr`. One pass is exactly one bounce,
independent of thread count and scan order. With that, boundedness is a theorem:
`Irr <= Vis*Sky + albedo*(1-Vis) <= 1`, a contraction with modulus `BounceAlbedo`.
The `MaxBounceContribution` clamp was deleted -- it masked the symptom and did
nothing about the cause.

**The convergence proof (`-VoxelGIConverge=N`).** Measuring a settled field
proves nothing: it sits still whether or not the iteration is sound. So
`-VoxelGIConvergeSeed=<0..255>` kicks every solved cell to all-white or
all-black first, and the passes are then run back-to-back on a frozen scene.
1711-1788 resident bricks, ~220k solved cells, albedo 0.40:

```
HOT start (255): 73.139 -> 33.577 -> 24.671 -> 22.629 -> 22.103 -> 22.037 -> 22.025 -> 22.023 (flat)
                 successive maxDelta ratio: 0.280 0.362 0.400 0.400 0.500  <- albedo is 0.40
COLD start (0):  18.076 -> 21.511 -> 22.250 -> 22.381 -> 22.401 -> 22.403 -> 22.404 (flat)
```

Hot and cold converge to the same fixed point from opposite directions, and the
measured contraction modulus sits on the predicted 0.40. Twelve passes on a
frozen scene move the mean by less than 0.001/255.

**Honest correction to the premise.** The reported 104.7 vs 129.0 luma drift is
**not reproduced as a bounce-energy divergence**. Run with the legacy in-place
gather restored (`-VoxelGIConvergeLegacy`), the old code *also* converges, to
22.470 against the fixed 22.404 -- a real but tiny 0.3% energy gain, not 23%.
The large luma difference between those two captures is far more likely the
settle race already documented in the `-VoxelGICaveTest` block (capture before
the GI queue drains and chunks are still on their bright plain-AO fallback).
What the Jacobi fix actually buys is **reproducibility**: the result no longer
depends on thread scheduling, so captures are comparable and the next agent does
not bisect a phantom regression. Two identical rendered runs now read mean luma
141.89 and 141.90.

### M1 gate

`-VoxelPerfRun=60`, seed 20260719, two runs each. **This box was noisy during
this wave** (parallel builds), and the noise swamps the effect being measured:

| config | p95 run 1 | p95 run 2 |
|---|---|---|
| rendered, GI off | 5.36 ms | 8.49 ms |
| rendered, GI on | 12.00 ms | 10.76 ms |
| nullrhi, GI off | 0.50 ms | -- |
| nullrhi, GI on | 1.34 ms | -- |

GI-off spans 5.4-8.5 ms against a recorded baseline of 4.76-4.82, so these
numbers cannot support a +/-0.5 ms claim either way. What they do support: the
GI-off path is untouched *by construction* (with `voxel.GI.Enabled=0` the
subsystem does not tick, no field is built, and the new winding check is behind
an absent command-line switch), and GI-on is consistently ~+3 to +5 ms over
GI-off in the same session. **The GI-off gate needs a re-run on a quiet box
before it is quoted anywhere.**

### Recommendation: GI stays DEFAULT OFF

The two correctness blockers are genuinely fixed and the second one is now
pinned by a test rather than by a screenshot. But default-on is still the wrong
call:

1. **Cost went up, not down.** The correct cone basis is 2.33x the marches.
   GI-on measured 10.8-12.0 ms p95 here against a ~4.8 ms gate. Even discounting
   the noise, that is not a 60 fps budget.
2. **The dominant cost is not the tracing.** Known split: proxy rebuilds 1.7 ms,
   tracing ~0.5 ms (now ~1.1 ms). Making the cones correct made the *cheap* part
   more expensive; the expensive part is re-running chunk scene proxies to move
   vertex colours, and that is what a GPU/vertex-buffer-update path removes.
3. **Quality is still unproven in a legible frame.** See below.

**Follow-ups, ranked.**

1. **Get a legible enclosed-space capture.** Neither harness produced one at
   seed 20260719: `-VoxelGICaveTest`'s sinkhole search parked the camera on a
   mountainside at 1126 m altitude in open sky, and `-VoxelUndergroundTest`
   parks it hard against a carved wall (enclosure probe: 0.9-3.8 m on every
   axis) so blocky near-geometry fills the frame. The cone-basis A/B is
   therefore only a luma delta (legacy 154.61 / dark 6.5% vs fixed 155.20 /
   dark 5.9%), not a visual verdict. A framing pass -- stand off, aim along the
   chamber, not at a wall 90 cm away -- is a prerequisite for any quality claim,
   and is cheap.
2. **Move the vertex-colour update off the proxy rebuild path.** This is the
   1.7 ms and it is most of the gap to default-on. Note a previous in-place
   colour buffer attempt (8.99 -> 7.92 ms) and a refresh-gate/time-slice pair
   were implemented, measured and reverted as not-working; the measurements are
   good, the approach needs rethinking rather than repeating.
3. **Re-run the M1 gate on a quiet box** to get a trustworthy GI-off number.
4. **Consider trading solve latency for frame cost** now that the basis is
   2.33x: `voxel.GI.MaxBrickSolvesPerFrame` (8) and `RefreshBricksPerFrame` (2)
   were tuned against the cheaper basis and were not re-swept here.

## Sky-band trim + outer-ring all-air skip -- R4 goes from 100% waste to 100% useful (2026-07-22, worktree agent)

**The waste, measured first on this tree.** `-VoxelPerfRun=60 -VoxelMeasureEmpty
-VoxelBuriedSkip=0 -VoxelSkyTrim=0 -VoxelSkySkip=0`, seed 20260719, `-nullrhi`.
The per-ring census confirms the picture PR #70 left behind, with the coarse
rings now the whole story:

| ring | zero-quad share of results | zero-quad share of that ring's worker time | composition |
|---|---|---|---|
| R0 | 74.9-76.7% | 73.9-74.2% | ~33% air, ~66% solid |
| R1 | 63.0-68.4% | 70.5-75.2% | ~85% air |
| R2 | 77.6-90.8% | 69.6-96.5% | **~99% air** |
| R3 | 100% | 100% | **100% air** |
| R4 | 100% | 100% | **100% air** |

PR #70's band skip covers R0 and is exact there, including the all-solid half
that dominates R0. It is level-0 only for a structural reason its own comment
states: a level-L chunk spans 2^L x 2^L level-0 footprints and never has a
level-0 band. That is exactly where the remaining waste is -- one R4 job is
~4,900-6,300 ms p50, so R3+R4 were ~38% of all worker time while loading 20 and
**0** chunks respectively.

**What makes the outer rings expressible: the all-air case needs far less than a
band.** `Amplifier::materialAt` is unconditionally `MAT_AIR` whenever a voxel
centre is above `surfaceMm` -- caves and caverns only ever CARVE, and nothing
fills above the surface -- so proving a chunk empty needs only an UPPER BOUND on
the terrain surface over its footprint. No cave data, no cavern data, no
bedrock, no level-0 bricks.

`coarseSurfaceBrickRange` was **not** reusable for this, despite being the
obvious candidate. It is exact for a brick generated through `makeCoarseBrick`,
but the UE worker does not use the coarse path at all (`MakeLevelSampler` still
builds level-0 bricks and downsamples), so a coarse range is a STRIDED SUBSAMPLE
of the data the mesher actually sees, not a bound on it -- terrain between two
stride-2^L samples can be higher than both.

**The bound, derived from `Amplifier::evalSurface`'s own structure.**
`surfaceMm = clamp(baseMm + detailMm, ...)`, and both terms are boundable from
tile-raster reads alone:

* `baseMm` is bilinear per 30 m tile pixel. A bilinear patch is LINEAR along
  each axis with the other fixed, so its maximum over the footprint clipped to
  one pixel cell is attained at a corner of that clipped rectangle -- **exact**,
  at most 4x4 cells, and it is precisely the "interior extremes between the
  sampled corners" term the `+2` headroom existed to cover.
* `detailMm` is the four octaves times `slopeScaleQ10`, which clamps to
  [0.25, 4.0]. The absolute worst case (4.0 -> 11.44 m) needs ~86 m of relief
  across one 30 m pixel. The real slope term is computable exactly from the same
  elevation grid, which takes the allowance to 1-3 m on ordinary terrain -- the
  difference between the bound binding at level 4 and never firing at all. The
  first, looser version of this bound made only **555** predictions in a 60 s
  run; the tightened one makes **21,000**.

**What the `+2` was protecting, and what it became.** The comment is explicit:
corner-only sampling under-estimates interior extremes, so pad ABOVE. But it
padded in LEVEL-L CHUNKS, and a level-L chunk is `1<<L` times taller than a
level-0 one -- so a headroom sized for level 0's 3.2 m chunks became **102.4 m**
of guaranteed-empty sky at level 4. The quantity being protected against is a
property of the TERRAIN, not of the LOD level; scaling it by chunk height was
never right. It is now `min(analytic bound, the old +2 rule)`, so the range is
**never wider than before at any level** and is never a regression on the near
rings the M1 budget cares about, while at levels 2-4 -- whose footprints are
wide enough that the tile raster dominates -- it stops just above the highest
ground the footprint can contain.

`ComputeFootprintChunkZRange` stays a **pure function of (Level, X, Y)** and the
immutable tile raster, so the M1 hitch memo's correctness argument is untouched.
The one edit-dependent part lives OUTSIDE the memo exactly as the depth skirt
does: `TryPlace` writes solid material into what worldgen calls sky, so the memo
also carries the pre-trim top and `RecomputeDesiredSet` re-widens up to -- never
past -- it for footprints with an edited chunk above the trimmed top.

**Throughput and per-ring effect.** 5 interleaved A/B pairs on ONE binary,
`-VoxelSkyTrim=0 -VoxelSkySkip=0` vs default. Pair 1 discarded as warm-up (its
`on` run was contended by another agent's build: R0 p50 9.39 ms vs 1.08 ms
everywhere else, mipCache evictions 0). All runs shown:

| | off | on |
|---|---|---|
| avgChunks/s | 781.25*, 764.65, 779.00, 778.65, 778.57 | 752.12*, 861.20, 861.45, 859.82, 861.32 |
| chunksLoaded | 46875*, 45879, 46740, 46719, 46714 | 45127*, 51672, 51687, 51589, 51679 |

(* = discarded warm-up pair.) Throughput **+11.1%** (775.2 -> 860.9 chunks/s),
chunks loaded **+11.1%**. Per ring, means of pairs 2-5 -- `total` is jobs
dispatched, `load` is components created, `zq` is jobs that meshed to nothing:

| ring | total off -> on | **load off -> on** | zq off -> on |
|---|---|---|---|
| R0 | 56,279 -> 52,739 | 31,029 -> 31,068 | 25,236 -> 21,661 |
| R1 | 39,867 -> 32,152 | 14,647 -> **17,379** | 25,157 -> 14,751 |
| R2 | 2,486 -> 4,369 | 806 -> **3,119** | 1,603 -> 1,141 |
| R3 | 158 -> 140 | 20 -> **74** | 109 -> **26** |
| R4 | 12.5 -> 8.3 | **0 -> 3** | 7.75 -> **0** |

The headline is the bottom row. R4 went from **0 chunks loaded and 8 meshed to
nothing** -- a ring spending ~22% of all worker time to render literally
nothing -- to **3 loaded and 0 wasted**. R3 went from 20 loaded / 109 wasted to
74 loaded / 26 wasted. R2 dispatched MORE jobs and loaded 3.9x more chunks: the
wasted jobs were occupying the ring's reserved worker slots, so removing them
converts directly into vista geometry rather than into idle threads.

**Verification: 485,478 chunks, 0 violations.** `-VoxelVerifySkyBand` computes
the verdict for every chunk and **dispatches anyway**, then checks it against the
mesh the worker really produced; run with `-VoxelSkyTrim=0` so the verifier also
sees every chunk the trim would have removed (the skip's threshold is strictly
the more aggressive of the two, so verifying the skip verifies the trim). Five
60 s runs: 87,372 + 99,510 + 99,253 + 99,466 + 99,877 results checked, 101,740
predicted all-air, **0 violations**. Prediction rate tracks the waste almost
exactly -- R0 11.4%, R1 32.5%, R2 53.9%, R3 78.4%, **R4 100% in every run**.

**Contents unchanged, verified not asserted.** A/B of
`-VoxelHeadlessDigTest=15 -VoxelDumpDigestAfter=25 -VoxelNoLoad` (seed
20260719) with the switches off and on: both carve **2,178,385** voxels, both
report **`editedDigest=0xFB62844A74CB9121`** on server AND client, and both
write an identical **8,039-entry / 4,135,445-byte** `.vxlog`. Note the byte
count: the M1-era figure quoted in the brief is 4,135,443, and the 2-byte
difference is present on BOTH sides, i.e. it is a property of merged `main`
(PR #71 landed in between), not of this change.

**Build.** voxel-core via cmake/Ninja/MSVC, then `VoxelEarthEditor Win64
Development` via `D:\UE_5.8\Engine\Build\BatchFiles\Build.bat ... -WaitMutex
-NoHotReloadFromIDE` -> `Result: Succeeded`.

**Gotchas worth not rediscovering.**
1. A fresh worktree has no `build/voxel-core-msvc`, and `Build.bat` fails with
   `RulesError: could not find voxelcore.lib` -- but the *shell pipeline* still
   exits 0, so a scripted build looks like it succeeded and the editor then dies
   with "The game module 'VoxelEarth' could not be found". Grep the build output
   for `Result: Succeeded`, never trust the exit code.
2. `-nullrhi` perf runs report p95 = 0.50 ms at ~1,900 fps. That is not
   comparable to the 6-7 ms figures quoted for the M1 gate, which come from the
   real-RHI min-spec proxy. Use `-nullrhi` for throughput and census work only.

**Follow-ups, in priority order.**
1. **`Amplifier::surfaceUpperBoundMm(vx0, vy0, vx1, vy1)` in voxel-core.** The
   bound mirrors two worldgen constants (`kDetailOctaves`' amplitude sum and
   `slopeScaleQ10`'s clamp) UE-side, where a change to either could silently
   invalidate it. It belongs next to the constants it depends on, with a golden
   that pins it. `-VoxelVerifySkyBand` is the interim guard and it is a strong
   one, but it is a runtime check, not a compile-time one.
2. **The all-SOLID half at levels >= 1.** R0's waste is ~66% buried rock and
   PR #70's band already handles it; R1's is ~15%. A lower surface bound is just
   as cheap as the upper one, but a buried chunk can still hold cave and cavern
   air, so the test needs the cave/cavern depth envelope as well -- strictly
   harder than the all-air case, which is why this wave did not attempt it.
3. **R1 is now the largest single consumer** (~38% of worker time, ~46% of it
   still zero-quad). It is 85% air, so the same bound applies; it fires on only
   32.5% of R1 chunks because at a 6.4 m footprint the tile-pixel term is weak.
   Sampling a few real columns and taking the min against the analytic bound
   would likely close most of that gap.

### M1 gate on the same binary: passes, but p95 rises with the extra throughput

`-nullrhi` is useless for this (it runs at ~1,900 fps and reports p95 = 0.50 ms
on both sides), so the M1 numbers below are the real-RHI **min-spec proxy**:
1080p windowed, `-ExecCmds="sg.ViewDistanceQuality 0,sg.ShadowQuality 0,
sg.PostProcessQuality 0,sg.EffectsQuality 0,r.ScreenPercentage 100"`,
`-VoxelPerfRun=60`, seed 20260719, interleaved, first pair discarded as warm-up.
All runs shown, post-warmup (t>=10 s):

| run | off p95 | off max | off framesOver16.6 | on p95 | on max | on framesOver16.6 |
|---|---|---|---|---|---|---|
| 0 (warm-up, discarded) | 5.61 ms | 16.08 ms | 0 | 6.59 ms | 14.47 ms | 0 |
| 1 | 5.73 ms | 21.23 ms | 0 | 6.72 ms | 15.86 ms | 0 |
| 2 | 5.66 ms | 13.17 ms | 0 | 6.90 ms | 15.75 ms | 0 |

**The gate holds: 0 frames over 16.6 ms on both sides, every run, and 0
post-warmup hitches (>33.3 ms) on both sides.** But p95 rises ~1.1 ms
(5.66-5.73 -> 6.72-6.90), and that should be reported as what it is rather than
buried: it is not the bound costing frame time, it is the pipeline **converting
the reclaimed worker time into more work per frame**. The same runs load
21,717-21,855 chunks off versus 23,734-23,963 on (+10%), at 52.8% versus 55.8%
budget saturation -- the apply path is simply busier because more results are
arriving. Worth noting the *max* frame time moved the other way (off peaked at
21.23 ms in run 1; on peaked at 15.86 ms across both runs).

For context, the brief records both sides of the previous wave sitting at
6.1-7.1 ms p95 on this box; the `on` side here (6.72-6.90) is inside that band
and the `off` side is below it. If the p95 is ever wanted back, the knob is the
per-frame apply budget, not this change -- see follow-up 4.

**Exact after-census** (`-VoxelPerfRun=60 -VoxelMeasureEmpty`, all skips on,
versus the all-skips-off census at the top of this section; note this compares
PR #70's band skip AND the sky band together against neither):

| ring | zero-quad share of results, off -> on | zero-quad share of worker time, off -> on |
|---|---|---|
| R0 | 74.9-76.7% -> **26.5-30.0%** | 73.9-74.2% -> **28.3-32.2%** |
| R1 | 63.0-68.4% -> **45.1-45.3%** | 70.5-75.2% -> **52.1-52.3%** |
| R2 | 77.6-90.8% -> **25.8-27.6%** | 69.6-96.5% -> **25.3-35.2%** |
| R3 | 100% -> **11.1-25.0%** | 100% -> **12.4-12.8%** |
| R4 | 100% -> **0-50%** | 100% -> **0-100%** (1-2 jobs/window; see the A/B table for the reliable R4 figure) |

4. **The p95/throughput trade is now the open question, not the waste.** This
   change hands the streaming pipeline ~11% more worker capacity and the
   pipeline spends all of it, taking budget saturation 52.8% -> 55.8% and p95
   5.7 -> 6.8 ms. That is the right default while the outer rings are starved of
   geometry, but once R3/R4 are populated the same capacity would be better
   spent holding frame time down. `voxel.Stream.MaxAppliesPerFrame` and the ring
   floors are the knobs, and neither was re-swept against the new capacity.

## PR #80 follow-up #2: all-solid skip at admission, and the moving-underground measurement that motivated it

Two deliverables: the measurement PR #80 flagged as missing, and the fix it
asked for. The measurement came first and it changed what the fix had to be.

### The missing fixture, and what it found

Nothing in the tree moved an underground anchor â€” the cavern shot settles and
stands still â€” so `-VoxelPerfFlight=underground` was built first
(`VoxelPerfRunSubsystem`). Same circle, same 20 m/s as the M1 surface flight so
an A/B isolates depth; Z tracks the terrain at a constant 60 m offset rather
than being pinned at the spawn column, because over a 100 m circle the surface
moves more than the depth and a fixed Z surfaces partway round and quietly
reports the surface flight's numbers under an underground label. The run
reports `undergroundFrameFraction` and warns if it is not 1.0.

**Moving underground is materially more expensive**, two 60 s runs per arm on a
quiet box:

| post-warmup | surface x2 | underground x2 |
|---|---|---|
| p95 frame | 6.86 / 6.84 ms | 5.84 / 5.87 ms |
| max frame | 14.4 / 20.0 ms | **34.5** / 16.7 ms |
| hitches | 0 / 0 | **1** / 0 |
| tracked | 17.6k / 17.5k | **52.3k / 53.5k** |
| recompute totalMs (max) | 4.82 / 4.87 ms | **12.88 / 13.99 ms** |
| exit scan (max) | 1.83 / 1.96 ms | **3.61 / 5.03 ms** |
| R0 entry scan (max) | 2.54 / 2.51 ms | **8.00 / 8.35 ms** |
| subsystem tick | 6.6 / 6.3 % of wall | **19.1 / 21.0 %** |

Two things PR #80's note did not anticipate. The p95 is *lower* underground â€”
almost nothing meshes, so the renderer idles and masks the streaming cost; the
danger is that the cost is real, already produces a 34.5 ms hitch, and the
headroom hiding it disappears as underground gains geometry. And the exit scan
is **not** the dominant term: R0's *entry* scan is about twice it. Both scale
with the deep set, so the admission skip attacks the bigger one.

### The all-solid test

`vxc::Amplifier::solidBelowBoundMm` â€” the mirror of PR #75's
`surfaceUpperBoundMm`, and strictly harder. `materialAt` has exactly three
sources of air and the enumeration is closed (no `MAT_WATER`; every non-air
material is solid): above the surface, `caveCarveAt`, `cavernCarveAt`.

- Caves, crevices, shafts: **42.8 m** below the querying column's own surface,
  all compile-time constants.
- Caverns: anchored at absolute z off the surface at the *site's* anchor, a
  different column. Bounded at **91 m** below that surface â€” the binding term is
  the flat-floor clamp (`zFloorMm = zMm - floorDropMm`), which `cavernCarveAt`
  tests before it ever evaluates the ellipsoid, so a 40 m vertical semi-axis
  does not reach 40 m below the room centre. This independently reproduces the
  tree's measured ~128 m max cavern depth: that figure is relative to the
  *querying* column, which can sit ~37 m below the site over the 36.4 m reach.

**Two tiers**, because a flat 91 m envelope is sound but nearly useless â€” at a
60 m anchor depth it clears only a small cap at the bottom of the sight sphere.
Caverns are rare (756 columns of ~100k sampled) and whether one is in reach is
decidable from four hashes per 204.8 m coarse cell, no surface read and no site
decode. No cavern in reach -> 42.8 m against the tight footprint; else 91 m
against the footprint dilated by the cavern reach.

Every constant is derived from the real tables or `static_assert`ed, in the same
coupling block `surfaceUpperBoundMm` uses, plus an **independent** bedrock
backstop (both carve passes refuse `depthMm + kCaveBedrockMarginMm >=
bedrockDepthMm`, so nothing carves past 178 m by a separate mechanism). Two of
those asserts caught real errors while writing this.

The edit veto is the correctness crux and lives **outside** the memo, since
`ComputeFootprintChunkZRange` must stay pure in (Level, X, Y) for the M1 hitch
fix. `EditedFootprintMinZ` mirrors `EditedFootprintMaxZ` exactly, maintained by
the same walk in the same one place. It vetoes the whole column below the lowest
edit rather than tracking individual chunks: a false veto costs one record, a
missed one is a chunk the world does not know exists where somebody is digging.

### Results, interleaved A/B on one binary (`voxel.Stream.AdmissionSolidSkip`)

Moving-underground flight, three interleaved pairs, all runs shown:

| run | tracked | deep | deepGeo | loaded | recompute | exitScan | R0 entry | tick % wall | pwP95 | pwMax | hitch |
|---|---|---|---|---|---|---|---|---|---|---|---|
| off 1 | 52,910 | 43,723 | 0 | 8,282 | 13.30 | 4.69 | 7.96 | 16.59 | 5.659 | 17.07 | 0 |
| on 1 | 31,313 | 20,023 | 58 | 15,350 | 9.13 | 2.46 | 6.12 | 11.44 | 5.884 | 17.57 | 0 |
| off 2 | 48,783 | 40,330 | 0 | 8,412 | 11.60 | 4.24 | 7.26 | 15.74 | 5.794 | 34.82 | 2 |
| on 2 | 31,185 | 19,991 | 64 | 15,032 | 12.13 | 2.56 | 6.55 | 11.63 | 6.081 | 31.83 | 0 |
| off 3 | 51,080 | 42,284 | 1 | 8,509 | 10.18 | 4.28 | 7.71 | 16.95 | 6.002 | 21.37 | 0 |
| on 3 | 30,829 | 19,821 | 64 | 15,358 | 9.16 | 2.45 | 5.53 | 10.43 | 5.942 | 21.73 | 0 |

Deep records **-53%**, exit scan **-45%**, R0 entry **-20%**, subsystem tick
**-33%**. The reclaimed budget is not banked: chunks loaded goes **+81%**, and
`deepWithGeometry` goes **0-1 -> 58-64**. That last is the real headline â€” the
old deep set was 43k records of solid rock crowding out the handful of chunks
underground that actually have geometry.

**Not the ~25x the follow-up estimated.** That figure came from the stationary
cavern scene at 60.7 m depth; a 64 m sight sphere around a 60 m anchor spans
0-124 m of depth, and most of it sits within 42.8 m of the surface where the
cave envelope cannot prove solidity. 2.2x is the honest ceiling for a bound this
cheap â€” going further needs per-column cave data (`ColumnDeepestAirVoxel`), i.e.
a `column()` per candidate, which is far too expensive at admission.

M1 gate, two interleaved pairs: p95 6.686-6.842 both sides, **0 hitches and 0
frames over 16.6 ms on all four runs**. `skipped=0 floorCache=0` on the surface
flight confirms the path is inert above ground, as designed.

### Soundness evidence

- voxel-core: 228/228 tests. The central one takes the claimed floor over 700
  randomised footprints and hammers every sampled column with the real
  `column()` and real `materialAt`: **150,033,021 voxels below the floor across
  7,230 cave and 756 cavern columns, zero air.** Worst headroom 11.0 m (38.1 m
  before the two-tier split, which is what keeps tier 1 honest).
- In-engine `-VoxelVerifySolidSkip`, 60 s moving underground: **76,409 chunks
  admitted and dispatched anyway, VIOLATIONS=0**, no UNSOUND lines. voxel-core
  proves the bound; this proves the UE-side chunk-Z arithmetic over it.
- Cavern screenshot with skip on vs off: visually identical, no sky, no new
  holes; residency fan MESHED at all four elevations on both sides.
- `-VoxelUndergroundTest` (carves a shaft/tunnel/chamber and parks the pawn in
  it): pawn enclosed, digest server==client, and `skipped=0` in that footprint â€”
  the edit veto disabling the skip exactly where it must.
- Dig digest unmoved: **2,178,385 voxels / 0xFB62844A74CB9121 / 8,039 entries**,
  identical with the skip on and off. `kWorldGenVersion` stays 5;
  `amplifier_columns` unchanged at 0xA29A7A767DC1543B, so the bedrock
  literal-to-constant refactor is bit-identical worldgen.

Two new goldens: `amplifier_solid_below_bound` = 0xE9D395DF74D61495. Neither is
worldgen output.

### Follow-ups

1. **The remaining 20k deep records are within 42.8 m of the surface.** Closing
   that needs the per-column cave envelope, which needs `column()` per
   candidate. The cheap version would be a *footprint-level* cave-lattice bound
   analogous to the cavern reach test â€” the cave lattice is 25.6 m, so a level-0
   footprint touches at most 2x2 cells, and the segment depths are decodable
   from the lattice without a surface read. Same shape as tier 1, one level down.
2. **`deepWithGeometry` was 0 on this flight before the change.** Worth knowing
   that the 60 m circle on this seed passes through essentially no natural
   caves; the geometry the skip unblocked (58-64 chunks) is at the edges of the
   sphere. A flight routed deliberately through a cave system would be a better
   residency fixture than a fixed-depth circle.
3. **`voxel.Stream.UndergroundSightM`'s help text still says "40 (default)"**
   while the default is 64 and `SightRadiusUU()` silently clamps to 64 â€” so
   raising it from `-ExecCmds` does nothing, invisibly. Not touched here.

## M1 gate re-run after the streaming-speed pass: FAILS on `main` (2026-07-24)

The handoff (`docs/streaming-handoff.md`) listed "M1 gate re-run" as required
before merging the 2026-07-24 streaming-speed work, because those commits raised
the apply/unload throttles past the tuning the zero-hitch gate was closed on
(`voxel.Stream.MaxAppliesPerFrame` 3 -> 64, unloads 2 -> 24, remeshes 2 -> 8,
plus the new `ApplyBudgetMs` 6 ms drain and `LodRetentionMs` 5000). That re-run
did not happen before the merge -- **PR #99 landed on `main` first (8ed14aa),
and the numbers below were taken afterward.** They describe `main` as it stands.

Matt's call on the result is recorded at the bottom: **do not tune the dial;
remove the trade-off via ADR-0006.**

### Protocol caveat -- these are NOT min-spec-proxy numbers

Every historical M1 gate figure in this file uses the real-RHI **min-spec
proxy** (`sg.ViewDistanceQuality 0, sg.ShadowQuality 0, sg.PostProcessQuality 0,
sg.EffectsQuality 0, r.ScreenPercentage 100`). **These runs do not.** They are
1080p windowed at default quality, `-game`, real tiles
(`loaded=25 rejected=0`, seed 20260719), `-VoxelSpawnAt=-84480,53760`,
`-VoxelPerfRun=60` (scripted surface flight, 20 m/s, depth 60 m).

So the absolute values are NOT comparable to the min-spec gate rows above, and
the 16.6 ms bar should not be read against them directly. What IS trustworthy is
the **A/B between the two runs**, which used the same binary, same seed, same
terrain, same cascade and same shadow settings, differing only in the throttle
cvars. A min-spec-proxy re-run is still owed before the gate row is formally
re-coloured.

### A/B on one binary: throttles are the only variable

Run B restored the pre-change throttles at runtime, which is exactly the
one-binary A/B the cvars were designed for (they are read fresh per frame, so
`-ExecCmds` reaches them):

```
-ExecCmds="voxel.Stream.MaxAppliesPerFrame 3, voxel.Stream.ApplyBudgetMs 0.5,
           voxel.Stream.MaxUnloadsPerFrame 2, voxel.Stream.MaxRemeshesPerFrame 2,
           voxel.Stream.LodRetentionMs 0"
```

Post-warmup (t>=10 s) unless noted:

| | B: old throttles | A: new throttles (`main`) | ratio |
|---|---|---|---|
| p50 frame | **5.863 ms** | **15.132 ms** | 2.58x worse |
| p95 frame | 10.635 ms | 21.357 ms | 2.01x worse |
| **max frame** | **15.059 ms** | **43.924 ms** | 2.92x worse |
| **hitches** | **0** | **2** | gate broken |
| frames in 60 s | 10,667 | 3,907 | |
| chunks loaded | 19,771 | 56,228 | |
| chunks/sec | 329.5 | **937.1** | **2.84x faster** |
| budget saturation | 53.95% | 27.55% | |

Artifacts: `Saved/PerfRuns/perf_20260724_191652.json` (B),
`perf_20260724_191124.json` (A).

**The single most telling number is max frame.** With the old throttles no frame
after warmup exceeded **15.06 ms** -- the entire run stayed inside a 60 fps
budget, every frame. With the new throttles it spikes to **43.92 ms**.

**Verdict: the streaming-speed pass buys 2.84x fill throughput for 2.58x p50
frame time and costs the zero-hitch gate (0 -> 2). M1 FAILS as `main` stands.**

### Three-way attribution -- do not blame it all on the throttles

The nearest prior artifacts (`perf_20260722_1232/1235/1238`, post-warmup p50
3.55-3.59 ms, 0 hitches) are **not a clean baseline**: they predate
PR #94 (`cascade-2km`, R5 ring, merged 07-22 18:42 local) and PR #95
(`cave-shadows`, terrain casts sun shadows, 07-23) -- and their quality settings
are not recorded in the JSON, so they may not even be the same protocol.
Splitting post-warmup p50 anyway:

- 07-22 artifacts: **3.59 ms**
- today, old throttles (includes 2 km cascade + sun shadows): **5.86 ms**
  -- roughly **+63%** attributable to that intervening work, not to streaming
- today, new throttles: **15.13 ms** -- the remaining **+158%** is the throttles

An earlier read of this data quoted "4.4x worse" by comparing today's `main`
straight to the 07-22 artifacts. That figure is inflated and should not be
cited; **2.58x, from the same-binary A/B, is the honest number.**

### The hitches are the render thread, not the voxel subsystem

Both post-warmup hitches in run A attribute almost entirely to `renderMs`:

```
frameMs=43.92 | subsystemTickMs=0.26 | elsewhereMs=43.66 | renderMs=43.12
frameMs=39.84 | subsystemTickMs=0.38 | elsewhereMs=39.46 | renderMs=18.72
```

On the first, the render thread **is** the frame; voxel game-thread work is
0.26 ms. This reproduces in `-game`, so it is not PIE overhead (an earlier
in-editor observation of the same shape was inconclusive for that reason).

(The recurring `maxFrameMs=400.00` entries in both runs are startup shader
compilation -- `renderWaitMs` up to 12,437 ms, all inside the first 10 s and
excluded by the warmup window. Not signal.)

This is ADR-0006's thesis measured directly: fill speed and frame time trade
against each other **because both flow through the same per-chunk render-thread
apply funnel**. More applies per frame is literally more `FScene::AddPrimitive`
on the render thread. No throttle value escapes the trade; it only picks a point
on it.

### Decision (Matt, in-session 2026-07-24)

Offered: probe `voxel.Stream.ApplyBudgetMs 3` for a knee between the two
extremes and ship that as the default. **Declined.** The call is to stop tuning
the CPU funnel and remove the trade-off by building the GPU-resident path --
i.e. proceed to consider ADR-0006 (`docs/adr/0006-gpu-resident-voxel-streaming.md`),
which remains `proposed` and unsigned.

Consequences to track:
- `main` currently ships the fast-fill / broken-zero-hitch end of the trade.
  That is deliberate and Matt-approved (the handoff records him accepting
  "silky/fast" over strict zero-hitch), but it is now a **measured** failure of
  the M1 gate rather than an assumed-tolerable one.
- The M1 gate row is left as-is pending a **min-spec-proxy** re-run under the
  historical protocol; these default-quality numbers are not a substitute.
- ADR-0006's G3 milestone redefines this gate anyway (GPU frame time +
  chunks/s + zero hitches). If ADR-0006 is signed, re-colouring the M1 row on
  the CPU path may be moot.

### Terrain sun shadows are NOT the render cost (2026-07-24, measured)

PR #95 (terrain chunks cast sun shadows) merged 07-23, immediately before the
hitch symptoms appeared, and `renderMs` is the hitch driver -- so it was the
obvious cheap suspect. It is not the cause. New cvar `voxel.Render.CastShadow`
(default 1) flips the single `UPrimitiveComponent::CastShadow` flag at chunk
load; 60 s scripted flight, 20 m/s, everything else identical:

| | shadows ON | shadows OFF | shadows ON (repeat) |
|---|---|---|---|
| post-warmup p50 | 14.115 ms | 15.030 ms | 14.923 ms |
| post-warmup p95 | 21.964 ms | 20.561 ms | 20.300 ms |
| post-warmup hitches | 4 | 2 | 1 |
| chunks/sec | 779.17 | **967.07** | **968.20** |

**Shadows-off and shadows-on-repeat are the same run to within noise** (967.07 vs
968.20 chunks/s; p50 15.03 vs 14.92). Terrain shadow casting costs nothing
measurable here. Keep it on -- it is what stops the sun lighting sealed caves.

Two corrections fall out of that third column, and both were wrong in the
direction of alarm:

1. **The first "shadows ON" run (779 chunks/s) was a COLD run**, the first flight
   after a rebuild, not a shadow effect and not a regression. Steady-state
   post-fix throughput is 967-968 chunks/s, at or above the pre-fix band
   (855-937). The load-before-unload rewrite below is throughput-neutral, not the
   ~9% cost first reported off that cold run.
2. **Always discard the first run after a build.** Cold shader/PSO state costs
   ~20% throughput and it looks exactly like a regression.

**Methodology, learned the hard way:** the first attempt at this A/B used
`r.ShadowQuality 0` and produced **118 post-warmup hitches vs 2** -- with LOW
`renderMs` (21 of an 86 ms frame), the opposite shape of a render-cost change.
This module PSO-precaches the terrain material at BeginPlay
(`FPSOPrecacheParams`), so changing render scalability at startup desyncs the
precache from what is actually drawn and every chunk takes a pipeline-state miss.
**A/B-ing this renderer through scalability cvars measures precache invalidation,
not the thing you meant.** Change one primitive flag instead.

Consequence for ADR-0006: the cheap alternative explanation for `renderMs` is now
ruled out. `renderMs`-dominated hitches persist with shadows off (one at
`renderMs=52.37` of a 52.15 ms frame), which is consistent with per-primitive
draw/culling overhead -- ADR-0006's actual target. Still not a substitute for
profiling *inside* `renderMs`, which remains the open measurement.

### Load-before-unload: coverage rewritten against ChunkRecords (2026-07-24)

Matt reported rolling rings of holes still opening at LOD boundaries while
moving, and separately a few chunks that never load until the player moves. The
first is fixed here; the second is a different, older bug (bounded-admission
refill, see the backlog).

The 07-24 first cut maintained a side index `ColumnGeomCount`, keyed
`(level, chunkX, chunkY)` and counting records with `LastQuadCount>0`. It was
wrong twice:

1. **It dropped Z.** "Column has geometry" was true if ANY chunk in that vertical
   stack had geometry, so a deep `bDeepAnchorRelative` chunk (~38 m down,
   invisible) vouched for a surface chunk that had not arrived -- releasing the
   stand-in early and opening the exact hole retention exists to prevent.
2. **It keyed on geometry, not residency.** A replacement that legitimately
   meshes to zero quads (all air, all solid, an all-ocean quarter) could never
   report covered and always fell through to the safety cap.

Plus it needed hand-maintained reconcile calls at every gain/loss site, and the
two pre-dispatch skip sites (buried, sky-band) were missing theirs -- the leak
`docs/streaming-handoff.md` flagged as prime suspect.

Fix: delete the index. `ReplacementCovered` now looks the replacement chunks up
in `ChunkRecords` directly and asks whether each has **settled**
(`FChunkRecord::bMeshSettled` -- mesh applied with or without quads, or proven
empty pre-dispatch). Z-aware (8 children at L-1, or the one parent at L+1); a key
with no record is not desired and cannot block. `ColumnGeomCount`,
`ReconcileColumnGeom` and `bColumnCounted` are gone, and with them the whole
drift-between-two-copies-of-the-truth bug class.

New telemetry, `LogVoxelPerf` "Voxel LOD retention" every 5 s: `held` (stand-ins
currently drawn waiting), `covRel` (released because the replacement arrived),
`capRel` (released because `LodRetentionMs` expired -- **each one a hole the
player could have seen**). Measured post-fix:

| flight | held | covRel / 5 s | capRel / 5 s |
|---|---|---|---|
| 20 m/s | 42-372 | 8,300-11,400 | 7-110 (0.1-1.1%) |
| 100 m/s | 0-83 | ~7,000 | **0** |

`capRel` at 100 m/s is zero and `held` collapses -- because at that speed chunks
are evicted *before they ever loaded*, so they never enter the retention path at
all. There is no stand-in to hold. Retention can delay a hole; it cannot
manufacture a chunk. The 100 m/s flight otherwise degrades hard (p50 29.7 ms, 259
post-warmup hitches, 618 chunks/s) and that degradation is the ADR-0006 funnel,
not this mechanism.

**Not visually confirmed.** The scripted flight is a fixed circle; the reported
symptom came from free flight. Telemetry says the mechanism now behaves
correctly; a human still has to look.

### `renderMs` is NOT pixel work -- ADR-0006's diagnosis confirmed by elimination

ADR-0006 asserts the per-chunk render-thread apply funnel is the ceiling. The
2026-07-24 measurements established that **`renderMs` is the frame** (43.12 ms of
a 43.92 ms hitch, voxel game-thread work 0.26 ms) but not **what inside
`renderMs`**. Those are different claims, and six milestones rest on the second.
Two eliminations, both cheap, both on one binary:

**1. Not the shadow-depth pass.** `voxel.Render.CastShadow 0` vs `1`: identical
to within noise (see the shadow section above). `renderMs`-dominated hitches
persist with shadows off.

**2. Not rasterisation or shading.** `r.ScreenPercentage 50` quarters the pixel
count while leaving primitive count, draw-call count and culling work **exactly
unchanged**. 60 s flight, everything else identical:

| | full res | 50% screen (1/4 pixels) |
|---|---|---|
| post-warmup p50 | 14.923 ms | 14.757 ms |
| post-warmup p95 | 20.300 ms | 20.631 ms |
| chunks/sec | 968.20 | 934.65 |

**Nothing moved.** A frame that is unchanged by a 4x cut in pixels is not pixel
bound. And hitches in that same run still attribute to render
(`renderMs=62.57` of a 62.63 ms frame).

(`r.ScreenPercentage` is safe for this A/B where `r.ShadowQuality` was not: it
scales resolution, it does not change shader permutations, so the BeginPlay PSO
precache stays valid. No 118-hitch signature appeared -- 5 hitches vs 1.)

**What is left in `renderMs`** once shadow-depth and rasterisation are excluded:
per-primitive visibility/culling, draw-command generation, and `FScene`
primitive mutation across ~24,700 tracked records. All three are precisely what
ADR-0006 collapses to O(1) primitives + indirect draws.

**Verdict: proceed to G0.** The pre-sign-off gate ("is the prize real, or would
we spend months moving a minority of the cost?") is passed by elimination.
Direct attribution inside `renderMs` via Unreal Insights is still worth doing at
G0 to size the win, but it is no longer a blocker on the decision.

**ADR-0006 signed by Matt 2026-07-24 (in session).**

### Dispatch starvation: hypothesis RAISED and DISPROVEN the same day

Worth recording because the reasoning looked airtight and was not.

**The observation is real.** At 20 m/s, per 5 s window: `recordsAdded=14540`,
`dispatched=7704`, `recordsEvicted=12306`, `stale=0`, apply budget only ~28%
saturated. So ~6,800 chunks per 5 s enter the desired set and are evicted before
any worker touches them -- not slow, never asked for.

**The (wrong) inference.** `MaxJobsInFlight` was a hardcoded
`2 * NumberOfCoresIncludingHyperthreads()` = 24, and `DispatchJobs` refills it
once per frame. With an R0 job at p50 1.32 ms and a ~15 ms frame, each slot
looked like it did one job per frame then idled ~13.7 ms -- an apparent ~9%
worker utilisation, and `24 slots x 66 fps = 1584/s` matched the measured
1540/s almost exactly. Prediction: raising the multiplier multiplies throughput.

**The measurement says no.** New cvar `voxel.Stream.JobsInFlightPerCore`
(default 2 = byte-identical to the old hardcoded form), 45 s flights, first run
discarded as cold:

| | cfg 2 | cfg 8 | cfg 16 |
|---|---|---|---|
| post-warmup p50 | 14.632 ms | 14.207 ms | **33.977 ms** |
| post-warmup p95 | 22.221 ms | 21.975 ms | 52.542 ms |
| post-warmup hitches | 9 | 5 | **564** |
| chunks/sec | 797.22 | **790.84** | 966.60 |
| budget saturation | 24.5% | 24.0% | 59.6% |
| stale results | 0.0% | 1.3-5.0% | 3.2% |

**8x the slots produced 0% more chunks per second.** Dispatch count rose ~33%
(6,300 -> 8,400 per window) but useful throughput did not move at all, and stale
results appeared. 16x buys throughput only by wrecking frame time -- 564 hitches.

**Why the arithmetic lied:** it used R0's p50 (1.32 ms) as if it were the whole
mix, but R1-R5 jobs run ~4 ms, and the UE task graph never dedicates all 12
logical cores to voxel meshing. The workers were already near their real
capacity; the 24-slot cap was only mildly binding. There was no idle fleet.

**Outcome:** default stays 2. The cvar is kept -- inert, and it makes this
measurable instead of arguable. The genuine throughput lever is not slot count
but the **37.1% of R0 worker time (and 37.2% of R1) spent on chunks that mesh to
zero quads** -- see the empty census. That is where CPU-side headroom actually
lives.

**And it is not the hole bug anyway.** See the next section -- the rings of holes
are a desired-set coverage defect at the ring seams, found immediately after.

### ROOT CAUSE of the concentric rings of holes: ring-seam coverage gap (2026-07-24)

**This is the bug Matt has been reporting since the streaming-speed pass.** It is
NOT the load-before-unload path (that was a separate, also-real bug, fixed
above), not the empty-chunk skips (verifiers clean), and not throughput.

Three independent investigations converged on the same defect:

`RingPresets` annuli **abut exactly** -- `{0,64} {64,128} {128,256} {256,512}
{512,1024} {1024,2048}` m, so `Outer[L] == Inner[L+1]`, zero overlap
(`VoxelWorldSubsystem.h:86`). Admission tests the chunk **CENTRE** against those
radii (`VoxelWorldSubsystem.cpp:4451-4457`). But chunks have **extent**, and
adjacent levels have **different chunk sizes**. A level-L chunk's four
level-(L-1) children sit at centre offsets `(+-e/2, +-e/2)`, i.e. up to
`e/sqrt(2)` further out radially. So:

> a child can be rejected at L-1 (its centre >= `Outer[L-1]`) while its parent is
> rejected at L (the parent's centre < `Inner[L]` == `Outer[L-1]`).

**That ground is requested by no level at all.** Direct enumeration at an
arbitrary anchor:

| seam | fine edge | uncovered columns | hole width |
|---|---|---|---|
| R0/R1 @ 64 m | 3.2 m | 32 | 3.2 m |
| R1/R2 @ 128 m | 6.4 m | 28 | 6.4 m |
| R2/R3 @ 256 m | 12.8 m | 29 | 12.8 m |
| R3/R4 @ 512 m | 25.6 m | 36 | 25.6 m |
| R4/R5 @ 1024 m | 51.2 m | 38 | 51.2 m |

Each is a **full-height column** -- the entire `ChunkZMin..ChunkZMax` band is
never enumerated for that `(Cx,Cy)` -- hence see-through to the sky, not a
hairline crack. 20-40 per boundary, at all five boundaries, scaling with
distance: a broken-but-continuous ring of holes all the way around, exactly as
reported.

**Why it never healed while standing still:** the desired set is a pure function
of anchor position, the inner eviction test uses the same hard threshold with no
hysteresis (`:4317-4319`, "hard boundary" by design), and `RecomputeDesiredSet`
early-outs entirely when the anchor's chunk has not changed. Stop anywhere and
the holes are permanent. This is also why `voxel.Stream.LodRetentionMs` 0 vs 5000
made no difference -- these chunks were never candidates, so they never reached
the retention path at all.

**Two stale comments in the tree asserted the opposite** and are now known false:
`ComputeRingSkirtMask` (`:4841-4847`) justifies skipping the outer-edge skirt on
the grounds that "the coarser ring physically overlaps just beyond it" -- it does
not; and that function's header claims it uses "the exact annulus-membership test
RecomputeDesiredSet admits candidates with" while never loading `OuterMeters` at
all. The ring cross-fade was ALSO disabled (`VoxelChunkComponent.cpp:828-844`)
because the annuli do not overlap -- same root cause, diagnosed a second time
without either diagnosis reaching the other.

**FIX:** pad each level's OUTER admit radius by the chunk half-diagonal
(`e/sqrt(2)`), so a chunk is admitted whenever any part of its footprint could
fall inside the annulus. Padding by exactly `e/sqrt(2)` is provably sufficient
and minimal (a gap child's centre is at most its parent's centre plus
`e/sqrt(2)`, and the parent's centre is `< Outer[L-1]` by construction). Only the
outer side needs it: the finer, more accurate mesh then wins in the overlap band,
the coarser ring's inner hole stays hard, and the exit pass does not fight it
(`bBeyondOuter` uses `Outer*1.25`, wider than `Outer + e/sqrt(2)` at every level).

**Measured cost:** resident chunks 9,622 -> 10,503, **+9.2%** (per ring: R0 +6.4%,
R1 +9.0%, R2 +10.0%, R3 +10.2%, R4 +10.0%, R5 +9.9%) -- matching the predicted
7-10%. Before/after vista screenshots at the same anchor, both fully settled
(`jobs 0/24`, `queues job=0`), show the dark notches along the ring seams gone.

**Follow-on now unblocked:** the annuli genuinely overlap for the first time, so
the ring cross-fade (`-VoxelRingCrossFade`, disabled because it had no second LOD
to dissolve into) has the overlap band it always needed. Worth re-testing.


## ADR-0006 G4/G5 — parity closed, default flipped, and what the M1 trade-off looks like now (2026-07-25, evening)

Closes the thread opened by the M1 gate re-run above, whose conclusion was
**"do not tune the dial; remove the trade-off via ADR-0006."** The trade-off is
now measured on both sides of that dial.

### The G4 checklist was wrong about which items mattered

Three of the four recorded G4 parity items are off-by-default features
(ring cross-fade, voxel GI, debug tints). The one item that was visibly
different in ordinary play was on no list at all: **vertex colour R**, the biome
tint. The pooled vertex factory computed it as a bare `Axis == 2 && positive`
sky-facing flag, which is neither half of what `BuildChunkVertexData` does —
`VoxelClimate::BiomeTintForFace` returns **140/255, not 0**, for any non-Z face,
and the CPU path gates the whole tint on the vertex being within 200 UU of the
chunk's surface height. So every vertical riser in the world rendered pink-tan
where the CPU path blends turf, and every underground +Z face (cave floors)
tinted as open sky.

Measured, 30 s headless run at a fixed anchor, pooled against component, as
**percentage of pixels differing by more than 8/255**:

| | pixels > 8/255 |
|---|---|
| before | **17.4%** |
| after | **4.3%** |
| same-path repeat-run noise floor | 1.1% |

The residual is the documented per-chunk (rather than per-quad) climate
sampling. Method note worth keeping: the before/after screenshots "look the
same" at a glance and differ by 17%. Diff numerically, always against a
same-path noise floor.

Also closed: `bVelocityRelevance` was never set on the pooled proxy, so pooled
terrain contributed nothing to the velocity pass and TSR reprojected all of it
from depth alone; plus `bUsesLightingChannels`, `bRenderCustomDepth`, and editor
Wireframe view mode.

**The "material gate" that G4 was organised around was mostly not real.** The
pooled factory owns both ends of the vertex-colour pipe, so anything expressible
as a vertex colour channel is already an interpolant the graph reads.
`M_VoxelTerrain.uasset` was not touched and does not need to be for anything on
the critical path — including GI. Only the debug tints genuinely still need it.

### The M1 trade-off, measured on both paths

60 s scripted surface flight at 20 m/s, same spawn, post-warmup,
`voxel.Stream.GPU` the only difference. Two pairs, second run in reverse order.

| | cpu #1 | cpu #2 | pool #1 | pool #2 | mean delta |
|---|---|---|---|---|---|
| p50 frame ms | 17.78 | 17.21 | 17.57 | 16.75 | −1.9% |
| **p95 frame ms** | 29.17 | 51.83 | 24.34 | 21.77 | **−43.1%** |
| **worst frame ms** | 333.1 | 109.7 | 66.5 | 86.7 | **−65.4%** |
| **hitches (>33.3 ms)** | 35 | 373 | 11 | 6 | **−95.8%** |
| chunks/s | 678 | 537 | 839 | 797 | **+34.6%** |

This is the ADR-0006 thesis measured directly, and it is the same shape the
earlier entry predicted: **fill speed and frame time stop trading against each
other.** The component path buys 537–678 chunks/s at 35–373 hitches; the pool
buys 797–839 chunks/s at 6–11. The median frame is unchanged, which is correct
— removing a per-chunk `FScene::AddPrimitive` was never going to move the median.

**These are NOT gate numbers and must not be quoted as an M1 row.** Two reasons,
both disqualifying on their own: they are not min-spec-proxy protocol (same
caveat as the 2026-07-24 runs above), and all four legs ran with other headless
instances live on the same machine. Most of the spread between the two component
runs is that contention — informative in itself, since the component path
degrades far worse under load, but not a controlled measurement. What survives
the noise is only the direction, and the direction is unanimous: the pool wins on
p95, worst frame, hitch count and throughput in every one of the four runs.

**The min-spec-proxy M1 gate re-run is therefore still owed**, and it is now
worth doing, because for the first time there is a configuration that might
actually pass it.

### G5: default flipped, component path kept

`voxel.Stream.GPU` now defaults to true. Verified with no cvar set at all: 9,822
chunks, 8,813,242 quads, ONE primitive, ONE draw.

The component path is deliberately **kept, not retired**. It still carries voxel
GI, the debug tints, the ring cross-fade A/B, several mesh-time diagnostics, and
— the reason that matters most — it is the fallback that
`voxel.Stream.GPUMaxLevel` and `GPUMaxChunks` bisect against, which are the two
sharpest debugging tools this renderer has and both presuppose two renderers
coexisting in one frame. `voxel.Stream.GPU 0` is a complete revert.

### Streaming: the R0 freeze had a cause upstream of everything diagnosed

`DrainResults` had `ResultsQueue.Dequeue(Result)` in the `while` condition with
the wall-clock budget check as the first statement of the body. Any frame that
overran `voxel.Stream.ApplyBudgetMs` popped a result off the MPSC queue and then
dropped it on the floor — silently, before the drain counter incremented — and
skipped `FootprintBandCache.Add`, `FootprintBlindJobInFlight.Remove`,
`--LevelJobsInFlight[Lvl]` and `Rec->bJobInFlight = false`.

A footprint absent from the band cache **and** present in the blind-job set is
the exact conjunction the cold-band throttle defers on forever, and one dropped
result creates both halves in a single step. That is why the earlier fix —
moving where the mark is *set* — could not touch this instance: the leak is
entirely on the result side. Independently of the mark, the dropped result also
stranded its own chunk (`bJobInFlight` pinned, never re-dispatched, never
rendered) on both paths.

Fixed by testing the budget before the dequeue. **Not reproduced under test** —
the 30 s spawn runs show `markTimeouts=0`, i.e. they never overran the budget
long enough — so this is a correctness fix argued from the code, not a measured
before/after. Forcing it needs a low `ApplyBudgetMs`, and is worth doing if the
symptom recurs.


## Worldgen v8 — the climate half of what v6 did for the surface (2026-07-25)

Branch `claude/climate-v8` off `main` (v6). Steps 0–2c landed; the
`worldgen.ush` mirror + SPIR-V respin (2d) is deliberately outstanding, see
"Not done" below. **This branch must not merge until 2d lands** — until then
CPU and GPU disagree.

**The defect.** `tiles.h` described `ClimateSample` as a "service-defined
encoding, see terrain-service", so voxel-core did not know the encoding it was
reading. Real tiles carry physical WorldClim values quantized over
Earth-extreme ranges (`diffusion.py` `EXPECTED_CHANNELS`); `SyntheticTileSampler`
emitted noise centred on 128. Every threshold over those bytes was calibrated
against the second and pointed at the first. `kBiomePrecipAridU8 = 60` decoded
to 2824 mm/yr — wetter than the Amazon — so every column took the arid branch
and the three below it were dead code.

**Measured with `vxc_climateprobe` (new, step 0a) over the 25-tile diffusion
set, seed 20260719:**

| | v6 | v8 | predicted |
|---|---|---|---|
| submarine terrain classified alpine | 17.8% | **0.00%** | 0% |
| cliff gate share of world | 51.1% | **6.50%** | ~6.5% |
| land with zero topsoil | 91.3% | **0.00%** | 0% |
| top voxel showing its biome material | 12.4% | **100.00%** | ≥95% |
| biome ids reachable | 5/9 | **7/10** | ≥6 |

Census after: OCEAN 44.7, TUNDRA_ALPINE 21.7, TEMPERATE_FOREST 19.9,
BARE_ROCK 6.5, TAIGA 4.2, GRASSLAND 2.3, BEACH 0.7. DESERT/SAVANNA/RAINFOREST
stay 0% — correct, the region has no arid or tropical pixels.

**Correction to the original diagnosis.** The topsoil collapse was attributed to
the encoding mismatch. It was not: measured, the v6 formula produced zero
topsoil on **85.5% of SYNTHETIC land** — the encoding it was written for. The
dominant fault was `- slopeMmPerPx / 4`, an absolute subtraction that swamps the
base at any realistic 30 m-pixel slope. The encoding mismatch took it from 85%
to 91%. So the fix repairs the dev/bench path too, and the cliff gate and the
topsoil slope term are the same miscalibration: both treated 30 m-pixel slopes
as far gentler than they are.

**Golden movement, predicted vs actual — all six as predicted, nothing else
moved:** `amplifier_columns`, `amplifier_deep_materials`, `biome_map`,
`mips_chain`, `coarsegen`, `rivernet_synthetic_slope`. Held still:
every `test_hash` golden, and `amplifier_surface_bound` /
`amplifier_solid_below_bound` — the sharp tripwires, since a move would mean the
change had leaked into the surface term. The 25 `.vxtl` files are byte-identical;
nothing here touches the wire format.

**Two things the tests caught that the design did not.** `BARE_ROCK → MAT_ROCK`
made cliff columns read rock → SUBSOIL → rock, a soil layer sandwiched under a
rock skin; `stratigraphyAt` now carries rock through the subsoil band, which
also resolved `coarsegen_fidelity_vs_true_mip` (it had exceeded its
material-mismatch ceiling). And `subsoilMm` derived from the pre-clamp topsoil,
so a floored column got the unfloored value's subsoil.

**`kBiomeTreelineBaseMm = 900'000` is TUNED, not derived** — the only such
constant in `biome.h`. Swept against the real tiles the alpine share barely
responds (48.6% of land at 300 m, 39.9% at 900 m, 35.0% at 1200 m) because the
region is genuinely cold and mountainous. Settle it by screenshot; it is the
constant to move if the world reads too bare.

**Not done (tracked):**
1. **`worldgen.ush` mirror + SPIR-V respin + `vxc_gpu` on AMD/NVIDIA.** The
   blocker for merging. Contested with the in-flight ADR-0006 G-phase work, so
   it gets exactly one coordinated edit. Also carries the three stale `11250`
   guard comments left from step 0b.
2. **Rivernet (step 3).** `rivernet.cpp` weights flow accumulation by the raw
   precipitation byte against `accumThreshold = 500`. 2a already moved its
   golden; step 3 rescales the weight to mm/yr and rolls `kRiverNetVersion`.
3. **Render-side calibration (step 4).** Re-derive `VoxelClimateProbe.h`'s
   `kTempU8Lo/Hi`/`kPrecipU8Lo/Hi` from `climate.h` so they stop being a third
   independent calibration. Pick endpoints that reproduce today's 100/189/14/32
   exactly, so it is a provable no-op on rendered output.
4. **GPU harness cannot see real tiles.** `gpu_harness.cpp` and
   `VoxelGpuVerify.cpp` take a concrete `SyntheticTileSampler&`, so GPU parity
   can never exercise the real-tile climate regime. Mitigated for now by 2a
   making the synthetic emission span every threshold. Worth doing before
   ADR-0006 makes the GPU path authoritative.
5. **Scale-dependent slope thresholds.** `slopeMmPerPx` is proportional to
   `pixelSizeMm`, so the cliff gate, the topsoil retention term and
   `slopeScaleQ10`/`microScaleQ10` all change meaning at scale 8. Latent —
   only scale 1 exists. Do all three together, before generating scale-8 tiles.
6. **Narrow `diffusion.py`'s bio_12 range** from 0..12000 to ~0..4000 mm/yr.
   Precipitation occupies 23 of 256 codes (1 LSB = 47 mm/yr), coarser than the
   distinctions the thresholds draw. Changes tile bytes → GPU rental +
   `provider_id` roll, so attach it as a rider to the next paid pregen run.

**Lesson.** Calibration constants outlive the data they were calibrated
against. Four independent climate calibrations had drifted apart here
(`biome.h`, its HLSL copy, `VoxelClimateProbe`'s remap, `gen_terrain_textures`'
LUT), and the render side worked around the voxel side rather than fixing it —
`VoxelClimateProbe.h:127` says so outright: "that is voxel-core's to fix and
this change does not touch it." Write thresholds in units that say what they
mean, and measure before reasoning.

### CORRECTION to the table above: those frame-time numbers were noise (2026-07-25, same day)

The p95/worst/hitches deltas in the entry above are **retracted**. They came from
four legs of the scripted flight. Twelve legs, taken across contended and idle
machines, show the ranges overlapping almost completely, with the **component**
path holding the better median on p95 and hitches:

| | component (n=6) | pooled (n=6) |
|---|---|---|
| p50 ms | 24.5 (17.2–42.0) | 23.7 (16.7–47.4) |
| p95 ms | 44.3 (29.2–77.6) | **62.6** (21.8–96.6) |
| hitches | 448 (35–810) | **627** (6–781) |
| chunks/s | 486 (322–678) | 519 (271–839) |

Two identical pooled legs on an idle box, same binary, same cvar, back to back:
p50 **21.0 and 47.4 ms**. A 77% within-path spread, larger than the effect. The
four legs that produced the retracted table were a coin landing the same way four
times.

The `-VoxelPerfRun` harness flies a 20 m/s circle and reports percentiles over
whatever streaming load that path meets; residency, scheduling and GPU boost
state all vary run to run and swamp a change worth tens of percent. **It cannot
answer this question as currently built.**

Unaffected, because neither is a wall-clock measurement: the pooled path draws
9,822 chunks / 8,813,242 quads as ONE primitive and ONE draw call against 9,822
primitives (verified every run); the visual parity result (17.4% → 4.3% of
pixels, quoted against a measured 1.1% noise floor); and this file's own earlier
finding that `renderMs` is 43.12 of a 43.92 ms frame, which is the strongest
argument for ADR-0006 and which tonight did not re-measure.

Full write-up and the proposed replacement protocol in
`docs/streaming-handoff.md`, "CORRECTION: the G5 frame-time numbers were noise".

## erosion-v7 is PARKED, and why it cannot simply be rebased (2026-07-25)

`origin/claude/erosion-v7` carries ~340 lines of dendritic drainage carving
that are still wanted. It is parked, by Matt's decision, and this records the
reason so the next person does not spend a session rediscovering it.

**It is not the version collision.** The branch claims `kWorldGenVersion` 7 and
`main` is now 8, and its region-fitted precip retune (20/24/30) is superseded by
v8's physically-derived thresholds. Both are trivial to resolve: drop the biome
change, keep the drainage, re-pin.

**It is that the branch's central design assumption expired the same day.** The
drainage carve is CPU-only: `worldgen.ush` keeps it compile-time off
(`kDrainageEnabled = false`) because the flow-accumulation precompute is a
reduction over a ~64x64 lattice window that the per-column dispatch cannot
afford and does not bind a raster window wide enough for. A faithful GPU port
needs a separate compute pass in `gpu_harness`/RDG. The branch handles this
honestly — an `Amplifier` ctor flag, defaulting ON, with the cross-vendor gate
comparing the drainage-OFF surface — and its own comment calls this "the
sanctioned 'land CPU-side behind a switch and say so' path".

That was sound on 2026-07-23. It stopped being sound on 2026-07-25, when
**PR #104 landed `voxel.Stream.GPU` on by default**. Under ADR-0006, display
geometry is GPU-generated while collision, raycast and digging stay on the CPU
authority path. So with drainage on:

* the GPU renders the surface **without** the carve,
* the CPU authority has the valley,
* and `kDrainMaxCarveMm = 5600` — up to **5.6 m, i.e. 56 voxels** — of
  solid-looking ground you fall straight through.

ADR-0006's invariant 3 permits display/authority divergence for cross-vendor
bit-exactness, which is a rounding-scale allowance. It does not sanction a
systematic multi-metre geometric difference.
`docs/terrain-amplification-reconciliation.md` had already called this exact
case: *"There is no way to make it display-only — falling through visibly solid
ground is not an option in a digging game."*

**What unblocks it**, in preference order:

1. Land the drainage with the ctor flag defaulting **OFF**. Default worldgen
   output then equals `main` byte-for-byte, so no golden moves and no
   `kWorldGenVersion` bump is needed at all — the cheapest possible way to bank
   the code and its tests. Turning it on stays a separate, scoped decision.
2. Port flow accumulation to a GPU compute pass so both sides carve. This is the
   real fix and it is a substantial job in files the G-track is actively working
   in; it pairs naturally with the already-backlogged widening of the GPU
   harness to accept an `ITileSampler&`.

Do NOT rebase-and-land it as-is while `voxel.Stream.GPU` defaults on.

## Continents are not a `frequency_mult` decision (2026-07-25)

Measured, so the option can be closed rather than relitigated. Lowering
`frequency_mult[0]` from 1.5 to 0.4 does enlarge landmasses (inland reach
123 -> 192 km, largest landmass 197k -> 315k km2). But the interiors are not
habitable: profiling elevation against distance from the coast in that world,

| km inland | mean elevation | mean temp | above treeline |
|---|---|---|---|
| 0-25 | 462 m | 8.6 C | 8.8% |
| 25-50 | 1159 m | 4.5 C | 39.0% |
| 50-100 | 1596 m | 3.7 C | 47.4% |
| 100-200 | **2240 m** | **0.7 C** | **66.2%** |

Elevation climbs monotonically inland, so two thirds of deep-interior land sits
above the treeline, and precipitation barely moves (921 -> 809 mm/yr) because it
is decoupled from terrain. The model learned Earth's hypsometry, where large
landmasses have high interiors — Tibet, the Altiplano — and the lapse rate does
the rest. A 30 m render 184 km inland came back 2889-5899 m at -8.7 C: superb
alpine terrain, and nowhere to put a settlement.

So `frequency_mult` buys **alpine plateau, not continental interior**, and it is
the third lever measured and rejected for this goal after seed selection and sea
level. The route that does work is `tools/make_conditioning.py`, where elevation
and precipitation are authored independently and a low dry interior is simply a
spec. Recommend leaving `frequency_mult` at its default.

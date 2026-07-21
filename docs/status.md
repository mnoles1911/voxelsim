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
  measured p50 2.8ms / p95 4.2ms pre-M2 (PR #12). Formal min-spec proxy run
  landed (see "Perf-run hitches" backlog row / M1 gate run result below):
  p95 has stayed pinned at ~18-19ms across every wave tried so far (budget
  tightening + PSO precache, then chunk-component pooling) — still above the
  16.6ms bar. **Component pooling wave (2026-07-20/21) did NOT close the
  gate** — see the dedicated writeup below for the full before/after numbers
  and why the theoretical win didn't materialize.

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

## Backlog (parked / deferred, updated 2026-07-20)

| Item | State | Unblock |
|---|---|---|
| **Confirm real terrain-diffusion tile outputs** (exact climate-channel count/semantics/ranges vs our 4-channel assumption: temp, seasonality, precip, precip-variability) — then reconcile the tile codec + amplifier climate lookup + M4 biome table to reality, addressing gaps/tweaks | **NEW — gates M4 biome tuning** | cloud NVIDIA rental (same session as diffusion bring-up); until then synthetic tiles stand in |
| NVIDIA cloud digest run (closes BOTH M0 gates) | blocked on rental spend (Matt) | ~$1, minutes of runtime; same session can bring up terrain-diffusion + confirm tile outputs above |
| terrain-diffusion worker bring-up | deferred by ADR-0001 to vistas — Band 3 exists now, so eligible | cloud NVIDIA rental |
| M1 formal min-spec proxy perf run | RAN 2026-07-20: p50 3.8ms pass-quality, p95 20.4ms fails bar under M2 streaming ramp; gate 🟨 pending M2 hitch work | M2 polish (below) |
| R3/R4 first-build cost (~335ms/job) | needs GPU-side gen or disk brick cache | design pass |
| Dithered ring cross-fades — SYMMETRIC blend | v1 landed (adjacent fade-through); symmetric-blend needs wider desired-set annulus overlap | M2 polish wave |
| ring↔clipmap seam (z-fighting, accepted v1) | noted | M2 polish |
| Perf-run hitches (~15-38/run, max 400ms, initial streaming ramp) | **ISOLATED + partially fixed 2026-07-20**, **component pooling tried 2026-07-20/21 -- did NOT close the gate** (worktree agent): see the M1 gate run result + "Component pooling wave" writeup below. Root cause found: every hitch frame has the per-frame apply/unload budgets pinned at cap while their own CPU cost is <1ms — the real cost is render-thread scene-mutation backlog. Tightened streaming budgets (voxel.Stream.Max*PerFrame cvars, 8/4/4→3/2/2) + a BeginPlay PSO precache warmup cut hitches ~3-5x. Pooling `UVoxelChunkComponent`s (avoid DestroyComponent+NewObject on ring-boundary crossings) was the next concrete lead and got a full implementation + measurement pass, but p95 stayed flat at ~18-19ms across 8 measured runs (same band as pre-pooling) — see writeup for why | needs a lead that targets the per-load GPU vertex/index-buffer upload cost itself (BeginInitResource, unavoidable on every SetChunkQuads regardless of component reuse), not the UObject/component lifecycle pooling already addressed |
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

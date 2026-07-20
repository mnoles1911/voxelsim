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
- [ ] Prediction/reconcile polish, join-time compacted-snapshot sync,
  validation hardening (rate caps), water/NPC readiness hooks — wave 2/3
  per docs/m3-plan.md.

Build: worktree `voxelcore.lib` rebuilt clean from scratch this wave
(`vxc_tests` 2/2 + `vxc_editlog_selftest` pass; voxel-core itself untouched
by M3 wave 1 — see the wire-format note above). `VoxelEarthEditor` Win64
Development builds clean (zero warnings from any file touched this wave;
same ~pre-existing engine-header deprecation baseline as prior waves).
`VoxelEarthServer` cannot build on this Installed-Build engine (see above);
its Target.cs is otherwise complete and UBT-valid.

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

## Backlog (parked / deferred, updated 2026-07-20)

| Item | State | Unblock |
|---|---|---|
| NVIDIA cloud digest run (closes BOTH M0 gates) | blocked on rental spend (Matt) | ~$1, minutes of runtime; same session can bring up terrain-diffusion |
| terrain-diffusion worker bring-up | deferred by ADR-0001 to vistas — Band 3 exists now, so eligible | cloud NVIDIA rental |
| Mip cache eviction (unbounded, ~716MB observed) | agent wave in flight | — |
| Config-driven world seed (constant 20260719) | agent wave in flight | — |
| SkyAtmosphere spawned at world origin (visible ≥1000s of km) | agent wave in flight | — |
| M1 formal min-spec proxy perf run | proxy settings defined below; run pending | — |
| R3/R4 first-build cost (~335ms/job) | needs GPU-side gen or disk brick cache | design pass |
| Dithered blue-noise ring cross-fades + ring↔clipmap seam | deliberate last (plan's slip-risk item) | M2 polish wave |
| Perf-run hitches (~15-19/run, max 400ms, during initial streaming ramp) | needs isolate + PSO precache investigation | M2 gate work |
| Clipmap follow-ups: two-sided material (winding unverified), A/B perf isolate, CDLOD replacement per ADR-0002 tripwire | noted in ADR/plan | M2 polish |
| Edit-log compaction unused by any caller | offline tooling | M3 persistence |
| Debug tooling P2/P3 (τ overlay, water ledgers) | phased with M2-polish/W2 | — |

M1 min-spec proxy definition: `sg.ViewDistanceQuality 0`, `sg.ShadowQuality 0`,
`r.ScreenPercentage 100` at 1080p, streaming budgets halved
(`voxel.*` budget cvars pending) — intent: approximate RTX-3060-class
headroom on this 7800 XT by measuring at these settings and requiring
p95 < 16.6ms with zero steady-state hitches (excluding the first 10s ramp).

**M1 gate run result (2026-07-20, 60s at proxy settings, post-M2 world):**
p50 3.78ms (steady-state comfortably 60fps+), but p95 20.4ms / 38 hitches /
max 400ms — FAILS the p95<16.6ms bar. Attribution: the M2 multi-ring +
clipmap streaming ramp (14,748 chunks loaded across the run, 59% budget
saturation) — M1-scope content alone measured p50 2.8/p95 4.2 pre-M2
(PR #12). Verdict: M1 gate stays 🟨; the blockers are the backlogged M2
items (streaming-ramp hitch isolation, PSO precache, R3/R4 first-build
cost, budget cvars for true throttling) — re-run after those land.

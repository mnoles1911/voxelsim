# Deep review: chunk load/unload performance — diagnosis of the ~600/s pooled plateau and a ranked idea set

Written 2026-07-27, from a four-track code review (streaming subsystem, GPU
pipeline, CPU core + component path, docs/measurements digest) of everything
that meshes, renders, culls, loads, and unloads the voxel cascade. Everything
below cites code by symbol + approximate line; line numbers drift, symbols
don't (ground rule 12).

Status of every claim: **[E]** = evidence read directly from code or an
existing measurement; **[H]** = hypothesis, needs a leg before acting.
Nothing here re-proposes anything in `docs/backlog.md` §7 or the falsified
levers in `docs/measurements/*` (cross-fade, AdmissionBandSkip,
DispatchAfterDrain, in-flight 1024, batch caps 16/32, ring-major dispatch,
slot-floor sweeps — all checked before writing).

---

## 1. The headline: the P0 plateau has a specific, convergent explanation

The open P0 (`gpu-throughput-wave-2026-07-27.txt`): pooled arms plateau at
~600 chunks/s at the 128 m / 4 km cascade regardless of mesher, while the
component arm posts ~1,080/s; "apply budget only 8.5% saturated — results are
not arriving." Three independent reads of the code converge on one mechanism
with two aggravators. This is the review's most valuable output.

### 1a. `PushUpdatesToProxy` is O(resident chunks) and runs once PER chunk applied — and per chunk unloaded [E]

`UVoxelGpuPoolComponent::AddChunk` / `AddChunkFromGpu` /
`RemoveChunkInternal` / `UpdateChunk` each end with their own
`PushUpdatesToProxy()` call (`VoxelGpuPoolComponent.cpp` ~:2009, :2101,
:2152, :2506). Each call, at the adopted cascade's ~39,020 resident chunks:

- **Game thread:** copies the whole chunk table — `OriginsCopy = ChunkOrigins`
  + `ParamsCopy = ChunkParams`, ~39k × 32 B ≈ **1.2–1.5 MB of memcpy per
  applied chunk** (~:2413) — and, since every add sets `bChunkTableDirty`,
  runs `BuildChunkRuns()`: an O(N) walk over all allocations **plus an
  O(N log N) `Runs.Sort`** (~:2574–2601), per applied chunk. Plus one render
  command per call.
- **Render thread:** `UpdateChunkTable_RenderThread` copies Origins/Runs
  again, whole-table `LockBuffer` uploads (~1.25 MB), then
  **`RebuildRunBounds()` recomputes ~39k `ComputeRunBounds`** — each an
  `FBox::TransformBy(FMatrix)` in LWC doubles (~:1060). The file's own
  comment prices the per-run work at 45–90 ns ⇒ **~2–4 ms of render-thread
  CPU per applied chunk**.

Unloads pay the same bill again (§1c). This one mechanism simultaneously
explains:

- **600 vs 1,080**: the component apply is O(1) on the game thread (pop
  parked component, `SetChunkQuads(MoveTemp(...))`, `MarkRenderStateDirty` —
  proxy build deferred and batched by the engine at end of frame). The pooled
  apply carries a fixed O(39k) tax per chunk. At equal wall budget the pooled
  path applies fewer chunks per frame — and gets *slower as residency grows*,
  which is exactly the shape of "fine at 64 m rings, plateaued at 4 km."
- **"renderMs median 75 ms on fork-on hitch frames"** (ring-gap file): the
  expensive half of the tax lands on the render thread a frame later.
- **Why every swept knob was dead**: producer-side levers can't move a
  consumer-side O(N)-per-item cost.

### 1b. The "8.5% saturated" number is a count metric; the real limiter is the 6 ms wall clock [E]

`DrainResults` breaks on `ApplyBudgetMs = 6.0` wall-clock once ≥4 applies ran
(`VoxelWorldSubsystem.cpp` ~:9054), but `LastAppliedFrac = Applied /
MaxApplies` reports against the **64-count ceiling** (~:9247). 8.5% of 64 ≈
5–6 applies/frame; at ~100–120 Hz that is **≈600/s exactly**. The published
"results are not arriving" reading is likely an artifact of the metric: the
loop is exiting on time, not on an empty queue, and nothing currently logs
which exit fired. Every stage of the pipeline is also paced per-tick
(BatchCap/tick, HarvestCap/poll, phase-2 next tick, delivery next tick,
applies/frame), so **throughput is proportional to frame rate** — a load
storm that drops fps drops chunks/s everywhere at once, which is what a
"plateau insensitive to all knobs" looks like.

### 1c. Aggravators on the same path [E]

- `SampleChunkParamsForPool` (~:8618) runs `GetSurfaceHeightUU` — a **full
  `Amplifier::column`, cave lattice and cavern passes included, on the game
  thread, per applied chunk** (~:12076) — plus a climate sample and a
  per-call `GetSubsystem` lookup. The producing job already computed this
  surface (the band); it is re-derived at apply time.
- CPU-form pooled applies run `PackVoxelChunkQuad` with a fresh
  `TArray<uint64>` per chunk (~:8757).
- **Pooled unload is O(quads)**: `RemoveChunk` stamps `kHiddenChunkId` over
  the freed range on the CPU shadow, re-uploads ~12 B/quad, and triggers
  another full `PushUpdatesToProxy` (`VoxelGpuPoolComponent.cpp` ~:2106).
  A component unload is O(1) + `RemovePrimitive`. Under flight every load has
  a matching evict, so the table tax is paid ~2× per net chunk.
- Stale results consume the same 6 ms budget before live ones (deliberate,
  ~:9029); stale drains measured +143% under churn.

### 1d. The 225 ms submit→deliver median is Little's law, not a per-job cost [E]

Observed ~150–256 in flight at ~600 delivered/s ⇒ latency ≈ 250–425 ms.
That is queue depth over throughput. It is **downstream of the plateau**, and
it is why `GpuMeshInFlight` 256→1024 made tails catastrophically worse
(1024/600 ≈ 1.7 s mean; 13 s max measured) without moving throughput. Fix
throughput and latency falls out; do not chase latency directly. The genuine
fixed floor is ~2 poll quanta (completion is detected by a per-tick poll of
`FRHIGPUBufferReadback::IsReady`, then delivered on the *next* game tick) —
~25–50 ms at 60–75 fps, removable with fences/callbacks (§3, T1-6).

**The one measurement to run first** (repo culture: instrument before fix):
split `DrainResults` exits (queue-empty vs wall-clock vs count-cap) and split
per-apply ms into pack / params / pool-add(game) / table-push(render). If §1a
holds, batching + incremental tables should close most of the 600→1,080 gap
on their own.

---

## 2. The rest of the review (things that are not the plateau)

### 2a. Prioritization is entirely reactive — nothing is velocity-aware [E]

- The anchor is the pawn's instantaneous position; **no velocity is read
  anywhere** in streaming. Sorting, admission cutoffs, entry-scan gating and
  the underground sight sphere all use where the camera *is* — i.e. work is
  prioritized for where the camera *was* by the time results arrive 225 ms
  later. At 20 m/s that is 4.5 m of error per pipeline transit, compounding
  with scan quantization below.
- Entry scans are gated on the anchor crossing a **level-L chunk** (~:6053):
  51.2 m at L4, 102.4 m at L5. A ring's annulus membership is only
  re-enumerated every ~2.5–5 s of travel at 20 m/s.
- The stale-scan refill explicitly requires **0.5 s of anchor quiescence**
  (rev B, ~:3923) — by design it heals nothing while moving.
- The **cold-band throttle serializes the leading edge**: a cold L0 footprint
  dispatches one blind seed job and defers its column-mates until the seed's
  result *drains* (~:7659) — a ≥28 ms GPU round trip plus poll quanta per new
  column, with a 5 s age-out backstop that equals **100 m of travel** at
  20 m/s if a mark strands. Bands are pure functions of (X,Y) and are never
  prefetched.
- There is **no inward pre-admission**: finer chunks become enumerable only
  once the footprint is inside their annulus. The outer edge has the 1.25×
  exit band and the e/√2 seam pad; the inner boundary crossing — the exact
  reported symptom — has nothing ahead of it. (The ring-gap handoff's
  "overlapping residency is not the same thing as cross-fading" note stands:
  cross-fade is closed, overlap is not.)

### 2b. Nothing is ever cached; unload burns everything [E]

- **No mesh cache exists at any layer.** `ChunkRecords.Remove(Key)` deletes
  the only per-chunk state; ring crossings at speed re-mesh ground that was
  resident seconds ago, twice per boundary (evict fine, build coarse — then
  back). Packed quads are 8 B each and deterministic given
  (seed, worldgen version, level, key) with edits vetoed by
  `NeedsOverlayAwarePath` — ideal cache keys already exist
  (`FChunkRecord::GenerationId`).
- Every L0 job rebuilds its 34×34 = 1,156 `Amplifier::column` grid; Z-siblings
  in the same footprint rebuild it identically. The probe to size a shared
  cache ships (`-VoxelL0GridCacheProbe`) but the cache was never built.
- `FSharedMipCache` (512 MB, sharded, LRU) is **dead by default**: it serves
  only `MakeLevelSampler`, and `kDefaultCoarseMinLevel = 1` routes every L≥1
  job to the coarse path, which bypasses it. Dead weight or a wiring
  opportunity — either way it's currently a 512 MB budget line for nothing.
- Only the footprint band/Z-range/solid-floor caches survive re-request, and
  they're pruned at 2× the unload ring, so long flights repay them.

### 2c. Lifecycle hazards found (correctness-adjacent) [E]

1. **Pool refusal strands a chunk permanently**: on `Pool.Alloc` failure,
   `GPUMaxChunks` cap, or payload/component collisions, `ApplyMeshResult`
   returns with the record unsettled, not in flight, and **not re-queued**
   (~:8850, :8775, :8733, :8822). The entry scan's "already tracked"
   early-return then never re-admits it. A transient pool-pressure spike
   during fast flight converts to a standing hole until eviction. (Same
   family as the scan-before-park race fixed in `508a2fa`.)
2. Chunk-table floor is 49,152 vs measured 39,020 settle — **~26% headroom**;
   motion retention double-residency pushes chunk count up, and crossing the
   floor forces a full render-state rebuild that (per D4-R1) invalidates
   every GPU-written range.
3. `LodRetentionMs` shipped default is 10,000 (`VoxelDebug.cpp`) but the
   retention census comment (~:4730) says "its 20000 default" — one of the
   two is wrong, and the "capRel=0 during flight" claim was taken at 20000.
4. Resurrection writes `RetainUntilSeconds = 0.0` where the documented
   sentinel is −1000 (~:6261 vs :1132) — behaviorally equal today, trap
   later.
5. No pool defrag exists; first-fit + churn at 80%+ occupancy will
   eventually refuse allocations with ample total free (the saturation Error
   is now loud, but the failure mode is #1 above — a stranded hole).
6. Freed pool ranges keep drawing as degenerate points until reused — vertex
   cost, invisible.

### 2d. The outermost boundary (ring 5 ↔ clipmap) leaks under motion [E]

- The clipmap hole is an axis-aligned **square** (half-extent 4,096 m); the
  cascade is **circular**. The corners out to 5.8 km belong to neither.
- The shared clipmap origin snaps on a **256 m grid** and levels rebuild
  round-robin ≤1/tick — at 20 m/s the hole edge lags the cascade edge by up
  to ±128 m on the leading edge.
- The clipmap samples raw 30 m tile bilinear, ring-5 voxels include the
  detail octaves (up to ~15.7 m worst-case) — metres of vertical mismatch at
  the seam, see-through at grazing angles. Ring 5's outer faces get no skirt
  (`ComputeRingSkirtMask` only skirts toward finer neighbours).

Concentric holes "at the far edge" during flight can come from here even
with the voxel cascade healthy.

### 2e. Draw-path debts already on record, confirmed in code [E]

- Per-range **single-frame uniform buffers**: up to K=1024 ranges × (camera +
  4–5 shadow gathers) ≈ ~5–6k `CreateUniformBufferImmediate` per frame; the
  documented 181–421 ms churn transients. The only per-element state is one
  `BaseQuad` uint — the vertex-factory indirection (ranges in one structured
  buffer) removes the whole class.
- Every gather walks all ~39k runs on the CPU (~195k frustum tests/frame with
  shadows); shadow gathers are 92.6% of submitted quads with no scheduling
  story (census on record).
- A table update racing a gather forces the uncached bounds fallback (the
  pre-hoist ~11.7 ms path) — churn couples streaming into draw cost.

---

## 3. Ideas — ranked, tiered, and filtered against the falsified list

Ordering within tiers is by (expected impact on chunks/s and on transient
holes) ÷ risk. T0/T1 are "do these"; T2 attacks the symptom directly; T3 are
throughput multipliers already partly designed; T4 are the radical bets.

### T0 — Instrument first (hours)

- **T0-1.** DrainResults exit-reason counters + per-apply stage timings
  (pack / params / pool-add / table-push game+render). Predicted result:
  wall-clock exit dominant, table-push dominant per-apply. This either
  confirms §1 in one leg or kills it cheaply (lesson 1 of the ring-gap
  night).
- **T0-2.** Submit→apply latency histogram per producer + per stage
  (queue/dispatch/GPU/poll/deliver/apply), logged with the perf snapshot, so
  the *next* plateau localizes itself.
- **T0-3.** Resolve the LodRetentionMs 10000-vs-20000 doc/default mismatch.

### T1 — Close the plateau (days; near-certain wins)

- **T1-1. Batch pool publication once per frame.** Accumulate adds/removes
  across `DrainResults`/`DrainUnloads`; one `PushUpdatesToProxy` flush per
  tick (one table copy, one `BuildChunkRuns`, one render command, one
  `RebuildRunBounds`). Divides the §1a tax by applies-per-frame (~8–64×).
  The single biggest lever on both chunks/s and churn hitches.
- **T1-2. Make table/run/bounds updates incremental.** An add/remove touches
  one entry and one run: sub-range upload for `ChunkOrigins/ChunkParams`
  (the quad path already has sub-range locks), maintain `Runs` ordered with
  insert/remove instead of rebuild+sort, update one `RunBounds` entry.
  O(N) per chunk → O(log N). With T1-1 this removes the residency-scaling
  term entirely — the pool stops getting slower as the world gets bigger.
- **T1-3. Carry SurfaceZ + climate in the job result.** Kill the per-apply
  `Amplifier::column` and climate probe (§1c); the worker/GPU job already
  reduced the band. Same fix for the component proxy's `ChunkSurfaceZUUU`
  re-derivation.
- **T1-4. Table-entry-only unload.** Hide a chunk by zeroing its table entry
  / scale instead of stamping ids over O(quads) and re-uploading the range
  (the shader already collapses hidden ids). Unload becomes O(1); the
  per-quad id rewrite exists only because ids live in a parallel buffer.
- **T1-5. Decouple pacing from frame rate.** Convert per-tick caps
  (BatchCap, HarvestCap, applies floor) into per-second budgets scaled by
  DeltaTime. Removes the throughput ∝ fps coupling that made every knob look
  dead. (Note: this is *not* the falsified DispatchAfterDrain — that added a
  second same-frame dispatch; this makes existing caps time-based.)
- **T1-6. Fences/callbacks instead of polling; start phase 2 render-side.**
  The direct path needs the 4-byte total only to size `Pool.Alloc`. Short
  form: RHI fence + callback delivers the total the moment it lands (−1–2
  poll quanta ≈ −25–50 ms median). Long form: pre-reserve pool space against
  the job's MaxQuads bound and enqueue the compaction write in the *same*
  RDG graph as the mesh chain — the CPU receives only a bookkeeping record
  and the 4-byte true-up arrives asynchronously. Submit→visible stops
  touching the game-thread tick at all.
- **T1-7. Un-strand pool-refusal chunks** (§2c-1): any refused apply pushes
  its key onto a retry queue; converts capacity spikes from permanent holes
  into delay. Small, and it's a correctness fix.

### T2 — Kill the transient LOD-boundary holes (the actual symptom)

- **T2-1. Velocity-aware anchor.** Use `Anchor + Velocity × τ` (τ ≈ 0.5–1 s ≈
  the measured pipeline latency) as the origin for sorting, admission
  cutoffs, and entry-scan gating — optionally a directional weight on
  `DistSq`. Zero structural change; same sorts, same caps, shifted center.
  This directly compensates the 225 ms transit: the pipeline starts working
  on where the camera *will be* when results land. Nothing in the falsified
  list touches this.
- **T2-2. Speed-scaled inward pre-admission band.** Mirror the outer seam
  pad on the inner edge: admit finer chunks up to `v × pipeline-latency`
  *before* the boundary reaches them. The outer pad (e/√2) already proves
  two-ring overlapping residency works and costs ~9% residency; this is the
  same mechanism pointed inward, scaled by speed so it costs nothing when
  stationary. Explicitly not the closed cross-fade — overlap, not fade.
- **T2-3. Cold-band prefetch along velocity.** Bands are pure (X,Y)
  functions; seed `FootprintBandCache` for the leading-edge annulus with
  cheap band-only evaluations (or `surfaceBoundsMm`), and/or allow K>1 blind
  seeds per cold footprint while moving. Un-serializes the leading edge from
  seed-job round trips. (This is the streaming-handoff's own "move the
  *throttle*, not the skip, to admission" suggestion — the skip itself stays
  off.)
- **T2-4. Pool-range parking instead of freeing (resurrection for
  geometry).** Radical but cheap given T1-4: on evict, *keep* the pool range
  and table entry, hidden, on an LRU; re-admit = flip the entry back.
  Under-motion ring oscillation and inward/outward crossings become O(1)
  round trips instead of full re-mesh. Evict parked ranges only under real
  pool pressure. The 44 M pool already carries +13% motion stand-in headroom;
  this formalizes it. The existing record-level resurrection fix (`508a2fa`)
  proved the lifecycle pattern; this extends it below the record to the
  geometry.
- **T2-5. Mesh LRU cache (RAM), keyed (level, key, GenerationId, worldgen
  version).** For ground that left the pool entirely: packed quads at
  8 B/quad ⇒ the whole 35 M-quad cascade is ~280 MB; a few-hundred-MB LRU
  over the churn band turns re-mesh into a memcpy + pool write. Edits
  already veto via `NeedsOverlayAwarePath`. (T2-4 covers the hot loop;
  this covers the warm one. Build T2-4 first, measure whether T2-5 still
  pays.)
- **T2-6. Retention: release stand-ins on *settle*, cap as backstop only.**
  Today `ReplacementCovered` re-runs per held stand-in per frame (~8k map
  probes/frame at held≈1,000) and the 10 s cap is the hole-producing release
  when the pipeline is slow. Make settle events (apply of the replacement)
  clear their stand-ins directly; keep the cap as a leak backstop. Cheaper
  *and* removes cap-releases as a hole source. (Note 20 s retention was
  measured NOT to change holes — the fix here is event-driven release, not a
  bigger cap.)
- **T2-7. Ring-5 ↔ clipmap seam** (§2d): overlap the hole by one coarse cell
  instead of abutting; move the hole mask per tick independent of the 256 m
  vertex-grid snap; give ring 5's outermost faces an outward skirt; bias the
  clipmap's inner rings down by the detail-band envelope (the bound is
  derivable from `kLandformAbsMaxMm + kMicroAbsMaxMm`).

### T3 — Throughput multipliers (designed or half-built; sequence after T0/T1)

- **T3-1. Tile batching** (4×4 lattice, up to 16 chunks/dispatch) — fully
  designed, occupancy census shipping. The docs' own sequencing stands:
  it multiplies the fork ceiling, so land it after the apply path stops
  being the wall. D2 chunk-local emit and the D5.3 skirt-in-kernel decision
  already made it composable.
- **T3-2. Uber-batch the RDG graph.** One graph per batch already exists but
  still builds 7–8 passes + its own raster upload per job. Batch N chunks
  per *dispatch* per kernel with per-chunk constants in a structured buffer
  (`QuadWriteBase` machinery anticipates this), and merge per-apply pool
  writes into one graph per frame (T1-1's render-side twin). Amortizes the
  3.4× halo waste for adjacent chunks.
- **T3-3. D6 band-reduce** (built, gated): removes the remaining ~45% CPU
  column pass from GPU L0 jobs. Turn on behind its gate once T0-2 can
  attribute its effect.
- **T3-4. Vertex-factory indirection** (ranges in one structured buffer,
  per-element = one integer): kills the ~5–6k single-frame uniform buffers
  per frame and the 181–421 ms churn transients; prerequisite for T3-5.
- **T3-5. GPU-driven cull → indirect draw → compute compaction** (the staged
  G1–G4 plan already on record): removes the 195k CPU frustum tests/frame,
  the K=1024 over-draw floor, and finally gives shadow gathers (92.6% of
  submitted quads) a scheduling story. This is the named unowned debt; it is
  also what makes streaming churn stop coupling into draw cost (§2e).
- **T3-6. Idle-time pool defrag**: a background `QuadPoolWriteMain`-shaped
  move pass keeping `GetLargestFreeRun` healthy; removes the ~10%
  fragmentation reserve and the silent-refusal failure mode.

### T4 — Radical bets (bigger, phased, each wants its own design note)

- **T4-1. Speculative generation into the idle GPU.** The bench measured
  **92,000 chunks/s** GPU generation at r=128 — ~150× the plateau. Once T1
  frees the consumer side, the GPU is essentially always idle from
  streaming's perspective. Spend it: pre-mesh the annulus in the velocity
  cone (T2-1's predicted anchor) into parked pool ranges (T2-4's mechanism)
  before admission ever asks. Admission then *finds* geometry instead of
  *commissioning* it. Quality and LOD distances untouched — this is the same
  worldgen, earlier.
- **T4-2. GPU-resident residency.** The logical endpoint of ADR-0006: move
  annulus enumeration + desired-set diff to a compute pass (the cascade is
  pure math over the anchor), with the CPU consuming a compact "admit/evict"
  delta buffer for records, collision, and edits only. Kills the O(records)
  exit scans (~2.4–2.7 ms at 16k records, ~6–8 ms at 40k) and the entry-scan
  quantization in one move. Large; stage behind T1/T2 results — it may not
  be needed if the plateau closes.
- **T4-3. On-disk deterministic chunk cache.** Worldgen is a versioned pure
  function; packed quads keyed (seed, worldgenVersion, level, key) are
  serializable, edits vetoed. Cold fill (~47 s at 4 km) and revisit become
  disk-bound. Also the natural transport for a future CDN vista layer
  (doctrine §6 already sanctions cacheable, client-independent offload).
- **T4-4. Async-compute meshing queue.** Move the mesh/compact chain to the
  async queue so it overlaps raster instead of competing with it — relevant
  once T1-5/T1-6 make the pipeline GPU-bound rather than tick-bound.
- **T4-5. Wire or delete `FSharedMipCache`** (512 MB of dead machinery by
  default), and if wiring: fold the L0 column-grid cache (probe already
  ships) into the same sharded LRU. Measure with `-VoxelL0GridCacheProbe`
  first — the probe exists precisely so this doesn't get built blind.

### Sequencing recommendation

1. T0-1/T0-2 (one session, one leg each) → confirm §1.
2. T1-1 + T1-2 + T1-3 + T1-4 (one wave) → re-run the head-to-head; expect
   the pooled arm at or past 1,080/s with holes(final) collapsing.
3. T2-1 + T2-2 + T2-3 (one wave, flight-focused) → re-run the ring-gap
   flight; expect flight-phase transient holes (GPU arm 854–907 median) to
   drop toward the CPU arm's 55.
4. Then T3 in the docs' own order (tile batching after the plateau closes),
   T2-4/T2-5 as the churn numbers dictate, T4 as appetite allows.

Every step lands behind the existing harnesses (`voxel-run-leg.ps1`, the
standard leg, CoverageVerify) and two-leg minimums — the ground rules that
caught the last three false wins apply unchanged.

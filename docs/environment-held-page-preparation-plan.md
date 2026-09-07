# Held environment page preparation

Design checkpoint: September 7, 2026. This is the next proposed implementation
after `voxel.Environment.RehearseHandoff`, not evidence that held preparation or
ownership publication has passed in the game. The next stage should prepare
replacement brick packs for every currently allocated affected page while the
original terrain remains visible. It must never call an ownership commit, publish
replacement pages, reveal the hidden actor, or mark either backend ready.

## Sequence and lifetime

1. Reuse canonical selection, clipped hidden actor preparation, complete
   `Work->Pages` enumeration, and the existing game-thread `FBarrier` freeze.
   Enumerate all eight levels, including absent pages and the existing apron.
2. Continue consuming already submitted ordinary CPU/GPU work. Wait for the
   ticket's per-key leases to become quiescent, independently of `ChunkRecords`.
3. Acquire `FVoxelBrickPool::AcquireEvictionPins` for the complete key set, then
   capture `SnapshotAllocation` for every key. A failed pin request cancels the
   whole preparation. Filter present tokens into the generation worklist; retain
   absent tokens for validation. Reject zero allocated pages as no-work evidence,
   not a successful replacement preparation.
4. Build and validate a private target render context. Prepare bounded private
   packs for every allocated page, without installing them in either live pool.
5. Revalidate source, edit/residency epochs, fixed-anchor/ring evidence, and every
   allocation token after preparation. Compare absent as well as present tokens.
6. Report counts, actual bytes, backend, and `publicationReady=0`. Discard private
   packs, release eviction-pressure pins, and release the World freeze.

**Pressure-pin semantics:** the existing pins prevent `EvictOne` from selecting
the keys under allocation pressure. They also apply if an absent key is later
inserted. They do **not** prohibit explicit `RemoveChunk` or same-key replacement.
They are not allocation reservations or a general mutation lock. World gates and
token revalidation are still required. Acquiring after ordinary work drains is a
clean observation boundary, not a workaround for pins rejecting replacements.

## Smallest useful first implementation: CPU packs only

Add a distinct explicit diagnostic request, rather than changing the ordinary
candidate or rehearsal commands. A CPU-only run can prepare *every allocated
affected page*, including coarse levels, even when normal rendering uses the GPU.
It proves the private CPU composition path; it cannot claim GPU parity or complete
backend readiness.

Extraction sites in `ue-project/Source/VoxelEarth/VoxelWorldSubsystem.cpp`
(line numbers are approximate and refer to this checkpoint):

| Existing site | Proposed reuse |
| --- | --- |
| `TickProductionCandidate` / `TickProductionRehearsal`, around 18099–18327 | Retain the completed preparation and extend the quiescent state with pressure-pin ownership, tokens, bounded worklist and cancellation state. |
| `FCoarseChunkGridSampler`, around 1344 | Reuse its explicit `RenderContext` argument and representative-coordinate rules for supported coarse pages. |
| Level-zero worker column grid and `GridSampler`, around 26100–26280 | Extract a worker-owned sampler preparation helper, including the canonical ordered resolved list, its private suppression markers, and column storage lifetime. Both ordinary CPU generation and the diagnostic should call it. |
| `VoxelBrickCpuArm::PackChunk`, around 2488 | Produce an `FVoxelBrickCpuPackRef` without adding it to a pool or building quad meshes. Do not gate diagnostic packing on ordinary `ShouldPack()` cvars. |
| `ProductionRenderSupported`, around 18085 | Preflight every worklist page. Refuse edited pages or unsupported coarse configurations before dispatch. Never use an unsuppressed `LevelSampler` fallback. |

The level-zero sampler first evaluates terrain and only then calls
`AssetField::materialAtResolvedForRender<true>`. Preserve that order and the
original resolved-instance order. Removing the selected instance from the list
would let an overlapping losing instance become visible in the vacated cells.

Use a private `vxc::AssetRenderOwnership` to obtain an unpublished target snapshot
through `begin(..., AssetRenderOwner::Object, ...)` and `target()`. Construct the
target from the current visible snapshot/source identities, and validate it with
`VoxelEnvironmentRender::Build`. Bind the original canonical bank grid using
`Work->Candidate.bankId`, `seedIndex`, `grid`, `CanonicalSourceHash`, and actual
provider/catalog strings. Do not bind the clipped actor's grid or modify
`ProductionOwnership.Visible()` / `ProductionRenderContext`.

The current pure ownership helper starts from its own visible snapshot; the first
pilot must explicitly refuse a nonempty production ownership snapshot unless a
proper snapshot-seeding API is introduced. Do not accidentally omit previously
owned sources from the target.

Reuse `VoxelProductionEnvironment::FPreparedPage` as a private storage type.
`FAdapter::StageCpuPage` and `StageGpuPage` currently mark backend readiness, and
`FAdapter::Prepare` requires a publication callback. Neither should be forced into
this diagnostic with a fake callback. A future preparation-only API may share
storage while keeping readiness and publication separate.

## GPU follow-up extraction

`SubmitGpuMeshJob` (around 23784–24460) currently mixes ordinary lease acquisition,
canonical region/request construction, shading/raster setup, request accounting,
submission, and registration in `GpuJobsPending`. Calling it unchanged for a
frozen key correctly fails ordinary lease admission.

Extract the request-building portion into a helper accepting an explicit validated
target context. Preserve `SetChunkFootprint`, coarse units, canonical resolved
instance order, `MarkPrivate`, and `SuppressTerrainRender` serialization. Keep
ordinary admission/counters out of this helper. The existing normal path must use
the extracted builder too, with fixture parity checked before adding the new arm.

Submit held jobs through `FVoxelGpuMeshJobManager::Submit` with
`bHoldBrickPublication=true`, `bDirectToPool=false`, and target ownership generation.
Existing held support disables resident publication and stacking and returns a
private `BrickVolume`. Require success, `bPublicationHeld`, matching generation,
and a valid brick payload before retaining a result. Generate brick packs only;
avoid optional quad payloads for this first marcher-focused diagnostic.

Held job IDs need a separate map and callback branch before ordinary
`OnGpuMeshJobComplete` processing. They must not enter `ResultsQueue`, ordinary
visible-generation acceptance, normal streaming counters, or speculative parking.
This extraction is substantially larger than the CPU-only seam and should be a
separate tested change.

## Budgets and cancellation ownership

Proposed initial hard limits, enforced before dispatch and checked on receipt:

- Complete frozen/pinned set: 8,192 unique keys, matching existing helpers.
- Allocated worklist: 1,024 pages. Exceeding it refuses the whole request, never a
  prefix presented as complete.
- Retained payloads: 128 MiB, including actual array allocations where available.
- Concurrent work: two CPU workers; the later GPU arm adds at most two GPU jobs.
- GPU request payload: at most 8 MiB per job, checked before submission/copies.
- Preparation: 60 seconds wall time from freeze, replacing rather than stacking
  the rehearsal's 30-second deadline. Existing bounded actor/source preparation
  has its own deadline before this stage.

A maximum current brick pack's descriptor/occupancy/material payload is
`(128 + 1024 + 8448) * 4 = 38,400` bytes. Two backend packs for 1,024 pages are
75 MiB before container overhead. GPU scratch, readback buffers, request copies,
source grids and the actor are separate costs; the retained-payload cap is not a
total process/VRAM promise. Measure those costs in the first real run.

A shared request object owns cancellation, generation identity, completed packs
and outstanding counts. Workers receive immutable context/input and return through
a dedicated completion queue. They never mutate UObjects, pressure pins, the page
barrier or live ownership. Ensure sampler arrays outlive every lambda using them.

Cancellation stops new submission and marks returned results discard-only. Retain
the request/context until outstanding tasks finish, preventing raw `Impl`/bank
pointers from outliving their owners. Because held work cannot publish, pressure
pins and the ordinary-work freeze may be released promptly on cancellation once
every held job is known to use the private route. Do not allow a new preparation
to exceed the global outstanding/byte budget while old cancelled work drains.

The shared GPU manager exposes `CancelAll`, not selective cancellation. Never call
it to cancel this diagnostic and thereby cancel unrelated streaming. Route late
held results by request generation and discard them. World teardown must wait for
workers/cancel the manager, destroy queued and retained payloads, then perform the
final render flush; payload destruction can enqueue render-resource release work.

## Acceptance and remaining boundaries

Before runtime, verify unchanged ordinary CPU sampler output on representative
level-zero/coarse fixtures and overlapping winner/loser cases. Exercise cancellation
with queued CPU work, late GPU delivery, allocation-token replacement/removal,
pressure eviction, epoch changes and budget refusal. Verify no token changes and
no index publication from the diagnostic itself; all allocated pages must produce
a pack, including empty replacement packs.

The real run must distinguish CPU-only from CPU+GPU evidence and log allocated,
absent, completed, cancelled, payload-byte and elapsed-time counts. If paired
backend content is checked, compare decoded voxel content, not container bytes.

Actual publication remains outside scope: `PublishPreparedBatch` still requires
the CPU arena allocator, and terrain index replacement has no atomic frame-boundary
reveal shared with the prepared procedural-mesh actor. Successful private packing
does not resolve either limitation.

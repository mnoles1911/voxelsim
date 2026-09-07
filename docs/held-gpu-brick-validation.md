# Private held GPU brick validation

`SubmitHeldBrickOnly` is an explicit diagnostic path. It admits one standard
48×48×6 apron request, derives the canonical 32³ brick interior, disables the
quad mesh chain per request, and requires a private held brick result. Ordinary
`Submit` defaults and global render settings remain unchanged. Return zero means
synchronous refusal without a callback; an admitted job retains the manager's
exactly-one completion contract.

Admission checks signed coordinate/anchor arithmetic, request shapes and existing
request validation before derivation. It counts allocated request-array capacity,
validation/derived copies, GPU/upload copies, columns/cells/claims, pack/scan scratch,
and private parity storage. The caller's limit is capped at 64 MiB per request.
`TotalBytes` covers the active request; `RetainedAndReadbackBytes` conservatively
covers the private pack, GPU slice/readback storage, CPU snapshots and canonical
decoder copies (eight maximum packs plus metadata slack). World must separately
charge its already retained CPU pages and previously completed private GPU pages.
This is not a bound on shared worklist/raster arenas, driver allocator granularity,
ordinary streaming, or the entire renderer.

A promoted held job cancelled/timed out/failed without proven successful completion
closes held admission for that manager's remaining lifetime. Reload/manager teardown
is required to retry this diagnostic path. A known queued predispatch cancellation
can reopen immediately. This avoids treating an early callback as GPU retirement;
ordinary jobs are unaffected. World transaction cancellation can leave its submitted
job running until actual completion, allowing the usual success path to recover.

`FVoxelHeldBrickParity` compares private CPU/GPU materials through the canonical
brick decoder; different valid palette encodings need not match bytewise. Bounds,
source offsets, descriptor kinds, material modes and occupancy masks are validated
before decoding. The expected origin is in level-L voxels (chunk key times 32).
The helper never writes live pools, admits registry objects, or enables rendering.

Only one helper readback can exist globally, including cancelled work awaiting
retirement. The global pointer is game-thread-owned; render-thread state uses atomic
completion publication. Each queued command captures shared state, never a destroyed
helper. Cancellation and destruction are nonblocking; the ticker drives asynchronous
readback readiness and the gate opens only after owned readbacks and array storage
are released. A retained helper/result does not retain that scratch storage.

Shutdown must call `DrainForShutdown` before the existing final render-command flush.
It queues a shutdown-only GPU idle wait when outstanding work exists and retires it;
it needs no subsequent ticker frame. Ordinary cancellation never waits for GPU idle.
This readiness/parity check is not proof of atomic terrain/object frame publication.

Tests: `Voxel.Objects.HeldBrickAdmission` and `Voxel.Objects.HeldBrickGpuParity`.
The latter uses real manager generation and asynchronous private readback, then tests
cancel/retry gating, helper destruction without a ticker frame, explicit shutdown
drain, and fail-closed promoted cancellation. Build/runtime results are recorded by
the coordinating session; adding these tests does not itself establish a pass.

Validated 2026-09-07: final Unreal build succeeded; complete DX12 retry passed37/37
with normal exit0. Real rhododendron probe matched20/20 allocated pages over a27-page
footprint, then discarded private packs and released pins, with normal exit0.
Preparation took20.376s from freeze; CPU arrays65988 bytes, conservatively accounted
retained GPU/parity6227200 bytes. See the handover for log paths and the initial
incomplete full-suite run, which is not counted as a pass.

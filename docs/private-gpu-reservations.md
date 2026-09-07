# Private GPU terrain reservations

`FVoxelBrickPool::BeginPrivateGpuReservation` is a bounded prerequisite for
publishing a whole environment replacement batch with the GPU allocator armed.
It accepts canonical CPU packs and a complete pressure-pin ticket, reserves
private descriptor slots, then runs the existing GPU claim/write kernels.
It never inserts a Resident entry or an index delta. There is no commit API yet.

Admission rejects duplicate keys, stale allocation identities, invalid origins,
levels outside0–15, malformed packs, inconsistent solidity flags, pending ordinary
claims, and another reservation. Limits are64 pages,8MiB conservative retained
storage and30seconds. Descriptor exhaustion rolls back without evicting terrain.

Readiness requires all eight claim words, all sixteen chunk-record words, and a
second readback of actual descriptor/occupancy/material payloads. Decoded materials
must match the private CPU snapshot. Allocation success alone is insufficient.
Only `ReadyPrivate` is reported; ownership and renderer publication stay disabled.

Cancellation queues GPU frees before descriptor slots can be reused. Free and
claim passes use the same graphics queue, with subsequent claims submitted through
later render commands. Readbacks retain their buffers and drain before the game
thread returns descriptors. Reset keeps its existing teardown-only contract;
destruction drains the owned GPU work. Abandoned caller tokens remain bounded by
the timeout because the pool and ticker retain the reservation until retirement.

Any pool index mutation conservatively invalidates the reservation, including an
unrelated streaming mutation. Live integration needs a stable final reservation
window or narrower validated mutation tracking, prepared index credits, and a
complete terrain/object publication transaction. This API does not provide them.

Validation: `build-environment-private-gpu-reservation-fixed.log` succeeded
(5actions,15.13seconds), then `environment-private-gpu-reservation-tests.log`
passed all46 DX12 object tests with normal exit0. Four new tests exercise actual
GPU exhaustion, descriptor exhaustion/rollback, successful payload proof followed
by cancellation/reset, and abandoned-token timeout. Negative admissions cover
budgets, duplicate reservation, flags and levels. The initial build failed on a
render-command macro missing a braced scope; that failure is retained in its log.

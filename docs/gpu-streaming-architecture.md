# GPU streaming architecture — the plan

**Date:** 2026-08-23
**Decision:** owner, this session — invest almost completely in GPU brick-pack streaming.
Edits and cold fallback stay on the CPU. Marched sun shadows off for now (backlog §0.0a).
**Reference:** [Aokana, I3D 2025](https://arxiv.org/abs/2505.02017) — and read §2, because
it does NOT say what it is usually summarised as saying.

---

## 1. The target, and why the CPU cannot reach it

| speed | level-0 chunks/s |
|---|---|
| 6 m/s walk | 1,245 |
| **30 m/s (current fly)** | **6,200 — the floor** |
| 100 m/s | 20,750 |
| 240 m/s | 49,800 |

**Where we are today (2026-08-23, measured):** 6,939 chunks/s mean, 11,081 peak, holes=0,
p50 7.64 ms. The floor is cleared.

**The CPU ceiling is arithmetic.** A chunk costs 0.34–0.59 ms p50 on a worker, and this box
has 8 background workers: **~16,000 chunks/s absolute**, assuming those workers do nothing
else. 20,750 is conceivable only in theory; 49,800 is not reachable. Generation has to
leave the CPU. That is the whole reason for this programme.

---

## 2. What Aokana actually says (and where we differ)

It is usually summarised as "GPU-driven, so put everything on the GPU". **That is not what
the paper does.**

- **Chunk selection is CPU-driven.** An implicit octree on the CPU, walked by coroutines,
  picks chunks by an LOD-error metric and issues load/unload commands.
- **The GPU is never waited on.** In the paper's words: *"CPU decides what to load; GPU
  renders what is available without synchronization bottlenecks."* No per-frame readback.
- The GPU-driven part is the **rendering** pipeline: chunk selection → 8×8 tile selection
  with Hi-Z occlusion → indirect-dispatch ray marching → 64-bit visibility buffer → colour
  resolve.

**The lesson is DECOUPLE, DON'T RELOCATE.** Zero synchronization, not zero CPU.

That matches our own measurements better than the "move the desired set to the GPU" idea
did. Per-chunk CPU *decision* cost was never our problem — the dispatch quota was, and
raising `JobsInFlightPerCore` fixed it. Our problem is a per-chunk **fence**.

**Where we differ from Aokana, and it matters:** their chunks are preprocessed SVDAG
streamed from disk. They never generate at runtime. **We generate procedurally**, which is
exactly the part that must move to the GPU. Aokana tells us how to decouple; it does not
tell us how to generate.

Worth stealing later, not now: SVDAG compression (9× memory, only ~5% of scene in VRAM,
424 MB for a ten-billion-voxel scene) and the tile/Hi-Z/visibility-buffer render path.

---

## 3. The keystone: why the readback exists, and how it dies

`FVoxelBrickPool::AllocateForChunk(Key, OccWords, MatWords, ...)` allocates the chunk's
range in the occupancy and material arenas — **on the CPU, and it needs the sizes.** Those
sizes are only known once the chunk has been generated, because a sparse chunk packs fewer
words than a dense one.

So the current loop is necessarily:

> GPU generates → **reads totals back to the CPU** → CPU allocates a range → GPU writes into it

**That readback is the fence.** Measured: `dispatchToReady` 28–37 ms ≈ 1.4 frames, against
`queued` p50 1,602 ms behind a 4-per-tick promotion. Throughput = in-flight ÷ latency, so
at one frame of latency 50,000 chunks/s needs ~1,000 chunks in flight. The manager holds
256. **The design cannot reach the target by tuning; the round trip has to go.**

**The fix: allocate on the GPU.** A suballocator living in a GPU buffer — an atomic bump
allocator over the arenas plus a free list — lets the generation pass claim its own range
with an `InterlockedAdd` and write its own descriptor. Nothing is read back, ever. This is
standard for GPU-driven pipelines and it is the single change the rest of this plan rests
on.

(The alternative — fixed worst-case slots per chunk — also removes the readback but wastes
VRAM proportional to sparsity. Rejected unless the suballocator proves harder than it looks.)

---

## 4. The measurements this plan must not contradict

Everything here was taken this session, matched legs, quiet box.

1. **Batching passes does not help.** B.1 cut GPU worldgen passes 3.4× (3,010 vs 10,335,
   crosscheck 0 FAIL) and moved throughput not at all. Pool-flush batching cut passes 2.5×
   and moved nothing. **Do not build more pass batching and expect throughput.**
2. **The GPU is not contended.** Turning off shadow marching freed ~13 ms of GPU per frame,
   doubled the frame rate, and moved streaming **+0.5%**. There is GPU headroom.
3. **Latency alone is not the cost.** `-VoxelGpuMeshQueueDepth=16` cut submit→deliver
   1,068 → 147 ms and the fork still lost on every axis. Something per-chunk on the CPU or
   render thread costs more than the wait did — **unexplained, and P1 must measure it, not
   assume it dies with the readback.**
4. **More throughput did not reduce holes.** +52% chunks/s took `uncovered` 3.99% → 4.97%.
   Holes are an admission-ORDER problem. **`uncovered`, not chunks/s, is the gate that
   matters to the owner.**

---

## 5. Phases

### P1 — GPU-side pool allocation, and the death of the readback  *(the keystone)*

Move arena allocation into a GPU suballocator. The generation pass claims its range,
writes occupancy/material words, writes its own chunk descriptor and record. **No totals
readback. No delivery to the game thread. No per-chunk apply.**

- The CPU submits a request and forgets it. Residency is discovered, not reported.
- Free is the hard half: eviction must return ranges without a CPU round trip either.
  Start with a GPU free list written by an eviction pass fed from the CPU's evict set —
  the CPU already knows what to evict; it just must not need an answer back.
- **Gate:** `submitToDeliver` stops existing as a concept for the direct path; the
  per-chunk fence count goes to zero; and finding #3 above resolves one way or the other —
  either throughput rises, or the residual per-chunk cost is now isolated and named.

### P2 — The chunk becomes resident by writing the index, not by being applied

Today a chunk becomes visible when the game thread applies a result and updates the march
index. Have the generation pass write its own index cells instead. **Today's delta-scatter
shader (`VoxelMarchIndexScatter.usf`) is the template** — it already scatters `[cell,value]`
pairs into the persistent index buffer from a compute pass.

- **Gate:** a chunk generated on the GPU appears in the marcher with the game thread never
  having touched it. Verified by the content hash the index already supports.

### P3 — Persistent worklist + indirect dispatch

CPU appends compact requests to a GPU ring buffer; one indirect dispatch per stage over
everything pending, group count read from a GPU buffer. Passes constant in N.

Note this is P3, not P1, **because of measurement #1** — pass count is not currently the
limiter. It becomes the limiter only once P1 and P2 remove the round trip. Build it then,
and re-measure before believing it helps.

### P4 — Retire the CPU generation arm to edits and cold fallback

The CPU path stays for: edited chunks (they touch the overlay and are not pure worldgen),
and as the fallback when the GPU path refuses a chunk. It stops being the steady-state
producer.

- **Gate:** the fraction of chunks produced by the CPU arm in steady flight falls to
  roughly the edited fraction. Every CPU-arm chunk that is *not* an edit is counted with a
  reason.

### Later, not now

- Aokana's render path (tile selection, Hi-Z, visibility buffer).
- SVDAG or similar compression for VRAM.
- Marched sun shadows (backlog §0.0a) — revisit with the reach dial, which was never chosen.

---

## 6. Rules carried into this programme

- **A gate that cannot come out the other way is worthless.** Every phase above states what
  failure looks like.
- **Never read a leg log with `grep | tail -1`** — the last window is post-flight linger and
  is all zeros. Use `tools/leg-summary.sh`.
- **Do not run legs while background agents work** — contention moved p50 by 6 ms and
  reversed the sign of a comparison in this session.
- **`voxel.GPU.MeshBatchCap` stays at 4/8.** Its raise is separately measured to hitch
  (32/64 → 367 hitches / 77.2k chunks against 4/8's 8 hitches / 89.4k).
- **Bit-exactness is checkable and must stay checked.** `voxel.GPU.VerifyCoarse` proves the
  GPU generator against the CPU one on columns, cells and quads;
  `voxel.GPU.VerifyBrickStack` proves batched against per-chunk. A new allocator needs its
  own equivalent, and it must be able to FAIL.

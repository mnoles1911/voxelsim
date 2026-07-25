# Pooled rendering: invariants and failure modes

Durable notes for the ADR-0006 GPU geometry pool. Handoff docs get superseded
every few days; this is meant to outlive them. Everything here was paid for in
debugging time, not read out of documentation.

The theme: **a pooled primitive fails silently.** One primitive drawing 3,000
chunks has no per-chunk state to inspect, no component to select in the outliner,
and no per-chunk proxy to breakpoint. Every distinct bug below produced the
identical symptom — an empty screen with a healthy-looking log — so the notes are
organised by *how to tell them apart*, not by what they are.

## The diagnostic ladder

Work down it. Each rung splits the remaining space roughly in half, and each was
needed at least once.

| Question | How to answer it | If no |
|---|---|---|
| Is the geometry in the pool? | `Voxel GPU pool:` log — live chunks, high water | Allocation or wiring bug, not rendering |
| Are the CPU tables sane? | `VoxelGpuPool upload:` — hidden/out-of-range ids, hidden entry scale | Chunk-id bookkeeping bug |
| Is the geometry where the camera is? | `VoxelGpuPool placement:` — component transform, first chunk, world bounds | Placement or rebase bug |
| Did the renderer ask for a draw? | `VoxelGpuPool draw SUBMITTED / SKIPPED` | **Relevance or culling** — the primitive never entered the visible set |
| Does it draw at small scale? | `voxel.Stream.GPUMaxChunks 64` | Scale-dependent (buffer size, draw size) |
| Does the same pool draw known-good data? | `voxel.GPU.SpawnPool 64 shot` in the same run | The pool is fine; the feed is wrong |

The last row is the strongest single move available: run the known-good test pool
**in the same process, same frame, same material** as the failing one. When the
test pool rendered and the streamed pool did not, that eliminated the pool, the
vertex factory, the material, the shader and the draw path in one screenshot.

## Invariants that are not obvious and not enforced by the compiler

**1. The pool must be the ROOT component of its own actor.**
Attached as a child of another component, the primitive never enters the visible
set — `GetDynamicMeshElements` is never called, with a live proxy, valid bounds
and no warning anywhere. As an actor root it draws. This is the single highest
cost-per-character fact in this document.

**2. `SetWorldLocation` AFTER `RegisterComponent`, never `SetRelativeLocation`
before.** `SetRootComponent` on a freshly `NewObject`ed component installs an
identity transform and redefines the actor's location as the world origin — which
in this world is 84 km from anything worth drawing.

**3. Bounds do not reach the renderer by themselves.**
`UpdateBounds()` updates the *component*. The scene keeps whatever bounds the
proxy was created with. On the incremental path the proxy is created once and
never rebuilt, so a pool that grows from 4 chunks to 3,000 is still culled
against the 6.4 m box it had on frame one. Push them with
`MarkRenderTransformDirty()` — **not** `MarkRenderStateDirty()`, which throws the
proxy away and rebuilds every buffer, defeating the entire point of incremental
upload.

**4. The chunk table is `float32` and this world is 84 km wide.**
At ~8.4M UU, float32's ULP is 1.0 UU against a 10 UU voxel. Chunk origins are
therefore stored relative to a rebase origin that the component carries in its
double-precision transform. Chunks stay within a couple of km of that point,
where the ULP is ~0.015 UU — three orders of magnitude of headroom. Set the
rebase once and never move it: re-basing a live pool means rewriting every entry
in the table.

**5. Sub-range writes require `EBufferUsageFlags::Static`, never `Dynamic`.**
This is the opposite of what "we update it every frame" suggests, and it is the
one that produced a *plausible* wrong image rather than a blank one. In the D3D12
RHI only the static lock path honours a lock offset: it stages exactly the locked
size and `CopyBufferRegion`s it to that offset (`D3D12Buffer.cpp:750, :801,
:818`). The dynamic path ignores the offset entirely and returns the buffer's
mapped base address (`:659`), then renames the whole buffer to a fresh upload
allocation on every lock after the first (`:667, :697`) — so everything outside
the range just written becomes uninitialised.

**6. The packed quad layout is a contract with the shader.**
`PackVoxelChunkQuad` (`VoxelMeshTypes.h`) and `DecodeVoxelQuadVertex`
(`VoxelQuadDecode.ush`) must agree bit for bit. The GPU mesher emits this layout
directly; the CPU packer is what lets CPU-meshed chunks share the pooled
renderer. Verify it by logging one packed word and decoding it by hand — the
fields are at fixed shifts, so this takes a minute and removes an entire
hypothesis.

## Testing lessons

**A test that cannot fail is worse than no test.** `voxel.GPU.SpawnPool`
originally did its add and its churn inside one console command, so the scene
proxy did not exist yet for any of it and every edit collapsed into a single
end-of-frame rebuild. The incremental upload path — the thing streaming leans on
constantly — never executed, and a wrong buffer usage flag passed verification
with a clean screenshot. `churnlive` defers the edits past proxy creation, which
is the difference between testing the code and testing its absence.

**Screenshot late.** The pool spawns into the live streamed world. A shot at 3 s
shows coarse half-loaded terrain *around* the pool that reads exactly like
corrupted pool geometry. One hour, chasing a bug that was not there. 10 s
minimum; 25–30 s for a full cascade.

**Prefer a control experiment to a bisect.** Twice, reasoning from the code led
to a confident wrong answer, and both times a control settled it in one run:
spawning a stock engine cube the same way (invisible too → the spawn was wrong,
not the mesh), and running the known-good pool beside the failing one.

## Environment

- **Pass the `.uproject` as an ABSOLUTE path** when launching headless. A
  relative path fails with exit code 1, no log, no crash report — indistinguishable
  from a broken build.
- Launching the editor needs the sandbox disabled; sandboxed it exits silently
  with no output and no log.
- Close the editor before building. It locks the DLL, and a failed link DELETES
  it, after which the project will not launch until a good build replaces it.
- **Discard the first run after any build.** Cold PSO state costs ~20% throughput
  and reads exactly like a regression.
- **Never A/B this renderer through scalability cvars.** `r.ShadowQuality 0`
  desyncs the BeginPlay PSO precache and measures precache invalidation instead.

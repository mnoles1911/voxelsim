# G3 — wiring the pool into the streaming cascade

The file-level plan for replacing per-chunk components with pool allocations.
Written 2026-07-25 from a read of `VoxelWorldSubsystem.cpp`, so the seams below
are where the code actually is, not where it ought to be.

**Prerequisite, already done:** `UVoxelGpuPoolComponent` holds many chunks in one
GPU buffer and draws them with ONE primitive and ONE draw call, with
`AddChunk`/`RemoveChunk` backed by the tested `FVoxelGpuGeometryPool`.
Demonstrated at 256 chunks / 876k quads, and under churn (remove 50, re-add 25,
75 live) with the primitive count constant at 1 throughout.

## The seams

| Function | Today | Becomes |
|---|---|---|
| `AcquireChunkComponent` (~5555) | pops `ComponentPool` or `NewObject` + `RegisterComponent` | `Pool->AddChunk(quads, origin, level)` → handle. No UObject per chunk. |
| `ReturnChunkComponentToPool` (~5578) | hides component, clears quads, pushes to pool | `Pool->RemoveChunk(handle)` |
| `ApplyMeshResult` (~5621) | **the real seam** — zero-quad branch frees; first-load branch acquires + positions; steady state calls `SetChunkQuads` | `RemoveChunk` / `AddChunk` / `UpdateChunk` |
| `DrainUnloads` (~5985) | `Rec->Component.IsValid()` gate, `ReturnChunkComponentToPool` | handle validity + `RemoveChunk` |
| `DrainResults`, `DrainGameThreadMesh` | only call `ApplyMeshResult` | **no structural change** |

`FChunkRecord`'s `TWeakObjectPtr<UVoxelChunkComponent> Component` becomes
`int32 PoolSlot = INDEX_NONE`. Every other field (`bMeshSettled`,
`RetainUntilSeconds`, `RetainReplaceDir`, `LastQuadCount`) is component-agnostic
and stays untouched. No GC concern either — the pool component is the only
UObject and is never destroyed per chunk.

**Chunk origin:** `VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level)`,
cast to `FVector3f`. Identical to what `SetRelativeLocation` uses today, provided
the pool component sits at the actor root with an identity relative transform —
otherwise subtract the pool's own offset first.

**Level:** already carried as `Key.Level`; pass it straight through.

**Cvar seam:** branch *inside* `ApplyMeshResult`, around the three
acquire/apply/release sites only. Everything upstream — `RecomputeDesiredSet`,
`DispatchJobs`, the apply budgets, retention and coverage — stays shared and
cvar-agnostic. That is exactly the "CPU desired-set stays authoritative" shape
G3 calls for.

**Threading:** the whole `DispatchJobs → DrainResults → DrainGameThreadMesh →
DrainUnloads` sequence is game-thread, so pool `Add`/`Remove`/`Update` are
game-thread only as required. Worker threads only produce quads into
`ResultsQueue`.

## Two API gaps to close first

1. **`UpdateChunk(handle, quads)`** — re-meshing a resident chunk (an edit)
   must update in place. Implementing it as free + realloc would fragment the
   allocator badly on actively-dug chunks, which are exactly the ones that
   re-mesh most.
2. **Incremental GPU upload.** Today the proxy rebuilds its buffers wholesale.
   At the live cascade's 9.4M quads that is 75 MB per change — unusable. Needs
   persistent buffers plus sub-range writes, per the `FRDGPooledBuffer` pattern
   in `docs/gpu-g2-draw-path.md` constraint 4.

## The three real risks

1. **Edited chunks.** The overlay re-mesh path (`DrainGameThreadMesh`) updates a
   component in place today. Without `UpdateChunk`, every dig fragments the pool.
2. **Load-before-unload retention.** Today "keep drawn" costs nothing — the
   component simply isn't returned. In the pool the retained chunk keeps its
   allocation *while its replacement takes another*, so a covered footprint is
   transiently double-allocated. Needs capacity headroom and strict ordering:
   allocate the replacement before freeing the retained one.
3. **Ring skirts at mip boundaries.** Skirt geometry crossing a chunk boundary
   must still resolve to the owning chunk's id in the shader lookup. Untested —
   G2 only ever proved static chunks.

## Fragmentation, measured

The allocator soak refuses ~10% of allocations at 64k-quad capacity with
realistic 600–2400 quad chunks, while still reporting plenty free. The live pool
is ~9.4M quads so the ratio should fall, but **G3 needs a compaction or eviction
policy** and this is the evidence for it. The number to watch is
`GetLargestFreeRun()` against `GetFreeQuads()`.

---

# Closing the climate gap (biome tint)

The GPU path renders with neutral biome tint because climate isn't available
GPU-side. Researched alongside the above.

`VoxelClimate::SampleClimateAtWorldUU` (`VoxelClimateProbe.cpp:139`) is a pure
function of world xy: a 4-tap bilinear over the climate raster, then a remap
through **global** constants (`kTempU8Lo/Hi`, `kPrecipU8Lo/Hi`) — one window for
the whole world, not per tile. Those same constants are baked into
`T_VoxelBiomeLUT`'s axes by `Tools/gen_terrain_textures.py`, so they must stay in
lockstep.

The raster is already on the GPU as `ClimatePacked` for `ColumnMain`, but two
things are missing there: it does a **nearest-pixel** read rather than bilinear,
and the remap constants don't exist in `worldgen.ush` at all.

**Recommendation: sample per chunk on the CPU and pass it alongside the origin.**
A chunk is 3.2 m across a 30 m raster cell — roughly 1/100th of a pixel's area —
and climate varies as a smooth bilinear ramp across it. The CPU path's own
comment already describes its per-quad sampling as ~10× oversampling the source.
Per-chunk is one step coarser on the same smooth ramp, so the error is a gentle
chunk-to-chunk gradient, not banding.

Implementation: a second SRV parallel to `ChunkOrigins`
(`StructuredBuffer<float2> ChunkClimate`, indexed by the chunk id the shader
already reads), 8 bytes per chunk, and three shader lines replacing the two
`0.5h` literals. `ChunkOrigins` is a fully-used `float4`, so this needs a new
buffer rather than a widened one.

Escalate only if boundary artifacts show: per-quad baked climate (~2–4 bytes per
quad, matches the CPU exactly), or full GPU sampling (exact, no per-quad memory,
but duplicates the bilinear and remap maths in a second place that must be kept
in lockstep with the CPU probe).

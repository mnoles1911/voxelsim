# Sparse environment storage

`voxel-core/include/voxelcore/sparseassetgrid.h` supplies the engine-free material grid. `VoxelEnvironmentSparseGrid.h` adapts it to the environment actor. Source VXA import, authoritative lookups, section meshing, derived LODs, carve edits, upper-layer reclamation, and snapshot capture/restore now use sparse storage in `VoxelEnvironmentLODPrototype.cpp`. This does **not** activate the separate production terrain/render ownership bridge.

## Coordinates and queries

Positive int32 dimensions describe a logical box. Signed int64 origins place local cell zero relative to the asset anchor; pitch is an exact integer number of micrometres. Chunks use zero-based local coordinates independently of origin. Out-of-bounds reads return air. The actor adapter retains its existing exact millimetre metadata and quarter-turn transform handling.

Only occupied 8-cubed chunks allocate material storage. Ordered chunk keys provide deterministic enumeration. Column runs merge matching adjacent materials across chunk boundaries. Box visits and occupied-section enumeration avoid scanning the entire logical volume. Reduction visits parent chunks touched by source chunks and uses `environmentReducedAtAnchor2`: ignore air, choose the most common solid material, break ties by lowest ID. Signed anchor grouping preserves alignment for odd negative origins. The separately named terrain reduction retains its threshold and surface-preservation modes.

## Budgets and mutation

Core edits and runs stage changed chunks before publication; range or budget failures leave the grid unchanged. Empty chunks are reclaimed. Resident and transaction chunk limits are independent. Each chunk contains 512 material bytes; map node and allocator overhead are additional, bounded in count but implementation-dependent. Caller-owned inputs are outside this accounting. Import constructs a temporary grid before publication.

The UE adapter accepts axes up to 16,384 cells and at most 131,072 chunks per grid (64 MiB of material payload). The complete hierarchy admits at most 262,144 chunks (128 MiB payload), 8,192 occupied sections, and 1,000,000 exposed faces. Hierarchy construction can temporarily hold the per-level maximum before aggregate admission. Face arrays contain approximately 296 bytes per face before capacity overhead, component conversion, GPU buffers, and temporary duplicate restore actors. These are bounded per-object admission limits, not a global world memory manager. Edits conservatively check worst-case new faces before committing. Sparse sources with large bounds can now be admitted, but arbitrary huge/dense sources are deliberately refused at these budgets.

In exception-enabled core builds, allocation failures return a failure result. In Unreal's exception-disabled configuration, explicit admission/range failures remain recoverable; actual allocator exhaustion follows the engine's fatal OOM policy. Invalid construction is observable through `valid()` when exceptions are disabled. Thread ownership/synchronization remains the caller's responsibility.

## Geometry wire format

Outer split geometry/dynamic snapshot version remains 1. Dynamic metadata still carries dimensions, origin, pitch, and upper-layer cutoff. The geometry body uses little-endian archive primitives:

1. `int32 -1`, `uint32 schema = 1`, `int32 chunkCount`.
2. For each chunk in strict lexicographic X/Y/Z order: three `int32` local chunk coordinates followed by 512 material bytes indexed `x + 8*y + 64*z`.

The decoder rejects unsupported versions, count/budget overflow, truncation, duplicate or unordered chunks, empty chunks, unknown material IDs, and occupied padding outside the logical shape. Reads are transactional. A nonnegative initial int32 is the old dense byte count; that legacy body retains the 64 MiB limit and Z-fastest indexing `(x*sizeY+y)*sizeZ+z`. Legacy dense bytes are streamed through a 512-byte buffer into sparse storage, then checked against the current sparse admission budgets. No dense allocation is required. New saves use only sparse bodies and omit derived LODs.

## Validation and remaining checks

Both focused core targets (`vxc_sparseassetgrid_tests` and `vxc_sparseassetgrid_noexceptions_tests`) pass, covering huge logical bounds, negative/limit origins, box/runs/reclamation, atomic budget failure, and reduction against dense reference policies. The no-exception target explicitly compiles with exception handling disabled. Log: `Saved/sparse-asset-grid-core-tests.log`.

`Voxel.Objects.EnvironmentSparseCodec` adds UE automation coverage for a 1,200-cubed logical grid, compact wire size, exact sparse roundtrip, signed origin lookup, upper reclamation, failed decode preserving data, and legacy dense compatibility. Its UE build/runtime verification is coordinated separately. Current VXA import still visits the source's XY column directory; cancellation is checked between staging phases/sections, not every chunk. The dedicated actor remains the integration surface; ordinary generated terrain assets are not automatically migrated.

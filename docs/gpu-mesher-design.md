# GPU greedy mesher — kernel design (M0 gate, ADR-0001)

The remaining GPU port. Hard part: greedy meshing emits a VARIABLE number of
quads per brick, and doctrine §2.3 demands bit-identical output — which rules
out the obvious "atomic counter bump-allocate" pattern (output order would
depend on GPU scheduling).

## Design (binding once implementation starts)

**Unit of work: one thread per face-mask.** A face-mask = (brick, axis, dir,
slice): an 8×8 occupancy/key grid. 6 dirs × 8 slices = 48 masks per brick.
Greedy rectangle merging within a mask is inherently serial — so let one
thread run the EXACT CPU algorithm (mesher.h: row-major scan, width-then-
height growth, material+AO in the merge key) over its own 8×8 mask. Small
fixed loops, no divergence across the mask set, and per-mask output order is
identical to CPU by construction.

**Determinism via count → scan → emit (no atomics in the output path):**
1. `MeshCountMain`: each mask-thread runs the greedy algorithm WITHOUT
   writing quads, outputs `quadCount[maskIndex]` (bounded ≤ 32).
2. Exclusive prefix scan over quadCount (deterministic tree scan; a simple
   two-kernel Blelloch over the mask array, or CPU-side scan for v1 — the
   count buffer is tiny: 48 × bricks entries).
3. `MeshEmitMain`: re-runs the greedy algorithm, writing quads at
   `scanOffset[maskIndex] + localIndex`.
   Redundant compute (run greedy twice) is cheaper than clever allocation
   and keeps both passes trivially deterministic. Total quad buffer size =
   scan total; overflow impossible by construction.

**Quad format**: pack the CPU `vxc::Quad` (axis, positive, slice, u0, v0, w,
h, ao, mat = 9 bytes) into 2×uint32; unpacked order must digest-match the CPU
quad stream: brick-major (dispatch order), then axis, dir, slice, then
row-scan emission order — which is exactly the CPU loop nesting.

**Apron/AO sampling**: mask visibility + AO read neighbor cells incl.
diagonals (±1 in all axes). Source = the VoxelizeMain cell buffer, meshing
only INTERIOR bricks of a voxelized region that includes a 1-brick halo
(mirrors the CPU flow where the apron sampler reads the same deterministic
function). Reads go through a helper indexing the brick-major cell layout;
out-of-region reads are a caller sizing bug (same defensive-clamp posture as
the raster reads in ColumnMain).

**Digest/verify**: harness meshes N regions on CPU (meshBrick per brick, same
brick order) and GPU; byte-compare quad streams; combined digest joins the
existing columns+cells digest. Gate metric: end-to-end columns→voxelize→mesh
wall-clock for the 128m radius on RTX-3060-class (<1s target; AMD leg first).

**Estimated cost**: 48 masks × 8×8 work ≈ 3k cell-visits per brick per pass,
×2 passes; at ~130k surface bricks (128m, 8³) ≈ 800M cell-visits — well
within budget next to the 1.4 Gcells/s voxelizer measurement.

## Deliberate non-goals (v1)
- No greedy merging across brick boundaries (CPU doesn't either).
- No shared-memory tiling until profiling demands it (masks read ~300 cells
  each from L2; correctness first).
- Scan on CPU first if it ships faster; move to GPU scan when the readback
  shows up in profiles.

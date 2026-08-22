# Resident brick volume — byte layout

**Status:** **PROVEN 2026-08-19.** Two independent implementations — `vxc::packChunkBricksCanonical`
(CPU reference) and `brickpack.ush` (GPU kernels) — were byte-compared over **16 chunks / 1,024 bricks
with 0 mismatches**, covering descriptors, occupancy, materials, the L1 mask and the 4³ masks, across
bpp 0/1/2/4/8 including uniform-air and uniform-solid. Three defects in this document were found by that
process and are corrected below (palette size, `allSolid` derivability, the skip mask's home).
**Consumers:** `brickpack.ush` (GPU producer), `vxc::packChunkBricksCanonical` (CPU reference),
`VoxelBrickTraverse.ush` (marcher), `VoxelBrickPool` (residency).
**This file is the contract.** Changing a field changes all four; say so in the commit.

---

## 0. The rule that shapes everything here

`vxc::Brick::paletteIndex` (`brick.h:114`) assigns palette slots **in first-seen order**. Two bricks
holding identical voxels in a different scan order therefore hold **different bytes**. A GPU producer
cannot reproduce that without reproducing CPU iteration order, and it should not try.

**So the resident format is CANONICAL: palette entries ascend by `MaterialId`.** That makes the packing
a pure function of brick *content*, which is what lets a GPU brick be byte-compared against a CPU one at
all. `vxc::packChunkBricksCanonical()` is the single definition; `vxc::Brick` stays exactly as it is and
is not the reference. This mirrors how `voxelcore/fluidoccupancy.h` relates to `VoxelFluidCollision.ush`.

## 1. Constants

| | |
|---|---|
| brick edge | 8 voxels (`kFluidBrickEdge`, `Brick<8>`) |
| cells per brick | 512 |
| chunk | 4x4x4 bricks = 32^3 voxels |
| cell index within a brick | `x + 8*(y + 8*z)` — **already canonical**, identical in `Brick<8>::cellIndex` (`brick.h:46`) and `fluidBrickBitIndex` (`fluidoccupancy.h:143`). Do not introduce a second order. |
| occupancy bit for cell i | word `i/32`, shift `i%32` (`fluidBrickWordOf`/`fluidBrickShiftOf`) |

## 2. `FVoxelBrickDesc` — 8 B, one per brick slot

```
uint OccWord;   // [0:27] dword offset into BrickOcc   [28:29] kind   [30] hasLocalPalette   [31] rsvd
uint MatWord;   // [0:27] dword offset into BrickMat   [28:31] bppCode
```

`kind`: `0` = uniform AIR, `1` = uniform SOLID (its material in `MatWord[0:7]`), `2` = MIXED.
`bppCode`: `0,1,2,4,8` bits per voxel. **`bppCode 0`** means a MIXED brick whose solid voxels are all one
material: local palette present, payload omitted entirely. (`MAT_WATERMARK`-only bricks and single-material
sparse bricks land here.) Powers of two only, so no payload entry straddles a dword.

**Uniform bricks carry no payload at all** — 8 bytes and nothing else. This is the whole reason the
format is affordable, and it is the direct analogue of `Brick::tryCollapse()` (`brick.h:80-91`). Both
sub-cases must work: all-air (`occupancy_.none()`) and all-solid (`Brick(fill)` sets occupancy when
`fill != MAT_AIR`, `brick.h:48-50`). A marcher skips either without a fetch.

## 3. `BrickOcc` — 64 B (16 dwords) per MIXED brick

512 bits, cell order as above. One 64 B load per entered mixed brick, then up to 22 DDA steps against
registers with **zero further memory traffic**. That property is why occupancy is separate from
materials rather than interleaved.

## 3a. The solidity predicate — and it is NOT the fluid one

§3 previously said "via `vxc::packBrickSolidBits`" and left the predicate implied. That function decides
solidity with **`isSolidForFluid`, which excludes `MAT_WATERMARK`** — correct for particles, and **wrong
for a marcher**: it would delete the water-marker debug instrument in exactly the mode someone enabled in
order to look at it. A silent, self-concealing failure.

**The render predicate is `isSolidForRender(m) = m != MAT_AIR`.** The CPU reference routes it *through*
`packBrickSolidBits` via a predicate shim so the bit walk itself stays literally shared — one definition
of the packing, two definitions of solidity, and the difference stated rather than inherited.

## 4. `BrickMat` — palette-indexed, compacted by occupancy

One entry per **solid** voxel in scan order. The index of a voxel is the **popcount of occupancy bits
below it** — at most 16 dwords, already in registers from §3.

A 16 B local palette (16 x `uint8` global `MaterialId`, ascending) precedes the payload when
`bppCode <= 4`. `bppCode == 8` means direct global ids and **no local palette**.

Typical mixed brick, ~256 solid voxels, 2-3 materials, 2 bpp:
`8 B desc + 64 B occ + 16 B palette + 64 B payload = 152 B`.

**CORRECTED 2026-08-19.** This example previously said "4 B palette … = 140 B", pricing the palette at
`ceil(paletteCount/4)` dwords while the normative sentence above says a fixed 16 B. Both P1 workstreams
hit the contradiction and implemented opposite things. **The fixed 16 B wins**: four bits index it
exactly, it is one 16 B load at a known offset, and it keeps arithmetic out of the inner loop. A
3-material 256-solid brick is **152 B**, not 140.

## 5. Acceleration — three levels, and only one of them is new

- **L2, chunk grid.** One dense toroidal grid per ring level, addressed exactly like
  `FVoxelFluidOccupancy`'s rolling window (`VoxelFluidOccupancy.h:31-49` — reuse that code shape; it is
  GPU-resident, unit-tested, and its `RecentreTo()` semantics are what ring recentring needs), plus a
  1-bit-per-chunk mask so the top-level DDA rejects a chunk cell with one bit.
- **L1, 64-bit brick mask in the chunk record.** "Which of my 64 bricks are non-empty." Skipping an
  empty brick then costs **zero memory traffic** — it is a bit test against a value already in a
  register. Built free by a group-reduce in the same pass as §3.
- **L0, the 16 occupancy dwords.** §3.

**The LOD pyramid is NOT new and must not be built.** `VoxelizeMain` already samples at
`coarseRep(z, CoarseScale)` (`worldgen.ush:2640-2642`), so ring levels R1..R5 *are* mip levels 1..5.
Porting `mips.h::downsampleBricks` to the GPU would create a second, divergent definition of a coarse
voxel, and `mips.h:11-16` warns the aggregation rule is worldgen-versioned and world-breaking to change.

*(Separate and genuinely additional: a MAX-aggregated 1-bit-per-brick occupancy mip chain for GI cone
marching. MAX, not `mips.h`'s >=4/8 majority — deliberate over-occlusion is the correct side to be wrong
on for a digging game, and that is an occlusion rule, not a render-LOD rule.)*

## 6. `FVoxelMarchChunk` — 32 B per resident chunk

```
int3  OriginVoxel;    // 12 B  chunk min corner, in LEVEL-L voxel coords
uint  LevelAndFlags;  //  4 B  [0:3] ring level  [4] anySolid  [5] allSolid
                      //         allSolid is NOT derivable from the 64 descriptors --
                      //         see the note below.
uint  BrickBase;      //  4 B  -> BrickDesc[BrickBase .. BrickBase+64)
uint  BrickSolidLo;   //  4 B  ) the 64-bit L1 mask
uint  BrickSolidHi;   //  4 B  )
uint  Pad;            //  4 B
```

**Brick order within a chunk is `bx + 4*(by + 4*bz)`.** Never previously stated. A z-major producer would
pass every per-brick test and fail only as a transposed world — the kind of defect that survives a unit
suite and dies in a screenshot. Both implementations independently chose this order; it is now pinned.

**Offsets in the descriptors are CHUNK-RELATIVE.** The pool adds a base, so a verify gate must either
dispatch with zero write bases or rebase before comparing; otherwise every descriptor differs by a
constant and the format looks broken.

**`allSolid` cannot be inferred from the brick descriptors, and it looks as though it can.** A brick may
be *fully solid* and still `kind == MIXED` — 512 solid cells holding two materials. So "all 64 descriptors
are uniform SOLID" is strictly stronger than `allSolid` and will under-report it. Compute it directly from
the cell data (the CPU reference does) or take it from there; do not derive it from the descriptor array.

**The 4³ intra-brick skip mask has no home in this document, deliberately.** §3 pins `BrickOcc` at exactly
64 B / 16 dwords — the census confirms it at 56.0 MiB = 917,504 × 64 B — so the mask cannot live there.
The GPU kernel emits it to its own buffer; the CPU reference argues it should be **derived, not resident**
(~7 MiB of VRAM to cache a handful of ORs over dwords already in registers). **Both agree on the bytes
that are compared**, so this is a free choice for P1-C. Recommendation: derive it, and spend the 7 MiB on
brick payload instead.

**`LevelAndFlags` must carry the ring level, and the table must be addressable by `(level, brickCoord)`.**
That is a one-line requirement now and a rewrite later: GI cone marching (Phase 7) steps *across* rings,
because rings are already 2x steps, and it can only do that if level is resident. The quad pool already
stores `Scale = float(1 << Level)` in `.w` for the vertex factory, so the information exists today.

## 6b. A trap for whoever writes the verify gate

The three write bases (`BrickDescBase` / `OccWriteBase` / `MatWriteBase`) follow the `QuadWriteBase`
pattern, and **a non-zero base lands in the descriptor's offset field**. So `voxel.Brick.VerifyBricks`
must dispatch with all three at zero, or it compares a *pool address* against a *chunk offset* and reports
a mismatch that is not one — the most expensive possible false positive, because it looks exactly like the
format being wrong.

## 7. What must be verified, and how

1. **`voxel.Brick.VerifyPack`** — read the GPU brick pool back and byte-compare against
   `packChunkBricksCanonical` over the same chunk. True byte identity. Shape it exactly like
   `voxel.GPU.VerifyPoolWrite` (direct + control + guard band).
2. **`voxel.March.VerifyQuads` — the stronger gate.** Regenerate the greedy quad stream *from the brick
   pool* and byte-compare against the shipped mesher for the same chunk. Both derive from the same
   `Cells` buffer, so **quad equality is volume equality**. No image, no runtime cost, and it proves the
   volume is the right world rather than merely a self-consistent one.

## 8. Halo

The mesher dispatches **48x48 columns x 6 bricks to produce 64** (`VoxelGpuMeshJobManager.h:107-114`)
because it needs a one-brick apron for AO and face-neighbour reads. **A marcher needs no apron**: face
normals come from the DDA's crossed axis, AO from occupancy bits already in registers, colour from the
voxel itself, and cross-brick reads go through the index. The dispatch becomes **32x32x4** —
**3.375x less voxelize work per chunk**.

This does not remove ring-boundary handling, which is a *traversal* concern, not a producer apron.

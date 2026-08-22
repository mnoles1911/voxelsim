#pragma once
// CPU reference packer for the resident brick volume the terrain ray marcher
// traverses (docs/brick-volume-format.md, docs/ray-marching-plan-2026-08-19.md
// Phase 1-A).
//
// WHAT LIVES HERE AND WHY. The shipping producer is a compute kernel
// (brickpack.ush) writing into a GPU pool that nothing on the CPU ever sees;
// the consumer is another compute kernel (VoxelBrickTraverse.ush). Neither can
// be looked at where it lives — the only instrument for either is to launch the
// editor and judge terrain, which is exactly the loop this project has been
// burned by. So this header is the engine-free definition of the packed layout:
// the bytes the producer must emit and the marcher must read, in a form
// vxc_tests can exercise and voxel.March.VerifyBricks can byte-compare a GPU
// readback against.
//
// It began as a REFERENCE ONLY, and as of 2026-08-19 IT IS ALSO A SHIPPING
// PRODUCER — the CPU mesh worker and the game-thread edit re-mesh call
// packChunkBricksCanonical per chunk (VoxelWorldSubsystem.cpp, namespace
// VoxelBrickCpuArm). That was not a change of mind about what this file is; it
// is the consequence of the GPU fork carrying only 5.2% of streaming traffic
// and of edited chunks never reaching the GPU at all, and it is exactly why the
// reference had to be byte-proven first: the shipping CPU path is now the same
// function voxel.GPU.VerifyBrickPack compared the GPU kernel against.
//
// TWO THINGS FOLLOW AND ARE EASY TO MISS. (1) This is now on the streaming path,
// so its COST is a shipping cost — it is timed per chunk and reported by
// voxel.Brick.Stats, and voxel.Brick.PackOnCpu 0 is the control arm. (2) A
// change here changes the world, not just a test: it is no longer true that
// "nothing in the runtime calls it".
//
// The relationship to voxelcore/fluidoccupancy.h and VoxelFluidCollision.ush is
// otherwise unchanged and deliberately the same shape: one header, one test
// file, every constant that could drift named here and quoted there.
//
// ---------------------------------------------------------------------------
// THE ONE THING THAT MAKES THIS FILE NECESSARY
// ---------------------------------------------------------------------------
//
// vxc::Brick::paletteIndex (brick.h:115) assigns palette slots in FIRST-SEEN
// order. Two bricks holding identical voxels written in a different scan order
// therefore hold different bytes. A GPU producer — 64 lanes writing a brick
// with no defined visitation order at all — cannot reproduce that without
// reproducing CPU iteration order, and it should not try.
//
// So the resident format is CANONICAL: a brick's palette entries ascend by
// MaterialId. That makes the packing a pure function of the brick's CONTENT,
// which is the only property under which a GPU brick can be byte-compared
// against a CPU one at all.
//
// vxc::Brick is NOT the reference and is not changed by any of this. It stays
// an insertion-ordered authoring container, which is the right thing for
// authoring; this file is the single definition of the resident bytes.
//
// ---------------------------------------------------------------------------
// FLOAT POLICY
// ---------------------------------------------------------------------------
//
// Integer-only, no exceptions. Bit indices, word indices, palette ranks and
// offsets never touch a float, per the library's determinism rules
// (docs/determinism.md). There is no float half here the way there is in
// fluidoccupancy.h — the traversal that would need one lives in HLSL.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES NOT DO
// ---------------------------------------------------------------------------
//
//  * It does not allocate from the resident pool. Offsets below are relative to
//    the START OF THIS CHUNK'S OWN arrays; the pool adds a per-chunk base, the
//    same way FVoxelMarchChunk::BrickBase rebases the descriptors (format §6).
//    voxel.March.VerifyBricks must rebase before comparing, or every offset
//    differs by a constant and the comparison fails on a non-difference.
//  * It does not build the LOD pyramid. Ring levels R1..R5 ARE mip levels 1..5
//    because VoxelizeMain already samples at coarseRep (format §5); a coarse
//    chunk is packed by calling this with a coarse material accessor, not by
//    downsampling a packed one.
//  * It does not fill FVoxelMarchChunk (format §6). Origin and ring level are
//    residency's, not the packer's — the packer is handed 32^3 materials and
//    has no idea where or at what scale they came from. What it CAN derive from
//    content it does: the 64-bit L1 mask and the anySolid/allSolid flags.

#include "voxelcore/core.h"
#include "voxelcore/fluidoccupancy.h"

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace vxc {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// The brick is 8^3 everywhere in this codebase and this is not a free choice:
// Brick<B> with B=8, VoxelCoords::BrickEdgeVoxels, kFluidBrickEdge and the
// mesher's unit of work all agree, and the occupancy block below is literally
// the fluid volume's 16-word block. Named from kFluidBrickEdge rather than
// re-spelled as 8 so a change there is a compile error here rather than a
// silent divergence.
inline constexpr int32_t kMarchBrickEdge = kFluidBrickEdge;
inline constexpr int32_t kMarchBrickCells =
    kMarchBrickEdge * kMarchBrickEdge * kMarchBrickEdge;          // 512
inline constexpr int32_t kMarchBrickOccWords = kFluidBrickWords;  // 16 dwords = 64 B

// 4x4x4 bricks per chunk = 32^3 voxels. Four because the L1 mask is then
// exactly 64 bits — one register, one bit test per skipped brick, zero memory
// traffic (format §5). That is the entire argument for the number.
inline constexpr int32_t kMarchChunkBricksPerAxis = 4;
inline constexpr int32_t kMarchChunkBricks =
    kMarchChunkBricksPerAxis * kMarchChunkBricksPerAxis * kMarchChunkBricksPerAxis; // 64
inline constexpr int32_t kMarchChunkEdgeVoxels =
    kMarchChunkBricksPerAxis * kMarchBrickEdge; // 32
static_assert(kMarchChunkBricks == 64,
              "the L1 brick-solid mask is a uint64_t and is sized from this");
static_assert(kMarchBrickCells == 512 && kMarchBrickOccWords * 32 == kMarchBrickCells,
              "format §3: 512 bits of occupancy in 16 dwords, exactly");

// The local palette is a FIXED 16 entries (16 x uint8 = 16 B = 4 dwords),
// present whenever bppCode <= 4, absent at bppCode 8 (format §4). Fixed rather
// than sized to the brick's actual palette because 4 bits index it exactly and
// a fixed stride means the marcher's palette fetch is one 16 B load at a known
// offset with no second-order arithmetic in the inner loop.
//
// NOTE FOR THE CONTRACT: format §4's worked example prices this at "4 B palette
// ... = 140 B", which is the VARIABLE-size reading of the same paragraph whose
// normative sentence says 16 B. This file implements the normative sentence, so
// a 3-material 256-solid brick is 152 B, not 140. A variable palette would save
// ~12 B on such a brick (~8%) and cost the fixed-stride property above; that is
// a contract decision, not a packer decision, and it is flagged rather than
// quietly taken.
inline constexpr int32_t kMarchLocalPaletteEntries = 16;
inline constexpr int32_t kMarchLocalPaletteWords = kMarchLocalPaletteEntries / 4; // 4

// ---------------------------------------------------------------------------
// Solidity — and the one material where this predicate is NOT the fluid one
// ---------------------------------------------------------------------------
//
// For the marcher, "solid" means "has a colour and stops a ray". That is every
// material except air, INCLUDING MAT_WATERMARK.
//
// isSolidForFluid (fluidoccupancy.h:127) deliberately excludes MAT_WATERMARK,
// and it is right to: the marker is a debug instrument that puts solid voxels
// where the bake says water is, so treating it as ground would wall off every
// river the moment -VoxelWaterMarker=1 is passed. But that argument is about
// what stops a PARTICLE. The marker exists to be LOOKED AT — it is a material
// rather than a render mode precisely so that it inherits the terrain path,
// view distance and the LOD chain (core.h:509-520) — so a marcher that skipped
// it would delete the instrument in the exact mode somebody turned on to use it.
//
// The two predicates therefore differ on exactly one material, in opposite
// directions, both on purpose. They are separate names for that reason and
// neither is defined in terms of the other.
constexpr bool isSolidForRender(MaterialId m) { return m != MAT_AIR; }

// ---------------------------------------------------------------------------
// Cell and brick ordering
// ---------------------------------------------------------------------------
//
// Cell order inside a brick is x + 8*(y + 8*z) and is ALREADY canonical: it is
// Brick<8>::cellIndex (brick.h:46) and fluidBrickBitIndex (fluidoccupancy.h:143)
// character for character, and three subsystems share it. Nothing here
// introduces a second order — the occupancy pack below calls packBrickSolidBits
// itself rather than re-deriving the walk.
//
// Brick order inside a chunk is the same shape one level up: bx + 4*(by + 4*bz).
// This is what BrickDesc[BrickBase .. BrickBase+64) is indexed by and what bit N
// of the L1 mask means; format §6 does not spell it out, and it must, because a
// producer that numbered bricks z-major would pass every per-brick test and fail
// only as a transposed world.
constexpr int32_t chunkBrickIndex(int32_t bx, int32_t by, int32_t bz) {
    return bx + kMarchChunkBricksPerAxis * (by + kMarchChunkBricksPerAxis * bz);
}

// ---------------------------------------------------------------------------
// FVoxelBrickDesc — 8 B, one per brick slot
// ---------------------------------------------------------------------------
//
//   uint OccWord;   // [0:27] dword offset into BrickOcc  [28:29] kind
//                   // [30] hasLocalPalette  [31] reserved
//   uint MatWord;   // [0:27] dword offset into BrickMat  [28:31] bppCode
//
// Named BrickDesc here and FVoxelBrickDesc on the UE side; voxel-core is
// UE-header-free by doctrine, so the F-prefixed mirror lives with the shader.
//
// UNIFORM BRICKS CARRY NO PAYLOAD AT ALL — 8 bytes and nothing else. This is the
// whole reason the format is affordable: the volume census measured 67.1% of
// slots collapsing, worth ~250 MiB of a 404 MiB cascade, so it is the single
// most load-bearing behaviour in the file. It is the direct analogue of
// Brick::tryCollapse (brick.h:80-91) and both of tryCollapse's sub-cases must
// work — all-air (occupancy_.none()) and all-solid (Brick(fill) sets occupancy
// when fill != MAT_AIR). A marcher skips either without a fetch.
//
// For kind == kBrickUniformSolid the offset field is REPURPOSED: bits [0:7] of
// MatWord hold the material, and there is no BrickMat block to point at.
// For kind == kBrickUniformAir the whole descriptor is eight zero bytes, which
// is a property worth having on purpose — a cleared pool reads as empty world
// rather than as garbage.
enum BrickKind : uint32_t {
    kBrickUniformAir = 0,
    kBrickUniformSolid = 1,
    kBrickMixed = 2,
};

inline constexpr uint32_t kBrickOffsetBits = 28;
// 268M dwords = 1 GiB of BrickOcc or BrickMat addressable from one descriptor.
inline constexpr uint32_t kBrickOffsetMask = (1u << kBrickOffsetBits) - 1u;
inline constexpr uint32_t kBrickKindShift = 28;
inline constexpr uint32_t kBrickHasPaletteBit = 30;
inline constexpr uint32_t kBrickBppShift = 28;

struct BrickDesc {
    uint32_t OccWord = 0;
    uint32_t MatWord = 0;

    uint32_t kind() const { return (OccWord >> kBrickKindShift) & 3u; }
    bool hasLocalPalette() const { return ((OccWord >> kBrickHasPaletteBit) & 1u) != 0u; }
    uint32_t occDwordOffset() const { return OccWord & kBrickOffsetMask; }

    // Bits per voxel in the material payload: 0, 1, 2, 4 or 8. The VALUE is the
    // bit count, not an enumerator index — 8 fits the 4-bit field, and a marcher
    // that wants a shift wants the number itself.
    uint32_t bppCode() const { return (MatWord >> kBrickBppShift) & 0xFu; }
    uint32_t matDwordOffset() const { return MatWord & kBrickOffsetMask; }

    // Only meaningful when kind() == kBrickUniformSolid.
    MaterialId uniformMaterial() const { return static_cast<MaterialId>(MatWord & 0xFFu); }

    friend bool operator==(const BrickDesc&, const BrickDesc&) = default;
};
static_assert(sizeof(BrickDesc) == 8, "format §2: 8 bytes per brick slot, exactly");

// bppCode from the brick's own palette size (the count of DISTINCT SOLID
// materials in the brick). Adaptive, and adaptive is worth it: the census over
// 916,901 real mixed bricks measured mean 3.73 materials, max 8, nothing above
// 8 — 15.0% at 2, 62.7% at 3-4, 22.3% at 5-8 — so a fixed 8 bpp would pay 4x
// for the common case and a fixed 4 bpp would pay 2x. Measured 2.01 bpp at R0
// rising to 2.63 at R5.
//
// The ladder is powers of two only, and that is a requirement rather than
// tidiness: 1, 2, 4 and 8 all divide 32, so no payload entry ever straddles a
// dword boundary and the marcher's read is one load, one shift, one mask.
//
// bppCode 0 is the degenerate case the format lists but does not explain: a
// MIXED brick (some air, some solid) whose solid voxels are all ONE material.
// Its palette has one entry, so the payload would carry no information and is
// not emitted — the local palette's entry 0 is the answer for every solid
// voxel. This is the common shape at a flat surface and it is free.
//
// bpp 8 is reachable but rare (max observed palette is exactly 8, at which 4
// bpp still suffices); it exists so that a brick the census never saw is
// packed correctly rather than clamped into a wrong colour.
constexpr uint32_t brickBppCodeFor(int32_t paletteSize) {
    if (paletteSize <= 1) return 0u;
    if (paletteSize <= 2) return 1u;
    if (paletteSize <= 4) return 2u;
    if (paletteSize <= kMarchLocalPaletteEntries) return 4u;
    return 8u; // direct global MaterialIds, no local palette
}

// Payload dwords for `solidCount` entries at `bpp` bits each, rounded up to a
// whole dword. Zero at bpp 0 by construction.
constexpr int32_t brickMatPayloadWords(int32_t solidCount, uint32_t bpp) {
    const int64_t bits = static_cast<int64_t>(solidCount) * static_cast<int64_t>(bpp);
    return static_cast<int32_t>((bits + 31) / 32);
}

// ---------------------------------------------------------------------------
// BrickMat — palette-indexed, COMPACTED BY OCCUPANCY
// ---------------------------------------------------------------------------
//
// One entry per SOLID voxel in cell order. A voxel's entry index is the
// POPCOUNT OF OCCUPANCY BITS BELOW IT — at most 16 dwords, and the marcher
// already holds all 16 in registers from its one 64 B occupancy load (format
// §3), so the rank costs no memory traffic whatsoever.
//
// Compaction is not a micro-optimisation. At the census's mean it is the
// difference between paying for 512 entries and paying for the ~256 that exist,
// i.e. half the material bytes of the whole volume. It is also the single
// easiest thing in this format to get subtly wrong: index by CELL instead of by
// RANK and a dense brick still looks perfect (rank == cell exactly when every
// bit below is set) while a sparse one takes every voxel's colour from some
// other voxel. tests/test_brickpack.cpp tests a deliberately sparse brick for
// exactly this reason.
constexpr int32_t solidRankBelow(const uint32_t* occWords, int32_t bitIndex) {
    const int32_t w = fluidBrickWordOf(bitIndex);
    const int32_t s = fluidBrickShiftOf(bitIndex);
    int32_t rank = 0;
    for (int32_t i = 0; i < w; ++i) rank += std::popcount(occWords[i]);
    // Shifting a uint32 by 32 is UB, hence the explicit s == 0 case rather than
    // a mask built as ((1u << s) - 1) with s free.
    if (s != 0) rank += std::popcount(occWords[w] & ((1u << s) - 1u));
    return rank;
}

// ---------------------------------------------------------------------------
// The 4^3 intra-brick skip mask
// ---------------------------------------------------------------------------
//
// One bit per 2x2x2 cell group, bit cx + 4*(cy + 4*cz), set if ANY of the eight
// cells is solid. 64 bits, so it is a uint64_t in a register, derived from the
// 16 occupancy dwords the marcher already has.
//
// DERIVED, NOT RESIDENT. It is deliberately not in BrickDesc and not in the
// byte stream: it is a pure function of the occupancy block, so storing it
// would add 8 B per mixed brick (~7 MiB across the census's 917k bricks) to
// cache something a shader rebuilds with a handful of ORs. It is produced here
// because a DDA that wants to step two voxels at a time through the empty parts
// of a mixed brick needs it, and because a GPU implementation of that reduction
// has to be checkable against something.
constexpr uint64_t brickCoarse4Mask(const uint32_t* occWords) {
    uint64_t mask = 0;
    for (int32_t cz = 0; cz < 4; ++cz)
        for (int32_t cy = 0; cy < 4; ++cy)
            for (int32_t cx = 0; cx < 4; ++cx) {
                bool any = false;
                for (int32_t dz = 0; dz < 2 && !any; ++dz)
                    for (int32_t dy = 0; dy < 2 && !any; ++dy)
                        for (int32_t dx = 0; dx < 2 && !any; ++dx)
                            any = brickSolidBit(occWords, cx * 2 + dx, cy * 2 + dy,
                                                cz * 2 + dz);
                if (any) mask |= uint64_t(1) << (cx + 4 * (cy + 4 * cz));
            }
    return mask;
}

// ---------------------------------------------------------------------------
// The packed chunk
// ---------------------------------------------------------------------------
//
// Three arrays, and they are separate rather than interleaved for the reason
// format §3 gives: one 64 B occupancy load per entered mixed brick, then up to
// 22 DDA steps against registers with ZERO further memory traffic. Interleaved
// materials would drag payload bytes through the cache on every empty step.
struct ChunkBrickPack {
    // 64 descriptors, always, in chunkBrickIndex order — including the collapsed
    // ones, whose slots must exist so BrickBase + brickIndex stays addressable
    // without an indirection.
    std::array<BrickDesc, kMarchChunkBricks> descs{};

    // 16 dwords per MIXED brick, appended in ascending brick index.
    std::vector<uint32_t> occ;

    // Per MIXED brick, appended in ascending brick index: the 4-dword local
    // palette (bppCode <= 4 only) followed by the payload.
    std::vector<uint32_t> mat;

    // L1, format §5: "which of my 64 bricks are non-empty", bit chunkBrickIndex.
    // Built free in the same pass. Skipping an empty brick then costs zero
    // memory traffic — it is a bit test against a value already in a register.
    uint64_t brickSolid = 0;

    // Derived, NOT resident. See brickCoarse4Mask. Uniform-air bricks are 0 and
    // uniform-solid bricks are all ones, so a caller need not special-case kind
    // before consulting it.
    std::array<uint64_t, kMarchChunkBricks> brickCoarse{};

    // FVoxelMarchChunk::LevelAndFlags bits [4] and [5] (format §6). allSolid
    // means every one of the 32,768 voxels is non-air, not merely that every
    // brick is non-empty.
    bool anySolid = false;
    bool allSolid = false;

    // Resident bytes for this chunk: descriptors always, plus whatever the
    // mixed bricks needed. The number the census is checked against.
    int64_t residentBytes() const {
        return static_cast<int64_t>(kMarchChunkBricks) * 8 +
               static_cast<int64_t>(occ.size()) * 4 +
               static_cast<int64_t>(mat.size()) * 4;
    }
};

namespace detail {

// Writes `value` into the `index`-th bpp-bit slot of a dword run. bpp is 1, 2, 4
// or 8, all of which divide 32, so a slot never straddles a dword — see
// brickBppCodeFor. LSB-first within the dword, which makes the packing a
// property of the DWORD STREAM and not of the host's byte order; the GPU sees
// dwords, so that is the level the contract has to be stated at.
inline void writePayloadSlot(uint32_t* words, int32_t index, uint32_t bpp, uint32_t value) {
    const int32_t perWord = static_cast<int32_t>(32u / bpp);
    const int32_t w = index / perWord;
    const uint32_t shift = static_cast<uint32_t>(index % perWord) * bpp;
    words[w] |= (value << shift);
}

inline uint32_t readPayloadSlot(const uint32_t* words, int32_t index, uint32_t bpp) {
    const int32_t perWord = static_cast<int32_t>(32u / bpp);
    const int32_t w = index / perWord;
    const uint32_t shift = static_cast<uint32_t>(index % perWord) * bpp;
    const uint32_t mask = (1u << bpp) - 1u;
    return (words[w] >> shift) & mask;
}

} // namespace detail

// ---------------------------------------------------------------------------
// The packer
// ---------------------------------------------------------------------------
//
// MaterialFn: MaterialId(int32_t x, int32_t y, int32_t z) over CHUNK-LOCAL
// coordinates 0..31. Callers pass whatever they have — World::materialAt with
// the chunk origin added, a Cells readback, a flat array — exactly as
// packBrickSolidBits does, and for the same reason: the packer must not know
// where the chunk is or what ring level it belongs to, because a coarse chunk is
// packed by handing it a coarse accessor and nothing else changes.
template <typename MaterialFn>
ChunkBrickPack packChunkBricksCanonical(const MaterialFn& materialAt) {
    ChunkBrickPack pack;
    pack.allSolid = true;

    for (int32_t bz = 0; bz < kMarchChunkBricksPerAxis; ++bz) {
        for (int32_t by = 0; by < kMarchChunkBricksPerAxis; ++by) {
            for (int32_t bx = 0; bx < kMarchChunkBricksPerAxis; ++bx) {
                const int32_t brick = chunkBrickIndex(bx, by, bz);
                const int32_t ox = bx * kMarchBrickEdge;
                const int32_t oy = by * kMarchBrickEdge;
                const int32_t oz = bz * kMarchBrickEdge;

                // Brick-local accessor. Everything below reads through this, so
                // the chunk offset is applied in exactly one place.
                const auto cell = [&](int32_t x, int32_t y, int32_t z) -> MaterialId {
                    return materialAt(ox + x, oy + y, oz + z);
                };

                // --- occupancy ---------------------------------------------
                //
                // Through packBrickSolidBits, not a re-derived walk, so the bit
                // order is the shared one by construction rather than by
                // agreement. It decides solidity with isSolidForFluid, which is
                // NOT this file's predicate (see isSolidForRender), so it is
                // handed a material that agrees with ours: every render-solid
                // material is presented to it as MAT_ROCK, which every solidity
                // predicate in the codebase calls solid. The substitution
                // changes the PREDICATE and nothing else — the bit walk, the
                // word and the shift are all still fluidoccupancy.h's.
                uint32_t occWords[kMarchBrickOccWords] = {};
                packBrickSolidBits(
                    [&](int32_t x, int32_t y, int32_t z) -> MaterialId {
                        return isSolidForRender(cell(x, y, z)) ? MAT_ROCK : MAT_AIR;
                    },
                    occWords);

                int32_t solidCount = 0;
                for (int32_t w = 0; w < kMarchBrickOccWords; ++w)
                    solidCount += std::popcount(occWords[w]);

                pack.brickCoarse[static_cast<size_t>(brick)] = brickCoarse4Mask(occWords);

                // --- uniform collapse, BOTH cases ---------------------------
                //
                // Mirrors Brick::tryCollapse (brick.h:80-91), which collapses
                // whenever all 512 cells hold one palette index — and that is
                // true of all-air and of all-solid-one-material alike. Neither
                // emits a single byte of payload.
                if (solidCount == 0) {
                    // Uniform AIR. The descriptor stays eight zero bytes.
                    pack.allSolid = false;
                    continue;
                }

                // Palette: the distinct SOLID materials, ASCENDING. A presence
                // array rather than a sort, because the material set is small,
                // fixed and dense, and "ascending" then falls out of the
                // iteration order with no comparator to get wrong and no
                // dependence on visitation order — which is the entire point of
                // this file.
                //
                // 256 entries and not kMaterialCount: MaterialId is a uint8_t
                // and core.h:517-521 records that an asset built from an id past
                // the end of the enum "does not fault" but indexes past every
                // material-keyed array — which was the live state of nine of
                // thirteen asset materials. A packer is the wrong place to turn
                // that class of upstream bug into memory corruption, and 768 B
                // of stack per brick buys immunity to it.
                bool seen[256] = {};
                for (int32_t z = 0; z < kMarchBrickEdge; ++z)
                    for (int32_t y = 0; y < kMarchBrickEdge; ++y)
                        for (int32_t x = 0; x < kMarchBrickEdge; ++x) {
                            const MaterialId m = cell(x, y, z);
                            if (isSolidForRender(m)) seen[m] = true;
                        }
                MaterialId palette[256] = {};
                uint8_t slotOf[256] = {};
                int32_t paletteSize = 0;
                for (int32_t m = 0; m < 256; ++m) {
                    if (!seen[m]) continue;
                    slotOf[m] = static_cast<uint8_t>(paletteSize);
                    palette[paletteSize++] = static_cast<MaterialId>(m);
                }

                pack.brickSolid |= uint64_t(1) << brick;
                pack.anySolid = true;

                if (solidCount == kMarchBrickCells && paletteSize == 1) {
                    // Uniform SOLID. Material in MatWord[0:7]; no occupancy
                    // block, no material block, no local palette.
                    BrickDesc& d = pack.descs[static_cast<size_t>(brick)];
                    d.OccWord = kBrickUniformSolid << kBrickKindShift;
                    d.MatWord = palette[0];
                    continue;
                }
                if (solidCount != kMarchBrickCells) pack.allSolid = false;

                // --- MIXED --------------------------------------------------
                const uint32_t occOffset = static_cast<uint32_t>(pack.occ.size());
                pack.occ.insert(pack.occ.end(), occWords, occWords + kMarchBrickOccWords);

                const uint32_t bpp = brickBppCodeFor(paletteSize);
                const bool localPalette = (bpp <= 4u);
                const uint32_t matOffset = static_cast<uint32_t>(pack.mat.size());

                if (localPalette) {
                    // 16 x uint8, ascending, four per dword LSB-first. Unused
                    // entries are zero, i.e. MAT_AIR, which cannot appear in a
                    // palette — so the filler is unambiguous and the bytes are
                    // reproducible, which is what byte-comparison needs.
                    uint32_t pal[kMarchLocalPaletteWords] = {};
                    for (int32_t i = 0; i < paletteSize; ++i)
                        pal[i / 4] |= static_cast<uint32_t>(palette[i]) << (8 * (i % 4));
                    pack.mat.insert(pack.mat.end(), pal, pal + kMarchLocalPaletteWords);
                }

                const int32_t payloadWords = brickMatPayloadWords(solidCount, bpp);
                const size_t payloadBase = pack.mat.size();
                pack.mat.resize(payloadBase + static_cast<size_t>(payloadWords), 0u);

                if (bpp != 0u) {
                    uint32_t* payload = pack.mat.data() + payloadBase;
                    for (int32_t z = 0; z < kMarchBrickEdge; ++z)
                        for (int32_t y = 0; y < kMarchBrickEdge; ++y)
                            for (int32_t x = 0; x < kMarchBrickEdge; ++x) {
                                const MaterialId m = cell(x, y, z);
                                if (!isSolidForRender(m)) continue;
                                const int32_t bit = fluidBrickBitIndex(x, y, z);
                                const int32_t rank = solidRankBelow(occWords, bit);
                                // bpp 8 is DIRECT global ids and has no local
                                // palette to index; anything narrower stores the
                                // ascending local slot.
                                const uint32_t value =
                                    (bpp == 8u) ? static_cast<uint32_t>(m) : slotOf[m];
                                detail::writePayloadSlot(payload, rank, bpp, value);
                            }
                }

                BrickDesc& d = pack.descs[static_cast<size_t>(brick)];
                d.OccWord = (kBrickMixed << kBrickKindShift) | occOffset |
                            (localPalette ? (1u << kBrickHasPaletteBit) : 0u);
                d.MatWord = (bpp << kBrickBppShift) | matOffset;
            }
        }
    }

    if (!pack.anySolid) pack.allSolid = false;
    return pack;
}

// ---------------------------------------------------------------------------
// The reference DECODER
// ---------------------------------------------------------------------------
//
// The scalar form of what VoxelBrickTraverse.ush does when a ray stops: rank the
// cell's occupancy bit, read the payload slot, resolve through the local
// palette. It lives here rather than in the test because a format is only half
// specified by its producer — "what the bytes are" is not "how to read them" —
// and because the shader author needs one correct reading to mirror.
//
// Returns MAT_AIR for an empty cell.
inline MaterialId decodeVoxelCanonical(const ChunkBrickPack& pack, int32_t brickIndex,
                                       int32_t x, int32_t y, int32_t z) {
    const BrickDesc& d = pack.descs[static_cast<size_t>(brickIndex)];
    if (d.kind() == kBrickUniformAir) return MAT_AIR;
    if (d.kind() == kBrickUniformSolid) return d.uniformMaterial();

    const uint32_t* occWords = pack.occ.data() + d.occDwordOffset();
    if (!brickSolidBit(occWords, x, y, z)) return MAT_AIR;

    const uint32_t* matBlock = pack.mat.data() + d.matDwordOffset();
    const uint32_t bpp = d.bppCode();
    const uint32_t* payload =
        matBlock + (d.hasLocalPalette() ? kMarchLocalPaletteWords : 0);

    if (bpp == 0u) {
        // One material for every solid voxel; the payload was never emitted, so
        // local palette entry 0 IS the answer.
        return static_cast<MaterialId>(matBlock[0] & 0xFFu);
    }

    const int32_t rank = solidRankBelow(occWords, fluidBrickBitIndex(x, y, z));
    const uint32_t value = detail::readPayloadSlot(payload, rank, bpp);
    if (!d.hasLocalPalette()) return static_cast<MaterialId>(value); // bpp 8: direct
    return static_cast<MaterialId>((matBlock[value / 4] >> (8 * (value % 4))) & 0xFFu);
}

// Chunk-local (0..31) convenience form: finds the brick, then decodes.
inline MaterialId decodeChunkVoxelCanonical(const ChunkBrickPack& pack, int32_t x,
                                            int32_t y, int32_t z) {
    const int32_t brick =
        chunkBrickIndex(x / kMarchBrickEdge, y / kMarchBrickEdge, z / kMarchBrickEdge);
    return decodeVoxelCanonical(pack, brick, x % kMarchBrickEdge, y % kMarchBrickEdge,
                                z % kMarchBrickEdge);
}

} // namespace vxc

#pragma once
// THE CRAFT LATTICE -- player-authored sub-voxel detail at 25 mm, bounded to
// deliberately PROMOTED terrain bricks.
//
// The design document is the plan "The craft lattice -- 2.5 cm sub-voxel
// building". This header is the STATE and the PROJECTION; craftvolume.h is the
// packing half, exactly as covervolume.h is the packing half of the cover
// accessor.
//
// ---------------------------------------------------------------------------
// WHY IT IS NOT CALLED "FINE"
// ---------------------------------------------------------------------------
// That word is taken: FVoxelFineTileStreamer, VoxelFineLockMeter.h,
// EStage::FineResidency, -VoxelFineTileDir= and
// docs/fine-bake-production-architecture.md all mean the baked .vxtl v2 2-D
// ELEVATION tier, an unrelated thing. Two systems called "fine" are
// indistinguishable in every grep and every log line. This one is "craft".
//
// ---------------------------------------------------------------------------
// THE UNIT OF PROMOTION IS THE 8^3 TERRAIN BRICK (80 cm), AND THE FORMAT CHOSE IT
// ---------------------------------------------------------------------------
// A craft chunk is 32 cells per axis (kMarchChunkEdgeVoxels, brickpack.h) and a
// craft cell is 25 mm, so a craft chunk is 800 mm -- which is exactly
// 8 * kVoxelSizeMm, i.e. ONE TERRAIN BRICK. So
//
//     craftChunkKeyOfCell(c) == ChunkMap<8>::keyForVoxel(voxelOfCraftCell(c))
//
// One key space, no mapping table, and an edit dirty set maps 1:1. The
// supersede bit the marcher needs is then one bit per brick, which is the shape
// the chunk record already has room for. Per-VOXEL promotion would need 32768
// bits per chunk and the record has 192 spare; per-BRICK needs 64.
//
// ---------------------------------------------------------------------------
// PROMOTION MATERIALISES ALL 64 CRAFT BRICKS, AND THAT IS NOT A PERFORMANCE
// CHOICE -- IT IS WHAT BREAKS A CIRCULARITY
// ---------------------------------------------------------------------------
// The tempting design is "a craft cell with no overlay entry reads its parent
// terrain voxel". But the parent terrain voxel of a promoted brick holds the
// PROJECTION, which is computed FROM the craft cells. Read one through the
// other and the definition is circular: after a carve, the projection feeds
// back into the cells it was derived from.
//
// So promote() expands the terrain brick into all 64 craft bricks up front, and
// inside a promoted brick there is never a fallback. Uniform regions collapse
// to homogeneous bricks, so the cost is small and the definition is acyclic.
//
// ---------------------------------------------------------------------------
// A MISSING CRAFT BRICK IS A REFUSAL, NEVER AIR
// ---------------------------------------------------------------------------
// This project has paid repeatedly for "absence reads as air" -- a missing fine
// tile returning sea level, a missing tile counting as provably empty -- and
// each time the result was DELETED TERRAIN with every counter healthy. So
// project() returns false and counts the refusal if any of the 64 craft bricks
// of a promoted terrain brick is absent. It does not hand downsampleBricks a
// null child and let the parent cell quietly stay MAT_AIR.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "voxelcore/brick.h"
#include "voxelcore/brickpack.h"
#include "voxelcore/chunkmap.h"
#include "voxelcore/core.h"
#include "voxelcore/hash.h"
#include "voxelcore/mips.h"

namespace vxc {

// ---------------------------------------------------------------------------
// Constants -- every one derived, none spelled twice
// ---------------------------------------------------------------------------

// 25 mm. Chosen because it divides kVoxelSizeMm by a POWER OF TWO: the marcher
// rescales a ray by 1/2^k per lattice (cover uses 1/2), the brick and chunk
// nesting has to stay aligned, and mips.h's 2x fold is the projection. 20 mm
// divides 100 mm too, but by 5, and none of those three properties survives it.
inline constexpr int32_t kCraftPitchMm = 25;

inline constexpr int32_t kCraftCellsPerVoxel = kVoxelSizeMm / kCraftPitchMm; // 4
static_assert(kVoxelSizeMm % kCraftPitchMm == 0,
              "the craft lattice must TILE the terrain lattice exactly");
static_assert(kCraftCellsPerVoxel == 4,
              "4 = 2^2, so the projection is exactly two mips.h folds and the "
              "marcher ray rescale is 1/4");

// The craft chunk is the march chunk's shape on the craft lattice. Named here
// rather than assumed so the two cannot drift silently -- covervolume.h's
// kCoverChunkEdgeCells does the same for its own pitch.
inline constexpr int32_t kCraftChunkEdgeCells = kMarchChunkEdgeVoxels; // 32

// Craft bricks per axis inside one craft chunk, and therefore inside one
// promoted terrain brick.
inline constexpr int32_t kCraftBricksPerAxis = kMarchChunkBricksPerAxis; // 4
inline constexpr int32_t kCraftBricksPerChunk = kMarchChunkBricks;       // 64

// Terrain voxels covered by one craft brick, per axis: 8 craft cells / 4 = 2.
inline constexpr int32_t kVoxelsPerCraftBrick = kMarchBrickEdge / kCraftCellsPerVoxel; // 2

// mips.h 2x folds needed to take a craft chunk down to one terrain brick:
// 32 craft cells -> 16 -> 8. Two.
inline constexpr int32_t kCraftProjectionFolds = 2;
static_assert((1 << kCraftProjectionFolds) == kCraftCellsPerVoxel,
              "each fold halves the cell count per axis");

// ---------------------------------------------------------------------------
// Coordinate transforms -- ONE spelling each
// ---------------------------------------------------------------------------

// Terrain voxel -> the min-corner craft cell of its kCraftCellsPerVoxel^3 group.
constexpr int64_t craftCellOfVoxelMin(int64_t v) { return v * kCraftCellsPerVoxel; }

// Craft cell -> the terrain voxel containing it. floorDiv, not '/': worlds sit
// at negative coordinates and C++ division truncates toward zero.
constexpr int64_t voxelOfCraftCell(int64_t c) {
    return floorDiv(c, static_cast<int64_t>(kCraftCellsPerVoxel));
}

// Craft cell -> the craft brick (8 craft cells = 20 cm) holding it.
inline BrickKey craftBrickKeyOfCell(int64_t cx, int64_t cy, int64_t cz) {
    return ChunkMap<kMarchBrickEdge>::keyForVoxel(cx, cy, cz);
}

// Craft cell -> the craft CHUNK, which is the SAME key as the terrain brick.
inline BrickKey craftChunkKeyOfCell(int64_t cx, int64_t cy, int64_t cz) {
    const int64_t e = kCraftChunkEdgeCells;
    return BrickKey{static_cast<int32_t>(floorDiv(cx, e)),
                    static_cast<int32_t>(floorDiv(cy, e)),
                    static_cast<int32_t>(floorDiv(cz, e))};
}

// The min-corner craft BRICK coordinate of a promoted terrain brick. Terrain
// brick K spans craft cells [K*32, K*32+32), i.e. craft bricks [K*4, K*4+4).
inline BrickKey craftBrickBaseOfTerrainBrick(const BrickKey& k) {
    return BrickKey{k.x * kCraftBricksPerAxis, k.y * kCraftBricksPerAxis,
                    k.z * kCraftBricksPerAxis};
}

// The inverse: which terrain brick owns a craft brick. floorDiv because craft
// brick coordinates are signed and 4 craft bricks make one terrain brick.
inline BrickKey terrainBrickOfCraftBrick(const BrickKey& cb) {
    const int64_t n = kCraftBricksPerAxis;
    return BrickKey{static_cast<int32_t>(floorDiv(int64_t(cb.x), n)),
                    static_cast<int32_t>(floorDiv(int64_t(cb.y), n)),
                    static_cast<int32_t>(floorDiv(int64_t(cb.z), n))};
}

// ---------------------------------------------------------------------------
// Counters -- a strict funnel, and they exist to FAIL
// ---------------------------------------------------------------------------
//
// This project has found instruments that reported plausibly while measuring
// nothing, including counters that could not fail. So these are separate on
// purpose and each answers a different question:
//
//   bricksPromoted == 0                -> nobody has chiselled. Not an error.
//   cellsWritten == 0, promoted > 0    -> promotion ran, no carve followed.
//   projectRefusedMissingBrick > 0     -> a promoted brick lost craft bricks.
//                                         A DEFECT, and the one that would
//                                         otherwise read as deleted terrain
//                                         with everything else green.
struct CraftLatticeCounters {
    std::atomic<uint64_t> bricksPromoted{0};
    std::atomic<uint64_t> promoteRejectedAlready{0};
    std::atomic<uint64_t> cellsWritten{0};
    std::atomic<uint64_t> cellsRejectedNotPromoted{0};
    std::atomic<uint64_t> projections{0};
    std::atomic<uint64_t> projectRefusedNotPromoted{0};
    std::atomic<uint64_t> projectRefusedMissingBrick{0};

    void reset() {
        bricksPromoted = 0;
        promoteRejectedAlready = 0;
        cellsWritten = 0;
        cellsRejectedNotPromoted = 0;
        projections = 0;
        projectRefusedNotPromoted = 0;
        projectRefusedMissingBrick = 0;
    }
};

// ---------------------------------------------------------------------------
// The lattice
// ---------------------------------------------------------------------------
//
// B is the TERRAIN brick edge (World<B>). The craft brick edge is always
// kMarchBrickEdge, because a craft chunk must be a canonical 32^3 brick chunk
// for packChunkBricksCanonical to pack it unchanged.
// THE ASSERT IS IN THE MEMBERS, NOT ON THE CLASS, AND THAT IS DELIBERATE.
// World<B> holds one of these by value, and World<16> is a real instantiation
// (test_editcompact.cpp). A class-scope static_assert would make merely
// DECLARING a World<16> a compile error, which would be this header breaking an
// unrelated lattice size to protect an invariant that world never uses. Member
// functions of a class template instantiate only when called, so the check
// lands on the first actual use of the mapping instead.
template <int B>
class CraftLattice {
    // A craft chunk is one terrain brick only when 4 * B == 32.
    static constexpr bool kChunkIsOneTerrainBrick = (kCraftCellsPerVoxel * B == kCraftChunkEdgeCells);

public:
    using CraftBrick = Brick<kMarchBrickEdge>;

    // --- promotion ---------------------------------------------------------

    bool isPromoted(const BrickKey& terrainBrick) const {
        return promoted_.find(terrainBrick) != promoted_.end();
    }
    size_t promotedCount() const { return promoted_.size(); }

    // Every promoted terrain brick, in deterministic order. Callers driving
    // residency or digests must not iterate the unordered set directly.
    std::vector<BrickKey> promotedSorted() const {
        std::vector<BrickKey> keys(promoted_.begin(), promoted_.end());
        std::sort(keys.begin(), keys.end(), BrickKeyLess{});
        return keys;
    }

    // Expand `source` -- the terrain brick exactly as it stands -- into all 64
    // craft bricks. Idempotent: returns false and counts it if already
    // promoted, so a caller cannot silently re-flatten a carved brick.
    bool promote(const BrickKey& terrainBrick, const Brick<B>& source) {
        static_assert(kChunkIsOneTerrainBrick,
                      "a craft chunk is one terrain brick only at terrain brick edge 8; "
                      "see the header note");
        if (isPromoted(terrainBrick)) {
            ++counters.promoteRejectedAlready;
            return false;
        }
        const BrickKey base = craftBrickBaseOfTerrainBrick(terrainBrick);
        for (int32_t bz = 0; bz < kCraftBricksPerAxis; ++bz)
            for (int32_t by = 0; by < kCraftBricksPerAxis; ++by)
                for (int32_t bx = 0; bx < kCraftBricksPerAxis; ++bx)
                    cells_.insert(BrickKey{base.x + bx, base.y + by, base.z + bz},
                                  expandCraftBrick(source, bx, by, bz));
        promoted_.insert(terrainBrick);
        ++counters.bricksPromoted;
        return true;
    }

    // --- craft cells, in GLOBAL craft-cell coordinates ---------------------

    // MAT_AIR outside a promoted brick is NOT a claim about the world -- it
    // means "the craft lattice has nothing to say here". Callers gate on
    // isPromoted() and read the terrain lattice otherwise; this is never the
    // authority for unpromoted ground.
    MaterialId materialAt(int64_t cx, int64_t cy, int64_t cz) const {
        const CraftBrick* b = cells_.find(craftBrickKeyOfCell(cx, cy, cz));
        if (b == nullptr) return MAT_AIR;
        const int64_t e = kMarchBrickEdge;
        return b->get(static_cast<int>(floorMod(cx, e)), static_cast<int>(floorMod(cy, e)),
                      static_cast<int>(floorMod(cz, e)));
    }

    // Write one craft cell. Refuses (false) outside a promoted brick rather
    // than promoting implicitly -- promotion needs the terrain brick's
    // contents and this call does not have them. The chisel verb promotes
    // first, then writes.
    bool setCell(int64_t cx, int64_t cy, int64_t cz, MaterialId mat) {
        CraftBrick* b = cells_.find(craftBrickKeyOfCell(cx, cy, cz));
        if (b == nullptr) {
            ++counters.cellsRejectedNotPromoted;
            return false;
        }
        const int64_t e = kMarchBrickEdge;
        b->set(static_cast<int>(floorMod(cx, e)), static_cast<int>(floorMod(cy, e)),
               static_cast<int>(floorMod(cz, e)), mat);
        b->tryCollapse();
        ++counters.cellsWritten;
        return true;
    }

    // --- the projection ----------------------------------------------------

    // Fold a promoted terrain brick's 64 craft bricks down to the one 8^3
    // terrain brick they project to, by two applications of mips.h's EXISTING
    // 2x rule. No new aggregation rule is defined anywhere in this feature.
    //
    // Defaults deliberately: solidThreshold 4 and surfacePreserve false, the
    // same rule the LOD pyramid uses. surfacePreserve exists to stop thin
    // GENERATED surface caps being outvoted by the body beneath them; carved
    // architecture has no such cap, and the majority vote is what makes
    // promote-then-project an identity (see the test of that name).
    //
    // Returns false (and counts it) rather than substituting air if the brick
    // is not promoted or any craft brick is missing. See the header note.
    bool project(const BrickKey& terrainBrick, Brick<B>& out) const {
        static_assert(kChunkIsOneTerrainBrick,
                      "a craft chunk is one terrain brick only at terrain brick edge 8; "
                      "see the header note");
        if (!isPromoted(terrainBrick)) {
            ++counters.projectRefusedNotPromoted;
            return false;
        }
        const BrickKey base = craftBrickBaseOfTerrainBrick(terrainBrick);

        // Round 1: 64 craft bricks (4x4x4, 25 mm cells) -> 8 bricks (5 cm cells).
        CraftBrick mid[8];
        for (int32_t gz = 0; gz < 2; ++gz)
            for (int32_t gy = 0; gy < 2; ++gy)
                for (int32_t gx = 0; gx < 2; ++gx) {
                    const CraftBrick* children[8] = {};
                    for (int32_t dz = 0; dz < 2; ++dz)
                        for (int32_t dy = 0; dy < 2; ++dy)
                            for (int32_t dx = 0; dx < 2; ++dx) {
                                const BrickKey k{base.x + 2 * gx + dx, base.y + 2 * gy + dy,
                                                 base.z + 2 * gz + dz};
                                const CraftBrick* c = cells_.find(k);
                                if (c == nullptr) {
                                    ++counters.projectRefusedMissingBrick;
                                    return false;
                                }
                                children[dx + 2 * dy + 4 * dz] = c;
                            }
                    mid[gx + 2 * gy + 4 * gz] = downsampleBricks<kMarchBrickEdge>(children);
                }

        // Round 2: 8 bricks (5 cm cells) -> one brick (10 cm cells), which is
        // exactly the terrain brick's own lattice.
        const CraftBrick* second[8];
        for (int i = 0; i < 8; ++i) second[i] = &mid[i];
        out = downsampleBricks<kMarchBrickEdge>(second);
        out.tryCollapse();
        ++counters.projections;
        return true;
    }

    // --- inspection --------------------------------------------------------

    const ChunkMap<kMarchBrickEdge>& craftBricks() const { return cells_; }
    size_t craftBrickCount() const { return cells_.size(); }

    // FAULT INJECTION, AND IT IS NOT A CONVENIENCE. No production path removes
    // a craft brick from a promoted terrain brick -- which is exactly why the
    // missing-brick refusals in project() and produceCraftChunk() would
    // otherwise be checks that CANNOT FAIL, the shape this project has found
    // eleven of in a single night. This is the only way to make them go red,
    // so it exists, and it is named so it cannot be mistaken for API.
    bool forceEraseCraftBrickForFaultInjection(const BrickKey& craftBrick) {
        return cells_.erase(craftBrick);
    }

    // Deterministic digest over promoted keys and their craft content, in
    // sorted key order. Folded into the world handshake digest so two peers
    // that disagree about a carve disagree loudly instead of visually.
    uint64_t digest() const {
        Digest d;
        for (const BrickKey& tk : promotedSorted()) {
            d.u64(static_cast<uint32_t>(tk.x));
            d.u64(static_cast<uint32_t>(tk.y));
            d.u64(static_cast<uint32_t>(tk.z));
            const BrickKey base = craftBrickBaseOfTerrainBrick(tk);
            for (int32_t bz = 0; bz < kCraftBricksPerAxis; ++bz)
                for (int32_t by = 0; by < kCraftBricksPerAxis; ++by)
                    for (int32_t bx = 0; bx < kCraftBricksPerAxis; ++bx) {
                        const CraftBrick* c =
                            cells_.find(BrickKey{base.x + bx, base.y + by, base.z + bz});
                        // A hole digests as a distinguishable marker rather than
                        // as air, so a missing brick cannot make two worlds agree.
                        if (c == nullptr) {
                            d.u64(0xC0FFEEu);
                            continue;
                        }
                        c->digest(d);
                    }
        }
        return d.h;
    }

    mutable CraftLatticeCounters counters;

private:
    // Craft brick (bx,by,bz) of a promoted terrain brick, filled from the
    // terrain brick's own voxels. A craft brick edge of 8 is 2 terrain voxels,
    // so each craft brick covers a 2x2x2 group of them.
    static CraftBrick expandCraftBrick(const Brick<B>& source, int32_t bx, int32_t by,
                                       int32_t bz) {
        const int32_t vx0 = bx * kVoxelsPerCraftBrick;
        const int32_t vy0 = by * kVoxelsPerCraftBrick;
        const int32_t vz0 = bz * kVoxelsPerCraftBrick;

        // Fast path: the whole 2x2x2 voxel group is one material, which is the
        // overwhelmingly common case (solid rock, or air). A homogeneous brick
        // is what makes promotion nearly free -- it collapses to an 8 B
        // descriptor with no payload by the time it reaches the packer.
        const MaterialId first = source.get(vx0, vy0, vz0);
        bool uniform = true;
        for (int32_t dz = 0; dz < kVoxelsPerCraftBrick && uniform; ++dz)
            for (int32_t dy = 0; dy < kVoxelsPerCraftBrick && uniform; ++dy)
                for (int32_t dx = 0; dx < kVoxelsPerCraftBrick && uniform; ++dx)
                    if (source.get(vx0 + dx, vy0 + dy, vz0 + dz) != first) uniform = false;
        if (uniform) return CraftBrick(first);

        CraftBrick out(MAT_AIR);
        for (int32_t z = 0; z < kMarchBrickEdge; ++z)
            for (int32_t y = 0; y < kMarchBrickEdge; ++y)
                for (int32_t x = 0; x < kMarchBrickEdge; ++x)
                    out.set(x, y, z,
                            source.get(vx0 + x / kCraftCellsPerVoxel,
                                       vy0 + y / kCraftCellsPerVoxel,
                                       vz0 + z / kCraftCellsPerVoxel));
        out.tryCollapse();
        return out;
    }

    std::unordered_set<BrickKey, BrickKeyHash> promoted_;
    ChunkMap<kMarchBrickEdge> cells_; // keyed in CRAFT-BRICK coordinates
};

} // namespace vxc

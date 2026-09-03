// What a chiselled brick COSTS, pinned per carve pattern.
//
// This is the exact half of the craft census (plan Part A.2). The other half --
// a settlement-scale total -- is a synthetic model and lives in a bench, because
// there is no procedural source for "what a player builds" and a model must not
// be quoted as a measurement. These numbers are different: they are a property
// of the FORMAT, not of anyone's guess about players, so they are assertions.
//
// WHY THIS IS A TEST AND NOT A BENCH. Pinning the bytes makes a future change to
// packChunkBricksCanonical, the 16-entry local palette or the bpp ladder fail
// loudly here instead of silently changing what crafting costs. A bench would
// print the new number and nobody would notice.
//
// THE NUMBERS ARE ANCHORED TO A REASON, NOT TO AN OBSERVATION. Pinning whatever
// the code happens to emit is a tautology: it passes by construction and can
// only ever confirm itself. So every case asserts the STRUCTURE that explains
// its size -- which bricks collapsed, whether a palette was needed -- as well as
// the total, and the totals are derived from the byte contract in
// docs/brick-volume-format.md:
//
//     uniform brick                       8 B, no payload
//     mixed brick   8 (desc) + 64 (occ) + 16 (palette, bpp<=4) + ceil(solid*bpp/8)
//
// WRITING THIS TEST CORRECTED TWO OF MY OWN FIGURES, which is the whole argument
// for having written it:
//
//   1. `ChunkBrickPack::residentBytes()` counts descriptors + occupancy +
//      materials and does NOT include the 64 B chunk record -- that lives in the
//      pool's separate ChunkTable. Every "576 B / 37,440 B" figure I quoted in
//      the plan and the design doc was residentBytes() PLUS the record. The
//      pack's own floor is 512 B and its ceiling is 37,376 B. Both framings are
//      defensible; mixing them is not, so this file pins the pack and names the
//      record apart.
//   2. A carve aligned to the 8-cell brick grid produces NO mixed bricks at all
//      and costs exactly the floor. My first draft used an aligned window as a
//      "typical" shape and would have pinned the cheapest possible case as
//      representative.

#include <cstdint>
#include <vector>

#include "voxelcore/craftlattice.h"
#include "voxelcore/craftvolume.h"
#include "vxctest.h"

using namespace vxc;

namespace {

using Lattice = CraftLattice<kMarchBrickEdge>;
using TerrainBrick = Brick<kMarchBrickEdge>;

constexpr int32_t E = kCraftChunkEdgeCells; // 32 craft cells per chunk axis

// What residentBytes() returns for a chunk whose every brick collapsed: 64
// descriptors and no payload.
constexpr int64_t kPackFloor = int64_t(kMarchChunkBricks) * 8; // 512

// Every brick mixed, 512 solid, more materials than a 16-entry local palette can
// hold, so 8 bpp with no palette: 64 * (8 + 64 + 512).
constexpr int64_t kPackCeiling = int64_t(kMarchChunkBricks) * (8 + 64 + 512); // 37,376

// NOT part of residentBytes(). The pool stores one 64 B record per chunk in its
// ChunkTable arena, so a resident craft chunk costs residentBytes() + this. Named
// here so the two framings cannot be mixed again.
constexpr int64_t kChunkRecordBytes = 64;

// BY REFERENCE because CraftLattice holds atomic counters, so it is neither
// copyable nor movable. Correct for a type whose counters are written from
// workers; the test just constructs in place.
void promoteRock(Lattice& lat) {
    lat.promote(BrickKey{0, 0, 0}, TerrainBrick(MAT_ROCK));
}

// Apply `f(x,y,z) -> MaterialId` over the chunk's own 32^3 craft cells. Returns
// cells actually written, so a pattern that silently carves nothing cannot
// masquerade as a cheap one.
template <typename F>
int64_t carve(Lattice& lat, const F& f) {
    int64_t written = 0;
    for (int32_t z = 0; z < E; ++z)
        for (int32_t y = 0; y < E; ++y)
            for (int32_t x = 0; x < E; ++x) {
                const MaterialId m = f(x, y, z);
                if (m == MAT_ROCK) continue; // unchanged
                if (lat.setCell(x, y, z, m)) ++written;
            }
    return written;
}

struct Cost {
    int64_t bytes = 0;
    int64_t solidCells = 0;
    bool anySolid = false;
    size_t occWords = 0;
    size_t matWords = 0;
    int32_t mixedBricks = 0;
};

Cost priceOf(const Lattice& lat) {
    CraftProducerCounters c;
    const CraftChunkResult r = produceCraftChunk(lat, BrickKey{0, 0, 0}, c);
    Cost out;
    out.bytes = r.residentBytes();
    out.anySolid = r.anySolid;
    out.occWords = r.pack.occ.size();
    out.matWords = r.pack.mat.size();
    out.solidCells = static_cast<int64_t>(c.solidCellsPacked.load());
    // Occupancy is emitted per MIXED brick only, so its word count divided by
    // the per-brick stride is the mixed count -- no second source needed.
    out.mixedBricks = static_cast<int32_t>(out.occWords / kMarchBrickOccWords);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The floor, and the ceiling
// ---------------------------------------------------------------------------

VXC_TEST(craftcost_promotion_alone_is_the_pack_floor) {
    // 64 uniform-SOLID descriptors and nothing else. This is the number the
    // promotion model rests on: promotion is nearly free, carving is what costs.
    // If it moves, the band ceiling moves with it.
    Lattice lat;
    promoteRock(lat);
    const Cost c = priceOf(lat);
    CHECK(c.mixedBricks == 0);
    CHECK(c.occWords == 0);
    CHECK(c.matWords == 0);
    CHECK(c.anySolid);
    CHECK(c.bytes == kPackFloor);
    CHECK(c.bytes == 512);
    // And the number a resident chunk actually occupies, stated once.
    CHECK(c.bytes + kChunkRecordBytes == 576);
}

VXC_TEST(craftcost_a_hollowed_brick_still_costs_the_floor_and_must_still_exist) {
    // Every cell air. The bricks collapse to uniform AIR, so it is the same
    // floor as uncarved -- and it MUST still be produced, because the marcher's
    // supersede rule means "a craft chunk is resident here"; drop it and the
    // carved-away rock comes back (docs/craft-lattice-2026-08-26.md 3.2).
    Lattice lat;
    promoteRock(lat);
    const int64_t written = carve(lat, [](int32_t, int32_t, int32_t) { return MAT_AIR; });
    CHECK(written == int64_t(E) * E * E);

    const Cost c = priceOf(lat);
    CHECK(!c.anySolid);
    CHECK(c.mixedBricks == 0);
    CHECK(c.bytes == kPackFloor);
}

VXC_TEST(craftcost_adversarial_upper_bound) {
    // NOT a shape anyone would carve -- a bound. Every cell solid and twenty
    // materials cycling, so every brick is mixed, the 16-entry local palette
    // cannot hold it, and every brick falls to 8 bpp with no palette. This is
    // the ceiling the band arithmetic uses; pinning it is what stops that
    // arithmetic drifting from the format.
    Lattice lat;
    promoteRock(lat);
    carve(lat, [](int32_t x, int32_t y, int32_t z) {
        return static_cast<MaterialId>(1 + ((x + y * 3 + z * 7) % 20));
    });

    const Cost c = priceOf(lat);
    CHECK(c.anySolid);
    CHECK(c.solidCells == int64_t(E) * E * E); // nothing was carved to air
    CHECK(c.mixedBricks == kMarchChunkBricks); // every brick mixed
    CHECK(c.bytes == kPackCeiling);
    CHECK(c.bytes == 37376);
    // The spread the census exists to resolve, asserted rather than asserted-in-prose.
    CHECK(kPackCeiling / kPackFloor >= 70);
}

// ---------------------------------------------------------------------------
// Shapes a player would actually carve
// ---------------------------------------------------------------------------

VXC_TEST(craftcost_alignment_is_the_dominant_term) {
    // THE FINDING THIS FILE EXISTS TO PIN, and it was a surprise.
    //
    // A carve whose faces land on the 8-cell brick grid leaves every brick
    // uniform, so it costs the FLOOR -- a 16x32x16 hole through the block is
    // free. Move the same hole by one cell and it straddles bricks, every one it
    // touches goes mixed, and it costs many times more.
    //
    // Cost tracks CARVED SURFACE MEASURED IN BRICKS, not carved volume. That is
    // the single most useful fact for predicting what a settlement costs, and it
    // means a builder who works on 20 cm boundaries pays almost nothing.
    Lattice aligned;
    promoteRock(aligned);
    const int64_t alignedWritten = carve(aligned, [](int32_t x, int32_t y, int32_t z) -> MaterialId {
        (void)y;
        return (x >= 8 && x < 24 && z >= 8 && z < 24) ? MAT_AIR : MAT_ROCK;
    });

    Lattice offset;
    promoteRock(offset);
    const int64_t offsetWritten = carve(offset, [](int32_t x, int32_t y, int32_t z) -> MaterialId {
        (void)y;
        return (x >= 7 && x < 23 && z >= 7 && z < 23) ? MAT_AIR : MAT_ROCK;
    });

    // The same amount of rock removed, one cell apart in position.
    CHECK(alignedWritten == offsetWritten);

    const Cost ca = priceOf(aligned);
    const Cost co = priceOf(offset);

    CHECK(ca.mixedBricks == 0);
    CHECK(ca.bytes == kPackFloor);
    CHECK(ca.bytes == 512);
    CHECK(co.mixedBricks == 32);
    CHECK(co.bytes == 3072);
    CHECK(co.bytes == 6 * ca.bytes);   // the headline: 6x for a one-cell shift

    std::printf("    [craftcost] aligned hole %lld B (0 mixed)  vs  offset by one cell %lld B "
                "(%d mixed)\n",
                static_cast<long long>(ca.bytes), static_cast<long long>(co.bytes),
                co.mixedBricks);
}

VXC_TEST(craftcost_real_carve_patterns_are_bounded_and_ordered) {
    // Shapes 2.5 cm exists to make possible. Each must carve something, leave
    // something, and land inside the floor/ceiling the two tests above pin.
    // EXACT bytes, not a range. A mutation exercise on 2026-08-27 showed why:
    // a bounds-only assertion ("more than the floor, less than the ceiling")
    // survives any change to the bpp ladder or the palette size that keeps the
    // result between them, which is most of them. A pin that cannot fail on the
    // change it exists to catch is not a pin.
    struct Case {
        const char* name;
        MaterialId (*f)(int32_t, int32_t, int32_t);
        int64_t expectBytes;
        int32_t expectMixed;
    };

    static const Case kCases[] = {
        // A stair: eight steps rising across the block.
        {"stair", [](int32_t x, int32_t y, int32_t z) -> MaterialId {
             (void)y;
             return (z >= (x / 4) * 4) ? MAT_AIR : MAT_ROCK;
         }, 1792, 16},
        // A chamfer: the 45-degree cut along one edge.
        {"chamfer", [](int32_t x, int32_t y, int32_t z) -> MaterialId {
             (void)y;
             return (x + z < 12) ? MAT_AIR : MAT_ROCK;
         }, 1472, 12},
        // A lattice screen: thin repeated structure, the expensive real shape.
        {"lattice", [](int32_t x, int32_t y, int32_t z) -> MaterialId {
             if (y >= 4) return MAT_AIR;                                  // a thin panel...
             return ((x % 4) < 2 && (z % 4) < 2) ? MAT_AIR : MAT_ROCK;    // ...pierced
         }, 1792, 16},
        // FOUR MATERIALS, and it is here because a mutation exercise found the
        // gap: every other pattern carves single-material rock, so the palette
        // is never larger than one and the 3-4 material rung of the bpp ladder
        // was untested. Real building is stone AND plaster AND timber AND
        // glazing, and that rung is where it lands -- 2 bpp instead of 1, so
        // twice the payload per mixed brick.
        {"4-material", [](int32_t x, int32_t y, int32_t z) -> MaterialId {
             if (y >= 4) return MAT_AIR;        // a wall panel...
             if ((x % 7) < 1) return MAT_AIR;   // ...pierced off the brick grid
             switch ((z / 5) % 4) {
                 case 0: return MAT_ROCK;
                 case 1: return MAT_CLAY;
                 case 2: return MAT_SAND;
                 default: return MAT_TOPSOIL;
             }
         }, 2440, 16},
    };

    for (const Case& k : kCases) {
        Lattice lat;
        promoteRock(lat);
        const int64_t written = carve(lat, k.f);
        const Cost c = priceOf(lat);

        // IT CARVED SOMETHING. A pattern that quietly did nothing would price as
        // the floor and read as a cheap shape -- the silent-success shape this
        // project keeps finding.
        CHECK(written > 0);
        // IT LEFT SOMETHING. Removing everything is the hollow case, not a shape.
        CHECK(c.anySolid);
        CHECK(c.solidCells > 0);
        // These three all straddle bricks, so all three cost more than the floor
        // -- and each costs EXACTLY this, which is what makes the pin a gate.
        CHECK(c.mixedBricks == k.expectMixed);
        CHECK(c.bytes == k.expectBytes);
        CHECK(c.bytes > kPackFloor);
        CHECK(c.bytes < kPackCeiling);

        std::printf("    [craftcost] %-8s %7lld B  %6lld solid  %3d mixed bricks\n", k.name,
                    static_cast<long long>(c.bytes), static_cast<long long>(c.solidCells),
                    c.mixedBricks);
    }
}

VXC_TEST(craftcost_a_single_notch_is_far_cheaper_than_a_lattice) {
    // The ordering that matters for the band arithmetic. If these ever compare
    // equal, the format has stopped rewarding uniformity and the whole promotion
    // economy changes.
    Lattice notch;
    promoteRock(notch);
    CHECK(notch.setCell(0, 0, 0, MAT_AIR));
    const Cost cn = priceOf(notch);

    Lattice lattice;
    promoteRock(lattice);
    carve(lattice, [](int32_t x, int32_t y, int32_t z) -> MaterialId {
        if (y >= 4) return MAT_AIR;
        return ((x % 4) < 2 && (z % 4) < 2) ? MAT_AIR : MAT_ROCK;
    });
    const Cost cl = priceOf(lattice);

    CHECK(cn.bytes < cl.bytes);
    // One notch dirties exactly one brick.
    CHECK(cn.mixedBricks == 1);
}

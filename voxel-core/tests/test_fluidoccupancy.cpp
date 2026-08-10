// GPU fluid occupancy volume + particle collision, CPU reference
// (voxelcore/fluidoccupancy.h). Phase 0 spike (c) of
// docs/water-rearchitecture-plan-2026-08-09.md.
//
// WHAT THESE TESTS ARE FOR. The shipping volume is 16 MB of bits inside a
// compute shader and the collision runs 300k times a substep in another one.
// Neither can be looked at. Everything that can be silently wrong about them
// is arithmetic — a bit index, a face sign, a step budget — and all of it is
// here, where a wrong answer is a FAIL line and not a river draining through a
// hillside three weeks later.
//
// EVERY TEST MUST BE DISTINGUISHABLE FROM "DID NOT RUN" (the standing rule
// after three absent-stat zeros produced false conclusions in one session).
// The harness prints [PASS]/[FAIL] per registered case, and beyond that the
// data-driven cases assert a non-trivial COUNT of the thing they measured, so
// a predicate that silently matched nothing fails instead of passing vacuously.

#include "voxelcore/fluidoccupancy.h"
#include "voxelcore/brick.h"
#include "voxelcore/raycast.h"
#include "vxctest.h"

#include <vector>

using namespace vxc;

namespace {

// Origin used by most collision cases: deliberately NOT the origin, and
// negative, so any place that confused a world voxel with a volume-local one
// shows up as an off-by-origin rather than passing by luck.
constexpr int32_t kOrigin[3] = {-40, -40, -40};
constexpr int32_t kZeroOrigin[3] = {0, 0, 0};

// One solid voxel at world (5, 0, 0).
bool singleBlock(int64_t vx, int64_t vy, int64_t vz) {
    return vx == 5 && vy == 0 && vz == 0;
}

// Everything below world z = 0 is ground.
bool floorWorld(int64_t, int64_t, int64_t vz) { return vz < 0; }

// A wall exactly one voxel thick at world x == wallX, nothing else.
struct ThinWall {
    int64_t wallX;
    bool operator()(int64_t vx, int64_t, int64_t) const { return vx == wallX; }
};

// Two walls meeting at a corner: +x blocked from x>=5, +y blocked from y>=1.
bool cornerWorld(int64_t vx, int64_t vy, int64_t) { return vx >= 5 || vy >= 1; }

// The synthetic terrain the region-fill test packs and then reads back. Mixed
// materials on purpose: the packer must route every one of them through
// isSolidForFluid rather than testing != MAT_AIR itself.
MaterialId regionMaterial(int32_t lx, int32_t ly, int32_t lz) {
    const int32_t h = (lx * 3 + ly * 5 + lz * 7) % 11;
    if (h == 6) return MAT_WATERMARK; // solid-looking, must pack as EMPTY
    if (h < 4) return (h == 0) ? MAT_BEDROCK : MAT_ROCK;
    return MAT_AIR;
}

} // namespace

// --- packing ---------------------------------------------------------------

VXC_TEST(fluidocc_brick_bit_index_matches_brick_cellindex) {
    // The packing rides Brick<8>'s own cell order so packBrickSolidBits can
    // walk a brick linearly. If Brick ever renumbers, this is the tripwire.
    int checked = 0;
    for (int z = 0; z < 8; ++z)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                CHECK_EQ(fluidBrickBitIndex(x, y, z), Brick<8>::cellIndex(x, y, z));
                ++checked;
            }
    CHECK_EQ(checked, 512);
}

VXC_TEST(fluidocc_solid_predicate) {
    CHECK(!isSolidForFluid(MAT_AIR));
    // The debug water marker stands where water is; treating it as ground
    // would wall off every river whenever -VoxelWaterMarker=1 is passed.
    CHECK(!isSolidForFluid(MAT_WATERMARK));
    CHECK(isSolidForFluid(MAT_BEDROCK));
    CHECK(isSolidForFluid(MAT_ROCK));
    CHECK(isSolidForFluid(MAT_SAND));
    CHECK(isSolidForFluid(MAT_TOPSOIL));
    CHECK(isSolidForFluid(MAT_MUD));
}

VXC_TEST(fluidocc_brick_pack_roundtrip) {
    uint32_t words[kFluidBrickWords];
    // A pattern with structure along every axis, so a transposed index is a
    // failure rather than a coincidence.
    const auto mat = [](int32_t bx, int32_t by, int32_t bz) -> MaterialId {
        return ((bx + 2 * by + 4 * bz) % 3 == 0) ? MAT_ROCK : MAT_AIR;
    };
    packBrickSolidBits(mat, words);

    int solids = 0;
    for (int32_t bz = 0; bz < 8; ++bz)
        for (int32_t by = 0; by < 8; ++by)
            for (int32_t bx = 0; bx < 8; ++bx) {
                const bool want = isSolidForFluid(mat(bx, by, bz));
                CHECK_EQ(brickSolidBit(words, bx, by, bz), want);
                solids += want ? 1 : 0;
            }
    // Non-vacuous: an all-zero pack would satisfy nothing here.
    CHECK_EQ(solids, 171);
}

VXC_TEST(fluidocc_brick_row_is_one_contiguous_byte) {
    // The GPU fill gathers a brick's x-row as ONE shifted byte instead of
    // eight bit tests. That shortcut is only legal because bx is the fastest
    // term of the bit index and a row therefore never straddles a word.
    for (int32_t bz = 0; bz < 8; ++bz)
        for (int32_t by = 0; by < 8; ++by) {
            const int32_t first = fluidBrickBitIndex(0, by, bz);
            const int32_t last = fluidBrickBitIndex(7, by, bz);
            CHECK_EQ(last - first, 7);
            CHECK_EQ(fluidBrickWordOf(first), fluidBrickWordOf(last));
            CHECK_EQ(fluidBrickShiftOf(first) % 8, 0);
        }
}

VXC_TEST(fluidocc_volume_bit_roundtrip) {
    // One row of words plus a couple of neighbours, not the whole 16 MB: the
    // indexing is what is under test, not the allocation.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), 0u);

    // Straddle every boundary the packing has: inside a word, the 31->32 word
    // edge, the end of an x row, and the y->z row edge.
    const int32_t probes[][3] = {{0, 0, 0},     {31, 0, 0},   {32, 0, 0},  {33, 0, 0},
                                 {511, 0, 0},   {0, 1, 0},    {0, 511, 0}, {0, 0, 1},
                                 {511, 511, 511}, {256, 128, 64}};
    const int n = static_cast<int>(sizeof(probes) / sizeof(probes[0]));
    for (int i = 0; i < n; ++i) {
        fluidVolumeSetBit(words.data(), probes[i][0], probes[i][1], probes[i][2], true);
    }
    for (int i = 0; i < n; ++i) {
        CHECK(fluidVolumeGetBit(words.data(), probes[i][0], probes[i][1], probes[i][2]));
    }
    // Distinct words/bits: exactly n bits are set in the whole volume.
    int64_t total = 0;
    for (uint32_t w : words) {
        uint32_t v = w;
        while (v) { total += v & 1u; v >>= 1; }
    }
    CHECK_EQ(total, static_cast<int64_t>(n));

    // Clearing is the inverse, not a no-op.
    fluidVolumeSetBit(words.data(), 33, 0, 0, false);
    CHECK(!fluidVolumeGetBit(words.data(), 33, 0, 0));
    CHECK(fluidVolumeGetBit(words.data(), 32, 0, 0));
}

VXC_TEST(fluidocc_volume_geometry_constants) {
    CHECK_EQ(kFluidVolumeDimVoxels, 512);
    CHECK_EQ(kFluidVolumeWordsPerRow, 16);
    CHECK_EQ(kFluidVolumeWords, 4194304);
    CHECK_EQ(kFluidVolumeWords * 4, 16 * 1024 * 1024);
    CHECK_EQ(kFluidBrickWords, 16);
    CHECK_EQ(kFluidBricksPerWordX, 4);
    // 512 voxels of 10 cm = 51.2 m, the plan's active-region cube.
    CHECK_EQ(static_cast<int>(kFluidVolumeDimVoxels * kVoxelSizeMm), 51200);
}

// --- outside-is-solid ------------------------------------------------------

VXC_TEST(fluidocc_outside_the_volume_is_solid) {
    // An entirely EMPTY volume: every bit clear. Anything reported solid can
    // only be the out-of-bounds rule, which is the point.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), 0u);
    const int32_t origin[3] = {1000, -2000, 30};

    // Inside, all eight corners: air.
    CHECK(!fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000, -2000, 30));
    CHECK(!fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000 + 511, -2000 + 511, 30 + 511));

    // One voxel past each of the six faces: solid.
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 999, -2000, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000 + 512, -2000, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000, -2001, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000, -2000 + 512, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000, -2000, 29));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000, -2000, 30 + 512));

    // Far away in every direction, including the negative side, which is
    // where a truncating conversion would wrap back inside.
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, -1000000, 0, 0));
    CHECK(fluidSolidAtVoxel(words.data(), origin, kFluidWrapOffsetNone, 1000000, 0, 0));
}

VXC_TEST(fluidocc_unbuilt_volume_is_solid_everywhere) {
    // What the clear pass writes. A freshly created or recentred volume must
    // freeze particles, not leak them.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords),
                                kFluidVolumeUnbuiltWord);
    int probed = 0;
    for (int32_t z = 0; z < 512; z += 97)
        for (int32_t y = 0; y < 512; y += 89)
            for (int32_t x = 0; x < 512; x += 83) {
                CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, x, y, z));
                ++probed;
            }
    CHECK_EQ(probed, 6 * 6 * 7);
}

// --- the region fill (brick -> volume transpose) ---------------------------

VXC_TEST(fluidocc_region_fill_matches_source_materials) {
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords),
                                kFluidVolumeUnbuiltWord);

    FluidRegion r;
    r.minVoxel[0] = 64;  r.sizeVoxels[0] = 64;   // 32-aligned along x
    r.minVoxel[1] = 32;  r.sizeVoxels[1] = 32;
    r.minVoxel[2] = 16;  r.sizeVoxels[2] = 24;
    CHECK(fluidRegionIsAligned(r));
    CHECK(fluidRegionInBounds(r));

    const int32_t bricksX = fluidRegionBricks(r.sizeVoxels[0]);
    const int32_t bricksY = fluidRegionBricks(r.sizeVoxels[1]);
    const int32_t bricksZ = fluidRegionBricks(r.sizeVoxels[2]);
    CHECK_EQ(bricksX, 8);
    CHECK_EQ(bricksY, 4);
    CHECK_EQ(bricksZ, 3);
    CHECK_EQ(fluidRegionBrickWordCount(r), static_cast<int64_t>(8 * 4 * 3 * 16));

    // The host's half: pack every brick of the region, in the region's own
    // brick order.
    std::vector<uint32_t> brickWords(static_cast<size_t>(fluidRegionBrickWordCount(r)), 0u);
    for (int32_t ibz = 0; ibz < bricksZ; ++ibz)
        for (int32_t iby = 0; iby < bricksY; ++iby)
            for (int32_t ibx = 0; ibx < bricksX; ++ibx) {
                const int64_t base = fluidRegionBrickWordBase(ibx, iby, ibz, bricksX, bricksY);
                const auto mat = [&](int32_t bx, int32_t by, int32_t bz) {
                    return regionMaterial(r.minVoxel[0] + ibx * kFluidBrickEdge + bx,
                                          r.minVoxel[1] + iby * kFluidBrickEdge + by,
                                          r.minVoxel[2] + ibz * kFluidBrickEdge + bz);
                };
                packBrickSolidBits(mat, &brickWords[static_cast<size_t>(base)]);
            }

    fluidFillRegion(words.data(), kFluidWrapOffsetNone, r, brickWords.data());

    // Every voxel inside the region now agrees with the source.
    int64_t solids = 0, markers = 0, cells = 0;
    for (int32_t rz = 0; rz < r.sizeVoxels[2]; ++rz)
        for (int32_t ry = 0; ry < r.sizeVoxels[1]; ++ry)
            for (int32_t rx = 0; rx < r.sizeVoxels[0]; ++rx) {
                const int32_t lx = r.minVoxel[0] + rx;
                const int32_t ly = r.minVoxel[1] + ry;
                const int32_t lz = r.minVoxel[2] + rz;
                const MaterialId m = regionMaterial(lx, ly, lz);
                const bool want = isSolidForFluid(m);
                CHECK_EQ(fluidVolumeGetBitLocal(words.data(), kFluidWrapOffsetNone, lx, ly, lz), want);
                solids += want ? 1 : 0;
                markers += (m == MAT_WATERMARK) ? 1 : 0;
                ++cells;
            }
    CHECK_EQ(cells, 64 * 32 * 24);
    // Non-vacuous on both sides: the region really is a mix, and it really
    // does contain marker cells that had to pack as empty.
    CHECK(solids > cells / 8);
    CHECK(solids < (cells * 7) / 8);
    CHECK(markers > 0);

    // The rest of the volume was NOT touched. A fill that ran wide would leave
    // air outside the region, which is exactly the leak the alignment rule
    // exists to prevent.
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, r.minVoxel[0] - 1, r.minVoxel[1], r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, r.minVoxel[0] + r.sizeVoxels[0],
                            r.minVoxel[1], r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, r.minVoxel[0], r.minVoxel[1] - 1, r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, r.minVoxel[0], r.minVoxel[1], r.minVoxel[2] - 1));
}

VXC_TEST(fluidocc_region_snap_is_outward_and_clipped) {
    FluidRegion r;

    // A single dirty brick at an awkward place snaps out to the update grid.
    const int32_t mn[3] = {40, 41, 42};
    const int32_t mx[3] = {47, 48, 49};
    CHECK(fluidSnapRegion(mn, mx, r));
    CHECK(fluidRegionIsAligned(r));
    CHECK_EQ(r.minVoxel[0], 32);   // 40 -> down to the 32-grid
    CHECK_EQ(r.sizeVoxels[0], 32); // 47 -> up to 64
    CHECK_EQ(r.minVoxel[1], 40);
    CHECK_EQ(r.sizeVoxels[1], 16); // 41..48 spans two bricks
    CHECK_EQ(r.minVoxel[2], 40);
    CHECK_EQ(r.sizeVoxels[2], 16);
    // Outward, never inward: the snapped box contains the requested box.
    for (int a = 0; a < 3; ++a) {
        CHECK(r.minVoxel[a] <= mn[a]);
        CHECK(r.minVoxel[a] + r.sizeVoxels[a] > mx[a]);
    }

    // Clipped at the volume edges rather than running past them.
    const int32_t mn2[3] = {-5, 500, 0};
    const int32_t mx2[3] = {3, 700, 0};
    CHECK(fluidSnapRegion(mn2, mx2, r));
    CHECK_EQ(r.minVoxel[0], 0);
    CHECK_EQ(r.sizeVoxels[0], 32);
    CHECK_EQ(r.minVoxel[1], 496);
    CHECK_EQ(r.sizeVoxels[1], 16); // 704 clipped back to the 512 edge
    CHECK(fluidRegionInBounds(r));

    // Entirely outside: refused, so an empty box cannot be dispatched as a
    // zero-sized update that silently does nothing.
    const int32_t mn3[3] = {600, 0, 0};
    const int32_t mx3[3] = {700, 0, 0};
    CHECK(!fluidSnapRegion(mn3, mx3, r));
}

// --- face determination ----------------------------------------------------

VXC_TEST(fluidocc_face_determination_all_six) {
    // One solid voxel at world (5,0,0); approach it down each axis from both
    // sides and check the reported face and the projected position.
    struct Case {
        float from[3];
        float to[3];
        int32_t axis;
        int32_t sign;
        float wantPos; // expected corrected component along `axis`
    };
    // Voxel (5,0,0) spans x in [50,60), y in [0,10), z in [0,10) UU.
    const Case cases[] = {
        {{45, 5, 5}, {58, 5, 5}, 0, +1, 50.0f - kFluidCollisionSkinUU},
        {{65, 5, 5}, {52, 5, 5}, 0, -1, 60.0f + kFluidCollisionSkinUU},
        {{55, -5, 5}, {55, 8, 5}, 1, +1, 0.0f - kFluidCollisionSkinUU},
        {{55, 15, 5}, {55, 2, 5}, 1, -1, 10.0f + kFluidCollisionSkinUU},
        {{55, 5, -5}, {55, 5, 8}, 2, +1, 0.0f - kFluidCollisionSkinUU},
        {{55, 5, 15}, {55, 5, 2}, 2, -1, 10.0f + kFluidCollisionSkinUU},
    };

    int resolved = 0;
    for (const Case& c : cases) {
        const FluidCollisionResult r =
            fluidResolveCollision(singleBlock, kZeroOrigin, c.to, c.from);
        CHECK(r.blocked);
        CHECK(!r.startedInside);
        CHECK(!r.speedClamped);
        CHECK_EQ(r.faceAxis, c.axis);
        CHECK_EQ(r.faceSign, c.sign);
        CHECK_EQ(r.iterations, 1);
        // Outward normal points back the way the particle came.
        CHECK_EQ(r.normal[c.axis], static_cast<float>(-c.sign));
        CHECK(std::fabs(r.posUU[c.axis] - c.wantPos) < 1e-3f);
        // The other two components are untouched.
        for (int a = 0; a < 3; ++a) {
            if (a == c.axis) continue;
            CHECK_EQ(r.normal[a], 0.0f);
            CHECK(std::fabs(r.posUU[a] - c.to[a]) < 1e-4f);
        }
        // And the corrected position is genuinely out of the solid voxel.
        const int64_t v = static_cast<int64_t>(std::floor(r.posUU[c.axis] / kFluidVoxelUU));
        CHECK(v != 5 || c.axis != 0);
        ++resolved;
    }
    CHECK_EQ(resolved, 6);
}

VXC_TEST(fluidocc_faces_agree_with_raycast_reference) {
    // raycast.h is the authoritative integer walk. Same geometry, same
    // segment, same answer — this is the tripwire against the float walk
    // drifting from the integer one.
    struct Seg { float from[3]; float to[3]; };
    const Seg segs[] = {
        {{45, 5, 5}, {58, 5, 5}},
        {{65, 5, 5}, {52, 5, 5}},
        {{55, -5, 5}, {55, 8, 5}},
        {{55, 5, 15}, {55, 5, 2}},
        {{41, -7, -3}, {57, 6, 7}},  // oblique, crosses several voxels first
    };
    int compared = 0;
    for (const Seg& s : segs) {
        const FluidWalkHit w = fluidWalkVoxelLine(singleBlock, kZeroOrigin, s.from, s.to);
        const auto matAt = [](int64_t vx, int64_t vy, int64_t vz) -> MaterialId {
            return singleBlock(vx, vy, vz) ? MAT_ROCK : MAT_AIR;
        };
        // UU -> mm is x10; the segment is origin + direction, as raycastVoxels
        // takes it.
        const RaycastHit h = raycastVoxels(
            matAt, static_cast<int64_t>(s.from[0] * 10.0f), static_cast<int64_t>(s.from[1] * 10.0f),
            static_cast<int64_t>(s.from[2] * 10.0f),
            static_cast<int64_t>((s.to[0] - s.from[0]) * 10.0f),
            static_cast<int64_t>((s.to[1] - s.from[1]) * 10.0f),
            static_cast<int64_t>((s.to[2] - s.from[2]) * 10.0f));
        CHECK_EQ(w.hit, h.hit);
        if (w.hit && h.hit) {
            CHECK_EQ(w.vx, h.vx);
            CHECK_EQ(w.vy, h.vy);
            CHECK_EQ(w.vz, h.vz);
            CHECK_EQ(w.faceAxis, h.faceAxis);
            CHECK_EQ(w.faceSign, h.faceSign);
        }
        ++compared;
    }
    CHECK_EQ(compared, 5);
    // Non-vacuous: at least one of those segments really did hit.
    const FluidWalkHit any = fluidWalkVoxelLine(singleBlock, kZeroOrigin, segs[0].from, segs[0].to);
    CHECK(any.hit);
}

VXC_TEST(fluidocc_free_motion_is_untouched) {
    const float from[3] = {5, 5, 5};
    const float to[3] = {9, 7, 6};
    const FluidCollisionResult r =
        fluidResolveCollision(singleBlock, kZeroOrigin, to, from);
    CHECK(!r.blocked);
    CHECK_EQ(r.iterations, 0);
    for (int a = 0; a < 3; ++a) {
        CHECK_EQ(r.posUU[a], to[a]);
        CHECK_EQ(r.normal[a], 0.0f);
    }
}

VXC_TEST(fluidocc_started_inside_solid_holds_position) {
    // raycast.h's faceAxis == -1 case. There is no face to project out of, so
    // the particle holds rather than being teleported somewhere plausible.
    const float from[3] = {55, 5, 5}; // inside the block at (5,0,0)
    const float to[3] = {75, 5, 5};
    const FluidCollisionResult r =
        fluidResolveCollision(singleBlock, kZeroOrigin, to, from);
    CHECK(r.blocked);
    CHECK(r.startedInside);
    CHECK_EQ(r.faceAxis, -1);
    for (int a = 0; a < 3; ++a) CHECK_EQ(r.posUU[a], from[a]);
}

VXC_TEST(fluidocc_corner_clamps_two_axes) {
    // Driven diagonally into the inside corner of two walls. One resolve pass
    // clamps one axis, so the corner needs two, and both normals survive.
    const float from[3] = {45, 5, 5};
    const float to[3] = {58, 15, 5};
    const FluidCollisionResult r =
        fluidResolveCollision(cornerWorld, kZeroOrigin, to, from);
    CHECK(r.blocked);
    CHECK_EQ(r.iterations, 2);
    CHECK_EQ(r.normal[0], -1.0f);
    CHECK_EQ(r.normal[1], -1.0f);
    CHECK_EQ(r.normal[2], 0.0f);
    CHECK(std::fabs(r.posUU[0] - (50.0f - kFluidCollisionSkinUU)) < 1e-3f);
    CHECK(std::fabs(r.posUU[1] - (10.0f - kFluidCollisionSkinUU)) < 1e-3f);
    // The final position is outside the solid set — the whole point.
    const int64_t vx = static_cast<int64_t>(std::floor(r.posUU[0] / kFluidVoxelUU));
    const int64_t vy = static_cast<int64_t>(std::floor(r.posUU[1] / kFluidVoxelUU));
    const int64_t vz = static_cast<int64_t>(std::floor(r.posUU[2] / kFluidVoxelUU));
    CHECK(!cornerWorld(vx, vy, vz));
}

VXC_TEST(fluidocc_edge_grazing_along_a_boundary_does_not_block) {
    // Sliding exactly along the face plane of a solid voxel must not be read
    // as entering it. The particle sits in voxel (4,0,0) and moves in +y with
    // x pinned to 49.99 — one skin outside the wall it was just projected off.
    const float from[3] = {50.0f - kFluidCollisionSkinUU, 2, 5};
    const float to[3] = {50.0f - kFluidCollisionSkinUU, 8, 5};
    const FluidCollisionResult r =
        fluidResolveCollision(singleBlock, kZeroOrigin, to, from);
    CHECK(!r.blocked);
    CHECK(std::fabs(r.posUU[1] - 8.0f) < 1e-4f);
}

VXC_TEST(fluidocc_origin_offset_and_negative_world_coordinates) {
    // Same geometry, expressed against a negative volume origin. Positions are
    // volume-local UU; the solid predicate sees WORLD voxels. Anything that
    // conflated the two fails here and nowhere else.
    const auto blockAtWorldMinus35 = [](int64_t vx, int64_t vy, int64_t vz) {
        return vx == -35 && vy == -40 && vz == -40;
    };
    // World voxel -35 is volume-local 5 with kOrigin = -40, i.e. UU [50,60).
    const float from[3] = {45, 5, 5};
    const float to[3] = {58, 5, 5};
    const FluidCollisionResult r =
        fluidResolveCollision(blockAtWorldMinus35, kOrigin, to, from);
    CHECK(r.blocked);
    CHECK_EQ(r.faceAxis, 0);
    CHECK_EQ(r.faceSign, 1);
    CHECK(std::fabs(r.posUU[0] - (50.0f - kFluidCollisionSkinUU)) < 1e-3f);
}

// --- multi-voxel traversal and tunnelling ----------------------------------

VXC_TEST(fluidocc_multi_voxel_step_stops_at_a_one_voxel_wall) {
    // Fifty voxels of motion in a single substep at a wall ten voxels out. The
    // endpoint test a naive implementation does would sail straight through:
    // the destination voxel (50,0,0) is air.
    const ThinWall wall{10};
    CHECK(!wall(50, 0, 0)); // the naive endpoint test really would pass
    const float from[3] = {5, 5, 5};
    const float to[3] = {505, 5, 5};
    const FluidCollisionResult r = fluidResolveCollision(wall, kZeroOrigin, to, from);
    CHECK(r.blocked);
    CHECK(!r.speedClamped);
    CHECK_EQ(r.faceAxis, 0);
    CHECK_EQ(r.faceSign, 1);
    CHECK(std::fabs(r.posUU[0] - (100.0f - kFluidCollisionSkinUU)) < 1e-3f);
    // In front of the wall, not behind it.
    CHECK(r.posUU[0] < 100.0f);
}

VXC_TEST(fluidocc_forty_metre_waterfall_does_not_tunnel) {
    // The plan's worst case: v = sqrt(2*g*h) for a 40 m fall = 28.0 m/s =
    // 2800 UU/s, straight down onto ground one voxel below.
    const float speedUU = 2800.0f;
    const float dts[] = {1.0f / 60.0f, 1.0f / 120.0f, 1.0f / 240.0f};
    int checked = 0;
    for (float dt : dts) {
        const float from[3] = {5, 5, 5};
        const float to[3] = {5, 5, 5.0f - speedUU * dt};
        const FluidCollisionResult r =
            fluidResolveCollision(floorWorld, kZeroOrigin, to, from);
        CHECK(r.blocked);
        CHECK(!r.speedClamped);
        CHECK_EQ(r.faceAxis, 2);
        CHECK_EQ(r.faceSign, -1);
        CHECK_EQ(r.normal[2], 1.0f);
        CHECK(r.posUU[2] > 0.0f);   // on top of the ground, not inside it
        CHECK(r.posUU[2] < 0.02f);
        ++checked;
    }
    CHECK_EQ(checked, 3);

    // And the budget is sized for it with margin, at the SLOWEST substep rate:
    // 5543 UU/s against 2800, i.e. 1.98x, which is a 157 m fall. Pinned as a
    // range so raising or lowering kFluidMaxCollisionSteps has to come here
    // and restate what the new budget actually covers.
    const float capAt60 = fluidMaxTraversalSpeedUU(1.0f / 60.0f);
    CHECK(capAt60 > 1.9f * speedUU);
    CHECK(capAt60 > 5500.0f && capAt60 < 5600.0f);
    // The obvious budget of 8 would have missed, by 1 %, on exactly this case.
    // Recorded mechanically so the comment in fluidoccupancy.h cannot rot.
    const float eightStepCap = 8.0f * kFluidVoxelUU * 60.0f / 1.7320508f;
    CHECK(eightStepCap < speedUU);
    CHECK(eightStepCap > 0.95f * speedUU);
}

VXC_TEST(fluidocc_over_budget_speed_is_clamped_not_leaked) {
    // Far beyond anything the budget covers: 500 voxels of motion in one step.
    // The contract is that the particle SLOWS, never passes the wall.
    const ThinWall wall{30};
    const float from[3] = {5, 5, 5};
    const float to[3] = {5005, 5, 5};
    const FluidCollisionResult r = fluidResolveCollision(wall, kZeroOrigin, to, from);
    CHECK(r.speedClamped);
    CHECK(!r.blocked);
    // Stopped inside the budget, well short of the wall at x=300.
    CHECK(r.posUU[0] < 300.0f);
    CHECK(r.posUU[0] > 5.0f); // it did move
    const int64_t v = static_cast<int64_t>(std::floor(r.posUU[0] / kFluidVoxelUU));
    CHECK(v <= kFluidMaxCollisionSteps);
    CHECK(!wall(v, 0, 0));
}

VXC_TEST(fluidocc_diagonal_traversal_is_the_worst_case) {
    // The sqrt(3) in the budget formula: a diagonal segment crosses more
    // boundaries per unit length than an axis-aligned one. Pinned here so the
    // tunnelling arithmetic is checked rather than asserted in prose.
    const auto empty = [](int64_t, int64_t, int64_t) { return false; };
    const float from[3] = {5, 5, 5};
    // Ten voxels of motion, split evenly across three axes.
    const float len = 100.0f;
    const float k = len / 1.7320508f;
    const float to[3] = {5 + k, 5 + k, 5 + k};
    const FluidWalkHit w = fluidWalkVoxelLine(empty, kZeroOrigin, from, to);
    // Nothing is solid, so the only way to come back budget-exhausted is to
    // have spent all 16 steps on boundary crossings — which is exactly the
    // claim: 100 UU diagonally costs 18 crossings, not 10.
    CHECK(w.budgetExhausted);
    CHECK(!w.hit);

    // The same length along one axis fits in the budget with room to spare.
    const float axial[3] = {5 + len, 5, 5};
    const FluidWalkHit a = fluidWalkVoxelLine(empty, kZeroOrigin, from, axial);
    CHECK(!a.budgetExhausted);
    CHECK(!a.hit);
}

VXC_TEST(fluidocc_collision_reads_the_packed_volume) {
    // End to end over the real bit layout rather than a lambda predicate: pack
    // a floor into bricks, fill a region, then collide against
    // fluidSolidAtVoxel. This is the composition the shader performs.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords),
                                kFluidVolumeUnbuiltWord);

    FluidRegion r;
    r.minVoxel[0] = 0;  r.sizeVoxels[0] = 32;
    r.minVoxel[1] = 0;  r.sizeVoxels[1] = 8;
    r.minVoxel[2] = 0;  r.sizeVoxels[2] = 16;
    CHECK(fluidRegionIsAligned(r));

    // Volume-local z < 4 is ground; everything above is air.
    const auto matAt = [](int32_t, int32_t, int32_t lz) -> MaterialId {
        return lz < 4 ? MAT_ROCK : MAT_AIR;
    };
    const int32_t bricksX = fluidRegionBricks(r.sizeVoxels[0]);
    const int32_t bricksY = fluidRegionBricks(r.sizeVoxels[1]);
    const int32_t bricksZ = fluidRegionBricks(r.sizeVoxels[2]);
    std::vector<uint32_t> brickWords(static_cast<size_t>(fluidRegionBrickWordCount(r)), 0u);
    for (int32_t ibz = 0; ibz < bricksZ; ++ibz)
        for (int32_t iby = 0; iby < bricksY; ++iby)
            for (int32_t ibx = 0; ibx < bricksX; ++ibx) {
                const int64_t base = fluidRegionBrickWordBase(ibx, iby, ibz, bricksX, bricksY);
                const auto mat = [&](int32_t bx, int32_t by, int32_t bz) {
                    return matAt(ibx * 8 + bx, iby * 8 + by, ibz * 8 + bz);
                };
                packBrickSolidBits(mat, &brickWords[static_cast<size_t>(base)]);
            }
    fluidFillRegion(words.data(), kFluidWrapOffsetNone, r, brickWords.data());

    const auto solidAt = [&](int64_t vx, int64_t vy, int64_t vz) {
        return fluidSolidAtVoxel(words.data(), kZeroOrigin, kFluidWrapOffsetNone, vx, vy, vz);
    };
    CHECK(solidAt(3, 3, 3));
    CHECK(!solidAt(3, 3, 4));

    // Fall onto the packed floor from inside the filled region, at the 40 m
    // waterfall speed and the slowest substep: 2800/60 = 46.7 UU, 4.7 voxels.
    const float from[3] = {35, 35, 75};
    const float to[3] = {35, 35, 75.0f - 2800.0f / 60.0f};
    const FluidCollisionResult res = fluidResolveCollision(solidAt, kZeroOrigin, to, from);
    CHECK(res.blocked);
    CHECK_EQ(res.faceAxis, 2);
    CHECK_EQ(res.faceSign, -1);
    // Rests on top of local voxel z=3, i.e. at 40 UU.
    CHECK(std::fabs(res.posUU[2] - (40.0f + kFluidCollisionSkinUU)) < 1e-3f);

    // And outside the FILLED region it is still solid, so a particle that
    // strays into unbuilt space stops instead of escaping.
    CHECK(solidAt(3, 3, 100));
}

// --- toroidal addressing (contract item 4) ---------------------------------
//
// WHAT THESE ARE GUARDING. The volume is a rolling window: a bit's address is
// (window coordinate + wrap offset) mod 512. Everything that can be silently
// wrong about that is arithmetic, and all of it is unobservable in the editor
// until water starts colliding with terrain from 51.2 m away -- which reads as
// worldgen, not as a bug. So: the indexing is pinned as a bijection, the verify
// gate's comparison is pinned as an equivalence, the collision walk is driven
// across the seam, and a recentre is run end to end over the real bit layout.

namespace {

// A world-anchored terrain function with structure on every axis, so a wrong
// address is a mismatch rather than a coincidence.
MaterialId wrapTerrain(int64_t wx, int64_t wy, int64_t wz) {
    const int64_t h = floorMod(wx * 3 + wy * 5 + wz * 7, 11);
    if (h == 6) return MAT_WATERMARK; // packs as EMPTY -- the packer must route it
    return (h < 4) ? MAT_ROCK : MAT_AIR;
}

// Packs and fills one region of `words` from a WORLD terrain function, exactly
// the way FVoxelFluidOccupancyVolume::PackRegionBricks + fluidFillRegion do.
void fillRegionFromWorld(uint32_t* words, const int32_t wrapOffset[3],
                         const int32_t originVoxel[3], const FluidRegion& r) {
    const int32_t bricksX = fluidRegionBricks(r.sizeVoxels[0]);
    const int32_t bricksY = fluidRegionBricks(r.sizeVoxels[1]);
    const int32_t bricksZ = fluidRegionBricks(r.sizeVoxels[2]);
    std::vector<uint32_t> brickWords(static_cast<size_t>(fluidRegionBrickWordCount(r)), 0u);
    for (int32_t ibz = 0; ibz < bricksZ; ++ibz)
        for (int32_t iby = 0; iby < bricksY; ++iby)
            for (int32_t ibx = 0; ibx < bricksX; ++ibx) {
                const int64_t base = fluidRegionBrickWordBase(ibx, iby, ibz, bricksX, bricksY);
                const int64_t wx = originVoxel[0] + r.minVoxel[0] + int64_t(ibx) * kFluidBrickEdge;
                const int64_t wy = originVoxel[1] + r.minVoxel[1] + int64_t(iby) * kFluidBrickEdge;
                const int64_t wz = originVoxel[2] + r.minVoxel[2] + int64_t(ibz) * kFluidBrickEdge;
                const auto mat = [&](int32_t bx, int32_t by, int32_t bz) {
                    return wrapTerrain(wx + bx, wy + by, wz + bz);
                };
                packBrickSolidBits(mat, &brickWords[static_cast<size_t>(base)]);
            }
    fluidFillRegion(words, wrapOffset, r, brickWords.data());
}

FluidRegion cellRegion(int32_t cx, int32_t cy, int32_t cz) {
    FluidRegion r;
    const int32_t c[3] = {cx, cy, cz};
    for (int a = 0; a < 3; ++a) {
        r.minVoxel[a] = c[a] * kFluidRecentreStepVoxels;
        r.sizeVoxels[a] = kFluidRecentreStepVoxels;
    }
    return r;
}

} // namespace

VXC_TEST(fluidocc_wrap_indexing_is_a_bijection_across_the_seam) {
    // Every legal wrap offset maps the window onto the buffer one-to-one, and
    // the inverse is the obvious one. If this is ever not a bijection, two
    // window voxels share a slot and one of them is silently the other.
    const int32_t offsets[] = {0, 64, 256, 448};
    int checked = 0;
    for (int32_t off : offsets) {
        std::vector<int> hits(static_cast<size_t>(kFluidVolumeDimVoxels), 0);
        for (int32_t l = 0; l < kFluidVolumeDimVoxels; ++l) {
            const int32_t s = fluidVolumeStorageAxis(l, off);
            CHECK(s >= 0 && s < kFluidVolumeDimVoxels);
            // The inverse: storage minus offset, back around the ring.
            CHECK_EQ(static_cast<int32_t>(floorMod(s - off, kFluidVolumeDimVoxels)), l);
            hits[static_cast<size_t>(s)]++;
        }
        for (int h : hits) CHECK_EQ(h, 1);
        ++checked;
    }
    CHECK_EQ(checked, 4);

    // The seam is where local wraps back to storage 0. With offset 448 that is
    // local 64, and the two sides really are 511 slots apart, not adjacent.
    CHECK_EQ(fluidVolumeStorageAxis(63, 448), 511);
    CHECK_EQ(fluidVolumeStorageAxis(64, 448), 0);
    CHECK_EQ(fluidVolumeStorageAxis(0, 448), 448);
    CHECK_EQ(fluidVolumeStorageAxis(511, 448), 447);
    // Offset zero is the identity -- the flat volume is the special case, not a
    // different code path.
    for (int32_t l = 0; l < kFluidVolumeDimVoxels; l += 37) {
        CHECK_EQ(fluidVolumeStorageAxis(l, 0), l);
    }
}

VXC_TEST(fluidocc_wrap_keeps_whole_words_whole) {
    // THE INVARIANT THE BUILDER'S WHOLE-WORD WRITE RESTS ON. A 32-voxel word
    // column of the window must land on exactly one storage word, or the fill
    // needs a read-modify-write and two regions can race on the shared word.
    // It holds because the offset is a multiple of the 64-voxel recentre step.
    int columns = 0;
    for (int32_t off = 0; off < kFluidVolumeDimVoxels; off += kFluidRecentreStepVoxels) {
        const int32_t wrap[3] = {off, 0, 0};
        CHECK(fluidWrapOffsetIsAligned(wrap));
        for (int32_t lx = 0; lx < kFluidVolumeDimVoxels; lx += kFluidBitsPerWord) {
            const int64_t want = fluidVolumeWordIndexLocal(wrap, lx, 0, 0);
            uint32_t maskSeen = 0u;
            for (int32_t j = 0; j < kFluidBitsPerWord; ++j) {
                CHECK_EQ(fluidVolumeWordIndexLocal(wrap, lx + j, 0, 0), want);
                const uint32_t bit = fluidVolumeBitMaskLocal(wrap, lx + j);
                CHECK_EQ(maskSeen & bit, 0u);   // each of the 32 bits used once
                maskSeen |= bit;
            }
            CHECK_EQ(maskSeen, 0xFFFFFFFFu);    // and all 32 covered
            ++columns;
        }
    }
    CHECK_EQ(columns, 8 * 16);

    // AND AN OFFSET FINER THAN A WORD IS EXACTLY WHAT WOULD BREAK IT. An offset
    // of 8 splits a 32-voxel word column across two storage words -- the
    // read-modify-write, and the race between two regions sharing a word, that
    // the alignment rule exists to remove.
    const int32_t bad[3] = {8, 0, 0};
    CHECK(!fluidWrapOffsetIsAligned(bad));
    CHECK_EQ(fluidVolumeWordIndexLocal(bad, 0, 0, 0), fluidVolumeWordIndex(8, 0, 0));
    CHECK(fluidVolumeWordIndexLocal(bad, 31, 0, 0) != fluidVolumeWordIndexLocal(bad, 0, 0, 0));

    // 32 would in fact keep x's words whole, and is STILL refused. The offset
    // grid is the recentre grid: it also has to be the grid the entering cells
    // are enumerated on, so a 32-voxel step would hand back cells the refill
    // path cannot express. Refusing the merely-sufficient case is deliberate.
    const int32_t halfCell[3] = {32, 0, 0};
    CHECK(!fluidWrapOffsetIsAligned(halfCell));
    CHECK_EQ(fluidVolumeWordIndexLocal(halfCell, 31, 0, 0),
             fluidVolumeWordIndexLocal(halfCell, 0, 0, 0));

    const int32_t outOfRange[3] = {kFluidVolumeDimVoxels, 0, 0};
    CHECK(!fluidWrapOffsetIsAligned(outOfRange));
    const int32_t negative[3] = {-64, 0, 0};
    CHECK(!fluidWrapOffsetIsAligned(negative));
}

VXC_TEST(fluidocc_verify_gate_equivalence_under_wrap) {
    // THE VERIFY GATE'S OWN PROPERTY, in the form the in-editor gate uses it:
    // fill the same window region into two volumes, one flat and one with a
    // non-zero wrap offset, and every word must agree WHEN LOOKED UP THE SAME
    // WAY. This is what goes wrong if the gate keeps comparing at a flat index
    // after the window has slid: it would compare two different voxels and
    // report every one of them as a mismatch.
    std::vector<uint32_t> flat(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);
    std::vector<uint32_t> wrapped(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);

    // Deliberately STRADDLING the seam: offset 448 puts the seam at local x 64,
    // and this region spans local x 32..96.
    const int32_t wrap[3] = {448, 64, 192};
    const int32_t origin[3] = {1000, -2000, 30}; // unaligned on purpose
    FluidRegion r;
    r.minVoxel[0] = 32; r.sizeVoxels[0] = 64;
    r.minVoxel[1] = 56; r.sizeVoxels[1] = 16;
    r.minVoxel[2] = 8;  r.sizeVoxels[2] = 16;
    CHECK(fluidRegionIsAligned(r));
    CHECK(fluidRegionInBounds(r));

    fillRegionFromWorld(flat.data(), kFluidWrapOffsetNone, origin, r);
    fillRegionFromWorld(wrapped.data(), wrap, origin, r);

    int64_t compared = 0, differingSlots = 0, solids = 0;
    const int32_t wordsX = r.sizeVoxels[0] / kFluidBitsPerWord;
    for (int32_t rz = 0; rz < r.sizeVoxels[2]; ++rz)
        for (int32_t ry = 0; ry < r.sizeVoxels[1]; ++ry)
            for (int32_t wx = 0; wx < wordsX; ++wx) {
                const int32_t lx = r.minVoxel[0] + wx * kFluidBitsPerWord;
                const int32_t ly = r.minVoxel[1] + ry;
                const int32_t lz = r.minVoxel[2] + rz;
                const int64_t iFlat = fluidVolumeWordIndexLocal(kFluidWrapOffsetNone, lx, ly, lz);
                const int64_t iWrap = fluidVolumeWordIndexLocal(wrap, lx, ly, lz);
                CHECK_EQ(flat[static_cast<size_t>(iFlat)], wrapped[static_cast<size_t>(iWrap)]);
                if (iFlat != iWrap) ++differingSlots;
                ++compared;
            }
    CHECK_EQ(compared, int64_t(wordsX) * r.sizeVoxels[1] * r.sizeVoxels[2]);
    // NON-VACUOUS: the wrap really did move every one of these words somewhere
    // else, so the equality above is an equivalence and not an identity.
    CHECK_EQ(differingSlots, compared);

    // And the bits agree with the terrain, read through the local accessor.
    for (int32_t rz = 0; rz < r.sizeVoxels[2]; ++rz)
        for (int32_t ry = 0; ry < r.sizeVoxels[1]; ++ry)
            for (int32_t rx = 0; rx < r.sizeVoxels[0]; ++rx) {
                const int32_t lx = r.minVoxel[0] + rx, ly = r.minVoxel[1] + ry,
                              lz = r.minVoxel[2] + rz;
                const bool want = isSolidForFluid(
                    wrapTerrain(origin[0] + lx, origin[1] + ly, origin[2] + lz));
                CHECK_EQ(fluidVolumeGetBitLocal(wrapped.data(), wrap, lx, ly, lz), want);
                CHECK_EQ(fluidSolidAtVoxel(wrapped.data(), origin, wrap, origin[0] + lx,
                                           origin[1] + ly, origin[2] + lz),
                         want);
                solids += want ? 1 : 0;
            }
    const int64_t cells = int64_t(r.sizeVoxels[0]) * r.sizeVoxels[1] * r.sizeVoxels[2];
    CHECK(solids > cells / 8);
    CHECK(solids < (cells * 7) / 8);
}

VXC_TEST(fluidocc_outside_the_window_is_solid_even_when_the_slot_is_air) {
    // THE ALIASING GUARD, and the one way to get toroidal addressing wrong that
    // still compiles: drop the window bounds test because "the modulo wraps
    // harmlessly". It does not. The slot one voxel past the west face is a
    // perfectly valid slot holding the terrain 51.2 m to the east.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);
    const int32_t wrap[3] = {64, 0, 0};
    const int32_t origin[3] = {0, 0, 0};

    // Write one word's worth of AIR at the far (east) end of the window.
    FluidRegion r;
    r.minVoxel[0] = kFluidVolumeDimVoxels - 32; r.sizeVoxels[0] = 32;
    r.minVoxel[1] = 0;  r.sizeVoxels[1] = 8;
    r.minVoxel[2] = 0;  r.sizeVoxels[2] = 8;
    std::vector<uint32_t> airBricks(static_cast<size_t>(fluidRegionBrickWordCount(r)), 0u);
    fluidFillRegion(words.data(), wrap, r, airBricks.data());
    CHECK(!fluidSolidAtVoxel(words.data(), origin, wrap, kFluidVolumeDimVoxels - 1, 0, 0));

    // Local -64 and local 448 share a slot under offset 64 -- that is the alias
    // that fires if the bounds test is dropped. It must still read SOLID.
    CHECK_EQ(fluidVolumeStorageAxis(448, 64),
             static_cast<int32_t>(floorMod(-64 + 64, kFluidVolumeDimVoxels)));
    CHECK(fluidSolidAtVoxel(words.data(), origin, wrap, -64, 0, 0));
    CHECK(fluidSolidAtVoxel(words.data(), origin, wrap, -1, 0, 0));
    CHECK(fluidSolidAtVoxel(words.data(), origin, wrap, kFluidVolumeDimVoxels, 0, 0));
}

VXC_TEST(fluidocc_collision_walk_crosses_the_wrap_seam) {
    // A particle walking through the storage seam must see the terrain that is
    // GEOMETRICALLY in front of it. The seam is invisible in window space by
    // construction; this is the test that says so out loud.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);
    const int32_t wrap[3] = {448, 0, 0};   // seam at local x = 64
    const int32_t origin[3] = {-40, -40, -40};

    // A band of window across the seam: local x 0..127, all AIR except two
    // one-voxel walls, one on each side of it -- so a walk in either direction
    // can cross the seam BEFORE it reaches the thing that stops it. Both are
    // inside the 16-step collision budget from their start points, which is the
    // other constraint on where they can go.
    const int32_t kWallLocalX = 70;      // 6 voxels past the seam
    const int32_t kWallLocalXLow = 58;   // 6 voxels before it
    FluidRegion band;
    band.minVoxel[0] = 0;   band.sizeVoxels[0] = 128;
    band.minVoxel[1] = 0;   band.sizeVoxels[1] = 8;
    band.minVoxel[2] = 0;   band.sizeVoxels[2] = 8;
    {
        const int32_t bricksX = fluidRegionBricks(band.sizeVoxels[0]);
        const int32_t bricksY = fluidRegionBricks(band.sizeVoxels[1]);
        const int32_t bricksZ = fluidRegionBricks(band.sizeVoxels[2]);
        std::vector<uint32_t> brickWords(
            static_cast<size_t>(fluidRegionBrickWordCount(band)), 0u);
        for (int32_t ibz = 0; ibz < bricksZ; ++ibz)
            for (int32_t iby = 0; iby < bricksY; ++iby)
                for (int32_t ibx = 0; ibx < bricksX; ++ibx) {
                    const int64_t base =
                        fluidRegionBrickWordBase(ibx, iby, ibz, bricksX, bricksY);
                    const auto mat = [&](int32_t bx, int32_t, int32_t) {
                        const int32_t lx = ibx * kFluidBrickEdge + bx;
                        return (lx == kWallLocalX || lx == kWallLocalXLow) ? MAT_ROCK : MAT_AIR;
                    };
                    packBrickSolidBits(mat, &brickWords[static_cast<size_t>(base)]);
                }
        fluidFillRegion(words.data(), wrap, band, brickWords.data());
    }

    const auto solidAt = [&](int64_t vx, int64_t vy, int64_t vz) {
        return fluidSolidAtVoxel(words.data(), origin, wrap, vx, vy, vz);
    };
    // The walls are where they were put, and their neighbours are not.
    CHECK(solidAt(origin[0] + kWallLocalX, origin[1] + 3, origin[2] + 3));
    CHECK(solidAt(origin[0] + kWallLocalXLow, origin[1] + 3, origin[2] + 3));
    CHECK(!solidAt(origin[0] + kWallLocalX - 1, origin[1] + 3, origin[2] + 3));
    CHECK(!solidAt(origin[0] + kWallLocalX + 1, origin[1] + 3, origin[2] + 3));
    CHECK(!solidAt(origin[0] + kWallLocalXLow - 1, origin[1] + 3, origin[2] + 3));
    CHECK(!solidAt(origin[0] + kWallLocalXLow + 1, origin[1] + 3, origin[2] + 3));
    // The seam itself is NOT a wall: local 63 and 64 are 511 slots apart in the
    // buffer and adjacent air in the world.
    CHECK(!solidAt(origin[0] + 63, origin[1] + 3, origin[2] + 3));
    CHECK(!solidAt(origin[0] + 64, origin[1] + 3, origin[2] + 3));

    // Walk +x from local voxel 62 (before the seam) toward 74 (past the wall):
    // crosses the seam at 64, then the wall at 70 stops it at that wall's low
    // face, in WINDOW UU. Eight boundary crossings, inside the 16-step budget.
    {
        const float from[3] = {625.0f, 35.0f, 35.0f};   // local voxel 62
        const float to[3] = {745.0f, 35.0f, 35.0f};     // local voxel 74
        const FluidCollisionResult r = fluidResolveCollision(solidAt, origin, to, from);
        CHECK(r.blocked);
        CHECK(!r.speedClamped);
        CHECK(!r.startedInside);
        CHECK_EQ(r.faceAxis, 0);
        CHECK_EQ(r.faceSign, 1);
        CHECK(std::fabs(r.posUU[0] -
                        (float(kWallLocalX) * kFluidVoxelUU - kFluidCollisionSkinUU)) < 1e-3f);
    }
    // And back the other way, crossing the seam from the far side: voxel 68 down
    // to 56, stopped by the low wall at 58, on its HIGH face.
    {
        const float from[3] = {685.0f, 35.0f, 35.0f};   // local voxel 68
        const float to[3] = {565.0f, 35.0f, 35.0f};     // local voxel 56
        const FluidCollisionResult r = fluidResolveCollision(solidAt, origin, to, from);
        CHECK(r.blocked);
        CHECK(!r.speedClamped);
        CHECK(!r.startedInside);
        CHECK_EQ(r.faceAxis, 0);
        CHECK_EQ(r.faceSign, -1);
        CHECK(std::fabs(r.posUU[0] -
                        (float(kWallLocalXLow + 1) * kFluidVoxelUU + kFluidCollisionSkinUU)) < 1e-3f);
    }
    // Free motion straight across the seam with no wall in the way: 63 -> 66.
    {
        const float from[3] = {635.0f, 35.0f, 35.0f};
        const float to[3] = {665.0f, 35.0f, 35.0f};
        const FluidCollisionResult r = fluidResolveCollision(solidAt, origin, to, from);
        CHECK(!r.blocked);
        CHECK_EQ(r.iterations, 0);
        CHECK(std::fabs(r.posUU[0] - 665.0f) < 1e-4f);
    }
}

VXC_TEST(fluidocc_recentre_entering_cells_are_enumerated_once) {
    // Straight, diagonal, backwards, and past the window. The failure this
    // catches is slab arithmetic double-counting the overlap on a diagonal
    // move, which marks (and refills) the same cell two or three times.
    const int32_t N = kFluidRecentreCellsPerAxis;             // 8
    struct Case { int64_t d[3]; int32_t want; };
    const Case cases[] = {
        {{0, 0, 0}, 0},
        {{64, 0, 0}, 8 * 8},                                   // one slab
        {{-64, 0, 0}, 8 * 8},
        {{128, 0, 0}, 2 * 8 * 8},
        {{64, 64, 0}, 8 * 8 * 8 - 7 * 7 * 8},                  // two slabs, once
        {{64, 64, 64}, 8 * 8 * 8 - 7 * 7 * 7},
        {{512, 0, 0}, 8 * 8 * 8},                              // past the window
        {{-4096, 128, 0}, 8 * 8 * 8},
    };
    int ran = 0;
    for (const Case& c : cases) {
        std::vector<int> seen(static_cast<size_t>(N * N * N), 0);
        const int32_t count = fluidForEachEnteringCell(c.d, [&](const FluidRegion& r) {
            CHECK(fluidRegionIsAligned(r));
            CHECK(fluidRegionInBounds(r));
            const int32_t cx = r.minVoxel[0] / kFluidRecentreStepVoxels;
            const int32_t cy = r.minVoxel[1] / kFluidRecentreStepVoxels;
            const int32_t cz = r.minVoxel[2] / kFluidRecentreStepVoxels;
            seen[static_cast<size_t>((cz * N + cy) * N + cx)]++;
        });
        CHECK_EQ(count, c.want);
        int32_t total = 0;
        for (int s : seen) { CHECK(s <= 1); total += s; }   // never twice
        CHECK_EQ(total, c.want);
        ++ran;
    }
    CHECK_EQ(ran, 8);

    // A 64-voxel step really is 1/8 of the volume -- the number the design is
    // sold on, checked rather than quoted.
    const int64_t oneStep[3] = {kFluidRecentreStepVoxels, 0, 0};
    const int32_t entering = fluidForEachEnteringCell(oneStep, [](const FluidRegion&) {});
    CHECK_EQ(entering * 8, N * N * N);
}

VXC_TEST(fluidocc_recentre_delta_alignment_is_required) {
    const int64_t ok[3] = {64, -128, 0};
    CHECK(fluidRecentreDeltaIsAligned(ok));
    const int64_t half[3] = {32, 0, 0};
    CHECK(!fluidRecentreDeltaIsAligned(half));   // legal for a word, not for the grid
    const int64_t brick[3] = {0, 8, 0};
    CHECK(!fluidRecentreDeltaIsAligned(brick));
    const int64_t one[3] = {0, 0, 1};
    CHECK(!fluidRecentreDeltaIsAligned(one));

    // The offset walks the ring in both directions and stays legal forever.
    int32_t off = 0;
    for (int i = 0; i < 40; ++i) {
        off = fluidWrapOffsetAfterMove(off, kFluidRecentreStepVoxels);
        const int32_t w[3] = {off, 0, 0};
        CHECK(fluidWrapOffsetIsAligned(w));
    }
    CHECK_EQ(off, static_cast<int32_t>(floorMod(40LL * kFluidRecentreStepVoxels,
                                                kFluidVolumeDimVoxels)));
    for (int i = 0; i < 100; ++i) {
        off = fluidWrapOffsetAfterMove(off, -kFluidRecentreStepVoxels);
        const int32_t w[3] = {off, 0, 0};
        CHECK(fluidWrapOffsetIsAligned(w));
    }
    CHECK_EQ(off, static_cast<int32_t>(floorMod((40LL - 100) * kFluidRecentreStepVoxels,
                                                kFluidVolumeDimVoxels)));
}

VXC_TEST(fluidocc_recentre_preserves_the_far_side_and_invalidates_the_near_one) {
    // END TO END over the real bit layout, and the reason this whole change
    // exists. Fill three cells of terrain, slide the window one step, and check
    // that (a) the terrain still in view is byte-for-byte where it was and still
    // answers for the right WORLD voxels, (b) the entering cell would otherwise
    // serve the terrain that just left -- the hazard -- and (c) marking it
    // unbuilt closes that hazard and touches nothing else.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);
    int32_t origin[3] = {1000, -2000, 30};    // unaligned on purpose: the offset
    int32_t wrap[3] = {0, 0, 0};              // design must not care

    for (int32_t cx = 0; cx < 3; ++cx) {
        fillRegionFromWorld(words.data(), wrap, origin, cellRegion(cx, 0, 0));
    }
    // Sanity before the move, and non-vacuous: the fill produced a real mix.
    int64_t solidsBefore = 0;
    for (int32_t lx = 0; lx < 192; lx += 3)
        for (int32_t ly = 0; ly < 64; ly += 5)
            for (int32_t lz = 0; lz < 64; lz += 7) {
                const bool want = isSolidForFluid(
                    wrapTerrain(origin[0] + lx, origin[1] + ly, origin[2] + lz));
                CHECK_EQ(fluidSolidAtVoxel(words.data(), origin, wrap, origin[0] + lx,
                                           origin[1] + ly, origin[2] + lz),
                         want);
                solidsBefore += want ? 1 : 0;
            }
    CHECK(solidsBefore > 0);

    // A snapshot of the raw buffer, to prove the slide MOVES NOTHING.
    const std::vector<uint32_t> before = words;

    // Slide one step in +x. The host's arithmetic, verbatim.
    const int64_t delta[3] = {kFluidRecentreStepVoxels, 0, 0};
    CHECK(fluidRecentreDeltaIsAligned(delta));
    const int32_t oldOrigin[3] = {origin[0], origin[1], origin[2]};
    for (int a = 0; a < 3; ++a) {
        origin[a] = static_cast<int32_t>(origin[a] + delta[a]);
        wrap[a] = fluidWrapOffsetAfterMove(wrap[a], delta[a]);
    }
    CHECK(fluidWrapOffsetIsAligned(wrap));
    CHECK_EQ(wrap[0], kFluidRecentreStepVoxels);

    // (a) NOT ONE WORD MOVED, and the surviving terrain answers for the same
    // world voxels it always did -- through a different window coordinate.
    CHECK(words == before);
    int64_t rechecked = 0;
    for (int32_t lx = 0; lx < 128; lx += 3)
        for (int32_t ly = 0; ly < 64; ly += 5)
            for (int32_t lz = 0; lz < 64; lz += 7) {
                const int64_t wx = origin[0] + lx, wy = origin[1] + ly, wz = origin[2] + lz;
                CHECK_EQ(fluidSolidAtVoxel(words.data(), origin, wrap, wx, wy, wz),
                         isSolidForFluid(wrapTerrain(wx, wy, wz)));
                ++rechecked;
            }
    CHECK(rechecked > 1000);
    // The cell that fell off the west edge is outside the window now, so it is
    // solid regardless of what its slots still hold.
    CHECK(fluidSolidAtVoxel(words.data(), origin, wrap, oldOrigin[0], oldOrigin[1] + 1,
                            oldOrigin[2] + 1));

    // (b) THE HAZARD IS REAL. Cell 7 of the new window has just come into view
    // and its slots still hold old cell 0's terrain, 51.2 m west and entirely
    // plausible. Count how much of it disagrees with the terrain actually there.
    int64_t stale = 0, wrongAnswers = 0;
    for (int32_t lx = 448; lx < 512; lx += 3)
        for (int32_t ly = 0; ly < 64; ly += 5)
            for (int32_t lz = 0; lz < 64; lz += 7) {
                const bool got = fluidSolidAtVoxel(words.data(), origin, wrap, origin[0] + lx,
                                                   origin[1] + ly, origin[2] + lz);
                const bool oldTerrain = isSolidForFluid(wrapTerrain(
                    oldOrigin[0] + (lx - 448), oldOrigin[1] + ly, oldOrigin[2] + lz));
                const bool newTerrain = isSolidForFluid(
                    wrapTerrain(origin[0] + lx, origin[1] + ly, origin[2] + lz));
                CHECK_EQ(got, oldTerrain);      // exactly the stale slab
                if (oldTerrain != newTerrain) ++wrongAnswers;
                ++stale;
            }
    CHECK(stale > 0);
    // Non-vacuous: leaving it would genuinely answer wrong, often.
    CHECK(wrongAnswers > stale / 8);

    // (c) MARKING CLOSES IT. Every entering cell, exactly as RecentreTo queues
    // them, and nothing else in the volume is touched.
    const std::vector<uint32_t> beforeMark = words;
    const int32_t entering = fluidForEachEnteringCell(delta, [&](const FluidRegion& r) {
        fluidMarkRegionUnbuilt(words.data(), wrap, r);
    });
    CHECK_EQ(entering, kFluidRecentreCellsPerAxis * kFluidRecentreCellsPerAxis);
    for (int32_t lx = 448; lx < 512; lx += 3)
        for (int32_t ly = 0; ly < 64; ly += 5)
            for (int32_t lz = 0; lz < 64; lz += 7) {
                CHECK(fluidSolidAtVoxel(words.data(), origin, wrap, origin[0] + lx,
                                        origin[1] + ly, origin[2] + lz));
            }
    // Bounded above by the slab, and non-zero: the mark hit the entering slab
    // and only the entering slab.
    int64_t wordsChanged = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        if (words[i] != beforeMark[i]) ++wordsChanged;
    }
    CHECK(wordsChanged > 0);
    CHECK(wordsChanged <= int64_t(entering) * kFluidRecentreStepVoxels *
                              kFluidRecentreStepVoxels *
                              (kFluidRecentreStepVoxels / kFluidBitsPerWord));
    for (int32_t lx = 0; lx < 128; lx += 3)
        for (int32_t ly = 0; ly < 64; ly += 5)
            for (int32_t lz = 0; lz < 64; lz += 7) {
                const int64_t wx = origin[0] + lx, wy = origin[1] + ly, wz = origin[2] + lz;
                CHECK_EQ(fluidSolidAtVoxel(words.data(), origin, wrap, wx, wy, wz),
                         isSolidForFluid(wrapTerrain(wx, wy, wz)));
            }
}

VXC_TEST(fluidocc_recentre_survives_a_full_lap_of_the_ring) {
    // Eight steps of 64 voxels is 512: the offset comes back to zero and the
    // window has replaced itself entirely. Nothing in the addressing may drift
    // over a lap -- a rolling window that is only correct for the first lap is
    // worse than none.
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), kFluidVolumeUnbuiltWord);
    int32_t origin[3] = {-777, 4096, -13};
    int32_t wrap[3] = {0, 0, 0};
    const int64_t delta[3] = {kFluidRecentreStepVoxels, 0, 0};

    int32_t laps = 0, refilled = 0;
    for (int step = 0; step < kFluidRecentreCellsPerAxis * 2; ++step) {
        for (int a = 0; a < 3; ++a) {
            origin[a] = static_cast<int32_t>(origin[a] + delta[a]);
            wrap[a] = fluidWrapOffsetAfterMove(wrap[a], delta[a]);
        }
        CHECK(fluidWrapOffsetIsAligned(wrap));
        // Refill ONLY the entering cells, which is the whole point.
        refilled += fluidForEachEnteringCell(delta, [&](const FluidRegion& r) {
            fluidMarkRegionUnbuilt(words.data(), wrap, r);
            fillRegionFromWorld(words.data(), wrap, origin, r);
        });
        if (wrap[0] == 0) ++laps;
    }
    CHECK_EQ(laps, 2);
    CHECK_EQ(wrap[0], 0);
    // 16 steps of 64 cells: two whole volumes' worth, incrementally.
    CHECK_EQ(refilled, 16 * kFluidRecentreCellsPerAxis * kFluidRecentreCellsPerAxis);

    // After two laps of incremental refills the ENTIRE window agrees with the
    // world, and it was never rebuilt wholesale.
    int64_t checkedCells = 0, solids = 0;
    for (int32_t lx = 0; lx < kFluidVolumeDimVoxels; lx += 13)
        for (int32_t ly = 0; ly < kFluidVolumeDimVoxels; ly += 61)
            for (int32_t lz = 0; lz < kFluidVolumeDimVoxels; lz += 59) {
                const int64_t wx = origin[0] + lx, wy = origin[1] + ly, wz = origin[2] + lz;
                const bool want = isSolidForFluid(wrapTerrain(wx, wy, wz));
                CHECK_EQ(fluidSolidAtVoxel(words.data(), origin, wrap, wx, wy, wz), want);
                solids += want ? 1 : 0;
                ++checkedCells;
            }
    CHECK(checkedCells > 1000);
    CHECK(solids > checkedCells / 8);
    CHECK(solids < (checkedCells * 7) / 8);
}

VXC_TEST(fluidocc_particle_rebase_is_exact_toward_the_origin) {
    // CONTRACT ITEM 8's precision claim, pinned. The delta is whole voxels, so
    // the shift is an exact multiple of 10 UU; the subtraction is EXACT whenever
    // the particle moves toward the new origin, and otherwise rounds by at most
    // half a ULP at the window's far corner.
    CHECK_EQ(fluidRebaseDeltaUU(kFluidRecentreStepVoxels), 640.0f);
    CHECK_EQ(fluidRebaseDeltaUU(-kFluidRecentreStepVoxels), -640.0f);
    CHECK_EQ(fluidRebaseDeltaUU(0), 0.0f);
    CHECK_EQ(fluidRebaseDeltaUU(kFluidVolumeDimVoxels), 5120.0f);

    // Half a ULP at 5120 UU. 5120 lies in [2^12, 2^13), so the ULP is 2^-11 and
    // the bound is 2^-12 UU = 2.4 um -- one fortieth of the collision skin.
    const float bound = 1.0f / 4096.0f;
    CHECK(bound * 40.0f < kFluidCollisionSkinUU * 1.001f);

    // Positions across the window on the rest lattice, plus adversarial ones
    // whose mantissas run all the way down. The lattice values alone would make
    // this test pass vacuously: p = i*10 + 95/256 is a multiple of 2^-8 and so
    // is every delta, so the sum is representable in both directions and
    // NOTHING ever rounds. The fine-mantissa values are the ones that exercise
    // the bound, and the count below insists they did.
    std::vector<float> positions;
    for (int i = 0; i <= 512; ++i) positions.push_back(static_cast<float>(i) * 10.0f + 0.37109375f);
    const float fine[] = {0.1f,        0.3f,        1.0f / 3.0f, 3.14159265f, 0.001f,
                          1234.5678f,  4999.9995f,  2047.3331f,  17.000001f,  5119.9995f};
    for (float f : fine) positions.push_back(f);

    int exactCount = 0, roundedCount = 0, checked = 0;
    for (int32_t dv = -512; dv <= 512; dv += kFluidRecentreStepVoxels) {
        const float d = fluidRebaseDeltaUU(dv);
        for (float p : positions) {
            const double want = static_cast<double>(p) - static_cast<double>(d);
            // A particle the move pushes outside the window is boundary-despawned,
            // so the claim is only about results that stay in [0, 5120] -- which
            // is also where "half a ULP at 5120 UU" is the right bound.
            if (std::fabs(want) > 5120.0) continue;
            const float got = p - d;
            const double err = std::fabs(static_cast<double>(got) - want);
            if (std::fabs(want) <= static_cast<double>(p)) {
                CHECK_EQ(err, 0.0);        // toward the origin: exact, always
                ++exactCount;
            } else {
                CHECK(err <= static_cast<double>(bound));
                if (err > 0.0) ++roundedCount;
            }
            ++checked;
        }
    }
    CHECK(checked > 4000);
    CHECK(exactCount > 1000);
    // Non-vacuous the other way too: the away-from-the-origin case really can
    // round, so the bound above is exercised and not merely asserted.
    CHECK(roundedCount > 0);

    // Round trip: undoing the move brings a position back inside the bound.
    for (int32_t dv = -512; dv <= 512; dv += kFluidRecentreStepVoxels) {
        const float d = fluidRebaseDeltaUU(dv);
        const float p = 2560.5f;
        const float back = (p - d) + d;
        CHECK(std::fabs(back - p) <= bound);
    }

    // The representable bound is a real limit and it is stated in voxels.
    CHECK(fluidRebaseIsExactlyRepresentable(0));
    CHECK(fluidRebaseIsExactlyRepresentable(kFluidVolumeDimVoxels));
    CHECK(fluidRebaseIsExactlyRepresentable(kFluidRebaseExactMaxVoxels));
    CHECK(!fluidRebaseIsExactlyRepresentable(kFluidRebaseExactMaxVoxels + 1));
    CHECK(!fluidRebaseIsExactlyRepresentable(-kFluidRebaseExactMaxVoxels - 1));
    // 167 km, far past anything a recentre can produce: the window is 51.2 m
    // and the step is 6.4 m.
    CHECK_EQ(kFluidRebaseExactMaxVoxels * kVoxelSizeMm / 1000000, 167);
}

VXC_TEST(fluidocc_mark_unbuilt_touches_only_its_region) {
    std::vector<uint32_t> words(static_cast<size_t>(kFluidVolumeWords), 0u);   // all AIR
    const int32_t wrap[3] = {192, 448, 64};
    const FluidRegion r = cellRegion(3, 4, 5);

    fluidMarkRegionUnbuilt(words.data(), wrap, r);

    int64_t solidsInside = 0, solidsOutside = 0, probed = 0;
    for (int32_t lx = 0; lx < kFluidVolumeDimVoxels; lx += 17)
        for (int32_t ly = 0; ly < kFluidVolumeDimVoxels; ly += 19)
            for (int32_t lz = 0; lz < kFluidVolumeDimVoxels; lz += 23) {
                const bool inside = lx >= r.minVoxel[0] && lx < r.minVoxel[0] + r.sizeVoxels[0] &&
                                    ly >= r.minVoxel[1] && ly < r.minVoxel[1] + r.sizeVoxels[1] &&
                                    lz >= r.minVoxel[2] && lz < r.minVoxel[2] + r.sizeVoxels[2];
                const bool bit = fluidVolumeGetBitLocal(words.data(), wrap, lx, ly, lz);
                CHECK_EQ(bit, inside);
                if (inside) { ++solidsInside; } else if (bit) { ++solidsOutside; }
                ++probed;
            }
    CHECK(probed > 1000);
    CHECK_EQ(solidsOutside, 0);
    CHECK(solidsInside > 0);   // non-vacuous: the probe grid really hit the cell
}

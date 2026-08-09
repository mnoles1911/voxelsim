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
    CHECK(!fluidSolidAtVoxel(words.data(), origin, 1000, -2000, 30));
    CHECK(!fluidSolidAtVoxel(words.data(), origin, 1000 + 511, -2000 + 511, 30 + 511));

    // One voxel past each of the six faces: solid.
    CHECK(fluidSolidAtVoxel(words.data(), origin, 999, -2000, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000 + 512, -2000, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000, -2001, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000, -2000 + 512, 30));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000, -2000, 29));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000, -2000, 30 + 512));

    // Far away in every direction, including the negative side, which is
    // where a truncating conversion would wrap back inside.
    CHECK(fluidSolidAtVoxel(words.data(), origin, -1000000, 0, 0));
    CHECK(fluidSolidAtVoxel(words.data(), origin, 1000000, 0, 0));
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
                CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, x, y, z));
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

    fluidFillRegion(words.data(), r, brickWords.data());

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
                CHECK_EQ(fluidVolumeGetBit(words.data(), lx, ly, lz), want);
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
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, r.minVoxel[0] - 1, r.minVoxel[1], r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, r.minVoxel[0] + r.sizeVoxels[0],
                            r.minVoxel[1], r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, r.minVoxel[0], r.minVoxel[1] - 1, r.minVoxel[2]));
    CHECK(fluidSolidAtVoxel(words.data(), kZeroOrigin, r.minVoxel[0], r.minVoxel[1], r.minVoxel[2] - 1));
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
    fluidFillRegion(words.data(), r, brickWords.data());

    const auto solidAt = [&](int64_t vx, int64_t vy, int64_t vz) {
        return fluidSolidAtVoxel(words.data(), kZeroOrigin, vx, vy, vz);
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

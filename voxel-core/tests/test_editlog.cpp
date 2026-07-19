// Edit log format + replay invariant (plan §5 task 5):
// world(tiles) + log == world(tiles) + log, always.

#include "voxelcore/world.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 424242;
}

VXC_TEST(editlog_normalize_last_write_wins) {
    std::vector<EditCell> cells = {{5, MAT_ROCK}, {2, MAT_SAND}, {5, MAT_AIR}, {2, MAT_SAND}};
    EditLog::normalize(cells);
    CHECK_EQ(cells.size(), size_t(2));
    CHECK_EQ(cells[0].cell, 2);
    CHECK_EQ(cells[0].mat, MAT_SAND);
    CHECK_EQ(cells[1].cell, 5);
    CHECK_EQ(cells[1].mat, MAT_AIR); // the later write
}

VXC_TEST(editlog_serialize_parse_roundtrip_sparse) {
    EditLog log(kSeed, 16);
    log.append(BrickKey{1, -2, 3}, {{0, MAT_AIR}, {100, MAT_ROCK}, {4095, MAT_SAND}});
    log.append(BrickKey{-7, 0, 12}, {{9, MAT_TOPSOIL}});
    std::vector<uint8_t> bytes;
    log.serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->seed(), kSeed);
    CHECK_EQ(int(parsed->brickEdge()), 16);
    CHECK(parsed->entries() == log.entries());
}

VXC_TEST(editlog_full_brick_uses_rle_and_roundtrips) {
    EditLog log(kSeed, 8);
    // Full-brick rewrite: bottom half rock, top half air -> 2 RLE runs.
    std::vector<EditCell> cells;
    for (uint16_t i = 0; i < 512; ++i)
        cells.push_back({i, i < 256 ? MAT_ROCK : MAT_AIR});
    log.append(BrickKey{0, 0, 0}, cells);
    std::vector<uint8_t> bytes;
    log.serialize(bytes);
    // Header (29B) + entry header (21B) + RLE payload (3B + 2 runs x 3B).
    CHECK(bytes.size() < 70);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->entries() == log.entries());
}

VXC_TEST(editlog_rejects_corrupt_input) {
    EditLog log(kSeed, 16);
    log.append(BrickKey{0, 0, 0}, {{7, MAT_ROCK}});
    std::vector<uint8_t> bytes;
    log.serialize(bytes);
    CHECK(!EditLog::parse(bytes.data(), bytes.size() - 1).has_value()); // truncated
    auto bad = bytes;
    bad[0] ^= 0xff; // magic
    CHECK(!EditLog::parse(bad.data(), bad.size()).has_value());
    auto badVer = bytes;
    badVer[4] = 0x77; // format version
    CHECK(!EditLog::parse(badVer.data(), badVer.size()).has_value());
}

VXC_TEST(editlog_replay_identity) {
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    World<16> a(kSeed, tilesA);

    // A spread of edits: dig a crater, build a pillar, overwrite some cells.
    const ColumnSample col = a.amplifier().column(0, 0);
    const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
    for (int64_t x = -5; x <= 5; ++x)
        for (int64_t y = -5; y <= 5; ++y)
            for (int64_t z = topVz - 3; z <= topVz; ++z)
                if (x * x + y * y + (z - topVz) * (z - topVz) < 20)
                    a.setVoxel(x, y, z, MAT_AIR);
    for (int64_t z = topVz; z < topVz + 12; ++z) a.setVoxel(20, 20, z, MAT_ROCK);
    a.setVoxel(20, 20, topVz + 5, MAT_SAND); // overwrite mid-pillar

    // Serialize the log, parse it, replay into a fresh world over the same
    // tiles: every edited brick must be bit-identical.
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());

    World<16> b(kSeed, tilesB);
    CHECK(b.replay(*parsed));
    CHECK_EQ(a.editedDigest(), b.editedDigest());
    CHECK_EQ(b.materialAt(20, 20, topVz + 5), MAT_SAND);
    CHECK_EQ(b.materialAt(0, 0, topVz), MAT_AIR);

    // Replaying the same log twice from serialized state stays identical
    // (append-only: a second serialize of b's log equals a's).
    std::vector<uint8_t> bytesB;
    b.log().serialize(bytesB);
    CHECK(bytes == bytesB);
}

VXC_TEST(editlog_replay_rejects_wrong_seed) {
    SyntheticTileSampler tiles(kSeed), tilesOther(kSeed + 1);
    World<16> a(kSeed, tiles);
    a.setVoxel(0, 0, 0, MAT_ROCK);
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    World<16> other(kSeed + 1, tilesOther);
    CHECK(!other.replay(*parsed));
}

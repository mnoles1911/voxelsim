// Offline edit-log compaction (plan §3.2: "edit log is append-only,
// compacted offline"; doctrine §2.1: "only diffs are ever stored").
//
// Invariant under test: replaying compactLog(log) must reproduce exactly
// the same edited-brick contents as replaying log itself, while collapsing
// each brick's history to one entry over only the cells it ever touched.

#include <algorithm>
#include <vector>

#include "voxelcore/editcompact.h"
#include "voxelcore/world.h"
#include "vxctest.h"

using namespace vxc;

namespace {
constexpr uint64_t kSeed = 424242;

// A deliberately messy edit sequence: a multi-brick crater dig, a pillar
// build with an overwrite-then-revert in the middle, a dig-then-refill
// (net no-op on that cell), and repeated flip-flopping on a single voxel —
// crossing several separate log entries so compaction has real work to do.
EditLog buildMessyLog() {
    SyntheticTileSampler tiles(kSeed);
    World<16> w(kSeed, tiles);
    const ColumnSample col = w.amplifier().column(0, 0);
    const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);

    // Dig a crater (spans multiple bricks).
    for (int64_t x = -5; x <= 5; ++x)
        for (int64_t y = -5; y <= 5; ++y)
            for (int64_t z = topVz - 3; z <= topVz; ++z)
                if (x * x + y * y + (z - topVz) * (z - topVz) < 20)
                    w.setVoxel(x, y, z, MAT_AIR);

    // Build a pillar in a different brick, then partially revert it.
    for (int64_t z = topVz; z < topVz + 12; ++z) w.setVoxel(20, 20, z, MAT_ROCK);
    w.setVoxel(20, 20, topVz + 5, MAT_SAND); // overwrite mid-pillar
    w.setVoxel(20, 20, topVz + 5, MAT_ROCK); // revert back to original
    w.setVoxel(20, 20, topVz + 2, MAT_AIR);  // dig into the pillar
    w.setVoxel(20, 20, topVz + 2, MAT_ROCK); // fill it back in (net no-op cell)

    // Flip-flop a single voxel across five separate entries.
    for (int i = 0; i < 5; ++i)
        w.setVoxel(0, 0, topVz - 1, (i % 2 == 0) ? MAT_AIR : MAT_SAND);

    return w.log();
}
} // namespace

VXC_TEST(editcompact_replay_equivalence) {
    EditLog log = buildMessyLog();

    SyntheticTileSampler tilesOrig(kSeed), tilesCompact(kSeed);
    World<16> wOrig(kSeed, tilesOrig);
    World<16> wCompact(kSeed, tilesCompact);
    CHECK(wOrig.replay(log));

    EditLog compacted = compactLog(log);
    CHECK(wCompact.replay(compacted));

    CHECK_EQ(wOrig.editedDigest(), wCompact.editedDigest());
}

VXC_TEST(editcompact_one_entry_per_brick_ordered_contiguous) {
    EditLog log = buildMessyLog();
    EditLog compacted = compactLog(log);

    std::vector<BrickKey> touched;
    for (const EditEntry& e : log.entries()) touched.push_back(e.key);
    std::sort(touched.begin(), touched.end(), BrickKeyLess{});
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    CHECK_EQ(compacted.entries().size(), touched.size());
    for (size_t i = 0; i < compacted.entries().size(); ++i) {
        CHECK_EQ(compacted.entries()[i].seq, uint64_t(i));
        CHECK(compacted.entries()[i].key == touched[i]);
        if (i > 0) CHECK(BrickKeyLess{}(touched[i - 1], touched[i])); // strictly increasing
    }
}

VXC_TEST(editcompact_idempotent) {
    EditLog log = buildMessyLog();
    EditLog c1 = compactLog(log);
    EditLog c2 = compactLog(c1);

    CHECK_EQ(c1.seed(), c2.seed());
    CHECK_EQ(int(c1.brickEdge()), int(c2.brickEdge()));
    CHECK(c1.entries() == c2.entries());
}

VXC_TEST(editcompact_shrinks_repeated_overwrites) {
    EditLog log(kSeed, 16);
    for (int i = 0; i < 100; ++i)
        log.append(BrickKey{3, -1, 2}, {{42, (i % 2 == 0) ? MAT_ROCK : MAT_SAND}});

    EditLog compacted = compactLog(log);
    CHECK_EQ(compacted.entries().size(), size_t(1));
    CHECK_EQ(compacted.entries()[0].cells.size(), size_t(1));
    CHECK_EQ(compacted.entries()[0].cells[0].cell, uint16_t(42));
    CHECK_EQ(compacted.entries()[0].cells[0].mat, MAT_SAND); // i=99 (odd) wrote last

    std::vector<uint8_t> origBytes, compactBytes;
    log.serialize(origBytes);
    compacted.serialize(compactBytes);
    CHECK(compactBytes.size() * 10 < origBytes.size());
}

VXC_TEST(editcompact_serialize_parse_roundtrip) {
    EditLog log = buildMessyLog();
    EditLog compacted = compactLog(log);

    std::vector<uint8_t> bytes;
    compacted.serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->seed(), compacted.seed());
    CHECK_EQ(int(parsed->brickEdge()), int(compacted.brickEdge()));
    CHECK(parsed->entries() == compacted.entries());
}

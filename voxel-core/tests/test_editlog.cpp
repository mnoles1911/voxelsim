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
    // Header (29B v1 fields + 2B empty providerId = 31B) + entry header
    // (21B) + RLE payload (3B + 2 runs x 3B).
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

// --- provider_id (data-integrity gap: a log recorded against one tile
// provider must not silently replay onto a DIFFERENT one) ---------------

VXC_TEST(editlog_provider_id_roundtrips_and_checks) {
    EditLog log(kSeed, 16, "tileset-A-deadbeef");
    log.append(BrickKey{0, 0, 0}, {{7, MAT_ROCK}});
    std::vector<uint8_t> bytes;
    log.serialize(bytes);

    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->providerId() == "tileset-A-deadbeef");
    CHECK(parsed->checkProvider("tileset-A-deadbeef") == EditLog::ProviderCheck::kMatch);
    CHECK(parsed->checkProvider("tileset-B-cafefeed") == EditLog::ProviderCheck::kMismatch);
}

VXC_TEST(editlog_provider_id_empty_string_is_unstamped) {
    // A log explicitly written with providerId="" (the default) is
    // indistinguishable from an old log that never had the field — both
    // report kUnstamped, never kMismatch, no matter what's asked.
    EditLog log(kSeed, 8);
    CHECK(log.providerId().empty());
    CHECK(log.checkProvider("anything") == EditLog::ProviderCheck::kUnstamped);
    CHECK(log.checkProvider("") == EditLog::ProviderCheck::kUnstamped);
}

VXC_TEST(editlog_parses_legacy_v1_bytes_without_provider_field) {
    // Hand-build a version-1 header (no providerId field at all) to prove
    // logs written before provider stamping existed still load — backward
    // compatibility is a hard requirement, not just "new files still work".
    std::vector<uint8_t> bytes;
    ByteWriter w(bytes);
    w.u32(EditLog::kMagic);
    w.u32(1); // kFormatVersion == 1, pre-provider-stamp
    w.u32(kWorldGenVersion);
    w.u64(kSeed);
    w.u8(16);
    w.u64(0); // zero entries

    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value()); // must NOT hard-fail on a pre-stamp log
    CHECK_EQ(parsed->seed(), kSeed);
    CHECK(parsed->providerId().empty());
    CHECK(parsed->checkProvider("some-provider") == EditLog::ProviderCheck::kUnstamped);
}

VXC_TEST(editlog_rejects_format_version_newer_than_supported) {
    std::vector<uint8_t> bytes;
    ByteWriter w(bytes);
    w.u32(EditLog::kMagic);
    w.u32(EditLog::kFormatVersion + 1); // from-the-future version
    w.u32(kWorldGenVersion);
    w.u64(kSeed);
    w.u8(16);
    w.u64(0);
    CHECK(!EditLog::parse(bytes.data(), bytes.size()).has_value());
}

VXC_TEST(editlog_replay_refuses_provider_mismatch) {
    SyntheticTileSampler tilesA(kSeed);
    World<16> a(kSeed, tilesA, "tileset-A-deadbeef");
    a.setVoxel(0, 0, 0, MAT_ROCK);
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());

    // Same seed/brickEdge (so the pre-existing guard would pass), but a
    // DIFFERENT provider identity for the destination world -- this is
    // exactly the silent-corruption case: replaying tile-set-A edits onto
    // tile-set-B terrain. Must be refused, not silently applied.
    SyntheticTileSampler tilesB(kSeed);
    World<16> b(kSeed, tilesB, "tileset-B-cafefeed");
    CHECK(!b.replay(*parsed, "tileset-B-cafefeed"));
    CHECK(b.lastProviderCheck() == EditLog::ProviderCheck::kMismatch);
    CHECK_EQ(b.editedBricks().size(), size_t(0)); // refused before anything was applied
}

VXC_TEST(editlog_replay_accepts_provider_match) {
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    World<16> a(kSeed, tilesA, "tileset-A-deadbeef");
    a.setVoxel(0, 0, 0, MAT_ROCK);
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());

    World<16> b(kSeed, tilesB, "tileset-A-deadbeef");
    CHECK(b.replay(*parsed, "tileset-A-deadbeef"));
    CHECK(b.lastProviderCheck() == EditLog::ProviderCheck::kMatch);
    CHECK_EQ(b.materialAt(0, 0, 0), MAT_ROCK);
}

VXC_TEST(editlog_replay_warns_but_accepts_unstamped_log) {
    // A log with no provider stamp (recorded before this feature, or by a
    // caller with no provider identity) must still load -- refusing it
    // outright would hard-fail every log that predates this change.
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    World<16> a(kSeed, tilesA); // no providerId -- unstamped
    a.setVoxel(0, 0, 0, MAT_ROCK);
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());

    World<16> b(kSeed, tilesB, "tileset-A-deadbeef");
    CHECK(b.replay(*parsed, "tileset-A-deadbeef")); // proceeds despite no stamp
    CHECK(b.lastProviderCheck() == EditLog::ProviderCheck::kUnstamped); // caller should warn
    CHECK_EQ(b.materialAt(0, 0, 0), MAT_ROCK);
}

VXC_TEST(editlog_replay_skips_check_when_no_provider_given) {
    // Passing no currentProviderId (the default) is a deliberate opt-out --
    // existing callers (compact/verify tooling, older tests) that never had
    // a provider identity must see byte-for-byte the old behavior.
    SyntheticTileSampler tilesA(kSeed), tilesB(kSeed);
    World<16> a(kSeed, tilesA, "tileset-A-deadbeef");
    a.setVoxel(0, 0, 0, MAT_ROCK);
    std::vector<uint8_t> bytes;
    a.log().serialize(bytes);
    auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());

    World<16> b(kSeed, tilesB); // different (unstamped) destination
    CHECK(b.replay(*parsed)); // no currentProviderId -> check skipped, always proceeds
    CHECK(!b.lastProviderCheck().has_value());
}

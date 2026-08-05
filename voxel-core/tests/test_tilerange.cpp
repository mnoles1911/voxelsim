// Tests for voxelcore/tilerange.h and the PARTIAL half of voxelcore/tilestore.h
// -- holding, planning and fetching a subset of a .vxtl v2 tile (task #52).
//
// THE PROPERTY THESE TESTS EXIST FOR, above all the others: a block whose bytes
// were never fetched must report NOT RESIDENT and must never decode to 0. On the
// elevation plane 0 is sea level, and tilestreaming.h is explicit that sea level
// on the fine tier is a different world rather than a degraded one -- so the
// failure mode of getting this wrong is not a visible glitch near the bug, it is
// terrain no other client reproduces, surfacing as a rendering problem somewhere
// far away. Several tests below therefore assert on the OUT PARAMETER staying
// untouched, not merely on the return value being false.
//
// The other half is the three layout facts that each produce a wrong fetcher
// (docs/tile-slicing-2026-08-04.md §2): CONSTANT blocks own no bytes, the
// preamble is four disjoint regions rather than a prefix, and planning must be
// by file offset rather than by (by,bx).

#include "voxelcore/tilerange.h"

#include <algorithm>  // std::equal
#include <cstring>    // std::memcpy -- MSVC supplies it transitively, libstdc++ does not
#include <filesystem>
#include <optional>

#include "voxelcore/tilestore.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// The Python-encoded golden v2 tiles, same provenance and same skip-if-absent
// rule as test_tilestore.cpp's: written by the ENCODER half of the contract
// against the same frozen document, with no code shared with this side.
std::filesystem::path goldenPath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_512.vxtl";
}
std::filesystem::path goldenWaterPath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_water_512.vxtl";
}
std::filesystem::path goldenBasinPath() {
    return std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_basins_512.vxtl";
}

std::optional<std::vector<uint8_t>> loadFixture(const std::filesystem::path& p) {
    if (!std::filesystem::exists(p)) return std::nullopt;
    return readFileBytes(p);
}

} // namespace

// ---------------------------------------------------------------------------
// FineTileBytes: the sparse store itself.
// ---------------------------------------------------------------------------

VXC_TEST(finetilebytes_whole_covers_everything) {
    FineTileBytes b = FineTileBytes::whole(std::vector<uint8_t>{1, 2, 3, 4, 5});
    CHECK_EQ(b.fileSize(), uint64_t(5));
    CHECK_EQ(b.residentBytes(), uint64_t(5));
    CHECK(b.isWhole());
    CHECK(b.covers(0, 5));
    CHECK(b.covers(2, 3));
    CHECK_EQ(int(*b.span(2, 1)), 3);
    // Past the end is absent, not clamped.
    CHECK(!b.covers(3, 3));
}

VXC_TEST(finetilebytes_absent_span_is_null_never_zeroes) {
    FineTileBytes b = FineTileBytes::forFile(100);
    CHECK(b.addSegment(10, std::vector<uint8_t>{7, 7, 7}));
    CHECK_EQ(b.residentBytes(), uint64_t(3));
    // Held.
    CHECK(b.covers(10, 3));
    CHECK_EQ(int(*b.span(11, 1)), 7);
    // NOT held -- and the answer is nullptr, not a buffer of zeroes. This is the
    // whole reason the class exists: a caller cannot accidentally read an
    // unfetched byte as a value.
    CHECK(b.span(0, 3) == nullptr);
    CHECK(b.span(9, 2) == nullptr);   // straddles the start of the segment
    CHECK(b.span(12, 2) == nullptr);  // straddles the end
    CHECK(b.span(50, 1) == nullptr);
}

VXC_TEST(finetilebytes_rejects_overlap_and_past_eof) {
    FineTileBytes b = FineTileBytes::forFile(100);
    CHECK(b.addSegment(10, std::vector<uint8_t>{1, 2, 3, 4}));
    // Overlap is REFUSED rather than merged: two answers for one byte can only
    // come from a mis-planned fetch or a transport that served a range other
    // than the one asked for, and both must be loud.
    CHECK(!b.addSegment(12, std::vector<uint8_t>{9, 9}));
    CHECK(!b.addSegment(8, std::vector<uint8_t>{9, 9, 9}));
    CHECK_EQ(b.residentBytes(), uint64_t(4));  // no state change on refusal
    CHECK_EQ(int(*b.span(12, 1)), 3);          // original bytes intact
    // Past the declared file length is refused too -- that is a truncated
    // download or a bad plan, not something to store.
    CHECK(!b.addSegment(98, std::vector<uint8_t>{1, 2, 3, 4}));
}

VXC_TEST(finetilebytes_merges_adjacent_into_one_span) {
    FineTileBytes b = FineTileBytes::forFile(100);
    // Deliberately out of order, and meeting exactly: a coalesced plan can
    // arrive as several requests, and each block's payload must still present
    // as ONE contiguous span afterwards or its decode fails for no good reason.
    CHECK(b.addSegment(20, std::vector<uint8_t>{3, 4}));
    CHECK(b.addSegment(10, std::vector<uint8_t>{1, 2}));
    CHECK(b.addSegment(12, std::vector<uint8_t>(8, 0)));
    CHECK_EQ(b.segmentCount(), size_t(1));
    CHECK(b.covers(10, 12));
    CHECK_EQ(int(*b.span(21, 1)), 4);
}

// ---------------------------------------------------------------------------
// planBlockRanges: the three layout traps.
// ---------------------------------------------------------------------------

namespace {

FineBlockEntry coded(uint64_t off, uint32_t len) {
    FineBlockEntry e;
    e.mode = kBlockCoded;
    e.offset = off;
    e.compLen = len;
    e.residBits = 16;
    return e;
}
FineBlockEntry constant(int16_t cp) {
    FineBlockEntry e;
    e.mode = kBlockConstant;
    e.constCp = cp;
    return e;
}

} // namespace

VXC_TEST(plan_drops_constant_blocks_entirely) {
    // TRAP #1. A CONSTANT block's (offset=0, comp_len=0) is NOT a range -- byte
    // 0 of the data section belongs to a different block. Asking for it would
    // fetch someone else's bytes and decode them as this block.
    const std::vector<FineBlockEntry> index{constant(5), constant(-3)};
    const std::vector<RangePlan> plans = planBlockRanges(index, 1000, {0, 1});
    CHECK(plans.empty());  // and that is SUCCESS: nothing needs fetching
}

VXC_TEST(plan_sorts_by_file_offset_not_by_block_order) {
    // TRAP #3. The encoder decided the file order. Block 0 living AFTER block 1
    // in the file is unusual but permitted, and a planner that sorted by index
    // would emit two backwards spans instead of one.
    const std::vector<FineBlockEntry> index{coded(500, 100), coded(0, 100)};
    const std::vector<RangePlan> plans = planBlockRanges(index, 0, {0, 1}, 1000);
    CHECK_EQ(plans.size(), size_t(1));
    CHECK_EQ(plans[0].span.start, uint64_t(0));
    CHECK_EQ(plans[0].span.length, uint64_t(600));
    CHECK_EQ(plans[0].usefulBytes, uint64_t(200));
    CHECK_EQ(plans[0].wastedBytes(), uint64_t(400));
    // Reported in file order, which is the order the blocks' bytes arrive in.
    CHECK_EQ(plans[0].blocks[0], uint32_t(1));
    CHECK_EQ(plans[0].blocks[1], uint32_t(0));
}

VXC_TEST(plan_coalesces_adjacent_and_splits_beyond_the_gap) {
    const std::vector<FineBlockEntry> index{coded(0, 100), coded(100, 100), coded(10000, 100)};
    // Gap 0: the first two are exactly adjacent and still merge; the third is
    // 9,800 B away and does not.
    const std::vector<RangePlan> tight = planBlockRanges(index, 0, {0, 1, 2}, 0);
    CHECK_EQ(tight.size(), size_t(2));
    CHECK_EQ(tight[0].span.length, uint64_t(200));
    CHECK_EQ(tight[0].wastedBytes(), uint64_t(0));
    CHECK_EQ(tight[1].span.start, uint64_t(10000));
    // A gap wide enough bridges it, paying the hole in wasted bytes to save a
    // request. This is the whole bandwidth-delay-product argument, in miniature.
    const std::vector<RangePlan> loose = planBlockRanges(index, 0, {0, 1, 2}, 77 * 1024);
    CHECK_EQ(loose.size(), size_t(1));
    CHECK_EQ(loose[0].span.length, uint64_t(10100));
    CHECK_EQ(loose[0].usefulBytes, uint64_t(300));
    CHECK_EQ(loose[0].wastedBytes(), uint64_t(9800));
}

VXC_TEST(plan_is_offset_relative_to_the_data_section) {
    // An index entry's `offset` is into its plane's DATA section, not the file.
    // Forgetting `dataOffset` reads the elevation section's bytes as water.
    const std::vector<FineBlockEntry> index{coded(64, 32)};
    const std::vector<RangePlan> plans = planBlockRanges(index, 51'533'051, {0});
    CHECK_EQ(plans.size(), size_t(1));
    CHECK_EQ(plans[0].span.start, uint64_t(51'533'115));
}

VXC_TEST(plan_deduplicates_so_wasted_bytes_stays_honest) {
    const std::vector<FineBlockEntry> index{coded(0, 100)};
    const std::vector<RangePlan> plans = planBlockRanges(index, 0, {0, 0, 0});
    CHECK_EQ(plans.size(), size_t(1));
    CHECK_EQ(plans[0].usefulBytes, uint64_t(100));  // not 300
    CHECK_EQ(plans[0].wastedBytes(), uint64_t(0));
}

// ---------------------------------------------------------------------------
// The preamble reader, and the partial parse it feeds.
// ---------------------------------------------------------------------------

VXC_TEST(preamble_only_parses_and_costs_one_request_for_ground) {
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;  // fixture absent: skip, as the other v2 tests do

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    FineError err = FineError::kNone;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, &err));
    CHECK_EQ(int(err), int(FineError::kNone));
    // ONE request: header, section table and ELEV_INDEX are contiguous and all
    // land inside the single head probe.
    CHECK_EQ(src.requests, uint64_t(1));
    CHECK(held.residentBytes() < bytes->size());

    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held), {}, &err);
    CHECK(tile.has_value());
    if (!tile) return;
    // The index is fully parsed even though almost no data bytes are held --
    // that is what makes a fetch plannable.
    CHECK_EQ(tile->elevIndex().size(), size_t(tile->blockCount()));
    CHECK(!tile->isWholeFile());
    CHECK(tile->residentFileBytes() < bytes->size());
}

VXC_TEST(unfetched_block_reports_not_resident_and_writes_nothing) {
    // THE RULE. This is the test the whole change exists to make true.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    // Find a block that actually costs bytes. (A CONSTANT one would be resident
    // already, which is a different and also-tested case.)
    const uint32_t perAxis = tile->blocksPerAxis();
    bool foundCoded = false;
    for (uint32_t id = 0; id < tile->elevIndex().size() && !foundCoded; ++id) {
        if (tile->elevIndex()[id].mode == kBlockConstant) continue;
        const uint32_t bx = id % perAxis, by = id / perAxis;
        foundCoded = true;

        CHECK(!tile->elevBlockResident(bx, by));

        // A sentinel the decoder must NOT overwrite. If this comes back as
        // zeroes, an unfetched block has just become sea-level terrain.
        std::vector<int16_t> out(tile->blockPixelCount(), int16_t(-12345));
        FineError err = FineError::kNone;
        CHECK(!tile->decodeElevBlock(bx, by, out, &err));
        CHECK_EQ(int(err), int(FineError::kBlockNotResident));
        CHECK_EQ(out.size(), size_t(tile->blockPixelCount()));
        CHECK_EQ(int(out[0]), -12345);
        CHECK_EQ(int(out[out.size() - 1]), -12345);

        // The single-point accessor tells the same story rather than a
        // convenient one.
        int16_t cp = 999;
        FineError cpErr = FineError::kNone;
        CHECK(!tile->controlPointAt(bx << tile->blockLog2(), by << tile->blockLog2(), cp, &cpErr));
        CHECK_EQ(int(cpErr), int(FineError::kBlockNotResident));
        CHECK_EQ(int(cp), 999);
    }
    CHECK(foundCoded);
}

VXC_TEST(constant_blocks_are_resident_with_zero_bytes_fetched) {
    // The reason a water refresh is cheap: on the shipped tiles 72-87% of the
    // water plane is CONSTANT and costs no request at all. A residency test that
    // demanded bytes for these would block a client holding everything it needs.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;

    const uint32_t perAxis = tile->blocksPerAxis();
    int checked = 0;
    for (uint32_t id = 0; id < tile->elevIndex().size(); ++id) {
        if (tile->elevIndex()[id].mode != kBlockConstant) continue;
        const uint32_t bx = id % perAxis, by = id / perAxis;
        CHECK(tile->elevBlockResident(bx, by));
        std::vector<int16_t> a, b;
        CHECK(tile->decodeElevBlock(bx, by, a));
        CHECK(whole->decodeElevBlock(bx, by, b));
        CHECK(a == b);
        ++checked;
    }
    // The golden fixture is built to carry mode diversity; if it ever stopped
    // carrying a CONSTANT block this test would silently pass while testing
    // nothing, so say so.
    CHECK(checked > 0);
}

VXC_TEST(fetched_blocks_decode_byte_identical_to_a_whole_file_parse) {
    // The interop proof for the fetch path: same bytes in, same values out,
    // whether they arrived as a whole file or as planned ranges. A fetcher that
    // grabbed a range one entry off would decode to plausible terrain, not to an
    // error, so nothing but a value comparison catches it.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    const std::vector<uint32_t> ids = fineNonConstantBlocks(*tile, FinePlane::kElevation);
    CHECK(!ids.empty());
    const uint64_t beforeBytes = src.bytesRead;
    CHECK(fetchFineTileBlocks(src, *tile, FinePlane::kElevation, ids));
    CHECK(src.bytesRead > beforeBytes);

    const uint32_t perAxis = tile->blocksPerAxis();
    for (uint32_t id : ids) {
        const uint32_t bx = id % perAxis, by = id / perAxis;
        CHECK(tile->elevBlockResident(bx, by));
        std::vector<int16_t> a, b;
        CHECK(tile->decodeElevBlock(bx, by, a));
        CHECK(whole->decodeElevBlock(bx, by, b));
        CHECK(a == b);
    }
    // Still cheaper than the whole file even after fetching every coded block,
    // because the flow plane and its index were never asked for.
    CHECK(tile->residentFileBytes() <= bytes->size());
}

VXC_TEST(fetching_twice_costs_nothing_the_second_time) {
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    const std::vector<uint32_t> ids = fineNonConstantBlocks(*tile, FinePlane::kElevation);
    CHECK(fetchFineTileBlocks(src, *tile, FinePlane::kElevation, ids));
    const uint64_t reqs = src.requests;
    // Idempotent: already-held blocks are filtered out before planning, which is
    // also what stops the second call being refused by the overlap check.
    CHECK(fetchFineTileBlocks(src, *tile, FinePlane::kElevation, ids));
    CHECK_EQ(src.requests, reqs);
}

// ---------------------------------------------------------------------------
// Water and basins: "not fetched" must not read as "dry" or "no lakes".
// ---------------------------------------------------------------------------

VXC_TEST(unfetched_water_block_is_not_resident_not_dry) {
    const auto bytes = loadFixture(goldenWaterPath());
    if (!bytes) return;
    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = true;
    want.wantBasins = true;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;
    CHECK(tile->hasWater());
    CHECK(tile->waterIndexResident());

    const uint32_t perAxis = tile->blocksPerAxis();
    bool found = false;
    for (uint32_t id = 0; id < tile->waterIndex().size() && !found; ++id) {
        if (tile->waterIndex()[id].mode == kBlockConstant) continue;
        found = true;
        std::vector<int16_t> out(tile->blockPixelCount(), int16_t(-777));
        FineError err = FineError::kNone;
        CHECK(!tile->decodeWaterBlock(id % perAxis, id / perAxis, out, &err));
        // NOT kWaterDryDepth. An unfetched reach must not read as a dry
        // riverbed -- that loses a river with no error anywhere.
        CHECK_EQ(int(err), int(FineError::kBlockNotResident));
        CHECK_EQ(int(out[0]), -777);
    }
    CHECK(found);

    // Now fetch the whole plane, which is what a water-only refresh does.
    const std::vector<uint32_t> wet = fineNonConstantBlocks(*tile, FinePlane::kWater);
    CHECK(fetchFineTileBlocks(src, *tile, FinePlane::kWater, wet));
    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;
    for (uint32_t id : wet) {
        std::vector<int16_t> a, b;
        CHECK(tile->decodeWaterBlock(id % perAxis, id / perAxis, a));
        CHECK(whole->decodeWaterBlock(id % perAxis, id / perAxis, b));
        CHECK(a == b);
    }
}

VXC_TEST(unfetched_flow_index_makes_every_flow_block_not_resident) {
    // A tile that DECLARES a flow plane whose index this client never asked for.
    // Nothing in ue-project reads flow, so this is the shipping configuration --
    // and with no index there is not even a way to know where a block is, so the
    // only honest answer is not-resident. It must not be an out-of-range read
    // either, which is what an empty index would be without the guard.
    const auto bytes = loadFixture(
        std::filesystem::path(VXC_TEST_FIXTURE_DIR) / "vxtl_v2_golden_flow_512.vxtl");
    if (!bytes) return;
    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    CHECK(tile->hasFlow());              // the FORMAT carries one
    CHECK(!tile->flowIndexResident());   // this CLIENT does not hold it
    CHECK(!tile->flowBlockResident(0, 0));
    std::vector<uint8_t> out(tile->blockPixelCount(), uint8_t(0xAB));
    FineError err = FineError::kNone;
    CHECK(!tile->decodeFlowBlock(0, 0, out, &err));
    CHECK_EQ(int(err), int(FineError::kBlockNotResident));
    CHECK_EQ(int(out[0]), 0xAB);
}

VXC_TEST(unfetched_basin_table_is_not_an_empty_one) {
    const auto bytes = loadFixture(goldenBasinPath());
    if (!bytes) return;
    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;
    CHECK(whole->hasBasins());
    CHECK(whole->basinsResident());
    CHECK(!whole->basins().empty());

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;   // deliberately NOT fetched
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));
    std::optional<FineTile> tile = FineTile::parsePartial(std::move(held));
    if (!tile) return;

    // hasBasins() still true -- the tile WAS surveyed -- but the registry is not
    // held, and basins() is empty. Those two facts together would read as "this
    // tile holds no lakes" to anything that looked only at the container, which
    // is why basinsResident() has to exist and why lakes.h consults it.
    CHECK(tile->hasBasins());
    CHECK(!tile->basinsResident());
    CHECK(tile->basins().empty());
}

// ---------------------------------------------------------------------------
// The sampler's honesty counters.
// ---------------------------------------------------------------------------

VXC_TEST(sampler_counts_not_resident_apart_from_decode_failure) {
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;

    BytesRangeSource src(*bytes);
    FinePreambleRequest want;
    want.wantFlow = false;
    want.wantWater = false;
    want.wantBasins = false;
    FineTileBytes held;
    CHECK(readFineTilePreamble(src, src.fileSize(), want, held, nullptr));

    FineTileSampler sampler(whole->seed());
    FineError err = FineError::kNone;
    CHECK(sampler.loadTilePartial(std::move(held), &err));

    // Pick a pixel inside a block that costs bytes and was not fetched.
    const uint32_t perAxis = whole->blocksPerAxis();
    const uint32_t dim = whole->blockDim();
    bool probed = false;
    for (uint32_t id = 0; id < whole->elevIndex().size() && !probed; ++id) {
        if (whole->elevIndex()[id].mode == kBlockConstant) continue;
        probed = true;
        const int64_t px = int64_t(whole->tileX()) * whole->size() + int64_t(id % perAxis) * dim;
        const int64_t py = int64_t(whole->tileY()) * whole->size() + int64_t(id / perAxis) * dim;

        CHECK(!sampler.blockBytesResident(px, py));
        CHECK(!sampler.blockDecoded(px, py));

        int16_t cp = 0;
        CHECK(!sampler.controlPointAt(px, py, cp));
        // The two counters mean opposite things -- "fetch more" versus "these
        // bytes are corrupt, discard the tile". A sliced client trips the first
        // routinely and is not broken; flattening them would make it look so.
        CHECK_EQ(sampler.notResidentBlockQueries.load(), uint64_t(1));
        CHECK_EQ(sampler.blockDecodeFailures.load(), uint64_t(0));
        CHECK_EQ(sampler.missingTileQueries.load(), uint64_t(0));
    }
    CHECK(probed);
}

VXC_TEST(sampler_reports_file_and_decoded_bytes_apart) {
    // Peak memory is the number that matters more than transfer here, and it has
    // two halves that move independently: slicing the FETCH reduces the first,
    // slicing the DECODE reduces the second. Reporting one total would hide
    // which of the two a change actually moved.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    const std::optional<FineTile> whole = FineTile::parse(*bytes);
    if (!whole) return;

    FineTileSampler sampler(whole->seed());
    CHECK(sampler.loadTile(*bytes, nullptr));
    CHECK_EQ(sampler.residentFileBytes(), uint64_t(bytes->size()));
    CHECK_EQ(sampler.decodedBlockBytes(), uint64_t(0));  // nothing decoded yet

    const int64_t px = int64_t(whole->tileX()) * whole->size();
    const int64_t py = int64_t(whole->tileY()) * whole->size();
    int16_t cp = 0;
    CHECK(sampler.controlPointAt(px, py, cp));
    // One decoded block: file bytes unchanged, decoded bytes now nonzero.
    CHECK_EQ(sampler.residentFileBytes(), uint64_t(bytes->size()));
    CHECK_EQ(sampler.decodedBlockBytes(),
             uint64_t(whole->blockPixelCount()) * sizeof(int16_t));
    CHECK(sampler.blockDecoded(px, py));
}

VXC_TEST(file_range_source_reads_the_same_bytes_as_a_whole_file_read) {
    // The transport the shipping client actually has. A file supports ranges
    // natively, so this needs no server and no protocol -- it is the same path
    // VoxelFineTileStreamer::LocalPathFor builds, seeked instead of slurped.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    FileRangeSource src(goldenPath());
    CHECK(src.ok());
    CHECK_EQ(src.fileSize(), uint64_t(bytes->size()));

    std::vector<uint8_t> got;
    CHECK(src.read(100, 64, got));
    CHECK_EQ(got.size(), size_t(64));
    CHECK(std::equal(got.begin(), got.end(), bytes->begin() + 100));
    CHECK_EQ(src.requests, uint64_t(1));
    CHECK_EQ(src.bytesRead, uint64_t(64));

    // A range past the end is a planning bug, refused rather than short-read.
    std::vector<uint8_t> over;
    CHECK(!src.read(bytes->size() - 4, 16, over));
    // ...except for the probe, which asks past the end on purpose and clamps.
    std::vector<uint8_t> probe;
    CHECK(src.readClamped(bytes->size() - 4, 16, probe));
    CHECK_EQ(probe.size(), size_t(4));
}

VXC_TEST(truncated_file_is_refused_rather_than_silently_short) {
    // The failure a whole-file read catches for free and a ranged read would
    // not: every individual range succeeds and the missing tail is simply never
    // asked for. The section table's own file length is compared against the
    // transport's, so a truncation is caught before a single block is planned.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;
    std::vector<uint8_t> truncated(bytes->begin(), bytes->end() - 1024);
    BytesRangeSource src(truncated);
    FineTileBytes held;
    FineError err = FineError::kNone;
    CHECK(!readFineTilePreamble(src, src.fileSize(), FinePreambleRequest{}, held, &err));
    CHECK_EQ(int(err), int(FineError::kBadSectionTable));
}

VXC_TEST(preamble_separates_a_newer_format_from_a_broken_file) {
    // readFineSectionTable answers one bool for three situations, and the
    // preamble used to flatten all three into kNotVxtl -- so a client reading a
    // tile written by a NEWER format was told its file was not a .vxtl. That is
    // the same defect as "bad-header" for an unknown flag bit, one layer down:
    // a reason code that cannot distinguish an old reader from a broken file.
    const auto bytes = loadFixture(goldenPath());
    if (!bytes) return;

    // A version this build does not read. Everything else about the file is
    // untouched and perfect.
    {
        std::vector<uint8_t> future(*bytes);
        const uint16_t v3 = 3;
        std::memcpy(future.data() + 4, &v3, 2);
        BytesRangeSource src(future);
        FineTileBytes held;
        FineError err = FineError::kNone;
        FineHeaderFacts facts;
        CHECK(!readFineTilePreamble(src, src.fileSize(), FinePreambleRequest{}, held, &err, &facts));
        CHECK_EQ(int(err), int(FineError::kWrongVersion));
        // The facts come back even though the read failed -- that is what the
        // out-param is for, and the message needs them.
        CHECK(facts.magicOk);
        CHECK_EQ(int(facts.formatVersion), 3);
        CHECK(!facts.v2Fields); // v2 field offsets do not apply to a v3 file
    }

    // Not one of ours at all: still kNotVxtl, and no facts to report.
    {
        std::vector<uint8_t> junk(*bytes);
        junk[0] = 'X';
        BytesRangeSource src(junk);
        FineTileBytes held;
        FineError err = FineError::kNone;
        FineHeaderFacts facts;
        CHECK(!readFineTilePreamble(src, src.fileSize(), FinePreambleRequest{}, held, &err, &facts));
        CHECK_EQ(int(err), int(FineError::kNotVxtl));
        CHECK(!facts.magicOk);
    }

    // And a good file still reports its facts on SUCCESS, so a caller can log
    // what it loaded without re-reading the header.
    {
        BytesRangeSource src(*bytes);
        FineTileBytes held;
        FineHeaderFacts facts;
        CHECK(readFineTilePreamble(src, src.fileSize(), FinePreambleRequest{}, held, nullptr, &facts));
        CHECK(facts.magicOk);
        CHECK(facts.v2Fields);
        CHECK_EQ(int(facts.formatVersion), int(kFineFormatVersion));
        CHECK_EQ(int(facts.unknownFlagBits()), 0);
    }
}

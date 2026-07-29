// Tests for voxelcore/tilestreaming.h -- the fine-tier residency/prefetch/
// eviction POLICY layer (docs/terrain-amplification-plan.md Phase 2 client
// side). Pure logic, no UE, no file I/O beyond what FineTile::parse itself
// does on an in-memory byte buffer we build here.
//
// The block-coverage-with-dilation maths gets the most scrutiny per the
// task brief: an off-by-one there does not fault, it silently returns edge
// values -- i.e. different terrain on two clients -- so it is tested
// against hand-computed expectations, including negative tile coordinates
// and rects that straddle a tile boundary.

#include "voxelcore/tilestreaming.h"

#include "voxelcore/bytes.h"
#include "vxctest.h"

using namespace vxc;

namespace {

// ---------------------------------------------------------------------------
// Minimal, deliberately separate .vxtl v2 encoder (CONSTANT blocks only) for
// validateAndParseFineTile's tests. Mirrors test_tilestore.cpp's buildFineTile
// (same file-scope spec, docs/vxtl-v2-format.md §3-4) but trimmed to the one
// shape these tests need: no CODED/RAW modes, no flow plane -- that ground is
// already covered by test_tilestore.cpp, and duplicating it here would test
// nothing new.
// ---------------------------------------------------------------------------

std::vector<uint8_t> buildConstantFineTile(uint64_t seed, int32_t x, int32_t y, uint16_t size,
                                           uint8_t blockLog2, int16_t constCp) {
    const uint32_t blocksPerAxis = uint32_t(size) >> blockLog2;
    const uint32_t blockCount = blocksPerAxis * blocksPerAxis;

    std::vector<uint8_t> elevIndex;
    {
        ByteWriter iw(elevIndex);
        for (uint32_t i = 0; i < blockCount; ++i) {
            iw.u64(0);              // offset (unused, CONSTANT has no data bytes)
            iw.u32(0);              // comp_len
            iw.u8(0);               // mode = CONSTANT
            iw.u16(uint16_t(constCp));
            iw.u8(0);               // resid_bits, meaningless for CONSTANT
            for (int p = 0; p < 4; ++p) iw.u8(0); // pad
        }
    }
    const std::vector<uint8_t> elevData; // CONSTANT blocks carry no data bytes

    const uint16_t nSections = 2;
    const uint64_t tableEnd = 43 + uint64_t(nSections) * 20;
    const uint64_t elevIndexOff = tableEnd;
    const uint64_t elevDataOff = elevIndexOff + elevIndex.size();

    std::vector<uint8_t> out;
    ByteWriter w(out);
    w.u8('V'); w.u8('X'); w.u8('T'); w.u8('L');
    w.u16(2);              // version
    w.u64(seed);
    w.i32(x);
    w.i32(y);
    w.u8(16);              // scale
    w.u16(size);
    w.u8(blockLog2);
    w.u8(1);               // predictor = PRED_MED
    w.u8(1);               // quant = 100mm/LSB
    w.u8(0);               // codec = RAW
    w.u16(42);             // bake_ver
    w.u16(0);              // flags: no flow plane
    w.i32(0);              // base_offset_mm
    w.u8(0);               // parent_scale
    w.u8(0); w.u8(0); w.u8(0); // reserved
    w.u16(nSections);
    w.u32(1); w.u64(elevIndexOff); w.u64(elevIndex.size());
    w.u32(2); w.u64(elevDataOff); w.u64(elevData.size());
    out.insert(out.end(), elevIndex.begin(), elevIndex.end());
    out.insert(out.end(), elevData.begin(), elevData.end());
    return out;
}

} // namespace

// --- dilateForCarrierStencil ------------------------------------------------

VXC_TEST(dilate_single_column_matches_stencil_constants) {
    const PixelRect footprint{10, 20, 10, 20};
    const PixelRect dilated = dilateForCarrierStencil(footprint);
    CHECK_EQ(dilated.px0, 10 + kCarrierStencilLo);
    CHECK_EQ(dilated.py0, 20 + kCarrierStencilLo);
    CHECK_EQ(dilated.px1, 10 + kCarrierStencilHi);
    CHECK_EQ(dilated.py1, 20 + kCarrierStencilHi);
    // Pin the literal numbers too: a silent change to kCarrierStencilLo/Hi
    // in tiles.h should fail THIS test loudly, not just look different.
    CHECK_EQ(dilated.px0, 9);
    CHECK_EQ(dilated.px1, 12);
}

VXC_TEST(dilate_negative_footprint) {
    const PixelRect footprint{-5, -5, -5, -5};
    const PixelRect dilated = dilateForCarrierStencil(footprint);
    CHECK_EQ(dilated.px0, -6);
    CHECK_EQ(dilated.px1, -3);
}

// --- fineReadPixelRect ------------------------------------------------------
//
// The residency gate's one conversion. Two things it must get right, and both
// are silent failures rather than faults if it does not: (a) the world rect is
// HALF-OPEN, so a rect that ends exactly on a pixel boundary must not claim the
// next pixel; (b) the read margin is applied in WORLD MILLIMETRES before the
// pixel division, because that is where the cavern layer's reach is defined and
// dividing first would round it away at this pitch.

VXC_TEST(fine_read_rect_zero_margin_is_dilate_of_the_closed_cell_rect) {
    constexpr int32_t px = 1875;
    // [0, 1875) mm is exactly pixel 0 on both axes; the half-open end must not
    // pull in pixel 1.
    const PixelRect r = fineReadPixelRect(0, 0, px, px, /*readMarginMm=*/0, px);
    CHECK_EQ(r.px0, 0 + kCarrierStencilLo);
    CHECK_EQ(r.px1, 0 + kCarrierStencilHi);
    CHECK_EQ(r.py0, 0 + kCarrierStencilLo);
    CHECK_EQ(r.py1, 0 + kCarrierStencilHi);
}

VXC_TEST(fine_read_rect_spans_two_pixels_when_it_really_does) {
    constexpr int32_t px = 1875;
    const PixelRect r = fineReadPixelRect(0, 0, px + 1, px + 1, 0, px);
    CHECK_EQ(r.px0, 0 + kCarrierStencilLo);
    CHECK_EQ(r.px1, 1 + kCarrierStencilHi);
}

VXC_TEST(fine_read_rect_margin_is_applied_in_world_mm) {
    constexpr int32_t px = 1875;
    // A 36.4 m margin is 19.4 pixels at this pitch: it must widen the rect by
    // 19 pixels on the low side (floorDiv(-36494, 1875) == -20, +1 for the
    // pixel the rect already had) rather than vanish.
    const PixelRect r = fineReadPixelRect(0, 0, px, px, 36494, px);
    CHECK_EQ(r.px0, floorDiv(int64_t(-36494), int64_t(px)) + kCarrierStencilLo);
    CHECK_EQ(r.px1, floorDiv(int64_t(px - 1 + 36494), int64_t(px)) + kCarrierStencilHi);
    CHECK(r.px0 <= -20 + kCarrierStencilLo);
    CHECK(r.px1 >= 19 + kCarrierStencilHi);
}

VXC_TEST(fine_read_rect_negative_world_coordinates_do_not_alias_across_zero) {
    constexpr int32_t px = 1875;
    // The pixel containing world mm -1 is pixel -1, not pixel 0. Truncating
    // division here would fold the whole first negative pixel onto 0 and mirror
    // terrain across the origin -- the exact aliasing floorDiv exists to avoid.
    const PixelRect r = fineReadPixelRect(-px, -px, 0, 0, 0, px);
    CHECK_EQ(r.px0, -1 + kCarrierStencilLo);
    CHECK_EQ(r.px1, -1 + kCarrierStencilHi);
}

VXC_TEST(fine_read_rect_rejects_a_nonsense_pitch) {
    const PixelRect r = fineReadPixelRect(0, 0, 100, 100, 0, 0);
    CHECK(r.px1 < r.px0); // inverted => covers nothing, rather than dividing by zero
}

// --- blocksCoveringRect ------------------------------------------------

VXC_TEST(blocks_single_pixel_interior) {
    // tileSizePx=512, blockDimPx=256 -> 2x2 blocks per tile. A rect entirely
    // inside the first block must yield exactly that one block.
    const auto blocks = blocksCoveringRect(PixelRect{10, 10, 10, 10}, 512, 256);
    CHECK_EQ(blocks.size(), size_t(1));
    CHECK(blocks[0].tile == (TileCoord{0, 0}));
    CHECK_EQ(blocks[0].blockX, 0u);
    CHECK_EQ(blocks[0].blockY, 0u);
}

VXC_TEST(blocks_straddling_block_boundary_same_tile) {
    // 250..260 straddles the 256 boundary within tile 0 -> blocks (0,*) and (1,*).
    const auto blocks = blocksCoveringRect(PixelRect{250, 100, 260, 100}, 512, 256);
    CHECK_EQ(blocks.size(), size_t(2));
    bool sawBx0 = false, sawBx1 = false;
    for (const BlockCoord& b : blocks) {
        CHECK(b.tile == (TileCoord{0, 0}));
        CHECK_EQ(b.blockY, 0u);
        if (b.blockX == 0) sawBx0 = true;
        if (b.blockX == 1) sawBx1 = true;
    }
    CHECK(sawBx0);
    CHECK(sawBx1);
}

VXC_TEST(blocks_straddling_tile_boundary_at_origin) {
    // px in [-5, 5] straddles the tile -1 / tile 0 boundary at px=0.
    // floorDiv(-5,512) == -1, floorDiv(5,512) == 0.
    // Tile -1 spans world px [-512, -1]; local coord of -5 there is 507
    // (block 507/256 == 1). Tile 0 spans [0,511]; local coord of 5 there is
    // 5 (block 0).
    const auto blocks = blocksCoveringRect(PixelRect{-5, 100, 5, 100}, 512, 256);
    CHECK_EQ(blocks.size(), size_t(2));
    bool sawTileMinus1Block1 = false, sawTile0Block0 = false;
    for (const BlockCoord& b : blocks) {
        if (b.tile == TileCoord{-1, 0}) {
            CHECK_EQ(b.blockX, 1u);
            sawTileMinus1Block1 = true;
        } else if (b.tile == TileCoord{0, 0}) {
            CHECK_EQ(b.blockX, 0u);
            sawTile0Block0 = true;
        } else {
            CHECK(false); // unexpected tile
        }
    }
    CHECK(sawTileMinus1Block1);
    CHECK(sawTile0Block0);
}

VXC_TEST(blocks_deep_negative_tile_coordinates) {
    // A single pixel deep inside tile (-2, -3) (verified by hand against
    // vxc::floorDiv, not just the code under test):
    //   floorDiv(-600, 512) == -2 (tile x); local X = -600 - (-2*512) = 424;
    //     block 424/256 == 1.
    //   floorDiv(-1200, 512) == -3 (tile y); local Y = -1200 - (-3*512) = 336;
    //     block 336/256 == 1.
    const auto blocks = blocksCoveringRect(PixelRect{-600, -1200, -600, -1200}, 512, 256);
    CHECK_EQ(blocks.size(), size_t(1));
    CHECK(blocks[0].tile == (TileCoord{-2, -3}));
    CHECK_EQ(blocks[0].blockX, 1u);
    CHECK_EQ(blocks[0].blockY, 1u);
}

VXC_TEST(blocks_inverted_rect_is_empty) {
    CHECK_EQ(blocksCoveringRect(PixelRect{10, 10, 5, 10}, 512, 256).size(), size_t(0));
    CHECK_EQ(blocksCoveringRect(PixelRect{10, 10, 10, 5}, 512, 256).size(), size_t(0));
}

VXC_TEST(blocks_degenerate_params_are_empty) {
    CHECK_EQ(blocksCoveringRect(PixelRect{0, 0, 0, 0}, 0, 256).size(), size_t(0));
    CHECK_EQ(blocksCoveringRect(PixelRect{0, 0, 0, 0}, 512, 0).size(), size_t(0));
}

VXC_TEST(dilated_block_coverage_composes_dilation_then_coverage) {
    // A single-column footprint at a block boundary (px=256): dilated range
    // is [255, 258] (Lo=-1, Hi=2), which straddles blocks 0 and 1.
    const auto blocks = dilatedBlockCoverage(PixelRect{256, 10, 256, 10}, 512, 256);
    CHECK_EQ(blocks.size(), size_t(2));
    bool saw0 = false, saw1 = false;
    for (const BlockCoord& b : blocks) {
        CHECK(b.tile == (TileCoord{0, 0}));
        if (b.blockX == 0) saw0 = true;
        if (b.blockX == 1) saw1 = true;
    }
    CHECK(saw0);
    CHECK(saw1);
}

// --- tileCoordForWorldMm -----------------------------------------------

VXC_TEST(tile_coord_for_world_mm_boundaries) {
    constexpr int64_t kFootprintMm = 15'360'000; // 15.36 km, one s1/fine tile
    CHECK(tileCoordForWorldMm(0, 0, kFootprintMm) == (TileCoord{0, 0}));
    CHECK(tileCoordForWorldMm(kFootprintMm - 1, 0, kFootprintMm) == (TileCoord{0, 0}));
    CHECK(tileCoordForWorldMm(kFootprintMm, 0, kFootprintMm) == (TileCoord{1, 0}));
    CHECK(tileCoordForWorldMm(-1, 0, kFootprintMm) == (TileCoord{-1, 0}));
    CHECK(tileCoordForWorldMm(-kFootprintMm, 0, kFootprintMm) == (TileCoord{-1, 0}));
    CHECK(tileCoordForWorldMm(-kFootprintMm - 1, 0, kFootprintMm) == (TileCoord{-2, 0}));
}

VXC_TEST(tile_coord_for_world_mm_degenerate_footprint) {
    CHECK(tileCoordForWorldMm(123, 456, 0) == (TileCoord{0, 0}));
    CHECK(tileCoordForWorldMm(123, 456, -1) == (TileCoord{0, 0}));
}

// --- squareTileRing ------------------------------------------------------

VXC_TEST(square_ring_radius_zero_is_just_center) {
    const auto ring = squareTileRing(TileCoord{-5, -5}, 0);
    CHECK_EQ(ring.size(), size_t(1));
    CHECK(ring[0] == (TileCoord{-5, -5}));
}

VXC_TEST(square_ring_radius_one_is_nine_tiles) {
    const auto ring = squareTileRing(TileCoord{0, 0}, 1);
    CHECK_EQ(ring.size(), size_t(9));
    // Every (dx,dy) in [-1,1]^2 must be present exactly once.
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            int count = 0;
            for (const TileCoord& t : ring) {
                if (t == TileCoord{dx, dy}) ++count;
            }
            CHECK_EQ(count, 1);
        }
    }
}

VXC_TEST(square_ring_negative_radius_is_empty) {
    CHECK_EQ(squareTileRing(TileCoord{0, 0}, -1).size(), size_t(0));
}

VXC_TEST(square_ring_coarse_radius_two_is_twentyfive_tiles) {
    // The plan's coarse ring radius.
    CHECK_EQ(squareTileRing(TileCoord{-6, 3}, 2).size(), size_t(25));
}

// --- formatFineTileCacheKey ----------------------------------------------

VXC_TEST(cache_key_matches_cache_py_layout) {
    const std::string key = formatFineTileCacheKey("terrain-diffusion-unlabeled-3e11cf157a836c70", 1, -6, 3);
    CHECK(key == std::string("terrain-diffusion-unlabeled-3e11cf157a836c70/0000000000000001/s16/-6_3"));
}

VXC_TEST(cache_key_honors_explicit_scale) {
    const std::string key = formatFineTileCacheKey("p", 0, 0, 0, 1);
    CHECK(key == std::string("p/0000000000000000/s1/0_0"));
}

// --- validateAndParseFineTile --------------------------------------------

VXC_TEST(validate_rejects_truncated_bytes) {
    std::vector<uint8_t> bytes = buildConstantFineTile(42, -6, 3, 512, 8, 7);
    bytes.resize(bytes.size() / 2); // truncate mid-file
    const FineTileValidationResult r = validateAndParseFineTile(std::move(bytes), 42, -6, 3);
    CHECK(r.verdict == FineTileVerdict::kCorrupt);
    CHECK(!r.tile.has_value());
}

VXC_TEST(validate_rejects_seed_mismatch) {
    std::vector<uint8_t> bytes = buildConstantFineTile(42, -6, 3, 512, 8, 7);
    const FineTileValidationResult r = validateAndParseFineTile(std::move(bytes), 99 /* wrong seed */, -6, 3);
    CHECK(r.verdict == FineTileVerdict::kIdentityMismatch);
    CHECK(!r.tile.has_value());
}

VXC_TEST(validate_rejects_coord_mismatch) {
    std::vector<uint8_t> bytesX = buildConstantFineTile(42, -6, 3, 512, 8, 7);
    CHECK(validateAndParseFineTile(std::move(bytesX), 42, -7 /* wrong x */, 3).verdict ==
          FineTileVerdict::kIdentityMismatch);

    std::vector<uint8_t> bytesY = buildConstantFineTile(42, -6, 3, 512, 8, 7);
    CHECK(validateAndParseFineTile(std::move(bytesY), 42, -6, 4 /* wrong y */).verdict ==
          FineTileVerdict::kIdentityMismatch);
}

VXC_TEST(validate_accepts_matching_tile) {
    std::vector<uint8_t> bytes = buildConstantFineTile(42, -6, 3, 512, 8, 7);
    FineTileValidationResult r = validateAndParseFineTile(std::move(bytes), 42, -6, 3);
    CHECK(r.verdict == FineTileVerdict::kOk);
    CHECK(r.tile.has_value());
    if (r.tile) {
        CHECK_EQ(r.tile->tileX(), -6);
        CHECK_EQ(r.tile->tileY(), 3);
        CHECK_EQ(r.tile->size(), uint32_t(512));
        CHECK_EQ(r.tile->blocksPerAxis(), uint32_t(2));
    }
}

// --- LruBudgetCache --------------------------------------------------------

VXC_TEST(lru_evicts_least_recently_used_first) {
    LruBudgetCache cache(100);
    cache.touch("a", 40);
    cache.touch("b", 40);
    cache.touch("c", 40); // 120 resident, 20 over budget
    const auto evict = cache.selectEvictions();
    // "a" is the oldest untouched-since entry; evicting it alone (40 bytes)
    // brings residency to 80, under the 100 budget, so it should be the only
    // one selected.
    CHECK_EQ(evict.size(), size_t(1));
    CHECK(evict[0] == std::string("a"));
}

VXC_TEST(lru_retouch_moves_entry_to_mru) {
    LruBudgetCache cache(100);
    cache.touch("a", 40);
    cache.touch("b", 40);
    cache.touch("a", 40); // re-touch: "a" is now MRU, "b" is now LRU
    cache.touch("c", 40); // 120 resident
    const auto evict = cache.selectEvictions();
    CHECK_EQ(evict.size(), size_t(1));
    CHECK(evict[0] == std::string("b"));
}

VXC_TEST(lru_pin_excludes_from_eviction_even_over_budget) {
    LruBudgetCache cache(10);
    cache.touch("a", 40);
    cache.touch("b", 40);
    cache.setPinned({"a", "b"});
    // Both entries are pinned, so nothing may be evicted even though
    // residency (80) is far over budget (10) -- pinning the active ring is a
    // harder constraint than the byte budget.
    CHECK_EQ(cache.selectEvictions().size(), size_t(0));
}

VXC_TEST(lru_pin_partial_still_evicts_unpinned_only) {
    LruBudgetCache cache(50);
    cache.touch("a", 40);
    cache.touch("b", 40);
    cache.setPinned({"a"});
    const auto evict = cache.selectEvictions();
    CHECK_EQ(evict.size(), size_t(1));
    CHECK(evict[0] == std::string("b"));
}

VXC_TEST(lru_under_budget_evicts_nothing) {
    LruBudgetCache cache(1000);
    cache.touch("a", 40);
    cache.touch("b", 40);
    CHECK_EQ(cache.selectEvictions().size(), size_t(0));
}

VXC_TEST(lru_remove_updates_resident_bytes) {
    LruBudgetCache cache(1000);
    cache.touch("a", 40);
    cache.touch("b", 60);
    CHECK_EQ(cache.residentBytes(), uint64_t(100));
    cache.remove("a");
    CHECK_EQ(cache.residentBytes(), uint64_t(60));
    CHECK(!cache.contains("a"));
    cache.remove("nonexistent"); // no-op, must not underflow
    CHECK_EQ(cache.residentBytes(), uint64_t(60));
}

VXC_TEST(lru_retouch_with_different_size_corrects_accounting) {
    LruBudgetCache cache(1000);
    cache.touch("a", 40);
    cache.touch("a", 25); // same key, different byte count
    CHECK_EQ(cache.residentBytes(), uint64_t(25));
    CHECK_EQ(cache.size(), size_t(1));
}

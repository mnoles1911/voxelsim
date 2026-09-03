// Craft-lattice persistence: edit-log format v3, the second stream, and the
// replay invariant.
//
// The invariant under test is the one the terrain log has always had, extended
// to two streams:
//
//     world + terrainLog + craftLog  ==  world + terrainLog + craftLog
//
// ...and, critically, that it is ORDERED. Promotion reads the terrain brick as
// it stands, so replaying craft before terrain promotes from ungraded generated
// terrain and every craft cell afterwards sits on the wrong base. A test that
// only ever replays in the right order would never notice.

#include <vector>

#include "voxelcore/craftlattice.h"
#include "voxelcore/editcompact.h"
#include "voxelcore/world.h"
#include "vxctest.h"

using namespace vxc;

namespace {

constexpr uint64_t kSeed = 918273;
constexpr int kB = kMarchBrickEdge;

int64_t surfaceVoxelZ(const World<kB>& w) {
    const ColumnSample col = w.amplifier().column(0, 0);
    return topSolidVoxelZ(col.surfaceMm);
}

// A short, deliberately messy carve session on ground the amplifier really
// generates: two neighbouring terrain bricks, a notch that does not reach the
// projection's threshold, a fully hollowed voxel that does, and an overwrite
// then revert on one cell so normalisation has work to do.
void carveSession(World<kB>& w, int64_t topVz) {
    const int64_t cx0 = craftCellOfVoxelMin(0);
    const int64_t cy0 = craftCellOfVoxelMin(0);
    const int64_t cz0 = craftCellOfVoxelMin(topVz);

    // A sub-voxel notch: one cell of 64.
    w.setCraftCell(cx0, cy0, cz0, MAT_AIR);

    // A whole voxel hollowed, one cell at a time.
    for (int64_t dz = 0; dz < kCraftCellsPerVoxel; ++dz)
        for (int64_t dy = 0; dy < kCraftCellsPerVoxel; ++dy)
            for (int64_t dx = 0; dx < kCraftCellsPerVoxel; ++dx)
                w.setCraftCell(cx0 + 4 + dx, cy0 + dy, cz0 + dz, MAT_AIR);

    // Overwrite then revert -- the net effect must be nothing.
    w.setCraftCell(cx0 + 1, cy0, cz0, MAT_SAND);
    w.setCraftCell(cx0 + 1, cy0, cz0, MAT_ROCK);

    // A second terrain brick, so the session is not single-brick.
    w.setCraftCell(craftCellOfVoxelMin(9), craftCellOfVoxelMin(1), cz0, MAT_AIR);
}

} // namespace

// ---------------------------------------------------------------------------
// Format v3
// ---------------------------------------------------------------------------

VXC_TEST(editlog_v3_round_trips_the_lattice_pitch) {
    EditLog craft(kSeed, kB, "prov-a", static_cast<uint32_t>(kCraftPitchMm));
    craft.append(BrickKey{1, -2, 3}, {{7, MAT_ROCK}, {9, MAT_AIR}});
    std::vector<uint8_t> bytes;
    craft.serialize(bytes);

    const auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->latticePitchMm() == static_cast<uint32_t>(kCraftPitchMm));
    CHECK(parsed->providerId() == "prov-a");
    CHECK(parsed->entries() == craft.entries());
}

VXC_TEST(a_terrain_log_is_still_stamped_v2_so_older_builds_can_read_it) {
    // THE RULE: write the LOWEST version that can carry the content.
    //
    // A terrain log's pitch is exactly what a v2 reader already assumes, so
    // stamping it v3 would make every pre-craft build REFUSE a save it could
    // have read perfectly -- parse() rejects fmt > kFormatVersion. The world
    // would be fine, the reader would be fine, and the version number alone
    // would break them apart. That is a silent data-loss shape, and it is the
    // risk the marcher lane carried while a craft-linked lib sat on disk.
    EditLog terrain(kSeed, kB, "prov");
    terrain.append(BrickKey{1, 2, 3}, {{5, MAT_ROCK}});
    CHECK(terrain.formatVersionForContent() == 2u);

    std::vector<uint8_t> bytes;
    terrain.serialize(bytes);
    // Byte 4 is the format word, immediately after the magic.
    CHECK(bytes.size() > 8);
    const uint32_t stamped = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) |
                             (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
    CHECK(stamped == 2u);

    // And it still round-trips through THIS build with the right pitch.
    const auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->latticePitchMm() == static_cast<uint32_t>(kVoxelSizeMm));
    CHECK(parsed->providerId() == "prov");
    CHECK(parsed->entries() == terrain.entries());

    // peekHeader agrees there is no pitch field to find -- version and field
    // presence move together, and reading one without the other would shift
    // every byte after it.
    const EditLog::HeaderPeek h = EditLog::peekHeader(bytes.data(), bytes.size());
    CHECK(h.haveFormat && h.format == 2u);
    CHECK(!h.haveLatticePitch);
}

VXC_TEST(a_craft_log_is_stamped_v3_because_it_actually_needs_the_field) {
    EditLog craft(kSeed, kB, "prov", static_cast<uint32_t>(kCraftPitchMm));
    craft.append(BrickKey{1, 2, 3}, {{5, MAT_ROCK}});
    CHECK(craft.formatVersionForContent() == 3u);

    std::vector<uint8_t> bytes;
    craft.serialize(bytes);
    const uint32_t stamped = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) |
                             (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
    CHECK(stamped == 3u);
    CHECK(stamped == EditLog::kFormatVersion);

    const auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->latticePitchMm() == static_cast<uint32_t>(kCraftPitchMm));
}

VXC_TEST(a_world_with_no_chiselling_writes_a_terrain_log_an_older_build_accepts) {
    // The end-to-end statement of the rule, at the level the risk was
    // described: play without touching a chisel, autosave, and the file must
    // still be one a pre-craft build opens.
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    w.setVoxel(0, 0, topVz, MAT_AIR);
    w.setVoxel(1, 0, topVz, MAT_AIR);
    CHECK(w.craftLattice().promotedCount() == 0);

    std::vector<uint8_t> bytes;
    w.log().serialize(bytes);
    const uint32_t stamped = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) |
                             (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
    CHECK(stamped == 2u);
}

VXC_TEST(editlog_default_construction_is_still_the_terrain_lattice) {
    // Every pre-existing caller passes no pitch and must keep meaning terrain.
    EditLog terrain(kSeed, kB);
    CHECK(terrain.latticePitchMm() == static_cast<uint32_t>(kVoxelSizeMm));
    std::vector<uint8_t> bytes;
    terrain.serialize(bytes);
    const auto parsed = EditLog::parse(bytes.data(), bytes.size());
    CHECK(parsed.has_value());
    CHECK(parsed->latticePitchMm() == static_cast<uint32_t>(kVoxelSizeMm));
}

VXC_TEST(peekHeader_reports_the_pitch_and_stays_total_on_a_truncated_file) {
    EditLog craft(kSeed, kB, "p", static_cast<uint32_t>(kCraftPitchMm));
    std::vector<uint8_t> bytes;
    craft.serialize(bytes);

    const EditLog::HeaderPeek full = EditLog::peekHeader(bytes.data(), bytes.size());
    CHECK(full.haveMagic && full.haveFormat && full.haveBrickEdge);
    CHECK(full.format == EditLog::kFormatVersion);
    CHECK(full.haveLatticePitch);
    CHECK(full.latticePitchMm == static_cast<uint32_t>(kCraftPitchMm));

    // Truncated at every length: never a crash, never a false claim.
    for (size_t n = 0; n <= bytes.size(); ++n) {
        const EditLog::HeaderPeek h = EditLog::peekHeader(bytes.data(), n);
        if (h.haveLatticePitch) CHECK(h.latticePitchMm == static_cast<uint32_t>(kCraftPitchMm));
    }
    CHECK(!EditLog::peekHeader(nullptr, 0).haveMagic);
}

VXC_TEST(compaction_preserves_the_lattice_pitch_and_the_provider) {
    // A compacted craft log that came back labelled "terrain" would replay 25 mm
    // diffs onto the 10 cm lattice and read as world corruption, not as a
    // mislabelled file.
    EditLog craft(kSeed, kB, "prov-b", static_cast<uint32_t>(kCraftPitchMm));
    craft.append(BrickKey{0, 0, 0}, {{1, MAT_ROCK}});
    craft.append(BrickKey{0, 0, 0}, {{1, MAT_AIR}, {2, MAT_SAND}});
    const EditLog packed = compactLog(craft);
    CHECK(packed.latticePitchMm() == static_cast<uint32_t>(kCraftPitchMm));
    CHECK(packed.providerId() == "prov-b");
    CHECK(packed.size() == 1);
}

// ---------------------------------------------------------------------------
// The streams do not accept each other
// ---------------------------------------------------------------------------

VXC_TEST(a_craft_log_is_refused_by_the_terrain_replay_and_vice_versa) {
    // brickEdge is 8 on BOTH lattices, so this refusal rests entirely on the
    // pitch field. Without it each log would replay into the other happily.
    SyntheticTileSampler tiles(kSeed);
    World<kB> src(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(src);
    src.setVoxel(0, 0, topVz, MAT_AIR);
    src.setCraftCell(craftCellOfVoxelMin(3), craftCellOfVoxelMin(3),
                     craftCellOfVoxelMin(topVz), MAT_AIR);
    CHECK(src.log().size() == 1);
    CHECK(src.craftLog().size() == 1);

    SyntheticTileSampler tiles2(kSeed);
    World<kB> dst(kSeed, tiles2);
    CHECK(!dst.replay(src.craftLog()));      // craft bytes into the terrain stream
    CHECK(!dst.replayCraft(src.log()));      // terrain bytes into the craft stream
    // ...and neither refusal left anything behind.
    CHECK(dst.log().size() == 0);
    CHECK(dst.craftLog().size() == 0);
    CHECK(dst.craftLattice().promotedCount() == 0);
}

// ---------------------------------------------------------------------------
// The replay invariant
// ---------------------------------------------------------------------------

VXC_TEST(two_stream_replay_reproduces_the_world_exactly) {
    SyntheticTileSampler tiles(kSeed);
    World<kB> src(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(src);
    // Terrain edits first, so the craft bricks promote from EDITED terrain and
    // the ordering rule is actually exercised.
    src.setVoxel(1, 1, topVz, MAT_SAND);
    src.setVoxel(2, 1, topVz, MAT_AIR);
    carveSession(src, topVz);
    CHECK(src.craftLattice().promotedCount() >= 2);

    // Serialize both streams, as a save would.
    std::vector<uint8_t> terrainBytes, craftBytes;
    src.log().serialize(terrainBytes);
    src.craftLog().serialize(craftBytes);
    const auto terrainLog = EditLog::parse(terrainBytes.data(), terrainBytes.size());
    const auto craftLog = EditLog::parse(craftBytes.data(), craftBytes.size());
    CHECK(terrainLog.has_value());
    CHECK(craftLog.has_value());

    SyntheticTileSampler tiles2(kSeed);
    World<kB> dst(kSeed, tiles2);
    CHECK(dst.replay(*terrainLog));
    CHECK(dst.replayCraft(*craftLog));

    // Both digests, because they answer different questions: editedDigest
    // covers the 10 cm world INCLUDING the written projections, craftDigest
    // covers the 25 mm cells the projection deliberately cannot see.
    CHECK(dst.editedDigest() == src.editedDigest());
    CHECK(dst.craftDigest() == src.craftDigest());
    CHECK(dst.craftLattice().promotedCount() == src.craftLattice().promotedCount());
}

VXC_TEST(replaying_craft_BEFORE_terrain_loses_the_carve) {
    // The ordering rule, shown rather than asserted.
    //
    // The case has to be chosen with care. Most orderings CONVERGE, because a
    // terrain edit into a promoted brick is routed into the craft stream and
    // ends up writing the same cells either way -- an earlier version of this
    // test used two different voxels, watched both orders agree, and would
    // have gone on passing if replay order had stopped mattering entirely.
    //
    // Order decides only when the two streams touch the SAME voxel: the dig
    // rewrites all 64 of that voxel's craft cells, so whichever stream lands
    // last erases the other.
    //
    // And note WHICH digest sees it. The projection of "solid with one 25 mm
    // notch" is the same solid voxel, so editedDigest is identical in both
    // orders. Only craftDigest can tell them apart -- which is exactly why the
    // handshake has to fold it.
    SyntheticTileSampler tiles(kSeed);
    World<kB> src(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(src);
    // A terrain edit, then a chisel into THE SAME voxel.
    src.setVoxel(0, 0, topVz, MAT_SAND);
    src.setCraftCell(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0),
                     craftCellOfVoxelMin(topVz), MAT_AIR);

    std::vector<uint8_t> tb, cb;
    src.log().serialize(tb);
    src.craftLog().serialize(cb);
    const auto tlog = EditLog::parse(tb.data(), tb.size());
    const auto clog = EditLog::parse(cb.data(), cb.size());
    CHECK(tlog.has_value() && clog.has_value());

    SyntheticTileSampler t1(kSeed);
    World<kB> right(kSeed, t1);
    CHECK(right.replay(*tlog));
    CHECK(right.replayCraft(*clog));
    CHECK(right.craftDigest() == src.craftDigest());

    // The wrong order is now REFUSED rather than quietly producing a world
    // where overlay != project(craft). Writing this test is what found that:
    // replay() applies entries straight to the overlay and does not route them
    // the way applyEdit does, so before the guard existed the craft-first order
    // silently left the dig on the overlay and the chisel in the craft lattice,
    // disagreeing about the same voxel.
    SyntheticTileSampler t2(kSeed);
    World<kB> wrong(kSeed, t2);
    CHECK(wrong.replayCraft(*clog));
    CHECK(!wrong.replay(*tlog));

    // The refusal is clean: nothing of the terrain log was applied.
    CHECK(wrong.log().size() == 0);
    CHECK(wrong.craftDigest() != right.craftDigest());

    // And in the right order the notch is there.
    const int64_t cx = craftCellOfVoxelMin(0), cy = craftCellOfVoxelMin(0),
                  cz = craftCellOfVoxelMin(topVz);
    CHECK(right.craftMaterialAt(cx, cy, cz) == MAT_AIR);
    CHECK(right.craftMaterialAt(cx + 1, cy, cz) == MAT_SAND);
}

VXC_TEST(the_out_of_order_guard_can_fire_and_leaves_the_world_untouched) {
    // The guard above must be shown to fire, or it is a check nobody has seen
    // work. A terrain log naming a brick that is already promoted is exactly
    // the state a craft-first replay produces.
    SyntheticTileSampler tiles(kSeed);
    World<kB> src(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(src);
    src.setVoxel(0, 0, topVz, MAT_SAND);
    std::vector<uint8_t> tb;
    src.log().serialize(tb);
    const auto tlog = EditLog::parse(tb.data(), tb.size());
    CHECK(tlog.has_value());

    SyntheticTileSampler t2(kSeed);
    World<kB> dst(kSeed, t2);
    // Not promoted yet: the replay is accepted.
    CHECK(dst.replay(*tlog));

    SyntheticTileSampler t3(kSeed);
    World<kB> promotedFirst(kSeed, t3);
    promotedFirst.setCraftCell(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0),
                               craftCellOfVoxelMin(topVz), MAT_AIR);
    const uint64_t before = promotedFirst.editedDigest();
    CHECK(!promotedFirst.replay(*tlog));
    CHECK(promotedFirst.editedDigest() == before);
}

VXC_TEST(a_routed_dig_projects_its_brick_exactly_once) {
    // A projection is two downsampleBricks rounds -- ~36,864 child-cell reads
    // and eight brick constructions -- and it runs on the game thread.
    //
    // routeTerrainEditToCraft groups a dig by craft brick, and if it projects
    // per group it redoes the SAME terrain brick once per group. A dig at the
    // maximum size spans up to three craft bricks per axis, so that is up to 27
    // projections where one would do.
    //
    // It is invisible to every other test because it is CORRECT: each
    // projection is right for the state at that moment and the last one wins.
    // Only counting them catches it.
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    const BrickKey tb = ChunkMap<kB>::keyForVoxel(0, 0, topVz);

    // Promote first, so the dig below is routed rather than taking the plain
    // terrain path. Promotion itself projects once; measure from after it.
    w.setCraftCell(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0), craftCellOfVoxelMin(topVz),
                   MAT_AIR);
    CHECK(w.isPromoted(tb));
    const uint64_t before = w.craftLattice().counters.projections.load();

    // One edit spanning several craft bricks inside this ONE terrain brick.
    // A craft brick is 8 craft cells = 2 terrain voxels, so voxels 0..3 on each
    // axis cross a craft-brick boundary on each axis: 2x2x2 = 8 groups.
    std::vector<EditCell> cells;
    const int64_t bx = int64_t(tb.x) * kB, by = int64_t(tb.y) * kB, bz = int64_t(tb.z) * kB;
    for (int lz = 0; lz < 4; ++lz)
        for (int ly = 0; ly < 4; ++ly)
            for (int lx = 0; lx < 4; ++lx)
                cells.push_back(EditCell{
                    static_cast<uint16_t>(Brick<kB>::cellIndex(lx, ly, lz)), MAT_AIR});
    (void)bx; (void)by; (void)bz;
    w.applyEdit(tb, std::move(cells));

    const uint64_t projections = w.craftLattice().counters.projections.load() - before;
    CHECK(projections == 1);

    // And it still produced the right answer -- a cheaper wrong result is not
    // the thing being asked for here.
    for (int lz = 0; lz < 4; ++lz)
        for (int ly = 0; ly < 4; ++ly)
            for (int lx = 0; lx < 4; ++lx)
                CHECK(w.materialAt(bx + lx, by + ly, bz + lz) == MAT_AIR);
}

VXC_TEST(a_dig_into_a_promoted_brick_keeps_overlay_equal_to_the_projection) {
    // The invariant the routing exists to protect. A terrain edit written
    // straight to the overlay of a promoted brick would look right until the
    // next chisel anywhere in that brick silently reverted it.
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    const BrickKey tb = ChunkMap<kB>::keyForVoxel(0, 0, topVz);

    // Promote via a chisel, then dig a whole 10 cm voxel elsewhere in the brick.
    w.setCraftCell(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0), craftCellOfVoxelMin(topVz),
                   MAT_AIR);
    CHECK(w.isPromoted(tb));
    const size_t terrainEntriesBefore = w.log().size();
    w.setVoxel(3, 2, topVz, MAT_AIR);

    // The dig went to the craft stream, not the terrain stream.
    CHECK(w.log().size() == terrainEntriesBefore);
    CHECK(w.materialAt(3, 2, topVz) == MAT_AIR);

    // overlay == project(craft), cell for cell.
    Brick<kB> reprojected;
    CHECK(w.craftLattice().project(tb, reprojected));
    const Brick<kB> stored = w.brickAt(tb);
    int diffs = 0;
    for (int z = 0; z < kB; ++z)
        for (int y = 0; y < kB; ++y)
            for (int x = 0; x < kB; ++x)
                if (stored.get(x, y, z) != reprojected.get(x, y, z)) ++diffs;
    CHECK(diffs == 0);

    // And the chisel from before the dig is still there.
    CHECK(w.craftMaterialAt(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0),
                            craftCellOfVoxelMin(topVz)) == MAT_AIR);
}

// ---------------------------------------------------------------------------
// The projection reaches the coarse world
// ---------------------------------------------------------------------------

VXC_TEST(a_craft_carve_moves_materialAt_only_when_it_crosses_the_threshold) {
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    const MaterialId before = w.materialAt(0, 0, topVz);
    CHECK(before != MAT_AIR); // the test needs solid ground under it

    // One craft cell of 64: the 10 cm voxel must NOT move, so collision,
    // pathfinding and water keep the answer they had.
    w.setCraftCell(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0), craftCellOfVoxelMin(topVz),
                   MAT_AIR);
    CHECK(w.materialAt(0, 0, topVz) == before);
    // ...but the craft lattice did change, or this test proves nothing.
    CHECK(w.craftMaterialAt(craftCellOfVoxelMin(0), craftCellOfVoxelMin(0),
                            craftCellOfVoxelMin(topVz)) == MAT_AIR);

    // Hollow the whole voxel: now the coarse world must follow.
    for (int64_t dz = 0; dz < kCraftCellsPerVoxel; ++dz)
        for (int64_t dy = 0; dy < kCraftCellsPerVoxel; ++dy)
            for (int64_t dx = 0; dx < kCraftCellsPerVoxel; ++dx)
                w.setCraftCell(craftCellOfVoxelMin(0) + dx, craftCellOfVoxelMin(0) + dy,
                               craftCellOfVoxelMin(topVz) + dz, MAT_AIR);
    CHECK(w.materialAt(0, 0, topVz) == MAT_AIR);
}

VXC_TEST(promotion_alone_does_not_move_the_coarse_world) {
    // The engine-level statement of promote_is_projection_identity: a carve
    // that changes nothing must leave every one of the brick's 512 voxels
    // exactly as it was, including the ones nobody touched.
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    const BrickKey tb = ChunkMap<kB>::keyForVoxel(0, 0, topVz);

    const Brick<kB> before = w.brickAt(tb);

    // Write a craft cell to its EXISTING value: promotes the brick, appends a
    // log entry, projects -- and must be a total no-op on the coarse world.
    const int64_t cx = craftCellOfVoxelMin(0), cy = craftCellOfVoxelMin(0),
                  cz = craftCellOfVoxelMin(topVz);
    w.setCraftCell(cx, cy, cz, w.materialAt(0, 0, topVz));
    CHECK(w.isPromoted(tb));

    const Brick<kB> after = w.brickAt(tb);
    int diffs = 0;
    for (int z = 0; z < kB; ++z)
        for (int y = 0; y < kB; ++y)
            for (int x = 0; x < kB; ++x)
                if (before.get(x, y, z) != after.get(x, y, z)) ++diffs;
    CHECK(diffs == 0);
}

VXC_TEST(craftMaterialAt_reads_terrain_where_nothing_is_promoted) {
    SyntheticTileSampler tiles(kSeed);
    World<kB> w(kSeed, tiles);
    const int64_t topVz = surfaceVoxelZ(w);
    // Far from anything chiselled: all 4^3 craft cells report the parent voxel.
    const MaterialId want = w.materialAt(40, 40, topVz);
    for (int64_t dz = 0; dz < kCraftCellsPerVoxel; ++dz)
        for (int64_t dy = 0; dy < kCraftCellsPerVoxel; ++dy)
            for (int64_t dx = 0; dx < kCraftCellsPerVoxel; ++dx)
                CHECK(w.craftMaterialAt(craftCellOfVoxelMin(40) + dx,
                                        craftCellOfVoxelMin(40) + dy,
                                        craftCellOfVoxelMin(topVz) + dz) == want);
    CHECK(w.craftLattice().promotedCount() == 0);
}

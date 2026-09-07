#include "vxctest.h"
#include "voxelcore/craftvolume.h"
#include "voxelcore/world.h"
#include "voxelcore/editcompact.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetmanifest.h"
#include <fstream>
using namespace vxc;

VXC_TEST(craft125_projection_identity_and_eight_pages) {
    using L=CraftLayout<3>;
    static_assert(L::kCraftPitchUm==12500 && L::kCraftChunkEdgeCells==64);
    CraftLattice<8,3> craft;
    Brick<8> source(MAT_ROCK);
    source.set(0,1,2,MAT_AIR);source.set(7,6,5,MAT_CLAY);
    const BrickKey key{-1,2,-3};
    CHECK(craft.promote(key,source));
    CHECK_EQ(craft.craftBrickCount(),512u);
    Brick<8> projection;
    CHECK(craft.project(key,projection));
    for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        CHECK_EQ(projection.get(x,y,z),source.get(x,y,z));
    CraftProducerCounters counters;
    const auto pages=produceCraftRegionPages(craft,key,counters);
    CHECK(pages.produced);CHECK_EQ(pages.pages.size(),8u);
    for(const auto& page:pages.pages) {
        for(int z=0;z<32;++z) for(int y=0;y<32;++y) for(int x=0;x<32;++x) {
            const int64_t cx=int64_t(page.pageKey.x)*32+x,cy=int64_t(page.pageKey.y)*32+y,cz=int64_t(page.pageKey.z)*32+z;
            CHECK_EQ(decodeChunkVoxelCanonical(page.pack,x,y,z),craft.materialAt(cx,cy,cz));
        }
    }
    CHECK(craft.forceEraseCraftBrickForFaultInjection(L::craftBrickBaseOfTerrainBrick(key)));
    const auto refused=produceCraftRegionPages(craft,key,counters);
    CHECK(!refused.produced);CHECK(refused.pages.empty());
}
VXC_TEST(craft125_air_is_authoritative_and_coordinates_are_signed) {
    using L=CraftLayout<3>;
    CraftLattice<8,3> craft;
    CHECK(craft.promote({-1,-1,-1},Brick<8>(MAT_AIR)));
    CHECK(L::craftChunkKeyOfCell(-1,-1,-1)==BrickKey(-1,-1,-1));
    CraftProducerCounters counters;
    const auto pages=produceCraftRegionPages(craft,{-1,-1,-1},counters);
    CHECK(pages.produced);CHECK_EQ(pages.pages.size(),8u);
    for(const auto& p:pages.pages) CHECK(!p.pack.anySolid);
    CHECK(craft.setCell(-1,-1,-1,MAT_CLAY));
    CHECK_EQ(craft.materialAt(-1,-1,-1),MAT_CLAY);
    CHECK_EQ(L::voxelOfCraftCell(-9),-2);
}
VXC_TEST(craft125_log_roundtrip_compaction_and_legacy_migration) {
    SyntheticTileSampler tiles(17);
    World<8,2> old(17,tiles);
    old.setCraftCell(-1,-1,-1,MAT_CLAY);
    World<8> fine(17,tiles);
    CHECK(fine.replayCraft(old.craftLog()));
    for(int z=-2;z<0;++z) for(int y=-2;y<0;++y) for(int x=-2;x<0;++x)
        CHECK_EQ(fine.craftMaterialAt(x,y,z),MAT_CLAY);
    CHECK_EQ(fine.craftLog().latticePitchUm(),12500u);
    CHECK_EQ(fine.craftLog().formatVersionForContent(),4u);
    std::vector<uint8_t> bytes;fine.craftLog().serialize(bytes);
    auto parsed=EditLog::parse(bytes.data(),bytes.size());CHECK(parsed.has_value());
    CHECK_EQ(EditLog::peekHeader(bytes.data(),bytes.size()).latticePitchMm,12.5);
    World<8> replay(17,tiles);CHECK(replay.replayCraft(*parsed));
    CHECK_EQ(fine.craftDigest(),replay.craftDigest());
    auto compacted=compactLog(*parsed);
    CHECK_EQ(compacted.latticePitchUm(),12500u);
    World<8> compactReplay(17,tiles);CHECK(compactReplay.replayCraft(compacted));
    CHECK_EQ(fine.craftDigest(),compactReplay.craftDigest());
    fine.applyEdit({-1,-1,-1},{{511,MAT_SAND}});
    for(int z=-8;z<0;++z) for(int y=-8;y<0;++y) for(int x=-8;x<0;++x)
        CHECK_EQ(fine.craftMaterialAt(x,y,z),MAT_SAND);
    CHECK_EQ(fine.craftMaterialAt(-9,-1,-1),replay.craftMaterialAt(-9,-1,-1));
}
VXC_TEST(manifest125_python_fixture_preserves_exact_scale) {
    std::ifstream file(std::string(VXC_TEST_FIXTURE_DIR)+"/pitch125.vxm",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    CHECK(!bytes.empty());
    AssetManifest manifest;CHECK_EQ(int(manifest.parse(bytes)),int(AssetManifestError::kOk));
    CHECK_EQ(manifest.species().size(),1u);
    CHECK_EQ(manifest.species()[0].voxelSizeMm,12.5);
}
VXC_TEST(asset125_python_fixture_preserves_exact_scale) {
    std::ifstream file(std::string(VXC_TEST_FIXTURE_DIR)+"/pitch125.vxa",std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    CHECK(!bytes.empty());
    AssetGrid grid;CHECK_EQ(int(grid.parse(bytes)),int(AssetParseError::kOk));
    CHECK_EQ(grid.voxelSizeUm(),12500u);CHECK_EQ(grid.voxelSizeMm(),12.5);
    CHECK_EQ(grid.sizeX(),3);CHECK_EQ(grid.originX(),-1);CHECK(!grid.onTerrainLattice());
    CHECK_EQ(grid.at(0,0,0),MAT_CLAY);CHECK_EQ(grid.at(2,1,0),MAT_ROCK);
}

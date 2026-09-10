#include "voxelcore/worldquery.h"
#include "voxelcore/raycast.h"
#include "vxctest.h"
#include <chrono>
#include <cstdio>
using namespace vxc;
namespace {
AssetGrid block(MaterialId material) {
    std::vector<uint8_t> bytes;
    auto put=[&](uint32_t v){for(int i=0;i<4;++i)bytes.push_back(uint8_t(v>>(8*i)));};
    put(kVxaMagic);put(kVxaVersion);put(uint32_t(-5));put(uint32_t(-5));put(0);
    put(10);put(10);put(30);put(100);put(1);put(0);put(0);
    bytes.push_back(uint8_t(material));put(3000);AssetGrid grid;
    CHECK(grid.parse(bytes)==AssetParseError::kOk);return grid;
}
struct Bank: IAssetBankSource {
    AssetGrid first=block(MAT_BARK),second=block(MAT_SAND);
    mutable int calls=0;
    const AssetGrid* bankGrid(uint16_t id,uint16_t) const override {++calls;return id==1?&first:&second;}
};
struct Channels: IAssetChannelSource {
    int calls=0;
    AssetColumnChannels channelsAt(int64_t x,int64_t y) override {
        ++calls;AssetColumnChannels c;c.distanceToWaterMm=((x+y)&1)?50:100;return c;
    }
};
struct Fixture {
    SyntheticTileSampler tiles{1234};Bank bank;Channels channels;AssetField field;World<8> world{1234,tiles};
    Fixture(int radius=600) {
        AssetLayer layers[2];AssetSpecies species[2];
        for(int i=0;i<2;++i){auto& l=layers[i];l.cellMm=400;l.maxHeightMm=3000;l.maxRadiusMm=radius;l.densityPerMille=1000;l.seedCount=1;
            auto& s=species[i];s.bankId=uint16_t(i+1);s.layer=uint8_t(i);s.heightMm=3000;s.voxelSizeMm=100;
            s.elevMinMm=-1000000;s.elevMaxMm=9000000;s.slopeMaxMmPerM=100000;s.waterMaxMm=200;
            for(int b=0;b<kBiomeCount;++b)s.weightPerMille[b]=1000;
        }
        field.setSeed(1234);field.setLayers(layers,2);field.setSpecies(species,2);field.setBankSource(&bank);
        world.setAssetField(&field);world.setAssetChannelSource(&channels);
    }
};
}
VXC_TEST(worldquery_exact_negative_overlap_terrain_channels_and_live_edits) {
    Fixture f;const AssetVoxelRect rect{-12,-11,4,3};WorldQuery<8> query(f.world,rect);
    CHECK(query.usesShortlist());CHECK(query.candidateCount()>0);CHECK(f.channels.calls>0);
    int assets=0,terrain=0,overlap=0;int64_t hitX=0,hitY=0,hitZ=0;
    const int bankBefore=f.bank.calls,channelsBefore=f.channels.calls;
    for(int64_t y=rect.vy0;y<=rect.vy1;++y)for(int64_t x=rect.vx0;x<=rect.vx1;++x){
        const int64_t top=topSolidVoxelZ(f.world.amplifier().columnCached(x,y).surfaceMm);
        for(int64_t z=top-2;z<top+34;++z){const auto m=query.materialAt(x,y,z);
            if(m==MAT_BARK){++assets;hitX=x;hitY=y;hitZ=z;} if(f.world.amplifier().materialAt(x,y,z)!=MAT_AIR)++terrain;
        }
    }
    CHECK_EQ(f.bank.calls,bankBefore);CHECK_EQ(f.channels.calls,channelsBefore);CHECK(assets>0);CHECK(terrain>0);
    for(int64_t y=rect.vy0;y<=rect.vy1;++y)for(int64_t x=rect.vx0;x<=rect.vx1;++x){
        const int64_t top=topSolidVoxelZ(f.world.amplifier().columnCached(x,y).surfaceMm);
        for(int64_t z=top-2;z<top+34;++z)CHECK_EQ(int(query.materialAt(x,y,z)),int(f.world.materialAt(x,y,z)));
        const auto instances=f.field.instancesForRect({x,y,x,y},[&](int64_t ax,int64_t ay){return assetColumnFactsFromSample(f.world.amplifier().columnCached(ax,ay),f.world.assetChannelsAt(ax,ay));});
        int winners=0;for(const auto& i:instances)if(f.field.materialAt({i},x,y,top+5)!=MAT_AIR)++winners;
        if(winners>1)++overlap;
    }
    CHECK(overlap>0);
    f.world.setVoxel(hitX,hitY,hitZ,MAT_AIR);CHECK_EQ(int(query.materialAt(hitX,hitY,hitZ)),int(MAT_AIR));
    f.world.setVoxel(hitX,hitY,hitZ,MAT_SAND);CHECK_EQ(int(query.materialAt(hitX,hitY,hitZ)),int(MAT_SAND));
    // Same live edited-brick precedence for neighboring unchanged voxels.
    for(int dz=-2;dz<=2;++dz)CHECK_EQ(int(query.materialAt(hitX,hitY,hitZ+dz)),int(f.world.materialAt(hitX,hitY,hitZ+dz)));
    CHECK_EQ(int(query.materialAt(5,3,hitZ)),int(f.world.materialAt(5,3,hitZ)));
    const auto old=[&](int64_t x,int64_t y,int64_t z){return f.world.materialAt(x,y,z);};
    const auto fast=[&](int64_t x,int64_t y,int64_t z){return query.materialAt(x,y,z);};
    for(int dx:{-1000,0,1000}){auto a=raycastVoxels(old,hitX*100,hitY*100,(hitZ+8)*100,dx,0,-2000);auto b=raycastVoxels(fast,hitX*100,hitY*100,(hitZ+8)*100,dx,0,-2000);
        CHECK_EQ(a.hit,b.hit);CHECK_EQ(a.vx,b.vx);CHECK_EQ(a.vy,b.vy);CHECK_EQ(a.vz,b.vz);CHECK_EQ(a.faceAxis,b.faceAxis);CHECK_EQ(a.faceSign,b.faceSign);}
}
VXC_TEST(worldquery_batch_reuse_rebuild_and_live_edits) {
    Fixture f;WorldQueryBatch<8> batch(f.world);
    batch.prepare({-12,-11,-8,-7});
    const int calls=f.channels.calls;
    const auto& near=batch.prepare({-10,-9,-6,-5});
    CHECK_EQ(batch.preparationCount(),size_t(1));CHECK_EQ(f.channels.calls,calls);
    const int64_t z=topSolidVoxelZ(f.world.amplifier().columnCached(-9,-8).surfaceMm)+5;
    CHECK_EQ(int(near.materialAt(-9,-8,z)),int(f.world.materialAt(-9,-8,z)));
    f.world.setVoxel(-9,-8,z,MAT_SAND);
    CHECK_EQ(int(near.materialAt(-9,-8,z)),int(MAT_SAND));
    f.world.setVoxel(-9,-8,z,MAT_AIR);
    CHECK_EQ(int(near.materialAt(-9,-8,z)),int(MAT_AIR));
    const auto& far=batch.prepare({100,101,102,103});
    CHECK_EQ(batch.preparationCount(),size_t(2));
    CHECK_EQ(int(far.materialAt(101,102,z)),int(f.world.materialAt(101,102,z)));
    const auto& invalid=batch.prepare({1,1,0,0});
    CHECK(!invalid.usesShortlist());
    CHECK_EQ(int(invalid.materialAt(-9,-8,z)),int(MAT_AIR));
    const auto& large=batch.prepare({-5000,-5000,5000,5000});
    CHECK(!large.usesShortlist());
    CHECK_EQ(int(large.materialAt(-9,-8,z)),int(MAT_AIR));
    // New caller updates rebuild even at the same location.
    WorldQueryBatch<8> next(f.world);next.prepare({-12,-11,-8,-7});
    CHECK_EQ(next.preparationCount(),size_t(1));
}

VXC_TEST(worldquery_point_reach_and_invalid_rectangle_fallback) {
    Fixture f(0);WorldQuery<8> query(f.world,{-10,-10,10,10});
    int air=0,solid=0;
    for(int64_t y=-10;y<=10;++y)for(int64_t x=-10;x<=10;++x){const auto z=topSolidVoxelZ(f.world.amplifier().columnCached(x,y).surfaceMm)+4;
        const auto m=f.world.materialAt(x,y,z);CHECK_EQ(int(query.materialAt(x,y,z)),int(m));if(m==MAT_AIR)++air;else ++solid;}
    CHECK(air>0);CHECK(solid>0);
    for(const AssetVoxelRect r: {AssetVoxelRect{1,0,0,1},AssetVoxelRect{-5000,0,5000,1}}){WorldQuery<8> fallback(f.world,r);CHECK(!fallback.usesShortlist());CHECK_EQ(int(fallback.materialAt(-1,-1,0)),int(f.world.materialAt(-1,-1,0)));}
    SyntheticTileSampler tiles(5678);World<8> empty(5678,tiles);WorldQuery<8> noAssets(empty,{-1,-1,1,1});CHECK(noAssets.usesShortlist());CHECK_EQ(noAssets.candidateCount(),size_t(0));
    CHECK_EQ(int(noAssets.materialAt(-1,-1,0)),int(empty.materialAt(-1,-1,0)));
}

VXC_TEST(worldquery_roof_probe_excludes_canopies_and_tracks_rock_edits) {
    Fixture f;WorldQuery<8> query(f.world,{-12,-12,12,12});
    bool found=false;
    for(int y=-12;y<=12&&!found;++y)for(int x=-12;x<=12&&!found;++x){
        const auto z=topSolidVoxelZ(f.world.amplifier().columnCached(x,y).surfaceMm)+5;
        if(query.materialAt(x,y,z)!=MAT_BARK)continue;
        CHECK(!query.undergroundRoofAt(x,y,z));
        f.world.setVoxel(x,y,z,MAT_LEAF_BROADLEAF);CHECK(!query.undergroundRoofAt(x,y,z));
        f.world.setVoxel(x,y,z,MAT_ROCK);CHECK(query.undergroundRoofAt(x,y,z));
        f.world.setVoxel(x,y,z,MAT_AIR);CHECK(!query.undergroundRoofAt(x,y,z));
        found=true;
    }
    CHECK(found);
    for(auto material:{MAT_BARK,MAT_BARK_PALE,MAT_HEARTWOOD,MAT_DEADWOOD,MAT_LEAF_BROADLEAF,MAT_LEAF_NEEDLE,MAT_LEAF_JUNGLE,MAT_LEAF_DRY,MAT_LEAF_BLOSSOM,MAT_LEAF_AUTUMN,MAT_WATERMARK})
        CHECK(!isUndergroundRoofMaterial(material));
    CHECK(isUndergroundRoofMaterial(MAT_BEDROCK));CHECK(isUndergroundRoofMaterial(MAT_TOPSOIL));
}

VXC_TEST(worldquery_ecological_scan_matches_points_and_reuses_placement) {
    Fixture f;
    EcoPlacementConfig ecology;ecology.biomeMask=uint16_t((1u<<kBiomeCount)-1);
    for(int i=0;i<2;++i){
        EcoSpeciesProfile p;p.bankId=uint16_t(i+1);p.stableId=uint64_t(i+11);p.tree=true;
        p.communityWeights={1000,1000,1000,1000};
        p.variants={{uint64_t(i+21),0,3000,true,600,100,EcoGrowthForm::Woodland}};
        ecology.species.push_back(p);
    }
    CHECK(f.field.setEcology(ecology));
    WorldQuery<8> query(f.world,{-12,-12,12,12});
    WorldQueryBatch<8> batch(f.world);
    CHECK(query.candidateCount()>0);
    int solids=0;
    for(int y=-12;y<=12;y+=6)for(int x=-12;x<=12;x+=6){
        const auto& batched=batch.prepare({x,y,x,y});
        const auto top=topSolidVoxelZ(f.world.amplifier().columnCached(x,y).surfaceMm);
        for(int dz=1;dz<=35;dz+=3){
            const auto before=f.channels.calls;
            const auto fast=query.materialAt(x,y,top+dz);
            CHECK_EQ(f.channels.calls,before);
            CHECK_EQ(int(fast),int(f.world.materialAt(x,y,top+dz)));
            CHECK_EQ(int(batched.materialAt(x,y,top+dz)),int(fast));
            if(fast!=MAT_AIR)++solids;
        }
    }
    CHECK(solids>0);
    CHECK_EQ(batch.preparationCount(),size_t(1));
}

VXC_TEST(worldquery_channel_gate_and_query_cost_evidence) {
    Fixture f;World<8> noChannels(1234,f.tiles);noChannels.setAssetField(&f.field);
    WorldQuery<8> absent(noChannels,{-12,-11,4,3});CHECK_EQ(absent.candidateCount(),size_t(0));
    using Clock=std::chrono::steady_clock;
    const auto begin=Clock::now();WorldQuery<8> query(f.world,{-12,-11,4,3});const auto prepared=Clock::now();
    CHECK(query.candidateCount()>0);
    const auto top=topSolidVoxelZ(f.world.amplifier().columnCached(-4,-4).surfaceMm);
    uint64_t slowSum=0,fastSum=0;
    const auto slowBegin=Clock::now();
    for(int n=0;n<32;++n)for(int z=35;z<60;++z)slowSum+=uint64_t(f.world.materialAt(-4,-4,top+z));
    const auto slowEnd=Clock::now();
    for(int n=0;n<32;++n)for(int z=35;z<60;++z)fastSum+=uint64_t(query.materialAt(-4,-4,top+z));
    const auto fastEnd=Clock::now();CHECK_EQ(slowSum,fastSum);
    std::printf("[query-cost] %zu candidates, prepare %.3f ms, 800 point samples %.3f ms -> %.3f ms (diagnostic, no timing threshold)\n",
        query.candidateCount(),std::chrono::duration<double,std::milli>(prepared-begin).count(),
        std::chrono::duration<double,std::milli>(slowEnd-slowBegin).count(),std::chrono::duration<double,std::milli>(fastEnd-slowEnd).count());
}

VXC_TEST(worldquery_integer_ray_rectangle_covers_every_dda_sample) {
    for(int64_t ox:{-200,-150,0,50,200})for(int64_t oy:{-200,-150,0,50,200})
    for(int64_t dx:{-250,-200,-50,0,50,200,250})for(int64_t dy:{-250,-200,-50,0,50,200,250}) {
        AssetVoxelRect rect;CHECK(worldQueryRayRect(ox,oy,dx,dy,rect));int samples=0;
        const auto allAir=[&](int64_t x,int64_t y,int64_t){++samples;CHECK(x>=rect.vx0&&x<=rect.vx1);CHECK(y>=rect.vy0&&y<=rect.vy1);return MAT_AIR;};
        CHECK(!raycastVoxels(allAir,ox,oy,0,dx,dy,0).hit);CHECK(samples>0);
    }
    AssetVoxelRect r;CHECK(worldQueryRayRect(50,50,-50,-50,r));CHECK_EQ(r.vx0,int64_t(-1));CHECK_EQ(r.vy0,int64_t(-1));
    CHECK_EQ(r.vx1,int64_t(0));CHECK_EQ(r.vy1,int64_t(0));
    CHECK(worldQueryRayRect(50,50,50,50,r));CHECK_EQ(r.vx1,int64_t(1));CHECK_EQ(r.vy1,int64_t(1));
    CHECK(worldQueryRayRect(0,0,0,0,r));CHECK_EQ(r.vx0,int64_t(0));CHECK_EQ(r.vx1,int64_t(0));
}
VXC_TEST(worldquery_integer_ray_rectangle_refuses_overflow_and_oversize) {
    AssetVoxelRect r;
    constexpr auto max=std::numeric_limits<int64_t>::max(),min=std::numeric_limits<int64_t>::min();
    CHECK(!worldQueryRayRect(max,0,1,0,r));CHECK(!r.valid());
    CHECK(!worldQueryRayRect(min,0,-1,0,r));CHECK(!r.valid());
    CHECK(!worldQueryRayRect(0,max,0,1,r));CHECK(!r.valid());
    CHECK(!worldQueryRayRect(0,min,0,-1,r));CHECK(!r.valid());
    CHECK(!worldQueryRayRect(0,0,409700,0,r));CHECK(!r.valid());
    CHECK(!worldQueryRayRect(0,0,0,-409700,r));CHECK(!r.valid());
    SyntheticTileSampler tiles(1234);World<8> world(1234,tiles);WorldQuery<8> fallback(world,r);
    CHECK(!fallback.usesShortlist());CHECK_EQ(int(fallback.materialAt(-1,2,3)),int(world.materialAt(-1,2,3)));
}

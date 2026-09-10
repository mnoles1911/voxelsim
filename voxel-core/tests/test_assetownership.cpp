#include "voxelcore/assetownership.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetcandidate.h"
#include "vxctest.h"
using namespace vxc;

namespace {
AssetGrid candidateFixture(int sx,int sy,int sz,int ox,int oy,int oz,
                           const std::vector<MaterialId>& cells,int pitch=100) {
    std::vector<uint8_t> bytes;
    auto word=[&](uint32_t v){for(int i=0;i<4;++i)bytes.push_back(uint8_t(v>>(i*8)));};
    word(kVxaMagic);word(3);word(ox);word(oy);word(oz);word(sx);word(sy);word(sz);
    word(pitch);word(uint32_t(cells.size()));word(0);word(0);
    for(auto m:cells){bytes.push_back(uint8_t(m));word(1);}
    AssetGrid g;CHECK(g.parse(bytes)==AssetParseError::kOk);return g;
}
ColumnSample candidateAir(int64_t,int64_t){ColumnSample c;c.surfaceMm=-100000;return c;}
}

VXC_TEST(asset_candidate_vxa_world_axes_all_yaws_negative_origins) {
    const std::vector<MaterialId> cells={MAT_ROCK,MAT_AIR,MAT_TOPSOIL,MAT_ROCK,MAT_AIR,MAT_TOPSOIL,
        MAT_TOPSOIL,MAT_ROCK,MAT_AIR,MAT_TOPSOIL,MAT_ROCK,MAT_AIR};
    auto g=candidateFixture(2,3,2,-4,-7,-2,cells);
    for(uint8_t yaw=0;yaw<4;++yaw){
        AssetField::ResolvedAssetInstance r;r.grid=&g;r.anchorVx=-91;r.anchorVy=-130;r.anchorVz=12;r.yawQuarter=yaw;
        AssetCandidateBounds b;CHECK(assetCandidateBounds(r,b));std::vector<uint8_t> bytes;
        int columns=0;CHECK(assetBuildCandidateVxa({r},0,[&](int64_t x,int64_t y){
            CHECK(x>=b.x0&&x<=b.x1&&y>=b.y0&&y<=b.y1);++columns;return candidateAir(x,y);},bytes));
        AssetGrid out;CHECK(out.parse(bytes)==AssetParseError::kOk);CHECK_EQ(out.voxelSizeMm(),100);
        CHECK_EQ(columns,6);CHECK_EQ(out.sizeX(),yaw%2?3:2);CHECK_EQ(out.sizeY(),yaw%2?2:3);CHECK_EQ(out.sizeZ(),2);
        CHECK_EQ(out.solidCount(),g.solidCount());
        for(int x=0;x<2;++x)for(int y=0;y<3;++y)for(int z=0;z<2;++z){
            int rx=x-4,ry=y-7;for(int q=0;q<yaw;++q){int old=rx;rx=-ry;ry=old;}
            const auto expected=cells[(x*3+y)*2+z];
            CHECK_EQ(out.at(rx-out.originX(),ry-out.originY(),z-2-out.originZ()),expected);
            CHECK_EQ(assetCandidateMaterial(r,b,r.anchorVx+rx,r.anchorVy+ry,r.anchorVz+z-2),expected);
            for(int cx=0;cx<2;++cx)for(int cy=0;cy<2;++cy)for(int cz=0;cz<2;++cz){
                // Rotate doubled corner offsets around the cell centre,
                // independently of the helper's reflected-corner branches.
                int dx=2*cx-1,dy=2*cy-1;
                for(int q=0;q<yaw;++q){int old=dx;dx=-dy;dy=old;}
                AssetCandidateSourceVertex vertex;
                CHECK(assetCandidateSourceVertex(r,r.anchorVx+rx,r.anchorVy+ry,r.anchorVz+z-2,
                    uint8_t((dx+1)/2),uint8_t((dy+1)/2),uint8_t(cz),vertex));
                CHECK_EQ(vertex.x,x-4+cx);CHECK_EQ(vertex.y,y-7+cy);CHECK_EQ(vertex.z,z-2+cz);
            }
            for(uint8_t sourceAxis=0;sourceAxis<3;++sourceAxis)for(int sign:{-1,1}){
                int normal[3]={0,0,0};normal[sourceAxis]=sign;
                for(int q=0;q<yaw;++q){int old=normal[0];normal[0]=-normal[1];normal[1]=old;}
                uint8_t worldAxis=0;while(normal[worldAxis]==0)++worldAxis;
                AssetCandidateSourceSample mapped;
                CHECK(assetCandidateSourceSample(r,r.anchorVx+rx,r.anchorVy+ry,r.anchorVz+z-2,worldAxis,normal[worldAxis]>0,mapped));
                CHECK_EQ(mapped.x,x-4);CHECK_EQ(mapped.y,y-7);CHECK_EQ(mapped.z,z-2);
                CHECK_EQ(mapped.axis,sourceAxis);CHECK_EQ(mapped.positive,sign>0);
            }
        }
        AssetCandidateSourceSample refused;
        CHECK(!assetCandidateSourceSample(r,b.x0-1,b.y0,b.z0,0,true,refused));
        CHECK(!assetCandidateSourceSample(r,b.x0,b.y1+1,b.z0,0,true,refused));
        CHECK(!assetCandidateSourceSample(r,b.x0,b.y0,b.z1+1,0,true,refused));
        CHECK(!assetCandidateSourceSample(r,b.x0,b.y0,b.z0,3,true,refused));
        AssetCandidateSourceVertex badVertex;
        CHECK(!assetCandidateSourceVertex(r,b.x0,b.y0,b.z0,2,0,0,badVertex));
        CHECK(!assetCandidateSourceVertex(r,b.x0,b.y0,b.z0,0,2,0,badVertex));
        CHECK(!assetCandidateSourceVertex(r,b.x0,b.y0,b.z0,0,0,2,badVertex));
        CHECK(!assetCandidateSourceVertex(r,b.x1+1,b.y0,b.z0,0,0,0,badVertex));
        CHECK_EQ(assetCandidateMaterial(r,b,b.x0-1,b.y0,b.z0),MAT_AIR);
        std::vector<uint8_t> again;CHECK(assetBuildCandidateVxa({r},0,candidateAir,again));CHECK(bytes==again);
    }
}

VXC_TEST(asset_candidate_terrain_and_ordered_occlusion_owned_winner) {
    auto g=candidateFixture(1,1,3,0,0,0,{MAT_ROCK,MAT_ROCK,MAT_ROCK});
    auto blocker=candidateFixture(1,1,3,0,0,0,{MAT_AIR,MAT_TOPSOIL,MAT_AIR});
    AssetField::ResolvedAssetInstance first,selected;first.grid=&blocker;selected.grid=&g;
    first.anchorVx=selected.anchorVx=-32;first.anchorVy=selected.anchorVy=-1;
    std::vector<AssetField::ResolvedAssetInstance> ordered{first,selected};std::vector<uint8_t> bytes;
    auto ground=[](int64_t,int64_t){ColumnSample c;c.surfaceMm=100;return c;};
    CHECK(assetBuildCandidateVxa(ordered,1,ground,bytes));AssetGrid out;CHECK(out.parse(bytes)==AssetParseError::kOk);
    CHECK_EQ(out.at(0,0,0),MAT_AIR);CHECK_EQ(out.at(0,0,1),MAT_AIR);CHECK_EQ(out.at(0,0,2),MAT_ROCK);
    CHECK_EQ(assetTerrainRenderMaterial(ordered,-32,-1,1,[](const auto&){return false;}),MAT_TOPSOIL);
    CHECK_EQ(assetTerrainRenderMaterial(ordered,-32,-1,1,[&](const auto& r){return r.grid==&blocker;}),MAT_AIR);
    CHECK_EQ(assetTerrainRenderMaterial(ordered,-32,-1,2,[&](const auto& r){return r.grid==&blocker;}),MAT_ROCK);
    CHECK_EQ(assetTerrainRenderMaterial(ordered,-32,-1,1,[&](const auto& r){return r.grid==&g;}),MAT_TOPSOIL);
    CHECK(assetBuildCandidateVxa(ordered,0,candidateAir,bytes));CHECK(out.parse(bytes)==AssetParseError::kOk);
    CHECK_EQ(out.at(0,0,1),MAT_TOPSOIL); // later asset cannot hide the earlier winner
    CHECK(!assetBuildCandidateVxa({selected,selected},1,candidateAir,bytes));CHECK(bytes.empty());
}

VXC_TEST(asset_candidate_failure_limits_clear_output) {
    auto g=candidateFixture(1,1,2,0,0,0,{MAT_ROCK,MAT_TOPSOIL});
    AssetField::ResolvedAssetInstance r;r.grid=&g;std::vector<uint8_t> bytes{1,2,3};
    CHECK(!assetBuildCandidateVxa({r},1,candidateAir,bytes));CHECK(bytes.empty());
    CHECK(!assetBuildCandidateVxa({r},0,candidateAir,bytes,1));CHECK(bytes.empty());
    CHECK(!assetBuildCandidateVxa({r},0,candidateAir,bytes,2,57));CHECK(bytes.empty());
    CHECK(assetBuildCandidateVxa({r},0,candidateAir,bytes,2,58));CHECK_EQ(bytes.size(),58u);
    AssetCandidateBounds b;r.yawQuarter=4;CHECK(!assetCandidateBounds(r,b));r.yawQuarter=0;
    r.anchorVx=(int64_t(1)<<50)+1;CHECK(!assetCandidateBounds(r,b));r.anchorVx=-(int64_t(1)<<50)-1;CHECK(!assetCandidateBounds(r,b));r.anchorVx=0;
    auto fine=candidateFixture(1,1,1,0,0,0,{MAT_ROCK},50);r.grid=&fine;CHECK(!assetCandidateBounds(r,b));
    auto far=candidateFixture(1,1,1,-1000001,0,0,{MAT_ROCK});r.grid=&far;CHECK(!assetCandidateBounds(r,b));
    AssetGrid invalid;r.grid=&invalid;CHECK(!assetCandidateBounds(r,b));r.grid=nullptr;CHECK(!assetCandidateBounds(r,b));
    bytes={1};CHECK(!assetBuildCandidateVxa({r},0,candidateAir,bytes));CHECK(bytes.empty());
    r.grid=&g;auto earth=[](int64_t,int64_t){ColumnSample c;c.surfaceMm=10000;return c;};
    CHECK(!assetBuildCandidateVxa({r},0,earth,bytes));CHECK(bytes.empty());
}

VXC_TEST(asset_candidate_page_aprons_signed_coordinates_and_limits) {
    for(uint8_t level=0;level<=4;++level)for(int64_t key=-2;key<=2;++key){
        const int64_t scale=int64_t(1)<<level;
        AssetRenderPage page{key,key,key,level,AssetCpu|AssetGpu};
        const int64_t lo=(key*32-1)*scale,hi=(key*32+33)*scale-1;
        auto point=[&](int64_t x,int64_t y,int64_t z){return assetPageTouches({x,y,z,x,y,z},page);};
        CHECK(point(lo,lo,lo));CHECK(point(hi,hi,hi));
        CHECK(!point(lo-1,lo,lo));CHECK(!point(hi+1,hi,hi));
        CHECK(!point(lo,lo-1,lo));CHECK(!point(hi,hi,hi+1));
    }
    AssetCandidateBounds origin{0,0,0,0,0,0};AssetRenderPage p{0,0,0,31,AssetCpu};CHECK(!assetPageTouches(origin,p));
    p.level=0;p.x=int64_t(INT32_MAX)+1;CHECK(!assetPageTouches(origin,p));
    p.x=int64_t(INT32_MIN)-1;CHECK(!assetPageTouches(origin,p));
    p={INT32_MIN,INT32_MIN,INT32_MIN,30,AssetGpu};CHECK(!assetPageTouches(origin,p));
}

VXC_TEST(asset_ownership_atomic_both_backends_and_stale_jobs) {
    AssetRenderOwnership state;AssetProvenance tree;tree.worldSeed=42;tree.anchorVx=-17;tree.bankId=4;
    const std::vector<AssetRenderPage> pages{{-1,0,2,0,AssetCpu|AssetGpu},{-1,0,1,1,AssetCpu|AssetGpu}};
    const auto original=state.visible();auto t=state.begin(tree,AssetRenderOwner::Object,3,3,pages);
    CHECK(t.serial);CHECK(!state.visible().objectOwns(tree));CHECK(state.target(t)->objectOwns(tree));
    CHECK(!state.objectReady(t,2));CHECK(state.objectReady(t,3));
    CHECK(!state.pageReady(t,pages[0],AssetCpu,0));
    CHECK(state.pageReady(t,pages[0],AssetCpu,1));CHECK(state.pageReady(t,pages[0],AssetGpu,1));
    CHECK(state.pageReady(t,pages[1],AssetCpu,1));CHECK(!state.ready(t));
    int calls=0;CHECK(!state.publish(t,[&](auto&,auto&,auto&){++calls;return true;}));CHECK_EQ(calls,0);
    CHECK(state.pageReady(t,pages[1],AssetGpu,1));CHECK(state.ready(t));
    CHECK(!state.publish(t,[&](auto&,auto&,auto&){++calls;return false;}));
    CHECK(!state.visible().objectOwns(tree));CHECK(state.acceptsVisibleJob(0));
    CHECK(state.publish(t,[&](const auto& before,const auto& after,const auto& ready){
        ++calls;return !before.objectOwns(tree)&&after.objectOwns(tree)&&ready.size()==2;
    }));
    CHECK(state.visible().objectOwns(tree));CHECK(!original.objectOwns(tree));
    CHECK(!state.acceptsVisibleJob(0));CHECK(state.acceptsVisibleJob(1));
    CHECK(!state.pageReady(t,pages[0],AssetCpu,1));CHECK(!state.cancel(t));
    CHECK_EQ(calls,2);
}
VXC_TEST(asset_ownership_demotion_needs_current_projection_and_cancel_preserves_scene) {
    AssetRenderOwnership state;AssetProvenance tree;tree.worldSeed=8;
    const std::vector<AssetRenderPage> pages{{0,0,0,0,AssetCpu}};
    CHECK(!state.begin(tree,AssetRenderOwner::Object,2,1,pages).serial);
    auto t=state.begin(tree,AssetRenderOwner::Object,2,2,pages);CHECK(t.serial);
    CHECK(!state.begin(tree,AssetRenderOwner::Terrain,2,2,pages).serial);
    state.objectReady(t,2);state.pageReady(t,pages[0],AssetCpu,1);
    CHECK(state.publish(t,[](auto&,auto&,auto&){return true;}));
    CHECK(!state.begin(tree,AssetRenderOwner::Terrain,1,1,pages).serial);
    t=state.begin(tree,AssetRenderOwner::Terrain,3,3,pages);CHECK(t.serial);
    CHECK(state.visible().objectOwns(tree));CHECK(!state.target(t)->objectOwns(tree));
    CHECK(state.cancel(t));CHECK(state.visible().objectOwns(tree));
    t=state.begin(tree,AssetRenderOwner::Terrain,3,3,pages);state.pageReady(t,pages[0],AssetCpu,2);
    CHECK(state.publish(t,[](auto&,auto&,auto&){return true;}));CHECK(!state.visible().objectOwns(tree));
    auto duplicate=pages;duplicate.push_back(pages[0]);
    CHECK(!state.begin(tree,AssetRenderOwner::Object,4,4,duplicate).serial);
}
VXC_TEST(asset_ownership_identity_and_render_filter_leave_authority_intact) {
    AssetProvenance a;a.worldSeed=123;a.providerFingerprint=7;a.catalogFingerprint=8;
    a.anchorVx=-1;a.anchorVy=-700;a.anchorVz=29;a.bankId=4;a.seedIndex=5;a.yawQuarter=3;
    auto b=a;b.anchorVx=1;CHECK(!(assetObjectId(a)==assetObjectId(b)));
    auto changed=a;changed.catalogFingerprint=9;CHECK(!(assetObjectId(a)==assetObjectId(changed)));
    changed=a;changed.providerFingerprint=9;CHECK(!(assetObjectId(a)==assetObjectId(changed)));
    CHECK(assetObjectId(a)==assetObjectId(a));
    std::vector<AssetProvenance> authoritative{a,b};
    AssetOwnershipSnapshot snapshot;snapshot.records.push_back({a,assetObjectId(a),AssetRenderOwner::Object,1,1});
    auto cpu=assetTerrainRenderInstances(authoritative,snapshot,[](const auto& p){return p;});
    auto gpu=assetTerrainRenderInstances(authoritative,snapshot,[](const auto& p){return p;});
    CHECK_EQ(authoritative.size(),2u);CHECK_EQ(cpu.size(),1u);CHECK(cpu==gpu);CHECK(cpu[0]==b);
    CHECK(authoritative[0]==a); // collision/edit source is never filtered
}
VXC_TEST(asset_resolved_instances_preserve_bank_provenance) {
    std::vector<uint8_t> bytes;auto word=[&](uint32_t value){for(int i=0;i<4;++i)bytes.push_back(uint8_t(value>>(8*i)));};
    word(kVxaMagic);word(3);word(0);word(0);word(0);word(1);word(1);word(1);word(100);word(1);word(0);word(0);
    bytes.push_back(MAT_ROCK);word(1);AssetGrid grid;CHECK(grid.parse(bytes.data(),bytes.size())==AssetParseError::kOk);
    struct Bank:IAssetBankSource {const AssetGrid* grid;explicit Bank(const AssetGrid* g):grid(g){} const AssetGrid* bankGrid(uint16_t,uint16_t)const override{return grid;}} bank(&grid);
    AssetField field;AssetLayer layer;layer.terrainLattice=true;field.setLayers(&layer,1);field.setBankSource(&bank);
    AssetInstance instance;instance.anchorXMm=-101;instance.anchorYMm=201;instance.anchorVz=8;
    instance.bankId=12;instance.seedIndex=3;instance.speciesIndex=25;instance.yawQuarter=2;
    auto resolved=field.resolveForCompose({instance});CHECK_EQ(resolved.size(),1u);
    CHECK_EQ(resolved[0].bankId,12);CHECK_EQ(resolved[0].seedIndex,3);CHECK_EQ(resolved[0].speciesIndex,25);
    CHECK_EQ(resolved[0].anchorVx,-2);CHECK_EQ(resolved[0].anchorVy,2);
    CHECK_EQ(AssetField::materialAtResolved(resolved,-2+grid.rotatedOriginX(2),2+grid.rotatedOriginY(2),8),MAT_ROCK);
}

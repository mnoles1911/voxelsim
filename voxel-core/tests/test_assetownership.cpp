#include "voxelcore/assetownership.h"
#include "voxelcore/assetfield.h"
#include "vxctest.h"
using namespace vxc;

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

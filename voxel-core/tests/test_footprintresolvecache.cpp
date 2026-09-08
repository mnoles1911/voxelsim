#include "voxelcore/footprintresolvecache.h"
#include "voxelcore/assetfield.h"
#include "voxelcore/assetbank.h"
#include "voxelcore/world.h"
#include "vxctest.h"
using namespace vxc;
VXC_TEST(footprint_resolve_cache_lru_bytes_history_and_order) {
    FootprintResolveCache<int> c(2,32,16,2,1);
    const FootprintResolveKey a{0,-1,2},b{1,-1,2},d{2,8,9};
    const std::vector<int> ordered{9,2,7};CHECK(c.put(a,ordered));
    CHECK(c.find(a)&&*c.find(a)==ordered); // canonical winner order retained
    CHECK(c.put(b,{}));CHECK(c.find(b)&&c.find(b)->empty()); // empty is valid hit
    CHECK(c.find(a));CHECK(c.put(d,{4,5}));CHECK(!c.find(b));CHECK(c.find(a));
    CHECK(c.bytes()<=32);CHECK(c.size()<=2);CHECK(c.historySize()<=2);CHECK_EQ(c.evictions,uint64_t(1));
    CHECK(!c.recentlyStored(a));CHECK(c.recentlyStored(b));CHECK_EQ(c.historyForgotten,uint64_t(1));
    const auto before=c.bytes();CHECK(!c.put({0,3,4},std::vector<int>(5,1)));CHECK_EQ(c.bytes(),before);
    std::vector<int> retainedCapacity;retainedCapacity.reserve(64);retainedCapacity.push_back(1);
    CHECK(!c.putOwned({0,3,4},std::move(retainedCapacity)));CHECK_EQ(c.bytes(),before);
    CHECK(!c.put(a,{1},false));CHECK(*c.find(a)==ordered);
    for(int i=0;i<1000;++i){CHECK(c.put({0,i,0},{i}));CHECK(c.size()<=2);CHECK(c.bytes()<=32);CHECK(c.historySize()<=2);}
}
VXC_TEST(footprint_resolve_cache_warm_epoch_and_retained_admission) {
    FootprintResolveCache<int> c(2,32,16,2,1),other(2,32,16,2,1);
    const FootprintResolveKey key{7,-12,10},next{7,-11,10};
    auto old=c.beginWarm(key);CHECK(bool(old));CHECK(!c.beginWarm(next));
    c.invalidate();CHECK(!c.beginWarm(next)); // cancelled/stale worker still occupies its slot
    CHECK(!c.complete(old,{1,2},true));CHECK_EQ(c.pendingSize(),size_t(0));CHECK(!c.find(key));
    auto fresh=c.beginWarm(key),foreign=other.beginWarm(key);CHECK(bool(fresh));CHECK(bool(foreign));
    CHECK(!c.complete(foreign,{5},true));CHECK_EQ(c.pendingSize(),size_t(1));
    CHECK(!c.complete(old,{6},true));CHECK_EQ(c.pendingSize(),size_t(1));
    CHECK(c.complete(fresh,{3,1,2},true));CHECK(*c.find(key)==std::vector<int>({3,1,2}));
    c.invalidate();auto nonresident=c.beginWarm(key);CHECK(!c.complete(nonresident,{8},false));CHECK(!c.find(key));
    auto oversized=c.beginWarm(key);CHECK(!c.complete(oversized,std::vector<int>(5,1),true));CHECK_EQ(c.pendingSize(),size_t(0));
    auto race=c.beginWarm(key);CHECK(c.put(key,{4}));CHECK(c.complete(race,{7},true));CHECK(*c.find(key)==std::vector<int>({4}));
    c.disable();CHECK(!c.find(key));CHECK(!c.beginWarm(key));CHECK(!c.put(key,{8}));
}
VXC_TEST(footprint_resolve_cache_all_layer_preflight_and_config_revisions) {
    AssetLayer layer{};layer.cellMm=100;layer.maxRadiusMm=0;
    std::vector<AssetLayer> layers{layer};
    AssetLayer tinyCells=layer;tinyCells.cellMm=1;
    CHECK(!footprintResolveSitesBound({0,0,0,0},{tinyCells})); // inclusive high edge spans 10,000 cells
    CHECK(footprintResolveSitesBound({-1,-1,1,1},layers));
    CHECK(!footprintResolveSitesBound({-100,-100,100,100},layers));
    CHECK(!footprintResolveSitesBound({INT64_MIN,0,0,0},layers));
    CHECK(!footprintResolveSitesBound({0,0,INT64_MAX,0},layers));
    CHECK(!footprintResolveSitesBound({0,0,1,1},layers,UINT64_MAX));
    layer.terrainLattice=false;layers.push_back(layer);
    CHECK(!footprintResolveSitesBound({0,0,63,63},layers,4096)); // detail sites allocated too
    AssetField field;auto rev=field.configurationRevision();field.setSeed(1);CHECK(field.configurationRevision()>rev);
    rev=field.configurationRevision();field.setSeed(1);CHECK(field.configurationRevision()>rev);
    AssetField replacement;rev=field.configurationRevision();field=replacement;CHECK(field.configurationRevision()>rev);
    AssetBankLibrary banks;rev=banks.configurationRevision();banks.configure(nullptr,"unused");CHECK(banks.configurationRevision()>rev);
    rev=banks.configurationRevision();banks.configure(nullptr,"unused");CHECK(banks.configurationRevision()>rev);
    SyntheticTileSampler tiles(1);World<16> world(1,tiles,"provider");
    rev=world.configurationRevision();world.setAssetField(&field);CHECK(world.configurationRevision()>rev);
    rev=world.configurationRevision();world.setWaterMarkerFillPx(0);CHECK(world.configurationRevision()>rev);
    rev=world.configurationRevision();world.setAssetChannelSource(nullptr);CHECK(world.configurationRevision()>rev);
}

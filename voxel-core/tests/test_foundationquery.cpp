#include "voxelcore/foundationquery.h"
#include "vxctest.h"
using namespace vxc;
namespace {
FoundationWaterSample dry(int64_t,int64_t){return {true,0};}
FoundationMaterialSample flat(int64_t,int64_t,int64_t z){return {true,z<0?MAT_ROCK:MAT_AIR};}
}
VXC_TEST(foundation_exact_negative_footprint_and_finite_depth) {
    int materialCalls=0,waterCalls=0;
    const auto r=surveyFoundation(-50,-75,0,[&](int64_t x,int64_t y,int64_t z){
        ++materialCalls;CHECK(x>=-50&&x<=-1);CHECK(y>=-75&&y<=-26);
        CHECK(z>=-10&&z<=29);return flat(x,y,z);
    },[&](int64_t,int64_t){++waterCalls;return FoundationWaterSample{true,0};});
    CHECK(r.validRequest);CHECK(r.columns.size()==2500);CHECK(waterCalls==2500);
    CHECK(materialCalls==2500*40);CHECK(r.provisionalCriteriaColumns==2500);
    CHECK(r.columns.front().x==-50);CHECK(r.columns.back().y==-26);
    CHECK(r.columns.front().fillGapMm==0);CHECK(r.columns.front().bearingThicknessMm==1000);
    CHECK(r.columns.front().firstVoidDepthMm==-1);
    // A cave below the finite scan is intentionally not certified away.
    const auto deep=surveyFoundation(0,0,0,[](int64_t x,int64_t y,int64_t z){
        return z==-11?FoundationMaterialSample{true,MAT_AIR}:flat(x,y,z);
    },dry);
    CHECK(deep.provisionalCriteriaColumns==2500);
}
VXC_TEST(foundation_edge_cave_and_live_edited_air) {
    bool edited=false;
    auto world=[&](int64_t x,int64_t y,int64_t z){
        if(x==49&&y==49&&z==-3&&edited)return FoundationMaterialSample{true,MAT_AIR};
        return flat(x,y,z);
    };
    CHECK(surveyFoundation(0,0,0,world,dry).provisionalCriteriaColumns==2500);
    edited=true;const auto r=surveyFoundation(0,0,0,world,dry);
    CHECK(r.provisionalCriteriaColumns==2499);CHECK(r.columns.back().bearingThicknessMm==200);
    CHECK(r.columns.back().firstVoidDepthMm==200);
}
VXC_TEST(foundation_fill_limit_and_foliage_not_bearing) {
    const auto fill=surveyFoundation(0,0,0,[](int64_t x,int64_t,int64_t z){
        return FoundationMaterialSample{true,z<-(x==0?6:5)?MAT_TOPSOIL:MAT_AIR};
    },dry);
    CHECK(!fill.columns.front().groundFound);CHECK(fill.columns[1].fillGapMm==500);
    CHECK(fill.provisionalCriteriaColumns==2450);
    const auto leaves=surveyFoundation(0,0,0,[](int64_t,int64_t,int64_t z){
        return FoundationMaterialSample{true,z<0?MAT_LEAF_BROADLEAF:MAT_AIR};
    },dry);
    CHECK(leaves.provisionalCriteriaColumns==0);CHECK(!leaves.columns[0].groundFound);
    CHECK(!foundationBearingMaterial(MAT_BARK));CHECK(!foundationBearingMaterial(MAT_SNOW));
}
VXC_TEST(foundation_water_edges_unknowns_and_headroom) {
    const auto r=surveyFoundation(-25,-25,0,[](int64_t x,int64_t y,int64_t z){
        if(x==-25&&y==-25&&z==29)return FoundationMaterialSample{true,MAT_LEAF_BROADLEAF};
        if(x==24&&y==24&&z==-2)return FoundationMaterialSample{false,MAT_AIR};
        return flat(x,y,z);
    },[](int64_t x,int64_t y){
        if(x==-25&&y==24)return FoundationWaterSample{false,0};
        return FoundationWaterSample{true,x==24&&y==-25?100:0};
    });
    CHECK(r.wetColumns==1);CHECK(r.unknownColumns==2);CHECK(r.obstructedColumns==1);
    CHECK(r.provisionalCriteriaColumns==2496);
    CHECK(r.columns[25*50+25].waterMm==0); // dry center does not hide wet edge
}
VXC_TEST(foundation_invalid_bounds_no_callback) {
    int calls=0;const auto r=surveyFoundation(INT64_MAX,0,0,
        [&](int64_t,int64_t,int64_t){++calls;return FoundationMaterialSample{};},dry);
    CHECK(!r.validRequest);CHECK(r.columns.empty());CHECK(calls==0);
}

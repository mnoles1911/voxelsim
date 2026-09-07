#include "voxelcore/sparseassetgrid.h"
#include "voxelcore/mips.h"
#include "vxctest.h"
#include <vector>
using namespace vxc;

VXC_TEST(sparse_asset_anchor_reduction_signed_origins) {
    // Independent oracle bins occupied source cells using mathematical floor.
    const auto half=[](int64_t v){return v/2-(v%2<0);};
    for(int origin=-3;origin<=3;++origin){
        SparseAssetGrid g({5,5,5,origin,origin-1,origin+1,25000});
        std::vector<SparseAssetGrid::Edit> edits;
        std::map<std::tuple<int64_t,int64_t,int64_t>,std::array<int,256>> votes;
        for(int z=0;z<5;++z)for(int y=0;y<5;++y)for(int x=0;x<5;++x){
            const uint8_t m=uint8_t((x+2*y+3*z)%5);
            edits.push_back({x,y,z,m});
            if(m)++votes[{half(x+origin),half(y+origin-1),half(z+origin+1)}][m];
        }
        CHECK(g.apply(edits)==SparseAssetGrid::Result::Ok);
        for(int z=-4;z<=5;++z)for(int y=-4;y<=5;++y)for(int x=-4;x<=5;++x){
            const auto& counts=votes[{x,y,z}];int expected=0;
            for(int m=1;m<256;++m)if(counts[m]>counts[expected])expected=m;
            CHECK_EQ(g.environmentReducedAtAnchor2(x,y,z),expected);
        }
    }
    SparseAssetGrid edge({1,1,1,INT64_MAX,INT64_MIN,0,25000});
    const SparseAssetGrid::Edit cell[]={{0,0,0,9}};
    CHECK(edge.apply(cell)==SparseAssetGrid::Result::Ok);
    CHECK_EQ(edge.environmentReducedAtAnchor2(INT64_MAX/2,INT64_MIN/2,0),9);
    CHECK_EQ(edge.environmentReducedAtAnchor2(INT64_MAX,0,0),0);
    CHECK_EQ(edge.environmentReducedAtAnchor2(INT64_MIN,0,0),0);
}

VXC_TEST(sparse_asset_large_bounds_negative_origin) {
    SparseAssetGrid g({1000000,1000000,1000000,-500000,-1,-900000,12500},{2,2});
    const SparseAssetGrid::Edit edits[]={{0,0,0,7},{999999,999999,999999,9}};
    CHECK(g.apply(edits)==SparseAssetGrid::Result::Ok);
    CHECK_EQ(g.residentPayloadBytes(),1024u);CHECK_EQ(g.atOriginRelative(-500000,-1,-900000),7);
    std::vector<SparseAssetGrid::Edit> occupied;g.visitOccupiedCells([&](auto e){occupied.push_back(e);});
    CHECK_EQ(occupied.size(),2u);CHECK_EQ(occupied[0].x,0);CHECK_EQ(occupied[1].x,999999);
    CHECK_EQ(g.at(999999,999999,999999),9);CHECK_EQ(g.at(-1,0,0),0);
    CHECK_EQ(g.atOriginRelative(INT64_MAX,INT64_MIN,0),0);
    SparseAssetGrid edge({1,1,1,INT64_MAX,INT64_MIN,0,100000});
    const SparseAssetGrid::Edit one[]={{0,0,0,6}};CHECK(edge.apply(one)==SparseAssetGrid::Result::Ok);
    CHECK_EQ(edge.atOriginRelative(INT64_MAX,INT64_MIN,0),6);
}
VXC_TEST(sparse_asset_runs_cross_chunk_and_clear) {
    SparseAssetGrid g({20,20,100,0,0,0,50000});
    const SparseAssetGrid::Run runs[]={{2,3,6,20,4},{2,3,12,2,0},{2,3,90,3,8}};
    CHECK(g.applyRuns(runs)==SparseAssetGrid::Result::Ok);
    std::vector<SparseAssetGrid::Run> out;g.visitColumnRuns(2,3,[&](auto r){out.push_back(r);});
    CHECK_EQ(out.size(),3u);CHECK_EQ(out[0].z,6);CHECK_EQ(out[0].length,6);
    CHECK_EQ(out[1].z,14);CHECK_EQ(out[1].length,12);CHECK_EQ(out[2].z,90);
    const SparseAssetGrid::Run clear[]={{2,3,0,100,0}};
    CHECK(g.applyRuns(clear)==SparseAssetGrid::Result::Ok);CHECK_EQ(g.chunkCount(),0u);
}
VXC_TEST(sparse_asset_budget_failure_is_atomic) {
    SparseAssetGrid g({32,32,32,0,0,0,100000},{1,2});
    const SparseAssetGrid::Edit first[]={{0,0,0,2}};CHECK(g.apply(first)==SparseAssetGrid::Result::Ok);
    const SparseAssetGrid::Edit over[]={{0,0,0,3},{16,0,0,4}};
    CHECK(g.apply(over)==SparseAssetGrid::Result::BudgetExceeded);CHECK_EQ(g.at(0,0,0),2);CHECK_EQ(g.at(16,0,0),0);
    const SparseAssetGrid::Edit scratch[]={{0,0,0,3},{8,0,0,4},{16,0,0,5}};
    CHECK(g.apply(scratch)==SparseAssetGrid::Result::BudgetExceeded);CHECK_EQ(g.at(0,0,0),2);
    const SparseAssetGrid::Edit invalid[]={{0,0,0,9},{32,0,0,1}};
    CHECK(g.apply(invalid)==SparseAssetGrid::Result::InvalidRange);CHECK_EQ(g.at(0,0,0),2);
    const SparseAssetGrid::Edit replace[]={{0,0,0,0},{16,0,0,4}};
    CHECK(g.apply(replace)==SparseAssetGrid::Result::Ok);CHECK_EQ(g.chunkCount(),1u);
}
VXC_TEST(sparse_asset_box_and_upper_reclamation) {
    SparseAssetGrid g({1000,1000,1000,-3,-5,-7,25000});
    const SparseAssetGrid::Run run[]={{3,4,6,20,16}};CHECK(g.applyRuns(run)==SparseAssetGrid::Result::Ok);
    int count=0;g.visitOccupiedBox(0,0,7,8,8,10,[&](auto){++count;});CHECK_EQ(count,3);
    g.clearAbove(9);CHECK_EQ(g.at(3,4,8),16);CHECK_EQ(g.at(3,4,9),0);CHECK_EQ(g.chunkCount(),2u);
    g.clearAbove(0);CHECK_EQ(g.chunkCount(),0u);
}
VXC_TEST(sparse_asset_reduction_matches_dense_mips) {
    SparseAssetGrid g({8,8,8,0,0,0,25000});Brick<8> dense;
    std::vector<SparseAssetGrid::Edit> edits;
    for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){
        const auto m=static_cast<MaterialId>((x+3*y+5*z)%10);dense.set(x,y,z,m);edits.push_back({x,y,z,uint8_t(m)});
    }
    CHECK(g.apply(edits)==SparseAssetGrid::Result::Ok);
    const Brick<8>* children[8]={&dense};
    for(int threshold=1;threshold<=8;++threshold)for(bool surface:{false,true}){
        const auto reduced=downsampleBricks<8>(children,threshold,surface);
        for(int z=0;z<4;++z)for(int y=0;y<4;++y)for(int x=0;x<4;++x)
            CHECK_EQ(g.reducedAt2(x,y,z,threshold,surface),reduced.get(x,y,z));
    }
    SparseAssetGrid twig({3,3,3,0,0,0,12500});const SparseAssetGrid::Edit thin[]={{2,2,2,8}};
    CHECK(twig.apply(thin)==SparseAssetGrid::Result::Ok);
    CHECK_EQ(twig.environmentReducedAt2(1,1,1),8);CHECK_EQ(twig.reducedAt2(1,1,1),0);
}

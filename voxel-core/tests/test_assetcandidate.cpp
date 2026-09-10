#include "voxelcore/assetcandidate.h"
#include "vxctest.h"
using namespace vxc;
namespace {
AssetGrid block(MaterialId material) {
    std::vector<uint8_t> bytes;
    auto word=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back(uint8_t(n>>(8*i)));};
    for(uint32_t n:{kVxaMagic,3u,0u,0u,0u,2u,1u,1u,100u,1u,0u,0u})word(n);
    bytes.push_back(material);word(2);AssetGrid grid;
    CHECK(grid.parse(bytes.data(),bytes.size())==AssetParseError::kOk);return grid;
}
}
VXC_TEST(asset_candidate_preserves_overlap_and_rotated_voxel_bounds) {
    auto rock=block(MAT_ROCK),wood=block(MaterialId(16));
    AssetField::ResolvedAssetInstance a;a.grid=&rock;a.anchorVz=100;a.bankId=1;
    auto b=a;b.grid=&wood;b.bankId=2;b.anchorVx=1;
    std::vector<AssetField::ResolvedAssetInstance> ordered{a,b};
    CHECK_EQ(assetTerrainRenderMaterial(ordered,1,0,100,[](const auto& r){return r.bankId==1;}),MAT_AIR);
    CHECK_EQ(assetTerrainRenderMaterial(ordered,2,0,100,[](const auto& r){return r.bankId==1;}),16);
    std::vector<uint8_t> bytes;
    auto air=[](int64_t,int64_t){return ColumnSample{};};
    CHECK(assetBuildCandidateVxa(ordered,1,air,bytes));AssetGrid clipped;
    CHECK(clipped.parse(bytes.data(),bytes.size())==AssetParseError::kOk);
    CHECK_EQ(clipped.at(0,0,0),MAT_AIR);CHECK_EQ(clipped.at(1,0,0),16);
    CHECK(!assetBuildCandidateVxa(ordered,1,air,bytes,1));CHECK(bytes.empty());
    for(uint8_t yaw=0;yaw<4;++yaw){
        b.yawQuarter=yaw;ordered={b};AssetCandidateBounds bounds;CHECK(assetCandidateBounds(b,bounds));
        CHECK(assetBuildCandidateVxa(ordered,0,air,bytes));AssetGrid rotated;
        CHECK(rotated.parse(bytes.data(),bytes.size())==AssetParseError::kOk);
        CHECK_EQ(rotated.sizeX(),wood.rotatedSizeX(yaw));CHECK_EQ(rotated.sizeY(),wood.rotatedSizeY(yaw));
        CHECK_EQ(assetCandidateMaterial(b,bounds,bounds.x0,bounds.y0,100),16);
    }
}
VXC_TEST(asset_candidate_page_aprons_cover_negative_boundaries) {
    AssetCandidateBounds b{-1,0,0,-1,0,0};
    CHECK(assetPageTouches(b,{0,0,0,0,AssetCpu}));
    CHECK(assetPageTouches(b,{-1,0,0,0,AssetCpu}));
    CHECK(!assetPageTouches(b,{1,0,0,0,AssetCpu}));
    CHECK(assetPageTouches(b,{0,0,0,3,AssetGpu}));
}

VXC_TEST(asset_render_request_marker_preserves_authoritative_composition) {
    auto first=block(MAT_ROCK),second=block(MaterialId(16));
    for(uint8_t yaw=0;yaw<4;++yaw) {
        AssetField::ResolvedAssetInstance a;
        a.grid=&first;a.anchorVx=-100;a.anchorVy=-50;a.anchorVz=100;a.yawQuarter=yaw;
        auto b=a;b.grid=&second;
        std::vector<AssetField::ResolvedAssetInstance> ordered{a,b};
        AssetCandidateBounds bounds;CHECK(assetCandidateBounds(a,bounds));
        for(int64_t x=bounds.x0-1;x<=bounds.x1+1;++x)
        for(int64_t y=bounds.y0-1;y<=bounds.y1+1;++y) {
            const auto original=AssetField::materialAtResolved(ordered,x,y,100);
            CHECK_EQ(AssetField::materialAtResolvedForRender(ordered,x,y,100),original);
            ordered[0].suppressTerrainRender=true;
            CHECK_EQ(AssetField::materialAtResolved(ordered,x,y,100),original);
            CHECK_EQ(AssetField::materialAtResolvedForRender(ordered,x,y,100),MAT_AIR);
            // Suppressing only the later instance cannot erase the earlier winner.
            ordered[0].suppressTerrainRender=false;ordered[1].suppressTerrainRender=true;
            CHECK_EQ(AssetField::materialAtResolvedForRender(ordered,x,y,100),original);
            ordered[1].suppressTerrainRender=false;
        }
        // A later instance remains visible where the owned earlier box is air.
        ordered[0].suppressTerrainRender=true;
        ordered[1].anchorVx+=10;
        CHECK(assetCandidateBounds(ordered[1],bounds));
        CHECK_EQ(AssetField::materialAtResolvedForRender(ordered,bounds.x0,bounds.y0,100),16);
    }
}

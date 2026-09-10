#include "voxelcore/assetappearancepage.h"
#include "voxelcore/assetappearancepack.h"
#include "voxelcore/assetrequestbounds.h"
#include <bit>
#include "vxctest.h"
using namespace vxc;
namespace {
AssetGrid fixture(MaterialId material,bool holes=false){
    std::vector<uint8_t> b;auto put=[&](uint32_t v){for(int i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));};
    put(kVxaMagic);put(kVxaVersion);put(uint32_t(-4));put(uint32_t(-7));put(uint32_t(-2));
    put(2);put(3);put(2);put(100);put(12);put(0);put(0);
    for(int i=0;i<12;++i){b.push_back(uint8_t(holes&&i%3==0?MaterialId(MAT_AIR):material));put(1);}
    AssetGrid g;CHECK(g.parse(b)==AssetParseError::kOk);return g;
}
AssetField::ResolvedAssetInstance instance(const AssetGrid& g,uint8_t yaw){
    AssetField::ResolvedAssetInstance r;r.grid=&g;r.anchorVx=-16;r.anchorVy=-16;r.anchorVz=-16;r.yawQuarter=yaw;return r;
}
}
VXC_TEST(asset_request_overlap_matches_rotated_source_cells){
    auto grid=fixture(MAT_BARK);
    for(uint32_t level=0;level<=30;++level)for(uint8_t yaw=0;yaw<4;++yaw)
    for(int origin:{-48,0,48})for(int axis=0;axis<3;++axis)for(int edge:{-1,0,47,48}){
        const int64_t scale=int64_t(1)<<level;
        auto r=instance(grid,yaw);
        r.anchorVx=(origin+20)*scale-grid.rotatedOriginX(yaw);
        r.anchorVy=(origin+20)*scale-grid.rotatedOriginY(yaw);
        r.anchorVz=(origin+20)*scale-grid.originZ();
        if(axis==0)r.anchorVx=(origin+edge)*scale-grid.rotatedOriginX(yaw);
        if(axis==1)r.anchorVy=(origin+edge)*scale-grid.rotatedOriginY(yaw);
        if(axis==2)r.anchorVz=(origin+edge)*scale-grid.originZ();
        bool expected=false;
        // Independently rotate every source voxel; any cell inside the full
        // 48-cell request (including its halo) prevents pruning.
        for(int x=0;x<grid.sizeX();++x)for(int y=0;y<grid.sizeY();++y)for(int z=0;z<grid.sizeZ();++z){
            int64_t sx=x+grid.originX(),sy=y+grid.originY(),rx=sx,ry=sy;
            if(yaw==1){rx=-sy;ry=sx;}else if(yaw==2){rx=-sx;ry=-sy;}else if(yaw==3){rx=sy;ry=-sx;}
            const int64_t p[3]={floorDiv(r.anchorVx+rx,scale),floorDiv(r.anchorVy+ry,scale),floorDiv(r.anchorVz+z+grid.originZ(),scale)};
            expected|=p[0]>=origin&&p[0]<origin+48&&p[1]>=origin&&p[1]<origin+48&&p[2]>=origin&&p[2]<origin+48;
        }
        CHECK_EQ(assetMayContributeToRequest(r,origin,origin,origin,48,48,48,level),expected);
    }
    auto r=instance(grid,0);
    CHECK(assetMayContributeToRequest(r,0,0,0,0,48,48,0));
    CHECK(assetMayContributeToRequest(r,INT64_MAX,0,0,48,48,48,0));
    CHECK(assetMayContributeToRequest(r,0,0,0,48,48,48,31));
    CHECK(assetMayContributeToRequest(r,0,0,0,UINT64_MAX,48,48,0));
    r.grid=nullptr;CHECK(assetMayContributeToRequest(r,0,0,0,48,48,48,0));
}
VXC_TEST(assetappearance_page_canonical_winner_terrain_touched_unapproved_all_yaws){
    auto first=fixture(MAT_BARK,true),second=fixture(MAT_SAND);
    for(uint8_t yaw=0;yaw<4;++yaw)for(bool approvedFirst:{false,true}){
        const std::vector<AssetField::ResolvedAssetInstance> ordered={instance(first,yaw),instance(second,yaw)};
        const std::vector<uint32_t> ids={approvedFirst?41u:0u,73u};
        // Independently forward-rotate source lattice indices to select actual
        // first-winner, later-winner, terrain-occluded and explicitly edited cells.
        std::vector<AssetAppearanceCell> records;
        int terrainCalls=0;
        const auto terrain=[&](int64_t,int64_t,int64_t z){++terrainCalls;return z==-18?MAT_ROCK:MAT_AIR;};
        const auto touched=[&](int64_t x,int64_t y,int64_t){return x==-20&&y==-23;};
        CHECK(assetAppearancePage(-1,-1,-1,ordered,ids,terrain,touched,records));
        CHECK_EQ(terrainCalls,12); // approved 2x3x2 box, not all 32768 page cells
        int approved=0,unapproved=0,later=0,ground=0;
        for(int x=0;x<2;++x)for(int y=0;y<3;++y)for(int z=0;z<2;++z){
            const int sx=x-4,sy=y-7,sz=z-2;int rx=sx,ry=sy;
            if(yaw==1){rx=-sy;ry=sx;}else if(yaw==2){rx=-sx;ry=-sy;}else if(yaw==3){rx=sy;ry=-sx;}
            const int64_t wx=-16+rx,wy=-16+ry,wz=-16+sz;
            const uint16_t cell=uint16_t((wx+32)+32*((wy+32)+32*(wz+32)));
            const auto found=std::find_if(records.begin(),records.end(),[&](const auto& c){return c.cell==cell;});
            const auto material=AssetField::materialAtResolved(ordered,wx,wy,wz);
            const bool firstWins=first.at(x,y,z)!=MAT_AIR;
            const uint32_t expectedId=ids[firstWins?0:1];
            if(terrain(wx,wy,wz)!=MAT_AIR){++ground;CHECK(found==records.end());continue;}
            if(touched(wx,wy,wz)){CHECK(found==records.end());continue;}
            if(!expectedId){++unapproved;CHECK(found==records.end());continue;}
            CHECK(found!=records.end());if(found==records.end())continue;
            ++approved;if(!firstWins)++later;
            CHECK_EQ(found->resource,expectedId);CHECK_EQ(found->instance,firstWins?0u:1u);
            CHECK_EQ(int(found->material),int(material));CHECK_EQ(found->sourceX,sx);CHECK_EQ(found->sourceY,sy);CHECK_EQ(found->sourceZ,sz);CHECK_EQ(found->yaw,yaw);
        }
        CHECK_EQ(records.size(),size_t(approved));CHECK(approved>0);CHECK(later>0);CHECK(ground>0);if(!approvedFirst)CHECK(unapproved>0);
        for(size_t i=1;i<records.size();++i)CHECK(records[i-1].cell<records[i].cell);
    }
}
VXC_TEST(assetappearance_page_no_appearance_allocations_and_failure_atomicity){
    auto grid=fixture(MAT_BARK);std::vector<AssetField::ResolvedAssetInstance> ordered={instance(grid,0)};
    const auto air=[](int64_t,int64_t,int64_t){return MAT_AIR;};
    const auto untouched=[](int64_t,int64_t,int64_t){return false;};
    std::vector<AssetAppearanceCell> out;
    CHECK(assetAppearancePage(-1,-1,-1,ordered,{0},air,untouched,out));CHECK(out.empty());CHECK_EQ(out.capacity(),size_t(0));
    CHECK(assetAppearancePage(5,6,7,ordered,{1},air,untouched,out));CHECK(out.empty());CHECK_EQ(out.capacity(),size_t(0));
    CHECK(assetAppearancePage(-1,-1,-1,ordered,{1},air,[](int64_t,int64_t,int64_t){return true;},out));CHECK(out.empty());CHECK_EQ(out.capacity(),size_t(0));
    CHECK(assetAppearancePage(-1,-1,-1,ordered,{1},air,untouched,out));CHECK(!out.empty());const auto count=out.size();const auto resource=out[0].resource;
    CHECK(!assetAppearancePage(-1,-1,-1,ordered,{},air,untouched,out));CHECK_EQ(out.size(),count);CHECK_EQ(out[0].resource,resource);
    ordered[0].yawQuarter=4;CHECK(!assetAppearancePage(-1,-1,-1,ordered,{1},air,untouched,out));CHECK_EQ(out.size(),count);
}

VXC_TEST(assetappearance_bounded_scan_matches_full_page_at_edges){
    auto grid=fixture(MAT_BARK);
    for(uint8_t level=0;level<=7;++level)for(uint8_t yaw:{uint8_t(0),uint8_t(1)})
    for(int page:{-1,0})for(int edge:{-1,0,31,32}){
        const int64_t scale=int64_t(1)<<level,half=level?scale/2:0;
        const int64_t origin=int64_t(page)*32*scale;
        auto placed=instance(grid,yaw);
        placed.anchorVx=origin+edge*scale+half-grid.rotatedOriginX(yaw);
        placed.anchorVy=origin+15*scale+half-grid.rotatedOriginY(yaw);
        placed.anchorVz=origin+edge*scale+half-grid.originZ();
        const std::vector<AssetField::ResolvedAssetInstance> ordered{placed};
        std::vector<AssetAppearanceCell> cells;int terrainCalls=0;
        CHECK(assetAppearancePage(page,page,page,ordered,{77},
            [&](auto,auto,auto){++terrainCalls;return MAT_AIR;},
            [](auto,auto,auto){return false;},cells,level));
        size_t expected=0;
        for(int z=0;z<32;++z)for(int y=0;y<32;++y)for(int x=0;x<32;++x){
            const auto material=AssetField::materialAtResolved(ordered,
                origin+x*scale+half,origin+y*scale+half,origin+z*scale+half);
            if(material==MAT_AIR)continue;
            CHECK(expected<cells.size());const auto& cell=cells[expected++];
            CHECK_EQ(cell.cell,uint16_t(x+32*(y+32*z)));CHECK_EQ(cell.material,material);
        }
        CHECK_EQ(cells.size(),expected);
        CHECK_EQ(size_t(terrainCalls),expected); // solid fixture has no internal holes
    }
}

VXC_TEST(assetappearance_sparse_gaps_do_not_sample_terrain){
    auto grid=fixture(MAT_LEAF_BROADLEAF,true);
    std::vector<AssetField::ResolvedAssetInstance> ordered{instance(grid,0)};
    std::vector<AssetAppearanceCell> cells;int terrainCalls=0,touchedCalls=0;
    CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},
        [&](auto,auto,auto){++terrainCalls;return MAT_AIR;},
        [&](auto,auto,auto){++touchedCalls;return false;},cells));
    CHECK_EQ(cells.size(),size_t(8));CHECK_EQ(terrainCalls,8);CHECK_EQ(touchedCalls,8);
}

VXC_TEST(assetappearance_pack_sparse_layout_source_mapping_and_refusals){
    auto grid=fixture(MAT_BARK);
    for(uint8_t yaw=0;yaw<4;++yaw){
        const std::vector<AssetField::ResolvedAssetInstance> ordered={instance(grid,yaw)};
        std::vector<AssetAppearanceCell> cells;
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},[](auto,auto,auto){return MAT_AIR;},[](auto,auto,auto){return false;},cells));
        std::vector<uint32_t> words;
        CHECK(assetPackAppearancePage(-1,-1,-1,0x123456789ull,ordered,cells,words));
        CHECK_EQ(words[0],1u);CHECK_EQ(words[1],uint32_t(words.size()));CHECK_EQ(words[6],1u);
        CHECK_EQ(words[7],uint32_t(cells.size()));CHECK_EQ(words[13],0x23456789u);CHECK_EQ(words[14],1u);
        // Decode every world cell independently, including unmarked cells.
        for(uint32_t z=0;z<32;++z)for(uint32_t y=0;y<32;++y)for(uint32_t x=0;x<32;++x){
            const uint32_t brick=((x>>3)+4*(y>>3))*4+(z>>3),bit=(x&7)+8*(y&7)+64*(z&7);
            const auto expected=AssetField::materialAtResolved(ordered,int64_t(x)-32,int64_t(y)-32,int64_t(z)-32);
            const uint32_t block=words[words[8]+brick];uint32_t handle=0;
            if(block){const uint32_t base=words[9]+17*(block-1),mask=words[base+1+bit/32];
                if(mask&(1u<<(bit%32))){uint32_t rank=0;for(uint32_t i=0;i<bit/32;++i)rank+=std::popcount(words[base+1+i]);
                    rank+=std::popcount(mask&((1u<<(bit%32))-1));const uint32_t index=words[base]+rank;
                    handle=(words[words[11]+index/2]>>((index%2)*16))&65535;}}
            CHECK_EQ(handle!=0,expected!=MAT_AIR);
            if(handle){const uint32_t base=words[10]+8*(handle-1);CHECK_EQ(words[base],77u);CHECK_EQ(words[base+4],uint32_t(yaw));
                CHECK_EQ(int32_t(words[base+1]),16);CHECK_EQ(int32_t(words[base+2]),16);CHECK_EQ(int32_t(words[base+3]),16);}
        }
        const auto before=words;auto corrupt=cells;corrupt.push_back(cells[0]);
        CHECK(!assetPackAppearancePage(-1,-1,-1,1,ordered,corrupt,words));CHECK(words==before);
        corrupt=cells;corrupt[0].sourceX++;CHECK(!assetPackAppearancePage(-1,-1,-1,1,ordered,corrupt,words));CHECK(words==before);
        corrupt=cells;corrupt[0].resource++;CHECK(!assetPackAppearancePage(-1,-1,-1,1,ordered,corrupt,words));CHECK(words==before);
        CHECK(!assetPackAppearancePage(-1,-1,-1,0,ordered,cells,words));CHECK(words==before);
        auto odd=cells;odd.pop_back();
        CHECK(assetPackAppearancePage(-1,-1,-1,1,ordered,odd,words));
        CHECK_EQ(words[7],uint32_t(odd.size()));CHECK_EQ(words.back()>>16,0u);
        CHECK(assetPackAppearancePage(-1,-1,-1,1,ordered,{},words));CHECK(words.empty());CHECK_EQ(words.capacity(),size_t(0));
    }
}

VXC_TEST(assetappearance_coarse_representative_matches_material_all_levels_yaws){
    auto grid=fixture(MAT_BARK);
    for(uint8_t level=1;level<=7;++level)for(uint8_t yaw=0;yaw<4;++yaw){
        const int64_t scale=int64_t(1)<<level,rep=-3*scale+scale/2;
        int64_t rx=-4,ry=-7;
        if(yaw==1){rx=7;ry=-4;}else if(yaw==2){rx=4;ry=7;}else if(yaw==3){rx=-7;ry=4;}
        auto placed=instance(grid,yaw);placed.anchorVx=rep-rx;placed.anchorVy=rep-ry;placed.anchorVz=rep+2;
        std::vector<AssetField::ResolvedAssetInstance> ordered{placed};
        std::vector<AssetAppearanceCell> cells;
        int terrainCalls=0;
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},[&](auto,auto,auto){++terrainCalls;return MAT_AIR;},[](auto,auto,auto){return false;},cells,level));
        CHECK(terrainCalls>0);CHECK(terrainCalls<=12);
        CHECK(!cells.empty());
        size_t expected=0;
        for(int z=0;z<32;++z)for(int y=0;y<32;++y)for(int x=0;x<32;++x){
            const int64_t wx=(int64_t(x)-32)*scale+scale/2,wy=(int64_t(y)-32)*scale+scale/2,wz=(int64_t(z)-32)*scale+scale/2;
            const auto material=AssetField::materialAtResolved(ordered,wx,wy,wz);
            if(material==MAT_AIR)continue;
            CHECK(expected<cells.size());const auto& c=cells[expected++];
            CHECK_EQ(c.cell,uint16_t(x+32*(y+32*z)));CHECK_EQ(c.material,material);
            int64_t sx=wx-placed.anchorVx,sy=wy-placed.anchorVy;
            if(yaw==1){const auto t=sx;sx=sy;sy=-t;}else if(yaw==2){sx=-sx;sy=-sy;}else if(yaw==3){const auto t=sx;sx=-sy;sy=t;}
            CHECK_EQ(c.sourceX,sx);CHECK_EQ(c.sourceY,sy);CHECK_EQ(c.sourceZ,wz-placed.anchorVz);
        }
        CHECK_EQ(cells.size(),expected);
        std::vector<uint32_t> words;CHECK(assetPackAppearancePage(-1,-1,-1,9,ordered,cells,words,level));
        CHECK_EQ(words[0],2u);CHECK_EQ(words[5],uint32_t(level));
        CHECK_EQ(int32_t(words[words[10]+1]),placed.anchorVx+32*scale);
        const auto before=words;
        CHECK(!assetPackAppearancePage(-1,-1,-1,9,ordered,cells,words,8));CHECK(words==before);
    }
}


VXC_TEST(assetappearance_recursive_offsets_preserve_selected_source){
    auto grid=fixture(MAT_BARK);
    for(uint8_t level=1;level<=7;++level)for(uint8_t yaw=0;yaw<4;++yaw){
        const int scale=1<<level,half=scale/2;
        std::vector<AssetField::ResolvedAssetInstance> ordered;
        std::vector<AssetAppearanceCell> cells;
        // Deliberately reverse brick/rank input order. Each instance places the
        // same known source voxel at a different contributing child coordinate.
        for(int n=0;n<3;++n){
            const int x=25-8*n,y=24-8*n,z=23-8*n;
            const int ox=n==0?-half:(n==1?scale-half-1:0);
            const int oy=n==0?scale-half-1:(n==1?-half:0);
            const int oz=n==2?0:-half;
            int rx=-4,ry=-7;
            if(yaw==1){rx=7;ry=-4;}else if(yaw==2){rx=4;ry=7;}else if(yaw==3){rx=-7;ry=4;}
            auto placed=instance(grid,yaw);
            placed.anchorVx=int64_t(x-32)*scale+half+ox-rx;
            placed.anchorVy=int64_t(y-64)*scale+half+oy-ry;
            placed.anchorVz=int64_t(z-96)*scale+half+oz+2;
            ordered.push_back(placed);
            AssetAppearanceCell c;
            c.cell=uint16_t(x+32*(y+32*z));c.instance=n;c.resource=77+n;
            c.sourceX=-4;c.sourceY=-7;c.sourceZ=-2;c.yaw=yaw;c.material=MAT_BARK;
            c.offsetX=int8_t(ox);c.offsetY=int8_t(oy);c.offsetZ=int8_t(oz);cells.push_back(c);
        }
        std::vector<uint32_t> words;
        CHECK(assetPackAppearancePage(-1,-2,-3,17,ordered,cells,words,level));
        CHECK_EQ(words[0],3u);CHECK_EQ(words[5],uint32_t(level));
        CHECK_EQ(words[15]+cells.size(),words.size());
        for(const auto& c:cells){
            const uint32_t x=c.cell%32,y=(c.cell/32)%32,z=c.cell/1024;
            const uint32_t brick=((x/8)+4*(y/8))*4+z/8,bit=x%8+8*(y%8)+64*(z%8);
            const uint32_t block=words[words[8]+brick];CHECK(block!=0);
            const uint32_t base=words[9]+17*(block-1),mask=words[base+1+bit/32];
            CHECK((mask&(1u<<(bit%32)))!=0);
            uint32_t rank=words[base];
            for(uint32_t j=0;j<bit/32;++j)rank+=std::popcount(words[base+1+j]);
            rank+=std::popcount(mask&((1u<<(bit%32))-1));
            const uint32_t packed=words[words[15]+rank];
            const auto signedByte=[](uint32_t v){return (v&128u)?int(v&255u)-256:int(v&255u);};
            CHECK_EQ(signedByte(packed),int(c.offsetX));
            CHECK_EQ(signedByte(packed>>8),int(c.offsetY));
            CHECK_EQ(signedByte(packed>>16),int(c.offsetZ));CHECK_EQ(packed>>24,0u);
            const uint32_t handle=(words[words[11]+rank/2]>>((rank%2)*16))&65535;
            CHECK(handle>0);const uint32_t ib=words[10]+8*(handle-1);
            CHECK_EQ(words[ib],c.resource);CHECK_EQ(words[ib+4],uint32_t(yaw));
            CHECK_EQ(int32_t(words[ib+1]),ordered[c.instance].anchorVx+32*scale);
        }
        const auto before=words;
        auto bad=cells;bad[0].offsetX=int8_t(-half-1);
        CHECK(!assetPackAppearancePage(-1,-2,-3,17,ordered,bad,words,level));CHECK(words==before);
        bad=cells;bad[0].offsetX=int8_t(scale-half);
        CHECK(!assetPackAppearancePage(-1,-2,-3,17,ordered,bad,words,level));CHECK(words==before);
        bad=cells;bad[0].sourceX++;
        CHECK(!assetPackAppearancePage(-1,-2,-3,17,ordered,bad,words,level));CHECK(words==before);
        // A zero-offset selection retains the old compact version2 layout.
        CHECK(assetPackAppearancePage(-1,-2,-3,17,ordered,{cells[2]},words,level));
        CHECK_EQ(words[0],2u);CHECK_EQ(words[15],0u);
    }
}

VXC_TEST(assetappearance_trace_selected_child_terrain_edits_and_refusal){
    auto grid=fixture(MAT_BARK);
    for(uint8_t level=1;level<=7;++level)for(uint8_t yaw=0;yaw<4;++yaw){
        const int64_t scale=int64_t(1)<<level,chosen=-3*scale;
        int rx=-4,ry=-7;
        if(yaw==1){rx=7;ry=-4;}else if(yaw==2){rx=4;ry=7;}else if(yaw==3){rx=-7;ry=4;}
        auto placed=instance(grid,yaw);placed.anchorVx=chosen-rx;placed.anchorVy=chosen-ry;placed.anchorVz=chosen+2;
        std::vector<AssetField::ResolvedAssetInstance> ordered{placed};
        AssetAppearanceTrace trace=[&](int64_t x,int64_t y,int64_t z,int64_t& fx,int64_t& fy,int64_t& fz,MaterialId& m){
            m=(x==-3&&y==-3&&z==-3)?MAT_BARK:MAT_AIR;fx=fy=fz=chosen;return true;
        };
        auto air=[](auto,auto,auto){return MAT_AIR;};auto clean=[](auto,auto,auto){return false;};
        std::vector<AssetAppearanceCell> cells;
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},air,clean,cells,level,trace));
        CHECK_EQ(cells.size(),size_t(1));if(cells.empty())continue;
        CHECK_EQ(cells[0].sourceX,-4);CHECK_EQ(cells[0].sourceY,-7);CHECK_EQ(cells[0].sourceZ,-2);
        CHECK_EQ(int(cells[0].offsetX),-scale/2);CHECK_EQ(int(cells[0].offsetY),-scale/2);CHECK_EQ(int(cells[0].offsetZ),-scale/2);
        std::vector<uint32_t> words;CHECK(assetPackAppearancePage(-1,-1,-1,1,ordered,cells,words,level));CHECK_EQ(words[0],3u);
        const auto before=cells;
        AssetAppearanceTrace failed=[](auto,auto,auto,auto&,auto&,auto&,auto&){return false;};
        CHECK(!assetAppearancePage(-1,-1,-1,ordered,{77},air,clean,cells,level,failed));CHECK_EQ(cells.size(),before.size());
        AssetAppearanceTrace outside=[&](auto x,auto y,auto z,auto& fx,auto& fy,auto& fz,auto& m){trace(x,y,z,fx,fy,fz,m);fx-=1;return true;};
        CHECK(!assetAppearancePage(-1,-1,-1,ordered,{77},air,clean,cells,level,outside));CHECK_EQ(cells[0].sourceX,before[0].sourceX);
        AssetAppearanceTrace wrong=[&](auto x,auto y,auto z,auto& fx,auto& fy,auto& fz,auto& m){trace(x,y,z,fx,fy,fz,m);if(m!=MAT_AIR)m=MAT_ROCK;return true;};
        CHECK(!assetAppearancePage(-1,-1,-1,ordered,{77},air,clean,cells,level,wrong));
        auto touched=[&](auto x,auto y,auto z){return x==chosen&&y==chosen&&z==chosen;};
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},air,touched,cells,level,trace));CHECK(cells.empty());
        auto ground=[&](auto x,auto y,auto z){return touched(x,y,z)?MAT_ROCK:MAT_AIR;};
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{77},ground,clean,cells,level,trace));CHECK(cells.empty());
        // Earlier unapproved geometry retains ownership of the selected child.
        ordered.push_back(placed);
        CHECK(assetAppearancePage(-1,-1,-1,ordered,{0,77},air,clean,cells,level,trace));CHECK(cells.empty());
    }
}

// Compare to the frozen complete pre-index implementation, not a second bin
// algorithm. Compare all records, packed GPU words and callback order.
#include "assetappearancepage_reference.h"
namespace {
void pageIndexParity(int32_t page,uint8_t level,
    const std::vector<AssetField::ResolvedAssetInstance>& ordered,const std::vector<uint32_t>& ids,
    int traceMode=0) {
    std::vector<AssetAppearanceCell> expected(1),actual(1);
    expected[0].resource=actual[0].resource=999;
    std::vector<std::array<int64_t,4>> callbacks[2];
    bool ok[2]={};
    for(int run=0;run<2;++run){
        const auto terrain=[&](int64_t x,int64_t y,int64_t z){callbacks[run].push_back({0,x,y,z});return floorMod(x+y+z,int64_t(7))==0?MAT_ROCK:MAT_AIR;};
        const auto touched=[&](int64_t x,int64_t y,int64_t z){callbacks[run].push_back({1,x,y,z});return floorMod(x-y+z,int64_t(11))==0;};
        AssetAppearanceTrace trace;
        if(traceMode)trace=[&](int64_t x,int64_t y,int64_t z,int64_t& wx,int64_t& wy,int64_t& wz,MaterialId& mat){
            callbacks[run].push_back({2,x,y,z});const int64_t scale=int64_t(1)<<level;
            wx=x*scale;wy=y*scale;wz=z*scale;
            mat=AssetField::materialAtResolved(ordered,wx,wy,wz);
            if(traceMode==2)return false; // unknown provenance
            if(traceMode==3){wx=(x+1)*scale;mat=MAT_BARK;} // outside child
            if(traceMode==4)mat=mat==MAT_AIR?MAT_AIR:MAT_CLAY; // mismatch
            return true;
        };
        if(run==0)ok[run]=assetAppearancePageReference(page,page,page,ordered,ids,terrain,touched,expected,level,trace);
        else ok[run]=assetAppearancePage(page,page,page,ordered,ids,terrain,touched,actual,level,trace);
    }
    CHECK_EQ(ok[0],ok[1]);CHECK(callbacks[0]==callbacks[1]);CHECK_EQ(expected.size(),actual.size());
    const auto fields=[](const AssetAppearanceCell& c){return std::array<int64_t,12>{c.cell,c.instance,c.resource,c.sourceX,c.sourceY,c.sourceZ,c.yaw,c.material,c.offsetX,c.offsetY,c.offsetZ,0};};
    for(size_t i=0;i<std::min(expected.size(),actual.size());++i)CHECK(fields(expected[i])==fields(actual[i]));
    if(ok[0]){std::vector<uint32_t> a,b;CHECK(assetPackAppearancePage(page,page,page,17,ordered,expected,a,level));CHECK(assetPackAppearancePage(page,page,page,17,ordered,actual,b,level));CHECK(a==b);}
}
}
VXC_TEST(assetappearance_xy_index_reference_parity) {
    auto leaf=fixture(MAT_LEAF_BROADLEAF,true),bark=fixture(MAT_BARK);
    for(int page:{-1,0})for(uint8_t level=0;level<=7;++level)for(int seed=0;seed<4;++seed){
        std::vector<AssetField::ResolvedAssetInstance> ordered;std::vector<uint32_t> ids;
        const int64_t scale=int64_t(1)<<level,base=int64_t(page)*32*scale,half=level?scale/2:0;
        for(int i=0;i<40;++i){auto r=instance(i%2?leaf:bark,uint8_t((i+seed)%4));
            const int edge=(i*13+seed*7)%36-2;
            r.anchorVx=base+edge*scale+half-r.grid->rotatedOriginX(r.yawQuarter);
            r.anchorVy=base+((i*7)%32)*scale+half-r.grid->rotatedOriginY(r.yawQuarter);
            r.anchorVz=base+((i*3)%32)*scale+half-r.grid->originZ();
            ordered.push_back(r);ids.push_back(i%3?uint32_t(i+1):0);
            if(i%5==0){ordered.push_back(r);ids.push_back(uint32_t(i+100));}
        }
        pageIndexParity(page,level,ordered,ids);
    }
    std::vector<AssetField::ResolvedAssetInstance> repeated(8,instance(leaf,0));
    std::vector<uint32_t> ids(8,4);ids[0]=0;
    for(int mode=1;mode<=4;++mode)pageIndexParity(-1,1,repeated,ids,mode);
    pageIndexParity(100,0,repeated,ids); // no approved bounds overlap
    pageIndexParity(-1,0,repeated,std::vector<uint32_t>(8,0));
    pageIndexParity(-1,8,repeated,ids); // unsupported coarse level
    repeated[0].anchorVx=(int64_t(1)<<50)+1;pageIndexParity(-1,0,repeated,ids);
    repeated[0].grid=nullptr;pageIndexParity(-1,0,repeated,ids); // malformed source
}
VXC_TEST(assetappearance_xy_index_bounded_fallback_reference_parity) {
    std::vector<uint8_t> bytes;auto put=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back(uint8_t(n>>(8*i)));};
    put(kVxaMagic);put(kVxaVersion);put(0);put(0);put(0);put(32);put(32);put(1);put(100);put(1);put(0);put(0);bytes.push_back(MAT_BARK);put(1024);
    AssetGrid broad;CHECK(broad.parse(bytes)==AssetParseError::kOk);
    auto r=instance(broad,0);r.anchorVx=r.anchorVy=-32;r.anchorVz=-16;
    //1025*64 bin references exceeds65536; first unapproved solid must continue
    // occluding the approved duplicate after fallback, never reveal it.
    std::vector<AssetField::ResolvedAssetInstance> ordered(1025,r);std::vector<uint32_t> ids(1025,0);ids.back()=7;
    pageIndexParity(-1,0,ordered,ids);
    ids[0]=9;pageIndexParity(-1,0,ordered,ids);
    ordered.resize(4097,r);ids.resize(4097,9);pageIndexParity(-1,0,ordered,ids); // input limit, output atomic
}

#pragma once
#include "voxelcore/assetfield.h"
#include "voxelcore/assetownership.h"
#include <limits>

namespace vxc {
struct AssetCandidateBounds {int64_t x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;};
inline bool assetCandidateBounds(const AssetField::ResolvedAssetInstance& r,AssetCandidateBounds& b){
    if(!r.grid||!r.grid->valid()||r.yawQuarter>3||r.grid->voxelSizeMm()!=100)return false;
    const auto& g=*r.grid;
    if(std::abs(int64_t(g.originX()))>1000000||std::abs(int64_t(g.originY()))>1000000||std::abs(int64_t(g.originZ()))>1000000)return false;
    constexpr int64_t limit=int64_t(1)<<50;
    if(r.anchorVx < -limit||r.anchorVx>limit||r.anchorVy < -limit||r.anchorVy>limit||r.anchorVz < -limit||r.anchorVz>limit)return false;
    b={r.anchorVx+g.rotatedOriginX(r.yawQuarter),r.anchorVy+g.rotatedOriginY(r.yawQuarter),r.anchorVz+g.originZ(),0,0,0};
    b.x1=b.x0+g.rotatedSizeX(r.yawQuarter)-1;b.y1=b.y0+g.rotatedSizeY(r.yawQuarter)-1;b.z1=b.z0+g.sizeZ()-1;return true;
}
inline MaterialId assetCandidateMaterial(const AssetField::ResolvedAssetInstance& r,const AssetCandidateBounds& b,int64_t x,int64_t y,int64_t z){
    if(x<b.x0||x>b.x1||y<b.y0||y>b.y1||z<b.z0||z>b.z1)return MAT_AIR;
    return r.grid->atYaw(int32_t(x-b.x0),int32_t(y-b.y0),int32_t(z-b.z0),r.yawQuarter);
}
// Map a world-aligned candidate cell/face back to the original source frame.
// Coordinates include the source origin, matching appearance packet sampling.
// This is a coordinate mapping only: callers must separately prove source
// identity and that the cell survives canonical composition.
struct AssetCandidateSourceSample {int32_t x=0,y=0,z=0;uint8_t axis=0;bool positive=false;};
inline bool assetCandidateSourceSample(const AssetField::ResolvedAssetInstance& r,
    int64_t x,int64_t y,int64_t z,uint8_t axis,bool positive,AssetCandidateSourceSample& out){
    AssetCandidateBounds b;
    if(axis>2||!assetCandidateBounds(r,b)||x<b.x0||x>b.x1||y<b.y0||y>b.y1||z<b.z0||z>b.z1)return false;
    // Bounds above make these anchor-relative differences small, even for
    // negative world coordinates near the admitted anchor limit.
    const int64_t dx=x-r.anchorVx,dy=y-r.anchorVy,dz=z-r.anchorVz;
    int64_t sx=dx,sy=dy;
    switch(r.yawQuarter){
        case 1:sx=dy;sy=-dx;break;
        case 2:sx=-dx;sy=-dy;break;
        case 3:sx=-dy;sy=dx;break;
        default:break;
    }
    uint8_t sourceAxis=axis;bool sourcePositive=positive;
    if(axis<2){
        if(r.yawQuarter&1u)sourceAxis=uint8_t(1-axis);
        if(r.yawQuarter==2||(r.yawQuarter==1&&axis==0)||(r.yawQuarter==3&&axis==1))sourcePositive=!positive;
    }
    out={int32_t(sx),int32_t(sy),int32_t(dz),sourceAxis,sourcePositive};return true;
}
// Corners need an additional reflection offset: canonical yaw rotates cell
// indices, whereas a reflected unit box spans [cell, cell+1]. Mapping a raw
// vertex with the cell rotation alone shifts leaf cutout coordinates by one
// voxel. Return the corresponding original-source lattice vertex explicitly.
struct AssetCandidateSourceVertex {int32_t x=0,y=0,z=0;};
inline bool assetCandidateSourceVertex(const AssetField::ResolvedAssetInstance& r,
    int64_t x,int64_t y,int64_t z,uint8_t cornerX,uint8_t cornerY,uint8_t cornerZ,
    AssetCandidateSourceVertex& out){
    AssetCandidateSourceSample cell;
    if(cornerX>1||cornerY>1||cornerZ>1||!assetCandidateSourceSample(r,x,y,z,2,true,cell))return false;
    int32_t cx=cornerX,cy=cornerY;
    switch(r.yawQuarter){
        case 1:cx=cornerY;cy=1-cornerX;break;
        case 2:cx=1-cornerX;cy=1-cornerY;break;
        case 3:cx=1-cornerY;cy=cornerX;break;
        default:break;
    }
    out={cell.x+cx,cell.y+cy,cell.z+cornerZ};return true;
}
// Winner suppression preserves occlusion by owned assets. Removing instances
// before composition would incorrectly reveal later overlapping instances.
template<class Owned> MaterialId assetTerrainRenderMaterial(const std::vector<AssetField::ResolvedAssetInstance>& ordered,
    int64_t x,int64_t y,int64_t z,Owned&& owned){
    for(const auto& r:ordered){AssetCandidateBounds b;if(!assetCandidateBounds(r,b))continue;
        const auto m=assetCandidateMaterial(r,b,x,y,z);if(m!=MAT_AIR)return owned(r)?MAT_AIR:m;}
    return MAT_AIR;
}
// Worker-safe with immutable ordered inputs and a thread-safe column provider.
// Output is VXA3 in world-aligned axes: apply anchor translation only, because
// canonical grid yaw rotates voxel indices, not voxel-box corners.
template<class Column> bool assetBuildCandidateVxa(const std::vector<AssetField::ResolvedAssetInstance>& ordered,
    size_t selected,Column&& column,std::vector<uint8_t>& output,uint64_t maxCells=64ull*1024*1024,size_t maxBytes=64u*1024*1024){
    output.clear();if(selected>=ordered.size()||maxBytes<53)return false;
    std::vector<AssetCandidateBounds> bounds;bounds.reserve(ordered.size());
    for(const auto& r:ordered){AssetCandidateBounds b;if(!assetCandidateBounds(r,b))return false;bounds.push_back(b);}
    const auto& r=ordered[selected];const auto& g=*r.grid;const auto& b=bounds[selected];
    const int32_t sx=g.rotatedSizeX(r.yawQuarter),sy=g.rotatedSizeY(r.yawQuarter),sz=g.sizeZ();
    if(uint64_t(sx)*sy*sz>maxCells)return false;
    std::vector<uint8_t> bytes;
    auto word=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back(uint8_t(n>>(8*i)));};
    word(kVxaMagic);word(3);word(uint32_t(g.rotatedOriginX(r.yawQuarter)));word(uint32_t(g.rotatedOriginY(r.yawQuarter)));word(uint32_t(g.originZ()));
    word(sx);word(sy);word(sz);word(100);word(0);word(0);word(0);
    uint32_t count=0,length=0;MaterialId previous=MAT_AIR;uint64_t solid=0;
    auto flush=[&](){if(!length)return true;if(bytes.size()+5>maxBytes)return false;bytes.push_back(uint8_t(previous));word(length);++count;length=0;return true;};
    for(int32_t x=0;x<sx;++x)for(int32_t y=0;y<sy;++y){
        const auto facts=column(b.x0+x,b.y0+y);
        for(int32_t z=0;z<sz;++z){
            auto m=g.atYaw(x,y,z,r.yawQuarter);
            if(m!=MAT_AIR){
                if(Amplifier::materialAt(facts,b.z0+z)!=MAT_AIR)m=MAT_AIR;
                else for(size_t i=0;i<selected;++i)if(assetCandidateMaterial(ordered[i],bounds[i],b.x0+x,b.y0+y,b.z0+z)!=MAT_AIR){m=MAT_AIR;break;}
            }
            if(length&&(m!=previous||length==UINT32_MAX))if(!flush())return false;
            previous=m;++length;if(m!=MAT_AIR)++solid;
        }
    }
    if(!solid||!flush())return false;
    for(int i=0;i<4;++i)bytes[36+i]=uint8_t(count>>(8*i));output=std::move(bytes);return true;
}
// Includes the mesher's apron at every level. Caller passes visible, parked and
// pending keys; future requests must also carry the committed generation.
inline bool assetPageTouches(const AssetCandidateBounds& b,const AssetRenderPage& p){
    if(p.level>30||p.x<INT32_MIN||p.x>INT32_MAX||p.y<INT32_MIN||p.y>INT32_MAX||p.z<INT32_MIN||p.z>INT32_MAX)return false;
    const int64_t scale=int64_t(1)<<p.level;
    auto overlap=[&](int64_t key,int64_t lo,int64_t hi){
        // Use division to avoid overflow at extreme page keys/levels.
        const int64_t coarseLo=floorDiv(lo,scale),coarseHi=floorDiv(hi,scale);
        return coarseLo<=key*32+32&&coarseHi>=key*32-1;
    };
    return overlap(p.x,b.x0,b.x1)&&overlap(p.y,b.y0,b.y1)&&overlap(p.z,b.z0,b.z1);
}
}

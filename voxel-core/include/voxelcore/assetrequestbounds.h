#pragma once
#include "voxelcore/assetcandidate.h"

namespace vxc {
// Conservative overlap with a complete generation request, including its halo.
// Coordinates/extents are level-space cells. Invalid inputs fail open: this
// optimization must never hide a source that downstream validation may need.
inline bool assetMayContributeToRequest(const AssetField::ResolvedAssetInstance& r,
    int64_t x,int64_t y,int64_t z,uint64_t nx,uint64_t ny,uint64_t nz,uint32_t level){
    AssetCandidateBounds b;
    if(level>30||!assetCandidateBounds(r,b))return true;
    const int64_t origins[3]={x,y,z},lo[3]={b.x0,b.y0,b.z0},hi[3]={b.x1,b.y1,b.z1};
    const uint64_t counts[3]={nx,ny,nz};
    // Validate every axis before allowing any rejection.
    for(int a=0;a<3;++a)
        if(!counts[a]||counts[a]>uint64_t(INT64_MAX)||
           origins[a]>INT64_MAX-int64_t(counts[a]-1))return true;
    const int64_t scale=int64_t(1)<<level;
    for(int a=0;a<3;++a)
        if(floorDiv(hi[a],scale)<origins[a]||
           floorDiv(lo[a],scale)>origins[a]+int64_t(counts[a]-1))return false;
    return true;
}
}

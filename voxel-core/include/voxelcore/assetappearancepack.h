#pragma once
#include "voxelcore/assetappearancepage.h"
#include <algorithm>
#include <array>
#include <numeric>

namespace vxc {
// GPU word layout: version1 level0, version2 levels1..7; optional version3
// retains a selected recursive child via per-cell signed XYZ offsets. No disk format.
// Coarse cells sample c*2^level+2^(level-1), matching asset stamp geometry.
// Instance anchors are page-relative in finest (100mm) voxels at every level.
// Header: version,totalWords,pageXYZ,level,instanceCount,cellCount,
// directoryOffset,maskOffset,instanceOffset,handleOffset,brickCount,
// generationLo,generationHi,offsetStart(version3 only). Directory: 64 mask-block indices+1.
// Version3 offsets follow handles in the same rank order; three signed bytes, high byte zero.
// Mask block: first packed-handle index then 16 occupancy dwords.
// Instance: resource, signed page-relative anchor XYZ, yaw, 3 reserved dwords.
// Handles: two uint16 values per dword, 1-based local instance indices.
// Brick order matches GPU cells: (bx+4*by)*4+bz; local x+8*y+64*z.
// Geometry and words MUST share allocation/generation publication and retirement.
inline bool assetPackAppearancePage(int32_t px,int32_t py,int32_t pz,uint64_t generation,
    const std::vector<AssetField::ResolvedAssetInstance>& ordered,
    const std::vector<AssetAppearanceCell>& cells,std::vector<uint32_t>& output,uint8_t level=0) {
    if(level>7||!generation||ordered.size()>4096||cells.size()>32768)return false;
    if(cells.empty()){std::vector<uint32_t>().swap(output);return true;}
    const auto key=[](uint16_t c){const uint32_t x=c%32,y=(c/32)%32,z=c/1024;
        return (((x/8)+4*(y/8))*4+z/8)*512+(x%8)+8*(y%8)+64*(z%8);};
    std::vector<uint32_t> indices(cells.size());std::iota(indices.begin(),indices.end(),0u);
    std::sort(indices.begin(),indices.end(),[&](uint32_t a,uint32_t b){return key(cells[a].cell)<key(cells[b].cell);});
    std::vector<uint32_t> local(ordered.size(),0),resources(ordered.size(),0),instances;
    std::array<uint32_t,64> directory{};std::vector<uint32_t> masks,handles,offsets;
    const bool explicitOffsets=std::any_of(cells.begin(),cells.end(),[](const auto& c){return c.offsetX||c.offsetY||c.offsetZ;});
    uint32_t previous=UINT32_MAX;
    const int64_t scale=int64_t(1)<<level,half=level?scale/2:0;
    const int64_t origin[3]={int64_t(px)*32*scale,int64_t(py)*32*scale,int64_t(pz)*32*scale};
    for(uint32_t index:indices){
        const auto& cell=cells[index];
        if(cell.cell>=32768||cell.instance>=ordered.size()||!cell.resource||cell.yaw>3)return false;
        const uint32_t k=key(cell.cell);if(k==previous)return false;previous=k;
        const auto& instance=ordered[cell.instance];
        for(int64_t offset:{int64_t(cell.offsetX),int64_t(cell.offsetY),int64_t(cell.offsetZ)})
            if(offset < -half || offset >= scale-half)return false;
        const int64_t wx=origin[0]+(cell.cell%32)*scale+half+cell.offsetX,
            wy=origin[1]+((cell.cell/32)%32)*scale+half+cell.offsetY,
            wz=origin[2]+(cell.cell/1024)*scale+half+cell.offsetZ;
        AssetCandidateSourceSample source;AssetCandidateBounds bounds;
        if(!assetCandidateBounds(instance,bounds)||!assetCandidateSourceSample(instance,wx,wy,wz,2,true,source)||
            source.x!=cell.sourceX||source.y!=cell.sourceY||source.z!=cell.sourceZ||instance.yawQuarter!=cell.yaw||
            assetCandidateMaterial(instance,bounds,wx,wy,wz)!=cell.material||cell.material==MAT_AIR)return false;
        if(!local[cell.instance]){
            const int64_t anchor[3]={instance.anchorVx-origin[0],instance.anchorVy-origin[1],instance.anchorVz-origin[2]};
            for(auto a:anchor)if(a<INT32_MIN||a>INT32_MAX)return false;
            local[cell.instance]=uint32_t(instances.size()/8)+1;resources[cell.instance]=cell.resource;
            instances.insert(instances.end(),{cell.resource,uint32_t(anchor[0]),uint32_t(anchor[1]),uint32_t(anchor[2]),cell.yaw,0,0,0});
        }else if(resources[cell.instance]!=cell.resource)return false;
        const uint32_t brick=k/512,bit=k%512;
        if(!directory[brick]){directory[brick]=uint32_t(masks.size()/17)+1;masks.push_back(uint32_t(handles.size()));masks.resize(masks.size()+16,0);}
        masks[(directory[brick]-1)*17+1+bit/32]|=1u<<(bit%32);
        handles.push_back(local[cell.instance]);
        if(explicitOffsets)offsets.push_back(uint32_t(uint8_t(cell.offsetX))|(uint32_t(uint8_t(cell.offsetY))<<8u)|(uint32_t(uint8_t(cell.offsetZ))<<16u));
    }
    const size_t total=80+masks.size()+instances.size()+(handles.size()+1)/2+offsets.size();
    const size_t maxWords=explicitOffsets?98304u:65536u;
    if(total>maxWords)return false;
    std::vector<uint32_t> words;words.reserve(total);words.resize(16,0);words[0]=explicitOffsets?3u:(level?2u:1u);words[5]=level;words[2]=uint32_t(px);words[3]=uint32_t(py);words[4]=uint32_t(pz);
    words[6]=uint32_t(instances.size()/8);words[7]=uint32_t(cells.size());words[8]=16;words[9]=80;
    words[10]=80+uint32_t(masks.size());words[11]=words[10]+uint32_t(instances.size());words[12]=uint32_t(masks.size()/17);
    words[13]=uint32_t(generation);words[14]=uint32_t(generation>>32);
    words.insert(words.end(),directory.begin(),directory.end());words.insert(words.end(),masks.begin(),masks.end());
    words.insert(words.end(),instances.begin(),instances.end());
    for(size_t i=0;i<handles.size();i+=2)words.push_back(handles[i]|(i+1<handles.size()?handles[i+1]<<16:0));
    if(explicitOffsets){words[15]=uint32_t(words.size());words.insert(words.end(),offsets.begin(),offsets.end());}
    words[1]=uint32_t(words.size());
    // Versions1/2 remain sub256KiB; version3 adds one signed XYZ dword
    // per contributing cell and remains bounded below384KiB.
    if(words.size()>maxWords)return false;
    output=std::move(words);return true;
}
} // namespace vxc

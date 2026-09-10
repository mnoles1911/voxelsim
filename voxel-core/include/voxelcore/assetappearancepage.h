#pragma once
#include "voxelcore/assetcandidate.h"
#include <functional>
#include <algorithm>
#include <array>

namespace vxc {
// In-memory evidence only, not a serialized/GPU layout. Cell order is x-fast,
// then y, then z. Resource IDs identify already verified approved source data;
// zero means no approved appearance. Instance index distinguishes placements
// sharing a source. Source coordinates include the original grid origin.
struct AssetAppearanceCell {
    uint16_t cell=0;
    uint32_t instance=0,resource=0;
    int32_t sourceX=0,sourceY=0,sourceZ=0;
    uint8_t yaw=0;
    MaterialId material=MAT_AIR;
    // Optional finest-world offset from the coarse representative. A recursive
    // reducer uses this to retain its actual contributing child, not the centre.
    int8_t offsetX=0,offsetY=0,offsetZ=0;
};
// The trace receives level-space coordinates and returns the actual selected
// finest-world cell and material. False is a provenance mismatch; true+air
// means no contributor. It must use the exact reducer that built geometry.
using AssetAppearanceTrace=std::function<bool(int64_t,int64_t,int64_t,int64_t&,int64_t&,int64_t&,MaterialId&)>;
// Exact 100 mm level-zero 32-cubed page. Terrain is the authoritative material
// sampler BEFORE assets. Touched is explicit per-cell write provenance, not an
// edited-brick predicate. It removes appearance only; no geometry is changed.
// Caller owns full ordered shortlist completeness and immutable source identity.
// Failure leaves output unchanged. Empty success has no appearance allocation.
template<class Terrain,class Touched>
bool assetAppearancePage(int32_t pageX,int32_t pageY,int32_t pageZ,
    const std::vector<AssetField::ResolvedAssetInstance>& ordered,
    const std::vector<uint32_t>& approvedResources,Terrain&& terrain,Touched&& touched,
    std::vector<AssetAppearanceCell>& output,uint8_t level=0,const AssetAppearanceTrace& trace={}) {
    if(level>7||ordered.size()!=approvedResources.size()||ordered.size()>4096)return false;
    std::vector<AssetCandidateBounds> bounds;
    bool anyApproved=false;
    for(size_t i=0;i<ordered.size();++i){
        AssetCandidateBounds b;if(!assetCandidateBounds(ordered[i],b))return false;
        if(approvedResources[i])anyApproved=true;
    }
    std::vector<AssetAppearanceCell> result;
    if(!anyApproved){output=std::move(result);return true;}
    bounds.reserve(ordered.size());
    for(const auto& r:ordered){AssetCandidateBounds b;assetCandidateBounds(r,b);bounds.push_back(b);}
    const int64_t scale=int64_t(1)<<level,half=level?scale/2:0;
    const int64_t ox=int64_t(pageX)*32*scale,oy=int64_t(pageY)*32*scale,oz=int64_t(pageZ)*32*scale;
    int begin[3]={0,0,0},end[3]={32,32,32};
    if(!trace){
        // Only approved sources can emit appearance. Keep all instances for
        // winner resolution, but avoid terrain callbacks outside their union.
        // A trace may select a non-representative child, so retains the full scan.
        std::fill(begin,begin+3,32);std::fill(end,end+3,0);
        const int64_t origins[3]={ox+half,oy+half,oz+half};
        for(size_t i=0;i<bounds.size();++i){
            if(!approvedResources[i])continue;
            const auto& b=bounds[i];
            const int64_t low[3]={b.x0,b.y0,b.z0},high[3]={b.x1,b.y1,b.z1};
            int first[3],last[3];
            for(int a=0;a<3;++a){
                first[a]=int(std::clamp<int64_t>(-floorDiv(-(low[a]-origins[a]),scale),0,32));
                last[a]=int(std::clamp<int64_t>(floorDiv(high[a]-origins[a],scale)+1,0,32));
            }
            if(first[0]>=last[0]||first[1]>=last[1]||first[2]>=last[2])continue;
            for(int a=0;a<3;++a){begin[a]=std::min(begin[a],first[a]);end[a]=std::max(end[a],last[a]);}
        }
    }
    if(!trace&&(begin[0]>=end[0]||begin[1]>=end[1]||begin[2]>=end[2])){output=std::move(result);return true;}
    // 8x8 bins of four representative page columns each. Every candidate,
    // including unapproved occluders, keeps its original winner-order index.
    // The immutable grid still decides Z occupancy/holes. Trace can choose
    // another finest child and deliberately retains the reference full scan.
    std::array<std::vector<uint16_t>,64> xy;
    bool indexed=!trace&&ordered.size()>=8;
    size_t references=0;
    if(indexed)for(size_t i=0;i<bounds.size();++i){
        const auto& b=bounds[i];
        const int x0=int(std::clamp<int64_t>(-floorDiv(-(b.x0-(ox+half)),scale),begin[0],end[0]));
        const int x1=int(std::clamp<int64_t>(floorDiv(b.x1-(ox+half),scale)+1,begin[0],end[0]));
        const int y0=int(std::clamp<int64_t>(-floorDiv(-(b.y0-(oy+half)),scale),begin[1],end[1]));
        const int y1=int(std::clamp<int64_t>(floorDiv(b.y1-(oy+half),scale)+1,begin[1],end[1]));
        if(x0>=x1||y0>=y1)continue;
        const size_t needed=size_t((x1-1)/4-x0/4+1)*size_t((y1-1)/4-y0/4+1);
        // At most65536 candidate IDs (plus bounded vector capacity); giant overlaps fall back without losing
        // candidates. No source/page-dependent unbounded spatial allocation.
        if(references+needed>65536){indexed=false;break;}
        references+=needed;
        for(int by=y0/4;by<=(y1-1)/4;++by)for(int bx=x0/4;bx<=(x1-1)/4;++bx)
            xy[size_t(bx+8*by)].push_back(uint16_t(i));
    }
    for(int z=begin[2];z<end[2];++z)for(int y=begin[1];y<end[1];++y)for(int x=begin[0];x<end[0];++x){
        int64_t wx=ox+x*scale+half,wy=oy+y*scale+half,wz=oz+z*scale+half;
        MaterialId selected=MAT_AIR;
        if(trace){
            if(!trace(int64_t(pageX)*32+x,int64_t(pageY)*32+y,int64_t(pageZ)*32+z,wx,wy,wz,selected))return false;
            if(selected==MAT_AIR)continue;
            if(wx<ox+x*scale||wx>=ox+(x+1)*scale||wy<oy+y*scale||wy>=oy+(y+1)*scale||wz<oz+z*scale||wz>=oz+(z+1)*scale)return false;
        }
        size_t firstCandidate=0;
        MaterialId firstMaterial=MAT_AIR;
        if(!trace){
            // Sparse foliage often leaves most of its box empty. Grid reads
            // are immutable: finding the first non-air candidate before the
            // terrain callback preserves precedence and avoids sampling gaps.
            if(indexed){
                firstCandidate=ordered.size();
                for(const auto i:xy[size_t(x/4+8*(y/4))]){
                    const auto m=assetCandidateMaterial(ordered[i],bounds[i],wx,wy,wz);
                    if(m!=MAT_AIR){firstCandidate=i;firstMaterial=m;break;}
                }
            }else{
                while(firstCandidate<ordered.size()){
                    firstMaterial=assetCandidateMaterial(ordered[firstCandidate],bounds[firstCandidate],wx,wy,wz);
                    if(firstMaterial!=MAT_AIR)break;
                    ++firstCandidate;
                }
            }
            if(firstCandidate==ordered.size())continue;
        }
        if(terrain(wx,wy,wz)!=MAT_AIR||touched(wx,wy,wz))continue;
        for(size_t i=firstCandidate;i<ordered.size();++i){
            // The immutable no-trace winner was already sampled above. Reuse
            // it rather than restarting the source column's RLE walk.
            const auto m=!trace&&i==firstCandidate?firstMaterial:
                assetCandidateMaterial(ordered[i],bounds[i],wx,wy,wz);
            if(m==MAT_AIR)continue;
            if(trace&&m!=selected)return false;
            // An unapproved winner still occludes later approved instances.
            if(approvedResources[i]){
                AssetCandidateSourceSample source;
                if(!assetCandidateSourceSample(ordered[i],wx,wy,wz,2,true,source))return false;
                result.push_back({uint16_t(x+32*(y+32*z)),uint32_t(i),approvedResources[i],
                    source.x,source.y,source.z,ordered[i].yawQuarter,m,
                    int8_t(wx-(ox+x*scale+half)),int8_t(wy-(oy+y*scale+half)),int8_t(wz-(oz+z*scale+half))});
            }
            break;
        }
    }
    output=std::move(result);return true;
}
} // namespace vxc

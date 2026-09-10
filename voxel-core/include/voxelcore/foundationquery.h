#pragma once
// Bounded evidence survey, not structural simulation or building approval.
// Coordinates are 100mm world voxels. Caller owns residency, live-world/edit
// identity and callback lifetime; this helper retains no world or cache.
#include "voxelcore/core.h"
#include <cstdint>
#include <limits>
#include <vector>

namespace vxc {
struct FoundationMaterialSample { bool known=false; MaterialId material=MAT_AIR; };
struct FoundationWaterSample { bool known=false; int32_t standingWaterMm=0; };
inline bool foundationBearingMaterial(MaterialId m) {
    // Snow, foliage, wood and craft materials are not ground bearing.
    return m>=MAT_BEDROCK && m<=MAT_CLAY && m!=MAT_SNOW;
}
struct FoundationColumn {
    int64_t x=0,y=0,firstGroundZ=0;
    bool groundFound=false,materialUnknown=false,waterKnown=false;
    int32_t waterMm=-1,fillGapMm=-1,bearingThicknessMm=0;
    // Depth below first ground TOP; -1 means no void observed in bounded scan.
    int32_t firstVoidDepthMm=-1;
    int32_t overheadOccupiedVoxels=0,nonGroundBelowPlaneVoxels=0;
    bool meetsProvisionalReportingCriteria=false;
};
struct FoundationSurvey {
    static constexpr int widthVoxels=50,headroomVoxels=30;
    static constexpr int maxFillMm=500,bearingScanMm=1000;
    bool validRequest=false;
    int64_t minX=0,minY=0,planeZ=0;
    std::vector<FoundationColumn> columns; // y-major, x increases first
    uint32_t unknownColumns=0,wetColumns=0,obstructedColumns=0;
    uint32_t provisionalCriteriaColumns=0;
};

// Plane is bottom of requested 3m clear volume. Search only the first 0.5m
// of possible fill below it; deeper/missing ground is unqualified, not absent
// everywhere. Inspect 1m of bearing starting at first ground, including its
// top voxel. A cave deeper than this remains unexamined. Fill is NOT placed.
// Material callback: (x,y,z)->FoundationMaterialSample, including authored AIR.
// Water callback: (x,y)->FoundationWaterSample; unknown must NOT become dry.
template<class MaterialAt,class WaterAt>
FoundationSurvey surveyFoundation(int64_t minX,int64_t minY,int64_t planeZ,
                                 MaterialAt&& materialAt,WaterAt&& waterAt) {
    FoundationSurvey out;out.minX=minX;out.minY=minY;out.planeZ=planeZ;
    const auto hi=std::numeric_limits<int64_t>::max();
    const auto lo=std::numeric_limits<int64_t>::min();
    if(minX>hi-49||minY>hi-49||planeZ>hi-29||planeZ<lo+15)return out;
    out.validRequest=true;out.columns.reserve(2500);
    for(int dy=0;dy<50;++dy)for(int dx=0;dx<50;++dx) {
        FoundationColumn c;c.x=minX+dx;c.y=minY+dy;
        const auto water=waterAt(c.x,c.y);
        c.waterKnown=water.known&&water.standingWaterMm>=0;
        if(c.waterKnown)c.waterMm=water.standingWaterMm;
        for(int up=0;up<30;++up) {
            const auto s=materialAt(c.x,c.y,planeZ+up);
            if(!s.known)c.materialUnknown=true;
            else if(s.material!=MAT_AIR)++c.overheadOccupiedVoxels;
        }
        for(int gap=0;gap<=5;++gap) {
            const int64_t z=planeZ-1-gap;
            const auto s=materialAt(c.x,c.y,z);
            if(!s.known){c.materialUnknown=true;break;}
            if(foundationBearingMaterial(s.material)) {
                c.groundFound=true;c.firstGroundZ=z;c.fillGapMm=gap*100;
                // Reuse this first sample; do not invoke a live callback twice.
                c.bearingThicknessMm=100;bool contiguous=true;
                for(int depth=1;depth<10;++depth) {
                    const auto b=materialAt(c.x,c.y,z-depth);
                    if(!b.known){c.materialUnknown=true;contiguous=false;continue;}
                    if(b.material==MAT_AIR&&c.firstVoidDepthMm<0)c.firstVoidDepthMm=depth*100;
                    if(!foundationBearingMaterial(b.material))contiguous=false;
                    if(contiguous)c.bearingThicknessMm+=100;
                }
                break;
            }
            if(s.material!=MAT_AIR)++c.nonGroundBelowPlaneVoxels;
        }
        c.meetsProvisionalReportingCriteria=c.groundFound&&!c.materialUnknown&&
            c.waterKnown&&c.waterMm==0&&c.overheadOccupiedVoxels==0&&
            c.nonGroundBelowPlaneVoxels==0&&c.bearingThicknessMm==1000;
        if(c.materialUnknown||!c.waterKnown)++out.unknownColumns;
        if(c.waterKnown&&c.waterMm>0)++out.wetColumns;
        if(c.overheadOccupiedVoxels||c.nonGroundBelowPlaneVoxels)++out.obstructedColumns;
        if(c.meetsProvisionalReportingCriteria)++out.provisionalCriteriaColumns;
        out.columns.push_back(c);
    }
    return out;
}
} // namespace vxc

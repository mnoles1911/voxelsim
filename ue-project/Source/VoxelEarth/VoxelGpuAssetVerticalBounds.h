#pragma once
#include <cstdint>
#include <limits>
// Complete brick-stack sample interval, including apron bricks. Never shift signed coordinates.
namespace VoxelGpuAssetVerticalBounds {
inline bool Add(int64_t A,int64_t B,int64_t& Out){
    constexpr auto Min=std::numeric_limits<int64_t>::min(),Max=std::numeric_limits<int64_t>::max();
    if((B>0&&A>Max-B)||(B<0&&A<Min-B))return false;Out=A+B;return true;
}
inline bool Scale(int64_t A,int64_t Positive,int64_t& Out){
    if(Positive<=0||A>std::numeric_limits<int64_t>::max()/Positive||A<std::numeric_limits<int64_t>::min()/Positive)return false;
    Out=A*Positive;return true;
}
// True only proves disjointness. Invalid/overflowing inputs retain the instance.
// Coarse representative gaps are intentionally included in this interval.
inline bool DefinitelyOutside(int64_t BrickZMin,uint64_t BricksZ,int32_t Level,
    int64_t AnchorZ,int64_t GridOriginZ,uint64_t GridSizeZ){
    if(Level<0||Level>15||BricksZ==0||GridSizeZ==0||
        BricksZ>uint64_t(std::numeric_limits<int64_t>::max())||GridSizeZ>uint64_t(std::numeric_limits<int64_t>::max()))return false;
    const int64_t Step=int64_t(1)<<Level,Half=Step/2;
    int64_t FirstCell,CellCount,LastCell,FirstSample,LastSample,AssetFirst,AssetLast;
    if(!Scale(BrickZMin,8,FirstCell)||!Scale(int64_t(BricksZ),8,CellCount)||!Add(FirstCell,CellCount-1,LastCell)||
        !Scale(FirstCell,Step,FirstSample)||!Add(FirstSample,Half,FirstSample)||
        !Scale(LastCell,Step,LastSample)||!Add(LastSample,Half,LastSample)||
        !Add(AnchorZ,GridOriginZ,AssetFirst)||!Add(AssetFirst,int64_t(GridSizeZ)-1,AssetLast))return false;
    return AssetLast<FirstSample||AssetFirst>LastSample;
}
}

#include "VoxelDirectCoarseAppearance.h"
#include <type_traits>
std::function<vxc::MaterialId(int64,int64,int64)> VoxelDirectCoarseAppearance::MakeSampler(
    std::function<vxc::MaterialId(int64,int64,int64)> Terrain,
    std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered,uint32 Level)
{
    check(Level>=1&&Level<=7);
    const int64 Scale=int64(1)<<Level,Half=Scale/2;
    return [Terrain=std::move(Terrain),Ordered=std::move(Ordered),Scale,Half](int64 X,int64 Y,int64 Z){
        const auto Material=Terrain(X,Y,Z);
        if(Material!=vxc::MAT_AIR)return Material;
        return vxc::AssetField::materialAtResolved(Ordered,X*Scale+Half,Y*Scale+Half,Z*Scale+Half);
    };
}

TSharedPtr<const FVoxelTerrainAppearancePage,ESPMode::ThreadSafe> VoxelDirectCoarseAppearance::Prepare(
    const vxc::GeneratedWorld<8>& Generated,FIntVector Key,uint32 Level,uint64 Generation,
    const std::vector<vxc::AssetField::ResolvedAssetInstance>& Ordered,
    const FVoxelAppearanceBankBinding& Binding,bool SurfacePreserve,
    TFunctionRef<vxc::MaterialId(int64,int64,int64)> ActualGeometry,FString& Error)
{
    if(Level<1||Level>7){Error=TEXT("direct coarse appearance requires level1..7");return nullptr;}
    const int64 Scale=int64(1)<<Level,Half=Scale/2,X0=int64(Key.X)*32,Y0=int64(Key.Y)*32;
    // Cache terrain-only columns independently of asset material. The fallback
    // brick sampler remains the geometry authority and is checked below.
    using FColumn=std::decay_t<decltype(Generated.amplifier().columnCached(0,0))>;
    TArray<FColumn> Columns;Columns.SetNum(1024);
    for(int Y=0;Y<32;++Y)for(int X=0;X<32;++X)Columns[X+32*Y]=Generated.amplifier().columnCached((X0+X)*Scale+Half,(Y0+Y)*Scale+Half);
    bool Mismatch=false;
    auto Page=FVoxelTerrainAppearancePage::Prepare(Key,Level,Generation,Ordered,Binding,
        [&](int64 X,int64 Y,int64 Z){const int32 LX=int32(vxc::floorDiv(X,Scale)-X0),LY=int32(vxc::floorDiv(Y,Scale)-Y0);return vxc::Amplifier::coarseSurfaceMaterialAt(Columns[LX+32*LY],Z,Scale,SurfacePreserve);},
        [&](int64 X,int64 Y,int64 Z){const auto Expected=vxc::AssetField::materialAtResolved(Ordered,X,Y,Z);const auto Actual=ActualGeometry(vxc::floorDiv(X,Scale),vxc::floorDiv(Y,Scale),vxc::floorDiv(Z,Scale));if(Expected!=Actual)Mismatch=true;return Expected!=Actual;},Error);
    if(Mismatch){Error=TEXT("direct coarse appearance refused geometry/source material mismatch");return nullptr;}
    return Page;
}

#pragma once
#include "CoreMinimal.h"
#include "voxelcore/amplifier.h"
#include "voxelcore/assetfield.h"

namespace VoxelEnvironmentPages
{
// Shared by ordinary level-zero generation and private held-page preparation.
// Storage is worker-owned/captured by the caller and must outlive this sampler.
struct FLevelZeroRenderSampler
{
    const vxc::ColumnSample* Columns=nullptr;
    int64 BaseX=0,BaseY=0;
    int32 Edge=34;
    const std::vector<vxc::AssetField::ResolvedAssetInstance>* Instances=nullptr;
    vxc::MaterialId operator()(int64 X,int64 Y,int64 Z) const
    {
        const int32 LX=int32(X-BaseX)+1,LY=int32(Y-BaseY)+1;
        checkSlow(Columns&&Instances&&LX>=0&&LX<Edge&&LY>=0&&LY<Edge);
        const auto Terrain=vxc::Amplifier::materialAt(Columns[LX+Edge*LY],Z);
        return Terrain==vxc::MAT_AIR&&!Instances->empty()
            ? vxc::AssetField::materialAtResolvedForRender<true>(*Instances,X,Y,Z) : Terrain;
    }
};
}

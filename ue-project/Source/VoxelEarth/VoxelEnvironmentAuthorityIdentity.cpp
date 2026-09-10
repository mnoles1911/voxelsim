#include "VoxelEnvironmentAuthorityIdentity.h"
#include "VoxelProductionCandidatePreparation.h"
#include "Misc/SecureHash.h"
#include "voxelcore/assetmanifest.h"
#include <tuple>
namespace VoxelEnvironmentAuthority {
namespace {
bool SameLayer(const vxc::AssetLayer& A,const vxc::AssetLayer& B) {
    return std::tie(A.cellMm,A.maxHeightMm,A.maxDepthMm,A.maxRadiusMm,A.densityPerMille,A.seedCount,A.terrainLattice)==
           std::tie(B.cellMm,B.maxHeightMm,B.maxDepthMm,B.maxRadiusMm,B.densityPerMille,B.seedCount,B.terrainLattice);
}
bool SameSpecies(const vxc::AssetSpecies& A,const vxc::AssetSpecies& B) {
    if(std::tie(A.bankId,A.layer,A.elevMinMm,A.elevMaxMm,A.slopeMinMmPerM,A.slopeMaxMmPerM,A.slopeFullMmPerM,
        A.moistureAffinity,A.talusAffinity,A.curvatureAffinity,A.waterMaxMm,A.clusterQ10,A.heightMm,A.depthMm,A.voxelSizeMm)!=
       std::tie(B.bankId,B.layer,B.elevMinMm,B.elevMaxMm,B.slopeMinMmPerM,B.slopeMaxMmPerM,B.slopeFullMmPerM,
        B.moistureAffinity,B.talusAffinity,B.curvatureAffinity,B.waterMaxMm,B.clusterQ10,B.heightMm,B.depthMm,B.voxelSizeMm))return false;
    for(int I=0;I<vxc::kBiomeCount;++I)if(A.weightPerMille[I]!=B.weightPerMille[I]||A.occupancyPerMille[I]!=B.occupancyPerMille[I])return false;
    return true;
}
bool Matches(const vxc::AssetField& Field,const std::vector<vxc::AssetLayer>& Layers,const std::vector<vxc::AssetSpecies>& Species) {
    if(Field.layers().size()!=Layers.size()||Field.species().size()!=Species.size())return false;
    for(size_t I=0;I<Layers.size();++I)if(!SameLayer(Field.layers()[I],Layers[I]))return false;
    for(size_t I=0;I<Species.size();++I)if(!SameSpecies(Field.species()[I],Species[I]))return false;
    return true;
}
std::string Utf8(const FString& Text){FTCHARToUTF8 Bytes(*Text);return std::string(Bytes.Get(),size_t(Bytes.Length()));}
}
TUniquePtr<FIdentity> FIdentity::Create(const vxc::AssetField& Field,TConstArrayView<uint8> Bytes,FString& Error) {
    Error.Reset();if(Bytes.Num()==0||Bytes.Num()>16*1024*1024||Field.empty()||Field.species().size()>4096){Error=TEXT("authority catalog admission bounds");return {};}
    vxc::AssetManifest Manifest;
    if(Manifest.parse(Bytes.GetData(),size_t(Bytes.Num()))!=vxc::AssetManifestError::kOk){Error=TEXT("authority catalog manifest refused");return {};}
    if(Manifest.species().size()>4096){Error=TEXT("authority catalog species bound");return {};}
    auto Out=TUniquePtr<FIdentity>(new FIdentity);
    Out->Layers=Manifest.layers();vxc::assetTightenLayerCaps(Manifest,Out->Layers);
    vxc::assetSpeciesTableFromManifest(Manifest,Out->Species);
    if(!Matches(Field,Out->Layers,Out->Species)){Error=TEXT("registered field differs from canonical manifest placement tables");return {};}
    Out->Registered=&Field;Out->Seed=Field.seed();Out->Catalog=Utf8(FMD5::HashBytes(Bytes.GetData(),Bytes.Num()));return Out;
}
std::string FIdentity::catalogIdentity(const vxc::AssetField& Field) const {
    if(&Field!=Registered||Field.seed()!=Seed||Field.empty()||!Matches(Field,Layers,Species))return {};
    return Catalog;
}
std::string FIdentity::contentHash(const vxc::AssetGrid& Grid) const {
    if(!Grid.valid()||!Grid.onTerrainLattice()||Grid.hasParts())return {};
    const uint64 Cells=uint64(Grid.sizeX())*uint64(Grid.sizeY())*uint64(Grid.sizeZ());
    if(Cells>VoxelProductionCandidate::MaxCells||Grid.footprintBytes()>64ull*1024*1024)return {};
    return Utf8(VoxelProductionCandidate::GridContentHash(Grid));
}
}

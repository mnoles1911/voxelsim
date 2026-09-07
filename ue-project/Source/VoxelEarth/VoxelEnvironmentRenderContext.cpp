#include "VoxelEnvironmentRenderContext.h"
#include "VoxelProductionCandidatePreparation.h"
#include "voxelcore/core.h"
namespace VoxelEnvironmentRender {
FContextRef Build(VoxelProductionEnvironment::FSnapshot Snapshot,uint64 WorldSeed,
    const FString& Provider,const FString& Catalog,const TArray<FSourceBinding>& Bindings,FString& Error)
{
    Error.Reset();auto Fail=[&](const TCHAR* Why)->FContextRef{Error=Why;return {};};
    if(!Snapshot)return Fail(TEXT("missing ownership snapshot"));
    if(Snapshot->records.size()>256)return Fail(TEXT("render snapshot admission bound"));
    auto Context=MakeShared<FContext,ESPMode::ThreadSafe>();Context->Snapshot=Snapshot;
    bool AnyOwned=false;for(const auto& R:Snapshot->records)AnyOwned|=R.owner==vxc::AssetRenderOwner::Object;
    if(!AnyOwned)return Context;
    if(Bindings.Num()>256)return Fail(TEXT("render context admission bound"));
    if(Provider.IsEmpty()||Catalog.IsEmpty())return Fail(TEXT("missing source identity"));
    const uint64 ProviderId=VoxelProductionCandidate::Fingerprint(Provider+FString::Printf(TEXT(":worldgen:%u"),vxc::kWorldGenVersion));
    TMap<uint32,FString> Validated;uint64 HashedCells=0;
    for(const auto& R:Snapshot->records){
        if(R.owner!=vxc::AssetRenderOwner::Object)continue;
        const auto& P=R.provenance;
        if(P.worldSeed!=WorldSeed||P.providerFingerprint!=ProviderId||P.yawQuarter>3||!(R.id==vxc::assetObjectId(P)))return Fail(TEXT("ownership provenance mismatch"));
        constexpr int64 Bound=int64(1)<<30;
        if(P.anchorVx < -Bound||P.anchorVx > Bound||P.anchorVy < -Bound||P.anchorVy > Bound||P.anchorVz < -Bound||P.anchorVz > Bound)return Fail(TEXT("source anchor outside bounds"));
        const FSourceBinding* Binding=nullptr;
        for(const auto& B:Bindings)if(B.BankId==P.bankId&&B.SeedIndex==P.seedIndex){if(Binding)return Fail(TEXT("duplicate source binding"));Binding=&B;}
        if(!Binding||!Binding->Grid||!Binding->Grid->valid()||!Binding->Grid->onTerrainLattice())return Fail(TEXT("missing canonical 100 mm bank binding"));
        const uint32 Key=(uint32(P.bankId)<<16)|P.seedIndex;
        if(!Validated.Contains(Key)){
            const auto& G=*Binding->Grid;const uint64 Cells=uint64(G.sizeX())*G.sizeY()*G.sizeZ();
            if(Cells>8ull*1024*1024||HashedCells+Cells>32ull*1024*1024)return Fail(TEXT("source hash work budget exceeded"));
            HashedCells+=Cells;const FString Hash=VoxelProductionCandidate::GridContentHash(G);
            if(Hash!=Binding->CanonicalSourceHash)return Fail(TEXT("canonical bank content changed"));
            Validated.Add(Key,Hash);
        }
        if(P.catalogFingerprint!=VoxelProductionCandidate::Fingerprint(Catalog+TEXT(":")+Validated.FindChecked(Key)))return Fail(TEXT("catalog/content fingerprint mismatch"));
        for(const auto& Previous:Context->Sources)if(Previous.Provenance==P)return Fail(TEXT("duplicate owned provenance"));
        Context->Sources.Add({P,Binding->Grid});
    }
    return Context;
}
bool FContext::AffectsBounds(int64 X0,int64 Y0,int64 Z0,int64 X1,int64 Y1,int64 Z1) const
{
    if(Sources.IsEmpty())return false;
    if(X0>X1||Y0>Y1||Z0>Z1)return true;
    for(const auto& S:Sources){const auto& P=S.Provenance;const auto& G=*S.Grid;
        const int64 X=P.anchorVx+G.rotatedOriginX(P.yawQuarter),Y=P.anchorVy+G.rotatedOriginY(P.yawQuarter),Z=P.anchorVz+G.originZ();
        if(X<=X1&&X+G.rotatedSizeX(P.yawQuarter)-1>=X0&&Y<=Y1&&Y+G.rotatedSizeY(P.yawQuarter)-1>=Y0&&Z<=Z1&&Z+G.sizeZ()-1>=Z0)return true;
    }
    return false;
}
bool FContext::MarkPrivate(std::vector<vxc::AssetField::ResolvedAssetInstance>& Instances) const
{
    if(Instances.size()>8192)return false;
    if(Sources.IsEmpty()){for(auto& I:Instances)I.suppressTerrainRender=false;return true;}
    const auto Matches=[](const auto& I,const auto& P){return I.bankId==P.bankId&&I.seedIndex==P.seedIndex&&I.anchorVx==P.anchorVx&&I.anchorVy==P.anchorVy&&I.anchorVz==P.anchorVz&&I.yawQuarter==P.yawQuarter&&I.layer==P.layer;};
    for(const auto& I:Instances)for(const auto& S:Sources)if(Matches(I,S.Provenance)&&I.grid!=S.Grid)return false;
    for(auto& I:Instances){I.suppressTerrainRender=false;for(const auto& S:Sources)if(Matches(I,S.Provenance)){I.suppressTerrainRender=true;break;}}
    return true;
}
}

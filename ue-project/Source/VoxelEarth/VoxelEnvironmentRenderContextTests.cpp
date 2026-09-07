#include "VoxelEnvironmentRenderContext.h"
#include "VoxelProductionCandidatePreparation.h"
#include "Misc/AutomationTest.h"
#include "voxelcore/core.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace {
vxc::AssetGrid RenderContextGrid(uint8 Material){
    std::vector<uint8> Bytes;auto Word=[&](uint32 W){for(int I=0;I<4;++I)Bytes.push_back(uint8(W>>(I*8)));};
    for(uint32 W:{vxc::kVxaMagic,3u,0u,0u,0u,2u,2u,2u,100u,1u,0u,0u})Word(W);Bytes.push_back(Material);Word(8);
    vxc::AssetGrid Grid;Grid.parse(Bytes.data(),Bytes.size());return Grid;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEnvironmentRenderContextTest,"Voxel.Objects.EnvironmentRenderContext",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelEnvironmentRenderContextTest::RunTest(const FString&){
    using namespace VoxelEnvironmentRender;
    auto A=RenderContextGrid(16),B=RenderContextGrid(17),Changed=RenderContextGrid(18);
    const FString Provider=TEXT("test-provider"),Catalog=TEXT("test-catalog");
    const auto Hash=VoxelProductionCandidate::GridContentHash(A);
    vxc::AssetProvenance P{19,VoxelProductionCandidate::Fingerprint(Provider+FString::Printf(TEXT(":worldgen:%u"),vxc::kWorldGenVersion)),VoxelProductionCandidate::Fingerprint(Catalog+TEXT(":")+Hash),-4,-7,-2,5,3,1,3};
    VoxelProductionEnvironment::FAdapter Adapter([](const auto&,const auto&,const auto&){return true;},vxc::AssetCpu);
    FString Error;auto Empty=Build(Adapter.Visible(),19,TEXT(""),TEXT(""),{},Error);
    TestTrue(TEXT("empty adapter context needs no hashes/bindings"),Empty.IsValid()&&!Empty->HasOwnedSources());
    std::vector<vxc::AssetField::ResolvedAssetInstance> Canonical(2);
    Canonical[0]={&A,P.anchorVx,P.anchorVy,P.anchorVz,P.yawQuarter,P.layer,P.bankId,P.seedIndex,77,false};
    Canonical[1]=Canonical[0];Canonical[1].grid=&B;Canonical[1].bankId=6;Canonical[1].speciesIndex=5;
    auto* Storage=Canonical.data();TestTrue(TEXT("empty marks without copying"),Empty->MarkPrivate(Canonical));TestTrue(TEXT("empty preserves storage"),Canonical.data()==Storage);
    VoxelProductionEnvironment::FSource Source;Source.Provenance=P;Source.Admission={true,true,true,false};
    const vxc::AssetRenderPage Page{0,0,0,0,vxc::AssetCpu};
    const auto Ticket=Adapter.Prepare(Source,vxc::AssetRenderOwner::Object,{Page});auto Prepared=Adapter.Prepared(Ticket);
    if(!TestTrue(TEXT("real adapter prepares immutable ownership"),Prepared.IsValid()))return false;
    Adapter.MarkObjectReady(Ticket,1);Adapter.MarkPageReady(Ticket,Page,vxc::AssetCpu,Prepared->generation);
    if(!TestTrue(TEXT("test callback publishes adapter snapshot"),Adapter.Commit(Ticket)))return false;
    TArray<FSourceBinding> Bindings;Bindings.Add({5,3,&A,Hash});
    auto Context=Build(Adapter.Visible(),19,Provider,Catalog,Bindings,Error);
    if(!TestTrue(*Error,Context.IsValid()))return false;
    TestEqual(TEXT("context captures adapter generation"),Context->Generation(),Adapter.Visible()->generation);
    auto Private=Canonical;Storage=Private.data();TestTrue(TEXT("job copy marks"),Context->MarkPrivate(Private));
    TestTrue(TEXT("mark preserves order and allocation"),Private.data()==Storage&&Private[0].grid==&A&&Private[1].grid==&B);
    TestTrue(TEXT("bank identity ignores differing species index"),Private[0].suppressTerrainRender);
    TestFalse(TEXT("species index equal to owned bank is not ownership"),Private[1].suppressTerrainRender);
    TestFalse(TEXT("canonical cache unchanged"),Canonical[0].suppressTerrainRender);
    auto Reused=Private;Storage=Reused.data();TestTrue(TEXT("empty context clears reused owned list"),Empty->MarkPrivate(Reused));
    TestTrue(TEXT("empty demotion preserves allocation"),Reused.data()==Storage);
    TestFalse(TEXT("old suppression cannot survive demotion"),Reused[0].suppressTerrainRender);
    TestEqual(TEXT("demoted list restores canonical composition"),vxc::AssetField::materialAtResolvedForRender<true>(Reused,P.anchorVx+A.rotatedOriginX(P.yawQuarter),P.anchorVy+A.rotatedOriginY(P.yawQuarter),P.anchorVz),vxc::MaterialId(16));
    auto Oversized=MakeShared<vxc::AssetOwnershipSnapshot,ESPMode::ThreadSafe>();Oversized->records.resize(257);
    TestFalse(TEXT("oversized all-terrain snapshot fails before scan"),Build(Oversized,19,Provider,Catalog,{},Error).IsValid());
    const int64 X=P.anchorVx+A.rotatedOriginX(P.yawQuarter),Y=P.anchorVy+A.rotatedOriginY(P.yawQuarter),Z=P.anchorVz;
    TestEqual(TEXT("default first winner"),vxc::AssetField::materialAtResolved(Canonical,X,Y,Z),vxc::MaterialId(16));
    TestEqual(TEXT("owned first winner becomes air without revealing overlap"),vxc::AssetField::materialAtResolvedForRender<true>(Private,X,Y,Z),vxc::MAT_AIR);
    TestEqual(TEXT("authoritative composition ignores marker"),vxc::AssetField::materialAtResolved(Private,X,Y,Z),vxc::MaterialId(16));
    std::swap(Private[0],Private[1]);Context->MarkPrivate(Private);
    TestEqual(TEXT("unowned earlier winner survives later ownership"),vxc::AssetField::materialAtResolvedForRender<true>(Private,X,Y,Z),vxc::MaterialId(17));
    auto Wrong=Canonical;Wrong[0].yawQuarter=2;Context->MarkPrivate(Wrong);TestFalse(TEXT("canonical yaw is identity"),Wrong[0].suppressTerrainRender);
    Wrong=Canonical;Wrong[0].anchorVx++;Context->MarkPrivate(Wrong);TestFalse(TEXT("signed anchor is identity"),Wrong[0].suppressTerrainRender);
    Wrong=Canonical;Wrong[0].grid=&Changed;TestFalse(TEXT("stale bank binding refused without partial mutation"),Context->MarkPrivate(Wrong));TestFalse(TEXT("failure preserves marker"),Wrong[0].suppressTerrainRender);
    TestFalse(TEXT("provider mismatch rejected"),Build(Adapter.Visible(),19,TEXT("other"),Catalog,Bindings,Error).IsValid());
    TestFalse(TEXT("catalog mismatch rejected"),Build(Adapter.Visible(),19,Provider,TEXT("other"),Bindings,Error).IsValid());
    TestFalse(TEXT("world seed mismatch rejected"),Build(Adapter.Visible(),20,Provider,Catalog,Bindings,Error).IsValid());
    Bindings[0].Grid=&Changed;TestFalse(TEXT("changed canonical content rejected"),Build(Adapter.Visible(),19,Provider,Catalog,Bindings,Error).IsValid());
    Bindings[0].CanonicalSourceHash=VoxelProductionCandidate::GridContentHash(Changed);TestFalse(TEXT("new content hash cannot impersonate old ownership fingerprint"),Build(Adapter.Visible(),19,Provider,Catalog,Bindings,Error).IsValid());
    return true;
}
#endif

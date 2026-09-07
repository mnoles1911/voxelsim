#include "VoxelProductionCandidatePreparation.h"
#include "VoxelEnvironmentSparseGrid.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
std::vector<uint8> CandidateFixtureVxa(bool Split)
{
    std::vector<uint8> Out;
    const auto Word=[&](uint32 V){for(int I=0;I<4;++I)Out.push_back(uint8(V>>(I*8)));};
    for(uint32 V:{vxc::kVxaMagic,3u,0u,0u,0u,2u,1u,1u,100u,Split?2u:1u,0u,0u})Word(V);
    Out.push_back(16);Word(Split?1u:2u);if(Split){Out.push_back(16);Word(1);}
    return Out;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionCandidateAdmissionTest,
    "Voxel.Objects.ProductionCandidateAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FVoxelProductionCandidateAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace VoxelProductionCandidate;
    FWorkRef Work=MakeShared<FWork,ESPMode::ThreadSafe>();Work->EditEpoch=7;Work->ResidencyEpoch=11;
    TestTrue(TEXT("matching immutable input epochs accepted"),IsCurrent(*Work,7,11));
    TestFalse(TEXT("edit during worker invalidates result"),IsCurrent(*Work,8,11));
    TestFalse(TEXT("tile answer change invalidates result"),IsCurrent(*Work,7,12));
    const auto Old=Work;Cancel(Work);
    TestFalse(TEXT("cancel releases active preparation"),Work.IsValid());
    TestFalse(TEXT("worker-retained cancelled result cannot be adopted"),IsCurrent(*Old,7,11));
    std::vector<vxc::AssetRenderPage> Pages;
    TestTrue(TEXT("negative cross-chunk source produces complete bounded apron superset"),EnumeratePages({-1,-1,0,1,1,3},8,Pages));
    bool Left=false,Right=false,Coarse=false;
    for(const auto& Page:Pages){Left|=Page.level==0&&Page.x==-1;Right|=Page.level==0&&Page.x==0;Coarse|=Page.level==7;}
    TestTrue(TEXT("negative and positive pages included"),Left&&Right);
    TestTrue(TEXT("coarsest visible level included"),Coarse);
    TestFalse(TEXT("unbounded page volume refused"),EnumeratePages({0,0,0,100000,100000,100000},8,Pages));
    TestTrue(TEXT("failed enumeration exposes no partial page set"),Pages.empty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelProductionCandidateSnapshotTest,
    "Voxel.Objects.ProductionCandidateSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FVoxelProductionCandidateSnapshotTest::RunTest(const FString& Parameters)
{
    using namespace VoxelProductionCandidate;
    const auto Vxa=CandidateFixtureVxa(false),Split=CandidateFixtureVxa(true);
    vxc::AssetGrid A,B;
    TestTrue(TEXT("canonical fixture parsed"),A.parse(Vxa.data(),Vxa.size())==vxc::AssetParseError::kOk);
    TestTrue(TEXT("alternate compression parsed"),B.parse(Split.data(),Split.size())==vxc::AssetParseError::kOk);
    TestEqual(TEXT("source content identity ignores compression boundaries"),GridContentHash(A),GridContentHash(B));
    FWork Work;Work.Provenance.anchorVx=-13;Work.Provenance.anchorVy=4;Work.Provenance.anchorVz=10;Work.Provenance.yawQuarter=3;
    Work.Descriptor.SpecId=TEXT("arbitrary-river-cobble");Work.Descriptor.Kind=TEXT("rock");Work.Descriptor.Category=TEXT("environment");
    Work.CanonicalSourceHash=GridContentHash(A);
    TestTrue(TEXT("arbitrary descriptor builds immutable staged input"),BuildImmutableSnapshot(Work,Vxa));
    if(!Work.Geometry)return false;
    TestTrue(TEXT("canonical and clipped hash fields both retained"),!Work.CanonicalSourceHash.IsEmpty()&&!Work.ClippedGeometryHash.IsEmpty());
    FMemoryReader Dynamic(Work.Dynamic);uint32 Version=0;Dynamic<<Version;
    FVoxelEnvironmentAssetDescriptor Descriptor;TestTrue(TEXT("descriptor readable"),VoxelEnvironmentAsset::SerializeIdentity(Dynamic,Descriptor));
    FTransform Transform;FVoxelEnvironmentSparseGrid Grid;bool Collision=true,Severed=true;
    Dynamic<<Transform<<Grid.Size<<Grid.Origin<<Grid.Mm<<Grid.MaxDataZ<<Collision<<Severed;
    TestEqual(TEXT("source metadata remains arbitrary"),Descriptor.SpecId,Work.Descriptor.SpecId);
    TestEqual(TEXT("canonical quarter yaw retained as provenance"),Work.Provenance.yawQuarter,uint8(3));
    TestTrue(TEXT("prepared actor does not rotate yaw-baked indices again"),Transform.GetRotation().Equals(FQuat::Identity));
    TestTrue(TEXT("canonical anchor translated exactly"),Transform.GetTranslation().Equals(FVector(-130.,40.,100.)));
    TestFalse(TEXT("prepared actor collision remains disabled"),Collision);
    FMemoryReader Geometry(*Work.Geometry);Geometry<<Version;
    TestTrue(TEXT("staged sparse body readable"),Grid.Serialize(Geometry));
    TestEqual(TEXT("source material preserved"),Grid.At(0,0,0),uint8(16));
    Work.Cancelled.Store(true);
    TestFalse(TEXT("cancelled preparation does not build another snapshot"),BuildImmutableSnapshot(Work,Vxa));
    return true;
}
#endif

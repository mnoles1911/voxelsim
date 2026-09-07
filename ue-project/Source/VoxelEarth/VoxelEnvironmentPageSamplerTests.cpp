#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelEnvironmentPageSampler.h"
#include "Misc/AutomationTest.h"
#include "voxelcore/core.h"

namespace {
vxc::AssetGrid HeldPageSamplerGrid(uint8 Material)
{
    std::vector<uint8> Bytes;
    const auto Word=[&](uint32 W){for(int I=0;I<4;++I)Bytes.push_back(uint8(W>>(I*8)));};
    for(uint32 W:{vxc::kVxaMagic,3u,0u,0u,0u,2u,2u,2u,100u,1u,0u,0u})Word(W);
    Bytes.push_back(Material);Word(8);
    vxc::AssetGrid Grid;Grid.parse(Bytes.data(),Bytes.size());return Grid;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeldPageSamplerTest,"Voxel.Objects.HeldPageSampler",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelHeldPageSamplerTest::RunTest(const FString&)
{
    auto Winner=HeldPageSamplerGrid(16),Loser=HeldPageSamplerGrid(17);
    TArray<vxc::ColumnSample> Columns;Columns.SetNum(34*34);
    for(auto& C:Columns)C.surfaceMm=0;
    std::vector<vxc::AssetField::ResolvedAssetInstance> Instances;
    Instances.push_back({&Winner,-32,-32,-1,0,0,1,0,1,false});
    Instances.push_back({&Loser,-32,-32,-1,0,0,2,0,2,false});
    const VoxelEnvironmentPages::FLevelZeroRenderSampler Sampler{Columns.GetData(),-32,-32,34,&Instances};
    TestEqual(TEXT("Negative chunk coordinates preserve canonical first winner"),int32(Sampler(-32,-32,0)),16);
    Instances[0].suppressTerrainRender=true;
    TestEqual(TEXT("Suppressed first winner remains air instead of exposing overlap loser"),int32(Sampler(-32,-32,0)),int32(vxc::MAT_AIR));
    const auto Terrain=vxc::Amplifier::materialAt(Columns[35],-1);
    TestTrue(TEXT("Fixture has real terrain below surface"),Terrain!=vxc::MAT_AIR);
    TestEqual(TEXT("Suppression never removes terrain under the asset"),int32(Sampler(-32,-32,-1)),int32(Terrain));
    std::swap(Instances[0],Instances[1]);
    TestEqual(TEXT("Earlier unsuppressed winner keeps its material"),int32(Sampler(-32,-32,0)),17);
    Instances.clear();
    TestEqual(TEXT("Empty shortlist samples air above terrain"),int32(Sampler(-32,-32,0)),int32(vxc::MAT_AIR));
    TestEqual(TEXT("Apron uses same absolute terrain sampling"),int32(Sampler(-33,-33,-1)),int32(vxc::Amplifier::materialAt(Columns[0],-1)));
    return true;
}
#endif

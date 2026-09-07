#include "VoxelGpuAssetVerticalBounds.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGpuAssetVerticalBoundsTest,"Voxel.Objects.GpuAssetVerticalBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGpuAssetVerticalBoundsTest::RunTest(const FString&){
    using VoxelGpuAssetVerticalBounds::DefinitelyOutside;
    for(int32 L=0;L<=15;++L)for(int64 B:{-17ll,-1ll,0ll,11ll}){
        const int64 Step=int64(1)<<L,First=B*8*Step+Step/2,Last=(B*8+47)*Step+Step/2;
        TestFalse(TEXT("first apron sample retained"),DefinitelyOutside(B,6,L,First,0,1));
        TestFalse(TEXT("last apron sample retained"),DefinitelyOutside(B,6,L,Last,0,1));
        if(L>0)TestFalse(TEXT("coarse gaps conservatively retained"),DefinitelyOutside(B,6,L,First+1,0,1));
        TestTrue(TEXT("strictly below interval culled"),DefinitelyOutside(B,6,L,First-2,0,2));
        TestTrue(TEXT("strictly above interval culled"),DefinitelyOutside(B,6,L,Last+1,0,1));
        TestFalse(TEXT("asset ending at first sample retained"),DefinitelyOutside(B,6,L,First-9,-1,11));
        TestFalse(TEXT("asset starting at last sample retained"),DefinitelyOutside(B,6,L,Last+9,-9,7));
        struct FAsset{int64 Min,Size;uint8 Material;};
        const FAsset Assets[]={{First-50,10,3},{First-2,4,16},{First,12,17},{Last-4,8,23},{Last+10,12,4}};
        for(int64 I=0;I<48;++I){
            const int64 Z=(B*8+I)*Step+Step/2;uint8 Before=0,After=0;
            for(const auto& A:Assets)if(Z>=A.Min&&Z<A.Min+A.Size){Before=A.Material;break;}
            for(const auto& A:Assets)if(!DefinitelyOutside(B,6,L,A.Min,0,A.Size)&&Z>=A.Min&&Z<A.Min+A.Size){After=A.Material;break;}
            TestEqual(TEXT("ordered winner unchanged at every sample"),After,Before);
        }
    }
    constexpr int64 Hi=std::numeric_limits<int64>::max(),Lo=std::numeric_limits<int64>::min();
    TestFalse(TEXT("brick overflow retains"),DefinitelyOutside(Hi,6,0,0,0,1));
    TestFalse(TEXT("negative brick overflow retains"),DefinitelyOutside(Lo,6,0,0,0,1));
    TestFalse(TEXT("coarse overflow retains"),DefinitelyOutside(Hi/8,1,15,0,0,1));
    TestFalse(TEXT("anchor overflow retains"),DefinitelyOutside(0,6,0,Hi,1,1));
    TestFalse(TEXT("asset bound overflow retains"),DefinitelyOutside(0,6,0,Hi,0,2));
    TestFalse(TEXT("anchor underflow retains"),DefinitelyOutside(0,6,0,Lo,-1,1));
    TestFalse(TEXT("unsigned extent overflow retains"),DefinitelyOutside(0,~uint64(0),0,0,0,1));
    TestFalse(TEXT("empty request retains"),DefinitelyOutside(0,0,0,0,0,1));
    TestFalse(TEXT("invalid level retains"),DefinitelyOutside(0,6,16,0,0,1));
    TestFalse(TEXT("empty malformed grid retains"),DefinitelyOutside(0,6,0,0,0,0));
    return true;
}
#endif

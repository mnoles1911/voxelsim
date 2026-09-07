#include "VoxelHeldBrickParity.h"
#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuWorldGen.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "voxelcore/brickpack.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace HeldBrickTestPrivate
{
FVoxelGpuRegionRequest Request()
{
    FVoxelGpuRegionRequest R;
    VoxelGpuChunkRegion::SetChunkFootprint(R,0,0,2500);
    R.RasterSize=FUintVector2(1,1); R.ElevationMm.Add(0); R.ClimatePacked.Add(0);
    auto& A=R.AssetInstances.AddDefaulted_GetRef();
    A.AnchorRelVx=12; A.AnchorRelVy=12; A.AnchorVz=80000;
    A.SizeX=8; A.SizeY=8; A.SizeZ=16;
    for(uint32 I=0;I<64;++I) { R.AssetColStarts.Add(I); R.AssetSpans.Add((16u<<8)|16u); }
    R.AssetColStarts.Add(64);
    return R;
}
FVoxelBrickCpuPackRef Expected()
{
    const auto P=vxc::packChunkBricksCanonical([](int32 X,int32 Y,int32 Z)->vxc::MaterialId
    { return X>=4 && X<12 && Y>=4 && Y<12 && Z<16 ? 16 : 0; });
    auto C=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>(); C->OriginVoxel=FIntVector(0,0,80000);
    for(const auto& D:P.descs) { C->Desc.Add(D.OccWord); C->Desc.Add(D.MatWord); }
    for(auto W:P.occ) C->Occ.Add(W); for(auto W:P.mat) C->Mat.Add(W);
    C->BrickSolid=P.brickSolid; C->bAnySolid=P.anySolid; C->bAllSolid=P.allSolid;
    return C;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeldBrickAdmissionTest,"Voxel.Objects.HeldBrickAdmission",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FVoxelHeldBrickAdmissionTest::RunTest(const FString&)
{
    FString Error; FVoxelGpuHeldBrickBudget Budget;
    auto R=HeldBrickTestPrivate::Request();
    TestTrue(TEXT("bounded canonical request admitted"),FVoxelGpuMeshJobManager::EstimateHeldBrickOnly(R,64ull<<20,Budget,Error));
    TestTrue(TEXT("accounts private parity storage"),Budget.RetainedAndReadbackBytes>=8*38408);
    const uint64 Bound=Budget.TotalBytes;
    TestFalse(TEXT("insufficient budget refuses before submission"),FVoxelGpuMeshJobManager::EstimateHeldBrickOnly(R,Bound-1,Budget,Error));
    R.DispatchColumns=FUintVector2(MAX_uint32,MAX_uint32);
    TestFalse(TEXT("overflow shape rejected"),FVoxelGpuMeshJobManager::EstimateHeldBrickOnly(R,MAX_uint64,Budget,Error));
    int32 Callbacks=0;
    FVoxelGpuMeshJobManager Manager(FVoxelGpuMeshJobComplete::CreateLambda([&](FVoxelGpuMeshJobResult&& Result)
    { ++Callbacks; TestTrue(TEXT("cancel is held-only"),Result.bHeldBrickOnly); TestEqual(TEXT("cancel status"),Result.Status,EVoxelGpuMeshJobStatus::Cancelled); }));
    TestTrue(TEXT("wrapper assigns job"),Manager.SubmitHeldBrickOnly(HeldBrickTestPrivate::Request(),7,9,64ull<<20,Budget,Error)!=0);
    TestEqual(TEXT("second held job refused"),Manager.SubmitHeldBrickOnly(HeldBrickTestPrivate::Request(),7,9,64ull<<20,Budget,Error),uint64(0));
    Manager.CancelAll(); TestEqual(TEXT("exactly one callback"),Callbacks,1);
    TestTrue(TEXT("slot reopens after cancellation"),Manager.SubmitHeldBrickOnly(HeldBrickTestPrivate::Request(),7,9,64ull<<20,Budget,Error)!=0);
    Manager.CancelAll(); TestEqual(TEXT("second cancellation delivered"),Callbacks,2);
    auto C=HeldBrickTestPrivate::Expected(); FVoxelHeldBrickParityResult Result;
    TestTrue(TEXT("canonical semantic self parity"),FVoxelHeldBrickParity::ComparePacks(*C,*C,0,0,Result));
    auto Bad=*C; Bad.Desc[0]=0x30000000;
    TestFalse(TEXT("invalid descriptor kind rejected"),FVoxelHeldBrickParity::ComparePacks(*C,Bad,0,0,Result));
    Bad=*C; Bad.Mat.Empty();
    TestFalse(TEXT("truncated material payload rejected"),FVoxelHeldBrickParity::ComparePacks(*C,Bad,0,0,Result));
    TestFalse(TEXT("source offsets cannot underflow"),FVoxelHeldBrickParity::ComparePacks(*C,*C,1,1,Result));
    return true;
}

namespace HeldBrickTestPrivate
{
class FGpuParityCommand final : public IAutomationLatentCommand
{
public:
    explicit FGpuParityCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    ~FGpuParityCommand() override
    {
        // The manager callback captures this command, so cancel while its state
        // is still alive. This also covers automation abort/destruction.
        if (Manager) Manager->CancelAll();
        Parity.Cancel();
        FVoxelHeldBrickParity::DrainForShutdown();
        FlushRenderingCommands();
    }
    bool Update() override
    {
        if (Phase==0)
        {
            Deadline=FPlatformTime::Seconds()+30;
            Manager=MakeUnique<FVoxelGpuMeshJobManager>(FVoxelGpuMeshJobComplete::CreateLambda(
                [this](FVoxelGpuMeshJobResult&& R) { Result=MoveTemp(R); Completed=true; }));
            if (!Test->TestTrue(TEXT("real held job admitted"),Manager->SubmitHeldBrickOnly(Request(),17,19,64ull<<20,Budget,Error)!=0))
                return true;
            Phase=1;
        }
        if (FPlatformTime::Seconds()>Deadline)
        {
            Test->AddError(TEXT("Held GPU parity latent fixture exceeded its 30 second deadline"));
            return true;
        }
        if (Phase==1)
        {
            Manager->Tick();
            if (!Completed) return false;
            if (!Test->TestEqual(TEXT("held generation succeeds"),Result.Status,EVoxelGpuMeshJobStatus::Success)) return true;
            Test->TestTrue(TEXT("held publication invariant"),Result.bPublicationHeld && Result.bHeldBrickOnly);
            Test->TestEqual(TEXT("no generated quads"),Result.NumQuads,uint32(0));
            Test->TestTrue(TEXT("private pack retained"),Result.BrickVolume.IsValid());
            if (!Test->TestTrue(TEXT("asynchronous private comparison starts"),Parity.Begin(Expected(),Result.BrickVolume,FIntVector(0,0,80000),19,Error))) return true;
            Test->TestEqual(TEXT("no premature completion"),Parity.Poll(Compared),EVoxelHeldBrickParityStatus::Pending);
            Phase=2;
            return false; // Engine, not this fixture, advances the core ticker.
        }
        const auto Status=Parity.Poll(Compared);
        if (Status==EVoxelHeldBrickParityStatus::Pending) return false;
        Test->TestEqual(TEXT("real private CPU/GPU materials match"),Status,EVoxelHeldBrickParityStatus::Passed);
        Test->TestEqual(TEXT("generation retained"),Compared.OwnershipGeneration,uint64(19));
        if (Status!=EVoxelHeldBrickParityStatus::Passed) { Test->AddError(Compared.Error); return true; }
        {
            FVoxelHeldBrickParity Cancelled;
            Test->TestTrue(TEXT("retry after retirement"),Cancelled.Begin(Expected(),Result.BrickVolume,FIntVector(0,0,80000),20,Error));
            Cancelled.Cancel();
            FVoxelHeldBrickParity Refused;
            Test->TestFalse(TEXT("cancel cannot bypass retirement gate"),Refused.Begin(Expected(),Result.BrickVolume,FIntVector(0,0,80000),21,Error));
        }
        // Deliberately within this same Update: no ticker frame after helper
        // destruction. Only the explicit shutdown drain/flush can retire it.
        FVoxelHeldBrickParity::DrainForShutdown(); FlushRenderingCommands();
        Test->TestFalse(TEXT("destroyed helper retired without ticker"),FVoxelHeldBrickParity::IsRetirementPending());
        {
            FVoxelHeldBrickParity Retry;
            Test->TestTrue(TEXT("retry after shutdown drain"),Retry.Begin(Expected(),Result.BrickVolume,FIntVector(0,0,80000),22,Error));
        }
        FVoxelHeldBrickParity::DrainForShutdown(); FlushRenderingCommands();
        Test->TestTrue(TEXT("new held job before promoted cancellation"),Manager->SubmitHeldBrickOnly(Request(),23,23,64ull<<20,Budget,Error)!=0);
        Manager->Tick(); // real promotion; an early callback is not retirement
        Manager->CancelAll();
        Test->TestEqual(TEXT("promoted cancellation closes held admission"),Manager->SubmitHeldBrickOnly(Request(),24,24,64ull<<20,Budget,Error),uint64(0));
        return true;
    }
private:
    FAutomationTestBase* Test;
    int32 Phase=0;
    double Deadline=0;
    bool Completed=false;
    FVoxelGpuMeshJobResult Result;
    FVoxelGpuHeldBrickBudget Budget;
    FString Error;
    FVoxelHeldBrickParity Parity;
    FVoxelHeldBrickParityResult Compared;
    TUniquePtr<FVoxelGpuMeshJobManager> Manager;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeldBrickGpuParityTest,"Voxel.Objects.HeldBrickGpuParity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FVoxelHeldBrickGpuParityTest::RunTest(const FString&)
{
    if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
    { AddWarning(TEXT("Held GPU parity requires SM6")); return true; }
    ADD_LATENT_AUTOMATION_COMMAND(HeldBrickTestPrivate::FGpuParityCommand(this));
    return true;
}
#endif

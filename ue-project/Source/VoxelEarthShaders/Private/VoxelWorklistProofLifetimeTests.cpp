#include "VoxelGpuWorklist.h"
#include "VoxelGpuWorldGen.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWorklistProofLifetimeTest,
    "Voxel.Objects.WorklistProofLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelWorklistProofLifetimeTest::RunTest(const FString&)
{
    if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
    {
        AddWarning(TEXT("Worklist proof requires SM6; no GPU lifetime proof on this RHI"));
        return true;
    }
    auto Start=[](uint32 Generation,int32 Count){
        auto Worklist=MakeUnique<FVoxelGpuWorklist>();Worklist->Init(8);
        TArray<FVoxelGpuChunkWorkRecord> Records;
        for(int32 I=0;I<Count;++I){auto& R=Records.AddDefaulted_GetRef();R.GenId=Generation+uint32(I);R.OriginVx=I*32;}
        Worklist->Append(Records);Worklist->Flush(uint32(Count));return Worklist;
    };
    auto GpuIdle=[](){
        ENQUEUE_RENDER_COMMAND(WorklistProofFixtureIdle)([](FRHICommandListImmediate& RHICmdList){RHICmdList.SubmitAndBlockUntilGPUIdle();});
        FlushRenderingCommands();
    };
    auto Check=[&](FVoxelGpuWorklist& Worklist){
        const auto Proof=Worklist.GetProofStatus();
        TestTrue(TEXT("This instance received its own nonvacuous proof"),Proof.Landed>0);
        TestEqual(TEXT("Distinct instance folds never cross-talk"),Proof.Failed,uint64(0));
        TestEqual(TEXT("Records are well formed"),Proof.MalformedOnGpu,uint64(0));
    };
    auto First=Start(11,2);auto Second=Start(101,3);
    GpuIdle();
    First->Flush(0);Second->Flush(0);FlushRenderingCommands();
    First->Flush(0);Second->Flush(0);FlushRenderingCommands();
    Check(*First);Check(*Second);
    First.Reset();
    // A new instance again requests sequence one, while Second retains a
    // different landed sequence-one payload. The mailbox must be independent.
    auto Recreated=Start(1001,1);GpuIdle();
    Recreated->Flush(0);FlushRenderingCommands();
    Recreated->Flush(0);FlushRenderingCommands();Check(*Recreated);Check(*Second);
    // Destroy with Flush still queued: destructor must drain its non-proof
    // borrows, and the shared proof readback must remain render-thread-owned.
    auto Queued=Start(2001,4);Queued.Reset();GpuIdle();
    return true;
}
#endif

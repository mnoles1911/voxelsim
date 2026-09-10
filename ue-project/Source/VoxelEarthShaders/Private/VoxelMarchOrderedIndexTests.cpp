#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelMarchChunkIndex.h"
#include "VoxelMarchIndexOrderedState.h"
#include "VoxelBrickPool.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMarchOrderedIndexTest,
    "Voxel.Objects.MarchOrderedIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelMarchOrderedIndexTest::RunTest(const FString&)
{
    // This exercises the real Seed/ApplyDelta capture and command installation,
    // not a mailbox model. A queued older view is held until B exists on GT.
    FVoxelMarchChunkIndex Index;
    const FVoxelBrickChunkKey Key{0,0,0,0};
    TArray<FVoxelBrickIndexEntry> Seed;Seed.Add(FVoxelBrickIndexEntry{Key,7});
    Index.DebugSeedOrderedForTest(Seed);
    FEvent* Constructed=FPlatformProcess::GetSynchEventFromPool(true);
    struct FRead {uint64 Generation=0;uint32 Value=0,Block=0;bool Released=false;};
    auto Before=MakeShared<FRead,ESPMode::ThreadSafe>();
    auto After=MakeShared<FRead,ESPMode::ThreadSafe>();
    ENQUEUE_RENDER_COMMAND(MarchOrderedOldView)([&Index,Constructed,Before](FRHICommandListImmediate&){
        Before->Released=Constructed->Wait(10000);
        Index.DebugReadOrderedForTest(0,Before->Generation,Before->Value,Before->Block);
    });
    FVoxelBrickIndexDelta Delta;Delta.Removed.Add(FVoxelBrickIndexEntry{Key,7});
    Index.DebugApplyOrderedForTest(Delta);
    ENQUEUE_RENDER_COMMAND(MarchOrderedNewView)([&Index,After](FRHICommandListImmediate&){
        Index.DebugReadOrderedForTest(0,After->Generation,After->Value,After->Block);
    });
    Constructed->Trigger();
    FlushRenderingCommands(); // Test-only synchronization; production never waits.
    FPlatformProcess::ReturnSynchEventToPool(Constructed);
    TestTrue(TEXT("B constructed before old view reads"),Before->Released);
    TestEqual(TEXT("Old queued view retains generation A"),Before->Generation,uint64(1));
    TestTrue(TEXT("Old view retains resident cell"),(Before->Value & FVoxelMarchChunkIndex::kResidentBit)!=0);
    TestTrue(TEXT("Old view retains matching coarse occupancy"),(Before->Block&1)!=0);
    TestEqual(TEXT("New view receives generation B"),After->Generation,uint64(2));
    TestFalse(TEXT("New view sees removed cell"),(After->Value & FVoxelMarchChunkIndex::kResidentBit)!=0);
    TestEqual(TEXT("New view sees matching empty coarse block"),After->Block&1,uint32(0));
    TestEqual(TEXT("All packet accounting retired"),Index.GetQueuedIndexPacketBytes(),uint64(0));

    auto BeforeReset=MakeShared<FRead,ESPMode::ThreadSafe>();
    auto AfterReset=MakeShared<FRead,ESPMode::ThreadSafe>();
    ENQUEUE_RENDER_COMMAND(MarchOrderedBeforeReset)([&Index,BeforeReset](FRHICommandListImmediate&){
        Index.DebugReadOrderedForTest(0,BeforeReset->Generation,BeforeReset->Value,BeforeReset->Block);
    });
    Index.DebugResetOrderedForTest();
    Index.DebugSeedOrderedForTest(Seed);
    ENQUEUE_RENDER_COMMAND(MarchOrderedAfterReset)([&Index,AfterReset](FRHICommandListImmediate&){
        Index.DebugReadOrderedForTest(0,AfterReset->Generation,AfterReset->Value,AfterReset->Block);
    });
    FlushRenderingCommands();
    TestEqual(TEXT("Queued pre-reset view retains old generation"),BeforeReset->Generation,uint64(2));
    TestEqual(TEXT("New epoch begins at first generation"),AfterReset->Generation,uint64(1));
    TestTrue(TEXT("New epoch cell and coarse seed visible together"),(AfterReset->Value & FVoxelMarchChunkIndex::kResidentBit)!=0 && (AfterReset->Block&1)!=0);

    // Shape/epoch validation and coalescing use the same state primitive as
    // production, with tiny buffers to test every failure without 72MiB copies.
    using namespace VoxelMarchOrdered;
    FState State;
    auto Packet=[](uint64 Base,uint32 Value){FPacket P;P.Epoch=1;P.BaseGeneration=Base;P.Generation=Base+1;
        P.Delta=true;P.MaxDeltaCells=4;P.Occupied={Value};P.AnyAbsent={~Value};P.AllSky={0};
        if(Base==0)P.Full={Value,0,0,0};else P.Pairs={0,Value};return P;};
    TestTrue(TEXT("Full seed accepted"),State.Apply(Packet(0,1),4,1));
    State.FullPending=false;
    TestTrue(TEXT("First patch accepted"),State.Apply(Packet(1,2),4,1));
    TestTrue(TEXT("Second patch coalesces before view"),State.Apply(Packet(2,3),4,1));
    TestEqual(TEXT("Coalesced dirty cells are unique"),State.DirtyCells.Num(),1);
    TestEqual(TEXT("Newest cell and mirror agree"),State.Cells[0],State.Occupied[0]);
    auto Bad=Packet(3,4);Bad.Pairs={4,4};
    TestFalse(TEXT("Invalid patch refused before state mutation"),State.Apply(MoveTemp(Bad),4,1));
    TestEqual(TEXT("Rejected patch retains generation"),State.Generation,uint64(3));
    auto Full=Packet(3,5);Full.Full={5,0,0,0};Full.Pairs.Reset();
    TestTrue(TEXT("Full supersedes queued patches"),State.Apply(MoveTemp(Full),4,1));
    TestTrue(TEXT("Full upload required"),State.FullPending);
    TestEqual(TEXT("Full clears obsolete dirty set"),State.DirtyCells.Num(),0);
    State.Reset(2);
    TestFalse(TEXT("Retired epoch cannot alter replacement state"),State.Apply(Packet(0,6),4,1));
    auto Fresh=Packet(0,7);Fresh.Epoch=2;
    TestTrue(TEXT("Replacement epoch seeds normally"),State.Apply(MoveTemp(Fresh),4,1));

    // A queued production packet owns lifetime state, not a destroyed index.
    FEvent* Destroyed=FPlatformProcess::GetSynchEventFromPool(true);
    ENQUEUE_RENDER_COMMAND(MarchOrderedHoldUntilDestruction)([Destroyed](FRHICommandListImmediate&){Destroyed->Wait(10000);});
    auto Disposable=MakeUnique<FVoxelMarchChunkIndex>();
    Disposable->DebugSeedOrderedForTest(Seed);
    Disposable.Reset();
    Destroyed->Trigger();
    FlushRenderingCommands();
    FPlatformProcess::ReturnSynchEventToPool(Destroyed);
    return true;
}
#endif

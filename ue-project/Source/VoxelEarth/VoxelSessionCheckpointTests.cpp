#include "VoxelSessionCheckpoint.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelSaveLibrary.h"
#include "VoxelAgentSubsystem.h"
#include "VoxelPlayerRecords.h"
#include "VoxelEarthPlayerController.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Engine.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "voxelcore/waterca.h"
#include "voxelcore/basinledger.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSessionCheckpointTest,"Voxel.Persistence.SessionDomains",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelSessionCheckpointTest::RunTest(const FString&)
{
    // NullRHI strips the bathymetry render resource. This fixture exercises
    // scalar persistence, with exactly one known rendering diagnostic per world.
    if (FParse::Param(FCommandLine::Get(),TEXT("nullrhi")))
        AddExpectedError(TEXT("BathyField: /Game/Voxel/T_VoxelBathyInfo.T_VoxelBathyInfo is 0x0 PF_Unknown with 0 mip(s)"),
            EAutomationExpectedErrorFlags::Contains,3,false);
    struct FWorldScope
    {
        UWorld* W=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        ~FWorldScope() { if (W) W->DestroyWorld(false); }
    } Source,Target,Refused;
    if (!Source.W || !Target.W || !Refused.W) return false;
    VoxelSave::SetActiveSlug(Source.W,TEXT("source-world"));
    VoxelSave::SetActiveSlug(Target.W,TEXT("target-world"));
    TestEqual(TEXT("Source slot independent"),VoxelSave::GetActiveSlug(Source.W),FString(TEXT("source-world")));
    TestEqual(TEXT("Target slot independent"),VoxelSave::GetActiveSlug(Target.W),FString(TEXT("target-world")));
    VoxelSave::SetActiveSlug(Source.W,TEXT("../escape"));
    TestEqual(TEXT("Invalid slot cannot replace binding"),VoxelSave::GetActiveSlug(Source.W),FString(TEXT("source-world")));
    TestTrue(TEXT("Invalid directory is refused"),VoxelSave::SaveDirectory(TEXT("../escape")).IsEmpty());
    TestFalse(TEXT("Cannot delete active world"),VoxelSave::Delete(TEXT("target-world")));
    auto Water=Source.W->GetSubsystem<UVoxelWaterSubsystem>();
    auto Sky=Source.W->GetSubsystem<UVoxelSkySubsystem>();
    auto TargetWater=Target.W->GetSubsystem<UVoxelWaterSubsystem>();
    auto RefusedWater=Refused.W->GetSubsystem<UVoxelWaterSubsystem>();
    if (!TestTrue(TEXT("Game-world subsystems initialized"),Water && Sky && TargetWater && RefusedWater)) return false;
    VoxelCheckpointStore::FSimulationPayload Snapshot;
    TestFalse(TEXT("Capture blocked before session selection"),VoxelSessionCheckpoint::Capture(Source.W,Snapshot));
    TestTrue(TEXT("Fresh session selected"),VoxelSessionCheckpoint::Restore(Source.W,FString(),nullptr));
    auto SourcePlayer=Source.W->SpawnActor<AVoxelEarthPlayerController>();
    if(SourcePlayer) SourcePlayer->Player=NewObject<ULocalPlayer>(GEngine);
    if(!TestTrue(TEXT("Admit local player"),VoxelPlayerRecords::BindHost(SourcePlayer))) return false;
    auto SourceInventory=SourcePlayer->GetInventory();
    for(int32 I=0;I<SourceInventory->NumSlots();++I)
    {
        const int32 Count=SourceInventory->GetSlot(I).Count;
        if(Count>0) SourceInventory->TryRemoveFromSlot(I,Count);
    }
    SourceInventory->SetSelectedSlot(3);
    TestEqual(TEXT("Agents exist before checkpoint"),Source.W->GetSubsystem<UVoxelAgentSubsystem>()->SpawnSwarmAtOffset(2,FVector::ZeroVector,FVector(1,0,0),100,0),2);
    TArray<uint8> BaselineWater,BaselineHydro; double Remainder=0; bool Implicit=false;
    TestTrue(TEXT("Capture defaults"),Water->CaptureCheckpoint(BaselineWater,BaselineHydro,Remainder,Implicit));

    vxc::WaterMobilizer Mob([](int64_t,int64_t,int64_t){return uint8_t(0);},
        [](int64_t,int64_t,int64_t){return vxc::MAT_AIR;});
    vxc::WaterCA CA(Mob.makeSolidFn()); CA.addWater(0,0,1000,73);
    std::vector<uint8_t> Bytes,LedgerBytes,HydroBytes;
    vxc::WaterState::serialize(CA,Mob,Bytes);
    vxc::BasinLedger Ledger; Ledger.restoreDelta(vxc::BasinId{7},321); Ledger.restoreTotals();
    vxc::BasinLedgerState::serialize(Ledger,LedgerBytes);
    vxc::ByteWriter Writer(HydroBytes); Writer.u32(0x59485856); Writer.u32(1); Writer.u32(uint32(LedgerBytes.size()));
    HydroBytes.insert(HydroBytes.end(),LedgerBytes.begin(),LedgerBytes.end()); Writer.u32(0);
    TArray<uint8> WaterBytes,Hydrology;
    WaterBytes.Append(Bytes.data(),int32(Bytes.size())); Hydrology.Append(HydroBytes.data(),int32(HydroBytes.size()));
    TestTrue(TEXT("Install nonzero water and basin state"),Water->RestoreCheckpoint(WaterBytes,Hydrology,0.037,Implicit));
    Sky->SetEpochSeconds(123456.75);
    TestTrue(TEXT("Capture coordinated simulation"),VoxelSessionCheckpoint::Capture(Source.W,Snapshot));
    TestTrue(TEXT("Water serialized without loss"),Snapshot.Water==WaterBytes);
    TestTrue(TEXT("Basin ledger serialized without loss"),Snapshot.Hydrology.Num()>=12+int32(LedgerBytes.size()) && FMemory::Memcmp(Snapshot.Hydrology.GetData()+12,LedgerBytes.data(),LedgerBytes.size())==0);
    TArray<uint8> Terrain,Detached;
    TestTrue(TEXT("Capture real terrain"),Source.W->GetSubsystem<UVoxelWorldSubsystem>()->CaptureSaveSnapshot(Terrain,Detached));
    const FString Logical=FPaths::ProjectSavedDir()/TEXT("Tests/SessionDomains")/FGuid::NewGuid().ToString(EGuidFormats::Digits)/TEXT("world.vxlog");
    TestTrue(TEXT("Commit complete fixture"),VoxelCheckpointStore::Commit(Logical,Terrain,Detached,FString(),-1,&Snapshot));
    VoxelCheckpointStore::FResolved Resolved;
    if (!TestTrue(TEXT("Resolve committed fixture"),VoxelCheckpointStore::Resolve(Logical,Resolved))) return false;
    TestTrue(TEXT("Restore into independent same-seed world"),VoxelSessionCheckpoint::Restore(Target.W,Logical,&Resolved));
    auto TargetPlayer=Target.W->SpawnActor<AVoxelEarthPlayerController>();
    if(TargetPlayer) TargetPlayer->Player=NewObject<ULocalPlayer>(GEngine);
    TestTrue(TEXT("Admit restored player"),VoxelPlayerRecords::BindHost(TargetPlayer));
    TestEqual(TEXT("Empty inventory is not reseeded"),TargetPlayer->GetInventory()->GetSlot(0).Count,0);
    TestEqual(TEXT("Selected slot restored"),TargetPlayer->GetInventory()->GetSelectedSlot(),3);
    VoxelCheckpointStore::FSimulationPayload Reloaded;
    TestTrue(TEXT("Recapture restored world"),VoxelSessionCheckpoint::Capture(Target.W,Reloaded));
    TestTrue(TEXT("Water round trip"),Reloaded.Water==Snapshot.Water);
    TestTrue(TEXT("Hydrology round trip"),Reloaded.Hydrology==Snapshot.Hydrology);
    TestTrue(TEXT("Clock and fixed-step remainder round trip"),Reloaded.Clock==Snapshot.Clock);
    TestTrue(TEXT("World identity and agents round trip"),Reloaded.Gameplay==Snapshot.Gameplay);
    TestEqual(TEXT("Restored agents installed before ready"),Target.W->GetSubsystem<UVoxelAgentSubsystem>()->GetAgentCount(),2);
    double Epoch=0; Target.W->GetSubsystem<UVoxelSkySubsystem>()->CaptureEpochSeconds(Epoch);
    TestEqual(TEXT("Exact epoch without offline catch-up"),Epoch,123456.75);

    TArray<uint8> Corrupt=Hydrology; Corrupt.Add(0);
    TestFalse(TEXT("Bad hydro refused before installing water"),RefusedWater->RestoreCheckpoint(WaterBytes,Corrupt,0,Implicit));
    TArray<uint8> AfterWater,AfterHydro;
    TestTrue(TEXT("Inspect refused target"),RefusedWater->CaptureCheckpoint(AfterWater,AfterHydro,Remainder,Implicit));
    TestTrue(TEXT("Refused load left water unchanged"),AfterWater==BaselineWater);
    TestTrue(TEXT("Refused load left ledger unchanged"),AfterHydro==BaselineHydro);
    auto TargetSky=Target.W->GetSubsystem<UVoxelSkySubsystem>();
    TestTrue(TEXT("Restore world-local clock settings"),TargetSky->RestoreClock(500,2,700,300));
    double ClockRate=0,ClockDay=0,ClockYear=0;
    TestTrue(TEXT("Capture world-local settings"),TargetSky->CaptureClock(Epoch,ClockRate,ClockDay,ClockYear));
    TestEqual(TEXT("Saved clock rate"),ClockRate,2.0);
    TestEqual(TEXT("Saved day length"),ClockDay,700.0);
    TestEqual(TEXT("Saved year length"),ClockYear,300.0);
    if (auto Enabled=IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Sky.Enabled")))
    {
        const int32 OldValue=Enabled->GetInt();
        const auto OldFlags=Enabled->GetFlags();
        Enabled->Set(0,ECVF_SetByCode);
        TestTrue(TEXT("Hidden sky retains simulation tick"),TargetSky->IsTickable());
        TargetSky->Tick(0.25f);
        TargetSky->CaptureEpochSeconds(Epoch);
        TestEqual(TEXT("Hidden sky advances clock"),Epoch,500.5);
        Enabled->Set(OldValue,ECVF_SetByCode);
        Enabled->ClearFlags(ECVF_SetByMask);
        Enabled->SetFlags(EConsoleVariableFlags(OldFlags & ECVF_SetByMask));
    }
    auto SourceAgents=Source.W->GetSubsystem<UVoxelAgentSubsystem>();
    auto TargetAgents=Refused.W->GetSubsystem<UVoxelAgentSubsystem>();
    if (!TestTrue(TEXT("Agent adapters initialized"),SourceAgents && TargetAgents)) return false;
    TestEqual(TEXT("Spawn checkpoint agents"),SourceAgents->SpawnSwarmAtOffset(2,FVector::ZeroVector,FVector(1,0,0),100,0),2);
    TArray<uint8> AgentBytes,AgentReloaded;
    TestTrue(TEXT("Capture agents"),SourceAgents->CaptureCheckpoint(AgentBytes));
    TestTrue(TEXT("Restore agents"),TargetAgents->RestoreCheckpoint(AgentBytes));
    TestTrue(TEXT("Recapture agents"),TargetAgents->CaptureCheckpoint(AgentReloaded));
    TestTrue(TEXT("Stable agent IDs and state round trip"),AgentBytes==AgentReloaded);
    AgentBytes.Add(0); TArray<FVoxelAgent> InvalidAgents;
    TestFalse(TEXT("Agent trailing bytes refused"),UVoxelAgentSubsystem::DecodeCheckpoint(AgentBytes,InvalidAgents));
    TestEqual(TEXT("Invalid agent decode publishes no records"),InvalidAgents.Num(),0);
    VoxelSessionCheckpoint::Fail(Source.W);
    TestFalse(TEXT("Failed session cannot publish under another name"),VoxelSessionCheckpoint::Capture(Source.W,Reloaded));
    return true;
}
#endif

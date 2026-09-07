#include "VoxelSessionCheckpoint.h"
#include "VoxelWaterSubsystem.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"
#include "VoxelDetachedPersistence.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
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
    auto Water=Source.W->GetSubsystem<UVoxelWaterSubsystem>();
    auto Sky=Source.W->GetSubsystem<UVoxelSkySubsystem>();
    auto TargetWater=Target.W->GetSubsystem<UVoxelWaterSubsystem>();
    auto RefusedWater=Refused.W->GetSubsystem<UVoxelWaterSubsystem>();
    if (!TestTrue(TEXT("Game-world subsystems initialized"),Water && Sky && TargetWater && RefusedWater)) return false;
    VoxelCheckpointStore::FSimulationPayload Snapshot;
    TestFalse(TEXT("Capture blocked before session selection"),VoxelSessionCheckpoint::Capture(Source.W,Snapshot));
    TestTrue(TEXT("Fresh session selected"),VoxelSessionCheckpoint::Restore(Source.W,FString(),nullptr));
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
    VoxelCheckpointStore::FSimulationPayload Reloaded;
    TestTrue(TEXT("Recapture restored world"),VoxelSessionCheckpoint::Capture(Target.W,Reloaded));
    TestTrue(TEXT("Water round trip"),Reloaded.Water==Snapshot.Water);
    TestTrue(TEXT("Hydrology round trip"),Reloaded.Hydrology==Snapshot.Hydrology);
    TestTrue(TEXT("Clock and fixed-step remainder round trip"),Reloaded.Clock==Snapshot.Clock);
    double Epoch=0; Target.W->GetSubsystem<UVoxelSkySubsystem>()->CaptureEpochSeconds(Epoch);
    TestEqual(TEXT("Exact epoch without offline catch-up"),Epoch,123456.75);

    TArray<uint8> Corrupt=Hydrology; Corrupt.Add(0);
    TestFalse(TEXT("Bad hydro refused before installing water"),RefusedWater->RestoreCheckpoint(WaterBytes,Corrupt,0,Implicit));
    TArray<uint8> AfterWater,AfterHydro;
    TestTrue(TEXT("Inspect refused target"),RefusedWater->CaptureCheckpoint(AfterWater,AfterHydro,Remainder,Implicit));
    TestTrue(TEXT("Refused load left water unchanged"),AfterWater==BaselineWater);
    TestTrue(TEXT("Refused load left ledger unchanged"),AfterHydro==BaselineHydro);
    VoxelSessionCheckpoint::Fail(Source.W);
    TestFalse(TEXT("Failed session cannot publish under another name"),VoxelSessionCheckpoint::Capture(Source.W,Reloaded));
    return true;
}
#endif

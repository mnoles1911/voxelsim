#include "VoxelCheckpointStore.h"
#include "VoxelDetachedPersistence.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCheckpointTransactionTest,"Voxel.Persistence.CheckpointTransaction",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelCheckpointTransactionTest::RunTest(const FString&)
{
    // Retain isolated artifacts for diagnosis; never use a player's slot.
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("Tests/Checkpoints")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*Directory,true);
    const FString Logical=Directory/TEXT("world.vxlog");
    const TArray<uint8> Terrain{1,2,3};
    TestTrue(TEXT("Legacy fixture"),FFileHelper::SaveArrayToFile(Terrain,*Logical));
    VoxelCheckpointStore::FResolved Before;
    TestTrue(TEXT("Legacy resolution"),VoxelCheckpointStore::Resolve(Logical,Before));
    TestFalse(TEXT("Legacy is not a checkpoint"),Before.bCheckpoint);
    VoxelDetachedPersistence::FSnapshot Objects;
    TArray<uint8> Detached;
    TestTrue(TEXT("Encode empty objects"),VoxelDetachedPersistence::EncodeSnapshot(Objects,Detached));
    TestTrue(TEXT("First commit"),VoxelCheckpointStore::Commit(Logical,Terrain,Detached,TEXT("{\"revision\":1}")));
    TestTrue(TEXT("Resolve first"),VoxelCheckpointStore::Resolve(Logical,Before));
    TestTrue(TEXT("Checkpoint selected"),Before.bCheckpoint);
    // Change only objects and metadata: this used to overwrite the old sidecar.
    VoxelObjects::FEntry Tombstone; Tombstone.Id=FGuid::NewGuid(); Tombstone.Kind=1;
    Tombstone.Residency=VoxelObjects::EResidency::Tombstone; Objects.Add(Tombstone);
    TestTrue(TEXT("Encode changed objects"),VoxelDetachedPersistence::EncodeSnapshot(Objects,Detached));
    for (int32 Stage=0; Stage<6; ++Stage)
    {
        TestFalse(FString::Printf(TEXT("Interrupted stage %d"),Stage),VoxelCheckpointStore::Commit(Logical,Terrain,Detached,TEXT("{\"revision\":2}"),Stage));
        VoxelCheckpointStore::FResolved After;
        TestTrue(TEXT("Resolve after interrupted commit"),VoxelCheckpointStore::Resolve(Logical,After));
        TestEqual(TEXT("Prior generation remains current"),After.TerrainPath,Before.TerrainPath);
    }
    TestTrue(TEXT("Object-only commit"),VoxelCheckpointStore::Commit(Logical,Terrain,Detached,TEXT("{\"revision\":2}")));
    VoxelCheckpointStore::FResolved Current;
    TestTrue(TEXT("Resolve second"),VoxelCheckpointStore::Resolve(Logical,Current));
    TestNotEqual(TEXT("New immutable path even for identical terrain"),Current.TerrainPath,Before.TerrainPath);
    FString Meta;
    FFileHelper::LoadFileToString(Meta,*Before.MetadataPath);
    TestEqual(TEXT("Old metadata untouched"),Meta,FString(TEXT("{\"revision\":1}")));
    TArray<uint8> Legacy; FFileHelper::LoadFileToArray(Legacy,*Logical);
    TestTrue(TEXT("Legacy bytes untouched"),Legacy==Terrain);
    // A missing required object sidecar invalidates the entire new generation.
    TArray<FString> Sidecars;
    IFileManager::Get().FindFiles(Sidecars,*(FPaths::GetPath(Current.TerrainPath)/TEXT("*.bin")),true,false);
    if (!TestEqual(TEXT("Exactly one sidecar"),Sidecars.Num(),1)) return false;
    const FString Sidecar=FPaths::GetPath(Current.TerrainPath)/Sidecars[0];
    TestTrue(TEXT("Remove required sidecar"),IFileManager::Get().Delete(*Sidecar));
    VoxelCheckpointStore::FResolved Recovered;
    TestTrue(TEXT("Recovery succeeds"),VoxelCheckpointStore::Resolve(Logical,Recovered));
    TestTrue(TEXT("Recovery reported"),Recovered.bRecovered);
    TestEqual(TEXT("Whole previous generation recovered"),Recovered.TerrainPath,Before.TerrainPath);
    TestTrue(TEXT("Damage last complete generation"),FFileHelper::SaveArrayToFile(TArray<uint8>{9},*Before.TerrainPath));
    TestFalse(TEXT("No silent legacy fallback after committed data loss"),VoxelCheckpointStore::Resolve(Logical,Recovered));
    TestFalse(TEXT("Refuse overwrite of damaged slot"),VoxelCheckpointStore::Commit(Logical,Terrain,Detached));
    const FString Inherited=Directory/TEXT("inherit.vxlog");
    TestTrue(TEXT("Metadata fixture"),VoxelCheckpointStore::Commit(Inherited,Terrain,Detached,TEXT("{\"revision\":3}")));
    TestTrue(TEXT("World-only save preserves metadata"),VoxelCheckpointStore::Commit(Inherited,Terrain,Detached));
    TestTrue(TEXT("Resolve inherited metadata"),VoxelCheckpointStore::Resolve(Inherited,Current));
    FFileHelper::LoadFileToString(Meta,*Current.MetadataPath);
    TestEqual(TEXT("Metadata inherited from committed generation"),Meta,FString(TEXT("{\"revision\":3}")));
    TestFalse(TEXT("Invalid metadata cannot publish"),VoxelCheckpointStore::Commit(Inherited,Terrain,Detached,TEXT("{")));

    const FString Complete=Directory/TEXT("simulation.vxlog");
    VoxelCheckpointStore::FSimulationPayload Simulation;
    Simulation.Water={11}; Simulation.Hydrology={12}; Simulation.Clock={13};
    TestTrue(TEXT("Publish all simulation domains"),VoxelCheckpointStore::Commit(Complete,Terrain,Detached,TEXT("{\"revision\":1}"),-1,&Simulation));
    TestTrue(TEXT("Resolve simulation checkpoint"),VoxelCheckpointStore::Resolve(Complete,Before));
    TestTrue(TEXT("Simulation domains required"),Before.bSimulation);
    Simulation.Water={21}; Simulation.Hydrology={22}; Simulation.Clock={23};
    for (int32 Stage=0; Stage<9; ++Stage)
    {
        TestFalse(FString::Printf(TEXT("Interrupted simulation stage %d"),Stage),VoxelCheckpointStore::Commit(Complete,Terrain,Detached,TEXT("{\"revision\":2}"),Stage,&Simulation));
        TestTrue(TEXT("Resolve interrupted simulation"),VoxelCheckpointStore::Resolve(Complete,Current));
        TestEqual(TEXT("No mixed generation after interruption"),Current.TerrainPath,Before.TerrainPath);
    }
    TestFalse(TEXT("Old writer cannot drop simulation"),VoxelCheckpointStore::Commit(Complete,Terrain,Detached));
    for (int32 Domain=0; Domain<3; ++Domain)
    {
        TestTrue(TEXT("Publish next simulation"),VoxelCheckpointStore::Commit(Complete,Terrain,Detached,TEXT("{\"revision\":2}"),-1,&Simulation));
        TestTrue(TEXT("Resolve new simulation"),VoxelCheckpointStore::Resolve(Complete,Current));
        const FString Missing=Domain==0?Current.WaterPath:(Domain==1?Current.HydrologyPath:Current.ClockPath);
        TestTrue(TEXT("Remove required simulation domain"),IFileManager::Get().Delete(*Missing));
        TestTrue(TEXT("Recover entire previous simulation"),VoxelCheckpointStore::Resolve(Complete,Recovered));
        TestEqual(TEXT("Terrain recovered with simulation"),Recovered.TerrainPath,Before.TerrainPath);
        TArray<uint8> Water,Hydrology,Clock;
        TestTrue(TEXT("Read recovered water"),FFileHelper::LoadFileToArray(Water,*Recovered.WaterPath));
        TestTrue(TEXT("Read recovered hydrology"),FFileHelper::LoadFileToArray(Hydrology,*Recovered.HydrologyPath));
        TestTrue(TEXT("Read recovered clock"),FFileHelper::LoadFileToArray(Clock,*Recovered.ClockPath));
        TestTrue(TEXT("Original water bytes"),Water==TArray<uint8>{11});
        TestTrue(TEXT("Original hydrology bytes"),Hydrology==TArray<uint8>{12});
        TestTrue(TEXT("Original clock bytes"),Clock==TArray<uint8>{13});
    }
    Simulation.Hydrology.Empty();
    TestFalse(TEXT("Empty domain cannot publish"),VoxelCheckpointStore::Commit(Complete,Terrain,Detached,FString(),-1,&Simulation));
    return true;
}
#endif

#include "VoxelDetachedPersistence.h"
#include "VoxelDebris.h"
#include "VoxelEarth.h"
#include "VoxelSaveGuard.h"
#include "Engine/World.h"
#include "Components/PrimitiveComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

namespace {
bool RunObjectWorldProbe(FString& Failure)
{
    using namespace VoxelObjects;
    using namespace VoxelDetachedPersistence;
    // These worlds have no engine world context and cannot alter the open game.
    UWorld* Source=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("ObjectProbe_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    UWorld* Target=nullptr;
    auto Cleanup=[&](){if(Source){Forget(Source);Source->DestroyWorld(false);}if(Target){Forget(Target);Target->DestroyWorld(false);}};
    auto Fail=[&](const TCHAR* Why){Failure=Why;Cleanup();return false;};
    if(!Source)return Fail(TEXT("create source world"));
    auto D=Source->SpawnActor<AVoxelDebris>();if(!D)return Fail(TEXT("spawn real debris"));
    TArray<VoxelCoords::FVoxelCoord> Cells;for(int32 I=0;I<9;++I)Cells.Add({I,0,100});D->InitFromIsland(Cells);
    auto Body=Cast<UPrimitiveComponent>(D->GetRootComponent());if(Body)Body->SetSimulatePhysics(false);
    auto Life=D->FindComponentByClass<UVoxelDebrisLifecycle>();FVoxelDebrisLifetimeState Lifetime;
    Lifetime.Kind=EVoxelDebrisLifetime::Harvestable;Lifetime.RemainingSeconds=123.;Life->RestoreState(Lifetime);
    auto& R=Get(Source);const FGuid Id=R.Bind(D,1);auto Entry=R.Find(Id);
    if(!Entry||!CaptureObject(*Entry)||!Entry->Geometry)return Fail(TEXT("capture real debris"));
    const auto OriginalGeometry=Entry->Geometry;const FVector Position=Entry->Transform.GetLocation();
    if(!CaptureObject(*Entry)||Entry->Geometry!=OriginalGeometry)return Fail(TEXT("unchanged actor did not reuse immutable geometry"));
    FCallbacks Callbacks;Callbacks.Capture=&CaptureObject;Callbacks.Evict=[](AActor* A){A->Destroy();};
    Callbacks.Restore=[Source](const FEntry& E){return RestoreObject(Source,E);};
    TArray<FView> Views;Views.Add({Position+FVector(30000.,0.,0.),FVector::ForwardVector});
    auto Away=R.Tick(Views,0.,Callbacks);
    if(Away.Evicted!=1||R.Find(Id)->Residency!=EResidency::Dormant||R.Find(Id)->Actor.IsValid())return Fail(TEXT("stream out real debris"));
    R.Tick(Views,10.,Callbacks);
    if(!FMath::IsNearlyEqual(R.Find(Id)->Lifetime.RemainingSeconds,113.,.001))return Fail(TEXT("dormant timer"));
    Views[0].Position=Position;
    auto Back=R.Tick(Views,0.,Callbacks);auto Restored=R.Find(Id);
    if(Back.Restored!=1||!Restored->Actor.IsValid()||Restored->Id!=Id||Restored->Geometry!=OriginalGeometry)return Fail(TEXT("restore stable identity and geometry"));
    auto RestoredLife=Restored->Actor->FindComponentByClass<UVoxelDebrisLifecycle>();
    if(!RestoredLife||!FMath::IsNearlyEqual(RestoredLife->CaptureState().RemainingSeconds,113.,.001))return Fail(TEXT("restore remaining lifetime"));
    FSnapshot Frozen=R.Snapshot();TArray<uint8> Payload;
    if(!EncodeSnapshot(Frozen,Payload))return Fail(TEXT("encode actual object"));
    const FString Path=FPaths::ProjectSavedDir()/TEXT("Tests/object-stream-v4-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".vxlog");
    TArray<uint8> Terrain;Terrain.Add(0x71);Terrain.Add(0x34);
    if(!WritePayload(Path,Terrain,Payload))return Fail(TEXT("write actual v4 sidecar"));
    const FString Sidecar=Path+TEXT(".detached-")+FMD5::HashBytes(Terrain.GetData(),Terrain.Num())+TEXT(".bin");
    TArray<uint8> Disk;if(!FFileHelper::LoadFileToArray(Disk,*Sidecar)||Disk.Num()<8)return Fail(TEXT("read actual v4 sidecar"));
    uint32 Version=0;FMemory::Memcpy(&Version,Disk.GetData()+4,4);if(Version!=4)return Fail(TEXT("disk container is not v4"));
    Target=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("ObjectReload_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
    if(!Target||!Load(Target,Path,Terrain))return Fail(TEXT("load actual v4 sidecar"));
    auto& Loaded=Get(Target);auto Saved=Loaded.Find(Id);
    if(!Saved||Saved->Revision!=Frozen[0].Revision||Saved->Residency!=EResidency::Dormant||!Saved->Geometry||*Saved->Geometry!=*OriginalGeometry)return Fail(TEXT("reload stable identity revision and geometry"));
    Callbacks.Restore=[Target](const FEntry& E){return RestoreObject(Target,E);};
    auto LoadedTick=Loaded.Tick(Views,0.,Callbacks);
    if(LoadedTick.Restored!=1||!Loaded.Find(Id)->Actor.IsValid())return Fail(TEXT("materialize loaded object"));
    RestoredLife=Loaded.Find(Id)->Actor->FindComponentByClass<UVoxelDebrisLifecycle>();
    if(!RestoredLife||!FMath::IsNearlyEqual(RestoredLife->CaptureState().RemainingSeconds,113.,.001))return Fail(TEXT("loaded lifetime"));
    const FString BadPath=Path+TEXT(".corrupt");const FString BadSidecar=BadPath+TEXT(".detached-")+FMD5::HashBytes(Terrain.GetData(),Terrain.Num())+TEXT(".bin");
    Disk[8]^=1;
    if(!FFileHelper::SaveArrayToFile(Disk,*BadSidecar)||Load(Target,BadPath,Terrain)||!VoxelSaveGuard::IsQuarantined(BadPath)||Loaded.Num()!=1||!Loaded.Find(Id)->Actor.IsValid())return Fail(TEXT("corrupt disk snapshot did not preserve live registry"));
    Cleanup();return true;
}
FAutoConsoleCommand ObjectProbe(TEXT("voxel.Objects.Probe"),TEXT("Exercise isolated real-debris streaming and v4 disk reload without modifying the active world."),
    FConsoleCommandDelegate::CreateLambda([](){FString Why;const bool Pass=RunObjectWorldProbe(Why);UE_LOG(LogVoxelEarth,Log,TEXT("ObjectPersistence PROBE %s reason=%s"),Pass?TEXT("PASS"):TEXT("FAIL"),*Why);}));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectSnapshotTest,"Voxel.Objects.Snapshot",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectSnapshotTest::RunTest(const FString&)
{
    using namespace VoxelObjects;using namespace VoxelDetachedPersistence;
    FEntry E;E.Id=FGuid::NewGuid();E.Kind=1;E.Revision=42;E.GeometryRevision=7;E.OwnerId=FGuid::NewGuid();E.bRetained=true;
    E.Transform=FTransform(FRotator(0.,45.,0.),FVector(-123.,456.,789.));E.Lifetime.RemainingSeconds=123.;
    TArray<uint8> Geometry;Geometry.Add(11);Geometry.Add(22);E.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>(MoveTemp(Geometry));E.Dynamic.Add(33);
    FEntry Dead;Dead.Id=FGuid::NewGuid();Dead.Kind=2;Dead.Revision=99;Dead.Residency=EResidency::Tombstone;
    FSnapshot Input;Input.Add(E);Input.Add(Dead);TArray<uint8> Payload;FSnapshot Output;
    TestTrue(TEXT("Encode v4"),EncodeSnapshot(Input,Payload));TestTrue(TEXT("Decode v4"),DecodeSnapshot(Payload,Output));
    if(Output.Num()!=2){AddError(TEXT("record count"));return false;}
    TestEqual(TEXT("Stable GUID"),Output[0].Id,E.Id);TestEqual(TEXT("Object revision"),Output[0].Revision,uint64(42));
    TestEqual(TEXT("Geometry revision"),Output[0].GeometryRevision,uint64(7));TestEqual(TEXT("Owner"),Output[0].OwnerId,E.OwnerId);
    TestTrue(TEXT("Retained"),Output[0].bRetained);TestTrue(TEXT("Transform preserved"),Output[0].Transform.Equals(E.Transform));
    TestTrue(TEXT("Exact geometry bytes"),Output[0].Geometry&&*Output[0].Geometry==*E.Geometry);TestTrue(TEXT("Exact dynamic bytes"),Output[0].Dynamic==E.Dynamic);
    TestTrue(TEXT("Tombstone preserved"),Output[1].Residency==EResidency::Tombstone);
    auto Short=Payload;Short.Pop();TestFalse(TEXT("Truncation refused"),DecodeSnapshot(Short,Output));
    auto Trailing=Payload;Trailing.Add(0);TestFalse(TEXT("Trailing bytes refused"),DecodeSnapshot(Trailing,Output));
    auto BadMarker=Payload;BadMarker[0]^=0x7f;TestFalse(TEXT("Bad marker refused"),DecodeSnapshot(BadMarker,Output));
    Input[1]=Input[0];EncodeSnapshot(Input,Short);TestFalse(TEXT("Duplicate GUID refused"),DecodeSnapshot(Short,Output));
    Input.SetNum(1);Input[0].Revision=0;EncodeSnapshot(Input,Short);TestFalse(TEXT("Zero revision refused"),DecodeSnapshot(Short,Output));
    Input[0]=E;Input[0].Geometry.Reset();EncodeSnapshot(Input,Short);TestFalse(TEXT("Missing live geometry refused"),DecodeSnapshot(Short,Output));
    Input[0]=E;Input[0].Lifetime.RemainingSeconds=-1.;EncodeSnapshot(Input,Short);TestFalse(TEXT("Invalid lifetime refused"),DecodeSnapshot(Short,Output));
    FRegistry Registry;Registry.Import(E);const auto Frozen=Registry.Snapshot();TArray<uint8> Other;Other.Add(99);Registry.PublishGeometry(E.Id,MoveTemp(Other));
    TestTrue(TEXT("Worker snapshot shares original geometry"),Frozen[0].Geometry==E.Geometry);
    TestEqual(TEXT("Worker snapshot unaffected by edit"),int32((*Frozen[0].Geometry)[0]),11);
    TestEqual(TEXT("Worker snapshot revision unchanged"),Frozen[0].Revision,uint64(42));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectWorldTest,"Voxel.Objects.Integrated",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectWorldTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("SaveGuard: AUTOSAVE DISABLED"),EAutomationExpectedErrorFlags::Contains,2);
    FString Failure;const bool Pass=RunObjectWorldProbe(Failure);if(!Pass)AddError(Failure);return Pass;
}
#endif

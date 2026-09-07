#if WITH_DEV_AUTOMATION_TESTS
#include "VoxelObjectPages.h"
#include "VoxelDetachedPersistence.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelObjectPagesTest,"Voxel.Objects.Pages",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelObjectPagesTest::RunTest(const FString&)
{
    using namespace VoxelObjects;using namespace VoxelObjectPages;
    TArray<uint8> Bytes;Bytes.SetNumUninitialized(65536);for(int32 I=0;I<Bytes.Num();++I)Bytes[I]=uint8(I*17);
    FEntry E;E.Id=FGuid::NewGuid();E.Kind=1;E.GeometryRevision=3;E.Geometry=MakeShared<const TArray<uint8>,ESPMode::ThreadSafe>(MoveTemp(Bytes));
    FRegistry Registry;Registry.Import(E);const auto Frozen=Registry.Snapshot();
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("Tests/ObjectPages")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    auto WriteJob=Async(EAsyncExecution::ThreadPool,[Directory,E](){FGeometryPage Page;Write(Directory,E.Geometry,Page);return Page;});
    const auto Page=WriteJob.Get();TestTrue(TEXT("Worker publishes verified compressed page"),Page.IsValid());
    if(!Page.IsValid())return false;
    TestTrue(TEXT("Durable write releases resident registry geometry"),PublishWrite(Registry,E.Id,3,E.Geometry,Page));
    TestFalse(TEXT("Dormant payload is disk backed"),Registry.Find(E.Id)->Geometry.IsValid());
    TestTrue(TEXT("In-flight immutable snapshot remains usable"),Frozen[0].Geometry==E.Geometry);
    FEntry DiskSnapshot=*Registry.Find(E.Id);
    auto PortableJob=Async(EAsyncExecution::ThreadPool,[DiskSnapshot,E](){
        VoxelDetachedPersistence::FSnapshot Input;Input.Add(DiskSnapshot);TArray<uint8> Payload;
        VoxelDetachedPersistence::FSnapshot Output;
        return VoxelDetachedPersistence::EncodeSnapshot(Input,Payload)&&VoxelDetachedPersistence::DecodeSnapshot(Payload,Output)&&
            Output.Num()==1&&Output[0].Id==E.Id&&Output[0].Geometry&&*Output[0].Geometry==*E.Geometry&&!Output[0].Page.IsValid();
    });TestTrue(TEXT("Page-only snapshot becomes portable self-contained save bytes"),PortableJob.Get());
    auto ReadJob=Async(EAsyncExecution::ThreadPool,[DiskSnapshot]()mutable{Hydrate(DiskSnapshot);return DiskSnapshot.Geometry;});
    const auto Loaded=ReadJob.Get();TestTrue(TEXT("Worker snapshot hydration exact bytes"),Loaded&&*Loaded==*E.Geometry);
    TestTrue(TEXT("Hydration publishes current geometry"),PublishRead(Registry,E.Id,3,Page,Loaded));
    TArray<uint8> Edited;Edited.Add(42);Registry.PublishGeometry(E.Id,MoveTemp(Edited));
    TestFalse(TEXT("Stale completed write cannot evict edited bytes"),PublishWrite(Registry,E.Id,3,E.Geometry,Page));
    TestFalse(TEXT("Edit invalidates old disk reference"),Registry.Find(E.Id)->Page.IsValid());
    Registry.Remove(E.Id);TestFalse(TEXT("Completed read cannot resurrect deletion"),PublishRead(Registry,E.Id,3,Page,Loaded));
    auto CorruptJob=Async(EAsyncExecution::ThreadPool,[Page](){
        TArray<uint8> File;if(!FFileHelper::LoadFileToArray(File,*Page.Path)||File.Num()<16)return false;File[12]^=1;
        if(!FFileHelper::SaveArrayToFile(File,*Page.Path))return false;
        VoxelObjects::FGeometry Value;return !Read(Page,Value)&&!Value;
    });TestTrue(TEXT("Corrupt page rejected before materialization"),CorruptJob.Get());
    auto CorruptSaveJob=Async(EAsyncExecution::ThreadPool,[DiskSnapshot](){
        VoxelDetachedPersistence::FSnapshot Input;Input.Add(DiskSnapshot);TArray<uint8> Payload;
        return !VoxelDetachedPersistence::EncodeSnapshot(Input,Payload);
    });TestTrue(TEXT("Corrupt referenced page refuses save encoding"),CorruptSaveJob.Get());
    auto FailureJob=Async(EAsyncExecution::ThreadPool,[Directory,E](){
        const FString Blocker=Directory/TEXT("not-a-directory");TArray<uint8> B;B.Add(1);
        if(!FFileHelper::SaveArrayToFile(B,*Blocker))return false;
        FGeometryPage Invalid;return !Write(Blocker/TEXT("child"),E.Geometry,Invalid)&&!Invalid.IsValid();
    });TestTrue(TEXT("Failed write does not publish a disk reference"),FailureJob.Get());
    TestTrue(TEXT("Failed IO retains original immutable data"),E.Geometry&&E.Geometry->Num()==65536);
    return true;
}
#endif

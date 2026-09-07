#include "VoxelEnvironmentAsset.h"
#include "VoxelEnvironmentLODPrototype.h"
#include "VoxelDetachedPersistence.h"
#include "Engine/World.h"
#include "Misc/SecureHash.h"
#include "voxelcore/assetgrid.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace {
// Synthetic source data tests actor identity/scale plumbing. Reader correctness
// is independently covered by voxel-core's real Asset Forge fixtures.
TArray<uint8> EnvironmentFixture(uint32 Pitch,bool Micrometres,uint8 Material){
    TArray<uint8> Out;Out.Append({'V','X','A','1'});
    auto U32=[&](uint32 V){for(int I=0;I<4;++I)Out.Add(uint8(V>>(I*8)));};
    U32(Micrometres?4:3);U32(0);U32(0);U32(0);U32(4);U32(4);U32(4);
    U32(Pitch);U32(1);U32(0);U32(0);Out.Add(Material);U32(64);return Out;
}
struct FGeneralRestoreStatus {bool Called=false,Success=false;};
class FGeneralEnvironmentProbe final : public IAutomationLatentCommand {
    FAutomationTestBase* Test;
    UWorld* World=nullptr;
    AVoxelEnvironmentLODPrototype* Source=nullptr;
    AVoxelEnvironmentLODPrototype* Restored=nullptr;
    FVoxelEnvironmentAssetDescriptor Expected;
    FVoxelImmutableGeometry DecodedGeometry;
    TSharedPtr<FGeneralRestoreStatus> Status;
    int32 Case=0,ExpectedLevels=0;
    bool Collision=false;
    double Started=0;
    void Cleanup(){if(World){VoxelObjects::Forget(World);World->DestroyWorld(false);World=nullptr;}}
    void DescriptorEquals(const FVoxelEnvironmentAssetDescriptor& Actual){
        Test->TestEqual(TEXT("arbitrary spec ID retained"),Actual.SpecId,Expected.SpecId);
        Test->TestEqual(TEXT("asset kind retained"),Actual.Kind,Expected.Kind);
        Test->TestEqual(TEXT("category retained"),Actual.Category,Expected.Category);
        Test->TestEqual(TEXT("source VXA digest retained"),Actual.SourceHash,Expected.SourceHash);
        Test->TestEqual(TEXT("spec hash retained"),Actual.SpecHash,Expected.SpecHash);
        Test->TestEqual(TEXT("catalog hash retained"),Actual.CatalogHash,Expected.CatalogHash);
        Test->TestEqual(TEXT("optional provider hash retained"),Actual.ProviderHash,Expected.ProviderHash);
        Test->TestEqual(TEXT("unsigned seed index retained"),Actual.SeedIndex,Expected.SeedIndex);
        Test->TestEqual(TEXT("fellable capability retained"),Actual.Fellable,Expected.Fellable);
        Test->TestFalse(TEXT("arbitrary identity stays generic"),Actual.Legacy);
    }
public:
    explicit FGeneralEnvironmentProbe(FAutomationTestBase* In):Test(In){}
    virtual bool Update() override {
        if(!World){
            Started=FPlatformTime::Seconds();
            World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FString::Printf(TEXT("GeneralEnvironment_%s"),*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
            if(!World){Test->AddError(TEXT("create isolated environment test world"));return true;}
            FVoxelEnvironmentAssetDescriptor D;D.SpecId=TEXT("independent-test-tree-91");D.Kind=TEXT("tree");D.Category=TEXT("environment");D.Fellable=true;
            auto Invalid=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Invalid){Test->AddError(TEXT("spawn validation actor"));Cleanup();return true;}
            const auto FineFixture=EnvironmentFixture(12500,true,16);vxc::AssetGrid FineSource;
            Test->TestTrue(TEXT("12.5 mm fixture is valid VXA"),FineSource.parse(FineFixture.GetData(),FineFixture.Num())==vxc::AssetParseError::kOk);
            Test->TestEqual(TEXT("fixture preserves 12.5 mm pitch"),FineSource.voxelSizeMm(),12.5);
            Test->TestFalse(TEXT("12.5 mm environment source rejected"),Invalid->InitializeAssetFromVxa(D,FineFixture,true,false));
            D.SourceHash=FString::ChrN(32,TCHAR('0'));
            Test->TestFalse(TEXT("mismatching source content hash rejected"),Invalid->InitializeAssetFromVxa(D,EnvironmentFixture(50,false,16),true,false));
            D.SourceHash.Reset();D.Category=TEXT("craftables");
            Test->TestFalse(TEXT("non-environment category rejected"),Invalid->InitializeAssetFromVxa(D,EnvironmentFixture(50,false,16),true,false));
            Invalid->Destroy();
        }
        if(FPlatformTime::Seconds()-Started>60){Test->AddError(TEXT("generic environment restore timed out"));Cleanup();return true;}
        if(!Restored){
            const TCHAR* Kinds[]={TEXT("tree"),TEXT("bush"),TEXT("rock"),TEXT("grass"),TEXT("reed"),TEXT("flower")};
            const TCHAR* Names[]={TEXT("coastal-alder-var17"),TEXT("salal-wet-slope-seed4"),TEXT("glacial-granite-fracture-b"),TEXT("bluejoint-tuft-var9"),TEXT("riverbank-reed-bed9"),TEXT("fireweed-late-summer-v2")};
            const uint32 Pitches[]={25,50,100};const uint8 Materials[]={16,18,5,8,8,8};
            const int32 Family=Case/3,PitchIndex=Case%3;
            Collision=Family<3;ExpectedLevels=3-PitchIndex;
            FVoxelEnvironmentAssetDescriptor D;D.SpecId=Names[Family];D.Kind=Kinds[Family];D.Category=TEXT("environment");D.Fellable=Family==0&&PitchIndex!=1;
            D.SpecHash=FString::ChrN(64,TCHAR('a'+Family));D.CatalogHash=FString::ChrN(64,TCHAR('c'));
            D.ProviderHash=Case%2?FString::ChrN(64,TCHAR('d')):FString();D.SeedIndex=0x80000001u+uint32(Case);
            const auto Vxa=EnvironmentFixture(Pitches[PitchIndex],false,Materials[Family]);
            if(Case%2)D.SourceHash=FMD5::HashBytes(Vxa.GetData(),Vxa.Num()).ToUpper();
            Source=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Source||!Source->InitializeAssetFromVxa(D,Vxa,Collision,false)){Test->AddError(FString::Printf(TEXT("initialize generic %s at %u mm"),Kinds[Family],Pitches[PitchIndex]));Cleanup();return true;}
            Source->SetActorLocation(FVector(Case*1000.,-200.,100.));Expected=Source->GetAssetDescriptor();
            Test->TestEqual(TEXT("source digest computed from actual VXA"),Expected.SourceHash,FMD5::HashBytes(Vxa.GetData(),Vxa.Num()));
            Test->TestEqual(TEXT("source LOD count follows pitch"),Source->LevelCount(),ExpectedLevels);
            FVoxelImmutableGeometry Original,Again;TArray<uint8> Dynamic,AgainDynamic;
            if(!Source->CaptureObjectState(Original,Dynamic)||!Source->CaptureObjectState(Again,AgainDynamic)){Test->AddError(TEXT("capture generic source"));Cleanup();return true;}
            Test->TestTrue(TEXT("unchanged source reuses geometry"),Original==Again);
            {FMemoryReader Reader(Dynamic);uint32 Version=0;Reader<<Version;Test->TestEqual(TEXT("split envelope remains v1"),Version,uint32(1));
                const int64 IdentityOffset=Reader.Tell();int32 Marker=0;Reader<<Marker;Test->TestEqual(TEXT("generic identity marker"),Marker,-1);Reader.Seek(IdentityOffset);
                FVoxelEnvironmentAssetDescriptor Stored;Test->TestTrue(TEXT("generic descriptor serializer reads capture"),VoxelEnvironmentAsset::SerializeIdentity(Reader,Stored));DescriptorEquals(Stored);}
            VoxelObjects::FEntry Entry;Entry.Id=FGuid::NewGuid();Entry.Kind=3;Entry.GeometryRevision=1;Entry.Geometry=Original;Entry.Dynamic=Dynamic;Entry.Transform=Source->GetActorTransform();Entry.BoundsExtent=FVector(20.);
            VoxelDetachedPersistence::FSnapshot Input;Input.Add(Entry);TArray<uint8> Encoded;VoxelDetachedPersistence::FSnapshot Decoded;
            if(!VoxelDetachedPersistence::EncodeSnapshot(Input,Encoded)||!VoxelDetachedPersistence::DecodeSnapshot(Encoded,Decoded)||Decoded.Num()!=1){Test->AddError(TEXT("encode/decode generic actor snapshot"));Cleanup();return true;}
            Test->TestTrue(TEXT("snapshot codec preserves geometry bytes"),Decoded[0].Geometry&&*Decoded[0].Geometry==*Original);
            Test->TestTrue(TEXT("snapshot codec preserves identity/dynamic bytes"),Decoded[0].Dynamic==Dynamic);
            DecodedGeometry=Decoded[0].Geometry;Restored=World->SpawnActor<AVoxelEnvironmentLODPrototype>();
            if(!Restored){Test->AddError(TEXT("spawn generic restore target"));Cleanup();return true;}
            Status=MakeShared<FGeneralRestoreStatus>();auto Completion=Status;
            Restored->BeginStagedObjectRestore(DecodedGeometry,Decoded[0].Dynamic,[Completion](bool Ok){Completion->Called=true;Completion->Success=Ok;});
            return false;
        }
        Restored->AdvanceStagedObjectRestore();if(!Status->Called)return false;
        if(!Status->Success){Test->AddError(FString::Printf(TEXT("staged generic restore failed: %s"),*Expected.Kind));Cleanup();return true;}
        Test->TestTrue(TEXT("ready generic actor remains unpublished"),Restored->IsHidden());
        if(!Restored->PublishStagedObjectRestore()){Test->AddError(TEXT("publish generic restore"));Cleanup();return true;}
        DescriptorEquals(Restored->GetAssetDescriptor());Test->TestEqual(TEXT("runtime fellable flag restored"),Restored->IsFellable(),Expected.Fellable);
        Test->TestEqual(TEXT("restored LODs follow source pitch"),Restored->LevelCount(),ExpectedLevels);
        const FVector Center=Restored->GetActorLocation()+FVector(5.,5.,5.);
        Test->TestEqual(TEXT("collision policy restored independently of kind"),Restored->SolidAt(Center),Collision);
        Test->TestEqual(TEXT("chopping respects capability rather than legacy name"),Restored->CanChop(Center),Expected.Fellable);
        Test->TestFalse(TEXT("outside source grid stays empty"),Restored->SolidAt(Restored->GetActorLocation()+FVector(100.)));
        FVector Hit;Test->TestTrue(TEXT("finest voxel trace restored for all kinds"),Restored->Trace(Center-FVector(30,0,0),FVector::ForwardVector,100,Hit));
        FVoxelImmutableGeometry Captured;TArray<uint8> CapturedDynamic;
        Test->TestTrue(TEXT("capture restored generic actor"),Restored->CaptureObjectState(Captured,CapturedDynamic));
        Test->TestTrue(TEXT("restore retains decoded immutable allocation"),Captured==DecodedGeometry);
        Source->Destroy();Restored->Destroy();Source=nullptr;Restored=nullptr;Status.Reset();DecodedGeometry.Reset();
        ++Case;if(Case==18){Cleanup();return true;}return false;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGeneralEnvironmentTest,"Voxel.Objects.GeneralEnvironment",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGeneralEnvironmentTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FGeneralEnvironmentProbe(this));return true;}
#endif

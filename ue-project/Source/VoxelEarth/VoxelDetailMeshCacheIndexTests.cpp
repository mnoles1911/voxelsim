#include "VoxelDetailMeshCacheIndex.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "voxelcore/assetgrid.h"
#include <openssl/sha.h>
namespace {
void CachePut(TArray<uint8>& B,int32 O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(I*8));}
FString TestSHA(const TArray<uint8>& B){uint8 H[32];SHA256(B.GetData(),B.Num(),H);return BytesToHex(H,32).ToLower();}
TArray<uint8> TestBytes(const FString& S){FTCHARToUTF8 U(*S);TArray<uint8> B;B.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length());return B;}
FString TestJson(const TSharedPtr<FJsonObject>& O){FString T;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&T));return T;}
struct FCacheFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/DetailIndex")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> Publication;TSharedPtr<FJsonObject> Index,Row;
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog;FVoxelDetailCacheIdentity E;FString Error;
    FCacheFixture(){
        TArray<uint8> G,V;G.SetNumZeroed(53);CachePut(G,0,vxc::kVxaMagic);CachePut(G,4,vxc::kVxaVersion);
        for(int O:{20,24,28})CachePut(G,O,1);CachePut(G,32,25);CachePut(G,36,1);G[48]=16;CachePut(G,49,1);
        const FString MD5=FMD5::HashBytes(G.GetData(),G.Num()).ToLower();
        V.SetNumZeroed(138);FMemory::Memcpy(V.GetData(),"VAC1",4);CachePut(V,4,1);
        for(int O:{8,12,16})CachePut(V,O,1);CachePut(V,32,25);CachePut(V,36,1);
        auto Digit=[](TCHAR C){return C<='9'?C-'0':C-'a'+10;};for(int I=0;I<16;++I)V[48+I]=uint8(Digit(MD5[2*I])*16+Digit(MD5[2*I+1]));
        V[134]=16;V[135]=80;V[136]=140;V[137]=20;
        TArray<uint8> Seal;Seal.Append(V.GetData(),96);Seal.Append(V.GetData()+128,10);SHA256(Seal.GetData(),Seal.Num(),V.GetData()+96);
        auto Source=MakeShared<FJsonObject>();Source->SetStringField(TEXT("id"),TEXT("test-grass-0001"));Source->SetStringField(TEXT("species"),TEXT("test-grass"));Source->SetNumberField(TEXT("seed"),1);
        Source->SetStringField(TEXT("geometry_md5"),MD5);Source->SetStringField(TEXT("geometry_sha256"),TestSHA(G));Source->SetStringField(TEXT("sha256"),TestSHA(V));Source->SetStringField(TEXT("file"),MD5+TEXT(".vac"));
        auto Pub=MakeShared<FJsonObject>();Pub->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(Source)});Publication=TestBytes(TestJson(Pub));
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks/test-grass")),true);
        FFileHelper::SaveArrayToFile(G,*(Directory/TEXT("banks/test-grass/test-grass-0001.vxa")));FFileHelper::SaveArrayToFile(V,*(Directory/TEXT("appearance")/(MD5+TEXT(".vac"))));FFileHelper::SaveArrayToFile(Publication,*(Directory/TEXT("appearance/published.json")));
        Catalog=FVoxelPublishedAppearanceCatalog::Load(Directory,Error);
        E.EngineVersion=TEXT("test-engine");E.HostPlatform=TEXT("test-host");E.Settings=TEXT("test-settings");E.BuilderIdentity=TEXT("verified-builder");E.MaterialSourceSHA256=TestSHA(TestBytes(TEXT("material")));
        const FString Key=TestSHA(TestBytes(TestSHA(G)+TEXT("\n")+TestSHA(V)+TEXT("\n")+E.BuilderIdentity+E.Settings+E.EngineVersion+E.MaterialSourceSHA256+E.HostPlatform));
        Index=MakeShared<FJsonObject>();Index->SetNumberField(TEXT("schema"),2);Index->SetNumberField(TEXT("render_attribute_fingerprint_schema"),1);Index->SetBoolField(TEXT("preview_only"),false);
        Index->SetStringField(TEXT("stage"),TEXT("uncooked-editor-source; platform cook and runtime acceptance pending"));Index->SetStringField(TEXT("engine_version"),E.EngineVersion);Index->SetStringField(TEXT("host_platform"),E.HostPlatform);Index->SetStringField(TEXT("settings"),E.Settings);Index->SetStringField(TEXT("builder_sha256"),TestSHA(TestBytes(E.BuilderIdentity)));Index->SetStringField(TEXT("material_source_sha256"),E.MaterialSourceSHA256);Index->SetStringField(TEXT("publication_sha256"),TestSHA(Publication));
        Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("id"),TEXT("test-grass-0001"));Row->SetStringField(TEXT("species"),TEXT("test-grass"));Row->SetStringField(TEXT("geometry_sha256"),TestSHA(G));Row->SetStringField(TEXT("appearance_sha256"),TestSHA(V));Row->SetStringField(TEXT("derived_key"),Key);Row->SetStringField(TEXT("package_sha256"),TestSHA(TestBytes(TEXT("mesh"))));Row->SetStringField(TEXT("material_sha256"),TestSHA(TestBytes(TEXT("mi"))));Row->SetNumberField(TEXT("voxel_pitch_um"),25000);SetRoot(TEXT("/Game/Voxel/Generated/Detail/run"));
        auto Facts=MakeShared<FJsonObject>();auto Lod=MakeShared<FJsonObject>();Lod->SetNumberField(TEXT("vertices"),24);Lod->SetNumberField(TEXT("triangles"),12);Lod->SetNumberField(TEXT("uv_channels"),2);Lod->SetNumberField(TEXT("screen_size"),1);
        Facts->SetNumberField(TEXT("fingerprint_schema"),1);Facts->SetStringField(TEXT("attribute_sha256"),TestSHA(TestBytes(TEXT("attributes"))));Facts->SetArrayField(TEXT("lods"),{MakeShared<FJsonValueObject>(Lod)});Facts->SetNumberField(TEXT("source_models"),1);Facts->SetStringField(TEXT("bounds"),TEXT("test bounds"));Row->SetObjectField(TEXT("mesh_facts"),Facts);
        Index->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(Row)});
    }
    void SetRoot(const FString& Root){const FString K=Row->GetStringField(TEXT("derived_key"));Row->SetStringField(TEXT("object_path"),Root/TEXT("D_")+K+TEXT(".D_")+K);Row->SetStringField(TEXT("material_object_path"),Root/TEXT("M_")+K+TEXT(".M_")+K);Row->SetStringField(TEXT("package_file"),FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Root/TEXT("D_")+K,TEXT(".uasset"))));Row->SetStringField(TEXT("material_file"),FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Root/TEXT("M_")+K,TEXT(".uasset"))));}
    auto Parse(){return FVoxelDetailMeshCacheIndex::ParseEditorSource(TestJson(Index),Publication,Catalog,E,Error);}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDetailCacheIndexTest,"Voxel.Detail.CacheIndex",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDetailCacheIndexTest::RunTest(const FString&){
    FCacheFixture F;if(!TestTrue(TEXT("verified source fixture"),F.Catalog.IsValid()))return false;
    auto Good=F.Parse();if(!TestTrue(TEXT("valid editor source cache"),Good.IsValid())){AddError(F.Error);return false;}
    TestEqual(TEXT("one mapped resource"),Good->Num(),1);TestTrue(TEXT("snapshot retained"),Good->SourceSnapshot()==F.Catalog);TestNotNull(TEXT("resource lookup"),Good->Find(1));
    for(const TCHAR* Field:{TEXT("engine_version"),TEXT("host_platform"),TEXT("settings"),TEXT("builder_sha256"),TEXT("material_source_sha256"),TEXT("publication_sha256"),TEXT("stage")}){
        const FString Saved=F.Index->GetStringField(Field);F.Index->SetStringField(Field,TEXT("wrong"));TestFalse(Field,F.Parse().IsValid());F.Index->SetStringField(Field,Saved);
    }
    F.Index->SetNumberField(TEXT("schema"),3);TestFalse(TEXT("schema mismatch"),F.Parse().IsValid());F.Index->SetNumberField(TEXT("schema"),2);
    for(const TCHAR* Field:{TEXT("geometry_sha256"),TEXT("appearance_sha256"),TEXT("derived_key"),TEXT("package_sha256"),TEXT("material_sha256")}){
        const FString Saved=F.Row->GetStringField(Field);F.Row->RemoveField(Field);TestFalse(TEXT("missing required hash"),F.Parse().IsValid());F.Row->SetStringField(Field,Saved);
    }
    const FString Path=F.Row->GetStringField(TEXT("object_path"));F.Row->SetStringField(TEXT("object_path"),TEXT("/Game/Other.Mesh"));TestFalse(TEXT("foreign path"),F.Parse().IsValid());F.Row->SetStringField(TEXT("object_path"),Path);
    F.SetRoot(TEXT("/Game/Voxel/Generated/Detail/../evil"));TestFalse(TEXT("traversal path"),F.Parse().IsValid());F.SetRoot(TEXT("/Game/Voxel/Generated/Detail/run"));
    F.Row->SetNumberField(TEXT("voxel_pitch_um"),50000);TestFalse(TEXT("wrong pitch"),F.Parse().IsValid());F.Row->SetNumberField(TEXT("voxel_pitch_um"),25000);
    F.Index->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(F.Row),MakeShared<FJsonValueObject>(F.Row)});TestFalse(TEXT("duplicate row"),F.Parse().IsValid());F.Index->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(F.Row)});
    {
        const auto Saved=F.Publication;FUTF8ToTCHAR PU(reinterpret_cast<const ANSICHAR*>(Saved.GetData()),Saved.Num());TSharedPtr<FJsonObject> Pub;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(PU.Length(),PU.Get())),Pub);
        auto PublishedRows=Pub->GetArrayField(TEXT("models"));auto SourceAlias=MakeShared<FJsonObject>(*PublishedRows[0]->AsObject());SourceAlias->SetStringField(TEXT("id"),TEXT("test-grass-0002"));SourceAlias->SetNumberField(TEXT("seed"),2);PublishedRows.Add(MakeShared<FJsonValueObject>(SourceAlias));Pub->SetArrayField(TEXT("models"),PublishedRows);
        F.Publication=TestBytes(TestJson(Pub));F.Index->SetStringField(TEXT("publication_sha256"),TestSHA(F.Publication));
        auto Alias=MakeShared<FJsonObject>(*F.Row);Alias->SetStringField(TEXT("id"),TEXT("test-grass-0002"));F.Index->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(F.Row),MakeShared<FJsonValueObject>(Alias)});
        TestTrue(TEXT("identical resource alias accepted"),F.Parse().IsValid());
        Alias->SetStringField(TEXT("package_sha256"),TestSHA(TestBytes(TEXT("different package"))));TestFalse(TEXT("conflicting alias rejected"),F.Parse().IsValid());
        F.Publication=Saved;F.Index->SetStringField(TEXT("publication_sha256"),TestSHA(Saved));F.Index->SetArrayField(TEXT("models"),{MakeShared<FJsonValueObject>(F.Row)});
    }
    F.Index->SetBoolField(TEXT("preview_only"),true);F.SetRoot(TEXT("/Game/Voxel/Generated/DetailPreview/run"));TestFalse(TEXT("preview default refusal"),F.Parse().IsValid());
    F.E.bAllowPreview=true;TestFalse(TEXT("preview must match active publication"),F.Parse().IsValid());F.E.bActivePublicationIsPreview=true;TestTrue(TEXT("explicit preview match"),F.Parse().IsValid());
    TestFalse(TEXT("malformed JSON refused"),FVoxelDetailMeshCacheIndex::ParseEditorSource(TEXT("{"),F.Publication,F.Catalog,F.E,F.Error).IsValid());
    // The bytes and their manifest digest agree with each other, but describe
    // a changed appearance for the same geometry. The retained catalog must
    // reject that pairing before an index can authorize stale in-memory data.
    const auto OriginalPublication=F.Publication;
    FUTF8ToTCHAR U(reinterpret_cast<const ANSICHAR*>(F.Publication.GetData()),F.Publication.Num());
    TSharedPtr<FJsonObject> Changed;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(U.Length(),U.Get())),Changed);
    Changed->GetArrayField(TEXT("models"))[0]->AsObject()->SetStringField(TEXT("sha256"),TestSHA(TestBytes(TEXT("new appearance"))));
    F.Publication=TestBytes(TestJson(Changed));F.Index->SetStringField(TEXT("publication_sha256"),TestSHA(F.Publication));
    TestFalse(TEXT("changed appearance cannot reuse old catalog"),F.Parse().IsValid());
    TestEqual(TEXT("retained identity is checked"),F.Error,FString(TEXT("publication hashes do not match retained catalog")));
    F.Publication=OriginalPublication;F.Index->SetStringField(TEXT("publication_sha256"),TestSHA(F.Publication));
    TestTrue(TEXT("previous immutable result unaffected"),Good->Find(1)!=nullptr);
    return true;
}
#endif

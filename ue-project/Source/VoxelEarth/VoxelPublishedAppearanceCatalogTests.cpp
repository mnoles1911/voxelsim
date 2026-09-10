#include "VoxelPublishedAppearanceCatalog.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "voxelcore/assetgrid.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void CatalogPut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FString CatalogSHA(const TArray<uint8>& B){uint8 H[32];check(SHA256(B.GetData(),B.Num(),H));return BytesToHex(H,32).ToLower();}
void CatalogSeal(TArray<uint8>& B){TArray<uint8> C;C.Append(B.GetData(),96);C.Append(B.GetData()+128,B.Num()-128);check(SHA256(C.GetData(),C.Num(),B.GetData()+96));}
struct FCatalogFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/AppearanceCatalog")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> Geometry,Packet;FString Hash;TArray<TSharedPtr<FJsonValue>> Rows;
    FCatalogFixture(){
        Geometry.SetNumZeroed(53);CatalogPut(Geometry,0,vxc::kVxaMagic);CatalogPut(Geometry,4,vxc::kVxaVersion);
        CatalogPut(Geometry,8,uint32(-1));CatalogPut(Geometry,12,uint32(-2));CatalogPut(Geometry,16,uint32(-3));
        for(int O:{20,24,28})CatalogPut(Geometry,O,1);CatalogPut(Geometry,32,100);CatalogPut(Geometry,36,1);Geometry[48]=16;CatalogPut(Geometry,49,1);
        Hash=FMD5::HashBytes(Geometry.GetData(),Geometry.Num()).ToLower();
        Packet.SetNumZeroed(138);FMemory::Memcpy(Packet.GetData(),"VAC1",4);CatalogPut(Packet,4,1);
        for(int O:{8,12,16})CatalogPut(Packet,O,1);CatalogPut(Packet,20,uint32(-1));CatalogPut(Packet,24,uint32(-2));CatalogPut(Packet,28,uint32(-3));CatalogPut(Packet,32,100);CatalogPut(Packet,36,1);
        auto Digit=[](TCHAR C){return C>='0'&&C<='9'?int(C-'0'):int(C-'a'+10);};
        for(int I=0;I<16;++I)Packet[48+I]=uint8(Digit(Hash[2*I])*16+Digit(Hash[2*I+1]));
        Packet[134]=16;Packet[135]=91;Packet[136]=147;Packet[137]=23;CatalogSeal(Packet);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks/test-oak")),true);
    }
    ~FCatalogFixture(){IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    TSharedPtr<FJsonObject> Add(int Seed){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),FString::Printf(TEXT("test-oak-%04d"),Seed));O->SetStringField(TEXT("species"),TEXT("test-oak"));O->SetNumberField(TEXT("seed"),Seed);O->SetStringField(TEXT("geometry_md5"),Hash);O->SetStringField(TEXT("geometry_sha256"),CatalogSHA(Geometry));O->SetStringField(TEXT("sha256"),CatalogSHA(Packet));O->SetStringField(TEXT("file"),Hash+TEXT(".vac"));Rows.Add(MakeShared<FJsonValueObject>(O));check(FFileHelper::SaveArrayToFile(Geometry,*(Directory/TEXT("banks/test-oak")/(FString::Printf(TEXT("test-oak-%04d.vxa"),Seed)))));return O;}
    void Write(){auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("models"),Rows);FString Text;auto W=TJsonWriterFactory<>::Create(&Text);check(FJsonSerializer::Serialize(O,W));check(FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("appearance/published.json"))));check(FFileHelper::SaveArrayToFile(Packet,*(Directory/TEXT("appearance")/(Hash+TEXT(".vac")))));}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPublishedCatalogTest,"Voxel.Appearance.PublishedCatalog",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelPublishedCatalogTest::RunTest(const FString&){
    FCatalogFixture F;FString Error;F.Write();auto Empty=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("empty publication accepted"),Empty.IsValid()))return false;
    TestEqual(TEXT("only reserved zero entry"),Empty->Sources().Num(),1);TestEqual(TEXT("stale packet does not authorize"),Empty->FindResource(F.Hash),0u);
    auto Row=F.Add(1);F.Write();auto One=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("published matching VXA/VAC accepted"),One.IsValid())){AddError(Error);return false;}
    TestEqual(TEXT("published resource ID"),One->FindResource(F.Hash.ToUpper()),1u);
    F.Add(2);F.Write();auto Dedup=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("duplicate geometry distinct seeds accepted"),Dedup.IsValid()))return false;
    TestEqual(TEXT("one resource for shared geometry"),Dedup->Sources().Num(),2);
    TestEqual(TEXT("dedup stable bytes"),Dedup->ResourceBytes(),One->ResourceBytes());
    TestFalse(TEXT("retained budget refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error,1).IsValid());
    TestFalse(TEXT("working budget refused before files"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error,1024*1024,1).IsValid());
    const auto DuplicateRow=F.Rows[0];F.Rows.Add(DuplicateRow);F.Write();TestFalse(TEXT("duplicate row identity refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());F.Rows.Pop();
    Row->SetStringField(TEXT("file"),TEXT("../escape.vac"));F.Write();TestFalse(TEXT("path traversal refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());Row->SetStringField(TEXT("file"),F.Hash+TEXT(".vac"));
    F.Write();auto Altered=F.Geometry;Altered[48]=17;check(FFileHelper::SaveArrayToFile(Altered,*(F.Directory/TEXT("banks/test-oak/test-oak-0002.vxa"))));
    TestFalse(TEXT("every duplicate bank authenticated"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());check(FFileHelper::SaveArrayToFile(F.Geometry,*(F.Directory/TEXT("banks/test-oak/test-oak-0002.vxa"))));
    const auto OriginalPacket=F.Packet;F.Packet[135]^=1;F.Write();TestFalse(TEXT("altered packet refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());
    // Authenticated but mismatched packet is still not a valid source mapping.
    F.Rows.SetNum(1);F.Packet=OriginalPacket;F.Packet[134]=17;CatalogSeal(F.Packet);Row->SetStringField(TEXT("sha256"),CatalogSHA(F.Packet));F.Write();
    TestFalse(TEXT("valid checksum wrong source material refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());
    F.Packet=OriginalPacket;CatalogPut(F.Packet,20,0);CatalogSeal(F.Packet);Row->SetStringField(TEXT("sha256"),CatalogSHA(F.Packet));F.Write();
    TestFalse(TEXT("valid checksum wrong source origin refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());
    for(int Offset:{8,32,36}){
        F.Packet=OriginalPacket;CatalogPut(F.Packet,Offset,Offset==32?50u:2u);CatalogSeal(F.Packet);Row->SetStringField(TEXT("sha256"),CatalogSHA(F.Packet));F.Write();
        TestFalse(TEXT("authenticated dimensions pitch or count mismatch refused"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());
    }
    // Even a manifest rehashed around malformed geometry must fail VXA parsing.
    F.Geometry[0]^=1;F.Hash=FMD5::HashBytes(F.Geometry.GetData(),F.Geometry.Num()).ToLower();F.Packet=OriginalPacket;
    auto Digit=[](TCHAR C){return C<='9'?int(C-'0'):int(C-'a'+10);};
    for(int I=0;I<16;++I)F.Packet[48+I]=uint8(Digit(F.Hash[2*I])*16+Digit(F.Hash[2*I+1]));CatalogSeal(F.Packet);
    F.Rows.Reset();F.Add(1);F.Write();TestFalse(TEXT("rehash cannot authorize malformed VXA"),FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error).IsValid());
    F.Hash=One->Sources()[1].GeometryMD5;
    F.Rows.Reset();F.Packet=OriginalPacket;F.Write();auto Revoked=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("revoked inventory loads empty"),Revoked.IsValid()))return false;
    TestEqual(TEXT("revoked source with stale bank and packet absent"),Revoked->FindResource(F.Hash),0u);
    TestEqual(TEXT("prior immutable snapshot remains valid"),One->FindResource(F.Hash),1u);
    return true;
}
#endif

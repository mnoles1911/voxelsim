#include "VoxelTerrainAppearancePage.h"
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
void PagePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FString PageSHA(const TArray<uint8>& B){uint8 H[32];check(SHA256(B.GetData(),B.Num(),H));return BytesToHex(H,32).ToLower();}
void PageSeal(TArray<uint8>& B){TArray<uint8> C;C.Append(B.GetData(),96);C.Append(B.GetData()+128,B.Num()-128);check(SHA256(C.GetData(),C.Num(),B.GetData()+96));}
struct FPageFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/AppearanceCatalog")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> Geometry,Packet;FString Hash;TArray<TSharedPtr<FJsonValue>> Rows;
    FPageFixture(){
        Geometry.SetNumZeroed(53);PagePut(Geometry,0,vxc::kVxaMagic);PagePut(Geometry,4,vxc::kVxaVersion);
        PagePut(Geometry,8,uint32(-1));PagePut(Geometry,12,uint32(-2));PagePut(Geometry,16,uint32(-3));
        for(int O:{20,24,28})PagePut(Geometry,O,1);PagePut(Geometry,32,100);PagePut(Geometry,36,1);Geometry[48]=16;PagePut(Geometry,49,1);
        Hash=FMD5::HashBytes(Geometry.GetData(),Geometry.Num()).ToLower();
        Packet.SetNumZeroed(138);FMemory::Memcpy(Packet.GetData(),"VAC1",4);PagePut(Packet,4,1);
        for(int O:{8,12,16})PagePut(Packet,O,1);PagePut(Packet,20,uint32(-1));PagePut(Packet,24,uint32(-2));PagePut(Packet,28,uint32(-3));PagePut(Packet,32,100);PagePut(Packet,36,1);
        auto Digit=[](TCHAR C){return C>='0'&&C<='9'?int(C-'0'):int(C-'a'+10);};
        for(int I=0;I<16;++I)Packet[48+I]=uint8(Digit(Hash[2*I])*16+Digit(Hash[2*I+1]));
        Packet[134]=16;Packet[135]=91;Packet[136]=147;Packet[137]=23;PageSeal(Packet);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks/test-oak")),true);
    }
    ~FPageFixture(){IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    TSharedPtr<FJsonObject> Add(int Seed){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),FString::Printf(TEXT("test-oak-%04d"),Seed));O->SetStringField(TEXT("species"),TEXT("test-oak"));O->SetNumberField(TEXT("seed"),Seed);O->SetStringField(TEXT("geometry_md5"),Hash);O->SetStringField(TEXT("geometry_sha256"),PageSHA(Geometry));O->SetStringField(TEXT("sha256"),PageSHA(Packet));O->SetStringField(TEXT("file"),Hash+TEXT(".vac"));Rows.Add(MakeShared<FJsonValueObject>(O));check(FFileHelper::SaveArrayToFile(Geometry,*(Directory/TEXT("banks/test-oak")/(FString::Printf(TEXT("test-oak-%04d.vxa"),Seed)))));return O;}
    void Write(){auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("models"),Rows);FString Text;auto W=TJsonWriterFactory<>::Create(&Text);check(FJsonSerializer::Serialize(O,W));check(FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("appearance/published.json"))));check(FFileHelper::SaveArrayToFile(Packet,*(Directory/TEXT("appearance")/(Hash+TEXT(".vac")))));}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainAppearancePageTest,"Voxel.Appearance.TerrainPagePreparation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelTerrainAppearancePageTest::RunTest(const FString&){
    FPageFixture F;F.Add(1);F.Write();FString Error;auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("fixture catalog accepted"),Catalog.IsValid())){AddError(Error);return false;}
    FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid,Unapproved;
    if(!TestTrue(TEXT("fixture grid parsed"),Grid.parse(F.Geometry.GetData(),size_t(F.Geometry.Num()))==vxc::AssetParseError::kOk))return false;
    if(!TestTrue(TEXT("independent unapproved grid parsed"),Unapproved.parse(F.Geometry.GetData(),size_t(F.Geometry.Num()))==vxc::AssetParseError::kOk))return false;
    Binding.Observe(Grid,F.Geometry.GetData(),size_t(F.Geometry.Num()));
    FVoxelTerrainAppearancePage::FTerrain Air=[](int64,int64,int64){return vxc::MAT_AIR;};
    FVoxelTerrainAppearancePage::FTouched Clean=[](int64,int64,int64){return false;};
    const FIntVector Key(-1,-1,-1);const uint64 Generation=0x123456789ull;
    vxc::AssetField::ResolvedAssetInstance I;I.grid=&Grid;I.anchorVx=-16;I.anchorVy=-16;I.anchorVz=-16;
    for(uint8 Yaw=0;Yaw<4;++Yaw){I.yawQuarter=Yaw;
        auto Page=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{I},Binding,Air,Clean,Error);
        if(!TestTrue(TEXT("all yaw preparations accepted"),Page.IsValid())){AddError(Error);return false;}
        auto Far=I;Far.anchorVz+=1000000;
        std::vector<vxc::AssetField::ResolvedAssetInstance> Broad(5000,Far);Broad.push_back(I);
        auto Filtered=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,Broad,Binding,Air,Clean,Error);
        if(!TestTrue(TEXT("broad footprint beyond cap is filtered to actual page contributors"),Filtered.IsValid()))return false;
        TestTrue(TEXT("broad footprint produces identical canonical bytes under every yaw"),Filtered->Words()==Page->Words());
        std::vector<vxc::AssetField::ResolvedAssetInstance> Overfull(4097,I);
        TestFalse(TEXT("actual intersecting overflow is refused without truncation"),FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,Overfull,Binding,Air,Clean,Error).IsValid());
        TestEqual(TEXT("negative page key retained"),Page->PageKey(),Key);TestEqual(TEXT("generation retained"),Page->PageGeneration(),Generation);
        TestTrue(TEXT("strong exact catalog retained"),Page->SourceSnapshot()==Catalog);
        auto Upload=Page->MakeUpload(Binding,Error);if(!TestTrue(TEXT("immutable upload conversion"),Upload.IsValid()))return false;
        TestEqual(TEXT("upload key retained"),Upload->PageKey,Key);TestEqual(TEXT("upload generation retained"),Upload->Generation,Generation);
        TestTrue(TEXT("source flatten cached per binding"),Upload->Sources==Binding.UploadSources(Error));
        TestTrue(TEXT("reserved resource range"),Upload->Sources->SourceRanges[0]==FUintVector2(0,0));
        TestTrue(TEXT("exact source words retained"),Upload->Sources->SourceWords==Catalog->Sources()[1].Sparse->Words);

        const auto& W=Page->Words();if(!TestTrue(TEXT("packed words exist"),W.Num()>80))return false;
        TestEqual(TEXT("one canonical source cell"),W[7],1u);TestEqual(TEXT("canonical yaw retained"),W[int32(W[10])+4],uint32(Yaw));
        TestEqual(TEXT("catalog resource retained"),W[int32(W[10])],1u);TestEqual(TEXT("high generation bits"),W[14],uint32(Generation>>32));
        auto Earlier=I;Earlier.grid=&Unapproved;
        auto Hidden=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{Earlier,I},Binding,Air,Clean,Error);
        if(!TestTrue(TEXT("unapproved first winner is valid fallback"),Hidden.IsValid()))return false;
        TestTrue(TEXT("later approved source not revealed"),Hidden->Words().IsEmpty());
    }
    auto Touched=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{I},Binding,Air,[](int64,int64,int64){return true;},Error);
    if(!TestTrue(TEXT("touched page accepted"),Touched.IsValid()))return false;TestTrue(TEXT("touched cells clear appearance"),Touched->Words().IsEmpty());
    auto Terrain=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{I},Binding,[](int64,int64,int64){return vxc::MAT_ROCK;},Clean,Error);
    if(!TestTrue(TEXT("terrain page accepted"),Terrain.IsValid()))return false;TestTrue(TEXT("terrain wins"),Terrain->Words().IsEmpty());
    auto Empty=FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{},Binding,Air,Clean,Error);
    if(!TestTrue(TEXT("no appearance accepted"),Empty.IsValid()))return false;TestEqual(TEXT("no appearance word allocation"),Empty->Words().GetAllocatedSize(),SIZE_T(0));
    for(uint32 L=1;L<=7;++L){
        const int64 Scale=int64(1)<<L;auto CoarseInstance=I;CoarseInstance.yawQuarter=0;
        CoarseInstance.anchorVx=-3*Scale+Scale/2+1;CoarseInstance.anchorVy=-4*Scale+Scale/2+2;CoarseInstance.anchorVz=-5*Scale+Scale/2+3;
        auto Coarse=FVoxelTerrainAppearancePage::Prepare(Key,L,Generation,{CoarseInstance},Binding,Air,Clean,Error);
        if(!TestTrue(TEXT("coarse representative prepared"),Coarse.IsValid()))return false;
        if(!TestTrue(TEXT("coarse source survives"),Coarse->Words().Num()>80))return false;
        TestEqual(TEXT("coarse packet version"),Coarse->Words()[0],2u);TestEqual(TEXT("coarse header level"),Coarse->Words()[5],L);
        auto Upload=Coarse->MakeUpload(Binding,Error);if(!TestTrue(TEXT("coarse upload created"),Upload.IsValid()))return false;
        TestEqual(TEXT("coarse upload level retained"),Upload->Level,L);
        CoarseInstance.anchorVx-=Scale/2;CoarseInstance.anchorVy-=Scale/2;CoarseInstance.anchorVz-=Scale/2;
        vxc::AssetAppearanceTrace Trace=[Scale](int64 X,int64 Y,int64 Z,int64& FX,int64& FY,int64& FZ,vxc::MaterialId& M){
            FX=-3*Scale;FY=-4*Scale;FZ=-5*Scale;M=X==-3&&Y==-4&&Z==-5?vxc::MAT_BARK:vxc::MAT_AIR;return true;
        };
        auto Recursive=FVoxelTerrainAppearancePage::Prepare(Key,L,Generation,{CoarseInstance},Binding,Air,Clean,Error,Trace);
        if(!TestTrue(TEXT("off-representative recursive source prepared"),Recursive.IsValid())){AddError(Error);return false;}
        if(!TestTrue(TEXT("recursive selected source survives volume filtering"),Recursive->Words().Num()>80))return false;
        TestEqual(TEXT("recursive selected offsets encoded"),Recursive->Words()[0],3u);
        auto RecursiveUpload=Recursive->MakeUpload(Binding,Error);TestTrue(TEXT("recursive immutable upload created"),RecursiveUpload.IsValid());
        auto Removed=FVoxelTerrainAppearancePage::Prepare(Key,L,Generation,{CoarseInstance},Binding,Air,
            [Scale](int64 X,int64 Y,int64 Z){return X==-3*Scale&&Y==-4*Scale&&Z==-5*Scale;},Error,Trace);
        if(!TestTrue(TEXT("recursive touched source preparation accepted"),Removed.IsValid()))return false;
        TestTrue(TEXT("recursive touched child loses authored appearance"),Removed->Words().IsEmpty());
    }
    TestFalse(TEXT("unsupported coarse level refused"),FVoxelTerrainAppearancePage::Prepare(Key,8,Generation,{I},Binding,Air,Clean,Error).IsValid());
    TestFalse(TEXT("zero generation refused"),FVoxelTerrainAppearancePage::Prepare(Key,0,0,{I},Binding,Air,Clean,Error).IsValid());
    TestFalse(TEXT("missing touched provenance refused"),FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{I},Binding,Air,{},Error).IsValid());
    I.yawQuarter=4;TestFalse(TEXT("noncanonical yaw refused"),FVoxelTerrainAppearancePage::Prepare(Key,0,Generation,{I},Binding,Air,Clean,Error).IsValid());
    return true;
}
#endif

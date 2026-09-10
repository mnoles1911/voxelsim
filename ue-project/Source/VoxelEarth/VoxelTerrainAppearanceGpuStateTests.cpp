#include "VoxelTerrainAppearancePage.h"
#include "VoxelTerrainAppearanceGpuState.h"
#include "VoxelTerrainAppearanceValidation.h"
#include "VoxelBrickPool.h"
#include "voxelcore/assetappearancepack.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
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
void GpuStatePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FString GpuStateSHA(const TArray<uint8>& B){uint8 H[32];check(SHA256(B.GetData(),B.Num(),H));return BytesToHex(H,32).ToLower();}
void GpuStateSeal(TArray<uint8>& B){TArray<uint8> C;C.Append(B.GetData(),96);C.Append(B.GetData()+128,B.Num()-128);check(SHA256(C.GetData(),C.Num(),B.GetData()+96));}
struct FGpuStateFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/AppearanceCatalog")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> Geometry,Packet;FString Hash;TArray<TSharedPtr<FJsonValue>> Rows;
    FGpuStateFixture(){
        Geometry.SetNumZeroed(53);GpuStatePut(Geometry,0,vxc::kVxaMagic);GpuStatePut(Geometry,4,vxc::kVxaVersion);
        GpuStatePut(Geometry,8,uint32(-1));GpuStatePut(Geometry,12,uint32(-2));GpuStatePut(Geometry,16,uint32(-3));
        for(int O:{20,24,28})GpuStatePut(Geometry,O,1);GpuStatePut(Geometry,32,100);GpuStatePut(Geometry,36,1);Geometry[48]=16;GpuStatePut(Geometry,49,1);
        Hash=FMD5::HashBytes(Geometry.GetData(),Geometry.Num()).ToLower();
        Packet.SetNumZeroed(138);FMemory::Memcpy(Packet.GetData(),"VAC1",4);GpuStatePut(Packet,4,1);
        for(int O:{8,12,16})GpuStatePut(Packet,O,1);GpuStatePut(Packet,20,uint32(-1));GpuStatePut(Packet,24,uint32(-2));GpuStatePut(Packet,28,uint32(-3));GpuStatePut(Packet,32,100);GpuStatePut(Packet,36,1);
        auto Digit=[](TCHAR C){return C>='0'&&C<='9'?int(C-'0'):int(C-'a'+10);};
        for(int I=0;I<16;++I)Packet[48+I]=uint8(Digit(Hash[2*I])*16+Digit(Hash[2*I+1]));
        Packet[134]=16;Packet[135]=91;Packet[136]=147;Packet[137]=23;GpuStateSeal(Packet);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks/test-oak")),true);
    }
    ~FGpuStateFixture(){IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    TSharedPtr<FJsonObject> Add(int Seed){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),FString::Printf(TEXT("test-oak-%04d"),Seed));O->SetStringField(TEXT("species"),TEXT("test-oak"));O->SetNumberField(TEXT("seed"),Seed);O->SetStringField(TEXT("geometry_md5"),Hash);O->SetStringField(TEXT("geometry_sha256"),GpuStateSHA(Geometry));O->SetStringField(TEXT("sha256"),GpuStateSHA(Packet));O->SetStringField(TEXT("file"),Hash+TEXT(".vac"));Rows.Add(MakeShared<FJsonValueObject>(O));check(FFileHelper::SaveArrayToFile(Geometry,*(Directory/TEXT("banks/test-oak")/(FString::Printf(TEXT("test-oak-%04d.vxa"),Seed)))));return O;}
    void Write(){auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("models"),Rows);FString Text;auto W=TJsonWriterFactory<>::Create(&Text);check(FJsonSerializer::Serialize(O,W));check(FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("appearance/published.json"))));check(FFileHelper::SaveArrayToFile(Packet,*(Directory/TEXT("appearance")/(Hash+TEXT(".vac")))));}
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainGpuStateTest,"Voxel.Appearance.TerrainGpuState",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelTerrainGpuStateTest::RunTest(const FString&){
    if(!TestTrue(TEXT("SM6 RHI initialized"),GIsRHIInitialized&&GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6))return false;
    FGpuStateFixture F;F.Add(1);F.Write();FString Error;auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("real fixture catalog"),Catalog.IsValid()))return false;
    FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid;
    if(!TestTrue(TEXT("real VXA parsed"),Grid.parse(F.Geometry.GetData(),size_t(F.Geometry.Num()))==vxc::AssetParseError::kOk))return false;
    Binding.Observe(Grid,F.Geometry.GetData(),size_t(F.Geometry.Num()));
    vxc::AssetField::ResolvedAssetInstance I;I.grid=&Grid;I.anchorVx=-16;I.anchorVy=-16;I.anchorVz=-16;I.yawQuarter=3;
    auto Page=FVoxelTerrainAppearancePage::Prepare(FIntVector(-1,-1,-1),0,7,{I},Binding,[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Error);
    if(!TestTrue(TEXT("canonical page prepared"),Page.IsValid()))return false;
    auto Upload=Page->MakeUpload(Binding,Error);if(!TestTrue(TEXT("canonical upload built"),Upload.IsValid()))return false;
    TArray<FString> Failures;
    ENQUEUE_RENDER_COMMAND(VoxelTerrainGpuStateTest)([&](FRHICommandListImmediate& Cmd){
        FVoxelTerrainAppearanceGpuState State;
        auto Check=[&](bool OK,const TCHAR* What){if(!OK)Failures.Add(What);};
        auto EmptyViews=State.GetViews(Cmd);
        Check(EmptyViews.Pages.IsValid()&&EmptyViews.Sources.IsValid()&&EmptyViews.Ranges.IsValid()&&EmptyViews.Slots.IsValid(),TEXT("raster empty bindings all valid"));
        Check(State.GetViews(Cmd).Slots==EmptyViews.Slots,TEXT("empty raster bindings stable across reads"));
        struct FSnapshot {TArray<uint32> Pages,Sources,Ranges,Slots;};
        auto Read=[&](bool Small,FSnapshot& Out){
            FRDGBuilder Graph(Cmd);auto B=State.Register(Graph);
            FRHIGPUBufferReadback RP(TEXT("Appearance.StatePages")),RS(TEXT("Appearance.StateSources")),RR(TEXT("Appearance.StateRanges")),RT(TEXT("Appearance.StateSlots"));
            const uint32 PB=Small?4:512,SB=Small?4:512,RB=Small?8:64,TB=Small?16:64;
            AddEnqueueCopyPass(Graph,&RP,B.Pages,PB);AddEnqueueCopyPass(Graph,&RS,B.Sources,SB);AddEnqueueCopyPass(Graph,&RR,B.Ranges,RB);AddEnqueueCopyPass(Graph,&RT,B.Slots,TB);
            Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();
            auto Copy=[&](FRHIGPUBufferReadback& R,uint32 Bytes,TArray<uint32>& A){if(!R.IsReady())return false;const void* P=R.Lock(Bytes);if(!P)return false;A.SetNumUninitialized(int32(Bytes/4));FMemory::Memcpy(A.GetData(),P,Bytes);R.Unlock();return true;};
            const bool OK=Copy(RP,PB,Out.Pages)&&Copy(RS,SB,Out.Sources)&&Copy(RR,RB,Out.Ranges)&&Copy(RT,TB,Out.Slots);Check(OK,TEXT("all persistent buffers read back"));return OK;
        };
        FSnapshot D;if(!Read(true,D))return;Check(D.Pages[0]==0&&D.Sources[0]==0&&D.Ranges[0]==0&&D.Slots[0]==0,TEXT("empty state binds zero dummies"));
        TArray<FVoxelTerrainAppearanceGpuState::FEntry> Entries;Entries.Add({2,Upload});
        if(!State.ApplyBatch(Cmd,4,{},Entries,Error)){Check(false,TEXT("first page batch admitted"));return;}
        const auto PopulatedViews=State.GetViews(Cmd);
        Check(PopulatedViews.Pages.IsValid()&&PopulatedViews.Sources.IsValid()&&PopulatedViews.Ranges.IsValid()&&PopulatedViews.Slots.IsValid(),TEXT("raster populated bindings all valid"));
        Check(PopulatedViews.Slots!=EmptyViews.Slots,TEXT("first admission replaces empty raster binding"));
        FSnapshot A;if(!Read(false,A))return;
        Check(A.Slots[8]==0&&A.Slots[9]==uint32(Upload->PageWords.Num())&&A.Slots[10]==0&&A.Slots[11]==2,TEXT("slot descriptor addresses exact page and source ranges"));
        Check(FMemory::Memcmp(A.Pages.GetData(),Upload->PageWords.GetData(),Upload->PageWords.Num()*4)==0,TEXT("GPU page equals canonical bytes"));
        Check(FMemory::Memcmp(A.Sources.GetData(),Upload->Sources->SourceWords.GetData(),Upload->Sources->SourceWords.Num()*4)==0,TEXT("GPU original source equals verified bytes"));
        Check(A.Ranges[2]==0&&A.Ranges[3]==uint32(Upload->Sources->SourceWords.Num()),TEXT("source range relocation exact"));
        Entries.Reset();Entries.Add({3,Upload});Check(State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("second page shares source"));
        FSnapshot B;if(!Read(false,B))return;Check(B.Slots[10]==B.Slots[14]&&B.Slots[8]!=B.Slots[12],TEXT("shared source separate page ranges"));
        TArray<uint32> Clear{2};Check(State.ApplyBatch(Cmd,4,Clear,{},Error),TEXT("clear one shared page"));
        FSnapshot C;if(!Read(false,C))return;Check(C.Slots[8]==0&&C.Slots[9]==0&&C.Slots[13]>0,TEXT("clear hides only requested page"));
        // Replace remaining slot with another immutable snapshot; last old source
        // is reclaimed, and first-fit reuses its source/range/page addresses.
        auto CopySources=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>(*Upload->Sources);
        auto Replacement=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Upload);Replacement->Sources=CopySources;
        Entries.Reset();Entries.Add({3,Replacement});Check(State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("replacement snapshot admitted"));
        FSnapshot E;if(!Read(false,E))return;Check(E.Slots[12]==0&&E.Slots[14]==0&&E.Ranges[2]==0,TEXT("last-reference arenas reclaimed and reused"));
        auto Invalid=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Replacement);Invalid->Generation++;
        Entries.Reset();Entries.Add({3,Invalid});Check(!State.ApplyBatch(Cmd,4,{},Entries,Error)&&!Error.IsEmpty(),TEXT("mismatched generation explicitly refused"));
        FSnapshot G;if(!Read(false,G))return;Check(G.Slots[12]==0&&G.Slots[13]==0&&G.Slots[14]==0&&G.Slots[15]==0,TEXT("failed replacement clears all descriptor words"));
        Entries.Reset();Entries.Add({2,Upload});Check(State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("valid page restored after refusal"));
        Entries.Reset();Entries.Add({2,Upload});Check(!State.ApplyBatch(Cmd,2,{},Entries,Error),TEXT("bounded slot capacity refusal"));
        FSnapshot H;if(!Read(false,H))return;Check(H.Slots[8]==0&&H.Slots[9]==0,TEXT("out-of-capacity replacement removes old descriptor"));
        auto BadSources=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>(*Upload->Sources);BadSources->SourceRanges[1]=FUintVector2(MAX_uint32,1);
        auto BadRange=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Upload);BadRange->Sources=BadSources;
        Entries.Reset();Entries.Add({2,BadRange});Check(!State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("overflowing source range refused"));
        FSnapshot J;if(!Read(false,J))return;Check(J.Slots[8]==0&&J.Slots[9]==0,TEXT("missing resource never maps another instance"));
        auto Coarse=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Upload);Coarse->Level=3;Coarse->PageWords[0]=2;Coarse->PageWords[5]=3;
        Entries.Reset();Entries.Add({2,Coarse});Check(State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("coarse appearance upload admitted"));
        FSnapshot CoarseRead;if(!Read(false,CoarseRead))return;Check(CoarseRead.Slots[9]==uint32(Coarse->PageWords.Num())&&CoarseRead.Pages[CoarseRead.Slots[8]+5]==3,TEXT("coarse level survives GPU publication"));
        auto WrongLevel=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Coarse);WrongLevel->Level=2;
        Entries.Reset();Entries.Add({2,WrongLevel});Check(!State.ApplyBatch(Cmd,4,{},Entries,Error),TEXT("header and upload level mismatch refused"));
        FSnapshot LevelRefused;if(!Read(false,LevelRefused))return;Check(LevelRefused.Slots[9]==0,TEXT("level mismatch clears stale appearance"));
        Check(State.GetViews(Cmd).Slots==PopulatedViews.Slots,TEXT("raster storage stable through page replacement and refusal"));
        State.Reset();
        Check(State.GetViews(Cmd).Slots.IsValid()&&State.GetViews(Cmd).Slots!=PopulatedViews.Slots,TEXT("reset refreshes raster binding without reusing stale page storage"));
        FSnapshot K;if(!Read(true,K))return;Check(K.Slots[0]==0&&K.Ranges[0]==0,TEXT("reset returns valid empty bindings"));
    });FlushRenderingCommands();
    for(const auto& Failure:Failures)AddError(Failure);
    return Failures.IsEmpty();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRecursiveAppearanceAdmissionTest,"Voxel.Appearance.RecursivePageAdmission",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelRecursiveAppearanceAdmissionTest::RunTest(const FString&){
    FGpuStateFixture F;F.Add(1);F.Write();FString Error;auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);if(!TestTrue(TEXT("recursive original catalog"),Catalog.IsValid()))return false;FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid;if(!TestTrue(TEXT("recursive source VXA"),Grid.parse(F.Geometry.GetData(),F.Geometry.Num())==vxc::AssetParseError::kOk))return false;Binding.Observe(Grid,F.Geometry.GetData(),F.Geometry.Num());
    auto Sources=Binding.UploadSources(Error);if(!TestTrue(TEXT("recursive verified sources"),Sources.IsValid()))return false;
    TArray<TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>> Valid;
    for(uint8 L=1;L<=7;++L){const int64 Scale=int64(1)<<L,Half=Scale/2;const int64 WX=-18,WY=-15,WZ=-19,CX=vxc::floorDiv(WX,Scale),CY=vxc::floorDiv(WY,Scale),CZ=vxc::floorDiv(WZ,Scale);
        vxc::AssetField::ResolvedAssetInstance I;I.grid=&Grid;I.anchorVx=-16;I.anchorVy=-16;I.anchorVz=-16;I.yawQuarter=3;
        vxc::AssetAppearanceCell C;C.cell=uint16((CX+32)+32*(CY+32)+1024*(CZ+32));C.instance=0;C.resource=1;C.sourceX=-1;C.sourceY=-2;C.sourceZ=-3;C.yaw=3;C.material=vxc::MAT_BARK;C.offsetX=int8(WX-(CX*Scale+Half));C.offsetY=int8(WY-(CY*Scale+Half));C.offsetZ=int8(WZ-(CZ*Scale+Half));
        std::vector<uint32_t> Words;if(!TestTrue(TEXT("actual recursive-offset core pack"),vxc::assetPackAppearancePage(-1,-1,-1,61,{I},{C},Words,L)))return false;
        auto U=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>();U->PageKey=FIntVector(-1,-1,-1);U->Level=L;U->Generation=61;U->Sources=Sources;U->PageWords.Append(Words.data(),int32(Words.size()));TestEqual(TEXT("nonrepresentative pack version"),U->PageWords[0],3u);if(!TestTrue(TEXT("valid canonical v3 admitted"),VoxelValidateRecursiveAppearancePage(*U)))return false;Valid.Add(U);
    }
    TArray<TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>> Invalid;
    auto Mutate=[&](TFunctionRef<void(FVoxelTerrainAppearanceUpload&)> Change){auto U=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(*Valid[0]);Change(*U);Invalid.Add(U);};
    Mutate([](auto& U){U.Level=0;U.PageWords[5]=0;});Mutate([](auto& U){U.PageWords[15]=MAX_uint32;});Mutate([](auto& U){--U.PageWords[15];});
    Mutate([](auto& U){U.PageWords[U.PageWords[15]]=0x010000ffu;});Mutate([](auto& U){U.PageWords[U.PageWords[15]]=1;});Mutate([](auto& U){U.PageWords[U.PageWords[15]]=254;});Mutate([](auto& U){U.PageWords[U.PageWords[15]]=0;});
    Mutate([](auto& U){U.PageWords.Pop();U.PageWords[1]=U.PageWords.Num();});Mutate([](auto& U){U.PageWords.Add(0);U.PageWords[1]=U.PageWords.Num();});
    Mutate([](auto& U){U.PageWords[7]=32769;});Mutate([](auto& U){U.PageWords[6]=4097;});Mutate([](auto& U){U.PageWords[12]=65;});
    Mutate([](auto& U){for(int I=16;I<80;++I)if(U.PageWords[I]){U.PageWords[I]=2;break;}});Mutate([](auto& U){U.PageWords[80]=1;});Mutate([](auto& U){for(int I=81;I<97;++I)U.PageWords[I]=0;});
    Mutate([](auto& U){U.PageWords[U.PageWords[11]]=0;});Mutate([](auto& U){U.PageWords[U.PageWords[11]]=2;});Mutate([](auto& U){U.PageWords[U.PageWords[11]]|=1u<<16;});
    Mutate([](auto& U){U.PageWords[U.PageWords[10]+4]=4;});Mutate([](auto& U){U.PageWords[U.PageWords[10]+5]=1;});Mutate([](auto& U){U.PageWords[U.PageWords[10]]=99;});
    Mutate([](auto& U){++U.Generation;});Mutate([](auto& U){++U.PageKey.X;});Mutate([](auto& U){auto S=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>(*U.Sources);S->SourceRanges[1]=FUintVector2(MAX_uint32,30);U.Sources=S;});
    FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;Config.ChunkCapacity=2;Config.OccWordCapacity=1024;Config.MatWordCapacity=1024;Pool.Init(Config);auto Air=MakeShared<FVoxelBrickCpuPack,ESPMode::ThreadSafe>();Air->Desc.SetNumZeroed(128);const FVoxelBrickChunkKey Key{-1,-1,-1,1};
    if(!TestTrue(TEXT("real brick allocation accepts v3 metadata"),Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading::Neutral(),Valid[0])!=INDEX_NONE))return false;
    FVoxelBrickPool::FResidentChunk Before;Pool.DebugGetResidentChunk(Key,Before);
    for(const auto& U:Invalid){TestFalse(TEXT("strict v3 malformed layout refused"),VoxelValidateRecursiveAppearancePage(*U));TestEqual(TEXT("brick refuses before replacement"),Pool.AddChunkFromCpu(Air,Key,FVoxelBrickChunkShading::Neutral(),U),INDEX_NONE);FVoxelBrickPool::FResidentChunk After;Pool.DebugGetResidentChunk(Key,After);TestEqual(TEXT("bad page cannot retire valid geometry"),After.AddSequence,Before.AddSequence);TestTrue(TEXT("bad page cannot replace source metadata"),After.Appearance==Valid[0]);}
    TArray<FString> Failures;
    ENQUEUE_RENDER_COMMAND(VoxelRecursiveAppearanceAdmissionTest)([&](FRHICommandListImmediate& Cmd){FVoxelTerrainAppearanceGpuState State;auto Check=[&](bool V,const TCHAR* M){if(!V)Failures.Add(M);};
        auto Read=[&](const FVoxelTerrainAppearanceUpload* Expected){FRDGBuilder Graph(Cmd);auto B=State.Register(Graph);FRHIGPUBufferReadback D(TEXT("Recursive.Slot")),P(TEXT("Recursive.Page"));AddEnqueueCopyPass(Graph,&D,B.Slots,32);AddEnqueueCopyPass(Graph,&P,B.Pages,512);Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();if(!D.IsReady()||!P.IsReady()){Check(false,TEXT("recursive readback ready"));return;}const auto* DP=static_cast<const uint32*>(D.Lock(32));const auto* PP=static_cast<const uint32*>(P.Lock(512));if(!DP||!PP){Check(false,TEXT("recursive readback lock"));if(DP)D.Unlock();if(PP)P.Unlock();return;}if(Expected){Check(DP[5]==uint32(Expected->PageWords.Num()),TEXT("v3 GPU descriptor exact count"));Check(DP[4]+DP[5]<=128,TEXT("v3 fixture readback bounds"));if(DP[4]+DP[5]<=128)Check(FMemory::Memcmp(PP+DP[4],Expected->PageWords.GetData(),Expected->PageWords.Num()*4)==0,TEXT("v3 offsets and original packet bytes retained"));}else Check(DP[4]==0&&DP[5]==0&&DP[6]==0&&DP[7]==0,TEXT("malformed RT replacement clears stale descriptor"));P.Unlock();D.Unlock();};
        for(const auto& U:Valid){TArray<FVoxelTerrainAppearanceGpuState::FEntry> Entries{{1,U}};Check(State.ApplyBatch(Cmd,2,{},Entries,Error),TEXT("v3 all levels GPU admitted"));Read(U.Get());}
        for(const auto& U:Invalid){TArray<FVoxelTerrainAppearanceGpuState::FEntry> Entries{{1,Valid[0]}};Check(State.ApplyBatch(Cmd,2,{},Entries,Error),TEXT("valid restored before malformed replacement"));Entries[0].Upload=U;Check(!State.ApplyBatch(Cmd,2,{},Entries,Error),TEXT("GPU independently refuses malformed v3"));Read(nullptr);}
    });FlushRenderingCommands();for(const auto& Fails:Failures)AddError(Fails);Pool.RemoveChunk(Key);return Failures.IsEmpty();
}
#endif

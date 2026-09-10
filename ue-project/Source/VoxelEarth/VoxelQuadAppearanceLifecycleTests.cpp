#include "VoxelTerrainAppearancePage.h"
#include "VoxelGpuPoolComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "AssetCompilingManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Math/OrthoMatrix.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "VoxelQuadSurfaceProbe.h"
#include "VoxelTerrainAppearanceGpuState.h"
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
void QuadLifePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FString QuadLifeSHA(const TArray<uint8>& B){uint8 H[32];check(SHA256(B.GetData(),B.Num(),H));return BytesToHex(H,32).ToLower();}
void QuadLifeSeal(TArray<uint8>& B){TArray<uint8> C;C.Append(B.GetData(),96);C.Append(B.GetData()+128,B.Num()-128);check(SHA256(C.GetData(),C.Num(),B.GetData()+96));}
struct FQuadLifeFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/AppearanceCatalog")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TArray<uint8> Geometry,Packet;FString Hash;TArray<TSharedPtr<FJsonValue>> Rows;
    FQuadLifeFixture(bool Leaf=false,int Width=1){
        const int SX=Width,SZ=Leaf?2:1,Count=SX*SZ;
        Geometry.SetNumZeroed(48+Count*5);QuadLifePut(Geometry,0,vxc::kVxaMagic);QuadLifePut(Geometry,4,vxc::kVxaVersion);
        QuadLifePut(Geometry,8,uint32(-1));QuadLifePut(Geometry,12,uint32(-2));QuadLifePut(Geometry,16,uint32(-3));
        QuadLifePut(Geometry,20,SX);QuadLifePut(Geometry,24,1);QuadLifePut(Geometry,28,SZ);QuadLifePut(Geometry,32,100);QuadLifePut(Geometry,36,Count);
        for(int X=0;X<SX;++X)for(int Z=0;Z<SZ;++Z){const int N=X*SZ+Z;Geometry[48+N*5]=Leaf&&Z==1?19:16;QuadLifePut(Geometry,49+N*5,1);}
        Hash=FMD5::HashBytes(Geometry.GetData(),Geometry.Num()).ToLower();
        Packet.SetNumZeroed(128+Count*10);FMemory::Memcpy(Packet.GetData(),"VAC1",4);QuadLifePut(Packet,4,1);
        QuadLifePut(Packet,8,SX);QuadLifePut(Packet,12,1);QuadLifePut(Packet,16,SZ);QuadLifePut(Packet,20,uint32(-1));QuadLifePut(Packet,24,uint32(-2));QuadLifePut(Packet,28,uint32(-3));QuadLifePut(Packet,32,100);QuadLifePut(Packet,36,Count);HexToBytes(Hash,Packet.GetData()+48);
        for(int X=0;X<SX;++X)for(int Z=0;Z<SZ;++Z){const int N=128+(X*SZ+Z)*10;Packet[N]=uint8(X);Packet[N+4]=uint8(Z);Packet[N+6]=Leaf&&Z==1?19:16;const auto C=Leaf?(Z==1?FColor(uint8(61+X*45),uint8(151-X*30),43):FColor(181,51,41)):FColor(91,147,23);Packet[N+7]=C.R;Packet[N+8]=C.G;Packet[N+9]=C.B;}
        QuadLifeSeal(Packet);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);
        IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks/test-oak")),true);
    }
    ~FQuadLifeFixture(){IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    TSharedPtr<FJsonObject> Add(int Seed){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("id"),FString::Printf(TEXT("test-oak-%04d"),Seed));O->SetStringField(TEXT("species"),TEXT("test-oak"));O->SetNumberField(TEXT("seed"),Seed);O->SetStringField(TEXT("geometry_md5"),Hash);O->SetStringField(TEXT("geometry_sha256"),QuadLifeSHA(Geometry));O->SetStringField(TEXT("sha256"),QuadLifeSHA(Packet));O->SetStringField(TEXT("file"),Hash+TEXT(".vac"));Rows.Add(MakeShared<FJsonValueObject>(O));check(FFileHelper::SaveArrayToFile(Geometry,*(Directory/TEXT("banks/test-oak")/(FString::Printf(TEXT("test-oak-%04d.vxa"),Seed)))));return O;}
    void Write(){auto O=MakeShared<FJsonObject>();O->SetArrayField(TEXT("models"),Rows);FString Text;auto W=TJsonWriterFactory<>::Create(&Text);check(FJsonSerializer::Serialize(O,W));check(FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("appearance/published.json"))));check(FFileHelper::SaveArrayToFile(Packet,*(Directory/TEXT("appearance")/(Hash+TEXT(".vac")))));}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelQuadAppearanceLifecycleTest,"Voxel.Appearance.QuadPoolLifecycle",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelQuadAppearanceLifecycleTest::RunTest(const FString&){
    if(!TestTrue(TEXT("SM6 RHI initialized"),GIsRHIInitialized&&GMaxRHIFeatureLevel>=ERHIFeatureLevel::SM6))return false;
    FQuadLifeFixture F;F.Add(1);F.Write();FString Error;auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
    if(!TestTrue(TEXT("real published fixture"),Catalog.IsValid()))return false;
    FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid;
    if(!TestTrue(TEXT("actual VXA parsed"),Grid.parse(F.Geometry.GetData(),size_t(F.Geometry.Num()))==vxc::AssetParseError::kOk))return false;
    Binding.Observe(Grid,F.Geometry.GetData(),size_t(F.Geometry.Num()));
    vxc::AssetField::ResolvedAssetInstance Instance;Instance.grid=&Grid;Instance.anchorVx=-16;Instance.anchorVy=-16;Instance.anchorVz=-16;Instance.yawQuarter=3;
    auto Prepare=[&](uint64 Generation){auto Page=FVoxelTerrainAppearancePage::Prepare(FIntVector(-1,-1,-1),0,Generation,{Instance},Binding,[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Error);return Page?Page->MakeUpload(Binding,Error):nullptr;};
    auto Upload=Prepare(11),Replacement=Prepare(12);
    if(!TestTrue(TEXT("canonical immutable uploads"),Upload.IsValid()&&Replacement.IsValid()))return false;
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));
    if(!TestNotNull(TEXT("isolated renderer world"),World))return false;
    auto Actor=World->SpawnActor<AActor>();auto Pool=NewObject<UVoxelGpuPoolComponent>(Actor);Actor->SetRootComponent(Pool);Actor->AddInstanceComponent(Pool);
    Pool->SetPoolName(TEXT("AppearanceLifecycleFixture"));Pool->SetChunkMaterial(UMaterial::GetDefaultMaterial(MD_Surface));Pool->InitPool(128);Pool->RegisterComponent();
    auto Sync=[&](){World->SendAllEndOfFrameUpdates();FlushRenderingCommands();};
    const FVector3f Origin(-320,-320,-320);const FVector4f Params(.5f,.5f,UVoxelGpuPoolComponent::kNoSurfaceGate,0);
    const TArray<uint64> A={uint64(16)<<24,uint64(16)<<24|1},B={uint64(16)<<24|7,uint64(16)<<24|9};
    int32 Handle=Pool->AddChunk(A,Origin,0,Params,nullptr,Upload);Sync();
    auto Runs=Pool->DebugGetChunkRuns();bool OK=TestTrue(TEXT("actual first chunk resident"),Handle!=INDEX_NONE&&Runs.Num()==1);
    auto ReadCheck=[&](const UVoxelGpuPoolComponent::FChunkRun& Run,const TArray<uint64>* Quads,TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe> Expected,bool Reset=false){
        auto Buffers=Pool->DebugGetPoolBuffers();if(!TestTrue(TEXT("actual persistent quad buffers"),Buffers.IsValid()&&Buffers->QuadBuffer.IsValid()))return false;
        TArray<FString> Failures;
        ENQUEUE_RENDER_COMMAND(VoxelQuadAppearanceLifecycleRead)([&](FRHICommandListImmediate& Cmd){
            auto Check=[&](bool Value,const TCHAR* What){if(!Value)Failures.Add(What);};
            FRDGBuilder Graph(Cmd);auto State=Buffers->Appearance->Register(Graph);
            FRHIGPUBufferReadback Q(TEXT("QuadLife.Geometry")),I(TEXT("QuadLife.Ids")),P(TEXT("QuadLife.Pages")),S(TEXT("QuadLife.Sources")),R(TEXT("QuadLife.Ranges")),D(TEXT("QuadLife.Descriptors"));
            auto Quad=Graph.RegisterExternalBuffer(Buffers->QuadPooled),Ids=Graph.RegisterExternalBuffer(Buffers->ChunkIdPooled);
            AddEnqueueCopyPass(Graph,&Q,Quad,1024);AddEnqueueCopyPass(Graph,&I,Ids,512);
            const uint32 PB=Reset?4:4096,SB=Reset?4:4096,RB=Reset?8:256,DB=Reset?16:256;
            AddEnqueueCopyPass(Graph,&P,State.Pages,PB);AddEnqueueCopyPass(Graph,&S,State.Sources,SB);AddEnqueueCopyPass(Graph,&R,State.Ranges,RB);AddEnqueueCopyPass(Graph,&D,State.Slots,DB);
            Graph.Execute();Cmd.SubmitAndBlockUntilGPUIdle();
            auto Copy=[&](FRHIGPUBufferReadback& Back,uint32 Bytes,TArray<uint32>& Out){if(!Back.IsReady())return false;auto Data=Back.Lock(Bytes);if(!Data)return false;Out.SetNumUninitialized(Bytes/4);FMemory::Memcpy(Out.GetData(),Data,Bytes);Back.Unlock();return true;};
            TArray<uint32> QB,IB,PBs,SBs,RBs,DBs;
            if(!(Copy(Q,1024,QB)&&Copy(I,512,IB)&&Copy(P,PB,PBs)&&Copy(S,SB,SBs)&&Copy(R,RB,RBs)&&Copy(D,DB,DBs))){Check(false,TEXT("GPU readbacks ready"));return;}
            if(Quads)for(int32 N=0;N<Quads->Num();++N){const uint32 Index=Run.FirstQuad+N;Check(QB[2*Index]==uint32((*Quads)[N])&&QB[2*Index+1]==uint32((*Quads)[N]>>32),TEXT("exact live quad bytes"));Check(IB[Index]==Run.ChunkId,TEXT("geometry points at same descriptor ID"));}
            if(!Quads&&!Reset)for(uint32 N=0;N<Run.NumQuads;++N)Check(IB[Run.FirstQuad+N]==0,TEXT("removed geometry IDs hidden"));
            if(Reset){Check(DBs[0]==0&&DBs[1]==0&&DBs[2]==0&&DBs[3]==0,TEXT("reset binds zero dummy descriptor"));return;}
            const uint32 O=Run.ChunkId*4;if(O+3>=uint32(DBs.Num())){Check(false,TEXT("descriptor read bounds"));return;}
            if(!Expected){Check(DBs[O]==0&&DBs[O+1]==0&&DBs[O+2]==0&&DBs[O+3]==0,TEXT("retired descriptor zero"));return;}
            const uint32 Base=DBs[O],Count=DBs[O+1],RangeBase=DBs[O+2],RangeCount=DBs[O+3];
            Check(Count==uint32(Expected->PageWords.Num())&&Base+Count<=uint32(PBs.Num()),TEXT("descriptor names exact page"));
            if(Count==uint32(Expected->PageWords.Num())&&Base+Count<=uint32(PBs.Num()))Check(FMemory::Memcmp(PBs.GetData()+Base,Expected->PageWords.GetData(),Count*4)==0,TEXT("canonical generation/page bytes match"));
            Check(RangeCount==uint32(Expected->Sources->SourceRanges.Num()),TEXT("same immutable source range count"));
            for(uint32 N=1;N<RangeCount;++N){uint32 RO=(RangeBase+N)*2;if(RO+1>=uint32(RBs.Num())){Check(false,TEXT("range bounds"));break;}const auto Original=Expected->Sources->SourceRanges[N];const uint32 SourceBase=RBs[RO],SourceCount=RBs[RO+1];Check(SourceCount==Original.Y&&SourceBase+SourceCount<=uint32(SBs.Num()),TEXT("source relocation bounds"));if(SourceCount==Original.Y&&SourceBase+SourceCount<=uint32(SBs.Num()))Check(FMemory::Memcmp(SBs.GetData()+SourceBase,Expected->Sources->SourceWords.GetData()+Original.X,SourceCount*4)==0,TEXT("approved original source bytes preserved"));}
        });FlushRenderingCommands();for(auto& Failure:Failures)AddError(Failure);return Failures.IsEmpty();
    };
    if(OK){auto Run=Runs[0];OK&=ReadCheck(Run,&A,Upload);
        Handle=Pool->UpdateChunk(Handle,B,Replacement);Sync();OK&=TestTrue(TEXT("in-place update retains handle"),Handle!=INDEX_NONE);OK&=ReadCheck(Run,&B,Replacement);
        Pool->UpdateChunk(Handle,A);Sync();OK&=ReadCheck(Run,&A,nullptr);
        Pool->UpdateChunk(Handle,B,Upload);Sync();OK&=ReadCheck(Run,&B,Upload);
        // Recreate with an actual pending CPU edit. This exercises the old lost-dirty-slice bug.
        Pool->DebugDeferProxyRecreation();Pool->UpdateChunk(Handle,A,Replacement);Sync();OK&=ReadCheck(Run,&A,Replacement);
        auto Holder=Pool->DebugGetPoolBuffers();Pool->MarkRenderStateDirty();Sync();OK&=TestTrue(TEXT("ordinary recreation retains holder"),Holder==Pool->DebugGetPoolBuffers());OK&=ReadCheck(Run,&A,Replacement);
        auto Gpu=MakeShared<FVoxelGpuQuadPayload,ESPMode::ThreadSafe>();Gpu->NumQuads=B.Num();
        ENQUEUE_RENDER_COMMAND(VoxelQuadLifecycleSource)([Gpu,B](FRHICommandListImmediate& Cmd){
            FRDGBuilder Graph(Cmd);auto Buffer=Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(8,B.Num()),TEXT("QuadLife.Source"));
            Graph.QueueBufferUpload(Buffer,B.GetData(),B.Num()*8,ERDGInitialDataFlags::None);Graph.QueueBufferExtraction(Buffer,&Gpu->Quads);Graph.Execute();
        });FlushRenderingCommands();
        const int32 GpuHandle=Pool->AddChunkFromGpu(Gpu,B.Num(),Origin,0,Params,Upload);Sync();
        Runs=Pool->DebugGetChunkRuns();OK&=TestTrue(TEXT("actual GPU-copy chunk admitted"),GpuHandle!=INDEX_NONE&&Runs.Num()==2);
        if(Runs.Num()==2){auto GpuRun=Runs[0].ChunkId==Run.ChunkId?Runs[1]:Runs[0];OK&=ReadCheck(GpuRun,&B,Upload);
            Pool->DebugDeferProxyRecreation();Pool->UpdateChunk(Handle,B,Replacement);Sync();
            OK&=ReadCheck(Run,&B,Replacement);OK&=ReadCheck(GpuRun,&B,Upload);
            Pool->RemoveChunk(GpuHandle);Sync();OK&=ReadCheck(GpuRun,nullptr,nullptr);
        }
        Pool->RemoveChunk(Handle);Sync();OK&=ReadCheck(Run,nullptr,nullptr);
        Handle=Pool->AddChunk(B,Origin,0,Params,nullptr,Upload);Sync();Runs=Pool->DebugGetChunkRuns();OK&=TestTrue(TEXT("removed ChunkId reused"),Runs.Num()==1&&Runs[0].ChunkId==Run.ChunkId);if(Runs.Num()==1)OK&=ReadCheck(Runs[0],&B,Upload);
        Pool->ClearChunks();Sync();OK&=ReadCheck(Run,nullptr,nullptr,true);
        Handle=Pool->AddChunk(A,Origin,0,Params,nullptr,Replacement);Sync();Runs=Pool->DebugGetChunkRuns();OK&=TestTrue(TEXT("readmitted after reset"),Handle!=INDEX_NONE&&Runs.Num()==1);if(Runs.Num()==1)OK&=ReadCheck(Runs[0],&A,Replacement);
    }
    Pool->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();return OK;
}

namespace {
// A synchronous capture must never measure the temporary default material while
// its actual material or receiver mesh is still compiling asynchronously.
bool QuadRasterMaterialReady(FAutomationTestBase& Test,UMaterialInterface* Material){
    FAssetCompilingManager::Get().FinishAllCompilation();
    FlushRenderingCommands();
    bool Ready=false;FString ActualName;
    const FMaterialRenderProxy* Proxy=Material->GetRenderProxy();
    // First RT lookup may itself submit missing material permutation jobs.
    ENQUEUE_RENDER_COMMAND(VoxelQuadRasterPrimeMaterial)([Proxy](FRHICommandListImmediate&){
        const FMaterialRenderProxy* Fallback=nullptr;
        Proxy->GetMaterialWithFallback(GMaxRHIFeatureLevel,Fallback);
    });
    FlushRenderingCommands();
    FAssetCompilingManager::Get().FinishAllCompilation();
    FlushRenderingCommands();
    ENQUEUE_RENDER_COMMAND(VoxelQuadRasterMaterialReady)([&](FRHICommandListImmediate&){
        const FMaterialRenderProxy* Used=nullptr;
        const FMaterial& Actual=Proxy->GetMaterialWithFallback(GMaxRHIFeatureLevel,Used);
        ActualName=Actual.GetFriendlyName();
        // UE leaves the out parameter untouched (null) when no fallback is used.
        Ready=Used==nullptr&&Actual.IsRenderingThreadShaderMapComplete();
    });
    FlushRenderingCommands();
    Test.AddInfo(FString::Printf(TEXT("Raster material %s resolves to %s; complete without fallback=%d"),*Material->GetPathName(),*ActualName,Ready));
    return Test.TestTrue(TEXT("actual raster material compiled, without fallback"),Ready);
}
uint64 QuadRasterTop(int X,int Y,int Z,int W,int H,uint8 Material){return uint64(2u|(1u<<4)|(uint32(Z)<<8)|(uint32(X)<<16)|(uint32(Y)<<24))|(uint64(uint32(W)|(uint32(H)<<8)|(255u<<16)|(uint32(Material)<<24))<<32);}
FIntVector QuadRasterRotate(FIntVector P,int Yaw){for(int I=0;I<Yaw;++I)P=FIntVector(-P.Y,P.X,P.Z);return P;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelQuadRasterAppearanceTest,"Voxel.Appearance.QuadRasterPixels",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelQuadRasterAppearanceTest::RunTest(const FString&){
    auto Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain"));
    if(!TestNotNull(TEXT("actual terrain material"),Material)||!TestEqual(TEXT("actual masked terrain"),Material->GetBlendMode(),BLEND_Masked))return false;
    if(!QuadRasterMaterialReady(*this,Material))return false;
    constexpr int Size=128;FString Error;int TotalHoles=0,TotalLeaves=0;
    for(int Mode=0;Mode<2;++Mode)for(int Yaw=0;Yaw<(Mode?4:1);++Yaw){
        const bool Leaf=Mode!=0;FQuadLifeFixture F(Leaf,Leaf?2:1);F.Add(1);F.Write();auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);
        if(!TestTrue(TEXT("raster source catalog"),Catalog.IsValid()))return false;FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid;
        if(!TestTrue(TEXT("raster geometry parsed"),Grid.parse(F.Geometry.GetData(),F.Geometry.Num())==vxc::AssetParseError::kOk))return false;Binding.Observe(Grid,F.Geometry.GetData(),F.Geometry.Num());
        vxc::AssetField::ResolvedAssetInstance Instance;Instance.grid=&Grid;Instance.anchorVx=-16;Instance.anchorVy=-16;Instance.anchorVz=-16;Instance.yawQuarter=Yaw;
        auto Page=FVoxelTerrainAppearancePage::Prepare(FIntVector(-1,-1,-1),0,23,{Instance},Binding,[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Error);
        auto Upload=Page?Page->MakeUpload(Binding,Error):nullptr;if(!TestTrue(TEXT("raster immutable upload"),Upload.IsValid()))return false;
        auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!TestNotNull(TEXT("raster isolated world"),World))return false;
        auto Actor=World->SpawnActor<AActor>();auto Pool=NewObject<UVoxelGpuPoolComponent>(Actor);Actor->SetRootComponent(Pool);Actor->AddInstanceComponent(Pool);Pool->SetChunkMaterial(Material);Pool->SetPoolName(TEXT("AppearanceRasterFixture"));Pool->InitPool(128);Pool->RegisterComponent();
        FIntVector SourceCells[2];for(int X=0;X<(Leaf?2:1);++X)SourceCells[X]=FIntVector(-16,-16,-16)+QuadRasterRotate(FIntVector(X-1,-2,-3),Yaw);
        const FIntVector First=SourceCells[0],Last=SourceCells[Leaf?1:0];const int MinX=FMath::Min(First.X,Last.X),MinY=FMath::Min(First.Y,Last.Y),W=FMath::Abs(Last.X-First.X)+1,H=FMath::Abs(Last.Y-First.Y)+1;
        TArray<uint64> Quads{QuadRasterTop(MinX+32,MinY+32,First.Z+32,W,H,16)};if(Leaf)Quads.Add(QuadRasterTop(MinX+32,MinY+32,First.Z+33,W,H,19));
        const int32 Handle=Pool->AddChunk(Quads,FVector3f(-320),0,FVector4f(.5f,.5f,UVoxelGpuPoolComponent::kNoSurfaceGate,0),nullptr,Upload);
        if(!TestTrue(TEXT("valid render faces admitted"),Handle!=INDEX_NONE)){World->DestroyWorld(false);return false;}
        auto CaptureActor=World->SpawnActor<AActor>();auto Capture=NewObject<USceneCaptureComponent2D>(CaptureActor);CaptureActor->SetRootComponent(Capture);CaptureActor->AddInstanceComponent(Capture);
        const float TopZ=float(First.Z+(Leaf?2:1))*10.f,Width=Leaf?28.f:16.f;
        const FVector Center((float(MinX)+float(W)*.5f)*10.f,(float(MinY)+float(H)*.5f)*10.f,TopZ);
        Capture->SetWorldLocation(Center+FVector(0,0,100));Capture->SetWorldRotation(FRotator(-90,0,0));Capture->ProjectionType=ECameraProjectionMode::Orthographic;Capture->OrthoWidth=Width;Capture->bAutoCalculateOrthoPlanes=false;Capture->bUpdateOrthoPlanes=false;Capture->bUseCustomProjectionMatrix=true;Capture->CustomProjectionMatrix=FReversedZOrthoMatrix(Width*.5f,Width*.5f,1.f/999.f,-1.f);Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->bAlwaysPersistRenderingState=true;
        Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;Capture->ShowOnlyComponent(Pool);Capture->ShowFlags.SetTemporalAA(false);Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetFog(false);Capture->ShowFlags.SetAtmosphere(false);Capture->RegisterComponent();
        auto Target=NewObject<UTextureRenderTarget2D>(CaptureActor);Target->RenderTargetFormat=RTF_RGBA32f;Target->ClearColor=FLinearColor::Black;Target->InitAutoFormat(Size,Size);Target->UpdateResourceImmediate(true);Capture->TextureTarget=Target;
        auto Read=[&](ESceneCaptureSource Kind,TArray<FLinearColor>& Pixels){Capture->CaptureSource=Kind;World->SendAllEndOfFrameUpdates();FlushRenderingCommands();Capture->CaptureScene();FlushRenderingCommands();FReadSurfaceDataFlags Flags(RCM_MinMax);Flags.SetLinearToGamma(false);return Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels,Flags)&&Pixels.Num()==Size*Size;};
        TArray<FLinearColor> Color,Depth;bool OK=Read(SCS_BaseColor,Color)&&Read(SCS_SceneDepth,Depth);
        if(!TestTrue(TEXT("actual base-color and scene-depth captures read"),OK)){Pool->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();return false;}
        TArray<FColor> PNG;for(auto C:Color)PNG.Add(C.ToFColor(true));TArray<uint8> Compressed;FImageUtils::ThumbnailCompressImageArray(Size,Size,PNG,Compressed);FFileHelper::SaveArrayToFile(Compressed,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("quad-raster-%s-yaw%d.png"),Leaf?TEXT("leaf"):TEXT("opaque"),Yaw)));
        const FRotationMatrix Rotation(Capture->GetComponentRotation());const FVector Right=Rotation.GetUnitAxis(EAxis::Y),Up=Rotation.GetUnitAxis(EAxis::Z);
        TArray<FVoxelQuadSurfaceProbeQuery> Queries;TArray<int> PixelIndices,SourceXs;TArray<FLinearColor> FrontColors,BackColors;
        const auto Runs=Pool->DebugGetChunkRuns();const uint32 ChunkId=Runs.Num()==1?Runs[0].ChunkId:0;
        for(int PY=0;PY<Size;++PY)for(int PX=0;PX<Size;++PX){const FVector Point=Center+Right*((float(PX)+.5f)/Size-.5f)*Width+Up*(.5f-(float(PY)+.5f)/Size)*Width;
            const float VX=float(Point.X/10.),VY=float(Point.Y/10.);const int CX=FMath::FloorToInt(VX),CY=FMath::FloorToInt(VY);const float FX=VX-CX,FY=VY-CY;if(FX<.1f||FX>.9f||FY<.1f||FY>.9f)continue;
            int SourceX=-1;for(int X=0;X<(Leaf?2:1);++X)if(SourceCells[X].X==CX&&SourceCells[X].Y==CY)SourceX=X;if(SourceX<0)continue;
            FVoxelQuadSurfaceProbeQuery Q;Q.X=VX+32;Q.Y=VY+32;Q.Z=float(First.Z+32)+(Leaf?1.5f:.5f);Q.Identity=float(ChunkId*8+5);Q.Material=Leaf?19:16;Q.DxY=Width/Size/10;Q.DyX=-Width/Size/10;Queries.Add(Q);PixelIndices.Add(PY*Size+PX);SourceXs.Add(SourceX);
            const FColor Front=Leaf?FColor(uint8(61+SourceX*45),uint8(151-SourceX*30),43):FColor(91,147,23),Back=Leaf?FColor(181,51,41):Front;
            FrontColors.Add(FVoxelAssetAppearance::FaceColor(Front,FIntVector(SourceX,0,Leaf?1:0),2,true));BackColors.Add(FVoxelAssetAppearance::FaceColor(Back,FIntVector(SourceX,0,0),2,true));
        }
        TArray<uint32> Sources=Upload->Sources->SourceWords;TArray<FUintVector4> Slots;Slots.SetNumZeroed(ChunkId+1);Slots[ChunkId]=FUintVector4(0,Upload->PageWords.Num(),0,Upload->Sources->SourceRanges.Num());TArray<FVoxelQuadSurfaceProbeResult> Expected;
        if(!VoxelRunQuadSurfaceProbe(Upload->PageWords,Sources,Upload->Sources->SourceRanges,Slots,Queries,Expected,Error)){AddError(Error);OK=false;}
        int BadColor=0,BadDepth=0,Holes=0,Leaves=0;uint32 SeenSources=0;
        if(OK)for(int N=0;N<Expected.Num();++N){const auto& E=Expected[N];if(E.Approved!=1){OK=false;break;}if(E.Coverage>0&&E.Coverage<1)continue;const bool Hole=E.Coverage<.5f;const int Pixel=PixelIndices[N];const auto Wanted=Hole?BackColors[N]:FrontColors[N];// SCS_BaseColor deliberately writes alpha zero; FaceColor returns
        // alpha one. Only RGB represents albedo (SceneCapturePixelShader.usf).
        const auto Actual=Color[Pixel];BadColor+=!FMath::IsNearlyEqual(Actual.R,Wanted.R,.018f)||!FMath::IsNearlyEqual(Actual.G,Wanted.G,.018f)||!FMath::IsNearlyEqual(Actual.B,Wanted.B,.018f);
        if(N==0){TestEqual(TEXT("base-color capture alpha is deliberately zero"),Actual.A,0.f);AddInfo(FString::Printf(TEXT("Raster first actual RGBA %.6f %.6f %.6f %.6f; expected RGB %.6f %.6f %.6f"),Actual.R,Actual.G,Actual.B,Actual.A,Wanted.R,Wanted.G,Wanted.B));}BadDepth+=!FMath::IsNearlyEqual(Depth[Pixel].R,Hole?110.f:100.f,.35f);if(Hole)++Holes;else ++Leaves;SeenSources|=1u<<SourceXs[N];}
        TestTrue(TEXT("rendered source samples available"),OK&&Queries.Num()>64);TestEqual(TEXT("actual raster source face colors"),BadColor,0);TestEqual(TEXT("actual hardware depth follows cutout backstop"),BadDepth,0);TestEqual(TEXT("all greedy source cells sampled"),SeenSources,Leaf?3u:1u);
        if(Leaf){TestTrue(TEXT("leaf pixels and backstop openings both rendered"),Holes>8&&Leaves>8);TotalHoles+=Holes;TotalLeaves+=Leaves;}
        AddInfo(FString::Printf(TEXT("Quad raster %s yaw%d: sampled%d leaf%d holes%d colorErrors%d depthErrors%d"),Leaf?TEXT("leaf"):TEXT("opaque"),Yaw,Queries.Num(),Leaves,Holes,BadColor,BadDepth));
        Pool->DestroyComponent();Capture->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();if(!OK||BadColor||BadDepth)return false;
    }
    TestTrue(TEXT("all-yaw raster leaf and cutout depth evidence"),TotalHoles>32&&TotalLeaves>32);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelQuadShadowAppearanceTest,"Voxel.Appearance.QuadShadowPixels",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelQuadShadowAppearanceTest::RunTest(const FString&){
    auto TerrainMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain"));auto Cube=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"));auto White=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if(!TestNotNull(TEXT("shadow actual terrain material"),TerrainMaterial)||!TestNotNull(TEXT("shadow receiver mesh"),Cube)||!TestNotNull(TEXT("shadow receiver material"),White))return false;
    TestEqual(TEXT("shadow terrain uses masked blend"),TerrainMaterial->GetBlendMode(),BLEND_Masked);
    if(!QuadRasterMaterialReady(*this,TerrainMaterial)||!QuadRasterMaterialReady(*this,White))return false;
    constexpr int Size=128;FString Error;int AllOpen=0,AllClosed=0;
    for(int Yaw=0;Yaw<4;++Yaw){
        FQuadLifeFixture F(true,2);F.Add(1);F.Write();auto Catalog=FVoxelPublishedAppearanceCatalog::Load(F.Directory,Error);if(!TestTrue(TEXT("shadow catalog verified"),Catalog.IsValid()))return false;FVoxelAppearanceBankBinding Binding(Catalog);vxc::AssetGrid Grid;
        if(!TestTrue(TEXT("shadow source VXA"),Grid.parse(F.Geometry.GetData(),F.Geometry.Num())==vxc::AssetParseError::kOk))return false;Binding.Observe(Grid,F.Geometry.GetData(),F.Geometry.Num());vxc::AssetField::ResolvedAssetInstance Instance;Instance.grid=&Grid;Instance.anchorVx=-16;Instance.anchorVy=-16;Instance.anchorVz=-16;Instance.yawQuarter=Yaw;
        auto Page=FVoxelTerrainAppearancePage::Prepare(FIntVector(-1,-1,-1),0,29,{Instance},Binding,[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Error);auto Upload=Page?Page->MakeUpload(Binding,Error):nullptr;if(!TestTrue(TEXT("shadow canonical page"),Upload.IsValid()))return false;
        const FIntVector First=FIntVector(-16,-16,-16)+QuadRasterRotate(FIntVector(-1,-2,-3),Yaw),Last=FIntVector(-16,-16,-16)+QuadRasterRotate(FIntVector(0,-2,-3),Yaw);const int MinX=FMath::Min(First.X,Last.X),MinY=FMath::Min(First.Y,Last.Y),W=FMath::Abs(First.X-Last.X)+1,H=FMath::Abs(First.Y-Last.Y)+1;const float LeafZ=float(First.Z+2)*10,ReceiverZ=LeafZ-40,Width=28;
        const FVector Center((float(MinX)+float(W)*.5f)*10,(float(MinY)+float(H)*.5f)*10,ReceiverZ);
        auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!TestNotNull(TEXT("shadow isolated world"),World))return false;
        auto Actor=World->SpawnActor<AActor>();auto Pool=NewObject<UVoxelGpuPoolComponent>(Actor);Actor->SetRootComponent(Pool);Actor->AddInstanceComponent(Pool);Pool->InitPool(128);Pool->SetChunkMaterial(TerrainMaterial);Pool->SetPoolName(TEXT("AppearanceShadowFixture"));Pool->RegisterComponent();const TArray<uint64> Quads{QuadRasterTop(MinX+32,MinY+32,First.Z+33,W,H,19)};const int32 Handle=Pool->AddChunk(Quads,FVector3f(-320),0,FVector4f(.5,.5,UVoxelGpuPoolComponent::kNoSurfaceGate,0));
        auto ReceiverActor=World->SpawnActor<AActor>();auto Receiver=NewObject<UStaticMeshComponent>(ReceiverActor);ReceiverActor->SetRootComponent(Receiver);ReceiverActor->AddInstanceComponent(Receiver);Receiver->SetMobility(EComponentMobility::Movable);Receiver->SetStaticMesh(Cube);Receiver->SetMaterial(0,White);Receiver->SetWorldLocation(Center-FVector(0,0,.5));Receiver->SetWorldScale3D(FVector(.4,.4,.01));Receiver->SetCastShadow(false);Receiver->RegisterComponent();
        auto LightActor=World->SpawnActor<AActor>();auto Light=NewObject<UDirectionalLightComponent>(LightActor);LightActor->SetRootComponent(Light);LightActor->AddInstanceComponent(Light);Light->SetMobility(EComponentMobility::Movable);Light->SetWorldRotation(FRotator(-90,0,0));Light->SetIntensity(3);Light->SetCastShadows(true);Light->SetCastRaytracedShadows(ECastRayTracedShadow::Disabled);Light->SetDynamicShadowDistanceMovableLight(500);Light->SetDynamicShadowCascades(1);Light->SetShadowBias(.001f);Light->SetShadowSlopeBias(0);Light->RegisterComponent();
        auto CaptureActor=World->SpawnActor<AActor>();auto Capture=NewObject<USceneCaptureComponent2D>(CaptureActor);CaptureActor->SetRootComponent(Capture);CaptureActor->AddInstanceComponent(Capture);
        // Camera is BETWEEN leaf and receiver. It cannot see or be occluded by
        // the leaf plane; only the downward light ray crosses that geometry.
        Capture->SetWorldLocation(Center+FVector(0,0,20));Capture->SetWorldRotation(FRotator(-90,0,0));Capture->ProjectionType=ECameraProjectionMode::Orthographic;Capture->OrthoWidth=Width;Capture->bAutoCalculateOrthoPlanes=false;Capture->bUpdateOrthoPlanes=false;Capture->bUseCustomProjectionMatrix=true;Capture->CustomProjectionMatrix=FReversedZOrthoMatrix(Width*.5f,Width*.5f,1.f/999.f,-1.f);Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->bAlwaysPersistRenderingState=true;Capture->CaptureSource=SCS_SceneColorHDR;
        Capture->ShowFlags.SetTemporalAA(false);Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetFog(false);Capture->ShowFlags.SetAtmosphere(false);Capture->ShowFlags.SetEyeAdaptation(false);Capture->ShowFlags.SetAmbientOcclusion(false);Capture->ShowFlags.SetDynamicShadows(true);
        Capture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod=true;Capture->PostProcessSettings.DynamicGlobalIlluminationMethod=EDynamicGlobalIlluminationMethod::None;Capture->PostProcessSettings.bOverride_ReflectionMethod=true;Capture->PostProcessSettings.ReflectionMethod=EReflectionMethod::None;Capture->RegisterComponent();
        auto Target=NewObject<UTextureRenderTarget2D>(CaptureActor);Target->RenderTargetFormat=RTF_RGBA32f;Target->ClearColor=FLinearColor::Black;Target->InitAutoFormat(Size,Size);Target->UpdateResourceImmediate(true);Capture->TextureTarget=Target;
        auto Read=[&](TArray<FLinearColor>& Pixels){World->SendAllEndOfFrameUpdates();FlushRenderingCommands();Capture->CaptureScene();FlushRenderingCommands();FReadSurfaceDataFlags Flags(RCM_MinMax);Flags.SetLinearToGamma(false);return Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels,Flags)&&Pixels.Num()==Size*Size;};
        TArray<FLinearColor> Lit,Opaque,Masked,Depth;Pool->SetCastShadow(false);bool OK=Read(Lit);Pool->SetCastShadow(true);OK=Read(Opaque)&&OK;Pool->UpdateChunk(Handle,Quads,Upload);OK=Read(Masked)&&OK;
        Capture->CaptureSource=SCS_SceneDepth;OK=Read(Depth)&&OK;
        if(!TestTrue(TEXT("shadow lighting and depth captures"),OK)){World->DestroyWorld(false);FlushRenderingCommands();return false;}
        for(int Which=0;Which<3;++Which){const auto& Image=Which==0?Lit:Which==1?Opaque:Masked;TArray<FColor> Pixels;for(auto C:Image)Pixels.Add(C.ToFColor(true));TArray<uint8> Bytes;FImageUtils::ThumbnailCompressImageArray(Size,Size,Pixels,Bytes);FFileHelper::SaveArrayToFile(Bytes,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("quad-shadow-yaw%d-%s.png"),Yaw,Which==0?TEXT("lit"):Which==1?TEXT("opaque"):TEXT("masked"))));}
        const FRotationMatrix Rotation(Capture->GetComponentRotation());const FVector Right=Rotation.GetUnitAxis(EAxis::Y),Up=Rotation.GetUnitAxis(EAxis::Z);const auto Runs=Pool->DebugGetChunkRuns();const uint32 Slot=Runs.Num()==1?Runs[0].ChunkId:0;TArray<FVoxelQuadSurfaceProbeQuery> Queries;TArray<int> Indices;
        for(int PY=0;PY<Size;++PY)for(int PX=0;PX<Size;++PX){const FVector Point=Center+Right*((float(PX)+.5f)/Size-.5f)*Width+Up*(.5f-(float(PY)+.5f)/Size)*Width;const float X=float(Point.X/10),Y=float(Point.Y/10);const int CX=FMath::FloorToInt(X),CY=FMath::FloorToInt(Y);if(CX<MinX||CX>=MinX+W||CY<MinY||CY>=MinY+H||X-CX<.15f||X-CX>.85f||Y-CY<.15f||Y-CY>.85f)continue;FVoxelQuadSurfaceProbeQuery Q;Q.X=X+32;Q.Y=Y+32;Q.Z=float(First.Z+32)+1.5f;Q.Identity=float(Slot*8+5);Q.Material=19;Q.DxY=Width/Size/10;Q.DyX=-Width/Size/10;Queries.Add(Q);Indices.Add(PY*Size+PX);}
        TArray<FUintVector4> Slots;Slots.SetNumZeroed(Slot+1);Slots[Slot]=FUintVector4(0,Upload->PageWords.Num(),0,Upload->Sources->SourceRanges.Num());TArray<FVoxelQuadSurfaceProbeResult> Expected;if(!VoxelRunQuadSurfaceProbe(Upload->PageWords,Upload->Sources->SourceWords,Upload->Sources->SourceRanges,Slots,Queries,Expected,Error)){AddError(Error);OK=false;}
        int Compared=0,Agreement=0,Open=0,Closed=0,DepthErrors=0;double OpaqueContrast=0;
        if(OK)for(int N=0;N<Expected.Num();++N){const int P=Indices[N];DepthErrors+=!FMath::IsNearlyEqual(Depth[P].R,20.f,.3f);const float L=Lit[P].GetLuminance(),O=Opaque[P].GetLuminance(),M=Masked[P].GetLuminance(),Contrast=L-O;OpaqueContrast+=Contrast;if(Contrast<FMath::Max(.005f,L*.15f)||Expected[N].Approved!=1||(Expected[N].Coverage>0&&Expected[N].Coverage<1))continue;const bool Hole=Expected[N].Coverage<.5f;const float Shadow=FMath::Clamp((L-M)/Contrast,0.f,1.f);Agreement+=Hole?Shadow<.5f:Shadow>=.5f;++Compared;if(Hole&&Shadow<.5f)++Open;if(!Hole&&Shadow>=.5f)++Closed;}
        TestEqual(TEXT("camera measures receiver, never leaf occlusion"),DepthErrors,0);TestTrue(TEXT("opaque control casts measurable receiver shadow"),OpaqueContrast>1&&Compared>64);TestTrue(TEXT("actual shadow mask matches source rotation"),Compared>0&&double(Agreement)/Compared>.70);TestTrue(TEXT("receiver has both open and closed leaf shadow pixels"),Open>8&&Closed>8);AllOpen+=Open;AllClosed+=Closed;
        AddInfo(FString::Printf(TEXT("Shadow yaw%d: pixels%d agreement%d open%d closed%d depthErrors%d contrast%.3f"),Yaw,Compared,Agreement,Open,Closed,DepthErrors,OpaqueContrast));
        Pool->DestroyComponent();Receiver->DestroyComponent();Light->DestroyComponent();Capture->DestroyComponent();World->DestroyWorld(false);FlushRenderingCommands();if(!OK)return false;
    }
    TestTrue(TEXT("all-yaw real shadow openings measured"),AllOpen>32&&AllClosed>32);return true;
}
#endif

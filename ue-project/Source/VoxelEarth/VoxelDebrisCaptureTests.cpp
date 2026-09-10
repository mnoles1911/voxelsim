#include "VoxelDebrisCapture.h"
#include "VoxelDirectCoarseAppearance.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryWriter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "voxelcore/assetgrid.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void CapturePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FString CaptureSHA(const TArray<uint8>& B){uint8 H[32];check(SHA256(B.GetData(),B.Num(),H));return BytesToHex(H,32).ToLower();}
FColor CaptureRGB(int Bank,FIntVector P){return FColor(uint8(61+Bank*71+P.X*7),uint8(83+P.Y*11),uint8(47+P.Z*9));}
struct FCaptureBank: vxc::IAssetBankSource {
    TArray<uint8> Bytes[2];vxc::AssetGrid Grids[2];
    const vxc::AssetGrid* bankGrid(uint16_t ID,uint16_t)const override{return ID>=1&&ID<=2?&Grids[ID-1]:nullptr;}
};
struct FCaptureChannels:vxc::IAssetChannelSource {vxc::AssetColumnChannels channelsAt(int64_t,int64_t)override{vxc::AssetColumnChannels C;C.distanceToWaterMm=100;return C;}};
struct FCaptureFixture {
    FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/DebrisCapture")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    vxc::SyntheticTileSampler Tiles{1234};FCaptureBank Bank;FCaptureChannels Channels;vxc::AssetField Field;vxc::World<8> World{1234,Tiles};
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog;
    explicit FCaptureFixture(int FirstRadius=0){
        TArray<TSharedPtr<FJsonValue>> Rows;IFileManager::Get().MakeDirectory(*(Directory/TEXT("appearance")),true);
        for(int B=0;B<2;++B){const int SX=5+B,SY=4,SZ=10,Count=SX*SY*SZ;auto& V=Bank.Bytes[B];V.SetNumZeroed(53);
            CapturePut(V,0,vxc::kVxaMagic);CapturePut(V,4,3);CapturePut(V,8,uint32(-2));CapturePut(V,12,uint32(-3));CapturePut(V,16,0);CapturePut(V,20,SX);CapturePut(V,24,SY);CapturePut(V,28,SZ);CapturePut(V,32,100);CapturePut(V,36,1);V[48]=19;CapturePut(V,49,Count);
            check(Bank.Grids[B].parse(V.GetData(),V.Num())==vxc::AssetParseError::kOk);const FString Hash=FMD5::HashBytes(V.GetData(),V.Num()).ToLower();
            TArray<uint8> P;P.SetNumZeroed(128+Count*10);FMemory::Memcpy(P.GetData(),"VAC1",4);CapturePut(P,4,1);CapturePut(P,8,SX);CapturePut(P,12,SY);CapturePut(P,16,SZ);CapturePut(P,20,uint32(-2));CapturePut(P,24,uint32(-3));CapturePut(P,32,100);CapturePut(P,36,Count);HexToBytes(Hash,P.GetData()+48);
            for(int X=0;X<SX;++X)for(int Y=0;Y<SY;++Y)for(int Z=0;Z<SZ;++Z){auto R=P.GetData()+128+((X*SY+Y)*SZ+Z)*10;R[0]=uint8(X);R[2]=uint8(Y);R[4]=uint8(Z);R[6]=19;const auto C=CaptureRGB(B,FIntVector(X,Y,Z));R[7]=C.R;R[8]=C.G;R[9]=C.B;}
            TArray<uint8> Checked;Checked.Append(P.GetData(),96);Checked.Append(P.GetData()+128,P.Num()-128);check(SHA256(Checked.GetData(),Checked.Num(),P.GetData()+96));
            const FString Species=FString::Printf(TEXT("capture-tree-%d"),B);IFileManager::Get().MakeDirectory(*(Directory/TEXT("banks")/Species),true);
            check(FFileHelper::SaveArrayToFile(V,*(Directory/TEXT("banks")/Species/(Species+TEXT("-0001.vxa")))));check(FFileHelper::SaveArrayToFile(P,*(Directory/TEXT("appearance")/(Hash+TEXT(".vac")))));
            auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("id"),Species+TEXT("-0001"));Row->SetStringField(TEXT("species"),Species);Row->SetNumberField(TEXT("seed"),1);Row->SetStringField(TEXT("geometry_md5"),Hash);Row->SetStringField(TEXT("geometry_sha256"),CaptureSHA(V));Row->SetStringField(TEXT("sha256"),CaptureSHA(P));Row->SetStringField(TEXT("file"),Hash+TEXT(".vac"));Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        auto Root=MakeShared<FJsonObject>();Root->SetArrayField(TEXT("models"),Rows);FString Text;auto Writer=TJsonWriterFactory<>::Create(&Text);check(FJsonSerializer::Serialize(Root,Writer));check(FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("appearance/published.json"))));FString Error;Catalog=FVoxelPublishedAppearanceCatalog::Load(Directory,Error);
        vxc::AssetLayer Layers[2];vxc::AssetSpecies Species[2];
        for(int I=0;I<2;++I){auto& L=Layers[I];L.cellMm=400;L.maxHeightMm=1000;L.maxRadiusMm=I==0?FirstRadius:600;L.densityPerMille=1000;L.seedCount=1;
            auto& S=Species[I];S.bankId=uint16_t(I+1);S.layer=uint8_t(I);S.heightMm=1000;S.voxelSizeMm=100;S.elevMinMm=-1000000;S.elevMaxMm=9000000;S.slopeMaxMmPerM=100000;S.waterMaxMm=200;for(int B=0;B<vxc::kBiomeCount;++B)S.weightPerMille[B]=1000;}
        Field.setSeed(1234);Field.setLayers(Layers,2);Field.setSpecies(Species,2);Field.setBankSource(&Bank);World.setAssetField(&Field);World.setAssetChannelSource(&Channels);
    }
    ~FCaptureFixture(){IFileManager::Get().DeleteDirectory(*Directory,false,true);}
    auto Instances(vxc::AssetVoxelRect R){return Field.instancesForRect(R,[&](int64_t X,int64_t Y){const auto Column=World.amplifier().columnCached(X,Y);return vxc::assetColumnFactsFromSample(Column,World.assetChannelsAt(X,Y));});}
    struct FExpected{int Bank=-1,Yaw=0;FIntVector Zero=FIntVector::ZeroValue;};
    FExpected Expected(const VoxelCoords::FVoxelCoord& V){
        if(World.amplifier().materialAt(V.X,V.Y,V.Z)!=vxc::MAT_AIR)return {};
        // Independent authoritative POINT enumeration, not footprint capture.
        for(const auto& I:Instances({V.X,V.Y,V.X,V.Y})){if(Field.materialAt({I},V.X,V.Y,V.Z)==vxc::MAT_AIR)continue;
            const int B=I.bankId-1;const auto& G=Bank.Grids[B];
            // Forward-map source lattice coordinates; no assetCandidate inverse.
            for(int X=0;X<G.sizeX();++X)for(int Y=0;Y<G.sizeY();++Y){int64 SX=X+G.originX(),SY=Y+G.originY();for(int Q=0;Q<I.yawQuarter;++Q){const auto Old=SX;SX=-SY;SY=Old;}
                if(vxc::floorDiv(I.anchorXMm,int64_t(100))+SX==V.X&&vxc::floorDiv(I.anchorYMm,int64_t(100))+SY==V.Y){const int64 Z=V.Z-I.anchorVz-G.originZ();if(Z>=0&&Z<G.sizeZ())return {B,I.yawQuarter,FIntVector(X,Y,int32(Z))};}}
            return {};
        }
        return {};
    }
};
TArray<uint8> CaptureMeshBytes(AVoxelDebris* A){TArray<uint8>B;FMemoryWriter W(B);TArray<UProceduralMeshComponent*> Cs;A->GetComponents(Cs);for(auto C:Cs)if(auto S=C->GetProcMeshSection(0)){for(auto V:S->ProcVertexBuffer)W<<V.Position<<V.Color<<V.UV0;auto Indices=S->ProcIndexBuffer;W<<Indices;}return B;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDebrisCaptureTest,"Voxel.Appearance.WorldDebrisCapture",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDebrisCaptureTest::RunTest(const FString&){
    FCaptureFixture F;if(!TestTrue(TEXT("published exact source catalog loaded"),F.Catalog.IsValid()))return false;
    FVoxelAppearanceBankBinding All(F.Catalog),SecondOnly(F.Catalog);for(int I=0;I<2;++I)All.Observe(F.Bank.Grids[I],F.Bank.Bytes[I].GetData(),F.Bank.Bytes[I].Num());SecondOnly.Observe(F.Bank.Grids[1],F.Bank.Bytes[1].GetData(),F.Bank.Bytes[1].Num());
    TArray<VoxelCoords::FVoxelCoord> Cells;TArray<FCaptureFixture::FExpected> Expected;int Terrain=0,ReachMismatch=0,YawBits=0;
    for(int64 Y=-12;Y<=4;++Y)for(int64 X=-12;X<=4;++X){const auto Top=vxc::topSolidVoxelZ(F.World.amplifier().columnCached(X,Y).surfaceMm);
        for(int DZ:{-1,3,7}){VoxelCoords::FVoxelCoord V{X,Y,Top+DZ};if(F.World.materialAt(V.X,V.Y,V.Z)==vxc::MAT_AIR)continue;Cells.Add(V);const auto E=F.Expected(V);Expected.Add(E);if(E.Bank<0)++Terrain;else YawBits|=1<<E.Yaw;
            if(E.Bank==1){const auto PX=vxc::floorDiv(X,int64_t(32)),PY=vxc::floorDiv(Y,int64_t(32));for(const auto& I:F.Instances({PX*32,PY*32,PX*32+31,PY*32+31})){if(F.Field.materialAt({I},V.X,V.Y,V.Z)==vxc::MAT_AIR)continue;ReachMismatch+=I.bankId==1;break;}}
        }
    }
    TestTrue(TEXT("terrain precedence exercised"),Terrain>0);TestEqual(TEXT("all canonical yaws exercised"),YawBits,15);TestTrue(TEXT("broad footprint differs from point winner"),ReachMismatch>0);
    TArray<FVoxelDebrisCellAppearance> Captured,Partial;
    if(!TestTrue(TEXT("actual world capture"),VoxelDebrisCapture::Capture(F.World,&All,Cells,Captured))||!TestTrue(TEXT("partly approved world capture"),VoxelDebrisCapture::Capture(F.World,&SecondOnly,Cells,Partial)))return false;
    int Bad=0,UnapprovedFirst=0,ApprovedSecond=0;
    for(int I=0;I<Cells.Num();++I){const auto& V=Cells[I];const auto& E=Expected[I];const auto& C=Captured[I];
        Bad+=!(C.Coord==V)||C.Material!=uint8(F.World.materialAt(V.X,V.Y,V.Z))||C.Approved!=(E.Bank>=0);
        if(E.Bank>=0)Bad+=C.SourceCell!=E.Zero||C.SourceYawQuarter!=E.Yaw||C.BaseRGB!=CaptureRGB(E.Bank,E.Zero)||!C.FoliageMask||C.Needle;
        Bad+=Partial[I].Approved!=(E.Bank==1);UnapprovedFirst+=E.Bank==0;ApprovedSecond+=E.Bank==1;
    }
    TestEqual(TEXT("point-winner provenance and material exact"),Bad,0);TestTrue(TEXT("unapproved earlier winner blocks later approval"),UnapprovedFirst>0);TestTrue(TEXT("approved later nonoccluded source remains"),ApprovedSecond>0);
    // Pick adjacent generated source cells in the SAME edited brick.
    TArray<VoxelCoords::FVoxelCoord> Pair;VoxelCoords::FVoxelCoord Untouched{};
    for(const auto& V:Cells){auto N=V;N.Z++;auto Third=N;Third.Z++;if(vxc::floorDiv(V.Z,int64_t(8))!=vxc::floorDiv(Third.Z,int64_t(8)))continue;if(F.Expected(V).Bank>=0&&F.Expected(N).Bank>=0&&F.Expected(Third).Bank>=0&&F.World.materialAt(N.X,N.Y,N.Z)==vxc::MAT_LEAF_BROADLEAF){Pair={V,N};Untouched=Third;break;}}
    if(!TestEqual(TEXT("same-brick source pair found"),Pair.Num(),2))return false;
    TArray<FVoxelDebrisCellAppearance> Before,After;TestTrue(TEXT("capture untouched pair"),VoxelDebrisCapture::Capture(F.World,&All,Pair,Before));
    const auto A=Pair[0],B=Pair[1];F.World.setVoxel(A.X,A.Y,A.Z,vxc::MAT_ROCK);
    TestTrue(TEXT("capture replaced cell and untouched neighbor"),VoxelDebrisCapture::Capture(F.World,&All,Pair,After));
    TestTrue(TEXT("actual replacement material without source approval"),!After[0].Approved&&After[0].Material==uint8(vxc::MAT_ROCK));
    TestTrue(TEXT("untouched same-brick neighbor preserves source"),After[1].Approved&&After[1].BaseRGB==Before[1].BaseRGB&&After[1].SourceCell==Before[1].SourceCell);
    F.World.setVoxel(A.X,A.Y,A.Z,vxc::MAT_LEAF_BROADLEAF);TestTrue(TEXT("capture same-material replacement"),VoxelDebrisCapture::Capture(F.World,&All,Pair,After));TestFalse(TEXT("same material cannot recover removed approval"),After[0].Approved);
    F.World.setCraftCell(F.World.craftCellOfVoxelMin(B.X)+1,F.World.craftCellOfVoxelMin(B.Y)+1,F.World.craftCellOfVoxelMin(B.Z)+1,vxc::MAT_ROCK);
    TestTrue(TEXT("capture actual12.5mm craft touch"),VoxelDebrisCapture::Capture(F.World,&All,{B,Untouched},After));
    TestFalse(TEXT("exact craft-touched cell drops source"),After[0].Approved);TestTrue(TEXT("untouched neighbor in craft-promoted brick retains approval"),After[1].Approved);
    // The handoff consumes the SAVED pre-removal payload, after the actual
    // world edit-log removal. It cannot reconstruct colors from the air world.
    for(const auto& V:Pair)F.World.setVoxel(V.X,V.Y,V.Z,vxc::MAT_AIR);
    TestFalse(TEXT("post-removal capture refused"),VoxelDebrisCapture::Capture(F.World,&All,Pair,After));TestTrue(TEXT("failed capture returns no partial records"),After.IsEmpty());
    auto World=UWorld::CreateWorld(EWorldType::Game,false,FName(*FGuid::NewGuid().ToString()));if(!TestNotNull(TEXT("handoff world"),World))return false;
    auto Debris=World->SpawnActor<AVoxelDebris>();bool OK=Debris&&TestEqual(TEXT("captured approved cells handed off after removal"),Debris->InitFromIslandWithAppearance(Pair,Before),2);
    if(OK){CastChecked<UStaticMeshComponent>(Debris->GetRootComponent())->SetSimulatePhysics(false);Debris->SetActorTickEnabled(false);TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe> G;TArray<uint8> D;OK=Debris->CaptureObjectState(G,D);if(OK){auto Restored=World->SpawnActor<AVoxelDebris>();OK=Restored&&Restored->RestoreObjectState(G,D);if(OK)TestTrue(TEXT("actual pre-removal appearance survives object rebuild"),CaptureMeshBytes(Debris)==CaptureMeshBytes(Restored));}}
    TestTrue(TEXT("world removal / captured debris / rebuild completed"),OK);World->DestroyWorld(false);return OK;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDirectCoarseAppearanceTest,"Voxel.Appearance.DirectCoarseFallback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelDirectCoarseAppearanceTest::RunTest(const FString&){
    FCaptureFixture F(600);if(!TestTrue(TEXT("direct fallback published bank"),F.Catalog.IsValid()))return false;
    FVoxelAppearanceBankBinding Binding(F.Catalog);for(int I=0;I<2;++I)Binding.Observe(F.Bank.Grids[I],F.Bank.Bytes[I].GetData(),F.Bank.Bytes[I].Num());
    const auto& Generated=F.World.generated();const int Level=1,Scale=2;const auto Top=vxc::topSolidVoxelZ(Generated.amplifier().columnCached(-16,-16).surfaceMm);
    const FIntVector Key(-1,-1,int32(vxc::floorDiv(Top+4,int64(64))));
    const auto Instances=F.Instances({-64,-64,-1,-1});const auto Ordered=F.Field.resolveForCompose(Instances);
    for(bool SurfacePreserve:{false,true}){
        std::unordered_map<vxc::BrickKey,vxc::Brick<8>,vxc::BrickKeyHash> Bricks;
        // Actual direct fallback authority: the identical makeCoarseBrick
        // called by MakeCoarseLevelSampler, including its surface-preserve mode.
        auto Terrain=[&](int64 X,int64 Y,int64 Z){const vxc::BrickKey BK{int32(vxc::floorDiv(X,int64(8))),int32(vxc::floorDiv(Y,int64(8))),int32(vxc::floorDiv(Z,int64(8)))};
            auto It=Bricks.find(BK);if(It==Bricks.end())It=Bricks.emplace(BK,Generated.makeCoarseBrick(Level,BK,SurfacePreserve)).first;
            return It->second.get(int(vxc::floorMod(X,int64(8))),int(vxc::floorMod(Y,int64(8))),int(vxc::floorMod(Z,int64(8))));};
        const auto Actual=VoxelDirectCoarseAppearance::MakeSampler(Terrain,Ordered,Level);
        FString TerrainError;
        TestFalse(TEXT("terrain-only geometry cannot admit an asset page"),VoxelDirectCoarseAppearance::Prepare(Generated,Key,Level,19,Ordered,Binding,SurfacePreserve,Terrain,TerrainError).IsValid());
        int TerrainWins=0,AssetCells=0,ParityErrors=0;
        for(int Z=0;Z<32;++Z)for(int Y=0;Y<32;++Y)for(int X=0;X<32;++X){
            const int64 CX=int64(Key.X)*32+X,CY=int64(Key.Y)*32+Y,CZ=int64(Key.Z)*32+Z;
            auto Expected=Terrain(CX,CY,CZ);if(Expected!=vxc::MAT_AIR)++TerrainWins;
            else {Expected=F.Field.materialAt(Instances,CX*Scale+Scale/2,CY*Scale+Scale/2,CZ*Scale+Scale/2);AssetCells+=Expected!=vxc::MAT_AIR;}
            ParityErrors+=Actual(CX,CY,CZ)!=Expected;
        }
        TestTrue(TEXT("direct geometry includes terrain and actual assets"),TerrainWins>0&&AssetCells>0);
        TestEqual(TEXT("independent original-instance composition parity"),ParityErrors,0);
        TArray<uint8> Before;Before.Reserve(32768);for(int Z=0;Z<32;++Z)for(int Y=0;Y<32;++Y)for(int X=0;X<32;++X)Before.Add(uint8(Actual(int64(Key.X)*32+X,int64(Key.Y)*32+Y,int64(Key.Z)*32+Z)));
        FString Error;auto Page=VoxelDirectCoarseAppearance::Prepare(Generated,Key,Level,19,Ordered,Binding,SurfacePreserve,Actual,Error);
        if(!TestTrue(TEXT("direct fallback appearance prepared"),Page.IsValid())){AddError(Error);return false;}
        TestTrue(TEXT("real source appearance emitted"),Page->Words().Num()>80&&Page->Words()[7]>0);TestEqual(TEXT("fallback page generation"),Page->PageGeneration(),uint64(19));
        auto Reference=FVoxelTerrainAppearancePage::Prepare(Key,Level,19,Ordered,Binding,[&](int64 X,int64 Y,int64 Z){const auto Column=Generated.amplifier().columnCached(X,Y);return vxc::Amplifier::coarseSurfaceMaterialAt(Column,Z,Scale,SurfacePreserve);},[](int64,int64,int64){return false;},Error);
        if(!TestTrue(TEXT("representative source reference prepared"),Reference.IsValid()))return false;
        TestTrue(TEXT("direct fallback preserves canonical representative bytes"),Page->Words()==Reference->Words());
        int Mismatch=0,Index=0;for(int Z=0;Z<32;++Z)for(int Y=0;Y<32;++Y)for(int X=0;X<32;++X)Mismatch+=Before[Index++]!=uint8(Actual(int64(Key.X)*32+X,int64(Key.Y)*32+Y,int64(Key.Z)*32+Z));TestEqual(TEXT("appearance preparation never changes geometry"),Mismatch,0);
        auto Wrong=[&](int64 X,int64 Y,int64 Z){const auto M=Actual(X,Y,Z);return M==vxc::MAT_LEAF_BROADLEAF?vxc::MAT_ROCK:M;};
        TestFalse(TEXT("different reduction material cannot receive representative source"),VoxelDirectCoarseAppearance::Prepare(Generated,Key,Level,19,Ordered,Binding,SurfacePreserve,Wrong,Error).IsValid());
        TestTrue(TEXT("semantic mismatch has explicit diagnostic"),Error.Contains(TEXT("material mismatch")));
        TestFalse(TEXT("level0 is not a direct coarse fallback"),VoxelDirectCoarseAppearance::Prepare(Generated,Key,0,19,Ordered,Binding,SurfacePreserve,Actual,Error).IsValid());
    }
    return true;
}
#endif

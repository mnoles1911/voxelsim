#include "VoxelBrickAppearanceProbe.h"
#include "VoxelTerrainSurfaceProbe.h"
#include "VoxelAssetAppearance.h"
#include "VoxelBrickCpuPackFromCore.h"
#include "voxelcore/assetappearancepack.h"
#include "Misc/SecureHash.h"
#include "RenderingThread.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void BrickAppearancePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FIntVector BrickAppearanceRotate(FIntVector P,int Yaw){for(int I=0;I<Yaw;++I)P=FIntVector(-P.Y,P.X,P.Z);return P;}
struct FBrickAppearanceSource {
    vxc::AssetGrid Grid;TSharedPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> Sources;
    bool Init(uint8 Material,FString& Error){
        TArray<uint8> V;V.SetNumZeroed(53);BrickAppearancePut(V,0,vxc::kVxaMagic);BrickAppearancePut(V,4,vxc::kVxaVersion);for(int O:{20,24,28})BrickAppearancePut(V,O,8);BrickAppearancePut(V,32,100);BrickAppearancePut(V,36,1);V[48]=Material;BrickAppearancePut(V,49,512);
        if(Grid.parse(V.GetData(),V.Num())!=vxc::AssetParseError::kOk)return false;const FString Hash=FMD5::HashBytes(V.GetData(),V.Num()).ToLower();
        TArray<uint8> B;B.SetNumZeroed(128+512*10);FMemory::Memcpy(B.GetData(),"VAC1",4);BrickAppearancePut(B,4,Material==24?2:1);for(int O:{8,12,16})BrickAppearancePut(B,O,8);BrickAppearancePut(B,32,Material==24?100000:100);BrickAppearancePut(B,36,512);HexToBytes(Hash,B.GetData()+48);
        for(int X=0;X<8;++X)for(int Y=0;Y<8;++Y)for(int Z=0;Z<8;++Z){const int N=128+((X*8+Y)*8+Z)*10;B[N]=X;B[N+2]=Y;B[N+4]=Z;B[N+6]=Material;B[N+7]=71;B[N+8]=151;B[N+9]=43;}
        TArray<uint8> Signed;Signed.Append(B.GetData(),96);Signed.Append(B.GetData()+128,B.Num()-128);if(!SHA256(Signed.GetData(),Signed.Num(),B.GetData()+96))return false;
        auto Packet=FVoxelAssetAppearance::Parse(MoveTemp(B),Hash,Error);if(!Packet)return false;auto Sparse=Packet->BuildSparseResource(Error);if(!Sparse)return false;
        auto S=MakeShared<FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe>();S->SourceWords=Sparse->Words;S->SourceRanges={FUintVector2(0,0),FUintVector2(0,S->SourceWords.Num())};Sources=S;return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBrickAppearanceTraversalTest,"Voxel.Appearance.BrickTraversalGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelBrickAppearanceTraversalTest::RunTest(const FString&){
    FString Error;FBrickAppearanceSource Leaf,Petal;if(!TestTrue(TEXT("verified leaf VAC source"),Leaf.Init(19,Error))||!TestTrue(TEXT("verified opaque generic VAC source"),Petal.Init(24,Error)))return false;
    int Open=0,Closed=0;
    for(int L=0;L<=7;++L)for(int Yaw=0;Yaw<4;++Yaw)for(int Mode=0;Mode<(L?2:4);++Mode){
        // Modes: mixed leaf slab, uniform leaf brick, opaque generic petal,
        // unapproved leaf slab. Backstop lies in a DIFFERENT brick below.
        const bool Uniform=Mode==1,Approved=Mode!=3;const uint8 Material=Mode==2?24:19;auto& Source=Mode==2?Petal:Leaf;
        const int Scale=1<<L,Half=L?Scale/2:0;vxc::AssetField::ResolvedAssetInstance I;I.grid=&Source.Grid;I.yawQuarter=Yaw;
        const FIntVector Anchor(-16+(!L&&(Yaw==1||Yaw==2)?7:0),-16+(!L&&(Yaw==2||Yaw==3)?7:0),L?-9:-16);
        I.anchorVx=Anchor.X*Scale;I.anchorVy=Anchor.Y*Scale;I.anchorVz=Anchor.Z*Scale;
        std::vector<vxc::AssetAppearanceCell> Cells;TArray<FIntVector> Occupied;
        for(int X=0;X<(L?1:8);++X)for(int Y=0;Y<(L?1:8);++Y)for(int Z=(Uniform?0:7);Z<8;++Z){
            if(L&&Z!=7)continue;const FIntVector S(X,Y,L?0:Z),W=L?FIntVector(-16,-16,-9):Anchor+BrickAppearanceRotate(S,Yaw);Occupied.Add(W);
            vxc::AssetAppearanceCell C;C.cell=uint16((W.X+32)+32*(W.Y+32)+1024*(W.Z+32));C.instance=0;C.resource=1;C.sourceX=S.X;C.sourceY=S.Y;C.sourceZ=S.Z;C.yaw=Yaw;C.material=vxc::MaterialId(Material);C.offsetX=C.offsetY=C.offsetZ=int8(-Half);Cells.push_back(C);
        }
        // Coarse uniform bricks contain one approved source cell and opaque
        // fallback neighbours. This exercises the restored uniform entry path
        // against the same independent surface-coverage oracle as mixed bricks.
        if(L&&Uniform)for(int X=-16;X<=-9;++X)for(int Y=-16;Y<=-9;++Y)for(int Z=-16;Z<=-9;++Z)Occupied.AddUnique(FIntVector(X,Y,Z));
        std::vector<uint32_t> Words;if(!TestTrue(TEXT("actual canonical source/page packing"),vxc::assetPackAppearancePage(-1,-1,-1,91,{I},Cells,Words,uint8(L))))return false;
        auto Upload=MakeShared<FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>();Upload->PageKey=FIntVector(-1);Upload->Level=L;Upload->Generation=91;Upload->Sources=Source.Sources;Upload->PageWords.Append(Words.data(),Words.size());
        if(L)TestEqual(TEXT("coarse selected corner uses v3 offsets"),Upload->PageWords[0],3u);
        auto Mat=[&](int X,int Y,int Z)->vxc::MaterialId{const FIntVector W(X-32,Y-32,Z-32);if(Occupied.Contains(W))return vxc::MaterialId(Material);return Z==7?vxc::MAT_BARK:vxc::MAT_AIR;};
        auto Pack=VoxelBrickCpuPackFromCore(vxc::packChunkBricksCanonical(Mat),-32,-32,-32);
        TestEqual(TEXT("fixture truly exercises uniform versus mixed descriptor"),(Pack->Desc[42*2]>>28)&3u,Uniform?1u:2u);
        FVoxelBrickPool Pool;FVoxelBrickPoolConfig Config;Config.ChunkCapacity=2;Config.OccWordCapacity=4096;Config.MatWordCapacity=8192;Pool.Init(Config);const FVoxelBrickChunkKey Key{-1,-1,-1,L};
        if(!TestTrue(TEXT("actual CPU brick pool admission"),Pool.AddChunkFromCpu(Pack,Key,FVoxelBrickChunkShading::Neutral(),Approved?TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe>(Upload):nullptr)!=INDEX_NONE))return false;
        // Sample interior rays away from exact XY boundaries, independently
        // enumerate the known source cells front-to-back for the oracle.
        TArray<FVoxelBrickAppearanceRay> Rays;TArray<FVoxelTerrainSurfaceProbeQuery> MaskQueries;
        for(int N=0;N<128;++N){const FIntVector Top=L?FIntVector(-16,-16,-9):Occupied.Last();const float FX=.05f+.9f*float(N%16)/15,FY=.05f+.9f*float(N/16)/7;
            FVoxelBrickAppearanceRay R;R.Origin=FVector3f((Top.X+FX)*10,(Top.Y+FY)*10,-75);R.Reach=200;R.Level=L;Rays.Add(R);
            for(int Z=-9;Z>=-16;--Z){FVoxelTerrainSurfaceProbeQuery Q;Q.PageCount=Upload->PageWords.Num();Q.RangeCount=2;Q.X=Top.X;Q.Y=Top.Y;Q.Z=Z;Q.FractionX=FX;Q.FractionY=FY;Q.Steps=1;Q.MaterialsLow=Material;Q.PitchMm=float(100*Scale);MaskQueries.Add(Q);}
        }
        TArray<FVoxelTerrainSurfaceProbeResult> Masks;if(!VoxelRunTerrainSurfaceProbe(Upload->PageWords,Source.Sources->SourceWords,Source.Sources->SourceRanges,MaskQueries,Masks,Error)){AddError(Error);return false;}
        if(L>=3)for(int N=0;N<Rays.Num();++N){
            TestEqual(TEXT("coarse approved face remains resolved for shading"),Masks[N*8].FirstApproved,1u);
            TestEqual(TEXT("coarse mask exactly opaque before traversal shortcut"),Masks[N*8].FirstCoverage,1.f);
        }
        TArray<FVoxelBrickAppearanceHit> Flat,Hier;if(!VoxelRunBrickAppearanceProbe(Pool,{Key},Rays,Flat,Hier,Error)){AddError(Error);return false;}
        int Bad=0;for(int N=0;N<Rays.Num();++N){const int X=FMath::FloorToInt(Rays[N].Origin.X/10),Y=FMath::FloorToInt(Rays[N].Origin.Y/10);int ZHit=-25;uint32 MHit=16;
            for(int Z=-9;Z>=-16;--Z)if(Occupied.Contains(FIntVector(X,Y,Z))&&(!Approved||Masks[N*8+(-9-Z)].FirstCoverage>=.5f)){ZHit=Z;MHit=Material;break;}
            if(ZHit==-25)++Open;else ++Closed;const float Distance=Rays[N].Origin.Z-float(ZHit+1)*10;
            for(const auto& H:{Flat[N],Hier[N]})Bad+=!(H.Hit==1&&H.Material==MHit&&H.X==X&&H.Y==Y&&H.Z==ZHit&&H.Axis==2&&H.Sign==-1&&FMath::IsNearlyEqual(H.Distance,Distance,.025f));
        }
        TestEqual(FString::Printf(TEXT("actual flat/hier exact hit L%d yaw%d mode%d"),L,Yaw,Mode),Bad,0);
        Pool.RemoveChunk(Key);Pool.Flush();FlushRenderingCommands();if(Bad)return false;
    }
    TestTrue(TEXT("both actual leaf coverage and holes reached"),Open>8&&Closed>8);return true;
}
#endif

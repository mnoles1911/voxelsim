#include "VoxelQuadSurfaceProbe.h"
#include "VoxelTerrainSurfaceProbe.h"
#include "VoxelAssetAppearance.h"
#include "Materials/MaterialInterface.h"
#include "voxelcore/assetappearancepack.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void QuadProbePut(TArray<uint8>& B,int O,uint32 V){for(int J=0;J<4;++J)B[O+J]=uint8(V>>(8*J));}
FVector3f QuadProbeRotate(FVector3f P,int Yaw){for(int J=0;J<Yaw;++J)P=FVector3f(-P.Y,P.X,P.Z);return P;}
FColor QuadProbeColor(int X){return FColor(uint8(80+X*90),uint8(130-X*50),uint8(30+X*80));}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelQuadSurfaceAppearanceTest,"Voxel.Appearance.QuadSurfaceGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelQuadSurfaceAppearanceTest::RunTest(const FString&){
    auto MaterialAsset=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain"));
    if(!TestNotNull(TEXT("actual terrain material exists"),MaterialAsset))return false;
    if(!TestEqual(TEXT("actual terrain asset uses masked blend"),MaterialAsset->GetBlendMode(),BLEND_Masked))return false;
    FString Error;int Holes=0,Faded=0;uint32 Levels=0,Yaws=0;bool GreedyDifference=false;
    const FIntVector Origin(-1,-2,-3),Cell(-17,-15,-13);
    for(int Policy=0;Policy<4;++Policy){const uint8 Material=Policy==0?16:Policy==1?24:Policy==2?19:20;
        std::vector<uint8_t> Vxa;auto Put=[&](uint32 V){for(int J=0;J<4;++J)Vxa.push_back(uint8(V>>(8*J)));};
        Put(vxc::kVxaMagic);Put(3);Put(uint32(Origin.X));Put(uint32(Origin.Y));Put(uint32(Origin.Z));Put(2);Put(1);Put(1);Put(100);Put(1);Put(0);Put(0);Vxa.push_back(Material);Put(2);
        vxc::AssetGrid Grid;if(!TestTrue(TEXT("two-cell original VXA"),Grid.parse(Vxa)==vxc::AssetParseError::kOk))return false;
        const FString Hash=FMD5::HashBytes(Vxa.data(),int32(Vxa.size()));TArray<uint8> Packet;Packet.SetNumZeroed(148);FMemory::Memcpy(Packet.GetData(),"VAC1",4);QuadProbePut(Packet,4,Policy==1?2:1);QuadProbePut(Packet,8,2);QuadProbePut(Packet,12,1);QuadProbePut(Packet,16,1);QuadProbePut(Packet,20,uint32(Origin.X));QuadProbePut(Packet,24,uint32(Origin.Y));QuadProbePut(Packet,28,uint32(Origin.Z));QuadProbePut(Packet,32,Policy==1?100000:100);QuadProbePut(Packet,36,2);QuadProbePut(Packet,40,Policy==3?1:0);HexToBytes(Hash,Packet.GetData()+48);
        for(int X=0;X<2;++X){const int O=128+X*10;const auto C=QuadProbeColor(X);Packet[O]=uint8(X);Packet[O+6]=Material;Packet[O+7]=C.R;Packet[O+8]=C.G;Packet[O+9]=C.B;}
        TArray<uint8> Checked;Checked.Append(Packet.GetData(),96);Checked.Append(Packet.GetData()+128,20);check(SHA256(Checked.GetData(),Checked.Num(),Packet.GetData()+96));
        auto Source=FVoxelAssetAppearance::Parse(Packet,Hash,Error);if(!TestTrue(TEXT("verified source packet"),Source.IsValid()))return false;auto Sparse=Source->BuildSparseResource(Error);if(!TestTrue(TEXT("sparse source"),Sparse.IsValid()))return false;
        TArray<uint32> Sources;Sources.SetNumZeroed(9);Sources.Append(Sparse->Words);TArray<FUintVector2> Ranges;Ranges.SetNumZeroed(9);Ranges[8]=FUintVector2(9,uint32(Sparse->Words.Num()));
        for(uint8 Level=0;Level<=7;++Level)for(uint8 Yaw=0;Yaw<4;++Yaw)for(int Recursive=0;Recursive<(Level?2:1);++Recursive){Levels|=1u<<Level;Yaws|=1u<<Yaw;const int Scale=1<<Level,Half=Level?Scale/2:0;
            const auto Rotated=QuadProbeRotate(FVector3f(Origin),Yaw);vxc::AssetField::ResolvedAssetInstance I;I.grid=&Grid;I.yawQuarter=Yaw;I.anchorVx=int64(Cell.X)*Scale+Half-int64(Rotated.X);I.anchorVy=int64(Cell.Y)*Scale+Half-int64(Rotated.Y);I.anchorVz=int64(Cell.Z)*Scale+Half-int64(Rotated.Z);
            std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered{I};std::vector<vxc::AssetAppearanceCell> Cells;std::vector<uint32_t> Packed;
            if(!TestTrue(TEXT("canonical page packed"),vxc::assetAppearancePage(-1,-1,-1,Ordered,{5},[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Cells,Level)&&vxc::assetPackAppearancePage(-1,-1,-1,97,Ordered,Cells,Packed,Level)))return false;
            TestEqual(TEXT("coarse representative count"),int32(Cells.size()),Level?1:2);
            const FIntVector Offset=Recursive?FIntVector(-Half,Half-1,-Half):FIntVector::ZeroValue;
            if(Recursive){
                Ordered[0].anchorVx+=Offset.X;Ordered[0].anchorVy+=Offset.Y;Ordered[0].anchorVz+=Offset.Z;
                for(auto& C:Cells){C.offsetX=int8_t(Offset.X);C.offsetY=int8_t(Offset.Y);C.offsetZ=int8_t(Offset.Z);}
                if(!TestTrue(TEXT("selected-child page packed"),vxc::assetPackAppearancePage(-1,-1,-1,97,Ordered,Cells,Packed,Level)))return false;
                TestEqual(TEXT("selected-child encoding active"),Packed[0],3u);
            }
            TArray<uint32> Pages;Pages.SetNumZeroed(7);Pages.Append(Packed.data(),int32(Packed.size()));TArray<FUintVector4> Slots;Slots.SetNumZeroed(5);Slots[4]=FUintVector4(7,uint32(Packed.size()),3,6);
            TArray<FVoxelQuadSurfaceProbeQuery> Queries;TArray<FVoxelTerrainSurfaceProbeQuery> ReferenceQueries;TArray<FLinearColor> Colors;TArray<FVector2f> ExpectedUV;
            for(int SourceX=0;SourceX<(Level?1:2);++SourceX){const auto Shift=QuadProbeRotate(FVector3f(float(SourceX),0,0),Yaw);const FIntVector C=Cell+FIntVector(int32(Shift.X),int32(Shift.Y),int32(Shift.Z));
                for(int Axis=0;Axis<3;++Axis)for(int Sign=0;Sign<2;++Sign)for(int Sample=0;Sample<24;++Sample)for(float Derivative:{0.f,3.f}){
                    FVector3f Fraction((Sample+.17f)/24.f,(Sample*.37f+.23f)/10.f,(Sample*.61f+.41f)/16.f);Fraction[Axis]=.5f;
                    FVoxelQuadSurfaceProbeQuery Q;Q.X=float(C.X+32)+Fraction.X;Q.Y=float(C.Y+32)+Fraction.Y;Q.Z=float(C.Z+32)+Fraction.Z;Q.Identity=float(4*8+Axis*2+Sign);Q.Material=Material;Q.DxX=Derivative;Queries.Add(Q);
                    FVoxelTerrainSurfaceProbeQuery R;R.PageBase=7;R.PageCount=uint32(Packed.size());R.RangeBase=3;R.RangeCount=6;R.X=C.X;R.Y=C.Y;R.Z=C.Z;R.Axis=Axis;R.Positive=Sign;R.Steps=1;R.MaterialsLow=Material;
                    // Account for the exact float interpolant's fractional part,
                    // independently construct world cell and normal from source.
                    R.FractionX=Q.X-FMath::FloorToFloat(Q.X);R.FractionY=Q.Y-FMath::FloorToFloat(Q.Y);R.FractionZ=Q.Z-FMath::FloorToFloat(Q.Z);if(Axis==0)R.FractionX=Sign;if(Axis==1)R.FractionY=Sign;if(Axis==2)R.FractionZ=Sign;
                    R.PitchMm=float(100*Scale);R.PatternFootprint=Derivative*(.1f*float(Scale)/.13f);ReferenceQueries.Add(R);
                    FVector3f Physical(R.FractionX,R.FractionY,R.FractionZ);
                    Physical=Physical*float(Scale)-FVector3f(float(Half))-FVector3f(Offset);
                    if(Yaw==1)Physical=FVector3f(Physical.Y,1-Physical.X,Physical.Z);
                    else if(Yaw==2)Physical=FVector3f(1-Physical.X,1-Physical.Y,Physical.Z);
                    else if(Yaw==3)Physical=FVector3f(1-Physical.Y,Physical.X,Physical.Z);
                    Physical=(Physical+FVector3f(float(SourceX),0,0))*.1f;
                    const int SourceUVAxis=Axis<2&&(Yaw&1)?1-Axis:Axis;
                    ExpectedUV.Add(SourceUVAxis==2?FVector2f(Physical.X,Physical.Y):SourceUVAxis==0?FVector2f(Physical.Y,Physical.Z):FVector2f(Physical.X,Physical.Z));
                    FVector3f N=FVector3f::ZeroVector;N[Axis]=Sign?1.f:-1.f;N=QuadProbeRotate(N,(4-Yaw)&3);int SourceAxis=0;while(N[SourceAxis]==0)++SourceAxis;Colors.Add(FVoxelAssetAppearance::FaceColor(QuadProbeColor(SourceX),FIntVector(SourceX,0,0),SourceAxis,N[SourceAxis]>0));
                }
            }
            TArray<FVoxelQuadSurfaceProbeResult> Results;TArray<FVoxelTerrainSurfaceProbeResult> Reference;
            if(!VoxelRunQuadSurfaceProbe(Pages,Sources,Ranges,Slots,Queries,Results,Error)||!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,ReferenceQueries,Reference,Error)){AddError(Error);return false;}
            int Bad=0;for(int N=0;N<Results.Num();++N){const auto& R=Results[N];Bad+=R.Approved!=1||R.DecodedIdentity!=uint32(Queries[N].Identity)||!FLinearColor(R.R,R.G,R.B).Equals(Colors[N],2.e-5f)||!FMath::IsNearlyEqual(R.Coverage,Reference[N].FirstCoverage,2.e-5f)||Reference[N].FirstApproved!=1;if(Policy<2)Bad+=R.Coverage!=1;Holes+=R.Coverage==0;Faded+=R.Coverage>0&&R.Coverage<1;}
            for(int N=0;N<Reference.Num();++N)if(Reference[N].Approved){
                Bad+=!FMath::IsNearlyEqual(Reference[N].U,ExpectedUV[N].X,2.e-5f)||!FMath::IsNearlyEqual(Reference[N].V,ExpectedUV[N].Y,2.e-5f);
            }
            if(!TestEqual(TEXT("quad source color identity and derivative coverage"),Bad,0))return false;
            if(Level==0)GreedyDifference|=!FLinearColor(Results[0].R,Results[0].G,Results[0].B).Equals(FLinearColor(Results[Results.Num()/2].R,Results[Results.Num()/2].G,Results[Results.Num()/2].B));
            TArray<FVoxelQuadSurfaceProbeQuery> Invalid;auto Q=Queries[0];Q.Identity=0;Invalid.Add(Q);Q=Queries[0];Q.Identity=6*8;Invalid.Add(Q);Q=Queries[0];Q.Identity=4*8+6;Invalid.Add(Q);Q=Queries[0];Q.X=-.1f;Invalid.Add(Q);Q.X=32;Invalid.Add(Q);Q=Queries[0];Q.Material=0;Invalid.Add(Q);
            auto Refuse=[&](const TArray<FVoxelQuadSurfaceProbeQuery>& Tests){if(!VoxelRunQuadSurfaceProbe(Pages,Sources,Ranges,Slots,Tests,Results,Error)){AddError(Error);return false;}for(const auto& R:Results)if(R.Approved||R.Coverage!=1){AddError(TEXT("invalid quad lookup must preserve opaque fallback"));return false;}return true;};
            if(!Refuse(Invalid))return false;
            const auto Good=Slots[4];Slots[4].X=uint32(Pages.Num()+1);if(!Refuse({Queries[0]}))return false;Slots[4]=Good;
            auto SavedPitch=Sources[9+13];Sources[9+13]=Policy==1?25000:25;if(!Refuse({Queries[0]}))return false;Sources[9+13]=SavedPitch;
            Pages[7+5]=8;if(!Refuse({Queries[0]}))return false;Pages[7+5]=Level;
            if(Policy==0&&Level==0&&Yaw==0){
                Slots.SetNumZeroed(1048576);Slots[1048575]=Good;TArray<FVoxelQuadSurfaceProbeQuery> Maximum;
                for(uint32 Axis=0;Axis<3;++Axis)for(uint32 Sign=0;Sign<2;++Sign){Q=Queries[0];Q.Identity=float(1048575u*8u+Axis*2u+Sign);Maximum.Add(Q);}
                if(!VoxelRunQuadSurfaceProbe(Pages,Sources,Ranges,Slots,Maximum,Results,Error)){AddError(Error);return false;}
                for(int N=0;N<Results.Num();++N){TestEqual(TEXT("maximum slot float identity exact"),Results[N].DecodedIdentity,uint32(Maximum[N].Identity));TestEqual(TEXT("maximum slot lookup accepted"),Results[N].Approved,1u);}
            }
        }
    }
    TestEqual(TEXT("all eight page levels"),Levels,255u);TestEqual(TEXT("all four source yaws"),Yaws,15u);TestTrue(TEXT("greedy face changes color at source cell boundary"),GreedyDifference);TestTrue(TEXT("actual quad coverage exercises holes and partial fade"),Holes>0&&Faded>0);return true;
}
#endif

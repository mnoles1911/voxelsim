#include "VoxelAssetAppearance.h"
#include "VoxelTerrainSurfaceProbe.h"
#include "VoxelApprovedAppearanceProbe.h"
#include "voxelcore/assetappearancepack.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void SurfacePut(TArray<uint8>& B,int O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(8*I));}
FColor SurfaceRGB(FIntVector C){return FColor(uint8(91+C.X*23),uint8(153+C.Y*31),uint8(57+C.Z*47));}
FVector3f SurfaceRotate(FVector3f P,int Yaw){for(int I=0;I<Yaw;++I)P=FVector3f(-P.Y,P.X,P.Z);return P;}
struct FSurfaceExpected {FIntVector Zero;FVector3f SourceFraction;int Axis=2;bool Positive=true;bool Ray=true;};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainSurfaceAppearanceTest,"Voxel.Appearance.TerrainSurfaceGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelTerrainSurfaceAppearanceTest::RunTest(const FString&){
    FString Error;int Holes[2]={0,0},Covered[2]={0,0};
    for(int Policy=0;Policy<3;++Policy){const uint8 Front=Policy==0?24:Policy==1?19:20;
        // Actual nonsquare VXA and verified VAC: foliage front sheet with bark
        // immediately behind it. Source origin and page are both negative.
        std::vector<uint8_t> Vxa;auto Put=[&](uint32 V){for(int I=0;I<4;++I)Vxa.push_back(uint8(V>>(8*I)));};
        Put(vxc::kVxaMagic);Put(3);Put(uint32(-1));Put(uint32(-2));Put(uint32(-1));Put(3);Put(2);Put(2);Put(100);Put(12);Put(0);Put(0);
        for(int X=0;X<3;++X)for(int Y=0;Y<2;++Y)for(int Z=0;Z<2;++Z){Vxa.push_back(Z?Front:16);Put(1);}
        vxc::AssetGrid Grid;if(!TestTrue(TEXT("surface geometry parsed"),Grid.parse(Vxa)==vxc::AssetParseError::kOk))return false;
        const FString Hash=FMD5::HashBytes(Vxa.data(),int32(Vxa.size()));TArray<uint8> Packet;Packet.SetNumZeroed(248);FMemory::Memcpy(Packet.GetData(),"VAC1",4);
        SurfacePut(Packet,4,Policy==0?2:1);SurfacePut(Packet,8,3);SurfacePut(Packet,12,2);SurfacePut(Packet,16,2);SurfacePut(Packet,20,uint32(-1));SurfacePut(Packet,24,uint32(-2));SurfacePut(Packet,28,uint32(-1));SurfacePut(Packet,32,Policy==0?100000:100);SurfacePut(Packet,36,12);SurfacePut(Packet,40,Policy==2?1:0);HexToBytes(Hash,Packet.GetData()+48);
        for(int X=0;X<3;++X)for(int Y=0;Y<2;++Y)for(int Z=0;Z<2;++Z){auto R=Packet.GetData()+128+((X*2+Y)*2+Z)*10;R[0]=uint8(X);R[2]=uint8(Y);R[4]=uint8(Z);R[6]=Z?Front:16;const auto C=SurfaceRGB(FIntVector(X,Y,Z));R[7]=C.R;R[8]=C.G;R[9]=C.B;}
        TArray<uint8> Check;Check.Append(Packet.GetData(),96);Check.Append(Packet.GetData()+128,120);check(SHA256(Check.GetData(),Check.Num(),Packet.GetData()+96));
        auto Source=FVoxelAssetAppearance::Parse(Packet,Hash,Error);if(!TestTrue(TEXT("verified surface packet"),Source.IsValid()))return false;auto Sparse=Source->BuildSparseResource(Error);if(!TestTrue(TEXT("sparse surface source"),Sparse.IsValid()))return false;
        TArray<uint32> Sources;Sources.SetNumZeroed(9);Sources.Append(Sparse->Words);TArray<FUintVector2> Ranges;Ranges.SetNumZeroed(20);Ranges[8]=FUintVector2(9,uint32(Sparse->Words.Num()));Ranges[16]=FUintVector2(9,19);
        for(uint8 Yaw=0;Yaw<4;++Yaw){
            vxc::AssetField::ResolvedAssetInstance Instance;Instance.grid=&Grid;Instance.anchorVx=-16;Instance.anchorVy=-16;Instance.anchorVz=-16;Instance.yawQuarter=Yaw;
            std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered{Instance};std::vector<vxc::AssetAppearanceCell> Cells;std::vector<uint32_t> Packed;
            if(!TestTrue(TEXT("canonical page built"),vxc::assetAppearancePage(-1,-1,-1,Ordered,{5},[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Cells)&&vxc::assetPackAppearancePage(-1,-1,-1,91,Ordered,Cells,Packed)))return false;
            TArray<uint32> Pages;Pages.SetNumZeroed(7);Pages.Append(Packed.data(),int32(Packed.size()));
            TArray<FVoxelTerrainSurfaceProbeQuery> Queries;TArray<FVoxelApprovedAppearanceProbeQuery> Formula;TArray<FSurfaceExpected> Expected;
            auto Add=[&](FIntVector Zero,FVector3f Fraction,int Axis,bool Positive,bool Ray,float Footprint){
                const FVector3f WorldCell=SurfaceRotate(FVector3f(Zero+FIntVector(-1,-2,-1)),Yaw)+FVector3f(-16);
                const auto WorldFraction=SurfaceRotate(Fraction-FVector3f(.5f),Yaw)+FVector3f(.5f);FVector3f Normal=FVector3f::ZeroVector;Normal[Axis]=Positive?1.f:-1.f;Normal=SurfaceRotate(Normal,Yaw);int WorldAxis=0;while(Normal[WorldAxis]==0)++WorldAxis;
                FVoxelTerrainSurfaceProbeQuery Q;Q.PageBase=7;Q.PageCount=uint32(Packed.size());Q.RangeBase=3;Q.RangeCount=6;Q.X=int32(WorldCell.X);Q.Y=int32(WorldCell.Y);Q.Z=int32(WorldCell.Z);Q.Axis=uint32(WorldAxis);Q.Positive=Normal[WorldAxis]>0?1u:0u;Q.FractionX=WorldFraction.X;Q.FractionY=WorldFraction.Y;Q.FractionZ=WorldFraction.Z;Q.MaterialsLow=uint32(Front)|(16u<<8);Q.Steps=Ray?2:1;Q.PatternFootprint=Footprint;Queries.Add(Q);
                const auto C=SurfaceRGB(Zero);FVoxelApprovedAppearanceProbeQuery R;R.PackedMaterialRGB=uint32(Front)|(uint32(C.R)<<8)|(uint32(C.G)<<16)|(uint32(C.B)<<24);R.WorldAxis=Q.Axis;R.Positive=Q.Positive;R.Yaw=Yaw;R.SourceZeroX=Zero.X;R.SourceZeroY=Zero.Y;R.SourceZeroZ=Zero.Z;R.Needle=Policy==2?1:0;R.FractionX=Q.FractionX;R.FractionY=Q.FractionY;R.FractionZ=Q.FractionZ;R.PatternFootprint=Footprint;R.Foliage=Policy==0?0:1;Formula.Add(R);Expected.Add({Zero,Fraction,Axis,Positive,Ray});
            };
            for(int X=0;X<3;++X)for(int Y=0;Y<2;++Y)for(int U=0;U<16;++U)for(int V=0;V<16;++V)for(float F:{0.f,1.8f})Add(FIntVector(X,Y,1),FVector3f((U+.31f)/16,(V+.67f)/16,1),2,true,true,F);
            for(int Axis=0;Axis<3;++Axis)for(int Sign=0;Sign<2;++Sign){FVector3f F(.17f,.43f,.81f);F[Axis]=float(Sign);Add(FIntVector(1,0,1),F,Axis,Sign!=0,false,0);}
            TArray<FVoxelTerrainSurfaceProbeResult> Results;TArray<FVoxelApprovedAppearanceProbeResult> Reference;
            if(!VoxelRunApprovedAppearanceProbe(Formula,Reference,Error)||!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,Queries,Results,Error)){AddError(Error);return false;}
            if(!TestEqual(TEXT("surface probe result count"),Results.Num(),Queries.Num())||!TestEqual(TEXT("formula reference count"),Reference.Num(),Queries.Num()))return false;
            int Bad=0;
            for(int I=0;I<Results.Num();++I){const auto& R=Results[I];const auto& Ref=Reference[I];const auto& E=Expected[I];const auto& Q=Queries[I];const bool Open=Ref.Coverage<.5f;
                Bad+=Ref.Valid!=1||R.FirstApproved!=1||!FMath::IsNearlyEqual(R.FirstCoverage,Ref.Coverage,1.e-6f);
                if(Policy>0&&Q.PatternFootprint==0&&E.Ray){if(Open)++Holes[Policy-1];else ++Covered[Policy-1];}
                if(!E.Ray&&Open){Bad+=R.Hit!=0||R.Rejected!=1;continue;}
                const uint32 Step=Open?1u:0u;Bad+=R.Hit!=1||R.Step!=Step||R.Rejected!=Step||R.Material!=(Open?16u:uint32(Front))||R.Approved!=1||R.Coverage<.5f;
                auto Zero=E.Zero;if(Open)--Zero.Z;const auto RGB=FVoxelAssetAppearance::FaceColor(SurfaceRGB(Zero),Zero,E.Axis,E.Positive);const FVector3f SourceMetres=(FVector3f(Zero)+E.SourceFraction)*.1f;const FVector2f UV=E.Axis==2?FVector2f(SourceMetres.X,SourceMetres.Y):E.Axis==0?FVector2f(SourceMetres.Y,SourceMetres.Z):FVector2f(SourceMetres.X,SourceMetres.Z);
                Bad+=!FLinearColor(R.R,R.G,R.B).Equals(RGB,2.e-5f)||!FVector2f(R.U,R.V).Equals(UV,1.e-5f);
                Bad+=R.X!=Q.X||R.Y!=Q.Y||R.Z!=(Q.Z-int32(Step));
                if(Policy==0)Bad+=R.Step!=0||R.Rejected!=0||R.FirstCoverage!=1;
            }
            if(!TestEqual(TEXT("descriptor-to-source colors and cutout continuation exact"),Bad,0))return false;
            // Descriptor/range/material errors must be ordinary opaque fallback,
            // never an invented hole revealing geometry behind the front cell.
            TArray<FVoxelTerrainSurfaceProbeQuery> Invalid;auto Good=Queries[0];
            auto AddBad=[&](auto Change){auto Q=Good;Change(Q);Invalid.Add(Q);};
            AddBad([](auto& Q){Q.PageBase=MAX_uint32;});AddBad([](auto& Q){Q.PageCount=79;});AddBad([](auto& Q){Q.RangeBase=11;});AddBad([](auto& Q){Q.RangeCount=5;});AddBad([](auto& Q){Q.MaterialsLow=17u|(16u<<8);});AddBad([](auto& Q){Q.X=0;});AddBad([](auto& Q){Q.PitchMm=75;});
            if(!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,Invalid,Results,Error)){AddError(Error);return false;}
            for(const auto& R:Results)if(R.Hit!=1||R.Step!=0||R.Approved!=0||R.Rejected!=0||R.FirstCoverage!=1){AddError(TEXT("malformed metadata must remain opaque fallback"));return false;}
        }
    }
    TestTrue(TEXT("actual integrated broadleaf and needle paths have openings and retained foliage"),Holes[0]>0&&Holes[1]>0&&Covered[0]>0&&Covered[1]>0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCoarseTerrainSurfaceAppearanceTest,"Voxel.Appearance.CoarseTerrainSurfaceGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelCoarseTerrainSurfaceAppearanceTest::RunTest(const FString&){
    FString Error;const FIntVector Zero(128,127,130),Origin(-127,-126,-129),CoarseCell(-17,-15,-13);const FColor Base(147,83,189);
    int PartiallyFaded=0,UnfadedHoles=0;uint32 SeenLevels=0,SeenYaws=0;
    for(int Policy=0;Policy<3;++Policy){const uint8 Material=Policy==0?24:Policy==1?19:20;
        // One marked source cell in a large sparse box. Its source coordinate
        // stays positive even at L7 face offsets, so the independent formula
        // probe can represent the exact CPU-derived UV without modulo tricks.
        constexpr uint32 SX=257,SY=255,SZ=259,Count=SX*SY*SZ;
        const uint32 Linear=(uint32(Zero.X)*SY+uint32(Zero.Y))*SZ+uint32(Zero.Z);
        std::vector<uint8_t> Vxa;auto Put=[&](uint32 V){for(int J=0;J<4;++J)Vxa.push_back(uint8(V>>(8*J)));};
        Put(vxc::kVxaMagic);Put(3);Put(uint32(Origin.X));Put(uint32(Origin.Y));Put(uint32(Origin.Z));Put(SX);Put(SY);Put(SZ);Put(100);Put(3);Put(0);Put(0);Vxa.push_back(0);Put(Linear);Vxa.push_back(Material);Put(1);Vxa.push_back(0);Put(Count-Linear-1);
        vxc::AssetGrid Grid;if(!TestTrue(TEXT("sparse coarse representative VXA"),Grid.parse(Vxa)==vxc::AssetParseError::kOk))return false;
        const FString Hash=FMD5::HashBytes(Vxa.data(),int32(Vxa.size()));TArray<uint8> Packet;Packet.SetNumZeroed(138);FMemory::Memcpy(Packet.GetData(),"VAC1",4);SurfacePut(Packet,4,Policy==0?2:1);SurfacePut(Packet,8,SX);SurfacePut(Packet,12,SY);SurfacePut(Packet,16,SZ);SurfacePut(Packet,20,uint32(Origin.X));SurfacePut(Packet,24,uint32(Origin.Y));SurfacePut(Packet,28,uint32(Origin.Z));SurfacePut(Packet,32,Policy==0?100000:100);SurfacePut(Packet,36,1);SurfacePut(Packet,40,Policy==2?1:0);HexToBytes(Hash,Packet.GetData()+48);
        Packet[128]=uint8(Zero.X);Packet[130]=uint8(Zero.Y);Packet[132]=uint8(Zero.Z);Packet[134]=Material;Packet[135]=Base.R;Packet[136]=Base.G;Packet[137]=Base.B;TArray<uint8> Check;Check.Append(Packet.GetData(),96);Check.Append(Packet.GetData()+128,10);check(SHA256(Check.GetData(),Check.Num(),Packet.GetData()+96));
        auto Source=FVoxelAssetAppearance::Parse(Packet,Hash,Error);if(!TestTrue(TEXT("coarse representative VAC verified"),Source.IsValid()))return false;auto Sparse=Source->BuildSparseResource(Error);if(!TestTrue(TEXT("coarse sparse source built"),Sparse.IsValid()))return false;
        TArray<uint32> Sources;Sources.SetNumZeroed(9);Sources.Append(Sparse->Words);TArray<FUintVector2> Ranges;Ranges.SetNumZeroed(9);Ranges[8]=FUintVector2(9,uint32(Sparse->Words.Num()));
        for(uint8 Level=1;Level<=7;++Level)for(uint8 Yaw=0;Yaw<4;++Yaw){const int Scale=1<<Level,Half=Scale/2;
            SeenLevels|=1u<<Level;SeenYaws|=1u<<Yaw;
            const auto SourceRotated=SurfaceRotate(FVector3f(Zero+Origin),Yaw);vxc::AssetField::ResolvedAssetInstance Instance;Instance.grid=&Grid;Instance.yawQuarter=Yaw;Instance.anchorVx=int64(CoarseCell.X)*Scale+Half-int64(SourceRotated.X);Instance.anchorVy=int64(CoarseCell.Y)*Scale+Half-int64(SourceRotated.Y);Instance.anchorVz=int64(CoarseCell.Z)*Scale+Half-int64(SourceRotated.Z);
            std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered{Instance};std::vector<vxc::AssetAppearanceCell> Cells;std::vector<uint32_t> Packed;
            if(!TestTrue(TEXT("coarse representative page packed"),vxc::assetAppearancePage(-1,-1,-1,Ordered,{5},[](int64,int64,int64){return vxc::MAT_AIR;},[](int64,int64,int64){return false;},Cells,Level)&&vxc::assetPackAppearancePage(-1,-1,-1,73,Ordered,Cells,Packed,Level)))return false;
            if(!TestEqual(TEXT("exactly one representative source cell"),int32(Cells.size()),1))return false;
            TArray<uint32> Pages;Pages.SetNumZeroed(7);Pages.Append(Packed.data(),int32(Packed.size()));
            TArray<FVoxelTerrainSurfaceProbeQuery> Queries;TArray<FVoxelApprovedAppearanceProbeQuery> ReferenceQueries;TArray<FLinearColor> ExpectedColors;TArray<FVector2f> ExpectedUV;
            for(int Axis=0;Axis<3;++Axis)for(int Sign=0;Sign<2;++Sign)for(int Sample=0;Sample<12;++Sample)for(float Supplied:{0.f,2.4f}){
                FVector3f F((Sample+.17f)/12.f,(Sample*.37f+.23f)/5.f,(Sample*.61f+.41f)/8.f);F[Axis]=float(Sign);
                FVoxelTerrainSurfaceProbeQuery Q;Q.PageBase=7;Q.PageCount=uint32(Packed.size());Q.RangeBase=3;Q.RangeCount=6;Q.X=CoarseCell.X;Q.Y=CoarseCell.Y;Q.Z=CoarseCell.Z;Q.Axis=uint32(Axis);Q.Positive=uint32(Sign);Q.FractionX=F.X;Q.FractionY=F.Y;Q.FractionZ=F.Z;Q.MaterialsLow=Material;Q.Steps=1;Q.PitchMm=float(100*Scale);Q.PatternFootprint=Supplied;Queries.Add(Q);
                // Independent inverse matrix rotation about the representative
                // cell centre, applied to the physical coarse face displacement.
                const auto Delta=(F-FVector3f(.5f))*float(Scale)-FVector3f(.5f);const auto SourceFraction=SurfaceRotate(Delta,(4-Yaw)&3)+FVector3f(.5f);const auto P=FVector3f(Zero)+SourceFraction;
                FVector3f N=FVector3f::ZeroVector;N[Axis]=Sign?1.f:-1.f;N=SurfaceRotate(N,(4-Yaw)&3);int SourceAxis=0;while(N[SourceAxis]==0)++SourceAxis;
                ExpectedColors.Add(FVoxelAssetAppearance::FaceColor(Base,Zero,SourceAxis,N[SourceAxis]>0));const FVector2f Plane=SourceAxis==2?FVector2f(P.X,P.Y):SourceAxis==0?FVector2f(P.Y,P.Z):FVector2f(P.X,P.Z);ExpectedUV.Add(Plane*.1f);
                // Existing approved-formula GPU reference gets the CPU-derived
                // source plane, NOT the new helper's UV or footprint outputs.
                FVoxelApprovedAppearanceProbeQuery R;R.PackedMaterialRGB=uint32(Material)|(uint32(Base.R)<<8)|(uint32(Base.G)<<16)|(uint32(Base.B)<<24);R.WorldAxis=2;R.Positive=1;R.SourceZeroX=FMath::FloorToInt(Plane.X);R.SourceZeroY=FMath::FloorToInt(Plane.Y);R.FractionX=Plane.X-float(R.SourceZeroX);R.FractionY=Plane.Y-float(R.SourceZeroY);R.FractionZ=1;R.Needle=Policy==2?1:0;R.Foliage=Policy==0?0:1;R.PatternFootprint=FMath::Max(Supplied,float(Scale-1)*.1f/(Policy==2?.13f:.19f));ReferenceQueries.Add(R);
            }
            TArray<FVoxelTerrainSurfaceProbeResult> Results;TArray<FVoxelApprovedAppearanceProbeResult> Reference;
            if(!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,Queries,Results,Error)||!VoxelRunApprovedAppearanceProbe(ReferenceQueries,Reference,Error)){AddError(Error);return false;}
            if(!TestEqual(TEXT("coarse result count"),Results.Num(),Queries.Num())||!TestEqual(TEXT("coarse reference count"),Reference.Num(),Queries.Num()))return false;
            int Bad=0;for(int I=0;I<Results.Num();++I){const auto& R=Results[I];const auto& E=Reference[I];const bool Covered=E.Coverage>=.5f;
                Bad+=E.Valid!=1||R.FirstApproved!=1||!FMath::IsNearlyEqual(R.FirstCoverage,E.Coverage,1.e-5f)||R.Hit!=(Covered?1u:0u)||R.Rejected!=(Covered?0u:1u);
                if(Covered)Bad+=R.Approved!=1||R.Step!=0||R.Material!=Material||!FLinearColor(R.R,R.G,R.B).Equals(ExpectedColors[I],2.e-5f)||!FVector2f(R.U,R.V).Equals(ExpectedUV[I],2.e-5f)||R.X!=CoarseCell.X||R.Y!=CoarseCell.Y||R.Z!=CoarseCell.Z;
                if(Policy==0||Level>=3||Queries[I].PatternFootprint>=1.8f)Bad+=R.FirstCoverage!=1;
                if(E.Coverage>0&&E.Coverage<1)++PartiallyFaded;if(E.Coverage==0)++UnfadedHoles;
            }
            if(!TestEqual(TEXT("coarse source color UV and physical-footprint mask fade"),Bad,0))return false;
            TArray<FVoxelTerrainSurfaceProbeQuery> Invalid;auto Q=Queries[0];Q.PitchMm=100;Invalid.Add(Q);Q=Queries[0];Q.PitchMm+=100;Invalid.Add(Q);
            if(!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,Invalid,Results,Error)){AddError(Error);return false;}for(const auto& R:Results)if(R.Approved!=0||R.FirstApproved!=0||R.Hit!=1||R.Rejected!=0){AddError(TEXT("mismatched coarse pitch must be opaque fallback"));return false;}
            Pages[7+5]=8;Invalid={Queries[0]};if(!VoxelRunTerrainSurfaceProbe(Pages,Sources,Ranges,Invalid,Results,Error)){AddError(Error);return false;}if(Results[0].Approved!=0||Results[0].Hit!=1||Results[0].Rejected!=0){AddError(TEXT("out-of-range page level must be opaque fallback"));return false;}
        }
    }
    TestEqual(TEXT("all seven coarse levels"),SeenLevels,254u);TestEqual(TEXT("all four coarse source yaws"),SeenYaws,15u);TestTrue(TEXT("coarse tests exercise partial fade and unfaded openings"),PartiallyFaded>0&&UnfadedHoles>0);
    return true;
}
#endif

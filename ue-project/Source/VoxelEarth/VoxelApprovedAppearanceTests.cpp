#include "VoxelAssetAppearance.h"
#include "VoxelApprovedAppearanceProbe.h"
#include <openssl/sha.h>
#include <limits>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
namespace {
void ApprovedPut(TArray<uint8>& B,int32 O,uint32 V){for(int I=0;I<4;++I)B[O+I]=uint8(V>>(I*8));}
TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> ApprovedSource(uint32 Pitch,FString& Error){
    TArray<uint8> B;B.SetNumZeroed(138);FMemory::Memcpy(B.GetData(),"VAC1",4);ApprovedPut(B,4,1);
    for(int A=0;A<3;++A)ApprovedPut(B,8+4*A,20);
    ApprovedPut(B,20,uint32(-7));ApprovedPut(B,24,uint32(-11));ApprovedPut(B,28,uint32(-13));ApprovedPut(B,32,Pitch);ApprovedPut(B,36,1);
    FMemory::Memset(B.GetData()+48,0x33,16);B[128]=2;B[130]=4;B[132]=6;B[134]=16;B[135]=89;B[136]=173;B[137]=41;
    TArray<uint8> Checked;Checked.Append(B.GetData(),96);Checked.Append(B.GetData()+128,10);check(SHA256(Checked.GetData(),Checked.Num(),B.GetData()+96));
    return FVoxelAssetAppearance::Parse(MoveTemp(B),FString::ChrN(32,'3'),Error);
}
FVector3f ApprovedRotate(FVector3f V,int Yaw){for(int I=0;I<Yaw;++I){const float X=V.X;V.X=-V.Y;V.Y=X;}return V;}
struct FApprovedExpected {FLinearColor Color;FVector2f UV;};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelApprovedFaceGpuTest,"Voxel.Appearance.ApprovedFaceGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelApprovedFaceGpuTest::RunTest(const FString&){
    TArray<FVoxelApprovedAppearanceProbeQuery> Queries;TArray<FApprovedExpected> Expected;FString Error;
    const FColor Base(89,173,41);const FIntVector SourceZero(2,4,6);const FVector3f SourceCell(-5,-7,-7);
    for(uint32 Pitch:{25u,50u,100u}){
        auto Source=ApprovedSource(Pitch,Error);if(!TestTrue(TEXT("verified source for CPU UV reference"),Source.IsValid()))return false;
        for(uint32 Yaw=0;Yaw<4;++Yaw){auto View=FVoxelAssetAppearance::ForCanonicalYaw(Source,uint8(Yaw));
            for(int Axis=0;Axis<3;++Axis)for(int Sign:{-1,1})for(uint32 Needle:{0u,1u})for(int Corner=0;Corner<9;++Corner){
                const FVector3f Fraction=Corner==8?FVector3f(.17f,.43f,.89f):FVector3f(float(Corner&1),float((Corner>>1)&1),float((Corner>>2)&1));
                const auto WorldFraction=ApprovedRotate(Fraction-FVector3f(.5f),int(Yaw))+FVector3f(.5f);
                FVector3f Normal=FVector3f::ZeroVector;Normal[Axis]=float(Sign);Normal=ApprovedRotate(Normal,int(Yaw));int WorldAxis=0;while(Normal[WorldAxis]==0)++WorldAxis;
                auto& Q=Queries.AddDefaulted_GetRef();Q.PackedMaterialRGB=16u|(89u<<8)|(173u<<16)|(41u<<24);Q.WorldAxis=uint32(WorldAxis);Q.Positive=Normal[WorldAxis]>0?1u:0u;Q.Yaw=Yaw;
                Q.SourceZeroX=2;Q.SourceZeroY=4;Q.SourceZeroZ=6;Q.Needle=Needle;Q.FractionX=WorldFraction.X;Q.FractionY=WorldFraction.Y;Q.FractionZ=WorldFraction.Z;Q.PitchMm=float(Pitch);Q.Foliage=0;
                const auto Color=FVoxelAssetAppearance::FaceColor(Base,SourceZero,Axis,Sign>0);
                const auto UV=Source->FaceUV((SourceCell+Fraction)*(float(Pitch)*.1f),Axis);
                const auto RotUV=View->FaceUV((ApprovedRotate(SourceCell,int(Yaw))+WorldFraction)*(float(Pitch)*.1f),WorldAxis);
                if(!UV.Equals(RotUV,1.e-5f)){AddError(TEXT("independent forward-rotation fixture disagrees with existing canonical UV mapping"));return false;}
                Expected.Add({Color,UV});
            }
        }
    }
    TArray<FVoxelApprovedAppearanceProbeResult> Results;
    if(!TestTrue(TEXT("GPU approved-face probe completed"),VoxelRunApprovedAppearanceProbe(Queries,Results,Error))){AddError(Error);return false;}
    if(!TestEqual(TEXT("all face queries returned"),Results.Num(),Expected.Num()))return false;
    for(int32 I=0;I<Results.Num();++I){const auto& R=Results[I];const auto& E=Expected[I];
        if(R.Valid!=1||!FLinearColor(R.R,R.G,R.B).Equals(E.Color,2.e-5f)||!FVector2f(R.U,R.V).Equals(E.UV,1.e-5f)||R.Coverage!=1.f){AddError(FString::Printf(TEXT("approved face/UV/bypass mismatch query%d"),I));return false;}
    }
    // Coverage checks use repeated IDENTICAL UV/needle samples at four explicit
    // footprints. They test the authority's fade without pretending CPU sin()
    // yields bit-identical randomized leaf edges on every GPU vendor.
    Queries.Reset();
    for(uint32 Needle:{0u,1u})for(int X=0;X<16;++X)for(int Y=0;Y<16;++Y)for(float Footprint:{0.f,.65f,1.225f,1.8f}){
        auto& Q=Queries.AddDefaulted_GetRef();Q.PackedMaterialRGB=16u|(89u<<8)|(173u<<16)|(41u<<24);Q.WorldAxis=2;Q.Positive=1;Q.Needle=Needle;Q.Foliage=1;
        Q.SourceZeroX=X;Q.SourceZeroY=Y;Q.FractionX=.31f;Q.FractionY=.67f;Q.FractionZ=1;Q.PatternFootprint=Footprint;
    }
    if(!VoxelRunApprovedAppearanceProbe(Queries,Results,Error)){AddError(Error);return false;}
    if(!TestEqual(TEXT("all coverage or invalid queries returned"),Results.Num(),Queries.Num()))return false;
    int Holes[2]={0,0},Leaves[2]={0,0};
    for(int I=0;I<Results.Num();I+=4){const auto& A=Results[I];const int Needle=int(Queries[I].Needle);
        if(A.Valid!=1||(A.Coverage!=0.f&&A.Coverage!=1.f)){AddError(TEXT("near leaf coverage must be binary"));return false;}
        if(A.Coverage==0)++Holes[Needle];else ++Leaves[Needle];
        for(int J=0;J<4;++J)if(Results[I+J].Valid!=1){AddError(TEXT("valid footprint refused"));return false;}
        if(Results[I+1].Coverage!=A.Coverage||!FMath::IsNearlyEqual(Results[I+2].Coverage,(1.f+A.Coverage)*.5f,1.e-5f)||Results[I+3].Coverage!=1.f){AddError(TEXT("footprint fade differs from approved material"));return false;}
    }
    TestTrue(TEXT("broadleaf and needle samples include real openings and solid leaf areas"),Holes[0]>0&&Holes[1]>0&&Leaves[0]>0&&Leaves[1]>0);
    Queries.Reset();FVoxelApprovedAppearanceProbeQuery Good;Good.PackedMaterialRGB=2u|(89u<<8)|(173u<<16)|(41u<<24);Good.Foliage=0;
    Queries.Add(Good); // no material-ID heuristic: explicit bypass applies to any supplied material
    auto Bad=[&](auto Change){auto Q=Good;Change(Q);Queries.Add(Q);};
    Bad([](auto& Q){Q.WorldAxis=3;});Bad([](auto& Q){Q.Yaw=4;});Bad([](auto& Q){Q.Positive=2;});Bad([](auto& Q){Q.Needle=2;});Bad([](auto& Q){Q.Foliage=2;});
    Bad([](auto& Q){Q.PitchMm=75;});Bad([](auto& Q){Q.FractionX=-.01f;});Bad([](auto& Q){Q.FractionY=1.01f;});Bad([](auto& Q){Q.SourceZeroX=-1;});Bad([](auto& Q){Q.SourceZeroZ=65535;});
    Bad([](auto& Q){Q.PatternFootprint=-1;});Bad([](auto& Q){Q.FractionZ=std::numeric_limits<float>::quiet_NaN();});Bad([](auto& Q){Q.PatternFootprint=std::numeric_limits<float>::infinity();});Bad([](auto& Q){Q.PitchMm=std::numeric_limits<float>::quiet_NaN();});Bad([](auto& Q){Q.Reserved0=1;});
    if(!VoxelRunApprovedAppearanceProbe(Queries,Results,Error)){AddError(Error);return false;}
    if(!TestEqual(TEXT("all coverage or invalid queries returned"),Results.Num(),Queries.Num()))return false;
    TestTrue(TEXT("explicit non-foliage ignores material categorization"),Results[0].Valid==1&&Results[0].Coverage==1);
    for(int I=1;I<Results.Num();++I)TestEqual(TEXT("invalid probe input refused"),Results[I].Valid,0u);
    return true;
}
#endif

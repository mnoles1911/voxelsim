#include "VoxelAssetAppearance.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace {
void Put32(TArray<uint8>& B,int32 Offset,uint32 V){for(int I=0;I<4;++I)B[Offset+I]=uint8(V>>(I*8));}
void Seal(TArray<uint8>& B){TArray<uint8> Checked;Checked.Append(B.GetData(),96);Checked.Append(B.GetData()+128,B.Num()-128);check(SHA256(Checked.GetData(),Checked.Num(),B.GetData()+96));}
TArray<uint8> Fixture(){
    TArray<uint8> B;B.SetNumZeroed(168);FMemory::Memcpy(B.GetData(),"VAC1",4);
    Put32(B,4,1);Put32(B,8,3);Put32(B,12,3);Put32(B,16,3);
    Put32(B,20,uint32(-2));Put32(B,24,uint32(-3));Put32(B,28,0);
    Put32(B,32,100);Put32(B,36,4);Put32(B,40,1);FMemory::Memset(B.GetData()+48,0x11,16);
    const FIntVector C[]={{0,0,0},{0,0,1},{1,1,1},{2,2,2}};
    for(int I=0;I<4;++I){uint8* R=B.GetData()+128+I*10;for(int A=0;A<3;++A){R[A*2]=uint8(C[I][A]);R[A*2+1]=0;}R[6]=16;R[7]=uint8(80+I);R[8]=99;R[9]=79;}
    Seal(B);return B;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetAppearanceTest,"Voxel.Appearance.SourceBinding",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAssetAppearanceTest::RunTest(const FString&){
    const FString MD5=FString::ChrN(32,'1');FString Error;
    auto A=FVoxelAssetAppearance::Parse(Fixture(),MD5,Error);
    if(!TestTrue(TEXT("valid complete source appearance"),A.IsValid()))return false;
    TestTrue(TEXT("needle metadata independent of RGB"),A->IsNeedle());
    TestEqual(TEXT("all sparse source records retained"),A->PointCount(),4);
    FColor Base;FIntVector Cell;
    TestTrue(TEXT("interior voxel has color before and after surface exposure"),A->Sample(FIntVector(-1,-2,1),100,16,Base,Cell));
    TestEqual(TEXT("source coordinate survives negative origin"),Cell,FIntVector(1,1,1));
    TestEqual(TEXT("interior RGB exact"),Base,FColor(82,99,79));
    TestFalse(TEXT("new material does not inherit old wood appearance"),A->Sample(FIntVector(-1,-2,1),100,20,Base,Cell));
    TestFalse(TEXT("unoccupied source location has no invented color"),A->Sample(FIntVector(-1,-3,0),100,16,Base,Cell));
    TestTrue(TEXT("coarse material selects deterministic source child"),A->Sample(FIntVector(-1,-1,0),200,16,Base,Cell));
    TestEqual(TEXT("coarse lookup source identity"),Cell,FIntVector(1,1,1));
    TestTrue(TEXT("source-relative mask coordinate"),A->FaceUV(FVector3f(-10,-20,10),2).Equals(FVector2f(.1f,.1f),1.e-6f));
    auto Refuse=[&](TArray<uint8> B,const TCHAR* Why){TestFalse(Why,FVoxelAssetAppearance::Parse(MoveTemp(B),MD5,Error).IsValid());TestFalse(TEXT("refusal supplies reason"),Error.IsEmpty());};
    auto B=Fixture();B.Pop();Refuse(B,TEXT("truncation refused"));
    B=Fixture();B[20]^=1;Refuse(B,TEXT("origin corruption refused"));
    B=Fixture();B[135]^=1;Refuse(B,TEXT("color corruption refused"));
    B=Fixture();B[48]=0x22;Seal(B);Refuse(B,TEXT("wrong geometry refused"));
    B=Fixture();FMemory::Memcpy(B.GetData()+138,B.GetData()+128,10);Seal(B);Refuse(B,TEXT("duplicate coordinates refused"));
    B=Fixture();B[128]=3;Seal(B);Refuse(B,TEXT("out-of-bounds coordinates refused"));
    B=Fixture();B[134]=0;Seal(B);Refuse(B,TEXT("air record refused"));
    B=Fixture();Put32(B,36,MAX_uint32);Refuse(B,TEXT("count overflow refused"));
    const FColor RGB(112,99,79);const FIntVector C(3,5,7);
    const auto Top=FVoxelAssetAppearance::FaceColor(RGB,C,2,true);
    const auto Side=FVoxelAssetAppearance::FaceColor(RGB,C,0,true);
    TestTrue(TEXT("face variation stable across repeated rebuilds"),Top==FVoxelAssetAppearance::FaceColor(RGB,C,2,true));
    TestFalse(TEXT("distinct face tones"),Top.Equals(Side,1.e-6f));
    TestTrue(TEXT("bounded valid face color"),Top.R>0&&Top.R<1&&Top.G>0&&Top.G<1&&Top.B>0&&Top.B<1);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetAppearanceYawTest,"Voxel.Appearance.CanonicalYaw",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAssetAppearanceYawTest::RunTest(const FString&){
    FString Error;auto Source=FVoxelAssetAppearance::Parse(Fixture(),FString::ChrN(32,'1'),Error);
    if(!Source)return false;
    const FIntVector Cells[]={{-2,-3,0},{-2,-3,1},{-1,-2,1},{0,-1,2}};
    auto Rotate=[](FVector3f P,int Q){for(int I=0;I<Q;++I){const float X=P.X;P.X=-P.Y;P.Y=X;}return P;};
    for(uint8 Q=0;Q<4;++Q){
        auto View=FVoxelAssetAppearance::ForCanonicalYaw(Source,Q);
        if(!TestTrue(TEXT("canonical view available"),View.IsValid()))return false;
        for(const auto& C:Cells){
            const auto W=Rotate(FVector3f(C),Q);const FIntVector World(int32(W.X),int32(W.Y),int32(W.Z));
            FColor Expected,Actual;FIntVector EC,AC;
            TestTrue(TEXT("original occupied cell"),Source->Sample(C,100,16,Expected,EC));
            TestTrue(TEXT("rotated occupied cell"),View->Sample(World,100,16,Actual,AC));
            TestEqual(TEXT("RGB preserved"),Actual,Expected);TestEqual(TEXT("hash cell preserved"),AC,EC);
            for(int Axis=0;Axis<3;++Axis)for(int Sign:{-1,1}){
                FVector3f N=FVector3f::ZeroVector;N[Axis]=float(Sign);N=Rotate(N,Q);
                int WorldAxis=0;while(N[WorldAxis]==0)++WorldAxis;
                TestTrue(TEXT("original face tone preserved"),View->ColorForFace(Actual,AC,WorldAxis,N[WorldAxis]>0).Equals(Source->ColorForFace(Expected,EC,Axis,Sign>0),1.e-6f));
                for(int Bits=0;Bits<8;++Bits){
                    const FVector3f Corner(float(Bits&1),float((Bits>>1)&1),float((Bits>>2)&1));
                    const auto WorldCorner=Rotate(Corner-FVector3f(.5f),Q)+FVector3f(.5f);
                    TestTrue(TEXT("original leaf UV phase preserved"),View->FaceUV((W+WorldCorner)*10.f,WorldAxis).Equals(Source->FaceUV((FVector3f(C)+Corner)*10.f,Axis),1.e-6f));
                }
            }
        }
    }
    TestFalse(TEXT("invalid yaw refused"),FVoxelAssetAppearance::ForCanonicalYaw(Source,4).IsValid());
    TestFalse(TEXT("nested transformed binding refused"),FVoxelAssetAppearance::ForCanonicalYaw(FVoxelAssetAppearance::ForCanonicalYaw(Source,1),1).IsValid());
    return true;
}
#endif

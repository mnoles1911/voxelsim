#include "VoxelAssetAppearance.h"
#include <openssl/sha.h>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "VoxelSparseAppearanceProbe.h"
#include "voxelcore/assetappearancepack.h"
namespace {
void SparsePut(TArray<uint8>& B,int32 Offset,uint32 V){for(int32 I=0;I<4;++I)B[Offset+I]=uint8(V>>(I*8));}
TArray<uint8> SparseFixture(uint32 Pitch=100){
    TArray<FIntVector> Cells;
    for(int32 X=0;X<19;++X)for(int32 Y=0;Y<18;++Y)for(int32 Z=0;Z<17;++Z)
        if((X+Y*3+Z*5)%7==0)Cells.Add(FIntVector(X,Y,Z));
    TArray<uint8> B;B.SetNumZeroed(128+Cells.Num()*10);FMemory::Memcpy(B.GetData(),"VAC1",4);
    SparsePut(B,4,1);SparsePut(B,8,19);SparsePut(B,12,18);SparsePut(B,16,17);
    SparsePut(B,20,uint32(-9));SparsePut(B,24,uint32(-13));SparsePut(B,28,uint32(-5));
    SparsePut(B,32,Pitch);SparsePut(B,36,uint32(Cells.Num()));SparsePut(B,40,1);FMemory::Memset(B.GetData()+48,0x22,16);
    for(int32 I=0;I<Cells.Num();++I){const auto C=Cells[I];uint8* R=B.GetData()+128+I*10;
        for(int A=0;A<3;++A){R[A*2]=uint8(C[A]);R[A*2+1]=uint8(C[A]>>8);}
        R[6]=uint8(16+I%3);R[7]=uint8(I*13);R[8]=uint8(I*37);R[9]=uint8(I*71);
    }
    TArray<uint8> Checked;Checked.Append(B.GetData(),96);Checked.Append(B.GetData()+128,B.Num()-128);
    check(SHA256(Checked.GetData(),Checked.Num(),B.GetData()+96));return B;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSparseAppearanceTest,"Voxel.Appearance.SparseSource",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelSparseAppearanceTest::RunTest(const FString&){
    FString Error;auto A=FVoxelAssetAppearance::Parse(SparseFixture(),FString::ChrN(32,'2'),Error);
    if(!TestTrue(TEXT("verified sparse fixture"),A.IsValid()))return false;
    TestFalse(TEXT("sort budget refused before building"),A->BuildSparseResource(Error,1024*1024,1).IsValid());
    TestFalse(TEXT("output budget refused"),A->BuildSparseResource(Error,1,1024*1024).IsValid());
    auto S=A->BuildSparseResource(Error);if(!TestTrue(TEXT("bounded sparse resource built"),S.IsValid()))return false;
    TestTrue(TEXT("multiple source bricks"),S->Words[3]>1);
    TestEqual(TEXT("all source records retained"),S->Words[2],uint32(A->PointCount()));
    TestEqual(TEXT("negative origin signed header"),int32(S->Words[10]),-9);
    TestEqual(TEXT("original MD5 words"),S->Words[16],0x22222222u);
    TestTrue(TEXT("one cached resource per original"),S==A->BuildSparseResource(Error));
    TestFalse(TEXT("cached output still obeys caller budget"),A->BuildSparseResource(Error,S->RequiredOutputBytes-1,S->RequiredWorkingBytes).IsValid());
    TestFalse(TEXT("cached working requirement cannot be bypassed"),A->BuildSparseResource(Error,S->RequiredOutputBytes,S->RequiredWorkingBytes-1).IsValid());
    int32 Hits=0,Misses=0;
    for(int32 X=-1;X<=19;++X)for(int32 Y=-1;Y<=18;++Y)for(int32 Z=-1;Z<=17;++Z)for(uint8 M:{uint8(16),uint8(17),uint8(18),uint8(25)}){
        const FIntVector P(X-9,Y-13,Z-5);FColor Expected,Actual;FIntVector EC,AC;
        const bool E=A->Sample(P,100,M,Expected,EC),G=S->Lookup(P,M,Actual,AC);
        if(E)++Hits;else ++Misses;
        if(E!=G||(E&&(Expected!=Actual||EC!=AC))){AddError(FString::Printf(TEXT("lookup mismatch at%d,%d,%d material%u"),X,Y,Z,uint32(M)));return false;}
    }
    TestEqual(TEXT("every colored cell matches exactly once"),Hits,A->PointCount());TestTrue(TEXT("holes and bounds checked"),Misses>Hits);
    for(uint8 Yaw:{uint8(1),uint8(2),uint8(3)})TestFalse(TEXT("yaw view cannot create wrong-frame resource"),FVoxelAssetAppearance::ForCanonicalYaw(A,Yaw)->BuildSparseResource(Error).IsValid());
    for(uint32 Pitch:{25u,50u}){auto Fine=FVoxelAssetAppearance::Parse(SparseFixture(Pitch),FString::ChrN(32,'2'),Error);if(!TestTrue(TEXT("verified fine-pitch fixture"),Fine.IsValid()))return false;auto R=Fine->BuildSparseResource(Error);
        if(!TestTrue(TEXT("fine-pitch resource built"),R.IsValid()))return false;
        TestEqual(TEXT("source pitch preserved"),R->Words[13],Pitch);FColor C;FIntVector P;TestTrue(TEXT("fine-pitch original cell lookup"),R->Lookup(FIntVector(-9,-13,-5),16,C,P));}
    FVoxelSparseAppearance Broken;Broken.Words=S->Words;Broken.Words[6]++;FColor C;FIntVector P;
    TestFalse(TEXT("corrupt offsets rejected"),Broken.Lookup(FIntVector(-9,-13,-5),16,C,P));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSparseAppearanceGpuTest,"Voxel.Appearance.SparseGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelSparseAppearanceGpuTest::RunTest(const FString&)
{
    FString Error;auto Source=FVoxelAssetAppearance::Parse(SparseFixture(),FString::ChrN(32,'2'),Error);
    if(!TestTrue(TEXT("verified GPU fixture"),Source.IsValid()))return false;
    auto Sparse=Source->BuildSparseResource(Error);if(!TestTrue(TEXT("sparse GPU resource"),Sparse.IsValid()))return false;
    // Independent VXA with the same source occupancy/materials as VAC1.
    std::vector<uint8_t> Bytes;auto Put=[&](uint32 V){for(int I=0;I<4;++I)Bytes.push_back(uint8_t(V>>(I*8)));};
    Put(vxc::kVxaMagic);Put(vxc::kVxaVersion);Put(uint32(-9));Put(uint32(-13));Put(uint32(-5));
    Put(19);Put(18);Put(17);Put(100);Put(19*18*17);Put(0);Put(0);
    uint32 Colored=0;
    for(int X=0;X<19;++X)for(int Y=0;Y<18;++Y)for(int Z=0;Z<17;++Z){
        const uint8 Material=(X+Y*3+Z*5)%7==0?uint8(16+(Colored++)%3):0;Bytes.push_back(Material);Put(1);}
    vxc::AssetGrid Grid;if(!TestTrue(TEXT("matching source VXA"),Grid.parse(Bytes)==vxc::AssetParseError::kOk))return false;
    TArray<uint32> Sources;Sources.SetNumZeroed(7);Sources.Append(Sparse->Words);
    TArray<FUintVector2> Ranges;Ranges.SetNumZeroed(8);Ranges[7]=FUintVector2(7,uint32(Sparse->Words.Num()));
    for(uint8 Yaw=0;Yaw<4;++Yaw){
        vxc::AssetField::ResolvedAssetInstance First;First.grid=&Grid;First.anchorVx=-16;First.anchorVy=-16;First.anchorVz=-16;First.yawQuarter=Yaw;
        auto Second=First;Second.anchorVx+=3;
        std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered{First,Second};
        auto Terrain=[](int64,int64,int64 Z){return Z==-18?vxc::MAT_ROCK:vxc::MAT_AIR;};
        auto Touched=[](int64 X,int64,int64){return X==-17;};
        std::vector<vxc::AssetAppearanceCell> Cells;
        if(!TestTrue(TEXT("canonical appearance cells"),vxc::assetAppearancePage(-1,-1,-1,Ordered,{7,7},Terrain,Touched,Cells)))return false;
        std::vector<uint32_t> Packed;if(!TestTrue(TEXT("packed appearance page"),vxc::assetPackAppearancePage(-1,-1,-1,91,Ordered,Cells,Packed)))return false;
        TArray<uint32> Page;Page.Append(Packed.data(),int32(Packed.size()));
        TArray<FVoxelSparseAppearanceProbeQuery> Queries;
        for(uint32 Z=0;Z<32;++Z)for(uint32 Y=0;Y<32;++Y)for(uint32 X=0;X<32;++X){
            const int64 WX=int64(X)-32,WY=int64(Y)-32,WZ=int64(Z)-32;
            const auto M=Terrain(WX,WY,WZ);Queries.Add({X,Y,Z,uint32(M!=vxc::MAT_AIR?M:vxc::AssetField::materialAtResolved(Ordered,WX,WY,WZ))});}
        TArray<FVoxelSparseAppearanceProbeResult> Results;
        if(!VoxelRunSparseAppearanceProbe(Page,Sources,Ranges,Queries,Results,Error)){AddError(Error);return false;}
        if(!TestEqual(TEXT("all GPU query results"),Results.Num(),Queries.Num()))return false;
        auto View=FVoxelAssetAppearance::ForCanonicalYaw(Source,Yaw);int Hits=0,FirstHit=-1;
        for(int I=0;I<Queries.Num();++I){const auto& Q=Queries[I];const auto& R=Results[I];
            const int64 WX=int64(Q.X)-32,WY=int64(Q.Y)-32,WZ=int64(Q.Z)-32;
            bool Expected=false;FColor Color;FIntVector Zero;
            if(Terrain(WX,WY,WZ)==vxc::MAT_AIR&&!Touched(WX,WY,WZ))for(const auto& Instance:Ordered){
                const auto M=vxc::AssetField::materialAtResolved({Instance},WX,WY,WZ);if(M==vxc::MAT_AIR)continue;
                Expected=View->Sample(FIntVector(int32(WX-Instance.anchorVx),int32(WY-Instance.anchorVy),int32(WZ-Instance.anchorVz)),100,uint8(M),Color,Zero);break;}
            const uint32 ExpectedPacked=Expected?(Q.Material|(uint32(Color.R)<<8)|(uint32(Color.G)<<16)|(uint32(Color.B)<<24)):0;
            if(bool(R.Hit)!=Expected||(Expected&&(R.Resource!=7||R.PackedMaterialRGB!=ExpectedPacked||R.Yaw!=Yaw||R.Needle!=1||
                R.ZeroX!=uint32(Zero.X)||R.ZeroY!=uint32(Zero.Y)||R.ZeroZ!=uint32(Zero.Z)||
                R.SourceX!=Zero.X-9||R.SourceY!=Zero.Y-13||R.SourceZ!=Zero.Z-5))){
                AddError(FString::Printf(TEXT("GPU source/page mismatch yaw%u cell%u,%u,%u expectedHit%d actualHit%u expectedPacked%u actualPacked%u"),uint32(Yaw),Q.X,Q.Y,Q.Z,Expected,R.Hit,ExpectedPacked,R.PackedMaterialRGB));return false;}
            if(Expected){++Hits;if(FirstHit<0)FirstHit=I;}
        }
        if(!TestTrue(TEXT("actual colored winners exercised"),Hits>0))return false;
        auto ShortRanges=Ranges;ShortRanges[7].Y--;TArray<FVoxelSparseAppearanceProbeQuery> One{Queries[FirstHit]};
        if(!VoxelRunSparseAppearanceProbe(Page,Sources,ShortRanges,One,Results,Error)){AddError(Error);return false;}
        TestEqual(TEXT("truncated source range refuses lookup"),Results[0].Hit,0u);
        // Source lookup uses the packet version's units. The page here supplies
        // coordinates only; production admission separately matches VXA pitch.
        for(uint32 Pitch:{12500u,25000u,100000u}){
            auto GenericSources=Sources;GenericSources[7]=2;GenericSources[7+13]=Pitch;GenericSources[7+15]=0;
            if(!VoxelRunSparseAppearanceProbe(Page,GenericSources,Ranges,One,Results,Error)){AddError(Error);return false;}
            TestEqual(TEXT("generic micrometre pitch remains visible on GPU"),Results[0].Hit,1u);
            const auto& Q=One[0];const int64 WX=int64(Q.X)-32,WY=int64(Q.Y)-32,WZ=int64(Q.Z)-32;
            FColor ExpectedColor;FIntVector ExpectedCell;
            for(const auto& Instance:Ordered){const auto M=vxc::AssetField::materialAtResolved({Instance},WX,WY,WZ);if(M==vxc::MAT_AIR)continue;
                View->Sample(FIntVector(int32(WX-Instance.anchorVx),int32(WY-Instance.anchorVy),int32(WZ-Instance.anchorVz)),100,uint8(M),ExpectedColor,ExpectedCell);break;}
            TestEqual(TEXT("generic GPU source RGB preserved"),Results[0].PackedMaterialRGB,Q.Material|(uint32(ExpectedColor.R)<<8)|(uint32(ExpectedColor.G)<<16)|(uint32(ExpectedColor.B)<<24));
        }
        for(uint32 Pitch:{0u,1000001u}){
            auto InvalidSources=Sources;InvalidSources[7]=2;InvalidSources[7+13]=Pitch;
            if(!VoxelRunSparseAppearanceProbe(Page,InvalidSources,Ranges,One,Results,Error)){AddError(Error);return false;}
            TestEqual(TEXT("invalid generic pitch refused by GPU"),Results[0].Hit,0u);
            FVoxelSparseAppearance Invalid=*Sparse;Invalid.Words[0]=2;Invalid.Words[13]=Pitch;FColor C;FIntVector P;
            TestFalse(TEXT("invalid generic pitch refused by CPU"),Invalid.Lookup(FIntVector(-9,-13,-5),16,C,P));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGenericAppearanceTest,"Voxel.Appearance.GenericPacket",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelGenericAppearanceTest::RunTest(const FString&) {
    for(uint32 Pitch:{10000u,12500u,25000u,100000u}) {
        auto Bytes=SparseFixture(100);SparsePut(Bytes,32,Pitch);SparsePut(Bytes,4,2);SparsePut(Bytes,44,0);
        for(int I=128;I<Bytes.Num();I+=10)Bytes[I+6]=1;
        TArray<uint8> Check;Check.Append(Bytes.GetData(),96);Check.Append(Bytes.GetData()+128,Bytes.Num()-128);
        check(SHA256(Check.GetData(),Check.Num(),Bytes.GetData()+96));
        FString Error;auto A=FVoxelAssetAppearance::Parse(Bytes,FString::ChrN(32,'2'),Error);
        if(!TestTrue(TEXT("generic non-tree material and pitch parsed"),A.IsValid()))return false;
        TestFalse(TEXT("explicit opaque policy"),A->UsesFoliageMask());
        TestFalse(TEXT("yaw preserves opaque policy"),FVoxelAssetAppearance::ForCanonicalYaw(A,1)->UsesFoliageMask());
        auto Resource=A->BuildSparseResource(Error);if(!TestTrue(TEXT("generic sparse transport"),Resource.IsValid()))return false;
        TestEqual(TEXT("generic resource version"),Resource->Words[0],2u);TestEqual(TEXT("mask opt-out transported"),Resource->Words[15],0u);
        FColor C;FIntVector Cell;TestTrue(TEXT("non-tree source material lookup"),Resource->Lookup(FIntVector(-9,-13,-5),1,C,Cell));
        TestTrue(TEXT("exact fractional pitch sample"),A->Sample(FIntVector(-9,-13,-5),double(Pitch)/1000.,1,C,Cell));
        TestFalse(TEXT("noninteger cell ratio refused"),A->Sample(FIntVector(-9,-13,-5),double(Pitch)/1000.*1.5,1,C,Cell));
        TestTrue(TEXT("eight-cell coarse hierarchy retains source appearance"),A->Sample(FIntVector(-2,-2,-1),double(Pitch)/1000.*8,1,C,Cell));
        TestFalse(TEXT("larger than supported hierarchy refused"),A->Sample(FIntVector::ZeroValue,double(Pitch)/1000.*9,1,C,Cell));
        for(uint8 Q=0;Q<4;++Q){
            const auto View=FVoxelAssetAppearance::ForCanonicalYaw(A,Q);
            TestFalse(TEXT("extreme coordinate cannot wrap into source"),View->Sample(FIntVector(MIN_int32,MAX_int32,MIN_int32),double(Pitch)/1000.*8,1,C,Cell));
        }
        auto Air=Bytes;Air[134]=0;Check.Reset();Check.Append(Air.GetData(),96);Check.Append(Air.GetData()+128,Air.Num()-128);check(SHA256(Check.GetData(),Check.Num(),Air.GetData()+96));
        TestFalse(TEXT("generic air record still refused"),FVoxelAssetAppearance::Parse(Air,FString::ChrN(32,'2'),Error).IsValid());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCoarseAppearanceGpuTest,"Voxel.Appearance.CoarseSparseGpu",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelCoarseAppearanceGpuTest::RunTest(const FString&) {
    FString Error;auto Source=FVoxelAssetAppearance::Parse(SparseFixture(),FString::ChrN(32,'2'),Error);
    if(!TestTrue(TEXT("coarse source parsed"),Source.IsValid()))return false;
    auto Sparse=Source->BuildSparseResource(Error);if(!Sparse)return false;
    std::vector<uint8_t> Bytes;auto Put=[&](uint32 V){for(int I=0;I<4;++I)Bytes.push_back(uint8(V>>(I*8)));};
    Put(vxc::kVxaMagic);Put(vxc::kVxaVersion);Put(uint32(-9));Put(uint32(-13));Put(uint32(-5));Put(19);Put(18);Put(17);Put(100);Put(19*18*17);Put(0);Put(0);
    uint32 Count=0;for(int X=0;X<19;++X)for(int Y=0;Y<18;++Y)for(int Z=0;Z<17;++Z){Bytes.push_back((X+Y*3+Z*5)%7==0?uint8(16+(Count++)%3):0);Put(1);}
    vxc::AssetGrid Grid;if(!TestTrue(TEXT("coarse matching bank"),Grid.parse(Bytes)==vxc::AssetParseError::kOk))return false;
    TArray<FUintVector2> Ranges;Ranges.SetNumZeroed(2);Ranges[1]=FUintVector2(0,uint32(Sparse->Words.Num()));
    for(uint8 Level=1;Level<=7;++Level)for(uint8 Yaw=0;Yaw<4;++Yaw){
        const int64 Scale=int64(1)<<Level;int64 RX=-9,RY=-13;
        if(Yaw==1){RX=13;RY=-9;}else if(Yaw==2){RX=9;RY=13;}else if(Yaw==3){RX=-13;RY=9;}
        vxc::AssetField::ResolvedAssetInstance A;A.grid=&Grid;A.yawQuarter=Yaw;A.anchorVx=-3*Scale+Scale/2-RX;A.anchorVy=-4*Scale+Scale/2-RY;A.anchorVz=-5*Scale+Scale/2+5;
        std::vector<vxc::AssetField::ResolvedAssetInstance> Ordered{A};std::vector<vxc::AssetAppearanceCell> Cells;std::vector<uint32_t> Words;
        if(!TestTrue(TEXT("coarse page prepared"),vxc::assetAppearancePage(-1,-1,-1,Ordered,{1},[](auto,auto,auto){return vxc::MAT_AIR;},[](auto,auto,auto){return false;},Cells,Level)))return false;
        if(!TestTrue(TEXT("coarse winners exercised"),!Cells.empty()))return false;
        if(!vxc::assetPackAppearancePage(-1,-1,-1,1,Ordered,Cells,Words,Level))return false;
        TArray<uint32> Page;Page.Append(Words.data(),int32(Words.size()));TArray<FVoxelSparseAppearanceProbeQuery> Queries;
        for(const auto& C:Cells)Queries.Add({uint32(C.cell%32),uint32((C.cell/32)%32),uint32(C.cell/1024),uint32(C.material)});
        Queries.Add({32,0,0,16});TArray<FVoxelSparseAppearanceProbeResult> Results;
        if(!VoxelRunSparseAppearanceProbe(Page,Sparse->Words,Ranges,Queries,Results,Error)){AddError(Error);return false;}
        auto View=FVoxelAssetAppearance::ForCanonicalYaw(Source,Yaw);
        for(int I=0;I<int(Cells.size());++I){const auto& Q=Queries[I];const auto& R=Results[I];
            FColor C;FIntVector Zero;const FIntVector Relative(int32((int64(Q.X)-32)*Scale+Scale/2-A.anchorVx),int32((int64(Q.Y)-32)*Scale+Scale/2-A.anchorVy),int32((int64(Q.Z)-32)*Scale+Scale/2-A.anchorVz));
            if(!TestTrue(TEXT("independent representative sample"),View->Sample(Relative,100,uint8(Q.Material),C,Zero)))return false;
            const uint32 Packed=Q.Material|(uint32(C.R)<<8)|(uint32(C.G)<<16)|(uint32(C.B)<<24);
            if(R.Hit!=1||R.Resource!=1||R.Yaw!=Yaw||R.PackedMaterialRGB!=Packed||R.ZeroX!=uint32(Zero.X)||R.ZeroY!=uint32(Zero.Y)||R.ZeroZ!=uint32(Zero.Z)){AddError(FString::Printf(TEXT("coarse GPU mismatch level%u yaw%u query%d"),uint32(Level),uint32(Yaw),I));return false;}
        }
        TestEqual(TEXT("coarse outside-page query refused"),Results.Last().Hit,0u);
    }
    return true;
}
#endif

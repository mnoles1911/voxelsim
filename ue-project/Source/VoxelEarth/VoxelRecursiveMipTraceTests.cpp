#include "VoxelRecursiveMipTestHook.h"
#include "VoxelGpuWorldGen.h"
#include "voxelcore/mips.h"
#include <unordered_map>
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRecursiveMipTraceTest,"Voxel.Appearance.RecursiveMipTrace",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelRecursiveMipTraceTest::RunTest(const FString&){
    const bool Surface=VoxelGpuWorldGen::SurfaceMipEnabled();
    using Brick=vxc::Brick<8>;
    std::unordered_map<vxc::BrickKey,Brick,vxc::BrickKeyHash> Bricks;
    auto Fine=[](int64 X,int64 Y,int64 Z){return ((X+Y+Z)&1)?vxc::MaterialId(19):vxc::MaterialId(16);};
    auto Source=[&](const vxc::BrickKey& K)->const Brick*{
        auto [It,New]=Bricks.try_emplace(K);if(New)for(int Z=0;Z<8;++Z)for(int Y=0;Y<8;++Y)for(int X=0;X<8;++X)It->second.set(X,Y,Z,Fine(int64(K.x)*8+X,int64(K.y)*8+Y,int64(K.z)*8+Z));return &It->second;
    };
    // Independent scalar recursive oracle: explicit majority/tie and highest
    // layer priority. It never calls reduceMipCell or downsampleBricks.
    struct Witness{int64 X,Y,Z;vxc::MaterialId M;};
    std::function<Witness(int,int64,int64,int64)> Ref=[&](int L,int64 X,int64 Y,int64 Z)->Witness{
        if(!L)return {X,Y,Z,Fine(X,Y,Z)};
        Witness C[8];int Counts[256]={};for(int I=0;I<8;++I){C[I]=Ref(L-1,X*2+(I&1),Y*2+((I>>1)&1),Z*2+(I>>2));++Counts[C[I].M];}
        int Pick=0;if(Surface){Pick=4;}else{int Winner=1;for(int M=2;M<256;++M)if(Counts[M]>Counts[Winner])Winner=M;while(C[Pick].M!=Winner)++Pick;}
        return C[Pick];
    };
    for(int L=1;L<=2;++L)for(bool Shared:{false,true})for(bool Warm:{false,true}){
        const auto E=Ref(L,-3,-5,-7);auto R=VoxelTestRecursiveMipTrace(Source,{},L,-3,-5,-7,Shared,Warm);
        TestTrue(TEXT("actual cold/warm builder trace valid"),R.Valid);TestEqual(TEXT("independent recursive material"),int(R.Material),int(E.M));
        TestTrue(TEXT("exact independent finest witness"),R.X==E.X&&R.Y==E.Y&&R.Z==E.Z);
    }
    Brick Solid(vxc::MaterialId(16));int Reads=0;
    auto Uniform=[&](const vxc::BrickKey&)->const Brick*{++Reads;return &Solid;};
    for(int L=1;L<=7;++L){
        TArray<FVoxelRecursiveMipSeed> Seeds;int64 X=-3,Y=-5,Z=-7;
        for(int K=L;K>0;--K){FVoxelRecursiveMipSeed S;S.Level=K;S.Key={int32(vxc::floorDiv(X,int64(8))),int32(vxc::floorDiv(Y,int64(8))),int32(vxc::floorDiv(Z,int64(8)))};S.Brick=Solid;Seeds.Add(S);X*=2;Y*=2;Z=Z*2+(Surface?1:0);}
        Reads=0;auto R=VoxelTestRecursiveMipTrace(Uniform,Seeds,L,-3,-5,-7,true);
        TestTrue(TEXT("all levels use seeded shared hits"),R.Valid&&R.X==X&&R.Y==Y&&R.Z==Z&&R.Material==16);
        TestTrue(TEXT("trace descends only selected cached path"),Reads<=3);
        Seeds[0].Brick=Brick(vxc::MaterialId(19));
        TestFalse(TEXT("corrupt cached parent cannot invent source"),VoxelTestRecursiveMipTrace(Uniform,Seeds,L,-3,-5,-7,true).Valid);
    }
    TestFalse(TEXT("advanced edit epoch refuses stale provenance"),VoxelTestRecursiveMipTrace(Uniform,{},1,-1,-1,-1,false,false,true).Valid);
    TestFalse(TEXT("unsupported level refused"),VoxelTestRecursiveMipTrace(Uniform,{},8,0,0,0,false).Valid);
    TestFalse(TEXT("coordinate overflow refused before recursion"),VoxelTestRecursiveMipTrace(Uniform,{},7,MAX_int64,0,0,false).Valid);
    auto Air=[](const vxc::BrickKey&)->const Brick*{return nullptr;};
    auto Empty=VoxelTestRecursiveMipTrace(Air,{},2,-1,-1,-1,false);
    TestTrue(TEXT("air has no fabricated contributor"),Empty.Valid&&Empty.Material==vxc::MAT_AIR);
    auto FineResult=VoxelTestRecursiveMipTrace(Uniform,{},0,-11,-12,-13,false);
    TestTrue(TEXT("level zero preserves actual cell"),FineResult.Valid&&FineResult.X==-11&&FineResult.Y==-12&&FineResult.Z==-13);
    return true;
}
#endif

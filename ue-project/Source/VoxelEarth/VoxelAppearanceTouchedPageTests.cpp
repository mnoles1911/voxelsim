#include "VoxelAppearanceTouchedPage.h"
#include "VoxelCoords.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAppearanceTouchedPageTest,"Voxel.Appearance.TouchedPage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVoxelAppearanceTouchedPageTest::RunTest(const FString&){
    for(uint32 Level=0;Level<=7;++Level){
        vxc::SyntheticTileSampler Tiles{1234};vxc::World<8> World(1234,Tiles);const FIntVector Key(-1,-2,-1);const int64 Scale=int64(1)<<Level,Half=Level?Scale/2:0;
        auto Point=[&](int64 X,int64 Y,int64 Z){return VoxelCoords::FVoxelCoord{(int64(Key.X)*32+X)*Scale+Half,(int64(Key.Y)*32+Y)*Scale+Half,(int64(Key.Z)*32+Z)*Scale+Half};};
        const auto A=Point(5,6,7),B=Point(8,6,7),C=Point(13,6,7),Outside=Point(-1,6,7);
        World.setVoxel(A.X,A.Y,A.Z,vxc::MAT_ROCK);
        World.setCraftCell(World.craftCellOfVoxelMin(B.X)+7,World.craftCellOfVoxelMin(B.Y)+7,World.craftCellOfVoxelMin(B.Z)+7,vxc::MAT_SAND);
        World.setVoxel(Outside.X,Outside.Y,Outside.Z,vxc::MAT_ROCK);
        if(Level>0)World.setVoxel(C.X+1,C.Y,C.Z,vxc::MAT_ROCK);
        FVoxelAppearanceTouchedPage Touched;
        if(!TestTrue(TEXT("negative page builds at every level"),Touched.Build(World,Key,Level)))return false;
        TestTrue(TEXT("terrain representative write marked"),Touched.IsTouched(A.X,A.Y,A.Z));
        TestTrue(TEXT("actual12.5mm craft representative marked"),Touched.IsTouched(B.X,B.Y,B.Z));
        TestFalse(TEXT("untouched fine neighbor in same edited brick remains unmarked"),Touched.IsTouched(A.X+1,A.Y,A.Z));
        TestFalse(TEXT("outside-page representative excluded"),Touched.IsTouched(Outside.X,Outside.Y,Outside.Z));
        TestFalse(TEXT("nonrepresentative write never blacks out a coarse cell"),Touched.IsTouched(C.X,C.Y,C.Z));
        uint32 Count=0;for(uint32 W:Touched.Words)while(W){W&=W-1;++Count;}
        TestEqual(TEXT("only exact terrain/craft representatives were marked"),Count,2u);
        // Independent complete page scan, comparing to explicit actual writes
        // rather than the helper's bitmap addressing or coarse modulo formulas.
        int Bad=0;
        for(int Z=0;Z<32;++Z)for(int Y=0;Y<32;++Y)for(int X=0;X<32;++X){const auto P=Point(X,Y,Z);const bool Expected=(X==5&&Y==6&&Z==7)||(X==8&&Y==6&&Z==7);Bad+=Touched.IsTouched(P.X,P.Y,P.Z)!=Expected;}
        TestEqual(TEXT("every representative matches explicit write provenance"),Bad,0);
        const int64 EndX=(int64(Key.X)+1)*32*Scale-1,EndY=(int64(Key.Y)+1)*32*Scale-1,EndZ=(int64(Key.Z)+1)*32*Scale-1;
        World.setCraftCell(World.craftCellOfVoxelMin(EndX)+7,World.craftCellOfVoxelMin(EndY)+7,World.craftCellOfVoxelMin(EndZ)+7,vxc::MAT_SAND);
        TestTrue(TEXT("recursive finest provenance builds"),Touched.Build(World,Key,Level,true));
        TestTrue(TEXT("recursive selected terrain child marked"),Touched.IsTouched(A.X,A.Y,A.Z));
        TestTrue(TEXT("recursive selected craft child marked"),Touched.IsTouched(B.X,B.Y,B.Z));
        TestTrue(TEXT("full-volume last fine child included"),Touched.IsTouched(EndX,EndY,EndZ));
        if(Level>0)TestTrue(TEXT("nonrepresentative edited child retained for recursive trace"),Touched.IsTouched(C.X+1,C.Y,C.Z));
        TestFalse(TEXT("recursive untouched neighbor keeps source appearance"),Touched.IsTouched(A.X+1,A.Y,A.Z));
        TestFalse(TEXT("recursive volume excludes outside edit"),Touched.IsTouched(Outside.X,Outside.Y,Outside.Z));
        TestEqual(TEXT("recursive provenance does not allocate dense volume"),Touched.Words.Num(),0);
        TestFalse(TEXT("level8 refused"),Touched.Build(World,Key,8));TestEqual(TEXT("failed build drops stale bits"),Touched.Words.Num(),0);TestFalse(TEXT("failed page cannot return previous touched state"),Touched.IsTouched(A.X,A.Y,A.Z));
    }
    return true;
}
#endif

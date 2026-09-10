#pragma once
#include "CoreMinimal.h"
#include "voxelcore/world.h"
#include <array>
#include <set>

// Exact write provenance at the SAME representative voxels used by geometry.
// Edited-brick occupancy alone would wrongly remove neighboring source colors.
struct FVoxelAppearanceTouchedPage {
    int64 X0=0,Y0=0,Z0=0,Scale=1;
    TArray<uint32> Words;
    bool AllFinest=false;
    std::set<std::array<int64,3>> Finest;
    bool Build(const vxc::World<8>& World,FIntVector Key,uint32 Level,bool InAllFinest=false){
        AllFinest=InAllFinest;Finest.clear();Words.Reset();if(Level>7)return false;
        Scale=int64(1)<<Level;const int64 Half=Level&&!AllFinest?Scale/2:0;
        X0=int64(Key.X)*32*Scale+Half;Y0=int64(Key.Y)*32*Scale+Half;Z0=int64(Key.Z)*32*Scale+Half;
        if(!AllFinest)Words.SetNumZeroed(1024);
        auto Mark=[&](int64 X,int64 Y,int64 Z){
            X-=X0;Y-=Y0;Z-=Z0;
            if(AllFinest){if(X>=0&&Y>=0&&Z>=0&&X<32*Scale&&Y<32*Scale&&Z<32*Scale)Finest.insert({X,Y,Z});return;}
            if(X<0||Y<0||Z<0||X>=32*Scale||Y>=32*Scale||Z>=32*Scale||X%Scale||Y%Scale||Z%Scale)return;
            const uint32 C=uint32(X/Scale+32*(Y/Scale+32*(Z/Scale)));Words[C/32]|=1u<<(C%32);
        };
        auto Scan=[&](const vxc::EditLog& Log,bool Craft){
            for(const auto& E:Log.entries()){
                const int64 EX=int64(E.key.x)*8,EY=int64(E.key.y)*8,EZ=int64(E.key.z)*8;
                const int64 LX=Craft?World.voxelOfCraftCell(EX):EX,LY=Craft?World.voxelOfCraftCell(EY):EY,LZ=Craft?World.voxelOfCraftCell(EZ):EZ;
                const int64 HX=Craft?World.voxelOfCraftCell(EX+7):EX+7,HY=Craft?World.voxelOfCraftCell(EY+7):EY+7,HZ=Craft?World.voxelOfCraftCell(EZ+7):EZ+7;
                if(HX<X0||HY<Y0||HZ<Z0||LX>X0+(AllFinest?32*Scale-1:31*Scale)||LY>Y0+(AllFinest?32*Scale-1:31*Scale)||LZ>Z0+(AllFinest?32*Scale-1:31*Scale))continue;
                for(const auto& C:E.cells){
                    int64 X=EX+C.cell%8,Y=EY+(C.cell/8)%8,Z=EZ+C.cell/64;
                    if(Craft){X=World.voxelOfCraftCell(X);Y=World.voxelOfCraftCell(Y);Z=World.voxelOfCraftCell(Z);}Mark(X,Y,Z);
                }
            }
        };
        Scan(World.log(),false);Scan(World.craftLog(),true);return true;
    }
    bool IsTouched(int64 X,int64 Y,int64 Z)const{
        X-=X0;Y-=Y0;Z-=Z0;
        if(AllFinest)return Finest.find({X,Y,Z})!=Finest.end();
        if(Words.Num()!=1024||X<0||Y<0||Z<0||X>=32*Scale||Y>=32*Scale||Z>=32*Scale||X%Scale||Y%Scale||Z%Scale)return false;
        const uint32 C=uint32(X/Scale+32*(Y/Scale+32*(Z/Scale)));return (Words[C/32]&(1u<<(C%32)))!=0;
    }
};

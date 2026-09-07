#pragma once
#include "CoreMinimal.h"
#include "voxelcore/sparseassetgrid.h"
#include "voxelcore/hash.h"
#include "voxelcore/core.h"

// Shared authoritative environment storage. UE builds use explicit admission
// failures; allocator exhaustion remains UE's fatal OOM policy (no exceptions).
struct FVoxelEnvironmentSparseGrid {
    static constexpr int32 MaxChunks=131072;
    static constexpr int32 MaxAxis=16384;
    FIntVector Size=FIntVector::ZeroValue,Origin=FIntVector::ZeroValue;
    double Mm=100;
    int32 MaxDataZ=MAX_int32;
    vxc::SparseAssetGrid Data{{1,1,1,0,0,0,100000},{MaxChunks,4096}};
    bool Init(){
        if(Size.GetMin()<=0||Size.GetMax()>MaxAxis||!FMath::IsFinite(Mm)||Mm<=0)return false;
        Data=vxc::SparseAssetGrid({Size.X,Size.Y,Size.Z,Origin.X,Origin.Y,Origin.Z,uint32(FMath::RoundToInt(Mm*1000))},{MaxChunks,4096});return Data.valid();
    }
    int32 sizeX()const{return Size.X;}int32 sizeY()const{return Size.Y;}int32 sizeZ()const{return Size.Z;}
    int32 originX()const{return Origin.X;}int32 originY()const{return Origin.Y;}int32 originZ()const{return Origin.Z;}
    double voxelSizeMm()const{return Mm;}
    uint8 At(int32 X,int32 Y,int32 Z)const{return Z>=MaxDataZ?0:Data.at(X,Y,Z);}
    uint8 LocalAt(int64 X,int64 Y,int64 Z)const{
        if(X<Origin.X||Y<Origin.Y||Z<Origin.Z||X>=int64(Origin.X)+Size.X||Y>=int64(Origin.Y)+Size.Y||Z>=int64(Origin.Z)+Size.Z)return 0;
        return At(int32(X-Origin.X),int32(Y-Origin.Y),int32(Z-Origin.Z));
    }
    bool Set(int32 X,int32 Y,int32 Z,uint8 M){if(M>=vxc::kMaterialCount)return false;const vxc::SparseAssetGrid::Edit E{X,Y,Z,M};return Data.apply({&E,1})==vxc::SparseAssetGrid::Result::Ok;}
    bool SetRun(int32 X,int32 Y,int32 Z,int32 Len,uint8 M){if(M>=vxc::kMaterialCount)return false;const vxc::SparseAssetGrid::Run R{X,Y,Z,Len,M};return Data.applyRuns({&R,1})==vxc::SparseAssetGrid::Result::Ok;}
    template<class F>void Visit(F&& Fn)const{Data.visitOccupiedCells([&](auto E){if(E.z<MaxDataZ)Fn(E.x,E.y,E.z,E.material);});}
    template<class F>void VisitBox(const FIntVector& Lo,const FIntVector& Hi,F&& Fn)const{
        if(int64(FMath::Max(0,Hi.X-Lo.X)/8+1)*(FMath::Max(0,Hi.Y-Lo.Y)/8+1)>int64(Data.chunkCount())){
            Visit([&](int X,int Y,int Z,uint8 M){if(X>=Lo.X&&Y>=Lo.Y&&Z>=Lo.Z&&X<Hi.X&&Y<Hi.Y&&Z<Hi.Z)Fn(X,Y,Z,M);});return;
        }
        Data.visitOccupiedBox(Lo.X,Lo.Y,Lo.Z,Hi.X,Hi.Y,FMath::Min(Hi.Z,MaxDataZ),[&](auto E){Fn(E.x,E.y,E.z,E.material);});
    }
    template<class F>void columnRuns(int32 X,int32 Y,F&& Fn)const{
        Data.visitColumnRuns(X,Y,[&](auto R){if(R.z<MaxDataZ)Fn(R.z,FMath::Min(R.length,MaxDataZ-R.z),R.material);});
    }
    TArray<FIntVector> SectionKeys(int32 Edge)const{
        TSet<FIntVector> Keys;
        Data.visitChunks([&](int32 X,int32 Y,int32 Z,auto){if(Z*8<MaxDataZ)Keys.Add(FIntVector(X*8/Edge,Y*8/Edge,Z*8/Edge));});
        auto Result=Keys.Array();Result.Sort([](const auto& A,const auto& B){return A.X!=B.X?A.X<B.X:A.Y!=B.Y?A.Y<B.Y:A.Z<B.Z;});return Result;
    }
    bool Equals(const FVoxelEnvironmentSparseGrid& Other)const{
        bool Equal=Size==Other.Size&&Origin==Other.Origin&&Mm==Other.Mm;
        Visit([&](int X,int Y,int Z,uint8 M){Equal&=Other.At(X,Y,Z)==M;});
        Other.Visit([&](int X,int Y,int Z,uint8 M){Equal&=At(X,Y,Z)==M;});return Equal;
    }
    // New body: int32(-1), uint32 schema(1), int32 chunk count, then ordered
    // int32 x/y/z chunk keys and 512 material bytes (x+8*y+64*z).
    // Legacy body begins with int32 dense byte count, order (x*Y+y)*Z+z.
    // Decode is transactional and rejects duplicate/unsorted/out-of-bounds data.
    bool Serialize(FArchive& Ar){
        int32 Marker=-1;Ar<<Marker;if(Ar.IsError())return false;
        if(Ar.IsSaving()){
            uint32 Schema=1;int32 Count=int32(Data.chunkCount());Ar<<Schema<<Count;
            Data.visitChunks([&](int32 X,int32 Y,int32 Z,auto Bytes){Ar<<X<<Y<<Z;Ar.Serialize(const_cast<uint8*>(Bytes.data()),512);});
            return !Ar.IsError();
        }
        FVoxelEnvironmentSparseGrid Temp;Temp.Size=Size;Temp.Origin=Origin;Temp.Mm=Mm;Temp.MaxDataZ=MaxDataZ;if(!Temp.Init())return false;
        if(Marker>=0){
            const int64 Cells=int64(Size.X)*Size.Y*Size.Z;
            if(Cells>64*1024*1024||Marker!=Cells||Marker>Ar.TotalSize()-Ar.Tell())return false;
            uint8 Buffer[512];int64 Offset=0;
            while(Offset<Cells){const int32 N=int32(FMath::Min<int64>(512,Cells-Offset));Ar.Serialize(Buffer,N);if(Ar.IsError())return false;
                // Group each bounded buffer into vertical material runs.
                for(int32 I=0;I<N;){const int64 Index=Offset+I;const int32 Z=int32(Index%Size.Z),Y=int32(Index/Size.Z%Size.Y),X=int32(Index/Size.Z/Size.Y);
                    int32 Len=1;while(I+Len<N&&Z+Len<Size.Z&&Buffer[I+Len]==Buffer[I])++Len;
                    if(Buffer[I]>=vxc::kMaterialCount)return false;
                    if(Buffer[I]&&!Temp.SetRun(X,Y,Z,Len,Buffer[I]))return false;I+=Len;
                }Offset+=N;
            }
        }else{
            uint32 Schema=0;int32 Count=0;Ar<<Schema<<Count;
            if(Marker!=-1||Schema!=1||Count<0||Count>MaxChunks||int64(Count)*524>Ar.TotalSize()-Ar.Tell())return false;
            FIntVector Last(-1,-1,-1);
            for(int32 I=0;I<Count;++I){FIntVector Key;Ar<<Key.X<<Key.Y<<Key.Z;uint8 Bytes[512];Ar.Serialize(Bytes,512);if(Ar.IsError())return false;
                if(Key.GetMin()<0||int64(Key.X)*8>=Size.X||int64(Key.Y)*8>=Size.Y||int64(Key.Z)*8>=Size.Z)return false;
                if(I&&!(Last.X<Key.X||(Last.X==Key.X&&(Last.Y<Key.Y||(Last.Y==Key.Y&&Last.Z<Key.Z)))))return false;Last=Key;
                vxc::SparseAssetGrid::Edit Edits[512];size_t N=0;
                for(int32 Z=0;Z<8;++Z)for(int32 Y=0;Y<8;++Y)for(int32 X=0;X<8;++X)if(const uint8 M=Bytes[X+8*Y+64*Z]){
                    if(M>=vxc::kMaterialCount)return false;
                    const FIntVector P=Key*8+FIntVector(X,Y,Z);if(P.X>=Size.X||P.Y>=Size.Y||P.Z>=Size.Z)return false;
                    Edits[N++]={P.X,P.Y,P.Z,M};
                }
                if(!N||Temp.Data.apply({Edits,N})!=vxc::SparseAssetGrid::Result::Ok)return false;
            }
        }
        Temp.Data.clearAbove(MaxDataZ);Data=MoveTemp(Temp.Data);return !Ar.IsError();
    }
};

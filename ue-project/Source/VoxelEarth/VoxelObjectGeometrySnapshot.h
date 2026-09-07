#pragma once
#include "CoreMinimal.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

// The pointer is handed off on the game thread; only its immutable allocation
// may be accessed by save/network workers. Actor cache members remain GT-only.
using FVoxelImmutableGeometry = TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>;
namespace VoxelObjectGeometrySnapshot {
constexpr uint32 FormatVersion=1;
constexpr int32 MaxBytes=512*1024*1024;
inline void WriteVersion(FArchive& Ar){uint32 Version=FormatVersion;Ar<<Version;}
inline bool Combine(const TArray<uint8>& Geometry,const TArray<uint8>& Dynamic,TArray<uint8>& Legacy){
    if(Geometry.Num()<4||Dynamic.Num()<4||Dynamic.Num()>4096||int64(Geometry.Num())+Dynamic.Num()>MaxBytes)return false;
    FMemoryReader G(Geometry),D(Dynamic);uint32 GV=0,DV=0;G<<GV;D<<DV;
    if(G.IsError()||D.IsError()||GV!=FormatVersion||DV!=FormatVersion)return false;
    Legacy.Reset(Geometry.Num()+Dynamic.Num()-8);
    Legacy.Append(Dynamic.GetData()+4,Dynamic.Num()-4);
    Legacy.Append(Geometry.GetData()+4,Geometry.Num()-4);
    return true;
}
}

#pragma once
#include "CoreMinimal.h"
#include "VoxelSparseAppearance.h"

// Immutable presentation data, bound to the unedited source VXA. Never supplies
// occupancy, collision, material IDs, world placement or saved edit identity.
class FVoxelAssetAppearance
{
public:
    static TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Load(const FString& SourceMD5);
    static TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Parse(TArray<uint8> Bytes,const FString& SourceMD5,FString& Error);
    // Presentation view for geometry whose canonical quarter yaw is already
    // baked into its cells. Shares the original packet without copying it.
    static TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> ForCanonicalYaw(TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Source,uint8 Yaw);
    bool Sample(const FIntVector& AssetCell,double GridMm,uint8 Material,FColor& Color,FIntVector& SourceCell) const;
    static FLinearColor FaceColor(FColor Base,const FIntVector& SourceCell,int32 Axis,bool Positive);
    FLinearColor ColorForFace(FColor Base,const FIntVector& SourceCell,int32 Axis,bool Positive) const;
    FVector2f FaceUV(const FVector3f& LocalPositionUU,int32 Axis) const;
    // Cached derived resource for ORIGINAL verified packets only. Budget covers
    // final output and peak sort-index + output bytes (input VAC1 excluded).
    TSharedPtr<const FVoxelSparseAppearance,ESPMode::ThreadSafe> BuildSparseResource(FString& Error,
        uint64 MaxOutputBytes=64ull*1024*1024,uint64 MaxWorkingBytes=128ull*1024*1024) const;
    bool IsNeedle() const {return Needle;}
    bool UsesFoliageMask() const {return MaskEnabled;}
    int32 PointCount() const {return Count;}
    double PitchMm() const {return Mm;}
private:
    mutable FCriticalSection SparseMutex;
    mutable TSharedPtr<const FVoxelSparseAppearance,ESPMode::ThreadSafe> Sparse;
    const uint8* Find(const FIntVector& Cell) const;
    TArray<uint8> Data;
    FIntVector Size=FIntVector::ZeroValue,Origin=FIntVector::ZeroValue;
    double Mm=0;
    int32 Count=0;
    bool Needle=false;
    bool MaskEnabled=true;
    uint32 PacketVersion=1;
    uint8 CanonicalYaw=0;
    TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Original;
};

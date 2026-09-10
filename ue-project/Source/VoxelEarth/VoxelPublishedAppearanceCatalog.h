#pragma once
#include "CoreMinimal.h"
#include "VoxelAssetAppearance.h"

// Immutable world-session snapshot. IDs are local to this snapshot; retain it
// with every page using them. Only published inventory rows authorize sources.
class FVoxelPublishedAppearanceCatalog {
public:
    struct FSource {
        FString GeometryMD5;
        TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Appearance;
        TSharedPtr<const FVoxelSparseAppearance,ESPMode::ThreadSafe> Sparse;
        FString GeometrySHA256,AppearanceSHA256;
    };
    static TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Load(
        // Combined verified packets + sparse lookup allocation bound. The full
        // temperate three-form fixture needs ~276 MiB; retain a finite ceiling.
        const FString& AssetDirectory,FString& Error,uint64 MaxSourceBytes=384ull*1024*1024,
        uint64 MaxWorkingBytes=512ull*1024*1024);
    // Named FindResourceId, not FindResource: winbase.h defines FindResource as a
    // macro, and a unity neighbour that includes Windows headers (VoxelPlayerRecords.cpp)
    // renamed the definition to FindResourceW while every other blob still called
    // FindResource -- an unresolved external at link, 2026-09-10.
    uint32 FindResourceId(const FString& GeometryMD5) const;
    const TArray<FSource>& Sources() const {return Entries;}
    uint64 ResourceBytes() const {return Bytes;}
private:
    TArray<FSource> Entries; // Entry zero deliberately empty.
    TMap<FString,uint32> ByHash;
    uint64 Bytes=0;
};

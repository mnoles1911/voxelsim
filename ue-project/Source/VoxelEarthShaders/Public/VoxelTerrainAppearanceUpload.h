#pragma once
#include "CoreMinimal.h"

// Immutable module-independent upload data. Resource ID zero is reserved;
// ranges are (base word, word count) in SourceWords, indexed by local ID.
// The pool must bind page and source ranges from the SAME snapshot. Neither
// raw pointers nor these local IDs may be persisted across world sessions.
struct FVoxelTerrainAppearanceSources {
    TArray<uint32> SourceWords;
    TArray<FUintVector2> SourceRanges;
};
struct FVoxelTerrainAppearanceUpload {
    TArray<uint32> PageWords;
    FIntVector PageKey=FIntVector::ZeroValue;
    uint32 Level=0;
    uint64 Generation=0;
    TSharedPtr<const FVoxelTerrainAppearanceSources,ESPMode::ThreadSafe> Sources;
};

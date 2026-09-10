#pragma once

#include "CoreMinimal.h"

namespace VoxelAssetColumns
{
// Source and destination are separate payload arrays. Allocate the appended
// range once so rebasing columns does not check capacity for every element.
inline void AppendRebased(TArray<uint32>& Destination, const TArray<uint32>& Source, uint32 SpanOffset)
{
    check(&Destination != &Source);
    const int32 Count = Source.Num();
    if (Count == 0) return;
    const int32 Base = Destination.AddUninitialized(Count);
    uint32* Out = Destination.GetData() + Base;
    const uint32* In = Source.GetData();
    for (int32 I = 0; I < Count; ++I)
    {
        Out[I] = In[I] + SpanOffset;
    }
}
}

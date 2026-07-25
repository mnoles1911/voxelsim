#include "VoxelGpuGeometryPool.h"

void FVoxelGpuGeometryPool::Init(uint32 InCapacityQuads)
{
	CapacityQuads = InCapacityQuads;
	Reset();
}

void FVoxelGpuGeometryPool::Reset()
{
	FreeRuns.Reset();
	if (CapacityQuads > 0)
	{
		FreeRuns.Add(FRun{ 0, CapacityQuads });
	}
	UsedQuads = 0;
	HighWaterMark = 0;
}

FVoxelGpuPoolAllocation FVoxelGpuGeometryPool::Alloc(uint32 NumQuads)
{
	FVoxelGpuPoolAllocation Result;

	// A zero-quad chunk is a real and common case — a fully buried or fully
	// empty chunk meshes to nothing. It gets a valid allocation of size zero
	// rather than a failure, so callers do not need a special path for it.
	if (NumQuads == 0)
	{
		Result.Offset = 0;
		Result.NumQuads = 0;
		return Result;
	}

	// First fit. Deliberately not best-fit: with a coalesced free list,
	// first-fit's extra fragmentation is small, and the traversal order stays
	// trivially predictable, which matters when debugging a corrupted pool.
	for (int32 Index = 0; Index < FreeRuns.Num(); ++Index)
	{
		FRun& Run = FreeRuns[Index];
		if (Run.Size < NumQuads)
		{
			continue;
		}

		Result.Offset = Run.Offset;
		Result.NumQuads = NumQuads;

		if (Run.Size == NumQuads)
		{
			FreeRuns.RemoveAt(Index, EAllowShrinking::No);
		}
		else
		{
			Run.Offset += NumQuads;
			Run.Size -= NumQuads;
		}

		UsedQuads += NumQuads;
		HighWaterMark = FMath::Max(HighWaterMark, Result.Offset + NumQuads);
		return Result;
	}

	// Out of contiguous space. Note this is NOT necessarily out of space —
	// GetFreeQuads() can still be large. The caller decides what to do
	// (compact, evict, or refuse); silently succeeding is not an option.
	return Result;
}

void FVoxelGpuGeometryPool::Free(const FVoxelGpuPoolAllocation& Allocation)
{
	if (!Allocation.IsValid() || Allocation.NumQuads == 0)
	{
		return;
	}

	const uint32 Offset = Allocation.Offset;
	const uint32 Size = Allocation.NumQuads;

	checkf(uint64(Offset) + uint64(Size) <= uint64(CapacityQuads),
	       TEXT("Freeing [%u, %u) which runs past the pool capacity %u"),
	       Offset, Offset + Size, CapacityQuads);

	// Find where this run belongs in the offset-sorted list.
	int32 InsertAt = 0;
	while (InsertAt < FreeRuns.Num() && FreeRuns[InsertAt].Offset < Offset)
	{
		++InsertAt;
	}

	// Catch a double free before it corrupts the list. Overlapping either
	// neighbour means this range was already free.
	if (InsertAt > 0)
	{
		checkf(FreeRuns[InsertAt - 1].End() <= Offset,
		       TEXT("Double free or overlapping free at [%u, %u): previous free run [%u, %u) overlaps"),
		       Offset, Offset + Size, FreeRuns[InsertAt - 1].Offset, FreeRuns[InsertAt - 1].End());
	}
	if (InsertAt < FreeRuns.Num())
	{
		checkf(Offset + Size <= FreeRuns[InsertAt].Offset,
		       TEXT("Double free or overlapping free at [%u, %u): next free run [%u, %u) overlaps"),
		       Offset, Offset + Size, FreeRuns[InsertAt].Offset, FreeRuns[InsertAt].End());
	}

	FreeRuns.Insert(FRun{ Offset, Size }, InsertAt);
	UsedQuads -= Size;

	// Coalesce with the next run, then the previous. Doing both keeps the
	// "never adjacent" invariant that first-fit and GetLargestFreeRun rely on.
	if (InsertAt + 1 < FreeRuns.Num() && FreeRuns[InsertAt].End() == FreeRuns[InsertAt + 1].Offset)
	{
		FreeRuns[InsertAt].Size += FreeRuns[InsertAt + 1].Size;
		FreeRuns.RemoveAt(InsertAt + 1, EAllowShrinking::No);
	}
	if (InsertAt > 0 && FreeRuns[InsertAt - 1].End() == FreeRuns[InsertAt].Offset)
	{
		FreeRuns[InsertAt - 1].Size += FreeRuns[InsertAt].Size;
		FreeRuns.RemoveAt(InsertAt, EAllowShrinking::No);
	}
}

uint32 FVoxelGpuGeometryPool::GetLargestFreeRun() const
{
	uint32 Largest = 0;
	for (const FRun& Run : FreeRuns)
	{
		Largest = FMath::Max(Largest, Run.Size);
	}
	return Largest;
}

bool FVoxelGpuGeometryPool::CheckInvariants(FString& OutError) const
{
	uint64 TotalFree = 0;
	uint32 PrevEnd = 0;

	for (int32 Index = 0; Index < FreeRuns.Num(); ++Index)
	{
		const FRun& Run = FreeRuns[Index];

		if (Run.Size == 0)
		{
			OutError = FString::Printf(TEXT("free run %d is empty"), Index);
			return false;
		}
		if (uint64(Run.Offset) + uint64(Run.Size) > uint64(CapacityQuads))
		{
			OutError = FString::Printf(TEXT("free run %d [%u, %u) runs past capacity %u"),
			                           Index, Run.Offset, Run.End(), CapacityQuads);
			return false;
		}
		if (Index > 0)
		{
			if (Run.Offset < PrevEnd)
			{
				OutError = FString::Printf(TEXT("free run %d starts at %u, before the previous run ended at %u"),
				                           Index, Run.Offset, PrevEnd);
				return false;
			}
			if (Run.Offset == PrevEnd)
			{
				OutError = FString::Printf(TEXT("free runs %d and %d are adjacent at %u and were not coalesced"),
				                           Index - 1, Index, Run.Offset);
				return false;
			}
		}

		TotalFree += Run.Size;
		PrevEnd = Run.End();
	}

	if (TotalFree + uint64(UsedQuads) != uint64(CapacityQuads))
	{
		OutError = FString::Printf(TEXT("free (%llu) + used (%u) = %llu, expected capacity %u"),
		                           TotalFree, UsedQuads, TotalFree + uint64(UsedQuads), CapacityQuads);
		return false;
	}

	return true;
}

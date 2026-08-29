// VoxelEofDirtyLedger.cpp -- see VoxelEofDirtyLedger.h for what the +3.64 ms
// EndOfFrameUpdates tail is, the reading rules, and why there is no `other=`.

#include "VoxelEofDirtyLedger.h"

namespace VoxelEofLedger
{
namespace
{

// GAME THREAD ONLY. Every counted call is an engine component API that is
// itself game-thread-only by contract, so these inherit that exclusivity --
// see the header's cost note. Nothing here is synchronised and nothing here may
// be called from a worker.
int64 Counts[int32(ESource::Count)] = {};
int64 Registers = 0;
int64 Unregisters = 0;

// Order and labels must match ESource exactly; the static_assert below is what
// keeps a newly added source from silently printing under its neighbour's name.
const TCHAR* const Labels[] = {
	TEXT("agentISM"),
	TEXT("detail"),
	TEXT("lakeCreate"),
	TEXT("lakeAdopt"),
	TEXT("lakeDestroy"),
	TEXT("clipmap"),
	TEXT("ribbon"),
	TEXT("chunkPublish"),
	TEXT("chunkMtl"),
	TEXT("waterNear"),
	TEXT("waterComp"),
	TEXT("gi"),
	TEXT("sky"),
	TEXT("debris"),
};
static_assert(UE_ARRAY_COUNT(Labels) == int32(ESource::Count),
              "VoxelEofLedger::Labels must have one entry per ESource, in ESource's order");

} // namespace

void Count(ESource Source, int64 N)
{
	Counts[int32(Source)] += N;
}

void CountRegister(int64 N)
{
	Registers += N;
}

void CountUnregister(int64 N)
{
	Unregisters += N;
}

FString FormatAndResetWindow()
{
	FString Line;
	int64 Total = 0;
	for (int32 I = 0; I < int32(ESource::Count); ++I)
	{
		Line += FString::Printf(TEXT("%s=%lld "), Labels[I], (long long)Counts[I]);
		Total += Counts[I];
		Counts[I] = 0;
	}
	// lakeAdopt is inside `total` as a matter of arithmetic but dirties nothing
	// (header, reading note). The suffix repeats that at the point of reading so
	// nobody sums the line into "components dirtied" from the log alone.
	Line += FString::Printf(
		TEXT("| total=%lld | components: reg=%lld unreg=%lld ")
		TEXT("| ENUMERATED LIST -- no other= exists (no global hook); a spiking ")
		TEXT("EndOfFrameUpdates over a flat ledger means an UN-ENUMERATED source, ")
		TEXT("read `Voxel pool publish (window)` pushes= next. lakeAdopt dirties nothing."),
		(long long)Total, (long long)Registers, (long long)Unregisters);
	Registers = 0;
	Unregisters = 0;
	return Line;
}

} // namespace VoxelEofLedger

#include "VoxelSaveRows.h"

#include "VoxelEarthUI.h"
#include "VoxelFrontEndSwitches.h"
#include "VoxelUIStrings.h"

#include "VoxelSaveLibrary.h"

namespace VoxelSaveRowsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// -VoxelDemoSaves. Six rows shaped like the mock's, mixing named saves and
// autosaves so the AUTO tag, the MANUAL/AUTO chips and the search all have
// something to act on, and one row named EXACTLY what the save dialog will
// propose so its overwrite band is reachable in a capture. See the switch's own
// note for why fabricating them is the sanctioned path here.
TArray<FVoxelSaveRowInfo> Demo()
{
	struct FDemo
	{
		const TCHAR* Name;
		const TCHAR* Stamp;
		bool bAuto;
	};
	const FDemo Set[] = {
		{TEXT(""), TEXT("11-04 18:42"), false}, // filled below with the proposed name
		{TEXT("Autosave"), TEXT("11-04 17:08"), true},
		{TEXT("Before the Chapel"), TEXT("11-04 16:15"), false},
		{TEXT("Autosave"), TEXT("11-03 22:51"), true},
		{TEXT("Hollowford cave-in"), TEXT("11-03 19:30"), false},
		{TEXT("Roland Day 1"), TEXT("11-02 14:02"), false},
	};

	TArray<FVoxelSaveRowInfo> Rows;
	int32 Index = 0;
	for (const FDemo& Entry : Set)
	{
		FVoxelSaveRowInfo Row;
		// Day 1 is what a freshly started capture world is on, so this is the
		// name SVoxelPauseMenu::OpenSaveDialog will offer.
		Row.DisplayName = Index == 0
		                      ? VoxelUIStrings::DefaultSaveName(VoxelUIStrings::Title(), 1)
		                      : FText::FromString(Entry.Name);
		Row.Slug = FString::Printf(TEXT("demo-%d"), Index);
		Row.Detail = FText::FromString(FString::Printf(TEXT("%s   X %d  Y 64  Z %d"), Entry.Stamp,
		                                               -1248 + Index * 37, 872 - Index * 51));
		Row.bIsAutosave = Entry.bAuto;
		Rows.Add(MoveTemp(Row));
		++Index;
	}
	return Rows;
}
} // namespace VoxelSaveRowsDetail

namespace VoxelSaveRows
{
TArray<FVoxelSaveRowInfo> Build(uint64 RunningSeed)
{
	if (FVoxelFrontEndSwitches::Get().bDemoSaves)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelSaveRows: -VoxelDemoSaves -- showing fabricated rows."));
		return VoxelSaveRowsDetail::Demo();
	}

	TArray<FVoxelSaveRowInfo> Rows;
	for (const VoxelSave::FSaveInfo& Info : VoxelSave::List())
	{
		FVoxelSaveRowInfo Row;
		Row.Slug = Info.Slug;
		Row.DisplayName = FText::FromString(Info.DisplayName);
		Row.bIsAutosave = Info.bIsAutosave;

		// MainMenu.gd's row reads
		//   "<save_name>\n<timestamp>   X %.0f  Y %.0f  Z %.0f"
		// so the shape is preserved exactly. The UNITS are not -- see the header.
		const FVector Metres = Info.PlayerPosition / 100.0;
		Row.Detail = FText::FromString(FString::Printf(TEXT("%s   X %.0f  Y %.0f  Z %.0f"), *Info.TimestampIso,
		                                               Metres.X, Metres.Y, Metres.Z));

		// Only reachable via -VoxelSeed=, so in ordinary play every row is
		// loadable -- but a row that silently did nothing when clicked would be
		// far worse than one that says why.
		if (RunningSeed != 0 && Info.Seed != RunningSeed)
		{
			Row.bLoadable = false;
			Row.DisabledReason = FText::FromString(
				FString::Printf(TEXT("requires relaunch with -VoxelSeed=%llu"), (unsigned long long)Info.Seed));
		}
		Rows.Add(MoveTemp(Row));
	}
	return Rows;
}
} // namespace VoxelSaveRows

#pragma once
// "Voxelmark Journal.html": the player's dated entries and the game's goals,
// one list on the left and a parchment page on the right.
//
// NO BACKING SYSTEM AT ALL. A case-sensitive grep for Quest, Journal, Objective
// and Goal across both modules on 2026-09-07 returned nothing but the English
// word inside "request". So both halves of this screen draw
// FVoxelJournalData -- seeded from the mock by VoxelScreenData::SeedJournal --
// and the widget cannot tell that from a real one.
//
// THE COMPOSER IS READ-ONLY. The mock's NEW ENTRY card opens a title field and
// a lined textarea and writes the result to localStorage. Building the fields
// is easy; the problem is where the entry goes. This project's save format has
// no journal in it, so a written entry would survive until the player quit and
// then vanish -- which is a worse experience than not offering the pen. The
// composer is therefore drawn as a disabled card that says what it is waiting
// for, and TodayStamp is still computed so the day it becomes writable the
// stamp is already right.
//
// TRACK / UNTRACK IS LIVE, because it is pure view state: which goals a player
// wants in front of them does not need a system to remember it within a
// session, and the flag already lives on FVoxelGoal.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class VOXELEARTHUI_API SVoxelJournalScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelJournalScreen) {}
		SLATE_ARGUMENT(FVoxelJournalData, Data)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelJournalScreen() override;

	static TArray<struct FVoxelScreenAction> Actions();

private:
	TSharedRef<class SWidget> BuildLeftColumn();
	void RebuildList();
	void RebuildPage();

	// journal | goals, and within goals, tracked | untracked.
	bool bGoalsSection = false;
	bool bShowUntracked = false;
	int32 SelectedEntry = 0;
	int32 SelectedGoal = 0;

	// The goals currently listed, as indices into Data.Goals. Rebuilt whenever
	// the tracked filter changes, so the list and the page cannot disagree
	// about what row 3 is.
	TArray<int32> VisibleGoals;

	FVoxelJournalData Data;
	TSharedPtr<class SVerticalBox> ListBox;
	TSharedPtr<class SVerticalBox> PageBox;
	TSharedPtr<class SHorizontalBox> SubTabRow;
};

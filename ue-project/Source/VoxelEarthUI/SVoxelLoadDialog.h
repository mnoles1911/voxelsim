#pragma once
// The LOAD GAME picker: the port of "Voxelmark Load Dialog.html" (2026-09-07).
//
// It REPLACES the look of SVoxelMainMenu::BuildLoadPanel -- the oak sub-panel
// with two lines of text per row -- and serves the pause overlay as well, which
// is why it is a widget of its own rather than another builder on the main
// menu. The two hosts differ only in what CANCEL means.
//
// FVoxelSaveRowInfo LIVES HERE NOW. It used to be declared in SVoxelMainMenu.h,
// which was the only widget that consumed it; the pause overlay makes that two,
// and a shared struct in the header of one of its two consumers is how include
// cycles start.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

DECLARE_DELEGATE_OneParam(FOnVoxelSaveAction, const FString& /*Slug*/);

// One row of the LOAD GAME list. Populated from VoxelSaveLibrary; kept as a
// plain struct so the widget has no dependency on the save system's headers
// and can be screenshot-tested with fabricated rows.
struct FVoxelSaveRowInfo
{
	FString Slug;
	FText DisplayName;
	FText Detail;        // "<timestamp>   X ..  Y ..  Z .."
	bool bLoadable = true;
	FText DisabledReason; // shown in place of Detail when !bLoadable
	// Drives the AUTO tag, the AUTO filter chip, and the dimmer name colour the
	// mock gives an autosave (.ld-name.auto). Carried from
	// VoxelSave::FSaveInfo::bIsAutosave, which the save format has always
	// stored and nothing has ever read.
	bool bIsAutosave = false;
};

// The mock's three chips. ALL is the state the dialog opens in.
enum class EVoxelSaveFilter : uint8
{
	All,
	Manual,
	Auto,
};

class VOXELEARTHUI_API SVoxelLoadDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelLoadDialog) {}
		SLATE_ARGUMENT(TArray<FVoxelSaveRowInfo>, Rows)
		SLATE_EVENT(FOnVoxelSaveAction, OnLoadSave)
		SLATE_EVENT(FOnVoxelSaveAction, OnDeleteSave)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelLoadDialog() override;

	// Rebuilds the list. Called whenever the save set changes underneath the
	// dialog -- a DELETE from one of its own rows, most often.
	void SetRows(TArray<FVoxelSaveRowInfo> InRows);

	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedRef<class SWidget> BuildFilterChip(const FText& Label, EVoxelSaveFilter Filter);
	TSharedRef<class SWidget> BuildRow(const FVoxelSaveRowInfo& Row, bool bIsLatest);
	// The tag plates beside a row's name (.ld-tag.latest / .ld-tag.auto).
	TSharedRef<class SWidget> BuildTag(const FText& Label, bool bWarm);
	void Rebuild();
	bool PassesFilter(const FVoxelSaveRowInfo& Row) const;

	TArray<FVoxelSaveRowInfo> Rows;
	EVoxelSaveFilter ActiveFilter = EVoxelSaveFilter::All;
	FString SearchQuery;
	// How many rows survived the last Rebuild -- what the header's count shows
	// and what decides between the list and the empty state.
	int32 VisibleCount = 0;

	TSharedPtr<class SVerticalBox> ListBox;
	TSharedPtr<class SVoxelMenuButton> CancelButton;

	FOnVoxelSaveAction OnLoadSave;
	FOnVoxelSaveAction OnDeleteSave;
	FSimpleDelegate OnCancel;
};

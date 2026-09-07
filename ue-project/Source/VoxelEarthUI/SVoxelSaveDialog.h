#pragma once
// The SAVE GAME dialog: the port of "Voxelmark Save Dialog.html" (2026-09-07).
//
// The pause menu's own embedded save state is superseded by this file, which is
// the dedicated mock: a context band saying what is being saved, a parchment
// text field with a live character counter, an overwrite warning that appears
// only when the typed name collides with a save that already exists, and
// CANCEL / CONFIRM.
//
// IT KNOWS NOTHING ABOUT THE SAVE SYSTEM. The dialog is handed a default name
// and the list of names already taken, and reports back a string; whoever hosts
// it does the writing. That is what lets it be built with fabricated data in a
// screenshot run, and it is the same contract FVoxelSaveRowInfo gives the LOAD
// list.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

DECLARE_DELEGATE_OneParam(FOnVoxelSaveNameConfirmed, const FString& /*DisplayName*/);

class VOXELEARTHUI_API SVoxelSaveDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelSaveDialog) {}
		// The context band's line -- what the player is saving, not what the
		// save will be called.
		SLATE_ARGUMENT(FText, ContextName)
		// Pre-filled and selected, so ENTER alone is a complete save.
		SLATE_ARGUMENT(FText, DefaultName)
		// Display names already on disk, for the overwrite warning. Compared
		// case-insensitively after trimming, which is how a player means it.
		SLATE_ARGUMENT(TArray<FString>, ExistingNames)
		SLATE_EVENT(FOnVoxelSaveNameConfirmed, OnConfirm)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelSaveDialog() override;

	// Puts the caret in the field with the default name selected.
	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	void HandleTextChanged(const FText& NewText);
	void HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void Confirm();

	// True when the trimmed field matches a name in ExistingNames -- what the
	// .sv-warn band's visibility and CONFIRM's wording both key off.
	bool CollidesWithExisting() const;
	FString TrimmedName() const;

	TSharedPtr<class SEditableText> NameField;
	FText CurrentName;
	TArray<FString> ExistingNames;
	// HandleTextChanged writes back to the field when a paste overruns the
	// length cap, which re-enters it. One bool is cheaper than reasoning about
	// whether SEditableText::SetText can ever notify.
	bool bApplyingClamp = false;

	FOnVoxelSaveNameConfirmed OnConfirm;
	FSimpleDelegate OnCancel;
};

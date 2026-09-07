#pragma once
// The in-game pause overlay: the port of "Voxelmark Pause Menu.html"
// (2026-09-07), plus the three dialogs it opens.
//
// ONE WIDGET FOR FOUR SCREENS, on the main menu's own pattern: a switcher whose
// slots are the pause list, the settings panel, the save dialog and the load
// dialog, with exactly one visible. The alternative -- four widgets the
// subsystem adds to and removes from the viewport -- would put the "which of
// these is on screen" question in a UObject that also owns the game's pause
// state, and answering it in two places is how a paused game ends up with no
// menu on it.
//
// The pause mock's OWN save and load states are not built from this file's
// mock: the 2026-09-07 set has dedicated Save and Load dialogs that supersede
// them, and this switcher hosts those.
//
// IT DOES NOT PAUSE ANYTHING. Pausing, input mode, the save write and the exit
// are the subsystem's (UVoxelPauseUISubsystem); this widget reports what was
// pressed. That keeps it constructible in a screenshot run with no world.

#include "CoreMinimal.h"
#include "SVoxelLoadDialog.h" // FVoxelSaveRowInfo, FOnVoxelSaveAction
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

DECLARE_DELEGATE_OneParam(FOnVoxelSaveNameChosen, const FString& /*DisplayName*/);

// Which of the overlay's four screens is up.
enum class EVoxelPausePanel : uint8
{
	Pause,
	Settings,
	Save,
	Load,
};

class VOXELEARTHUI_API SVoxelPauseMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelPauseMenu)
		: _DayNumber(0)
	{}
		// The footer's "Day N". Zero hides the footer rather than claiming a
		// day this session cannot name -- see VoxelUIStrings::PauseFooter.
		SLATE_ARGUMENT(int32, DayNumber)
		// What the save dialog's context band says, and the stem of the name it
		// offers.
		SLATE_ARGUMENT(FText, SaveContextName)
		SLATE_ARGUMENT(TArray<FVoxelSaveRowInfo>, Rows)
		SLATE_EVENT(FSimpleDelegate, OnResume)
		SLATE_EVENT(FSimpleDelegate, OnExitToMenu)
		SLATE_EVENT(FSimpleDelegate, OnQuit)
		SLATE_EVENT(FOnVoxelSaveNameChosen, OnSaveConfirmed)
		SLATE_EVENT(FOnVoxelSaveAction, OnLoadSave)
		SLATE_EVENT(FOnVoxelSaveAction, OnDeleteSave)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelPauseMenu() override;

	// Rebuilds the LOAD list and the save dialog's collision set together, so
	// the two can never disagree about what is on disk.
	void SetSaveRows(TArray<FVoxelSaveRowInfo> InRows);

	void ShowPanel(EVoxelPausePanel Panel);
	EVoxelPausePanel GetVisiblePanel() const { return VisiblePanel; }
	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedRef<class SWidget> BuildPausePanel();
	// The save dialog is rebuilt every time it becomes visible rather than kept:
	// its default name and its collision set are both snapshots of the moment
	// SAVE was pressed, and a dialog that remembered last time's would offer a
	// stale one. Called from ShowPanel, not from the button -- see that
	// function.
	void BuildSaveDialog();

	TSharedPtr<class SWidgetSwitcher> Switcher;
	TSharedPtr<class SBox> SaveDialogHost;
	TSharedPtr<class SVoxelLoadDialog> LoadDialog;
	TSharedPtr<class SVoxelSettingsPanel> SettingsPanel;
	TSharedPtr<class SVoxelSaveDialog> SaveDialog;
	TArray<TSharedPtr<class SVoxelMenuButton>> PauseButtons;

	TArray<FVoxelSaveRowInfo> Rows;
	FText SaveContextName;
	int32 DayNumber = 0;
	EVoxelPausePanel VisiblePanel = EVoxelPausePanel::Pause;

	FSimpleDelegate OnResume;
	FSimpleDelegate OnExitToMenu;
	FSimpleDelegate OnQuit;
	FOnVoxelSaveNameChosen OnSaveConfirmed;
	FOnVoxelSaveAction OnLoadSave;
	FOnVoxelSaveAction OnDeleteSave;
};

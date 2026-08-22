#pragma once
// The main menu: the port of scripts/MainMenu.gd.
//
// WHAT IS DELIBERATELY NOT PORTED. MainMenu.gd does its own mouse hit-testing
// in _input() -- _dispatch_click walks the visible panel's buttons comparing
// get_global_rect().has_point(pos), and _dispatch_scroll does the same for the
// wheel. That is not a design; it is a workaround for the Dialogic addon
// consuming LMB before _gui_input ever fires, and the file says so. Slate
// routes input properly, so every line of it is dropped and ordinary button
// events take over. The same goes for the manual slider dragging in
// Settings.gd, when that screen is ported.
//
// WHAT IS NEW. The Godot build has NO keyboard or gamepad navigation on any
// menu -- no grab_focus, no focus_neighbor, no ui_accept handling anywhere in
// MainMenu.gd, PauseMenu.gd or Settings.gd. A shipping front end needs it, so
// it is authored here rather than ported.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

DECLARE_DELEGATE(FOnVoxelMenuAction);
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
};

// Which of the four mutually exclusive panels is on screen. Exactly one is
// visible at a time, matching MainMenu.gd's three sibling Controls plus the
// settings placeholder.
enum class EVoxelMenuPanel : uint8
{
	MainColumn,
	Load,
	Help,
	Credits,
	Settings,
};

class VOXELEARTHUI_API SVoxelMainMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMainMenu) {}
		SLATE_EVENT(FOnVoxelMenuAction, OnContinue)
		SLATE_EVENT(FOnVoxelMenuAction, OnNewGame)
		SLATE_EVENT(FOnVoxelMenuAction, OnQuit)
		SLATE_EVENT(FOnVoxelSaveAction, OnLoadSave)
		SLATE_EVENT(FOnVoxelSaveAction, OnDeleteSave)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Rebuilds the LOAD GAME list and, with it, the enabled state of CONTINUE
	// and LOAD GAME -- MainMenu.gd::_show_main_column disables both whenever
	// GameState.list_save_files() is empty, and so does this.
	void SetSaveRows(TArray<FVoxelSaveRowInfo> Rows);

	void ShowPanel(EVoxelMenuPanel Panel);
	EVoxelMenuPanel GetVisiblePanel() const { return VisiblePanel; }

	// Escape backs out of a sub-panel to the main column, and does nothing on
	// the main column itself (there is nowhere further back to go from the
	// first screen of the game).
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedRef<class SWidget> BuildMainColumn();

	FOnVoxelMenuAction OnContinue;
	FOnVoxelMenuAction OnNewGame;
	FOnVoxelMenuAction OnQuit;
	FOnVoxelSaveAction OnLoadSave;
	FOnVoxelSaveAction OnDeleteSave;

	TSharedPtr<class SWidgetSwitcher> PanelSwitcher;
	TSharedPtr<class SVoxelMenuButton> ContinueButton;
	TSharedPtr<class SVoxelMenuButton> LoadGameButton;
	TSharedPtr<class SVerticalBox> SaveList;
	TSharedPtr<class SWidget> FirstFocusTarget;

	TArray<FVoxelSaveRowInfo> SaveRows;
	EVoxelMenuPanel VisiblePanel = EVoxelMenuPanel::MainColumn;

	bool HasAnyLoadableSave() const;
};

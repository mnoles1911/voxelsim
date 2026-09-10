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
// FVoxelSaveRowInfo and FOnVoxelSaveAction moved to SVoxelLoadDialog.h when the
// pause overlay became a second consumer of both -- a shared struct declared in
// the header of one of its two consumers is how include cycles start.
#include "SVoxelLoadDialog.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Styling/SlateColor.h"

DECLARE_DELEGATE(FOnVoxelMenuAction);

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
	// This widget holds bare pointers into FVoxelUIStyle (SButton's
	// FButtonStyle*, SImage's FSlateBrush*), so its lifetime is what that
	// singleton's Shutdown assertion counts.
	virtual ~SVoxelMainMenu() override;

	// Rebuilds the LOAD GAME list and, with it, the enabled state of CONTINUE
	// and LOAD GAME -- MainMenu.gd::_show_main_column disables both whenever
	// GameState.list_save_files() is empty, and so does this.
	void SetSaveRows(TArray<FVoxelSaveRowInfo> Rows);

	void ShowPanel(EVoxelMenuPanel Panel);
	// Puts keyboard focus on the visible panel's first usable control. Called
	// when a panel opens and by the front end when the menu first appears.
	void FocusDefaultWidget();
	EVoxelMenuPanel GetVisiblePanel() const { return VisiblePanel; }

	// Escape backs out of a sub-panel to the main column, and does nothing on
	// the main column itself (there is nowhere further back to go from the
	// first screen of the game).
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedRef<class SWidget> BuildMainColumn();
	// SVoxelLoadDialog, since 2026-09-07. The oak sub-panel with two lines of
	// text per row is gone; the filter chips, the search field and the tagged
	// rows are the mock's, and the pause overlay shows the same widget.
	TSharedRef<class SWidget> BuildLoadPanel();
	// HELP, CREDITS and SETTINGS are the same shape: a title, a paragraph and a
	// BACK button. One builder rather than three near-identical ones, because
	// three copies of a panel is how three panels start disagreeing.
	TSharedRef<class SWidget> BuildMessagePanel(EVoxelMenuPanel Panel, const FText& Title, const FText& Body);
	// SVoxelSettingsPanel since 2026-09-07 -- no longer message-panel shaped,
	// and no longer registered in MessagePanelBackButtons, because it owns its
	// own footer, its own Escape and its own focus default.
	TSharedRef<class SWidget> BuildSettingsPanel();
	// menu_body_panel(): oak fill, 2px black border, 18px content margin, plus
	// the drop shadow Slate brushes cannot express -- see the .cpp.
	TSharedRef<class SWidget> WrapInPanelFrame(TSharedRef<class SWidget> Content);

	FOnVoxelMenuAction OnContinue;
	FOnVoxelMenuAction OnNewGame;
	FOnVoxelMenuAction OnQuit;
	FOnVoxelSaveAction OnLoadSave;
	FOnVoxelSaveAction OnDeleteSave;

	TSharedPtr<class SWidgetSwitcher> PanelSwitcher;
	TSharedPtr<class SVoxelMenuButton> ContinueButton;
	TSharedPtr<class SVoxelMenuButton> LoadGameButton;
	// The two mock-ported sub-panels, each of which owns its own focus default
	// and its own Escape.
	TSharedPtr<class SVoxelLoadDialog> LoadDialog;
	TSharedPtr<class SVoxelSettingsPanel> SettingsPanel;
	// Per-panel focus targets. Slate finds neighbours on its own once something
	// in the panel HAS focus; these are what give it that starting point when a
	// panel opens, so a player can drive the whole menu from a gamepad without
	// touching the mouse first.
	TArray<TSharedPtr<class SVoxelMenuButton>> ColumnButtons;
	// One per message panel. A single shared pointer would end up holding
	// whichever panel was built LAST, and focusing an invisible widget is a
	// gamepad player pressing A at nothing.
	TMap<EVoxelMenuPanel, TSharedPtr<class SVoxelMenuButton>> MessagePanelBackButtons;

	TArray<FVoxelSaveRowInfo> SaveRows;
	EVoxelMenuPanel VisiblePanel = EVoxelMenuPanel::MainColumn;

	bool HasAnyLoadableSave() const;
	// Whether the keyboard is on none of the title-screen items. Drives the
	// resting `.title-menu__item.active` cartouche on NEW GAME -- see the call
	// site for why a focus-only highlight was not enough.
	bool HasNoColumnFocus() const;

	const struct FSlateBrush* GetBackgroundBrush() const;
	FSlateColor GetBackgroundTint() const;

	// Which entry of the shuffled background rotation this menu is showing.
	// Chosen once in Construct: MainMenu.gd re-picks per launch, not per
	// frame, and a background that changed while somebody was reading the menu
	// would be a new behaviour rather than a ported one.
	int32 BackgroundIndex = 0;
};

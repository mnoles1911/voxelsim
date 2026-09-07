#pragma once
// The frame the five in-game screens share: the tab bar, the oak body panel it
// sits on, and the contextual action bar underneath.
//
// ONE SHELL, FIVE BODIES. "Voxelmark Inventory / Map / Journal / Player /
// Codex.html" are five files that each restate the same .menu-shell,
// .menu-tabs, .menu-body and .action-bar markup, with only the body between
// them differing. Porting that literally would mean five copies of the tab bar
// -- and the tab bar is exactly the thing that must be identical on all five,
// because the player sees it move between them. So this widget owns the chrome
// and the tab set, and each screen supplies a body and its own action hints.
//
// A WIDGET RATHER THAN A VoxelOverlayChrome BUILDER, which is the opposite of
// the call made for the overlay family. The difference is state: the chrome
// builders are decorations with no lifetime, whereas this holds which tab is
// active, owns the tab buttons keyboard focus has to land on, and handles the
// tab and Escape keys. That is a widget's job.
//
// IT SWITCHES NOTHING BY ITSELF. Pressing a tab raises OnTabChanged and the
// subsystem rebuilds; this widget never constructs a screen body. That keeps it
// constructible in a screenshot run with no world, on SVoxelPauseMenu's rule.

#include "CoreMinimal.h"
#include "VoxelScreenData.h" // EVoxelScreenTab
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnVoxelScreenTabChanged, EVoxelScreenTab /*Tab*/);

// One `<kbd>KEY</kbd> label` pair in the .action-bar. Built as data rather than
// as widgets so a screen can declare its hints in one array literal next to the
// behaviour they describe.
struct FVoxelScreenAction
{
	FText Key;
	FText Label;
	// .action.danger -- reads HP_BRIGHT rather than gold when lit.
	bool bDanger = false;
	// Pushes everything after it to the right (.action-bar .spacer).
	bool bSpacerBefore = false;

	FVoxelScreenAction() = default;
	FVoxelScreenAction(FText InKey, FText InLabel, bool bInSpacerBefore = false, bool bInDanger = false)
		: Key(MoveTemp(InKey)), Label(MoveTemp(InLabel)), bDanger(bInDanger), bSpacerBefore(bInSpacerBefore)
	{
	}
};

class VOXELEARTHUI_API SVoxelScreenShell : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelScreenShell)
		: _ActiveTab(EVoxelScreenTab::Inventory)
		, _ShrinkWrap(false)
	{}
		SLATE_ARGUMENT(EVoxelScreenTab, ActiveTab)
		// The INVENTORY mock replaces the compact shell's fixed size with
		// `width:max-content`. True sets NO size override, so the frame is as
		// big as its contents; false uses ScreenShellWidth/Height.
		SLATE_ARGUMENT(bool, ShrinkWrap)
		SLATE_ARGUMENT(TArray<FVoxelScreenAction>, Actions)
		SLATE_NAMED_SLOT(FArguments, Body)
		SLATE_EVENT(FOnVoxelScreenTabChanged, OnTabChanged)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelScreenShell() override;

	virtual bool SupportsKeyboardFocus() const override { return true; }
	// Escape closes; the five tab keys switch; Q/E page between tabs, which is
	// the gamepad-shoulder idiom the mock's key hints describe.
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

	// Focus lands on the ACTIVE TAB's button, so arrowing works from the moment
	// a screen opens -- SVoxelPauseMenu::FocusDefaultWidget's rule, and for the
	// reason given there: SetWidgetToFocus reaches OnKeyDown and does not seed
	// navigation.
	void FocusDefaultWidget();

	// The key each tab answers to, and the letter drawn in its .key span. Public
	// because the subsystem binds the same five keys globally, and two lists of
	// them would drift.
	static FKey TabKey(EVoxelScreenTab Tab);
	static FText TabKeyLabel(EVoxelScreenTab Tab);
	static FText TabLabel(EVoxelScreenTab Tab);
	static EVoxelScreenTab TabAtOffset(EVoxelScreenTab From, int32 Delta);

private:
	TSharedRef<class SWidget> BuildTabBar();
	TSharedRef<class SWidget> BuildActionBar() const;

	EVoxelScreenTab ActiveTab = EVoxelScreenTab::Inventory;
	TArray<FVoxelScreenAction> Actions;
	TArray<TSharedPtr<class SVoxelMenuButton>> TabButtons;

	FOnVoxelScreenTabChanged OnTabChanged;
	FSimpleDelegate OnClose;
};

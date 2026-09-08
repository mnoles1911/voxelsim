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
//
// TWO OWNER DIRECTIVES SHAPE THIS FILE, 2026-09-07, verbatim:
//
//   *"for the unified inventory/journal/map/player/codex UI menu, make it
//   resizable if a player wants to make the entire thing larger or smaller in
//   run time."*
//
//   *"also, it currently appears that the inventory UI menu size is slightly
//   larger than the default sizing for map, journal, player, and codex
//   sections. unify this."*
//
// The first is VoxelScreenShellSettings plus GetShellRenderTransform and the
// Ctrl bindings in OnKeyDown / OnMouseWheel. The second is the retirement of
// the ShrinkWrap argument below, plus the down-only fit around the body in
// Construct that lets the inventory's content live inside one shared frame
// instead of growing it.

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
		// RETIRED, AND KEPT ONLY SO ITS CALLER STILL COMPILES. Owner directive,
		// 2026-09-07, verbatim: *"it currently appears that the inventory UI
		// menu size is slightly larger than the default sizing for map,
		// journal, player, and codex sections. unify this."*
		//
		// This argument WAS that difference. The inventory mock's .menu-shell
		// carries `width:max-content` where the other four carry the compact
		// shell's fixed size, and passing true here dropped the size override
		// entirely so the frame grew to whatever the 8x8 pack needed --
		// measured on the 2026-09-07 captures as 1348x944 against 1326x906 for
		// the other four, i.e. 16 units wider and 28 taller in authored space.
		// Five screens the player tabs between cannot be five sizes.
		//
		// The shell is now ALWAYS ScreenShellWidth x ScreenShellHeight and this
		// flag does nothing. It is still declared because
		// VoxelScreensUISubsystem passes it at two call sites owned by another
		// agent; whoever holds that file can delete both and then this.
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
	// the gamepad-shoulder idiom the mock's key hints describe. Ctrl with
	// plus / minus / zero drives the resize -- see OnMouseWheel.
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;
	// Ctrl + wheel resizes the shell. Plain wheel is left alone so the lists
	// inside these screens still scroll.
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& MouseEvent) override;

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

	// The player's Menu Size dial, as a render transform about the frame's
	// centre. A RENDER transform and not a layout one, which is the whole point:
	// nothing inside the shell re-measures or re-wraps at any setting, so the
	// screens cannot look different from each other at 1.25 than they do at
	// 1.00. Read every paint from VoxelScreenShellSettings, so a change made
	// from the Settings panel while a screen is open lands on the next frame
	// with nothing to subscribe to.
	TOptional<FSlateRenderTransform> GetShellRenderTransform() const;

	EVoxelScreenTab ActiveTab = EVoxelScreenTab::Inventory;
	TArray<FVoxelScreenAction> Actions;
	TArray<TSharedPtr<class SVoxelMenuButton>> TabButtons;

	FOnVoxelScreenTabChanged OnTabChanged;
	FSimpleDelegate OnClose;
};

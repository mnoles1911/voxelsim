#pragma once
// One oak menu button: the port of UIStyles.apply_menu_button().
//
// A CLASS RATHER THAN A FACTORY FUNCTION, for three reasons that all turn out
// to be about having one place to put things:
//
//   * The Godot StyleBoxFlat carries fill AND border in one object; Slate's
//     FSlateBrush carries only fill. Reproducing the 2px border therefore
//     means stacking two boxes per state, and that stacking wants to live
//     somewhere other than every call site.
//   * Keyboard and gamepad navigation is NEW WORK -- the Godot build has none
//     at all (its menus do manual mouse hit-testing in _input, a Dialogic
//     workaround that must not be ported). Focus visuals belong here.
//   * The HTML mock's hover behaviour (colour to #F5D06E plus a 4px slide)
//     was never implemented in the Godot build. It lives here behind
//     voxel.UI.HoverSlide, default 0, so adopting it later is a flag flip and
//     a screenshot rather than a rewrite.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Framework/SlateDelegates.h" // FOnClicked
#include "Styling/SlateColor.h"

// WHICH CHROME THIS BUTTON WEARS. One widget, four looks, because focus,
// navigation, the enabled ladder and the click plumbing are identical across
// all of them and a second widget class is how two of those start disagreeing.
enum class EVoxelMenuButtonVariant : uint8
{
	// UIStyles.apply_menu_button(): the oak plate with a 2 px stacked border.
	// Still what every sub-panel control uses.
	Oak,
	// .title-menu__item (2026-09-07 title screen). No box: right-aligned
	// parchment text over the scene art, and on hover or keyboard focus a
	// translucent gold "cartouche" -- a soft gold fill between a 1 px gold rule
	// above and below -- with the face growing to ActiveFontSize.
	Cartouche,
	// The overlay family's action plate: .se-act / .sv-act / .ld-btn /
	// .ld-cancel / .ld-filter. A leather-gradient fill inside a 1 px
	// --leather-edge rule; lit (hover or focus) it takes a --warm-primary
	// border and a gold label. Primary swaps the fill for the warm plate.
	Leather,
	// .pa-btn, the pause list line: no box at all, a "›" chevron, the label
	// left-aligned, and a hairline rule underneath.
	PauseItem,
};

class VOXELEARTHUI_API SVoxelMenuButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMenuButton)
		: _FontSize(24)
		, _MinHeight(56.f)
		, _MinWidth(0.f)
		, _TextColorOverride()
		, _Variant(EVoxelMenuButtonVariant::Oak)
		, _Muted(false)
		, _Primary(false)
		, _Danger(false)
		, _Active(false)
		, _ContentPadding(FMargin(0.f))
		, _ActiveFontSize(0)
		, _LetterSpacing(0)
	{}
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ARGUMENT(int32, FontSize)
		SLATE_ARGUMENT(float, MinHeight)
		SLATE_ARGUMENT(float, MinWidth)
		SLATE_ARGUMENT(EVoxelMenuButtonVariant, Variant)
		// Cartouche: the QUIT item -- dimmer at rest, and it does NOT take the
		// cartouche on hover (the mock's .quit:hover).
		SLATE_ARGUMENT(bool, Muted)
		// Leather: the .primary plate -- a --warm-primary/--warm-secondary fill
		// with #fff8e0 text. The affirmative action of each dialog.
		SLATE_ARGUMENT(bool, Primary)
		// Leather and PauseItem: the destructive label colour (.pa-btn.danger,
		// .ld-btn.del). Independent of Primary; no dialog has both.
		SLATE_ARGUMENT(bool, Danger)
		// Leather: .ld-filter.on -- the selected filter chip, which paints like
		// Primary. AN ATTRIBUTE, not a flag, because exactly one chip of a group
		// is on at a time and the group's state lives in the panel, not here.
		SLATE_ATTRIBUTE(bool, Active)
		// Leather: the plate's own padding (.se-act is 9px 24px, .ld-btn 8/14).
		// Zero means the variant's default.
		SLATE_ARGUMENT(FMargin, ContentPadding)
		SLATE_ARGUMENT(int32, ActiveFontSize)
		// FSlateFontInfo::LetterSpacing units (1/1000 em).
		SLATE_ARGUMENT(int32, LetterSpacing)
		// The per-save DELETE button paints its label HP_BRIGHT rather than
		// INK. Unset means the ordinary INK/GOLD/GOLD_DEEP/INK_MUTE ladder.
		SLATE_ARGUMENT(TOptional<FLinearColor>, TextColorOverride)
		// NOTE: there is deliberately no SLATE_ATTRIBUTE(bool, IsEnabled) here.
		// FSlateBaseNamedArgs -- which every SLATE_BEGIN_ARGS block inherits --
		// already declares one, and redeclaring it is a compile error rather
		// than an override. Call sites therefore use the inherited
		// .IsEnabled(...), SWidget::SWidgetConstruct applies it to this widget,
		// and Construct forwards it down to the inner SButton explicitly (see
		// the .cpp: a disabled ancestor does not by itself make a descendant
		// SButton non-interactive).
		SLATE_EVENT(FOnClicked, OnClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	// This widget holds bare pointers into FVoxelUIStyle (SButton's
	// FButtonStyle*, SImage's FSlateBrush*), so its lifetime is what that
	// singleton's Shutdown assertion counts.
	virtual ~SVoxelMenuButton() override;

	// The inner SButton -- what keyboard focus has to land on.
	//
	// FOCUS DOES NOT LAND ON *THIS*. SVoxelMenuButton is an SCompoundWidget
	// wrapping an SButton so it can stack a border behind the fill, and
	// SCompoundWidget does not take keyboard focus. Slate's own navigation
	// finds the inner button on its own when arrowing between siblings, but
	// anything that focuses a button DELIBERATELY -- opening a panel, say --
	// has to name the widget that can actually hold focus.
	TSharedPtr<class SWidget> GetFocusWidget() const;

private:
	// The four-state colour ladder from UIStyles.apply_menu_button():
	// INK normally, GOLD hovered, GOLD_DEEP pressed, INK_MUTE disabled.
	FSlateColor GetLabelColour() const;
	// Border colour tracks the same states: black normally, GOLD hovered,
	// GOLD_DEEP pressed, IRON_DEEP disabled.
	FSlateColor GetBorderColour() const;
	// voxel.UI.HoverSlide: the mock's translateX(4px). Zero by default.
	FMargin GetHoverSlidePadding() const;

	// "Lit" means hovered or keyboard-focused AND enabled -- the one predicate
	// every variant's fill, rule, colour and font size keys off, so they cannot
	// disagree about the state. The Cartouche variant additionally excludes
	// Muted; that exception lives inside the function rather than at four call
	// sites.
	bool IsLit() const;
	FSlateColor GetCartoucheLabelColour() const;
	FSlateColor GetCartoucheFillColour() const;
	FSlateColor GetCartoucheRuleColour() const;
	FSlateFontInfo GetCartoucheFont() const;

	// Leather (.se-act / .ld-btn / .sv-act / .ld-filter). Whether the plate is
	// painted warm is Primary OR the Active attribute -- a selected filter chip
	// is the same plate as a primary action.
	bool IsWarmPlate() const;
	FSlateColor GetLeatherFillColour() const;
	FSlateColor GetLeatherBorderColour() const;
	FSlateColor GetLeatherLabelColour() const;

	// PauseItem (.pa-btn): the label, the chevron, and the chevron's 4 px slide.
	FSlateColor GetPauseLabelColour() const;
	FSlateColor GetPauseChevronColour() const;
	FMargin GetPauseChevronPadding() const;

	TSharedPtr<class SButton> Button;
	TOptional<FLinearColor> TextColorOverride;
	EVoxelMenuButtonVariant Variant = EVoxelMenuButtonVariant::Oak;
	bool bMuted = false;
	bool bPrimary = false;
	bool bDanger = false;
	TAttribute<bool> ActiveAttribute;
	int32 RestFontSize = 24;
	int32 ActiveFontSize = 24;
	int32 LetterSpacing = 0;
};

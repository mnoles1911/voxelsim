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

class VOXELEARTHUI_API SVoxelMenuButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelMenuButton)
		: _FontSize(24)
		, _MinHeight(56.f)
		, _MinWidth(0.f)
		, _TextColorOverride()
	{}
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ARGUMENT(int32, FontSize)
		SLATE_ARGUMENT(float, MinHeight)
		SLATE_ARGUMENT(float, MinWidth)
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

private:
	// The four-state colour ladder from UIStyles.apply_menu_button():
	// INK normally, GOLD hovered, GOLD_DEEP pressed, INK_MUTE disabled.
	FSlateColor GetLabelColour() const;
	// Border colour tracks the same states: black normally, GOLD hovered,
	// GOLD_DEEP pressed, IRON_DEEP disabled.
	FSlateColor GetBorderColour() const;
	// voxel.UI.HoverSlide: the mock's translateX(4px). Zero by default.
	FMargin GetHoverSlidePadding() const;

	TSharedPtr<class SButton> Button;
	TOptional<FLinearColor> TextColorOverride;
};

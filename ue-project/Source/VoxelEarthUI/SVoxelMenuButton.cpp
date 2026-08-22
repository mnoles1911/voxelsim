#include "SVoxelMenuButton.h"

#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "HAL/IConsoleManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"

namespace SVoxelMenuButtonDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// voxel.UI.HoverSlide -- the HTML mock's `transform: translateX(4px)` on
// hover. NOT in the shipped Godot build, which is the thing being cloned, so
// it defaults to 0 and exists only so that adopting the mock's behaviour later
// is a flag and a capture rather than an edit to every button.
float GHoverSlidePx = 0.f;
FAutoConsoleVariableRef CVarHoverSlide(TEXT("voxel.UI.HoverSlide"),
                                       GHoverSlidePx,
                                       TEXT("Pixels a menu button's label slides right on hover. 0 (default) matches the ")
                                       TEXT("shipped Godot build; 4 matches the HTML mock."),
                                       ECVF_Default);

// The button's 2px border width, from UIStyles.menu_button_styles().
constexpr float kBorderPx = 2.f;
} // namespace SVoxelMenuButtonDetail

void SVoxelMenuButton::Construct(const FArguments& InArgs)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	TextColorOverride = InArgs._TextColorOverride;

	// THE BORDER, STACKED. Slate has no border-plus-fill brush (see
	// VoxelUIStyle.cpp), so the button is an SOverlay: a full-bleed box in the
	// border colour, then the SButton itself inset by the border width. The
	// SButton's own four-state brush supplies the fill, so hover and press
	// still come from FButtonStyle exactly as they would on a bare SButton --
	// only the outline is hand-drawn.
	TSharedRef<SButton> ButtonRef =
		SNew(SButton)
		.ButtonStyle(&Style.MenuButton())
		.ContentPadding(FMargin(0.f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		// Forwarded from the inherited argument, which SWidgetConstruct has
		// already applied to `this`. Reading it back through SWidget::IsEnabled
		// keeps one source of truth and means the inner button follows a
		// disabled parent without the caller having to say so twice.
		.IsEnabled_Lambda([this]() { return IsEnabled(); })
		.OnClicked(InArgs._OnClicked)
		// The label's colour has to track hover/press/disabled, which
		// FButtonStyle does not do for content -- hence the attribute.
		.ForegroundColor(this, &SVoxelMenuButton::GetLabelColour)
		[
			SNew(SBox)
			.Padding(this, &SVoxelMenuButton::GetHoverSlidePadding)
			[
				SNew(STextBlock)
				.Text(InArgs._Text)
				.Font(Style.Serif(InArgs._FontSize))
				.ColorAndOpacity(this, &SVoxelMenuButton::GetLabelColour)
				.Justification(ETextJustify::Center)
			]
		];
	Button = ButtonRef;

	TSharedRef<SBox> Sized =
		SNew(SBox)
		.MinDesiredHeight(InArgs._MinHeight)
		.MinDesiredWidth(InArgs._MinWidth > 0.f ? FOptionalSize(InArgs._MinWidth) : FOptionalSize())
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage)
				.Image(Style.SolidWhite())
				.ColorAndOpacity(this, &SVoxelMenuButton::GetBorderColour)
			]
			+ SOverlay::Slot()
			.Padding(FMargin(SVoxelMenuButtonDetail::kBorderPx))
			[
				ButtonRef
			]
		];

	ChildSlot
	[
		Sized
	];
}

FSlateColor SVoxelMenuButton::GetLabelColour() const
{
	using namespace VoxelUITheme;
	// UIStyles.apply_menu_button(): font_color INK, hover GOLD, pressed
	// GOLD_DEEP, disabled INK_MUTE. Order matters -- disabled first, because a
	// disabled button can still report hovered.
	if (!IsEnabled())
	{
		return FSlateColor(Tint(InkMute));
	}
	if (Button.IsValid() && Button->IsPressed())
	{
		return FSlateColor(Tint(GoldDeep));
	}
	if (IsHovered())
	{
		return FSlateColor(Tint(Gold));
	}
	// The DELETE button's HP_BRIGHT label. Applied only in the resting state,
	// matching the Godot version, which overrides `font_color` alone and
	// leaves the hover/pressed entries at their gold defaults -- so a hovered
	// DELETE goes gold like everything else.
	if (TextColorOverride.IsSet())
	{
		return FSlateColor(*TextColorOverride);
	}
	return FSlateColor(Tint(Ink));
}

FSlateColor SVoxelMenuButton::GetBorderColour() const
{
	using namespace VoxelUITheme;
	// menu_button_styles(): normal border black, hover GOLD, pressed
	// GOLD_DEEP, disabled IRON_DEEP.
	if (!IsEnabled())
	{
		return FSlateColor(Tint(IronDeep));
	}
	if (Button.IsValid() && Button->IsPressed())
	{
		return FSlateColor(Tint(GoldDeep));
	}
	if (IsHovered())
	{
		return FSlateColor(Tint(Gold));
	}
	return FSlateColor(FLinearColor::Black);
}

FMargin SVoxelMenuButton::GetHoverSlidePadding() const
{
	const float Slide = SVoxelMenuButtonDetail::GHoverSlidePx;
	if (Slide <= 0.f || !IsHovered() || !IsEnabled())
	{
		return FMargin(0.f);
	}
	// Left padding only, so the label slides right inside a button whose box
	// does not move -- which is what the mock's translateX does.
	return FMargin(Slide, 0.f, 0.f, 0.f);
}

TSharedPtr<SWidget> SVoxelMenuButton::GetFocusWidget() const
{
	return Button;
}

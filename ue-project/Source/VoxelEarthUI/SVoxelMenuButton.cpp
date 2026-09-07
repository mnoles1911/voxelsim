#include "SVoxelMenuButton.h"

#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "HAL/IConsoleManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Types/NavigationMetaData.h"

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
// The overlay plates' border is 1 px (`border:1px solid var(--leather-edge)`).
constexpr float kLeatherBorderPx = 1.f;
// .pa-btn::before -- U+203A. MEASURED PRESENT in all four shipped faces
// (a cmap dump of Content/UI/Fonts, 2026-09-07), unlike the mocks' U+2726 star
// and U+2205 empty set, which are in none of them and are drawn geometrically
// instead. Which is why this one is a glyph and those are boxes.
const TCHAR* const kChevron = TEXT("›");
// .pa-btn:hover::before -- translateX(4px). Unconditional here, unlike the oak
// variant's voxel.UI.HoverSlide: the pause list is NEW screen, not a clone of a
// shipped Godot one, so there is no prior behaviour to preserve a flag for.
constexpr float kChevronSlidePx = 4.f;
} // namespace SVoxelMenuButtonDetail

SVoxelMenuButton::~SVoxelMenuButton()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelMenuButton::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	TextColorOverride = InArgs._TextColorOverride;
	Variant = InArgs._Variant;
	bMuted = InArgs._Muted;
	bPrimary = InArgs._Primary;
	bDanger = InArgs._Danger;
	ActiveAttribute = InArgs._Active;
	RestFontSize = InArgs._FontSize;
	ActiveFontSize = InArgs._ActiveFontSize > 0 ? InArgs._ActiveFontSize : InArgs._FontSize;
	LetterSpacing = InArgs._LetterSpacing;

	if (Variant == EVoxelMenuButtonVariant::Cartouche)
	{
		// .title-menu__item: the SButton is transparent in every state (the
		// style below paints nothing) and exists for focus, hover and click.
		// The chrome is three tinted boxes whose colours all read
		// IsLit(): a fill, and a 1 px rule above and below. The font
		// swaps size on the same predicate -- the mock's 29 -> 33 -- which is
		// why the label's Font is an attribute here rather than a value.
		TSharedRef<SButton> ButtonRef =
			SNew(SButton)
			.ButtonStyle(&Style.CartoucheButton())
			.ContentPadding(FMargin(0.f))
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			.IsEnabled_Lambda([this]() { return IsEnabled(); })
			.OnClicked(InArgs._OnClicked)
			.ForegroundColor(this, &SVoxelMenuButton::GetCartoucheLabelColour)
			[
				SNew(STextBlock)
				.Text(InArgs._Text)
				.Font(this, &SVoxelMenuButton::GetCartoucheFont)
				.ColorAndOpacity(this, &SVoxelMenuButton::GetCartoucheLabelColour)
				.Justification(ETextJustify::Right)
				// --shadow-emboss-md: 3px 3px 0 #000. The 18 px gold glow the
				// mock adds when lit has no Slate equivalent and is not faked.
				.ShadowOffset(FVector2D(3.f, 3.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
			];
		Button = ButtonRef;

		TSharedRef<FNavigationMetaData> Navigation = MakeShared<FNavigationMetaData>();
		Navigation->SetNavigationWrap(EUINavigation::Up);
		Navigation->SetNavigationWrap(EUINavigation::Down);
		ButtonRef->AddMetadata(Navigation);

		const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
		ChildSlot
		[
			SNew(SBox)
			.MinDesiredWidth(InArgs._MinWidth > 0.f ? FOptionalSize(InArgs._MinWidth) : FOptionalSize(L.TitleMenuItemMinWidth))
			.MinDesiredHeight(InArgs._MinHeight)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelMenuButton::GetCartoucheFillColour)
				]
				+ SOverlay::Slot().VAlign(VAlign_Top)
				[
					SNew(SBox).HeightOverride(VoxelUITheme::RulePx)
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(this, &SVoxelMenuButton::GetCartoucheRuleColour)
					]
				]
				+ SOverlay::Slot().VAlign(VAlign_Bottom)
				[
					SNew(SBox).HeightOverride(VoxelUITheme::RulePx)
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(this, &SVoxelMenuButton::GetCartoucheRuleColour)
					]
				]
				+ SOverlay::Slot()
				.Padding(FMargin(L.TitleMenuItemPadX, L.TitleMenuItemPadY))
				[
					ButtonRef
				]
			]
		];
		return;
	}

	if (Variant == EVoxelMenuButtonVariant::Leather)
	{
		// .se-act / .sv-act / .ld-btn / .ld-cancel / .ld-filter, which differ
		// only in font size, padding and whether they are the primary plate --
		// so they are one variant with arguments rather than five styles.
		//
		// Same stacking rule as the oak plate: a full-bleed box in the border
		// colour, the fill inset by the border width. The fill is an ATTRIBUTE
		// here rather than an FButtonStyle brush, because the Active chip state
		// has to be able to change it after construction and FButtonStyle is
		// chosen once.
		const FMargin& Given = InArgs._ContentPadding;
		const bool bDefaultPadding = Given.Left == 0.f && Given.Top == 0.f && Given.Right == 0.f && Given.Bottom == 0.f;
		const FMargin Padding = bDefaultPadding ? FMargin(14.f, 8.f) : Given;
		FSlateFontInfo LabelFont = Style.Serif(InArgs._FontSize);
		LabelFont.LetterSpacing = LetterSpacing;

		TSharedRef<SButton> ButtonRef =
			SNew(SButton)
			.ButtonStyle(&Style.CartoucheButton())
			.ContentPadding(Padding)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.IsEnabled_Lambda([this]() { return IsEnabled(); })
			.OnClicked(InArgs._OnClicked)
			.ForegroundColor(this, &SVoxelMenuButton::GetLeatherLabelColour)
			[
				SNew(STextBlock)
				.Text(InArgs._Text)
				.Font(LabelFont)
				.ColorAndOpacity(this, &SVoxelMenuButton::GetLeatherLabelColour)
				.Justification(ETextJustify::Center)
				// text-shadow:1px 1px 0 #000, which every plate in the overlay
				// mocks carries.
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
			];
		Button = ButtonRef;

		TSharedRef<FNavigationMetaData> Navigation = MakeShared<FNavigationMetaData>();
		Navigation->SetNavigationWrap(EUINavigation::Up);
		Navigation->SetNavigationWrap(EUINavigation::Down);
		ButtonRef->AddMetadata(Navigation);

		ChildSlot
		[
			SNew(SBox)
			.MinDesiredHeight(InArgs._MinHeight)
			.MinDesiredWidth(InArgs._MinWidth > 0.f ? FOptionalSize(InArgs._MinWidth) : FOptionalSize())
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelMenuButton::GetLeatherBorderColour)
				]
				+ SOverlay::Slot()
				.Padding(FMargin(SVoxelMenuButtonDetail::kLeatherBorderPx))
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelMenuButton::GetLeatherFillColour)
				]
				+ SOverlay::Slot()
				[
					ButtonRef
				]
			]
		];
		return;
	}

	if (Variant == EVoxelMenuButtonVariant::Tab || Variant == EVoxelMenuButtonVariant::Chip)
	{
		// .menu-tab and .filter-tab are the same construction at two sizes: a
		// full-bleed black rect, a 1 px edge ring, the plate fill, and -- when
		// selected -- a 3 px gold rule along the bottom edge standing in for the
		// CSS `inset 0 -3px 0 var(--gold)`.
		//
		// THE UNDERLINE IS A SIBLING BOX, NOT AN INSET SHADOW. Slate's brushes
		// carry no inset shadow, and drawing it as a bottom-aligned box inside
		// the same overlay is the only way to get a rule that is exactly 3 px
		// regardless of the plate's height.
		const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
		const bool bIsTab = Variant == EVoxelMenuButtonVariant::Tab;

		const FMargin& Given = InArgs._ContentPadding;
		const bool bDefaultPadding = Given.Left == 0.f && Given.Top == 0.f && Given.Right == 0.f && Given.Bottom == 0.f;
		const FMargin Padding = bDefaultPadding
			? (bIsTab ? FMargin(L.TabPadX, L.TabPadTop, L.TabPadX, L.TabPadBottom)
			          : FMargin(L.InvFilterPadX, L.InvFilterPadY))
			: Given;

		FSlateFontInfo LabelFont = Style.Serif(InArgs._FontSize);
		LabelFont.LetterSpacing = LetterSpacing;

		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);
		// .menu-tab .key -- the shortcut letter, in the mono face, ahead of the
		// name. Collapsed rather than empty when a caller passes nothing, so a
		// keyless tab does not carry the 8 px gap.
		TAttribute<FText> KeyAttr = InArgs._KeyLabel;
		if (KeyAttr.IsSet())
		{
			Content->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, L.TabKeyGap, 0.f))
			[
				SNew(STextBlock)
				.Text(KeyAttr)
				.Font(Style.Mono(L.TabKeySize))
				.ColorAndOpacity(this, &SVoxelMenuButton::GetTabKeyColour)
				.Visibility_Lambda([KeyAttr]()
				{
					return KeyAttr.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
				})
			];
		}
		Content->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(InArgs._Text)
			.Font(LabelFont)
			.ColorAndOpacity(this, &SVoxelMenuButton::GetTabLabelColour)
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
		];
		// .filter-tab .n -- the tally, in the mono face, after the name.
		TAttribute<FText> CountAttr = InArgs._CountLabel;
		if (CountAttr.IsSet())
		{
			Content->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(L.ActionKeyGap, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(CountAttr)
				.Font(Style.Mono(InArgs._FontSize + 4))
				.ColorAndOpacity(this, &SVoxelMenuButton::GetTabKeyColour)
				.Visibility_Lambda([CountAttr]()
				{
					return CountAttr.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
				})
			];
		}

		TSharedRef<SButton> ButtonRef =
			SNew(SButton)
			.ButtonStyle(&Style.CartoucheButton())
			.ContentPadding(Padding)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.IsEnabled_Lambda([this]() { return IsEnabled(); })
			.OnClicked(InArgs._OnClicked)
			.ForegroundColor(this, &SVoxelMenuButton::GetTabLabelColour)
			[
				Content
			];
		Button = ButtonRef;

		// The tab bar is a row and the chip groups are rows, so Left/Right wrap
		// -- the opposite axis from every other variant in this file.
		TSharedRef<FNavigationMetaData> Navigation = MakeShared<FNavigationMetaData>();
		Navigation->SetNavigationWrap(EUINavigation::Left);
		Navigation->SetNavigationWrap(EUINavigation::Right);
		ButtonRef->AddMetadata(Navigation);

		ChildSlot
		[
			SNew(SBox)
			.MinDesiredHeight(InArgs._MinHeight)
			.MinDesiredWidth(InArgs._MinWidth > 0.f ? FOptionalSize(InArgs._MinWidth) : FOptionalSize())
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(FColor::Black))
				]
				+ SOverlay::Slot()
				.Padding(FMargin(SVoxelMenuButtonDetail::kBorderPx))
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelMenuButton::GetTabEdgeColour)
				]
				+ SOverlay::Slot()
				.Padding(FMargin(SVoxelMenuButtonDetail::kBorderPx + SVoxelMenuButtonDetail::kLeatherBorderPx))
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelMenuButton::GetTabFillColour)
				]
				// The gold underline, bottom-aligned inside the border.
				+ SOverlay::Slot()
				.VAlign(VAlign_Bottom)
				.Padding(FMargin(SVoxelMenuButtonDetail::kBorderPx, 0.f,
				                 SVoxelMenuButtonDetail::kBorderPx, SVoxelMenuButtonDetail::kBorderPx))
				[
					SNew(SBox)
					.HeightOverride(L.TabUnderlinePx)
					[
						SNew(SImage)
						.Image(Style.SolidWhite())
						.ColorAndOpacity(this, &SVoxelMenuButton::GetTabUnderlineColour)
					]
				]
				+ SOverlay::Slot()
				[
					ButtonRef
				]
			]
		];
		return;
	}

	if (Variant == EVoxelMenuButtonVariant::PauseItem)
	{
		// .pa-btn: a chevron, a left-aligned label, and a hairline rule under
		// the row. No fill in any state -- the whole affordance is the colour
		// change and the chevron's 4 px slide.
		FSlateFontInfo LabelFont = Style.Serif(InArgs._FontSize);
		LabelFont.LetterSpacing = LetterSpacing;
		const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

		TSharedRef<SButton> ButtonRef =
			SNew(SButton)
			.ButtonStyle(&Style.CartoucheButton())
			.ContentPadding(FMargin(0.f, L.PauseItemPadY))
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.IsEnabled_Lambda([this]() { return IsEnabled(); })
			.OnClicked(InArgs._OnClicked)
			.ForegroundColor(this, &SVoxelMenuButton::GetPauseLabelColour)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				// A slot's Padding takes a TAttribute, not the (object, method) pair
			// a widget argument would; spelled out so the chevron's slide stays
			// an attribute rather than becoming a fixed margin.
			.Padding(TAttribute<FMargin>::CreateSP(this, &SVoxelMenuButton::GetPauseChevronPadding))
				[
					SNew(STextBlock)
					.Text(FText::FromString(SVoxelMenuButtonDetail::kChevron))
					.Font(Style.Serif(L.PauseChevronSize))
					.ColorAndOpacity(this, &SVoxelMenuButton::GetPauseChevronColour)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(InArgs._Text)
					.Font(LabelFont)
					.ColorAndOpacity(this, &SVoxelMenuButton::GetPauseLabelColour)
					.Justification(ETextJustify::Left)
				]
			];
		Button = ButtonRef;

		TSharedRef<FNavigationMetaData> Navigation = MakeShared<FNavigationMetaData>();
		Navigation->SetNavigationWrap(EUINavigation::Up);
		Navigation->SetNavigationWrap(EUINavigation::Down);
		ButtonRef->AddMetadata(Navigation);

		ChildSlot
		[
			SNew(SBox)
			.MinDesiredHeight(InArgs._MinHeight)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					ButtonRef
				]
				// border-bottom:1px solid rgba(90,56,24,0.3).
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).HeightOverride(VoxelUITheme::RulePx)
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(FSlateColor(Tint(LeatherEdge, 0.30f)))
					]
				]
			]
		];
		return;
	}

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

	// NAVIGATION METADATA GOES ON THE FOCUSABLE WIDGET, NOT ON A CONTAINER.
	// FSlateApplication::AttemptNavigation reads FNavigationMetaData off the
	// widget that currently HAS focus, to decide what happens when navigation
	// finds no neighbour in that direction. Attaching it to the enclosing
	// SVerticalBox -- which is where this started -- means it is never
	// consulted, so pressing Up on the first button does nothing at all and a
	// gamepad player cannot tell a menu at its top edge from a broken one.
	//
	// Harmless on a button in the MIDDLE of a list: a real neighbour is found
	// first and the boundary rule never comes up.
	TSharedRef<FNavigationMetaData> Navigation = MakeShared<FNavigationMetaData>();
	Navigation->SetNavigationWrap(EUINavigation::Up);
	Navigation->SetNavigationWrap(EUINavigation::Down);
	ButtonRef->AddMetadata(Navigation);

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

bool SVoxelMenuButton::IsLit() const
{
	if (!IsEnabled())
	{
		return false;
	}
	// The Cartouche variant's QUIT item is the one control that stays dark when
	// pointed at (.title-menu__item.quit:hover), so the exception lives here
	// rather than being re-tested by every colour accessor.
	if (bMuted && Variant == EVoxelMenuButtonVariant::Cartouche)
	{
		return false;
	}
	// KEYBOARD FOCUS COUNTS AS HOVER, in every variant. The mocks only describe
	// :hover because a web page has no gamepad; a player driving this with a
	// stick has to be able to see where they are.
	const bool bFocused = Button.IsValid() && Button->HasKeyboardFocus();
	return IsHovered() || bFocused;
}

bool SVoxelMenuButton::IsCartoucheOn() const
{
	if (IsLit())
	{
		return true;
	}
	// The Muted (QUIT) exception is IsLit's; restated here because this branch
	// does not go through it. A disabled item never takes the band either --
	// the mock has no `.active` on a greyed CONTINUE.
	if (!IsEnabled() || bMuted)
	{
		return false;
	}
	return ActiveAttribute.Get(false);
}

bool SVoxelMenuButton::IsWarmPlate() const
{
	return bPrimary || ActiveAttribute.Get(false);
}

FSlateColor SVoxelMenuButton::GetLeatherFillColour() const
{
	using namespace VoxelUITheme;
	if (IsWarmPlate())
	{
		// .se-act.primary, and its hover, which slides the whole plate one stop
		// warmer rather than only changing the label.
		return FSlateColor(IsLit() ? Tint(Mix(Gold, WarmPrimary)) : Tint(Mix(WarmPrimary, WarmSecondary)));
	}
	// linear-gradient(180deg,var(--leather-1),var(--leather-2)) in every state:
	// the leather plate's hover changes its BORDER and its label, not its fill.
	return FSlateColor(Tint(Mix(Leather1, Leather2), IsEnabled() ? 1.f : 0.5f));
}

FSlateColor SVoxelMenuButton::GetLeatherBorderColour() const
{
	using namespace VoxelUITheme;
	if (!IsEnabled())
	{
		return FSlateColor(Tint(IronDeep));
	}
	if (IsWarmPlate())
	{
		return FSlateColor(FLinearColor::Black); // .primary border-color:#000
	}
	if (IsLit())
	{
		return FSlateColor(Tint(bDanger ? DangerHover : WarmPrimary));
	}
	return FSlateColor(Tint(LeatherEdge));
}

FSlateColor SVoxelMenuButton::GetLeatherLabelColour() const
{
	using namespace VoxelUITheme;
	if (!IsEnabled())
	{
		return FSlateColor(Tint(Parchment, 0.25f));
	}
	if (IsWarmPlate())
	{
		// #fff8e0 at rest; the hover plate is bright enough that the label goes
		// to the parchment INK instead (.se-act.primary:hover).
		return FSlateColor(Tint(IsLit() ? ParchmentInk : OnWarm));
	}
	if (bDanger)
	{
		return FSlateColor(Tint(IsLit() ? DangerHover : DeleteRest));
	}
	return FSlateColor(Tint(IsLit() ? Gold : Parchment));
}

bool SVoxelMenuButton::IsSelected() const
{
	return ActiveAttribute.Get(false);
}

FSlateColor SVoxelMenuButton::GetTabFillColour() const
{
	using namespace VoxelUITheme;
	// linear-gradient(180deg, --panel-iron, #14100a) at rest; the oak plate
	// when this is the open screen. Both flattened by Mix, as everywhere else.
	static const FColor IronBottom(0x14, 0x10, 0x0a);
	return IsSelected() ? Tint(Mix(PanelOak1, PanelOak2)) : Tint(Mix(PanelIron, IronBottom));
}

FSlateColor SVoxelMenuButton::GetTabEdgeColour() const
{
	using namespace VoxelUITheme;
	return IsSelected() ? Tint(PanelOakEdge) : Tint(PanelIronEdge);
}

FSlateColor SVoxelMenuButton::GetTabLabelColour() const
{
	using namespace VoxelUITheme;
	if (!IsEnabled())
	{
		return Tint(InkMute, 0.5f);
	}
	// SELECTION BEATS HOVER. `.menu-tab.active` is gold whether or not the
	// pointer is on it, and `.menu-tab:hover` only lifts an inactive tab from
	// --ink-mute to --ink.
	if (IsSelected())
	{
		return Tint(Gold);
	}
	// The resting-state override, on GetLabelColour's own terms: applied only
	// when the chip is neither selected nor lit, so a hovered chip still goes
	// INK like every other one. The skill ladder uses it to paint an unlocked
	// node gold and a locked one dim while both sit unselected.
	if (TextColorOverride.IsSet() && !IsLit())
	{
		return FSlateColor(*TextColorOverride);
	}
	return IsLit() ? Tint(Ink) : Tint(InkMute);
}

FSlateColor SVoxelMenuButton::GetTabKeyColour() const
{
	using namespace VoxelUITheme;
	// `.menu-tab .key` is GOLD and `.menu-tab.active .key` is INK -- the key
	// glyph and the label swap colours when a tab opens, so the pair always
	// reads as two tones rather than one.
	if (!IsEnabled())
	{
		return Tint(InkMute, 0.5f);
	}
	return IsSelected() ? Tint(Ink) : Tint(Gold);
}

FSlateColor SVoxelMenuButton::GetTabUnderlineColour() const
{
	using namespace VoxelUITheme;
	// Fully transparent rather than collapsed: the rule occupies its 3 px in
	// every state, so a tab does not change height as it is selected.
	return IsSelected() ? Tint(Gold) : Tint(Gold, 0.f);
}

FSlateColor SVoxelMenuButton::GetPauseLabelColour() const
{
	using namespace VoxelUITheme;
	// .pa-btn.disabled is parchment @ 0.25 and does not respond to hover, which
	// falls out of testing enabled first.
	if (!IsEnabled())
	{
		return FSlateColor(Tint(Parchment, 0.25f));
	}
	if (bDanger)
	{
		return FSlateColor(Tint(IsLit() ? DangerHover : DangerRest));
	}
	return FSlateColor(Tint(IsLit() ? Gold : Parchment));
}

FSlateColor SVoxelMenuButton::GetPauseChevronColour() const
{
	using namespace VoxelUITheme;
	if (!IsEnabled())
	{
		return FSlateColor(Tint(LeatherEdge, 0.40f));
	}
	return FSlateColor(Tint(IsLit() ? Gold : WarmSecondary));
}

FMargin SVoxelMenuButton::GetPauseChevronPadding() const
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	// The gap to the label is constant; the slide is taken out of the LEFT so
	// the row's total width never changes as the chevron moves.
	const float Slide = IsLit() ? SVoxelMenuButtonDetail::kChevronSlidePx : 0.f;
	return FMargin(Slide, 0.f, L.PauseItemChevronGap - Slide, 0.f);
}

FSlateColor SVoxelMenuButton::GetCartoucheLabelColour() const
{
	using namespace VoxelUITheme;
	// .title-menu__item colours: parchment at rest; #fff5d0 lit; the QUIT item
	// parchment @ 0.45 at rest and plain INK on hover; disabled parchment @ 0.25.
	if (!IsEnabled())
	{
		return FSlateColor(Tint(Parchment, 0.25f));
	}
	if (bMuted)
	{
		return FSlateColor(IsHovered() ? Tint(Ink) : Tint(Parchment, 0.45f));
	}
	return FSlateColor(IsCartoucheOn() ? Tint(CartoucheText) : Tint(Parchment));
}

FSlateColor SVoxelMenuButton::GetCartoucheFillColour() const
{
	using namespace VoxelUITheme;
	return FSlateColor(IsCartoucheOn() ? Tint(Gold, CartoucheFillAlpha) : FLinearColor::Transparent);
}

FSlateColor SVoxelMenuButton::GetCartoucheRuleColour() const
{
	using namespace VoxelUITheme;
	return FSlateColor(IsCartoucheOn() ? Tint(Gold, GoldRuleAlpha) : FLinearColor::Transparent);
}

FSlateFontInfo SVoxelMenuButton::GetCartoucheFont() const
{
	FSlateFontInfo Font = FVoxelUIStyle::Get().Serif(IsCartoucheOn() ? ActiveFontSize : RestFontSize);
	Font.LetterSpacing = LetterSpacing;
	return Font;
}

TSharedPtr<SWidget> SVoxelMenuButton::GetFocusWidget() const
{
	return Button;
}

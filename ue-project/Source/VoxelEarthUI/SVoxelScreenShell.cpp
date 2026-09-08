#include "SVoxelScreenShell.h"

#include "SVoxelMenuButton.h"
#include "VoxelScreenShellSettings.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h" // the body's down-only fit -- see Construct
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelScreenShellDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// The tab order is the MOCK'S, which is not the order the porting brief lists
// the screens in. All five files carry the same <nav>: MAP, JOURNAL, INVENTORY,
// PLAYER, CODEX. The bar is the thing the player learns the position of, so it
// follows the screen rather than the document.
constexpr EVoxelScreenTab kTabOrder[] = {
	EVoxelScreenTab::Map,
	EVoxelScreenTab::Journal,
	EVoxelScreenTab::Inventory,
	EVoxelScreenTab::Player,
	EVoxelScreenTab::Codex,
};
constexpr int32 kTabCount = UE_ARRAY_COUNT(kTabOrder);

int32 IndexOf(EVoxelScreenTab Tab)
{
	for (int32 I = 0; I < kTabCount; ++I)
	{
		if (kTabOrder[I] == Tab)
		{
			return I;
		}
	}
	return 0;
}

// .menu-body's ring stack, outermost first: 2 px black border, then the CSS's
// `inset 0 0 0 1px --panel-oak-edge, inset 0 0 0 2px #0a0805, inset 0 0 0 4px
// --panel-oak-edge`. Four rings and a fill, which is five nested SOverlay slots
// -- the same stacking idiom VoxelOverlayChrome::Panel uses for the leather
// dialogs, at a different thickness.
// ADR-0011: no band may be one unit. The CSS's rings are 1 / 1 / 2 measured
// from the border inward, so the first two were exactly the hairlines the ADR
// forbids -- and they did not show up in a grep for `FMargin(1.f)` because they
// are named constants summed into the insets below. Two each; .menu-body's
// chrome grows by two units per side in total.
constexpr float kBodyBorderPx = 2.f;
constexpr float kBodyRing1Px = VoxelUITheme::RulePx;
constexpr float kBodyRing2Px = VoxelUITheme::RulePx;
constexpr float kBodyRing3Px = 2.f;
const FColor kBodyInnerDark(0x0a, 0x08, 0x05);
} // namespace SVoxelScreenShellDetail

SVoxelScreenShell::~SVoxelScreenShell()
{
	FVoxelUIStyle::UnregisterWidget();
}

FKey SVoxelScreenShell::TabKey(EVoxelScreenTab Tab)
{
	// M / J / I / P / K. I is the key the survival inventory already answers to
	// (UVoxelSurvivalUISubsystem binds it), which is why this screen takes it
	// over rather than choosing a free letter -- see VoxelScreensUISubsystem.
	// K rather than C for the codex: C is not free either way and the mock's
	// own hint strip does not name these keys, so the set is chosen here.
	switch (Tab)
	{
	case EVoxelScreenTab::Map:       return EKeys::M;
	case EVoxelScreenTab::Journal:   return EKeys::J;
	case EVoxelScreenTab::Inventory: return EKeys::I;
	case EVoxelScreenTab::Player:    return EKeys::P;
	case EVoxelScreenTab::Codex:     return EKeys::K;
	}
	return EKeys::I;
}

FText SVoxelScreenShell::TabKeyLabel(EVoxelScreenTab Tab)
{
	switch (Tab)
	{
	case EVoxelScreenTab::Map:       return VoxelUIStrings::ScreenKeyMap();
	case EVoxelScreenTab::Journal:   return VoxelUIStrings::ScreenKeyJournal();
	case EVoxelScreenTab::Inventory: return VoxelUIStrings::ScreenKeyInventory();
	case EVoxelScreenTab::Player:    return VoxelUIStrings::ScreenKeyPlayer();
	case EVoxelScreenTab::Codex:     return VoxelUIStrings::ScreenKeyCodex();
	}
	return FText::GetEmpty();
}

FText SVoxelScreenShell::TabLabel(EVoxelScreenTab Tab)
{
	switch (Tab)
	{
	case EVoxelScreenTab::Map:       return VoxelUIStrings::ScreenTabMap();
	case EVoxelScreenTab::Journal:   return VoxelUIStrings::ScreenTabJournal();
	case EVoxelScreenTab::Inventory: return VoxelUIStrings::ScreenTabInventory();
	case EVoxelScreenTab::Player:    return VoxelUIStrings::ScreenTabPlayer();
	case EVoxelScreenTab::Codex:     return VoxelUIStrings::ScreenTabCodex();
	}
	return FText::GetEmpty();
}

EVoxelScreenTab SVoxelScreenShell::TabAtOffset(EVoxelScreenTab From, int32 Delta)
{
	using namespace SVoxelScreenShellDetail;
	const int32 Index = (IndexOf(From) + Delta % kTabCount + kTabCount) % kTabCount;
	return kTabOrder[Index];
}

void SVoxelScreenShell::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	using namespace SVoxelScreenShellDetail;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	ActiveTab = InArgs._ActiveTab;
	Actions = InArgs._Actions;
	OnTabChanged = InArgs._OnTabChanged;
	OnClose = InArgs._OnClose;

	// .menu-body: an oak plate inside four rings. The `0 8px 0 -2px
	// rgba(0,0,0,.55)` drop shadow is drawn the way SVoxelMainMenu draws the
	// panel shadow -- an offset black box behind it -- because FSlateBrush has
	// no shadow parameter.
	TSharedRef<SWidget> Body =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(kBodyBorderPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge))
		]
		+ SOverlay::Slot().Padding(FMargin(kBodyBorderPx + kBodyRing1Px))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(kBodyInnerDark))
		]
		+ SOverlay::Slot().Padding(FMargin(kBodyBorderPx + kBodyRing1Px + kBodyRing2Px))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge))
		]
		+ SOverlay::Slot().Padding(FMargin(kBodyBorderPx + kBodyRing1Px + kBodyRing2Px + kBodyRing3Px))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(PanelOak1, PanelOak2)))
		]
		+ SOverlay::Slot().Padding(FMargin(L.ScreenBodyPad))
		[
			// THE BODY FITS THE FRAME; THE FRAME NO LONGER FITS THE BODY.
			//
			// This is the other half of unifying the five shell sizes. The
			// inventory's 8x8 pack plus its hotbar, filter row and head is a
			// little taller and wider than the authored 1060x760 leaves for a
			// body -- 16 x 28 units over, measured on the 2026-09-07 captures.
			// The old answer was to let the frame grow (see ShrinkWrap in the
			// header); the owner's answer is one size for all five.
			//
			// ScaleToFit + DownOnly is what makes that safe rather than a
			// clipping bet. A body that fits is untouched -- DownOnly can only
			// ever return a factor of 1 for the four screens that already fit,
			// so this changes nothing about them. A body that does not fit is
			// drawn uniformly smaller instead of losing a pack row off the
			// bottom, which is the failure the earlier fixed-size draft was
			// abandoned over. No reflow either: the content keeps its authored
			// layout and is scaled, so the inventory looks like itself, only
			// fractionally smaller than the other four.
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			.StretchDirection(EStretchDirection::DownOnly)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				InArgs._Body.Widget
			]
		];

	// .menu-body::before -- a bronze stud inset 8 px at each corner. Four small
	// boxes rather than the CSS's four radial gradients, which is the same
	// substitution VoxelOverlayChrome::Diamond makes and for the same reason.
	TSharedRef<SOverlay> BodyWithStuds = SNew(SOverlay) + SOverlay::Slot()[Body];
	const EHorizontalAlignment HAligns[] = {HAlign_Left, HAlign_Right, HAlign_Left, HAlign_Right};
	const EVerticalAlignment VAligns[] = {VAlign_Top, VAlign_Top, VAlign_Bottom, VAlign_Bottom};
	for (int32 I = 0; I < 4; ++I)
	{
		BodyWithStuds->AddSlot()
		.HAlign(HAligns[I])
		.VAlign(VAligns[I])
		.Padding(FMargin(L.ScreenBodyStudInset))
		[
			SNew(SBox)
			.WidthOverride(L.ScreenBodyStudSize)
			.HeightOverride(L.ScreenBodyStudSize)
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Bronze))
			]
		];
	}

	TSharedRef<SVerticalBox> Column =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			BuildTabBar()
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			BodyWithStuds
		];

	if (Actions.Num() > 0)
	{
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, L.ActionBarTopGap, 0.f, 0.f))
		[
			BuildActionBar()
		];
	}

	// ONE SIZE, ALWAYS, AND InArgs._ShrinkWrap IS IGNORED. Owner directive,
	// 2026-09-07: *"it currently appears that the inventory UI menu size is
	// slightly larger than the default sizing for map, journal, player, and
	// codex sections. unify this."* The flag used to drop these two overrides
	// for the inventory alone. See the header for the measurement and for why
	// the argument still exists.
	//
	// The earlier draft's objection -- that a fixed height would clip a pack row
	// -- is answered on the CONTENT side now, by the down-only fit around the
	// body above. Nothing here can clip.
	TSharedRef<SBox> Frame =
		SNew(SBox)
		.WidthOverride(L.ScreenShellWidth)
		.HeightOverride(L.ScreenShellHeight)
		.Padding(FMargin(L.ScreenShellPadX, L.ScreenShellPadTop, L.ScreenShellPadX, L.ScreenShellPadBottom))
		// THE PLAYER'S MENU SIZE DIAL. A render transform about the frame's
		// centre, read fresh every paint, so the Settings row and Ctrl+wheel
		// are the same control and neither has to tell the other. The pivot is
		// the frame's own centre, so the shell grows and shrinks in place
		// inside the centring box below rather than walking off one corner.
		.RenderTransform(this, &SVoxelScreenShell::GetShellRenderTransform)
		.RenderTransformPivot(FVector2D(0.5f, 0.5f))
		[
			Column
		];

	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Frame
		]
	];
}

TSharedRef<SWidget> SVoxelScreenShell::BuildTabBar()
{
	using namespace VoxelUITheme;
	using namespace SVoxelScreenShellDetail;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 I = 0; I < kTabCount; ++I)
	{
		const EVoxelScreenTab Tab = kTabOrder[I];
		TSharedPtr<SVoxelMenuButton> Button;
		Row->AddSlot().AutoWidth()
		// margin-right:-2px: adjacent tabs share one 2 px border rather than
		// stacking two. Slate honours a negative slot margin, so the CSS
		// translates directly.
		.Padding(FMargin(0.f, 0.f, I == kTabCount - 1 ? 0.f : -kBodyBorderPx, 0.f))
		[
			SAssignNew(Button, SVoxelMenuButton)
			.Text(TabLabel(Tab))
			.KeyLabel(TabKeyLabel(Tab))
			.Variant(EVoxelMenuButtonVariant::Tab)
			.FontSize(L.TabFontSize)
			.LetterSpacing(L.TabLetterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this, Tab]() { return ActiveTab == Tab; })
			.OnClicked_Lambda([this, Tab]()
			{
				if (Tab != ActiveTab)
				{
					OnTabChanged.ExecuteIfBound(Tab);
				}
				return FReply::Handled();
			})
		];
		TabButtons.Add(Button);
	}

	// .menu-spacer -- the bar stops at the last tab and a bare 2 px rule runs
	// on to the right edge, so the row reads as a set of tabs on a shelf rather
	// than as a full-width strip.
	Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Bottom)
	[
		SNew(SBox)
		.HeightOverride(L.TabBarRulePx)
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
	];

	return SNew(SBox)
		.Padding(FMargin(L.TabBarPadLeft, 0.f, 0.f, 0.f))
		[
			Row
		];
}

TSharedRef<SWidget> SVoxelScreenShell::BuildActionBar() const
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (const FVoxelScreenAction& Action : Actions)
	{
		if (Action.bSpacerBefore)
		{
			Row->AddSlot().FillWidth(1.f)[SNullWidget::NullWidget];
		}

		TSharedRef<SHorizontalBox> Hint = SNew(SHorizontalBox);
		if (!Action.Key.IsEmpty())
		{
			// .action kbd -- a gold letter on an iron plate with a 2 px black
			// border and a 2 px black drop rule. The drop rule is the bottom of
			// the border box, so the cap is drawn as a bordered box like every
			// other plate in this front end.
			Hint->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, L.ActionKeyGap, 0.f))
			[
				SNew(SBox)
				.MinDesiredWidth(L.ActionKeyMinWidth)
				.HeightOverride(L.ActionKeyHeight)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
					]
					+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelIronEdge))
					]
					+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelIron))
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(FMargin(L.ActionKeyPadX, 0.f))
					[
						SNew(STextBlock)
						.Text(Action.Key)
						.Font(Style.Mono(L.ActionKeyFontSize))
						.ColorAndOpacity(Tint(Action.bDanger ? HpBright : Gold))
					]
				]
			];
		}
		Hint->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Action.Label)
			.Font(Style.Mono(L.ActionBarFontSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];

		Row->AddSlot().AutoWidth().Padding(FMargin(0.f, 0.f, L.ActionBarGap, 0.f))[Hint];
	}
	return Row;
}

void SVoxelScreenShell::FocusDefaultWidget()
{
	const int32 Index = SVoxelScreenShellDetail::IndexOf(ActiveTab);
	if (TabButtons.IsValidIndex(Index) && TabButtons[Index].IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = TabButtons[Index]->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

TOptional<FSlateRenderTransform> SVoxelScreenShell::GetShellRenderTransform() const
{
	const float Scale = VoxelScreenShellSettings::GetScale();
	if (FMath::IsNearlyEqual(Scale, 1.f, UE_KINDA_SMALL_NUMBER))
	{
		// UNSET AT 1.00, not an identity transform. A widget with no render
		// transform takes Slate's plain path; one with an identity transform
		// still allocates a transform, still fails the renderer's is-identity
		// fast paths, and -- the part that matters here -- would make the
		// default case behave differently from the codebase's other widgets for
		// no visible gain. At the shipped default this widget is exactly what
		// it was before the dial existed.
		return TOptional<FSlateRenderTransform>();
	}
	return FSlateRenderTransform(FScale2D(Scale, Scale));
}

FReply SVoxelScreenShell::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	// CTRL AND ONLY CTRL. A bare wheel has to keep scrolling the codex list, the
	// journal entries and the perk detail; the modifier is what makes a resize
	// gesture and a scroll gesture distinguishable over the same pixels.
	if (!MouseEvent.IsControlDown())
	{
		return SCompoundWidget::OnMouseWheel(Geometry, MouseEvent);
	}

	const float Delta = MouseEvent.GetWheelDelta();
	if (FMath::IsNearlyZero(Delta))
	{
		return FReply::Unhandled();
	}
	// One notch, one stop, whatever the platform reports as a notch's magnitude
	// -- the sign is the only part of GetWheelDelta this trusts. Stepping by the
	// raw delta would give a different size per mouse.
	VoxelScreenShellSettings::StepScale(Delta > 0.f ? +1 : -1);
	return FReply::Handled();
}

FReply SVoxelScreenShell::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();

	// CTRL + PLUS / MINUS / ZERO, TESTED FIRST, and first for a reason: the
	// unmodified letters below are tab switches, and a Ctrl chord that fell
	// through to them would page the screen instead of resizing it.
	//
	// THREE SPELLINGS OF EACH SIGN, because a keyboard has more than one. The
	// main row's `+` arrives as EKeys::Equals with shift held (Slate reports the
	// physical key, not the shifted character), `-` as EKeys::Hyphen, and the
	// numpad has its own pair. A player who finds one and not the other reads
	// the feature as broken.
	if (KeyEvent.IsControlDown())
	{
		if (Key == EKeys::Equals || Key == EKeys::Add)
		{
			VoxelScreenShellSettings::StepScale(+1);
			return FReply::Handled();
		}
		if (Key == EKeys::Hyphen || Key == EKeys::Underscore || Key == EKeys::Subtract)
		{
			VoxelScreenShellSettings::StepScale(-1);
			return FReply::Handled();
		}
		if (Key == EKeys::Zero || Key == EKeys::NumPadZero)
		{
			// Back to the authored size. Every dial that can be moved by
			// accident needs a way back that does not require remembering what
			// it was.
			VoxelScreenShellSettings::SetScale(VoxelScreenShellSettings::ScaleDefault());
			return FReply::Handled();
		}
	}

	if (Key == EKeys::Escape || Key == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		OnClose.ExecuteIfBound();
		return FReply::Handled();
	}

	// Q / E and the shoulder buttons page between tabs, which is what the map
	// mock's own hint strip promises for its rotate keys and what every KCD2-
	// idiom tab bar does. The map re-uses Q/E for rotation and therefore
	// consumes them before they reach here.
	if (Key == EKeys::Q || Key == EKeys::Gamepad_LeftShoulder)
	{
		OnTabChanged.ExecuteIfBound(TabAtOffset(ActiveTab, -1));
		return FReply::Handled();
	}
	if (Key == EKeys::E || Key == EKeys::Gamepad_RightShoulder)
	{
		OnTabChanged.ExecuteIfBound(TabAtOffset(ActiveTab, +1));
		return FReply::Handled();
	}

	// The five direct keys, so the screen the player is on is reachable from
	// any other without paging. A press on the OPEN screen's own key closes the
	// stack, which is what makes I a toggle rather than a one-way door.
	for (int32 I = 0; I < SVoxelScreenShellDetail::kTabCount; ++I)
	{
		const EVoxelScreenTab Tab = SVoxelScreenShellDetail::kTabOrder[I];
		if (Key == TabKey(Tab))
		{
			if (Tab == ActiveTab)
			{
				OnClose.ExecuteIfBound();
			}
			else
			{
				OnTabChanged.ExecuteIfBound(Tab);
			}
			return FReply::Handled();
		}
	}

	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

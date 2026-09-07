#include "SVoxelScreenShell.h"

#include "SVoxelMenuButton.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
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
constexpr float kBodyBorderPx = 2.f;
constexpr float kBodyRing1Px = 1.f;
constexpr float kBodyRing2Px = 1.f;
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
			InArgs._Body.Widget
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

	// SHRINK-WRAP MEANS NO OVERRIDE AT ALL, which is what `width:max-content`
	// on the inventory mock's .menu-shell actually says. An earlier draft gave
	// it a second FIXED size and that was wrong by arithmetic: the 8x8 pack is
	// 8 x 58 px of slot plus its gaps, well padding and borders -- about 570 px
	// of body before the tab bar and action bar are added -- so any height that
	// also fitted the other four screens would have clipped a row off the pack.
	// Sizing to content cannot clip, and the centring box above keeps it on
	// screen.
	TSharedRef<SBox> Frame =
		SNew(SBox)
		.Padding(FMargin(L.ScreenShellPadX, L.ScreenShellPadTop, L.ScreenShellPadX, L.ScreenShellPadBottom))
		[
			Column
		];
	if (!InArgs._ShrinkWrap)
	{
		Frame->SetWidthOverride(L.ScreenShellWidth);
		Frame->SetHeightOverride(L.ScreenShellHeight);
	}

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

FReply SVoxelScreenShell::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();

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

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
		+ SOverlay::Slot()
		[
			// .menu-body's own padding, on an SBox rather than on the slot
			// above so it can be an ATTRIBUTE: it is one of the three paddings
			// the shell trims when the viewport is too short for the authored
			// frame (FVoxelMenuLayout::ShellChromeForViewport). Layout-identical
			// to a slot padding -- a box pads its child either way -- and this
			// form has an attribute overload that a slot's does not need to
			// grow for one caller.
			SNew(SBox)
			.Padding_Lambda([this]()
			{
				const FVoxelShellChrome Chrome = CurrentChrome();
				return FMargin(Chrome.BodyPadX, Chrome.BodyPadY);
			})
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
				// clipping bet. A body that does not fit is drawn uniformly smaller
				// instead of losing a pack row off the bottom, which is the failure
				// the earlier fixed-size draft was abandoned over. No reflow either:
				// the content keeps its authored layout and is scaled, so the
				// inventory looks like itself, only fractionally smaller.
				//
				// "DOWNONLY CAN ONLY EVER RETURN 1 FOR THE FOUR SCREENS THAT ALREADY
				// FIT" -- THAT SENTENCE STOOD HERE AND IT WAS FALSE. It was derived,
				// never measured, and the owner measured it for us the same night:
				// *"journal UI elements within that pane look too small now"*. A
				// ScaleBox decides its factor from the child's DESIRED size, and a
				// desired size is what a widget ASKS for, not what it needs. An
				// auto-wrapping STextBlock with no width asks for its longest line
				// unwrapped, and a vertical SScrollBox asks for its whole content
				// height. The journal's parchment page did both, so it asked for
				// ~1603 units against the 988 here and was drawn at 0.62.
				//
				// SO THE STANDING RULE IS ON THE SCREENS, NOT HERE: a screen body
				// must bound its own fluid columns -- a fixed width on a text
				// column, MaxDesiredHeight(0) on a scroll region -- so that whatever
				// desired size reaches this box is a size the screen genuinely
				// needs. This box then scales for RIGIDITY only, which is the
				// inventory's fixed 8x8 pack and nothing else. See
				// SVoxelJournalScreen::Construct for the worked case.
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::DownOnly)
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					InArgs._Body.Widget
				]
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
		Column->AddSlot().AutoHeight()
		[
			// The gap above the action bar, as an attribute for the reason the
			// body's padding above is one: it is trimmed when the frame is.
			SNew(SBox)
			.Padding_Lambda([this]()
			{
				return FMargin(0.f, CurrentChrome().ActionBarTopGap, 0.f, 0.f);
			})
			[
				BuildActionBar()
			]
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
	// The shell's padding moved off the frame box and onto this inner one so the
	// grip can sit in the corner of the frame ITSELF rather than inside the
	// padding's content box. Same layout: an SBox's padding pads its child
	// either way.
	TSharedRef<SOverlay> FrameStack =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBox)
			.Padding_Lambda([this]()
			{
				const FVoxelShellChrome Chrome = CurrentChrome();
				return FMargin(Chrome.PadX, Chrome.PadTop, Chrome.PadX, Chrome.PadBottom);
			})
			[
				Column
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.ShellGripInset))
		[
			// 3 + 16 = 19 units in from each edge, against 18 units of shell
			// padding -- so the gripper sits in the corner of the padding and
			// stops one unit short of the action bar's last hint rather than
			// over it.
			BuildResizeGrip()
		];

	// THE FRAME'S HEIGHT IS NOT A CONSTANT ANY MORE (ADR-0011 decision 4). It is
	// the authored 760 whenever the viewport has room for it, which is every
	// 16:9 display at INTERFACE SIZE 1.00 and Menu Size up to 1.35 -- and less
	// when it does not, because a frame drawn past the edge of the screen is
	// worse than a frame with tighter chrome. See ShellChromeForViewport for
	// what makes the room shrink, and for the measurement showing it is not the
	// resolution.
	//
	// THE WIDTH IS STILL A CONSTANT, deliberately: the five screens bound their
	// columns to ScreenBodyContentWidth() at Construct, so a live width would
	// have to reach them too. The gap is written down where the trim is decided.
	TSharedRef<SBox> Frame =
		SNew(SBox)
		.WidthOverride(L.ScreenShellWidth)
		.HeightOverride_Lambda([this]()
		{
			return FOptionalSize(CurrentChrome().ShellHeight);
		})
		// THE PLAYER'S MENU SIZE DIAL. A render transform about the frame's
		// centre, read fresh every paint, so the Settings row, Ctrl+wheel and
		// the corner drag are the same control and none has to tell the others.
		// The pivot is the frame's own centre, so the shell grows and shrinks in
		// place inside the centring box below rather than walking off one
		// corner -- which is also what lets the hit region below be arithmetic
		// on this widget's centre instead of a lookup of where the frame landed.
		.RenderTransform(this, &SVoxelScreenShell::GetShellRenderTransform)
		.RenderTransformPivot(FVector2D(0.5f, 0.5f))
		[
			FrameStack
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

TSharedRef<SWidget> SVoxelScreenShell::BuildResizeGrip()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// A 3x3 lattice with the anti-diagonal and everything below it drawn: six
	// squares in the shape of a stair, which is the resize gripper every desktop
	// window corner has worn since the nineties. Squares rather than the usual
	// diagonal rules because ADR-0011 forbids a one-unit band and a rule thin
	// enough to read as a gripper line would be exactly that; a 4-unit square on
	// a 2-unit gap says the same thing and survives any scale factor.
	TSharedRef<SVerticalBox> Stair = SNew(SVerticalBox);
	for (int32 Row = 0; Row < 3; ++Row)
	{
		TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox);
		for (int32 Col = 0; Col < 3; ++Col)
		{
			TSharedRef<SWidget> Cell = SNullWidget::NullWidget;
			if (Row + Col >= 2)
			{
				Cell = SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity(this, &SVoxelScreenShell::GripColour);
			}
			Line->AddSlot().AutoWidth()
			.Padding(FMargin(0.f, 0.f, Col == 2 ? 0.f : L.ShellGripGap, 0.f))
			[
				SNew(SBox)
				.WidthOverride(L.ShellGripDot)
				.HeightOverride(L.ShellGripDot)
				[
					Cell
				]
			];
		}
		Stair->AddSlot().AutoHeight()
		.Padding(FMargin(0.f, 0.f, 0.f, Row == 2 ? 0.f : L.ShellGripGap))
		[
			Line
		];
	}

	// HIT-TEST INVISIBLE, ON PURPOSE. The grip is a picture of the hit region,
	// not the hit region: ResizeZoneAt owns that, because the two EDGES have
	// nothing drawn on them and a widget cannot be the target for a band that
	// has no widget. One authority for where the shell can be grabbed.
	return SNew(SBox)
		.Visibility(EVisibility::HitTestInvisible)
		[
			Stair
		];
}

FSlateColor SVoxelScreenShell::GripColour() const
{
	using namespace VoxelUITheme;
	// Lit while the pointer is anywhere on the resize band, not only on the
	// gripper itself -- so the corner answers for the edges too and the player
	// learns that the whole rim is draggable without having to find it twice.
	const bool bHot = DragZone != EResizeZone::None || HoverZone != EResizeZone::None;
	return Tint(bHot ? Gold : Bronze);
}

float SVoxelScreenShell::LiveScale() const
{
	return DragScale.IsSet() ? DragScale.GetValue() : VoxelScreenShellSettings::GetScale();
}

FVoxelShellChrome SVoxelScreenShell::CurrentChrome() const
{
	// GetTickSpaceGeometry AND NOT GetPaintSpaceGeometry: the paint geometry
	// carries the Menu Size render transform, and asking how much room the
	// frame has in a space the frame has already been scaled into would feed the
	// scale back into itself. The tick space is the layout space -- the viewport
	// with the engine's DPI scale and the player's INTERFACE SIZE divided out,
	// which is exactly the space ShellChromeForViewport wants.
	//
	// ZERO BEFORE THE FIRST ARRANGE, and ShellChromeForViewport answers that
	// with the authored chrome rather than with the smallest one.
	return FVoxelMenuLayout::Get().ShellChromeForViewport(
		FVector2D(GetTickSpaceGeometry().GetLocalSize()), LiveScale());
}

SVoxelScreenShell::EResizeZone SVoxelScreenShell::ResizeZoneAt(const FVector2D& LocalPos,
                                                               const FVector2D& LocalSize,
                                                               float Scale)
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// The frame is centred in this widget and scaled about its own centre, so
	// its painted half-extent is its half-extent times the scale. That is the
	// whole geometry of the hit region -- no cached arranged rectangle, nothing
	// to keep in step with a relayout.
	//
	// THE HEIGHT COMES FROM THE CHROME, not from ScreenShellHeight: the frame is
	// trimmed when the viewport is too short for the authored one, and the two
	// disagree at exactly the sizes a player is most likely to be dragging.
	const FVoxelShellChrome Chrome = L.ShellChromeForViewport(LocalSize, Scale);
	const double HalfW = double(Chrome.ShellWidth) * 0.5 * double(Scale);
	const double HalfH = double(Chrome.ShellHeight) * 0.5 * double(Scale);
	const double CentreX = LocalSize.X * 0.5;
	const double CentreY = LocalSize.Y * 0.5;
	const double Right = CentreX + HalfW;
	const double Bottom = CentreY + HalfH;

	// UNSCALED, see ShellResizeEdge: the band is a constant number of screen
	// pixels at every Menu Size, which is what a hit target wants to be.
	const double Band = double(L.ShellResizeEdge);

	// The band's own slack is added at the far ends of each edge so the corner
	// is reachable from outside the frame as well as inside it -- a player
	// aiming at a corner overshoots it, and a target that only exists on the
	// inside reads as one that does not work.
	const bool bOnRight = FMath::Abs(LocalPos.X - Right) <= Band
	                   && LocalPos.Y >= CentreY - HalfH - Band
	                   && LocalPos.Y <= Bottom + Band;
	const bool bOnBottom = FMath::Abs(LocalPos.Y - Bottom) <= Band
	                    && LocalPos.X >= CentreX - HalfW - Band
	                    && LocalPos.X <= Right + Band;

	if (bOnRight && bOnBottom)
	{
		return EResizeZone::Corner;
	}
	if (bOnRight)
	{
		return EResizeZone::RightEdge;
	}
	if (bOnBottom)
	{
		return EResizeZone::BottomEdge;
	}
	return EResizeZone::None;
}

float SVoxelScreenShell::ResizeRatioAt(EResizeZone Zone, const FVector2D& LocalPos, const FVector2D& LocalSize)
{
	// THE AUTHORED HALF-EXTENT, ON PURPOSE, AND NOT THE TRIMMED ONE. This is a
	// drag MAPPING, not a hit region: the gesture is the difference of two of
	// these readings, so all it needs is a denominator that does not move
	// during the drag. The trimmed height does move -- it is a function of the
	// scale, which is what the drag is changing -- and feeding it in here would
	// make the shell accelerate away from the cursor at the sizes where the
	// trim engages. ResizeZoneAt is the one that must follow the painted frame.
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const double HalfW = FMath::Max(double(L.ScreenShellWidth) * 0.5, 1.0);
	const double HalfH = FMath::Max(double(L.ScreenShellHeight) * 0.5, 1.0);
	const double RatioX = FMath::Abs(LocalPos.X - LocalSize.X * 0.5) / HalfW;
	const double RatioY = FMath::Abs(LocalPos.Y - LocalSize.Y * 0.5) / HalfH;

	switch (Zone)
	{
	case EResizeZone::RightEdge:  return float(RatioX);
	case EResizeZone::BottomEdge: return float(RatioY);
	// THE DOMINANT AXIS, not the diagonal distance. A corner drag has two
	// answers and the player is watching one of them; taking the larger means
	// the corner never falls behind the cursor on either axis, and because the
	// shell scales uniformly the other axis follows for free.
	case EResizeZone::Corner:     return float(FMath::Max(RatioX, RatioY));
	default:                      break;
	}
	return 1.f;
}

void SVoxelScreenShell::CommitResizeDrag()
{
	if (DragScale.IsSet())
	{
		// One write, one flush, one `VoxelScreenShell: scale %.2f` line, at the
		// end of the gesture. SetScale already declines to do any of the three
		// when the value has not moved, so a drag that ended where it started is
		// silent rather than confirming a change that did not happen.
		VoxelScreenShellSettings::SetScale(DragScale.GetValue());
	}
	DragScale.Reset();
	DragZone = EResizeZone::None;
}

FCursorReply SVoxelScreenShell::OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& CursorEvent) const
{
	const FVector2D Local = Geometry.AbsoluteToLocal(CursorEvent.GetScreenSpacePosition());
	const FVector2D Size = Geometry.GetLocalSize();

	// The LATCHED zone wins while a drag is in flight, so the cursor does not
	// flicker back to an arrow the moment the pointer leaves the band it
	// grabbed -- which it does immediately, since dragging is what moves it.
	const EResizeZone Zone = DragZone != EResizeZone::None
		? DragZone
		: ResizeZoneAt(Local, Size, LiveScale());

	switch (Zone)
	{
	case EResizeZone::Corner:     return FCursorReply::Cursor(EMouseCursor::ResizeSouthEast);
	case EResizeZone::RightEdge:  return FCursorReply::Cursor(EMouseCursor::ResizeLeftRight);
	case EResizeZone::BottomEdge: return FCursorReply::Cursor(EMouseCursor::ResizeUpDown);
	default:                      break;
	}
	// UNHANDLED, not "the default cursor". A reply here would claim the cursor
	// for the whole shell and override whatever a child asks for over its own
	// pixels; the query walks leaf-to-root and stops at the first answer, so
	// declining is how the rest of the screen keeps its own.
	return FCursorReply::Unhandled();
}

FReply SVoxelScreenShell::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return SCompoundWidget::OnMouseButtonDown(Geometry, MouseEvent);
	}

	const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D Size = Geometry.GetLocalSize();
	const EResizeZone Zone = ResizeZoneAt(Local, Size, LiveScale());
	if (Zone == EResizeZone::None)
	{
		// Every other click on the shell's empty area still falls through to the
		// base class, which is what keeps this widget holding focus -- see the
		// z-order note in VoxelScreensUISubsystem for why that matters.
		return SCompoundWidget::OnMouseButtonDown(Geometry, MouseEvent);
	}

	DragZone = Zone;
	DragGrabScale = VoxelScreenShellSettings::GetScale();
	DragGrabRatio = ResizeRatioAt(Zone, Local, Size);
	DragScale = DragGrabScale;

	// THE CAPTURE IS NOT OPTIONAL. Without it the drag ends the moment the
	// pointer crosses out of the frame, which for a grow gesture is instantly.
	return FReply::Handled().CaptureMouse(SharedThis(this)).PreventThrottling();
}

FReply SVoxelScreenShell::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D Size = Geometry.GetLocalSize();

	if (DragZone != EResizeZone::None && HasMouseCapture())
	{
		// ADDITIVE, NOT ABSOLUTE. The grabbed edge keeps whatever offset it was
		// grabbed at: a player who catches the band a few pixels inside the edge
		// does not want the shell to jump so the edge lands under the cursor.
		// Snapped through the settings module's own funnel so that what is drawn
		// during the drag is exactly what will be stored on release -- a preview
		// that lands somewhere else on the way down is its own small lie.
		const float Ratio = ResizeRatioAt(DragZone, Local, Size);
		DragScale = VoxelScreenShellSettings::SnapScale(DragGrabScale + (Ratio - DragGrabRatio));
		return FReply::Handled();
	}

	HoverZone = ResizeZoneAt(Local, Size, LiveScale());
	return SCompoundWidget::OnMouseMove(Geometry, MouseEvent);
}

FReply SVoxelScreenShell::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (DragZone != EResizeZone::None && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		CommitResizeDrag();
		return FReply::Handled().ReleaseMouseCapture();
	}
	return SCompoundWidget::OnMouseButtonUp(Geometry, MouseEvent);
}

void SVoxelScreenShell::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	HoverZone = EResizeZone::None;
	SCompoundWidget::OnMouseLeave(MouseEvent);
}

void SVoxelScreenShell::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	// Reached twice on the ordinary path -- ReleaseMouseCapture above lands here
	// after CommitResizeDrag has already run -- which is why the commit clears
	// its own state and is safe to call again.
	CommitResizeDrag();
	SCompoundWidget::OnMouseCaptureLost(CaptureLostEvent);
}

TOptional<FSlateRenderTransform> SVoxelScreenShell::GetShellRenderTransform() const
{
	const float Scale = LiveScale();
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

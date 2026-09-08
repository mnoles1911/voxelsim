#include "SVoxelMainMenu.h"

#include "SVoxelCoverImage.h"
#include "SVoxelMenuButton.h"
#include "SVoxelSettingsPanel.h"
#include "VoxelEarthUI.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelMainMenuDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Maps the panel enum onto the switcher's slot indices. Kept next to the
// switcher's construction order so the two cannot drift.
int32 PanelIndex(EVoxelMenuPanel Panel)
{
	switch (Panel)
	{
	case EVoxelMenuPanel::MainColumn: return 0;
	case EVoxelMenuPanel::Load:       return 1;
	case EVoxelMenuPanel::Help:       return 2;
	case EVoxelMenuPanel::Credits:    return 3;
	case EVoxelMenuPanel::Settings:   return 4;
	}
	return 0;
}
} // namespace SVoxelMainMenuDetail

SVoxelMainMenu::~SVoxelMainMenu()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelMainMenu::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnContinue = InArgs._OnContinue;
	OnNewGame = InArgs._OnNewGame;
	OnQuit = InArgs._OnQuit;
	OnLoadSave = InArgs._OnLoadSave;
	OnDeleteSave = InArgs._OnDeleteSave;

	// MainMenu.gd re-scans the background folder and picks at random on every
	// launch, so dropping a new image in needs no code change. Shuffling the
	// library's order and taking entry 0 reproduces that, and leaves the
	// rotation in a state the loading screen can walk forward from.
	FRandomStream Stream = MakeVoxelUIRandomStream();
	FVoxelUIAssetLibrary::Get().ShuffleOrder(Stream);
	BackgroundIndex = 0;

	// LAYER ORDER, straight from MainMenu.gd::_ready:
	//   0  backdrop      #0a0a0f (the .tscn's ColorRect, not the palette's
	//                    BG_NIGHT -- see VoxelUITheme::MenuBackdrop)
	//   1  background art (added in step 3; the slot exists now so the layer
	//                    order is settled before anything is drawn into it)
	//   2  tint          BG_NIGHT @ 0.55
	//   3  panels        exactly one visible
	//   4  version stamp bottom-left
	ChildSlot
	[
		SNew(SOverlay)

		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(Tint(MenuBackdrop)))
		]

		+ SOverlay::Slot()
		[
			SNew(SVoxelCoverImage)
			.Brush(this, &SVoxelMainMenu::GetBackgroundBrush)
		]

		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(this, &SVoxelMainMenu::GetBackgroundTint)
		]

		// THE SWITCHER FILLS THE SCREEN NOW (2026-09-07). The title screen places
		// its logo, menu and callout against the frame edges, so its panel has
		// to be given the whole viewport; the sub-panels still want to be
		// centred, and each one centres ITSELF below rather than inheriting a
		// centring that the main column can no longer share.
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SAssignNew(PanelSwitcher, SWidgetSwitcher)
			// SLOT ORDER IS THE ENUM ORDER. SVoxelMainMenuDetail::PanelIndex
			// maps between them and lives next to this list for that reason.
			+ SWidgetSwitcher::Slot()
			[
				BuildMainColumn()
			]
			+ SWidgetSwitcher::Slot()
			[
				// Not wrapped in a centring SBox like its siblings: the overlay
				// dialogs centre themselves, so that the same widget centres the
				// same way over the paused world.
				BuildLoadPanel()
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					BuildMessagePanel(EVoxelMenuPanel::Help, VoxelUIStrings::HelpPanelTitle(), VoxelUIStrings::HelpPanelBody())
				]
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					BuildMessagePanel(EVoxelMenuPanel::Credits, VoxelUIStrings::CreditsPanelTitle(), VoxelUIStrings::CreditsPanelBody())
				]
			]
			+ SWidgetSwitcher::Slot()
			[
				// The 2026-09-07 SETTINGS overlay, which centres itself for the
				// same reason the LOAD dialog above it does. Rows still arrive
				// only through the A/B-plus-owner-verdict pipeline -- see
				// VoxelGraphicsUserSettings.h for the doctrine.
				BuildSettingsPanel()
			]
		]

		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.VersionInsetLeft, 0.f, 0.f, L.VersionInsetBottom))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::VersionStamp())
			.Font(Style.Serif(L.VersionFontSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		]
	];

	ShowPanel(EVoxelMenuPanel::MainColumn);
}

TSharedRef<SWidget> SVoxelMainMenu::BuildMainColumn()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// MainMenu.gd: _main_panel is a Control at PRESET_CENTER with offsets
	// (-260,-400)-(260,400), holding a VBoxContainer with separation 14 and
	// ALIGNMENT_CENTER. Slate has no inter-slot separation property, so the
	// gap is expressed as symmetric per-slot padding of half the separation --
	// which sums to exactly 14 between neighbours and adds 7 at the ends, a
	// difference invisible inside a centred, fixed-height box.
	// THE 2026-09-07 TITLE SCREEN (Voxelmark Main Menu.html). Three groups
	// pinned to the frame: the hero logo top-RIGHT, the menu list hanging
	// under it right-aligned, and a patch-notes callout top-LEFT. The centred
	// oak column this function used to build is gone from THIS panel; the
	// sub-panels keep their oak chrome until their own mocks land.
	//
	// The list uses the same SVoxelMenuButton the oak panels do, in its
	// Cartouche variant -- so every line of focus and navigation below is
	// unchanged, which is the point of making the chrome a flag.
	const float HalfSep = L.TitleMenuGap * 0.5f;
	const FMargin SlotPad(0.f, HalfSep);

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	TSharedPtr<SVoxelMenuButton> NewGameButton;
	TSharedPtr<SVoxelMenuButton> SettingsButton;
	TSharedPtr<SVoxelMenuButton> HelpButton;
	TSharedPtr<SVoxelMenuButton> CreditsButton;
	TSharedPtr<SVoxelMenuButton> QuitButton;

	// .title-logo__name: 132 px serif, 6 px tracking, --shadow-emboss-lg
	// (0 4px 0 black @ .55 plus a 28 px blur the port does not fake).
	FSlateFontInfo LogoFont = Style.Serif(L.LogoFontSize);
	LogoFont.LetterSpacing = L.LogoLetterSpacing;
	TSharedRef<SWidget> Logo =
		SNew(STextBlock)
		.Text(VoxelUIStrings::Title())
		.Font(LogoFont)
		.ColorAndOpacity(FSlateColor(Tint(InkBright)))
		.ShadowOffset(FVector2D(0.f, 4.f))
		.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.55f))
		.Justification(ETextJustify::Right);

	// .callout-news: a 64 px glyph box, a bordered column of tag / italic
	// title / copy, bounded to 520 wide.
	FSlateFontInfo TagFont = Style.Serif(L.CalloutTagSize);
	TagFont.LetterSpacing = 187; // 3 px at 16 px
	TSharedRef<SWidget> Callout =
		SNew(SBox)
		.MaxDesiredWidth(L.CalloutMaxWidth)
		[
			SNew(SHorizontalBox)
			// Left rule, then content, then right rule: the mock's
			// border-left/border-right at gold @ .35.
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(VoxelUITheme::RulePx)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(Gold, 0.35f)))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.CalloutPadX, 4.f, 0.f, 0.f)).VAlign(VAlign_Top)
			[
				SNew(SBox).WidthOverride(L.CalloutGlyphSize).HeightOverride(L.CalloutGlyphSize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(Gold, 0.5f)))
					]
					+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(PanelIron)))
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						// .callout-news__glyph holds U+2726, a four-pointed star.
						// None of the four shipped faces has it, and what the
						// 2026-09-07 capture showed in its place was Slate's
						// last-resort tile. A gold diamond is the ornament instead.
						SNew(SBox).WidthOverride(L.CalloutOrnamentSize).HeightOverride(L.CalloutOrnamentSize)
						.RenderTransform(FSlateRenderTransform(FQuat2D(FMath::DegreesToRadians(45.f))))
						.RenderTransformPivot(FVector2D(0.5f, 0.5f))
						[
							SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FVoxelUIStyle::TitleColour())
						]
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(L.CalloutGap, 0.f, L.CalloutPadX, 0.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 4.f))
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::CalloutTag())
					.Font(TagFont)
					.ColorAndOpacity(FSlateColor(Tint(CalloutTag)))
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::CalloutTitle())
					.Font(Style.HandItalic(L.CalloutTitleSize))
					.ColorAndOpacity(FVoxelUIStyle::TitleColour())
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::CalloutCopy())
					.Font(Style.Hand(L.CalloutCopySize))
					.ColorAndOpacity(FSlateColor(Tint(Parchment, 0.85f)))
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f))
					.AutoWrapText(true)
					.LineHeightPercentage(HandLineHeight(1.45f))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(VoxelUITheme::RulePx)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(Gold, 0.35f)))
				]
			]
		];

	// The six buttons, in source order. CONTINUE and LOAD GAME are attribute-
	// bound to HasAnyLoadableSave() rather than being enabled/disabled
	// imperatively, so SetSaveRows does not have to remember to update them.
	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(ContinueButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonContinue())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.IsEnabled(this, &SVoxelMainMenu::HasAnyLoadableSave)
		.OnClicked_Lambda([this]() { OnContinue.ExecuteIfBound(); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(NewGameButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonNewGame())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		// NO RESTING-ACTIVE STATE ON NEW GAME. The mock's `.title-menu__item.active`
		// was ported on 2026-09-07 as a resting selection (bound to HasNoColumnFocus),
		// and the owner rejected it live the same night: "the new game menu option
		// is constantly/always highlighted and expanded. hovering over settings and
		// help and credits is correctly dynamic and only highlights and expands when
		// mouse hovers over it." So NEW GAME lights and grows exactly as its siblings
		// do: on hover or on keyboard focus (SVoxelMenuButton::IsLit), never at rest.
		// HasNoColumnFocus stays defined for the keyboard path; nothing binds it here.
		.OnClicked_Lambda([this]() { OnNewGame.ExecuteIfBound(); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(LoadGameButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonLoadGame())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.IsEnabled(this, &SVoxelMainMenu::HasAnyLoadableSave)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Load); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(SettingsButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonSettings())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Settings); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(HelpButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonHelp())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Help); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(CreditsButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonCredits())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuActiveSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Credits); return FReply::Handled(); })
	];

	// The gap before QUIT (.title-menu__item.quit margin-top: 52px). It is the
	// one piece of vertical rhythm in the list that is not the uniform gap,
	// and it is what stops a mis-aimed click from leaving the game.
	Column->AddSlot().AutoHeight()
	[
		SNew(SSpacer).Size(FVector2D(0.f, L.TitleMenuQuitGap))
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Right)
	[
		SAssignNew(QuitButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonQuit())
		.Variant(EVoxelMenuButtonVariant::Cartouche)
		.Muted(true)
		.FontSize(L.TitleMenuItemSize)
		.ActiveFontSize(L.TitleMenuItemSize)
		.LetterSpacing(L.TitleMenuLetterSpacing)
		.MinHeight(0.f)
		.OnClicked_Lambda([this]() { OnQuit.ExecuteIfBound(); return FReply::Handled(); })
	];

	// In visual order, which is the order navigation walks them.
	ColumnButtons = {ContinueButton, NewGameButton, LoadGameButton, SettingsButton, HelpButton, CreditsButton,
	                 QuitButton};

	// KEYBOARD AND GAMEPAD NAVIGATION IS NEW WORK, not a port. The Godot build
	// has none anywhere -- no grab_focus, no focus_neighbor, no ui_accept
	// handling in MainMenu.gd, PauseMenu.gd or Settings.gd. Its menus are
	// mouse-only, and the manual hit-testing that makes that work is a Dialogic
	// workaround this port deliberately dropped.
	//
	// Slate handles the rest: arrowing between siblings, and the gamepad
	// mapping (D-pad and left stick to EUINavigation, Virtual_Accept to a
	// click) from the application's navigation config. The end-of-list WRAP
	// lives on each SVoxelMenuButton's inner SButton rather than here -- see
	// that file for why a container is the wrong place for it.

	// Three groups pinned to the frame. Overlay slots take an alignment and a
	// padding, which is exactly the mock's "top/right/left" absolute offsets
	// once the panel is given the whole viewport (see Construct). The old
	// TitleBoxWidth clamp story (backlog 0.0l) does not arise here: the logo
	// sits in its own auto-sized slot and clamps against the screen, not a
	// 520-wide column.
	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.LogoTop, L.LogoRight, 0.f))
		[
			Logo
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.TitleMenuTop, L.TitleMenuRight, 0.f))
		[
			SNew(SBox)
			.WidthOverride(L.TitleMenuWidth)
			.HAlign(HAlign_Right)
			[
				Column
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(L.CalloutLeft, L.CalloutTop, 0.f, 0.f))
		[
			Callout
		];
}

void SVoxelMainMenu::FocusDefaultWidget()
{
	TSharedPtr<SVoxelMenuButton> Target;
	switch (VisiblePanel)
	{
	case EVoxelMenuPanel::MainColumn:
		// The first ENABLED button, not simply the first: on a fresh install
		// CONTINUE and LOAD GAME are both greyed out, and focusing a disabled
		// control leaves a gamepad player pressing A at nothing.
		for (const TSharedPtr<SVoxelMenuButton>& Button : ColumnButtons)
		{
			if (Button.IsValid() && Button->IsEnabled())
			{
				Target = Button;
				break;
			}
		}
		break;
	case EVoxelMenuPanel::Load:
		// The dialog owns its own focus default now, and knows which of its
		// controls exists in the empty state.
		if (LoadDialog.IsValid()) { LoadDialog->FocusDefaultWidget(); }
		return;
	case EVoxelMenuPanel::Settings:
		if (SettingsPanel.IsValid()) { SettingsPanel->FocusDefaultWidget(); }
		return;
	default:
		if (const TSharedPtr<SVoxelMenuButton>* Found = MessagePanelBackButtons.Find(VisiblePanel))
		{
			Target = *Found;
		}
		break;
	}

	if (Target.IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = Target->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

TSharedRef<SWidget> SVoxelMainMenu::WrapInPanelFrame(TSharedRef<SWidget> Content)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// UIStyles.menu_body_panel() is a StyleBoxFlat carrying four things:
	// PANEL_OAK_1 fill, a 2px black border, an 18px content margin, and a drop
	// shadow of rgba(0,0,0,0.6) at size 8, offset (0,4).
	//
	// Slate's FSlateBrush carries NONE of the last three. So the panel is
	// three stacked boxes: the shadow (offset down and inflated), the border,
	// and the fill inset by the border width. Reproducing a shadow this way
	// gives a hard-edged rectangle rather than Godot's soft falloff -- an
	// honest limitation of drawing it with boxes instead of a blur, and one
	// worth having rather than dropping the shadow entirely, because the panel
	// sits on photographic art and needs the separation.
	constexpr float kBorderPx = 2.f;
	constexpr float kShadowSize = 8.f;
	constexpr float kShadowOffsetY = 4.f;

	return SNew(SBox)
		.WidthOverride(L.SubPanelHalfWidth * 2.f)
		.HeightOverride(L.SubPanelHalfHeight * 2.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			// Negative padding grows the slot outward. The shadow rect is the
			// panel inflated by kShadowSize on every side and then shifted
			// DOWN by kShadowOffsetY, which moves its top edge in by the offset
			// and its bottom edge out by it -- hence the asymmetry.
			.Padding(FMargin(-kShadowSize, kShadowOffsetY - kShadowSize, -kShadowSize, -(kShadowSize + kShadowOffsetY)))
			[
				SNew(SImage)
				.Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.6f)))
			]
			+ SOverlay::Slot()
			[
				SNew(SImage)
				.Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(FLinearColor::Black))
			]
			+ SOverlay::Slot()
			.Padding(FMargin(kBorderPx))
			[
				SNew(SImage)
				.Image(Style.MenuBodyPanel())
			]
			+ SOverlay::Slot()
			.Padding(FMargin(kBorderPx + L.SubPanelPadding))
			[
				Content
			]
		];
}

TSharedRef<SWidget> SVoxelMainMenu::BuildLoadPanel()
{
	// NO OAK FRAME HERE ANY MORE. SVoxelLoadDialog brings the overlay family's
	// leather panel with it, which is the whole point of the 2026-09-07 mock:
	// the title screen's LOAD list and the pause menu's are one screen, so they
	// are one widget.
	return SAssignNew(LoadDialog, SVoxelLoadDialog)
		.Rows(SaveRows)
		.OnLoadSave(FOnVoxelSaveAction::CreateLambda([this](const FString& Slug)
		{
			OnLoadSave.ExecuteIfBound(Slug);
		}))
		.OnDeleteSave(FOnVoxelSaveAction::CreateLambda([this](const FString& Slug)
		{
			OnDeleteSave.ExecuteIfBound(Slug);
		}))
		.OnCancel(FSimpleDelegate::CreateLambda([this]() { ShowPanel(EVoxelMenuPanel::MainColumn); }));
}

TSharedRef<SWidget> SVoxelMainMenu::BuildMessagePanel(EVoxelMenuPanel Panel, const FText& Title, const FText& Body)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const float HalfSep = L.SubPanelSeparation * 0.5f;
	TSharedPtr<SVoxelMenuButton> BackButton;

	TSharedRef<SWidget> Frame = WrapInPanelFrame(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, HalfSep)).HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(Title)
			.Font(Style.Serif(L.SubPanelTitleSize))
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		]
		// SCROLLED, because the body outgrew the panel the moment CREDITS
		// stopped being "Credits coming soon." The frame is a fixed
		// SubPanelHalfWidth x SubPanelHalfHeight box (720x560), which fits
		// roughly twenty lines at SubPanelBodySize -- the third-party notices
		// are already longer than that and only ever grow. Without this the
		// overflow is silent: the text simply stops, and a missing licence
		// notice that LOOKS like the end of the list is the worst possible
		// failure for this particular panel.
		//
		// HELP and SETTINGS get it too. They are placeholders today and will
		// have the same problem on the day they are not.
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, HalfSep))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(STextBlock)
				.Text(Body)
				.Font(Style.Serif(L.SubPanelBodySize))
				.ColorAndOpacity(FVoxelUIStyle::BodyColour())
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, HalfSep)).HAlign(HAlign_Center)
		[
			SAssignNew(BackButton, SVoxelMenuButton)
			.Text(VoxelUIStrings::ButtonBack())
			.FontSize(L.SaveRowButtonFont)
			.MinHeight(L.DialogButtonHeight)
			.MinWidth(L.DialogButtonWidth)
			.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::MainColumn); return FReply::Handled(); })
		]);

	MessagePanelBackButtons.Add(Panel, BackButton);
	return Frame;
}

TSharedRef<SWidget> SVoxelMainMenu::BuildSettingsPanel()
{
	// SVoxelSettingsPanel since 2026-09-07: the mock's audio and display
	// sections around the four VoxelGraphicsUserSettings rows that used to be
	// the whole panel. Those rows are KEPT -- they are player-facing settings
	// that shipped with owner verdicts behind them, and the mock simply predates
	// them. The row table, the plain-function-pointer idiom and the
	// read-modify-write-through-the-getter rule all moved into that file
	// unchanged; see its header.
	//
	// It is no longer registered in MessagePanelBackButtons: the panel owns its
	// own footer, its own Escape and its own focus default, which is exactly
	// what stopped it being message-panel-shaped.
	return SAssignNew(SettingsPanel, SVoxelSettingsPanel)
		.OnLeave(FSimpleDelegate::CreateLambda([this]() { ShowPanel(EVoxelMenuPanel::MainColumn); }));
}

bool SVoxelMainMenu::HasNoColumnFocus() const
{
	// True when the keyboard is not on any title-screen item, which is the only
	// time the resting `.active` cartouche on NEW GAME should paint. Reads the
	// inner SButton because that is the widget focus lands on -- see
	// SVoxelMenuButton::GetFocusWidget.
	for (const TSharedPtr<SVoxelMenuButton>& Button : ColumnButtons)
	{
		if (!Button.IsValid())
		{
			continue;
		}
		const TSharedPtr<SWidget> FocusWidget = Button->GetFocusWidget();
		if (FocusWidget.IsValid() && FocusWidget->HasKeyboardFocus())
		{
			return false;
		}
	}
	return true;
}

bool SVoxelMainMenu::HasAnyLoadableSave() const
{
	// MainMenu.gd::_show_main_column disables CONTINUE and LOAD GAME together,
	// on `GameState.list_save_files().is_empty()`. The extra bLoadable term
	// here covers rows this build cannot open (a save recorded under a
	// different world seed -- see VoxelSaveLibrary): a list of nothing but
	// those should grey the buttons exactly as an empty list does, because
	// pressing either would fail.
	for (const FVoxelSaveRowInfo& Row : SaveRows)
	{
		if (Row.bLoadable)
		{
			return true;
		}
	}
	return false;
}

void SVoxelMainMenu::SetSaveRows(TArray<FVoxelSaveRowInfo> Rows)
{
	SaveRows = MoveTemp(Rows);
	if (LoadDialog.IsValid())
	{
		LoadDialog->SetRows(SaveRows);
	}
}

void SVoxelMainMenu::ShowPanel(EVoxelMenuPanel Panel)
{
	VisiblePanel = Panel;
	if (PanelSwitcher.IsValid())
	{
		const int32 Index = SVoxelMainMenuDetail::PanelIndex(Panel);
		// Clamped defensively. Every enumerator has a slot today, so this is a
		// no-op -- but PanelIndex and the slot list are two things that have to
		// agree, and a menu that crashes on its own SETTINGS button is a worse
		// failure than one that shows the wrong panel.
		PanelSwitcher->SetActiveWidgetIndex(FMath::Clamp(Index, 0, PanelSwitcher->GetNumWidgets() - 1));
	}
	FocusDefaultWidget();
}

FReply SVoxelMainMenu::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		if (VisiblePanel != EVoxelMenuPanel::MainColumn)
		{
			// PauseMenu.gd's ESC chain does the same thing one level up: a sub
			// panel's Escape means "back", not "close the menu".
			ShowPanel(EVoxelMenuPanel::MainColumn);
			return FReply::Handled();
		}
		// On the main column there is nowhere further back to go. Deliberately
		// NOT wired to QUIT: this is the first screen of the game, and an
		// Escape that exits the process without confirmation is how people
		// lose work.
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

const FSlateBrush* SVoxelMainMenu::GetBackgroundBrush() const
{
	// One image, picked once per menu-show from the shuffled rotation, exactly
	// as MainMenu.gd does (`_bg_paths[randi() % _bg_paths.size()]`). Null while
	// the decode is in flight, which SVoxelCoverImage treats as "draw nothing
	// this frame" rather than as an error.
	return FVoxelUIAssetLibrary::Get().RequestBackground(BackgroundIndex);
}

FSlateColor SVoxelMainMenu::GetBackgroundTint() const
{
	using namespace VoxelUITheme;
	// THE TINT GOES AWAY WITH THE ART, and this is a port of a deliberate
	// decision rather than an accident: MainMenu.gd::_setup_background returns
	// early when the pool is empty, so the 55% black wash is never added and
	// the menu sits on its flat #0a0a0f backdrop. Applying the wash anyway
	// would put near-black buttons on near-black, which is how a graceful
	// degradation turns into an unreadable screen.
	if (!FVoxelUIAssetLibrary::Get().HasAnyBackground())
	{
		return FSlateColor(FLinearColor::Transparent);
	}
	return FSlateColor(Tint(BgNight, FVoxelMenuLayout::Get().BackgroundTintAlpha));
}

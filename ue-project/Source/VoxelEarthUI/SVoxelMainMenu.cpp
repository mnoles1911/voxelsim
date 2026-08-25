#include "SVoxelMainMenu.h"

#include "SVoxelCoverImage.h"
#include "SVoxelMenuButton.h"
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

		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
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
				BuildLoadPanel()
			]
			+ SWidgetSwitcher::Slot()
			[
				BuildMessagePanel(EVoxelMenuPanel::Help, VoxelUIStrings::HelpPanelTitle(), VoxelUIStrings::HelpPanelBody())
			]
			+ SWidgetSwitcher::Slot()
			[
				BuildMessagePanel(EVoxelMenuPanel::Credits, VoxelUIStrings::CreditsPanelTitle(), VoxelUIStrings::CreditsPanelBody())
			]
			+ SWidgetSwitcher::Slot()
			[
				// SETTINGS is out of scope this pass (docs/front-end-plan.md
				// R8). It gets the same placeholder treatment HELP and CREDITS
				// already have in the Godot build rather than being hidden: the
				// menu a player sees is the menu that shipped, and a missing
				// button would be the more visible divergence.
				BuildMessagePanel(EVoxelMenuPanel::Settings, VoxelUIStrings::SettingsPanelTitle(), VoxelUIStrings::SettingsPanelBody())
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
	const float HalfSep = L.MainColumnSeparation * 0.5f;
	const FMargin SlotPad(0.f, HalfSep);

	// TWO COLUMNS, NOT ONE, AND THE SPLIT IS THE FIX. Everything in `Column`
	// is 520 units wide (MainPanelHalfWidth * 2, the Godot _main_panel width)
	// because the BUTTONS want to be. The title does not fit in 520 and must
	// not inherit it -- see the return at the bottom of this function.
	TSharedRef<SVerticalBox> TitleColumn = SNew(SVerticalBox);
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	TSharedPtr<SVoxelMenuButton> NewGameButton;
	TSharedPtr<SVoxelMenuButton> SettingsButton;
	TSharedPtr<SVoxelMenuButton> HelpButton;
	TSharedPtr<SVoxelMenuButton> CreditsButton;
	TSharedPtr<SVoxelMenuButton> QuitButton;

	TitleColumn->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(VoxelUIStrings::Title())
		.Font(Style.Serif(L.TitleFontSize))
		.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		.Justification(ETextJustify::Center)
	];

	TitleColumn->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(VoxelUIStrings::Subtitle())
		.Font(Style.Serif(L.SubtitleFontSize))
		.ColorAndOpacity(FVoxelUIStyle::DimColour())
		.Justification(ETextJustify::Center)
	];

	TitleColumn->AddSlot().AutoHeight()
	[
		SNew(SSpacer).Size(FVector2D(0.f, L.TitleToButtonsSpacer))
	];

	// The six buttons, in source order. CONTINUE and LOAD GAME are attribute-
	// bound to HasAnyLoadableSave() rather than being enabled/disabled
	// imperatively, so SetSaveRows does not have to remember to update them.
	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(ContinueButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonContinue())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.IsEnabled(this, &SVoxelMainMenu::HasAnyLoadableSave)
		.OnClicked_Lambda([this]() { OnContinue.ExecuteIfBound(); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(NewGameButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonNewGame())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { OnNewGame.ExecuteIfBound(); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(LoadGameButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonLoadGame())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.IsEnabled(this, &SVoxelMainMenu::HasAnyLoadableSave)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Load); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(SettingsButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonSettings())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Settings); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(HelpButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonHelp())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Help); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(CreditsButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonCredits())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Credits); return FReply::Handled(); })
	];

	// The 80px gap before QUIT. It is the one piece of vertical rhythm in the
	// column that is not the uniform separation, and it is what stops a
	// mis-aimed click from leaving the game.
	Column->AddSlot().AutoHeight()
	[
		SNew(SSpacer).Size(FVector2D(0.f, L.QuitSpacer))
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SAssignNew(QuitButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonQuit())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
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

	// THE TITLE SITS OUTSIDE THE BUTTON PANEL'S WIDTH, and it has to -- a child
	// cannot escape a narrower parent by asking for more. Slate's AlignChild
	// (LayoutUtils.h) clamps a non-Fill child to its parent by default:
	//
	//     ChildSize = bClampToParent ? Min(ChildDesiredSize, AllottedSize) : ...
	//
	// so an inner SBox asking for 720 inside a 520 parent is silently given
	// 520. That was tried first and produced a byte-identical capture, which is
	// how the clamp was found. The width has to come from the PARENT chain.
	//
	// So the outer box is TitleBoxWidth and the buttons get their own 520 box
	// inside it. Everything the buttons care about is unchanged: same width,
	// same centring, same fixed height. Only the title and subtitle see the
	// wider box. See backlog 0.0l.
	return SNew(SBox)
		.WidthOverride(L.TitleBoxWidth)
		.HeightOverride(L.MainPanelHalfHeight * 2.f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				TitleColumn
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(L.MainPanelHalfWidth * 2.f)
				.HAlign(HAlign_Fill)
				[
					Column
				]
			]
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
		// CANCEL rather than the first save row: it is the control that is
		// always there, and arrowing up from it reaches the rows.
		Target = LoadPanelCancelButton;
		break;
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
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const float HalfSep = L.SubPanelSeparation * 0.5f;

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);

	Column->AddSlot().AutoHeight().Padding(FMargin(0.f, HalfSep)).HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(VoxelUIStrings::LoadPanelTitle())
		.Font(Style.Serif(L.SubPanelTitleSize))
		.ColorAndOpacity(FVoxelUIStyle::TitleColour())
	];

	// FillHeight, so the list takes whatever the title and CANCEL leave --
	// the Godot ScrollContainer is SIZE_EXPAND_FILL for the same reason.
	Column->AddSlot().FillHeight(1.f).Padding(FMargin(0.f, HalfSep))
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SAssignNew(SaveList, SVerticalBox)
		]
	];

	Column->AddSlot().AutoHeight().Padding(FMargin(0.f, HalfSep)).HAlign(HAlign_Center)
	[
		SAssignNew(LoadPanelCancelButton, SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonCancel())
		.FontSize(L.SaveRowButtonFont)
		.MinHeight(L.DialogButtonHeight)
		.MinWidth(L.DialogButtonWidth)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::MainColumn); return FReply::Handled(); })
	];

	RebuildSaveList();
	return WrapInPanelFrame(Column);
}

void SVoxelMainMenu::RebuildSaveList()
{
	using namespace VoxelUITheme;
	if (!SaveList.IsValid())
	{
		return; // the LOAD panel has not been built yet
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	SaveList->ClearChildren();

	if (SaveRows.Num() == 0)
	{
		// MainMenu.gd's empty state, word for word.
		SaveList->AddSlot().AutoHeight().Padding(FMargin(0.f, L.SaveRowSeparation))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::LoadPanelEmpty())
			.Font(Style.Serif(L.SaveRowFontSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			.AutoWrapText(true)
		];
		return;
	}

	for (const FVoxelSaveRowInfo& Row : SaveRows)
	{
		const FString Slug = Row.Slug;
		// Godot renders this as ONE Label with an embedded newline:
		//   "<save_name>\n<timestamp>   X ..  Y ..  Z .."
		// Two STextBlocks in a vertical box give the same two lines with
		// independent colours, which is worth the extra widget: the name wants
		// INK and the detail wants INK_DIM, and a single label cannot do both.
		TSharedRef<SVerticalBox> Info = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Row.DisplayName)
				.Font(Style.Serif(L.SaveRowFontSize))
				.ColorAndOpacity(FVoxelUIStyle::BodyColour())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				// An unloadable row shows WHY in place of its coordinates.
				// The alternative -- showing the position of a world this
				// build cannot open -- is an invitation to click it.
				.Text(Row.bLoadable ? Row.Detail : Row.DisabledReason)
				.Font(Style.Serif(L.SaveRowFontSize - 2))
				.ColorAndOpacity(Row.bLoadable ? FVoxelUIStyle::DimColour() : FVoxelUIStyle::MutedColour())
			];

		SaveList->AddSlot().AutoHeight().Padding(FMargin(0.f, L.SaveRowSeparation * 0.5f))
		[
			SNew(SBox)
			.MinDesiredHeight(L.SaveRowMinHeight)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					Info
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(L.SaveRowSeparation, 0.f, 0.f, 0.f))
				[
					SNew(SVoxelMenuButton)
					.Text(VoxelUIStrings::ButtonLoad())
					.FontSize(L.SaveRowButtonFont)
					.MinHeight(L.SaveRowButtonHeight)
					.MinWidth(L.SaveRowButtonWidth)
					.IsEnabled(Row.bLoadable)
					.OnClicked_Lambda([this, Slug]() { OnLoadSave.ExecuteIfBound(Slug); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(L.SaveRowSeparation, 0.f, 0.f, 0.f))
				[
					SNew(SVoxelMenuButton)
					.Text(VoxelUIStrings::ButtonDelete())
					.FontSize(L.SaveRowButtonFont)
					.MinHeight(L.SaveRowButtonHeight)
					.MinWidth(L.SaveRowButtonWidth)
					// HP_BRIGHT, matching the Godot row's font_color override.
					// DELETE is enabled even on an unloadable save: not being
					// able to OPEN a world is no reason to be stuck with it.
					.TextColorOverride(Tint(HpBright))
					.OnClicked_Lambda([this, Slug]() { OnDeleteSave.ExecuteIfBound(Slug); return FReply::Handled(); })
				]
			]
		];
	}
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
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, HalfSep))
		[
			SNew(STextBlock)
			.Text(Body)
			.Font(Style.Serif(L.SubPanelBodySize))
			.ColorAndOpacity(FVoxelUIStyle::BodyColour())
			.AutoWrapText(true)
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
	RebuildSaveList();
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

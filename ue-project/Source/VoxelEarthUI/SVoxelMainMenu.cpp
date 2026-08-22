#include "SVoxelMainMenu.h"

#include "SVoxelMenuButton.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
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

void SVoxelMainMenu::Construct(const FArguments& InArgs)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnContinue = InArgs._OnContinue;
	OnNewGame = InArgs._OnNewGame;
	OnQuit = InArgs._OnQuit;
	OnLoadSave = InArgs._OnLoadSave;
	OnDeleteSave = InArgs._OnDeleteSave;

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
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(Tint(BgNight, L.BackgroundTintAlpha)))
		]

		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SAssignNew(PanelSwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()
			[
				BuildMainColumn()
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

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(VoxelUIStrings::Title())
		.Font(Style.Serif(L.TitleFontSize))
		.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		.Justification(ETextJustify::Center)
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad).HAlign(HAlign_Center)
	[
		SNew(STextBlock)
		.Text(VoxelUIStrings::Subtitle())
		.Font(Style.Serif(L.SubtitleFontSize))
		.ColorAndOpacity(FVoxelUIStyle::DimColour())
		.Justification(ETextJustify::Center)
	];

	Column->AddSlot().AutoHeight()
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
		SNew(SVoxelMenuButton)
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
		SNew(SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonSettings())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Settings); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SNew(SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonHelp())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { ShowPanel(EVoxelMenuPanel::Help); return FReply::Handled(); })
	];

	Column->AddSlot().AutoHeight().Padding(SlotPad)
	[
		SNew(SVoxelMenuButton)
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
		SNew(SVoxelMenuButton)
		.Text(VoxelUIStrings::ButtonQuit())
		.FontSize(L.ButtonFontSize)
		.MinHeight(L.ButtonMinHeight)
		.OnClicked_Lambda([this]() { OnQuit.ExecuteIfBound(); return FReply::Handled(); })
	];

	return SNew(SBox)
		.WidthOverride(L.MainPanelHalfWidth * 2.f)
		.HeightOverride(L.MainPanelHalfHeight * 2.f)
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Center)
		[
			Column
		];
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
	// The list widget itself arrives with the LOAD panel; until then, storing
	// the rows is enough to drive the two buttons' enabled state.
}

void SVoxelMainMenu::ShowPanel(EVoxelMenuPanel Panel)
{
	VisiblePanel = Panel;
	if (PanelSwitcher.IsValid())
	{
		const int32 Index = SVoxelMainMenuDetail::PanelIndex(Panel);
		// Clamped because the sub-panels land in a later step: asking a
		// two-slot switcher for slot 3 would assert, and a menu that crashes
		// on its own SETTINGS button is a worse failure than one that ignores
		// it.
		PanelSwitcher->SetActiveWidgetIndex(FMath::Clamp(Index, 0, PanelSwitcher->GetNumWidgets() - 1));
	}
}

FReply SVoxelMainMenu::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Back)
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

#include "SVoxelPauseMenu.h"

#include "SVoxelMenuButton.h"
#include "SVoxelOverlayChrome.h"
#include "SVoxelSaveDialog.h"
#include "SVoxelSettingsPanel.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelPauseMenuDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Maps the panel enum onto the switcher's slot indices. Kept next to the
// switcher's construction order so the two cannot drift -- the same arrangement
// SVoxelMainMenu uses, for the same reason.
int32 PanelIndex(EVoxelPausePanel Panel)
{
	switch (Panel)
	{
	case EVoxelPausePanel::Pause:    return 0;
	case EVoxelPausePanel::Settings: return 1;
	case EVoxelPausePanel::Save:     return 2;
	case EVoxelPausePanel::Load:     return 3;
	}
	return 0;
}
} // namespace SVoxelPauseMenuDetail

SVoxelPauseMenu::~SVoxelPauseMenu()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelPauseMenu::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();

	OnResume = InArgs._OnResume;
	OnExitToMenu = InArgs._OnExitToMenu;
	OnQuit = InArgs._OnQuit;
	OnSaveConfirmed = InArgs._OnSaveConfirmed;
	OnLoadSave = InArgs._OnLoadSave;
	OnDeleteSave = InArgs._OnDeleteSave;
	Rows = InArgs._Rows;
	SaveContextName = InArgs._SaveContextName;
	DayNumber = InArgs._DayNumber;

	ChildSlot
	[
		SNew(SOverlay)
		// The dimmed world. One layer, not the mock's blur plus vignette plus
		// scanline grain -- see VoxelOverlayChrome::Scrim.
		+ SOverlay::Slot()
		[
			VoxelOverlayChrome::Scrim()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SAssignNew(Switcher, SWidgetSwitcher)
			// SLOT ORDER IS THE ENUM ORDER; PanelIndex above maps between them.
			+ SWidgetSwitcher::Slot()
			[
				BuildPausePanel()
			]
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(SettingsPanel, SVoxelSettingsPanel)
				.OnLeave(FSimpleDelegate::CreateLambda([this]() { ShowPanel(EVoxelPausePanel::Pause); }))
			]
			+ SWidgetSwitcher::Slot()
			[
				// Filled by OpenSaveDialog on each SAVE press; empty until then.
				SAssignNew(SaveDialogHost, SBox)
			]
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(LoadDialog, SVoxelLoadDialog)
				.Rows(Rows)
				.OnLoadSave(OnLoadSave)
				.OnDeleteSave(OnDeleteSave)
				.OnCancel(FSimpleDelegate::CreateLambda([this]() { ShowPanel(EVoxelPausePanel::Pause); }))
			]
		]
	];

	ShowPanel(EVoxelPausePanel::Pause);
}

TSharedRef<SWidget> SVoxelPauseMenu::BuildPausePanel()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// .pa-panel's six items, in the mock's order, all one variant with two of
	// them flagged danger. `gap:6px` between them, which Slate expresses as
	// symmetric half-gap padding.
	const FMargin ItemPad(0.f, L.PausePanelGap * 0.5f);

	struct FPauseItem
	{
		FText Label;
		bool bDanger;
		TFunction<void()> Action;
	};
	TArray<FPauseItem> Items;
	Items.Add({VoxelUIStrings::ButtonResume(), false, [this]() { OnResume.ExecuteIfBound(); }});
	Items.Add({VoxelUIStrings::ButtonSave(), false, [this]() { ShowPanel(EVoxelPausePanel::Save); }});
	// LOAD, not LOAD GAME: the mock's third .pa-btn is the short word. The long
	// one belongs to the title screen, where it sits next to CONTINUE and has to
	// say which game. ButtonLoad() already existed for the per-save row button.
	Items.Add({VoxelUIStrings::ButtonLoad(), false, [this]() { ShowPanel(EVoxelPausePanel::Load); }});
	Items.Add({VoxelUIStrings::ButtonSettings(), false, [this]() { ShowPanel(EVoxelPausePanel::Settings); }});
	Items.Add({VoxelUIStrings::ButtonExitToMenu(), true, [this]() { OnExitToMenu.ExecuteIfBound(); }});
	Items.Add({VoxelUIStrings::ButtonQuit(), true, [this]() { OnQuit.ExecuteIfBound(); }});

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	for (const FPauseItem& Item : Items)
	{
		TSharedPtr<SVoxelMenuButton> Button;
		Column->AddSlot().AutoHeight().Padding(ItemPad)
		[
			SAssignNew(Button, SVoxelMenuButton)
			.Text(Item.Label)
			.Variant(EVoxelMenuButtonVariant::PauseItem)
			.Danger(Item.bDanger)
			.FontSize(L.PauseItemSize)
			.LetterSpacing(L.PauseItemLetterSpacing)
			.MinHeight(0.f)
			.OnClicked_Lambda([Action = Item.Action]() { Action(); return FReply::Handled(); })
		];
		PauseButtons.Add(Button);
	}

	TSharedRef<SVerticalBox> Body =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			VoxelOverlayChrome::Title(VoxelUIStrings::PauseTitle(), L.PauseTitleSize, L.PauseTitleLetterSpacing)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.PauseRuleTopGap, 0.f, L.PauseRuleBottomGap))
		[
			VoxelOverlayChrome::Rule()
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Column
		];

	// .pa-foot -- omitted entirely rather than shown blank when the session has
	// no day to report (a capture run with no sky subsystem, most often).
	if (DayNumber > 0)
	{
		Body->AddSlot().AutoHeight().Padding(FMargin(0.f, L.PauseFootTopGap, 0.f, 0.f)).HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::PauseFooter(DayNumber))
			.Font(Style.HandItalic(L.PauseFootSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];
	}

	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			VoxelOverlayChrome::Panel(Body, L.PausePanelWidth, 0.f,
			                          FMargin(L.PausePanelPadX, L.PausePanelPadY))
		];
}

void SVoxelPauseMenu::BuildSaveDialog()
{
	// The collision set is every name on disk RIGHT NOW, and the default name
	// is built from the same snapshot -- see the header for why the dialog is
	// rebuilt rather than reused.
	TArray<FString> Existing;
	Existing.Reserve(Rows.Num());
	for (const FVoxelSaveRowInfo& Row : Rows)
	{
		Existing.Add(Row.DisplayName.ToString());
	}

	const FText Context = SaveContextName.IsEmpty() ? VoxelUIStrings::Title() : SaveContextName;
	const FText Default = DayNumber > 0 ? VoxelUIStrings::DefaultSaveName(Context, DayNumber) : Context;

	SaveDialogHost->SetContent(
		SAssignNew(SaveDialog, SVoxelSaveDialog)
		.ContextName(Context)
		.DefaultName(Default)
		.ExistingNames(Existing)
		.OnConfirm(FOnVoxelSaveNameConfirmed::CreateLambda([this](const FString& Name)
		{
			OnSaveConfirmed.ExecuteIfBound(Name);
			// Back to the list, not out of the overlay: the mock's CONFIRM
			// returns to the pause panel, and a player who saved mid-session
			// usually meant to carry on from there.
			ShowPanel(EVoxelPausePanel::Pause);
		}))
		.OnCancel(FSimpleDelegate::CreateLambda([this]() { ShowPanel(EVoxelPausePanel::Pause); })));
}

void SVoxelPauseMenu::SetSaveRows(TArray<FVoxelSaveRowInfo> InRows)
{
	Rows = MoveTemp(InRows);
	if (LoadDialog.IsValid())
	{
		LoadDialog->SetRows(Rows);
	}
}

void SVoxelPauseMenu::ShowPanel(EVoxelPausePanel Panel)
{
	// THE SAVE SLOT IS EMPTY UNTIL IT IS SHOWN, and building it here rather
	// than at the SAVE button is what makes that true for every caller. The
	// 2026-09-07 capture of -VoxelPausePanel=save photographed a blank screen
	// for exactly this reason: it switched to the slot without going through
	// the button. A dialog whose default name and collision set are snapshots
	// has to be built at the moment it becomes visible, whoever asked.
	if (Panel == EVoxelPausePanel::Save)
	{
		BuildSaveDialog();
	}
	VisiblePanel = Panel;
	if (Switcher.IsValid())
	{
		const int32 Index = SVoxelPauseMenuDetail::PanelIndex(Panel);
		Switcher->SetActiveWidgetIndex(FMath::Clamp(Index, 0, Switcher->GetNumWidgets() - 1));
	}
	FocusDefaultWidget();
}

void SVoxelPauseMenu::FocusDefaultWidget()
{
	switch (VisiblePanel)
	{
	case EVoxelPausePanel::Pause:
		for (const TSharedPtr<SVoxelMenuButton>& Button : PauseButtons)
		{
			if (Button.IsValid() && Button->IsEnabled())
			{
				if (const TSharedPtr<SWidget> FocusWidget = Button->GetFocusWidget())
				{
					FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
				}
				return;
			}
		}
		return;
	case EVoxelPausePanel::Settings:
		if (SettingsPanel.IsValid()) { SettingsPanel->FocusDefaultWidget(); }
		return;
	case EVoxelPausePanel::Save:
		if (SaveDialog.IsValid()) { SaveDialog->FocusDefaultWidget(); }
		return;
	case EVoxelPausePanel::Load:
		if (LoadDialog.IsValid()) { LoadDialog->FocusDefaultWidget(); }
		return;
	}
}

FReply SVoxelPauseMenu::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		// ONE LEVEL AT A TIME, which is SVoxelMainMenu's rule and PauseMenu.gd's
		// before it: from a dialog, Escape means "back to the pause list"; from
		// the list itself it means "unpause". The sub-panels handle their own
		// Escape and never reach here, so this branch is the list's.
		if (VisiblePanel != EVoxelPausePanel::Pause)
		{
			ShowPanel(EVoxelPausePanel::Pause);
			return FReply::Handled();
		}
		OnResume.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

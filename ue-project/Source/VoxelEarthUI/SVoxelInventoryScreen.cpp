#include "SVoxelInventoryScreen.h"

#include "SVoxelMenuButton.h"
#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelInventoryScreenDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Which .filter-tab an item answers to. THE MAPPING IS MOSTLY EMPTY, because
// FVoxelItemDef's four categories do not reach the mock's seven chips -- see
// the header. Index 0 is "All" and matches everything.
bool MatchesFilter(const FVoxelInventoryScreenItem& Item, int32 Filter)
{
	if (Filter <= 0)
	{
		return true;
	}
	const TArray<FText>& Names = VoxelUIStrings::InvFilterNames();
	return Names.IsValidIndex(Filter)
	    && Item.Category.EqualToCaseIgnored(Names[Filter]);
}

bool MatchesSearch(const FVoxelInventoryScreenItem& Item, const FText& Query)
{
	if (Query.IsEmpty())
	{
		return true;
	}
	return Item.DisplayName.ToString().Contains(Query.ToString());
}
} // namespace SVoxelInventoryScreenDetail

SVoxelInventoryScreen::~SVoxelInventoryScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

TArray<FVoxelScreenAction> SVoxelInventoryScreen::Actions()
{
	return {
		FVoxelScreenAction(FText::FromString(TEXT("Q/E")), VoxelUIStrings::ScreenActionPage()),
		FVoxelScreenAction(FText::FromString(TEXT("ESC")), VoxelUIStrings::ScreenActionClose(), /*bSpacerBefore=*/true),
	};
}

void SVoxelInventoryScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			BuildPackColumn()
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.InvColumnGap, 0.f, 0.f, 0.f))
		[
			BuildSideColumn()
		]
	];

	RebuildPack();
}

TSharedRef<SWidget> SVoxelInventoryScreen::BuildPackColumn()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// --- .panel-head: name, search box, weight -------------------------------
	TSharedRef<SHorizontalBox> Head =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			VoxelScreenChrome::PanelHeading(VoxelUIStrings::InvPack())
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		.Padding(FMargin(12.f, 0.f))
		[
			SNew(SBox)
			.MaxDesiredWidth(280.f)
			[
				SNew(SEditableTextBox)
				.Font(Style.Mono(L.InvWeightSize))
				.HintText(VoxelUIStrings::InvSearchHint())
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					SearchText = NewText;
					RebuildPack();
				})
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::InvWeight(Data.CarriedKg))
			.Font(Style.Mono(L.InvWeightSize))
			.ColorAndOpacity(FVoxelUIStyle::DimColour())
		];

	// --- .filter-row: seven chips, each carrying its own tally ---------------
	TSharedRef<SHorizontalBox> Filters = SNew(SHorizontalBox);
	const TArray<FText>& FilterNames = VoxelUIStrings::InvFilterNames();
	for (int32 I = 0; I < FilterNames.Num(); ++I)
	{
		TSharedPtr<SVoxelMenuButton> Chip;
		Filters->AddSlot().AutoWidth()
		.Padding(FMargin(0.f, 0.f, I == FilterNames.Num() - 1 ? 0.f : -2.f, 0.f))
		[
			SAssignNew(Chip, SVoxelMenuButton)
			.Text(FilterNames[I])
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this, I]() { return ActiveFilter == I; })
			.CountLabel_Lambda([this, I]()
			{
				int32 Count = 0;
				for (const FVoxelInventoryScreenItem& Item : Data.Pack)
				{
					if (!Item.IsEmpty() && SVoxelInventoryScreenDetail::MatchesFilter(Item, I))
					{
						++Count;
					}
				}
				return FText::AsNumber(Count);
			})
			.OnClicked_Lambda([this, I]()
			{
				ActiveFilter = I;
				RebuildPack();
				return FReply::Handled();
			})
		];
		if (I == 0)
		{
			FirstChip = Chip;
		}
	}

	// --- .pack-box: the 8x8 grid, a rule, and the hotbar underneath ----------
	TSharedRef<SVerticalBox> Well =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(PackGrid, SUniformGridPanel)
			.SlotPadding(FMargin(L.InvSlotGap * 0.5f))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(1.f, L.InvHotbarRuleGap))
		[
			SNew(SBox)
			.HeightOverride(2.f)
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge, 0.5f))
			]
		];

	// The hotbar is the LIVE selected row, so its cells carry the 1..8 digits
	// and the selected cell takes the gold ring.
	TSharedRef<SHorizontalBox> Hotbar = SNew(SHorizontalBox);
	for (int32 I = 0; I < L.InvHotbarSlots; ++I)
	{
		const FVoxelInventoryScreenItem Item = Data.Hotbar.IsValidIndex(I)
			? Data.Hotbar[I] : FVoxelInventoryScreenItem();
		Hotbar->AddSlot().AutoWidth().Padding(FMargin(L.InvSlotGap * 0.5f))
		[
			VoxelScreenChrome::ItemSlot(Item, L.InvSlotSize, /*bHotbar=*/true,
			                            /*bSelected=*/I == Data.SelectedHotbarSlot,
			                            FText::AsNumber(I + 1))
		];
	}
	Well->AddSlot().AutoHeight()[Hotbar];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Head]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 10.f, 0.f, 8.f))[Filters]
		+ SVerticalBox::Slot().AutoHeight()
		[
			VoxelScreenChrome::IronWell(Well, FMargin(FVoxelMenuLayout::Get().InvPackBoxPad))
		];
}

void SVoxelInventoryScreen::RebuildPack()
{
	if (!PackGrid.IsValid())
	{
		return;
	}
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	PackGrid->ClearChildren();

	// FILTERING COMPACTS THE GRID, which is the mock's own behaviour: it filters
	// the array and then fills the 64 cells from the front, so a filtered pack
	// reads as a short list rather than as a sparse one.
	TArray<FVoxelInventoryScreenItem> Shown;
	for (const FVoxelInventoryScreenItem& Item : Data.Pack)
	{
		if (!Item.IsEmpty()
		    && SVoxelInventoryScreenDetail::MatchesFilter(Item, ActiveFilter)
		    && SVoxelInventoryScreenDetail::MatchesSearch(Item, SearchText))
		{
			Shown.Add(Item);
		}
	}

	const int32 Cells = L.InvPackCols * L.InvPackRows;
	for (int32 I = 0; I < Cells; ++I)
	{
		const FVoxelInventoryScreenItem Item = Shown.IsValidIndex(I) ? Shown[I] : FVoxelInventoryScreenItem();
		PackGrid->AddSlot(I % L.InvPackCols, I / L.InvPackCols)
		[
			VoxelScreenChrome::ItemSlot(Item, L.InvSlotSize)
		];
	}
}

TSharedRef<SWidget> SVoxelInventoryScreen::BuildSideColumn()
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FSlateFontInfo HeadingFont = Style.Serif(L.InvPanelNameSize);
	HeadingFont.LetterSpacing = L.InvPanelNameSpacing;

	// .mode-toggle -- two chips that swap the panel below them. A switcher
	// rather than two visibility attributes, on SVoxelPauseMenu's rule: "which
	// of these is on screen" is one question and belongs in one widget.
	TSharedRef<SHorizontalBox> Toggle =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, -2.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::InvModeCraft())
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize + 1)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this]() { return !bCharacterMode; })
			.OnClicked_Lambda([this]()
			{
				bCharacterMode = false;
				if (SideSwitcher.IsValid()) { SideSwitcher->SetActiveWidgetIndex(0); }
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::InvModeCharacter())
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize + 1)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this]() { return bCharacterMode; })
			.OnClicked_Lambda([this]()
			{
				bCharacterMode = true;
				if (SideSwitcher.IsValid()) { SideSwitcher->SetActiveWidgetIndex(1); }
				return FReply::Handled();
			})
		];

	TSharedRef<SHorizontalBox> Head =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			// NOT VoxelScreenChrome::PanelHeading, which takes a fixed FText:
			// this caption is the one heading in the front end that changes
			// after construction, because the toggle beside it renames the
			// panel it labels.
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return bCharacterMode ? VoxelUIStrings::InvCharacter() : VoxelUIStrings::InvCrafting();
			})
			.Font(HeadingFont)
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right).VAlign(VAlign_Center)
		[
			Toggle
		];

	return SNew(SBox)
		.WidthOverride(L.InvSidePanelWidth)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Head]
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, 10.f, 0.f, 0.f))
			[
				VoxelScreenChrome::IronWell(
					SAssignNew(SideSwitcher, SWidgetSwitcher)
					+ SWidgetSwitcher::Slot()[BuildCraftMode()]
					+ SWidgetSwitcher::Slot()[BuildCharacterMode()],
					FMargin(L.InvSideBoxPad))
			]
		];
}

TSharedRef<SWidget> SVoxelInventoryScreen::BuildCraftMode()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SUniformGridPanel> Grid =
		SNew(SUniformGridPanel).SlotPadding(FMargin(L.InvCraftSlotGap * 0.5f));
	for (int32 I = 0; I < 9; ++I)
	{
		Grid->AddSlot(I % 3, I / 3)
		[
			VoxelScreenChrome::ItemSlot(FVoxelInventoryScreenItem(), L.InvCraftSlotSize)
		];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)[Grid]
		// .craft-arrow -- a downward bronze triangle. Slate has no polygon
		// brush, so it is the same geometric substitution the front end makes
		// everywhere else: a narrow bar standing in for the arrow's stem.
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 8.f))
		[
			SNew(SBox).WidthOverride(18.f).HeightOverride(3.f)
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			VoxelScreenChrome::ItemSlot(FVoxelInventoryScreenItem(), L.InvCraftSlotSize)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 12.f, 0.f, 8.f))
		[
			// BLANK, NOT "NO MATCH". The grid is inert (there is no recipe
			// system), so it is always empty -- and "NO MATCH" against an empty
			// grid reads as a failed search rather than as an unasked question.
			// The mock blanks this line for the same state.
			SNew(STextBlock)
			.Text(FText::GetEmpty())
			.Font(Style.Mono(L.InvStatsSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			// DISABLED, NOT HIDDEN. There is no recipe system to press this
			// against, and a button that vanished would leave the panel looking
			// unfinished rather than unimplemented.
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::InvCraftButton())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.FontSize(L.InvFilterSize + 2)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.IsEnabled(false)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 10.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::InvCraftHint())
			.Font(Style.Mono(L.InvStatsSize - 1))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			.Justification(ETextJustify::Center)
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SVoxelInventoryScreen::BuildCharacterMode()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const TArray<FText>& SlotNames = VoxelUIStrings::InvEquipSlotNames();

	// One labelled socket. The label sits UNDER the cell (the mock's `.eq .lab`
	// is `bottom:-13px`), which is why each is a small vertical box rather than
	// a bare ItemSlot.
	auto Socket = [&](int32 Index) -> TSharedRef<SWidget>
	{
		const FVoxelInventoryScreenItem Item = Data.Equipment.IsValidIndex(Index)
			? Data.Equipment[Index] : FVoxelInventoryScreenItem();
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				VoxelScreenChrome::ItemSlot(Item, L.InvEquipSlotSize)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(SlotNames.IsValidIndex(Index) ? SlotNames[Index] : FText::GetEmpty())
				.Font(Style.Mono(L.InvSlotQtySize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			];
	};

	auto Column = [&](int32 First) -> TSharedRef<SWidget>
	{
		TSharedRef<SVerticalBox> Col = SNew(SVerticalBox);
		for (int32 I = First; I < First + 4; ++I)
		{
			Col->AddSlot().AutoHeight().Padding(FMargin(0.f, L.InvEquipColumnGap * 0.5f))[Socket(I)];
		}
		return Col;
	};

	// .render-frame -- the turntable's home. See the header for why the figure
	// itself is not drawn; the plinth and the frame are kept because they are
	// what makes the two gear columns read as flanking something.
	TSharedRef<SWidget> RenderFrame =
		SNew(SBox)
		.HeightOverride(L.InvRenderFrameHeight)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				VoxelScreenChrome::IronWell(SNullWidget::NullWidget, FMargin(0.f))
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(VoxelUIStrings::InvNoEquipment())
				.Font(Style.HandItalic(L.InvStatsSize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0.f, 0.f, 0.f, 14.f))
			[
				SNew(SBox).WidthOverride(140.f).HeightOverride(6.f)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(IronDeep, FColor(0x14, 0x10, 0x0a))))
				]
			]
		];

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Column(0)]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		.Padding(FMargin(L.InvEquipColumnGap, 0.f))
		[
			RenderFrame
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Column(4)];
}

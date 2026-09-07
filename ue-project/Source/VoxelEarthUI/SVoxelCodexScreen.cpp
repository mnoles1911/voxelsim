#include "SVoxelCodexScreen.h"

#include "SVoxelMenuButton.h"
#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

SVoxelCodexScreen::~SVoxelCodexScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

TArray<FVoxelScreenAction> SVoxelCodexScreen::Actions()
{
	return {
		FVoxelScreenAction(FText::FromString(TEXT("Q/E")), VoxelUIStrings::ScreenActionPage()),
		FVoxelScreenAction(FText::FromString(TEXT("ESC")), VoxelUIStrings::ScreenActionClose(),
		                   /*bSpacerBefore=*/true),
	};
}

void SVoxelCodexScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;
	Places = InArgs._Places;

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(L.CodexCategoryWidth)[BuildCategoryColumn()]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.CodexColumnGap, 0.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(L.CodexEntryListWidth)
			[
				SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(EntryListBox, SVerticalBox)]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(L.CodexColumnGap, 0.f, 0.f, 0.f))
		[
			VoxelScreenChrome::ParchmentPanel(
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				.Padding(FMargin(L.JournalPagePadX, L.JournalPagePadY))
				[
					SAssignNew(PageBox, SVerticalBox)
				])
		]
	];

	RebuildEntryList();
	RebuildPage();
}

TSharedRef<SWidget> SVoxelCodexScreen::BuildCategoryColumn()
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);

	auto Row = [&](const FText& Label, const FText& Tally, int32 Which)
	{
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 3.f))
		[
			SNew(SVoxelMenuButton)
			.Text(Label)
			.CountLabel(Tally)
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.CodexCategorySize)
			.MinHeight(0.f)
			.ContentPadding(FMargin(10.f, 7.f))
			.Active_Lambda([this, Which]() { return Category == Which; })
			.OnClicked_Lambda([this, Which]()
			{
				Category = Which;
				SelectedEntry = 0;
				RebuildEntryList();
				RebuildPage();
				return FReply::Handled();
			})
		];
	};

	Row(VoxelUIStrings::CodexRecipes(), FText::AsNumber(Data.Recipes.Num()), kRecipes);
	Row(VoxelUIStrings::CodexPlaces(), FText::AsNumber(Places.Num()), kPlaces);
	for (int32 I = 0; I < Data.Categories.Num(); ++I)
	{
		const FVoxelCodexCategory& Cat = Data.Categories[I];
		int32 Known = 0;
		for (const FVoxelCodexEntry& Entry : Cat.Entries)
		{
			Known += Entry.bLocked ? 0 : 1;
		}
		Row(Cat.Name, FText::Format(FTextFormat::FromString(TEXT("{0}/{1}")),
		                            FText::AsNumber(Known), FText::AsNumber(Cat.Entries.Num())), I);
	}

	return Column;
}

void SVoxelCodexScreen::RebuildEntryList()
{
	using namespace VoxelUITheme;
	if (!EntryListBox.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	EntryListBox->ClearChildren();

	// One selectable row: an empty chip with its content laid over it, the
	// idiom SVoxelJournalScreen's cards use and for the same reason.
	auto Row = [&](TSharedRef<SWidget> Content, bool bSelected, bool bEnabled, TFunction<void()> OnPick)
	{
		EntryListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 3.f))
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SVoxelMenuButton)
				.Text(FText::GetEmpty())
				.Variant(EVoxelMenuButtonVariant::Chip)
				.MinHeight(0.f)
				.IsEnabled(bEnabled)
				.Active(bSelected)
				.ContentPadding(FMargin(10.f, 7.f))
				.OnClicked_Lambda([OnPick]() { OnPick(); return FReply::Handled(); })
			]
			+ SOverlay::Slot().Padding(FMargin(10.f, 7.f))
			[
				Content
			]
		];
	};

	if (Category == kRecipes)
	{
		EntryListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			SNew(SEditableTextBox)
			.Font(Style.Mono(L.CodexEntrySize))
			.HintText(VoxelUIStrings::CodexSearchHint())
			.OnTextChanged_Lambda([this](const FText& NewText)
			{
				SearchText = NewText;
				SelectedEntry = 0;
				RebuildEntryList();
				RebuildPage();
			})
		];

		VisibleRecipes.Reset();
		const FString Query = SearchText.ToString();
		for (int32 I = 0; I < Data.Recipes.Num(); ++I)
		{
			const FVoxelRecipe& Recipe = Data.Recipes[I];
			bool bMatch = Query.IsEmpty()
			           || Recipe.Name.ToString().Contains(Query)
			           || Recipe.Category.ToString().Contains(Query);
			if (!bMatch)
			{
				for (const FVoxelRecipeIngredient& Ingredient : Recipe.Ingredients)
				{
					if (Ingredient.Name.ToString().Contains(Query))
					{
						bMatch = true;
						break;
					}
				}
			}
			if (bMatch)
			{
				VisibleRecipes.Add(I);
			}
		}

		if (VisibleRecipes.Num() == 0)
		{
			EntryListBox->AddSlot().AutoHeight().Padding(FMargin(10.f, 20.f))
			[
				SNew(STextBlock).Text(VoxelUIStrings::CodexNoRecipe())
				.Font(Style.HandItalic(L.CodexEntrySize)).ColorAndOpacity(FVoxelUIStyle::MutedColour())
				.Justification(ETextJustify::Center).AutoWrapText(true)
			];
			return;
		}

		FText CurrentGroup;
		for (int32 Slot = 0; Slot < VisibleRecipes.Num(); ++Slot)
		{
			const FVoxelRecipe& Recipe = Data.Recipes[VisibleRecipes[Slot]];
			if (!Recipe.Category.EqualTo(CurrentGroup))
			{
				CurrentGroup = Recipe.Category;
				EntryListBox->AddSlot().AutoHeight().Padding(FMargin(2.f, 8.f, 0.f, 4.f))
				[
					SNew(STextBlock).Text(Recipe.Category).Font(Style.Serif(L.SubTabLabelSize))
					.ColorAndOpacity(FVoxelUIStyle::MutedColour())
				];
			}
			Row(SNew(SHorizontalBox)
			    .Visibility(EVisibility::HitTestInvisible)
			    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			    .Padding(FMargin(0.f, 0.f, 10.f, 0.f))
			    [
				    VoxelScreenChrome::ItemGlyph(Recipe.Glyph, L.ListRowIconSize)
			    ]
			    + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			    [
				    SNew(STextBlock).Text(Recipe.Name).Font(Style.Mono(L.CodexEntrySize))
				    .ColorAndOpacity(FVoxelUIStyle::BodyColour())
			    ],
			    Slot == SelectedEntry, /*bEnabled=*/true,
			    [this, Slot]() { SelectedEntry = Slot; RebuildEntryList(); RebuildPage(); });
		}
		return;
	}

	if (Category == kPlaces)
	{
		if (Places.Num() == 0)
		{
			EntryListBox->AddSlot().AutoHeight().Padding(FMargin(10.f, 20.f))
			[
				SNew(STextBlock).Text(VoxelUIStrings::MapNoPlaces())
				.Font(Style.HandItalic(L.CodexEntrySize)).ColorAndOpacity(FVoxelUIStyle::MutedColour())
				.Justification(ETextJustify::Center).AutoWrapText(true)
			];
			return;
		}
		for (int32 I = 0; I < Places.Num(); ++I)
		{
			Row(SNew(STextBlock).Text(Places[I].Name).Font(Style.Mono(L.CodexEntrySize))
			    .ColorAndOpacity(FVoxelUIStyle::BodyColour())
			    .Visibility(EVisibility::HitTestInvisible),
			    I == SelectedEntry, /*bEnabled=*/true,
			    [this, I]() { SelectedEntry = I; RebuildEntryList(); RebuildPage(); });
		}
		return;
	}

	if (!Data.Categories.IsValidIndex(Category))
	{
		return;
	}
	const FVoxelCodexCategory& Cat = Data.Categories[Category];
	for (int32 I = 0; I < Cat.Entries.Num(); ++I)
	{
		const FVoxelCodexEntry& Entry = Cat.Entries[I];
		TSharedRef<SHorizontalBox> Line =
			SNew(SHorizontalBox)
			.Visibility(EVisibility::HitTestInvisible)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Entry.Name).Font(Style.Mono(L.CodexEntrySize))
				.ColorAndOpacity(Entry.bLocked ? FVoxelUIStyle::MutedColour()
				                               : FVoxelUIStyle::BodyColour())
			];
		if (Entry.bUnread)
		{
			// The mock's `st:'new'` state. A gold pip rather than a word, so the
			// row reads at a glance and the name keeps its full width.
			Line->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(6.f).HeightOverride(6.f)
				[
					SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite())
					.ColorAndOpacity(Tint(Gold))
				]
			];
		}
		// A locked entry is DISABLED, not hidden: the mock lists it as "???" so
		// the player can see there is more to find, and a disabled chip says
		// exactly that without pretending to open.
		Row(Line, I == SelectedEntry, /*bEnabled=*/!Entry.bLocked,
		    [this, I]() { SelectedEntry = I; RebuildEntryList(); RebuildPage(); });
	}
}

void SVoxelCodexScreen::RebuildPage()
{
	using namespace VoxelUITheme;
	if (!PageBox.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	PageBox->ClearChildren();

	auto Kind = [&](const FText& Text)
	{
		PageBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(Text).Font(Style.Mono(L.JournalPageKindSize))
			.ColorAndOpacity(Tint(PageKind))
		];
	};
	auto Title = [&](const FText& Text, const FText& Sub)
	{
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 6.f))
		[
			SNew(STextBlock).Text(Text).Font(Style.Serif(L.CodexTitleSize))
			.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true)
		];
		if (!Sub.IsEmpty())
		{
			PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 12.f))
			[
				SNew(STextBlock).Text(Sub).Font(Style.HandItalic(L.JournalPageStampSize))
				.ColorAndOpacity(Tint(InkFaded)).AutoWrapText(true)
			];
		}
	};

	if (Category == kRecipes)
	{
		if (!VisibleRecipes.IsValidIndex(SelectedEntry))
		{
			return;
		}
		const FVoxelRecipe& Recipe = Data.Recipes[VisibleRecipes[SelectedEntry]];
		Kind(Recipe.Category.ToUpper());
		Title(Recipe.Name, VoxelUIStrings::CodexYield(Recipe.Category, Recipe.Yield));
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 14.f))
		[
			SNew(STextBlock).Text(Recipe.Description).Font(Style.Hand(L.CodexBodySize))
			.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true).LineHeightPercentage(1.45f)
		];

		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			SNew(STextBlock).Text(VoxelUIStrings::CodexIngredients())
			.Font(Style.Mono(L.JournalPageKindSize)).ColorAndOpacity(Tint(PageDropCap))
		];
		for (const FVoxelRecipeIngredient& Ingredient : Recipe.Ingredients)
		{
			PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 5.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 10.f, 0.f))
				[
					VoxelScreenChrome::ItemGlyph(Ingredient.Glyph, L.ListRowIconSize)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::CodexIngredientLine(Ingredient.Needed, Ingredient.Name))
					.Font(Style.Mono(L.CodexBodySize)).ColorAndOpacity(Tint(ParchmentInk))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Ingredient.Held >= 0 ? VoxelUIStrings::CodexHeldLine(Ingredient.Held)
					                           : VoxelUIStrings::CodexHeldUnknown())
					.Font(Style.HandItalic(L.CodexBodySize - 1))
					.ColorAndOpacity(Tint(Ingredient.Held >= 0 && Ingredient.Held >= Ingredient.Needed
					                          ? ParchmentInk : InkFaded))
				]
			];
		}

		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 14.f, 0.f, 0.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					VoxelScreenChrome::ItemSlot(
						[&]
						{
							FVoxelInventoryScreenItem Out;
							Out.ItemId = TEXT("codex.yield");
							Out.Glyph = Recipe.Glyph;
							Out.Count = Recipe.Yield;
							Out.DisplayName = Recipe.Name;
							return Out;
						}(),
						L.CodexRecipeSlotSize)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 3.f, 0.f, 0.f))
				[
					SNew(STextBlock).Text(VoxelUIStrings::CodexYieldLabel())
					.Font(Style.Mono(L.JournalPageKindSize)).ColorAndOpacity(Tint(InkFaded))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			.Padding(FMargin(16.f, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock).Text(VoxelUIStrings::CodexMadeAt(Recipe.Station))
				.Font(Style.Hand(L.CodexBodySize)).ColorAndOpacity(Tint(ParchmentInk))
				.AutoWrapText(true)
			]
		];
		return;
	}

	if (Category == kPlaces)
	{
		if (!Places.IsValidIndex(SelectedEntry))
		{
			return;
		}
		const FVoxelMapMark& Mark = Places[SelectedEntry];
		Kind(VoxelUIStrings::CodexPlaces().ToUpper());
		Title(Mark.Name, FText::GetEmpty());
		PageBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::MapPositionValue(FVector(Mark.WorldXY.X, Mark.WorldXY.Y, 0.0)))
			.Font(Style.Mono(L.CodexBodySize)).ColorAndOpacity(Tint(ParchmentInk))
		];
		return;
	}

	if (!Data.Categories.IsValidIndex(Category))
	{
		return;
	}
	const FVoxelCodexCategory& Cat = Data.Categories[Category];
	if (!Cat.Entries.IsValidIndex(SelectedEntry))
	{
		return;
	}
	const FVoxelCodexEntry& Entry = Cat.Entries[SelectedEntry];
	Kind(Cat.Kind);
	if (Entry.bLocked)
	{
		Title(Entry.Name, FText::GetEmpty());
		PageBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(VoxelUIStrings::CodexLocked())
			.Font(Style.HandItalic(L.CodexBodySize)).ColorAndOpacity(Tint(InkFaded))
		];
		return;
	}
	Title(Entry.Name, Entry.Subtitle);

	// The fact table: two columns, label left in small caps, value right.
	for (const TPair<FText, FText>& Fact : Entry.Facts)
	{
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 3.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(120.f)
				[
					SNew(STextBlock).Text(Fact.Key).Font(Style.Mono(L.CodexBodySize - 1))
					.ColorAndOpacity(Tint(InkFaded))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock).Text(Fact.Value).Font(Style.Mono(L.CodexBodySize))
				.ColorAndOpacity(Tint(ParchmentInk))
			]
		];
	}

	PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 12.f, 0.f, 10.f))
	[
		SNew(SBox).HeightOverride(1.f)
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.4f))
		]
	];
	for (const FText& Paragraph : Entry.Paragraphs)
	{
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
		[
			SNew(STextBlock).Text(Paragraph).Font(Style.Hand(L.CodexBodySize + 1))
			.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true).LineHeightPercentage(1.45f)
		];
	}
	if (!Entry.Quote.IsEmpty())
	{
		PageBox->AddSlot().AutoHeight().Padding(FMargin(14.f, 6.f, 0.f, 0.f))
		[
			SNew(STextBlock).Text(Entry.Quote).Font(Style.HandItalic(L.CodexBodySize + 1))
			.ColorAndOpacity(Tint(PageDropCap)).AutoWrapText(true).LineHeightPercentage(1.4f)
		];
	}
}

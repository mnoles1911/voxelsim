#include "SVoxelPlayerScreen.h"

#include "SVoxelMenuButton.h"
#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelPlayerScreenDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// .rep-row's three bands. The mock keys them off a class on the row, which it
// sets by hand; the threshold is inferred from the rows it applies them to
// (friendly at +68, hostile at -84, everything between plain).
FColor StandingColour(int32 Standing)
{
	using namespace VoxelUITheme;
	if (Standing >= 50)  { return RarityUncommon; }
	if (Standing <= -50) { return HpBright; }
	return Gold;
}
} // namespace SVoxelPlayerScreenDetail

SVoxelPlayerScreen::~SVoxelPlayerScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

TArray<FVoxelScreenAction> SVoxelPlayerScreen::Actions()
{
	return {
		FVoxelScreenAction(FText::FromString(TEXT("Q/E")), VoxelUIStrings::ScreenActionPage()),
		FVoxelScreenAction(FText::FromString(TEXT("ESC")), VoxelUIStrings::ScreenActionClose(),
		                   /*bSpacerBefore=*/true),
	};
}

void SVoxelPlayerScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.SubTabRowBottomGap))
		[
			BuildSubTabs()
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(PageSwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()[BuildStatsPage()]
			+ SWidgetSwitcher::Slot()[BuildSkillsPage()]
			+ SWidgetSwitcher::Slot()[BuildPerksPage()]
			+ SWidgetSwitcher::Slot()[BuildReputationPage()]
		]
	];

	RebuildSkillDetail();
	RebuildPerkDetail();
}

TSharedRef<SWidget> SVoxelPlayerScreen::BuildSubTabs()
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

	auto Tab = [&](const FText& Label, EVoxelPlayerPage Which, int32 Index)
	{
		Row->AddSlot().AutoWidth().Padding(FMargin(0.f, 0.f, L.SubTabGap, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(Label)
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.SubTabLabelSize)
			.LetterSpacing(L.SubTabLetterSpacing)
			.MinHeight(0.f)
			.MinWidth(L.SubTabMinWidth)
			.Active_Lambda([this, Which]() { return Page == Which; })
			.OnClicked_Lambda([this, Which, Index]()
			{
				Page = Which;
				if (PageSwitcher.IsValid()) { PageSwitcher->SetActiveWidgetIndex(Index); }
				return FReply::Handled();
			})
		];
	};
	Tab(VoxelUIStrings::PlayerSubStats(), EVoxelPlayerPage::Stats, 0);
	Tab(VoxelUIStrings::PlayerSubSkills(), EVoxelPlayerPage::Skills, 1);
	Tab(VoxelUIStrings::PlayerSubPerks(), EVoxelPlayerPage::Perks, 2);
	Tab(VoxelUIStrings::PlayerSubReputation(), EVoxelPlayerPage::Reputation, 3);

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Row]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SubTabRowPadBottom, 0.f, 0.f))
		[
			VoxelScreenChrome::CardRule(0.35f)
		];
}

TSharedRef<SWidget> SVoxelPlayerScreen::BuildStatsPage()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// --- .char-card ----------------------------------------------------------
	FSlateFontInfo NameFont = Style.Serif(L.PlayerNameSize);
	NameFont.LetterSpacing = L.PlayerNameSpacing;

	auto GearColumn = [&](int32 First)
	{
		TSharedRef<SVerticalBox> Col = SNew(SVerticalBox);
		// THE PLAYER SCREEN'S OWN LABELS, not the inventory's. The two mocks
		// disagree about the left column -- see VoxelUIStrings::
		// PlayerEquipSlotNames -- and sharing one list put the chest piece on a
		// NECK socket in the first capture of this screen.
		const TArray<FText>& Names = VoxelUIStrings::PlayerEquipSlotNames();
		for (int32 I = First; I < First + 4; ++I)
		{
			const FVoxelInventoryScreenItem Item = Data.Equipment.IsValidIndex(I)
				? Data.Equipment[I] : FVoxelInventoryScreenItem();
			// THE LABEL SITS UNDER THE CELL, NOT OVER IT. The mock's `.eq .tag`
			// is inside the socket because its cells are mostly empty; ours are
			// filled by the seeded gear, and the first capture had HEAD, CHEST
			// and RING printed illegibly across the glyph they labelled. Under
			// the cell is where the inventory screen already puts it, so the
			// two paperdolls now read the same way.
			Col->AddSlot().AutoHeight().Padding(FMargin(0.f, L.PlayerEquipGap * 0.5f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					VoxelScreenChrome::ItemSlot(Item, L.PlayerEquipSlotSize)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				.Padding(FMargin(0.f, 2.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(Names.IsValidIndex(I) ? Names[I] : FText::GetEmpty())
					.Font(Style.Mono(L.InvSlotQtySize - 2))
					.ColorAndOpacity(FVoxelUIStyle::MutedColour())
					.Visibility(EVisibility::HitTestInvisible)
				]
			];
		}
		return Col;
	};

	const float XpFraction = Data.XpForNextLevel > 0
		? float(Data.Xp) / float(Data.XpForNextLevel) : 0.f;

	TSharedRef<SVerticalBox> CharCard =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
			[
				SNew(STextBlock).Text(Data.Name).Font(NameFont)
				.ColorAndOpacity(Tint(InkBright))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right).VAlign(VAlign_Bottom)
			[
				SNew(STextBlock).Text(Data.Epithet).Font(Style.Mono(L.PlayerHeadSize))
				.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 7.f))
		[
			VoxelScreenChrome::CardRule()
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[GearColumn(0)]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(10.f, 0.f))
			[
				// SAYS WHY IT IS EMPTY. The first capture drew this as a bare
				// black rectangle the height of the card, which reads as a
				// failed render rather than as a turntable this front end
				// cannot draw. The inventory screen's own render frame already
				// carried the note; this one now matches it.
				VoxelScreenChrome::IronWell(
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(FMargin(12.f))
					[
						SNew(STextBlock)
						.Text(VoxelUIStrings::InvNoEquipment())
						.Font(Style.HandItalic(L.InvStatsSize - 1))
						.ColorAndOpacity(FVoxelUIStyle::MutedColour())
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					],
					FMargin(0.f))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[GearColumn(4)]
		]
		// .level-bar
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
		[
			VoxelScreenChrome::CardRule()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(VoxelUIStrings::PlayerMainLevel())
					.Font(Style.Mono(L.PlayerHeadSize)).ColorAndOpacity(FVoxelUIStyle::TitleColour())
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right)
				[
					SNew(STextBlock).Text(FText::AsNumber(Data.Level))
					.Font(Style.Serif(L.JournalCardHeadSize)).ColorAndOpacity(Tint(InkBright))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f))
			[
				VoxelScreenChrome::Track(L.PlayerLevelTrackHeight, Tint(Gold), XpFraction)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(VoxelUIStrings::PlayerXp())
					.Font(Style.Mono(L.PlayerHeadSize)).ColorAndOpacity(FVoxelUIStyle::TitleColour())
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right)
				[
					SNew(STextBlock).Text(VoxelUIStrings::PlayerXpValue(Data.Xp, Data.XpForNextLevel))
					.Font(Style.Mono(L.PlayerHeadSize)).ColorAndOpacity(FVoxelUIStyle::TitleColour())
				]
			]
		];

	// --- the stat blocks down the right --------------------------------------
	TSharedRef<SVerticalBox> Blocks = SNew(SVerticalBox);
	for (const FVoxelPlayerStatBlock& Block : Data.Blocks)
	{
		TSharedRef<SUniformGridPanel> Grid =
			SNew(SUniformGridPanel).SlotPadding(FMargin(7.f, 2.f));
		for (int32 I = 0; I < Block.Attributes.Num(); ++I)
		{
			const FVoxelPlayerAttribute& Attr = Block.Attributes[I];
			Grid->AddSlot(I % 3, I / 3)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 8.f, 0.f))
				[
					VoxelScreenChrome::ItemGlyph(Attr.Glyph, L.PlayerAttrGlyphSize)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					// ELLIPSIS AND A GAP. The COMBAT block's names ("Armour ·
					// blunt") are long enough to meet their own value in a
					// third-of-a-card column, and the first capture read
					// "Armour · slasl21" where the two collided.
					SNew(STextBlock).Text(Attr.Name).Font(Style.Serif(L.PlayerAttrNameSize))
					.ColorAndOpacity(Attr.bFeatured ? FVoxelUIStyle::TitleColour()
					                                : FVoxelUIStyle::BodyColour())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock).Text(FText::AsNumber(Attr.Value))
					.Font(Style.Mono(L.PlayerAttrValueSize + 4))
					.ColorAndOpacity(Attr.bFeatured ? FSlateColor(Tint(CartoucheText))
					                                : FVoxelUIStyle::TitleColour())
				]
			];
		}

		Blocks->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 9.f))
		[
			VoxelScreenChrome::OakCard(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					VoxelScreenChrome::PanelHeading(Block.Title, Block.Meta)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 5.f, 0.f, 7.f))
				[
					VoxelScreenChrome::CardRule()
				]
				+ SVerticalBox::Slot().AutoHeight()[Grid],
				FMargin(L.PlayerCardPadX + 1.f, L.PlayerCardPadY))
		];
	}

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.95f)
		[
			VoxelScreenChrome::OakCard(CharCard, FMargin(L.PlayerCardPadX, L.PlayerCardPadY))
		]
		+ SHorizontalBox::Slot().FillWidth(1.05f).Padding(FMargin(L.PlayerColumnGap, 0.f, 0.f, 0.f))
		[
			SNew(SScrollBox) + SScrollBox::Slot()[Blocks]
		];
}

TSharedRef<SWidget> SVoxelPlayerScreen::BuildSkillsPage()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// --- the discipline list -------------------------------------------------
	TSharedRef<SVerticalBox> Disciplines = SNew(SVerticalBox);
	for (int32 I = 0; I < Data.Disciplines.Num(); ++I)
	{
		const FVoxelSkillDiscipline& Discipline = Data.Disciplines[I];
		Disciplines->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 3.f))
		[
			SNew(SVoxelMenuButton)
			.Text(Discipline.Name)
			.CountLabel(VoxelUIStrings::PlayerDisciplineTally(Discipline.Unlocked, Discipline.Total))
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.PlayerAttrNameSize)
			.MinHeight(0.f)
			.ContentPadding(FMargin(10.f, 6.f))
			.Active_Lambda([this, I]() { return SelectedDiscipline == I; })
			.OnClicked_Lambda([this, I]()
			{
				SelectedDiscipline = I;
				SelectedNode = 0;
				RebuildSkillDetail();
				return FReply::Handled();
			})
		];
	}

	// --- the node ladder -----------------------------------------------------
	// See the header: tier rows in dependency order, not the mock's graph.
	TSharedRef<SVerticalBox> Column =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			VoxelScreenChrome::OakCard(
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(VoxelUIStrings::PlayerSkillPoints())
					.Font(Style.Mono(L.PlayerHeadSize)).ColorAndOpacity(FVoxelUIStyle::TitleColour())
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::AsNumber(Data.SkillPoints))
					.Font(Style.Serif(L.JournalCardHeadSize)).ColorAndOpacity(Tint(InkBright))
				],
				FMargin(10.f, 5.f))
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			VoxelScreenChrome::IronWell(
				SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(SkillNodeBox, SVerticalBox)],
				FMargin(10.f))
		];

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(200.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					VoxelScreenChrome::PanelHeading(VoxelUIStrings::PlayerDisciplines())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f))
				[
					VoxelScreenChrome::CardRule()
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(SScrollBox) + SScrollBox::Slot()[Disciplines]
				]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(L.PlayerColumnGap, 0.f))
		[
			Column
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(280.f)
			[
				VoxelScreenChrome::OakCard(
					SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(SkillDetailBox, SVerticalBox)],
					FMargin(L.PlayerCardPadX, L.PlayerCardPadY))
			]
		];
}

void SVoxelPlayerScreen::RebuildSkillDetail()
{
	using namespace VoxelUITheme;
	if (!SkillNodeBox.IsValid() || !SkillDetailBox.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	SkillNodeBox->ClearChildren();
	SkillDetailBox->ClearChildren();

	if (!Data.Disciplines.IsValidIndex(SelectedDiscipline))
	{
		return;
	}
	const FVoxelSkillDiscipline& Discipline = Data.Disciplines[SelectedDiscipline];
	if (Discipline.Nodes.Num() == 0)
	{
		// A discipline with no authored nodes -- five of the six seeded ones.
		SkillNodeBox->AddSlot().AutoHeight().Padding(FMargin(10.f, 30.f))
		[
			SNew(STextBlock).Text(VoxelUIStrings::CodexLocked())
			.Font(Style.HandItalic(L.MapDrawerRowSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			.Justification(ETextJustify::Center)
		];
		return;
	}

	// Group by tier, so each row of the ladder is one step of the dependency
	// chain and reading downwards reads the tree's order.
	int32 MaxTier = 0;
	for (const FVoxelSkillNode& Node : Discipline.Nodes)
	{
		MaxTier = FMath::Max(MaxTier, Node.Tier);
	}
	for (int32 Tier = 0; Tier <= MaxTier; ++Tier)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		bool bAny = false;
		for (int32 I = 0; I < Discipline.Nodes.Num(); ++I)
		{
			const FVoxelSkillNode& Node = Discipline.Nodes[I];
			if (Node.Tier != Tier)
			{
				continue;
			}
			bAny = true;
			// LOCKED NODES ARE DIMMED AND STILL CLICKABLE. The mock refuses the
			// click; showing a locked node's requirement is the whole reason a
			// player looks at one, so this lets it be selected and lets the
			// detail panel say what it needs.
			Row->AddSlot().AutoWidth().Padding(FMargin(4.f))
			[
				SNew(SVoxelMenuButton)
				.Text(Node.Name)
				.CountLabel(VoxelUIStrings::PlayerRankBadge(Node.Rank, Node.MaxRank))
				.Variant(EVoxelMenuButtonVariant::Chip)
				.FontSize(L.SubTabLabelSize)
				.LetterSpacing(L.SubTabLetterSpacing)
				.MinHeight(0.f)
				.ContentPadding(FMargin(10.f, 8.f))
				.Active_Lambda([this, I]() { return SelectedNode == I; })
				.TextColorOverride(Node.bUnlocked ? TOptional<FLinearColor>(Tint(Gold))
				                 : Node.bAvailable ? TOptional<FLinearColor>()
				                                   : TOptional<FLinearColor>(Tint(InkMute, 0.6f)))
				.OnClicked_Lambda([this, I]()
				{
					SelectedNode = I;
					RebuildSkillDetail();
					return FReply::Handled();
				})
			];
		}
		if (bAny)
		{
			SkillNodeBox->AddSlot().AutoHeight().HAlign(HAlign_Center)
			.Padding(FMargin(0.f, 6.f))
			[
				Row
			];
		}
	}

	if (!Discipline.Nodes.IsValidIndex(SelectedNode))
	{
		return;
	}
	const FVoxelSkillNode& Node = Discipline.Nodes[SelectedNode];
	FSlateFontInfo TierFont = Style.Mono(L.PlayerHeadSize);
	SkillDetailBox->AddSlot().AutoHeight()
	[
		SNew(STextBlock)
		.Text(FText::Format(FTextFormat::FromString(TEXT("TIER {0} · {1}")),
		                    FText::AsNumber(Node.Tier + 1), Discipline.Name))
		.Font(TierFont).ColorAndOpacity(FVoxelUIStyle::TitleColour())
	];
	SkillDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 8.f))
	[
		SNew(STextBlock).Text(Node.Name).Font(Style.Serif(L.CodexTitleSize))
		.ColorAndOpacity(Tint(InkBright))
	];
	SkillDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
	[
		SNew(STextBlock).Text(Node.Flavour).Font(Style.HandItalic(L.CodexBodySize))
		.ColorAndOpacity(FVoxelUIStyle::DimColour()).AutoWrapText(true)
	];
	SkillDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 6.f))
	[
		SNew(STextBlock).Text(VoxelUIStrings::PlayerRankLine(Node.Rank, Node.Rank + 1))
		.Font(Style.Mono(L.CodexBodySize)).ColorAndOpacity(FVoxelUIStyle::TitleColour())
	];
	SkillDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
	[
		SNew(STextBlock).Text(Node.RankEffect).Font(Style.Mono(L.CodexBodySize))
		.ColorAndOpacity(FVoxelUIStyle::BodyColour()).AutoWrapText(true)
	];
	if (!Node.Requires.IsEmpty())
	{
		SkillDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
		[
			SNew(STextBlock).Text(Node.Requires).Font(Style.Mono(L.CodexBodySize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour()).AutoWrapText(true)
		];
	}
	SkillDetailBox->AddSlot().AutoHeight()
	[
		// Disabled always: spending a point needs a progression system to spend
		// it in. Drawn for the same reason the CRAFT button is.
		SNew(SVoxelMenuButton)
		.Text(VoxelUIStrings::PlayerUnlockCost(Node.Cost))
		.Variant(EVoxelMenuButtonVariant::Leather)
		.FontSize(L.InvFilterSize + 1)
		.MinHeight(0.f)
		.IsEnabled(false)
	];
}

TSharedRef<SWidget> SVoxelPlayerScreen::BuildPerksPage()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	FText CurrentGroup;
	for (int32 I = 0; I < Data.Perks.Num(); ++I)
	{
		const FVoxelPerk& Perk = Data.Perks[I];
		if (!Perk.Group.EqualTo(CurrentGroup))
		{
			CurrentGroup = Perk.Group;
			Rows->AddSlot().AutoHeight()[VoxelScreenChrome::ListDivider(Perk.Group)];
		}
		const FSlateColor RowColour = Perk.bLocked ? FSlateColor(Tint(InkMute))
		                                           : FSlateColor(Tint(ParchmentInk));
		Rows->AddSlot().AutoHeight()
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SVoxelMenuButton)
				.Text(FText::GetEmpty())
				.Variant(EVoxelMenuButtonVariant::Chip)
				.MinHeight(0.f)
				.Active_Lambda([this, I]() { return SelectedPerk == I; })
				.ContentPadding(FMargin(L.ListRowPadX, L.ListRowPadY))
				.OnClicked_Lambda([this, I]()
				{
					SelectedPerk = I;
					RebuildPerkDetail();
					return FReply::Handled();
				})
			]
			+ SOverlay::Slot().Padding(FMargin(L.ListRowPadX, L.ListRowPadY))
			[
				SNew(SHorizontalBox)
				.Visibility(EVisibility::HitTestInvisible)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
				[
					VoxelScreenChrome::ItemGlyph(Perk.bLocked ? TEXT("stone") : TEXT("goldb"),
					                             L.ListRowIconSize)
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Perk.Name).Font(Style.Hand(L.ListRowSize + 1))
					.ColorAndOpacity(RowColour)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(FMargin(12.f, 0.f))
				[
					SNew(STextBlock).Text(FText::AsNumber(Perk.Level))
					.Font(Style.Mono(L.ListRowSize)).ColorAndOpacity(RowColour)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).MinDesiredWidth(60.f)
					[
						SNew(STextBlock)
						.Text(Perk.bLocked ? VoxelUIStrings::PlayerStatusLocked()
						    : Perk.bOwned  ? VoxelUIStrings::PlayerStatusOwned()
						                   : FText::FromString(TEXT("—")))
						.Font(Style.Mono(L.ListRowSize))
						.ColorAndOpacity(RowColour)
						.Justification(ETextJustify::Right)
					]
				]
			]
		];
	}

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			VoxelScreenChrome::ParchmentPanel(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					VoxelScreenChrome::ListHeader(VoxelUIStrings::PlayerColumnName(),
					                              {VoxelUIStrings::PlayerColumnLevel(),
					                               VoxelUIStrings::PlayerColumnStatus()})
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(SScrollBox) + SScrollBox::Slot()[Rows]
				])
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.PlayerColumnGap, 0.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(340.f)
			[
				// .perks-detail -- the one crimson plate in the front end.
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
				]
				+ SOverlay::Slot().Padding(FMargin(2.f))
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PerkPanelEdge))
				]
				// 4, not 3: the crimson edge band was one unit. ADR-0011.
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(Mix(PerkPanelTop, PerkPanelBottom)))
				]
				+ SOverlay::Slot().Padding(FMargin(22.f, 18.f))
				[
					SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(PerkDetailBox, SVerticalBox)]
				]
			]
		];
}

void SVoxelPlayerScreen::RebuildPerkDetail()
{
	using namespace VoxelUITheme;
	if (!PerkDetailBox.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	PerkDetailBox->ClearChildren();

	if (!Data.Perks.IsValidIndex(SelectedPerk))
	{
		return;
	}
	const FVoxelPerk& Perk = Data.Perks[SelectedPerk];
	FSlateFontInfo TitleFont = Style.Serif(22);
	TitleFont.LetterSpacing = 136; // 3 px at 22 px

	PerkDetailBox->AddSlot().AutoHeight()
	[
		SNew(STextBlock).Text(Perk.Name).Font(TitleFont).ColorAndOpacity(Tint(InkBright))
		.AutoWrapText(true)
	];
	PerkDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 14.f))
	[
		SNew(STextBlock).Text(Perk.Requirement).Font(Style.Mono(L.PlayerHeadSize))
		.ColorAndOpacity(Tint(CalloutTag))
	];
	PerkDetailBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 12.f))
	[
		SNew(STextBlock).Text(Perk.Description).Font(Style.Hand(L.CodexBodySize))
		.ColorAndOpacity(Tint(Parchment)).AutoWrapText(true).LineHeightPercentage(HandLineHeight(1.4f))
	];
	PerkDetailBox->AddSlot().AutoHeight()
	[
		SNew(STextBlock).Text(Perk.Flavour).Font(Style.HandItalic(L.CodexBodySize))
		.ColorAndOpacity(Tint(Parchment, 0.7f)).AutoWrapText(true).LineHeightPercentage(HandLineHeight(1.4f))
	];
}

TSharedRef<SWidget> SVoxelPlayerScreen::BuildReputationPage()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FVoxelFactionStanding& Faction : Data.Factions)
	{
		const FColor Colour = SVoxelPlayerScreenDetail::StandingColour(Faction.Standing);
		const float Magnitude = FMath::Clamp(FMath::Abs(Faction.Standing) / 100.f, 0.f, 1.f);
		const bool bNegative = Faction.Standing < 0;

		// THE BAR GROWS FROM THE CENTRE, in whichever direction the standing
		// runs. Two fill slots either side of a hairline, rather than the
		// mock's absolutely-positioned div, because a Slate box panel can
		// express "half the width, filled from the middle outwards" and cannot
		// express `left:50%; width:34%`.
		TSharedRef<SWidget> Bar =
			SNew(SBox)
			.HeightOverride(L.PlayerRepBarHeight)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentEdge))
				]
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.35f))
				]
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(bNegative ? 1.f - Magnitude : 1.f)
					[
						SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot().FillWidth(FMath::Max(Magnitude, 0.0001f))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Colour))
					]
					+ SHorizontalBox::Slot().FillWidth(bNegative ? 1.f : 1.f - Magnitude)
					[
						SNullWidget::NullWidget
					]
				]
			];

		Rows->AddSlot().AutoHeight().Padding(FMargin(L.ListRowPadX, 12.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(L.PlayerRepNameColumn)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(FMargin(0.f, 0.f, 10.f, 0.f))
					[
						SNew(SBox).WidthOverride(20.f).HeightOverride(24.f)
						[
							SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Colour))
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(Faction.Name).Font(Style.Serif(L.PlayerRepNameSize))
						.ColorAndOpacity(Tint(ParchmentInk))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			.Padding(FMargin(18.f, 0.f))
			[
				Bar
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(L.PlayerRepValueColumn)
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(Faction.Standing))
					.Font(Style.Mono(L.ListRowSize))
					.ColorAndOpacity(Tint(Faction.Standing == 0 ? ParchmentInk : Colour))
					.Justification(ETextJustify::Right)
				]
			]
		];
	}

	return VoxelScreenChrome::ParchmentPanel(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			VoxelScreenChrome::ListHeader(VoxelUIStrings::PlayerColumnFaction(),
			                              {VoxelUIStrings::PlayerColumnStanding(),
			                               VoxelUIStrings::PlayerColumnValue()})
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox) + SScrollBox::Slot()[Rows]
		]);
}

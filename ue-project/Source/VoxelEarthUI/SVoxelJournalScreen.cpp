#include "SVoxelJournalScreen.h"

#include "SVoxelMenuButton.h"
#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

SVoxelJournalScreen::~SVoxelJournalScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

TArray<FVoxelScreenAction> SVoxelJournalScreen::Actions()
{
	return {
		FVoxelScreenAction(FText::FromString(TEXT("Q/E")), VoxelUIStrings::ScreenActionPage()),
		FVoxelScreenAction(FText::FromString(TEXT("ESC")), VoxelUIStrings::ScreenActionClose(),
		                   /*bSpacerBefore=*/true),
	};
}

void SVoxelJournalScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(L.JournalListWidth)[BuildLeftColumn()]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(L.JournalColumnGap, 0.f, 0.f, 0.f))
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

	RebuildList();
	RebuildPage();
}

TSharedRef<SWidget> SVoxelJournalScreen::BuildLeftColumn()
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// .sect-switch -- the two top-level sections, as Chip buttons.
	TSharedRef<SHorizontalBox> Sections =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, -2.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::JournalSectionEntries())
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this]() { return !bGoalsSection; })
			.OnClicked_Lambda([this]()
			{
				bGoalsSection = false;
				RebuildList(); RebuildPage();
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::JournalSectionGoals())
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize)
			.LetterSpacing(L.InvFilterSpacing)
			.MinHeight(0.f)
			.Active_Lambda([this]() { return bGoalsSection; })
			.OnClicked_Lambda([this]()
			{
				bGoalsSection = true;
				RebuildList(); RebuildPage();
				return FReply::Handled();
			})
		];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[Sections]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f))
		[
			SAssignNew(SubTabRow, SHorizontalBox)
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(ListBox, SVerticalBox)]
		];
}

void SVoxelJournalScreen::RebuildList()
{
	using namespace VoxelUITheme;
	if (!ListBox.IsValid() || !SubTabRow.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	ListBox->ClearChildren();
	SubTabRow->ClearChildren();

	// One list card. The whole row is a Chip button so the selected card takes
	// the same gold treatment every other selectable row in this front end does.
	auto Card = [&](const FText& Stamp, const FColor& StampColour, const FText& Head,
	                const FText& Snip, bool bSelected, TFunction<void()> OnPick,
	                TSharedRef<SWidget> Footer)
	{
		// SelfHitTestInvisible on the column and HitTestInvisible on each line:
		// the text must not swallow the click meant for the chip underneath,
		// but the FOOTER added below it is a real button and has to stay
		// hittable -- which is exactly the distinction between the two flags.
		TSharedRef<SVerticalBox> Body =
			SNew(SVerticalBox)
			.Visibility(EVisibility::SelfHitTestInvisible)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(Stamp).Font(Style.Mono(L.JournalStampSize))
				.ColorAndOpacity(Tint(StampColour))
				.Visibility(EVisibility::HitTestInvisible)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 3.f, 0.f, 0.f))
			[
				SNew(STextBlock).Text(Head).Font(Style.Serif(L.JournalCardHeadSize))
				.ColorAndOpacity(FVoxelUIStyle::BodyColour())
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
				.Visibility(EVisibility::HitTestInvisible)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
			[
				// ELLIPSIS, NOT A HARD CLIP. `.card .snip` is
				// `text-overflow:ellipsis` and the first capture cut its
				// snippets mid-word against the card edge, which reads as a
				// layout fault rather than as a truncated preview.
				SNew(STextBlock).Text(Snip).Font(Style.Mono(L.JournalCardSnipSize))
				.ColorAndOpacity(FVoxelUIStyle::DimColour())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Visibility(EVisibility::HitTestInvisible)
			];
		Body->AddSlot().AutoHeight().Padding(FMargin(0.f, 7.f, 0.f, 0.f))[Footer];

		// A CARD IS AN EMPTY CHIP WITH ITS CONTENT LAID OVER IT. The chip
		// variant supplies the plate, the selection ring and the click; it
		// centres a single line of text, and this row is three lines plus a
		// footer that in the goals case has a button of its own. So the chip is
		// built with no label and the real content sits on top of it,
		// hit-test-invisible so every click still reaches the button beneath --
		// except the footer, which is added outside that box and keeps its own.
		ListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.JournalCardGap))
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SVoxelMenuButton)
				.Text(FText::GetEmpty())
				.Variant(EVoxelMenuButtonVariant::Chip)
				.MinHeight(0.f)
				.Active(bSelected)
				.ContentPadding(FMargin(L.JournalCardPadX, L.JournalCardPadY))
				.OnClicked_Lambda([OnPick]() { OnPick(); return FReply::Handled(); })
			]
			+ SOverlay::Slot().Padding(FMargin(L.JournalCardPadX, L.JournalCardPadY))
			[
				Body
			]
		];
	};

	if (!bGoalsSection)
	{
		SubTabRow->AddSlot().AutoWidth()
		[
			SNew(STextBlock)
			.Text(FText::Format(FTextFormat::FromString(TEXT("{0}  {1}")),
			                    VoxelUIStrings::JournalEntriesTab(),
			                    FText::AsNumber(Data.Entries.Num())))
			.Font(Style.Mono(L.MapDrawerRowSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];

		// The composer card, disabled. See the header for why the pen is not
		// offered rather than offered and then silently discarded.
		ListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.JournalCardGap))
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SVoxelMenuButton)
				.Text(FText::GetEmpty())
				.Variant(EVoxelMenuButtonVariant::Chip)
				.MinHeight(0.f)
				.IsEnabled(false)
				.ContentPadding(FMargin(L.JournalCardPadX, L.JournalCardPadY))
			]
			+ SOverlay::Slot().Padding(FMargin(L.JournalCardPadX, L.JournalCardPadY))
			[
				SNew(SBox).Visibility(EVisibility::HitTestInvisible)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(VoxelUIStrings::JournalNewEntry())
						.Font(Style.Mono(L.JournalStampSize))
						.ColorAndOpacity(FVoxelUIStyle::MutedColour())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 3.f, 0.f, 0.f))
					[
						SNew(STextBlock).Text(VoxelUIStrings::JournalWriteHere())
						.Font(Style.Serif(L.JournalCardHeadSize))
						.ColorAndOpacity(FVoxelUIStyle::MutedColour())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 2.f, 0.f, 0.f))
					[
						SNew(STextBlock).Text(Data.TodayStamp)
						.Font(Style.Mono(L.JournalCardSnipSize))
						.ColorAndOpacity(FVoxelUIStyle::MutedColour())
					]
				]
			]
		];

		if (Data.Entries.Num() == 0)
		{
			ListBox->AddSlot().AutoHeight().Padding(FMargin(10.f, 22.f))
			[
				SNew(STextBlock).Text(VoxelUIStrings::JournalEmpty())
				.Font(Style.HandItalic(L.MapDrawerRowSize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
				.Justification(ETextJustify::Center)
			];
		}
		for (int32 I = 0; I < Data.Entries.Num(); ++I)
		{
			const FVoxelJournalEntry& Entry = Data.Entries[I];
			FString FirstLine;
			Entry.Body.ToString().Split(TEXT("\n"), &FirstLine, nullptr);
			Card(VoxelUIStrings::JournalCardStamp(Entry.Day, Entry.Season), Gold,
			     Entry.Title,
			     FText::FromString(FirstLine.IsEmpty() ? Entry.Body.ToString() : FirstLine),
			     I == SelectedEntry,
			     [this, I]() { SelectedEntry = I; RebuildList(); RebuildPage(); },
			     SNullWidget::NullWidget);
		}
		return;
	}

	// --- Goals ---------------------------------------------------------------
	int32 TrackedCount = 0;
	for (const FVoxelGoal& Goal : Data.Goals)
	{
		TrackedCount += Goal.bTracked ? 1 : 0;
	}
	auto SubTab = [&](const FText& Label, int32 Count, bool bUntracked)
	{
		SubTabRow->AddSlot().AutoWidth().Padding(FMargin(0.f, 0.f, -1.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(Label)
			.CountLabel(FText::AsNumber(Count))
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize)
			.MinHeight(0.f)
			.Active_Lambda([this, bUntracked]() { return bShowUntracked == bUntracked; })
			.OnClicked_Lambda([this, bUntracked]()
			{
				bShowUntracked = bUntracked;
				SelectedGoal = 0;
				RebuildList(); RebuildPage();
				return FReply::Handled();
			})
		];
	};
	SubTab(VoxelUIStrings::JournalTracked(), TrackedCount, false);
	SubTab(VoxelUIStrings::JournalUntracked(), Data.Goals.Num() - TrackedCount, true);

	VisibleGoals.Reset();
	for (int32 I = 0; I < Data.Goals.Num(); ++I)
	{
		if (Data.Goals[I].bTracked != bShowUntracked)
		{
			VisibleGoals.Add(I);
		}
	}
	if (VisibleGoals.Num() == 0)
	{
		ListBox->AddSlot().AutoHeight().Padding(FMargin(10.f, 22.f))
		[
			SNew(STextBlock)
			.Text(bShowUntracked ? VoxelUIStrings::JournalNothingUntracked()
			                     : VoxelUIStrings::JournalNothingTracked())
			.Font(Style.HandItalic(L.MapDrawerRowSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			.Justification(ETextJustify::Center)
		];
		return;
	}

	SelectedGoal = FMath::Clamp(SelectedGoal, 0, VisibleGoals.Num() - 1);
	for (int32 Row = 0; Row < VisibleGoals.Num(); ++Row)
	{
		const int32 GoalIndex = VisibleGoals[Row];
		const FVoxelGoal& Goal = Data.Goals[GoalIndex];
		int32 Done = 0;
		for (const FVoxelGoalStep& Step : Goal.Steps)
		{
			Done += Step.State == EVoxelGoalStepState::Done ? 1 : 0;
		}

		TSharedRef<SWidget> Footer =
			SNew(SVoxelMenuButton)
			.Text(Goal.bTracked ? VoxelUIStrings::JournalUntrack() : VoxelUIStrings::JournalTrack())
			.Variant(EVoxelMenuButtonVariant::Chip)
			.FontSize(L.InvFilterSize - 1)
			.MinHeight(0.f)
			.ContentPadding(FMargin(8.f, 2.f))
			.OnClicked_Lambda([this, GoalIndex]()
			{
				Data.Goals[GoalIndex].bTracked = !Data.Goals[GoalIndex].bTracked;
				RebuildList(); RebuildPage();
				return FReply::Handled();
			});

		Card(Goal.bTracked ? VoxelUIStrings::JournalKindTracked() : VoxelUIStrings::JournalKindUntracked(),
		     Goal.bTracked ? RarityUncommon : InkMute,
		     Goal.Name,
		     VoxelUIStrings::JournalStepProgress(Done, Goal.Steps.Num(), Goal.Place),
		     Row == SelectedGoal,
		     [this, Row]() { SelectedGoal = Row; RebuildList(); RebuildPage(); },
		     SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[Footer]);
	}
}

void SVoxelJournalScreen::RebuildPage()
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
	auto TitleAndStamp = [&](const FText& Title, const FText& Stamp)
	{
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 8.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(Title).Font(Style.Serif(L.JournalPageTitleSize))
				.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
			[
				SNew(SBox).HeightOverride(1.f)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.4f))
				]
			]
		];
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 14.f))
		[
			SNew(STextBlock).Text(Stamp).Font(Style.HandItalic(L.JournalPageStampSize))
			.ColorAndOpacity(Tint(InkFaded))
		];
	};

	if (!bGoalsSection)
	{
		if (!Data.Entries.IsValidIndex(SelectedEntry))
		{
			Kind(VoxelUIStrings::JournalKindEntry());
			PageBox->AddSlot().AutoHeight()
			[
				SNew(STextBlock).Text(VoxelUIStrings::JournalEmpty())
				.Font(Style.Hand(L.JournalPageBodySize)).ColorAndOpacity(Tint(ParchmentInk))
			];
			return;
		}
		const FVoxelJournalEntry& Entry = Data.Entries[SelectedEntry];
		Kind(VoxelUIStrings::JournalKindEntry());
		TitleAndStamp(Entry.Title, Entry.Stamp);
		// THE DROP CAP IS NOT PORTED. The mock floats the first character at
		// 48 px with the body wrapping around it, and Slate's text layout has no
		// float -- an STextBlock cannot flow around a sibling. The body is drawn
		// whole instead, which loses an ornament and no words.
		PageBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(Entry.Body).Font(Style.Hand(L.JournalPageBodySize))
			.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true).LineHeightPercentage(1.55f)
		];
		return;
	}

	if (!VisibleGoals.IsValidIndex(SelectedGoal))
	{
		return;
	}
	const FVoxelGoal& Goal = Data.Goals[VisibleGoals[SelectedGoal]];
	Kind(Goal.bTracked ? VoxelUIStrings::JournalKindTracked() : VoxelUIStrings::JournalKindUntracked());
	TitleAndStamp(Goal.Name, FText::Format(FTextFormat::FromString(TEXT("— {0} · {1}")),
	                                       Goal.Source, Goal.Place));
	PageBox->AddSlot().AutoHeight()
	[
		SNew(STextBlock).Text(Goal.Flavour).Font(Style.Hand(L.JournalPageBodySize))
		.ColorAndOpacity(Tint(ParchmentInk)).AutoWrapText(true).LineHeightPercentage(1.55f)
	];

	FSlateFontInfo SectionFont = Style.Serif(L.JournalSectionSize);
	SectionFont.LetterSpacing = 231; // 3 px at 13 px, matching .p-sect
	PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 16.f, 0.f, 8.f))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).Text(VoxelUIStrings::JournalSteps()).Font(SectionFont)
			.ColorAndOpacity(Tint(PageDropCap))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 0.f))
		[
			SNew(SBox).HeightOverride(1.f)
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.4f))
			]
		]
	];

	for (const FVoxelGoalStep& Step : Goal.Steps)
	{
		// .p-steps li::before -- a 10 px square whose colour is the state:
		// parchment-edge pending, rust for the step in hand, green for done.
		const FColor Marker = Step.State == EVoxelGoalStepState::Done ? RarityUncommon
		                    : Step.State == EVoxelGoalStepState::Current ? PageKind
		                                                                 : ParchmentEdge;
		PageBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 3.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			.Padding(FMargin(0.f, 5.f, 10.f, 0.f))
			[
				SNew(SBox)
				.WidthOverride(L.JournalStepMarkSize)
				.HeightOverride(L.JournalStepMarkSize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk))
					]
					+ SOverlay::Slot().Padding(FMargin(1.f))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Marker))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				// A DONE STEP IS DIMMED, NOT STRUCK THROUGH. The mock uses
				// `text-decoration: line-through`, which FSlateFontInfo has no
				// equivalent for; the green marker beside it already says done.
				SNew(STextBlock).Text(Step.Text).Font(Style.Mono(L.JournalStepSize))
				.ColorAndOpacity(Tint(Step.State == EVoxelGoalStepState::Done ? InkMute : ParchmentInk))
				.AutoWrapText(true)
			]
		];
	}
}

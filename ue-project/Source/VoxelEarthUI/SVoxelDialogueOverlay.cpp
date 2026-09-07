#include "SVoxelDialogueOverlay.h"

#include "SVoxelMenuButton.h"
#include "SVoxelOverlayChrome.h"
#include "SVoxelScreenChrome.h"
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

namespace SVoxelDialogueDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

FText BandLabel(EVoxelSkillCheckBand Band)
{
	switch (Band)
	{
	case EVoxelSkillCheckBand::Easy:   return VoxelUIStrings::DlgBandEasy();
	case EVoxelSkillCheckBand::Medium: return VoxelUIStrings::DlgBandMedium();
	case EVoxelSkillCheckBand::Hard:   return VoxelUIStrings::DlgBandHard();
	case EVoxelSkillCheckBand::None:   break;
	}
	return FText::GetEmpty();
}

FColor BandColour(EVoxelSkillCheckBand Band)
{
	using namespace VoxelUITheme;
	switch (Band)
	{
	case EVoxelSkillCheckBand::Easy:   return DlgCheckEasy;
	case EVoxelSkillCheckBand::Medium: return Gold;
	case EVoxelSkillCheckBand::Hard:   return HpBright;
	case EVoxelSkillCheckBand::None:   break;
	}
	return InkMute;
}
} // namespace SVoxelDialogueDetail

SVoxelDialogueOverlay::~SVoxelDialogueOverlay()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelDialogueOverlay::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	Data = InArgs._Data;
	OnChoose = InArgs._OnChoose;
	OnClose = InArgs._OnClose;

	// The first SELECTABLE option, not option zero: the mock opens with row 0
	// highlighted, but if that row happens to fail its check the highlight
	// would sit on something the player cannot take.
	Selected = 0;
	for (int32 I = 0; I < Data.Options.Num(); ++I)
	{
		if (Data.Options[I].bAvailable)
		{
			Selected = I;
			break;
		}
	}

	// Whether any option carries a check is what decides the strip, which is
	// the mock's own rule ("render only when at least one skill check option is
	// present").
	bool bAnyCheck = false;
	for (const FVoxelDialogueOption& Option : Data.Options)
	{
		bAnyCheck |= Option.Band != EVoxelSkillCheckBand::None;
	}

	TSharedRef<SOverlay> Stage =
		SNew(SOverlay)
		// The world stays legible: DlgDimAlpha is well below the pause menu's,
		// because a conversation happens IN the scene.
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(Tint(FColor::Black, L.DlgDimAlpha))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.DlgStagePadX, 0.f, 0.f, L.DlgStagePadY))
		[
			BuildSpeaker()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.DlgStagePadY, L.DlgStagePadX, 0.f))
		[
			BuildOptions()
		];

	if (bAnyCheck && Data.Skills.Num() > 0)
	{
		Stage->AddSlot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, L.DlgStagePadX, L.DlgStagePadY))
		[
			BuildSkillStrip()
		];
	}
	if (!Data.CompanionName.IsEmpty())
	{
		Stage->AddSlot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(L.DlgStagePadX, L.DlgStagePadY, 0.f, 0.f))
		[
			BuildCompanion()
		];
	}

	ChildSlot[Stage];
}

TSharedRef<SWidget> SVoxelDialogueOverlay::BuildSpeaker() const
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	FSlateFontInfo NameFont = Style.Serif(L.DlgSpeakerNameSize);
	NameFont.LetterSpacing = L.DlgSpeakerNameSpacing;

	// .dlg-portrait -- the bronze ring stack, empty. See the header.
	TSharedRef<SWidget> Portrait =
		SNew(SBox)
		.WidthOverride(L.DlgPortraitWidth)
		.HeightOverride(L.DlgPortraitHeight)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Bronze))
			]
			+ SOverlay::Slot().Padding(FMargin(3.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep))
			]
			+ SOverlay::Slot().Padding(FMargin(5.f))
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(Tint(Mix(PanelIron, FColor(0x14, 0x10, 0x0a))))
			]
		];

	return SNew(SBox)
		.MaxDesiredWidth(L.DlgSpeakerMaxWidth)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			.Padding(FMargin(0.f, 0.f, L.DlgSpeakerGap, 0.f))
			[
				Portrait
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
					[
						SNew(STextBlock).Text(Data.SpeakerName).Font(NameFont)
						.ColorAndOpacity(FVoxelUIStyle::TitleColour())
						.ShadowOffset(FVector2D(0.f, 2.f))
						.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
					.Padding(FMargin(8.f, 0.f, 0.f, 0.f))
					[
						SNew(STextBlock).Text(Data.SpeakerRole)
						.Font(Style.HandItalic(L.DlgSpeakerRoleSize))
						.ColorAndOpacity(FVoxelUIStyle::DimColour())
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 8.f, 0.f, 0.f))
				[
					SNew(SBox)
					.MaxDesiredWidth(L.DlgSpeakerLineMaxWidth)
					[
						SNew(STextBlock).Text(Data.SpeakerLine)
						.Font(Style.HandItalic(L.DlgSpeakerLineSize))
						.ColorAndOpacity(Tint(InkBright))
						.AutoWrapText(true)
						.LineHeightPercentage(1.35f)
						.ShadowOffset(FVector2D(0.f, 2.f))
						.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
					]
				]
			]
		];
}

TSharedRef<SWidget> SVoxelDialogueOverlay::BuildOptions()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	OptionButtons.Reset();

	for (int32 I = 0; I < Data.Options.Num(); ++I)
	{
		const FVoxelDialogueOption& Option = Data.Options[I];

		TSharedRef<SHorizontalBox> Row =
			SNew(SHorizontalBox)
			.Visibility(EVisibility::HitTestInvisible);

		if (Option.Band != EVoxelSkillCheckBand::None)
		{
			// .dlg-option__badge has `order:-1` -- it is authored after the key
			// in the markup and drawn before everything. Placed first here,
			// which is the same result without a flexbox.
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 10.f, 0.f))
			[
				SNew(STextBlock)
				.Text(SVoxelDialogueDetail::BandLabel(Option.Band))
				.Font(Style.Mono(L.DlgBadgeSize))
				.ColorAndOpacity(Tint(SVoxelDialogueDetail::BandColour(Option.Band),
				                      Option.bAvailable ? 1.f : 0.45f))
			];
		}
		if (!Option.SkillGlyph.IsNone())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 10.f, 0.f))
			[
				VoxelScreenChrome::ItemGlyph(Option.SkillGlyph, 18.f)
			];
		}
		// .dlg-option__key -- the digit on a dark plate.
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 0.f, 10.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(L.DlgOptionKeySize)
			.HeightOverride(L.DlgOptionKeySize)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep))
				]
				+ SOverlay::Slot().Padding(FMargin(1.5f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(FColor(0x14, 0x0e, 0x08), 0.7f))
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Option.Key).Font(Style.Mono(L.DlgOptionKeyFontSize))
					.ColorAndOpacity(Tint(Gold, Option.bAvailable ? 1.f : 0.45f))
				]
			]
		];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			// ONE LINE PER REPLY, NO WRAPPING. Each row is AutoWidth inside a
			// right-aligned column, so an auto-wrapping text block was being
			// handed whatever width the longest sibling happened to claim and
			// broke "Do you know who I am?" onto two lines with "am?" alone.
			// The mock gives every option a single line; the column is sized by
			// its longest reply and the stage is 780 px wide, which fits them.
			SNew(STextBlock)
			.Text(Option.Text)
			.Font(Style.HandItalic(L.DlgOptionSize))
			.ColorAndOpacity(Tint(Option.bAvailable ? InkBright : InkMute))
			.Justification(ETextJustify::Right)
			.ShadowOffset(FVector2D(0.f, 2.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
		];
		// .dlg-option__diamond -- a small rotated gold square. Slate has no
		// rotation on a brush, so it is drawn square, on the same terms as
		// VoxelOverlayChrome::Diamond's substitution for the missing glyph.
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		.Padding(FMargin(10.f, 0.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(L.DlgOptionDiamondSize)
			.HeightOverride(L.DlgOptionDiamondSize)
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(Tint(Gold, Option.bAvailable ? 1.f : 0.3f))
			]
		];

		TSharedPtr<SVoxelMenuButton> Button;
		Column->AddSlot().AutoHeight().HAlign(HAlign_Right)
		.Padding(FMargin(0.f, L.DlgOptionGap * 0.5f))
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				// The Cartouche variant IS the selected-reply chrome: the CSS
				// says so itself ("same chrome family as the title-screen active
				// item"), so the reply stack reuses it rather than restating it.
				SAssignNew(Button, SVoxelMenuButton)
				.Text(FText::GetEmpty())
				.Variant(EVoxelMenuButtonVariant::Cartouche)
				.MinHeight(0.f)
				.IsEnabled(Option.bAvailable)
				.ContentPadding(FMargin(L.DlgOptionPadX, L.DlgOptionPadY))
				.OnClicked_Lambda([this, I]() { Commit(I); return FReply::Handled(); })
			]
			+ SOverlay::Slot().Padding(FMargin(L.DlgOptionPadX, L.DlgOptionPadY))
			[
				Row
			]
		];
		OptionButtons.Add(Button);
	}

	return SNew(SBox).MaxDesiredWidth(L.DlgSpeakerMaxWidth)[Column];
}

TSharedRef<SWidget> SVoxelDialogueOverlay::BuildSkillStrip() const
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	FSlateFontInfo NameFont = Style.Serif(L.DlgSkillNameSize);
	NameFont.LetterSpacing = L.DlgSkillNameSpacing;

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 I = 0; I < Data.Skills.Num(); ++I)
	{
		const FVoxelDialogueSkill& Skill = Data.Skills[I];
		TSharedRef<SVerticalBox> Cell =
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				VoxelScreenChrome::ItemGlyph(Skill.Glyph, L.DlgSkillIconSize)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			.Padding(FMargin(0.f, 4.f, 0.f, 2.f))
			[
				SNew(STextBlock).Text(FText::AsNumber(Skill.Value))
				.Font(Style.Mono(L.DlgSkillValueSize)).ColorAndOpacity(Tint(InkBright))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(STextBlock).Text(Skill.Name).Font(NameFont)
				.ColorAndOpacity(FVoxelUIStyle::DimColour())
			];

		Row->AddSlot().AutoWidth().Padding(FMargin(L.DlgSkillPadX, L.DlgSkillPadY))[Cell];
		if (I < Data.Skills.Num() - 1)
		{
			// .dlg-skill's `border-right`, as a hairline between cells.
			Row->AddSlot().AutoWidth()
			[
				SNew(SBox).WidthOverride(1.f)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep, 0.4f))
				]
			];
		}
	}

	// The strip's own bronze ring stack, same as the portrait's.
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(2.f))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Bronze))
		]
		+ SOverlay::Slot().Padding(FMargin(3.f))
		[
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(Tint(Over(Mix(FColor(0x28, 0x1c, 0x10), FColor(0x14, 0x0e, 0x08)),
			                          0.85f, FColor::Black)))
		]
		+ SOverlay::Slot().Padding(FMargin(3.f))
		[
			Row
		];
}

TSharedRef<SWidget> SVoxelDialogueOverlay::BuildCompanion() const
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	return SNew(SBox)
		.MaxDesiredWidth(L.DlgCompanionWidth)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			.Padding(FMargin(0.f, 0.f, 12.f, 0.f))
			[
				SNew(SBox).WidthOverride(48.f).HeightOverride(60.f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
					]
					+ SOverlay::Slot().Padding(FMargin(2.f))
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(Tint(FColor(0x3a, 0x3a, 0x48)))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(Data.CompanionName)
					.Font(Style.Serif(L.SubTabLabelSize + 2))
					.ColorAndOpacity(FVoxelUIStyle::TitleColour())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 4.f, 0.f, 0.f))
				[
					SNew(STextBlock).Text(Data.CompanionLine)
					.Font(Style.HandItalic(L.CodexBodySize))
					.ColorAndOpacity(FVoxelUIStyle::BodyColour())
					.AutoWrapText(true)
				]
			]
		];
}

void SVoxelDialogueOverlay::MoveSelection(int32 Delta)
{
	if (Data.Options.Num() == 0)
	{
		return;
	}
	// Steps over unavailable rows rather than landing on them: they are drawn
	// so the player can read what they missed, not so the cursor can rest on
	// something that will not answer.
	for (int32 Step = 1; Step <= Data.Options.Num(); ++Step)
	{
		const int32 Candidate =
			(Selected + Delta * Step % Data.Options.Num() + Data.Options.Num() * 2) % Data.Options.Num();
		if (Data.Options[Candidate].bAvailable)
		{
			Selected = Candidate;
			break;
		}
	}
	FocusDefaultWidget();
}

void SVoxelDialogueOverlay::Commit(int32 Index)
{
	if (!Data.Options.IsValidIndex(Index) || !Data.Options[Index].bAvailable)
	{
		return;
	}
	Selected = Index;
	OnChoose.ExecuteIfBound(Index);
}

void SVoxelDialogueOverlay::FocusDefaultWidget()
{
	if (OptionButtons.IsValidIndex(Selected) && OptionButtons[Selected].IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = OptionButtons[Selected]->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

FReply SVoxelDialogueOverlay::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();

	if (Key == EKeys::Escape || Key == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		OnClose.ExecuteIfBound();
		return FReply::Handled();
	}
	if (Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up)
	{
		MoveSelection(-1);
		return FReply::Handled();
	}
	if (Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down)
	{
		MoveSelection(+1);
		return FReply::Handled();
	}
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		Commit(Selected);
		return FReply::Handled();
	}
	// The 1..9 digits, matched against each option's own key label rather than
	// against its index -- the mock assigns them explicitly, and a node whose
	// third reply is keyed "5" has to answer to 5.
	for (int32 I = 0; I < Data.Options.Num(); ++I)
	{
		if (Data.Options[I].Key.ToString() == Key.GetDisplayName(false).ToString())
		{
			Commit(I);
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

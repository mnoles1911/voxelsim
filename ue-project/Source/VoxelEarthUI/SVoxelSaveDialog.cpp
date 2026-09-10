#include "SVoxelSaveDialog.h"

#include "SVoxelMenuButton.h"
#include "SVoxelOverlayChrome.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelSaveDialogDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// A name collides if it matches after trimming, ignoring case. VoxelSave
// slugifies display names, so "Roland Day 12" and "roland day 12" would land in
// the same directory whatever this dialog thinks -- warning on the looser
// comparison is the one that matches what actually happens on disk.
bool Matches(const FString& A, const FString& B)
{
	return A.TrimStartAndEnd().Equals(B.TrimStartAndEnd(), ESearchCase::IgnoreCase);
}
} // namespace SVoxelSaveDialogDetail

SVoxelSaveDialog::~SVoxelSaveDialog()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelSaveDialog::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnConfirm = InArgs._OnConfirm;
	OnCancel = InArgs._OnCancel;
	ExistingNames = InArgs._ExistingNames;
	CurrentName = InArgs._DefaultName;

	// .sv-context: the row plate, a 38 px icon box and the line of what is
	// being saved. The mock's icon is U+2726, which no shipped face carries --
	// VoxelOverlayChrome::Diamond is the standing substitute.
	TSharedRef<SWidget> ContextBand =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(LeatherEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
		[
			// Opaque, composited once against the panel -- see VoxelUITheme::Over.
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(Tint(Over(Mix(OverlayRowTop, OverlayRowBottom), OverlayRowAlpha,
			                                       Mix(OverlayPanelTop, OverlayPanelBottom)))))
		]
		+ SOverlay::Slot().Padding(FMargin(L.LoadRowPadX, L.LoadRowPadY))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(L.SaveContextIconSize).HeightOverride(L.SaveContextIconSize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(LeatherEdge)))
					]
					+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(FSlateColor(Tint(Mix(Leather1, Leather2))))
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						VoxelOverlayChrome::Diamond(16.f, FSlateColor(Tint(WarmPrimary)))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(L.SettingsRowGap, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(InArgs._ContextName)
				.Font([&Style, &L]()
				{
					FSlateFontInfo Font = Style.Serif(L.SaveContextNameSize);
					Font.LetterSpacing = 118; // 2 px at 17 px
					return Font;
				}())
				.ColorAndOpacity(FVoxelUIStyle::TitleColour())
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
		];

	// .sv-input-wrap: a parchment plate, the field, and the counter pinned to
	// its right edge. The counter is `pointer-events:none` in the mock, which in
	// Slate is the overlay slot simply not being hit-testable -- STextBlock is
	// not, so nothing has to say so.
	TSharedRef<SWidget> InputPlate =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(FLinearColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(ParchmentEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
		[
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(Tint(Mix(Parchment, Parchment2))))
		]
		+ SOverlay::Slot().Padding(FMargin(L.SaveInputPadX, L.SaveInputPadY, L.SaveCounterGutter, L.SaveInputPadY))
		.VAlign(VAlign_Center)
		[
			SAssignNew(NameField, SEditableText)
			.Text(CurrentName)
			.Font(Style.HandItalic(L.SaveInputSize))
			.ColorAndOpacity(FSlateColor(Tint(ParchmentInk)))
			.SelectAllTextWhenFocused(true)
			// ESCAPE MUST REACH THIS DIALOG. RevertTextOnEscape would have the
			// field consume the key to undo an edit, and the player would press
			// Escape twice to close a dialog that looked like it ignored them.
			.RevertTextOnEscape(false)
			.ClearKeyboardFocusOnCommit(false)
			.OnTextChanged(this, &SVoxelSaveDialog::HandleTextChanged)
			.OnTextCommitted(this, &SVoxelSaveDialog::HandleTextCommitted)
		]
		+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 12.f, 0.f))
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return VoxelUIStrings::SaveDialogCounter(CurrentName.ToString().Len(),
				                                         FVoxelMenuLayout::Get().SaveNameMaxLength);
			})
			.Font(Style.Mono(L.SaveCounterSize))
			.ColorAndOpacity_Lambda([this]()
			{
				const FVoxelMenuLayout& Layout = FVoxelMenuLayout::Get();
				// .sv-counter.warn -- the readout goes warm before the cap is
				// reached, so a long name is a warning rather than a surprise.
				return FSlateColor(CurrentName.ToString().Len() > Layout.SaveNameWarnLength ? Tint(WarmSecondary)
				                                                                            : Tint(InkMute));
			})
		];

	// .sv-warn, collapsed unless the typed name is already taken.
	TSharedRef<SWidget> WarnBand =
		SNew(SBox)
		.Visibility_Lambda([this]() { return CollidesWithExisting() ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(Tint(WarnWash, WarnWashAlpha)))
			]
			+ SOverlay::Slot().HAlign(HAlign_Left)
			[
				SNew(SBox).WidthOverride(L.SaveWarnBarWidth)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(WarmSecondary)))
				]
			]
			+ SOverlay::Slot().Padding(FMargin(L.SaveWarnBarWidth + 9.f, 8.f, 12.f, 8.f))
			[
				// THREE BLOCKS, NOT ONE FORMATTED STRING: the colliding name is
				// picked out in gold serif inside an italic line, and an FText
				// carries no styling. SWrapBox so a long name wraps the sentence
				// instead of clipping it.
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+ SWrapBox::Slot().Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::SaveOverwritePrefix())
					.Font(Style.HandItalic(L.SaveWarnSize))
					.ColorAndOpacity(FSlateColor(Tint(WarnText)))
				]
				+ SWrapBox::Slot().Padding(FMargin(0.f, 0.f, 4.f, 0.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(TrimmedName()); })
					.Font(Style.Serif(L.SaveWarnSize))
					.ColorAndOpacity(FSlateColor(Tint(WarnName)))
				]
				+ SWrapBox::Slot()
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::SaveOverwriteSuffix())
					.Font(Style.HandItalic(L.SaveWarnSize))
					.ColorAndOpacity(FSlateColor(Tint(WarnText)))
				]
			]
		];

	// THE KEY HINTS GET THEIR OWN LINE, WHICH THE MOCK'S DO NOT (.sv-foot puts
	// them left of the buttons on one row). MEASURED: at the mock's 560 px the
	// row is ENTER + "to confirm" + ESC + "to cancel" + CANCEL + CONFIRM, and in
	// Macondo Swash Caps -- a wide display face, unlike whatever the mock's
	// browser fell back to -- that overruns by about the width of the last hint.
	// The 2026-09-07 save capture showed "to cancel" squeezed to nothing. A
	// second line keeps every word and cannot clip at any name length.
	TSharedRef<SHorizontalBox> KeyHints =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			VoxelOverlayChrome::KeyCap(VoxelUIStrings::KeyEnter())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 14.f, 0.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::SaveKeysConfirm())
			.Font(Style.HandItalic(L.LoadKeyHintSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			VoxelOverlayChrome::KeyCap(VoxelUIStrings::KeyEscape())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::SaveKeysCancel())
			.Font(Style.HandItalic(L.LoadKeyHintSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];

	TSharedRef<SHorizontalBox> Footer =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SSpacer)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::ButtonCancel())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.FontSize(L.SettingsActionSize)
			.LetterSpacing(L.SettingsActionLetterSpacing)
			.ContentPadding(FMargin(L.SettingsActionPadX, L.SettingsActionPadY))
			.MinHeight(0.f)
			.OnClicked_Lambda([this]() { OnCancel.ExecuteIfBound(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::ButtonConfirm())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.Primary(true)
			.FontSize(L.SettingsActionSize)
			.LetterSpacing(L.SettingsActionLetterSpacing)
			.ContentPadding(FMargin(L.SettingsActionPadX, L.SettingsActionPadY))
			.MinHeight(0.f)
			// A blank name has nothing to slugify and would land in a directory
			// named for nothing, so CONFIRM is unavailable until there is one.
			.IsEnabled_Lambda([this]() { return !TrimmedName().IsEmpty(); })
			.OnClicked_Lambda([this]() { Confirm(); return FReply::Handled(); })
		];

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		VoxelOverlayChrome::Panel(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.SaveDialogGap * 0.5f))
			[
				VoxelOverlayChrome::Title(VoxelUIStrings::SaveDialogTitle(), L.SaveDialogTitleSize,
				                          L.SaveDialogTitleLetterSpacing)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.SaveDialogGap))
			[
				VoxelOverlayChrome::Rule()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				ContextBand
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SaveDialogGap, 0.f, 6.f))
			[
				SNew(STextBlock)
				.Text(VoxelUIStrings::SaveDialogPrompt())
				.Font([&Style, &L]()
				{
					FSlateFontInfo Font = Style.Serif(L.SavePromptSize);
					Font.LetterSpacing = L.SavePromptLetterSpacing;
					return Font;
				}())
				.ColorAndOpacity(FSlateColor(Tint(WarmPrimary)))
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				InputPlate
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SaveDialogGap, 0.f, 0.f))
			[
				WarnBand
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SaveDialogGap, 0.f, 0.f))
			[
				KeyHints
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SaveDialogGap * 0.5f))
			[
				VoxelOverlayChrome::Rule()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Footer
			],
			L.SaveDialogWidth, 0.f,
			FMargin(L.SaveDialogPadX, L.SaveDialogPadTop, L.SaveDialogPadX, L.SaveDialogPadBottom))
	];
}

FString SVoxelSaveDialog::TrimmedName() const
{
	return CurrentName.ToString().TrimStartAndEnd();
}

bool SVoxelSaveDialog::CollidesWithExisting() const
{
	const FString Name = TrimmedName();
	if (Name.IsEmpty())
	{
		return false;
	}
	for (const FString& Existing : ExistingNames)
	{
		if (SVoxelSaveDialogDetail::Matches(Existing, Name))
		{
			return true;
		}
	}
	return false;
}

void SVoxelSaveDialog::HandleTextChanged(const FText& NewText)
{
	if (bApplyingClamp)
	{
		return;
	}
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FString Value = NewText.ToString();
	// maxlength="40". Enforced here rather than by refusing characters, because
	// the cap also has to survive a paste, and OnIsTypedCharValid never sees one.
	if (Value.Len() > L.SaveNameMaxLength)
	{
		Value = Value.Left(L.SaveNameMaxLength);
		CurrentName = FText::FromString(Value);
		if (NameField.IsValid())
		{
			TGuardValue<bool> Guard(bApplyingClamp, true);
			NameField->SetText(CurrentName);
		}
		return;
	}
	CurrentName = NewText;
}

void SVoxelSaveDialog::HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	CurrentName = NewText;
	if (CommitType == ETextCommit::OnEnter)
	{
		Confirm();
	}
}

void SVoxelSaveDialog::Confirm()
{
	const FString Name = TrimmedName();
	if (Name.IsEmpty())
	{
		return; // CONFIRM is disabled in this state; Enter has to agree with it
	}
	OnConfirm.ExecuteIfBound(Name);
}

void SVoxelSaveDialog::FocusDefaultWidget()
{
	if (NameField.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(NameField, EFocusCause::SetDirectly);
	}
}

FReply SVoxelSaveDialog::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		OnCancel.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

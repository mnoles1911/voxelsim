#include "SVoxelDeathScreen.h"

#include "SVoxelMenuButton.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

SVoxelDeathScreen::~SVoxelDeathScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelDeathScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnRespawn = InArgs._OnRespawn;
	OnQuit = InArgs._OnQuit;

	FSlateFontInfo TitleFont = Style.Serif(L.DeathTitleSize);
	TitleFont.LetterSpacing = L.DeathTitleSpacing;
	FSlateFontInfo StampFont = Style.Serif(L.DeathStampSize);
	StampFont.LetterSpacing = L.DeathStampSpacing;

	// .rule -- a horizontal gradient from transparent through blood to bright
	// and back. Flattened to the bright centre stop, because a three-box
	// approximation of a symmetric fade reads worse than one clean rule.
	TSharedRef<SWidget> Rule =
		SNew(SBox)
		.WidthOverride(L.DeathRuleWidth)
		.HeightOverride(L.DeathRuleHeight)
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(HpBright))
		];

	TSharedRef<SVerticalBox> Stack =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::DeathTitle())
			.Font(TitleFont)
			// The heading is HpBright over a DeathBlood shadow: the mock's
			// `text-shadow: 0 0 40px rgba(142,31,20,.75)` is a glow Slate cannot
			// draw, and an offset shadow in the same colour is the nearest
			// thing that still separates the letters from the black behind.
			.ColorAndOpacity(Tint(HpBright))
			.ShadowOffset(FVector2D(4.f, 4.f))
			.ShadowColorAndOpacity(Tint(DeathBlood, 0.85f))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		.Padding(FMargin(0.f, L.DeathRuleTopGap, 0.f, L.DeathRuleBottomGap))
		[
			Rule
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(SBox)
			.MaxDesiredWidth(L.DeathQuipMaxWidth)
			[
				SNew(STextBlock)
				.Text(InArgs._Data.Quip)
				.Font(Style.HandItalic(L.DeathQuipSize))
				.ColorAndOpacity(Tint(Parchment))
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
				.ShadowOffset(FVector2D(0.f, 2.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
		];

	// .stamp -- omitted rather than blank when the session cannot name a day,
	// which is SVoxelPauseMenu's rule for its own footer.
	if (!InArgs._Data.Stamp.IsEmpty())
	{
		Stack->AddSlot().AutoHeight().HAlign(HAlign_Center)
		.Padding(FMargin(0.f, L.DeathStampTopGap, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(InArgs._Data.Stamp)
			.Font(StampFont)
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];
	}

	TSharedRef<SHorizontalBox> Actions =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.f, 0.f, L.DeathButtonGap, 0.f))
		[
			SAssignNew(RespawnButton, SVoxelMenuButton)
			.Text(VoxelUIStrings::DeathRespawn())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.Primary(true)
			.FontSize(L.DeathButtonSize)
			.LetterSpacing(L.DeathButtonSpacing)
			.MinHeight(0.f)
			.ContentPadding(FMargin(L.DeathButtonPadX, L.DeathButtonPadY))
			.OnClicked_Lambda([this]() { OnRespawn.ExecuteIfBound(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::DeathQuit())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.FontSize(L.DeathButtonSize)
			.LetterSpacing(L.DeathButtonSpacing)
			.MinHeight(0.f)
			.ContentPadding(FMargin(L.DeathButtonPadX, L.DeathButtonPadY))
			.OnClicked_Lambda([this]() { OnQuit.ExecuteIfBound(); return FReply::Handled(); })
		];

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(Tint(FColor::Black, L.DeathDimAlpha))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Stack]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			.Padding(FMargin(0.f, L.DeathActionsTopGap, 0.f, 0.f))
			[
				Actions
			]
		]
		// .version-stamp -- the mock puts one on this screen at
		// `left:48px; bottom:32px` and the port drew nothing there. Same string
		// and same treatment as the title screen's and the loading screen's, so
		// the three cannot say different things about the same build; the
		// insets are the shared VersionInset* pair for the same reason.
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
}

void SVoxelDeathScreen::FocusDefaultWidget()
{
	if (RespawnButton.IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = RespawnButton->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

FReply SVoxelDeathScreen::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();
	// Gamepad_FaceButton_Bottom rather than EKeys::Virtual_Accept: the virtual
	// keys resolve per platform and the concrete face button is what a pad
	// actually sends here. Escape's pairing below matches SVoxelPauseMenu's
	// exactly, so the two overlays back out the same way.
	if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		OnRespawn.ExecuteIfBound();
		return FReply::Handled();
	}
	if (Key == EKeys::Escape || Key == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		OnQuit.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

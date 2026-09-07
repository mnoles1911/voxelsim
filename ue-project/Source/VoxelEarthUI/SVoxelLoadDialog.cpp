#include "SVoxelLoadDialog.h"

#include "SVoxelMenuButton.h"
#include "SVoxelOverlayChrome.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelLoadDialogDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// THE ROW SIGIL (.ld-thumb). The mock draws U+2726 / U+25B2 / U+2020 / U+25C7 /
// U+26EF, of which a cmap dump of the four shipped faces on 2026-09-07 found
// exactly ONE (the dagger, and only in IM Fell, not in the serif this row uses).
// So the ring is four characters MEASURED PRESENT in Macondo Swash Caps, picked
// by a hash of the slug -- which makes a save's sigil stable across sessions
// without storing anything, and makes two saves look different without meaning
// anything by it.
const TCHAR* const kSigils[] = {TEXT("§"), TEXT("¤"), TEXT("•"), TEXT("*")};

// The four .ld-thumb palettes, as {top, bottom, ink}. An autosave always takes
// the first (the mock's .cave, which is what its autosave rows use); a named
// save takes one of the other three by the same hash as its sigil.
struct FThumbPalette
{
	FColor Top;
	FColor Bottom;
	FColor Ink;
};

FThumbPalette PaletteFor(const FString& Slug, bool bIsAutosave)
{
	using namespace VoxelUITheme;
	const FThumbPalette Cave{ThumbCaveTop, ThumbCaveBottom, ThumbCaveInk};
	if (bIsAutosave)
	{
		return Cave;
	}
	const FThumbPalette Named[] = {
		{ThumbHolyTop, ThumbHolyBottom, Gold},
		{ThumbFieldTop, ThumbFieldBottom, ThumbFieldInk},
		{ThumbDeepTop, ThumbDeepBottom, ThumbDeepInk},
	};
	return Named[GetTypeHash(Slug) % UE_ARRAY_COUNT(Named)];
}

FText SigilFor(const FString& Slug)
{
	return FText::FromString(kSigils[(GetTypeHash(Slug) / 3u) % UE_ARRAY_COUNT(kSigils)]);
}
} // namespace SVoxelLoadDialogDetail

SVoxelLoadDialog::~SVoxelLoadDialog()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelLoadDialog::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnLoadSave = InArgs._OnLoadSave;
	OnDeleteSave = InArgs._OnDeleteSave;
	OnCancel = InArgs._OnCancel;
	Rows = InArgs._Rows;

	// .ld-header: the chips, the search field and the count, on one line.
	TSharedRef<SHorizontalBox> Header =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			BuildFilterChip(VoxelUIStrings::LoadFilterAll(), EVoxelSaveFilter::All)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			BuildFilterChip(VoxelUIStrings::LoadFilterManual(), EVoxelSaveFilter::Manual)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
		[
			BuildFilterChip(VoxelUIStrings::LoadFilterAuto(), EVoxelSaveFilter::Auto)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(10.f, 0.f))
		[
			// .ld-search -- the same parchment plate as the save dialog's field,
			// at a smaller size.
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(FLinearColor::Black))
			]
			+ SOverlay::Slot().Padding(FMargin(1.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(ParchmentEdge)))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(Tint(Mix(Parchment, Parchment2))))
			]
			+ SOverlay::Slot().Padding(FMargin(12.f, 7.f)).VAlign(VAlign_Center)
			[
				SNew(SEditableText)
				.HintText(VoxelUIStrings::LoadSearchHint())
				.Font(Style.HandItalic(L.LoadSearchSize))
				.ColorAndOpacity(FSlateColor(Tint(ParchmentInk)))
				.RevertTextOnEscape(false)
				.ClearKeyboardFocusOnCommit(false)
				.OnTextChanged_Lambda([this](const FText& NewText)
				{
					SearchQuery = NewText.ToString().TrimStartAndEnd().ToLower();
					Rebuild();
				})
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			// .ld-count: `<b>visible</b> / total`, the first half gold serif and
			// the second italic -- two blocks, because one FText has one style.
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return FText::AsNumber(VisibleCount); })
				.Font(Style.Serif(L.LoadCountSize))
				.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.f, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return VoxelUIStrings::LoadCountTotal(Rows.Num()); })
				.Font(Style.HandItalic(L.LoadCountSize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			]
		];

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		VoxelOverlayChrome::Panel(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.LoadDialogGap * 0.5f))
			[
				VoxelOverlayChrome::Title(VoxelUIStrings::LoadPanelTitle(), L.LoadDialogTitleSize,
				                          L.LoadDialogTitleLetterSpacing)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.LoadDialogGap))
			[
				VoxelOverlayChrome::Rule()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Header
			]
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, L.LoadDialogGap))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(ListBox, SVerticalBox)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.LoadDialogGap))
			[
				VoxelOverlayChrome::Rule()
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SAssignNew(CancelButton, SVoxelMenuButton)
				.Text(VoxelUIStrings::ButtonCancel())
				.Variant(EVoxelMenuButtonVariant::Leather)
				.FontSize(L.SettingsActionSize)
				.LetterSpacing(L.SettingsActionLetterSpacing)
				.ContentPadding(FMargin(L.SettingsActionPadX, L.SettingsActionPadY))
				.MinHeight(0.f)
				.OnClicked_Lambda([this]() { OnCancel.ExecuteIfBound(); return FReply::Handled(); })
			],
			L.LoadDialogWidth, L.LoadDialogHeight,
			FMargin(L.LoadDialogPadX, L.LoadDialogPadTop, L.LoadDialogPadX, L.LoadDialogPadBottom))
	];

	Rebuild();
}

TSharedRef<SWidget> SVoxelLoadDialog::BuildFilterChip(const FText& Label, EVoxelSaveFilter Filter)
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	return SNew(SVoxelMenuButton)
		.Text(Label)
		.Variant(EVoxelMenuButtonVariant::Leather)
		// AN ATTRIBUTE, so exactly one chip is warm and no chip has to be told
		// when another is pressed. The state is ActiveFilter and nothing else.
		.Active(TAttribute<bool>::CreateLambda([this, Filter]() { return ActiveFilter == Filter; }))
		.FontSize(L.LoadFilterSize)
		.LetterSpacing(L.LoadFilterLetterSpacing)
		.ContentPadding(FMargin(L.LoadFilterPadX, L.LoadFilterPadY))
		.MinHeight(0.f)
		.OnClicked_Lambda([this, Filter]()
		{
			ActiveFilter = Filter;
			Rebuild();
			return FReply::Handled();
		});
}

TSharedRef<SWidget> SVoxelLoadDialog::BuildTag(const FText& Label, bool bWarm)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FSlateFontInfo Font = Style.Serif(L.LoadTagSize);
	Font.LetterSpacing = L.LoadTagLetterSpacing;

	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// .ld-tag.latest sets border-color:#000; the plain tag keeps the
			// --leather-edge rule every other plate has.
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(bWarm ? FLinearColor::Black : Tint(LeatherEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(1.f))
		[
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(bWarm ? Tint(Mix(WarmPrimary, WarmSecondary)) : Tint(Leather2)))
		]
		+ SOverlay::Slot().Padding(FMargin(6.f, 2.f))
		[
			SNew(STextBlock)
			.Text(Label)
			.Font(Font)
			.ColorAndOpacity(FSlateColor(bWarm ? Tint(OnWarm) : Tint(Gold)))
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
		];
}

TSharedRef<SWidget> SVoxelLoadDialog::BuildRow(const FVoxelSaveRowInfo& Row, bool bIsLatest)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const FString Slug = Row.Slug;
	const SVoxelLoadDialogDetail::FThumbPalette Palette =
		SVoxelLoadDialogDetail::PaletteFor(Slug, Row.bIsAutosave);

	TSharedRef<SHorizontalBox> NameRow =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).MaxDesiredWidth(L.LoadRowNameMaxWidth)
			[
				SNew(STextBlock)
				// An autosave has no name the player chose, so the mock labels
				// every one "Autosave" and dims it; the timestamp underneath is
				// what tells two of them apart.
				.Text(Row.bIsAutosave ? VoxelUIStrings::LoadAutosaveName() : Row.DisplayName)
				.Font([&Style, &L]()
				{
					FSlateFontInfo Font = Style.Serif(L.LoadRowNameSize);
					Font.LetterSpacing = 56; // 1 px at 18 px
					return Font;
				}())
				.ColorAndOpacity(FSlateColor(Row.bIsAutosave ? Tint(Parchment, 0.65f) : Tint(Gold)))
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];

	if (bIsLatest)
	{
		NameRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.f, 0.f, 0.f, 0.f))
		[
			BuildTag(VoxelUIStrings::LoadTagLatest(), /*bWarm=*/true)
		];
	}
	if (Row.bIsAutosave)
	{
		NameRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.f, 0.f, 0.f, 0.f))
		[
			BuildTag(VoxelUIStrings::LoadTagAuto(), /*bWarm=*/false)
		];
	}

	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(LeatherEdge)))
		]
		+ SOverlay::Slot().Padding(FMargin(1.f))
		[
			// Opaque, not `rgba(...,.55)`: see VoxelUITheme::Over. The 0.55 is
			// still there, applied once against the panel it sits on.
			SNew(SImage).Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(Tint(Over(Mix(OverlayRowTop, OverlayRowBottom), OverlayRowAlpha,
			                                       Mix(OverlayPanelTop, OverlayPanelBottom)))))
		]
		+ SOverlay::Slot().Padding(FMargin(L.LoadRowPadX, L.LoadRowPadY))
		[
			SNew(SHorizontalBox)
			// .ld-thumb: the sigil plate.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(L.LoadThumbSize).HeightOverride(L.LoadThumbSize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(FLinearColor::Black))
					]
					+ SOverlay::Slot().Padding(FMargin(1.f))
					[
						SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(LeatherEdge)))
					]
					+ SOverlay::Slot().Padding(FMargin(2.f))
					[
						SNew(SImage).Image(Style.SolidWhite())
						.ColorAndOpacity(FSlateColor(Tint(Mix(Palette.Top, Palette.Bottom))))
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(SVoxelLoadDialogDetail::SigilFor(Slug))
						.Font(Style.Serif(L.LoadThumbGlyphSize))
						.ColorAndOpacity(FSlateColor(Tint(Palette.Ink)))
						.ShadowOffset(FVector2D(1.f, 1.f))
						.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(L.SettingsRowGap, 0.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					NameRow
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 3.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					// An unloadable row shows WHY in place of its coordinates.
					// The alternative -- showing the position of a world this
					// build cannot open -- is an invitation to click it.
					.Text(Row.bLoadable ? Row.Detail : Row.DisabledReason)
					.Font(Style.HandItalic(L.LoadRowMetaSize))
					.ColorAndOpacity(FSlateColor(Row.bLoadable ? Tint(Parchment, 0.60f) : Tint(InkMute)))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVoxelMenuButton)
				.Text(VoxelUIStrings::ButtonLoad())
				.Variant(EVoxelMenuButtonVariant::Leather)
				// The mock gives the primary plate to the row it opens with
				// selected, which is the newest one.
				.Primary(bIsLatest)
				.FontSize(L.LoadRowButtonSize)
				.LetterSpacing(L.LoadTagLetterSpacing)
				.ContentPadding(FMargin(L.LoadRowButtonPadX, L.LoadRowButtonPadY))
				.MinHeight(0.f)
				.IsEnabled(Row.bLoadable)
				.OnClicked_Lambda([this, Slug]() { OnLoadSave.ExecuteIfBound(Slug); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
			[
				SNew(SVoxelMenuButton)
				.Text(VoxelUIStrings::ButtonDelete())
				.Variant(EVoxelMenuButtonVariant::Leather)
				.Danger(true)
				.FontSize(L.LoadRowButtonSize)
				.LetterSpacing(L.LoadTagLetterSpacing)
				.ContentPadding(FMargin(L.LoadRowButtonPadX, L.LoadRowButtonPadY))
				.MinHeight(0.f)
				// DELETE is enabled even on an unloadable save: not being able
				// to OPEN a world is no reason to be stuck with it.
				.OnClicked_Lambda([this, Slug]() { OnDeleteSave.ExecuteIfBound(Slug); return FReply::Handled(); })
			]
		];
}

bool SVoxelLoadDialog::PassesFilter(const FVoxelSaveRowInfo& Row) const
{
	if (ActiveFilter == EVoxelSaveFilter::Manual && Row.bIsAutosave)
	{
		return false;
	}
	if (ActiveFilter == EVoxelSaveFilter::Auto && !Row.bIsAutosave)
	{
		return false;
	}
	if (SearchQuery.IsEmpty())
	{
		return true;
	}
	// The mock searches a per-row `data-name`, which is the name plus its
	// timestamp; searching the detail line too is what makes a query like "11/04"
	// find an autosave, whose name is the same word for every one of them.
	return Row.DisplayName.ToString().ToLower().Contains(SearchQuery)
	       || Row.Detail.ToString().ToLower().Contains(SearchQuery);
}

void SVoxelLoadDialog::Rebuild()
{
	using namespace VoxelUITheme;
	if (!ListBox.IsValid())
	{
		return;
	}
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	ListBox->ClearChildren();
	VisibleCount = 0;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FVoxelSaveRowInfo& Row = Rows[Index];
		if (!PassesFilter(Row))
		{
			continue;
		}
		++VisibleCount;
		// LATEST is the newest save, not the first row that survived the filter.
		// VoxelSave::List() is newest-first, so that is index 0 of the WHOLE
		// list -- a MANUAL filter that hid the newest autosave must not promote
		// the row below it to "latest".
		ListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, L.LoadRowGap * 0.5f))
		[
			BuildRow(Row, /*bIsLatest=*/Index == 0)
		];
	}

	if (VisibleCount == 0)
	{
		// .ld-empty. Two wordings, because they are two different situations:
		// a fresh install has no saves at all and should be told how to make
		// one, while a query that matched nothing should be told just that.
		const bool bNoSavesAtAll = Rows.Num() == 0;
		ListBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 40.f, 0.f, 0.f)).HAlign(HAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				// The mock's U+2205 is in none of the shipped faces; the
				// standing diamond ornament stands in, at the glyph's size.
				VoxelOverlayChrome::Diamond(L.LoadEmptyGlyphSize * 0.6f, FSlateColor(Tint(LeatherEdge)))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 16.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(bNoSavesAtAll ? VoxelUIStrings::LoadPanelEmpty() : VoxelUIStrings::LoadNoMatch())
				.Font(Style.HandItalic(L.LoadEmptySize))
				.ColorAndOpacity(FSlateColor(Tint(Parchment, 0.50f)))
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
			]
		];
	}
}

void SVoxelLoadDialog::SetRows(TArray<FVoxelSaveRowInfo> InRows)
{
	Rows = MoveTemp(InRows);
	Rebuild();
}

void SVoxelLoadDialog::FocusDefaultWidget()
{
	// CANCEL rather than the first row: it is the control that is always there,
	// including in the empty state, and arrowing up from it reaches the rows.
	if (CancelButton.IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = CancelButton->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

FReply SVoxelLoadDialog::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		OnCancel.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

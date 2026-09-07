#include "SVoxelSettingsPanel.h"

#include "SVoxelMenuButton.h"
#include "SVoxelOverlayChrome.h"
#include "VoxelAudioUserSettings.h"
#include "VoxelEarthUI.h"
#include "VoxelGraphicsUserSettings.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Engine/Engine.h" // GEngine, guarding UGameUserSettings::GetGameUserSettings
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelSettingsPanelDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// FULLSCREEN reads and writes UGameUserSettings, which is the store this
// project already uses for window mode (AVoxelEarthGameMode's forced-resolution
// path drives the same object).
//
// WINDOWED-FULLSCREEN, NOT EXCLUSIVE. `EWindowMode::Fullscreen` takes exclusive
// control of the display and is the mode that makes alt-tabbing out of a
// crashed build a fight; the game mode's own force-res path already chose
// WindowedFullscreen for the same reason, and having two places in this
// codebase mean different things by "fullscreen" is worse than either choice.
bool IsFullscreenNow()
{
	const UGameUserSettings* Settings = GEngine ? UGameUserSettings::GetGameUserSettings() : nullptr;
	return Settings != nullptr && Settings->GetFullscreenMode() != EWindowMode::Windowed;
}

void SetFullscreenNow(bool bFullscreen)
{
	UGameUserSettings* Settings = GEngine ? UGameUserSettings::GetGameUserSettings() : nullptr;
	if (Settings == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("Settings: no UGameUserSettings; FULLSCREEN not applied."));
		return;
	}
	Settings->SetFullscreenMode(bFullscreen ? EWindowMode::WindowedFullscreen : EWindowMode::Windowed);
	// false: do not check for a command-line override. The player just asked.
	Settings->ApplyResolutionSettings(/*bCheckForCommandLineOverrides=*/false);
	Settings->SaveSettings();
	UE_LOG(LogVoxelUI, Log, TEXT("Settings: FULLSCREEN -> %s."),
	       bFullscreen ? TEXT("windowed-fullscreen") : TEXT("windowed"));
}

// The four GRAPHICS rows, as data. Same table shape and the same
// plain-function-pointer reasoning as SVoxelMainMenu::BuildSettingsPanel's,
// which this replaces: one place where a label, a toggle and a description are
// wired together, so a row can only be wrong in its data.
struct FGraphicsRow
{
	FText (*Label)();
	FText (*Desc)();
	bool (*Get)();
	void (*Set)(bool);
};

const FGraphicsRow kGraphicsRows[] = {
	{&VoxelUIStrings::SettingsFineDetailLabel, &VoxelUIStrings::SettingsFineDetailDesc,
	 &VoxelGraphicsUserSettings::GetFineDetailSmoothing, &VoxelGraphicsUserSettings::SetFineDetailSmoothing},
	{&VoxelUIStrings::SettingsFasterTerrainLabel, &VoxelUIStrings::SettingsFasterTerrainDesc,
	 &VoxelGraphicsUserSettings::GetFasterTerrainDrawing, &VoxelGraphicsUserSettings::SetFasterTerrainDrawing},
	{&VoxelUIStrings::SettingsWaterWaveLabel, &VoxelUIStrings::SettingsWaterWaveDesc,
	 &VoxelGraphicsUserSettings::GetWaterWaveDetail, &VoxelGraphicsUserSettings::SetWaterWaveDetail},
	{&VoxelUIStrings::SettingsOceanDetailLabel, &VoxelUIStrings::SettingsOceanDetailDesc,
	 &VoxelGraphicsUserSettings::GetOceanMeshDetail, &VoxelGraphicsUserSettings::SetOceanMeshDetail},
};

// The keyboard step. 5% per arrow press puts the full range 20 presses away,
// which is coarse enough to cross quickly and fine enough to land on a value.
constexpr float kSliderStep = 0.05f;
} // namespace SVoxelSettingsPanelDetail

SVoxelSettingsPanel::~SVoxelSettingsPanel()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelSettingsPanel::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	OnLeave = InArgs._OnLeave;
	bPendingFullscreen = SVoxelSettingsPanelDetail::IsFullscreenNow();

	const FMargin RowPad(0.f, L.SettingsPanelGap * 0.5f);

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		VoxelOverlayChrome::SectionHeader(VoxelUIStrings::SettingsSectionAudio())
	];
	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		BuildSliderRow(VoxelUIStrings::SettingsMasterLabel(), &VoxelAudioUserSettings::GetMasterVolume,
		               &VoxelAudioUserSettings::SetMasterVolume)
	];
	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		BuildSliderRow(VoxelUIStrings::SettingsMusicLabel(), &VoxelAudioUserSettings::GetMusicVolume,
		               &VoxelAudioUserSettings::SetMusicVolume)
	];

	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		VoxelOverlayChrome::SectionHeader(VoxelUIStrings::SettingsSectionDisplay())
	];
	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		BuildCheckRow(VoxelUIStrings::SettingsFullscreenLabel(), VoxelUIStrings::SettingsFullscreenHint(),
		              TAttribute<bool>::CreateLambda([this]() { return bPendingFullscreen; }),
		              [this](bool bChecked)
		              {
			              // PENDING ONLY. The swap chain is rebuilt by APPLY --
			              // see the header for why this row alone defers.
			              bPendingFullscreen = bChecked;
		              })
	];

	Body->AddSlot().AutoHeight().Padding(RowPad)
	[
		VoxelOverlayChrome::SectionHeader(VoxelUIStrings::SettingsSectionGraphics())
	];
	for (const SVoxelSettingsPanelDetail::FGraphicsRow& Row : SVoxelSettingsPanelDetail::kGraphicsRows)
	{
		Body->AddSlot().AutoHeight().Padding(RowPad)
		[
			BuildCheckRow(Row.Label(), Row.Desc(),
			              TAttribute<bool>::CreateLambda([Get = Row.Get]() { return Get(); }),
			              [Set = Row.Set](bool bChecked)
			              {
				              // Straight through to the persisted setting, which
				              // applies its cvar itself. Nothing is cached here,
				              // so a write that failed VISIBLY fails to flip.
				              Set(bChecked);
			              })
		];
	}

	// The footer: an ESC hint on the left, then APPLY and the primary
	// SAVE & LEAVE. `justify-content:flex-end` with the hint at `margin-right:auto`.
	TSharedRef<SHorizontalBox> Footer =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			VoxelOverlayChrome::KeyCap(VoxelUIStrings::KeyEscape())
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::SettingsBackHint())
			.Font(Style.HandItalic(L.SettingsHintSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
		[
			SAssignNew(ApplyButton, SVoxelMenuButton)
			.Text(VoxelUIStrings::ButtonApply())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.FontSize(L.SettingsActionSize)
			.LetterSpacing(L.SettingsActionLetterSpacing)
			.ContentPadding(FMargin(L.SettingsActionPadX, L.SettingsActionPadY))
			.MinHeight(0.f)
			.OnClicked_Lambda([this]() { ApplyPending(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
		[
			SNew(SVoxelMenuButton)
			.Text(VoxelUIStrings::ButtonSaveAndLeave())
			.Variant(EVoxelMenuButtonVariant::Leather)
			.Primary(true)
			.FontSize(L.SettingsActionSize)
			.LetterSpacing(L.SettingsActionLetterSpacing)
			.ContentPadding(FMargin(L.SettingsActionPadX, L.SettingsActionPadY))
			.MinHeight(0.f)
			.OnClicked_Lambda([this]()
			{
				ApplyPending();
				OnLeave.ExecuteIfBound();
				return FReply::Handled();
			})
		];

	ChildSlot
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		VoxelOverlayChrome::Panel(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.SettingsPanelGap))
			[
				VoxelOverlayChrome::Title(VoxelUIStrings::SettingsPanelTitle(), L.SettingsTitleSize,
				                          L.SettingsTitleLetterSpacing)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				VoxelOverlayChrome::Rule()
			]
			// SCROLLED, for the reason BuildMessagePanel's body is: this panel
			// carries the mock's two sections AND the four graphics rows that
			// already shipped, and it is the rows that will keep arriving.
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, L.SettingsPanelGap))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					Body
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				VoxelOverlayChrome::Rule()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, L.SettingsPanelGap, 0.f, 0.f))
			[
				Footer
			],
			L.SettingsPanelWidth, L.SettingsPanelHeight,
			FMargin(L.SettingsPanelPadX, L.SettingsPanelPadTop, L.SettingsPanelPadX, L.SettingsPanelPadBottom))
	];
}

TSharedRef<SWidget> SVoxelSettingsPanel::BuildSliderRow(const FText& Label, float (*Get)(), void (*Set)(float))
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// THE FILL IS TWO FILLWIDTH SLOTS, NOT A SIZED BOX. FillWidth takes an
	// attribute, so binding the warm half to the value and the dark half to
	// 1 - value gives a proportional bar that re-lays-out itself, with no
	// per-frame width arithmetic and no custom paint.
	TSharedRef<SWidget> Track =
		SNew(SBox)
		.HeightOverride(L.SliderTrackHeight)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(FLinearColor::Black))
			]
			+ SOverlay::Slot().Padding(FMargin(1.f))
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(Tint(LeatherEdge)))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(FSlateColor(Tint(Mix(WellTop, WellBottom))))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Get]() { return Get(); }))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(FSlateColor(Tint(Mix(WarmPrimary, WarmSecondary))))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Get]() { return 1.f - Get(); }))
				[
					SNew(SSpacer)
				]
			]
			+ SOverlay::Slot()
			[
				SNew(SSlider)
				.Style(&Style.Slider())
				.Value(TAttribute<float>::CreateLambda([Get]() { return Get(); }))
				.StepSize(SVoxelSettingsPanelDetail::kSliderStep)
				.IsFocusable(true)
				// SSlider defaults to requiring a gamepad "lock" press before
				// the stick moves the value. On a settings panel a player has
				// already committed by navigating onto the row, and the extra
				// press reads as the slider being broken.
				.RequiresControllerLock(false)
				// The setter persists AND applies, so a drag is heard as it
				// happens -- which is the only way a volume slider can be
				// judged, and the reason this row does not wait for APPLY.
				.OnValueChanged_Lambda([Set](float NewValue) { Set(NewValue); })
			]
		];

	TSharedRef<SHorizontalBox> Row =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(L.SettingsLabelColumn)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(Style.Serif(L.SettingsRowLabelSize))
				.ColorAndOpacity(FSlateColor(Tint(Parchment)))
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		.Padding(FMargin(L.SettingsRowGap, 0.f))
		[
			Track
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(L.SliderValueWidth)
			[
				SNew(STextBlock)
				// .sl-val is Courier at 14 px; VT323 is this port's --mono and
				// the readout is the same kind of thing -- a fixed-width number
				// that must not jitter as it counts.
				.Text(TAttribute<FText>::CreateLambda([Get]()
				{
					return FText::AsNumber(FMath::RoundToInt(Get() * 100.f));
				}))
				.Font(Style.Mono(L.SliderValueSize))
				.ColorAndOpacity(FVoxelUIStyle::TitleColour())
				.Justification(ETextJustify::Right)
			]
		];

	return Row;
}

TSharedRef<SWidget> SVoxelSettingsPanel::BuildCheckRow(const FText& Label, const FText& Hint,
                                                       TAttribute<bool> Checked, TFunction<void(bool)> OnToggled)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// .ck-box plus .ck-box::after. The mark's scale(0)->scale(1) transition has
	// no Slate equivalent that is worth a ticking widget, so it is drawn or it
	// is not; the box, the label and the hint all sit inside the SCheckBox so
	// the whole row is the click target the mock's `cursor:pointer` implies.
	//
	// THE HINT IS ON ITS OWN LINE, WHICH THE MOCK'S IS NOT (.ck-hint sits
	// beside the label). The mock only ever had FULLSCREEN to hint at and gave
	// it nothing; the four GRAPHICS rows this panel keeps carry a sentence each,
	// written for players, and inline they overran the 600 px panel and were
	// clipped at its border -- measured, in the 2026-09-07 settings capture.
	// Under the label they wrap into the full panel width and the rows still
	// read as rows.
	return SNew(SCheckBox)
		.Style(&Style.OverlayCheckBox())
		.IsChecked_Lambda([Checked]() { return Checked.Get(false) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([OnToggled = MoveTemp(OnToggled)](ECheckBoxState NewState)
		{
			OnToggled(NewState == ECheckBoxState::Checked);
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(L.CheckboxSize).HeightOverride(L.CheckboxSize)
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
							.ColorAndOpacity(FSlateColor(Tint(Mix(WellTop, WellBottom))))
						]
						+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
						[
							SNew(SBox).WidthOverride(L.CheckboxMarkSize).HeightOverride(L.CheckboxMarkSize)
							[
								SNew(SImage).Image(Style.SolidWhite())
								.ColorAndOpacity_Lambda([Checked]()
								{
									return FSlateColor(Checked.Get(false) ? Tint(Mix(Gold, WarmPrimary))
									                                      : FLinearColor::Transparent);
								})
							]
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(L.SettingsRowGap, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(Style.Serif(L.SettingsRowLabelSize))
					.ColorAndOpacity(FSlateColor(Tint(Parchment)))
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			// Indented past the well so the hint reads as belonging to the row
			// above it rather than to the section.
			.Padding(FMargin(L.CheckboxSize + L.SettingsRowGap, 2.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(Hint)
				.Font(Style.HandItalic(L.SettingsHintSize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
				.WrapTextAt(L.SettingsHintWrapWidth)
			]
		];
}

void SVoxelSettingsPanel::ApplyPending()
{
	if (bPendingFullscreen == SVoxelSettingsPanelDetail::IsFullscreenNow())
	{
		return; // the ordinary case: nothing deferred is outstanding
	}
	SVoxelSettingsPanelDetail::SetFullscreenNow(bPendingFullscreen);
}

void SVoxelSettingsPanel::FocusDefaultWidget()
{
	// The APPLY button rather than the first slider: it is always present, it
	// is where Escape's alternative lives, and arrowing UP from the footer
	// reaches the rows. Focusing a slider first would make the very first
	// left-arrow press change a volume.
	if (ApplyButton.IsValid())
	{
		if (const TSharedPtr<SWidget> FocusWidget = ApplyButton->GetFocusWidget())
		{
			FSlateApplication::Get().SetKeyboardFocus(FocusWidget, EFocusCause::SetDirectly);
		}
	}
}

FReply SVoxelSettingsPanel::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape || KeyEvent.GetKey() == EKeys::Virtual_Gamepad_Back.GetVirtualKey())
	{
		// The mock's `<kbd>ESC</kbd> back`. Escape LEAVES WITHOUT APPLYING the
		// pending window mode -- which is what "back" has to mean next to a
		// button labelled APPLY, or the two controls would be the same control.
		OnLeave.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(Geometry, KeyEvent);
}

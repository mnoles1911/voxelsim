#include "SVoxelLoadingScreen.h"

#include "SVoxelCoverImage.h"
#include "SVoxelHourglass.h"
#include "VoxelEarthUI.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelLoadingDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

constexpr int32 kFrameSamples = 60;
// The GDScript's threshold for tinting the worst-frame figure red. It is the
// same 33.3 ms VoxelDebug::kHitchThresholdMs uses, so the loading screen and
// the perf HUD agree about what counts as a bad frame.
constexpr float kWorstFrameBadMs = 33.f;

// Builds a shuffled index list over Count entries.
TArray<int32> ShuffledIndices(int32 Count, FRandomStream& Stream)
{
	TArray<int32> Order;
	Order.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Order.Add(Index);
	}
	for (int32 i = Order.Num() - 1; i > 0; --i)
	{
		Order.Swap(i, Stream.RandRange(0, i));
	}
	return Order;
}
} // namespace SVoxelLoadingDetail

void SVoxelLoadingScreen::Construct(const FArguments& InArgs)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	FrameTimesMs.SetNumZeroed(SVoxelLoadingDetail::kFrameSamples);

	const float HalfSep = L.LoadingSeparation * 0.5f;

	// The centred column. Godot builds it as a fixed 600x360 VBox at
	// PRESET_CENTER; the SBox reproduces the fixed size, and the per-slot
	// half-separation padding reproduces the 18px gap.
	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox)
		// The hourglass lives inside a fixed wrapper so the bob can move it
		// without the layout fighting back -- the GDScript wraps it in a
		// non-Container Control for exactly that reason (a Container parent
		// resets child positions every layout pass).
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, HalfSep))
		[
			SNew(SBox)
			.WidthOverride(L.HourglassWrapWidth)
			.HeightOverride(L.HourglassWrapHeight)
			.VAlign(VAlign_Top)
			.Padding(this, &SVoxelLoadingScreen::GetHourglassBobPadding)
			[
				SNew(SBox)
				.WidthOverride(L.HourglassWidth)
				.HeightOverride(L.HourglassHeight)
				[
					SAssignNew(Hourglass, SVoxelHourglass)
					.Progress_Lambda([this]() { return Progress; })
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, HalfSep))
		[
			SNew(STextBlock)
			// "L O A D I N G" -- the spaces are literal. Godot Labels have no
			// letter-spacing property and neither does Slate, so the source
			// fakes the tracking with thin spaces and the port keeps the same
			// trick rather than inventing a different one.
			.Text(VoxelUIStrings::LoadingTitle())
			.Font(Style.Serif(L.LoadingTitleSize))
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			.ShadowOffset(FVector2D(3.f, 3.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.95f))
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, HalfSep))
		[
			SNew(SBox)
			// Room for two lines, so a long quip wrapping does not shove the
			// bar and percentage down and back up every 2.5 seconds.
			.MinDesiredHeight(L.LoadingQuipMinHeight)
			.WidthOverride(L.LoadingColumnWidth)
			[
				SNew(STextBlock)
				.Text(this, &SVoxelLoadingScreen::GetQuipText)
				.Font(Style.Serif(L.LoadingQuipSize))
				.ColorAndOpacity(this, &SVoxelLoadingScreen::GetQuipColour)
				.ShadowOffset(FVector2D(2.f, 2.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, HalfSep))
		[
			SNew(SBox)
			.WidthOverride(L.LoadingBarWidth)
			.HeightOverride(L.LoadingBarHeight)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					// "Dark leather", the GDScript's own word for it.
					.ColorAndOpacity(FSlateColor(FLinearColor(0.04f, 0.024f, 0.016f, 1.f)))
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				[
					SNew(SBox)
					.WidthOverride(this, &SVoxelLoadingScreen::GetBarFillWidth)
					[
						SNew(SImage)
						.Image(Style.SolidWhite())
						.ColorAndOpacity(FSlateColor(Tint(SandBright)))
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, HalfSep))
		[
			SNew(STextBlock)
			.Text(this, &SVoxelLoadingScreen::GetPercentText)
			.Font(Style.Serif(L.LoadingPctSize))
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			.ShadowOffset(FVector2D(2.f, 2.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
		];

	// The FPS readout's outline is part of the font, not the text block.
	FSlateFontInfo FpsFont = Style.Serif(L.FpsFontSize);
	FpsFont.OutlineSettings.OutlineSize = 4;
	FpsFont.OutlineSettings.OutlineColor = FLinearColor(0.f, 0.f, 0.f, 0.85f);

	ChildSlot
	[
		SNew(SOverlay)

		// Crossfading background pair.
		+ SOverlay::Slot()
		[
			SNew(SVoxelCoverImage)
			.Brush(this, &SVoxelLoadingScreen::GetBackgroundA)
			.Opacity(this, &SVoxelLoadingScreen::GetBackgroundAOpacity)
		]
		+ SOverlay::Slot()
		[
			SNew(SVoxelCoverImage)
			.Brush(this, &SVoxelLoadingScreen::GetBackgroundB)
			.Opacity(this, &SVoxelLoadingScreen::GetBackgroundBOpacity)
		]

		// 62% black. Also what stands in for the retired vignette.
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(FSlateColor(FLinearColor(0.f, 0.f, 0.f, L.LoadingTintAlpha)))
		]

		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(L.LoadingColumnWidth)
			.HeightOverride(L.LoadingColumnHeight)
			[
				Column
			]
		]

		// TIP footer, pinned to the bottom edge with the GDScript's insets.
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.LoadingTipInsetX, 0.f, L.LoadingTipInsetX, L.LoadingTipInsetBottom))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				// Godot renders this through a BBCode RichTextLabel purely to
				// colour the "TIP" prefix gold. Two text blocks give the same
				// pixels without pulling in SRichTextBlock and its decorator
				// and style-set requirements.
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text(VoxelUIStrings::TipPrefix())
					.Font(Style.Serif(L.LoadingTipSize))
					.ColorAndOpacity(FSlateColor(Tint(TipGold)))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(12.f, 0.f, 0.f, 0.f))
				[
					SNew(STextBlock)
					.Text(this, &SVoxelLoadingScreen::GetTipText)
					.Font(Style.Serif(L.LoadingTipSize))
					.ColorAndOpacity(FSlateColor(Tint(InkDim, 0.55f)))
				]
			]
		]

		// FPS readout, top-right.
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, 12.f, 12.f, 0.f))
		[
			SNew(STextBlock)
			.Text(this, &SVoxelLoadingScreen::GetFpsText)
			.Font(FpsFont)
			.ColorAndOpacity(this, &SVoxelLoadingScreen::GetFpsColour)
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.7f))
			.Justification(ETextJustify::Right)
		]
	];

	OnShown();
}

void SVoxelLoadingScreen::OnShown()
{
	FRandomStream Stream = MakeVoxelUIRandomStream();
	FVoxelUIAssetLibrary::Get().ShuffleOrder(Stream);
	BackgroundIndexA = 0;
	BackgroundIndexB = 1;
	BackgroundTimer = 0.f;
	CrossfadeAlpha = 0.f;
	bCrossfading = false;

	// Both lists reshuffled per show, matching _show_loading_screen. Two
	// separate orders, because the two rotators run at different cadences and
	// sharing one would make them drift into lockstep.
	QuipOrder = SVoxelLoadingDetail::ShuffledIndices(VoxelUIStrings::LoadingQuips().Num(), Stream);
	TipOrder = SVoxelLoadingDetail::ShuffledIndices(VoxelUIStrings::GameplayTips().Num(), Stream);
	QuipCursor = 0;
	TipCursor = 0;
	QuipTimer = 0.f;
	TipTimer = 0.f;
	QuipFadeAlpha = 1.f;
	BobTime = 0.f;
}

void SVoxelLoadingScreen::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// The curtain fade. Applied through SWidget::SetRenderOpacity rather than
	// a bound argument because RenderOpacity is a construction-time value in
	// Slate, not an attribute -- and this one changes every frame of the 0.4 s
	// fade.
	SetRenderOpacity(CurtainOpacity);

	// ANIMATION USES A CLAMPED DELTA, gameplay timing does not. A cold chunk
	// cascade genuinely produces 100 ms frames, and letting the bob and the
	// quip crossfade step that far at once turns smooth motion into a series
	// of jumps. The GDScript clamps at 0.05 for the same reason.
	const float AnimDelta = FMath::Min(InDeltaTime, L.MaxAnimationDelta);

	BobTime += AnimDelta;

	// --- Background rotation ------------------------------------------------
	if (FVoxelUIAssetLibrary::Get().NumBackgrounds() > 1)
	{
		if (bCrossfading)
		{
			CrossfadeAlpha += AnimDelta / FMath::Max(L.BackgroundFade, 0.001f);
			if (CrossfadeAlpha >= 1.f)
			{
				// The incoming image becomes the resident one and the other
				// slot is freed to load the next.
				CrossfadeAlpha = 0.f;
				bCrossfading = false;
				BackgroundIndexA = BackgroundIndexB;
				BackgroundIndexB = BackgroundIndexA + 1;
				BackgroundTimer = 0.f;
			}
		}
		else
		{
			BackgroundTimer += AnimDelta;
			if (BackgroundTimer >= L.BackgroundRotate)
			{
				bCrossfading = true;
				CrossfadeAlpha = 0.f;
			}
		}
	}

	// --- Quip rotation ------------------------------------------------------
	// Fade out over QuipFade, swap, fade back in over QuipFade, hold until
	// QuipRotate has elapsed in total.
	QuipTimer += AnimDelta;
	const float FadeOutStart = FMath::Max(L.QuipRotate - L.QuipFade, 0.f);
	if (QuipTimer >= L.QuipRotate)
	{
		QuipTimer = 0.f;
		QuipCursor = QuipOrder.Num() > 0 ? (QuipCursor + 1) % QuipOrder.Num() : 0;
		QuipFadeAlpha = 0.f;
	}
	else if (QuipTimer >= FadeOutStart)
	{
		QuipFadeAlpha = 1.f - (QuipTimer - FadeOutStart) / FMath::Max(L.QuipFade, 0.001f);
	}
	else
	{
		// Fading back in after a swap, then held at 1.
		QuipFadeAlpha = FMath::Min(1.f, QuipTimer / FMath::Max(L.QuipFade, 0.001f));
	}

	// --- Tip rotation (hard cut, no fade) -----------------------------------
	TipTimer += AnimDelta;
	if (TipTimer >= L.TipRotate)
	{
		TipTimer = 0.f;
		TipCursor = TipOrder.Num() > 0 ? (TipCursor + 1) % TipOrder.Num() : 0;
	}

	// --- FPS readout --------------------------------------------------------
	// Sampled from the REAL delta, not the clamped one: the entire point of
	// this readout is to show how bad the frame time is.
	FrameTimesMs[FrameCursor] = InDeltaTime * 1000.f;
	FrameCursor = (FrameCursor + 1) % FrameTimesMs.Num();

	FpsRefreshTimer += InDeltaTime;
	if (FpsRefreshTimer >= L.FpsRefreshInterval)
	{
		FpsRefreshTimer = 0.f;
		float WorstMs = 0.f;
		for (const float Sample : FrameTimesMs)
		{
			WorstMs = FMath::Max(WorstMs, Sample);
		}
		const int32 Fps = InDeltaTime > 0.f ? FMath::RoundToInt(1.f / InDeltaTime) : 0;
		// Rebuilt only four times a second. An STextBlock re-lays-out its text
		// whenever the string changes, and doing that every frame while the
		// machine is already struggling is precisely the wrong time to pay for
		// it -- the GDScript throttles this for the same reason.
		CachedFpsText = FText::FromString(FString::Printf(TEXT("FPS: %d\nworst: %d ms"), Fps,
		                                                  FMath::RoundToInt(WorstMs)));
		bFpsWorstIsBad = WorstMs > SVoxelLoadingDetail::kWorstFrameBadMs;
	}
}

// --- Attribute readers ------------------------------------------------------

const FSlateBrush* SVoxelLoadingScreen::GetBackgroundA() const
{
	return FVoxelUIAssetLibrary::Get().RequestBackground(BackgroundIndexA);
}

const FSlateBrush* SVoxelLoadingScreen::GetBackgroundB() const
{
	return FVoxelUIAssetLibrary::Get().RequestBackground(BackgroundIndexB);
}

float SVoxelLoadingScreen::GetBackgroundAOpacity() const
{
	return bCrossfading ? 1.f - CrossfadeAlpha : 1.f;
}

float SVoxelLoadingScreen::GetBackgroundBOpacity() const
{
	return bCrossfading ? CrossfadeAlpha : 0.f;
}

FText SVoxelLoadingScreen::GetQuipText() const
{
	const TArray<FText>& Quips = VoxelUIStrings::LoadingQuips();
	if (QuipOrder.Num() == 0 || Quips.Num() == 0)
	{
		return FText::GetEmpty();
	}
	return Quips[QuipOrder[QuipCursor % QuipOrder.Num()]];
}

FSlateColor SVoxelLoadingScreen::GetQuipColour() const
{
	return FSlateColor(VoxelUITheme::Tint(VoxelUITheme::InkDim, QuipFadeAlpha));
}

FText SVoxelLoadingScreen::GetTipText() const
{
	const TArray<FText>& Tips = VoxelUIStrings::GameplayTips();
	if (TipOrder.Num() == 0 || Tips.Num() == 0)
	{
		return FText::GetEmpty();
	}
	return Tips[TipOrder[TipCursor % TipOrder.Num()]];
}

FText SVoxelLoadingScreen::GetPercentText() const
{
	// floor, not round: 99.6% should read 99%, because a bar that says 100%
	// while the world is still landing is the specific lie this whole progress
	// model exists to avoid.
	return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::FloorToInt(Progress * 100.f)));
}

FOptionalSize SVoxelLoadingScreen::GetBarFillWidth() const
{
	return FOptionalSize(FVoxelMenuLayout::Get().LoadingBarWidth * Progress);
}

FMargin SVoxelLoadingScreen::GetHourglassBobPadding() const
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	// sin over a 5.2 s cycle, +/-2 px, negated so the hourglass rises first --
	// the GDScript's `bob_phase * -2.0`.
	const float Phase = FMath::Sin((BobTime / FMath::Max(L.HourglassBobPeriod, 0.001f)) * 2.f * PI);
	const float TopPx = FMath::Max(0.f, L.HourglassOffsetY + Phase * -L.HourglassBobPixels);
	return FMargin(0.f, TopPx, 0.f, 0.f);
}

FText SVoxelLoadingScreen::GetFpsText() const
{
	return CachedFpsText;
}

FSlateColor SVoxelLoadingScreen::GetFpsColour() const
{
	return FSlateColor(bFpsWorstIsBad ? FLinearColor(1.f, 0.5f, 0.5f, 0.95f) : FLinearColor(1.f, 1.f, 1.f, 0.85f));
}

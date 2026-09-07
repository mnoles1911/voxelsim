#include "SVoxelLoadingScreen.h"

#include "SVoxelCoverImage.h"
#include "SVoxelHourglass.h"
#include "VoxelEarthUI.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Layout/SSpacer.h"

#include "HAL/IConsoleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelLoadingDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// voxel.UI.LoadingFps -- the loading screen's top-right FPS / worst-frame
// readout, REMOVED FROM THE DEFAULT SCREEN by owner directive (2026-09-05):
// under the load theatre the screen is a show, and a frame-health figure in
// its corner is stagecraft showing its rigging. The capability stays, behind
// this flag, because it is still the cheapest way to see whether the
// loading-screen streaming cap is actually protecting frame pacing -- the
// same voxel.UI.* debug-flag shape as voxel.UI.HoverSlide. Read once per
// show, at Construct: the screen is rebuilt every time it appears.
int32 GLoadingFpsReadout = 0;
FAutoConsoleVariableRef CVarLoadingFpsReadout(TEXT("voxel.UI.LoadingFps"),
                                              GLoadingFpsReadout,
                                              TEXT("1 shows the loading screen's top-right FPS / worst-ms readout. ")
                                              TEXT("0 (default): absent -- the readout is a debug instrument now, ")
                                              TEXT("not part of the screen."),
                                              ECVF_Default);

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

SVoxelLoadingScreen::~SVoxelLoadingScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelLoadingScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// The frame-sample ring exists only when the debug readout does.
	bFpsReadoutEnabled = SVoxelLoadingDetail::GLoadingFpsReadout != 0;
	if (bFpsReadoutEnabled)
	{
		FrameTimesMs.SetNumZeroed(SVoxelLoadingDetail::kFrameSamples);
	}

	const float HalfSep = L.LoadingSeparation * 0.5f;

	// 2026-09-07 mock: the title is tracked (10 px at 40 px) rather than
	// spelled with spaces, and the quip and tip are set in the italic hand face.
	FSlateFontInfo TitleFont = Style.Serif(L.LoadingTitleSize);
	TitleFont.LetterSpacing = L.LoadingTitleLetterSpacing;
	FSlateFontInfo VersionFont = Style.Serif(L.VersionFontSize);
	VersionFont.LetterSpacing = 187; // 3 px at 16 px

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
					// Self-driven (2026-09-07 mock): drains, rests, turns over,
					// and tells this screen to change its line at each turn.
					// Load progress no longer drives the sand -- see the header.
					SAssignNew(Hourglass, SVoxelHourglass)
					.SelfDriven(true)
					.OnFlipped(this, &SVoxelLoadingScreen::OnHourglassFlipped)
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
			.Font(TitleFont)
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
				// .msg: the italic hand face. The colour attribute below keeps
				// the crossfade; its base is parchment, per the mock.
				.Font(Style.HandItalic(L.LoadingQuipSize))
				.ColorAndOpacity(this, &SVoxelLoadingScreen::GetQuipColour)
				.ShadowOffset(FVector2D(2.f, 2.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
			]
		];
		// The progress bar and percentage that used to follow the quip were
		// REMOVED 2026-09-07 (owner directive; the mock has neither -- see the
		// header). The theatre progress still drives the curtain, not pixels.

	TSharedRef<SOverlay> Root =
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

		// The stack sits in the upper third (.stack top:33%), not centred as the
		// Godot build had it. Two fill-height spacers carry the fraction so the
		// placement scales with the viewport instead of being a pixel offset.
		//
		// The column matches the mock exactly now: hourglass, LOADING, one line.
		// The bar and percentage kept on 2026-09-06 were dropped on 2026-09-07 at
		// the owner's direction, in favour of the mock's turning hourglass.
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Fill)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(L.LoadingStackTopFrac)
			[
				SNew(SSpacer)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(L.LoadingColumnWidth)
				[
					Column
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f - L.LoadingStackTopFrac)
			[
				SNew(SSpacer)
			]
		]

		// .version-stamp, bottom-left, new on this screen in the 2026-09-07 mock.
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.VersionInsetLeft, 0.f, 0.f, L.VersionInsetBottom))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::VersionStamp())
			.Font(VersionFont)
			.ColorAndOpacity(FSlateColor(Tint(Parchment, 0.32f)))
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f))
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
					// .tip: italic hand face at rgba(230,213,168,.55) ~ parchment @ 0.55.
					.Font(Style.HandItalic(L.LoadingTipSize))
					.ColorAndOpacity(FSlateColor(Tint(Parchment, 0.55f)))
				]
			]
		];

	// FPS readout, top-right -- DEBUG-ONLY, absent by default, per the owner
	// directive that removed it from the shipping screen. voxel.UI.LoadingFps
	// puts it back; see the flag's comment at the top of this file.
	if (bFpsReadoutEnabled)
	{
		// The readout's outline is part of the font, not the text block.
		FSlateFontInfo FpsFont = Style.Serif(L.FpsFontSize);
		FpsFont.OutlineSettings.OutlineSize = 4;
		FpsFont.OutlineSettings.OutlineColor = FLinearColor(0.f, 0.f, 0.f, 0.85f);

		Root->AddSlot()
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
		];
	}

	ChildSlot
	[
		Root
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
	QuipPhase = EQuipPhase::Idle;
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

	// --- Quip change, once per turn of the glass ----------------------------
	// OnHourglassFlipped starts the fade-out; this carries it through the swap
	// and the fade-in and then holds. No timer of its own: the cadence is the
	// hourglass's (the mock's rotateTip() is only called from the flip phase).
	{
		const float Fade = FMath::Max(L.QuipFade, 0.001f);
		switch (QuipPhase)
		{
		case EQuipPhase::FadingOut:
			QuipTimer += AnimDelta;
			QuipFadeAlpha = FMath::Max(0.f, 1.f - QuipTimer / Fade);
			if (QuipTimer >= Fade)
			{
				QuipCursor = QuipOrder.Num() > 0 ? (QuipCursor + 1) % QuipOrder.Num() : 0;
				QuipPhase = EQuipPhase::FadingIn;
				QuipTimer = 0.f;
			}
			break;
		case EQuipPhase::FadingIn:
			QuipTimer += AnimDelta;
			QuipFadeAlpha = FMath::Min(1.f, QuipTimer / Fade);
			if (QuipTimer >= Fade)
			{
				QuipPhase = EQuipPhase::Idle;
				QuipFadeAlpha = 1.f;
			}
			break;
		case EQuipPhase::Idle:
		default:
			QuipFadeAlpha = 1.f;
			break;
		}
	}

	// --- Tip rotation (hard cut, no fade) -----------------------------------
	TipTimer += AnimDelta;
	if (TipTimer >= L.TipRotate)
	{
		TipTimer = 0.f;
		TipCursor = TipOrder.Num() > 0 ? (TipCursor + 1) % TipOrder.Num() : 0;
	}

	// --- FPS readout (debug-only; see GLoadingFpsReadout) -------------------
	// Sampled from the REAL delta, not the clamped one: the entire point of
	// this readout is to show how bad the frame time is. When the readout is
	// off -- the default -- the screen pays nothing here at all.
	if (bFpsReadoutEnabled && FrameTimesMs.Num() > 0)
	{
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
			// Rebuilt only four times a second. An STextBlock re-lays-out its
			// text whenever the string changes, and doing that every frame
			// while the machine is already struggling is precisely the wrong
			// time to pay for it -- the GDScript throttles this for the same
			// reason.
			CachedFpsText = FText::FromString(FString::Printf(TEXT("FPS: %d\nworst: %d ms"), Fps,
			                                                  FMath::RoundToInt(WorstMs)));
			bFpsWorstIsBad = WorstMs > SVoxelLoadingDetail::kWorstFrameBadMs;
		}
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
	if (bLoadFailed) return FText::FromString(TEXT("This save could not be restored."));
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
	if (bLoadFailed) return FText::FromString(TEXT("Your checkpoint has been preserved. Restart the game before trying another save."));
	const TArray<FText>& Tips = VoxelUIStrings::GameplayTips();
	if (TipOrder.Num() == 0 || Tips.Num() == 0)
	{
		return FText::GetEmpty();
	}
	return Tips[TipOrder[TipCursor % TipOrder.Num()]];
}

void SVoxelLoadingScreen::OnHourglassFlipped()
{
	// A turn that lands mid-fade restarts the fade-out from the current alpha
	// rather than snapping; with 7.8 s between turns and a 0.4 s fade that is
	// a defensive branch, not an expected one.
	if (QuipPhase == EQuipPhase::FadingOut)
	{
		return;
	}
	QuipPhase = EQuipPhase::FadingOut;
	QuipTimer = (1.f - QuipFadeAlpha) * FMath::Max(FVoxelMenuLayout::Get().QuipFade, 0.001f);
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

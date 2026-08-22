#pragma once
// The loading screen: a port of TransitionManager._build_loading_screen and
// the per-frame work in its _process.
//
// Layered bottom-up: a crossfading pair of background images, a 62% black
// tint, a centred column (hourglass, "L O A D I N G", rotating quip, progress
// bar, percentage), a TIP footer pinned to the bottom, and an FPS readout in
// the top-right corner.
//
// TWO PERFORMANCE SIMPLIFICATIONS ARE CARRIED FORWARD AS DECISIONS, and they
// are the reason this screen looks slightly plainer than the HTML mock:
//
//   * NO VIGNETTE. The mock's `box-shadow: inset 0 0 240px rgba(0,0,0,0.7)`
//     was implemented in the Godot build as a full-screen radial-darken
//     shader and then REMOVED, because the GPU behind this curtain is already
//     chewing through chunk streaming and a full-viewport per-pixel pass on
//     top spiked frame delta into visible-stutter range. The 62% tint provides
//     the corner-darkened feel. If the radial shape is ever wanted back, the
//     GDScript's own recommendation stands: bake it offline and draw it as one
//     full-screen image, which FVoxelUIAssetLibrary can now load trivially.
//
//   * FLAT PROGRESS BAR. The mock's `linear-gradient(90deg, sand-deep,
//     sand-bright)` plus a trailing white highlight went the same way, for the
//     same reason. Slate's MakeCustomVerts would give per-vertex colour for
//     free, so the gradient COULD come back cheaply here -- but only with a
//     measurement, and never silently.
//
// The reason to write both of those down is that each looks like an omission
// and is in fact a decision somebody already paid for once.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Types/SlateStructs.h" // FOptionalSize, used in the bar-fill accessor

class VOXELEARTHUI_API SVoxelLoadingScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelLoadingScreen) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	// This widget holds bare pointers into FVoxelUIStyle (SButton's
	// FButtonStyle*, SImage's FSlateBrush*), so its lifetime is what that
	// singleton's Shutdown assertion counts.
	virtual ~SVoxelLoadingScreen() override;

	// Called each time the screen appears. Reshuffles the background, quip and
	// tip orders -- the Godot build shuffles all three fresh on every show, so
	// a player rarely sees the same opener twice.
	void OnShown();

	// 0..1. The caller is responsible for the monotone clamp and the
	// elapsed-time floor; this widget just draws what it is given.
	void SetProgress(float InProgress) { Progress = FMath::Clamp(InProgress, 0.f, 1.f); }

	// Drives the fade in and out of the whole curtain.
	void SetCurtainOpacity(float InOpacity) { CurtainOpacity = FMath::Clamp(InOpacity, 0.f, 1.f); }

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	// --- Attribute readers, bound into the widget tree ----------------------
	const struct FSlateBrush* GetBackgroundA() const;
	const struct FSlateBrush* GetBackgroundB() const;
	float GetBackgroundAOpacity() const;
	float GetBackgroundBOpacity() const;
	FText GetQuipText() const;
	FSlateColor GetQuipColour() const;
	FText GetTipText() const;
	FText GetPercentText() const;
	FOptionalSize GetBarFillWidth() const;
	// The bob, expressed as top padding inside the hourglass wrapper. Slate
	// exposes RenderTransform as a construction ARGUMENT rather than a bound
	// attribute, so an animated offset has to move through layout -- and the
	// wrapper is 12 px taller than the hourglass precisely to give it room
	// (the GDScript sizes it that way for the same bob, plus cap overflow).
	FMargin GetHourglassBobPadding() const;
	FText GetFpsText() const;
	FSlateColor GetFpsColour() const;

	float Progress = 0.f;
	float CurtainOpacity = 1.f;

	// --- Background rotation ------------------------------------------------
	// Two slots crossfading; the "incoming" one is whichever is currently
	// fading up. Indices walk forward through the shuffled library order.
	int32 BackgroundIndexA = 0;
	int32 BackgroundIndexB = 1;
	float BackgroundTimer = 0.f;
	float CrossfadeAlpha = 0.f;  // 0 = A fully visible, 1 = B fully visible
	bool bCrossfading = false;

	// --- Text rotators ------------------------------------------------------
	TArray<int32> QuipOrder;
	int32 QuipCursor = 0;
	float QuipTimer = 0.f;
	// The quip fades out, swaps, and fades back in; the tip is a hard cut.
	float QuipFadeAlpha = 1.f;

	TArray<int32> TipOrder;
	int32 TipCursor = 0;
	float TipTimer = 0.f;

	// --- Hourglass bob ------------------------------------------------------
	float BobTime = 0.f;

	// --- FPS readout --------------------------------------------------------
	// A fixed 60-sample ring, sized once, so the per-frame write never
	// allocates -- the same reason the GDScript pre-sizes its array.
	TArray<float> FrameTimesMs;
	int32 FrameCursor = 0;
	float FpsRefreshTimer = 0.f;
	FText CachedFpsText;
	bool bFpsWorstIsBad = false;

	// Held rather than discarded: the widget drives itself from the Progress
	// attribute, so nothing reads this today, but a handle to the one piece of
	// this screen with its own simulation is worth having when something needs
	// to be reset or paused.
	TSharedPtr<class SVoxelHourglass> Hourglass;
};

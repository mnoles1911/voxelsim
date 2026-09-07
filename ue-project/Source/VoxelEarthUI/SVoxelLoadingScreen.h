#pragma once
// The loading screen: a port of TransitionManager._build_loading_screen and
// the per-frame work in its _process.
//
// Layered bottom-up: a crossfading pair of background images, a 62% black
// tint, a column in the upper third (the self-turning hourglass, "LOADING",
// a line of text that changes once per turn of the glass), and a TIP footer
// pinned to the bottom with the version stamp bottom-left. The Godot build's
// top-right FPS readout is DEBUG-ONLY now (owner directive, 2026-09-05):
// absent by default, restored by voxel.UI.LoadingFps -- see the flag's
// comment in the .cpp.
//
// NO PROGRESS BAR AND NO PERCENTAGE (owner directive, 2026-09-07): "the latest
// Voxelmark Loading Screen html asset does not have a loading bar at all and
// now only has a rotating hourglass icon". The 2026-09-06 smooth-bar directive
// is superseded by the mock; the theatre progress model
// (ComputeTheatreProgress) still runs and still gates the curtain, it simply
// no longer draws anything. SetProgress stays so the caller is unchanged.
//
// ONE PERFORMANCE SIMPLIFICATION IS CARRIED FORWARD AS A DECISION, and it is
// the reason this screen looks slightly plainer than the HTML mock:
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
// The reason to write that down is that it looks like an omission and is in
// fact a decision somebody already paid for once.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

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

	// 0..1. The caller owns the progress model (ComputeTheatreProgress: the
	// eased artificial timer, the monotone clamp, the ~97% hold while the
	// world is slower than the theatre); this widget just draws what it is
	// given.
	void SetLoadFailed() { bLoadFailed=true; }
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
	// Bound to SVoxelHourglass::OnFlipped: starts the quip's fade-out; the swap
	// and fade-in follow in Tick. The line changes once per turn of the glass
	// (about every 7.8 s), never on its own timer -- the mock's rotateTip() is
	// only ever called from the flip phase.
	void OnHourglassFlipped();
	// The bob, expressed as top padding inside the hourglass wrapper. Slate
	// exposes RenderTransform as a construction ARGUMENT rather than a bound
	// attribute, so an animated offset has to move through layout -- and the
	// wrapper is 12 px taller than the hourglass precisely to give it room
	// (the GDScript sizes it that way for the same bob, plus cap overflow).
	FMargin GetHourglassBobPadding() const;
	FText GetFpsText() const;
	FSlateColor GetFpsColour() const;

	bool bLoadFailed=false;
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
	// The quip fades out, swaps, and fades back in (QuipFade each way); the
	// tip is a hard cut. Idle: alpha held at 1. FadingOut/FadingIn: QuipTimer
	// runs 0..QuipFade. Kicked off by OnHourglassFlipped, never by a timer.
	enum class EQuipPhase : uint8 { Idle, FadingOut, FadingIn };
	EQuipPhase QuipPhase = EQuipPhase::Idle;
	float QuipTimer = 0.f;
	float QuipFadeAlpha = 1.f;

	TArray<int32> TipOrder;
	int32 TipCursor = 0;
	float TipTimer = 0.f;

	// --- Hourglass bob ------------------------------------------------------
	float BobTime = 0.f;

	// --- FPS readout (debug-only; voxel.UI.LoadingFps) ----------------------
	// Latched at Construct, so the widget tree and the tick agree for the
	// whole show whatever the flag does mid-load.
	bool bFpsReadoutEnabled = false;
	// A fixed 60-sample ring, sized once (and only when the readout is on), so
	// the per-frame write never allocates -- the same reason the GDScript
	// pre-sizes its array.
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

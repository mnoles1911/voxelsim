#pragma once
// The animated hourglass: a port of scripts/LoadingHourglass.gd, which is
// itself a port of the .hg block in assets/ui/html/Voxelmark Loading Screen.html.
//
// Drawn back to front:
//   1. Glass diamond -- two opposed triangles meeting at a two-unit-wide
//      waist, filled at 5% and stroked in gold.
//   2. Top sand, a triangle that drains from full to nothing as progress
//      advances 0 -> 1.
//   3. Bottom mound, growing from a point at the waist, with a small apex
//      bevel so it reads as settled sand rather than a wedge.
//   4. Falling grains: sub-pixel squares spawned at the waist, under gravity,
//      despawning on contact with the mound or the floor.
//   5. Front pillars, then the brass caps over everything.
//
// THREE ELEMENTS THE GDSCRIPT ALREADY DROPPED STAY DROPPED, and this is worth
// stating because the file's own header comment still lists them: the back
// support pillars, the inner glass stroke, and the sand-surface highlight.
// Each was removed for the same measured reason -- extra per-frame draw calls
// during chunk streaming, for something imperceptible at 96x144. Porting them
// back from the comment would undo a decision, not complete one.
//
// ALL MATH IS IN THE MOCK'S 40x60 SVG SPACE and scaled to the widget's actual
// size at paint time, so the look is identical whatever size the host picks.
//
// The simulation runs whether or not progress is moving, so the hourglass
// stays alive while the world streams -- which is most of the time it is on
// screen.
//
// TWO WAYS TO DRIVE IT:
//
//   * Progress-driven (the original): the caller binds Progress 0..1 and the
//     sand follows it. Still what a capture fixture wants, since a strip of
//     frames at known progress values is comparable.
//
//   * SELF-DRIVEN (SelfDriven=true; the loading screen, 2026-09-07 mock): the
//     glass drains on its own in kFillSeconds, rests for a beat, then TURNS
//     OVER -- a 180-degree render-transform rotation about its centre over
//     kFlipSeconds -- and starts again, firing OnFlipped at the start of each
//     turn so the host can change its line of text once per turn, as the mock
//     does. Progress is ignored. This is what the owner meant by "rotating
//     hourglass icon"; the mock's `.hg-flip { transform: rotateZ(var(--flip)) }`
//     is exactly this, minus the 3D tilt.
//
//     The turn is drawn by rotating the WHOLE widget, then snapping back to 0
//     and resetting the sand to full-top. That is not a cheat: the settled
//     mound at p=1 (base on the floor, apex at the waist) rotated by 180
//     degrees is the SAME polygon as the top sand at p=0 (base on the cap,
//     apex at the waist), so the snap is invisible -- which is also why the
//     mock can get away with accumulating --flip forever and counter-rotating
//     only the sand.

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Math/RandomStream.h"

class VOXELEARTHUI_API SVoxelHourglass : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelHourglass)
		: _Progress(0.f)
		, _SelfDriven(false)
	{}
		SLATE_ATTRIBUTE(float, Progress)
		// True: ignore Progress and run the fill -> rest -> flip loop above.
		SLATE_ARGUMENT(bool, SelfDriven)
		// Self-driven only: fired at the start of every turn of the glass.
		SLATE_EVENT(FSimpleDelegate, OnFlipped)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	                      FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override;

	// 96x144 -- the 2.4x upscale of the mock's 40x60 that the Godot build
	// settled on. The caller overrides it via an SBox; this is the fallback.
	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(96.f, 144.f); }

private:
	struct FGrain
	{
		float X = 0.f;
		float Y = 0.f;
		float VelX = 0.f;
		float VelY = 0.f;
		float Size = 0.7f;
		FLinearColor Colour = FLinearColor::White;
	};

	void PhysicsTick();
	void SpawnGrain();
	bool MoundContains(float X, float Y) const;

	TAttribute<float> ProgressAttribute;
	// Cached at Tick, read at OnPaint, because OnPaint is const and the
	// attribute may be a delegate the caller wants evaluated once per frame
	// rather than once per draw.
	float Progress = 0.f;

	// --- Self-driven loop (see the header comment) --------------------------
	enum class EPhase : uint8
	{
		Fill, // sand draining, Progress 0 -> 1 over kFillSeconds
		Rest, // a beat with the bottom chamber full
		Flip  // the 180-degree turn; sand frozen, widget rotating
	};
	void SelfDrivenTick(float DeltaSeconds);

	bool bSelfDriven = false;
	FSimpleDelegate OnFlipped;
	EPhase Phase = EPhase::Fill;
	float PhaseSeconds = 0.f;

	TArray<FGrain> Grains;
	float SpawnAccumulator = 0.f;
	float PhysicsAccumulator = 0.f;
	FRandomStream Random;
};

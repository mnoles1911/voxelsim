#pragma once
// "Voxelmark HUD v2": the compass strip along the top, and the hotbar dock at
// the bottom.
//
// THE OWNER'S RULING (2026-09-06, verbatim): "Kill crosshair. Remove mode line.
// Bottom left text block can be killed. Kill charge bar. all debug and
// diagnostics can stay but gate them behind a F1 or F3 overlay."
//
// The mock is ALREADY consistent with that ruling -- it draws no crosshair, no
// mode line, no bottom-left block and no charge bar -- so nothing in it had to
// be held back on those grounds. The Canvas HUD that used to draw all four
// (AVoxelEarthHUD::DrawHUD) had them removed under the same ruling before this
// port, and its diagnostics now sit behind bOverlayVisible or voxel.Debug >= 1.
// This widget therefore adds no always-on element the ruling forbids.
//
// WHAT THE MOCK SHOWS THAT THIS DOES NOT DRAW, and why:
//
//   * THE HEALTH AND HUNGER BARS. There is no health, no hunger, no stamina and
//     no damage anywhere in this project -- see VoxelScreenData.h for the
//     greps. A health bar with no health behind it would be drawn full, and a
//     full health bar is a claim, not a decoration: it tells the player they
//     are unhurt in a game that cannot hurt them, and it would keep telling
//     them that after something could. So the bars are gated on
//     FVoxelHudData::bHasVitals, which nothing sets yet. The code path, the
//     layout and the colours are all here; turning them on is one flag.
//   * THE LOW-HP VIGNETTE and the second-wind and stagger flashes, for the same
//     reason and behind the same flag.
//   * THE INTERACTION PROMPT ("E Examine the cairn"). There is no interaction
//     system and no examinable actor; the prompt is gated on a non-empty
//     FVoxelHudData::InteractPrompt, which nothing sets.
//
// WHAT IS REAL: the compass, which reads the player's yaw, and the hotbar,
// which is UVoxelInventoryComponent -- the same source SVoxelSurvivalPanel's
// hotbar already reads.
//
// THE COMPASS TAPE IS CLIPPED, NOT MASKED. The mock slides a 24-segment tape
// under a fixed frame with `overflow:hidden` and fades nothing. Slate's
// EWidgetClipping::ClipToBounds is the same thing, and the tape is offset by a
// negative left padding -- which Slate honours on a box-panel slot -- rather
// than by a render transform, so hit-testing and layout stay in agreement.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

// FVoxelHudData is declared in VoxelScreenData.h with the other screens' data,
// so the UHT-parsed UVoxelScreensUISubsystem.h can name it without pulling in
// Slate. See the comment above it there.

class VOXELEARTHUI_API SVoxelGameHud : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelGameHud) {}
		// AN ATTRIBUTE, NOT AN ARGUMENT. The HUD is the one widget in this
		// front end that must track a world that is still running, so it pulls
		// its state every paint rather than being rebuilt -- the same choice
		// SVoxelSurvivalPanel makes, and the reason its comment gives.
		SLATE_ATTRIBUTE(FVoxelHudData, Data)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelGameHud() override;

	// PULLED ONCE PER FRAME, NOT ONCE PER LAMBDA. The dock alone has three
	// attribute lambdas per hotbar cell and there are ten cells; reading the
	// attribute inside each would copy an FVoxelHudData -- which owns a TArray
	// -- forty-odd times a frame, every frame, for the whole session. This
	// project is frame-time bound (see UVoxelFrontEndSubsystem::IsTickable's
	// comment for the same reasoning applied to a per-frame no-op), so the
	// snapshot is taken here and every lambda reads the cached copy.
	virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override;

private:
	TSharedRef<class SWidget> BuildCompass();
	TSharedRef<class SWidget> BuildDock();
	// The `.bar::after` 10% rules, shared by both vitals bars.
	TSharedRef<class SWidget> BuildBarTicks() const;

	TAttribute<FVoxelHudData> Data;
	FVoxelHudData Cached;
	// The compass tape's offset, recomputed every paint from the heading.
	FMargin GetTapePadding() const;
};

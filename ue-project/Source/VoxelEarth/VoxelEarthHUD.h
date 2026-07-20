#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "VoxelEarthHUD.generated.h"

// m1-plan.md "Player experience decisions" (Matt sign-off), HUD row: plain
// DrawHUD canvas calls (no UMG) -- a crosshair dot plus bottom-left text
// showing the current dig/place cube size, palette material, and (while
// charging) a fill bar for the explosive throw charge. Reads its state from
// AVoxelEarthPlayerController (PlayerOwner); wired into
// AVoxelEarthGameMode::HUDClass.
UCLASS()
class VOXELEARTH_API AVoxelEarthHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	// Small filled square at the screen center.
	static constexpr float CrosshairHalfSizePx = 2.0f;

	// Bottom-left text block layout.
	static constexpr float MarginPx = 24.0f;
	static constexpr float LineHeightPx = 20.0f;

	// Charge bar (drawn above the text block while charging).
	static constexpr float ChargeBarWidthPx = 160.0f;
	static constexpr float ChargeBarHeightPx = 12.0f;
};

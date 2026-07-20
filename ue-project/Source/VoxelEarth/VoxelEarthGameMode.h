#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VoxelEarthGameMode.generated.h"

// Default game mode (docs/m1-plan.md Stage 2 decisions table item 3): fly
// pawn + our dig/place controller, spawned above the terrain surface at
// (0,0) rather than at a level-placed PlayerStart (none exists yet -- see
// RestartPlayer override).
UCLASS()
class VOXELEARTH_API AVoxelEarthGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AVoxelEarthGameMode();

	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual void BeginPlay() override;

private:
	FTimerHandle ScreenshotTimerHandle;
	FTimerHandle SecondShotTimerHandle;
	FTimerHandle QuitTimerHandle;

	// M2 wave 2 item 2 verification (docs/m2-plan.md "Distant-edit mip
	// propagation"): -VoxelHeadlessDigTest fires a large CarveSphere at the
	// spawn column once R0/R1 have had time to settle, so a headless -game
	// run can prove (via log lines + a screenshot) that ring chunks over the
	// dig site actually re-mesh through the overlay-aware path.
	FTimerHandle HeadlessDigTestTimerHandle;
};

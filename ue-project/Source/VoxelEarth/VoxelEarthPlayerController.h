#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VoxelEarthPlayerController.generated.h"

// Dig/place input (docs/m1-plan.md Stage 2 decisions table): LMB = dig, RMB
// = place, both via legacy raw-key bindings (no Enhanced Input assets). The
// actual raycast + edit-log submission lives in UVoxelWorldSubsystem
// (TryDig/TryPlace) -- this controller only supplies the camera ray.
UCLASS()
class VOXELEARTH_API AVoxelEarthPlayerController : public APlayerController
{
	GENERATED_BODY()

protected:
	virtual void SetupInputComponent() override;

private:
	void OnDig();
	void OnPlace();
};

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VoxelEarthPlayerController.generated.h"

// Dig/place/explosives input (docs/m1-plan.md "Player experience decisions",
// Matt sign-off): LMB = dig, RMB = place, both instant on press and sized by
// DigSizeVoxels (mouse wheel / number keys 1-3); 'T' cycles the placement
// palette; 'F' held charges a thrown explosive, released throws it. The
// actual raycast + edit-log submission lives in UVoxelWorldSubsystem
// (TryDig/TryPlace/CarveSphere) -- this controller only supplies the camera
// ray, the selected size/material, and (for explosives) spawns
// AVoxelExplosive. AVoxelEarthHUD reads the size/material/charge state back
// out via the getters below.
UCLASS()
class VOXELEARTH_API AVoxelEarthPlayerController : public APlayerController
{
	GENERATED_BODY()

protected:
	virtual void SetupInputComponent() override;

public:
	// --- HUD queries (AVoxelEarthHUD) --------------------------------------

	int32 GetDigSizeVoxels() const { return DigSizeVoxels; }

	// Raw vxc::MaterialId value (this header stays voxel-core-free by
	// doctrine, so the type is uint8, not vxc::MaterialId).
	uint8 GetPaletteMaterialId() const { return PaletteMaterialId; }

	bool IsChargingExplosive() const { return bChargingExplosive; }

	// 0 at charge start, 1 at/after MaxChargeSeconds; 0 when not charging.
	float GetExplosiveChargeAlpha() const;

private:
	void OnDig();
	void OnPlace();

	// Dig/place cube size selection (m1-plan.md "Dig sizes" row): mouse
	// wheel cycles 1<->2<->4, number keys 1/2/3 select directly.
	void CycleDigSizeUp();
	void CycleDigSizeDown();
	void SelectDigSize1();
	void SelectDigSize2();
	void SelectDigSize4();

	// Creative placement palette cycle (m1-plan.md "Place" row): rock -> soil
	// -> sand -> rock ...
	void CyclePaletteMaterial();

	// Explosive charge/throw (m1-plan.md "Explosives v1" row): hold F to
	// charge, release to throw -- see VoxelExplosive.h for the fuse/carve.
	void OnChargeStart();
	void OnChargeRelease();

	// docs/debug-tooling-plan.md P1 "CVars + F3": cycles voxel.Debug 0->1->2->0.
	void OnCycleDebugMode();

	int32 DigSizeVoxels = 1;

	// vxc::MAT_ROCK == 2 (voxelcore/core.h); kept as a numeric literal here
	// since this UHT-parsed header must stay voxel-core-free by doctrine.
	uint8 PaletteMaterialId = 2;

	bool bChargingExplosive = false;
	float ChargeStartTimeSeconds = 0.f;

	// m1-plan.md "Explosives v1" row: 0.3s->1.5s charge hold maps to
	// 600->1600 UU/s throw speed; thrown with a 30-degree upward arc.
	static constexpr float MinChargeSeconds = 0.3f;
	static constexpr float MaxChargeSeconds = 1.5f;
	static constexpr float MinThrowSpeedUU = 600.0f;
	static constexpr float MaxThrowSpeedUU = 1600.0f;
	static constexpr float ThrowUpwardArcDegrees = 30.0f;
};

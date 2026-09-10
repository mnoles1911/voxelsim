#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelSurvivalUISubsystem.generated.h"

// Client-only UI owns its input binding; gameplay has no reverse UI dependency.
UCLASS()
class VOXELEARTHUI_API UVoxelSurvivalUISubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;
	void ToggleInventory();
	void CloseInventory();

	// Hide the hotbar (and any open inventory) while a full-screen overlay owns
	// the viewport. Called by UVoxelPauseUISubsystem when the pause menu opens
	// and closes.
	//
	// VISIBILITY, NOT Detach(). Detach would destroy the input component and the
	// widget and rebuild both on resume, which is a lot of churn for something
	// the player is looking at for ten seconds -- and CanShow() cannot be the
	// hook anyway, because this subsystem does not tick while the game is
	// paused and would never see the state change.
	void SetHiddenForOverlay(bool bHidden);
private:
	// What the hotbar's visibility should return to. Kept because
	// CloseInventory also restores visibility and must not un-hide a hotbar the
	// pause menu is covering.
	EVisibility HotbarVisibility() const;

	bool bHiddenForOverlay = false;
	bool CanShow() const;
	void Detach();
	TWeakObjectPtr<class AVoxelEarthPlayerController> Controller;
	UPROPERTY(Transient) TObjectPtr<class UInputComponent> Input;
	TSharedPtr<class SVoxelSurvivalPanel> Panel;
	TSharedPtr<class SWidget> Hotbar;
	bool bSavedCursor = false;
};

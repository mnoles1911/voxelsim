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
private:
	bool CanShow() const;
	void Detach();
	TWeakObjectPtr<class AVoxelEarthPlayerController> Controller;
	UPROPERTY(Transient) TObjectPtr<class UInputComponent> Input;
	TSharedPtr<class SVoxelSurvivalPanel> Panel;
	TSharedPtr<class SWidget> Hotbar;
	bool bSavedCursor = false;
};

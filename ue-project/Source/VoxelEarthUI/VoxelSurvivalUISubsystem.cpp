#include "VoxelSurvivalUISubsystem.h"
#include "SVoxelSurvivalPanel.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelFrontEndSubsystem.h"
#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

namespace VoxelSurvivalUISubsystemPrivate
{
TAutoConsoleVariable<int32> Enabled(TEXT("voxel.UI.Survival"), 1,
	TEXT("Show the survival hotbar and I-key inventory/fieldcraft handbook. 0 disables both."));
}

bool UVoxelSurvivalUISubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
	return Type == EWorldType::Game || Type == EWorldType::PIE;
}

bool UVoxelSurvivalUISubsystem::IsTickable() const
{
	return !IsTemplate() && GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer;
}

TStatId UVoxelSurvivalUISubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelSurvivalUISubsystem, STATGROUP_Tickables);
}

bool UVoxelSurvivalUISubsystem::CanShow() const
{
	if (!GetWorld() || !GetWorld()->HasBegunPlay() || !VoxelSurvivalUISubsystemPrivate::Enabled.GetValueOnGameThread()) return false;
	const auto* Front = GetWorld()->GetSubsystem<UVoxelFrontEndSubsystem>();
	if (Front && Front->GetState() != EVoxelFrontEndState::Inactive && Front->GetState() != EVoxelFrontEndState::Playing) return false;
	const auto* PC = GetWorld()->GetFirstPlayerController();
	return PC && PC->IsLocalController() && PC->GetPawn() && GetWorld()->GetGameViewport();
}

void UVoxelSurvivalUISubsystem::Tick(float)
{
	if (!CanShow()) { if (Input || Hotbar || Panel) Detach(); return; }
	auto* PC = Cast<AVoxelEarthPlayerController>(GetWorld()->GetFirstPlayerController());
	if (PC != Controller.Get()) Detach();
	if (!PC || Input) return;
	Controller = PC;
	Input = NewObject<UInputComponent>(PC, TEXT("SurvivalUIInput"));
	Input->Priority = 20;
	Input->RegisterComponent();
	Input->BindKey(EKeys::I, IE_Pressed, this, &UVoxelSurvivalUISubsystem::ToggleInventory);
	PC->PushInputComponent(Input);
	Hotbar = SVoxelSurvivalPanel::MakeHotbar(Controller);
	// The passive hotbar never catches mouse input over the world.
	Hotbar->SetVisibility(HotbarVisibility());
	GetWorld()->GetGameViewport()->AddViewportWidgetContent(Hotbar.ToSharedRef(), 15);
}

void UVoxelSurvivalUISubsystem::ToggleInventory()
{
	if (Panel) { CloseInventory(); return; }
	if (!CanShow() || !Controller.IsValid() || Controller->IsChargingExplosive()) return;
	auto* PC = Controller.Get();
	bSavedCursor = PC->bShowMouseCursor;
	Panel = SNew(SVoxelSurvivalPanel).Controller(Controller)
		.OnClose(FSimpleDelegate::CreateUObject(this, &UVoxelSurvivalUISubsystem::CloseInventory));
	Hotbar->SetVisibility(EVisibility::Collapsed);
	GetWorld()->GetGameViewport()->AddViewportWidgetContent(Panel.ToSharedRef(), 30);
	if (PC->PlayerInput) PC->PlayerInput->FlushPressedKeys();
	PC->SetIgnoreMoveInput(true);
	PC->SetIgnoreLookInput(true);
	PC->bShowMouseCursor = true;
	FInputModeUIOnly Mode;
	Mode.SetWidgetToFocus(Panel);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
}

void UVoxelSurvivalUISubsystem::CloseInventory()
{
	if (!Panel) return;
	if (auto* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
		Viewport->RemoveViewportWidgetContent(Panel.ToSharedRef());
	Panel.Reset();
	if (Controller.IsValid())
	{
		Controller->SetIgnoreMoveInput(false);
		Controller->SetIgnoreLookInput(false);
		Controller->bShowMouseCursor = bSavedCursor;
		Controller->SetInputMode(FInputModeGameOnly());
	}
	if (Hotbar) Hotbar->SetVisibility(HotbarVisibility());
}

EVisibility UVoxelSurvivalUISubsystem::HotbarVisibility() const
{
	return bHiddenForOverlay ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
}

void UVoxelSurvivalUISubsystem::SetHiddenForOverlay(bool bHidden)
{
	if (bHiddenForOverlay == bHidden) return;
	bHiddenForOverlay = bHidden;
	// An inventory open when the pause menu arrives would sit UNDER it holding
	// its own UI input mode, and the two would fight over the cursor on resume.
	if (bHidden) CloseInventory();
	if (Hotbar) Hotbar->SetVisibility(HotbarVisibility());
}

void UVoxelSurvivalUISubsystem::Detach()
{
	CloseInventory();
	if (Hotbar)
	{
		if (auto* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr) Viewport->RemoveViewportWidgetContent(Hotbar.ToSharedRef());
		Hotbar.Reset();
	}
	if (Input)
	{
		if (Controller.IsValid()) Controller->PopInputComponent(Input);
		Input->DestroyComponent();
		Input = nullptr;
	}
	Controller.Reset();
}

void UVoxelSurvivalUISubsystem::Deinitialize()
{
	Detach();
	Super::Deinitialize();
}

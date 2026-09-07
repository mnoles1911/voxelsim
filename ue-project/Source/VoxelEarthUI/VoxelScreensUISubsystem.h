#pragma once
// The owner of the 2026-09-07 in-game screens: the five-tab stack, the death
// screen, the dialogue overlay and the HUD.
//
// A THIRD WORLD SUBSYSTEM, alongside UVoxelSurvivalUISubsystem and
// UVoxelPauseUISubsystem, and for the reason UVoxelPauseUISubsystem's header
// gives: UVoxelFrontEndSubsystem stops ticking in EVoxelFrontEndState::Playing
// on purpose, so anything that has to be alive while the player is playing
// cannot live there. This one is shaped like the pause subsystem, defers to the
// front end the same way, and overrides IsTickableWhenPaused for the same
// reason -- without it the -VoxelScreenShot chain stalls the instant it opens a
// screen, because the settle never elapses.
//
// IT TAKES THE I KEY FROM UVoxelSurvivalUISubsystem. That subsystem binds I at
// input priority 20 to open SVoxelSurvivalPanel, which is the EARLIER
// inventory screen -- raw FCoreStyle, hardcoded 1120x710, predating the whole
// overlay family. The 2026-09-07 inventory mock is its replacement, so this
// subsystem binds I at priority 25 (above survival's 20, below pause's 30) and
// shadows it. The old panel and its hotbar are hidden through the lever the
// pause menu already uses; nothing is deleted, so reverting is one cvar.
//
// THE HUD IS INSTALLED ONCE AND LIVES AS LONG AS THE WORLD. Every other widget
// this front end owns is built when something opens it; the HUD is the one that
// has to be there the whole time, which is why it pulls its state through an
// attribute instead of being rebuilt (see SVoxelGameHud).

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelScreenData.h" // EVoxelScreenTab, FVoxelHudData and the screen structs
#include "VoxelScreensUISubsystem.generated.h"

class APlayerController;
class UInputComponent;
// Forward-declared, not included: this header is UHT-parsed and stays free of
// Slate, exactly as VoxelPauseUISubsystem.h does with SVoxelPauseMenu.
class SVoxelGameHud;
class SWidget;

UCLASS()
class VOXELEARTHUI_API UVoxelScreensUISubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	// Mandatory, exactly as on UVoxelPauseUISubsystem: this subsystem's screens
	// pause the game, and FTickableGameObject stops ticking a paused world
	// unless told otherwise -- without which every capture here would stall
	// after the shutter was armed and before it fired.
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	bool IsScreenOpen() const { return Shell.IsValid(); }

	// Opens the stack on a given tab, or switches to it if already open.
	void OpenScreen(EVoxelScreenTab Tab);
	void CloseScreen();

	// The death screen and the dialogue overlay. Both are CAPTURE-ONLY today:
	// nothing can kill the player and there is no conversation system, so the
	// only callers are the -VoxelDeathShot and -VoxelDialogueShot arms below.
	// They are public so that the day either system lands, it calls these
	// rather than growing a second UI owner.
	void OpenDeathScreen();
	void OpenDialogue(const FVoxelDialogueData& Node);
	void CloseOverlay();

	// The map's marks, which the codex's PLACES list also reads. Session-scoped;
	// see SVoxelMapScreen for why they are not saved.
	const TArray<FVoxelMapMark>& GetMarks() const { return Marks; }

private:
	// UInputComponent::BindKey's delegate signature takes no payload, so each
	// key needs its own nullary target -- the same shape
	// AVoxelEarthPlayerController's OnUseHotbar1..5 thunks have, and for the
	// same reason.
	void OpenMapTab();
	void OpenJournalTab();
	void OpenInventoryTab();
	void OpenPlayerTab();
	void OpenCodexTab();

	bool CanShow() const;
	void EnsureInput(APlayerController* PC);
	void InstallHud(APlayerController* PC);
	void RemoveHud();

	// Builds the body for a tab and swaps it into the shell.
	void ShowTab(EVoxelScreenTab Tab);
	// Everything the screens read out of the world, gathered in one place so
	// the five bodies never reach into a subsystem themselves.
	FVoxelMapScreenData GatherMapData() const;
	FVoxelInventoryScreenData GatherInventoryData() const;
	FVoxelHudData GatherHudData() const;
	FText DayStamp() const;

	void ApplyOverlayInput(TSharedRef<class SWidget> Widget);
	void RestoreGameInput();
	void TeardownStack();

	TSharedPtr<class SVoxelScreenShell> Shell;
	TSharedPtr<class SVoxelDeathScreen> DeathScreen;
	TSharedPtr<class SVoxelDialogueOverlay> Dialogue;
	TSharedPtr<class SVoxelGameHud> Hud;

	UPROPERTY(Transient)
	TObjectPtr<UInputComponent> Input = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<APlayerController> Controller = nullptr;

	EVoxelScreenTab ActiveTab = EVoxelScreenTab::Inventory;
	TArray<FVoxelMapMark> Marks;
	bool bSavedShowCursor = false;

	// --- Capture switches ---------------------------------------------------
	// -VoxelScreenShot / -VoxelDeathShot / -VoxelDialogueShot / -VoxelHudShot,
	// all on the -VoxelPauseShot pattern: play for N seconds, open the thing,
	// settle another N so its glyphs rasterise, photograph with the UI on, quit.
	bool bCaptureRequested = false;
	float PlayingSeconds = 0.f;
	float CaptureQuitAtSeconds = 0.f;
	bool bShotOpened = false;
};

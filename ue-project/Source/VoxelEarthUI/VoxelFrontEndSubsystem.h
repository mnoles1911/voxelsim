#pragma once
// The front end's state machine: what is on screen, and when the world starts.
//
// A WORLD SUBSYSTEM IN THE UI MODULE, which is what keeps the dependency
// pointing one way. UVoxelWorldSubsystem and AVoxelEarthGameMode know nothing
// about this class; it finds them. The single fact the gameplay module needs
// -- does the front end run at all -- lives in VoxelEarth's own
// VoxelFrontEndPolicy.h precisely so that this header never has to be included
// from there. See VoxelEarthUI.Build.cs.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelFrontEndSubsystem.generated.h"

UENUM()
enum class EVoxelFrontEndState : uint8
{
	// The front end is suppressed for this run. IsTickable() is false forever
	// and nothing is ever added to the viewport.
	Inactive,
	// The main menu owns the screen. Streaming is held: ChunkOwner is null, so
	// UVoxelWorldSubsystem::Tick returns on its first line.
	Menu,
	// NEW GAME/CONTINUE pressed, loading screen up, waiting for the renderer
	// to actually paint it before the world starts. See ArmFrames.
	ArmLoading,
	// The world is streaming and the readiness gate is being polled.
	Loading,
	// The gate passed (or timed out); fading the curtain out.
	HandOff,
	// The player has the world. IsTickable() goes false; this is where a pause
	// menu would plug in.
	Playing,
};

UCLASS()
class VOXELEARTHUI_API UVoxelFrontEndSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

	EVoxelFrontEndState GetState() const { return State; }

private:
	void EnterMenu();
	void ApplyMenuInputMode();
	void RefreshSaveRows();

	void RequestNewGame();
	void RequestContinue();
	void RequestLoad(const FString& Slug);
	void RequestDelete(const FString& Slug);
	void RequestQuit();

	// Removes the menu from the viewport and hands input back to the game.
	void TeardownMenu();

	EVoxelFrontEndState State = EVoxelFrontEndState::Inactive;

	TSharedPtr<class SVoxelMainMenu> MenuWidget;

	// The player controller may not exist on the tick OnWorldBeginPlay runs,
	// so cursor/input-mode/HUD setup is deferred to the first tick that finds
	// one. This records whether that has happened.
	bool bMenuInputApplied = false;

	// Seconds spent in the current state, for the capture switches' timed
	// actions and for the unattended watchdog.
	float StateSeconds = 0.f;

	// True once the watchdog has fired, so it fires exactly once.
	bool bWatchdogTripped = false;

	// --- Capture-switch bookkeeping -----------------------------------------
	// Set when a -Voxel*Shot switch has taken its picture. The quit follows on
	// a delay rather than immediately: FScreenshotRequest only QUEUES the
	// request, and the frame that services it has to be rendered and written
	// before the process may leave. The existing -VoxelOverlayShot chain uses
	// 4 s for exactly this; matching it beats inventing a second number.
	bool bCaptureRequested = false;
	float CaptureQuitAtSeconds = 0.f;

	// Requests a screenshot WITH the UI on screen and arms the quit.
	void CaptureAndQuit(const TCHAR* ShotName);
};

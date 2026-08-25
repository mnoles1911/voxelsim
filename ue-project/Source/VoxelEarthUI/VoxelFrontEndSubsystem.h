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
	// Enabled, but the world has not begun play yet -- Initialize has run and
	// OnWorldBeginPlay has not.
	//
	// THIS STATE EXISTS BECAUSE ITS ABSENCE WAS A SHOWSTOPPER. State defaults
	// to Inactive, Initialize only ASSIGNED Inactive on the suppressed branch,
	// and OnWorldBeginPlay opens with `if (State == Inactive) return;` -- so on
	// the enabled path the log said "active" and the early-out fired anyway.
	// The menu never appeared, IsTickable() was false forever, and nothing said
	// so. "Enabled" and "not started yet" have to be distinguishable states,
	// not the same one.
	Pending,
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
	// Both out of line in the .cpp -- see the comment there. ReadyProbe holds
	// an incomplete type in this header.
	UVoxelFrontEndSubsystem();
	virtual ~UVoxelFrontEndSubsystem() override;
	// AND THE VTABLE HELPER, which is the half that was missing.
	//
	// Declaring the destructor out of line is necessary but NOT sufficient:
	// UHT emits UVoxelFrontEndSubsystem(FVTableHelper&) into
	// Module.VoxelEarthUI.gen.cpp, and that constructor instantiates
	// TUniquePtr's deleter in a translation unit that sees only this header --
	// where FVoxelWorldReadyProbe is still incomplete. That is the C4150 that
	// broke the module build. Declaring it here and defining it in the .cpp
	// moves the instantiation to where the type is complete.
	UVoxelFrontEndSubsystem(FVTableHelper& Helper);

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
	// -VoxelHourglassShot: the hourglass ALONE, at fixed progress values, on a
	// flat field. Isolated from the rest of the front end deliberately -- it is
	// the densest drawing here and the most likely to need iteration, and a
	// strip of it at 0/0.25/0.5/0.75/1.0 is one comparable image rather than
	// five screenshots that have to be lined up by hand.
	void EnterHourglassShot();
	void ApplyMenuInputMode();
	void RefreshSaveRows();

	void RequestNewGame();
	void RequestContinue();
	void RequestLoad(const FString& Slug);
	void RequestDelete(const FString& Slug);
	void RequestQuit();

	// Raises the curtain and arms the world start. EditLogPath is empty for
	// NEW GAME; SpawnOverride is null unless a save is restoring a position.
	void BeginLoad(const FString& EditLogPath, const FTransform* SpawnOverride);
	// Runs at the end of ArmLoading: opens the streaming gate and spawns the
	// pawn. Separated from BeginLoad so the two frames between them can be
	// spent PAINTING, which is the whole point -- see ArmFrames.
	void StartWorldAndPawn();
	void TickLoading(float DeltaSeconds);
	void TickHandOff(float DeltaSeconds);
	// The monotone, time-floored progress model. See the .cpp.
	float ComputeProgress() const;

	// Removes the menu from the viewport and hands input back to the game.
	void TeardownMenu();

	EVoxelFrontEndState State = EVoxelFrontEndState::Inactive;

	TSharedPtr<class SVoxelMainMenu> MenuWidget;
	TSharedPtr<class SWidget> HourglassShotWidget;
	TSharedPtr<class SVoxelLoadingScreen> LoadingWidget;

	// --- Loading state ------------------------------------------------------
	// Frames still to spend painting the curtain before the world starts.
	int32 ArmFrames = 0;
	FString PendingEditLogPath;
	TOptional<FTransform> PendingSpawnTransform;
	float LoadElapsedSeconds = 0.f;
	// Never allowed to decrease -- see ComputeProgress.
	float LastProgress = 0.f;
	float HandOffSeconds = 0.f;
	// Offsets from -VoxelLoadingShotAt that have not been captured yet.
	int32 NextLoadingShotIndex = 0;

	TUniquePtr<class FVoxelWorldReadyProbe> ReadyProbe;

	// The player controller may not exist on the tick OnWorldBeginPlay runs,
	// so cursor/input-mode/HUD setup is deferred to the first tick that finds
	// one. This records whether that has happened.
	bool bMenuInputApplied = false;

	// The music fade is a one-shot, and TickHandOff runs every frame until
	// the curtain finishes. Without this the fade would be restarted on each
	// tick and never actually descend.
	bool bMusicFadeStarted = false;

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

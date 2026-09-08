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
// By value, not TUniquePtr: a complete type here costs nothing (the header
// pulls in no MoviePlayer type -- see it) and avoids repeating the incomplete-
// type destructor dance ReadyProbe needed below.
#include "VoxelLoadingCurtainThread.h"
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

	// The loading-screen streaming cap: lowers voxel.Stream.ApplyBudgetMs for
	// the duration of the load theatre and restores it at reveal, so the screen
	// the player is actually watching stays smooth. See the .cpp.
	void CapStreamingForTheatre();
	void RestoreStreamingBudget();

	// Removes the menu from the viewport and hands input back to the game.
	//
	// bKeepMusic is the ONE thing that differs between the hand-off call and
	// every other one. Hand-off is not an exit -- the player is carrying on into
	// a world that is meant to keep the soundtrack -- so it passes true and the
	// music survives; a quit, a Deinitialize or a run that ends on the loading
	// screen passes the default and the track is stopped as it always was.
	void TeardownMenu(bool bKeepMusic = false);

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
	// THE SAME LOAD, ON THE WALL CLOCK, and the pair exists because they
	// disagree by 7.5x under a heavy load. On the 2026-09-07 live session the
	// readiness probe -- which accumulates the same DeltaSeconds this field
	// does -- reported "READY after 16.02s" at a wall-clock 119.9 s after it
	// started: the tick delta is clamped, so a 41.4 s theatre roll had about
	// five more WALL minutes to run before the curtain would have lifted. The
	// theatre duration is a quantity of the player's life, so it is measured
	// against the player's clock; see TickLoading.
	double LoadWallStartSeconds = 0.0;
	float LoadWallSeconds = 0.f;
	// Never allowed to decrease -- see ComputeTheatreProgress.
	float LastProgress = 0.f;
	float HandOffSeconds = 0.f;
	// Offsets from -VoxelLoadingShotAt that have not been captured yet.
	int32 NextLoadingShotIndex = 0;

	// --- Load theatre (owner directive, 2026-09-05) -------------------------
	// The rolled artificial duration the bar plays out against. Rolled fresh in
	// BeginLoad; see the comment there for the seeding.
	float TheatreDurationSeconds = 0.f;
	// When the world's gate first opened (ready or timed out), in loading
	// seconds; negative until it has. Feeds the reveal log line, which is the
	// engagement evidence a gate can grep for.
	float WorldReadyAtSeconds = -1.f;

	// --- Loading-screen streaming cap ---------------------------------------
	// Whether CapStreamingForTheatre changed voxel.Stream.ApplyBudgetMs, and
	// the value to put back. Restored at reveal, with TeardownMenu as the
	// backstop for every path that never reaches one.
	bool bStreamBudgetCapped = false;
	float SavedApplyBudgetMs = 0.f;

	// The second theatre cap (2026-09-08): the raster atlas's per-tick page
	// sweep, voxel.Stream.AtlasFillMs. Separate flag from the apply budget's on
	// purpose -- either cap may decline to engage, and one restore path must
	// not be able to skip the other. The saved value may legitimately be
	// NEGATIVE: -1 is the cvar's "use the latched -VoxelGpuRasterAtlasFillMs"
	// sentinel and restoring it is how an ordinary run gets its 2.0 ms back.
	bool bAtlasFillCapped = false;
	float SavedAtlasFillMs = -1.f;

	TUniquePtr<class FVoxelWorldReadyProbe> ReadyProbe;

	// --- The threaded loading curtain, 2026-09-07 (Phase 4) -----------------
	// Live only between BeginLoad and TeardownMenu. Inert unless
	// -VoxelLoadingScreenThread=1 AND the process can honour it (it cannot in
	// PIE); see VoxelLoadingCurtainThread.h.
	FVoxelLoadingCurtainThread CurtainThread;

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

#pragma once
// The in-game pause overlay's owner: Escape, the pause state, the save write,
// and the two ways out of a running world.
//
// WHY NOT UVoxelFrontEndSubsystem, which the header of that file names as
// "where a pause menu would plug in". Because of the line directly under it:
// `IsTickable()` returns false in EVoxelFrontEndState::Playing, on purpose and
// with a recorded reason -- "this project is frame-time bound and a per-frame
// no-op is still a per-frame call". Pause has to live exactly where that
// subsystem has stopped ticking. Making the front end tick through the whole
// session to hold a menu nobody has opened would undo a deliberate decision to
// save writing one small class.
//
// So this is a second world subsystem, shaped like UVoxelSurvivalUISubsystem --
// which solved the identical problem for the I key -- and it defers to the
// front end for the one thing it must not race: it opens nothing while a menu
// or a loading screen is up.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelPauseUISubsystem.generated.h"

class APlayerController;
class UInputComponent;
enum class EVoxelPausePanel : uint8;

// The one fact that has to outlive a UWorld.
//
// LOAD from the pause menu reopens the map (see ReturnToMenu) and the save has
// to be claimed on the far side of that, by the front end of the NEW world. A
// process-scope slug is what survives; it is consumed exactly once, so a later
// visit to the title screen does not load it again.
namespace VoxelPauseUIHandoff
{
VOXELEARTHUI_API void SetPendingLoadSlug(const FString& Slug);
VOXELEARTHUI_API FString ConsumePendingLoadSlug();
} // namespace VoxelPauseUIHandoff

UCLASS()
class VOXELEARTHUI_API UVoxelPauseUISubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	// THIS SUBSYSTEM'S WHOLE JOB HAPPENS WHILE THE GAME IS PAUSED, and
	// FTickableGameObject stops ticking a paused world unless told otherwise.
	// Without this the -VoxelPauseShot chain stalls the instant it opens the
	// overlay -- the second settle never elapses and the shutter never fires --
	// and any future per-frame work here would silently stop too. Resuming does
	// not depend on it (that arrives through Slate, which never stops), so the
	// failure would have looked like a broken capture rather than a broken tick.
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override;

	bool IsPaused() const { return Overlay.IsValid(); }

private:
	// True once the world belongs to the player: the front end is Playing (or
	// was never active at all), there is a local controller with a pawn, and
	// there is a viewport to draw in.
	bool CanPause() const;

	void TogglePause();
	void OpenPause();
	void ClosePause();
	// Removes the overlay and puts input, cursor and HUD back. Separated from
	// ClosePause because the capture path and Deinitialize both need the
	// teardown without the unpause bookkeeping being the caller's problem.
	void TeardownOverlay();

	void RefreshRows();
	void HandleSaveConfirmed(const FString& DisplayName);
	void HandleDeleteSave(const FString& Slug);
	// EXIT TO MENU, and LOAD -- which is the same journey with a destination.
	//
	// THERE IS NO WORLD TEARDOWN IN THIS PROJECT. UVoxelWorldSubsystem has
	// StartWorldSession and no counterpart; the front end's state machine is
	// one-way and ends at Playing; nothing anywhere calls OpenLevel. Writing a
	// real StopWorldSession is a streaming-side job well outside a UI port, so
	// both paths reopen the current map, which builds a fresh UWorld whose
	// UVoxelFrontEndSubsystem::OnWorldBeginPlay puts the title screen up again.
	// That is the documented fallback the brief allows, and it is honest about
	// its cost: a reopen is a full cold start, not a fade back to a menu.
	void ReturnToMenu(const FString& PendingLoadSlug);

	// The overlay, and the input component that opens it.
	TSharedPtr<class SVoxelPauseMenu> Overlay;

	UPROPERTY(Transient)
	TObjectPtr<UInputComponent> Input = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<APlayerController> Controller = nullptr;

	// Restored on close, because the survival hotbar and the F-key overlays all
	// have opinions about the cursor and this must not overwrite them.
	bool bSavedShowCursor = false;

	// --- Capture switches ---------------------------------------------------
	// -VoxelPauseShot: seconds spent in Playing before the overlay is opened on
	// its own, then settled and photographed. Mirrors -VoxelMenuShot exactly,
	// including the four-second delay between the shutter and the exit.
	bool bCaptureRequested = false;
	float PlayingSeconds = 0.f;
	float CaptureQuitAtSeconds = 0.f;
	bool bShotOpened = false;
};

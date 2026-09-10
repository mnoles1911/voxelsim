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

	// The map's marks, which the codex's PLACES list also reads.
	//
	// NO LONGER SESSION-SCOPED (2026-09-08). They are read from
	// Saved/VoxelWorlds/<seed>.vxmarks.json the first time a screen opens and
	// written back on every change -- see VoxelMapMarks.h for why that file and
	// not the checkpoint system.
	const TArray<FVoxelMapMark>& GetMarks() const { return Marks; }

	// THE HUD'S ONE VISIBILITY AUTHORITY (2026-09-08 backlog: "HUD stays lit
	// behind overlay panels"). It does not toggle -- it re-reads
	// OverlayOwnsInput() and makes the HUD agree with it, which is what makes
	// nested and interleaved overlays safe: a pause menu opened over an open
	// inventory and then closed must NOT put the compass back while the
	// inventory is still up, and a toggle would.
	//
	// PUBLIC BECAUSE THE PAUSE MENU IS A DIFFERENT SUBSYSTEM. UVoxelPauseUISubsystem
	// owns z-order 120 and already hides the older survival hotbar for exactly
	// this reason; it calls this for the newer HUD. Cheap enough to call every
	// tick: it early-outs unless the answer changed.
	void RefreshHudForOverlays();

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

	// The music transport's three keys. Same nullary shape and same reason as
	// the five tab thunks above; each drives FVoxelUIMusic directly, which is
	// also what the HUD cluster's three buttons do, so there is exactly one
	// implementation of "next track" in the project.
	void MusicPrevious();
	void MusicTogglePause();
	void MusicNext();

	// Starts the soundtrack once, on the first tick that has a player in a
	// world, if the player has left music-in-game on. A no-op when the front end
	// already carried a track across the hand-off. See the .cpp.
	void EnsureMusicInGame();

	// --- Hold-to-point (owner, live, 2026-09-08) ----------------------------
	// "holding tab should cause the mouse cursor to pop up on screen and be able
	// to be controlled. when holding tab and moving the mouse, player can use
	// the cursor to click anything on screen."
	//
	// Bound nullary on the same input component as the transport keys. Begin is
	// Tab pressed; End is Tab released AND the three ways a hold ends without a
	// key-up (see TickPointMode).
	void BeginPointMode();
	void EndPointMode();
	void TickPointMode();
	// The world belongs to the player, the run is attended, and nothing else has
	// the cursor. Also the predicate TickPointMode re-tests every frame of a hold.
	bool CanEnterPointMode() const;
	// A screen, a death card, a dialogue or the PAUSE MENU (a different
	// subsystem) currently owns the cursor and the input mode.
	bool OverlayOwnsInput() const;

	bool CanShow() const;
	void EnsureInput(APlayerController* PC);
	void InstallHud(APlayerController* PC);
	void RemoveHud();

	// Builds the body for a tab and swaps it into the shell.
	void ShowTab(EVoxelScreenTab Tab);
	// Reads the world's marks file once per world. Called from ShowTab rather
	// than from GatherMapData because the gather is const and because the codex
	// reads the same array -- one load, before either body is built.
	void EnsureMarksLoaded();
	// The map screen's write-back. Takes the whole new list (see
	// FOnVoxelMapMarksChanged) and writes it through immediately: the
	// alternative is a list that survives a clean quit and not a crash.
	void HandleMapMarksChanged(const TArray<FVoxelMapMark>& NewMarks);
	// The player's position and yaw, re-read every frame by the map's live
	// marker. Cheap on purpose -- a pawn transform and a view rotation, and
	// none of the subsystem/filesystem work GatherMapData does.
	FVoxelMapPose GetLiveMapPose() const;
	// Everything the screens read out of the world, gathered in one place so
	// the five bodies never reach into a subsystem themselves.
	FVoxelMapScreenData GatherMapData() const;
	FVoxelInventoryScreenData GatherInventoryData() const;
	FVoxelHudData GatherHudData() const;
	FText DayStamp() const;

	// NO bKeepMusicCluster PARAMETER ANY MORE (2026-09-08). It used to hold the
	// music transport on screen under the five in-game screens; RefreshHudForOverlays
	// now takes the whole HUD down for every overlay kind, so there is nothing
	// left to vary per call site. See that function for why the 2026-09-07
	// decision it encoded no longer applies.
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
	// Once per world, not once per screen open. A world reopen builds a fresh
	// subsystem, so this is also once per world session.
	bool bMarksLoaded = false;
	bool bSavedShowCursor = false;
	// What RefreshHudForOverlays last applied. Held so the per-tick call is a
	// comparison rather than two SetVisibility calls a frame -- SWidget::SetVisibility
	// invalidates layout whether or not the value changed.
	bool bHudHiddenForOverlay = false;

	// EnsureMusicInGame runs exactly once per world. Not per session: a map
	// reopen (which is how EXIT TO MENU and LOAD work here -- see
	// UVoxelPauseUISubsystem) builds a fresh subsystem, and the new world's
	// front end owns the decision again from the top.
	bool bMusicChecked = false;

	// --- Hold-to-point state ------------------------------------------------
	bool bPointMode = false;
	// What bShowMouseCursor was before the hold. Restored on release, and only
	// when nothing else has claimed the cursor in the meantime.
	bool bPointSavedShowCursor = false;

	// --- Capture switches ---------------------------------------------------
	// -VoxelScreenShot / -VoxelDeathShot / -VoxelDialogueShot / -VoxelHudShot,
	// all on the -VoxelPauseShot pattern: play for N seconds, open the thing,
	// settle another N so its glyphs rasterise, photograph with the UI on, quit.
	bool bCaptureRequested = false;
	float PlayingSeconds = 0.f;
	float CaptureQuitAtSeconds = 0.f;
	bool bShotOpened = false;
};

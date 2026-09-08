#include "VoxelScreensUISubsystem.h"

#include "SVoxelCodexScreen.h"
#include "SVoxelDeathScreen.h"
#include "SVoxelDialogueOverlay.h"
#include "SVoxelGameHud.h"
#include "SVoxelInventoryScreen.h"
#include "SVoxelJournalScreen.h"
#include "SVoxelMapScreen.h"
#include "SVoxelPlayerScreen.h"
#include "SVoxelScreenShell.h"
#include "VoxelAudioUserSettings.h"
#include "VoxelEarthUI.h"
#include "VoxelFrontEndSubsystem.h"
#include "VoxelFrontEndSwitches.h"
#include "VoxelPauseUISubsystem.h"
#include "VoxelSurvivalUISubsystem.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelMapMarks.h"
#include "VoxelUIMusic.h"
#include "VoxelUIStrings.h"

#include "VoxelCoords.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEphemeris.h"
#include "VoxelInventoryComponent.h"
#include "VoxelItem.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"

#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h" // EKeys, for the music transport
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h" // FApp::IsUnattended
#include "UnrealClient.h" // FScreenshotRequest
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"

namespace VoxelScreensUIDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Above the survival panel (30) and level with the pause overlay's neighbours,
// but BELOW pause itself (120): opening the pause menu over an inventory screen
// has to cover it, not sit under it.
constexpr int32 kScreenZOrder = 110;
// 18 -> 111 (COORDINATOR DECISION, 2026-09-07), and the reason is a click path,
// not a picture.
//
// The owner's directive is "clicking pause button stops the music", and the
// cursor only ever exists while an overlay owns the screen -- so the HUD's music
// transport has to be reachable through an open screen or the directive is
// unmet. The obvious fix was to stop the screen shell eating clicks in the empty
// area around its panel. THAT WAS TRIED AND REJECTED: SVoxelScreenShell
// SupportsKeyboardFocus and handles Escape in OnKeyDown, and Slate focuses the
// first focusable widget in the clicked path -- so a shell that stops taking
// those clicks stops holding focus, and a player who clicks the background of an
// open inventory would find Escape no longer closes it. The shell must keep
// eating empty-area clicks.
//
// Moving the HUD instead costs nothing, because the body is COLLAPSED for
// exactly the states this now sits above (see ApplyOverlayInput). Above the
// screen stack (110), below pause (120): the transport is clickable over an open
// screen, and the pause overlay still covers everything, scrim included.
//
// It is also still above the survival hotbar's old 15, which is what the
// original 18 was for and which SetHiddenForOverlay makes moot anyway.
constexpr int32 kHudZOrder = 111;
// Matches UVoxelPauseUISubsystem: FScreenshotRequest only QUEUES, and the frame
// that services it has to be rendered and written before the process may leave.
constexpr float kCaptureQuitDelaySeconds = 4.0f;

// voxel.UI.Screens -- the whole wave-2 layer, on one switch. Off restores the
// pre-port behaviour exactly: UVoxelSurvivalUISubsystem keeps the I key and its
// own hotbar, and nothing here binds or draws anything.
int32 GScreensEnabled = 1;
FAutoConsoleVariableRef CVarScreens(TEXT("voxel.UI.Screens"),
                                    GScreensEnabled,
                                    TEXT("1 (default) enables the 2026-09-07 in-game screens and HUD; ")
                                    TEXT("0 restores the earlier survival panel and its hotbar."),
                                    ECVF_Default);

EVoxelScreenTab TabFromSwitch(const FString& Name)
{
	if (Name == TEXT("map"))     return EVoxelScreenTab::Map;
	if (Name == TEXT("journal")) return EVoxelScreenTab::Journal;
	if (Name == TEXT("player"))  return EVoxelScreenTab::Player;
	if (Name == TEXT("codex"))   return EVoxelScreenTab::Codex;
	return EVoxelScreenTab::Inventory;
}

// The whole-day index, on UVoxelPauseUISubsystem::CurrentDayNumber's own terms:
// zero means "this session cannot name a day", which every caller reads as
// "omit the stamp" rather than as day zero.
int32 CurrentDayNumber(UWorld* World)
{
	const UVoxelSkySubsystem* Sky = World ? World->GetSubsystem<UVoxelSkySubsystem>() : nullptr;
	if (Sky == nullptr)
	{
		return 0;
	}
	const double DayLength = VoxelSky::GetDayLengthSeconds();
	if (DayLength <= 0.0)
	{
		return 0;
	}
	return 1 + FMath::FloorToInt32(Sky->GetSkyState().EpochSeconds / DayLength);
}
} // namespace VoxelScreensUIDetail

bool UVoxelScreensUISubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
	return Type == EWorldType::Game || Type == EWorldType::PIE;
}

bool UVoxelScreensUISubsystem::IsTickable() const
{
	return !IsTemplate() && GetWorld() != nullptr && GetWorld()->GetNetMode() != NM_DedicatedServer;
}

TStatId UVoxelScreensUISubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelScreensUISubsystem, STATGROUP_Tickables);
}

void UVoxelScreensUISubsystem::Deinitialize()
{
	TeardownStack();
	RemoveHud();
	Super::Deinitialize();
}

bool UVoxelScreensUISubsystem::CanShow() const
{
	if (VoxelScreensUIDetail::GScreensEnabled == 0)
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (World == nullptr || !World->HasBegunPlay() || World->GetGameViewport() == nullptr)
	{
		return false;
	}
	// DEFER TO THE FRONT END, on UVoxelPauseUISubsystem::CanPause's reasoning:
	// Menu, ArmLoading, Loading and HandOff each own the viewport and their own
	// input mode. Inactive is included because that is the state under
	// -VoxelNoMenu and every unattended leg, where the player has the world
	// from frame one.
	const UVoxelFrontEndSubsystem* Front = World->GetSubsystem<UVoxelFrontEndSubsystem>();
	if (Front != nullptr && Front->GetState() != EVoxelFrontEndState::Playing
	    && Front->GetState() != EVoxelFrontEndState::Inactive)
	{
		return false;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	return PC != nullptr && PC->IsLocalController() && PC->GetPawn() != nullptr;
}

void UVoxelScreensUISubsystem::Tick(float DeltaTime)
{
	// BEFORE CanShow'S EARLY-OUT, and that placement is the whole safety of the
	// feature: point mode holds the player's cursor, input mode and look-input
	// token, and it has to be able to GIVE THEM BACK in exactly the frames where
	// the rest of this subsystem stands down -- a pawn that died, a controller
	// that swapped, a window that lost focus mid-hold.
	TickPointMode();

	if (!CanShow())
	{
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	if (PC != Controller)
	{
		// A new controller means the old input component is bound to a dead
		// object -- the same handling UVoxelPauseUISubsystem does.
		TeardownStack();
		RemoveHud();
		Input = nullptr;
		Controller = PC;
	}
	EnsureInput(PC);
	InstallHud(PC);
	EnsureMusicInGame();

	// --- the four capture switches -------------------------------------------
	PlayingSeconds += DeltaTime;
	if (bCaptureRequested)
	{
		if (PlayingSeconds >= CaptureQuitAtSeconds)
		{
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	const bool bAnyShot = Switches.bScreenShot || Switches.bDeathShot
	                   || Switches.bDialogueShot || Switches.bHudShot;
	if (!bAnyShot)
	{
		return;
	}
	const float Settle = Switches.bScreenShot   ? Switches.ScreenShotSeconds
	                   : Switches.bDeathShot    ? Switches.DeathShotSeconds
	                   : Switches.bDialogueShot ? Switches.DialogueShotSeconds
	                                            : Switches.HudShotSeconds;

	if (!bShotOpened && PlayingSeconds >= Settle)
	{
		bShotOpened = true;
		if (Switches.bScreenShot)
		{
			OpenScreen(VoxelScreensUIDetail::TabFromSwitch(Switches.ScreenPanel));
		}
		else if (Switches.bDeathShot)
		{
			OpenDeathScreen();
		}
		else if (Switches.bDialogueShot)
		{
			OpenDialogue(VoxelScreenData::SeedDialogue());
		}
		// -VoxelHudShot opens nothing: the HUD is already installed, and what
		// it is there to photograph is the world with only the HUD on it.

		// A second settle after the widget is up, for the reason
		// -VoxelPauseShot settles twice: glyphs rasterise lazily and frame one
		// of a panel is a half-built panel.
		CaptureQuitAtSeconds = PlayingSeconds + Settle;
		return;
	}
	if (bShotOpened && PlayingSeconds >= CaptureQuitAtSeconds)
	{
		const TCHAR* Name = Switches.bScreenShot   ? TEXT("VoxelScreen")
		                  : Switches.bDeathShot    ? TEXT("VoxelDeath")
		                  : Switches.bDialogueShot ? TEXT("VoxelDialogue")
		                                           : TEXT("VoxelHud");
		FScreenshotRequest::RequestScreenshot(Name, /*bShowUI=*/true, /*bAddFilenameSuffix=*/true);
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreens: screenshot '%s' requested at t=%.2fs."),
		       Name, PlayingSeconds);
		bCaptureRequested = true;
		CaptureQuitAtSeconds = PlayingSeconds + VoxelScreensUIDetail::kCaptureQuitDelaySeconds;
	}
}

void UVoxelScreensUISubsystem::EnsureInput(APlayerController* PC)
{
	if (Input != nullptr || PC == nullptr)
	{
		return;
	}
	Input = NewObject<UInputComponent>(PC, TEXT("VoxelScreensUIInput"));
	// Between the survival UI's 20 and pause's 30: this takes the I key from
	// the earlier inventory panel and still yields Escape to pause.
	Input->Priority = 25;
	Input->RegisterComponent();

	Input->BindKey(SVoxelScreenShell::TabKey(EVoxelScreenTab::Map), IE_Pressed,
	               this, &UVoxelScreensUISubsystem::OpenMapTab);
	Input->BindKey(SVoxelScreenShell::TabKey(EVoxelScreenTab::Journal), IE_Pressed,
	               this, &UVoxelScreensUISubsystem::OpenJournalTab);
	Input->BindKey(SVoxelScreenShell::TabKey(EVoxelScreenTab::Inventory), IE_Pressed,
	               this, &UVoxelScreensUISubsystem::OpenInventoryTab);
	Input->BindKey(SVoxelScreenShell::TabKey(EVoxelScreenTab::Player), IE_Pressed,
	               this, &UVoxelScreensUISubsystem::OpenPlayerTab);
	Input->BindKey(SVoxelScreenShell::TabKey(EVoxelScreenTab::Codex), IE_Pressed,
	               this, &UVoxelScreensUISubsystem::OpenCodexTab);

	// --- The music transport ------------------------------------------------
	//
	// THE HOTKEYS ARE THE PRIMARY PATH, NOT A CONVENIENCE. The HUD cluster is
	// only clickable while a cursor is up, and the cursor is only up while an
	// overlay owns the screen -- so for a player who is actually playing, the
	// keys are the whole feature. They are bound on THIS component, which is
	// pushed on the player controller and therefore live under
	// FInputModeGameOnly, which is what "works while the mouse is captured"
	// means here.
	//
	// THE ENGINE HAS NO MEDIA KEYS. A grep of InputCoreTypes.h on 2026-09-07
	// found no Media_Play_Pause, Media_Next_Track or Media_Prev_Track and no
	// Volume keys of any kind, so binding the keyboard's own transport row is
	// not available without a new FKey registration. That is a real option later
	// and it is not this change.
	//
	// WHY , . / AND NOT THE BRACKETS. The brackets were the obvious media-ish
	// pair and they are TAKEN: AVoxelEarthFlyPawn binds LeftBracket and
	// RightBracket to the fly-speed dial. A grep of EKeys:: across both modules
	// gives the full occupied set -- A C D E F G I J K M P Q S T V W X, the
	// digits 1-9, the arrows, Enter, Escape, Space, Shift, Ctrl, Alt, F1, F3,
	// both brackets, both mouse buttons and the wheel -- and , . / are free in
	// all of it. They also sit adjacent on the keyboard in the same left-to-
	// right order as the three buttons on screen, which is the strongest
	// affordance a three-key transport can have.
	//
	// NUMPAD 4/5/6 ARE THE SAME THREE, in the same left-to-right order, for
	// anyone whose hand is over there. Also free.
	Input->BindKey(EKeys::Comma, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicPrevious);
	Input->BindKey(EKeys::Period, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicTogglePause);
	Input->BindKey(EKeys::Slash, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicNext);
	Input->BindKey(EKeys::NumPadFour, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicPrevious);
	Input->BindKey(EKeys::NumPadFive, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicTogglePause);
	Input->BindKey(EKeys::NumPadSix, IE_Pressed, this, &UVoxelScreensUISubsystem::MusicNext);

	// --- Hold-to-point ------------------------------------------------------
	//
	// OWNER, LIVE, 2026-09-08: "holding tab should cause the mouse cursor to pop
	// up on screen and be able to be controlled. when holding tab and moving the
	// mouse, player can use the cursor to click anything on screen."
	//
	// TAB WAS FREE. A grep of EKeys:: across both modules and of Config/
	// DefaultInput.ini (which declares no ActionMappings at all -- every binding
	// in this project is code) found no Tab anywhere, so nothing had to be moved
	// to honour the owner's word.
	//
	// PRESSED AND RELEASED ON THE SAME COMPONENT the transport keys use, so the
	// hold is live under FInputModeGameOnly. The release is belt-and-braces only:
	// UVoxelScreensUISubsystem::TickPointMode also ends the mode when the key is
	// no longer down, because point mode switches to GameAndUI and a Slate widget
	// that takes focus can eat the key-up that would otherwise end it.
	Input->BindKey(EKeys::Tab, IE_Pressed, this, &UVoxelScreensUISubsystem::BeginPointMode);
	Input->BindKey(EKeys::Tab, IE_Released, this, &UVoxelScreensUISubsystem::EndPointMode);

	PC->PushInputComponent(Input);
	UE_LOG(LogVoxelUI, Log,
	       TEXT("VoxelScreens: M/J/I/P/K bound; music transport on , . / and numpad 4/5/6; Tab holds point mode."));
}

// UInputComponent::BindKey's delegate signature takes no payload, so each key
// needs its own nullary target -- the same shape the five tab thunks above have,
// and for the same reason.
void UVoxelScreensUISubsystem::MusicPrevious()
{
	FVoxelUIMusic::Get().Previous();
}

void UVoxelScreensUISubsystem::MusicTogglePause()
{
	FVoxelUIMusic::Get().TogglePause();
}

void UVoxelScreensUISubsystem::MusicNext()
{
	FVoxelUIMusic::Get().Next();
}

bool UVoxelScreensUISubsystem::OverlayOwnsInput() const
{
	if (Shell.IsValid() || DeathScreen.IsValid() || Dialogue.IsValid())
	{
		return true;
	}
	// THE PAUSE MENU IS A DIFFERENT SUBSYSTEM AND IT MUST STILL BE ASKED. It
	// takes the cursor and the input mode exactly as the screens here do, and
	// point mode handing them back underneath it would leave a pause menu the
	// player cannot point at.
	UWorld* World = GetWorld();
	const UVoxelPauseUISubsystem* Pause = World ? World->GetSubsystem<UVoxelPauseUISubsystem>() : nullptr;
	return Pause != nullptr && Pause->IsPaused();
}

bool UVoxelScreensUISubsystem::CanEnterPointMode() const
{
	// NOT ON AN UNATTENDED RUN. Every leg and every capture switch passes
	// -unattended and drives no mouse; a cursor popping up mid-capture would be
	// in the picture.
	if (FApp::IsUnattended())
	{
		return false;
	}
	// The world has to belong to the player -- CanShow is the same gate the HUD
	// and the screens use, and it is what keeps this out of the menu, the
	// loading screen and the hand-off.
	if (!CanShow())
	{
		return false;
	}
	// A screen, a death card, a dialogue or the pause menu already own the
	// cursor. Point mode is the state for when nothing else does.
	return !OverlayOwnsInput();
}

void UVoxelScreensUISubsystem::BeginPointMode()
{
	if (bPointMode || !CanEnterPointMode())
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC == nullptr)
	{
		return;
	}

	bPointMode = true;
	bPointSavedShowCursor = PC->bShowMouseCursor;

	// LOOK ONLY, NOT MOVE -- the one place this deliberately differs from
	// ApplyOverlayInput, which stops both. A player reading a panel is not
	// walking; a player holding Tab still is, and the owner asked for "holding
	// tab and moving the mouse", not for standing still. Suppressing look is
	// what stops the camera swinging away under a cursor the player is trying to
	// aim: AVoxelEarthFlyPawn::Turn and LookUp go through AddControllerYawInput
	// and AddControllerPitchInput, which honour APlayerController::IgnoreLookInput.
	PC->SetIgnoreLookInput(true);
	PC->bShowMouseCursor = true;

	// GameAndUI, NOT UIOnly: movement keys have to keep reaching the pawn.
	FInputModeGameAndUI InputMode;
	// The pointer must not walk out of the window during a hold -- on a second
	// monitor that would leave the player clicking their desktop.
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
	// FALSE, AND THIS IS THE ONE THAT DECIDES WHETHER THE FEATURE WORKS AT ALL.
	// The flag defaults to TRUE, which hides the cursor for as long as the mouse
	// is captured -- so the first click on a HUD button would make the cursor
	// vanish under the player's hand, which is the exact opposite of what was
	// asked for.
	InputMode.SetHideCursorDuringCapture(false);
	PC->SetInputMode(InputMode);

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelInput: point mode ON (Tab held)."));
}

void UVoxelScreensUISubsystem::EndPointMode()
{
	if (!bPointMode)
	{
		return;
	}
	bPointMode = false;

	UWorld* World = GetWorld();
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		// ALWAYS RELEASED. IgnoreLookInput is a COUNTER, not a flag, so every
		// SetIgnoreLookInput(true) owes exactly one (false) -- and an overlay
		// that opened during the hold has pushed its own. Skipping this because
		// something else now owns the cursor would leave the camera dead for the
		// rest of the session, one token short forever.
		PC->SetIgnoreLookInput(false);

		// THE CURSOR AND THE INPUT MODE ARE ONLY OURS TO PUT BACK IF NOTHING ELSE
		// HAS TAKEN THEM. The case this exists for: Escape opens the pause menu
		// mid-hold, that subsystem sets its own cursor and UIOnly mode, and this
		// runs a frame later. Restoring here would hide the cursor and hand input
		// back to the game underneath an open pause menu.
		if (!OverlayOwnsInput())
		{
			PC->bShowMouseCursor = bPointSavedShowCursor;
			PC->SetInputMode(FInputModeGameOnly());
			// AND THE FOCUS GOES BACK TO THE GAME (2026-09-08). The transport
			// buttons are IsFocusable(false) now, so nothing here should be
			// holding focus -- but "should" is what the last bug was made of, and
			// a Slate widget that keeps focus after the cursor has gone turns
			// every keystroke into a chance that some button eats it. Restoring
			// the game viewport's focus makes that impossible rather than
			// unlikely, and it costs one call on a key release.
			//
			// INSIDE THIS BRANCH, not beside it: when a pause menu or a screen
			// opened during the hold it owns the focus as well as the cursor, and
			// yanking it to the viewport would leave that menu unable to hear
			// Escape.
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetAllUserFocusToGameViewport();
			}
		}
	}

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelInput: point mode OFF."));
}

void UVoxelScreensUISubsystem::TickPointMode()
{
	if (!bPointMode)
	{
		return;
	}
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;

	// THREE WAYS A HOLD ENDS WITHOUT A KEY-UP, and each of them has cost
	// somebody a session somewhere:
	//   * The key-up never arrives. Point mode runs under GameAndUI, and a Slate
	//     widget that takes focus on a click can consume it. IsInputKeyDown is
	//     the ground truth, so the binding is a fast path and this is the fact.
	//   * The window lost focus mid-hold -- alt-tab, a crash dialogue, the
	//     editor stealing focus. The player comes back to a cursor they cannot
	//     dismiss and a camera that will not turn.
	//   * Something else took the screen: a menu opened, the pawn died, the
	//     controller swapped.
	const bool bKeyStillDown = PC != nullptr && PC->IsInputKeyDown(EKeys::Tab);
	const bool bWindowActive = FSlateApplication::IsInitialized() && FSlateApplication::Get().IsActive();
	if (!bKeyStillDown || !bWindowActive || !CanEnterPointMode())
	{
		EndPointMode();
	}
}

void UVoxelScreensUISubsystem::EnsureMusicInGame()
{
	if (bMusicChecked)
	{
		return;
	}
	bMusicChecked = true;

	// THE PLAYER HAS THE WORLD. This is the one place that knows it for
	// certain -- this function is called once the HUD is installed, i.e. once
	// there is a pawn -- and it is what switches the music from the Menu pool
	// to the world's own (docs/music-design.md section 2). Set BEFORE the two
	// early returns below, because the context is true regardless of whether
	// this run is allowed to start a cue: an unattended leg and a player who
	// turned music off are both in the world, and FVoxelUIMusic::
	// PoolChangesAllowed is what keeps both of them silent.
	FVoxelUIMusic::Get().SetContext(EVoxelMusicContext::InWorld);

	// UNATTENDED RUNS GET NO MUSIC FROM HERE, AND THAT IS A MEASUREMENT
	// DECISION, NOT A TASTE ONE. Every headless leg and every capture switch
	// passes -unattended. Starting a track on those would read 30-92 MB off disk
	// and decode it during the first seconds of a leg -- the exact window a
	// streaming or frame-time leg is measuring -- and the status quo for a
	// -VoxelNoMenu leg is silence, because the front end never ran. Nobody is
	// listening to a leg. Note that this does NOT change the menu's music: a leg
	// that does open the menu still gets whatever EnterMenu starts, exactly as
	// before, so no existing capture changes.
	if (FApp::IsUnattended())
	{
		return;
	}

	if (!VoxelAudioUserSettings::GetMusicInGame())
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: music in game is off; not starting a track."));
		return;
	}
	// NORMALLY A NO-OP, AND THE PATH IT EXISTS FOR IS THE OTHER ONE. When the
	// front end ran, it has already carried its track across the hand-off and
	// StartRandom adopts rather than restarts. When it did NOT run -- every
	// -VoxelNoMenu leg, every unattended capture, and any future path that drops
	// the player straight into a world -- nothing else would ever start the
	// music, and "in game" would silently mean "only if you came through the
	// menu".
	if (UWorld* World = GetWorld())
	{
		FRandomStream MusicStream = MakeVoxelUIRandomStream();
		FVoxelUIMusic::Get().StartRandom(World, MusicStream);
	}
}

void UVoxelScreensUISubsystem::InstallHud(APlayerController* PC)
{
	if (Hud.IsValid() || PC == nullptr)
	{
		return;
	}
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		return;
	}

	Hud = SNew(SVoxelGameHud)
		.Data_UObject(this, &UVoxelScreensUISubsystem::GatherHudData);
	Viewport->AddViewportWidgetContent(Hud.ToSharedRef(), VoxelScreensUIDetail::kHudZOrder);

	// THE OLD HOTBAR IS HIDDEN FOR GOOD, not for the duration of an overlay.
	// SetHiddenForOverlay is the lever the pause menu already uses and the only
	// one that subsystem exposes; using it permanently is deliberate, because
	// two hotbars drawn at once is the alternative. voxel.UI.Screens 0 is what
	// gives the old one back.
	if (UVoxelSurvivalUISubsystem* Survival = World->GetSubsystem<UVoxelSurvivalUISubsystem>())
	{
		Survival->SetHiddenForOverlay(true);
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreens: HUD installed; the earlier survival hotbar is hidden."));
}

void UVoxelScreensUISubsystem::RemoveHud()
{
	if (!Hud.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(Hud.ToSharedRef());
	}
	Hud.Reset();
	if (UVoxelSurvivalUISubsystem* Survival = World ? World->GetSubsystem<UVoxelSurvivalUISubsystem>() : nullptr)
	{
		Survival->SetHiddenForOverlay(false);
	}
}

// A press on the OPEN screen's own key closes the stack, which is what makes I
// a toggle rather than a one-way door -- and matches the shell's own handling
// of the same five keys once it has focus.
void UVoxelScreensUISubsystem::OpenMapTab()
{
	if (Shell.IsValid() && ActiveTab == EVoxelScreenTab::Map) { CloseScreen(); return; }
	OpenScreen(EVoxelScreenTab::Map);
}

void UVoxelScreensUISubsystem::OpenJournalTab()
{
	if (Shell.IsValid() && ActiveTab == EVoxelScreenTab::Journal) { CloseScreen(); return; }
	OpenScreen(EVoxelScreenTab::Journal);
}

void UVoxelScreensUISubsystem::OpenInventoryTab()
{
	if (Shell.IsValid() && ActiveTab == EVoxelScreenTab::Inventory) { CloseScreen(); return; }
	OpenScreen(EVoxelScreenTab::Inventory);
}

void UVoxelScreensUISubsystem::OpenPlayerTab()
{
	if (Shell.IsValid() && ActiveTab == EVoxelScreenTab::Player) { CloseScreen(); return; }
	OpenScreen(EVoxelScreenTab::Player);
}

void UVoxelScreensUISubsystem::OpenCodexTab()
{
	if (Shell.IsValid() && ActiveTab == EVoxelScreenTab::Codex) { CloseScreen(); return; }
	OpenScreen(EVoxelScreenTab::Codex);
}

void UVoxelScreensUISubsystem::OpenScreen(EVoxelScreenTab Tab)
{
	if (!CanShow())
	{
		return;
	}
	// FIRST, while nothing else owns the cursor yet. ApplyOverlayInput saves
	// PC->bShowMouseCursor to put back on close, and point mode leaves that true
	// -- so a screen opened during a hold would "restore" a cursor the player
	// never asked for and leave it on the screen for the rest of the session.
	EndPointMode();
	if (Shell.IsValid())
	{
		// Already up: this is a tab switch, not a second open.
		ShowTab(Tab);
		return;
	}
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World->GetGameViewport();

	ActiveTab = Tab;
	Shell = SNew(SVoxelScreenShell)
		.ActiveTab(Tab)
		.ShrinkWrap(Tab == EVoxelScreenTab::Inventory)
		.OnTabChanged(FOnVoxelScreenTabChanged::CreateUObject(this, &UVoxelScreensUISubsystem::ShowTab))
		.OnClose(FSimpleDelegate::CreateUObject(this, &UVoxelScreensUISubsystem::CloseScreen));

	Viewport->AddViewportWidgetContent(Shell.ToSharedRef(), VoxelScreensUIDetail::kScreenZOrder);
	ShowTab(Tab);

	// PAUSED SIMULATION, LIVE PICTURE -- the same pairing the pause menu makes,
	// and for the same reason its comment gives.
	UGameplayStatics::SetGamePaused(World, true);
	ApplyOverlayInput(Shell.ToSharedRef(), /*bKeepMusicCluster=*/true);
	Shell->FocusDefaultWidget();
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreens: opened."));
}

void UVoxelScreensUISubsystem::ShowTab(EVoxelScreenTab Tab)
{
	if (!Shell.IsValid())
	{
		OpenScreen(Tab);
		return;
	}
	// THE SHELL IS REBUILT, NOT REPARENTED. Its width differs between the
	// inventory (which shrink-wraps its pack) and the other four, and its
	// action-bar hints are per-screen -- both of which are Construct-time
	// arguments. Rebuilding one compound widget on a tab press, while the game
	// is paused, is cheaper than making either of those an attribute.
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		return;
	}

	// BEFORE THE SWITCH, because TWO bodies read the same array: the map draws
	// the marks on the sheet and the codex lists them under PLACES. Loading in
	// either one would leave the other showing an empty list depending on which
	// tab the player opened first.
	EnsureMarksLoaded();

	TSharedRef<SWidget> Body = SNullWidget::NullWidget;
	TArray<FVoxelScreenAction> Actions;
	switch (Tab)
	{
	case EVoxelScreenTab::Map:
	{
		const FVoxelMapScreenData MapData = GatherMapData();
		Actions = SVoxelMapScreen::Actions(MapData.bHasTerrainRaster);
		Body = SNew(SVoxelMapScreen)
			.Data(MapData)
			// THE LIVE FEED. CreateUObject rather than a raw `this` capture:
			// the widget outlives nothing here in practice, but a Slate
			// attribute holding a bare subsystem pointer is a dangling read
			// waiting for the first world teardown that leaves a widget up.
			// The UObject form goes weak on its own.
			.LivePose(TAttribute<FVoxelMapPose>::Create(
				TAttribute<FVoxelMapPose>::FGetter::CreateUObject(
					this, &UVoxelScreensUISubsystem::GetLiveMapPose)))
			.OnMarksChanged(FOnVoxelMapMarksChanged::CreateUObject(
				this, &UVoxelScreensUISubsystem::HandleMapMarksChanged));
		break;
	}
	case EVoxelScreenTab::Journal:
		Actions = SVoxelJournalScreen::Actions();
		Body = SNew(SVoxelJournalScreen).Data(VoxelScreenData::SeedJournal(DayStamp()));
		break;
	case EVoxelScreenTab::Inventory:
		Actions = SVoxelInventoryScreen::Actions();
		Body = SNew(SVoxelInventoryScreen).Data(GatherInventoryData());
		break;
	case EVoxelScreenTab::Player:
		Actions = SVoxelPlayerScreen::Actions();
		Body = SNew(SVoxelPlayerScreen).Data(VoxelScreenData::SeedPlayer());
		break;
	case EVoxelScreenTab::Codex:
		Actions = SVoxelCodexScreen::Actions();
		Body = SNew(SVoxelCodexScreen).Data(VoxelScreenData::SeedCodex()).Places(Marks);
		break;
	}

	Viewport->RemoveViewportWidgetContent(Shell.ToSharedRef());
	ActiveTab = Tab;
	Shell = SNew(SVoxelScreenShell)
		.ActiveTab(Tab)
		.ShrinkWrap(Tab == EVoxelScreenTab::Inventory)
		.Actions(Actions)
		.OnTabChanged(FOnVoxelScreenTabChanged::CreateUObject(this, &UVoxelScreensUISubsystem::ShowTab))
		.OnClose(FSimpleDelegate::CreateUObject(this, &UVoxelScreensUISubsystem::CloseScreen))
		.Body()
		[
			Body
		];
	Viewport->AddViewportWidgetContent(Shell.ToSharedRef(), VoxelScreensUIDetail::kScreenZOrder);

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		FInputModeUIOnly InputMode;
		InputMode.SetWidgetToFocus(Shell);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(InputMode);
	}
	Shell->FocusDefaultWidget();
}

void UVoxelScreensUISubsystem::CloseScreen()
{
	if (!Shell.IsValid())
	{
		return;
	}
	TeardownStack();
	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::SetGamePaused(World, false);
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreens: closed."));
}

void UVoxelScreensUISubsystem::OpenDeathScreen()
{
	if (!CanShow() || DeathScreen.IsValid())
	{
		return;
	}
	// See OpenScreen: ended before anything else claims the cursor.
	EndPointMode();
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World->GetGameViewport();

	// SAID OUT LOUD, ONCE, because a reader finding this screen in a log will
	// otherwise go looking for the death system that opened it.
	UE_LOG(LogVoxelUI, Log,
	       TEXT("VoxelScreens: death screen opened. NOTE: this project has no death or respawn ")
	       TEXT("system -- the only caller is -VoxelDeathShot, and RESPAWN simply closes."));

	DeathScreen = SNew(SVoxelDeathScreen)
		.Data(VoxelScreenData::SeedDeath(DayStamp()))
		.OnRespawn(FSimpleDelegate::CreateUObject(this, &UVoxelScreensUISubsystem::CloseOverlay))
		.OnQuit(FSimpleDelegate::CreateLambda([this]()
		{
			if (UWorld* W = GetWorld())
			{
				UKismetSystemLibrary::QuitGame(W, W->GetFirstPlayerController(), EQuitPreference::Quit,
				                               /*bIgnorePlatformRestrictions=*/false);
			}
		}));

	Viewport->AddViewportWidgetContent(DeathScreen.ToSharedRef(), VoxelScreensUIDetail::kScreenZOrder);
	UGameplayStatics::SetGamePaused(World, true);
	ApplyOverlayInput(DeathScreen.ToSharedRef(), /*bKeepMusicCluster=*/false);
	DeathScreen->FocusDefaultWidget();
}

void UVoxelScreensUISubsystem::OpenDialogue(const FVoxelDialogueData& Node)
{
	if (!CanShow() || Dialogue.IsValid())
	{
		return;
	}
	// See OpenScreen: ended before anything else claims the cursor.
	EndPointMode();
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World->GetGameViewport();

	UE_LOG(LogVoxelUI, Log,
	       TEXT("VoxelScreens: dialogue overlay opened. NOTE: there is no conversation system and ")
	       TEXT("design/CONVERSATION_SYSTEM.md does not exist -- choosing a reply only closes."));

	Dialogue = SNew(SVoxelDialogueOverlay)
		.Data(Node)
		.OnChoose(FOnVoxelDialogueOptionChosen::CreateLambda([this](int32) { CloseOverlay(); }))
		.OnClose(FSimpleDelegate::CreateUObject(this, &UVoxelScreensUISubsystem::CloseOverlay));

	Viewport->AddViewportWidgetContent(Dialogue.ToSharedRef(), VoxelScreensUIDetail::kScreenZOrder);
	// THE WORLD IS NOT PAUSED FOR DIALOGUE. The mock's own note says the scene
	// stays live behind the overlay ("world stays visible but dimmed"), and the
	// dim alpha is set well below the pause menu's for that reason. Input is
	// still taken, so the player cannot dig while reading.
	ApplyOverlayInput(Dialogue.ToSharedRef(), /*bKeepMusicCluster=*/false);
	Dialogue->FocusDefaultWidget();
}

void UVoxelScreensUISubsystem::CloseOverlay()
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	bool bWasPaused = false;

	if (DeathScreen.IsValid())
	{
		if (Viewport) { Viewport->RemoveViewportWidgetContent(DeathScreen.ToSharedRef()); }
		DeathScreen.Reset();
		bWasPaused = true;
	}
	if (Dialogue.IsValid())
	{
		if (Viewport) { Viewport->RemoveViewportWidgetContent(Dialogue.ToSharedRef()); }
		Dialogue.Reset();
	}
	RestoreGameInput();
	if (bWasPaused && World)
	{
		UGameplayStatics::SetGamePaused(World, false);
	}
}

void UVoxelScreensUISubsystem::ApplyOverlayInput(TSharedRef<SWidget> Widget, bool bKeepMusicCluster)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC == nullptr)
	{
		return;
	}
	// The same input-mode discipline UVoxelPauseUISubsystem::OpenPause applies,
	// for the reason it gives: AVoxelEarthPlayerController binds raw keys with
	// no notion of UI focus, so under GameAndUI a player reading a screen would
	// still be digging.
	bSavedShowCursor = PC->bShowMouseCursor;
	if (PC->PlayerInput != nullptr)
	{
		// Otherwise the key that opened the screen is still down on resume.
		PC->PlayerInput->FlushPressedKeys();
	}
	PC->SetIgnoreMoveInput(true);
	PC->SetIgnoreLookInput(true);
	PC->bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(Widget);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(InputMode);
	if (PC->MyHUD != nullptr)
	{
		PC->MyHUD->bShowHUD = false;
	}
	if (Hud.IsValid())
	{
		// COORDINATOR DECISION, 2026-09-07: the BODY only, for the five in-game
		// screens. The compass, the dock and the prompt would otherwise sit under
		// the panel, dimmed but legible, which is not what any of the mocks show
		// -- but the music transport in the corner has to STAY, because this is
		// the only state in which a cursor exists to click it with.
		Hud->SetBodyVisible(false);
		// THE DEATH SCREEN AND THE DIALOGUE OVERLAY ARE THE EXCEPTION, and it is
		// a composition judgement rather than a mechanical one. Both are
		// full-bleed dramatic screens whose mocks show nothing in that corner,
		// and a transport drawn over a death card reads as a UI bug. They keep
		// the old behaviour: the whole HUD goes.
		Hud->SetMusicClusterVisible(bKeepMusicCluster);
	}
}

void UVoxelScreensUISubsystem::RestoreGameInput()
{
	UWorld* World = GetWorld();
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		PC->SetIgnoreMoveInput(false);
		PC->SetIgnoreLookInput(false);
		PC->bShowMouseCursor = bSavedShowCursor;
		PC->SetInputMode(FInputModeGameOnly());
		if (PC->MyHUD != nullptr)
		{
			PC->MyHUD->bShowHUD = true;
		}
	}
	if (Hud.IsValid())
	{
		// Body back, and NOT via SetVisibility on the widget: that would put the
		// root to HitTestInvisible and silently undo the SelfHitTestInvisible the
		// music cluster needs to be hittable at all.
		Hud->SetBodyVisible(true);
		// Unconditional, because this is the ONE restore path for all three
		// overlay kinds: a death screen closed after a dialogue must not leave
		// the transport switched off for the rest of the session.
		Hud->SetMusicClusterVisible(true);
	}
}

void UVoxelScreensUISubsystem::TeardownStack()
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Shell.IsValid())
	{
		if (Viewport) { Viewport->RemoveViewportWidgetContent(Shell.ToSharedRef()); }
		Shell.Reset();
	}
	if (DeathScreen.IsValid())
	{
		if (Viewport) { Viewport->RemoveViewportWidgetContent(DeathScreen.ToSharedRef()); }
		DeathScreen.Reset();
	}
	if (Dialogue.IsValid())
	{
		if (Viewport) { Viewport->RemoveViewportWidgetContent(Dialogue.ToSharedRef()); }
		Dialogue.Reset();
	}
	RestoreGameInput();
}

FText UVoxelScreensUISubsystem::DayStamp() const
{
	const int32 Day = VoxelScreensUIDetail::CurrentDayNumber(GetWorld());
	if (Day <= 0)
	{
		return FText::GetEmpty();
	}
	// DAY ONLY, NO SEASON. VoxelUIStrings.h:108-118 records why: the HUD's
	// SeasonName helper assumes a 365-day year while voxel.Sky.DaysPerYear
	// defaults to 48, so it prints a confidently wrong season. The pause
	// footer made the same choice for the same reason.
	return VoxelUIStrings::PauseFooter(Day);
}

FVoxelMapScreenData UVoxelScreensUISubsystem::GatherMapData() const
{
	FVoxelMapScreenData Out;
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return Out;
	}

	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			Out.PlayerWorld = Pawn->GetActorLocation();
		}
		FVector Eye;
		FRotator Aim;
		PC->GetPlayerViewPoint(Eye, Aim);
		Out.HeadingDeg = Aim.Yaw;
	}

	if (const UVoxelSkySubsystem* Sky = World->GetSubsystem<UVoxelSkySubsystem>())
	{
		// ALREADY COMPUTED EVERY FRAME, from the player's own position -- see
		// VoxelSkySubsystem.cpp's GeoFromWorldUU call. Nothing here re-derives
		// it.
		Out.LatitudeDeg = Sky->GetSkyState().LatitudeDeg;
		Out.LongitudeDeg = Sky->GetSkyState().LongitudeDeg;
	}
	if (const UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>())
	{
		Out.Seed = Terrain->GetSeed();
		Out.SurfaceHeightUU = Terrain->GetSurfaceHeightUU(Out.PlayerWorld.X, Out.PlayerWorld.Y);
	}

	const VoxelCoords::FVoxelCoord Voxel = VoxelCoords::WorldToVoxel(Out.PlayerWorld);
	Out.VoxelCoord = FIntVector(int32(Voxel.X), int32(Voxel.Y), int32(Voxel.Z));
	const VoxelCoords::FVoxelChunkKey Chunk = VoxelCoords::ChunkKeyForVoxel(Voxel);
	Out.ChunkKey = FIntVector(Chunk.X, Chunk.Y, Chunk.Z);
	Out.DayNumber = VoxelScreensUIDetail::CurrentDayNumber(World);
	Out.Marks = Marks;

	// The raster is offered only when it can be attributed to THIS world --
	// which means the provider, not the seed. See SVoxelMapScreen's header: two
	// directories under world-maps/ carry seed 20260719 and they are different
	// planets, so the old seed-only rule was drawing the wrong one. An empty
	// path is the ordinary answer for a world with no map.
	//
	// RESOLVED ONCE, HERE. It reads the tile directory out of the ini and stats
	// a file; the sheet then asks the asset library for the brush every paint,
	// which is a map lookup after the first success.
	Out.RasterPath = SVoxelMapScreen::RasterPathForSeed(Out.Seed);
	Out.bHasTerrainRaster =
		!Out.RasterPath.IsEmpty()
		&& (FVoxelUIAssetLibrary::Get().RequestImageFile(Out.RasterPath) != nullptr
		    || IFileManager::Get().FileExists(*Out.RasterPath));
	// The extent the view pans and zooms inside, whether or not there is a
	// picture to fill it.
	SVoxelMapScreen::ResolveExtentUU(Out.ExtentMinUU, Out.ExtentMaxUU);
	return Out;
}

FVoxelMapPose UVoxelScreensUISubsystem::GetLiveMapPose() const
{
	FVoxelMapPose Out;
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC == nullptr)
	{
		return Out;
	}
	if (const APawn* Pawn = PC->GetPawn())
	{
		const FVector Location = Pawn->GetActorLocation();
		Out.WorldXY = FVector2D(Location.X, Location.Y);
	}
	// THE VIEW YAW, NOT THE PAWN'S. The marker is a heading indicator and the
	// heading a player means is where they are looking -- which is also what
	// the readout's HEADING row already prints, so the arrow and the number
	// cannot disagree.
	FVector Eye;
	FRotator Aim;
	PC->GetPlayerViewPoint(Eye, Aim);
	Out.YawDeg = float(Aim.Yaw);
	return Out;
}

void UVoxelScreensUISubsystem::EnsureMarksLoaded()
{
	if (bMarksLoaded)
	{
		return;
	}
	// SET BEFORE THE READ, not after. A world with no seed subsystem yet, or a
	// file that will not parse, must not make this retry on every tab switch --
	// it would re-log the same warning forever and read a file the player is
	// about to overwrite anyway.
	bMarksLoaded = true;
	const UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (Terrain == nullptr)
	{
		return;
	}
	Marks = VoxelMapMarks::Load(Terrain->GetSeed());
}

void UVoxelScreensUISubsystem::HandleMapMarksChanged(const TArray<FVoxelMapMark>& NewMarks)
{
	Marks = NewMarks;
	const UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (Terrain == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("VoxelMap: no world subsystem, so %d mark(s) cannot be keyed to a world and are ")
		       TEXT("session-only."),
		       Marks.Num());
		return;
	}
	// WRITE-THROUGH ON EVERY CHANGE. The log line is the confirmation that the
	// file moved; a silent success here is exactly the failure this project
	// keeps paying for.
	VoxelMapMarks::Save(Terrain->GetSeed(), Marks);
}

FVoxelInventoryScreenData UVoxelScreensUISubsystem::GatherInventoryData() const
{
	FVoxelInventoryScreenData Out;
	UWorld* World = GetWorld();
	UVoxelInventoryComponent* Inventory =
		World ? UVoxelInventoryComponent::FindForLocalPlayer(World) : nullptr;
	if (Inventory == nullptr)
	{
		return Out;
	}

	Out.SelectedHotbarSlot = Inventory->GetSelectedSlot();
	const int32 SlotCount = Inventory->NumSlots();
	for (int32 I = 0; I < SlotCount; ++I)
	{
		const FVoxelInventorySlot Slot = Inventory->GetSlot(I);
		if (Slot.IsEmpty())
		{
			Out.Hotbar.Add(FVoxelInventoryScreenItem());
			continue;
		}
		FVoxelInventoryScreenItem Item;
		Item.ItemId = Slot.ItemId;
		Item.Count = Slot.Count;
		Item.Glyph = VoxelScreenData::GlyphForItem(Slot.ItemId);
		if (const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Slot.ItemId))
		{
			Item.DisplayName = FText::FromString(Def->DisplayName);
			Out.CarriedKg += Def->MassKg * float(Slot.Count);
			// The only two filter chips a real item can land in: everything in
			// the registry is a Block, a Throwable or a Misc, and none of the
			// mock's other five categories has anything that maps to it.
			const TArray<FText>& Filters = VoxelUIStrings::InvFilterNames();
			Item.Category = Def->Category == EVoxelItemCategory::Tool
				? Filters[3]   // Tools
				: Filters[5];  // Materials
		}
		else
		{
			// A save from a newer build, or a console-typed id. Drawn rather
			// than dropped, which is what FVoxelItemRegistry::Find's own
			// contract asks of callers.
			Item.DisplayName = FText::FromName(Slot.ItemId);
		}
		Out.Hotbar.Add(Item);
	}

	// THE PACK IS THE SAME ARRAY AS THE HOTBAR. UVoxelInventoryComponent has ONE
	// flat slot list and no notion of a backpack behind a quick bar, so the
	// mock's 8x8 pack above an 8-slot hotbar is one inventory shown twice. That
	// is honest -- both views are live and agree -- and it is what the screen
	// will keep doing until the component grows a second tier.
	Out.Pack = Out.Hotbar;
	Out.bHasEquipmentSystem = false;
	return Out;
}

FVoxelHudData UVoxelScreensUISubsystem::GatherHudData() const
{
	FVoxelHudData Out;
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return Out;
	}
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		FVector Eye;
		FRotator Aim;
		PC->GetPlayerViewPoint(Eye, Aim);
		Out.HeadingDeg = Aim.Yaw;
		// The music cluster's hit-testability, and the only input state this
		// struct carries. See FVoxelHudData::bCursorVisible.
		Out.bCursorVisible = PC->bShowMouseCursor;
	}
	const FVoxelInventoryScreenData Inventory = GatherInventoryData();
	Out.Hotbar = Inventory.Hotbar;
	Out.SelectedSlot = Inventory.SelectedHotbarSlot;

	// bHasVitals is FALSE IN ORDINARY PLAY and there is no code path that sets
	// it from gameplay, because there is no health or hunger to set it from --
	// see SVoxelGameHud's header. -VoxelDemoVitals is the capture-only arm, on
	// -VoxelDemoSaves' own pattern: it fabricates what no system supplies so
	// the layout can be reviewed, and nothing in play can turn it on.
	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	if (Switches.bDemoVitals)
	{
		Out.bHasVitals = true;
		Out.HealthFraction = Switches.DemoHealth / 100.f;
		Out.HungerFraction = Switches.DemoHunger / 100.f;
		Out.WoundFraction = Switches.DemoWound / 100.f;
		// The interaction prompt has no system either, and the HUD mock shows
		// one; it rides the same switch rather than a second one, because both
		// are the same question -- "draw the parts of this screen nothing can
		// fill yet".
		Out.InteractPrompt = VoxelUIStrings::HudDemoInteract();
		Out.InteractKey = VoxelUIStrings::HudDemoInteractKey();
	}
	return Out;
}

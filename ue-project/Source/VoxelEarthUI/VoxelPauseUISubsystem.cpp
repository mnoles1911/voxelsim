#include "VoxelPauseUISubsystem.h"

#include "SVoxelPauseMenu.h"
#include "VoxelEarthUI.h"
#include "VoxelFrontEndSubsystem.h"
#include "VoxelFrontEndSwitches.h"
#include "VoxelSaveRows.h"
#include "VoxelScreensUISubsystem.h"
#include "VoxelSurvivalUISubsystem.h"
#include "VoxelUIStrings.h"

#include "VoxelFrontEndPolicy.h"
#include "VoxelSaveLibrary.h"
#include "VoxelSkySubsystem.h"
#include "VoxelWorldSubsystem.h"

#include "Components/InputComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UnrealClient.h" // FScreenshotRequest

namespace VoxelPauseUIDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Above the survival panel (30) and above the front end's own menu layer (100),
// because a pause overlay that a HUD element can cover is not a pause overlay.
constexpr int32 kPauseZOrder = 120;

// Matches UVoxelFrontEndSubsystem::CaptureAndQuit: FScreenshotRequest only
// QUEUES the request, and the frame that services it has to be rendered and
// written before the process may leave.
constexpr float kCaptureQuitDelaySeconds = 4.0f;

EVoxelPausePanel PanelFromSwitch(const FString& Name)
{
	if (Name == TEXT("settings")) return EVoxelPausePanel::Settings;
	if (Name == TEXT("save"))     return EVoxelPausePanel::Save;
	if (Name == TEXT("load"))     return EVoxelPausePanel::Load;
	return EVoxelPausePanel::Pause;
}

// The whole-day index the pause footer and the default save name are built
// from: elapsed game seconds over the length of a game day, counted from one so
// the first day of a world is "Day 1" rather than "Day 0".
//
// Returns 0 -- which both call sites read as "do not claim a day" -- when there
// is no sky subsystem, which is the case in a bare capture run.
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

// --- The cross-world hand-off -------------------------------------------------
//
// LOAD from the pause menu cannot load in place: there is no world teardown in
// this project (see the header), so the map is reopened and the save is claimed
// on the OTHER side of that reopen. A process-scope string is what survives a
// UWorld being destroyed, and it is consumed exactly once so a second visit to
// the menu does not load the same save again.
FString GPendingLoadSlug;
} // namespace VoxelPauseUIDetail

namespace VoxelPauseUIHandoff
{
void SetPendingLoadSlug(const FString& Slug) { VoxelPauseUIDetail::GPendingLoadSlug = Slug; }

FString ConsumePendingLoadSlug()
{
	FString Slug = MoveTemp(VoxelPauseUIDetail::GPendingLoadSlug);
	VoxelPauseUIDetail::GPendingLoadSlug.Reset();
	return Slug;
}
} // namespace VoxelPauseUIHandoff

bool UVoxelPauseUISubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{
	return Type == EWorldType::Game || Type == EWorldType::PIE;
}

bool UVoxelPauseUISubsystem::IsTickable() const
{
	return !IsTemplate() && GetWorld() != nullptr && GetWorld()->GetNetMode() != NM_DedicatedServer;
}

TStatId UVoxelPauseUISubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelPauseUISubsystem, STATGROUP_Tickables);
}

bool UVoxelPauseUISubsystem::CanPause() const
{
	UWorld* World = GetWorld();
	if (World == nullptr || !World->HasBegunPlay() || World->GetGameViewport() == nullptr)
	{
		return false;
	}
	// DEFER TO THE FRONT END. Menu, ArmLoading, Loading and HandOff all have
	// something of their own on screen and their own input mode; opening a
	// pause overlay over a loading screen would be two owners of the same
	// viewport. Inactive means the front end was suppressed for this run
	// (-VoxelNoMenu and every unattended leg), where the player has the world
	// from the first frame and pause still has to work.
	const UVoxelFrontEndSubsystem* Front = World->GetSubsystem<UVoxelFrontEndSubsystem>();
	if (Front != nullptr && Front->GetState() != EVoxelFrontEndState::Playing
	    && Front->GetState() != EVoxelFrontEndState::Inactive)
	{
		return false;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	return PC != nullptr && PC->IsLocalController() && PC->GetPawn() != nullptr;
}

void UVoxelPauseUISubsystem::Tick(float DeltaTime)
{
	if (!CanPause())
	{
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	if (PC != Controller)
	{
		// A new controller means the old input component is bound to a dead
		// object. Drop everything and re-attach below.
		TeardownOverlay();
		Input = nullptr;
		Controller = PC;
	}
	if (Input == nullptr)
	{
		Input = NewObject<UInputComponent>(PC, TEXT("VoxelPauseUIInput"));
		// Above the survival UI's 20, so Escape reaches pause before anything
		// else in this module can claim it.
		Input->Priority = 30;
		Input->RegisterComponent();
		Input->BindKey(EKeys::Escape, IE_Pressed, this, &UVoxelPauseUISubsystem::TogglePause);
		PC->PushInputComponent(Input);
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: Escape bound."));
	}

	// --- -VoxelPauseShot ----------------------------------------------------
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
	if (!Switches.bPauseShot)
	{
		return;
	}
	if (!bShotOpened && PlayingSeconds >= Switches.PauseShotSeconds)
	{
		bShotOpened = true;
		OpenPause();
		if (Overlay.IsValid())
		{
			Overlay->ShowPanel(VoxelPauseUIDetail::PanelFromSwitch(Switches.PausePanel));
		}
		// A second settle after the panel is up, for the same reason the menu
		// shot settles at all: glyphs rasterise lazily and frame one of a panel
		// is a half-built panel.
		CaptureQuitAtSeconds = PlayingSeconds + Switches.PauseShotSeconds;
		return;
	}
	if (bShotOpened && PlayingSeconds >= CaptureQuitAtSeconds)
	{
		FScreenshotRequest::RequestScreenshot(TEXT("VoxelPause"), /*bShowUI=*/true, /*bAddFilenameSuffix=*/true);
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: screenshot requested (panel '%s') at t=%.2fs."),
		       Switches.PausePanel.IsEmpty() ? TEXT("pause") : *Switches.PausePanel, PlayingSeconds);
		bCaptureRequested = true;
		CaptureQuitAtSeconds = PlayingSeconds + VoxelPauseUIDetail::kCaptureQuitDelaySeconds;
	}
}

void UVoxelPauseUISubsystem::TogglePause()
{
	if (Overlay.IsValid())
	{
		ClosePause();
		return;
	}
	OpenPause();
}

void UVoxelPauseUISubsystem::OpenPause()
{
	UWorld* World = GetWorld();
	if (Overlay.IsValid() || !CanPause())
	{
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	UGameViewportClient* Viewport = World->GetGameViewport();

	// -VoxelDemoSaves fabricates the save list, so it pins the day too: the
	// fabricated rows include one named exactly what the save dialog proposes on
	// day 1, which is what makes the overwrite band reachable in a capture. A
	// demo run whose world happened to be on day 11 would offer "Day 11", match
	// nothing, and the band could never be photographed.
	const int32 Day = FVoxelFrontEndSwitches::Get().bDemoSaves ? 1 : VoxelPauseUIDetail::CurrentDayNumber(World);
	// The context band's line: the save this session is writing into, or the
	// game's own name on a session that has never been named.
	const FString ActiveSlug = VoxelSave::GetActiveSlug(World);
	FText Context = VoxelUIStrings::Title();
	for (const VoxelSave::FSaveInfo& Info : VoxelSave::List())
	{
		if (Info.Slug == ActiveSlug && !Info.DisplayName.IsEmpty())
		{
			Context = FText::FromString(Info.DisplayName);
			break;
		}
	}

	Overlay = SNew(SVoxelPauseMenu)
		.DayNumber(Day)
		.SaveContextName(Context)
		.OnResume(FSimpleDelegate::CreateUObject(this, &UVoxelPauseUISubsystem::ClosePause))
		.OnExitToMenu(FSimpleDelegate::CreateLambda([this]() { ReturnToMenu(FString()); }))
		.OnQuit(FSimpleDelegate::CreateLambda([this]()
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: QUIT."));
			if (UWorld* W = GetWorld())
			{
				UKismetSystemLibrary::QuitGame(W, W->GetFirstPlayerController(), EQuitPreference::Quit,
				                               /*bIgnorePlatformRestrictions=*/false);
			}
		}))
		.OnSaveConfirmed(FOnVoxelSaveNameChosen::CreateUObject(this, &UVoxelPauseUISubsystem::HandleSaveConfirmed))
		.OnLoadSave(FOnVoxelSaveAction::CreateLambda([this](const FString& Slug) { ReturnToMenu(Slug); }))
		.OnDeleteSave(FOnVoxelSaveAction::CreateUObject(this, &UVoxelPauseUISubsystem::HandleDeleteSave));

	Viewport->AddViewportWidgetContent(Overlay.ToSharedRef(), VoxelPauseUIDetail::kPauseZOrder);
	RefreshRows();

	// PAUSED SIMULATION, LIVE PICTURE. SetGamePaused stops actor ticking; Slate
	// is engine-level and keeps ticking and painting, so the overlay animates
	// and takes input over a world that has genuinely stopped.
	UGameplayStatics::SetGamePaused(World, true);

	// The same input-mode discipline UVoxelFrontEndSubsystem::ApplyMenuInputMode
	// applies, and for the same reason it gives: AVoxelEarthPlayerController
	// binds raw keys with no notion of a UI focus state, so under GameAndUI a
	// player reading the pause menu would still be digging.
	bSavedShowCursor = PC->bShowMouseCursor;
	if (PC->PlayerInput != nullptr)
	{
		// Otherwise the key that opened the menu is still down when the world
		// resumes, and so is whatever was held with it.
		PC->PlayerInput->FlushPressedKeys();
	}
	PC->SetIgnoreMoveInput(true);
	PC->SetIgnoreLookInput(true);
	PC->bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(Overlay);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(InputMode);
	if (PC->MyHUD != nullptr)
	{
		PC->MyHUD->bShowHUD = false;
	}

	// The survival hotbar draws at z-order 15 and would otherwise sit under the
	// scrim, dimmed but legible, which is not what a paused screen looks like in
	// any of the four mocks.
	if (UVoxelSurvivalUISubsystem* Survival = World->GetSubsystem<UVoxelSurvivalUISubsystem>())
	{
		Survival->SetHiddenForOverlay(true);
	}

	// THE SAME ARGUMENT, FOR THE NEWER HUD (2026-09-08). The Voxelmark HUD sits
	// at z-order 109 and the pause scrim is not opaque, so the compass, the dock
	// and the music transport were all still legible through it -- the exact
	// symptom the line above was written for, on the widget that replaced the
	// hotbar it names. UVoxelScreensUISubsystem owns that HUD's visibility;
	// asking it to refresh is enough, because its answer is OverlayOwnsInput()
	// and that already includes this subsystem.
	if (UVoxelScreensUISubsystem* Screens = World->GetSubsystem<UVoxelScreensUISubsystem>())
	{
		Screens->RefreshHudForOverlays();
	}

	// Focus a BUTTON, not the overlay: SetWidgetToFocus is enough for Escape to
	// reach OnKeyDown and not enough for navigation, which starts from the
	// focused widget and a compound widget has no siblings to move between.
	Overlay->FocusDefaultWidget();
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: opened (day %d)."), Day);
}

void UVoxelPauseUISubsystem::ClosePause()
{
	if (!Overlay.IsValid())
	{
		return;
	}
	TeardownOverlay();
	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::SetGamePaused(World, false);
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: resumed."));
}

void UVoxelPauseUISubsystem::TeardownOverlay()
{
	if (!Overlay.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
	}
	Overlay.Reset();

	if (UVoxelSurvivalUISubsystem* Survival = World ? World->GetSubsystem<UVoxelSurvivalUISubsystem>() : nullptr)
	{
		Survival->SetHiddenForOverlay(false);
	}

	// AFTER Overlay.Reset() ABOVE, and that ordering is the whole correctness of
	// it: the refresh re-reads IsPaused(), which is this pointer. Refreshing
	// before the reset would leave the HUD hidden until the next tick, and
	// refreshing unconditionally would show it over a screen the player opened
	// the pause menu on top of.
	if (UVoxelScreensUISubsystem* Screens = World ? World->GetSubsystem<UVoxelScreensUISubsystem>() : nullptr)
	{
		Screens->RefreshHudForOverlays();
	}

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
}

void UVoxelPauseUISubsystem::RefreshRows()
{
	if (!Overlay.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	const uint64 RunningSeed = WorldSub ? WorldSub->GetSeed() : 0;

	// The same row construction UVoxelFrontEndSubsystem::RefreshSaveRows does,
	// including the metres conversion and the seed-mismatch reason -- see
	// VoxelSaveRows.h, which is where it now lives so the two screens cannot
	// describe the same save differently.
	Overlay->SetSaveRows(VoxelSaveRows::Build(RunningSeed));
}

void UVoxelPauseUISubsystem::HandleSaveConfirmed(const FString& DisplayName)
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (WorldSub == nullptr)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelPause: SAVE '%s' -- no voxel world subsystem."), *DisplayName);
		return;
	}

	// Position and view direction, which is what the save format stores and
	// what RequestLoad hands back to BeginLoad as a spawn transform.
	FTransform PlayerTransform = FTransform::Identity;
	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			PlayerTransform = FTransform(PC->GetControlRotation(), Pawn->GetActorLocation());
		}
	}

	// bIsAutosave=false: this is the player naming a save. PlayTimeSeconds is 0
	// because nothing in this project accumulates one yet -- every existing
	// caller of Write passes 0 for the same reason, and inventing a number here
	// would make this the only save whose field meant anything.
	const bool bWritten = VoxelSave::Write(*WorldSub, DisplayName, /*bIsAutosave=*/false, PlayerTransform,
	                                       /*PlayTimeSeconds=*/0);
	if (!bWritten)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelPause: SAVE '%s' FAILED."), *DisplayName);
		return;
	}
	// Claim it, so an autosave-on-shutdown after this writes back into the save
	// the player just named rather than into the seed-derived default.
	VoxelSave::SetActiveSlug(World,VoxelSave::Slugify(DisplayName));
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: SAVE '%s' written."), *DisplayName);
	RefreshRows();
}

void UVoxelPauseUISubsystem::HandleDeleteSave(const FString& Slug)
{
	if (!VoxelSave::Delete(Slug))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelPause: DELETE %s -- nothing to delete."), *Slug);
	}
	// Rebuild either way: if the directory was already gone, the row still on
	// screen is the thing that is wrong.
	RefreshRows();
}

void UVoxelPauseUISubsystem::ReturnToMenu(const FString& PendingLoadSlug)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	// The overlay and the pause state both go first: OpenLevel on a paused
	// world leaves the NEXT world paused, and a title screen nobody can click
	// is a hang with a picture on it.
	TeardownOverlay();
	UGameplayStatics::SetGamePaused(World, false);

	VoxelPauseUIHandoff::SetPendingLoadSlug(PendingLoadSlug);
	const FString LevelName = UGameplayStatics::GetCurrentLevelName(World, /*bRemovePrefixString=*/true);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelPause: reopening '%s' to reach the title screen%s."), *LevelName,
	       PendingLoadSlug.IsEmpty() ? TEXT("") : TEXT(" and load a save"));
	UGameplayStatics::OpenLevel(World, FName(*LevelName));
}

void UVoxelPauseUISubsystem::Deinitialize()
{
	TeardownOverlay();
	if (Controller != nullptr && Input != nullptr)
	{
		Controller->PopInputComponent(Input);
	}
	if (Input != nullptr)
	{
		Input->DestroyComponent();
		Input = nullptr;
	}
	Controller = nullptr;
	Super::Deinitialize();
}

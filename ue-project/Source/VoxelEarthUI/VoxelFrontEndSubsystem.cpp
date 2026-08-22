#include "VoxelFrontEndSubsystem.h"

#include "SVoxelMainMenu.h"
#include "VoxelEarthUI.h"
#include "VoxelFrontEndSwitches.h"
#include "VoxelUIStyle.h"

#include "VoxelFrontEndPolicy.h"
#include "VoxelEarthGameMode.h"
#include "VoxelSaveLibrary.h"
#include "VoxelWorldSubsystem.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"                // FScreenshotRequest
#include "Kismet/KismetSystemLibrary.h" // QuitGame / EQuitPreference
#include "Misc/App.h"                    // FApp::IsUnattended for the watchdog

namespace VoxelFrontEndDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// Z-order for the two front-end layers. Above anything the game draws, and
// the loading screen above the menu so a hand-off never shows the menu through
// a partially faded curtain.
constexpr int32 kMenuZOrder = 100;
constexpr int32 kLoadingZOrder = 110;
} // namespace VoxelFrontEndDetail

bool UVoxelFrontEndSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Mirrors UVoxelWorldSubsystem's: game worlds only, so opening the level
	// editor never puts a main menu over it.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelFrontEndSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!VoxelFrontEnd::IsEnabledThisRun())
	{
		State = EVoxelFrontEndState::Inactive;
		// ONE LINE, IN EVERY RUN, WHICHEVER ARM IT TOOK. A capture that came
		// out wrong is then diagnosable from its own log -- "was the front end
		// even on?" is the first question and this is the answer to it.
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: suppressed (%s)."), VoxelFrontEnd::WhyThisAnswer());
		return;
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: active (%s)."), VoxelFrontEnd::WhyThisAnswer());
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: menu font %s."),
	       FVoxelUIStyle::Get().IsProjectFontAvailable() ? TEXT("loaded") : TEXT("FALLBACK (engine default face)"));
}

void UVoxelFrontEndSubsystem::Deinitialize()
{
	TeardownMenu();
	Super::Deinitialize();
}

void UVoxelFrontEndSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (State == EVoxelFrontEndState::Inactive)
	{
		return;
	}
	EnterMenu();
}

bool UVoxelFrontEndSubsystem::IsTickable() const
{
	// Inactive and Playing both mean "nothing left to do". Returning false
	// rather than early-returning inside Tick keeps the front end genuinely
	// free once the player has the world, which matters because this project
	// is frame-time bound and a per-frame no-op is still a per-frame call.
	return State != EVoxelFrontEndState::Inactive && State != EVoxelFrontEndState::Playing;
}

TStatId UVoxelFrontEndSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelFrontEndSubsystem, STATGROUP_Tickables);
}

void UVoxelFrontEndSubsystem::EnterMenu()
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		// No viewport and the policy said we could render: something is wrong
		// enough that a menu would be the smaller problem. Fall through to the
		// world rather than leaving the player on a black screen.
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelFrontEnd: no game viewport; starting the world without a menu."));
		if (UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
		{
			WorldSub->StartWorldSession(FString());
		}
		State = EVoxelFrontEndState::Playing;
		return;
	}

	MenuWidget = SNew(SVoxelMainMenu)
		.OnContinue_UObject(this, &UVoxelFrontEndSubsystem::RequestContinue)
		.OnNewGame_UObject(this, &UVoxelFrontEndSubsystem::RequestNewGame)
		.OnQuit_UObject(this, &UVoxelFrontEndSubsystem::RequestQuit)
		.OnLoadSave_UObject(this, &UVoxelFrontEndSubsystem::RequestLoad)
		.OnDeleteSave_UObject(this, &UVoxelFrontEndSubsystem::RequestDelete);

	Viewport->AddViewportWidgetContent(MenuWidget.ToSharedRef(), VoxelFrontEndDetail::kMenuZOrder);

	RefreshSaveRows();

	// -VoxelMenuPanel=load|help|credits opens straight onto a sub-panel, so a
	// capture can photograph one without a click.
	const FString& Panel = FVoxelFrontEndSwitches::Get().MenuPanel;
	if (Panel == TEXT("load"))
	{
		MenuWidget->ShowPanel(EVoxelMenuPanel::Load);
	}
	else if (Panel == TEXT("help"))
	{
		MenuWidget->ShowPanel(EVoxelMenuPanel::Help);
	}
	else if (Panel == TEXT("credits"))
	{
		MenuWidget->ShowPanel(EVoxelMenuPanel::Credits);
	}
	else if (Panel == TEXT("settings"))
	{
		MenuWidget->ShowPanel(EVoxelMenuPanel::Settings);
	}
	else if (!Panel.IsEmpty())
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("-VoxelMenuPanel=%s not recognised; showing the main column."), *Panel);
	}

	State = EVoxelFrontEndState::Menu;
	StateSeconds = 0.f;
	bMenuInputApplied = false;
}

void UVoxelFrontEndSubsystem::ApplyMenuInputMode()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC == nullptr || !MenuWidget.IsValid())
	{
		return; // try again next tick
	}

	PC->bShowMouseCursor = true;
	// FInputModeUIOnly IS WHAT STOPS THE GAME PLAYING ITSELF BEHIND THE MENU.
	// AVoxelEarthPlayerController binds raw keys directly -- F1, F3, the digit
	// keys, LMB/RMB dig and place -- with no notion of a UI focus state. Under
	// GameAndUI those all still fire while somebody is reading the menu, so a
	// player who taps 2 before pressing NEW GAME silently changes their dig
	// size. DoNotLock keeps the cursor free of the viewport, which also
	// neutralises DefaultInput.ini's CapturePermanently_IncludingInitialMouseDown.
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(MenuWidget);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(InputMode);

	// AVoxelEarthHUD needs no change of its own: AHUD::PostRender already
	// guards DrawHUD on bShowHUD, and restores it exactly.
	if (PC->MyHUD != nullptr)
	{
		PC->MyHUD->bShowHUD = false;
	}
	bMenuInputApplied = true;
}

void UVoxelFrontEndSubsystem::RefreshSaveRows()
{
	if (!MenuWidget.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	const uint64 RunningSeed = WorldSub ? WorldSub->GetSeed() : 0;

	TArray<FVoxelSaveRowInfo> Rows;
	for (const VoxelSave::FSaveInfo& Info : VoxelSave::List())
	{
		FVoxelSaveRowInfo Row;
		Row.Slug = Info.Slug;
		Row.DisplayName = FText::FromString(Info.DisplayName);

		// MainMenu.gd's row reads
		//   "<save_name>\n<timestamp>   X %.0f  Y %.0f  Z %.0f"
		// so the shape is preserved exactly. The UNITS are not: Godot stored
		// metres and this project stores Unreal units (1 UU = 1 cm), which
		// would print a coordinate like -6510200 and read as noise. Converted
		// to metres, which is what the F1 overlay and every log line in this
		// project use -- so the number a player sees here matches the one they
		// see in game.
		const FVector Metres = Info.PlayerPosition / 100.0;
		Row.Detail = FText::FromString(FString::Printf(TEXT("%s   X %.0f  Y %.0f  Z %.0f"), *Info.TimestampIso,
		                                               Metres.X, Metres.Y, Metres.Z));

		// THE ONE PLACE A 1:1 CLONE CANNOT HOLD. The world seed is baked into
		// the amplifier when Impl is constructed, long before any menu exists,
		// so a save recorded under a different seed cannot be opened without
		// rebuilding the voxel world wholesale. Only reachable via -VoxelSeed=,
		// so in ordinary play every row is loadable -- but a row that silently
		// did nothing when clicked would be far worse than one that says why.
		if (RunningSeed != 0 && Info.Seed != RunningSeed)
		{
			Row.bLoadable = false;
			Row.DisabledReason = FText::FromString(
				FString::Printf(TEXT("requires relaunch with -VoxelSeed=%llu"), (unsigned long long)Info.Seed));
		}
		Rows.Add(MoveTemp(Row));
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: %d save(s) listed."), Rows.Num());
	MenuWidget->SetSaveRows(MoveTemp(Rows));
}

void UVoxelFrontEndSubsystem::RequestNewGame()
{
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: NEW GAME."));
	// The loading screen and the hand-off land in a later step. Starting the
	// world directly here keeps the menu honest in the meantime: the button
	// does what it says, and the only thing missing is the curtain.
	UWorld* World = GetWorld();
	if (UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
	{
		WorldSub->StartWorldSession(FString());
	}
	if (AVoxelEarthGameMode* GameMode = World ? World->GetAuthGameMode<AVoxelEarthGameMode>() : nullptr)
	{
		GameMode->BeginPlayerSession(nullptr);
	}
	TeardownMenu();
	State = EVoxelFrontEndState::Playing;
}

void UVoxelFrontEndSubsystem::RequestContinue()
{
	// CONTINUE IS LITERALLY "THE NEWEST SAVE". MainMenu.gd implements it as
	// GameState.list_save_files()[0], relying on that list being newest-first;
	// VoxelSave::List() keeps the same contract for the same reason.
	const TArray<VoxelSave::FSaveInfo> Saves = VoxelSave::List();
	if (Saves.Num() == 0)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelFrontEnd: CONTINUE with no saves; ignoring."));
		return;
	}
	RequestLoad(Saves[0].Slug);
}

void UVoxelFrontEndSubsystem::RequestLoad(const FString& Slug)
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (WorldSub == nullptr)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelFrontEnd: LOAD %s -- no voxel world subsystem."), *Slug);
		return;
	}

	// Re-read the metadata rather than trusting the row: the list was built
	// when the menu opened, and the save could have been deleted since -- by
	// the DELETE button sitting right next to it, if nothing else.
	const TArray<VoxelSave::FSaveInfo> Saves = VoxelSave::List();
	const VoxelSave::FSaveInfo* Info = Saves.FindByPredicate(
		[&Slug](const VoxelSave::FSaveInfo& Candidate) { return Candidate.Slug == Slug; });
	if (Info == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelFrontEnd: LOAD %s -- no such save any more."), *Slug);
		RefreshSaveRows();
		return;
	}
	if (Info->Seed != WorldSub->GetSeed())
	{
		UE_LOG(LogVoxelUI, Warning,
		       TEXT("VoxelFrontEnd: LOAD %s -- recorded under seed %llu but this session is seed %llu. ")
		       TEXT("Relaunch with -VoxelSeed=%llu."),
		       *Slug, (unsigned long long)Info->Seed, (unsigned long long)WorldSub->GetSeed(),
		       (unsigned long long)Info->Seed);
		return;
	}

	// Claim the save BEFORE the world starts, so that an autosave-on-shutdown
	// at any point after this writes back into it rather than into the
	// seed-derived default -- otherwise a player's next CONTINUE quietly
	// reopens the state they had when they loaded, losing the session.
	VoxelSave::SetActiveSlug(Slug);

	const FTransform SpawnTransform(Info->PlayerRotation, Info->PlayerPosition);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: LOAD %s (%lld edit(s))."), *Slug, (long long)Info->EditCount);

	WorldSub->StartWorldSession(VoxelSave::WorldLogPath(Slug));
	if (AVoxelEarthGameMode* GameMode = World->GetAuthGameMode<AVoxelEarthGameMode>())
	{
		GameMode->BeginPlayerSession(&SpawnTransform);
	}
	TeardownMenu();
	State = EVoxelFrontEndState::Playing;
}

void UVoxelFrontEndSubsystem::RequestDelete(const FString& Slug)
{
	if (!VoxelSave::Delete(Slug))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelFrontEnd: DELETE %s -- nothing to delete."), *Slug);
	}
	// Rebuild the list either way: if the directory was already gone, the row
	// still on screen is the thing that is wrong.
	RefreshSaveRows();
}

void UVoxelFrontEndSubsystem::RequestQuit()
{
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: QUIT."));
	if (UWorld* World = GetWorld())
	{
		UKismetSystemLibrary::QuitGame(World, World->GetFirstPlayerController(), EQuitPreference::Quit,
		                               /*bIgnorePlatformRestrictions=*/false);
	}
}

void UVoxelFrontEndSubsystem::TeardownMenu()
{
	UWorld* World = GetWorld();
	if (MenuWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			Viewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
		}
		MenuWidget.Reset();
	}
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->bShowMouseCursor = false;
		if (PC->MyHUD != nullptr)
		{
			PC->MyHUD->bShowHUD = true;
		}
	}
}

void UVoxelFrontEndSubsystem::CaptureAndQuit(const TCHAR* ShotName)
{
	// bShowUI=TRUE, which is the whole point and the reason these switches
	// exist alongside -VoxelScreenshotAfter rather than reusing it: that chain
	// captures with the UI off, which for a front-end capture would photograph
	// whatever is behind the menu -- on the main menu, nothing at all.
	FScreenshotRequest::RequestScreenshot(ShotName, /*bShowUI=*/true, /*bAddFilenameSuffix=*/true);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: %s screenshot requested at t=%.2fs."), ShotName, StateSeconds);
	bCaptureRequested = true;
	CaptureQuitAtSeconds = StateSeconds + 4.0f;
}

void UVoxelFrontEndSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	StateSeconds += DeltaTime;

	// A capture is in flight: nothing else may happen -- including the auto
	// start -- or the picture and the world would race each other.
	if (bCaptureRequested)
	{
		if (StateSeconds >= CaptureQuitAtSeconds)
		{
			FPlatformMisc::RequestExit(false);
		}
		return;
	}

	if (State == EVoxelFrontEndState::Menu)
	{
		if (!bMenuInputApplied)
		{
			ApplyMenuInputMode();
		}

		const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();

		// -VoxelMenuShot[=<s>]: settle, photograph the menu, quit. Settling
		// matters even on a screen with no world behind it -- the background
		// art decodes on a worker and glyphs rasterise lazily, so a capture on
		// frame one would photograph a half-built menu and read as a
		// regression.
		if (Switches.bMenuShot && StateSeconds >= Switches.MenuShotSeconds)
		{
			CaptureAndQuit(TEXT("VoxelMenu"));
			return;
		}

		// -VoxelMenuAutoStart: the compatibility switch. Pressing NEW GAME on
		// the run's behalf is what lets every existing -Voxel* capture run
		// through the front end and still photograph the world it always did.
		if (Switches.bAutoStart && StateSeconds >= Switches.AutoStartSeconds)
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: -VoxelMenuAutoStart firing after %.2fs."), StateSeconds);
			RequestNewGame();
			return;
		}

		// THE WATCHDOG. An unattended run that reaches a menu is already a
		// mistake -- some flag was wrong -- and the failure mode without this
		// is a machine sitting at a title screen until somebody notices, which
		// on a shared capture box can be hours. Same shape as
		// -VoxelPerfExitWatchdog, and deliberately an Error rather than a Log:
		// it should show up in a grep of the run's log, not be discovered by
		// reading it.
		if (!bWatchdogTripped && FApp::IsUnattended() && Switches.MenuWatchdogSeconds > 0.f
		    && StateSeconds >= Switches.MenuWatchdogSeconds)
		{
			bWatchdogTripped = true;
			UE_LOG(LogVoxelUI, Error,
			       TEXT("VoxelFrontEnd: unattended run sat on the main menu for %.0fs (-VoxelMenuWatchdog). ")
			       TEXT("Exiting -- pass -VoxelMenuAutoStart to drive through it, or -VoxelNoMenu to skip it."),
			       StateSeconds);
			if (UWorld* World = GetWorld())
			{
				UKismetSystemLibrary::QuitGame(World, World->GetFirstPlayerController(), EQuitPreference::Quit, true);
			}
		}
	}
}

#include "VoxelFrontEndSubsystem.h"
#include "VoxelGraphicsUserSettings.h"

#include "SVoxelHourglass.h"
#include "SVoxelLoadingScreen.h"
#include "SVoxelMainMenu.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIMusic.h"
#include "VoxelWorldReadyProbe.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"
#include "VoxelEarthUI.h"
#include "VoxelFrontEndSwitches.h"

#include "VoxelFrontEndPolicy.h"
#include "VoxelEarthGameMode.h"
#include "VoxelSaveLibrary.h"
#include "VoxelWorldSubsystem.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"                // FScreenshotRequest
#include "Kismet/KismetSystemLibrary.h" // QuitGame / EQuitPreference
#include "Misc/App.h"                    // FApp::IsUnattended for the watchdog
#include "Misc/DateTime.h"               // the NEW GAME world backup stamp
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"           // FPlatformMisc::RequestExit

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

	// Player graphics settings latch here, BEFORE the front-end suppression
	// branch: a -game run with the menu suppressed is precisely the run where
	// the player is playing, and their persisted choices must land before the
	// first marched frame either way. Idempotent per world.
	VoxelGraphicsUserSettings::ApplyAll();

	if (!VoxelFrontEnd::IsEnabledThisRun())
	{
		State = EVoxelFrontEndState::Inactive;
		// ONE LINE, IN EVERY RUN, WHICHEVER ARM IT TOOK. A capture that came
		// out wrong is then diagnosable from its own log -- "was the front end
		// even on?" is the first question and this is the answer to it.
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: suppressed (%s)."), VoxelFrontEnd::WhyThisAnswer());
		return;
	}
	State = EVoxelFrontEndState::Pending;
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: active (%s)."), VoxelFrontEnd::WhyThisAnswer());
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: menu font %s."),
	       FVoxelUIStyle::Get().IsProjectFontAvailable() ? TEXT("loaded") : TEXT("FALLBACK (engine default face)"));
}

// Out of line, and this is not a formality. ReadyProbe is a
// TUniquePtr<FVoxelWorldReadyProbe> and the generated
// UVoxelFrontEndSubsystem(FVTableHelper&) instantiates the implicit destructor
// inside VoxelFrontEndSubsystem.gen.cpp, which includes only the header --
// where FVoxelWorldReadyProbe is an incomplete type. Deleting through it there
// is C4150 on MSVC and -Wdelete-incomplete on Clang, and either way
// ~FVoxelWorldReadyProbe silently does not run. Declaring the destructor and
// defining it HERE, where the type is complete, is the standard UE pImpl
// answer. (The TSharedPtr widget members need none of this: TSharedPtr
// type-erases its deleter.)
UVoxelFrontEndSubsystem::UVoxelFrontEndSubsystem() = default;
UVoxelFrontEndSubsystem::~UVoxelFrontEndSubsystem() = default;
// The generated one would live in gen.cpp, where FVoxelWorldReadyProbe is
// incomplete. See the header.
UVoxelFrontEndSubsystem::UVoxelFrontEndSubsystem(FVTableHelper& Helper) : Super(Helper) {}

void UVoxelFrontEndSubsystem::Deinitialize()
{
	TeardownMenu();
	Super::Deinitialize();
}

void UVoxelFrontEndSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// The policy, not the state, because a state that has been mis-assigned is
	// exactly how this went wrong once already.
	if (!VoxelFrontEnd::IsEnabledThisRun())
	{
		return;
	}
	if (FVoxelFrontEndSwitches::Get().bHourglassShot)
	{
		EnterHourglassShot();
		return;
	}
	EnterMenu();
}

void UVoxelFrontEndSubsystem::EnterHourglassShot()
{
	using namespace VoxelUITheme;
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("-VoxelHourglassShot: no game viewport."));
		return;
	}
	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);
	for (const float ProgressValue : Switches.HourglassProgress)
	{
		Strip->AddSlot().AutoWidth().Padding(FMargin(32.f, 0.f)).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(L.HourglassWidth)
				.HeightOverride(L.HourglassHeight)
				[
					SNew(SVoxelHourglass).Progress(ProgressValue)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 20.f, 0.f, 0.f))
			[
				// Labelled, because an unlabelled strip of five hourglasses is
				// only diffable against another strip -- with the numbers on
				// it, a single image answers "does 0.25 look right".
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%.2f"), ProgressValue)))
				.Font(Style.Serif(L.LoadingPctSize))
				.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			]
		];
	}

	// A flat field, not the menu background: the point is to see the hourglass,
	// and photographic art behind it would make a pixel diff meaningless.
	HourglassShotWidget = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(FSlateColor(Tint(BgNight)))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Strip
		];

	Viewport->AddViewportWidgetContent(HourglassShotWidget.ToSharedRef(), VoxelFrontEndDetail::kLoadingZOrder);
	UE_LOG(LogVoxelUI, Log, TEXT("-VoxelHourglassShot: %d hourglass(es) on screen."), Switches.HourglassProgress.Num());
	State = EVoxelFrontEndState::Menu;
	StateSeconds = 0.f;
	// Input mode is left alone: there is nothing to click, and the run quits.
	bMenuInputApplied = true;
}

bool UVoxelFrontEndSubsystem::IsTickable() const
{
	// Inactive and Playing both mean "nothing left to do". Returning false
	// rather than early-returning inside Tick keeps the front end genuinely
	// free once the player has the world, which matters because this project
	// is frame-time bound and a per-frame no-op is still a per-frame call.
	// Pending ticks harmlessly -- Tick's dispatch has no branch for it -- and
	// is a state the subsystem passes through in the frames before the world
	// begins play.
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

	// MUSIC STARTS WITH THE MENU AND IS NOT RESTARTED AFTERWARDS. The contract
	// ADR-0009 recorded before there was any audio to apply it to is "adopt at
	// BeginLoad, fade at hand-off" -- so the track that starts here is the same
	// one still playing through the loading screen. StartRandom is a no-op once
	// something is playing, which is what makes that true without a state flag.
	//
	// The stream is the one the backgrounds were shuffled from, so a seeded run
	// pairs the same art with the same track.
	{
		FRandomStream MusicStream = MakeVoxelUIRandomStream();
		FVoxelUIMusic::Get().StartRandom(GetWorld(), MusicStream);
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

	// FOCUS A BUTTON, NOT THE MENU. SetWidgetToFocus above puts focus on the
	// SVoxelMainMenu itself, which is enough for Escape to reach OnKeyDown and
	// not enough for anything else -- a gamepad player pressing Down would get
	// nothing, because navigation starts from the focused widget and a
	// compound widget has no siblings to move between. Handing focus to the
	// first usable button is what makes the menu drivable without a mouse.
	MenuWidget->FocusDefaultWidget();

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

	// THE PORT OF reset_for_new_game(), with one difference. The Godot version
	// deletes the existing world deltas outright; this renames them aside with
	// a UTC stamp. A new game genuinely should not inherit the old world's
	// edits -- but this repository does not silently destroy evidence, and a
	// player who presses NEW GAME on the wrong menu should be able to get
	// their world back from the Saved directory rather than from a backup they
	// did not make.
	UWorld* World = GetWorld();
	if (UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
	{
		const FString DefaultWorld = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds")
		                             / FString::Printf(TEXT("%llu.vxlog"), (unsigned long long)WorldSub->GetSeed());
		if (IFileManager::Get().FileExists(*DefaultWorld))
		{
			const FString Backup = DefaultWorld + TEXT(".bak-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			if (IFileManager::Get().Move(*Backup, *DefaultWorld))
			{
				UE_LOG(LogVoxelUI, Log, TEXT("NEW GAME: moved the previous world aside to %s."), *Backup);
			}
		}
	}
	// No active save: a new game writes to the seed-derived default until the
	// player names one.
	VoxelSave::SetActiveSlug(FString());
	BeginLoad(FString(), nullptr);
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

	BeginLoad(VoxelSave::WorldLogPath(Slug), &SpawnTransform);
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

void UVoxelFrontEndSubsystem::BeginLoad(const FString& EditLogPath, const FTransform* SpawnOverride)
{
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelFrontEnd: no viewport at BeginLoad; starting the world uncovered."));
		PendingEditLogPath = EditLogPath;
		PendingSpawnTransform = SpawnOverride ? TOptional<FTransform>(*SpawnOverride) : TOptional<FTransform>();
		StartWorldAndPawn();
		State = EVoxelFrontEndState::Playing;
		return;
	}

	PendingEditLogPath = EditLogPath;
	PendingSpawnTransform = SpawnOverride ? TOptional<FTransform>(*SpawnOverride) : TOptional<FTransform>();

	LoadingWidget = SNew(SVoxelLoadingScreen);
	Viewport->AddViewportWidgetContent(LoadingWidget.ToSharedRef(), VoxelFrontEndDetail::kLoadingZOrder);

	// The menu goes away now, not at hand-off: it is behind an opaque curtain
	// either way, and leaving it alive would keep its background texture
	// resident for the whole load.
	if (MenuWidget.IsValid())
	{
		Viewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
		MenuWidget.Reset();
	}

	// TWO FRAMES OF PAINTING BEFORE ANY WORK STARTS. This is the port of the
	// GDScript's two awaited process_frames, and the reason is the same even
	// though the work is different. There, a synchronous scene load froze the
	// main thread for 5-8 s and the player stared at a black rectangle. Here
	// there is no blocking load -- but StartWorldSession's first tick builds
	// the initial desired set for a whole cascade, which is the single most
	// expensive frame of the session. Painting the curtain first means the
	// player sees a loading screen during that frame rather than a frozen
	// menu.
	ArmFrames = 2;
	LoadElapsedSeconds = 0.f;
	LastProgress = 0.f;
	NextLoadingShotIndex = 0;

	// Normally a no-op: the menu started a track and this is the same session,
	// so the loading screen inherits it rather than restarting -- which is the
	// whole point of "adopt at BeginLoad". It matters for the path where a
	// loading screen goes up WITHOUT a menu in front of it, which no switch
	// takes today and which would otherwise be silently music-less.
	{
		FRandomStream MusicStream = MakeVoxelUIRandomStream();
		FVoxelUIMusic::Get().StartRandom(GetWorld(), MusicStream);
	}

	State = EVoxelFrontEndState::ArmLoading;
	StateSeconds = 0.f;
}

void UVoxelFrontEndSubsystem::StartWorldAndPawn()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (WorldSub == nullptr)
	{
		UE_LOG(LogVoxelUI, Error, TEXT("VoxelFrontEnd: no voxel world subsystem; cannot start the world."));
		return;
	}

	WorldSub->StartWorldSession(PendingEditLogPath);
	if (AVoxelEarthGameMode* GameMode = World->GetAuthGameMode<AVoxelEarthGameMode>())
	{
		GameMode->BeginPlayerSession(PendingSpawnTransform.IsSet() ? &PendingSpawnTransform.GetValue() : nullptr);
	}

	// The probe rings the spawn column, which is where the anchor override was
	// already pointed at OnWorldBeginPlay -- so the probes and the streaming
	// footprint agree from the very first poll rather than a frame later.
	FVoxelReadyProbeConfig ProbeConfig;
	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	ProbeConfig.GateMaxRingLevel = Switches.LoadGateMaxRing;
	ProbeConfig.MaxWaitSeconds = Switches.LoadMaxHoldSeconds;

	FVector Anchor = FVector::ZeroVector;
	if (PendingSpawnTransform.IsSet())
	{
		Anchor = PendingSpawnTransform->GetLocation();
	}
	else
	{
		double SpawnX = 0.0;
		double SpawnY = 0.0;
		VoxelEarthSpawn::ParseSpawnColumnUU(SpawnX, SpawnY);
		Anchor = FVector(SpawnX, SpawnY, WorldSub->GetSurfaceHeightUU(SpawnX, SpawnY));
	}

	ReadyProbe = MakeUnique<FVoxelWorldReadyProbe>();
	ReadyProbe->Start(Anchor, ProbeConfig);
}

float UVoxelFrontEndSubsystem::ComputeProgress() const
{
	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	const float TimeTerm = Switches.LoadMaxHoldSeconds > 0.f
	                           ? LoadElapsedSeconds / Switches.LoadMaxHoldSeconds
	                           : 0.f;
	float Spatial = 0.f;
	float RingFill = 0.f;
	if (ReadyProbe.IsValid())
	{
		const FVoxelReadyProbeStatus& Probe = ReadyProbe->GetStatus();
		Spatial = Probe.ProbeTotal > 0 ? float(Probe.ProbeHits) / float(Probe.ProbeTotal) : 0.f;
		RingFill = Probe.RingFillFraction;
	}
	// The model itself is a pure function, in VoxelWorldReadyProbe.h, so its
	// three invariants can be tested without a world.
	return ComputeLoadProgress(TimeTerm, Spatial, RingFill, LastProgress);
}

void UVoxelFrontEndSubsystem::TickLoading(float DeltaSeconds)
{
	LoadElapsedSeconds += DeltaSeconds;

	UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (ReadyProbe.IsValid() && WorldSub != nullptr)
	{
		ReadyProbe->Tick(DeltaSeconds, *WorldSub);
	}

	LastProgress = ComputeProgress();
	if (LoadingWidget.IsValid())
	{
		LoadingWidget->SetProgress(LastProgress);
	}

	// -VoxelLoadingShotAt=<s,s,s>: capture at each offset in turn. Ordered and
	// consumed one at a time, so a burst produces one image per offset rather
	// than one image and two missed shutters.
	const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
	if (Switches.bLoadingShot && NextLoadingShotIndex < Switches.LoadingShotSeconds.Num()
	    && LoadElapsedSeconds >= Switches.LoadingShotSeconds[NextLoadingShotIndex])
	{
		++NextLoadingShotIndex;
		if (NextLoadingShotIndex >= Switches.LoadingShotSeconds.Num())
		{
			CaptureAndQuit(TEXT("VoxelLoading"));
		}
		else
		{
			FScreenshotRequest::RequestScreenshot(TEXT("VoxelLoading"), /*bShowUI=*/true, /*bAddFilenameSuffix=*/true);
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: loading screenshot at t=%.2fs (%d of %d)."),
			       LoadElapsedSeconds, NextLoadingShotIndex, Switches.LoadingShotSeconds.Num());
		}
		return;
	}

	// THE HOLD CONTRACT, ported from TransitionManager._do_transition:
	// wait at least MinHold, then leave as soon as the world reports ready;
	// leave regardless at MaxHold. The minimum exists so a warm cache does not
	// flash the loading screen for a third of a second, which reads as a
	// glitch rather than as speed.
	const bool bReady = ReadyProbe.IsValid() && ReadyProbe->IsReady();
	const bool bTimedOut = ReadyProbe.IsValid() && ReadyProbe->HasTimedOut();
	if ((LoadElapsedSeconds >= Switches.LoadMinHoldSeconds && bReady) || bTimedOut)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: closing the curtain after %.2fs (%s)."), LoadElapsedSeconds,
		       bReady ? TEXT("world ready") : TEXT("timed out"));
		LastProgress = 1.f;
		if (LoadingWidget.IsValid())
		{
			LoadingWidget->SetProgress(1.f);
		}
		HandOffSeconds = 0.f;
		State = EVoxelFrontEndState::HandOff;
	}
}

void UVoxelFrontEndSubsystem::TickHandOff(float DeltaSeconds)
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	HandOffSeconds += DeltaSeconds;

	// The fade is started ONCE, on the first hand-off tick, and runs alongside
	// the curtain rather than after it. MusicFadeOut (1.5s) is independent of
	// FadeDuration on purpose: the picture and the sound do not have to leave
	// at the same rate, and the Godot build's music outlived its curtain.
	if (!bMusicFadeStarted)
	{
		bMusicFadeStarted = true;
		FVoxelUIMusic::Get().FadeOut(L.MusicFadeOut);
	}

	const float Alpha = L.FadeDuration > 0.f ? FMath::Clamp(HandOffSeconds / L.FadeDuration, 0.f, 1.f) : 1.f;
	if (LoadingWidget.IsValid())
	{
		LoadingWidget->SetCurtainOpacity(1.f - Alpha);
	}
	if (Alpha < 1.f)
	{
		return;
	}

	// _hide_loading_screen's ordering, which matters: the world is already
	// rendering underneath by the time the curtain starts fading, so the fade
	// reveals a live world rather than cutting to one.
	TeardownMenu();
	ReadyProbe.Reset();
	// The front end is the only thing that draws the background art, and it is
	// now finished with it -- six 1920-wide BGRA8 textures is roughly 48 MB to
	// be holding for the rest of a session on a project whose stated
	// constraint is the frame-time tail.
	FVoxelUIAssetLibrary::Get().ReleaseTextures();
	// And the track, which is 30-92 MB of decoded PCM. Stopped only now, after
	// the curtain is fully down: stopping it when the fade STARTED would cut
	// the fade off at its first frame, which sounds like a bug rather than a
	// choice. If MusicFadeOut is ever set longer than FadeDuration the tail is
	// clipped here, and that is the trade -- the front end does not outlive
	// itself to finish a fade.
	FVoxelUIMusic::Get().Stop();
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: handed off to the player."));
	State = EVoxelFrontEndState::Playing;
}

void UVoxelFrontEndSubsystem::TeardownMenu()
{
	// Every path out of the front end passes through here, including the
	// ones that never reach hand-off -- a quit from the menu, or a run that
	// ends while the loading screen is still up. Stop() is idempotent and
	// silent when nothing is playing, so this is the backstop rather than a
	// second owner of the decision.
	FVoxelUIMusic::Get().Stop();

	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (MenuWidget.IsValid())
	{
		if (Viewport != nullptr)
		{
			Viewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
		}
		MenuWidget.Reset();
	}
	if (HourglassShotWidget.IsValid())
	{
		if (Viewport != nullptr)
		{
			Viewport->RemoveViewportWidgetContent(HourglassShotWidget.ToSharedRef());
		}
		HourglassShotWidget.Reset();
	}
	if (LoadingWidget.IsValid())
	{
		if (Viewport != nullptr)
		{
			Viewport->RemoveViewportWidgetContent(LoadingWidget.ToSharedRef());
		}
		LoadingWidget.Reset();
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

	if (State == EVoxelFrontEndState::ArmLoading)
	{
		// Count DOWN frames, not seconds: the point is that the renderer has
		// actually presented the curtain, and at 8 FPS two frames is a quarter
		// of a second while at 200 FPS it is ten milliseconds. Frames are the
		// unit that means what is intended here.
		if (--ArmFrames <= 0)
		{
			StartWorldAndPawn();
			LoadElapsedSeconds = 0.f;
			State = EVoxelFrontEndState::Loading;
		}
		return;
	}

	if (State == EVoxelFrontEndState::Loading)
	{
		TickLoading(DeltaTime);
		return;
	}

	if (State == EVoxelFrontEndState::HandOff)
	{
		TickHandOff(DeltaTime);
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
		if (Switches.bHourglassShot && StateSeconds >= 2.0f)
		{
			// Two seconds: long enough for the grain field to fill and for a
			// mound to have formed at the mid-progress values, which an
			// immediate capture would miss entirely.
			CaptureAndQuit(TEXT("VoxelHourglass"));
			return;
		}

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

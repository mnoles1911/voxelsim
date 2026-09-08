#include "VoxelFrontEndSubsystem.h"
#include "VoxelSessionCheckpoint.h"
#include "VoxelAudioUserSettings.h"
#include "VoxelGraphicsUserSettings.h"
#include "VoxelPauseUISubsystem.h" // VoxelPauseUIHandoff
#include "VoxelSaveRows.h"

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
#include "VoxelFramePhase.h" // NoteMenuFrame -- HOOK 0, the menu's own frame-dist row
#include "VoxelMenuScalability.h" // the menu-state render drop, applied and released below
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
#include "HAL/IConsoleManager.h"        // the loading-screen streaming cap
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

// --- The loading-screen streaming cap ---------------------------------------
//
// WHAT MAKES THE LOADING SCREEN HITCH. Slate ticks and paints on the game
// thread, so the hourglass can only be as smooth as the frame, and during the
// initial cascade the frame belongs to streaming: DrainResults applies
// finished chunk meshes until voxel.Stream.ApplyBudgetMs (default 6.0) of
// wall clock is spent EVERY tick, and that cvar's own help text names the
// trade -- "a large budget during a load storm can hitch... Lower it to
// protect frame pacing at the cost of slower fill." That is precisely the
// lever wanted here, already built and already safe, so the front end sets it
// rather than inventing streaming machinery of its own.
//
// WHY THE TRADE IS FREE *HERE* AND ONLY HERE. The pre-directive screen
// revealed as soon as the world was ready (~6 s), so every ms of fill mattered
// and 6.0 was the right budget. Under the 30-60 s load theatre the world only
// has to beat a timer five times that long; a third of the fill rate still
// gets the R0-R3 gate open with tens of seconds to spare, and the player is
// watching the hourglass, not the fill. Restored at reveal -- the curtain
// starts lifting on a world that wants its full budget back -- with
// TeardownMenu as the backstop, and logged both ways so a leg's log shows
// exactly when the cap was on.
//
// voxel.Stream.MaxAppliesPerFrame is deliberately NOT touched: it is a count
// ceiling, not the steady-state throttle (VoxelDebug.cpp says so), and the
// apply-exit census depends on it staying put.
constexpr float kTheatreApplyBudgetMs = 2.0f;
const TCHAR* const kApplyBudgetCVarName = TEXT("voxel.Stream.ApplyBudgetMs");

// --- THE SECOND GAME-THREAD BURST: THE RASTER ATLAS SWEEP (2026-09-08) -------
//
// The 2026-09-08 live load's largest single game-thread item was not the apply
// budget. It was
//
//   [raster-atlas] window: served=33114 ... fills=1645 (205.62 MiB, 2016.4 ms GT)
//
// -- 2,016 ms of game thread inside a 5.00 s window, i.e. ~40% of every frame
// the hourglass had to paint in. Slate ticks and paints on the game thread, so
// that is the hourglass stopping.
//
// WHICH KNOB THIS IS, BECAUSE THE OBVIOUS ONE IS THE WRONG ONE. The line's own
// `cap=256/tick` is DemandPagesPerTick, and it bounds only the demand-rescue
// path -- which on that same window cost 9.4 ms of the 2,016. The other ~2,007
// ms is the page SWEEP, whose only bound is the per-tick deadline in
// FVoxelRasterAtlasCpu::Tick, i.e. FillBudgetMs(). So the sweep budget is what
// the theatre caps. Lowering the demand cap instead is not merely off-target,
// it is already MEASURED WORSE: VoxelRasterAtlas.cpp records that at 64 it
// produced capHit=1070/noAtlas=1070 by pushing declined chunks onto the inline
// full-window path. That lever is spent; this one is not.
//
// 2.0 -> 0.5, AND WHAT THAT ACTUALLY DOES. The deadline is tested BEFORE a page
// and never during, so any value below one page's cost degrades to EXACTLY ONE
// PAGE PER TICK -- 1.23 ms measured at the fine tier on that load. The load ran
// ~2.9 pages/tick; the theatre runs one. ~2 ms of game thread per frame goes
// back to Slate, the unfilled pages stay queued and the sweep collects them on
// later ticks. Nothing is dropped; the load gets LONGER and smoother, which is
// the trade the owner asked for in as many words ("Happy to have player sit on
// loading screen for more than a minute ... However, the loading screen should
// not feel chunky or hitching").
//
// 0.5 RATHER THAN 0. Zero would reach the same one-page floor, but a budget of
// zero reads like "off" to the next person and this is not off -- the sweep
// must keep running or the world never warms. 0.5 says "one page" in a unit
// the cvar's help text explains.
//
// RESTORED AT THE REVEAL, exactly as the apply budget is, through the same two
// callers (the reveal and TeardownMenu as the backstop).
constexpr float kTheatreAtlasFillMs = 0.5f;
const TCHAR* const kAtlasFillCVarName = TEXT("voxel.Stream.AtlasFillMs");
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
	// And the player's volumes, for exactly the same reason and on the same
	// side of the suppression branch: a suppressed front end is a run where the
	// player is playing, and their audio choices must land either way.
	VoxelAudioUserSettings::ApplyAll();

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
	// menu tick gate 2026-09-07 (docs/backlog.md §0.0o-adjacent): the latched
	// A/B value, printed once so a leg's own log proves which arm ran rather
	// than relying on the command line the leg was launched with.
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: menu tick gates=%d."),
	       VoxelFrontEnd::MenuTickGatesEnabled() ? 1 : 0);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: menu font %s."),
	       FVoxelUIStyle::Get().IsProjectFontAvailable() ? TEXT("loaded") : TEXT("FALLBACK (engine default face)"));
	// Phase 4, 2026-09-07: which thread will paint the loading curtain. The
	// LATCHED SWITCH VALUE, printed at init beside the other arms, so a leg's
	// own log proves which arm ran rather than relying on the command line it
	// was launched with. The second half -- whether this process can actually
	// honour it -- is printed by FVoxelLoadingCurtainThread::Begin as
	// "LoadScreen: curtain thread requested=N available=...", because the
	// answer is not known until there is a viewport.
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: loading screen thread=%d"),
	       FVoxelFrontEndSwitches::Get().bLoadingScreenThread ? 1 : 0);
	// 2026-09-08, printed here for the same reason the curtain thread is: the
	// LATCHED value beside the other arms, so a log says which arm ran. The
	// second half -- whether the pre-warm actually reached a world subsystem
	// and what it committed -- is printed by EnterMenu and by
	// UVoxelWorldSubsystem::PrewarmGpuPools, because that is not known until
	// there is a menu.
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: menu GPU pool pre-warm=%d"),
	       FVoxelFrontEndSwitches::Get().bMenuPrewarm ? 1 : 0);
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

	// AFTER THE CURTAIN IS UP, NOT BEFORE IT. SVoxelMainMenu's bottom overlay
	// slot is an opaque full-viewport fill, and viewport widgets composite
	// after the scene render, so from this line on every pixel the renderer
	// produces is painted over. The drop makes those pixels cheap and cannot
	// change what is on screen. Released at the reveal, beside
	// RestoreStreamingBudget in TickLoading, with TeardownMenu as the backstop
	// for every path that never reaches one -- see VoxelMenuScalability.h.
	VoxelMenuScalability::Apply();

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
		// The title screen is the Menu pool (docs/music-design.md section 2,
		// row 5). Set BEFORE StartRandom, because StartRandom resolves the pool
		// from the context it finds.
		FVoxelUIMusic::Get().SetContext(EVoxelMusicContext::Menu);
		FRandomStream MusicStream = MakeVoxelUIRandomStream();
		FVoxelUIMusic::Get().StartRandom(GetWorld(), MusicStream);
	}

	// --- PRE-WARM THE GPU POOLS UNDER THE MENU (2026-09-08) -----------------
	//
	// AFTER the menu widget is on screen and the scalability drop is applied,
	// so the commit lands behind a curtain that is already up and already
	// cheap to draw; and on the Menu path only, which is what keeps it off
	// every -unattended leg (those never reach EnterMenu at all -- see
	// VoxelFrontEnd::IsEnabledThisRun above).
	//
	// NON-BLOCKING BY CONTRACT. PrewarmGpuPools enqueues render commands and
	// returns; it must never flush. The whole reason the threaded curtain is
	// off by default is that a blocking wait on the render thread hung the
	// owner's session on 2026-09-07 (VoxelLoadingCurtainThread.h), and this is
	// not the file to re-learn that in.
	//
	// The world stays HELD: ChunkOwner is still null after this line, nothing
	// is admitted, no edit log is read. Only the allocation moves.
	if (FVoxelFrontEndSwitches::Get().bMenuPrewarm)
	{
		if (UVoxelWorldSubsystem* WorldSub = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			WorldSub->PrewarmGpuPools();
		}
		else
		{
			// The subsystem is how the menu reaches anything at all; if it is
			// missing the pre-warm is a silent no-op, and a silent no-op is the
			// house failure mode on this project. Say it.
			UE_LOG(LogVoxelUI, Warning,
			       TEXT("VoxelFrontEnd: -VoxelMenuPrewarm is on but there is no UVoxelWorldSubsystem in this ")
			       TEXT("world; the GPU pools were NOT pre-warmed and the commit stays inside the load."));
		}
	}
	else
	{
		UE_LOG(LogVoxelUI, Log,
		       TEXT("VoxelFrontEnd: GPU pool pre-warm OFF (-VoxelMenuPrewarm=0); the control arm -- the ")
		       TEXT("brick-pool arenas commit inside the first streaming frames, as they did before."));
	}

	State = EVoxelFrontEndState::Menu;
	StateSeconds = 0.f;
	bMenuInputApplied = false;

	// THE PAUSE MENU'S LOAD LANDS HERE. There is no world teardown in this
	// project, so UVoxelPauseUISubsystem::ReturnToMenu reopens the map and
	// leaves the chosen slug behind; this is the far side of that journey. The
	// menu is built and on screen first, so that if the load is refused (the
	// save was deleted between the click and the reopen, say) the player is
	// looking at the title screen rather than at nothing.
	//
	// Consumed unconditionally, even when empty, so a plain EXIT TO MENU cannot
	// leave a stale slug for the next visit.
	if (const FString PendingSlug = VoxelPauseUIHandoff::ConsumePendingLoadSlug(); !PendingSlug.IsEmpty())
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: resuming a pause-menu LOAD of '%s'."), *PendingSlug);
		RequestLoad(PendingSlug);
	}
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

	// THE CONVERSION MOVED, NOT THE BEHAVIOUR. The pause overlay's LOAD dialog
	// shows the same saves, and the two judgements this loop used to carry --
	// metres, and the seed-mismatch reason -- must not be made differently on
	// two screens. See VoxelSaveRows.h.
	TArray<FVoxelSaveRowInfo> Rows = VoxelSaveRows::Build(WorldSub ? WorldSub->GetSeed() : 0);
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

	// Phase 4: from here the curtain may also be painted on the engine's Slate
	// loading thread while the game thread is inside a long world tick. THE
	// SAME WIDGET INSTANCE, moved out of the viewport and back per armed frame
	// -- a second instance would shuffle its own backgrounds and tips and the
	// picture would jump every time a block started. See
	// VoxelLoadingCurtainThread.h for the mechanism, and for why the answer is
	// not "keep a loading thread up for the whole load".
	CurtainThread.Begin(World, Viewport, LoadingWidget.ToSharedRef(),
	                    VoxelFrontEndDetail::kLoadingZOrder,
	                    FVoxelFrontEndSwitches::Get().bLoadingScreenThread);

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
	LoadWallStartSeconds = FPlatformTime::Seconds();
	LoadWallSeconds = 0.f;
	LastProgress = 0.f;
	NextLoadingShotIndex = 0;
	WorldReadyAtSeconds = -1.f;

	// THE ARTIFICIAL LOAD DURATION (owner directive, 2026-09-05). A uniform
	// roll in [LoadTheatreMin, LoadTheatreMax] -- 30-60 s by default, random so
	// consecutive loads feel different. Seeded from the WALL CLOCK, never the
	// world seed: this is theatre, not simulation, and two players on the same
	// seed deserve different shows. MakeVoxelUIRandomStream is exactly that
	// contract already (clock-seeded interactively, fixed under -unattended so
	// the capture strips stay diffable), so the roll shares it with the
	// backgrounds rather than inventing a second seeding rule.
	{
		FRandomStream TheatreStream = MakeVoxelUIRandomStream();
		const FVoxelFrontEndSwitches& Switches = FVoxelFrontEndSwitches::Get();
		TheatreDurationSeconds = TheatreStream.FRandRange(Switches.LoadTheatreMinSeconds,
		                                                  Switches.LoadTheatreMaxSeconds);
		// The engagement evidence, greppable, first of the pair TickLoading's
		// reveal line completes. A gate can fail on its absence.
		UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: theatre duration %.1f s rolled."), TheatreDurationSeconds);
	}

	// Normally a no-op: the menu started a track and this is the same session,
	// so the loading screen inherits it rather than restarting -- which is the
	// whole point of "adopt at BeginLoad". It matters for the path where a
	// loading screen goes up WITHOUT a menu in front of it, which no switch
	// takes today and which would otherwise be silently music-less.
	{
		// The loading curtain shares the Menu pool with the title screen
		// (docs/music-design.md section 2, row 5: "title and loading screens"),
		// so this is a context change and NOT a pool change -- the cue the menu
		// started keeps playing across it, which is what "adopt at BeginLoad"
		// has meant since ADR-0009.
		FVoxelUIMusic::Get().SetContext(EVoxelMusicContext::Loading);
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
	if (VoxelSessionCheckpoint::Failed(World)) return;
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
	ProbeConfig.bRequireFineRing = Switches.bLoadGateFineRing;
	// THE GATE'S OWN CEILING, not the curtain's hold (2026-09-07). These were
	// the same number until Phase 4 and are two different questions; see
	// FVoxelFrontEndSwitches::LoadGateMaxWaitSeconds.
	ProbeConfig.MaxWaitSeconds = Switches.LoadGateMaxWaitSeconds;

	FVector Anchor = FVector::ZeroVector;
	if (PendingSpawnTransform.IsSet())
	{
		Anchor = PendingSpawnTransform->GetLocation();
	}
	else
	{
		double SpawnX = 0.0;
		double SpawnY = 0.0;
		// Resolve, not Parse (2026-09-07): the spawn rule moves an ordinary
		// launch to a fine-baked column, and the 112 probes must ring THAT
		// column, not the origin.
		VoxelEarthSpawn::ResolveSpawnColumnUU(World, SpawnX, SpawnY);
		Anchor = FVector(SpawnX, SpawnY, WorldSub->GetSurfaceHeightUU(SpawnX, SpawnY));
	}

	ReadyProbe = MakeUnique<FVoxelWorldReadyProbe>();
	ReadyProbe->Start(Anchor, ProbeConfig);

	// The streaming session is live as of StartWorldSession above, so this is
	// the first moment the cap has anything to cap.
	CapStreamingForTheatre();
}

void UVoxelFrontEndSubsystem::CapStreamingForTheatre()
{
	// TWO INDEPENDENT CAPS, IN TWO INDEPENDENT SCOPES, and that is load-bearing
	// rather than tidiness: the apply-budget block has two early exits (cvar
	// missing, already tighter) and if they were `return`s from this function
	// they would silently skip the atlas cap below. A run with a hand-tuned
	// -VoxelApplyBudgetMs would then get half the theatre and no line saying so.
	{
		// The rationale, the lever, and the trade all live on the constants --
		// see VoxelFrontEndDetail at the top of this file.
		IConsoleVariable* BudgetVar =
			IConsoleManager::Get().FindConsoleVariable(VoxelFrontEndDetail::kApplyBudgetCVarName);
		if (BudgetVar == nullptr)
		{
			// The cvar is owned by VoxelEarth and looked up by NAME, so a rename
			// there must degrade to "no cap" here, loudly, not to a crash.
			UE_LOG(LogVoxelUI, Warning, TEXT("LoadScreen: %s not found; loading runs uncapped."),
			       VoxelFrontEndDetail::kApplyBudgetCVarName);
		}
		else if (BudgetVar->GetFloat() <= VoxelFrontEndDetail::kTheatreApplyBudgetMs)
		{
			// Somebody already runs tighter than the theatre cap; capping would
			// RAISE their budget on restore-order mishaps. Leave it alone.
		}
		else
		{
			SavedApplyBudgetMs = BudgetVar->GetFloat();
			bStreamBudgetCapped = true;
			BudgetVar->Set(VoxelFrontEndDetail::kTheatreApplyBudgetMs, ECVF_SetByCode);
			UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: capped %s %.1f -> %.1f for the load theatre."),
			       VoxelFrontEndDetail::kApplyBudgetCVarName, SavedApplyBudgetMs,
			       VoxelFrontEndDetail::kTheatreApplyBudgetMs);
		}
	}

	// --- The atlas sweep, same shape, same save/restore ---------------------
	//
	// See kTheatreAtlasFillMs for which knob this is and why it is not the
	// `cap=256/tick` the atlas line prints. -1 is the cvar's "use the latched
	// -VoxelGpuRasterAtlasFillMs" sentinel, so a run that never entered the
	// theatre is byte-identical to one built before this change -- and it is
	// also why the "already tighter" test below tests `>= 0` first: a negative
	// value is not a tighter budget, it is no budget set at all.
	{
		IConsoleVariable* AtlasVar =
			IConsoleManager::Get().FindConsoleVariable(VoxelFrontEndDetail::kAtlasFillCVarName);
		if (AtlasVar == nullptr)
		{
			// Owned by VoxelEarth and looked up by NAME. A rename there must
			// degrade to "no cap", loudly -- same contract as the apply budget.
			UE_LOG(LogVoxelUI, Warning, TEXT("LoadScreen: %s not found; the atlas sweep runs uncapped."),
			       VoxelFrontEndDetail::kAtlasFillCVarName);
		}
		else if (AtlasVar->GetFloat() >= 0.f
		         && AtlasVar->GetFloat() <= VoxelFrontEndDetail::kTheatreAtlasFillMs)
		{
			// Already at or under the theatre cap; leave it alone.
		}
		else
		{
			SavedAtlasFillMs = AtlasVar->GetFloat();
			bAtlasFillCapped = true;
			AtlasVar->Set(VoxelFrontEndDetail::kTheatreAtlasFillMs, ECVF_SetByCode);
			UE_LOG(LogVoxelUI, Log,
			       TEXT("LoadScreen: capped %s %.2f -> %.2f for the load theatre (a negative saved value is ")
			       TEXT("the 'use -VoxelGpuRasterAtlasFillMs' sentinel, not a budget). The sweep now fills ")
			       TEXT("ONE page per tick; `[raster-atlas] fill` carries the page count that proves it."),
			       VoxelFrontEndDetail::kAtlasFillCVarName, SavedAtlasFillMs,
			       VoxelFrontEndDetail::kTheatreAtlasFillMs);
		}
	}
}

void UVoxelFrontEndSubsystem::RestoreStreamingBudget()
{
	// Idempotent, because it has two callers by design: the reveal (the normal
	// path) and TeardownMenu (the backstop for a quit or world teardown while
	// the screen is still up).
	//
	// TWO INDEPENDENT FLAGS, no early return between them, for the same reason
	// CapStreamingForTheatre uses two scopes: either cap may have declined to
	// engage, and a shared `return` would leave the other one applied for the
	// rest of the session. A cap that outlives the theatre is worse than one
	// that never ran.
	if (bStreamBudgetCapped)
	{
		bStreamBudgetCapped = false;
		if (IConsoleVariable* BudgetVar =
		        IConsoleManager::Get().FindConsoleVariable(VoxelFrontEndDetail::kApplyBudgetCVarName))
		{
			BudgetVar->Set(SavedApplyBudgetMs, ECVF_SetByCode);
			UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: restored %s to %.1f."),
			       VoxelFrontEndDetail::kApplyBudgetCVarName, SavedApplyBudgetMs);
		}
	}
	if (bAtlasFillCapped)
	{
		bAtlasFillCapped = false;
		if (IConsoleVariable* AtlasVar =
		        IConsoleManager::Get().FindConsoleVariable(VoxelFrontEndDetail::kAtlasFillCVarName))
		{
			// Put back EXACTLY what was there, sentinel included: restoring -1
			// hands the budget back to the latched -VoxelGpuRasterAtlasFillMs,
			// which is what an ordinary run had before the theatre.
			AtlasVar->Set(SavedAtlasFillMs, ECVF_SetByCode);
			UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: restored %s to %.2f."),
			       VoxelFrontEndDetail::kAtlasFillCVarName, SavedAtlasFillMs);
		}
	}
}

void UVoxelFrontEndSubsystem::TickLoading(float DeltaSeconds)
{
    if (GetWorld() && GetWorld()->GetNetMode()!=NM_Client)
    {
        if (VoxelSessionCheckpoint::Failed(GetWorld()))
        {
            if (LoadingWidget.IsValid()) LoadingWidget->SetLoadFailed();
            return;
        }
        if (!VoxelSessionCheckpoint::Ready(GetWorld())) return;
    }
	LoadElapsedSeconds += DeltaSeconds;
	LoadWallSeconds = LoadWallStartSeconds > 0.0
	                      ? float(FPlatformTime::Seconds() - LoadWallStartSeconds)
	                      : LoadElapsedSeconds;

	// HOOK 0b (Phase 4, 2026-09-07): the CURTAIN'S OWN PAINT CADENCE into
	// VoxelFramePhase's seg=LOADING row -- not this tick's DeltaSeconds.
	//
	// THE DIFFERENCE IS THE ENTIRE POINT. The owner has said the game thread may
	// take as long as it needs behind the curtain; what must stay smooth is how
	// often the hourglass is redrawn. Under -VoxelLoadingScreenThread=1 those
	// are different numbers, because the Slate loading thread paints across a
	// long world tick; under =0 they are the same number and the row reads
	// seconds. Drained here rather than pushed from the widget because
	// VoxelFramePhase's buckets and its 5 s flush are game-thread state and the
	// loading thread is one of the painters.
	VoxelLoadingCurtain::DrainPaintIntervals([](double IntervalMs)
	{
		VoxelFramePhase::NoteLoadingFrame(IntervalMs);
	});

	UWorld* World = GetWorld();
	const UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (ReadyProbe.IsValid() && WorldSub != nullptr)
	{
		ReadyProbe->Tick(DeltaSeconds, *WorldSub);
	}

	// The world's gate: genuinely ready, or the probe's own MaxWait expired
	// and the curtain lifts anyway (the probe logged that arm as a Warning).
	// One bit is all the bar gets to know about streaming -- the model and its
	// reveal semantics are documented on ComputeTheatreProgress.
	const bool bReady = ReadyProbe.IsValid() && ReadyProbe->IsReady();
	const bool bTimedOut = ReadyProbe.IsValid() && ReadyProbe->HasTimedOut();
	const bool bGateOpen = bReady || bTimedOut;
	if (bGateOpen && WorldReadyAtSeconds < 0.f)
	{
		WorldReadyAtSeconds = LoadElapsedSeconds;
	}

	// The bar plays the rolled theatre duration out, eased. A zero duration
	// (-VoxelLoadTheatre=0) degenerates to "full as soon as the gate opens",
	// which is the pre-directive timing.
	// ON THE WALL CLOCK, NOT THE TICK CLOCK (2026-09-07 evening). The rolled
	// duration is "how long the player watches the show", and LoadElapsedSeconds
	// is not that: it accumulates the engine's CLAMPED tick delta, which on the
	// owner's live load ran at about one seventh of wall time -- the readiness
	// probe, on the same clock, printed "READY after 16.02s" 119.9 wall seconds
	// after it started. A 41.4 s theatre against that clock is five real
	// minutes of hourglass, which is what the owner was looking at.
	const float TheatreFraction = TheatreDurationSeconds > 0.f
	                                  ? LoadWallSeconds / TheatreDurationSeconds
	                                  : 1.f;
	LastProgress = ComputeTheatreProgress(TheatreFraction, bGateOpen, LastProgress);
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

	// THE REVEAL CONTRACT (owner directive, 2026-09-05), which supersedes the
	// ported TransitionManager hold: the world is revealed at
	// max(artificial timer elapsed, world actually ready). A world that beats
	// the timer waits behind the curtain while the theatre plays out -- the
	// owner's explicit intent. A world slower than the timer holds the bar at
	// ~97% (the model's cap above) with the hourglass still animating until
	// the gate opens. LoadMinHoldSeconds still backstops the warm-cache flash
	// when the theatre is overridden shorter than it (-VoxelLoadTheatre=0).
	const float MinimumSeconds = FMath::Max(TheatreDurationSeconds, Switches.LoadMinHoldSeconds);
	if (LoadWallSeconds >= MinimumSeconds && bGateOpen)
	{
		// The second half of the engagement evidence; BeginLoad logged the
		// roll. Greppable, and a gate can fail on either line's absence.
		if (bReady)
		{
			UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: world ready at %.1f s (%s timer), revealing at %.1f s."),
			       WorldReadyAtSeconds, WorldReadyAtSeconds <= TheatreDurationSeconds ? TEXT("before") : TEXT("after"),
			       LoadElapsedSeconds);
		}
		else
		{
			// The timeout arm gets its own wording rather than borrowing the
			// contract line: "world ready" would be the exact lie the probe
			// just warned about.
			UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: world NOT ready (gate timed out) at %.1f s, revealing at %.1f s."),
			       WorldReadyAtSeconds, LoadElapsedSeconds);
		}
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: closing the curtain after %.2fs wall (%.2fs ticked) (%s)."),
		       LoadWallSeconds, LoadElapsedSeconds,
		       bReady ? TEXT("world ready") : TEXT("timed out"));

		// THE CURTAIN THREAD STOPS HERE, AT THE REVEAL -- not at the end of
		// teardown. TeardownMenu is the backstop and runs after the 0.4 s fade;
		// leaving the mechanism live across that fade means a long frame during
		// it can arm a block while TickHandOff is animating the very widget the
		// block moves out of the viewport. End() latches first and is
		// idempotent, so calling it here and again in TeardownMenu is free.
		// After the 2026-09-07 hang the rule is: stop it BEFORE anything else
		// touches the curtain.
		CurtainThread.End();

		// The world is about to be on screen and wants its full apply budget
		// back before the fade starts, not after it finishes.
		RestoreStreamingBudget();
		// And its full RENDER quality, for the same reason and at the same
		// instant: the curtain is about to lift, so the fade must reveal the
		// world the player will play rather than the cheap one that was
		// hiding behind it.
		VoxelMenuScalability::Restore();

		// 100% is claimed exactly here -- the gate has passed and the curtain
		// is starting to lift, so the bar shows full only during the fade,
		// never frozen.
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

	// THE MUSIC NO LONGER LEAVES WITH THE CURTAIN BY DEFAULT (owner directive,
	// 2026-09-07: "by default, music from the game's soundtrack/library should
	// play when in game"). What used to be an unconditional fade-and-stop is now
	// a read of one persisted setting, and the two arms are genuinely different
	// journeys rather than the same one with a flag:
	//
	//   CONTINUING -- nothing is faded, nothing is stopped, and the track the
	//     player has been listening to since the title screen simply keeps
	//     playing over the world. The decoded PCM (30-92 MB) stays resident for
	//     the session; see FVoxelUIMusic's header, where that was previously
	//     described as released here.
	//   NOT CONTINUING -- exactly the old behaviour, fade then stop.
	//
	// Read ONCE, into a local, rather than at each of the three sites below. The
	// setting is player-editable ini and could in principle change between them;
	// a hand-off that faded the music out and then declined to stop it would
	// leave a live silent component for the rest of the session.
	const bool bContinueMusic = VoxelAudioUserSettings::GetMusicInGame();

	// The fade is started ONCE, on the first hand-off tick, and runs alongside
	// the curtain rather than after it. MusicFadeOut (1.5s) is independent of
	// FadeDuration on purpose: the picture and the sound do not have to leave
	// at the same rate, and the Godot build's music outlived its curtain.
	if (!bMusicFadeStarted)
	{
		bMusicFadeStarted = true;
		if (bContinueMusic)
		{
			// The engagement evidence for the continue arm, greppable, and a
			// gate can fail on its absence. Without it "there is music in game"
			// and "the fade simply did not run" look identical in a log.
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: continuing into gameplay ('%s')."),
			       *FVoxelUIMusic::Get().NowPlaying());
		}
		else
		{
			FVoxelUIMusic::Get().FadeOut(L.MusicFadeOut);
		}
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
	TeardownMenu(/*bKeepMusic=*/bContinueMusic);
	ReadyProbe.Reset();
	// The front end is the only thing that draws the background art, and it is
	// now finished with it -- six 1920-wide BGRA8 textures is roughly 48 MB to
	// be holding for the rest of a session on a project whose stated
	// constraint is the frame-time tail.
	FVoxelUIAssetLibrary::Get().ReleaseTextures();
	if (!bContinueMusic)
	{
		// The track, which is 30-92 MB of decoded PCM. Stopped only now, after
		// the curtain is fully down: stopping it when the fade STARTED would cut
		// the fade off at its first frame, which sounds like a bug rather than a
		// choice. If MusicFadeOut is ever set longer than FadeDuration the tail
		// is clipped here, and that is the trade -- the front end does not
		// outlive itself to finish a fade.
		FVoxelUIMusic::Get().Stop();
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelFrontEnd: handed off to the player."));
	State = EVoxelFrontEndState::Playing;
}

void UVoxelFrontEndSubsystem::TeardownMenu(bool bKeepMusic)
{
	// FIRST, and before the loading widget is removed below: End() disarms any
	// in-flight block and puts the curtain back in the viewport, so the removal
	// that follows is removing a widget that is actually there. Idempotent, and
	// this is the backstop for every path that never reaches hand-off.
	CurtainThread.End();

	// Every path out of the front end passes through here, including the
	// ones that never reach hand-off -- a quit from the menu, or a run that
	// ends while the loading screen is still up. Stop() is idempotent and
	// silent when nothing is playing, so this is the backstop rather than a
	// second owner of the decision.
	//
	// EXCEPT ON THE ONE PATH THAT IS NOT AN EXIT. Hand-off calls this to take
	// the menu off the screen while the player carries on into a world that is
	// meant to keep the music -- so the backstop has to know the difference
	// between "the front end is finished" and "the front end is finished WITH
	// THE SCREEN". Everything else here (the curtain, the streaming cap, the
	// scalability drop, the widgets) is torn down on both paths; only the audio
	// outlives one of them.
	if (!bKeepMusic)
	{
		FVoxelUIMusic::Get().Stop();
	}

	// Same backstop shape for the streaming cap: the cvar is process-wide, so
	// a quit or PIE teardown mid-theatre must not leave the apply budget
	// throttled for the next session in the same process. No-op when the
	// reveal already restored it.
	RestoreStreamingBudget();
	// The scalability drop is process-wide for exactly the same reason and
	// gets exactly the same backstop: a quit from the menu, a PIE stop or a
	// Deinitialize must not leave r.ScreenPercentage at 25 for whatever runs
	// next in this process. No-op when the reveal already released it.
	VoxelMenuScalability::Restore();

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
		// HOOK 0 (VoxelFramePhase.h): the menu's own "Voxel frame dist"
		// row, seg=MENU. First in the block, ahead of every branch below
		// (settle-then-capture, autostart, the watchdog) so a leg that quits
		// early via -VoxelMenuShot still banks every Menu-state frame it was
		// actually ticked, not just the ones before the first early return.
		VoxelFramePhase::NoteMenuFrame(double(DeltaTime) * 1000.0);

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

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
#include "VoxelEarthUI.h"
#include "VoxelFrontEndSubsystem.h"
#include "VoxelFrontEndSwitches.h"
#include "VoxelSurvivalUISubsystem.h"
#include "VoxelUIAssetLibrary.h"
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
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
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
// The HUD sits below every menu and above the survival hotbar's old 15, so that
// while the two coexist the new one is the visible layer.
constexpr int32 kHudZOrder = 18;
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
	PC->PushInputComponent(Input);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelScreens: M/J/I/P/K bound."));
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
	ApplyOverlayInput(Shell.ToSharedRef());
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

	TSharedRef<SWidget> Body = SNullWidget::NullWidget;
	TArray<FVoxelScreenAction> Actions;
	switch (Tab)
	{
	case EVoxelScreenTab::Map:
	{
		const FVoxelMapScreenData MapData = GatherMapData();
		Actions = SVoxelMapScreen::Actions(MapData.bHasTerrainRaster);
		Body = SNew(SVoxelMapScreen).Data(MapData);
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
	ApplyOverlayInput(DeathScreen.ToSharedRef());
	DeathScreen->FocusDefaultWidget();
}

void UVoxelScreensUISubsystem::OpenDialogue(const FVoxelDialogueData& Node)
{
	if (!CanShow() || Dialogue.IsValid())
	{
		return;
	}
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
	ApplyOverlayInput(Dialogue.ToSharedRef());
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

void UVoxelScreensUISubsystem::ApplyOverlayInput(TSharedRef<SWidget> Widget)
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
		// The HUD would otherwise sit under the scrim, dimmed but legible --
		// which is not what any of the mocks show.
		Hud->SetVisibility(EVisibility::Collapsed);
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
		Hud->SetVisibility(EVisibility::HitTestInvisible);
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

	// The raster is offered only when the seed matches the directory it was
	// rendered for. See SVoxelMapScreen's header: a hillshade of a different
	// world is not a rough map, it is a wrong one.
	const FString RasterPath = SVoxelMapScreen::RasterPathForSeed(Out.Seed);
	Out.bHasTerrainRaster = FVoxelUIAssetLibrary::Get().RequestImageFile(RasterPath) != nullptr
	                     || IFileManager::Get().FileExists(*RasterPath);
	return Out;
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

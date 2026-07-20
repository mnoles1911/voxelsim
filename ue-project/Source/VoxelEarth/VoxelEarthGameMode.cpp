#include "VoxelEarthGameMode.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "VoxelEarth.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelOceanActor.h"
#include "VoxelWorldSubsystem.h"

AVoxelEarthGameMode::AVoxelEarthGameMode()
{
	DefaultPawnClass = AVoxelEarthFlyPawn::StaticClass();
	PlayerControllerClass = AVoxelEarthPlayerController::StaticClass();
}

void AVoxelEarthGameMode::BeginPlay()
{
	Super::BeginPlay();

	// No authored map yet (Entry is empty): spawn the light rig from code so
	// the voxel world is actually lit — sun + sky light + atmosphere.
	UWorld* World = GetWorld();
	if (World)
	{
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
		if (Sun)
		{
			Sun->SetActorRotation(FRotator(-45.f, 30.f, 0.f));
			if (UDirectionalLightComponent* SunComp =
			        Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				SunComp->SetIntensity(8.f);
				SunComp->SetAtmosphereSunLight(true);
			}
		}
		if (ASkyLight* Sky = World->SpawnActor<ASkyLight>())
		{
			if (USkyLightComponent* SkyComp = Sky->GetLightComponent())
			{
				SkyComp->SetRealTimeCaptureEnabled(true);
			}
		}
		if (AActor* Atmosphere = World->SpawnActor<AActor>())
		{
			USkyAtmosphereComponent* AtmosphereComp =
				NewObject<USkyAtmosphereComponent>(Atmosphere);
			AtmosphereComp->RegisterComponent();
			Atmosphere->SetRootComponent(AtmosphereComp);
		}

		// Water track W1 (docs/voxel-earth-implementation-plan.md SS3.7 /
		// SS4): same "no authored map, spawn from code" reasoning as the
		// light rig above -- the ocean actor is editor-independent.
		World->SpawnActor<AVoxelOceanActor>();
	}

	// Unattended visual verification: -VoxelScreenshotAfter=<seconds> waits
	// for streaming to populate, captures a screenshot, then quits ~3s later
	// (screenshot write is async). Drives phase-verification captures from
	// scripts/CI without editor tooling.
	float DelaySeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelScreenshotAfter="), DelaySeconds) && DelaySeconds > 0.f)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("Screenshot verification run: capturing in %.1fs"), DelaySeconds);
		GetWorldTimerManager().SetTimer(
			ScreenshotTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					// Aim down toward the terrain well before capturing, on
					// both the controller AND the pawn (belt and braces —
					// the first capture attempt showed control rotation
					// alone not reflected in the captured view).
					APlayerController* PC = GetWorld()->GetFirstPlayerController();
					if (PC)
					{
						PC->SetControlRotation(FRotator(-40.f, 45.f, 0.f));
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorRotation(FRotator(-40.f, 45.f, 0.f));
						}
					}
					// Two captures, 2s apart, with the camera pose logged at
					// each request so a framing failure is diagnosable from
					// the log alone.
					auto Capture = [this]()
					{
						if (APlayerController* Ctrl = GetWorld()->GetFirstPlayerController())
						{
							if (Ctrl->PlayerCameraManager)
							{
								const FVector Loc = Ctrl->PlayerCameraManager->GetCameraLocation();
								const FRotator Rot = Ctrl->PlayerCameraManager->GetCameraRotation();
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("Capture: cam loc=(%.0f, %.0f, %.0f) rot=(pitch %.1f yaw %.1f)"),
								       Loc.X, Loc.Y, Loc.Z, Rot.Pitch, Rot.Yaw);
							}
						}
						FScreenshotRequest::RequestScreenshot(TEXT("VoxelVerify"), false, true);
					};
					GetWorldTimerManager().SetTimer(
						ScreenshotTimerHandle, FTimerDelegate::CreateWeakLambda(this, Capture), 1.f, false);
					GetWorldTimerManager().SetTimer(
						SecondShotTimerHandle, FTimerDelegate::CreateWeakLambda(this, Capture), 3.f, false);
					GetWorldTimerManager().SetTimer(
						QuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }),
						6.f, false);
				}),
			DelaySeconds, false);
	}
}

void AVoxelEarthGameMode::RestartPlayer(AController* NewPlayer)
{
	// docs/m1-plan.md Stage 2 decisions table item 3: spawn above the
	// terrain surface (Amplifier column at 0,0), +5m -- rather than via
	// FindPlayerStart/APlayerStart, since no level in this repo places one
	// yet. Falls back to the default PlayerStart-based flow if the voxel
	// world subsystem isn't available for some reason.
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!NewPlayer || !Subsystem)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	// Water track W1 verification aid (same pattern as VoxelWorldSubsystem's
	// -VoxelDefaultMaterial switch): an unattended -game run can't drive the
	// pawn into the ocean by hand, so this switch spawns underwater instead
	// of above the terrain -- purely to observe AVoxelOceanActor's
	// above/below transition log line without an interactive session. No
	// effect unless passed explicitly; normal spawn behavior is unchanged.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelForceUnderwaterSpawn")))
	{
		constexpr double UnderwaterSpawnDepthUU = -500.0; // -5m, well below sea level (z=0)
		const FVector SpawnLocation(0.0, 0.0, UnderwaterSpawnDepthUU);
		const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);
		RestartPlayerAtTransform(NewPlayer, SpawnTransform);
		return;
	}

	constexpr double SpawnHeightAboveSurfaceUU = 500.0; // +5m (1 UU = 1 cm)

	// -VoxelSpawnAt=X,Y (meters, world): stage 3c LWC verification switch --
	// overrides the spawn column with the same surface-height-query-plus-5m
	// logic used for the default (0,0) column above, just evaluated at an
	// arbitrary far-from-origin column. Default behavior (spawn at 0,0) is
	// unchanged when the switch is absent.
	double SpawnWorldX = 0.0;
	double SpawnWorldY = 0.0;
	FString SpawnAtArg;
	// bShouldStopOnSeparator=false: FParse::Value's default terminator set
	// includes ',' (it's meant for stopping at the end of one positional
	// value in a list), which would truncate "X,Y" at the comma and silently
	// drop Y. This switch's value is the whole "X,Y" pair, so read up to the
	// next whitespace instead.
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSpawnAt="), SpawnAtArg, /*bShouldStopOnSeparator=*/false))
	{
		FString XStr, YStr;
		if (SpawnAtArg.Split(TEXT(","), &XStr, &YStr))
		{
			const double SpawnMetersX = FCString::Atod(*XStr);
			const double SpawnMetersY = FCString::Atod(*YStr);
			SpawnWorldX = SpawnMetersX * 100.0; // meters -> UU (1 UU = 1 cm)
			SpawnWorldY = SpawnMetersY * 100.0;
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSpawnAt override: spawning at column (%.1f, %.1f) m"),
			       SpawnMetersX, SpawnMetersY);
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("-VoxelSpawnAt=%s malformed (expected X,Y in meters); falling back to (0,0)."), *SpawnAtArg);
		}
	}

	const double SurfaceUU = Subsystem->GetSurfaceHeightUU(SpawnWorldX, SpawnWorldY);
	const FVector SpawnLocation(SpawnWorldX, SpawnWorldY, SurfaceUU + SpawnHeightAboveSurfaceUU);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	RestartPlayerAtTransform(NewPlayer, SpawnTransform);
}

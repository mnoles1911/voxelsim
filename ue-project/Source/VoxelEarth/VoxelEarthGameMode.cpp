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

	constexpr double SpawnHeightAboveSurfaceUU = 500.0; // +5m (1 UU = 1 cm)
	const double SurfaceUU = Subsystem->GetSurfaceHeightUU(0.0, 0.0);
	const FVector SpawnLocation(0.0, 0.0, SurfaceUU + SpawnHeightAboveSurfaceUU);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	RestartPlayerAtTransform(NewPlayer, SpawnTransform);
}

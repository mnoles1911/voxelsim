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
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelClipmapActor.h"
#include "VoxelEarthHUD.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelOceanActor.h"
#include "VoxelWorldSubsystem.h"

namespace
{
// -VoxelSpawnAt=X,Y (meters, world): shared by RestartPlayer's pawn spawn
// column AND BeginPlay's SkyAtmosphere placement (M2 task "SkyAtmosphere
// origin fix" -- see BeginPlay) so both land on the EXACT same column and
// can never drift apart. Returns false (Out* left at 0,0 UU) if the switch
// is absent; true with parsed meters->UU values if present and well-formed.
// Malformed input logs a warning and returns false (falls back to (0,0)).
//
// bShouldStopOnSeparator=false on the FParse::Value call below: its default
// terminator set includes ',' (meant for stopping at the end of one
// positional value in a list), which would truncate "X,Y" at the comma and
// silently drop Y -- this switch's value is the whole "X,Y" pair, so read up
// to the next whitespace instead.
bool ParseSpawnColumnUU(double& OutWorldX, double& OutWorldY)
{
	OutWorldX = 0.0;
	OutWorldY = 0.0;
	FString SpawnAtArg;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelSpawnAt="), SpawnAtArg, /*bShouldStopOnSeparator=*/false))
	{
		return false;
	}
	FString XStr, YStr;
	if (!SpawnAtArg.Split(TEXT(","), &XStr, &YStr))
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("-VoxelSpawnAt=%s malformed (expected X,Y in meters); falling back to (0,0)."), *SpawnAtArg);
		return false;
	}
	const double SpawnMetersX = FCString::Atod(*XStr);
	const double SpawnMetersY = FCString::Atod(*YStr);
	OutWorldX = SpawnMetersX * 100.0; // meters -> UU (1 UU = 1 cm)
	OutWorldY = SpawnMetersY * 100.0;
	return true;
}
} // namespace

AVoxelEarthGameMode::AVoxelEarthGameMode()
{
	DefaultPawnClass = AVoxelEarthFlyPawn::StaticClass();
	PlayerControllerClass = AVoxelEarthPlayerController::StaticClass();
	HUDClass = AVoxelEarthHUD::StaticClass();
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

			// M2 task "SkyAtmosphere origin fix": the component's default
			// TransformMode (PlanetTopAtAbsoluteWorldOrigin) hardcodes the
			// planet's ground level at world (0,0,0). At a far LWC spawn
			// (e.g. -VoxelSpawnAt=2000000,1500000 -- 20,000km out) the
			// player is nowhere near that assumed ground level, so the
			// atmosphere's horizon sphere (computed relative to the
			// hardcoded origin) renders badly misplaced -- visible as a
			// horizon-sphere artifact cutting across the sky (see the lwc
			// verification runs this fix is checked against).
			// PlanetTopAtComponentTransform instead makes the planet's
			// ground level follow THIS component's own world transform, so
			// placing the actor at the pawn's spawn column (below) keeps
			// the horizon correct at any spawn offset, not just near-origin
			// ones.
			AtmosphereComp->TransformMode = ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform;

			AtmosphereComp->RegisterComponent();
			Atmosphere->SetRootComponent(AtmosphereComp);

			double SpawnColumnXUU = 0.0;
			double SpawnColumnYUU = 0.0;
			ParseSpawnColumnUU(SpawnColumnXUU, SpawnColumnYUU); // no-op (stays 0,0) if -VoxelSpawnAt is absent
			Atmosphere->SetActorLocation(FVector(SpawnColumnXUU, SpawnColumnYUU, 0.0));
		}

		// Water track W1 (docs/voxel-earth-implementation-plan.md SS3.7 /
		// SS4): same "no authored map, spawn from code" reasoning as the
		// light rig above -- the ocean actor is editor-independent.
		World->SpawnActor<AVoxelOceanActor>();

		// M2 Band 3 first slice (docs/m2-plan.md): same "no authored map,
		// spawn from code" reasoning -- the heightmap clipmap extends
		// terrain from the ring cascade's edge (~1km) out to ~30km.
		World->SpawnActor<AVoxelClipmapActor>();
	}

	// M2 ring debug verification (docs/m2-plan.md first implementation wave
	// item 4/5): -VoxelDebugRings forces voxel.Debug=2 + voxel.Debug.Rings=1
	// from the command line -- simplest way to get a headless -game run
	// showing ring tints without needing -ExecCmds plumbing for two cvars.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelDebugRings")))
	{
		VoxelDebug::SetDebugMode(2);
		VoxelDebug::SetRingsEnabled(true);
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDebugRings: forcing voxel.Debug=2, voxel.Debug.Rings=1"));
	}

	// M2 task "Mip cache eviction" verification aid: -VoxelMipCacheBudgetMB=<N>
	// forces voxel.MipCacheBudgetMB from the command line -- same
	// FindConsoleVariable-set-by-code pattern SetDebugMode/SetRingsEnabled use
	// above, simplest way to force a small budget for a headless
	// -VoxelPerfRun run without depending on -ExecCmds cvar-parsing timing.
	int32 MipCacheBudgetMBOverride = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelMipCacheBudgetMB="), MipCacheBudgetMBOverride))
	{
		VoxelDebug::SetMipCacheBudgetMB(MipCacheBudgetMBOverride);
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelMipCacheBudgetMB override: forcing voxel.MipCacheBudgetMB=%d"), MipCacheBudgetMBOverride);
	}

	// M2 wave 2 item 2 verification (docs/m2-plan.md "Distant-edit mip
	// propagation"): -VoxelHeadlessDigTest[=<delaySeconds>] carves a sphere
	// ~100m from spawn (see the CarveOffsetUU comment below for WHY it can't
	// be at the anchor itself) once the player has had time to settle in
	// (default 20s -- R1 chunks near the inner edge of its 64-128m annulus
	// typically resident by then, per wave-1's measured fill rates in
	// docs/status.md's M2 section) so the dig lands on terrain that's
	// actually streamed in as an R1+ chunk, rather than racing streaming.
	// Combine with -VoxelScreenshotAfter=<seconds> (existing switch, with a
	// larger seconds value so the capture happens AFTER the carve) for a
	// self-contained headless dig-then-screenshot run. The carve itself logs
	// "Distant-edit mip propagation" (every dirtied ancestor chunk, every
	// level) and "Distant-edit mip re-mesh" (every level>=1 chunk that
	// actually re-meshed via the overlay-aware path) lines from
	// VoxelWorldSubsystem.cpp -- that log evidence proves R1+ propagation
	// independent of whether a screenshot is also requested.
	float DigTestDelaySeconds = 20.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelHeadlessDigTest="), DigTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelHeadlessDigTest")))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelHeadlessDigTest: carving near spawn in %.1fs"), DigTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			HeadlessDigTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* DigWorld = GetWorld();
					APlayerController* PC = DigWorld ? DigWorld->GetFirstPlayerController() : nullptr;
					APawn* Pawn = PC ? PC->GetPawn() : nullptr;
					UVoxelWorldSubsystem* Subsystem = DigWorld ? DigWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Pawn || !Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelHeadlessDigTest: no pawn/subsystem yet, skipping carve."));
						return;
					}
					// Rings are an ANNULUS around the anchor (m2-plan.md "Ring
					// structure" row): R1's own inner edge excludes anything
					// within 64m of the anchor (that band is R0's exclusive
					// territory), so a carve AT the pawn's own location would
					// never have a resident R1+ chunk to re-mesh -- there is
					// nothing to prove propagation against. Offsetting 100m
					// along X lands inside R1's [64,128) annulus instead,
					// where a chunk has actually streamed in as pure-generated
					// by dig time, giving MarkChunkDirtyForRemesh a real
					// resident record to requeue.
					constexpr double CarveOffsetUU = 10000.0; // 100m, inside R1's annulus
					constexpr double RadiusUU = 1000.0;       // 10m
					constexpr double JitterUU = 200.0;        // 2m
					const FVector PawnLoc = Pawn->GetActorLocation();
					const double TargetX = PawnLoc.X + CarveOffsetUU;
					const double TargetY = PawnLoc.Y;
					const double SurfaceUU = Subsystem->GetSurfaceHeightUU(TargetX, TargetY);
					const FVector CarveCenter(TargetX, TargetY, SurfaceUU);
					const int32 Removed = Subsystem->CarveSphere(CarveCenter, RadiusUU, JitterUU);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelHeadlessDigTest: carved at (%.0f,%.0f,%.0f) r=%.0fUU, removed %d voxels -- watch for ")
					       TEXT("'Distant-edit mip propagation'/'Distant-edit mip re-mesh' log lines (LogVoxelEdit)."),
					       CarveCenter.X, CarveCenter.Y, CarveCenter.Z, RadiusUU, Removed);
				}),
			DigTestDelaySeconds, false);
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

	constexpr double DefaultSpawnHeightAboveSurfaceUU = 500.0; // +5m (1 UU = 1 cm)
	double SpawnHeightAboveSurfaceUU = DefaultSpawnHeightAboveSurfaceUU;

	// -VoxelSpawnAt=X,Y (meters, world): stage 3c LWC verification switch --
	// overrides the spawn column with the same surface-height-query-plus-5m
	// logic used for the default (0,0) column above, just evaluated at an
	// arbitrary far-from-origin column. Default behavior (spawn at 0,0) is
	// unchanged when the switch is absent. Parsing itself lives in the
	// file-scope ParseSpawnColumnUU (shared with BeginPlay's SkyAtmosphere
	// placement, M2 task "SkyAtmosphere origin fix") so the pawn and the
	// atmosphere actor can never land on different columns.
	double SpawnWorldX = 0.0;
	double SpawnWorldY = 0.0;
	if (ParseSpawnColumnUU(SpawnWorldX, SpawnWorldY))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSpawnAt override: spawning at column (%.1f, %.1f) m"),
		       SpawnWorldX / 100.0, SpawnWorldY / 100.0);
	}

	// -VoxelCameraHigh=<meters>: M2 Band 3 verification switch (docs/m2-plan.md
	// "Verification" row) -- a summit-level ground spawn can't see the 30km
	// clipmap well, so this spawns the pawn N meters above the surface
	// instead of the default +5m, giving a vista screenshot that actually
	// shows rings near / clipmap far / ocean beyond coastlines. No effect
	// unless passed explicitly.
	float CameraHighMeters = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCameraHigh="), CameraHighMeters) && CameraHighMeters > 0.f)
	{
		SpawnHeightAboveSurfaceUU = double(CameraHighMeters) * 100.0; // meters -> UU
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCameraHigh override: spawning %.0fm above the surface"), CameraHighMeters);
	}

	const double SurfaceUU = Subsystem->GetSurfaceHeightUU(SpawnWorldX, SpawnWorldY);
	const FVector SpawnLocation(SpawnWorldX, SpawnWorldY, SurfaceUU + SpawnHeightAboveSurfaceUU);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	RestartPlayerAtTransform(NewPlayer, SpawnTransform);
}

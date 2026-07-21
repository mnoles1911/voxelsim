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
#include "VoxelAgentSubsystem.h" // M6 NPC swarm: -VoxelSwarmTest switch below
#include "VoxelEarth.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelClipmapActor.h"
#include "VoxelEarthHUD.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEditRelay.h"
#include "VoxelOceanActor.h"
#include "VoxelWaterSubsystem.h"
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

		// M3 wave 1 (docs/m3-plan.md): the edit-log replication transport
		// (AVoxelEditRelay), spawned by the GameMode on authority -- but only
		// when this world is actually networked. NM_Standalone spawns
		// nothing at all, so single-player has zero relay-related code paths
		// touched (docs/m3-plan.md gate requirement: "standalone behavior
		// byte-identical"). GameMode::BeginPlay only ever runs server-side
		// (dedicated server, or the server half of a listen server), so this
		// unconditionally means "spawned once, by the authority."
		if (World->GetNetMode() != NM_Standalone)
		{
			World->SpawnActor<AVoxelEditRelay>();
		}
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

	// M3 wave 1 gate verification (docs/m3-plan.md "two clients dig the same
	// hole"): -VoxelDumpDigestAfter=<s> logs this SERVER process's seed +
	// World::editedDigest() -- the client-side equivalent lives on
	// AVoxelEarthPlayerController (GameMode never exists client-side).
	float ServerDumpDigestAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDumpDigestAfter="), ServerDumpDigestAfterSeconds) && ServerDumpDigestAfterSeconds > 0.f)
	{
		GetWorldTimerManager().SetTimer(
			ServerDumpDigestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* DumpWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = DumpWorld ? DumpWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						return;
					}
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDigestDump: role=Server seed=%llu editedDigest=0x%016llX"),
					       (unsigned long long)Subsystem->GetSeed(), (unsigned long long)Subsystem->GetEditedDigest());

					// Self-quit a few seconds later (gate-run convenience:
					// nothing else naturally ends a headless -server process).
					GetWorldTimerManager().SetTimer(
						ServerDigestQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }),
						5.f, false);
				}),
			ServerDumpDigestAfterSeconds, false);
	}

	// M3 wave 2 persistence verification (docs/m3-plan.md "Save/load"):
	// -VoxelSaveWorldAfter=<s> calls the same UVoxelWorldSubsystem::SaveWorld()
	// the voxel.SaveWorld console command uses, logs entries/digest, then
	// self-quits a few seconds later -- same convenience pattern as
	// -VoxelDumpDigestAfter above. Combine with -VoxelHeadlessDigTest (a
	// smaller delay) so the save captures the dig's edits, e.g.
	// -VoxelHeadlessDigTest=20 -VoxelSaveWorldAfter=25.
	float SaveWorldAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSaveWorldAfter="), SaveWorldAfterSeconds) && SaveWorldAfterSeconds > 0.f)
	{
		GetWorldTimerManager().SetTimer(
			SaveWorldTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* SaveWorldPtr = GetWorld();
					UVoxelWorldSubsystem* Subsystem = SaveWorldPtr ? SaveWorldPtr->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						return;
					}
					const bool bOk = Subsystem->SaveWorld();
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSaveWorldAfter: SaveWorld() -> %s (editedDigest=0x%016llX)"),
					       bOk ? TEXT("OK") : TEXT("FAILED"), (unsigned long long)Subsystem->GetEditedDigest());

					GetWorldTimerManager().SetTimer(
						SaveWorldQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f, false);
				}),
			SaveWorldAfterSeconds, false);
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
					UWorld* ShotWorld = GetWorld();
					APlayerController* PC = ShotWorld ? ShotWorld->GetFirstPlayerController() : nullptr;

					// W2 verification framing (task items 5a/5b): a v0 water
					// pool/crater is only a few meters across -- easy to miss
					// entirely at the default oblique terrain-survey angle
					// from spawn height. When a water test switch drove this
					// run, instead hover the pawn directly above the known
					// pour/breach column and look close to straight down.
					float Throwaway = 0.f;
					const bool bSpawnWaterTestActive = FParse::Param(FCommandLine::Get(), TEXT("VoxelSpawnWaterTest")) ||
					                                    FParse::Value(FCommandLine::Get(), TEXT("VoxelSpawnWaterTest="), Throwaway);
					const bool bBreachTestActive = FParse::Param(FCommandLine::Get(), TEXT("VoxelBreachTest")) ||
					                                FParse::Value(FCommandLine::Get(), TEXT("VoxelBreachTest="), Throwaway);

					bool bOverheadFraming = false;
					FVector OverheadColumnWorld = FVector::ZeroVector;
					UVoxelWorldSubsystem* ShotTerrain = ShotWorld ? ShotWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelWaterSubsystem* ShotWater = ShotWorld ? ShotWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;

					// Prefer the water body's ACTUAL centroid (wherever the
					// CA's lateral spread/gravity actually settled it) over
					// the pour/breach column it started from -- a pool can
					// drift meters away from where it was poured before
					// coming to rest, especially on sloped terrain.
					if (ShotWater && (bSpawnWaterTestActive || bBreachTestActive) && ShotWater->GetStoredWaterCentroidUU(OverheadColumnWorld))
					{
						bOverheadFraming = true;
					}
					else if (ShotTerrain && bSpawnWaterTestActive)
					{
						OverheadColumnWorld = FVector(0.0, 0.0, ShotTerrain->GetSurfaceHeightUU(0.0, 0.0));
						bOverheadFraming = true;
					}
					else if (ShotTerrain && bBreachTestActive)
					{
						// Same synchronous grid scan BeginPlay used to pick the
						// breach column (duplicated here rather than threading
						// BreachColumnWorldX/Y through -- this lambda is a
						// separate closure defined earlier in BeginPlay than
						// that block's local variables).
						constexpr double ScanStepUU = 20000.0; // 200m grid step
						constexpr int32 ScanRadiusSteps = 100; // +/- 20km around spawn
						for (int32 Iy = -ScanRadiusSteps; Iy <= ScanRadiusSteps && !bOverheadFraming; ++Iy)
						{
							for (int32 Ix = -ScanRadiusSteps; Ix <= ScanRadiusSteps; ++Ix)
							{
								const double Wx = double(Ix) * ScanStepUU;
								const double Wy = double(Iy) * ScanStepUU;
								const double SurfUU = ShotTerrain->GetSurfaceHeightUU(Wx, Wy);
								if (SurfUU < 0.0)
								{
									OverheadColumnWorld = FVector(Wx, Wy, SurfUU);
									bOverheadFraming = true;
									break;
								}
							}
						}
					}

					// M5 chop-test framing (task item 5): stand back from the tree
					// column and look at it, so the detached/fallen canopy is in
					// frame. Highest priority when a tree/chop test drove the run.
					// M5 large-edit collapse framing: stand well back and to the
					// side of the wall+roof fixture, level with the roof, so the
					// standing (wall-supported) part of the slab and the fallen
					// span are both in frame. Highest priority of all -- the
					// whole point of this run is to SEE the aftermath.
					if (bStructureTestActive && PC)
					{
						const double StructSurfUU =
							ShotTerrain ? ShotTerrain->GetSurfaceHeightUU(StructureTestColumnXUU, StructureTestColumnYUU) : 0.0;
						const FVector StructMid(StructureTestColumnXUU + 640.0, StructureTestColumnYUU + 160.0,
						                        StructSurfUU + 250.0);
						const FVector CamPos = StructMid + FVector(-200.0, -1600.0, 700.0); // broadside, back, up
						const FRotator Look = (StructMid - CamPos).Rotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(CamPos);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					else if (bTreeTestActive && PC)
					{
						const double TreeSurfUU = ShotTerrain ? ShotTerrain->GetSurfaceHeightUU(TreeTestColumnXUU, TreeTestColumnYUU) : 0.0;
						const FVector TreeMid(TreeTestColumnXUU, TreeTestColumnYUU, TreeSurfUU + 150.0);
						const FVector CamPos = TreeMid + FVector(-500.0, -350.0, 300.0); // back / side / up
						const FRotator Look = (TreeMid - CamPos).Rotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(CamPos);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					else if (bOverheadFraming && PC)
					{
						constexpr double HoverHeightAboveUU = 1500.0; // 15m: fills frame, clears splash geometry
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(OverheadColumnWorld + FVector(0.0, 0.0, HoverHeightAboveUU));
							P->SetActorRotation(FRotator(-85.f, 0.f, 0.f));
						}
						PC->SetControlRotation(FRotator(-85.f, 0.f, 0.f));
					}
					else if (PC)
					{
						// Aim down toward the terrain well before capturing, on
						// both the controller AND the pawn (belt and braces -
						// the first capture attempt showed control rotation
						// alone not reflected in the captured view).
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

	// W2 verification (task item 5a): -VoxelSpawnWaterTest[=<delaySeconds>]
	// (default 20s) pours a water pool above the spawn column, logs the
	// ledger volume + digest at pour time, then again ~15s later as a
	// settle-check (active bricks should trend toward 0 as the pool flattens
	// and rests). Combine with -VoxelScreenshotAfter=<seconds> (a larger
	// value than this delay) for a visual capture of the pool.
	float SpawnWaterTestDelaySeconds = 20.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelSpawnWaterTest="), SpawnWaterTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelSpawnWaterTest")))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSpawnWaterTest: pouring a water pool near spawn in %.1fs"), SpawnWaterTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			SpawnWaterTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* PourWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = PourWorld ? PourWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelWaterSubsystem* Water = PourWorld ? PourWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					if (!Terrain || !Water)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelSpawnWaterTest: subsystems not ready, skipping."));
						return;
					}

					// 30,000 fill units ~= 118 full voxels' worth (255 each).
					// Verification testing found the v0 mesher's fill>=128
					// visibility threshold (task item 4) matters here: a
					// SMALL pour (e.g. 3,000 units) spreads thin enough over
					// an open, gently-sloped surface that every settled
					// cell's fill stays under 128 (observed maxFill=11 for a
					// 3,000-unit pour) -- CA-correct (volume conserved,
					// settles to 0 active bricks) but invisible, since there
					// is no boundary to mesh a face at. 30,000 keeps enough
					// of the pour concentrated near the pour column to stay
					// above the meshing threshold while it settles. Poured
					// 5m above the (0,0) spawn column's surface so it falls
					// and pools under gravity + lateral equalization.
					constexpr uint32 PourAmount = 30000;
					const double SurfaceUU = Terrain->GetSurfaceHeightUU(0.0, 0.0);
					const FVector PourLoc(0.0, 0.0, SurfaceUU + 500.0);
					const uint32 Placed = Water->SpawnWaterAt(PourLoc, PourAmount);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelSpawnWaterTest: poured %u/%u fill units at (0,0,%.0f); ledger volume=%llu digest=0x%016llX"),
					       Placed, PourAmount, PourLoc.Z, (unsigned long long)Water->GetPerfSnapshot().TotalVolume,
					       (unsigned long long)Water->GetWaterDigest());

					GetWorldTimerManager().SetTimer(
						SpawnWaterTestSettleTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this]()
							{
								UWorld* SettleWorld = GetWorld();
								UVoxelWaterSubsystem* SettleWater = SettleWorld ? SettleWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
								if (!SettleWater)
								{
									return;
								}
								const FVoxelWaterPerfSnapshot Snap = SettleWater->GetPerfSnapshot();
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("VoxelSpawnWaterTest settle-check: activeBricks=%lld storedBricks=%lld volume=%llu ")
								       TEXT("maxFill=%d digest=0x%016llX"),
								       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume,
								       SettleWater->GetMaxStoredFill(), (unsigned long long)SettleWater->GetWaterDigest());
							}),
						45.f, false);
				}),
			SpawnWaterTestDelaySeconds, false);
	}

	// W2 verification (task item 5b): -VoxelBreachTest[=<delaySeconds>]
	// (default 20s) scans a grid around spawn RIGHT NOW (BeginPlay, so the
	// chosen spot is logged before the delayed carve, not racing streaming)
	// for a column whose surface elevation is already below sea level (an
	// offshore seabed column -- the implicit ocean's non-solid z<0 cells
	// already sit directly above it) via UVoxelWorldSubsystem::
	// GetSurfaceHeightUU (a pure, cheap query safe to call this early). Once
	// found, carving a sphere centered AT that surface exposes solid voxels
	// whose neighbors are already open water, satisfying
	// UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared's breach condition --
	// the crater floods from the Reservoir v0 boundary cells this carve
	// creates.
	float BreachTestDelaySeconds = 20.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelBreachTest="), BreachTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelBreachTest")))
	{
		UVoxelWorldSubsystem* Terrain = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
		bool bFoundBreachColumn = false;
		double BreachColumnWorldX = 0.0, BreachColumnWorldY = 0.0;
		if (Terrain)
		{
			constexpr double ScanStepUU = 20000.0; // 200m grid step
			constexpr int32 ScanRadiusSteps = 100; // +/- 20km around spawn
			for (int32 Iy = -ScanRadiusSteps; Iy <= ScanRadiusSteps && !bFoundBreachColumn; ++Iy)
			{
				for (int32 Ix = -ScanRadiusSteps; Ix <= ScanRadiusSteps; ++Ix)
				{
					const double Wx = double(Ix) * ScanStepUU;
					const double Wy = double(Iy) * ScanStepUU;
					if (Terrain->GetSurfaceHeightUU(Wx, Wy) < 0.0)
					{
						BreachColumnWorldX = Wx;
						BreachColumnWorldY = Wy;
						bFoundBreachColumn = true;
						break;
					}
				}
			}
		}

		if (!bFoundBreachColumn)
		{
			UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelBreachTest: no below-sea-level column found within the scan radius -- test skipped."));
		}
		else
		{
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelBreachTest: chosen column (%.1f, %.1f) m, surface %.2f m below sea level; carving in %.1fs"),
			       BreachColumnWorldX / 100.0, BreachColumnWorldY / 100.0, -Terrain->GetSurfaceHeightUU(BreachColumnWorldX, BreachColumnWorldY) / 100.0,
			       BreachTestDelaySeconds);
			GetWorldTimerManager().SetTimer(
				BreachTestTimerHandle,
				FTimerDelegate::CreateWeakLambda(this,
					[this, BreachColumnWorldX, BreachColumnWorldY]()
					{
						UWorld* CarveWorld = GetWorld();
						UVoxelWorldSubsystem* CarveTerrain = CarveWorld ? CarveWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
						UVoxelWaterSubsystem* CarveWater = CarveWorld ? CarveWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
						if (!CarveTerrain || !CarveWater)
						{
							UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelBreachTest: subsystems not ready, skipping."));
							return;
						}
						const double SurfUU = CarveTerrain->GetSurfaceHeightUU(BreachColumnWorldX, BreachColumnWorldY);
						const FVector Center(BreachColumnWorldX, BreachColumnWorldY, SurfUU);
						constexpr double RadiusUU = 600.0; // 6m
						constexpr double JitterUU = 100.0; // 1m
						const int32 Removed = CarveTerrain->CarveSphere(Center, RadiusUU, JitterUU);
						UE_LOG(LogVoxelEarth, Log,
						       TEXT("VoxelBreachTest: carved %d voxels at (%.0f,%.0f,%.0f) -- watch for 'Dig breach' log lines ")
						       TEXT("(LogVoxelWater)"),
						       Removed, Center.X, Center.Y, Center.Z);

						GetWorldTimerManager().SetTimer(
							BreachTestSettleTimerHandle,
							FTimerDelegate::CreateWeakLambda(this,
								[this]()
								{
									UWorld* SettleWorld = GetWorld();
									UVoxelWaterSubsystem* SettleWater = SettleWorld ? SettleWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
									if (!SettleWater)
									{
										return;
									}
									const FVoxelWaterPerfSnapshot Snap = SettleWater->GetPerfSnapshot();
									UE_LOG(LogVoxelEarth, Log,
									       TEXT("VoxelBreachTest settle-check: activeBricks=%lld storedBricks=%lld volume=%llu ")
									       TEXT("maxFill=%d digest=0x%016llX"),
									       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume,
									       SettleWater->GetMaxStoredFill(), (unsigned long long)SettleWater->GetWaterDigest());
								}),
							25.f, false);
					}),
				BreachTestDelaySeconds, false);
		}
	}

	// M5 destruction (first slice, docs/m4-plan.md Round 2 reframe):
	// -VoxelTreeTest[=<delaySeconds>] (default 8s) places a stand-in tree
	// FIXTURE ~6m ahead of the spawn column once R0 has streamed in.
	// -VoxelChopTest[=<delaySeconds>] (default 18s; implies -VoxelTreeTest)
	// then carves through the trunk, severing the canopy so the M5 chop ->
	// island-detect -> fall pipeline promotes it to falling debris. Combine
	// with -VoxelScreenshotAfter=<n> (a value LARGER than the chop delay) for a
	// self-contained headless capture of the detached canopy on the ground; the
	// screenshot block above aims at the tree column when either switch is set.
	float ChopTestDelaySeconds = 18.f;
	const bool bChopTestActive = FParse::Value(FCommandLine::Get(), TEXT("VoxelChopTest="), ChopTestDelaySeconds) ||
	                             FParse::Param(FCommandLine::Get(), TEXT("VoxelChopTest"));
	float TreeTestDelaySeconds = 8.f;
	const bool bTreeTestRequested = FParse::Value(FCommandLine::Get(), TEXT("VoxelTreeTest="), TreeTestDelaySeconds) ||
	                                FParse::Param(FCommandLine::Get(), TEXT("VoxelTreeTest"));

	if (bTreeTestRequested || bChopTestActive)
	{
		bTreeTestActive = true;

		// Place the tree ~6m ahead (+X) of the spawn column (the fly pawn spawns
		// facing +X), so it lands in front of the pawn and inside R0.
		double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
		ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);
		TreeTestColumnXUU = SpawnColXUU + 600.0;
		TreeTestColumnYUU = SpawnColYUU;

		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelTreeTest: placing stand-in tree fixture at (%.0f,%.0f) in %.1fs"),
		       TreeTestColumnXUU, TreeTestColumnYUU, TreeTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			TreeTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* TreeWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = TreeWorld ? TreeWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelTreeTest: subsystem not ready, skipping tree placement."));
						return;
					}
					const int32 Count = Subsystem->SpawnTreeFixtureAt(TreeTestColumnXUU, TreeTestColumnYUU);
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelTreeTest: tree fixture placed (%d voxels)."), Count);
				}),
			TreeTestDelaySeconds, false);
	}

	if (bChopTestActive)
	{
		// Make sure the chop lands AFTER the tree has been placed (and its
		// chunks meshed) -- clamp the chop delay to comfortably follow the tree.
		if (ChopTestDelaySeconds <= TreeTestDelaySeconds)
		{
			ChopTestDelaySeconds = TreeTestDelaySeconds + 10.f;
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelChopTest: chopping the trunk in %.1fs"), ChopTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			ChopTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* ChopWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = ChopWorld ? ChopWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelChopTest: subsystem not ready, skipping chop."));
						return;
					}
					// Carve straight through the trunk at ~mid-height (12 voxels
					// above the surface). Radius 40UU (4 voxels) fully clears the
					// 2x2 trunk cross-section over an ~8-voxel band -> a clean
					// sever. Jitter 0 for a deterministic cut. CarveSphere routes
					// through the M5 chop hook (PromoteDetachedIslands), which
					// detects the now-floating canopy, removes it from the grid,
					// and spawns the falling debris.
					const double SurfUU = Subsystem->GetSurfaceHeightUU(TreeTestColumnXUU, TreeTestColumnYUU);
					const FVector CutCentre(TreeTestColumnXUU + 10.0, TreeTestColumnYUU + 10.0, SurfUU + 120.0);
					const int32 Removed = Subsystem->CarveSphere(CutCentre, 40.0, 0.0);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelChopTest: carved trunk at (%.0f,%.0f,%.0f), removed %d voxels -- watch for 'Destruction:' / ")
					       TEXT("'VoxelDebris' log lines (LogVoxelEdit/LogVoxelEarth)."),
					       CutCentre.X, CutCentre.Y, CutCentre.Z, Removed);
				}),
			ChopTestDelaySeconds, false);
	}

	// M5 LARGE-EDIT structural collapse (docs/status.md "Structural collapse
	// (M5, large-edit)"): -VoxelStructureTest[=<delaySeconds>] (default 8s)
	// places a wall + roof-slab + far-pillars FIXTURE ~10m ahead of the spawn
	// column. -VoxelCollapseTest[=<delaySeconds>] (default 20s; implies
	// -VoxelStructureTest) then fires ONE large CarveSphere through the far
	// pillars. The carve is far too big for the voxel-resolution island region
	// (its cleared AABB alone exceeds the 48-voxel cap), so it takes the
	// brick-resolution differential-support path -- and because the roof stays
	// 6-connected to the ground through the wall the whole time, anything that
	// falls here is the SUPPORT model working, not connectivity. Combine with
	// -VoxelScreenshotAfter=<n> (larger than the collapse delay) for a headless
	// capture of the aftermath.
	float CollapseTestDelaySeconds = 20.f;
	const bool bCollapseTestActive = FParse::Value(FCommandLine::Get(), TEXT("VoxelCollapseTest="), CollapseTestDelaySeconds) ||
	                                 FParse::Param(FCommandLine::Get(), TEXT("VoxelCollapseTest"));
	float StructureTestDelaySeconds = 8.f;
	const bool bStructureTestRequested =
		FParse::Value(FCommandLine::Get(), TEXT("VoxelStructureTest="), StructureTestDelaySeconds) ||
		FParse::Param(FCommandLine::Get(), TEXT("VoxelStructureTest"));

	if (bStructureTestRequested || bCollapseTestActive)
	{
		bStructureTestActive = true;

		// ~10m ahead (+X) of the spawn column so the whole 12.8m span sits in
		// front of the pawn and inside R0 (same reasoning as -VoxelTreeTest).
		double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
		ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);
		StructureTestColumnXUU = SpawnColXUU + 1000.0;
		StructureTestColumnYUU = SpawnColYUU;

		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelStructureTest: placing wall+roof fixture at (%.0f,%.0f) in %.1fs"),
		       StructureTestColumnXUU, StructureTestColumnYUU, StructureTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			StructureTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* StructWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = StructWorld ? StructWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelStructureTest: subsystem not ready, skipping fixture."));
						return;
					}
					const int32 Count = Subsystem->SpawnStructureFixtureAt(StructureTestColumnXUU, StructureTestColumnYUU);
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelStructureTest: fixture placed (%d voxels), digest=0x%016llX."),
					       Count, (unsigned long long)Subsystem->GetEditedDigest());
				}),
			StructureTestDelaySeconds, false);
	}

	if (bCollapseTestActive)
	{
		if (CollapseTestDelaySeconds <= StructureTestDelaySeconds)
		{
			CollapseTestDelaySeconds = StructureTestDelaySeconds + 12.f;
		}
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCollapseTest: blowing out the far pillars in %.1fs"), CollapseTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			CollapseTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* BlastWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = BlastWorld ? BlastWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelCollapseTest: subsystem not ready, skipping blast."));
						return;
					}
					// Centred on the far pillar pair (the fixture's +X end,
					// mid-width, a little above the surface). Radius 2.6m
					// comfortably swallows BOTH pillars (they sit +/-1.6m from
					// the centre line); the cleared AABB is ~52 voxels across,
					// well past the voxel-resolution region's 48-voxel cap, so
					// this takes the large-edit path by construction. Jitter 0
					// keeps the blast itself deterministic for the A/B digest
					// comparison.
					const double SurfUU = Subsystem->GetSurfaceHeightUU(StructureTestColumnXUU, StructureTestColumnYUU);
					const FVector BlastCentre(StructureTestColumnXUU + 1240.0, StructureTestColumnYUU + 160.0, SurfUU + 80.0);
					const int32 Removed = Subsystem->CarveSphere(BlastCentre, 260.0, 0.0);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelCollapseTest: blast at (%.0f,%.0f,%.0f) r=260UU removed %d voxels -- watch for ")
					       TEXT("'Collapse:' / 'VoxelDebris' log lines (LogVoxelEdit/LogVoxelEarth)."),
					       BlastCentre.X, BlastCentre.Y, BlastCentre.Z, Removed);

					GetWorldTimerManager().SetTimer(
						CollapseTestSettleTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this]()
							{
								UWorld* SettleWorld = GetWorld();
								UVoxelWorldSubsystem* S = SettleWorld ? SettleWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
								if (S)
								{
									// The post-collapse edited-world digest IS
									// the determinism check: the collapse
									// removals are edit-log entries like any
									// dig, so an A/B of two identical runs must
									// print the same value here.
									UE_LOG(LogVoxelEarth, Log,
									       TEXT("VoxelCollapseTest: post-collapse seed=%llu editedDigest=0x%016llX"),
									       (unsigned long long)S->GetSeed(), (unsigned long long)S->GetEditedDigest());
								}
							}),
						8.f, false);
				}),
			CollapseTestDelaySeconds, false);
	}

	// M6 NPC swarm verification (docs/status.md M6 section, plan SS3.6):
	// -VoxelSwarmTest[=<N>] (default N=200) spawns N pursuit agents ringed
	// around the player ~2s after BeginPlay -- authority only (GameMode::
	// BeginPlay only ever runs server-side, same reasoning
	// -VoxelHeadlessDigTest/-VoxelTreeTest's blocks above already rely on).
	// Unlike those two, this delay is short and fixed (not tunable, not
	// waiting on render streaming): UVoxelAgentSubsystem::SpawnSwarm's
	// ground-placement query (UVoxelWorldSubsystem::GetSurfaceHeightUU) is a
	// pure amplifier read, safe immediately once the terrain subsystem's
	// Impl exists (see that method's own doc comment) -- the only thing
	// this delay actually waits on is RestartPlayer's pawn existing. Combine
	// with -VoxelScreenshotAfter=<seconds> (a value LARGER than this delay)
	// for a self-contained headless capture of the swarm converging on the
	// player; tier counts and the mean-distance-to-player convergence
	// metric are logged periodically by UVoxelAgentSubsystem::Tick itself
	// ("VoxelSwarm:" lines, LogVoxelEarth), independent of whether a
	// screenshot is also requested.
	int32 SwarmTestCount = UVoxelAgentSubsystem::DefaultSwarmCount;
	const bool bSwarmTestActive = FParse::Value(FCommandLine::Get(), TEXT("VoxelSwarmTest="), SwarmTestCount) ||
	                               FParse::Param(FCommandLine::Get(), TEXT("VoxelSwarmTest"));
	if (bSwarmTestActive)
	{
		constexpr float SwarmSpawnDelaySeconds = 2.f;
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSwarmTest: spawning %d agents in %.1fs"), SwarmTestCount, SwarmSpawnDelaySeconds);
		GetWorldTimerManager().SetTimer(
			SwarmTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, SwarmTestCount]()
				{
					UWorld* SwarmWorld = GetWorld();
					UVoxelAgentSubsystem* AgentSubsystem = SwarmWorld ? SwarmWorld->GetSubsystem<UVoxelAgentSubsystem>() : nullptr;
					if (!AgentSubsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelSwarmTest: no UVoxelAgentSubsystem, skipping spawn."));
						return;
					}
					const int32 Spawned = AgentSubsystem->SpawnSwarm(SwarmTestCount);
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSwarmTest: spawned %d/%d requested agents."), Spawned, SwarmTestCount);
				}),
			SwarmSpawnDelaySeconds, false);
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

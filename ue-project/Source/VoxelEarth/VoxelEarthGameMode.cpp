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
#include "VoxelAgentReplication.h" // M6 gap closure: AVoxelAgentReplicator spawn below
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
// -VoxelWaterWakeTest: runs the SAME basin/pour/dig/place/carve/collapse
// sequence as -VoxelWaterMemoTest, but with every test-harness "nudge"
// (SpawnWaterAt after each edit) SUPPRESSED. Those nudges only ever existed
// because a settled water body used to be unwakeable from a terrain edit --
// the gap UVoxelWaterSubsystem::NotifyTerrainRegionEdited -> WaterCA::wakeRegion
// now closes. With them off, any change in the logged digest/volume is caused
// SOLELY by the terrain edit waking the water, and total volume must be
// identical at every checkpoint (waking moves water, it never adds any).
// Read from the command line rather than threaded through the switch's deeply
// nested timer lambdas, to keep this diff minimal and local.
bool IsWaterWakeTestMode()
{
	float Ignored = 0.f;
	return FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterWakeTest="), Ignored) ||
	       FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterWakeTest"));
}

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

			// M6 gap closure (docs/status.md M6 section "Tier-1 hierarchical
			// planning + NPC replication"): the NPC-state replication
			// transport (AVoxelAgentReplicator, VoxelAgentReplication.h/.cpp
			// -- a new file for this slice, deliberately NOT a third stream
			// bolted onto AVoxelEditRelay, see that class's header comment).
			// Same "only when actually networked" gating as the edit relay
			// above, same reasoning (byte-identical standalone behavior).
			World->SpawnActor<AVoxelAgentReplicator>();
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

	// ==== M4 voxel light field + cone-traced GI verification ================
	// (ONE self-contained block, deliberately: it does its own fixture, its own
	// camera framing and its own capture/quit rather than adding a branch to
	// the shared -VoxelScreenshotAfter framing chain below, so it cannot
	// collide with other agents' edits to that chain.)
	//
	// -VoxelGIOn            force voxel.GI.Enabled 1 from the command line
	//                       (same SetByCode pattern as -VoxelDebugRings above;
	//                       more reliable than -ExecCmds timing). Usable on
	//                       its own, including with -VoxelPerfRun.
	// -VoxelGITest=<s>      after <s> seconds, stamp the wall+roof fixture on
	//                       the flattest column near spawn and put the camera
	//                       UNDER the roof slab looking down the covered span.
	//                       This is the "enclosed space should go dark" proof.
	// -VoxelGIBreach        additionally punch a hole through the middle of the
	//                       roof slab a few seconds before the capture -- the
	//                       "blow open a roof and light pours in" proof.
	//
	// WHY AN ABOVE-GROUND ENCLOSURE AND NOT A DUG TUNNEL. The obvious version
	// of this test -- carve a chamber underground and stand in it -- does not
	// work in this build, and three runs were spent finding out why: the
	// streaming footprint only meshes a band of chunks around the terrain
	// SURFACE (see ComputeFootprintChunkZRange in VoxelWorldSubsystem.cpp), so
	// a camera 9m or more below the surface is outside the resident set and
	// renders open sky beneath a thin crust of terrain no matter how much rock
	// voxel-core says is there. That is a streaming/LOD limitation, not a GI
	// one, but it makes an underground shot meaningless. The wall+roof fixture
	// (SpawnStructureFixtureAt, already used by -VoxelStructureTest) gives a
	// real 12.8m x 3.2m covered span with 3.2m of headroom, entirely inside the
	// R0 ring and unambiguously rendered.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelGIOn")))
	{
		if (IConsoleVariable* GIVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Enabled")))
		{
			GIVar->Set(1, ECVF_SetByCode);
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGIOn: forcing voxel.GI.Enabled=1"));
		}
	}

	float GITestDelaySeconds = 20.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGITest="), GITestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelGITest")))
	{
		const bool bGIBreach = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIBreach"));
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGITest: roofed fixture near spawn in %.1fs (breach=%d)"),
		       GITestDelaySeconds, bGIBreach ? 1 : 0);

		GetWorldTimerManager().SetTimer(
			GITestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, bGIBreach]()
				{
					UWorld* GIWorld = GetWorld();
					APlayerController* PC = GIWorld ? GIWorld->GetFirstPlayerController() : nullptr;
					APawn* Pawn = PC ? PC->GetPawn() : nullptr;
					UVoxelWorldSubsystem* Terrain = GIWorld ? GIWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Pawn || !Terrain)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelGITest: no pawn/subsystem yet, skipping."));
						return;
					}

					// Flattest column within 60m of spawn: the fixture's covered
					// span is only 3.2m high, so a sloping floor under it would
					// close the corridor off and ruin the framing. Deterministic
					// scan -> seed-stable framing across the off/on pair.
					const FVector PawnLoc = Pawn->GetActorLocation();
					double BestX = PawnLoc.X, BestY = PawnLoc.Y;
					double BestSurf = Terrain->GetSurfaceHeightUU(PawnLoc.X, PawnLoc.Y);
					double BestRelief = TNumericLimits<double>::Max();
					constexpr double ScanStepUU = 1000.0; // 10m
					constexpr int32 ScanSteps = 6;        // +/- 60m, comfortably inside R0
					for (int32 Iy = -ScanSteps; Iy <= ScanSteps; ++Iy)
					{
						for (int32 Ix = -ScanSteps; Ix <= ScanSteps; ++Ix)
						{
							const double Cx = PawnLoc.X + double(Ix) * ScanStepUU;
							const double Cy = PawnLoc.Y + double(Iy) * ScanStepUU;
							const double Cs = Terrain->GetSurfaceHeightUU(Cx, Cy);
							if (Cs <= 0.0)
							{
								continue; // below sea level
							}
							double Relief = 0.0;
							for (int32 Ni = 0; Ni <= 13; ++Ni)
							{
								// Sample along the +X span the fixture will occupy.
								const double Ns = Terrain->GetSurfaceHeightUU(Cx + double(Ni) * 100.0, Cy + 160.0);
								Relief = FMath::Max(Relief, FMath::Abs(Ns - Cs));
							}
							if (Relief < BestRelief)
							{
								BestRelief = Relief;
								BestX = Cx; BestY = Cy; BestSurf = Cs;
							}
						}
					}
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelGITest: flattest column = (%.0f,%.0f) surface %.0f, relief %.0fUU along the span"),
					       BestX, BestY, BestSurf, BestRelief);

					const int32 Count = Terrain->SpawnStructureFixtureAt(BestX, BestY);
					UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGITest: wall+roof fixture stamped (%d voxels)"), Count);

					// Fixture-local geometry, in the same voxel units
					// SpawnStructureFixtureAt uses: span 128 vx (+X), width 32 vx
					// (+Y), roof underside 32 vx (3.2m) above the surface.
					GITestChamberCentreUU = FVector(BestX, BestY, BestSurf);
					GITestSurfaceUU = BestSurf;

					if (bGIBreach)
					{
						GetWorldTimerManager().SetTimer(
							GITestBreachTimerHandle,
							FTimerDelegate::CreateWeakLambda(this,
								[this]()
								{
									UWorld* BWorld = GetWorld();
									UVoxelWorldSubsystem* BTerrain = BWorld ? BWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
									if (!BTerrain)
									{
										return;
									}
									// Punch a ~4m skylight through the middle of the
									// 20cm roof slab.
									const FVector Hole(GITestChamberCentreUU.X + 640.0,
									                   GITestChamberCentreUU.Y + 160.0,
									                   GITestSurfaceUU + 340.0);
									const int32 BreachRemoved = BTerrain->CarveSphere(Hole, 200.0, 0.0);
									UE_LOG(LogVoxelEarth, Log,
									       TEXT("VoxelGITest: BREACH -- roof holed at (%.0f,%.0f,%.0f), removed %d voxels"),
									       Hole.X, Hole.Y, Hole.Z, BreachRemoved);
								}),
							11.f, false);
					}

					// Two-phase framing, matching the -VoxelScreenshotAfter pattern
					// that is already known to work: assert the pose on one timer,
					// capture on a LATER one. Doing both in a single lambda
					// captured the pre-move view.
					auto PoseUnderRoof = [this]()
					{
						UWorld* CWorld = GetWorld();
						APlayerController* Ctrl = CWorld ? CWorld->GetFirstPlayerController() : nullptr;
						if (!Ctrl)
						{
							return;
						}
						// Stand just inside the pillar end, 1.5m up, looking back
						// along the covered span toward the wall, tilted slightly
						// up so the roof underside fills the top of frame. The
						// shaded floor, the shaded slab underside and the sunlit
						// ground past the wall are all in one shot.
						const FVector CamPos(GITestChamberCentreUU.X + 1180.0,
						                     GITestChamberCentreUU.Y + 160.0,
						                     GITestSurfaceUU + 150.0);
						const FRotator Look(6.f, 180.f, 0.f);
						if (APawn* P = Ctrl->GetPawn())
						{
							P->SetActorLocation(CamPos, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
							P->SetActorRotation(Look);
							Ctrl->SetViewTarget(P);
						}
						Ctrl->SetControlRotation(Look);
					};
					auto CaptureUnderRoof = [this, PoseUnderRoof]()
					{
						PoseUnderRoof(); // re-assert, in case the pawn drifted
						if (APlayerController* Ctrl = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
						{
							if (Ctrl->PlayerCameraManager)
							{
								const FVector L = Ctrl->PlayerCameraManager->GetCameraLocation();
								const FRotator R = Ctrl->PlayerCameraManager->GetCameraRotation();
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("VoxelGITest capture: ACTUAL cam=(%.0f,%.0f,%.0f) rot=(pitch %.1f yaw %.1f)"),
								       L.X, L.Y, L.Z, R.Pitch, R.Yaw);
							}
						}
						FScreenshotRequest::RequestScreenshot(TEXT("VoxelGI"), false, true);
					};

					// Pose early (streaming + the GI solve both key off the view
					// origin, so the camera wants to be in position well before
					// the shot), then capture.
					const float ShotDelay = bGIBreach ? 20.f : 14.f;
					GetWorldTimerManager().SetTimer(
						GITestPoseTimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseUnderRoof), 2.f, false);
					GetWorldTimerManager().SetTimer(
						GITestShotTimerHandle, FTimerDelegate::CreateWeakLambda(this, CaptureUnderRoof), ShotDelay, false);
					GetWorldTimerManager().SetTimer(
						GITestQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }),
						ShotDelay + 4.f, false);
				}),
			GITestDelaySeconds, false);
	}
	// ==== end M4 voxel GI verification ======================================

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

					// ADR-0003 item 3 water-memo-test framing: stand back and
					// slightly above the basin so both the settled/redistributed
					// water AND the collapsed roof debris around it are in
					// frame. Highest priority of all -- this switch's whole
					// point is to SEE water react to the edit sequence.
					if (bWaterMemoTestActive && PC)
					{
						const double BasinSurfUU =
							ShotTerrain ? ShotTerrain->GetSurfaceHeightUU(WaterMemoTestBasinXUU, WaterMemoTestBasinYUU) : 0.0;
						const FVector BasinMid(WaterMemoTestBasinXUU, WaterMemoTestBasinYUU, BasinSurfUU + 100.0);
						FVector Subject = BasinMid;
						FVector CamOffset(-1400.0, -1000.0, 900.0); // back / side / up
						if (IsWaterWakeTestMode())
						{
							// -VoxelWaterWakeTest frames much tighter, and on the
							// WATER ITSELF (its actual centroid, wherever the CA
							// left it) rather than on the basin: its subject is the
							// pool that is present before the dig and gone after
							// it, which at the memo run's 14m/10m/9m standoff is a
							// few dozen pixels and reads as terrain. The wide
							// framing stays for -VoxelWaterMemoTest, whose subject
							// is the collapsed roof debris AROUND the basin.
							FVector WaterCentroid = FVector::ZeroVector;
							if (ShotWater && ShotWater->GetStoredWaterCentroidUU(WaterCentroid))
							{
								Subject = WaterCentroid;
							}
							// Stand 8m/6m back at a fixed 5m ABOVE THE SURFACE, not
							// above the subject: the pool's centroid sits several
							// metres down inside the basin, so a camera offset
							// vertically from IT ends up buried in terrain.
							CamOffset = FVector(-800.0, -600.0, (BasinSurfUU + 500.0) - Subject.Z);
						}
						const FVector CamPos = Subject + CamOffset;
						const FRotator Look = (Subject - CamPos).Rotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(CamPos);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					// M5 chop-test framing (task item 5): stand back from the tree
					// column and look at it, so the detached/fallen canopy is in
					// frame. Highest priority when a tree/chop test drove the run.
					// M5 large-edit collapse framing: stand well back and to the
					// side of the wall+roof fixture, level with the roof, so the
					// standing (wall-supported) part of the slab and the fallen
					// span are both in frame. Highest priority of all -- the
					// whole point of this run is to SEE the aftermath.
					else if (bStructureTestActive && PC)
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
					// M6 digging-while-pathing framing (docs/status.md M6
					// section): stand off to the side of the wall column and
					// look along its face, low enough (~2m) to actually see
					// the mined tunnel/hole rather than looking down on the
					// wall's top from above.
					else if (bDigSwarmTestActive && PC)
					{
						const double WallSurfUU =
							ShotTerrain ? ShotTerrain->GetSurfaceHeightUU(DigSwarmTestColumnXUU, DigSwarmTestColumnYUU) : 0.0;
						const FVector WallMid(DigSwarmTestColumnXUU, DigSwarmTestColumnYUU, WallSurfUU + 140.0);
						const FVector CamPos = WallMid + FVector(-450.0, -550.0, 60.0); // back / side / slightly up
						const FRotator Look = (WallMid - CamPos).Rotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(CamPos);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					// M6 gap closure framing (docs/status.md M6 section):
					// same broadside-and-back shape as bDigSwarmTestActive
					// above, just aimed at the Tier1RegionGraphTest wall/gap
					// column instead.
					else if (bTier1RegionGraphTestActive && PC)
					{
						const double WallSurfUU = ShotTerrain
							? ShotTerrain->GetSurfaceHeightUU(Tier1RegionGraphTestColumnXUU, Tier1RegionGraphTestColumnYUU)
							: 0.0;
						const FVector WallMid(Tier1RegionGraphTestColumnXUU, Tier1RegionGraphTestColumnYUU, WallSurfUU + 140.0);
						const FVector CamPos = WallMid + FVector(-450.0, -650.0, 500.0); // back / side / well up (see the whole detour, not just the wall face)
						const FRotator Look = (WallMid - CamPos).Rotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(CamPos);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					// Underground streaming proof: re-assert the pose the carve
					// timer set, so the generic "aim down at the terrain from
					// spawn" branch below cannot yank the camera back into the
					// sky. Re-asserting (rather than trusting the earlier set)
					// also survives anything that moved the pawn in between,
					// e.g. the walk-mode kinematic settle onto the tunnel floor.
					else if (bUndergroundTestActive && PC)
					{
						const FVector Pose = UndergroundTestCameraLocation();
						const FRotator Look = UndergroundTestCameraRotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(Pose);
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

	// ADR-0003 item 3 verification (docs/adr/0003-hydrostatic-persistent-body.md
	// "item 2 resolution"): -VoxelWaterMemoTest[=<delaySeconds>] (default 15s)
	// carves a basin, settles a pool in it, then fires the full edit vocabulary
	// (dig, place, carve, then an M5 structural collapse whose roof spans
	// directly over the basin) around/beneath the water, logging the water
	// digest after every step. This is the SAME public UVoxelWorldSubsystem
	// API real gameplay uses (TryDig/TryPlace/CarveSphere/SpawnStructureFixtureAt),
	// so it exercises the actual notification/invalidation wiring end to end,
	// not a bypass. The proof itself is a CROSS-PROCESS A/B (mirrors the M3
	// determinism-guard convention): run this switch once with
	// -ExecCmds="voxel.Water.SolidCacheEnabled 0" and once with 1 (same seed,
	// same everything else), then diff the two runs' "VoxelWaterMemoTest
	// FINAL" log line -- byte-identical waterDigest across both proves the
	// memo's invalidation contract is fully honoured by this edit sequence.
	// The structure fixture's far-pillar blast is positioned with the SAME
	// relative offsets -VoxelCollapseTest already uses (proven to trigger the
	// brick-resolution collapse path, docs/status.md M5 section), just at a
	// column chosen so its roof overhangs the basin -- see BasinX/Y below.
	//
	// HISTORICAL NOTE + the wake-test variant. Each edit below is followed by
	// a small SpawnWaterAt "nudge". Those nudges are test-harness-only and
	// exist for a reason that is now FIXED: a fully settled body
	// (activeBricks==0) touches nothing on subsequent step() calls
	// (waterca.h "Activity / settling"), and at the time this switch was
	// written a terrain edit could not wake it -- the first pass at this
	// scenario produced an UNCHANGING digest through every dig/place/carve/
	// structure/collapse. The nudge injected volume purely to force the CA
	// back into an active state so the memo-safety question could be tested
	// at all.
	//
	// UVoxelWaterSubsystem::NotifyTerrainRegionEdited now calls
	// vxc::WaterCA::wakeRegion (voxelcore/waterca.h "Terrain-edit
	// reactivation") on every edit, so the nudges are no longer necessary for
	// water to react. They are KEPT here so the memo A/B keeps producing the
	// exact same digests it was signed off with; pass -VoxelWaterWakeTest
	// INSTEAD of -VoxelWaterMemoTest to run the identical sequence with every
	// nudge suppressed (IsWaterWakeTestMode above). In that mode nothing but
	// the terrain edits themselves can move the water, and total volume must
	// be identical at every logged checkpoint -- which is exactly the
	// "settled pond actually drains when you dig under it, and conserves"
	// proof (docs/status.md, "Water reactivation on terrain edits").
	float WaterMemoTestDelaySeconds = 15.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMemoTest="), WaterMemoTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMemoTest")) ||
	    FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterWakeTest="), WaterMemoTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterWakeTest")))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelWaterMemoTest: starting basin/pour/dig/place/carve/collapse sequence in %.1fs"),
		       WaterMemoTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			WaterMemoTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* TestWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = TestWorld ? TestWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelWaterSubsystem* Water = TestWorld ? TestWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					if (!Terrain || !Water)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelWaterMemoTest: subsystems not ready, skipping."));
						return;
					}

					double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
					ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);
					// 50m ahead: clear of every other test fixture's own offset
					// (all <=1240 UU / 12.4m), and still inside R0's 64m load
					// radius so streaming isn't the bottleneck.
					const double StructAnchorX = SpawnColXUU + 5000.0;
					const double StructAnchorY = SpawnColYUU;
					// Under the middle of the roof span (roof covers local
					// X:[0,1270], Y:[0,310] UU -- see SpawnStructureFixtureAt),
					// and past the wall's ~480UU cantilever budget, so the
					// collapse's removed voxels land directly above/around it.
					const double BasinX = StructAnchorX + 800.0;
					const double BasinY = StructAnchorY + 160.0;
					bWaterMemoTestActive = true;
					WaterMemoTestBasinXUU = BasinX;
					WaterMemoTestBasinYUU = BasinY;

					const double SurfUU = Terrain->GetSurfaceHeightUU(BasinX, BasinY);
					// Deliberately shallow (max depth 100+350=450UU=4.5m below
					// surface, dead-center) so every dig/place raycast below can
					// reach its target within DigPlaceRangeMeters (8m/800UU) from
					// a camera comfortably above the surface -- an earlier,
					// deeper basin (center -250, radius 550) put the floor out
					// of an 8m ray's reach entirely (TryDig/TryPlace both
					// silently returned false, applied=0 -- verified in the log,
					// not assumed).
					constexpr double BasinCenterDownUU = 100.0;
					constexpr double BasinRadiusUU = 350.0; // 3.5m
					const int32 CarvedBasin =
						Terrain->CarveSphere(FVector(BasinX, BasinY, SurfUU - BasinCenterDownUU), BasinRadiusUU, 0.0);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelWaterMemoTest: carved basin (%d voxels) at (%.0f,%.0f,%.0f); memoEnabled=%d"),
					       CarvedBasin, BasinX, BasinY, SurfUU, (int32)Water->IsSolidCacheEnabled());

					GetWorldTimerManager().SetTimer(
						WaterMemoTestTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this, BasinX, BasinY, SurfUU]()
							{
								UWorld* PourWorld = GetWorld();
								UVoxelWaterSubsystem* PourWater = PourWorld ? PourWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
								if (!PourWater)
								{
									return;
								}
								constexpr uint32 PourAmount = 20000;
								const uint32 Placed =
									PourWater->SpawnWaterAt(FVector(BasinX, BasinY, SurfUU + 150.0), PourAmount);
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("VoxelWaterMemoTest: poured %u/%u fill units into the basin; digest=0x%016llX"),
								       Placed, PourAmount, (unsigned long long)PourWater->GetWaterDigest());

								GetWorldTimerManager().SetTimer(
									WaterMemoTestTimerHandle,
									FTimerDelegate::CreateWeakLambda(this,
										[this, BasinX, BasinY, SurfUU]()
										{
											UWorld* DigWorld = GetWorld();
											UVoxelWorldSubsystem* DigTerrain =
												DigWorld ? DigWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
											UVoxelWaterSubsystem* DigWater =
												DigWorld ? DigWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
											if (!DigTerrain || !DigWater)
											{
												return;
											}
											UE_LOG(LogVoxelEarth, Log,
											       TEXT("VoxelWaterMemoTest settle-check (pre-edit): digest=0x%016llX volume=%llu"),
											       (unsigned long long)DigWater->GetWaterDigest(),
											       (unsigned long long)DigWater->GetPerfSnapshot().TotalVolume);

											// DIG: straight down through the settled
											// pool onto the basin floor -- the raycast
											// ignores water (terrain-only), so this
											// opens a hole beneath it exactly like
											// task item 5's "flow into a hole you dig
											// under it" verification. Camera 3m above
											// the surface, dead-center over the basin
											// (where the pour settled) -- within the
											// 8m dig range of the ~4.5m-deep floor.
											const bool bDug = DigTerrain->TryDig(
												FVector(BasinX, BasinY, SurfUU + 300.0), FVector(0.0, 0.0, -1.0), 4);
											UE_LOG(LogVoxelEarth, Log,
											       TEXT("VoxelWaterMemoTest: dig beneath basin -> applied=%d"), (int32)bDug);

											// Test-harness nudge (see the switch's own
											// doc comment above): a settled body does
											// not reactivate on its own from a terrain
											// edit alone, so re-pour a small amount to
											// put the CA back into an active state and
											// let it actually re-examine the
											// (memo-invalidated) solidity near the dig.
											if (!IsWaterWakeTestMode())
											{
												DigWater->SpawnWaterAt(FVector(BasinX, BasinY, SurfUU + 150.0), 300);
											}
											UE_LOG(LogVoxelEarth, Log,
											       TEXT("VoxelWaterMemoTest: post-dig %s digest=0x%016llX volume=%llu activeBricks=%lld"),
											       IsWaterWakeTestMode() ? TEXT("(wakeRegion only, no nudge)") : TEXT("nudge"),
											       (unsigned long long)DigWater->GetWaterDigest(),
											       (unsigned long long)DigWater->GetPerfSnapshot().TotalVolume,
											       DigWater->GetPerfSnapshot().ActiveBricks);

											GetWorldTimerManager().SetTimer(
												WaterMemoTestTimerHandle,
												FTimerDelegate::CreateWeakLambda(this,
													[this, BasinX, BasinY, SurfUU]()
													{
														UWorld* PlaceWorld = GetWorld();
														UVoxelWorldSubsystem* PlaceTerrain =
															PlaceWorld ? PlaceWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
														UVoxelWaterSubsystem* PlaceWater =
															PlaceWorld ? PlaceWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
														if (!PlaceTerrain || !PlaceWater)
														{
															return;
														}
														UE_LOG(LogVoxelEarth, Log,
														       TEXT("VoxelWaterMemoTest: post-dig settled digest=0x%016llX volume=%llu"),
														       (unsigned long long)PlaceWater->GetWaterDigest(),
						       (unsigned long long)PlaceWater->GetPerfSnapshot().TotalVolume);

														// PLACE: a solid cube near the
														// basin's floor, offset from the
														// dig column, well clear of the
														// player (task item 5's "displaced
														// by a block you place"). Same
														// camera-height reasoning as the dig
														// above -- within 8m of the floor.
														constexpr uint8 MatRock = 2; // vxc::MAT_ROCK
														const bool bPlaced = PlaceTerrain->TryPlace(
															FVector(BasinX + 100.0, BasinY, SurfUU + 300.0), FVector(0.0, 0.0, -1.0),
															4, MatRock, FVector(1.0e7, 1.0e7, 1.0e7));
														UE_LOG(LogVoxelEarth, Log,
														       TEXT("VoxelWaterMemoTest: place at basin floor -> applied=%d"), (int32)bPlaced);

														// Test-harness nudge (see doc comment
														// above the switch parse block).
														if (!IsWaterWakeTestMode())
														{
															PlaceWater->SpawnWaterAt(FVector(BasinX, BasinY, SurfUU + 150.0), 300);
														}
														UE_LOG(LogVoxelEarth, Log,
														       TEXT("VoxelWaterMemoTest: post-place %s digest=0x%016llX volume=%llu activeBricks=%lld"),
														       IsWaterWakeTestMode() ? TEXT("(wakeRegion only, no nudge)") : TEXT("nudge"),
														       (unsigned long long)PlaceWater->GetWaterDigest(),
														       (unsigned long long)PlaceWater->GetPerfSnapshot().TotalVolume,
														       PlaceWater->GetPerfSnapshot().ActiveBricks);

														GetWorldTimerManager().SetTimer(
															WaterMemoTestTimerHandle,
															FTimerDelegate::CreateWeakLambda(this,
																[this, BasinX, BasinY, SurfUU]()
																{
																	UWorld* CarveWorld = GetWorld();
																	UVoxelWorldSubsystem* CarveTerrain =
																		CarveWorld ? CarveWorld->GetSubsystem<UVoxelWorldSubsystem>()
																		           : nullptr;
																	UVoxelWaterSubsystem* CarveWater =
																		CarveWorld ? CarveWorld->GetSubsystem<UVoxelWaterSubsystem>()
																		           : nullptr;
																	if (!CarveTerrain || !CarveWater)
																	{
																		return;
																	}
																	UE_LOG(LogVoxelEarth, Log,
																	       TEXT("VoxelWaterMemoTest: post-place settled digest=0x%016llX volume=%llu"),
																	       (unsigned long long)CarveWater->GetWaterDigest(),
						       (unsigned long long)CarveWater->GetPerfSnapshot().TotalVolume);

																	// CARVE: breach a side channel next
																	// to the basin.
																	const int32 CarvedSide = CarveTerrain->CarveSphere(
																		FVector(BasinX - 500.0, BasinY, SurfUU - 100.0), 300.0, 60.0);
																	UE_LOG(LogVoxelEarth, Log,
																	       TEXT("VoxelWaterMemoTest: side carve (%d voxels)"), CarvedSide);

																	// Test-harness nudge (see doc comment
																	// above the switch parse block).
																	if (!IsWaterWakeTestMode())
																	{
																		CarveWater->SpawnWaterAt(FVector(BasinX, BasinY, SurfUU + 150.0), 300);
																	}
																	UE_LOG(LogVoxelEarth, Log,
																	       TEXT("VoxelWaterMemoTest: post-carve %s digest=0x%016llX volume=%llu activeBricks=%lld"),
																	       IsWaterWakeTestMode() ? TEXT("(wakeRegion only, no nudge)") : TEXT("nudge"),
																	       (unsigned long long)CarveWater->GetWaterDigest(),
																	       (unsigned long long)CarveWater->GetPerfSnapshot().TotalVolume,
																	       CarveWater->GetPerfSnapshot().ActiveBricks);

																	GetWorldTimerManager().SetTimer(
																		WaterMemoTestTimerHandle,
																		FTimerDelegate::CreateWeakLambda(this,
																			[this, BasinX, BasinY, SurfUU]()
																			{
																				UWorld* StructWorld = GetWorld();
																				UVoxelWorldSubsystem* StructTerrain =
																					StructWorld ? StructWorld->GetSubsystem<UVoxelWorldSubsystem>()
																					            : nullptr;
																				UVoxelWaterSubsystem* StructWater =
																					StructWorld ? StructWorld->GetSubsystem<UVoxelWaterSubsystem>()
																					            : nullptr;
																				if (!StructTerrain || !StructWater)
																				{
																					return;
																				}
																				UE_LOG(LogVoxelEarth, Log,
																				       TEXT("VoxelWaterMemoTest: post-carve settled digest=0x%016llX volume=%llu"),
																				       (unsigned long long)StructWater->GetWaterDigest(),
						       (unsigned long long)StructWater->GetPerfSnapshot().TotalVolume);
																				double SpawnColXUU2 = 0.0, SpawnColYUU2 = 0.0;
																				ParseSpawnColumnUU(SpawnColXUU2, SpawnColYUU2);
																				const double StructAnchorX2 = SpawnColXUU2 + 5000.0;
																				const double StructAnchorY2 = SpawnColYUU2;
																				// STRUCTURE: a wall+roof+far-pillars
																				// fixture whose roof spans directly
																				// over the basin (non-air MAT_ROCK
																				// edit right above/around water --
																				// exercises the memo fix in
																				// SpawnStructureFixtureAt itself).
																				const int32 StampedCount =
																					StructTerrain->SpawnStructureFixtureAt(StructAnchorX2, StructAnchorY2);
																				UE_LOG(LogVoxelEarth, Log,
																				       TEXT("VoxelWaterMemoTest: structure fixture (%d voxels) over basin -> digest=0x%016llX"),
																				       StampedCount, (unsigned long long)StructWater->GetWaterDigest());

																				GetWorldTimerManager().SetTimer(
																					WaterMemoTestTimerHandle,
																					FTimerDelegate::CreateWeakLambda(this,
																						[this, StructAnchorX2, StructAnchorY2, BasinX, BasinY, SurfUU]()
																						{
																							UWorld* BlastWorld = GetWorld();
																							UVoxelWorldSubsystem* BlastTerrain =
																								BlastWorld ? BlastWorld->GetSubsystem<UVoxelWorldSubsystem>()
																								           : nullptr;
																							UVoxelWaterSubsystem* BlastWater =
																								BlastWorld ? BlastWorld->GetSubsystem<UVoxelWaterSubsystem>()
																								           : nullptr;
																							if (!BlastTerrain || !BlastWater)
																							{
																								return;
																							}
																							// COLLAPSE: blow out the far
																							// pillars (same relative offsets
																							// -VoxelCollapseTest uses) --
																							// the roof above the basin loses
																							// support past the wall's
																							// cantilever budget and comes
																							// down via the M5 edit-log
																							// collapse path.
																							const double BlastSurfUU =
																								BlastTerrain->GetSurfaceHeightUU(StructAnchorX2, StructAnchorY2);
																							const int32 BlastRemoved = BlastTerrain->CarveSphere(
																								FVector(StructAnchorX2 + 1240.0, StructAnchorY2 + 160.0,
																								        BlastSurfUU + 80.0),
																								260.0, 0.0);
																							UE_LOG(LogVoxelEarth, Log,
																							       TEXT("VoxelWaterMemoTest: collapse blast (%d voxels)"), BlastRemoved);

																							// Test-harness nudge (see doc
																							// comment above the switch parse
																							// block).
																							if (!IsWaterWakeTestMode())
																							{
																								BlastWater->SpawnWaterAt(FVector(BasinX, BasinY, SurfUU + 150.0), 300);
																							}
																							UE_LOG(LogVoxelEarth, Log,
																							       TEXT("VoxelWaterMemoTest: post-collapse %s digest=0x%016llX volume=%llu activeBricks=%lld"),
																							       IsWaterWakeTestMode() ? TEXT("(wakeRegion only, no nudge)") : TEXT("nudge"),
																							       (unsigned long long)BlastWater->GetWaterDigest(),
																							       (unsigned long long)BlastWater->GetPerfSnapshot().TotalVolume,
																							       BlastWater->GetPerfSnapshot().ActiveBricks);

																							GetWorldTimerManager().SetTimer(
																								WaterMemoTestTimerHandle,
																								FTimerDelegate::CreateWeakLambda(this,
																									[this]()
																									{
																										UWorld* FinalWorld = GetWorld();
																										UVoxelWorldSubsystem* FinalTerrain =
																											FinalWorld ? FinalWorld->GetSubsystem<UVoxelWorldSubsystem>()
																											           : nullptr;
																										UVoxelWaterSubsystem* FinalWater =
																											FinalWorld ? FinalWorld->GetSubsystem<UVoxelWaterSubsystem>()
																											           : nullptr;
																										if (!FinalWater)
																										{
																											return;
																										}
																										const FVoxelWaterPerfSnapshot Snap =
																											FinalWater->GetPerfSnapshot();
																										// The A/B comparison line: diff
																										// this across two runs (memo
																										// cvar 0 vs 1, same seed) --
																										// waterDigest must match exactly.
																										UE_LOG(LogVoxelEarth, Log,
																										       TEXT("VoxelWaterMemoTest FINAL: memoEnabled=%d waterDigest=0x%016llX ")
																										       TEXT("editedDigest=0x%016llX activeBricks=%lld storedBricks=%lld volume=%llu"),
																										       (int32)FinalWater->IsSolidCacheEnabled(),
																										       (unsigned long long)FinalWater->GetWaterDigest(),
																										       (unsigned long long)(FinalTerrain ? FinalTerrain->GetEditedDigest() : 0),
																										       Snap.ActiveBricks, Snap.StoredBricks,
																										       (unsigned long long)Snap.TotalVolume);

																										// Self-quit for a bare headless
																										// gate run (no screenshot
																										// requested) -- mirrors
																										// -VoxelDumpDigestAfter's
																										// convenience. A combined
																										// -VoxelScreenshotAfter run
																										// owns its own quit timer
																										// instead, scheduled well
																										// after this one fires.
																										float Throwaway = 0.f;
																										const bool bScreenshotRequested =
																											FParse::Value(FCommandLine::Get(), TEXT("VoxelScreenshotAfter="), Throwaway) &&
																											Throwaway > 0.f;
																										if (!bScreenshotRequested)
																										{
																											GetWorldTimerManager().SetTimer(
																												WaterMemoTestTimerHandle,
																												FTimerDelegate::CreateLambda(
																													[]() { FPlatformMisc::RequestExit(/*bForce*/ false); }),
																												5.f, false);
																										}
																									}),
																								8.f, false);
																						}),
																					3.f, false);
																			}),
																		6.f, false);
																}),
															6.f, false);
													}),
												6.f, false);
										}),
									10.f, false);
							}),
						3.f, false);
				}),
			WaterMemoTestDelaySeconds, false);
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

	// (Underground camera pose helpers are defined just below BeginPlay.)

	// Underground streaming proof (docs/status.md "Underground streaming
	// (vertical footprint)"): -VoxelUndergroundTest[=<delaySeconds>] (default
	// 10s) carves a shaft down from the spawn column, a short horizontal tunnel
	// off its foot and a chamber at the tunnel's end, then parks the pawn in
	// the chamber (or, with -VoxelUndergroundView=shaft, at the shaft foot
	// looking along the tunnel). Every carve goes through the ordinary
	// CarveSphere edit-log authority path -- exactly what a player's explosive
	// does -- so this fixture changes nothing about worldgen or determinism, it
	// just makes an air pocket in a place the streaming system previously
	// refused to mesh. Pair with -VoxelScreenshotAfter=<larger value>.
	float UndergroundTestDelaySeconds = 10.f;
	const bool bUndergroundTestRequested =
		FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundTest="), UndergroundTestDelaySeconds) ||
		FParse::Param(FCommandLine::Get(), TEXT("VoxelUndergroundTest"));
	if (bUndergroundTestRequested)
	{
		bUndergroundTestActive = true;

		float DepthMeters = 24.f;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundDepth="), DepthMeters) && DepthMeters > 4.f)
		{
			UndergroundTestDepthUU = double(DepthMeters) * 100.0;
		}

		double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
		ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);
		UndergroundTestColumnXUU = SpawnColXUU;
		UndergroundTestColumnYUU = SpawnColYUU;

		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelUndergroundTest: carving shaft+tunnel+chamber at (%.0f,%.0f), %.1fm down, in %.1fs"),
		       UndergroundTestColumnXUU, UndergroundTestColumnYUU, UndergroundTestDepthUU / 100.0,
		       UndergroundTestDelaySeconds);
		GetWorldTimerManager().SetTimer(
			UndergroundTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* DigWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = DigWorld ? DigWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelUndergroundTest: subsystem not ready, skipping."));
						return;
					}
					UndergroundTestSurfaceUU =
						Subsystem->GetSurfaceHeightUU(UndergroundTestColumnXUU, UndergroundTestColumnYUU);

					// Geometry, all in UU (1 UU = 1cm). The shaft is a stack of
					// overlapping spheres (CarveSphere is the only carve
					// primitive, and it is what an explosive uses); 100UU
					// spacing against a 160UU radius keeps the column
					// continuous with no scalloped gaps between spheres.
					// Kept deliberately SMALL: these carves go through the same
					// large-edit path as an explosive, and an oversized chamber
					// both costs a long structural-collapse prepass per carve
					// and takes the shot further from "a player dug this".
					constexpr double ShaftRadiusUU = 110.0;
					constexpr double ShaftStepUU = 80.0;
					constexpr double TunnelRadiusUU = 130.0;
					constexpr double TunnelLengthUU = 900.0;
					constexpr double ChamberRadiusUU = 220.0;
					constexpr double JitterUU = 12.0; // slight raggedness, same as an explosive's edge
					const double FloorZUU = UndergroundTestSurfaceUU - UndergroundTestDepthUU;

					int32 Removed = 0;
					// Shaft: from a little ABOVE the surface (so the mouth is
					// open to the sky and the shot proves a real hole, not a
					// sealed bubble) down to the tunnel level.
					for (double Z = UndergroundTestSurfaceUU + 200.0; Z >= FloorZUU; Z -= ShaftStepUU)
					{
						Removed += Subsystem->CarveSphere(
							FVector(UndergroundTestColumnXUU, UndergroundTestColumnYUU, Z), ShaftRadiusUU, JitterUU);
					}
					// Horizontal tunnel off the shaft foot, running +X.
					for (double D = 0.0; D <= TunnelLengthUU; D += ShaftStepUU)
					{
						Removed += Subsystem->CarveSphere(
							FVector(UndergroundTestColumnXUU + D, UndergroundTestColumnYUU, FloorZUU), TunnelRadiusUU,
							JitterUU);
					}
					// Chamber at the tunnel's end.
					Removed += Subsystem->CarveSphere(
						FVector(UndergroundTestColumnXUU + TunnelLengthUU, UndergroundTestColumnYUU, FloorZUU),
						ChamberRadiusUU, JitterUU);

					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelUndergroundTest: carved %d voxels; surface=%.0f floor=%.0f digest=0x%016llX."),
					       Removed, UndergroundTestSurfaceUU, FloorZUU,
					       (unsigned long long)Subsystem->GetEditedDigest());

					// Park the pawn underground immediately, so the streaming
					// system has the remaining seconds before the screenshot to
					// build the deep set around it (the whole point of the
					// fixture -- the anchor being underground is what triggers
					// the deep box).
					if (APlayerController* PC = DigWorld->GetFirstPlayerController())
					{
						const FVector Pose = UndergroundTestCameraLocation();
						const FRotator Look = UndergroundTestCameraRotation();
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(Pose);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelUndergroundTest: pawn parked at (%.0f,%.0f,%.0f)."),
						       Pose.X, Pose.Y, Pose.Z);

						// Enclosure probe: six axis rays from the parked pose.
						// This is the ground truth for "is there a world down
						// here" -- independent of anything the renderer does,
						// it asks voxel-core directly how far the rock is in
						// each direction. A miss on -Z is literally the bug
						// this task is about (no floor under your feet).
						const FVector Dirs[6] = {FVector(1, 0, 0),  FVector(-1, 0, 0), FVector(0, 1, 0),
						                         FVector(0, -1, 0), FVector(0, 0, 1),  FVector(0, 0, -1)};
						const TCHAR* Names[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
						FString Report;
						for (int32 I = 0; I < 6; ++I)
						{
							FVector Hit, Prev;
							const bool bHit = Subsystem->RaycastVoxelWorld(Pose, Dirs[I], 6000.0, Hit, Prev);
							Report += FString::Printf(TEXT("%s=%s "), Names[I],
							                          bHit ? *FString::Printf(TEXT("%.1fm"), (Hit - Pose).Size() / 100.0)
							                               : TEXT("MISS"));
						}
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelUndergroundTest: enclosure probe (60m rays): %s"), *Report);
					}
				}),
			UndergroundTestDelaySeconds, false);
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

					// M6 gap closure verification (docs/status.md M6 section
					// "Tier-1 hierarchical planning + NPC replication"):
					// -VoxelReplicationSelfTest exercises UVoxelAgentSubsystem::
					// CollectReplicationSnapshot -> ApplyReplicatedAgentSnapshot
					// IN-PROCESS (no real network hop) right after the swarm
					// spawns -- see RunReplicationSelfTest's doc comment for
					// exactly what this does and doesn't prove. A genuine
					// cross-process replication test (two headless processes,
					// real UDP) was ALSO attempted for this gap closure but the
					// client-side handshake did not complete in this sandboxed
					// dev environment (the server accepts the connection, but
					// the client's own connection times out waiting for a
					// reply -- consistent with a firewall/loopback constraint
					// this account has no permission to fix, not a bug in the
					// replication code) -- this self-test is the fallback
					// verification for the actual snapshot/quantize/interpolate
					// logic.
					if (FParse::Param(FCommandLine::Get(), TEXT("VoxelReplicationSelfTest")))
					{
						AgentSubsystem->RunReplicationSelfTest();
					}
				}),
			SwarmSpawnDelaySeconds, false);
	}

	// M6 digging-while-pathing verification (docs/status.md M6 section
	// "Digging-while-pathing"): -VoxelDigSwarmTest[=<N>] (default N=10) is
	// the decisive scenario the M6 payoff needs -- place the player behind a
	// diggable wall so the ONLY short route to them is through it, spawn a
	// small (legible) swarm on the far side, and let the SAME cost function
	// (voxel-core/include/voxelcore/pathfind.h) that chooses Walk/StepUp/
	// Climb/Fall/Jump everywhere else in this subsystem choose Mine instead,
	// live, authoritatively, through UVoxelWorldSubsystem::TryDig/TryPlace.
	//
	// The wall itself is built here (test-fixture code, same "no authored
	// content yet, build fixtures from code" precedent as
	// UVoxelWorldSubsystem::SpawnTreeFixtureAt) via repeated
	// UVoxelWorldSubsystem::TryPlace calls -- the SAME authoritative
	// raycast-and-place API the agents themselves use for Bridge, so this
	// fixture doubles as an independent exercise of that call path before
	// any agent relies on it. TryPlace can only snap a new cube against an
	// EXISTING solid face (it cannot place a fully floating voxel), so the
	// wall is built bottom-up, one straight-down raycast per 4x4x4-voxel
	// cube: layer 0 snaps against natural terrain, every layer after snaps
	// against the cube placed directly below it.
	//
	// Sizing is deliberately larger than a single Tier 0 search window
	// (UVoxelAgentSubsystem::WindowHalfExtentVoxels/WindowHalfHeightVoxels,
	// ~3.3m horizontal x 2.5m vertical) on every side that would let an
	// agent avoid digging: 4m wide x 2.8m tall x 40cm (4-voxel) thick. A
	// SINGLE windowed search can therefore neither step over the top nor
	// find either end of the wall -- the only route the cost function can
	// find within one window is straight through the (thin, cheap-to-mine)
	// 4-voxel thickness. Agents are spawned via
	// UVoxelAgentSubsystem::SpawnSwarmAtOffset -- NOT the random-360-degree
	// SpawnSwarm -- specifically so every one of them starts beyond the
	// wall's far face, guaranteeing the wall actually sits between them and
	// the player rather than leaving some agents with a clear line already.
	//
	// Combine with -VoxelScreenshotAfter=<seconds> (larger than this delay)
	// for a visual capture of the tunnel/hole. For the "digging disabled"
	// control run (proving the COST FUNCTION, not hardcoded behavior,
	// drives this), add -ExecCmds="voxel.NPCDig.Enabled 0" -- see that
	// cvar's doc comment (VoxelAgentSubsystem.cpp) for why disabling it
	// changes PlanPath's cost config for every tier, not just execution.
	int32 DigSwarmTestCount = 10;
	// NOT named bDigSwarmTestActive -- that name is the MEMBER (VoxelEarthGameMode.h)
	// the screenshot-framing block below reads; it's only set true once the
	// wall is actually built (inside the timer callback), not just once the
	// switch is parsed here.
	const bool bDigSwarmTestRequested = FParse::Value(FCommandLine::Get(), TEXT("VoxelDigSwarmTest="), DigSwarmTestCount) ||
	                                     FParse::Param(FCommandLine::Get(), TEXT("VoxelDigSwarmTest"));
	if (bDigSwarmTestRequested)
	{
		// 10s, not -VoxelSwarmTest's 2s: a little extra margin before the
		// pawn/subsystems are guaranteed ready, since (unlike SpawnSwarm,
		// which only reads GetSurfaceHeightUU, a pure amplifier query) the
		// wall build below raycasts through UVoxelWorldSubsystem::TryPlace
		// at BeginPlay. NOTE for anyone re-running this switch by hand: the
		// wall/agent edits from a PRIOR run persist (M3 wave 2 "Save/load" --
		// UVoxelWorldSubsystem autoloads Saved/VoxelWorlds/<seed>.vxlog on
		// startup and autosaves on shutdown), so a repeated run on top of a
		// leftover save can land the wall raycast on an already-tunneled
		// wall instead of fresh terrain -- delete that .vxlog (or pass a
		// fresh -VoxelSeed=<N>) for a clean, reproducible repeat. Mirrors
		// -VoxelHeadlessDigTest's identical
		// reasoning for its own (20s) delay.
		constexpr float DigSwarmSpawnDelaySeconds = 10.f;
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDigSwarmTest: building a wall + spawning %d agents beyond it in %.1fs"),
		       DigSwarmTestCount, DigSwarmSpawnDelaySeconds);
		GetWorldTimerManager().SetTimer(
			DigSwarmTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, DigSwarmTestCount]()
				{
					UWorld* TestWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = TestWorld ? TestWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelAgentSubsystem* AgentSubsystem = TestWorld ? TestWorld->GetSubsystem<UVoxelAgentSubsystem>() : nullptr;
					APlayerController* PC = TestWorld ? TestWorld->GetFirstPlayerController() : nullptr;
					APawn* Pawn = PC ? PC->GetPawn() : nullptr;
					if (!Terrain || !AgentSubsystem || !Pawn)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelDigSwarmTest: subsystems/pawn not ready, skipping."));
						return;
					}

					const FVector PlayerLoc = Pawn->GetActorLocation();

					constexpr double WallOffsetXUU = 600.0;  // 6m ahead of the player (+X)
					constexpr int32 WallColumns = 10;        // 10 x 4-voxel cubes = 40 voxels = 4m wide (Y)
					constexpr int32 WallLayers = 7;           // 7 x 4-voxel cubes = 28 voxels = 2.8m tall (Z)
					constexpr double CubeEdgeUU = 40.0;       // 4 voxels x VoxelCoords::VoxelSizeUU (10UU/voxel)
					constexpr int32 CubeSizeVoxels = 4;       // UVoxelWorldSubsystem::MaxCubeSizeVoxels
					constexpr uint8 WallMaterialId = 2;       // vxc::MAT_ROCK (voxelcore/core.h) -- hard, per
					                                           // kMineCostByMaterial (VoxelAgentSubsystem.cpp)

					const double WallCenterX = PlayerLoc.X + WallOffsetXUU;
					const double WallCenterY = PlayerLoc.Y;
					const double WallMinY = WallCenterY - (double(WallColumns) * CubeEdgeUU) * 0.5;

					int32 CubesPlaced = 0;
					for (int32 ColIdx = 0; ColIdx < WallColumns; ++ColIdx)
					{
						const double ColumnX = WallCenterX;
						const double ColumnY = WallMinY + double(ColIdx) * CubeEdgeUU + CubeEdgeUU * 0.5;
						// Tracks (approximately -- see below) where the next
						// layer's cube should rest; the raycast always
						// starts comfortably ABOVE this, so any small drift
						// between this estimate and the actual lattice-
						// snapped cube top self-corrects every layer (the
						// ray finds whatever is REALLY topmost, not this
						// estimate).
						double NextLayerBottomZ = Terrain->GetSurfaceHeightUU(ColumnX, ColumnY);
						for (int32 LayerIdx = 0; LayerIdx < WallLayers; ++LayerIdx)
						{
							const FVector CameraLoc(ColumnX, ColumnY, NextLayerBottomZ + CubeEdgeUU * 3.0);
							const FVector DownDir(0.0, 0.0, -1.0);
							// PlayerActorLocation pushed far away: the wall
							// is built well clear of the player's actual
							// position, so TryPlace's overlap-reject check
							// can never legitimately fire here.
							const bool bPlaced = Terrain->TryPlace(CameraLoc, DownDir, CubeSizeVoxels, WallMaterialId,
							                                        PlayerLoc + FVector(1.0e7, 1.0e7, 1.0e7));
							if (bPlaced)
							{
								++CubesPlaced;
							}
							NextLayerBottomZ += CubeEdgeUU;
						}
					}
					const int32 CubesExpected = WallColumns * WallLayers;
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelDigSwarmTest: wall built at X=%.0f, Y=[%.0f,%.0f], Z from ground +%.0fUU -- %d/%d cubes placed."),
					       WallCenterX, WallMinY, WallMinY + double(WallColumns) * CubeEdgeUU, double(WallLayers) * CubeEdgeUU,
					       CubesPlaced, CubesExpected);
					if (CubesPlaced < CubesExpected)
					{
						// Diagnosable, not silent: a shortfall here means some
						// column's TryPlace raycast missed (most likely that
						// column's terrain/materialAt data wasn't resolved
						// yet this early in a headless run -- see
						// DigSwarmSpawnDelaySeconds' doc comment above) --
						// the wall may have gaps an agent could walk through
						// without digging, which would understate the demo.
						UE_LOG(LogVoxelEarth, Warning,
						       TEXT("VoxelDigSwarmTest: wall is INCOMPLETE (%d/%d cubes) -- some columns' terrain may not have been ")
						       TEXT("resolved yet; consider a larger DigSwarmSpawnDelaySeconds if this recurs."),
						       CubesPlaced, CubesExpected);
					}

					// -VoxelScreenshotAfter framing (see that block's
					// bDigSwarmTestActive branch): the REAL build column, set
					// here (not earlier) so it always matches where the wall
					// actually landed.
					bDigSwarmTestActive = true;
					DigSwarmTestColumnXUU = WallCenterX;
					DigSwarmTestColumnYUU = WallCenterY;

					// Spawn beyond the wall's far face (+X past the wall's
					// 4-voxel thickness), so the ONLY short route back to
					// the player crosses it.
					constexpr double LateralJitterUU = 150.0;
					const double SpawnOffsetUU = WallOffsetXUU + CubeEdgeUU + 250.0; // wall far face + 2.5m clearance
					const int32 Spawned =
						AgentSubsystem->SpawnSwarmAtOffset(DigSwarmTestCount, PlayerLoc, FVector(1.0, 0.0, 0.0), SpawnOffsetUU, LateralJitterUU);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelDigSwarmTest: spawned %d/%d agents beyond the wall -- watch for 'VoxelAgent N: Mine/Bridge' and ")
					       TEXT("'VoxelSwarm NPC edits this tick' log lines (LogVoxelEarth)."),
					       Spawned, DigSwarmTestCount);
				}),
			DigSwarmSpawnDelaySeconds, false);
	}

	// M6 gap closure verification (docs/status.md M6 section "Tier-1
	// hierarchical planning + NPC replication"): -VoxelTier1RegionGraphTest[=<N>]
	// (default N=3) is the decisive scenario for BOTH halves of the gap
	// closure this switch exists to prove: (1) Tier 1 agents actually plan
	// via UVoxelAgentSubsystem::PlanPathTier1's hierarchical path (not the
	// pre-M6 fine fallback) -- guaranteed by spawning strictly inside the
	// region graph's coverage box, AND (2) dirty invalidation actually
	// re-routes a live agent -- proven by digging a gap partway through the
	// run and watching the corridor cost drop on the next replan.
	//
	// Unlike -VoxelDigSwarmTest's wall (built right next to the player, for
	// Tier 0's dig-while-pathing demo), this wall sits at WallOffsetXUU (8m)
	// -- inside UVoxelAgentSubsystem::Tier1GraphHorizontalRadiusRegions'
	// ~19.2m-per-axis coverage box around the player (MEASURED down from an
	// original, much bigger box that hung for 95+ seconds without
	// completing a single buildRegionGraph call -- see that constant's doc
	// comment) -- and the agents spawn at SpawnOffsetUU (17m), which is
	// ALWAYS classified Tier 1 from their very first tick (beyond
	// Tier0EnterUU=15m -- the MINIMUM distance for Tier 1 at all -- well
	// under Tier1ExitUU=100m) and also inside the graph's ~19.2m coverage,
	// so PlanPathTier1 takes the hierarchical branch, not the fallback, for
	// this whole test.
	int32 Tier1RegionGraphTestCount = 3;
	const bool bTier1RegionGraphTestRequested =
		FParse::Value(FCommandLine::Get(), TEXT("VoxelTier1RegionGraphTest="), Tier1RegionGraphTestCount) ||
		FParse::Param(FCommandLine::Get(), TEXT("VoxelTier1RegionGraphTest"));
	if (bTier1RegionGraphTestRequested)
	{
		constexpr float Tier1TestSpawnDelaySeconds = 10.f;
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelTier1RegionGraphTest: building a wall + spawning %d Tier1-range agents in %.1fs"),
		       Tier1RegionGraphTestCount, Tier1TestSpawnDelaySeconds);
		GetWorldTimerManager().SetTimer(
			Tier1RegionGraphTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, Tier1RegionGraphTestCount]()
				{
					UWorld* TestWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = TestWorld ? TestWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelAgentSubsystem* AgentSubsystem = TestWorld ? TestWorld->GetSubsystem<UVoxelAgentSubsystem>() : nullptr;
					APlayerController* PC = TestWorld ? TestWorld->GetFirstPlayerController() : nullptr;
					APawn* Pawn = PC ? PC->GetPawn() : nullptr;
					if (!Terrain || !AgentSubsystem || !Pawn)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelTier1RegionGraphTest: subsystems/pawn not ready, skipping."));
						return;
					}

					const FVector PlayerLoc = Pawn->GetActorLocation();

					constexpr double WallOffsetXUU = 800.0;   // 8m ahead -- inside the ~19.2m Tier1 region graph box
					constexpr int32 WallColumns = 15;         // 15 x 4-voxel cubes = 60 voxels = 6m wide (Y) -- forces a real detour
					constexpr int32 WallLayers = 6;            // 6 x 4-voxel cubes = 24 voxels = 2.4m tall (Z)
					constexpr double CubeEdgeUU = 40.0;        // 4 voxels x VoxelCoords::VoxelSizeUU
					constexpr int32 CubeSizeVoxels = 4;
					constexpr uint8 WallMaterialId = 2; // vxc::MAT_ROCK

					const double WallCenterX = PlayerLoc.X + WallOffsetXUU;
					const double WallCenterY = PlayerLoc.Y;
					const double WallMinY = WallCenterY - (double(WallColumns) * CubeEdgeUU) * 0.5;

					int32 CubesPlaced = 0;
					for (int32 ColIdx = 0; ColIdx < WallColumns; ++ColIdx)
					{
						const double ColumnX = WallCenterX;
						const double ColumnY = WallMinY + double(ColIdx) * CubeEdgeUU + CubeEdgeUU * 0.5;
						double NextLayerBottomZ = Terrain->GetSurfaceHeightUU(ColumnX, ColumnY);
						for (int32 LayerIdx = 0; LayerIdx < WallLayers; ++LayerIdx)
						{
							const FVector CameraLoc(ColumnX, ColumnY, NextLayerBottomZ + CubeEdgeUU * 3.0);
							const FVector DownDir(0.0, 0.0, -1.0);
							const bool bPlaced = Terrain->TryPlace(CameraLoc, DownDir, CubeSizeVoxels, WallMaterialId,
							                                        PlayerLoc + FVector(1.0e7, 1.0e7, 1.0e7));
							if (bPlaced)
							{
								++CubesPlaced;
							}
							NextLayerBottomZ += CubeEdgeUU;
						}
					}
					const int32 CubesExpected = WallColumns * WallLayers;
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelTier1RegionGraphTest: wall built at X=%.0f, Y=[%.0f,%.0f] -- %d/%d cubes placed."),
					       WallCenterX, WallMinY, WallMinY + double(WallColumns) * CubeEdgeUU, CubesPlaced, CubesExpected);

					bTier1RegionGraphTestActive = true;
					Tier1RegionGraphTestColumnXUU = WallCenterX;
					Tier1RegionGraphTestColumnYUU = WallCenterY;

					// Tier-1-range spawn (17m -- Tier0EnterUU=15m < dist <
					// Tier1ExitUU=100m, so every agent starts life as Tier 1
					// and stays there -- UVoxelAgentSubsystem.h's hysteresis
					// constants), beyond the wall, and inside the ~19.2m
					// region graph coverage box.
					constexpr double SpawnOffsetUU = 1700.0; // 17m
					constexpr double LateralJitterUU = 50.0;
					const int32 Spawned = AgentSubsystem->SpawnSwarmAtOffset(
						Tier1RegionGraphTestCount, PlayerLoc, FVector(1.0, 0.0, 0.0), SpawnOffsetUU, LateralJitterUU);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelTier1RegionGraphTest: spawned %d/%d Tier1-range agents beyond the wall -- watch for ")
					       TEXT("'VoxelAgent N (Tier1 hierarchical): corridor cost=...' log lines (LogVoxelEarth, Verbose)."),
					       Spawned, Tier1RegionGraphTestCount);

					// Dig a gap through the wall's center some time later --
					// proves dirty invalidation (docs/status.md M6 section):
					// the agents should re-route through it on their NEXT
					// natural Tier 1 replan (at most UVoxelAgentSubsystem::
					// Tier1ReplanIntervalSeconds=2.5s later), not just
					// continue their old detour.
					constexpr float GapDelaySeconds = 15.f;
					GetWorldTimerManager().SetTimer(
						Tier1RegionGraphGapTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this, WallCenterX, WallCenterY, WallLayers, CubeEdgeUU]()
							{
								UWorld* GapWorld = GetWorld();
								UVoxelWorldSubsystem* GapTerrain = GapWorld ? GapWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
								if (!GapTerrain)
								{
									return;
								}
								const double SurfaceUU = GapTerrain->GetSurfaceHeightUU(WallCenterX, WallCenterY);
								// One horizontal dig per layer, straight through
								// the wall's ~40UU thickness at its center
								// column -- exactly like a player's own
								// left-click dig (UVoxelWorldSubsystem::TryDig),
								// the SAME authoritative path this whole gap
								// closure's "player digs" dirty-invalidation
								// case is about.
								int32 DigsApplied = 0;
								for (int32 LayerIdx = 0; LayerIdx < WallLayers; ++LayerIdx)
								{
									const FVector CameraLoc(WallCenterX - 200.0, WallCenterY,
									                          SurfaceUU + CubeEdgeUU * double(LayerIdx) + CubeEdgeUU * 0.5);
									const FVector DigDir(1.0, 0.0, 0.0);
									if (GapTerrain->TryDig(CameraLoc, DigDir, 4))
									{
										++DigsApplied;
									}
								}
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("VoxelTier1RegionGraphTest: opened a gap in the wall at (%.0f,%.0f) -- %d/%d dig(s) applied. ")
								       TEXT("Watch for the NEXT 'VoxelAgent N (Tier1 hierarchical): corridor cost=...' line to drop, and ")
								       TEXT("'VoxelSwarm Tier1 region graph: edit-log grew ... dirtied N region(s)'."),
								       WallCenterX, WallCenterY, DigsApplied, WallLayers);
							}),
						GapDelaySeconds, false);
				}),
			Tier1TestSpawnDelaySeconds, false);
	}
}

// --- underground streaming proof: camera poses -------------------------------
//
// Geometry constants below MUST stay in step with the carve in BeginPlay's
// -VoxelUndergroundTest block (shaft at the spawn column, tunnel running +X for
// 900UU, chamber at its end). Both poses sit ~120UU above the tunnel floor,
// i.e. roughly standing eye height inside a passage carved with a 190UU radius.

bool AVoxelEarthGameMode::IsUndergroundShaftView() const
{
	FString View;
	return FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundView="), View) && View.Equals(TEXT("shaft"), ESearchCase::IgnoreCase);
}

FVector AVoxelEarthGameMode::UndergroundTestCameraLocation() const
{
	const double FloorZUU = UndergroundTestSurfaceUU - UndergroundTestDepthUU;
	if (IsUndergroundShaftView())
	{
		// At the shaft foot, just inside the tunnel mouth: the frame is the dug
		// passage itself -- walls left/right, floor below, roof above.
		return FVector(UndergroundTestColumnXUU + 60.0, UndergroundTestColumnYUU, FloorZUU + 120.0);
	}
	// In the chamber, looking back toward the tunnel: rock on every side.
	return FVector(UndergroundTestColumnXUU + 900.0, UndergroundTestColumnYUU, FloorZUU + 120.0);
}

FRotator AVoxelEarthGameMode::UndergroundTestCameraRotation() const
{
	// Shaft view looks along +X down the tunnel; chamber view looks back along
	// -X toward the tunnel mouth. A few degrees of downward pitch in both so
	// the floor is in frame (a floor is precisely what used to be missing).
	return IsUndergroundShaftView() ? FRotator(-8.f, 0.f, 0.f) : FRotator(-8.f, 180.f, 0.f);
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

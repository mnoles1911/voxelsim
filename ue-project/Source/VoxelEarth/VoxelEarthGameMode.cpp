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
		//
		// -VoxelNoClipmap suppresses it. This is the CONTROL for "is that
		// distant terrain actually voxels?": the clipmap is a smooth triangle
		// heightfield covering the same ground the outer voxel rings do, and in
		// a screenshot the two are easy to confuse. With the clipmap absent,
		// anything still drawn past the ring cascade's edge is voxel geometry by
		// construction, and anything that becomes empty sky was clipmap. A
		// command-line switch rather than a cvar, matching -VoxelPendingJobCap
		// and friends: -ExecCmds cvars land after streaming has begun, and this
		// one has to be decided before the actor is ever spawned.
		if (!FParse::Param(FCommandLine::Get(), TEXT("VoxelNoClipmap")))
		{
			World->SpawnActor<AVoxelClipmapActor>();
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning, TEXT("Voxel clipmap: SUPPRESSED by -VoxelNoClipmap (far terrain is voxels only)"));
		}

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

	// ==== downward escape hatch proof: dig past the skirt floor =============
	//
	// -VoxelDigDownTest[=<delaySeconds>]   (default 30s)
	// -VoxelDigDownDepth=<metres>          (default 60m -- the skirt floor is
	//                                       ~41.6m, so this clears it by ~18m)
	// -VoxelDigDownSettle=<seconds>        (default 20s)
	//
	// This is the test that protects the terrain, so it is deliberately the
	// crudest possible one: carve a vertical shaft with the same authoritative
	// CarveSphere path a player's explosive uses, then read the column back out
	// of the streaming system and print it. No screenshot, no framing, nothing
	// that can be argued with -- either the chunks under the shaft are tracked
	// with geometry or they are not.
	//
	// The pawn is NOT moved. It stands on the surface for the whole test, so
	// bAnchorUnderground stays false and the anchor-relative deep box never
	// opens. The only thing that can put the bottom of the shaft into the
	// desired set is the EditedFootprintMinZ widening of ChunkZMin.
	float DigDownDelaySeconds = 30.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDigDownTest="), DigDownDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelDigDownTest")))
	{
		float DigDownDepthM = 60.f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelDigDownDepth="), DigDownDepthM);
		float DigDownSettleSeconds = 20.f;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelDigDownSettle="), DigDownSettleSeconds);
		const bool bDigDownQuit = FParse::Param(FCommandLine::Get(), TEXT("VoxelDigDownQuit"));

		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelDigDownTest: shaft to %.0fm below the surface in %.1fs, report %.1fs later."),
		       DigDownDepthM, DigDownDelaySeconds, DigDownSettleSeconds);

		GetWorldTimerManager().SetTimer(
			DigDownTestDigTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, DigDownDepthM, DigDownSettleSeconds, bDigDownQuit]()
				{
					UWorld* DigWorld = GetWorld();
					APlayerController* PC = DigWorld ? DigWorld->GetFirstPlayerController() : nullptr;
					APawn* Pawn = PC ? PC->GetPawn() : nullptr;
					UVoxelWorldSubsystem* Subsystem = DigWorld ? DigWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Pawn || !Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelDigDownTest: no pawn/subsystem yet, skipping."));
						return;
					}
					const FVector PawnLoc = Pawn->GetActorLocation();
					// Offset a few metres so the shaft is not directly under the
					// pawn's collision capsule (which would drop it in and make
					// the anchor underground -- the one thing this test must not
					// let happen), but well inside R0's near band so the skirt
					// really is 12 chunks deep there.
					const double ColX = PawnLoc.X + 600.0;
					const double ColY = PawnLoc.Y;
					const double SurfaceUU = Subsystem->GetSurfaceHeightUU(ColX, ColY);

					LogDigDownColumn(*Subsystem, ColX, ColY, TEXT("pre-dig"));

					// One sphere per metre of depth, radius 1.5m: overlapping,
					// so the shaft is continuous rather than a string of beads.
					const double DepthUU = double(DigDownDepthM) * 100.0;
					int32 Removed = 0;
					int32 Spheres = 0;
					for (double D = 100.0; D <= DepthUU; D += 100.0)
					{
						Removed += Subsystem->CarveSphere(FVector(ColX, ColY, SurfaceUU - D), 150.0, 0.0);
						++Spheres;
					}
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelDigDownTest: carved a shaft at (%.0f,%.0f) surface=%.0f, %d spheres, %d voxels removed, ")
					       TEXT("floor %.0fm below the surface (skirt floor is ~41.6m)."),
					       ColX, ColY, SurfaceUU, Spheres, Removed, double(DigDownDepthM));

					GetWorldTimerManager().SetTimer(
						DigDownTestReportTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this, ColX, ColY, bDigDownQuit]()
							{
								UWorld* RWorld = GetWorld();
								UVoxelWorldSubsystem* Sub = RWorld ? RWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
								if (Sub)
								{
									LogDigDownColumn(*Sub, ColX, ColY, TEXT("post-dig"));
								}
								if (bDigDownQuit)
								{
									GetWorldTimerManager().SetTimer(
										DigDownTestQuitTimerHandle,
										FTimerDelegate::CreateWeakLambda(this,
											[this]()
											{
												if (UWorld* QWorld = GetWorld())
												{
													if (APlayerController* QPC = QWorld->GetFirstPlayerController())
													{
														QPC->ConsoleCommand(TEXT("quit"), true);
													}
												}
											}),
										2.f, false);
								}
							}),
						DigDownSettleSeconds, false);
				}),
			DigDownDelaySeconds, false);
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

	// -VoxelGIDebug=<n> / -VoxelGIVis=<n>. Both of these are read when a scene
	// proxy is BUILT, and a proxy is only rebuilt when something marks it
	// dirty -- so setting them through -ExecCmds leaves every already-resident
	// chunk on the old value and silently produces a half-instrumented frame.
	// Command line, read at init, same reasoning as -VoxelGIOn and
	// -VoxelNoUnderground.
	int32 GIDebugLevel = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGIDebug="), GIDebugLevel))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Debug")))
		{
			V->Set(GIDebugLevel, ECVF_SetByCode);
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGIDebug: voxel.GI.Debug=%d"), GIDebugLevel);
		}
	}
	int32 GIVisMode = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGIVis="), GIVisMode))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.DebugVis")))
		{
			V->Set(GIVisMode, ECVF_SetByCode);
			UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGIVis: voxel.GI.DebugVis=%d"), GIVisMode);
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
	// -VoxelGICaveTest=<s> ---------------------------------------------------
	//
	// THE case a voxel light field exists for: a dark cave with daylight
	// falling in through a sinkhole. Underground streaming (PR #55) is what
	// made this testable -- the previous GI pass had to use an above-ground
	// fixture because nothing below the surface was meshed.
	//
	// Finds a real WORLDGEN sinkhole (nothing is carved, so this is pristine
	// terrain, not an edit), parks the camera in the cave a few metres off the
	// shaft axis, and looks back up at the lit shaft mouth so the same frame
	// contains daylight, the lit floor under the shaft, and cave wall the light
	// does not reach. Pair with -VoxelGIOn for the A/B.
	float GICaveDelaySeconds = 18.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGICaveTest="), GICaveDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelGICaveTest")))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGICaveTest: searching for a sinkhole near spawn in %.1fs"),
		       GICaveDelaySeconds);
		GetWorldTimerManager().SetTimer(
			GICaveTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* CWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = CWorld ? CWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Terrain)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelGICaveTest: subsystem not ready."));
						return;
					}
					double SpawnX = 0.0, SpawnY = 0.0;
					ParseSpawnColumnUU(SpawnX, SpawnY);

					FVector ShaftBaseUU;
					double ShaftSurfaceUU = 0.0;
					if (!FindSinkholeColumn(*Terrain, SpawnX, SpawnY, ShaftBaseUU, ShaftSurfaceUU))
					{
						UE_LOG(LogVoxelEarth, Warning,
						       TEXT("VoxelGICaveTest: no sinkhole found within the search radius."));
						return;
					}

					// Stand back from the shaft axis so the lit floor patch and
					// the shaft above it are both in frame. Placement is done
					// with direct solidity probes, NOT RaycastVoxelWorld: at
					// this point the sinkhole is ~300 m away and none of its
					// chunks are resident, so a raycast against the resident set
					// reports "no hit" for solid rock and cheerfully parks the
					// camera inside a wall (it did exactly that on the first
					// attempt).
					const FVector Cardinals[4] = {FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0)};
					auto IsAirAt = [Terrain](const FVector& P)
					{
						return !Terrain->IsSolidAtVoxel((int64)FMath::FloorToDouble(P.X / VoxelCoords::VoxelSizeUU),
						                                (int64)FMath::FloorToDouble(P.Y / VoxelCoords::VoxelSizeUU),
						                                (int64)FMath::FloorToDouble(P.Z / VoxelCoords::VoxelSizeUU));
					};
					// A spot is standable if there is air from knee to head
					// height -- one air voxel is not a place to put a camera.
					auto IsStandable = [&IsAirAt](const FVector& Base)
					{
						return IsAirAt(Base + FVector(0, 0, 50.0)) && IsAirAt(Base + FVector(0, 0, 150.0)) &&
						       IsAirAt(Base + FVector(0, 0, 250.0));
					};

					FVector BestDir = Cardinals[0];
					double BestClear = 0.0;
					for (const FVector& D : Cardinals)
					{
						double Clear = 0.0;
						for (double Dist = 50.0; Dist <= 900.0; Dist += 50.0)
						{
							if (!IsStandable(ShaftBaseUU + D * Dist))
							{
								break; // contiguous run only
							}
							Clear = Dist;
						}
						if (Clear > BestClear)
						{
							BestClear = Clear;
							BestDir = D;
						}
					}
					// Keep a voxel of margin off the wall, and cap the standoff
					// so the shaft stays inside a 59-degree vertical FOV.
					// 2 m of standoff, not the full clearance: at 6 m the camera
					// ends up hard against the far wall with the shaft mouth off
					// the top of frame (measured -- first two attempts).
					float StandoffCapUU = 200.f;
					FParse::Value(FCommandLine::Get(), TEXT("VoxelGICaveStandoff="), StandoffCapUU);
					const double StandOffUU = FMath::Clamp(BestClear - 50.0, 0.0, double(StandoffCapUU));
					GICaveCameraUU = ShaftBaseUU + FVector(0, 0, 150.0) + BestDir * StandOffUU;
					const FVector ToShaft = (ShaftBaseUU - GICaveCameraUU).GetSafeNormal2D();
					// Look UP the shaft. This is the legible framing: the lit
					// mouth sits in the middle of the frame ringed by cave the
					// daylight does not reach, so GI on/off differ over most of
					// the image rather than in one corner.
					float CavePitchDeg = 55.f;
					FParse::Value(FCommandLine::Get(), TEXT("VoxelGICavePitch="), CavePitchDeg);
					GICaveCameraRot = FRotator(
						CavePitchDeg,
						StandOffUU > 1.0 ? float(FMath::RadiansToDegrees(FMath::Atan2(ToShaft.Y, ToShaft.X))) : 0.f,
						0.f);
					bGICaveFound = true;

					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelGICaveTest: sinkhole shaft at (%.0f,%.0f), surface %.0f, cave floor %.0f ")
					       TEXT("(%.1fm below daylight); camera (%.0f,%.0f,%.0f) standoff %.0fUU pitch %.1f yaw %.1f"),
					       ShaftBaseUU.X, ShaftBaseUU.Y, ShaftSurfaceUU, ShaftBaseUU.Z,
					       (ShaftSurfaceUU - ShaftBaseUU.Z) / 100.0, GICaveCameraUU.X, GICaveCameraUU.Y,
					       GICaveCameraUU.Z, StandOffUU, GICaveCameraRot.Pitch, GICaveCameraRot.Yaw);

					auto PoseInCave = [this]()
					{
						UWorld* PWorld = GetWorld();
						APlayerController* Ctrl = PWorld ? PWorld->GetFirstPlayerController() : nullptr;
						if (!Ctrl || !bGICaveFound)
						{
							return;
						}
						if (APawn* P = Ctrl->GetPawn())
						{
							P->SetActorLocation(GICaveCameraUU, false, nullptr, ETeleportType::TeleportPhysics);
							P->SetActorRotation(GICaveCameraRot);
							Ctrl->SetViewTarget(P);
						}
						Ctrl->SetControlRotation(GICaveCameraRot);
					};
					// Pose first so streaming and the GI solve both key off the
					// final view origin, then capture well after (the camera has
					// teleported hundreds of metres and the deep footprint has to
					// be recomputed and meshed from scratch).
					// Two-phase, matching -VoxelGITest: assert the pose on its own
					// timer well before the capture. Posing and capturing inside
					// one lambda captures the PRE-move view (documented in the
					// GITest block; reproduced here).
					//
					// SETTLE TIME IS LOAD-BEARING, and 24 s was not enough. The
					// camera teleports ~300 m and 13 m down, so the whole deep
					// footprint is meshed from scratch; the GI dirty queue sits
					// around 1700 bricks and the re-shade queue several hundred,
					// draining at MaxBrickSolvesPerFrame / MaxChunkRefreshesPerFrame.
					// Capture before that drains and chunks are still on their
					// plain-AO fallback, so the cave photographs BRIGHT -- and
					// whether it does is a race, i.e. the same build and seed
					// gave a dark cave one run and a bright one the next. Two
					// runs were spent bisecting a "regression" that was this
					// race. Default settle is now 50 s; -VoxelGICaveSettle=<s>
					// overrides.
					float CaveSettleSeconds = 50.f;
					FParse::Value(FCommandLine::Get(), TEXT("VoxelGICaveSettle="), CaveSettleSeconds);
					GetWorldTimerManager().SetTimer(
						GICavePoseTimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseInCave), 0.5f, false);
					GetWorldTimerManager().SetTimer(
						GICaveRepose1TimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseInCave),
						FMath::Max(1.f, CaveSettleSeconds - 4.f), false);
					GetWorldTimerManager().SetTimer(
						GICaveRepose2TimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseInCave),
						FMath::Max(1.f, CaveSettleSeconds - 2.f), false);
					GetWorldTimerManager().SetTimer(
						GICaveShotTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this, PoseInCave]()
							{
								// Report the ACTUAL camera, not the intended one.
								// Two runs differing only in standoff produced
								// near-identical frames, which is the signature
								// of the pose not sticking; this is the line that
								// tells the two apart.
								if (APlayerController* CC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
								{
									if (CC->PlayerCameraManager)
									{
										const FVector L = CC->PlayerCameraManager->GetCameraLocation();
										const FRotator R = CC->PlayerCameraManager->GetCameraRotation();
										UE_LOG(LogVoxelEarth, Log,
										       TEXT("VoxelGICaveTest capture: ACTUAL cam=(%.0f,%.0f,%.0f) rot=(pitch %.1f yaw %.1f) ")
										       TEXT("| intended=(%.0f,%.0f,%.0f) pitch %.1f"),
										       L.X, L.Y, L.Z, R.Pitch, R.Yaw, GICaveCameraUU.X, GICaveCameraUU.Y,
										       GICaveCameraUU.Z, GICaveCameraRot.Pitch);
									}
								}
								// Probe at CAPTURE time, when the chunks around
								// the camera are actually resident -- a probe at
								// search time reports nothing but MISS and is
								// what let two runs park the camera in a wall.
								UWorld* SWorld = GetWorld();
								if (UVoxelWorldSubsystem* T = SWorld ? SWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr)
								{
									const FVector Dirs[6] = {FVector(1, 0, 0),  FVector(-1, 0, 0), FVector(0, 1, 0),
									                         FVector(0, -1, 0), FVector(0, 0, 1),  FVector(0, 0, -1)};
									const TCHAR* Names[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
									FString Report;
									for (int32 I = 0; I < 6; ++I)
									{
										FVector Hit, Prev;
										Report += FString::Printf(
											TEXT("%s=%s "), Names[I],
											T->RaycastVoxelWorld(GICaveCameraUU, Dirs[I], 6000.0, Hit, Prev)
												? *FString::Printf(TEXT("%.1fm"), (Hit - GICaveCameraUU).Size() / 100.0)
												: TEXT("open"));
									}
									UE_LOG(LogVoxelEarth, Log, TEXT("VoxelGICaveTest capture probe: %s"), *Report);
								}
								FScreenshotRequest::RequestScreenshot(TEXT("VoxelGICave"), false, true);
							}),
						CaveSettleSeconds, false);
					GetWorldTimerManager().SetTimer(
						GICaveQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(false); }),
						CaveSettleSeconds + 4.f, false);
				}),
			GICaveDelaySeconds, false);
	}

	// ==== C7/C8 underground water verification ==============================
	//
	// -VoxelFloodTest[=<delaySeconds>] is the deliverable for the cavern-water
	// pass: ONE run producing TWO shots from the SAME camera -- a static
	// implicit cavern lake, then that lake drained. Same-camera matters; two
	// differently framed shots prove nothing about the water level.
	//
	// The drain has to be an outflow tunnel OUT of the site's flood reach, not
	// a hole in the lake floor. The flood field is defined on CURRENT air below
	// floodZ, so digging downward inside a flooded column only creates more
	// implicit water -- correct groundwater semantics, and exactly what makes a
	// cavern lake feel like an aquifer rather than a bathtub -- so the water can
	// only actually leave by breaking out past the reach disc. See
	// UVoxelWaterSubsystem::CarveCavernOutflow.
	float FloodTestDelaySeconds = 20.f;
	const bool bFloodTestRequested =
		FParse::Value(FCommandLine::Get(), TEXT("VoxelFloodTest="), FloodTestDelaySeconds) ||
		FParse::Param(FCommandLine::Get(), TEXT("VoxelFloodTest"));
	if (bFloodTestRequested)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelFloodTest: searching for a flooded cavern in %.1fs"), FloodTestDelaySeconds);

		// Poses the camera on the lake shore. Split out and fired on its own
		// timers well before each capture: posing and capturing in one lambda
		// captures the PRE-move view (documented in the GITest/GICaveTest
		// blocks above, reproduced here).
		auto PoseOnShore = [this]()
		{
			UWorld* PWorld = GetWorld();
			APlayerController* Ctrl = PWorld ? PWorld->GetFirstPlayerController() : nullptr;
			if (!Ctrl || !bFloodTestFound)
			{
				return;
			}
			if (APawn* P = Ctrl->GetPawn())
			{
				P->SetActorLocation(FloodTestCameraUU, false, nullptr, ETeleportType::TeleportPhysics);
				P->SetActorRotation(FloodTestCameraRot);
				Ctrl->SetViewTarget(P);
			}
			Ctrl->SetControlRotation(FloodTestCameraRot);
		};

		// Reports where the camera ACTUALLY ended up and what surrounds it.
		auto LogCameraProbe = [this](const TCHAR* Stage)
		{
			UWorld* PWorld = GetWorld();
			APlayerController* Ctrl = PWorld ? PWorld->GetFirstPlayerController() : nullptr;
			UVoxelWorldSubsystem* T = PWorld ? PWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
			if (!Ctrl || !Ctrl->PlayerCameraManager || !T)
			{
				return;
			}
			const FVector L = Ctrl->PlayerCameraManager->GetCameraLocation();
			const FRotator R = Ctrl->PlayerCameraManager->GetCameraRotation();
			const FVector Dirs[6] = {FVector(1, 0, 0),  FVector(-1, 0, 0), FVector(0, 1, 0),
			                         FVector(0, -1, 0), FVector(0, 0, 1),  FVector(0, 0, -1)};
			const TCHAR* Names[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
			FString Report;
			for (int32 I = 0; I < 6; ++I)
			{
				FVector Hit, Prev;
				Report += FString::Printf(TEXT("%s=%s "), Names[I],
				                          T->RaycastVoxelWorld(L, Dirs[I], 8000.0, Hit, Prev)
				                              ? *FString::Printf(TEXT("%.1fm"), (Hit - L).Size() / 100.0)
				                              : TEXT("open"));
			}
			// Is there actually water under our feet? A vertical column of the
			// implicit field straight down from the camera answers directly,
			// rather than leaving it to be guessed from the screenshot.
			FString WaterCol;
			if (UVoxelWaterSubsystem* W = PWorld->GetSubsystem<UVoxelWaterSubsystem>())
			{
				for (int32 D = -4; D <= 8; ++D)
				{
					WaterCol += FString::Printf(TEXT("%d "), int32(W->GetImplicitFillAtWorld(L - FVector(0, 0, D * 100.0))));
				}
			}
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelFloodTest [%s] camera: ACTUAL (%.0f,%.0f,%.0f) rot(pitch %.1f yaw %.1f) | intended (%.0f,%.0f,%.0f) | walls: %s | implicitFill from cam-4m down to cam+8m: %s"),
			       Stage, L.X, L.Y, L.Z, R.Pitch, R.Yaw, FloodTestCameraUU.X, FloodTestCameraUU.Y, FloodTestCameraUU.Z,
			       *Report, *WaterCol);
		};

		auto LogLedger = [this](const TCHAR* Stage)
		{
			UWorld* LWorld = GetWorld();
			UVoxelWaterSubsystem* Water = LWorld ? LWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
			if (!Water)
			{
				return;
			}
			int32 MobBricks = 0;
			uint64 Deb = 0, Cred = 0, Short = 0;
			Water->GetMobilizationStats(MobBricks, Deb, Cred, Short);
			const FVoxelWaterPerfSnapshot Snap = Water->GetPerfSnapshot();
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelFloodTest [%s]: mobilizedBricks=%d debited=%llu credited=%llu SHORTFALL=%llu | CA stored=%lld active=%lld volume=%llu"),
			       Stage, MobBricks, (unsigned long long)Deb, (unsigned long long)Cred, (unsigned long long)Short,
			       Snap.StoredBricks, Snap.ActiveBricks, (unsigned long long)Snap.TotalVolume);
			if (Short != 0)
			{
				// The one failure this whole feature is built to make
				// impossible: units the implicit field gave up that the CA did
				// not accept, i.e. water destroyed by the handover.
				UE_LOG(LogVoxelEarth, Error,
				       TEXT("VoxelFloodTest: MOBILIZATION SHORTFALL %llu -- water was destroyed by the implicit->CA handover."),
				       (unsigned long long)Short);
			}
		};

		GetWorldTimerManager().SetTimer(
			FloodTestFindTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, PoseOnShore, LogLedger]()
				{
					UWorld* FWorld = GetWorld();
					UVoxelWaterSubsystem* Water = FWorld ? FWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					if (!Water)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelFloodTest: water subsystem not ready, skipping."));
						return;
					}

					double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
					ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);
					// 2 km: cavern sites sit 204.8 m apart and only ~60% of them
					// are wet, so a generous radius keeps the test seed-agnostic.
					if (!Water->FindFloodedCavernNear(FVector(SpawnColXUU, SpawnColYUU, 0.0), 200000.0,
					                                  FloodTestLakeSurfaceUU))
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelFloodTest: no flooded cavern found; nothing to capture."));
						return;
					}
					bFloodTestFound = true;

					// Stand just above the waterline at the column the search
					// picked, which is the most open point of the chamber. Do
					// NOT back the camera off horizontally: a room tapers fast,
					// and a camera that ends up inside rock renders the whole
					// world see-through (every surface around it is a backface).
					// The tunnel is carved toward +X, so looking +X keeps its
					// mouth in frame as the lake drops.
					// Pitch STEEPLY down and stand a few metres up, so the lake
					// surface fills the frame instead of the horizon. Underground
					// the far field is a heightfield whose underside is backface-
					// culled, so anything aimed near the horizon renders as a
					// see-through world; aiming at the water avoids that entirely.
					// ABOVE the lake looking down, not wading in it. Standing at
					// the waterline puts the camera inside a 25 m translucent
					// slab and the frame turns into featureless blue; from up
					// near the roof the surface reads as a surface, with an edge,
					// and a dropping level is unmistakable between the two shots.
					// HEIGHT IS LOAD-BEARING, and not for the framing. Underground
					// chunk residency is tight and keyed to the camera: the
					// outflow carve has to break rock ~30 m away, and it removed
					// 341744 voxels with the camera 1.2 m over the water and
					// NOTHING (0/102 spheres, five retries) with it 9 m up. So
					// sit just above the waterline -- where the rock we need to
					// cut is actually streamed in -- and get the framing from
					// pitch instead.
					FloodTestCameraUU = FloodTestLakeSurfaceUU + FVector(0.0, 0.0, 150.0);
					FloodTestCameraRot = FRotator(-25.0, 0.0, 0.0); // look +X, down onto the lake
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelFloodTest: lake surface (%.0f,%.0f,%.0f) UU; camera (%.0f,%.0f,%.0f)"),
					       FloodTestLakeSurfaceUU.X, FloodTestLakeSurfaceUU.Y, FloodTestLakeSurfaceUU.Z,
					       FloodTestCameraUU.X, FloodTestCameraUU.Y, FloodTestCameraUU.Z);
					LogLedger(TEXT("found"));
					PoseOnShore();
				}),
			FloodTestDelaySeconds, false);

		// Re-assert the pose as streaming catches up: the camera teleports a
		// long way underground and the deep footprint meshes from scratch.
		GetWorldTimerManager().SetTimer(FloodTestPose1TimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseOnShore),
		                                FloodTestDelaySeconds + 8.f, false);

		// SHOT 1 -- the untouched implicit lake. Nothing has mobilized yet, so
		// this is worldgen water costing zero storage and zero ticks.
		GetWorldTimerManager().SetTimer(
			FloodTestShot1TimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, LogLedger, LogCameraProbe]()
				{
					if (!bFloodTestFound)
					{
						return;
					}
					LogLedger(TEXT("lake/static"));
					// Probe at CAPTURE time, when the chunks around the camera
					// are actually resident. A camera stuck in rock produces a
					// see-through world rather than an obviously black frame,
					// so this is the line that tells a good shot from a bad one.
					LogCameraProbe(TEXT("lake/static"));
					FScreenshotRequest::RequestScreenshot(TEXT("VoxelFloodLake"), false, true);
				}),
			FloodTestDelaySeconds + 24.f, false);

		// THE DIG, with RETRIES. CarveSphere only removes voxels from chunks that
		// are currently resident, and the outflow has to break out past the
		// site's ~36 m flood reach -- right at the edge of what is streamed in
		// underground. Observed removing 341744 voxels on one run and 0 on the
		// next from byte-identical inputs, so a single attempt is not enough.
		// Re-arms itself (a plain function-object member, since a lambda cannot
		// reschedule itself) until something actually gives way.
		FloodTestCarveRetryDelegate = FTimerDelegate::CreateWeakLambda(this,
			[this, LogLedger]()
			{
				UWorld* CWorld = GetWorld();
				UVoxelWaterSubsystem* Water = CWorld ? CWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
				if (!Water || !bFloodTestFound)
				{
					return;
				}
				if (Water->CarveCavernOutflow(FloodTestLakeSurfaceUU) == 0 && FloodTestCarveAttempts < 5)
				{
					++FloodTestCarveAttempts;
					UE_LOG(LogVoxelEarth, Warning,
					       TEXT("VoxelFloodTest: outflow carve removed nothing (attempt %d/5) -- chunks likely not resident; retrying in 3s."),
					       FloodTestCarveAttempts);
					GetWorldTimerManager().SetTimer(FloodTestCarveTimerHandle, FloodTestCarveRetryDelegate, 3.f, false);
					return;
				}
				LogLedger(TEXT("post-dig"));
			});
		GetWorldTimerManager().SetTimer(FloodTestCarveTimerHandle, FloodTestCarveRetryDelegate,
		                                FloodTestDelaySeconds + 28.f, false);

		GetWorldTimerManager().SetTimer(FloodTestPose2TimerHandle, FTimerDelegate::CreateWeakLambda(this, PoseOnShore),
		                                FloodTestDelaySeconds + 34.f, false);

		// SHOT 2 -- same camera, after the advancing front has converted the
		// lake brick by brick and the CA has drained it out through the tunnel.
		GetWorldTimerManager().SetTimer(
			FloodTestShot2TimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, LogLedger, LogCameraProbe]()
				{
					if (!bFloodTestFound)
					{
						return;
					}
					LogLedger(TEXT("drained"));
					LogCameraProbe(TEXT("drained"));
					FScreenshotRequest::RequestScreenshot(TEXT("VoxelFloodDrain"), false, true);
				}),
			FloodTestDelaySeconds + 52.f, false);

		GetWorldTimerManager().SetTimer(FloodTestQuitTimerHandle,
		                                FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(false); }),
		                                FloodTestDelaySeconds + 58.f, false);
	}

	// ==== end C7/C8 underground water verification ==========================

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

					// ADR-0005: persist the water blob ALONGSIDE the edit log on
					// the same explicit-save path (the Deinitialize autosave
					// already pairs them on shutdown; this pairs them here too).
					if (UVoxelWaterSubsystem* Water = SaveWorldPtr->GetSubsystem<UVoxelWaterSubsystem>())
					{
						const bool bWaterOk = Water->SaveWaterState();
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSaveWorldAfter: SaveWaterState() -> %s (waterDigest=0x%016llX)"),
						       bWaterOk ? TEXT("OK") : TEXT("skipped/failed"), (unsigned long long)Water->GetWaterDigest());
					}

					GetWorldTimerManager().SetTimer(
						SaveWorldQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f, false);
				}),
			SaveWorldAfterSeconds, false);
	}

	// LOD-cascade vista framing for -VoxelScreenshotAfter. Read once at first
	// use, like every other switch in this file, so it cannot be raced by
	// -ExecCmds landing after streaming has begun.
	// -VoxelVistaShot            -> defaults (400 m camera, -10 deg pitch)
	// -VoxelVistaShot=<metres>   -> camera height
	// -VoxelVistaPitch=<degrees> -> pitch (negative looks down)
	struct VoxelVistaShot
	{
		static bool IsActive()
		{
			static const bool bActive = FParse::Param(FCommandLine::Get(), TEXT("VoxelVistaShot")) ||
			                            FCString::Strifind(FCommandLine::Get(), TEXT("VoxelVistaShot=")) != nullptr;
			return bActive;
		}
		static float HeightMeters()
		{
			static const float Height = []
			{
				float Value = 400.f;
				FParse::Value(FCommandLine::Get(), TEXT("VoxelVistaShot="), Value);
				return Value;
			}();
			return Height;
		}
		static float PitchDegrees()
		{
			static const float Pitch = []
			{
				float Value = -10.f;
				FParse::Value(FCommandLine::Get(), TEXT("VoxelVistaPitch="), Value);
				return Value;
			}();
			return Pitch;
		}
		// -VoxelVistaYaw=<degrees>. Yaw was previously pinned to the same 45 deg
		// the default framing uses, which makes the vista shot unusable as
		// evidence whenever the interesting terrain is not on that bearing --
		// the spawn used for LOD verification looks out over open ocean at 45,
		// so a cascade shot there photographs water no matter how far the
		// voxels actually reach. Overridable so the camera can be aimed at land.
		static float YawDegrees()
		{
			static const float Yaw = []
			{
				float Value = 45.f;
				FParse::Value(FCommandLine::Get(), TEXT("VoxelVistaYaw="), Value);
				return Value;
			}();
			return Yaw;
		}
	};

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
					else if (bCaveTestActive && bCaveTestFound && PC)
					{
						PoseInCaveTest();
						// The measurement the fixture exists for, on this path
						// too: the at-park dump is taken before any streaming
						// tick and is always empty, so without this the run
						// produces no usable streaming evidence at all.
						if (UVoxelWorldSubsystem* CaveSub = GetWorld()->GetSubsystem<UVoxelWorldSubsystem>())
						{
							LogUndergroundChunkStatus(*CaveSub, CaveTestCameraPos, TEXT("at-capture"));
						}
					}
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
						// -VoxelWaterUnderside: same subject and same settle,
						// but the camera sits INSIDE the water body looking
						// straight up, so the frame is the top water face seen
						// from beneath. That is the view that a wrong triangle
						// winding destroys -- with the faces inverted the
						// underside is culled and you see straight out through
						// the surface -- so the above/below pair is the
						// verification for the water winding fix.
						const bool bUnderside = FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterUnderside"));
						const FVector Pose = bUnderside
							? OverheadColumnWorld
							: OverheadColumnWorld + FVector(0.0, 0.0, HoverHeightAboveUU);
						const FRotator Look(bUnderside ? 85.f : -85.f, 0.f, 0.f);
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorLocation(Pose);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
					}
					else if (VoxelVistaShot::IsActive() && PC)
					{
						// -VoxelVistaShot[=<heightMeters>] (docs/status.md "R2-R4
						// ring starvation fix"): the LOD-cascade acceptance shot.
						// The default -VoxelScreenshotAfter framing is a -40 deg
						// pitch from the spawn height, which fills the frame with
						// R0/R1 terrain a few tens of metres away and shows
						// literally nothing about whether the coarse rings loaded
						// -- it is why "the cascade renders" had never actually
						// been looked at. This one lifts the camera and pitches
						// shallow so the outer annuli (R2 128-256 m, R3 256-512 m,
						// R4 512-1024 m) are what fills the frame.
						// RELATIVE to the spawn height, not absolute: terrain at the
						// default spawn sits around 1,200 m of world Z, so an
						// absolute height buries the camera in rock (measured -- the
						// first attempt produced a frame of nothing but fog, seen
						// from inside the ground through back-face culling).
						const float HeightUU = VoxelVistaShot::HeightMeters() * 100.f;
						const FRotator Look(VoxelVistaShot::PitchDegrees(), VoxelVistaShot::YawDegrees(), 0.f);
						if (APawn* P = PC->GetPawn())
						{
							FVector Loc = P->GetActorLocation();
							Loc.Z += HeightUU;
							P->SetActorLocation(Loc);
							P->SetActorRotation(Look);
						}
						PC->SetControlRotation(Look);
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

	// --- Water pool parity anchor -------------------------------------------
	//
	// -VoxelWaterParityTest[=<delaySeconds>] (default 25s). Poses on the
	// surface above the spawn column, WAITS FOR TERRAIN STREAMING TO GO QUIET
	// BEFORE ANY WATER EXISTS, pours a fixed volume, waits for the CA to settle
	// to zero active bricks, re-asserts the identical pose, and captures.
	//
	// Run it twice with the same `voxel.Water.GPU` value to get the noise
	// floor, then twice more with the other value. If the two same-config
	// readings straddle the cross-config difference, there is no difference to
	// report -- which is exactly what happened at the -VoxelFloodTest anchor
	// and is why this fixture exists (see the header comment).
	//
	// The frame-cost half of the measurement is NOT automatable here and must
	// not be faked: -VoxelPerfRun samples the world delta, which the engine
	// clamps at MaxUndilatedFrameTime (400 ms), and these anchors run below
	// that on the full cascade, so every automated sample reads exactly 400.00
	// and two configurations come back identical. A human reading `stat unit`
	// is the instrument for that number -- see manual-verification-checklist.md
	// item 4a. This fixture's job is to make the SCENE reproducible so that
	// reading means something; Draw is the number to watch, not Frame.
	float WaterParityDelaySeconds = 25.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterParityTest="), WaterParityDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterParityTest")))
	{
		// Poll cadence and caps. The caps exist so a run on a slow box still
		// terminates and still captures SOMETHING -- but a capped-out run logs
		// loudly that it did, because a capture taken while streaming is still
		// churning is precisely the useless measurement this fixture replaces.
		constexpr float PollIntervalSeconds = 2.f;
		constexpr int32 MaxTerrainPolls = 30; // 60s
		constexpr int32 MaxWaterPolls = 20;   // 40s

		// Same 30,000 units as -VoxelSpawnWaterTest, and for the same reason
		// recorded there: a smaller pour spreads thin enough over an open
		// surface that it settles below the meshing floor and is invisible.
		constexpr uint32 ParityPourAmount = 30000;

		auto PoseOnAnchor = [this]()
		{
			UWorld* PWorld = GetWorld();
			APlayerController* Ctrl = PWorld ? PWorld->GetFirstPlayerController() : nullptr;
			if (!Ctrl)
			{
				return;
			}
			if (APawn* P = Ctrl->GetPawn())
			{
				P->SetActorLocation(WaterParityCameraUU, false, nullptr, ETeleportType::TeleportPhysics);
				P->SetActorRotation(WaterParityCameraRot);
				Ctrl->SetViewTarget(P);
			}
			Ctrl->SetControlRotation(WaterParityCameraRot);
		};

		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelWaterParityTest: posing on the surface anchor in %.1fs, then settling terrain BEFORE any water exists."),
		       WaterParityDelaySeconds);

		GetWorldTimerManager().SetTimer(
			WaterParityPoseTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this, PoseOnAnchor, PollIntervalSeconds, MaxTerrainPolls, MaxWaterPolls, ParityPourAmount]()
				{
					UWorld* PWorld = GetWorld();
					UVoxelWorldSubsystem* Terrain = PWorld ? PWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Terrain)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelWaterParityTest: terrain subsystem not ready, skipping."));
						return;
					}

					// Stand back from and above the pour column, pitched down
					// far enough that the pool fills the lower frame and the
					// horizon stays out of it. Aiming at the horizon would put
					// the distant cascade -- the most residency-sensitive thing
					// in the scene -- into the very frame we are trying to hold
					// constant.
					const double SurfaceUU = Terrain->GetSurfaceHeightUU(0.0, 0.0);
					WaterParityCameraUU = FVector(-900.0, 0.0, SurfaceUU + 700.0);
					WaterParityCameraRot = FRotator(-38.0, 0.0, 0.0); // look +X, down at the pour column
					PoseOnAnchor();

					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelWaterParityTest: anchor posed at (%.0f,%.0f,%.0f) rot(pitch %.1f yaw %.1f); surface at %.0f. ")
					       TEXT("Waiting for terrain residency to settle (poll %.0fs, cap %d)."),
					       WaterParityCameraUU.X, WaterParityCameraUU.Y, WaterParityCameraUU.Z,
					       WaterParityCameraRot.Pitch, WaterParityCameraRot.Yaw, SurfaceUU,
					       PollIntervalSeconds, MaxTerrainPolls);

					// --- Phase 1: terrain quiet, with NO water in the world ---
					WaterParityTerrainPollDelegate = FTimerDelegate::CreateWeakLambda(this,
						[this, PoseOnAnchor, PollIntervalSeconds, MaxTerrainPolls, MaxWaterPolls, ParityPourAmount]()
						{
							UWorld* TWorld = GetWorld();
							UVoxelWorldSubsystem* T = TWorld ? TWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
							UVoxelWaterSubsystem* W = TWorld ? TWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
							if (!T || !W)
							{
								return;
							}
							++WaterParityTerrainPolls;

							// Hold the pose every poll: the teleport above
							// kicks a large streaming footprint and the pawn
							// must not drift while it fills.
							PoseOnAnchor();

							const FVoxelPerfSnapshot Snap = T->GetPerfSnapshot();
							const bool bQuiet = Snap.JobsInFlight == 0 && Snap.PendingJobQueueDepth == 0 &&
							                    Snap.PendingGameThreadQueueDepth == 0 && Snap.PendingUnloadQueueDepth == 0 &&
							                    Snap.ChunksLoadedPerSec <= 0.f;

							UE_LOG(LogVoxelEarth, Log,
							       TEXT("VoxelWaterParityTest terrain-poll %d/%d: inFlight=%d pendingJob=%d pendingGT=%d ")
							       TEXT("pendingUnload=%d loaded/s=%.1f -> %s"),
							       WaterParityTerrainPolls, MaxTerrainPolls, Snap.JobsInFlight, Snap.PendingJobQueueDepth,
							       Snap.PendingGameThreadQueueDepth, Snap.PendingUnloadQueueDepth, Snap.ChunksLoadedPerSec,
							       bQuiet ? TEXT("QUIET") : TEXT("still streaming"));

							if (!bQuiet && WaterParityTerrainPolls < MaxTerrainPolls)
							{
								return; // repeating timer, poll again
							}
							GetWorldTimerManager().ClearTimer(WaterParityTerrainPollTimerHandle);

							if (!bQuiet)
							{
								UE_LOG(LogVoxelEarth, Warning,
								       TEXT("VoxelWaterParityTest: terrain did NOT go quiet within %d polls. The capture below ")
								       TEXT("is contaminated by streaming and MUST NOT be used as a parity number -- raise the ")
								       TEXT("cap or the delay and re-run."),
								       MaxTerrainPolls);
							}

							// --- Phase 2: pour, now that the scene is still ---
							const double PourSurfaceUU = T->GetSurfaceHeightUU(0.0, 0.0);
							const FVector PourLoc(0.0, 0.0, PourSurfaceUU + 500.0);
							const uint32 Placed = W->SpawnWaterAt(PourLoc, ParityPourAmount);
							UE_LOG(LogVoxelEarth, Log,
							       TEXT("VoxelWaterParityTest: terrain settled; poured %u/%u fill units at (0,0,%.0f). ")
							       TEXT("Waiting for the CA to settle (poll %.0fs, cap %d)."),
							       Placed, ParityPourAmount, PourLoc.Z, PollIntervalSeconds, MaxWaterPolls);

							// --- Phase 3: water quiet ---
							WaterParityWaterPollDelegate = FTimerDelegate::CreateWeakLambda(this,
								[this, PoseOnAnchor, MaxWaterPolls]()
								{
									UWorld* WWorld = GetWorld();
									UVoxelWaterSubsystem* WW = WWorld ? WWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
									if (!WW)
									{
										return;
									}
									++WaterParityWaterPolls;
									PoseOnAnchor();

									const FVoxelWaterPerfSnapshot WSnap = WW->GetPerfSnapshot();
									const bool bSettled = WSnap.ActiveBricks == 0;

									UE_LOG(LogVoxelEarth, Log,
									       TEXT("VoxelWaterParityTest water-poll %d/%d: activeBricks=%lld storedBricks=%lld ")
									       TEXT("volume=%llu maxFill=%d -> %s"),
									       WaterParityWaterPolls, MaxWaterPolls, WSnap.ActiveBricks, WSnap.StoredBricks,
									       (unsigned long long)WSnap.TotalVolume, WW->GetMaxStoredFill(),
									       bSettled ? TEXT("SETTLED") : TEXT("still moving"));

									if (!bSettled && WaterParityWaterPolls < MaxWaterPolls)
									{
										return;
									}
									GetWorldTimerManager().ClearTimer(WaterParityWaterPollTimerHandle);

									if (!bSettled)
									{
										UE_LOG(LogVoxelEarth, Warning,
										       TEXT("VoxelWaterParityTest: water still active after %d polls -- the capture will ")
										       TEXT("catch a moving surface and is not comparable across runs."),
										       MaxWaterPolls);
									}

									// --- Phase 4: hold the pose, then shoot ---
									// One more beat between the last pose
									// assertion and the shutter, so the frame
									// captured is the settled one.
									GetWorldTimerManager().SetTimer(
										WaterParityShotTimerHandle,
										FTimerDelegate::CreateWeakLambda(this,
											[this, PoseOnAnchor]()
											{
												UWorld* SWorld = GetWorld();
												UVoxelWaterSubsystem* SW =
													SWorld ? SWorld->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
												PoseOnAnchor();
												if (SW)
												{
													const FVoxelWaterPerfSnapshot Final = SW->GetPerfSnapshot();
													UE_LOG(LogVoxelEarth, Log,
													       TEXT("VoxelWaterParityTest CAPTURE: pose (%.0f,%.0f,%.0f) ")
													       TEXT("rot(pitch %.1f yaw %.1f) | activeBricks=%lld ")
													       TEXT("storedBricks=%lld volume=%llu maxFill=%d ")
													       TEXT("digest=0x%016llX"),
													       WaterParityCameraUU.X, WaterParityCameraUU.Y,
													       WaterParityCameraUU.Z, WaterParityCameraRot.Pitch,
													       WaterParityCameraRot.Yaw, Final.ActiveBricks,
													       Final.StoredBricks, (unsigned long long)Final.TotalVolume,
													       SW->GetMaxStoredFill(),
													       (unsigned long long)SW->GetWaterDigest());
												}
												FScreenshotRequest::RequestScreenshot(TEXT("VoxelWaterParity"), false, true);
											}),
										2.f, false);
								});
							GetWorldTimerManager().SetTimer(WaterParityWaterPollTimerHandle, WaterParityWaterPollDelegate,
							                                PollIntervalSeconds, true);
						});
					GetWorldTimerManager().SetTimer(WaterParityTerrainPollTimerHandle, WaterParityTerrainPollDelegate,
					                                PollIntervalSeconds, true);
				}),
			WaterParityDelaySeconds, false);
	}

	// ADR-0005 water persistence verification (docs/adr/0005-water-persistence.md):
	// -VoxelWaterPersistTest[=<delaySeconds>] (default 12s) pours a pool at the
	// spawn column and, best-effort, drains a flooded cavern near spawn (so the
	// saved state carries BOTH plain CA fill and mobilized-cavern water), lets it
	// settle, then SaveWaterState()s the blob and runs an in-process disk
	// round-trip: it reloads the actual on-disk .vxwater into a FRESH CA/mobilizer
	// and asserts digest/volume/mobilized-count all match. Self-quits, leaving the
	// blob for a cross-process -VoxelWaterLoadCheck reload.
	float WaterPersistDelaySeconds = 12.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterPersistTest="), WaterPersistDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterPersistTest")))
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelWaterPersistTest: will pour+drain near spawn in %.1fs, then save + round-trip verify."),
		       WaterPersistDelaySeconds);
		GetWorldTimerManager().SetTimer(
			WaterPersistTestPourTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* PW = GetWorld();
					UVoxelWorldSubsystem* Terrain = PW ? PW->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					UVoxelWaterSubsystem* Water = PW ? PW->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					if (!Terrain || !Water)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelWaterPersistTest: subsystems not ready, skipping."));
						return;
					}

					double SpawnX = 0.0, SpawnY = 0.0;
					ParseSpawnColumnUU(SpawnX, SpawnY); // (0,0) if -VoxelSpawnAt absent
					const double SurfaceUU = Terrain->GetSurfaceHeightUU(SpawnX, SpawnY);

					// (a) A plain CA-fill pour -- irreducible simulation state that
					// is NOT re-derivable from seed+edit-log, exactly the thing
					// ADR-0005 exists to persist. 30,000 units keeps enough above
					// the mesher's fill>=128 threshold (see -VoxelSpawnWaterTest).
					const uint32 Placed = Water->SpawnWaterAt(FVector(SpawnX, SpawnY, SurfaceUU + 500.0), 30000);

					// (b) Best-effort: drain a flooded cavern near spawn so the
					// blob also carries MOBILIZED water (the ADR's headline case:
					// mobilized set WITHOUT the CA fill destroys the lake on
					// reload). Carving fires the terrain-edit hooks that mobilize.
					// If none is reachable/resident this logs and is skipped -- the
					// pour alone still proves the round trip.
					FVector LakeSurface = FVector::ZeroVector;
					if (Water->FindFloodedCavernNear(FVector(SpawnX, SpawnY, SurfaceUU), 60000.0, LakeSurface))
					{
						const int32 Removed = Water->CarveCavernOutflow(LakeSurface);
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelWaterPersistTest: cavern-outflow carve removed %d voxel(s)."), Removed);
					}

					int32 MobBricks = 0; uint64 Deb = 0, Cred = 0, Short = 0;
					Water->GetMobilizationStats(MobBricks, Deb, Cred, Short);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelWaterPersistTest: poured %u fill units; pre-settle volume=%llu mobilized=%d shortfall=%llu"),
					       Placed, (unsigned long long)Water->GetPerfSnapshot().TotalVolume, MobBricks, (unsigned long long)Short);

					// Let the CA settle, then save + verify the disk round trip.
					GetWorldTimerManager().SetTimer(
						WaterPersistTestSaveTimerHandle,
						FTimerDelegate::CreateWeakLambda(this,
							[this]()
							{
								UWorld* SW = GetWorld();
								UVoxelWaterSubsystem* SWater = SW ? SW->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
								if (!SWater)
								{
									return;
								}
								const bool bSaved = SWater->SaveWaterState();

								uint64 LiveDig = 0, RelDig = 0, LiveVol = 0, RelVol = 0;
								int32 LiveMob = 0, RelMob = 0;
								const bool bRT = SWater->VerifyWaterDiskRoundTrip(LiveDig, RelDig, LiveVol, RelVol, LiveMob, RelMob);
								const bool bMatch = bRT && LiveDig == RelDig && LiveVol == RelVol && LiveMob == RelMob;
								UE_LOG(LogVoxelEarth, Log,
								       TEXT("VoxelWaterPersistTest: save=%s roundTrip=%s -- live[digest=0x%016llX vol=%llu mob=%d] ")
								       TEXT("reloaded[digest=0x%016llX vol=%llu mob=%d] => %s"),
								       bSaved ? TEXT("OK") : TEXT("FAILED"), bRT ? TEXT("read-back OK") : TEXT("read-back FAILED"),
								       (unsigned long long)LiveDig, (unsigned long long)LiveVol, LiveMob,
								       (unsigned long long)RelDig, (unsigned long long)RelVol, RelMob,
								       bMatch ? TEXT("PASS (drained/poured state survives serialize->disk->reload)")
								              : TEXT("FAIL (water state did NOT survive the round trip)"));

								GetWorldTimerManager().SetTimer(
									WaterPersistTestQuitTimerHandle,
									FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f, false);
							}),
						30.f, false);
				}),
			WaterPersistDelaySeconds, false);
	}

	// ADR-0005 cross-process reload half: -VoxelWaterLoadCheck[=<delaySeconds>]
	// (default 10s) logs the water state the OnWorldBeginPlay load path restored
	// from the on-disk .vxwater (volume/digest/mobilized), then quits. Launch it
	// on the SAME seed after a -VoxelWaterPersistTest run to prove the drained/
	// poured water genuinely came back across a process boundary (and, when the
	// blob is absent/stale, that the loud implicit fallback fires).
	float WaterLoadCheckDelaySeconds = 10.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterLoadCheck="), WaterLoadCheckDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterLoadCheck")))
	{
		GetWorldTimerManager().SetTimer(
			WaterLoadCheckTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* LW = GetWorld();
					UVoxelWaterSubsystem* LWater = LW ? LW->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
					if (!LWater)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelWaterLoadCheck: no water subsystem, skipping."));
						return;
					}
					const FVoxelWaterPerfSnapshot Snap = LWater->GetPerfSnapshot();
					int32 MobBricks = 0; uint64 Deb = 0, Cred = 0, Short = 0;
					LWater->GetMobilizationStats(MobBricks, Deb, Cred, Short);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelWaterLoadCheck: restored-from-disk water state storedBricks=%lld volume=%llu mobilized=%d ")
					       TEXT("digest=0x%016llX (nonzero volume/mobilized => the drained/poured state survived the reload)"),
					       Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, MobBricks,
					       (unsigned long long)LWater->GetWaterDigest());

					GetWorldTimerManager().SetTimer(
						WaterLoadCheckQuitTimerHandle,
						FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f, false);
				}),
			WaterLoadCheckDelaySeconds, false);
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

	// -VoxelCaveTest[=<delaySeconds>] (default 12s): find a REAL M4 cave void
	// near spawn and park the pawn in it. Nothing is edited -- a cave is
	// pristine worldgen, so its chunks are meshed only if the streaming
	// footprint actually reaches that depth. That makes this, and not the dug
	// chamber, the honest A/B for the underground fix: with
	// voxel.Stream.Underground 0 the cave is simply not there.
	float CaveTestDelaySeconds = 12.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveTest="), CaveTestDelaySeconds) ||
	    FParse::Param(FCommandLine::Get(), TEXT("VoxelCaveTest")))
	{
		bCaveTestActive = true;
		// Own the capture unless the caller explicitly drives it with
		// -VoxelScreenshotAfter, in which case the shared framing chain below
		// re-asserts the pose and takes the shot as before. Defaulting to
		// self-capture is the fix for the fixture's real failure mode: run
		// bare, it used to teleport into a cave and then never photograph it.
		float UnusedShotAfter = 0.f;
		bCaveTestSelfCapture =
			!FParse::Value(FCommandLine::Get(), TEXT("VoxelScreenshotAfter="), UnusedShotAfter);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCaveTest: searching for a cave near spawn in %.1fs (%s)"), CaveTestDelaySeconds,
		       bCaveTestSelfCapture ? TEXT("self-capture") : TEXT("capture driven by -VoxelScreenshotAfter"));
		GetWorldTimerManager().SetTimer(
			CaveTestTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* CaveWorld = GetWorld();
					UVoxelWorldSubsystem* Subsystem = CaveWorld ? CaveWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
					if (!Subsystem)
					{
						UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelCaveTest: subsystem not ready, skipping."));
						return;
					}
					double SpawnColXUU = 0.0, SpawnColYUU = 0.0;
					ParseSpawnColumnUU(SpawnColXUU, SpawnColYUU);

					FVector Center;
					double FloorZUU = 0.0;
					if (!FindCaveVoid(*Subsystem, SpawnColXUU, SpawnColYUU, Center, FloorZUU))
					{
						UE_LOG(LogVoxelEarth, Warning,
						       TEXT("VoxelCaveTest: no cave void found within the search radius -- try -VoxelSpawnAt."));
						return;
					}

					// --- framing ------------------------------------------
					//
					// The old pose was the raw search result with a hardcoded
					// FRotator(-35, 0, 0): the exact vertical midpoint of the
					// air run, aimed 35 degrees down at +X regardless of what
					// was actually there. In a 2.5 m pocket that is a lens
					// about a metre from the floor pointed at the floor, and
					// the yaw is whatever the world happened to put in the +X
					// direction -- usually a wall. Same class of bug as
					// -VoxelGICaveTest's sunlit mountainside and
					// -VoxelUndergroundTest's roof-jammed tunnel pose.
					//
					// Instead: measure the open directions with direct solidity
					// probes (NOT raycasts -- at search time these chunks are
					// not resident and a raycast reports MISS through solid
					// rock, which is what put the GI camera in a wall), then
					// stand back against the wall opposite the longest open run
					// and look ALONG it. That frames the length of the tunnel,
					// which is the thing worth photographing, and it puts the
					// nearest surface behind the camera rather than in the lens.
					auto IsAirAt = [Subsystem](const FVector& P)
					{
						return !Subsystem->IsSolidAtVoxel((int64)FMath::FloorToDouble(P.X / VoxelCoords::VoxelSizeUU),
						                                  (int64)FMath::FloorToDouble(P.Y / VoxelCoords::VoxelSizeUU),
						                                  (int64)FMath::FloorToDouble(P.Z / VoxelCoords::VoxelSizeUU));
					};
					const FVector Cardinals[4] = {FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0),
					                              FVector(0, -1, 0)};
					double ClearUU[4] = {0.0, 0.0, 0.0, 0.0};
					for (int32 I = 0; I < 4; ++I)
					{
						for (double Dist = 50.0; Dist <= 1500.0; Dist += 50.0)
						{
							if (!IsAirAt(Center + Cardinals[I] * Dist))
							{
								break; // contiguous run only
							}
							ClearUU[I] = Dist;
						}
					}
					int32 ViewIdx = 0;
					for (int32 I = 1; I < 4; ++I)
					{
						if (ClearUU[I] > ClearUU[ViewIdx])
						{
							ViewIdx = I;
						}
					}
					// Back up along the opposite cardinal, keeping half a metre
					// off that wall, and never more than 2 m (past that the
					// near wall crowds the frame -- the standoff lesson from
					// -VoxelGICaveTest).
					const int32 BackIdx = ViewIdx ^ 1; // pairs are (0,1) and (2,3)
					const double BackOffUU = FMath::Clamp(ClearUU[BackIdx] - 50.0, 0.0, 200.0);
					CaveTestCameraPos = Center + Cardinals[BackIdx] * BackOffUU;
					// Pitched gently down, deliberately. The bug this fixture
					// proves is "there is nothing under your feet": some floor
					// must be in frame, or a cave and a hole in the world look
					// identical. But 35 degrees down from a metre up is all
					// floor -- 12 degrees keeps the tunnel ahead AND the floor
					// under it in the same frame.
					float CavePitchDeg = -12.f;
					FParse::Value(FCommandLine::Get(), TEXT("VoxelCavePitch="), CavePitchDeg);
					CaveTestCameraRot =
						FRotator(CavePitchDeg,
					             float(FMath::RadiansToDegrees(FMath::Atan2(Cardinals[ViewIdx].Y, Cardinals[ViewIdx].X))),
					             0.f);
					bCaveTestFound = true;
					PoseInCaveTest();

					const double SurfaceUU = Subsystem->GetSurfaceHeightUU(Center.X, Center.Y);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelCaveTest: parked at (%.0f,%.0f,%.0f), %.1fm below surface, %.2fm above the ")
					       TEXT("cave floor; clearance +X=%.1fm -X=%.1fm +Y=%.1fm -Y=%.1fm; looking yaw %.0f pitch %.0f ")
					       TEXT("(standoff %.0fUU)"),
					       CaveTestCameraPos.X, CaveTestCameraPos.Y, CaveTestCameraPos.Z,
					       (SurfaceUU - CaveTestCameraPos.Z) / 100.0, (Center.Z - FloorZUU) / 100.0,
					       ClearUU[0] / 100.0, ClearUU[1] / 100.0, ClearUU[2] / 100.0, ClearUU[3] / 100.0,
					       CaveTestCameraRot.Yaw, CaveTestCameraRot.Pitch, BackOffUU);

					// Streaming diagnostic: is there actually a meshed chunk
					// here, and at the surface directly overhead? This
					// at-park reading is taken in the SAME FRAME as the
					// teleport, before UVoxelWorldSubsystem::Tick has seen the
					// new anchor and recomputed the desired set, so it reads
					// all-"--0" by construction and means nothing on its own.
					// It is kept only as the baseline for the at-capture
					// reading below, which is the one that measures streaming.
					LogUndergroundChunkStatus(*Subsystem, CaveTestCameraPos, TEXT("at-park"));

					// --- settle, then capture -----------------------------
					//
					// The fixture used to have no schedule of its own and
					// relied entirely on -VoxelScreenshotAfter. That is why it
					// photographed nothing: the pawn teleports tens of metres
					// underground, which makes the anchor underground for the
					// first time and requires the whole deep streaming box to
					// be recomputed and meshed from scratch, and the only
					// evidence anyone had was the at-park dump above -- taken
					// before a single streaming tick. (Worse, with
					// -VoxelScreenshotAfter <= the search delay, the cave
					// branch there never fired at all and the run silently
					// produced an ordinary surface screenshot.)
					//
					// Own the schedule, exactly as -VoxelGICaveTest does, and
					// re-assert the pose on the way in so nothing can drift the
					// camera between the teleport and the shutter.
					if (bCaveTestSelfCapture)
					{
						float CaveSettleSeconds = 40.f;
						FParse::Value(FCommandLine::Get(), TEXT("VoxelCaveSettle="), CaveSettleSeconds);
						auto Repose = [this]() { PoseInCaveTest(); };
						GetWorldTimerManager().SetTimer(
							CavePoseTimerHandle, FTimerDelegate::CreateWeakLambda(this, Repose), 0.5f, false);
						GetWorldTimerManager().SetTimer(CaveRepose1TimerHandle,
						                                FTimerDelegate::CreateWeakLambda(this, Repose),
						                                FMath::Max(1.f, CaveSettleSeconds - 4.f), false);
						GetWorldTimerManager().SetTimer(CaveRepose2TimerHandle,
						                                FTimerDelegate::CreateWeakLambda(this, Repose),
						                                FMath::Max(1.f, CaveSettleSeconds - 2.f), false);
						GetWorldTimerManager().SetTimer(
							CaveShotTimerHandle,
							FTimerDelegate::CreateWeakLambda(this,
								[this]()
								{
									PoseInCaveTest();
									UWorld* SWorld = GetWorld();
									UVoxelWorldSubsystem* T =
										SWorld ? SWorld->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
									if (APlayerController* CC = SWorld ? SWorld->GetFirstPlayerController() : nullptr)
									{
										if (CC->PlayerCameraManager)
										{
											// The ACTUAL camera, not the
											// intended one: the only way to
											// tell "the pose did not stick"
											// from "the pose was wrong".
											const FVector L = CC->PlayerCameraManager->GetCameraLocation();
											const FRotator R = CC->PlayerCameraManager->GetCameraRotation();
											UE_LOG(LogVoxelEarth, Log,
											       TEXT("VoxelCaveTest capture: ACTUAL cam=(%.0f,%.0f,%.0f) ")
											       TEXT("rot=(pitch %.1f yaw %.1f) | intended=(%.0f,%.0f,%.0f) ")
											       TEXT("pitch %.1f yaw %.1f"),
											       L.X, L.Y, L.Z, R.Pitch, R.Yaw, CaveTestCameraPos.X,
											       CaveTestCameraPos.Y, CaveTestCameraPos.Z, CaveTestCameraRot.Pitch,
											       CaveTestCameraRot.Yaw);
										}
									}
									if (T)
									{
										// Re-probe at CAPTURE time, when the
										// chunks around the camera are actually
										// resident. A probe at search time
										// reports nothing but MISS.
										const FVector Dirs[6] = {FVector(1, 0, 0),  FVector(-1, 0, 0),
										                         FVector(0, 1, 0),  FVector(0, -1, 0),
										                         FVector(0, 0, 1),  FVector(0, 0, -1)};
										const TCHAR* Names[6] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"),
										                         TEXT("-Y"), TEXT("+Z"), TEXT("-Z")};
										FString Report;
										for (int32 I = 0; I < 6; ++I)
										{
											FVector Hit, Prev;
											Report += FString::Printf(
												TEXT("%s=%s "), Names[I],
												T->RaycastVoxelWorld(CaveTestCameraPos, Dirs[I], 6000.0, Hit, Prev)
													? *FString::Printf(TEXT("%.1fm"),
													                   (Hit - CaveTestCameraPos).Size() / 100.0)
													: TEXT("open"));
										}
										UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCaveTest capture probe: %s"), *Report);
										// THE measurement this fixture exists
										// for, and the one that was missing:
										// the promised second column dump,
										// taken when streaming has had the
										// whole settle to catch up.
										LogUndergroundChunkStatus(*T, CaveTestCameraPos, TEXT("at-capture"));
									}
									FScreenshotRequest::RequestScreenshot(TEXT("VoxelCave"), false, true);
								}),
							CaveSettleSeconds, false);
						GetWorldTimerManager().SetTimer(
							CaveQuitTimerHandle,
							FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(false); }),
							CaveSettleSeconds + 4.f, false);
					}
				}),
			CaveTestDelaySeconds, false);
	}

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

// --- downward escape hatch proof: read the whole column back -----------------

void AVoxelEarthGameMode::LogDigDownColumn(UVoxelWorldSubsystem& Subsystem, double ColumnXUU, double ColumnYUU,
                                           const TCHAR* Phase) const
{
	// One entry per level-0 chunk from just above the surface down to 25
	// chunks (80 m) below it -- well past both the 12-chunk depth skirt and any
	// shaft this test digs. Each entry is "<depth>:<T|-><C|-><quads>", the same
	// shape LogUndergroundChunkStatus uses, plus a summary of the part that
	// actually matters: how deep the TRACKED column reaches.
	const double SurfaceUU = Subsystem.GetSurfaceHeightUU(ColumnXUU, ColumnYUU);
	FString Line;
	int32 DeepestTrackedStep = 0;
	int32 DeepestGeometryStep = 0;
	int32 TrackedCount = 0;
	for (int32 Step = 1; Step <= 25; ++Step)
	{
		const double Z = SurfaceUU - double(Step) * VoxelCoords::ChunkEdgeUU;
		bool bTracked = false, bComp = false;
		int32 Quads = 0;
		Subsystem.DebugChunkStatusAt(FVector(ColumnXUU, ColumnYUU, Z), bTracked, bComp, Quads);
		Line += FString::Printf(TEXT("%-.0fm:%s%s%d "), (SurfaceUU - Z) / 100.0, bTracked ? TEXT("T") : TEXT("-"),
		                        bComp ? TEXT("C") : TEXT("-"), Quads);
		if (bTracked)
		{
			++TrackedCount;
			DeepestTrackedStep = Step;
		}
		if (Quads > 0)
		{
			DeepestGeometryStep = Step;
		}
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelDigDownTest [%s] column at (%.0f,%.0f) surface=%.0f: tracked=%d/25 deepestTracked=%.1fm ")
	       TEXT("deepestGeometry=%.1fm | %s"),
	       Phase, ColumnXUU, ColumnYUU, SurfaceUU, TrackedCount,
	       double(DeepestTrackedStep) * VoxelCoords::ChunkEdgeUU / 100.0,
	       double(DeepestGeometryStep) * VoxelCoords::ChunkEdgeUU / 100.0, *Line);
}

// --- underground streaming proof: streaming diagnostic -----------------------

void AVoxelEarthGameMode::LogUndergroundChunkStatus(UVoxelWorldSubsystem& Subsystem, const FVector& Center,
                                                    const TCHAR* Phase) const
{
	// Sample a vertical stack through the camera's column: the cave itself,
	// a few chunks above and below it, and the surface. Each entry reads
	// "<relative metres>:<T|-><C|-><quads>" where T = the chunk is in the
	// desired set, C = it owns a live component.
	const double SurfaceUU = Subsystem.GetSurfaceHeightUU(Center.X, Center.Y);
	FString Line;
	for (int32 Step = -6; Step <= 6; ++Step)
	{
		const double Z = Center.Z + double(Step) * VoxelCoords::ChunkEdgeUU;
		bool bTracked = false, bComp = false;
		int32 Quads = 0;
		Subsystem.DebugChunkStatusAt(FVector(Center.X, Center.Y, Z), bTracked, bComp, Quads);
		Line += FString::Printf(TEXT("%+.1fm:%s%s%d "), (Z - SurfaceUU) / 100.0, bTracked ? TEXT("T") : TEXT("-"),
		                        bComp ? TEXT("C") : TEXT("-"), Quads);
	}
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelCaveTest [%s] column (depth:tracked/component/quads): %s"), Phase, *Line);
}

// --- GI proof: finding a real worldgen sinkhole ------------------------------
//
// A sinkhole is a column whose air runs CONTINUOUSLY from just under the
// surface down past the top of the cave band -- i.e. daylight reaches
// underground there. docs/status.md puts them at roughly one per 205 m square,
// so the search has to reach well past R0; the camera teleports there and the
// streaming system catches up before the capture.
//
// Nothing is carved. This is pristine worldgen, which is the point: it is the
// world's own geometry lighting itself, not a fixture built to flatter the
// feature.
bool AVoxelEarthGameMode::FindSinkholeColumn(UVoxelWorldSubsystem& Subsystem, double OriginXUU, double OriginYUU,
                                             FVector& OutShaftBaseUU, double& OutSurfaceUU) const
{
	constexpr double SearchRadiusUU = 30000.0; // 300 m -- ~2 sinkhole spacings
	constexpr double StepXYUU = 200.0;         // 2 m; a sinkhole throat is wider than this
	constexpr double ThroatDepthUU = 800.0;    // 8 m of continuous air below the surface
	constexpr double ProbeStepUU = 50.0;
	constexpr double MaxFloorDepthUU = 4000.0; // stop looking for a floor at 40 m

	for (double Ring = 0.0; Ring <= SearchRadiusUU; Ring += StepXYUU)
	{
		for (double OffY = -Ring; OffY <= Ring; OffY += StepXYUU)
		{
			for (double OffX = -Ring; OffX <= Ring; OffX += StepXYUU)
			{
				// Perimeter only: the interior was covered by a smaller ring,
				// so the first hit is the CLOSEST sinkhole to spawn.
				if (Ring > 0.0 && FMath::Abs(OffX) < Ring - 0.5 && FMath::Abs(OffY) < Ring - 0.5)
				{
					continue;
				}
				const double WorldX = OriginXUU + OffX;
				const double WorldY = OriginYUU + OffY;
				const double SurfaceUU = Subsystem.GetSurfaceHeightUU(WorldX, WorldY);
				if (SurfaceUU <= 0.0)
				{
					continue; // below sea level
				}
				const int64 Vx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
				const int64 Vy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);

				// Continuous air from 0.5 m under the surface to ThroatDepthUU.
				bool bOpenThroat = true;
				for (double D = ProbeStepUU; D <= ThroatDepthUU && bOpenThroat; D += ProbeStepUU)
				{
					const int64 Vz = (int64)FMath::FloorToDouble((SurfaceUU - D) / VoxelCoords::VoxelSizeUU);
					bOpenThroat = !Subsystem.IsSolidAtVoxel(Vx, Vy, Vz);
				}
				if (!bOpenThroat)
				{
					continue;
				}

				// Follow the shaft down to the first solid -- the cave floor
				// the daylight actually lands on.
				double FloorZ = SurfaceUU - ThroatDepthUU;
				for (double D = ThroatDepthUU; D <= MaxFloorDepthUU; D += ProbeStepUU)
				{
					const double Z = SurfaceUU - D;
					const int64 Vz = (int64)FMath::FloorToDouble(Z / VoxelCoords::VoxelSizeUU);
					if (Subsystem.IsSolidAtVoxel(Vx, Vy, Vz))
					{
						FloorZ = Z;
						break;
					}
					FloorZ = Z;
				}
				// The shaft has to bottom out in something worth standing in:
				// require ~2.5 m of head room above the floor, or keep looking.
				// Without this the search happily returns a blind throat that
				// pinches shut, and the camera ends up wedged in rock.
				const FVector Base(WorldX, WorldY, FloorZ + VoxelCoords::VoxelSizeUU);
				bool bRoomBelow = true;
				for (double H = 50.0; H <= 250.0 && bRoomBelow; H += 100.0)
				{
					const int64 Vz = (int64)FMath::FloorToDouble((Base.Z + H) / VoxelCoords::VoxelSizeUU);
					bRoomBelow = !Subsystem.IsSolidAtVoxel(Vx, Vy, Vz);
				}
				if (!bRoomBelow)
				{
					continue;
				}

				OutShaftBaseUU = Base;
				OutSurfaceUU = SurfaceUU;
				return true;
			}
		}
	}
	return false;
}

// --- underground streaming proof: finding a real cave ------------------------

bool AVoxelEarthGameMode::FindCaveVoid(UVoxelWorldSubsystem& Subsystem, double OriginXUU, double OriginYUU,
                                       FVector& OutCenter, double& OutFloorZUU) const
{
	// voxelcore/caves.h: tunnel axes run 9m..34m below the surface, radius up
	// to 2.8m, so every cave voxel is inside roughly [6m, 37m] of depth. Probe
	// that band only. Cave nodes sit on a kCaveLatticeMm = 25.6m grid, so a
	// +-64m search around spawn crosses several lattice cells and is very
	// likely to cross a tunnel; the step is 1m horizontally (tunnels are
	// >=2.4m across, so 1m cannot step over one) and 0.5m vertically.
	constexpr double SearchRadiusUU = 6400.0;
	constexpr double StepXYUU = 100.0;
	constexpr double MinDepthUU = 600.0;
	constexpr double MaxDepthUU = 3700.0;
	constexpr double StepZUU = 50.0;
	constexpr double MinVoidHeightUU = 250.0; // 2.5m: room for a camera plus floor clearance

	double BestHeightUU = 0.0;
	double BestDistUU = 0.0;
	bool bFound = false;

	auto IsAirAt = [&Subsystem](double X, double Y, double Z)
	{
		return !Subsystem.IsSolidAtVoxel((int64)FMath::FloorToDouble(X / VoxelCoords::VoxelSizeUU),
		                                 (int64)FMath::FloorToDouble(Y / VoxelCoords::VoxelSizeUU),
		                                 (int64)FMath::FloorToDouble(Z / VoxelCoords::VoxelSizeUU));
	};
	// A void is only usable if the camera has somewhere to LOOK: a single-voxel
	// crack passes the "2 m of air overhead" test and then photographs as a wall
	// 30 cm from the lens. Require air one metre out on at least two opposite
	// cardinals at eye height, i.e. a pocket at least ~2 m across. This is the
	// same class of check -VoxelGICaveTest grew after it parked a camera in a
	// wall, and -VoxelUndergroundTest after it parked one in a tunnel roof.
	auto HasLateralRoom = [&IsAirAt](double X, double Y, double EyeZ)
	{
		const bool bXOpen = IsAirAt(X + 100.0, Y, EyeZ) && IsAirAt(X - 100.0, Y, EyeZ);
		const bool bYOpen = IsAirAt(X, Y + 100.0, EyeZ) && IsAirAt(X, Y - 100.0, EyeZ);
		return bXOpen || bYOpen;
	};

	// Walk outward in rings so the cave we pick is the CLOSEST decent one to
	// spawn. Distance is not cosmetic: UVoxelWorldSubsystem's underground skirt
	// is 12 chunks (38.4 m of depth) only inside the R0 NEAR band, drops to 5
	// chunks (16 m) in the mid band and to ZERO past 32 m out. A cave 60 m from
	// spawn is below the streamed depth at park time no matter how long you
	// wait for the initial footprint, so a search that returns a far cave when
	// a near one exists is itself a way to photograph nothing.
	//
	// The stride here USED TO BE `Ring += StepXYUU * 4.0` while the offsets
	// stepped by StepXYUU, so only the square outlines at 0, 4, 8, ... m were
	// ever probed and three metres in every four were skipped -- which both
	// missed near caves outright and, when it did hit, returned a cave up to
	// four metres further out than the nearest one. Rings now advance by the
	// same step the offsets do, so the scan is complete.
	for (double Ring = 0.0; Ring <= SearchRadiusUU && !bFound; Ring += StepXYUU)
	{
		for (double OffY = -Ring; OffY <= Ring; OffY += StepXYUU)
		{
			for (double OffX = -Ring; OffX <= Ring; OffX += StepXYUU)
			{
				// Only the ring's perimeter (the interior was covered by a
				// previous, smaller ring).
				if (Ring > 0.0 && FMath::Abs(OffX) < Ring - 0.5 && FMath::Abs(OffY) < Ring - 0.5)
				{
					continue;
				}
				const double WorldX = OriginXUU + OffX;
				const double WorldY = OriginYUU + OffY;
				const double SurfaceUU = Subsystem.GetSurfaceHeightUU(WorldX, WorldY);
				const int64 Vx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
				const int64 Vy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);
				const double DistUU = FMath::Sqrt(OffX * OffX + OffY * OffY);

				// March down the column, tracking contiguous air runs. A run is
				// a candidate once it ends (or the band does), so its FLOOR is
				// known -- the camera is placed relative to the floor, never at
				// the midpoint of the run, because the midpoint of a tall
				// chimney is nowhere in particular.
				double RunTopUU = 0.0;
				double RunHeightUU = 0.0;
				auto ConsiderRun = [&](double FloorZUU)
				{
					if (RunHeightUU < MinVoidHeightUU)
					{
						return;
					}
					const double EyeZUU = FloorZUU + FMath::Min(150.0, RunHeightUU * 0.5);
					if (!HasLateralRoom(WorldX, WorldY, EyeZUU))
					{
						return; // a crack, not a chamber
					}
					// Prefer the tallest void on this ring; ties go to the
					// closest column.
					if (RunHeightUU > BestHeightUU || (RunHeightUU == BestHeightUU && DistUU < BestDistUU))
					{
						BestHeightUU = RunHeightUU;
						BestDistUU = DistUU;
						OutCenter = FVector(WorldX, WorldY, EyeZUU);
						OutFloorZUU = FloorZUU;
						bFound = true;
					}
				};
				for (double Depth = MinDepthUU; Depth <= MaxDepthUU; Depth += StepZUU)
				{
					const double Z = SurfaceUU - Depth;
					const int64 Vz = (int64)FMath::FloorToDouble(Z / VoxelCoords::VoxelSizeUU);
					if (!Subsystem.IsSolidAtVoxel(Vx, Vy, Vz))
					{
						if (RunHeightUU <= 0.0)
						{
							RunTopUU = Z;
						}
						RunHeightUU += StepZUU;
					}
					else
					{
						ConsiderRun(Z + StepZUU); // first air step above this solid
						RunHeightUU = 0.0;
					}
				}
				// A run still open at the bottom of the band: its floor is
				// below MaxDepth, so measure from the deepest air we saw.
				ConsiderRun(RunTopUU - RunHeightUU + StepZUU);
			}
		}
	}

	if (bFound)
	{
		const double SurfaceUU = Subsystem.GetSurfaceHeightUU(OutCenter.X, OutCenter.Y);
		// The distance and band are logged because they are the first thing to
		// check when the column dump comes back empty: see the skirt bands in
		// UVoxelWorldSubsystem's VoxelUnderground namespace.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelCaveTest: found a %.1fm-tall cave void at (%.0f,%.0f,%.0f), floor %.0f, ")
		       TEXT("%.1fm below surface, %.1fm from spawn (%s band)."),
		       BestHeightUU / 100.0, OutCenter.X, OutCenter.Y, OutCenter.Z, OutFloorZUU,
		       (SurfaceUU - OutCenter.Z) / 100.0, BestDistUU / 100.0,
		       BestDistUU < 1600.0 ? TEXT("near") : (BestDistUU < 3200.0 ? TEXT("mid") : TEXT("FAR -- no skirt")));
	}
	return bFound;
}

void AVoxelEarthGameMode::PoseInCaveTest() const
{
	UWorld* PWorld = GetWorld();
	APlayerController* Ctrl = PWorld ? PWorld->GetFirstPlayerController() : nullptr;
	if (!Ctrl || !bCaveTestFound)
	{
		return;
	}
	if (APawn* P = Ctrl->GetPawn())
	{
		// TeleportPhysics + SetViewTarget, matching -VoxelGICaveTest: a plain
		// SetActorLocation leaves the movement component's velocity and the
		// view target untouched, which is how a pose silently fails to stick.
		P->SetActorLocation(CaveTestCameraPos, false, nullptr, ETeleportType::TeleportPhysics);
		P->SetActorRotation(CaveTestCameraRot);
		Ctrl->SetViewTarget(P);
	}
	Ctrl->SetControlRotation(CaveTestCameraRot);
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
	if (IsUndergroundTunnelView())
	{
		// -VoxelUndergroundView=tunnel: the FRAMING fix. The two poses above
		// both sit 120UU above the tunnel AXIS, but the tunnel is carved with
		// a 130UU radius -- so a "standing eye height" offset puts the camera
		// within 10UU of the tunnel ROOF, aimed at the rock above the tunnel
		// mouth rather than down the passage. That is why the chamber shot
		// reads as a wall 2.3m from the lens (the measured -X enclosure
		// distance) instead of a 9m corridor.
		//
		// Sit just above the axis instead (40UU, comfortably inside the
		// radius) at the chamber end, so the shot looks straight down the
		// full 900UU tunnel to the shaft foot -- and up the shaft to real
		// daylight, which is also the check that the underground veil is not
		// blocking legitimate sky.
		return FVector(UndergroundTestColumnXUU + 900.0, UndergroundTestColumnYUU, FloorZUU + 40.0);
	}
	// In the chamber, looking back toward the tunnel: rock on every side.
	return FVector(UndergroundTestColumnXUU + 900.0, UndergroundTestColumnYUU, FloorZUU + 120.0);
}

bool AVoxelEarthGameMode::IsUndergroundTunnelView() const
{
	FString View;
	return FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundView="), View) &&
	       View.Equals(TEXT("tunnel"), ESearchCase::IgnoreCase);
}

FRotator AVoxelEarthGameMode::UndergroundTestCameraRotation() const
{
	// Shaft view looks along +X down the tunnel; chamber view looks back along
	// -X toward the tunnel mouth. A few degrees of downward pitch in both so
	// the floor is in frame (a floor is precisely what used to be missing).
	if (IsUndergroundTunnelView())
	{
		// Dead level along the tunnel axis (-X, back toward the shaft). Any
		// downward pitch here just fills the frame with the near floor, which
		// is what "a few degrees so the floor is in frame" costs once the
		// camera is actually ON the axis rather than against the roof.
		return FRotator(0.f, 180.f, 0.f);
	}
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

	// GetSurfaceHeightUU is the PURE WORLDGEN column height -- it ignores caving.
	// At a column where a cavern or cave void breaches down from (or up to) the
	// surface, the worldgen surface is open air and a pawn spawned there+5m falls
	// tens of metres and grounds on the cavern floor, enclosed by rock walls
	// (originally observed at -VoxelSpawnAt=-66240,67200, which has since drifted
	// to open ground under later worldgen; a currently-carved column is
	// (116.7,-96.3) m -- worldgen surface 1077.4 m, true walkable surface 16.0 m
	// below). Ground the pawn on the ACTUAL walkable surface
	// instead: the highest solid voxel of the column that has air directly above
	// it -- which is where a falling pawn would come to rest anyway, so this
	// removes the long drop rather than changing where the pawn ends up.
	const double SurfaceUU = Subsystem->GetSurfaceHeightUU(SpawnWorldX, SpawnWorldY);
	double GroundTopUU = SurfaceUU; // default / open-ground: worldgen surface, unchanged
	{
		const int64 Vx = int64(FMath::FloorToDouble(SpawnWorldX / VoxelCoords::VoxelSizeUU));
		const int64 Vy = int64(FMath::FloorToDouble(SpawnWorldY / VoxelCoords::VoxelSizeUU));
		// Start a couple of voxels ABOVE the worldgen surface so a column with a
		// little placed/overhang material above it is still caught, and scan down.
		const int64 TopVz = int64(FMath::CeilToDouble(SurfaceUU / VoxelCoords::VoxelSizeUU)) + 2;
		// ~600 m of downward reach: far past the ~24 m cavern in the report and
		// past any plausible near-surface void, still a cheap bounded probe (a few
		// thousand IsSolidAtVoxel calls, worldgen-cheap, once per pawn spawn).
		constexpr int64 MaxScanVoxels = 6000;
		int64 FoundVz = INT64_MIN;
		for (int64 Vz = TopVz; Vz >= TopVz - MaxScanVoxels; --Vz)
		{
			if (Subsystem->IsSolidAtVoxel(Vx, Vy, Vz) && !Subsystem->IsSolidAtVoxel(Vx, Vy, Vz + 1))
			{
				FoundVz = Vz;
				break;
			}
		}
		if (FoundVz != INT64_MIN)
		{
			const double TrueTopUU = double(FoundVz + 1) * VoxelCoords::VoxelSizeUU;
			// Only override when the real walkable surface is meaningfully BELOW
			// the worldgen surface (a genuine carved column). For ordinary open
			// ground the two coincide to within a voxel, and keeping SurfaceUU
			// verbatim there leaves every existing spawn and headless fixture
			// (which assume the worldgen-surface spawn) byte-for-byte unchanged.
			if (TrueTopUU < SurfaceUU - VoxelCoords::VoxelSizeUU)
			{
				GroundTopUU = TrueTopUU;
				UE_LOG(LogVoxelEarth, Log,
				       TEXT("Spawn column (%.1f, %.1f) m is carved: worldgen surface z=%.1f m but the true walkable surface ")
				       TEXT("(top solid voxel with air above) is z=%.1f m, %.1f m down -- grounding the pawn there instead of ")
				       TEXT("dropping it through the void."),
				       SpawnWorldX / 100.0, SpawnWorldY / 100.0, SurfaceUU / 100.0, TrueTopUU / 100.0,
				       (SurfaceUU - TrueTopUU) / 100.0);
			}
		}
		else
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Spawn column (%.1f, %.1f) m: no solid voxel found within %.0f m below the worldgen surface -- ")
			       TEXT("using the worldgen surface height as the spawn ground."),
			       SpawnWorldX / 100.0, SpawnWorldY / 100.0, double(MaxScanVoxels) * VoxelCoords::VoxelSizeUU / 100.0);
		}
	}

	const FVector SpawnLocation(SpawnWorldX, SpawnWorldY, GroundTopUU + SpawnHeightAboveSurfaceUU);
	const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

	RestartPlayerAtTransform(NewPlayer, SpawnTransform);
}

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VoxelEarthGameMode.generated.h"

// -VoxelSpawnAt=X,Y (meters, world) -> world UU.
//
// PROMOTED OUT OF VoxelEarthGameMode.cpp's anonymous namespace, and the reason
// matters more than the mechanics. This helper was already deliberately shared
// between the PAWN spawn and the old BeginPlay SKY placement so the two could
// not drift apart (M2 task "SkyAtmosphere origin fix"); when the light rig moved
// into UVoxelSkySubsystem (W4, docs/lighting-weather-plan.md) the sky half moved
// into a different translation unit. Copying the parse over there would have
// re-created exactly the drift this helper exists to prevent -- and the symptom
// of that drift is an atmosphere 20,000 km from the player at a far LWC spawn,
// which reads as a rendering bug rather than as two parsers disagreeing.
//
// Returns false (Out* left at 0,0 UU) if the switch is absent OR malformed
// (malformed also logs a warning); true with parsed meters->UU values otherwise.
// Definition stays in VoxelEarthGameMode.cpp -- there is still exactly one.
// Declared at the BOTTOM of this header, after the UCLASS, matching VoxelGI.h's
// placement of namespace VoxelGI.

// Default game mode (docs/m1-plan.md Stage 2 decisions table item 3): fly
// pawn + our dig/place controller, spawned above the terrain surface at
// (0,0) rather than at a level-placed PlayerStart (none exists yet -- see
// RestartPlayer override).
UCLASS()
class VOXELEARTH_API AVoxelEarthGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AVoxelEarthGameMode();

	virtual void RestartPlayer(AController* NewPlayer) override;

	// --- Front-end seam (docs/front-end-plan.md) -----------------------------
	//
	// Spawns and possesses the player pawn, once, when the main menu hands off
	// on NEW GAME/CONTINUE. Only ever called when
	// VoxelFrontEnd::IsEnabledThisRun() is true -- otherwise AGameModeBase
	// spawns the pawn at StartPlay exactly as it always did, which is what the
	// self-driving verification switches depend on.
	//
	// SpawnOverride is a saved player transform to restore, or null for the
	// ordinary spawn column. It is honoured inside the RestartPlayer override
	// (which is where every -VoxelSpawn* switch already lives), rather than by
	// a second spawn path that would have to duplicate all of them.
	//
	// Idempotent: a second call warns and returns. Returns without spawning
	// (and warns) if no player controller exists yet, so a caller that runs a
	// frame early can simply try again next tick.
	void BeginPlayerSession(const FTransform* SpawnOverride);
	virtual void BeginPlay() override;

private:

	// --- Front-end seam state (docs/front-end-plan.md) -----------------------
	// Set once by BeginPlayerSession; guards against a double NEW GAME press
	// spawning two pawns.
	bool bPlayerSessionBegun = false;
	// A saved player transform to restore on the next RestartPlayer, set and
	// cleared around that call by BeginPlayerSession. Unset in every run where
	// the front end is suppressed, so the spawn-shaping -Voxel* switches keep
	// their existing precedence untouched.
	TOptional<FTransform> PendingSessionSpawnTransform;

	FTimerHandle ScreenshotTimerHandle;
	FTimerHandle SecondShotTimerHandle;
	FTimerHandle QuitTimerHandle;

	// M2 wave 2 item 2 verification (docs/m2-plan.md "Distant-edit mip
	// propagation"): -VoxelHeadlessDigTest fires a large CarveSphere at the
	// spawn column once R0/R1 have had time to settle, so a headless -game
	// run can prove (via log lines + a screenshot) that ring chunks over the
	// dig site actually re-mesh through the overlay-aware path.
	FTimerHandle HeadlessDigTestTimerHandle;

	// Downward escape hatch proof (docs/streaming-handoff.md "Deep-column
	// waste"): -VoxelDigDownTest[=<delaySeconds>] sinks a shaft from the spawn
	// column straight down PAST the depth skirt's floor -- deeper than any
	// worldgen-only rule admits -- and logs the whole column's
	// tracked/component/quads state before and after. Without the
	// EditedFootprintMinZ widening of ChunkZMin the bottom of that shaft is
	// outside the desired set and reads as a see-through hole; with it, the
	// chunks fill in. The pawn stays ABOVE ground throughout, deliberately, so
	// the anchor-relative deep box (which only exists once the anchor is
	// underground) cannot cover for the hatch and make a broken hatch look
	// healthy.
	FTimerHandle DigDownTestDigTimerHandle;
	FTimerHandle DigDownTestReportTimerHandle;
	FTimerHandle DigDownTestQuitTimerHandle;
	void LogDigDownColumn(class UVoxelWorldSubsystem& Subsystem, double ColumnXUU, double ColumnYUU, const TCHAR* Phase) const;

	// M3 wave 1 gate verification (docs/m3-plan.md "two clients dig the same
	// hole"): -VoxelDumpDigestAfter=<s> logs THIS process's (the server's)
	// seed + World::editedDigest() -- see AVoxelEarthPlayerController's
	// identical switch for the client-side dump (GameMode only exists
	// server-side, so the server's own dump has to live here instead).
	// Self-quits a few seconds after dumping (see the .cpp) so a headless
	// dedicated-server process run for the gate exits on its own instead of
	// needing to be killed externally.
	// M4 voxel light field + cone-traced GI verification (-VoxelGITest,
	// -VoxelGIBreach, -VoxelGIOn). Self-contained: this switch does its own
	// carve, its own camera framing and its own capture/quit, so it adds
	// nothing to the shared -VoxelScreenshotAfter framing chain.
	FVector GITestChamberCentreUU = FVector::ZeroVector;
	double GITestSurfaceUU = 0.0;
	FTimerHandle GITestTimerHandle;
	FTimerHandle GITestBreachTimerHandle;
	FTimerHandle GITestPoseTimerHandle;
	FTimerHandle GITestShotTimerHandle;
	FTimerHandle GITestQuitTimerHandle;

	// -VoxelGICaveTest: find a real worldgen SINKHOLE near spawn, drop the
	// camera into the cave under it and frame the daylight coming down the
	// shaft. Same self-contained pose/capture/quit shape as -VoxelGITest.
	FVector GICaveCameraUU = FVector::ZeroVector;
	FRotator GICaveCameraRot = FRotator::ZeroRotator;
	bool bGICaveFound = false;
	FTimerHandle GICaveTestTimerHandle;
	FTimerHandle GICavePoseTimerHandle;
	FTimerHandle GICaveRepose1TimerHandle;
	FTimerHandle GICaveRepose2TimerHandle;
	FTimerHandle GICaveShotTimerHandle;
	// -VoxelGICaveTorch: the L1 gate's second arm. ONE process, ONE camera, two
	// captures -- torch off then torch on -- because this project's screenshot
	// noise floor is 0.00% within a session and 1.81% between them
	// (VoxelGpuVerify.cpp:2074-2084), and a 4x luma ratio measured across two
	// launches would be carrying a per-session latch it cannot separate from the
	// signal. See docs/sky-and-local-light-plan.md §4, phase L1.
	FTimerHandle GICaveTorchTimerHandle;
	FTimerHandle GICaveTorchShotTimerHandle;
	FTimerHandle GICaveQuitTimerHandle;

	// --- C7/C8 underground water verification (docs/cavern-design.md SS5) ----
	// -VoxelFloodTest[=<delaySeconds>]: finds a real flooded cavern near spawn,
	// poses the camera on its shore and captures the STATIC IMPLICIT lake
	// (VoxelFloodLake), then carves an outflow tunnel out through the flood
	// reach and captures the same view again once the lake has DRAINED into it
	// (VoxelFloodDrain). Two shots, one run, same camera -- the pair is the
	// deliverable. Logs the mobilization ledger at every stage; shortfall must
	// be 0 (waterca.h).
	FTimerHandle FloodTestFindTimerHandle;
	FTimerHandle FloodTestPose1TimerHandle;
	FTimerHandle FloodTestShot1TimerHandle;
	FTimerHandle FloodTestCarveTimerHandle;
	FTimerHandle FloodTestPose2TimerHandle;
	FTimerHandle FloodTestShot2TimerHandle;
	FTimerHandle FloodTestQuitTimerHandle;
	bool bFloodTestFound = false;
	// The outflow carve re-arms itself on this delegate until it removes
	// something (chunk residency makes a single attempt unreliable).
	FTimerDelegate FloodTestCarveRetryDelegate;
	int32 FloodTestCarveAttempts = 0;
	FVector FloodTestLakeSurfaceUU = FVector::ZeroVector;
	FVector FloodTestCameraUU = FVector::ZeroVector;
	FRotator FloodTestCameraRot = FRotator::ZeroRotator;

	FTimerHandle ServerDumpDigestTimerHandle;
	FTimerHandle ServerDigestQuitTimerHandle;

	// M3 wave 2 persistence verification (docs/m3-plan.md "Save/load"):
	// -VoxelSaveWorldAfter=<s> calls the same UVoxelWorldSubsystem::SaveWorld()
	// the voxel.SaveWorld console command uses, logs the result (entries +
	// editedDigest), then self-quits a few seconds later -- same headless-run
	// convenience as ServerDumpDigestTimerHandle above. Works on any
	// authority role (standalone/listen/dedicated server all run
	// GameMode::BeginPlay server-side); SaveWorld() itself already no-ops
	// (with a warning) if this somehow runs on NM_Client.
	FTimerHandle SaveWorldTimerHandle;
	FTimerHandle SaveWorldQuitTimerHandle;

	// W2 verification (task item 5a): -VoxelSpawnWaterTest[=<delaySeconds>]
	// pours a water pool near spawn, then logs a settle-check (active
	// bricks/volume/digest) ~15s later -- combine with
	// -VoxelScreenshotAfter=<seconds> (a larger value) for the visual.
	FTimerHandle SpawnWaterTestTimerHandle;
	FTimerHandle SpawnWaterTestSettleTimerHandle;

	// --- Water pool parity anchor -------------------------------------------
	// -VoxelWaterParityTest[=<delaySeconds>]: a SURFACE pour at a flat anchor,
	// posed and captured identically every run, so that `voxel.Water.GPU` 0 vs
	// 1 can be A/B'd against a same-path noise floor.
	//
	// WHY THIS EXISTS RATHER THAN REUSING -VoxelFloodTest. That anchor cannot
	// support the comparison: same-path repeat runs there differ by 20.4-87.7%
	// of pixels at >8/255, against a pooled-vs-component difference of
	// 28.6-79.2% -- the ranges overlap completely, in both directions, so the
	// instrument says nothing (docs/gpu-water-pool-design.md, "What was NOT
	// established"). The variance is UNDERGROUND TERRAIN-CHUNK RESIDENCY, not
	// the water (docs/status.md C7/C8 gotchas 3 and 4).
	//
	// So the whole design of this fixture is removing that one variable:
	// pose on the surface, then WAIT FOR TERRAIN RESIDENCY TO GO QUIET BEFORE
	// ANY WATER EXISTS, and only then pour. The settle wait is not a
	// convenience, it IS the instrument -- a run that captures while chunks are
	// still streaming is measuring streaming, not water.
	FTimerHandle WaterParityPoseTimerHandle;
	FTimerHandle WaterParityTerrainPollTimerHandle;
	FTimerHandle WaterParityWaterPollTimerHandle;
	FTimerHandle WaterParityShotTimerHandle;
	FTimerDelegate WaterParityTerrainPollDelegate;
	FTimerDelegate WaterParityWaterPollDelegate;
	int32 WaterParityTerrainPolls = 0;
	int32 WaterParityWaterPolls = 0;
	FVector WaterParityCameraUU = FVector::ZeroVector;
	FRotator WaterParityCameraRot = FRotator::ZeroRotator;

	// ADR-0005 water persistence verification (docs/adr/0005-water-persistence.md):
	// -VoxelWaterPersistTest[=<delaySeconds>] pours a pool (and best-effort drains
	// a flooded cavern) near spawn, lets it settle, then SaveWaterState()s the
	// blob and runs an in-process disk round-trip (VerifyWaterDiskRoundTrip) that
	// reloads the actual .vxwater file into a FRESH CA/mobilizer and asserts the
	// digest/volume/mobilized-count match -- proving the UE save/load wiring, not
	// just the serializer's own unit tests. Self-quits so a headless save run
	// exits on its own, leaving the blob on disk for a cross-process reload.
	// -VoxelWaterLoadCheck[=<delaySeconds>] is that second half: on a re-launch
	// with the same seed it logs the water state the OnWorldBeginPlay load path
	// restored from disk (volume/digest/mobilized) and quits -- the genuine
	// reload, proving a drained cavern stays drained across processes.
	FTimerHandle WaterPersistTestPourTimerHandle;
	FTimerHandle WaterPersistTestSaveTimerHandle;
	FTimerHandle WaterPersistTestQuitTimerHandle;
	FTimerHandle WaterLoadCheckTimerHandle;
	FTimerHandle WaterLoadCheckQuitTimerHandle;

	// W2 verification (task item 5b): -VoxelBreachTest[=<delaySeconds>]
	// scans for a below-sea-level column near spawn on BeginPlay (logged),
	// then carves a crater there once the delay elapses, seeding a Reservoir
	// v0 breach -- settle-check logged ~15s after the carve, same shape as
	// the spawn-water test above.
	FTimerHandle BreachTestTimerHandle;
	FTimerHandle BreachTestSettleTimerHandle;

	// ADR-0003 item 3 verification (docs/adr/0003-hydrostatic-persistent-body.md
	// "item 2 resolution"): -VoxelWaterMemoTest[=<delaySeconds>] carves a
	// basin, settles a water pool in it, then runs dig/place/carve/collapse
	// edits beneath and around it, logging the water digest after every step
	// plus one final summary line -- the cross-process A/B this ADR's proof
	// is built from (run once with -ExecCmds="voxel.Water.SolidCacheEnabled 0",
	// once with 1, same seed, diff the FINAL line's waterDigest). See the .cpp
	// for the exact scenario and docs/status.md's "Water edit-notification
	// completeness + memo enablement" entry for the result. Every stage after
	// the first uses a throwaway local FTimerHandle (fire-and-forget, never
	// cancelled) -- only the entry point needs a member.
	FTimerHandle WaterMemoTestTimerHandle;
	bool bWaterMemoTestActive = false;
	double WaterMemoTestBasinXUU = 0.0;
	double WaterMemoTestBasinYUU = 0.0;

	// M5 destruction (first slice, docs/m4-plan.md Round 2): -VoxelTreeTest
	// places a stand-in tree FIXTURE near spawn; -VoxelChopTest (implies
	// -VoxelTreeTest) then carves through its trunk after the tree has settled,
	// severing the canopy so it detaches and falls as cosmetic debris. The
	// screenshot framing (below, in the -VoxelScreenshotAfter block) aims at
	// the tree column when either switch is active.
	FTimerHandle TreeTestTimerHandle;
	FTimerHandle ChopTestTimerHandle;
	bool bTreeTestActive = false;
	double TreeTestColumnXUU = 0.0;
	double TreeTestColumnYUU = 0.0;

	// M5 LARGE-EDIT structural collapse (docs/status.md "Structural collapse
	// (M5, large-edit)"): -VoxelStructureTest places a wall+roof+far-pillars
	// FIXTURE near spawn; -VoxelCollapseTest (implies -VoxelStructureTest) then
	// fires one large CarveSphere through the far pillars, so the roof loses its
	// support while STILL being connected to the ground via the wall -- the case
	// island detection structurally cannot answer. Screenshot framing below aims
	// broadside at the structure when either switch is active.
	FTimerHandle StructureTestTimerHandle;
	FTimerHandle CollapseTestTimerHandle;
	FTimerHandle CollapseTestSettleTimerHandle;
	bool bStructureTestActive = false;
	double StructureTestColumnXUU = 0.0;
	double StructureTestColumnYUU = 0.0;

	// M6 NPC swarm verification (docs/status.md M6 section): -VoxelSwarmTest[=<N>]
	// spawns the pursuit swarm a couple of seconds after BeginPlay -- see the
	// .cpp for why this doesn't need to wait for render streaming the way
	// -VoxelHeadlessDigTest/-VoxelTreeTest do.
	FTimerHandle SwarmTestTimerHandle;

	// M6 digging-while-pathing verification (docs/status.md M6 section
	// "Digging-while-pathing"): -VoxelDigSwarmTest[=<N>] builds a
	// deliberate rock wall a few meters ahead of the player, then spawns N
	// agents on the FAR side of it (UVoxelAgentSubsystem::SpawnSwarmAtOffset)
	// so the only short route back to the player crosses the wall -- see the
	// .cpp for the wall/spawn geometry and why it's sized against the Tier 0
	// search window.
	FTimerHandle DigSwarmTestTimerHandle;
	// Set once the wall is actually built (inside DigSwarmTestTimerHandle's
	// callback, so these reflect the REAL build column, not just the parsed
	// spawn arg) -- read by the -VoxelScreenshotAfter framing block (mirrors
	// bTreeTestActive/TreeTestColumnXUU/YUU's identical shape) so a combined
	// run captures the wall/tunnel instead of the generic oblique default.
	bool bDigSwarmTestActive = false;
	double DigSwarmTestColumnXUU = 0.0;
	double DigSwarmTestColumnYUU = 0.0;

	// M6 gap closure verification (docs/status.md M6 section "Tier-1
	// hierarchical planning + NPC replication"): -VoxelTier1RegionGraphTest[=<N>]
	// builds a short wall INSIDE UVoxelAgentSubsystem::Tier1GraphHorizontalRadius
	// Regions' coverage (unlike -VoxelDigSwarmTest's wall, which sits right
	// next to the player and is meant for Tier 0's dig-while-pathing), spawns
	// N Tier-1-range agents on the far side of it, then (after they've had
	// time to detour around it) DIGS A GAP in the wall's center -- proving
	// dirty invalidation: the next natural Tier 1 replan should route through
	// the new opening instead of continuing the old detour. See the .cpp for
	// the exact geometry/timing and watch for "VoxelAgent N (Tier1
	// hierarchical): corridor cost=..." log lines before/after the gap.
	FTimerHandle Tier1RegionGraphTestTimerHandle;
	FTimerHandle Tier1RegionGraphGapTimerHandle;
	bool bTier1RegionGraphTestActive = false;
	double Tier1RegionGraphTestColumnXUU = 0.0;
	double Tier1RegionGraphTestColumnYUU = 0.0;

	// Underground streaming proof (docs/status.md "Underground streaming
	// (vertical footprint)"): -VoxelUndergroundTest[=<delaySeconds>] carves a
	// vertical shaft from the spawn column down to UndergroundTestDepthUU, a
	// short horizontal tunnel off its foot, and a chamber at the tunnel's end,
	// then parks the pawn underground -- the shot that was impossible before,
	// because the streaming footprint only meshed a band around the SURFACE.
	// Combine with -VoxelScreenshotAfter=<a larger value> to capture it, and
	// -VoxelUndergroundView=shaft to frame the shaft/tunnel instead of the
	// chamber. Same one-shot-timer + column-cache shape as the tree/structure
	// fixtures above.
	FTimerHandle UndergroundTestTimerHandle;
	bool bUndergroundTestActive = false;
	double UndergroundTestColumnXUU = 0.0;
	double UndergroundTestColumnYUU = 0.0;
	double UndergroundTestSurfaceUU = 0.0;
	// Depth of the chamber floor below the surface. 24m is chosen to sit BELOW
	// the unconditional depth skirt (~19.2m of level-0 chunks), so this fixture
	// exercises the anchor-relative deep box and not just the skirt.
	double UndergroundTestDepthUU = 2400.0;
	// Where the pawn is parked for the capture, and which way it looks. Two
	// framings, selected by -VoxelUndergroundView: "chamber" (default) stands
	// in the chamber looking back up the tunnel -- rock on every side; "shaft"
	// stands at the shaft foot looking along the tunnel, so the shot shows a
	// dug passage with walls and a floor. Both read UndergroundTestSurfaceUU,
	// so they are only valid after the carve timer has run.
	FVector UndergroundTestCameraLocation() const;
	FRotator UndergroundTestCameraRotation() const;
	bool IsUndergroundShaftView() const;
	// -VoxelUndergroundView=tunnel: on-axis corridor framing (see the .cpp for
	// why the other two poses land against the tunnel roof).
	bool IsUndergroundTunnelView() const;

	// -VoxelCaveTest[=<delaySeconds>]: searches the columns around spawn for a
	// genuine M4 cave void (voxelcore/caves.h -- pristine worldgen, NOT an
	// edit), parks the pawn inside the tallest one it finds and logs a
	// six-axis enclosure probe. This is the real proof that the world exists
	// below the surface: unlike the dug-chamber fixture, a cave is unedited,
	// so nothing forces its chunks to be meshed -- they are there only if the
	// streaming footprint reaches them. Pair with -VoxelScreenshotAfter.
	FTimerHandle CaveTestTimerHandle;
	// Like -VoxelGICaveTest, the fixture now owns its own settle/pose/capture/
	// quit schedule instead of depending on -VoxelScreenshotAfter: the pawn
	// teleports tens of metres underground and the deep streaming footprint has
	// to be recomputed and meshed from scratch, which does not happen in the
	// same frame the teleport does.
	FTimerHandle CavePoseTimerHandle;
	FTimerHandle CaveRepose1TimerHandle;
	FTimerHandle CaveRepose2TimerHandle;
	FTimerHandle CaveShotTimerHandle;
	FTimerHandle CaveQuitTimerHandle;
	bool bCaveTestActive = false;
	bool bCaveTestFound = false;
	bool bCaveTestSelfCapture = false;
	FVector CaveTestCameraPos = FVector::ZeroVector;
	FRotator CaveTestCameraRot = FRotator::ZeroRotator;
	// Scans for the tallest air pocket in the cave depth band around
	// (OriginXUU, OriginYUU) that a camera actually fits inside; returns false
	// if nothing qualifying was found. OutFloorZUU is the top of the solid floor
	// under the void, which is what the camera height is measured from.
	bool FindCaveVoid(class UVoxelWorldSubsystem& Subsystem, double OriginXUU, double OriginYUU, FVector& OutCenter,
	                  double& OutFloorZUU) const;
	// Poses the pawn at the cave camera. Idempotent, and re-asserted on several
	// timers, because a single SetActorLocation is not enough (see the .cpp).
	void PoseInCaveTest() const;

	// -VoxelGICaveTest: locate a pristine worldgen sinkhole (a column with
	// continuous air from the surface into the cave band) and report the cave
	// floor the daylight lands on.
	bool FindSinkholeColumn(class UVoxelWorldSubsystem& Subsystem, double OriginXUU, double OriginYUU,
	                        FVector& OutShaftBaseUU, double& OutSurfaceUU) const;
	// Logs the tracked/component/quad state of the level-0 chunks in a vertical
	// stack through Center -- the measurement that distinguishes "nothing is
	// streamed down here" from "it is streamed but renders oddly".
	void LogUndergroundChunkStatus(class UVoxelWorldSubsystem& Subsystem, const FVector& Center, const TCHAR* Phase) const;
};

// -VoxelSpawnAt=X,Y (meters, world) -> world UU. See the note above the UCLASS
// for why this is no longer file-local to VoxelEarthGameMode.cpp; the definition
// still lives there and there is still exactly one.
namespace VoxelEarthSpawn
{
	VOXELEARTH_API bool ParseSpawnColumnUU(double& OutWorldX, double& OutWorldY);
}

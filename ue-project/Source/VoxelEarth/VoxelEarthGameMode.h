#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VoxelEarthGameMode.generated.h"

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
	virtual void BeginPlay() override;

private:
	FTimerHandle ScreenshotTimerHandle;
	FTimerHandle SecondShotTimerHandle;
	FTimerHandle QuitTimerHandle;

	// M2 wave 2 item 2 verification (docs/m2-plan.md "Distant-edit mip
	// propagation"): -VoxelHeadlessDigTest fires a large CarveSphere at the
	// spawn column once R0/R1 have had time to settle, so a headless -game
	// run can prove (via log lines + a screenshot) that ring chunks over the
	// dig site actually re-mesh through the overlay-aware path.
	FTimerHandle HeadlessDigTestTimerHandle;

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
};

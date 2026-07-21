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
};

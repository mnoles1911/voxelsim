#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelAgent.h" // FVoxelAgent, EVoxelAgentTier -- UE-only, voxel-core-free
#include "VoxelAgentSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UVoxelWorldSubsystem;

// voxel-core owns the actual pathfinding search (vxc::findPath,
// voxelcore/pathfind.h). Kept behind a PImpl so this UHT-parsed header
// never includes a voxel-core header -- same doctrine/idiom as
// UVoxelWorldSubsystem's FVoxelWorldImpl / UVoxelWaterSubsystem's
// FVoxelWaterImpl (see either header's identical doc comment). Defined only
// in VoxelAgentSubsystem.cpp: owns, per agent (parallel array, same index
// as Agents below), the full vxc::PathResult Tier 0/1 revalidate against
// (pathStillValid) and replan from, plus TWO shared vxc::PathCostConfigs
// (digging-while-pathing, docs/status.md M6 section) -- DigConfig (Mine/
// Bridge enabled, Tier 0 only) and NavConfig (Mine/Bridge effectively
// disabled, exactly the old v0 single-config setup, Tier 1 -- see
// PlanPath's tier dispatch).
struct FVoxelAgentImpl;

// M6 NPC swarm (docs/voxel-earth-implementation-plan.md SS3.6 "NPCs & AI
// (simulation LOD)"). Digging-while-pathing (docs/status.md M6 section
// "Digging-while-pathing"): Tier 0 agents plan with Mine/Bridge ENABLED
// (FVoxelAgentImpl::DigConfig, VoxelAgentSubsystem.cpp) and execute those
// steps through the SAME authoritative edit-log path player digs use
// (UVoxelWorldSubsystem::TryDig/TryPlace -- see TryExecuteWaypointEdit's doc
// comment for exactly how a point-target Mine/Bridge step is expressed as
// one of those raycast-based calls). Tier 1/2 remain navigation-only
// (FVoxelAgentImpl::NavConfig, Mine/Bridge priced exactly as the old v0
// single-config setup did) -- see "Rate limiting -- tier restriction" below
// for why digging is Tier-0-exclusive in this slice, not "Tier 0 and 1" as
// the plan doctrine's phrasing allows.
//
// Owns a POOLED, instance-rendered agent swarm (see VoxelAgent.h's class
// comment for why: hundreds of AActors would be far too heavy) and runs the
// Tier 0/1/2 LOD scheduler every tick, keyed on distance-to-player with
// hysteresis (VoxelAgent.h's ComputeNextVoxelAgentTier):
//   Tier 0 (near, ~Tier0EnterUU/ExitUU band) -- full windowed vxc::findPath,
//     replanned whenever the current path is exhausted, invalid
//     (pathStillValid fails), or the player's goal has drifted
//     ReplanGoalDriftThresholdUU away from where the path was planned to.
//     Smooth per-tick movement along the step sequence, ground-snapped via
//     UVoxelWorldSubsystem::RaycastVoxelWorld every tick (mirrors
//     AVoxelDebris' settle style).
//   Tier 1 (mid) -- the SAME vxc::findPath call, just on a coarser cadence:
//     replans (and the pathStillValid recheck that can trigger one) are
//     additionally gated by Tier1ReplanIntervalSeconds, and the ground-snap
//     raycast runs on the much coarser Tier1GroundSnapIntervalSeconds
//     instead of every tick. // TODO(M6): swap Tier 1's findPath call for a
//     hierarchical regiongraph.h lookup once that layer lands (a separate
//     track is building it; NOT merged yet, so this subsystem does not
//     depend on it -- see docs/status.md M6 section).
//   Tier 2 (far) -- no A* at all: bounded steering straight toward the
//     player (VoxelAgent.h's SteerVoxelAgentTier2) plus a small per-agent
//     wander, ground-snapped only every Tier2GroundSnapIntervalSeconds.
//     This is what makes hundreds of agents affordable.
// Tier 0 replans additionally share a per-tick BUDGET (MaxTier0ReplansPerTick)
// so a big swarm converging at once can never blow a frame on pathfinding
// -- an agent whose replan is due but the budget is spent simply waits
// (still moves along its old path, or idles at its path's end) until a
// future tick's budget covers it.
//
// Server-authoritative (plan doctrine SS2): SpawnSwarm/Tick's simulation
// step only ever runs on the authority (checked via UWorld::GetNetMode() !=
// NM_Client, mirroring UVoxelWaterSubsystem's identical role-split
// doctrine) -- spawning, target selection, and every path decision are
// authority-only. KNOWN v0 SCOPE LIMIT (documented, not a bug): this slice
// does NOT replicate agent state to remote clients (no FastArraySerializer/
// RPC wiring here -- VoxelEditRelay.h is outside this slice's owned-files
// list) -- every verification run in this slice is NM_Standalone. Real
// cross-network agent replication is a follow-up, left for whoever wires
// swarm state into AVoxelEditRelay (see docs/status.md's M6 subsection for
// this run). A dedicated server (no viewport) still runs the full
// simulation -- only the cosmetic AgentISM is skipped (see
// OnWorldBeginPlay), same "sim always, render never" split
// UVoxelWorldSubsystem's chunk streaming uses for NM_DedicatedServer.
UCLASS()
class VOXELEARTH_API UVoxelAgentSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelAgentSubsystem();
	// See UVoxelWorldSubsystem's identical destructor/FVTableHelper doc
	// comment: TUniquePtr<FVoxelAgentImpl>'s destructor needs
	// FVoxelAgentImpl's full definition, which this UHT-parsed header must
	// not see.
	virtual ~UVoxelAgentSubsystem() override;
	UVoxelAgentSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

	// -VoxelSwarmTest=<N> (AVoxelEarthGameMode): spawns Count agents ringed
	// around the FIRST player controller's pawn (random angle,
	// SpawnRingInnerUU..SpawnRingOuterUU radius so the initial swarm spans
	// all three tiers -- see class comment), ground-placed via
	// UVoxelWorldSubsystem::GetSurfaceHeightUU (a pure query, safe before
	// any chunk has streamed in). Authority only (role-split doctrine
	// above): a no-op (logged, returns 0) on NM_Client or if no player pawn
	// exists yet. Returns the number of agents actually spawned.
	int32 SpawnSwarm(int32 Count);

	// -VoxelDigSwarmTest=<N> (AVoxelEarthGameMode, docs/status.md M6
	// "Digging-while-pathing" section): a DETERMINISTIC variant of SpawnSwarm
	// for that verification scenario -- SpawnSwarm's random 360-degree ring
	// can't guarantee every agent starts on one particular side of a
	// deliberately-built obstacle, which the decisive "only route is through
	// the wall" demo needs. Places Count agents at CenterWorldPos +
	// Offset.GetSafeNormal() * OffsetRadiusUU, each additionally jittered
	// +-LateralJitterUU along the axis perpendicular to Offset (XY plane
	// only) so the cluster doesn't spawn as a single stacked point. Ground-
	// placed the same way as SpawnSwarm (GetSurfaceHeightUU); same authority-
	// only refusal on NM_Client / no terrain subsystem. Returns the number of
	// agents actually spawned.
	int32 SpawnSwarmAtOffset(int32 Count, const FVector& CenterWorldPos, const FVector& Offset, double OffsetRadiusUU,
	                          double LateralJitterUU);

	int32 GetAgentCount() const { return Agents.Num(); }

	// docs/debug-tooling-plan.md-style snapshot (same 1Hz-ish refresh shape
	// as UVoxelWorldSubsystem::GetPerfSnapshot): tier counts + the
	// convergence metric (mean distance-to-player across the whole pool),
	// refreshed every ConvergenceLogIntervalSeconds by Tick and also logged
	// there -- -VoxelSwarmTest verification reads this from the log, not
	// this getter, but it is exposed for any future HUD row.
	struct FSwarmSnapshot
	{
		int32 Tier0Count = 0;
		int32 Tier1Count = 0;
		int32 Tier2Count = 0;
		double MeanDistanceToPlayerUU = 0.0;
	};
	FSwarmSnapshot GetLastSwarmSnapshot() const { return LastSnapshot; }

	// --- LOD scheduler tuning (design documented in the class comment above) ---

	// Tier 0/1 hysteresis band (VoxelAgent.h's ComputeNextVoxelAgentTier):
	// entering Tier 0 needs distance < Tier0EnterUU (15m); leaving it back
	// to Tier 1 needs distance > Tier0ExitUU (20m).
	static constexpr double Tier0EnterUU = 1500.0;
	static constexpr double Tier0ExitUU = 2000.0;
	// Tier 1/2 hysteresis band: entering Tier 1 (from Tier 2) needs
	// distance < Tier1EnterUU (80m); leaving to Tier 2 needs distance >
	// Tier1ExitUU (100m).
	static constexpr double Tier1EnterUU = 8000.0;
	static constexpr double Tier1ExitUU = 10000.0;

	// Pursuit standoff (class comment "Pursuit behavior"): an agent within
	// StandoffRadiusUU (2.5m) of the player stops advancing; it only
	// resumes once the player is back beyond StandoffResumeUU (4m) --
	// same hysteresis shape as the tier bands, so an agent parked at the
	// standoff boundary doesn't twitch in and out of idle every tick.
	static constexpr double StandoffRadiusUU = 250.0;
	static constexpr double StandoffResumeUU = 400.0;

	// Uniform physical move speed across all three tiers (~3.8 m/s,
	// human-walk pace) -- the LOD tiers differ in DECISION cost (how the
	// path/steering direction is computed and how often), never in raw
	// motion speed; see class comment.
	static constexpr double MoveSpeedUUPerSec = 380.0;
	// XY distance to a waypoint at which AdvanceAlongWaypoints considers it
	// "reached" and moves on to the next one (1.5 voxels).
	static constexpr double WaypointAdvanceThresholdUU = 15.0;

	// Tier 0/1 windowed vxc::SearchWindow half-extents, in VOXELS, centered
	// on the agent's own current voxel each replan (NOT sized to reach the
	// player directly -- see PlanPath's doc comment for why a small window
	// plus repeated incremental replanning, rather than one huge window, is
	// the "local windowed voxel A*" the plan doctrine actually calls for).
	// 33x33x25 voxels = ~27k cells/search, cheap enough for
	// MaxTier0ReplansPerTick calls every tick without a frame spike.
	static constexpr int64 WindowHalfExtentVoxels = 16;
	static constexpr int64 WindowHalfHeightVoxels = 12;
	static constexpr int32 MaxExpansionsPerSearch = 8000;

	// Force a replan once the (ground-projected) player goal has moved this
	// far (3m) from the goal the current path was planned toward, even if
	// the path itself isn't exhausted yet -- keeps a Tier 0 agent from
	// doggedly finishing a path toward a spot the player left long ago.
	static constexpr double ReplanGoalDriftThresholdUU = 300.0;

	// Replan budget (class comment): at most this many vxc::findPath calls
	// (Tier 0 AND Tier 1 combined, Tier 0 claims its share first) per
	// subsystem Tick -- bounds worst-case per-frame pathfinding cost
	// regardless of how many agents want to replan at once.
	static constexpr int32 MaxTier0ReplansPerTick = 6;

	// Tier 1's coarser replan cadence (class comment): a Tier 1 agent only
	// even ATTEMPTS a replan once this many seconds have passed since its
	// last one (unless its path is exhausted/invalid, which still forces
	// one, budget permitting).
	static constexpr double Tier1ReplanIntervalSeconds = 2.5;

	// Ground-snap raycast cadence (seconds), per tier -- Tier 0 snaps every
	// tick (no interval check at all); Tier 1/2 gate the raycast on
	// FVoxelAgent::LastGroundSnapTimeSeconds vs. these intervals instead,
	// coarser the farther the tier (doc comment "Tier 1 ... lower movement
	// update rate" / "Tier 2 ... updated infrequently"). Since every
	// agent's timestamp starts effectively unset and then desyncs at its
	// own cadence, per-tier raycast load naturally spreads across ticks
	// after the first one rather than bursting every Nth frame in lockstep.
	static constexpr double Tier1GroundSnapIntervalSeconds = 0.5;
	static constexpr double Tier2GroundSnapIntervalSeconds = 1.5;
	static constexpr double Tier2WanderAmplitudeUUPerSec = 60.0;

	// Half the agent body's visual height (0.9m => 1.8m-tall body) -- the
	// vertical offset from FVoxelAgent::Position (feet) to the ISM
	// instance's transform origin (body center).
	static constexpr double AgentHalfHeightUU = 90.0;
	static constexpr double AgentBodyWidthUU = 40.0;

	static constexpr double ConvergenceLogIntervalSeconds = 2.0;

	// -VoxelSwarmTest spawn ring (SpawnSwarm's doc comment): spans all
	// three tiers on spawn (10m-150m) so a headless run's tier-count log
	// exercises the whole scheduler, not just Tier 0.
	static constexpr double SpawnRingInnerUU = 1000.0;
	static constexpr double SpawnRingOuterUU = 15000.0;
	static constexpr int32 DefaultSwarmCount = 200;

	// --- Digging-while-pathing rate limits (docs/status.md M6 section) ---
	//
	// A swarm of hundreds of agents each mining every tick would vaporize
	// the world and swamp the edit log/replication -- three independent
	// limits, all of which must hold simultaneously:
	//
	// 1. Per-agent cooldown: an agent that just successfully dug/placed one
	//    voxel cannot attempt another for this many seconds (FVoxelAgent::
	//    LastDigTimeSeconds) -- "mining costs hardness x TIME", not an
	//    instant swap, matching pathfind.h's own cost-function doctrine
	//    comment. At MoveSpeedUUPerSec (3.8 m/s) this reads as a believable
	//    "the agent visibly pauses, chewing through the rock" beat per
	//    voxel rather than a teleport-through-solid-matter glitch.
	static constexpr double NPCDigCooldownSeconds = 1.5;
	// 2. Global per-tick cap: at most this many NPC edits (Mine+Bridge
	//    combined, across the WHOLE swarm) land per UVoxelAgentSubsystem::
	//    Tick, regardless of how many Tier 0 agents are simultaneously off
	//    cooldown and want to dig -- the actual bound on worst-case per-tick
	//    edit-log growth from this subsystem. Sized well above what a
	//    LEGIBLE demo swarm (5-20 agents, 1.5s cooldown each) can ever hit in
	//    practice, but far below what hundreds of agents converging on a
	//    single wall at once could otherwise do without it.
	static constexpr int32 MaxNPCEditsPerTick = 4;
	// 3. Tier restriction (class comment "Rate limiting -- tier
	//    restriction"): ONLY Tier 0 agents ever execute an edit -- Tier 1/2
	//    plan (Tier 1) or steer (Tier 2) through FVoxelAgentImpl::NavConfig,
	//    which prices Mine/Bridge exactly as effectively-disabled as the
	//    pre-M6 single-config setup did, so their paths/steering never
	//    require an edit in the first place. This is deliberately narrower
	//    than "Tier 0 and optionally Tier 1": a Tier 1 agent's path is
	//    reused across a much coarser replan cadence (Tier1ReplanIntervalSeconds)
	//    and would otherwise carry a Mine/Bridge step (planned once, stale
	//    for up to that whole interval) into ticks where AdvanceAlongWaypoints
	//    -- which has NO collision/edit awareness of its own -- would walk
	//    the agent straight through still-solid terrain waiting for an edit
	//    that Tier 1 never attempts. Keeping digging Tier-0-only sidesteps
	//    that failure mode entirely rather than having to solve it.

	// Placeholder solid material a Bridge action's placed scaffold voxel
	// uses (UVoxelWorldSubsystem::TryPlace's MaterialId parameter) -- no
	// distinct "scaffold" material exists in vxc::Material yet (voxelcore/
	// core.h), so this reuses MAT_ROCK, the same placeholder-material
	// precedent UVoxelWorldSubsystem::SpawnTreeFixtureAt already set for a
	// hand-authored fixture that isn't real world content either.
	static constexpr uint8 BridgeScaffoldMaterialId = 2; // vxc::MAT_ROCK (voxelcore/core.h) -- see above

private:
	// Per-tier per-agent update. All three share the same signature shape
	// (agent pool index, the terrain subsystem, live player state, dt, and
	// -- Tier 0/1 only -- the shared replan budget) even though Tier 2
	// ignores several of them -- keeps Tick's per-agent dispatch a single
	// readable switch.
	// (All three fetch GetWorld()->GetTimeSeconds() internally where needed
	// -- e.g. Tier 1's replan-interval gate, Tier 2's wander phase -- rather
	// than threading a timestamp through every call.)
	// TickTier0 additionally threads InOutEditBudget (MaxNPCEditsPerTick's
	// live counter, shared across every Tier 0 agent this Tick -- see "Rate
	// limiting" above) alongside the existing replan budget; TickTier1/2
	// never dig (tier restriction above) so they don't need it.
	void TickTier0(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
	               const FVector& GoalWorldPos, double DeltaSeconds, int32& InOutReplanBudget, int32& InOutEditBudget);
	void TickTier1(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
	               const FVector& GoalWorldPos, double DeltaSeconds, int32& InOutReplanBudget);
	void TickTier2(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
	               double DeltaSeconds);

	// Runs one windowed vxc::findPath from the agent's current voxel toward
	// GoalWorldPos and, on success, rewrites Agents[AgentIndex].Waypoints +
	// resets WaypointIndex/LastReplanTimeSeconds/LastPlanGoalWorld. Returns
	// false (no state changed) if the agent's start voxel isn't inside the
	// search window (should not happen in practice -- window is centered on
	// the agent) or the search finds literally zero steps (agent already at
	// the reached cell). Digging-while-pathing (docs/status.md M6 section):
	// which of FVoxelAgentImpl::DigConfig/NavConfig this call uses is
	// selected purely from Agents[AgentIndex].Tier (already set by Tick's
	// dispatch before either TickTier0/1 runs) -- Tier 0 gets DigConfig
	// (Mine/Bridge enabled), everything else gets NavConfig. Also records,
	// per agent, whether the freshly-planned path IS dig-capable
	// (FVoxelAgentImpl::PathIsDigCapable) -- TickTier1 forces an immediate
	// replan the moment it inherits a dig-capable path from a just-demoted
	// Tier 0 agent, rather than trying (and failing) to walk a Mine/Bridge
	// step it will never execute -- see TickTier1's doc comment.
	bool PlanPath(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& GoalWorldPos);

	// Digging-while-pathing execution (docs/status.md M6 section): if the
	// step Agents[AgentIndex] is currently walking toward
	// (Impl->Paths[AgentIndex].steps[Agent.WaypointIndex]) is a Mine or
	// Bridge action, attempts the corresponding authoritative edit through
	// UVoxelWorldSubsystem::TryDig/TryPlace -- see the .cpp for exactly how
	// a point-target voxel edit is expressed as one of those raycast-based
	// calls. Rate-limited (NPCDigCooldownSeconds, InOutEditBudget -- see
	// "Rate limiting" above) and gated by the voxel.NPCDig.Enabled safety
	// valve cvar. Returns true if the agent is clear to advance toward this
	// waypoint THIS tick (the step isn't Mine/Bridge at all, the edit was
	// already applied by a previous tick/another agent, or an edit was just
	// successfully applied this call); false if an edit is still pending
	// (cooldown/budget/cvar-off/no-valid-placement-face) and the agent must
	// hold its current position rather than advance into still-solid (Mine)
	// or still-unsupported (Bridge) terrain.
	bool TryExecuteWaypointEdit(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
	                             int32& InOutEditBudget);

	// Shared per-agent construction (ground-placement query + FVoxelAgent
	// pool append + Impl->Paths/PathIsDigCapable append-in-lockstep + ISM
	// instance creation) -- factored out of SpawnSwarm so SpawnSwarmAtOffset
	// (a different WorldX/WorldY generation policy, same everything-after)
	// doesn't duplicate it. Returns the new agent's pool index.
	int32 SpawnOneAgentAt(double WorldX, double WorldY, UVoxelWorldSubsystem& Terrain);

	// Tier-aware ground-snap: no-ops (cheaply, just a timestamp compare)
	// unless this agent's tier-specific cadence has elapsed since its last
	// snap (Tier 0: every call; Tier 1/2: Tier1/Tier2GroundSnapIntervalSeconds
	// -- see those constants). When due, casts straight down from the agent
	// (mirrors AVoxelDebris::Tick's settle raycast) and snaps Position.Z to
	// the hit surface's top face (feet rest AT the surface); leaves the
	// previous Z untouched if nothing solid is found within a generous
	// range. Safe/cheap to call unconditionally every tick for every agent
	// regardless of tier.
	void GroundSnap(int32 AgentIndex, UVoxelWorldSubsystem& Terrain);

	// Pushes Agents[AgentIndex]'s current Position/Tier into its ISM
	// instance transform (no-op if AgentISM is null, i.e. dedicated
	// server). Does NOT call MarkRenderStateDirty -- Tick does that once,
	// after every agent this frame has been updated.
	void UpdateInstanceTransform(int32 AgentIndex) const;

	TArray<FVoxelAgent> Agents;
	TUniquePtr<FVoxelAgentImpl> Impl;

	// Owns AgentISM (mirrors UVoxelWorldSubsystem's ChunkOwner/ChunkRoot
	// pattern: one plain actor hosting the render component). Left null on
	// NM_DedicatedServer -- see OnWorldBeginPlay.
	UPROPERTY(Transient)
	TObjectPtr<AActor> SwarmOwner;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> AgentISM;

	FSwarmSnapshot LastSnapshot;
	double LastConvergenceLogTimeSeconds = -1.0;
};

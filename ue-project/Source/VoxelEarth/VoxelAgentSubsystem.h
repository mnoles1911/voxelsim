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
//     instead of every tick. M6 GAP CLOSURE (docs/status.md M6 section
//     "Tier-1 hierarchical planning + NPC replication"): Tier 1 now plans
//     via PlanPathTier1 -- vxc::findHierarchicalPath over a region graph
//     (voxelcore/regiongraph.h, now merged) when the graph covers the
//     query, falling back to the ORIGINAL fine windowed PlanPath otherwise.
//     See the Tier1Graph* constants' doc comment below for the graph's
//     extent/lifetime design and PlanPathTier1's doc comment for the
//     refine/fallback logic.
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
// Server-authoritative (plan doctrine SS2): SpawnSwarm/Tick's SIMULATION
// step only ever runs on the authority (checked via UWorld::GetNetMode() !=
// NM_Client, mirroring UVoxelWaterSubsystem's identical role-split
// doctrine) -- spawning, target selection, and every path/dig decision are
// authority-only, unconditionally, even after the M6 gap closure below. A
// dedicated server (no viewport) still runs the full simulation -- only the
// cosmetic AgentISM is skipped (see OnWorldBeginPlay), same "sim always,
// render never" split UVoxelWorldSubsystem's chunk streaming uses for
// NM_DedicatedServer.
//
// M6 GAP CLOSURE -- NPC state replication (docs/status.md M6 section
// "Tier-1 hierarchical planning + NPC replication"): a NEW actor,
// AVoxelAgentReplicator (VoxelAgentReplication.h/.cpp), now broadcasts a
// compact, relevancy-filtered snapshot of Tier 0/1 agent positions+tiers to
// every connected client at a fixed 10Hz cadence (see that class's header
// comment for the full rate/batching/compression/relevancy design). A
// client's OWN Agents/Impl stay empty forever (still true -- a client NEVER
// simulates paths or digs); what it renders instead comes from
// ClientAgents, a separate client-only mirror Tick's NM_Client branch
// (TickClientReplicatedAgents) interpolates and pushes into the SAME
// AgentISM. Server-authoritative doctrine is intact either way: a client
// only ever draws what the server told it, never decides anything.
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

	// --- M6 gap closure: NPC state replication (docs/status.md M6 section
	// "Tier-1 hierarchical planning + NPC replication") -----------------------
	//
	// Server-side snapshot builder consumed by AVoxelAgentReplicator
	// (VoxelAgentReplication.h, a new file -- see its class comment for the
	// full replication design) every AVoxelAgentReplicator::BroadcastInterval
	// Seconds. Serializes only agents within RelevancyRadiusUU of OriginWorld
	// into a compact quantized wire format: uint32 count, then per agent
	// int32 AgentId + int16 relative position per axis (UU, relative to
	// OriginWorld -- keeps the wire format LWC-safe: agents can be
	// planet-scale far from the engine origin, but never far from
	// OriginWorld, which the caller re-anchors near the players every
	// broadcast) + uint8 Tier = 11 bytes/agent. Tier 2 agents are NEVER
	// included regardless of distance (see AVoxelAgentReplicator's class
	// comment "Relevancy/culling" for why: they are always farther than
	// Tier1ExitUU from every player, and the least cosmetically important
	// tier already). Read-only; safe to call on any net mode, but only
	// meaningful on the authority -- a client's own Agents pool is always
	// empty (Tick's class comment "KNOWN v0 SCOPE LIMIT"), so this returns
	// zero agents there (harmless, just never called client-side in
	// practice since AVoxelAgentReplicator only broadcasts from HasAuthority()).
	void CollectReplicationSnapshot(const FVector& OriginWorld, double RelevancyRadiusUU, TArray<uint8>& OutBytes) const;

	// Client-side counterpart: parses CollectReplicationSnapshot's wire
	// format and updates ClientAgents (the client-only replicated-state
	// mirror that Tick's NM_Client branch renders from -- see
	// TickClientReplicatedAgents), keyed by AgentId. Called by
	// AVoxelAgentReplicator::MulticastAgentSnapshot_Implementation on every
	// remote (non-authority) instance.
	void ApplyReplicatedAgentSnapshot(const FVector& OriginWorld, const TArray<uint8>& Bytes);

	// Verification aid (docs/status.md M6 section "Tier-1 hierarchical
	// planning + NPC replication"): runs CollectReplicationSnapshot ->
	// ApplyReplicatedAgentSnapshot IN-PROCESS on THIS SAME instance (no
	// actual network hop) and logs, per agent, the authoritative position
	// vs. the resulting ClientAgents entry's position, plus the worst-case
	// delta across the whole pool. This is NOT a substitute for a genuine
	// cross-process replication test -- it proves the SERIALIZE/QUANTIZE/
	// DESERIALIZE/INTERPOLATE-SETUP logic round-trips correctly (wire
	// format, relevancy filter, int16 quantization precision), independent
	// of whatever the OS/network transport between two real processes does
	// or doesn't do. -VoxelReplicationSelfTest (AVoxelEarthGameMode).
	void RunReplicationSelfTest();

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

	// --- Tier 1 hierarchical region-graph planner (M6 gap closure, docs/
	// status.md M6 section "Tier-1 hierarchical planning + NPC replication")
	// -------------------------------------------------------------------
	//
	// Graph lifetime/extent: ONE vxc::RegionGraph (voxelcore/regiongraph.h),
	// owned PIMPL-side (FVoxelAgentImpl::Tier1RegionGraph -- same "voxel-core
	// types never touch this UHT header" doctrine as everything else here),
	// built/rebuilt by EnsureTier1RegionGraph over a box of
	// (2*Tier1GraphHorizontalRadiusRegions+1)^2 * (2*Tier1GraphVerticalRadiusRegions+1)
	// regions centered on the PLAYER's own ground column (re-derived from
	// GetSurfaceHeightUU, matching Tick's own goal-projection philosophy, NOT
	// the player's possibly-airborne Z). At kRegionEdge=16 voxels/region =
	// 160UU = 1.6m/region (voxelcore/regiongraph.h), radius 16 regions
	// horizontal = 2560UU = 25.6m, radius 3 vertical = 480UU = 4.8m -- a
	// deliberately MODEST box (33x33x7 = 7,623 regions), NOT sized to cover
	// the whole Tier 1 catchment (Tier0ExitUU..Tier1ExitUU = 20m..100m):
	// building a region graph is O(regions) portal-detection + O(portals^2)
	// intra-region fine findPath calls per region, and this header's own
	// "Model" doc comment already frames the natural scale of a 16-voxel
	// region as "a multi-hundred-voxel world collapses to a FEW DOZEN
	// regions per axis" -- i.e. tens of meters, not 100m+. A box big enough
	// to blanket the full Tier 1 band would take multiple seconds to
	// (re)build even once, which is not affordable on a hitch that can
	// recur every time the player walks Tier1GraphRebuildTriggerUU. Tier 1
	// agents OUTSIDE this box's coverage (the common case beyond ~25m)
	// gracefully fall back to the SAME fine windowed vxc::findPath Tier 1
	// used pre-M6 (PlanPath, unchanged) -- see PlanPathTier1's doc comment.
	// This is "extend as the player moves" realized as periodic RE-CENTERED
	// REBUILDS (regiongraph.h's RegionGraph has no dynamic-resize API --
	// markRegionDirty only recomputes WITHIN existing bounds), gated by
	// Tier1GraphRebuildTriggerUU so an agent standing still, or the player
	// drifting a few meters, never triggers one -- "don't rebuild the whole
	// graph per frame" is satisfied by rebuilding only on this coarse
	// hysteresis, not never.
	// MEASURED (docs/status.md M6 section): the first cut of this box
	// (horizontal radius 16 / vertical radius 3 = 33x33x7 = 7,623 regions,
	// intra-region searches uncapped at regiongraph.h's own
	// kRegionVolume=4096 default) hung for 95+ seconds on a single
	// buildRegionGraph call and never completed in that headless run -- the
	// real cost driver is O(regions x portals^2) fine findPath CALLS, each
	// allocating dense bookkeeping arrays sized to its (capped) window
	// volume, not raw search complexity. Cut down from that measured
	// failure to a box that actually completes (see the class comment's
	// updated numbers): radius 12 horizontal (19.2m -- just over
	// Tier0EnterUU=15m, the minimum radius for ANY Tier 1 agent's start
	// position to ever fall inside coverage at all) / radius 1 vertical
	// (1.6m -- agents are ground-snapped and stay close to the surface, so a
	// thin slab is sufficient), AND capping every intra-region/entry/exit
	// fine search at Tier1GraphIntraMaxExpansions/Tier1PerRegionMaxExpansions
	// (256, not kRegionVolume's 4096) -- both changes compound (fewer
	// regions x much cheaper per-call allocation) to bring the build back
	// into real-time-affordable territory. KNOWN v0 TRADEOFF: 256 expansions
	// is comfortably above one region's worst-case straight-line distance
	// (~45 cells corner-to-corner) but can still under-connect a region with
	// unusually convoluted internal terrain (a missed intra-region edge,
	// never a wrong one) -- this only ever shows up as an extra fine-search
	// fallback, never incorrect movement (see PlanPathTier1's fallback path).
	static constexpr int64 Tier1GraphHorizontalRadiusRegions = 12;
	static constexpr int64 Tier1GraphVerticalRadiusRegions = 1;
	// Rebuild once the player's ground column drifts this far (UU) from the
	// graph's cached center -- half the horizontal radius, so the player can
	// cross roughly half the box before a rebuild, not every step.
	static constexpr double Tier1GraphRebuildTriggerUU = 960.0;
	// MEASURED (docs/status.md M6 section): a tight cap (256) built fast
	// (~160s for this box) but corridor-finding NEVER succeeded (0/751 in
	// one headless run) -- entry/exit/intra-region searches inside
	// genuinely rock-dense regions need more than 256 expansions to find a
	// real connection, so they were being truncated as "incomplete" before
	// finding one. The FULL default (kRegionVolume=4096) fixes that but
	// measurably makes the build far slower (still running after 8+
	// minutes at this box size in one attempt -- killed before completion).
	// 1024 is the middle ground actually shipped here: 4x the failing cap's
	// headroom (empirically enough for this terrain -- see docs/status.md's
	// measured numbers), well under the 4096 worst case.
	static constexpr int32 Tier1GraphIntraMaxExpansions = 1024;
	static constexpr int32 Tier1PerRegionMaxExpansions = 1024;

	// --- NPC state replication (M6 gap closure) -------------------------
	// Client-side render-position interpolation window (TickClientReplicated
	// Agents/ApplyReplicatedAgentSnapshot): a fresh snapshot re-anchors a
	// tracked agent's interpolation from wherever its render position
	// currently sits toward the new target over this many seconds. Set to
	// roughly match AVoxelAgentReplicator::BroadcastIntervalSeconds (with a
	// little slack) so the render position finishes easing into one
	// snapshot's target at about the moment the next one arrives -- not
	// coupled by a shared header (VoxelAgentReplication.h depends on THIS
	// header, not vice versa) since exact synchronization isn't required for
	// visual smoothness, only rough agreement.
	static constexpr double ClientAgentInterpolationWindowSeconds = 0.15;

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

	// Tier 1 hierarchical region-graph planner (M6 gap closure -- see the
	// constants' doc comment above for the extent/lifetime design). Builds or
	// rebuilds FVoxelAgentImpl::Tier1RegionGraph, centered on the player's
	// ground column, if the graph doesn't exist yet or the player has
	// drifted more than Tier1GraphRebuildTriggerUU from the cached center.
	// No-op (cheap distance compare) otherwise. Also resets
	// FVoxelAgentImpl::LastSeenEditLogSize to the terrain's CURRENT log size
	// on every (re)build, so PollWorldEditsForTier1DirtyRegions never treats
	// entries the fresh graph already reflects as "new."
	void EnsureTier1RegionGraph(UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos);

	// Tier 1's planner entry point (replaces the raw vxc::findPath call
	// TickTier1 used pre-M6): ensures the region graph is current, then --
	// if the graph covers BOTH the agent's start region and GoalWorldPos's
	// goal region -- calls vxc::findHierarchicalPath(refine=true) and
	// rewrites Agents[AgentIndex].Waypoints/Impl->Paths[AgentIndex] from its
	// concretePath, exactly like PlanPath does for the fine search (see
	// PlanPathTier1's own refinement doc comment in the .cpp for why
	// refine=true, not the coarse-corridor-waypoints option, was chosen).
	// Falls back to the pre-M6 fine windowed PlanPath (unchanged) whenever
	// the graph doesn't cover the query OR findHierarchicalPath itself
	// fails (regions disconnected in the abstract graph, or a stale-graph
	// refinement failure) -- this Tier 1 agent is NEVER worse off than
	// before this slice, only sometimes cheaper. Tracks
	// FVoxelAgentImpl::Tier1Hierarchical*/Tier1FineFallback* call/expansion
	// counters either way, for the before/after cost comparison (docs/
	// status.md M6 section). Returns true iff a path (hierarchical or
	// fallback) was successfully planned.
	bool PlanPathTier1(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos, const FVector& GoalWorldPos);

	// Dirty invalidation, NPC-edit case (precise): called right after
	// TryExecuteWaypointEdit successfully applies a Mine/Bridge edit.
	// Computes the edited voxel's region and, if it falls inside
	// FVoxelAgentImpl::Tier1RegionGraph's current bounds, calls
	// vxc::markRegionDirty on exactly that region -- the graph is always
	// exactly in sync with every NPC-caused edit the instant it lands,
	// since this subsystem is the one making the edit and knows the exact
	// cell. No-op if the graph hasn't been built yet or the cell falls
	// outside its bounds.
	void MarkTerrainEditDirty(UVoxelWorldSubsystem& Terrain, int64 VoxelX, int64 VoxelY, int64 VoxelZ);

	// Dirty invalidation, player-dig/place and M5 structural-collapse case
	// (best-effort -- see the .cpp for the full doctrine-mandated rationale:
	// no delegate exists on UVoxelWorldSubsystem to report which cells an
	// edit touched, and that file is out of scope for this slice to modify).
	// Polls UVoxelWorldSubsystem::GetLogSize() once per Tick (already public,
	// already monotonically counts every applied edit -- player AND NPC AND
	// M5 collapse, since all three go through the same edit-log authority
	// path); whenever it has grown since the last check, dirties the region
	// containing every currently-tracked Tier 0/1 agent's OWN position
	// (deduplicated, so a cluster of agents in one region only dirties it
	// once) -- ONE dirty-scan pass per Tick regardless of how many log
	// entries just landed ("batch per edit" satisfied at Tick granularity,
	// not per raw log entry). No-op if the graph hasn't been built yet.
	void PollWorldEditsForTier1DirtyRegions(UVoxelWorldSubsystem& Terrain);

	// Client-side replicated-agent rendering (Tick's NM_Client branch):
	// interpolates ClientAgents toward each entry's latest replicated
	// snapshot (PrevPosition@PrevTimeSeconds -> TargetPosition@
	// TargetTimeSeconds) and pushes the result into AgentISM -- entirely
	// separate from the authoritative Agents/Impl simulation state (which
	// stays empty on a client -- see Tick's class comment "KNOWN v0 SCOPE
	// LIMIT"). Server-authoritative doctrine intact: a client never decides
	// where an agent goes, only where to DRAW it from what the server told it.
	void TickClientReplicatedAgents(float DeltaSeconds);

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

	// Client-side mirror of one replicated agent's visual state (position +
	// tier only -- no Waypoints/pathfinding data, since a client never
	// simulates path decisions; see class comment "KNOWN v0 SCOPE LIMIT").
	// Interpolated render position: PrevPosition@PrevTimeSeconds ->
	// TargetPosition@TargetTimeSeconds, re-anchored every time a fresh
	// snapshot arrives (ApplyReplicatedAgentSnapshot) so movement reads
	// smooth at the (much coarser) AVoxelAgentReplicator::BroadcastInterval
	// Seconds wire cadence instead of snapping every broadcast.
	struct FVoxelAgentClientView
	{
		FVector PrevPosition = FVector::ZeroVector;
		FVector TargetPosition = FVector::ZeroVector;
		double PrevTimeSeconds = 0.0;
		double TargetTimeSeconds = 0.0;
		uint8 Tier = 0;
		int32 InstanceIndex = INDEX_NONE;
		// Scratch flag, reset false at the start of every ApplyReplicatedAgentSnapshot
		// call and set true for every id the incoming snapshot actually
		// contains -- an entry left false afterward has aged out of the
		// server's relevancy radius (see CollectReplicationSnapshot) and gets
		// its instance hidden (NOT removed -- see ApplyReplicatedAgentSnapshot's
		// doc comment for why hiding, not UInstancedStaticMeshComponent::
		// RemoveInstance, is used) rather than torn down, since the same
		// AgentId will likely reappear later.
		bool bSeenLastSnapshot = false;
	};
	TMap<int32, FVoxelAgentClientView> ClientAgents;

	// Verification switches (docs/status.md M6 section "Tier-1 hierarchical
	// planning + NPC replication") parsed once in OnWorldBeginPlay -- see the
	// .cpp for exactly what each logs/captures. Both live HERE (not GameMode)
	// because they must run on EVERY net mode including a pure remote
	// client, and AVoxelEarthGameMode only ever exists on the authority (a
	// UWorldSubsystem is the only thing this module has that's common to
	// every role).
	FTimerHandle DumpAgentsTimerHandle;
	FTimerHandle DumpAgentsQuitTimerHandle;
	FTimerHandle ClientScreenshotTimerHandle;
	FTimerHandle ClientScreenshotQuitTimerHandle;

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

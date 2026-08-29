#include "VoxelAgentSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "VoxelCoords.h"
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution -- one count per whole-ISM proxy rebuild
#include "VoxelWorldSubsystem.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelAgentSubsystem.h's identical PImpl doc comment). pathfind.h is the
// M6-groundwork windowed voxel A* this subsystem drives (docs/status.md's
// M6 section) -- consumed as-is, never modified (voxel-core stays
// engine-free and owned by that other slice). regiongraph.h is the M6 gap
// closure this file now ALSO drives for Tier 1 (docs/status.md M6 section
// "Tier-1 hierarchical planning + NPC replication") -- likewise consumed
// as-is, never modified.
#include "voxelcore/core.h"
#include "voxelcore/pathfind.h"
#include "voxelcore/regiongraph.h"

namespace
{
// The synthetic "solid" material fed to vxc::findPath's MaterialFn (see
// PlanPath below): UVoxelWorldSubsystem::IsSolidAtVoxel only reports a
// bool, not a real vxc::MaterialId, so every solid voxel is reported as
// this one sentinel material regardless of what it actually is. MAT_ROCK
// (not MAT_AIR, not MAT_BEDROCK) is used so classifyMove's "is this air?" /
// "is this unconditionally-impassable bedrock?" checks both do the right
// thing.
//
// KNOWN v0 LIMITATION carried into digging-while-pathing (docs/status.md M6
// section, follow-up): because every solid voxel reports as this ONE
// sentinel material, kMineCostByMaterial below has exactly ONE entry that
// can ever actually be read at runtime -- MAT_ROCK's. The full table is
// still filled in (documenting the intended relative-hardness ordering) for
// whoever adds a real material-returning query to UVoxelWorldSubsystem
// (VoxelWorldSubsystem.h/.cpp is READ-ONLY for this slice -- see
// VoxelAgentSubsystem.h's class comment) -- until then, "softer materials
// mine cheaper" is documented intent, not yet observable behavior.
constexpr vxc::MaterialId kSolidSentinelMaterial = vxc::MAT_ROCK;

// Mine cost per material, relative to PathCostConfig::walkCost's default
// (10) -- "hardness x dig-time" per pathfind.h's own cost-function doctrine
// comment. Indices match vxc::Material (voxelcore/core.h); MAT_AIR is never
// queried (Mine only ever fires on a solid destination -- classifyMove
// checks destMat != MAT_AIR first). MAT_BEDROCK's entry is irrelevant
// (pathfind.h hard-codes it impassable regardless of config -- "a caller
// cannot accidentally make it mineable by misconfiguring the cost table")
// but is still set to the documented impassable sentinel (negative) here
// for self-consistency with every other genuinely-impassable entry.
constexpr int32 kMineCostByMaterial[vxc::kMaterialCount] = {
    /* MAT_AIR           */ 0,
    /* MAT_BEDROCK       */ -1,
    /* MAT_ROCK          */ 35, // hard: ~3.5x walkCost -- the ONLY entry solidFn can produce today, see above
    /* MAT_GRAVEL        */ 18,
    /* MAT_SAND          */ 14,
    /* MAT_SUBSOIL       */ 20,
    /* MAT_TOPSOIL       */ 16,
    /* MAT_SNOW          */ 12,
    /* MAT_GRASS         */ 16,
    /* MAT_JUNGLE_SOIL   */ 18,
    /* MAT_SAVANNA_GRASS */ 15,
    /* MAT_PODZOL        */ 19,
    /* MAT_PERMAFROST    */ 22,
    /* MAT_MUD           */ 14,
    /* MAT_CLAY          */ 20,
    // Debug instrument, never real content. Was implicitly 0 before this line
    // existed: the static_assert below checks the array's declared SIZE, which
    // C++ satisfies by zero-filling anything the initializer omits, so a
    // missing entry is not a compile error. Written out to keep that from
    // being invisible; the value is unchanged.
    /* MAT_WATERMARK     */ 0,

    // Asset materials. Wood is roughly as hard as packed subsoil to cut and
    // heartwood harder than the bark around it; dead wood is dry and brittle
    // and gives way sooner. Foliage is barely an obstacle -- a few frames of
    // pushing through, not a dig -- which is what keeps an agent from routing
    // a mile around a tree it could walk through the canopy of.
    /* MAT_BARK          */ 22,
    /* MAT_HEARTWOOD     */ 26,
    /* MAT_DEADWOOD      */ 16,
    /* MAT_LEAF_BROADLEAF*/ 5,
    /* MAT_LEAF_NEEDLE   */ 5,
    /* MAT_LEAF_JUNGLE   */ 6,
    /* MAT_LEAF_DRY      */ 4,
    /* MAT_BARK_PALE     */ 20,
    /* MAT_LEAF_BLOSSOM  */ 5,
    /* MAT_LEAF_AUTUMN   */ 5,

    // Creature materials. An animal is a DETAIL ENTITY: it is spawned near the
    // player, nothing about it is saved, and it is deleted shortly after the
    // player leaves. It is not terrain and it is not scenery to be dug through
    // -- an agent that meets a fish should pass it, not mine it.
    //
    // 1 rather than 0 because 0 is MAT_AIR's value and means "not there at
    // all", and rather than the negative impassable sentinel because a fish
    // does not block anything either. It is the smallest value that still says
    // a voxel is present, and it sits below foliage's 4-6, which is already
    // documented above as "a few frames of pushing through, not a dig".
    //
    // If these ever want a real cost it will be because something here stopped
    // being a detail entity, and that is a design change rather than a tuning
    // one.
    /* MAT_SKIN_DARK     */ 1,
    /* MAT_SKIN_PALE     */ 1,
    /* MAT_SKIN_SILVER   */ 1,
    /* MAT_SKIN_OLIVE    */ 1,
    /* MAT_SKIN_BROWN    */ 1,
    /* MAT_SKIN_ORANGE   */ 1,
    /* MAT_SKIN_YELLOW   */ 1,
    /* MAT_SKIN_RED      */ 1,
    /* MAT_SKIN_BLUE     */ 1,
    /* MAT_SKIN_GREEN    */ 1,

    // Plumage. Same argument as the skins above and the same value: a bird is
    // a detail entity, spawned near the player from (species, seed) and gone
    // when it despawns, with nothing about the individual saved. An agent that
    // meets one should pass through it, not dig it.
    //
    // The bill and legs (MAT_BEAK_HORN) get 1 as well, even though keratin is
    // harder than feather. Relative hardness between two parts of the same
    // two-voxel bill is not a distinction anything can act on, and giving it a
    // higher number would be inventing a difference to look thorough.
    /* MAT_PLUME_WHITE   */ 1,
    /* MAT_PLUME_GREY    */ 1,
    /* MAT_PLUME_SLATE   */ 1,
    /* MAT_PLUME_BUFF    */ 1,
    /* MAT_PLUME_RUFOUS  */ 1,
    /* MAT_PLUME_CRIMSON */ 1,
    /* MAT_PLUME_LIME    */ 1,
    /* MAT_PLUME_CYAN    */ 1,
    /* MAT_PLUME_LILAC   */ 1,
    /* MAT_PLUME_IRIDESCENT */ 1,
    /* MAT_BEAK_HORN     */ 1,
};
static_assert(sizeof(kMineCostByMaterial) / sizeof(kMineCostByMaterial[0]) == vxc::kMaterialCount,
              "kMineCostByMaterial must cover every vxc::Material entry");

} // namespace

// Digging-while-pathing safety valve (docs/status.md M6 section), mirroring
// voxel.Destruction.Enabled's identical role for M5 (VoxelWorldSubsystem.cpp):
// on by default; -ExecCmds="voxel.NPCDig.Enabled 0" (or the console at
// runtime) makes PlanPath route EVERY tier -- including Tier 0 -- through
// FVoxelAgentImpl::NavConfig instead of DigConfig (see PlanPath), so
// disabling this is realized as a COST FUNCTION change (Mine/Bridge become
// exactly as impassable/prohibitive as they are for Tier 1 today), not a
// separate imperative "block the dig" bolted onto execution -- proving the
// M6 payoff ("walking, mining, tunneling, bridging fall out of ONE cost
// function") applies to this switch too: agents genuinely plan detours (or
// get capped short of the goal) rather than planning a tunnel and then
// having it silently fail to execute. TryExecuteWaypointEdit ALSO checks
// this cvar directly (defensive, belt-and-braces -- a runtime toggle
// between an agent's last replan and its next waypoint step should still
// take effect immediately rather than waiting for that agent's next
// replan).
static TAutoConsoleVariable<bool> CVarVoxelNPCDigEnabled(
	TEXT("voxel.NPCDig.Enabled"), true,
	TEXT("M6: allow Tier 0 NPC agents to execute authoritative Mine/Bridge terrain edits while pathing."),
	ECVF_Default);

// Tier 1 hierarchical planner toggle (M6 gap closure, docs/status.md M6
// section "Tier-1 hierarchical planning + NPC replication"): on by default
// (Tier 1 uses vxc::findHierarchicalPath when the region graph covers the
// query, PlanPath's fine windowed search otherwise -- see PlanPathTier1).
// Set to 0 to force EVERY Tier 1 planning call through the pre-M6 fine
// windowed search regardless of graph coverage -- this is the "before"
// side of the before/after cost comparison this gap closure needs to
// report: run the identical -VoxelSwarmTest/-VoxelTier1RegionGraphTest
// scenario once with this at its default (1) and once with
// "-ExecCmds=\"voxel.Agent.Tier1RegionGraph.Enabled 0\"", then diff the
// "VoxelSwarm Tier1 planner:" log lines.
// DEFAULT ON (2026-07-21). It was briefly defaulted OFF because the
// region-graph BUILD was synchronous and measured at ~160s (256-expansion
// cap) to >500s (4096 cap, killed) for the 1,875-region box, which would
// have stalled the game thread for minutes on the first Tier-1 plan. Both
// halves of that are now fixed and MEASURED (see
// UVoxelAgentSubsystem.h's "Tier-1 region graph build budget" and
// regiongraph.h's "Build cost"):
//   - the build itself got ~4x cheaper in CPU and 67x cheaper in MaterialFn
//     traffic (the term that actually dominated in-engine), with the
//     resulting graph bit-identical -- golden digest 0xeb05deb529b8f143 and
//     the incremental-equals-from-scratch test both still pass;
//   - and it is no longer synchronous: AdvanceTier1RegionGraphBuild spends
//     at most Tier1GraphBuildBudgetMsPerTick (2.0ms) per frame on a
//     resumable vxc::RegionGraphBuilder and swaps the finished graph in only
//     when complete, so NO frame pays the build.
// Until the first build lands, Tier 1 planning degrades to exactly the
// pre-M6 fine windowed search -- never a stall, never a half-built graph.
// Set to 0 to force EVERY Tier 1 planning call through the fine windowed
// search regardless of graph coverage (the "before" side of the before/after
// comparison described above).
static TAutoConsoleVariable<bool> CVarVoxelTier1UseRegionGraph(
	TEXT("voxel.Agent.Tier1RegionGraph.Enabled"), true,
	TEXT("M6: Tier 1 agents plan via the hierarchical region-graph planner (regiongraph.h) when the graph covers ")
	TEXT("the query, falling back to the pre-M6 fine windowed search otherwise. Set to 0 to force ALL Tier 1 ")
	TEXT("planning through the fine windowed search (before/after cost comparison)."),
	ECVF_Default);

// PImpl (VoxelAgentSubsystem.h's class comment): the voxel-core state this
// UHT-parsed header must never see directly.
struct FVoxelAgentImpl
{
	// Parallel to UVoxelAgentSubsystem::Agents (same index, grown/shrunk in
	// lockstep by SpawnSwarm) -- Tier 0/1's full vxc::PathResult per agent,
	// kept ONLY here so FVoxelAgent (VoxelAgent.h) stays voxel-core-free.
	// FVoxelAgent::Waypoints is the UE-side world-space projection of
	// Paths[i].steps, rebuilt by PlanPath every replan; Tier 2 agents'
	// entries are simply left default-constructed (empty PathResult) and
	// never read.
	TArray<vxc::PathResult> Paths;

	// Parallel to Paths (same index): true iff Paths[i] was planned with
	// DigConfig (below) -- i.e. it may contain Mine/Bridge steps. Set by
	// PlanPath every replan. TickTier1 uses this to detect the moment it
	// inherits a just-demoted Tier 0 agent's dig-capable path and forces an
	// immediate re-plan (with NavConfig) rather than trying to walk a step
	// it will never execute -- see TickTier1's doc comment.
	TArray<bool> PathIsDigCapable;

	// Digging-while-pathing (docs/status.md M6 section): TWO shared cost
	// configs, selected per-call by PlanPath from the requesting agent's
	// Tier (and the voxel.NPCDig.Enabled cvar -- see that cvar's doc
	// comment). DigConfig enables Mine/Bridge with real (if currently
	// material-flattened -- see kSolidSentinelMaterial's doc comment) costs;
	// NavConfig is BYTE-IDENTICAL to the pre-M6 single-config setup (Mine
	// literally impossible, Bridge priced far above any realistic detour) --
	// see Initialize for the exact values and the "Rate limiting -- tier
	// restriction" doc comment (VoxelAgentSubsystem.h) for why Tier 1/2
	// always get NavConfig regardless of the cvar.
	vxc::PathCostConfig DigConfig;
	vxc::PathCostConfig NavConfig;

	// --- Tier 1 hierarchical region-graph planner (M6 gap closure) ---------
	// See UVoxelAgentSubsystem.h's Tier1Graph* constants doc comment for the
	// extent/lifetime design and EnsureTier1RegionGraph/PlanPathTier1 (.cpp,
	// below) for the build/query/fallback logic.
	vxc::RegionGraph Tier1RegionGraph;
	bool bTier1GraphValid = false;
	// In-flight TIME-SLICED build (regiongraph.h header comment "Build
	// cost"): non-null exactly while a (re)build is in progress. The live
	// Tier1RegionGraph above is NEVER touched until this builder reports
	// done() -- so a build in progress costs Tier 1 nothing but freshness
	// (it keeps querying the previous graph, or falls back to the fine
	// windowed search before the very first build lands), and never a
	// multi-second game-thread stall. Driven by
	// AdvanceTier1RegionGraphBuild once per Tick.
	TUniquePtr<vxc::RegionGraphBuilder> Tier1GraphBuilder;
	FVector Tier1GraphBuildCenterWorld = FVector::ZeroVector;
	double Tier1GraphBuildStartSeconds = 0.0;
	double Tier1GraphBuildBusySeconds = 0.0;   // CPU actually spent, summed over slices
	double Tier1GraphBuildWorstSliceMs = 0.0;  // worst single tick's slice -- the frame-budget number
	int32 Tier1GraphBuildSlices = 0;
	int32 Tier1GraphBuildLoggedQuarter = 0; // progress-log throttle, see AdvanceTier1RegionGraphBuild
	// World-space (ground-projected) center the graph was last (re)built
	// around -- EnsureTier1RegionGraph rebuilds once the player's ground
	// column drifts UVoxelAgentSubsystem::Tier1GraphRebuildTriggerUU away
	// from this cached value.
	FVector Tier1GraphCenterWorld = FVector::ZeroVector;
	// Dirty invalidation, player/M5-edit case (PollWorldEditsForTier1DirtyRegions's
	// doc comment, UVoxelAgentSubsystem.h): UVoxelWorldSubsystem::GetLogSize()
	// as of the last time this subsystem checked it (or the graph's last
	// (re)build, whichever is more recent) -- a poll-based "did ANYTHING
	// change" signal, since no delegate reports WHERE.
	uint64 LastSeenEditLogSize = 0;

	// Tier 1 planning-cost measurement (docs/status.md M6 section "measure
	// it"): running totals split by which planner actually served the call --
	// logged periodically (Tick, same cadence as the existing convergence
	// log) as an average expansions/call for each side, the real "before
	// (fine fallback) vs after (hierarchical)" comparison number.
	int64 Tier1HierarchicalCalls = 0;
	int64 Tier1HierarchicalExpansionsTotal = 0;
	int64 Tier1FineFallbackCalls = 0;
	int64 Tier1FineFallbackExpansionsTotal = 0;
};

UVoxelAgentSubsystem::UVoxelAgentSubsystem() = default;
UVoxelAgentSubsystem::~UVoxelAgentSubsystem() = default;
UVoxelAgentSubsystem::UVoxelAgentSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelAgentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Doesn't strictly need UVoxelWorldSubsystem to exist THIS instant
	// (Tick/SpawnSwarm both look it up lazily via GetSubsystem when they
	// actually run), but declaring the dependency still gets it
	// constructed first -- same ordering discipline UVoxelWaterSubsystem's
	// Initialize documents for the identical reason.
	Collection.InitializeDependency<UVoxelWorldSubsystem>();

	Impl = MakeUnique<FVoxelAgentImpl>();

	// DigConfig (Tier 0, digging-while-pathing -- docs/status.md M6
	// section): the M6 payoff itself. Mine costs come from
	// kMineCostByMaterial (file-scope table above; only MAT_ROCK's entry is
	// reachable today -- see kSolidSentinelMaterial's doc comment).
	// bridgeCost is nudged up from PathCostConfig's own default (20 -> 26,
	// ~2.6x walkCost) -- placing a scaffold voxel is a real authoritative
	// edit with the same rate-limited-and-logged cost as a Mine, so it
	// should sit clearly above "a modest walked detour" (a few walkCost
	// cells) while staying cheaper than tunneling through hard rock
	// (kMineCostByMaterial[MAT_ROCK] = 35) -- bridging a gap is usually
	// less work than excavating solid material through it. Walk/StepUp/
	// StepDown/Climb/Fall/Jump keep pathfind.h's own PathCostConfig
	// defaults (walkCost=10, stepUpCost=14, fallCostPerVoxel=5,
	// jumpGapCost=15) unchanged in both configs.
	for (int32 MatIndex = 0; MatIndex < vxc::kMaterialCount; ++MatIndex)
	{
		Impl->DigConfig.mineCostByMaterial[MatIndex] = kMineCostByMaterial[MatIndex];
	}
	Impl->DigConfig.bridgeCost = 26;

	// NavConfig (Tier 1/2, and Tier 0 too whenever voxel.NPCDig.Enabled is
	// off -- see PlanPath): BYTE-IDENTICAL to the pre-M6 single-config
	// setup this subsystem originally shipped with (Mine literally
	// impossible via the documented negative-cost "impassable" sentinel;
	// Bridge priced far above any realistic detour, effectively -- not
	// literally -- disabled). See pathfind.h's classifyMove doc comment for
	// why a negative mineCostByMaterial entry, not an infinite bridgeCost,
	// is the correct way to make Mine truly unreachable.
	for (int32 MatIndex = 0; MatIndex < vxc::kMaterialCount; ++MatIndex)
	{
		Impl->NavConfig.mineCostByMaterial[MatIndex] = -1;
	}
	Impl->NavConfig.bridgeCost = 1'000'000;
}

void UVoxelAgentSubsystem::Deinitialize()
{
	Agents.Empty();
	Impl.Reset();
	AgentISM = nullptr;
	SwarmOwner = nullptr;

	Super::Deinitialize();
}

bool UVoxelAgentSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem/UVoxelWaterSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelAgentSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// M6 gap closure verification switches (docs/status.md M6 section
	// "Tier-1 hierarchical planning + NPC replication") -- parsed here (a
	// UWorldSubsystem), NOT AVoxelEarthGameMode, specifically because they
	// must run on EVERY net mode including a pure remote client, and
	// GameMode only ever exists on the authority (see this method's
	// dedicated-server branch below for the analogous "sim always, render
	// never" split this module already uses).
	//
	// -VoxelDumpAgentsAfter=<seconds>: logs role=Server/Client (from THIS
	// process's own GetNetMode()), agent count, and the first few agents'
	// (sorted by AgentId/pool-index) raw world positions + tier -- the
	// "two clients dig the same hole"-style position-agreement evidence
	// (docs/m3-plan.md), just for NPC state instead of terrain edits. A
	// SERVER process reads straight from the authoritative Agents pool; a
	// CLIENT process reads from ClientAgents (the replicated-state mirror,
	// see ApplyReplicatedAgentSnapshot) -- comparing the two processes' log
	// lines (run with roughly the same -VoxelDumpAgentsAfter delay) is the
	// verification. Self-quits 5s after dumping, same convenience pattern as
	// every other headless-run verification switch in this module
	// (UVoxelWorldSubsystem::SaveWorld/AVoxelEarthGameMode's -VoxelDumpDigestAfter).
	float DumpAgentsAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDumpAgentsAfter="), DumpAgentsAfterSeconds) && DumpAgentsAfterSeconds > 0.f)
	{
		InWorld.GetTimerManager().SetTimer(
			DumpAgentsTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* DumpWorld = GetWorld();
					if (!DumpWorld)
					{
						return;
					}
					const bool bIsClient = DumpWorld->GetNetMode() == NM_Client;
					const TCHAR* RoleStr = bIsClient ? TEXT("Client") : TEXT("Server");

					if (bIsClient)
					{
						TArray<int32> Ids;
						ClientAgents.GetKeys(Ids);
						Ids.Sort();
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelAgentDump: role=%s count=%d"), RoleStr, Ids.Num());
						for (int32 i = 0; i < FMath::Min(5, Ids.Num()); ++i)
						{
							const FVoxelAgentClientView& View = ClientAgents[Ids[i]];
							UE_LOG(LogVoxelEarth, Log,
							       TEXT("VoxelAgentDump: role=%s agentId=%d pos=(%.1f,%.1f,%.1f) tier=%d"),
							       RoleStr, Ids[i], View.TargetPosition.X, View.TargetPosition.Y, View.TargetPosition.Z,
							       View.Tier);
						}
					}
					else
					{
						UE_LOG(LogVoxelEarth, Log, TEXT("VoxelAgentDump: role=%s count=%d"), RoleStr, Agents.Num());
						for (int32 i = 0; i < FMath::Min(5, Agents.Num()); ++i)
						{
							const FVoxelAgent& Agent = Agents[i];
							UE_LOG(LogVoxelEarth, Log,
							       TEXT("VoxelAgentDump: role=%s agentId=%d pos=(%.1f,%.1f,%.1f) tier=%d"),
							       RoleStr, i, Agent.Position.X, Agent.Position.Y, Agent.Position.Z, (int32)Agent.Tier);
						}
					}

					UWorld* QuitWorld = GetWorld();
					if (QuitWorld)
					{
						QuitWorld->GetTimerManager().SetTimer(
							DumpAgentsQuitTimerHandle,
							FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 5.f, false);
					}
				}),
			DumpAgentsAfterSeconds, false);
	}

	// -VoxelClientScreenshotAfter=<seconds>: a CLIENT-capable counterpart to
	// AVoxelEarthGameMode's existing -VoxelScreenshotAfter (which can never
	// fire on a pure remote client process -- AGameModeBase only ever exists
	// on the authority). Deliberately a SEPARATE switch name, not a
	// second handler for the same -VoxelScreenshotAfter flag, so a
	// standalone/listen-server run's existing screenshot behavior (relied on
	// by every other milestone's verification) is never touched by this
	// slice. Aims the local camera down at the swarm, requests one capture,
	// then quits -- same shape as GameMode's Capture lambda, minus the
	// per-test-fixture framing branches this subsystem has no need of.
	float ClientScreenshotAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelClientScreenshotAfter="), ClientScreenshotAfterSeconds) &&
	    ClientScreenshotAfterSeconds > 0.f)
	{
		InWorld.GetTimerManager().SetTimer(
			ClientScreenshotTimerHandle,
			FTimerDelegate::CreateWeakLambda(this,
				[this]()
				{
					UWorld* ShotWorld = GetWorld();
					if (!ShotWorld || ShotWorld->GetNetMode() == NM_DedicatedServer)
					{
						return; // no viewport to capture from
					}
					if (APlayerController* PC = ShotWorld->GetFirstPlayerController())
					{
						PC->SetControlRotation(FRotator(-35.f, 0.f, 0.f));
						if (APawn* P = PC->GetPawn())
						{
							P->SetActorRotation(FRotator(-35.f, 0.f, 0.f));
						}
					}
					FScreenshotRequest::RequestScreenshot(TEXT("VoxelClientSwarm"), false, true);

					UWorld* QuitWorld = GetWorld();
					if (QuitWorld)
					{
						QuitWorld->GetTimerManager().SetTimer(
							ClientScreenshotQuitTimerHandle,
							FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 4.f, false);
					}
				}),
			ClientScreenshotAfterSeconds, false);
	}

	// Dedicated server: no viewport, so no cosmetic ISM -- but the swarm
	// simulation still runs authoritatively regardless (Tick() below
	// doesn't check AgentISM for the sim half, only UpdateInstanceTransform
	// does, and that's already a no-op without it). Same "sim always,
	// render never" split UVoxelWorldSubsystem's chunk streaming and
	// UVoxelWaterSubsystem's CA both use for NM_DedicatedServer -- see
	// class comment.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSwarm rendering DISABLED (NM_DedicatedServer has no viewport): the swarm still simulates ")
		       TEXT("authoritatively."));
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SwarmOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (SwarmOwner == nullptr)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("VoxelAgentSubsystem: failed to spawn the swarm owner actor; swarm rendering will not start."));
		return;
	}
#if WITH_EDITOR
	SwarmOwner->SetActorLabel(TEXT("VoxelSwarmOwner"));
#endif

	// Cheap agent body (class comment, VoxelAgent.h's class comment): one
	// InstancedStaticMeshComponent, one instance per agent -- mirrors
	// AVoxelDebris' "engine cubes" ISM style rather than N actors, which is
	// the whole point for hundreds of agents. No collision, no shadow-cost
	// surprises -- purely cosmetic/verification, same as VoxelDebris' ISM.
	AgentISM = NewObject<UInstancedStaticMeshComponent>(SwarmOwner, TEXT("VoxelAgentISM"));
	SwarmOwner->SetRootComponent(AgentISM);

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh)
	{
		AgentISM->SetStaticMesh(CubeMesh);
	}
	else
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem: /Engine/BasicShapes/Cube.Cube not found -- swarm will render with no mesh assigned."));
	}
	AgentISM->SetMobility(EComponentMobility::Movable);
	AgentISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	AgentISM->SetCanEverAffectNavigation(false);
	AgentISM->RegisterComponent();
	VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	VoxelEofLedger::CountRegister();

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelAgentSubsystem initialized: swarm ISM ready."));
}

TStatId UVoxelAgentSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelAgentSubsystem, STATGROUP_Tickables);
}

int32 UVoxelAgentSubsystem::SpawnSwarm(int32 Count)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		// Server-authoritative (class comment): spawning is an authority-only
		// decision, mirroring UVoxelWaterSubsystem::SpawnWaterAt's identical
		// role-split refusal.
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarm: refused on NM_Client."));
		return 0;
	}
	if (!Impl)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarm: subsystem not initialized."));
		return 0;
	}

	UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* PlayerPawn = PC ? PC->GetPawn() : nullptr;
	if (!Terrain || !PlayerPawn)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarm: no terrain subsystem / player pawn yet -- nothing spawned."));
		return 0;
	}

	const FVector PlayerLoc = PlayerPawn->GetActorLocation();

	int32 SpawnedCount = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		// Ring around the player (SpawnSwarm's doc comment): random angle,
		// radius spanning SpawnRingInnerUU..SpawnRingOuterUU (10m-150m) so
		// the initial swarm spans all three LOD tiers at once.
		const double Angle = FMath::FRandRange(0.0, 2.0 * PI);
		const double Radius = FMath::FRandRange(SpawnRingInnerUU, SpawnRingOuterUU);
		const double WorldX = PlayerLoc.X + FMath::Cos(Angle) * Radius;
		const double WorldY = PlayerLoc.Y + FMath::Sin(Angle) * Radius;
		SpawnOneAgentAt(WorldX, WorldY, *Terrain);
		++SpawnedCount;
	}

	if (AgentISM)
	{
		AgentISM->MarkRenderStateDirty();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSwarm: spawned %d agents ringed %.0fm-%.0fm around the player at (%.0f,%.0f)."),
	       SpawnedCount, SpawnRingInnerUU / 100.0, SpawnRingOuterUU / 100.0, PlayerLoc.X, PlayerLoc.Y);
	return SpawnedCount;
}

int32 UVoxelAgentSubsystem::SpawnSwarmAtOffset(int32 Count, const FVector& CenterWorldPos, const FVector& Offset,
                                                double OffsetRadiusUU, double LateralJitterUU)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarmAtOffset: refused on NM_Client."));
		return 0;
	}
	if (!Impl)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarmAtOffset: subsystem not initialized."));
		return 0;
	}
	UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelAgentSubsystem::SpawnSwarmAtOffset: no terrain subsystem yet -- nothing spawned."));
		return 0;
	}

	FVector Dir = Offset.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector(1.0, 0.0, 0.0);
	}
	// Perpendicular (XY-plane) axis for the lateral jitter -- keeps the
	// cluster spread ACROSS the approach direction, not toward/away from it,
	// so every spawned agent stays roughly the same distance from the
	// obstacle this test scenario places along Offset.
	const FVector Lateral(-Dir.Y, Dir.X, 0.0);
	const FVector BaseWorldPos = CenterWorldPos + Dir * OffsetRadiusUU;

	int32 SpawnedCount = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		const double Jitter = FMath::FRandRange(-LateralJitterUU, LateralJitterUU);
		const FVector SpawnPos = BaseWorldPos + Lateral * Jitter;
		SpawnOneAgentAt(SpawnPos.X, SpawnPos.Y, *Terrain);
		++SpawnedCount;
	}

	if (AgentISM)
	{
		AgentISM->MarkRenderStateDirty();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSwarm: spawned %d agents at offset %.0fm from (%.0f,%.0f) toward (%.2f,%.2f,%.2f), lateral jitter +-%.0fm."),
	       SpawnedCount, OffsetRadiusUU / 100.0, CenterWorldPos.X, CenterWorldPos.Y, Dir.X, Dir.Y, Dir.Z,
	       LateralJitterUU / 100.0);
	return SpawnedCount;
}

int32 UVoxelAgentSubsystem::SpawnOneAgentAt(double WorldX, double WorldY, UVoxelWorldSubsystem& Terrain)
{
	const double InstanceScaleXY = AgentBodyWidthUU / 100.0; // cube base mesh is 100UU
	const double InstanceScaleZ = (AgentHalfHeightUU * 2.0) / 100.0;

	// GetSurfaceHeightUU is a pure query over the amplifier (safe before any
	// chunk has streamed in -- see UVoxelWorldSubsystem.h's doc comment), so
	// agents can be ground-placed immediately at BeginPlay with no need to
	// wait for render streaming.
	const double SurfaceUU = Terrain.GetSurfaceHeightUU(WorldX, WorldY);

	FVoxelAgent Agent;
	Agent.Position = FVector(WorldX, WorldY, SurfaceUU);
	// Placeholder starting tier -- the very next Tick's
	// ComputeNextVoxelAgentTier call reclassifies every agent from its REAL
	// distance to the player regardless of this initial guess (at most a
	// one-tick startup transient, see that function's doc comment for why
	// it can't skip more than one tier boundary per call anyway).
	Agent.Tier = EVoxelAgentTier::Tier1_Abstract;

	if (AgentISM)
	{
		const FTransform InstanceTransform(FRotator::ZeroRotator, Agent.Position + FVector(0.0, 0.0, AgentHalfHeightUU),
		                                    FVector(InstanceScaleXY, InstanceScaleXY, InstanceScaleZ));
		Agent.InstanceIndex = AgentISM->AddInstance(InstanceTransform, /*bWorldSpace=*/true);
	}

	const int32 NewIndex = Agents.Add(Agent);
	if (Impl)
	{
		Impl->Paths.Add(vxc::PathResult{});
		Impl->PathIsDigCapable.Add(false);
	}
	return NewIndex;
}

bool UVoxelAgentSubsystem::PlanPath(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& GoalWorldPos)
{
	if (!Impl || !Agents.IsValidIndex(AgentIndex) || !Impl->Paths.IsValidIndex(AgentIndex))
	{
		return false;
	}
	FVoxelAgent& Agent = Agents[AgentIndex];

	UVoxelWorldSubsystem* TerrainPtr = &Terrain;
	const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
	};

	// Windowed search, NOT one huge search straight to the (possibly far
	// away) goal: the window is centered on the agent's OWN current voxel,
	// sized WindowHalfExtent/HeightVoxels (~3.3m x 3.3m x 2.4m -- see those
	// constants' doc comments) -- this is the "local windowed voxel A*"
	// the plan doctrine (SS3.6) actually calls for, and it matters for
	// cost: vxc::findPath allocates its bookkeeping arrays sized to the
	// FULL window volume up front, regardless of how much of it a
	// particular search actually needs (see pathfind.h's own "Search /
	// windowing" doc comment) -- a window sized to reach a player 20m away
	// would be ~40x bigger per axis and cost orders of magnitude more,
	// every replan, for every Tier 0/1 agent. When the goal lies outside
	// this small window (the common case beyond a few meters), findPath
	// returns capped=true with a best-effort path to whichever settled
	// cell is closest (Manhattan distance) to the goal -- which, since the
	// window is symmetric around the agent, is naturally the edge of the
	// window nearest the goal. The agent walks that partial path, reaches
	// its end, and this function gets called again from the new position:
	// repeated small windowed searches chain into steady progress toward a
	// moving goal, which is exactly the incremental-replanning design
	// pathfind.h's own header comment describes as the caller's
	// responsibility (see its "Incremental invalidation" section).
	const VoxelCoords::FVoxelCoord StartVoxel = VoxelCoords::WorldToVoxel(Agent.Position);
	const VoxelCoords::FVoxelCoord GoalVoxel = VoxelCoords::WorldToVoxel(GoalWorldPos);

	const vxc::PathCoord Start{StartVoxel.X, StartVoxel.Y, StartVoxel.Z};
	const vxc::PathCoord Goal{GoalVoxel.X, GoalVoxel.Y, GoalVoxel.Z};

	vxc::SearchWindow Window;
	Window.minCorner = vxc::PathCoord{Start.x - WindowHalfExtentVoxels, Start.y - WindowHalfExtentVoxels,
	                                   Start.z - WindowHalfHeightVoxels};
	Window.maxCorner = vxc::PathCoord{Start.x + WindowHalfExtentVoxels, Start.y + WindowHalfExtentVoxels,
	                                   Start.z + WindowHalfHeightVoxels};
	Window.maxExpansions = MaxExpansionsPerSearch;

	// Digging-while-pathing config selection (docs/status.md M6 section):
	// Tier 0 gets DigConfig (Mine/Bridge enabled) UNLESS the
	// voxel.NPCDig.Enabled safety valve is off, in which case every tier
	// falls back to NavConfig -- see that cvar's doc comment for why this
	// is realized as a cost-function swap rather than a separate
	// execution-side block. Tier 1/2 (Tier 2 never calls PlanPath at all)
	// always get NavConfig regardless of the cvar -- "Rate limiting -- tier
	// restriction" (VoxelAgentSubsystem.h class comment).
	const bool bWantsDigConfig = Agent.Tier == EVoxelAgentTier::Tier0_Embodied && CVarVoxelNPCDigEnabled.GetValueOnGameThread();
	const vxc::PathCostConfig& ConfigToUse = bWantsDigConfig ? Impl->DigConfig : Impl->NavConfig;

	vxc::PathResult Result = vxc::findPath(SolidFn, Start, Goal, ConfigToUse, Window);

	Agent.Waypoints.Reset();
	Agent.Waypoints.Reserve((int32)Result.steps.size());
	for (const vxc::PathStep& Step : Result.steps)
	{
		const VoxelCoords::FVoxelCoord Cell{Step.cell.x, Step.cell.y, Step.cell.z};
		Agent.Waypoints.Add(VoxelCoords::VoxelToWorldCenter(Cell));
	}
	Agent.WaypointIndex = 0;

	const UWorld* World = GetWorld();
	Agent.LastReplanTimeSeconds = World ? World->GetTimeSeconds() : Agent.LastReplanTimeSeconds;
	Agent.LastPlanGoalWorld = GoalWorldPos;

	Impl->Paths[AgentIndex] = MoveTemp(Result);
	if (Impl->PathIsDigCapable.IsValidIndex(AgentIndex))
	{
		Impl->PathIsDigCapable[AgentIndex] = bWantsDigConfig;
	}

	return Agent.Waypoints.Num() > 0;
}

void UVoxelAgentSubsystem::AdvanceTier1RegionGraphBuild(UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos)
{
	if (!Impl)
	{
		return;
	}

	// Ground-projected center (Tick's own GoalWorldPos philosophy, see its
	// class comment): agents are ground-snapped, so the graph should be
	// centered on the terrain surface near the player, not the player's
	// possibly-airborne Z.
	const double GroundZ = Terrain.GetSurfaceHeightUU(PlayerWorldPos.X, PlayerWorldPos.Y);
	const FVector GraphCenterWorld(PlayerWorldPos.X, PlayerWorldPos.Y, GroundZ);

	UVoxelWorldSubsystem* TerrainPtr = &Terrain;
	const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
	};

	// --- (1) Decide whether a NEW build needs to start ---------------------
	// Re-centering hysteresis is measured against whichever center is most
	// recent: the in-flight build's center if one is running (so a player
	// walking steadily doesn't restart the same build every few meters and
	// never finish one), otherwise the live graph's.
	const bool bBuilding = Impl->Tier1GraphBuilder.IsValid();
	const FVector ReferenceCenter = bBuilding ? Impl->Tier1GraphBuildCenterWorld : Impl->Tier1GraphCenterWorld;
	const bool bHaveReference = bBuilding || Impl->bTier1GraphValid;
	const bool bNeedsNewBuild =
		!bHaveReference ||
		FVector::DistSquared(GraphCenterWorld, ReferenceCenter) > FMath::Square(Tier1GraphRebuildTriggerUU);

	if (bNeedsNewBuild)
	{
		const VoxelCoords::FVoxelCoord CenterVoxel = VoxelCoords::WorldToVoxel(GraphCenterWorld);
		const vxc::PathCoord CenterPath{CenterVoxel.X, CenterVoxel.Y, CenterVoxel.Z};
		const vxc::RegionCoord CenterRegion = vxc::regionOfVoxel(CenterPath);

		const vxc::RegionCoord MinRegion{CenterRegion.x - Tier1GraphHorizontalRadiusRegions,
		                                   CenterRegion.y - Tier1GraphHorizontalRadiusRegions,
		                                   CenterRegion.z - Tier1GraphVerticalRadiusRegions};
		const vxc::RegionCoord MaxRegion{CenterRegion.x + Tier1GraphHorizontalRadiusRegions,
		                                   CenterRegion.y + Tier1GraphHorizontalRadiusRegions,
		                                   CenterRegion.z + Tier1GraphVerticalRadiusRegions};

		// NavConfig, not DigConfig: Tier 1 never plans with Mine/Bridge
		// enabled (class comment "Rate limiting -- tier restriction") -- the
		// abstract graph's portals/edges must reflect the SAME walkability
		// notion Tier 1's own findHierarchicalPath call queries with. The
		// config is captured by the builder's caller (us) on every advance,
		// so it must stay the same one for the whole build.
		Impl->Tier1GraphBuilder = MakeUnique<vxc::RegionGraphBuilder>(MinRegion, MaxRegion, Tier1GraphIntraMaxExpansions);
		Impl->Tier1GraphBuildCenterWorld = GraphCenterWorld;
		Impl->Tier1GraphBuildStartSeconds = FPlatformTime::Seconds();
		Impl->Tier1GraphBuildBusySeconds = 0.0;
		Impl->Tier1GraphBuildSlices = 0;
		Impl->Tier1GraphBuildWorstSliceMs = 0.0;
		Impl->Tier1GraphBuildLoggedQuarter = 0;
		// Snapshot the edit-log baseline at build START, not at completion:
		// a time-sliced build spans many ticks, and an edit that lands
		// PART-WAY through it is only reflected in the regions this builder
		// had not reached yet. Baselining at the start makes
		// PollWorldEditsForTier1DirtyRegions re-dirty for those edits once
		// the graph goes live, rather than silently trusting a graph that
		// saw two different world states.
		Impl->LastSeenEditLogSize = Terrain.GetLogSize();

		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSwarm Tier1 region graph build STARTED (time-sliced, %.1fms/tick budget): ")
		       TEXT("regions=[%lld..%lld]x[%lld..%lld]x[%lld..%lld] (%lld region-steps of work)"),
		       Tier1GraphBuildBudgetMsPerTick, MinRegion.x, MaxRegion.x, MinRegion.y, MaxRegion.y,
		       MinRegion.z, MaxRegion.z, Impl->Tier1GraphBuilder->totalRegionSteps());
	}

	if (!Impl->Tier1GraphBuilder.IsValid())
	{
		return; // graph is live and still well-centered -- nothing to do
	}

	// --- (2) Spend at most this tick's budget on it ------------------------
	// The budget is checked BETWEEN slices, so a frame can overshoot it by at
	// most one slice's worth of work (Tier1GraphBuildRegionsPerSlice regions
	// -- deliberately 1, so the overshoot is one region's portal detection or
	// one region's intra-edge searches, bounded by
	// Tier1GraphIntraMaxExpansions x that region's portal count and by the
	// ~20^3 voxels regiongraph.h's RegionMaterialCache reads per region).
	// See the class comment "Tier-1 region graph build budget".
	const double SliceStart = FPlatformTime::Seconds();
	const double BudgetSeconds = Tier1GraphBuildBudgetMsPerTick / 1000.0;
	bool bDone = false;
	do
	{
		bDone = Impl->Tier1GraphBuilder->advance(SolidFn, Impl->NavConfig, Tier1GraphBuildRegionsPerSlice);
	} while (!bDone && (FPlatformTime::Seconds() - SliceStart) < BudgetSeconds);

	const double SliceMs = (FPlatformTime::Seconds() - SliceStart) * 1000.0;
	Impl->Tier1GraphBuildBusySeconds += SliceMs / 1000.0;
	Impl->Tier1GraphBuildWorstSliceMs = FMath::Max(Impl->Tier1GraphBuildWorstSliceMs, SliceMs);
	++Impl->Tier1GraphBuildSlices;

	// Permanent guardrail, not debug scaffolding: the whole justification for
	// defaulting voxel.Agent.Tier1RegionGraph.Enabled ON is that the build
	// costs a bounded slice of a frame. A slice that exceeds a whole 60Hz
	// frame means that claim has regressed (a pathological region, a much
	// more expensive MaterialFn, or a raised Tier1GraphBuildRegionsPerSlice)
	// and should be seen, not silently absorbed.
	static constexpr double kFrameBudgetMs = 1000.0 / 60.0;
	if (SliceMs > kFrameBudgetMs)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelSwarm Tier1 region graph build slice took %.2fms -- over the %.2fms 60Hz frame budget ")
		       TEXT("(budget is %.1fms/tick, checked between %lld-region slices)"),
		       SliceMs, kFrameBudgetMs, Tier1GraphBuildBudgetMsPerTick, Tier1GraphBuildRegionsPerSlice);
	}

	if (!bDone)
	{
		// Quarter-by-quarter progress. Not decoration: a time-sliced build's
		// wall-clock-to-ready depends on the HOST's tick rate, not just on the
		// build's own CPU cost (measured: ~21s wall with 3 agents, but still
		// unfinished after 4 minutes under a 200-agent load, because a busy
		// frame delivers the same 4ms slice far less often). Without this the
		// only two observable states are "started" and "finished", which makes
		// a slow-but-healthy build indistinguishable from a wedged one.
		const int64 Total = Impl->Tier1GraphBuilder->totalRegionSteps();
		const int64 Done = Impl->Tier1GraphBuilder->completedRegionSteps();
		const int32 Quarter = Total > 0 ? (int32)((Done * 4) / Total) : 0;
		if (Quarter > Impl->Tier1GraphBuildLoggedQuarter)
		{
			Impl->Tier1GraphBuildLoggedQuarter = Quarter;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelSwarm Tier1 region graph build %d%% (%lld/%lld steps, %.1fms CPU across %d ticks, ")
			       TEXT("worst tick %.2fms, %.1fs wall so far)"),
			       Quarter * 25, Done, Total, Impl->Tier1GraphBuildBusySeconds * 1000.0,
			       Impl->Tier1GraphBuildSlices, Impl->Tier1GraphBuildWorstSliceMs,
			       FPlatformTime::Seconds() - Impl->Tier1GraphBuildStartSeconds);
		}
		return; // still building -- the PREVIOUS graph (if any) stays live
	}

	// --- (3) Swap the finished graph in ------------------------------------
	// Only now does the new graph become queryable. Until this point Tier 1
	// kept serving from the previous graph, or (on the very first build) fell
	// back to the pre-M6 fine windowed search -- never a stall, never a
	// half-built graph.
	Impl->Tier1RegionGraph = MoveTemp(Impl->Tier1GraphBuilder->graph());
	Impl->Tier1GraphCenterWorld = Impl->Tier1GraphBuildCenterWorld;
	Impl->bTier1GraphValid = true;
	Impl->Tier1GraphBuilder.Reset();

	const double WallSeconds = FPlatformTime::Seconds() - Impl->Tier1GraphBuildStartSeconds;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelSwarm Tier1 region graph (re)built: regions=[%lld..%lld]x[%lld..%lld]x[%lld..%lld] ")
	       TEXT("(%d portals, %d edges, %lld fine expansions spent building) -- %.1fms of CPU across %d ticks ")
	       TEXT("(worst single tick %.2fms), %.1fs wall"),
	       Impl->Tier1RegionGraph.minRegion.x, Impl->Tier1RegionGraph.maxRegion.x,
	       Impl->Tier1RegionGraph.minRegion.y, Impl->Tier1RegionGraph.maxRegion.y,
	       Impl->Tier1RegionGraph.minRegion.z, Impl->Tier1RegionGraph.maxRegion.z,
	       (int32)Impl->Tier1RegionGraph.portals.size(), (int32)Impl->Tier1RegionGraph.edges.size(),
	       Impl->Tier1RegionGraph.totalFinePathExpansions, Impl->Tier1GraphBuildBusySeconds * 1000.0,
	       Impl->Tier1GraphBuildSlices, Impl->Tier1GraphBuildWorstSliceMs, WallSeconds);
}

bool UVoxelAgentSubsystem::PlanPathTier1(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
                                          const FVector& GoalWorldPos)
{
	if (!Impl || !Agents.IsValidIndex(AgentIndex))
	{
		return false;
	}

	if (!CVarVoxelTier1UseRegionGraph.GetValueOnGameThread())
	{
		// Forced "before" mode (before/after cost comparison, docs/status.md
		// M6 section) -- go straight to the pre-M6 fine windowed search,
		// same as if the graph never covered this query.
		const bool bOk = PlanPath(AgentIndex, Terrain, GoalWorldPos);
		if (Impl->Paths.IsValidIndex(AgentIndex))
		{
			Impl->Tier1FineFallbackExpansionsTotal += Impl->Paths[AgentIndex].expansionsUsed;
		}
		++Impl->Tier1FineFallbackCalls;
		return bOk;
	}

	// Non-blocking: kicks a time-sliced build off (or nudges the in-flight
	// one) and returns immediately. If no graph is live YET, bCovered below
	// is false and this call falls back to the fine windowed search -- the
	// same graceful degradation an out-of-box agent already gets.
	AdvanceTier1RegionGraphBuild(Terrain, PlayerWorldPos);

	FVoxelAgent& Agent = Agents[AgentIndex];
	const VoxelCoords::FVoxelCoord StartVoxel = VoxelCoords::WorldToVoxel(Agent.Position);
	const VoxelCoords::FVoxelCoord GoalVoxel = VoxelCoords::WorldToVoxel(GoalWorldPos);
	const vxc::PathCoord Start{StartVoxel.X, StartVoxel.Y, StartVoxel.Z};
	const vxc::PathCoord Goal{GoalVoxel.X, GoalVoxel.Y, GoalVoxel.Z};

	const bool bCovered = Impl->bTier1GraphValid &&
	                       vxc::regionInBounds(vxc::regionOfVoxel(Start), Impl->Tier1RegionGraph.minRegion,
	                                            Impl->Tier1RegionGraph.maxRegion) &&
	                       vxc::regionInBounds(vxc::regionOfVoxel(Goal), Impl->Tier1RegionGraph.minRegion,
	                                            Impl->Tier1RegionGraph.maxRegion);

	auto FallBackToFineSearch = [this, AgentIndex, &Terrain, &GoalWorldPos]() -> bool
	{
		// Graceful degradation (class comment "Graph lifetime/extent"): a
		// Tier 1 agent this graph doesn't (yet) cover, or whose corridor
		// query genuinely failed, is NEVER worse off than pre-M6 Tier 1 --
		// it just falls through to the identical fine windowed search
		// PlanPath already provides Tier 0.
		const bool bOk = PlanPath(AgentIndex, Terrain, GoalWorldPos);
		if (Impl->Paths.IsValidIndex(AgentIndex))
		{
			Impl->Tier1FineFallbackExpansionsTotal += Impl->Paths[AgentIndex].expansionsUsed;
		}
		++Impl->Tier1FineFallbackCalls;
		return bOk;
	};

	if (!bCovered)
	{
		return FallBackToFineSearch();
	}

	UVoxelWorldSubsystem* TerrainPtr = &Terrain;
	const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
	};

	// refine=true (concrete step path), not the coarser corridor-waypoints
	// option: AdvanceAlongWaypoints (VoxelAgent.cpp) walks a STRAIGHT LINE
	// between consecutive Waypoints with no collision/edit awareness of its
	// own -- it has no way to detour around an obstacle a coarse
	// portal-to-portal straight line might clip through mid-region. Every
	// refined intra-region hop is still bounded to ONE region's own
	// SearchWindow (kRegionVolume=4096 cells max, vs. Tier 0/1's pre-M6
	// window of ~27,225 cells -- WindowHalfExtentVoxels/HalfHeightVoxels
	// above), and the ROUTE-FINDING itself (which regions/portals to cross)
	// is a cheap Dijkstra over a few dozen portal nodes, not another
	// bounded voxel search -- so even with refine=true, the total fine-A*
	// work for a query spanning N regions is O(N) SMALL bounded searches
	// rather than the pre-M6 approach's repeated ~27,225-cell windowed
	// searches chained across many replans to slowly close a long distance.
	// See docs/status.md's measured before/after numbers.
	const vxc::HierarchicalPathResult Result = vxc::findHierarchicalPath(
		Impl->Tier1RegionGraph, Start, Goal, SolidFn, Impl->NavConfig, /*refine=*/true, Tier1PerRegionMaxExpansions);

	++Impl->Tier1HierarchicalCalls;
	const int64 ExpansionsThisCall =
		Result.entryExitExpansionsUsed + (Result.refined ? Result.concretePath.expansionsUsed : 0);
	Impl->Tier1HierarchicalExpansionsTotal += ExpansionsThisCall;

	if (!Result.corridor.found || !Result.refined)
	{
		// Diagnostic (docs/status.md M6 section -- kept permanently, not
		// just for this slice's own debugging): counts alive portals in
		// Start's/Goal's own regions directly from the graph, distinguishing
		// "this region has zero portals at all" (would mean the box's
		// vertical/horizontal extent doesn't actually reach usable terrain
		// here) from "portals exist but the corridor/entry/exit search
		// itself failed" (an expansion-cap/connectivity issue) -- both are
		// real, distinct failure modes this system can hit, and Verbose
		// alone wasn't enough to diagnose which one was happening when this
		// was first built (see docs/status.md's measured numbers).
		const vxc::RegionCoord StartRegion = vxc::regionOfVoxel(Start);
		const vxc::RegionCoord GoalRegion = vxc::regionOfVoxel(Goal);
		int32 StartPortalCount = 0, GoalPortalCount = 0;
		for (const vxc::Portal& P : Impl->Tier1RegionGraph.portals)
		{
			if (!P.alive) continue;
			if (P.region == StartRegion) ++StartPortalCount;
			if (P.region == GoalRegion) ++GoalPortalCount;
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelAgent %d Tier1 DIAGNOSTIC: corridor.found=%d refined=%d startRegion=(%lld,%lld,%lld) portals=%d ")
		       TEXT("goalRegion=(%lld,%lld,%lld) portals=%d start=(%lld,%lld,%lld) goal=(%lld,%lld,%lld)"),
		       AgentIndex, Result.corridor.found ? 1 : 0, Result.refined ? 1 : 0,
		       StartRegion.x, StartRegion.y, StartRegion.z, StartPortalCount,
		       GoalRegion.x, GoalRegion.y, GoalRegion.z, GoalPortalCount,
		       Start.x, Start.y, Start.z, Goal.x, Goal.y, Goal.z);

		// Regions disconnected in the abstract graph, or a stale-graph
		// refinement failure (header comment "Hierarchical query": refine
		// does NOT re-validate the abstract corridor itself against a
		// changed solidFn) -- fall back rather than leaving the agent stuck.
		return FallBackToFineSearch();
	}

	Agent.Waypoints.Reset();
	Agent.Waypoints.Reserve((int32)Result.concretePath.steps.size());
	for (const vxc::PathStep& Step : Result.concretePath.steps)
	{
		const VoxelCoords::FVoxelCoord Cell{Step.cell.x, Step.cell.y, Step.cell.z};
		Agent.Waypoints.Add(VoxelCoords::VoxelToWorldCenter(Cell));
	}
	Agent.WaypointIndex = 0;

	const UWorld* World = GetWorld();
	Agent.LastReplanTimeSeconds = World ? World->GetTimeSeconds() : Agent.LastReplanTimeSeconds;
	Agent.LastPlanGoalWorld = GoalWorldPos;

	if (Impl->Paths.IsValidIndex(AgentIndex))
	{
		Impl->Paths[AgentIndex] = Result.concretePath; // for pathStillValid revalidation, same as PlanPath
	}
	if (Impl->PathIsDigCapable.IsValidIndex(AgentIndex))
	{
		Impl->PathIsDigCapable[AgentIndex] = false; // Tier 1 is never dig-capable (NavConfig, see class comment)
	}

	UE_LOG(LogVoxelEarth, Verbose,
	       TEXT("VoxelAgent %d (Tier1 hierarchical): corridor cost=%lld portals=%d steps=%d expansions=%lld"),
	       AgentIndex, Result.corridor.totalCost, (int32)Result.corridor.portalIds.size(),
	       (int32)Result.concretePath.steps.size(), ExpansionsThisCall);

	return Agent.Waypoints.Num() > 0;
}

void UVoxelAgentSubsystem::MarkTerrainEditDirty(UVoxelWorldSubsystem& Terrain, int64 VoxelX, int64 VoxelY, int64 VoxelZ)
{
	if (!Impl || !Impl->bTier1GraphValid)
	{
		return;
	}
	const vxc::PathCoord VoxelPath{VoxelX, VoxelY, VoxelZ};
	const vxc::RegionCoord Region = vxc::regionOfVoxel(VoxelPath);
	if (!vxc::regionInBounds(Region, Impl->Tier1RegionGraph.minRegion, Impl->Tier1RegionGraph.maxRegion))
	{
		return; // outside the graph's current coverage -- nothing to keep in sync
	}

	UVoxelWorldSubsystem* TerrainPtr = &Terrain;
	const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
	};
	vxc::markRegionDirty(Impl->Tier1RegionGraph, Region, SolidFn, Impl->NavConfig, Tier1GraphIntraMaxExpansions);

	UE_LOG(LogVoxelEarth, Verbose,
	       TEXT("VoxelSwarm Tier1 region graph: dirtied region (%lld,%lld,%lld) after an NPC edit at voxel (%lld,%lld,%lld)."),
	       Region.x, Region.y, Region.z, VoxelX, VoxelY, VoxelZ);
}

void UVoxelAgentSubsystem::PollWorldEditsForTier1DirtyRegions(UVoxelWorldSubsystem& Terrain)
{
	if (!Impl || !Impl->bTier1GraphValid)
	{
		return;
	}
	const uint64 CurrentLogSize = Terrain.GetLogSize();
	if (CurrentLogSize == Impl->LastSeenEditLogSize)
	{
		return; // nothing new since the last check (or the graph's own last (re)build)
	}
	Impl->LastSeenEditLogSize = CurrentLogSize;

	// KNOWN v0 LIMITATION (doctrine-mandated fallback -- see
	// UVoxelAgentSubsystem.h's doc comment on this method): no delegate
	// exists on UVoxelWorldSubsystem reporting WHICH cells a player dig/
	// place or M5 structural-collapse edit touched, and that file is out of
	// scope for this slice to modify (another agent owns it this run). This
	// is the agent-side best-effort fallback the doctrine anticipates:
	// whenever the edit log has grown, dirty the region around every
	// currently-tracked Tier 0/1 agent's OWN position -- markRegionDirty's
	// own up-to-6-neighbor sweep (regiongraph.h) already extends that to
	// immediately adjacent regions too. This reliably catches the scenarios
	// that actually matter to this system (an edit landing near an agent's
	// CURRENT path), at the cost of an occasional false-positive dirty (a
	// region re-scanned that in fact wasn't touched) whenever the real edit
	// landed somewhere else entirely. Follow-up: a precise coordinate-level
	// notification (e.g. a delegate on UVoxelWorldSubsystem broadcasting
	// touched bricks) would make this exact instead of approximate.
	UVoxelWorldSubsystem* TerrainPtr = &Terrain;
	const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
	{
		return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
	};

	TArray<vxc::RegionCoord> DirtiedThisPoll;
	for (const FVoxelAgent& Agent : Agents)
	{
		if (Agent.Tier == EVoxelAgentTier::Tier2_Statistical)
		{
			continue; // Tier 2 never paths at all -- nothing to keep in sync for it
		}
		const VoxelCoords::FVoxelCoord AgentVoxel = VoxelCoords::WorldToVoxel(Agent.Position);
		const vxc::RegionCoord Region = vxc::regionOfVoxel(vxc::PathCoord{AgentVoxel.X, AgentVoxel.Y, AgentVoxel.Z});
		if (!vxc::regionInBounds(Region, Impl->Tier1RegionGraph.minRegion, Impl->Tier1RegionGraph.maxRegion))
		{
			continue;
		}
		bool bAlready = false;
		for (const vxc::RegionCoord& R : DirtiedThisPoll)
		{
			if (R.x == Region.x && R.y == Region.y && R.z == Region.z)
			{
				bAlready = true;
				break;
			}
		}
		if (bAlready)
		{
			continue;
		}
		DirtiedThisPoll.Add(Region);
		vxc::markRegionDirty(Impl->Tier1RegionGraph, Region, SolidFn, Impl->NavConfig, Tier1GraphIntraMaxExpansions);
	}

	if (DirtiedThisPoll.Num() > 0)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSwarm Tier1 region graph: edit-log grew (size %llu) -- dirtied %d region(s) near active ")
		       TEXT("agents (best-effort -- no precise edit-location delegate available, see doc comment)."),
		       (unsigned long long)CurrentLogSize, DirtiedThisPoll.Num());
	}
}

bool UVoxelAgentSubsystem::TryExecuteWaypointEdit(int32 AgentIndex, UVoxelWorldSubsystem& Terrain,
                                                    const FVector& PlayerWorldPos, int32& InOutEditBudget)
{
	if (!Impl || !Agents.IsValidIndex(AgentIndex) || !Impl->Paths.IsValidIndex(AgentIndex))
	{
		return true;
	}
	FVoxelAgent& Agent = Agents[AgentIndex];
	const vxc::PathResult& Path = Impl->Paths[AgentIndex];
	if (Agent.WaypointIndex < 0 || Agent.WaypointIndex >= (int32)Path.steps.size())
	{
		return true; // nothing queued -- exhausted path, TickTier0's own exhausted check handles a replan
	}
	const vxc::PathStep& Step = Path.steps[Agent.WaypointIndex];
	if (Step.action != vxc::Action::Mine && Step.action != vxc::Action::Bridge)
	{
		return true; // ordinary move -- nothing to edit
	}

	// Safety valve, defensive (PlanPath already routes Tier 0 to NavConfig
	// -- which never produces a Mine/Bridge step -- the instant this cvar
	// goes off; this catches the narrow window between a toggle and this
	// agent's next replan -- see the cvar's doc comment).
	if (!CVarVoxelNPCDigEnabled.GetValueOnGameThread())
	{
		return false;
	}
	// Tier restriction (VoxelAgentSubsystem.h class comment "Rate limiting
	// -- tier restriction"): should be unreachable in practice (DigConfig,
	// the only config that ever produces a Mine/Bridge step, is only used
	// while Agent.Tier == Tier0_Embodied -- see PlanPath), but fail OPEN
	// (let the agent through) rather than leaving it stuck forever if this
	// invariant is ever violated by a future change.
	if (Agent.Tier != EVoxelAgentTier::Tier0_Embodied)
	{
		return true;
	}

	const VoxelCoords::FVoxelCoord DestCell{Step.cell.x, Step.cell.y, Step.cell.z};
	const VoxelCoords::FVoxelCoord AffectedCell{Step.affectedCell.x, Step.affectedCell.y, Step.affectedCell.z};

	// Re-derive "is the edit already done" from LIVE world state rather than
	// tracking a separate per-waypoint done-flag: another agent (or the
	// player) may have already dug/placed this exact cell, and this check
	// makes that free to detect -- no extra bookkeeping, and no risk of
	// double-charging the rate limit for an edit that already landed.
	if (Step.action == vxc::Action::Mine)
	{
		if (!Terrain.IsSolidAtVoxel(DestCell.X, DestCell.Y, DestCell.Z))
		{
			return true; // already dug -- proceed
		}
	}
	else // Bridge
	{
		if (Terrain.IsSolidAtVoxel(AffectedCell.X, AffectedCell.Y, AffectedCell.Z))
		{
			return true; // scaffold already present -- proceed
		}
	}

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (Agent.LastDigTimeSeconds >= 0.0 && (Now - Agent.LastDigTimeSeconds) < NPCDigCooldownSeconds)
	{
		return false; // per-agent cooldown (rate limit 1/3) -- still recovering from the last edit
	}
	if (InOutEditBudget <= 0)
	{
		return false; // global per-tick cap (rate limit 2/3) -- spent for this Tick
	}

	bool bApplied = false;
	VoxelCoords::FVoxelCoord EditedCell = DestCell;
	if (Step.action == vxc::Action::Mine)
	{
		// Aim from the CENTER of the agent's own current voxel (not its feet
		// position -- see FVoxelAgent::Position's doc comment) straight at
		// the destination voxel's center. classifyMove only ever produces a
		// Mine step for one of pathfind.h's 18 fixed neighbor offsets (at
		// most sqrt(2) voxels away -- Jump never mines), so this ray is
		// always short and unobstructed: the destination is the nearest
		// solid voxel in that exact direction, which is exactly what
		// UVoxelWorldSubsystem::TryDig's raycast-and-dig-the-first-hit
		// contract needs. SizeVoxels=1 (MinCubeSizeVoxels) digs EXACTLY the
		// hit voxel (no bias cube growth -- see TryDig's doc comment), i.e.
		// exactly DestCell, matching the ONE-voxel-per-step body model
		// pathfind.h documents.
		const VoxelCoords::FVoxelCoord AgentVoxel = VoxelCoords::WorldToVoxel(Agent.Position);
		const FVector EyeWorld = VoxelCoords::VoxelToWorldCenter(AgentVoxel);
		const FVector DestCenterWorld = VoxelCoords::VoxelToWorldCenter(DestCell);
		bApplied = Terrain.TryDig(EyeWorld, DestCenterWorld - EyeWorld, UVoxelWorldSubsystem::MinCubeSizeVoxels);
	}
	else // Bridge
	{
		// UVoxelWorldSubsystem::TryPlace can only snap a new cube against an
		// EXISTING solid face hit by its raycast (m1-plan.md "Place" row
		// semantics) -- it has no "place a fully floating voxel" mode. A
		// Bridge's AffectedCell (the scaffold destination -- see pathfind.h's
		// PathStep::affectedCell doc comment) is air by definition (that is
		// what "unsupported" means), so the ray has to be aimed at one of
		// AffectedCell's own solid FACE-NEIGHBORS instead, found by direct
		// query (IsSolidAtVoxel) rather than assumed -- e.g. for a FLAT
		// bridge the floor under the agent's OWN current cell is always
		// exactly one such neighbor, but a STEP-UP bridge has no such static
		// guarantee, so this scans all six faces. A one-voxel-long ray from
		// inside AffectedCell straight at that neighbor's center lands the
		// DDA's hit EXACTLY on AffectedCell as the "last empty voxel before
		// the hit" (Hit.px/py/pz), which is what TryPlace's SizeVoxels=1
		// (MinCubeSizeVoxels) placement uses as its cube's sole voxel.
		EditedCell = AffectedCell;
		static const int64 kFaceOffsets[6][3] = {
			{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
		};
		bool bFoundNeighbor = false;
		FVector NeighborDir = FVector::ZeroVector;
		for (const auto& Off : kFaceOffsets)
		{
			const int64 Nx = AffectedCell.X + Off[0];
			const int64 Ny = AffectedCell.Y + Off[1];
			const int64 Nz = AffectedCell.Z + Off[2];
			if (Terrain.IsSolidAtVoxel(Nx, Ny, Nz))
			{
				NeighborDir = FVector((double)Off[0], (double)Off[1], (double)Off[2]);
				bFoundNeighbor = true;
				break;
			}
		}
		if (!bFoundNeighbor)
		{
			// KNOWN v0 LIMITATION (follow-up -- see docs/status.md M6
			// section): a Bridge destination with no solid face-neighbor on
			// ANY side (a fully floating scaffold, e.g. bridging the middle
			// of a wide chasm two-plus voxels from either wall) cannot be
			// placed through today's public dig/place API at all --
			// UVoxelWorldSubsystem would need a direct point-target stamp
			// entry point (mirroring the already-Impl-only StampVoxels this
			// subsystem is not allowed to reach -- see VoxelAgentSubsystem.h's
			// class comment). The agent simply waits here (rate-limited
			// retries, same as a cooldown miss) rather than silently
			// skipping the edit; pathStillValid keeps re-validating this
			// same Bridge step as still correct, so it never gets stuck any
			// WORSE than "paused".
			UE_LOG(LogVoxelEarth, Verbose,
			       TEXT("VoxelAgent %d: Bridge at (%lld,%lld,%lld) has no solid face-neighbor to place against -- waiting."),
			       AgentIndex, AffectedCell.X, AffectedCell.Y, AffectedCell.Z);
			return false;
		}

		const FVector AffectedCenterWorld = VoxelCoords::VoxelToWorldCenter(AffectedCell);
		// PlayerWorldPos, not Agent.Position, for the overlap-reject check's
		// PlayerActorLocation argument -- TryPlace's contract is "never
		// place a cube overlapping the PLAYER's own collision box"; an NPC
		// edit should still honor that (never trap the real player), not
		// bypass it.
		bApplied = Terrain.TryPlace(AffectedCenterWorld, NeighborDir, UVoxelWorldSubsystem::MinCubeSizeVoxels,
		                             BridgeScaffoldMaterialId, PlayerWorldPos);
	}

	if (bApplied)
	{
		Agent.LastDigTimeSeconds = Now;
		--InOutEditBudget;

		// Dirty invalidation, NPC-edit case (docs/status.md M6 section
		// "Tier-1 hierarchical planning + NPC replication"): this subsystem
		// IS the one making this edit and knows the exact cell, so the
		// Tier 1 region graph (if built and covering it) is kept precisely
		// in sync the instant the edit lands -- no polling needed for this
		// case (see PollWorldEditsForTier1DirtyRegions for the OTHER case,
		// player digs/places and M5 collapse, which this subsystem doesn't
		// directly cause).
		MarkTerrainEditDirty(Terrain, EditedCell.X, EditedCell.Y, EditedCell.Z);

		// %hs (not %s): vxc::actionName returns a narrow `const char*`
		// (voxelcore/pathfind.h is engine-free, ANSI-only), while TEXT(...)
		// format strings are TCHAR -- UE's format-string sanitizer requires
		// the narrow specifier for a narrow argument.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelAgent %d: %hs at (%lld,%lld,%lld) -- authoritative edit applied (edit budget %d/%d remaining this tick)."),
		       AgentIndex, vxc::actionName(Step.action), EditedCell.X, EditedCell.Y, EditedCell.Z, InOutEditBudget,
		       MaxNPCEditsPerTick);
	}
	return bApplied;
}

void UVoxelAgentSubsystem::GroundSnap(int32 AgentIndex, UVoxelWorldSubsystem& Terrain)
{
	if (!Agents.IsValidIndex(AgentIndex))
	{
		return;
	}
	FVoxelAgent& Agent = Agents[AgentIndex];

	// Tier-aware cadence (class comment / Tier1/Tier2GroundSnapIntervalSeconds
	// doc comments): Tier 0 has no interval at all (0.0 == always due);
	// Tier 1/2 gate on LastGroundSnapTimeSeconds so hundreds of agents don't
	// all pay a raycast every tick.
	double IntervalSeconds = 0.0;
	switch (Agent.Tier)
	{
	case EVoxelAgentTier::Tier1_Abstract:
		IntervalSeconds = Tier1GroundSnapIntervalSeconds;
		break;
	case EVoxelAgentTier::Tier2_Statistical:
		IntervalSeconds = Tier2GroundSnapIntervalSeconds;
		break;
	default:
		break;
	}

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (IntervalSeconds > 0.0 && Agent.LastGroundSnapTimeSeconds >= 0.0 &&
	    (Now - Agent.LastGroundSnapTimeSeconds) < IntervalSeconds)
	{
		return; // not due yet
	}

	// Same style as AVoxelDebris::Tick's settle raycast: cast down from
	// slightly above the agent so a body that has drifted a touch below
	// the surface between updates (e.g. after crossing a StepDown) still
	// finds the surface above it and clamps back up rather than tunnelling.
	FVector HitCentre, PrevCentre;
	const FVector Start = Agent.Position + FVector(0.0, 0.0, VoxelCoords::VoxelSizeUU * 4.0);
	constexpr double MaxDistUU = 100000.0; // 1km, generous -- terrain is always below a ground-swarm agent
	if (Terrain.RaycastVoxelWorld(Start, FVector(0.0, 0.0, -1.0), MaxDistUU, HitCentre, PrevCentre))
	{
		Agent.Position.Z = HitCentre.Z + VoxelCoords::VoxelSizeUU * 0.5; // top face of the hit voxel -- feet rest here
	}
	Agent.LastGroundSnapTimeSeconds = Now;
}

void UVoxelAgentSubsystem::UpdateInstanceTransform(int32 AgentIndex) const
{
	if (!AgentISM || !Agents.IsValidIndex(AgentIndex))
	{
		return;
	}
	const FVoxelAgent& Agent = Agents[AgentIndex];
	if (Agent.InstanceIndex == INDEX_NONE)
	{
		return;
	}

	const double InstanceScaleXY = AgentBodyWidthUU / 100.0;
	const double InstanceScaleZ = (AgentHalfHeightUU * 2.0) / 100.0;
	const FVector Translation = Agent.Position + FVector(0.0, 0.0, AgentHalfHeightUU);
	const FTransform InstanceTransform(FRotator::ZeroRotator, Translation,
	                                    FVector(InstanceScaleXY, InstanceScaleXY, InstanceScaleZ));
	AgentISM->UpdateInstanceTransform(Agent.InstanceIndex, InstanceTransform, /*bWorldSpace=*/true,
	                                   /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
}

void UVoxelAgentSubsystem::TickTier0(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
                                      const FVector& GoalWorldPos, double DeltaSeconds, int32& InOutReplanBudget,
                                      int32& InOutEditBudget)
{
	FVoxelAgent& Agent = Agents[AgentIndex];

	// Pursuit standoff (class comment "Pursuit behavior"): checked against
	// the REAL player position (not the ground-projected goal) every tick,
	// for every tier, so an agent that closes in never actually enters the
	// player's own cell.
	const double DistToPlayer = FVector::Dist(Agent.Position, PlayerWorldPos);
	if (Agent.bIdleAtStandoff)
	{
		if (DistToPlayer > StandoffResumeUU)
		{
			Agent.bIdleAtStandoff = false;
		}
	}
	else if (DistToPlayer <= StandoffRadiusUU)
	{
		Agent.bIdleAtStandoff = true;
	}

	bool bNeedsReplan = false;
	if (!Agent.bIdleAtStandoff)
	{
		const bool bPathExhausted = Agent.Waypoints.Num() == 0 || Agent.WaypointIndex >= Agent.Waypoints.Num();
		if (bPathExhausted)
		{
			bNeedsReplan = true;
		}
		else if (FVector::DistSquared(Agent.LastPlanGoalWorld, GoalWorldPos) >
		         FMath::Square(ReplanGoalDriftThresholdUU))
		{
			bNeedsReplan = true;
		}
		else if (Impl && Impl->Paths.IsValidIndex(AgentIndex))
		{
			// Tier 0 revalidates every tick it doesn't already need a
			// replan for another reason -- cheap: pathStillValid only
			// re-classifies the REMAINING steps of an already-windowed
			// (small) path, at most a few dozen cells. Digging-while-pathing
			// (docs/status.md M6 section) makes this ALSO the cross-agent
			// (and cross-player) invalidation mechanism "for free": any
			// terrain edit -- this agent's own successful Mine/Bridge,
			// another agent's, or a player dig -- changes what IsSolidAtVoxel
			// reports at the touched cell, so the very next tick's
			// pathStillValid re-classification of that step will disagree
			// with what was RECORDED for it and force a replan. A future
			// per-dirty-brick invalidation (regiongraph.h's markRegionDirty,
			// merged but NOT a dependency of this slice -- see class
			// comment) would replace this "every agent re-scans its own
			// path every tick" approach with "only agents whose path
			// touches a just-dirtied brick get invalidated" -- strictly a
			// cost optimization, not a correctness change, since this
			// per-tick scan is already sound.
			UVoxelWorldSubsystem* TerrainPtr = &Terrain;
			const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
			{
				return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
			};
			if (!vxc::pathStillValid(Impl->Paths[AgentIndex], SolidFn, Impl->DigConfig))
			{
				bNeedsReplan = true;
			}
		}
	}

	if (bNeedsReplan && InOutReplanBudget > 0)
	{
		// Budget is spent on the ATTEMPT (a full findPath call), regardless
		// of whether it finds a usable path -- the cost was already paid
		// either way (class comment "replan budget").
		PlanPath(AgentIndex, Terrain, GoalWorldPos);
		--InOutReplanBudget;
	}

	// Digging-while-pathing (docs/status.md M6 section): if the waypoint the
	// agent is currently walking toward requires a Mine/Bridge edit that
	// hasn't landed yet, TryExecuteWaypointEdit either applies it now
	// (rate limits/cvar permitting) or returns false -- in which case the
	// agent HOLDS its current position rather than advance into still-solid
	// (Mine) or still-unsupported (Bridge) terrain. This is also what makes
	// "mining takes time" visible: the agent visibly pauses at the wall
	// face for NPCDigCooldownSeconds between voxels, not just a background
	// bookkeeping delay.
	bool bBlockedByPendingEdit = false;
	if (!Agent.bIdleAtStandoff)
	{
		bBlockedByPendingEdit = !TryExecuteWaypointEdit(AgentIndex, Terrain, PlayerWorldPos, InOutEditBudget);
	}

	if (!Agent.bIdleAtStandoff && !bBlockedByPendingEdit)
	{
		AdvanceAlongWaypoints(Agent, DeltaSeconds, MoveSpeedUUPerSec, WaypointAdvanceThresholdUU);
	}

	GroundSnap(AgentIndex, Terrain);
}

void UVoxelAgentSubsystem::TickTier1(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
                                      const FVector& GoalWorldPos, double DeltaSeconds, int32& InOutReplanBudget)
{
	FVoxelAgent& Agent = Agents[AgentIndex];

	const double DistToPlayer = FVector::Dist(Agent.Position, PlayerWorldPos);
	if (Agent.bIdleAtStandoff)
	{
		if (DistToPlayer > StandoffResumeUU)
		{
			Agent.bIdleAtStandoff = false;
		}
	}
	else if (DistToPlayer <= StandoffRadiusUU)
	{
		Agent.bIdleAtStandoff = true;
	}

	// Digging-while-pathing tier restriction (VoxelAgentSubsystem.h class
	// comment "Rate limiting -- tier restriction"): Tier 1 never executes an
	// edit, so it must never be left holding (let alone ADVANCING along) a
	// path that needs one. The only way that can happen is inheriting one
	// from a just-demoted Tier 0 agent (PlanPath tags every freshly-planned
	// path via FVoxelAgentImpl::PathIsDigCapable) -- checked BEFORE
	// AdvanceAlongWaypoints below (unlike the exhausted/drift/pathStillValid
	// checks further down, which run after -- those never risk stepping
	// onto bad terrain, this one specifically would) so the very tick a
	// Tier 0 agent demotes mid-tunnel, it holds position instead of
	// AdvanceAlongWaypoints -- which has no collision/edit awareness of its
	// own -- taking one step toward a Mine/Bridge waypoint Tier 1 will never
	// execute.
	const bool bInheritedDigCapablePath =
		Impl && Impl->PathIsDigCapable.IsValidIndex(AgentIndex) && Impl->PathIsDigCapable[AgentIndex];

	if (!Agent.bIdleAtStandoff && !bInheritedDigCapablePath)
	{
		AdvanceAlongWaypoints(Agent, DeltaSeconds, MoveSpeedUUPerSec, WaypointAdvanceThresholdUU);
	}

	// Coarser cadence (class comment): a path-exhausted agent still replans
	// immediately (budget permitting) -- an idle Tier 1 agent standing
	// still would look broken -- but the goal-drift/pathStillValid checks
	// that can ALSO trigger a replan only run once Tier1ReplanIntervalSeconds
	// has actually elapsed since the last one, which is both the cadence
	// throttle and (since pathStillValid is the relatively expensive part)
	// the cost-saving measure the "cheaper... reuse cached paths" doc
	// comment describes.
	const bool bPathExhausted = Agent.Waypoints.Num() == 0 || Agent.WaypointIndex >= Agent.Waypoints.Num();
	bool bNeedsReplan = bPathExhausted || bInheritedDigCapablePath;

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const bool bCooldownElapsed = (Now - Agent.LastReplanTimeSeconds) >= Tier1ReplanIntervalSeconds;

	if (!bNeedsReplan && !Agent.bIdleAtStandoff && bCooldownElapsed)
	{
		const bool bGoalDrifted =
			FVector::DistSquared(Agent.LastPlanGoalWorld, GoalWorldPos) > FMath::Square(ReplanGoalDriftThresholdUU);
		bool bPathInvalid = false;
		if (!bGoalDrifted && Impl && Impl->Paths.IsValidIndex(AgentIndex))
		{
			UVoxelWorldSubsystem* TerrainPtr = &Terrain;
			const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
			{
				return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
			};
			// NavConfig, not DigConfig: this agent's own cached path was
			// planned with NavConfig (Tier 1 never gets DigConfig -- see
			// PlanPath), so it must be revalidated against the SAME config
			// it was classified with, exactly like Tier 0's identical
			// pairing above.
			bPathInvalid = !vxc::pathStillValid(Impl->Paths[AgentIndex], SolidFn, Impl->NavConfig);
		}
		bNeedsReplan = bGoalDrifted || bPathInvalid;
	}

	if (!Agent.bIdleAtStandoff && bNeedsReplan && InOutReplanBudget > 0)
	{
		// M6 gap closure (docs/status.md M6 section): PlanPathTier1 replaces
		// the raw vxc::findPath call this used to make directly -- see its
		// doc comment (UVoxelAgentSubsystem.h) for the hierarchical-vs-fine
		// dispatch/fallback logic.
		PlanPathTier1(AgentIndex, Terrain, PlayerWorldPos, GoalWorldPos);
		--InOutReplanBudget;
	}

	GroundSnap(AgentIndex, Terrain);
}

void UVoxelAgentSubsystem::TickTier2(int32 AgentIndex, UVoxelWorldSubsystem& Terrain, const FVector& PlayerWorldPos,
                                      double DeltaSeconds)
{
	FVoxelAgent& Agent = Agents[AgentIndex];

	const double DistToPlayer = FVector::Dist(Agent.Position, PlayerWorldPos);
	if (Agent.bIdleAtStandoff)
	{
		if (DistToPlayer > StandoffResumeUU)
		{
			Agent.bIdleAtStandoff = false;
		}
	}
	else if (DistToPlayer <= StandoffRadiusUU)
	{
		Agent.bIdleAtStandoff = true;
	}

	// Tier 2 never holds a path (no A* at all -- class comment): drop any
	// stale Waypoints left over from a prior Tier 0/1 stint so a later
	// promotion starts clean rather than resuming a now-ancient path.
	if (Agent.Waypoints.Num() > 0)
	{
		Agent.Waypoints.Reset();
		Agent.WaypointIndex = 0;
	}

	if (!Agent.bIdleAtStandoff)
	{
		const UWorld* World = GetWorld();
		const double Now = World ? World->GetTimeSeconds() : 0.0;
		SteerVoxelAgentTier2(Agent, PlayerWorldPos, DeltaSeconds, MoveSpeedUUPerSec, Tier2WanderAmplitudeUUPerSec, Now,
		                      AgentIndex);
	}

	GroundSnap(AgentIndex, Terrain);
}

void UVoxelAgentSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (World->GetNetMode() == NM_Client)
	{
		// M6 gap closure (docs/status.md M6 section "Tier-1 hierarchical
		// planning + NPC replication"): a client's OWN Agents/Impl stay
		// empty forever (server-authoritative simulation, class comment) --
		// what a client renders instead comes from ClientAgents, the
		// replicated-state mirror AVoxelAgentReplicator feeds via
		// ApplyReplicatedAgentSnapshot. This is intentionally a SEPARATE,
		// much lighter code path (no pathfinding, no world edits -- just
		// interpolating already-decided positions) rather than sharing the
		// authority branch below.
		TickClientReplicatedAgents(DeltaTime);
		return;
	}

	if (Agents.Num() == 0 || !Impl)
	{
		return;
	}

	UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* PlayerPawn = PC ? PC->GetPawn() : nullptr;
	if (!Terrain || !PlayerPawn)
	{
		return; // nothing to pursue yet -- try again next tick
	}

	const FVector PlayerWorldPos = PlayerPawn->GetActorLocation();

	// Pursuit target (class comment "Pursuit behavior"): the player's
	// current column, Z-PROJECTED ONTO THE TERRAIN SURFACE rather than the
	// player's literal (possibly airborne -- AVoxelEarthFlyPawn can fly
	// freely) position. pathfind.h's v0 Climb/Fall model has no "this is
	// an open shaft vs. a genuine wall" distinction (a documented header
	// simplification -- Climb only checks the destination is air, not that
	// there's anything to climb), so pathing agents literally toward a
	// hovering player would have them Climb straight up through open sky
	// one voxel at a time. A nav-only GROUND swarm should chase where the
	// player is STANDING OVER, not the camera. LOD tiering + standoff below
	// still use the player's REAL (3D) position, so a player who flies away
	// is correctly seen as "far" even while hovering directly overhead.
	const double GroundSurfaceUU = Terrain->GetSurfaceHeightUU(PlayerWorldPos.X, PlayerWorldPos.Y);
	const FVector GoalWorldPos(PlayerWorldPos.X, PlayerWorldPos.Y, GroundSurfaceUU);

	// M6 gap closure (docs/status.md M6 section): dirty invalidation for
	// edits this subsystem did NOT itself make (player digs/places, M5
	// structural-collapse removals) -- one poll/dirty-scan pass per Tick,
	// before any agent this Tick reads the graph. See the method's own doc
	// comment (UVoxelAgentSubsystem.h) for why this is a best-effort,
	// position-based heuristic rather than a precise coordinate-level hook.
	// M6 Tier-1 enablement (regiongraph.h header comment "Build cost"): spend
	// at most Tier1GraphBuildBudgetMsPerTick of THIS frame on the region-graph
	// build, every tick, whether or not any agent happens to plan this frame.
	// Driving it from Tick rather than only from PlanPathTier1 is what keeps
	// the build making steady progress instead of arriving in bursts on the
	// frames that also happen to be replanning.
	AdvanceTier1RegionGraphBuild(*Terrain, PlayerWorldPos);

	PollWorldEditsForTier1DirtyRegions(*Terrain);

	int32 ReplanBudget = MaxTier0ReplansPerTick;
	// Digging-while-pathing global per-tick cap (VoxelAgentSubsystem.h "Rate
	// limiting" -- limit 2/3): shared across every Tier 0 agent this Tick,
	// same "one shared counter, claimed first-come across the loop below"
	// shape as ReplanBudget above.
	int32 EditBudget = MaxNPCEditsPerTick;
	const double DeltaSeconds = double(DeltaTime);

	int32 Tier0Count = 0, Tier1Count = 0, Tier2Count = 0;
	double DistanceSumUU = 0.0;

	for (int32 i = 0; i < Agents.Num(); ++i)
	{
		FVoxelAgent& Agent = Agents[i];

		// Bucket by distance-to-player, WITH hysteresis (VoxelAgent.h's
		// ComputeNextVoxelAgentTier) -- distance measured against the
		// agent's position as of the START of this tick (last tick's
		// settled position), same "evaluate before moving" order every
		// per-agent update in this loop uses.
		const double DistanceToPlayerUU = FVector::Dist(Agent.Position, PlayerWorldPos);
		Agent.Tier = ComputeNextVoxelAgentTier(Agent.Tier, DistanceToPlayerUU, Tier0EnterUU, Tier0ExitUU, Tier1EnterUU,
		                                        Tier1ExitUU);

		switch (Agent.Tier)
		{
		case EVoxelAgentTier::Tier0_Embodied:
			TickTier0(i, *Terrain, PlayerWorldPos, GoalWorldPos, DeltaSeconds, ReplanBudget, EditBudget);
			++Tier0Count;
			break;
		case EVoxelAgentTier::Tier1_Abstract:
			TickTier1(i, *Terrain, PlayerWorldPos, GoalWorldPos, DeltaSeconds, ReplanBudget);
			++Tier1Count;
			break;
		case EVoxelAgentTier::Tier2_Statistical:
			TickTier2(i, *Terrain, PlayerWorldPos, DeltaSeconds);
			++Tier2Count;
			break;
		}

		DistanceSumUU += DistanceToPlayerUU;
		UpdateInstanceTransform(i);
	}

	// Digging-while-pathing rate-limit evidence (docs/status.md M6 section
	// verification): only logged on ticks that actually spent budget, so a
	// quiet swarm (nobody currently mining) doesn't spam the log every
	// frame -- the per-edit "authoritative edit applied" line above
	// (TryExecuteWaypointEdit) already covers the individual-action detail.
	const int32 EditsThisTick = MaxNPCEditsPerTick - EditBudget;
	if (EditsThisTick > 0)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelSwarm NPC edits this tick: %d/%d (cap)."), EditsThisTick, MaxNPCEditsPerTick);
	}

	if (AgentISM)
	{
		// One MarkRenderStateDirty for the whole batch, not per-instance
		// (UpdateInstanceTransform above is called with
		// bMarkRenderStateDirty=false specifically so this is the only one
		// per tick) -- hundreds of agents updating every tick would
		// otherwise mean hundreds of redundant render-state invalidations.
		AgentISM->MarkRenderStateDirty();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	}

	LastSnapshot.Tier0Count = Tier0Count;
	LastSnapshot.Tier1Count = Tier1Count;
	LastSnapshot.Tier2Count = Tier2Count;
	LastSnapshot.MeanDistanceToPlayerUU = Agents.Num() > 0 ? DistanceSumUU / Agents.Num() : 0.0;

	const double Now = World->GetTimeSeconds();
	if (LastConvergenceLogTimeSeconds < 0.0 || (Now - LastConvergenceLogTimeSeconds) >= ConvergenceLogIntervalSeconds)
	{
		LastConvergenceLogTimeSeconds = Now;
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelSwarm: agents=%d tier0=%d tier1=%d tier2=%d meanDistToPlayer=%.1fm"),
		       Agents.Num(), Tier0Count, Tier1Count, Tier2Count, LastSnapshot.MeanDistanceToPlayerUU / 100.0);

		// Tier 1 planning-cost measurement (docs/status.md M6 section
		// "measure it" -- the before/after comparison this gap closure needs
		// to report). Same cadence/gating as the convergence log line above
		// so it doesn't spam a quiet swarm; only printed once either side has
		// actually been called at least once.
		if (Impl && (Impl->Tier1HierarchicalCalls > 0 || Impl->Tier1FineFallbackCalls > 0))
		{
			const double AvgHierarchicalExpansions = Impl->Tier1HierarchicalCalls > 0
				? double(Impl->Tier1HierarchicalExpansionsTotal) / double(Impl->Tier1HierarchicalCalls)
				: 0.0;
			const double AvgFineFallbackExpansions = Impl->Tier1FineFallbackCalls > 0
				? double(Impl->Tier1FineFallbackExpansionsTotal) / double(Impl->Tier1FineFallbackCalls)
				: 0.0;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("VoxelSwarm Tier1 planner: hierarchical calls=%lld totalExpansions=%lld avgExpansions=%.1f | ")
			       TEXT("fineFallback calls=%lld totalExpansions=%lld avgExpansions=%.1f"),
			       Impl->Tier1HierarchicalCalls, Impl->Tier1HierarchicalExpansionsTotal, AvgHierarchicalExpansions,
			       Impl->Tier1FineFallbackCalls, Impl->Tier1FineFallbackExpansionsTotal, AvgFineFallbackExpansions);
		}
	}
}

// --- M6 gap closure: NPC state replication (docs/status.md M6 section "Tier-1
// hierarchical planning + NPC replication") -----------------------------------

void UVoxelAgentSubsystem::CollectReplicationSnapshot(const FVector& OriginWorld, double RelevancyRadiusUU,
                                                        TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();

	// Wire format (own compact scheme, NOT vxc::ByteWriter -- this is
	// ephemeral UE-side network data, not terrain/edit-log data, so it has
	// no reason to route through voxel-core): int32 count, then per agent
	// int32 AgentId + int16 relative-position-per-axis (UU, relative to
	// OriginWorld) + uint8 Tier = 11 bytes/agent.
	struct FSample
	{
		int32 AgentId = 0;
		int16 Dx = 0, Dy = 0, Dz = 0;
		uint8 Tier = 0;
	};
	TArray<FSample> Samples;
	Samples.Reserve(Agents.Num());

	const double RadiusSq = RelevancyRadiusUU * RelevancyRadiusUU;
	for (int32 i = 0; i < Agents.Num(); ++i)
	{
		const FVoxelAgent& Agent = Agents[i];

		// Relevancy/culling (AVoxelAgentReplicator's class comment
		// "Relevancy/culling"): Tier 2 agents are NEVER replicated,
		// regardless of distance -- the tier scheduler's own hysteresis
		// (ComputeNextVoxelAgentTier) already guarantees a Tier 2 agent is
		// farther than Tier1ExitUU from EVERY player, and it is the least
		// cosmetically important tier to begin with (bounded steering, no
		// path, no interesting behavior to show a remote client). Tier 0/1
		// agents are included only within RelevancyRadiusUU of THIS
		// broadcast's origin.
		if (Agent.Tier == EVoxelAgentTier::Tier2_Statistical)
		{
			continue;
		}
		if (FVector::DistSquared(Agent.Position, OriginWorld) > RadiusSq)
		{
			continue;
		}

		const FVector Rel = Agent.Position - OriginWorld;
		FSample Sample;
		Sample.AgentId = i; // pool index -- stable for this slice (agents are never removed, see class comment)
		Sample.Dx = (int16)FMath::Clamp(FMath::RoundToInt(Rel.X), -32767, 32767);
		Sample.Dy = (int16)FMath::Clamp(FMath::RoundToInt(Rel.Y), -32767, 32767);
		Sample.Dz = (int16)FMath::Clamp(FMath::RoundToInt(Rel.Z), -32767, 32767);
		Sample.Tier = (uint8)Agent.Tier;
		Samples.Add(Sample);
	}

	FMemoryWriter Writer(OutBytes, /*bIsPersistent=*/false);
	int32 Count = Samples.Num();
	Writer << Count;
	for (FSample& Sample : Samples)
	{
		Writer << Sample.AgentId << Sample.Dx << Sample.Dy << Sample.Dz << Sample.Tier;
	}
}

void UVoxelAgentSubsystem::ApplyReplicatedAgentSnapshot(const FVector& OriginWorld, const TArray<uint8>& Bytes)
{
	if (Bytes.Num() < (int32)sizeof(int32))
	{
		return;
	}
	FMemoryReader Reader(Bytes, /*bIsPersistent=*/false);
	int32 Count = 0;
	Reader << Count;
	if (Count < 0)
	{
		return;
	}

	// Full-snapshot semantics (not an incremental add/remove event stream):
	// every currently-known ClientAgents entry starts this call marked
	// "not seen," and only ids actually present in THIS snapshot get
	// re-marked seen below -- see FVoxelAgentClientView::bSeenLastSnapshot's
	// doc comment for why (self-correcting even across a dropped/late
	// packet, robust to reordering).
	for (TPair<int32, FVoxelAgentClientView>& Pair : ClientAgents)
	{
		Pair.Value.bSeenLastSnapshot = false;
	}

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const double InstanceScaleXY = AgentBodyWidthUU / 100.0;
	const double InstanceScaleZ = (AgentHalfHeightUU * 2.0) / 100.0;

	for (int32 i = 0; i < Count; ++i)
	{
		if (Reader.IsError())
		{
			break;
		}
		int32 AgentId = 0;
		int16 Dx = 0, Dy = 0, Dz = 0;
		uint8 TierByte = 0;
		Reader << AgentId << Dx << Dy << Dz << TierByte;
		if (Reader.IsError())
		{
			break;
		}

		const FVector WorldPos = OriginWorld + FVector((double)Dx, (double)Dy, (double)Dz);
		FVoxelAgentClientView& View = ClientAgents.FindOrAdd(AgentId);

		if (View.InstanceIndex == INDEX_NONE && AgentISM == nullptr)
		{
			// Dedicated-server-style process with no viewport -- nothing to
			// render into (mirrors FVoxelAgent::InstanceIndex's identical
			// INDEX_NONE convention, OnWorldBeginPlay's doc comment). Still
			// track the position (harmless, cheap) in case a viewport shows
			// up later; just never touch AgentISM.
			View.PrevPosition = WorldPos;
			View.TargetPosition = WorldPos;
			View.PrevTimeSeconds = Now;
			View.TargetTimeSeconds = Now;
		}
		else if (View.InstanceIndex == INDEX_NONE)
		{
			// First time this AgentId has been seen -- give it a fresh ISM instance.
			View.PrevPosition = WorldPos;
			View.TargetPosition = WorldPos;
			View.PrevTimeSeconds = Now;
			View.TargetTimeSeconds = Now;
			const FTransform InstanceTransform(FRotator::ZeroRotator, WorldPos + FVector(0.0, 0.0, AgentHalfHeightUU),
			                                    FVector(InstanceScaleXY, InstanceScaleXY, InstanceScaleZ));
			View.InstanceIndex = AgentISM->AddInstance(InstanceTransform, /*bWorldSpace=*/true);
		}
		else
		{
			// Re-anchor the interpolation window from wherever the render
			// position CURRENTLY sits (not necessarily the old Target, if a
			// snapshot arrived a little early/late) toward the fresh target.
			const double OldAlpha = (View.TargetTimeSeconds > View.PrevTimeSeconds)
				? FMath::Clamp((Now - View.PrevTimeSeconds) / (View.TargetTimeSeconds - View.PrevTimeSeconds), 0.0, 1.0)
				: 1.0;
			View.PrevPosition = FMath::Lerp(View.PrevPosition, View.TargetPosition, OldAlpha);
			View.PrevTimeSeconds = Now;
			View.TargetPosition = WorldPos;
			View.TargetTimeSeconds = Now + ClientAgentInterpolationWindowSeconds;
		}
		View.Tier = TierByte;
		View.bSeenLastSnapshot = true;
	}

	// Entries not touched this snapshot have aged out of the server's
	// relevancy radius -- HIDE (zero-scale, not
	// UInstancedStaticMeshComponent::RemoveInstance) their instance rather
	// than tearing it down: RemoveInstance can shuffle other instances'
	// indices (typically a swap-with-last), which would silently invalidate
	// every OTHER tracked agent's cached InstanceIndex unless this class
	// also maintained a reverse index->id map to fix them up. Since AgentIds
	// never truly disappear in this slice (agents aren't despawned) and
	// commonly re-enter relevancy later (the player walks back toward them),
	// keeping a permanently-reserved, currently-invisible slot is simpler
	// and avoids that whole class of bug.
	bool bAnyHidden = false;
	for (TPair<int32, FVoxelAgentClientView>& Pair : ClientAgents)
	{
		if (!Pair.Value.bSeenLastSnapshot && AgentISM && Pair.Value.InstanceIndex != INDEX_NONE)
		{
			const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector::ZeroVector);
			AgentISM->UpdateInstanceTransform(Pair.Value.InstanceIndex, Hidden, /*bWorldSpace=*/true,
			                                    /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
			bAnyHidden = true;
		}
	}
	if (bAnyHidden && AgentISM)
	{
		AgentISM->MarkRenderStateDirty();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	}
}

void UVoxelAgentSubsystem::RunReplicationSelfTest()
{
	if (Agents.Num() == 0)
	{
		UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelReplicationSelfTest: no agents to test -- run with -VoxelSwarmTest first."));
		return;
	}

	// Origin = the first agent's own position, NOT world zero: production
	// (AVoxelAgentReplicator::Tick) always anchors OriginWorld at a live
	// player's location, which sits close (a few meters at most) to the
	// ground-snapped agents it's broadcasting -- keeping every relative
	// offset comfortably inside int16 range (+-32767UU = +-327m). World
	// zero would NOT be representative here: this world's absolute terrain
	// elevation is ~1000m+ at this seed, so an agent-Z-minus-zero offset
	// overflows int16 and quantizes to garbage -- a self-test artifact of a
	// badly chosen origin, not a wire-format bug (confirmed by re-running
	// with a realistic origin, see docs/status.md's measured results).
	const FVector OriginWorld = Agents[0].Position;
	TArray<uint8> Bytes;
	CollectReplicationSnapshot(OriginWorld, 1.0e9, Bytes);
	ApplyReplicatedAgentSnapshot(OriginWorld, Bytes);

	int32 Compared = 0;
	double MaxDeltaUU = 0.0;
	for (int32 i = 0; i < Agents.Num(); ++i)
	{
		if (Agents[i].Tier == EVoxelAgentTier::Tier2_Statistical)
		{
			continue; // never replicated by design -- nothing to compare
		}
		const FVoxelAgentClientView* View = ClientAgents.Find(i);
		if (!View)
		{
			UE_LOG(LogVoxelEarth, Warning, TEXT("VoxelReplicationSelfTest: agent %d missing from ClientAgents after apply!"), i);
			continue;
		}
		const double DeltaUU = FVector::Dist(Agents[i].Position, View->TargetPosition);
		MaxDeltaUU = FMath::Max(MaxDeltaUU, DeltaUU);
		++Compared;
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelReplicationSelfTest: agent %d authoritative=(%.2f,%.2f,%.2f) replicated=(%.2f,%.2f,%.2f) ")
		       TEXT("delta=%.3fUU tier(auth=%d,rep=%d)"),
		       i, Agents[i].Position.X, Agents[i].Position.Y, Agents[i].Position.Z, View->TargetPosition.X,
		       View->TargetPosition.Y, View->TargetPosition.Z, DeltaUU, (int32)Agents[i].Tier, (int32)View->Tier);
	}
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelReplicationSelfTest: compared %d/%d agents, maxDelta=%.3fUU (int16 UU quantization rounding, ")
	       TEXT("expect <=~0.87UU worst case)."),
	       Compared, Agents.Num(), MaxDeltaUU);
}

void UVoxelAgentSubsystem::TickClientReplicatedAgents(float DeltaSeconds)
{
	(void)DeltaSeconds; // interpolation here is time-stamp-based (Now vs Prev/TargetTimeSeconds), not delta-accumulated
	if (ClientAgents.Num() == 0 || !AgentISM)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const double InstanceScaleXY = AgentBodyWidthUU / 100.0;
	const double InstanceScaleZ = (AgentHalfHeightUU * 2.0) / 100.0;

	bool bAnyUpdated = false;
	for (TPair<int32, FVoxelAgentClientView>& Pair : ClientAgents)
	{
		FVoxelAgentClientView& View = Pair.Value;
		if (View.InstanceIndex == INDEX_NONE || !View.bSeenLastSnapshot)
		{
			continue; // no viewport, or hidden (aged out of relevancy) -- ApplyReplicatedAgentSnapshot already handled it
		}
		const double Alpha = (View.TargetTimeSeconds > View.PrevTimeSeconds)
			? FMath::Clamp((Now - View.PrevTimeSeconds) / (View.TargetTimeSeconds - View.PrevTimeSeconds), 0.0, 1.0)
			: 1.0;
		const FVector RenderPos = FMath::Lerp(View.PrevPosition, View.TargetPosition, Alpha);
		const FTransform InstanceTransform(FRotator::ZeroRotator, RenderPos + FVector(0.0, 0.0, AgentHalfHeightUU),
		                                    FVector(InstanceScaleXY, InstanceScaleXY, InstanceScaleZ));
		AgentISM->UpdateInstanceTransform(View.InstanceIndex, InstanceTransform, /*bWorldSpace=*/true,
		                                    /*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);
		bAnyUpdated = true;
	}
	if (bAnyUpdated)
	{
		AgentISM->MarkRenderStateDirty();
		VoxelEofLedger::Count(VoxelEofLedger::ESource::AgentISM);
	}
}

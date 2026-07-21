#include "VoxelAgentSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "VoxelCoords.h"
#include "VoxelEarth.h" // LogVoxelEarth
#include "VoxelWorldSubsystem.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelAgentSubsystem.h's identical PImpl doc comment). pathfind.h is the
// M6-groundwork windowed voxel A* this subsystem drives (docs/status.md's
// M6 section) -- consumed as-is, never modified (voxel-core stays
// engine-free and owned by that other slice).
#include "voxelcore/core.h"
#include "voxelcore/pathfind.h"

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
		PlanPath(AgentIndex, Terrain, GoalWorldPos);
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

	if (Agents.Num() == 0 || !Impl)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		// Server-authoritative (class comment "KNOWN v0 SCOPE LIMIT"): no
		// client-side simulation or replication in this slice.
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
	}
}

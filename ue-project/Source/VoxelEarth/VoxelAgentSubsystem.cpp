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
// this one sentinel material regardless of what it actually is. That's
// fine for a navigation-only slice -- FVoxelAgentImpl::Config below marks
// EVERY material's mine cost as impassable (see Initialize), so which
// specific solid material a cell reports as never changes the outcome:
// Mine is never a legal move either way. MAT_ROCK (not MAT_AIR, not
// MAT_BEDROCK) is used purely so classifyMove's "is this air?" / "is this
// unconditionally-impassable bedrock?" checks both do the right thing.
constexpr vxc::MaterialId kSolidSentinelMaterial = vxc::MAT_ROCK;
} // namespace

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

	// Shared cost weights for every findPath call this subsystem makes.
	// Mine/Bridge are effectively disabled here (set once, in
	// UVoxelAgentSubsystem::Initialize) per the task doctrine: this is a
	// navigation-only slice, agents only Walk/Step/Climb/Fall/Jump over
	// EXISTING terrain, never dig or place scaffolding.
	vxc::PathCostConfig Config;
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

	// Cost config: Mine/Bridge effectively disabled (task doctrine
	// "navigation-only v0 ... agents only Walk/Step/Climb/Fall/Jump over
	// EXISTING terrain"). pathfind.h has no separate "forbid this action"
	// flag, only cost -- so Mine is made LITERALLY impossible (every
	// material's mineCostByMaterial entry set to the documented
	// "impassable" sentinel, a negative value -- see pathfind.h's
	// classifyMove), while Bridge is priced high enough (not infinite,
	// vxc::PathCostConfig requires cost fields to fit in int32_t and stay
	// finite) that it is never the cheaper choice UNLESS it is the only
	// way through a search window at all -- effectively, not literally,
	// disabled, exactly as the task doctrine phrases it. Walk/StepUp/
	// StepDown/Climb/Fall/Jump keep their PathCostConfig defaults; Jump
	// stays enabled -- it places nothing, it just leaps a real gap, and is
	// explicitly one of the allowed actions per the task doctrine.
	for (int32 MatIndex = 0; MatIndex < vxc::kMaterialCount; ++MatIndex)
	{
		Impl->Config.mineCostByMaterial[MatIndex] = -1;
	}
	Impl->Config.bridgeCost = 1'000'000;
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
	const double InstanceScaleXY = AgentBodyWidthUU / 100.0; // cube base mesh is 100UU
	const double InstanceScaleZ = (AgentHalfHeightUU * 2.0) / 100.0;

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
		// GetSurfaceHeightUU is a pure query over the amplifier (safe before
		// any chunk has streamed in -- see UVoxelWorldSubsystem.h's doc
		// comment), so agents can be ground-placed immediately at BeginPlay
		// with no need to wait for render streaming.
		const double SurfaceUU = Terrain->GetSurfaceHeightUU(WorldX, WorldY);

		FVoxelAgent Agent;
		Agent.Position = FVector(WorldX, WorldY, SurfaceUU);
		// Placeholder starting tier -- the very next Tick's
		// ComputeNextVoxelAgentTier call reclassifies every agent from its
		// REAL distance to the player regardless of this initial guess (at
		// most a one-tick startup transient, see that function's doc
		// comment for why it can't skip more than one tier boundary per
		// call anyway).
		Agent.Tier = EVoxelAgentTier::Tier1_Abstract;

		if (AgentISM)
		{
			const FTransform InstanceTransform(FRotator::ZeroRotator, Agent.Position + FVector(0.0, 0.0, AgentHalfHeightUU),
			                                    FVector(InstanceScaleXY, InstanceScaleXY, InstanceScaleZ));
			Agent.InstanceIndex = AgentISM->AddInstance(InstanceTransform, /*bWorldSpace=*/true);
		}

		Agents.Add(Agent);
		Impl->Paths.Add(vxc::PathResult{});
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

	vxc::PathResult Result = vxc::findPath(SolidFn, Start, Goal, Impl->Config, Window);

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

	return Agent.Waypoints.Num() > 0;
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
                                      const FVector& GoalWorldPos, double DeltaSeconds, int32& InOutReplanBudget)
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
			// (small) path, at most a few dozen cells.
			UVoxelWorldSubsystem* TerrainPtr = &Terrain;
			const auto SolidFn = [TerrainPtr](int64 X, int64 Y, int64 Z) -> vxc::MaterialId
			{
				return TerrainPtr->IsSolidAtVoxel(X, Y, Z) ? kSolidSentinelMaterial : vxc::MAT_AIR;
			};
			if (!vxc::pathStillValid(Impl->Paths[AgentIndex], SolidFn, Impl->Config))
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

	if (!Agent.bIdleAtStandoff)
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

	if (!Agent.bIdleAtStandoff)
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
	bool bNeedsReplan = bPathExhausted;

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
			bPathInvalid = !vxc::pathStillValid(Impl->Paths[AgentIndex], SolidFn, Impl->Config);
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
			TickTier0(i, *Terrain, PlayerWorldPos, GoalWorldPos, DeltaSeconds, ReplanBudget);
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

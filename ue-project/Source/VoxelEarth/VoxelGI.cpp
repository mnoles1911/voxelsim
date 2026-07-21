#include "VoxelGI.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelLightField.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGI, Log, All);

// --- cvars -----------------------------------------------------------------
//
// Naming follows this module's existing convention (voxel.<Area>.<Name>, see
// VoxelDebug.cpp). Declared here rather than in VoxelDebug.cpp purely as file
// ownership hygiene while several agents are live in this module.

namespace
{
	TAutoConsoleVariable<int32> CVarGIEnabled(
		TEXT("voxel.GI.Enabled"), 0,
		TEXT("Voxel light field + cone-traced GI (M4). 0 = off (default, and genuinely zero-cost: ")
		TEXT("the subsystem does not tick and the scene proxy emits byte-identical vertex colours). ")
		TEXT("1 = on. CLIENT-SIDE RENDERING ONLY -- outside the determinism boundary."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarGIStrength(
		TEXT("voxel.GI.Strength"), 1.0f,
		TEXT("Blend between the mesher's geometric AO alone (0) and the full cone-traced term (1)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIAmbientFloor(
		TEXT("voxel.GI.AmbientFloor"), 0.06f,
		TEXT("Minimum ambient a fully enclosed surface keeps. Pure zero reads as a rendering bug ")
		TEXT("rather than as darkness, and there is no other light source in a sealed tunnel."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxQuadSpanVoxels(
		TEXT("voxel.GI.MaxQuadSpanVoxels"), 8,
		TEXT("When GI is on, greedy quads are subdivided so no quad spans more than this many ")
		TEXT("voxels. GI is evaluated per VERTEX, so an unsubdivided 32-voxel quad would smear a ")
		TEXT("3.2m gradient across 4 corner samples. 0 disables subdivision."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIRadiusUU(
		TEXT("voxel.GI.RadiusUU"), 7000.f,
		TEXT("Light field build radius around the view origin, UU. Deliberately larger than the R0 ")
		TEXT("ring (6400 UU) so every level-0 chunk has field coverage and there is no partial-")
		TEXT("coverage seam inside R0."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIFadeStartUU(
		TEXT("voxel.GI.FadeStartUU"), 4800.f,
		TEXT("Distance from the field centre at which GI starts blending back to plain geometric AO."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIFadeEndUU(
		TEXT("voxel.GI.FadeEndUU"), 6400.f,
		TEXT("Distance at which GI is fully faded out (matches the R0 ring outer radius)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxVoxelizePerFrame(
		TEXT("voxel.GI.MaxVoxelizePerFrame"), 16,
		TEXT("Chunks converted into light field bricks per frame. Bounds the streaming ramp."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBrickSolvesPerFrame(
		TEXT("voxel.GI.MaxBrickSolvesPerFrame"), 8,
		TEXT("Bricks cone-traced per frame (blocking ParallelFor over workers). THIS is the knob ")
		TEXT("that keeps a large explosion from stalling the frame: a big edit makes the dirty ")
		TEXT("queue longer, never the frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIRefreshBricksPerFrame(
		TEXT("voxel.GI.RefreshBricksPerFrame"), 2,
		TEXT("Resident bricks re-solved per frame in round-robin, on top of the dirty queue. This ")
		TEXT("is what lets the progressive one-bounce gather converge."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxChunkRefreshesPerFrame(
		TEXT("voxel.GI.MaxChunkRefreshesPerFrame"), 4,
		TEXT("Scene proxies rebuilt per frame to pick up new GI vertex colours. Same order of ")
		TEXT("magnitude as the streaming system's own applies-per-frame budget."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIEditDirtyRadiusBricks(
		TEXT("voxel.GI.EditDirtyRadiusBricks"), 2,
		TEXT("Bricks around an edited brick that are re-solved. 2 -> a 5x5x5 neighbourhood (1.6m ")
		TEXT("of light bleed around the change)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBricks(
		TEXT("voxel.GI.MaxBricks"), 4096,
		TEXT("Hard cap on resident light field bricks (~3.6 KB each, so 4096 ~= 15 MB)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIConeDistanceUU(
		TEXT("voxel.GI.ConeDistanceUU"), 3000.f,
		TEXT("Cone march termination distance, UU."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIBounceAlbedo(
		TEXT("voxel.GI.BounceAlbedo"), 0.4f,
		TEXT("Diffuse albedo used for the progressive one-bounce gather."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIDebug(
		TEXT("voxel.GI.Debug"), 0,
		TEXT("1 = log light field stats (bricks, queue depths, solve ms) once a second."),
		ECVF_Default);

	constexpr int32 kMaxPendingVoxelize = 1024;
	constexpr int32 kCoarseRebuildIntervalFrames = 30;
}

namespace VoxelGI
{
	bool IsEnabled() { return CVarGIEnabled.GetValueOnAnyThread() != 0; }
	float GetStrength() { return FMath::Clamp(CVarGIStrength.GetValueOnAnyThread(), 0.f, 1.f); }
	float GetAmbientFloor() { return FMath::Clamp(CVarGIAmbientFloor.GetValueOnAnyThread(), 0.f, 1.f); }
	int32 GetMaxQuadSpanVoxels() { return FMath::Clamp(CVarGIMaxQuadSpanVoxels.GetValueOnAnyThread(), 0, 32); }
	float GetFadeStartUU() { return CVarGIFadeStartUU.GetValueOnAnyThread(); }
	float GetFadeEndUU() { return CVarGIFadeEndUU.GetValueOnAnyThread(); }
	int32 GetDebugLevel() { return CVarGIDebug.GetValueOnAnyThread(); }
}

// --- subsystem lifetime ----------------------------------------------------

void UVoxelGISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Field = MakeUnique<FVoxelLightField>();
}

void UVoxelGISubsystem::Deinitialize()
{
	ClearAllState();
	Field.Reset();
	Super::Deinitialize();
}

bool UVoxelGISubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UVoxelGISubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelGISubsystem, STATGROUP_Tickables);
}

bool UVoxelGISubsystem::IsTickable() const
{
	// Zero per-frame cost when GI is off. bHasState keeps ticking for exactly
	// as long as it takes to release the field after a runtime toggle-off.
	return VoxelGI::IsEnabled() || bHasState;
}

void UVoxelGISubsystem::ClearAllState()
{
	if (Field)
	{
		Field->Reset();
	}
	PendingVoxelize.Reset();
	DirtyQueue.Reset();
	DirtySet.Reset();
	RefreshQueue.Reset();
	RefreshSet.Reset();
	BrickComponents.Reset();
	RefreshRotation.Reset();
	RefreshCursor = 0;
	bCoarseDirty = false;
	bHasState = false;
}

// --- ingest ----------------------------------------------------------------

void UVoxelGISubsystem::NotifyChunkMeshUpdated(UVoxelChunkComponent* Component)
{
	// First branch, before anything else: with GI off this is one cvar read
	// per SetChunkQuads call and nothing more.
	if (!VoxelGI::IsEnabled() || !Component)
	{
		return;
	}
	// Only level-0 chunks feed the field. Coarse rings are the LOD story:
	// beyond R0 there is no light field and chunks shade with the mesher's
	// geometric AO exactly as they do today.
	if (Component->GetLevel() != 0)
	{
		return;
	}
	if (PendingVoxelize.Num() >= kMaxPendingVoxelize)
	{
		// Overflow (a very large simultaneous edit): drop the oldest rather
		// than growing without bound. The dropped chunk is picked up again by
		// the round-robin refresh, just later.
		PendingVoxelize.RemoveAt(0, 1, EAllowShrinking::No);
	}
	PendingVoxelize.Add(Component);
	bHasState = true;
}

void UVoxelGISubsystem::PushDirty(const FIntVector& Key)
{
	bool bAlready = false;
	DirtySet.Add(Key, &bAlready);
	if (!bAlready)
	{
		DirtyQueue.Add(Key);
	}
}

void UVoxelGISubsystem::MarkBrickNeighbourhoodDirty(const FIntVector& BrickCoord, int32 RadiusBricks)
{
	for (int32 DZ = -RadiusBricks; DZ <= RadiusBricks; ++DZ)
	for (int32 DY = -RadiusBricks; DY <= RadiusBricks; ++DY)
	for (int32 DX = -RadiusBricks; DX <= RadiusBricks; ++DX)
	{
		PushDirty(BrickCoord + FIntVector(DX, DY, DZ));
	}
}

FVector UVoxelGISubsystem::ResolveViewOriginUU() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
			{
				return Cam->GetCameraLocation();
			}
			if (const APawn* Pawn = PC->GetPawn())
			{
				return Pawn->GetActorLocation();
			}
		}
	}
	return FieldCentreUU;
}

// --- tick ------------------------------------------------------------------

void UVoxelGISubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!VoxelGI::IsEnabled())
	{
		if (bHasState)
		{
			ClearAllState(); // runtime toggle-off releases everything in one frame
		}
		return;
	}
	if (!Field)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const double TickStart = Now;
	FieldCentreUU = ResolveViewOriginUU();

	// 1) Voxelize newly meshed chunks (stream-in AND post-edit remesh land in
	//    the same queue -- see NotifyChunkMeshUpdated).
	const int32 VoxelizeBudget = FMath::Max(0, CVarGIMaxVoxelizePerFrame.GetValueOnGameThread());
	const int32 EditRadius = FMath::Clamp(CVarGIEditDirtyRadiusBricks.GetValueOnGameThread(), 0, 4);
	const double BuildRadiusUU = CVarGIRadiusUU.GetValueOnGameThread();
	const int32 MaxBricks = FMath::Max(64, CVarGIMaxBricks.GetValueOnGameThread());

	int32 Voxelized = 0;
	while (Voxelized < VoxelizeBudget && PendingVoxelize.Num() > 0)
	{
		TWeakObjectPtr<UVoxelChunkComponent> WeakComp = PendingVoxelize[0];
		PendingVoxelize.RemoveAt(0, 1, EAllowShrinking::No);

		UVoxelChunkComponent* Comp = WeakComp.Get();
		if (!Comp || Comp->GetLevel() != 0)
		{
			continue;
		}
		const FVector OriginUU = Comp->GetComponentLocation();
		if (FVector::Dist(OriginUU, FieldCentreUU) > BuildRadiusUU)
		{
			continue; // outside the GI ring; nothing to build
		}
		if (Field->NumBricks() >= MaxBricks && !Field->HasBrick(FVoxelLightField::WorldToBrick(OriginUU)))
		{
			continue; // at the memory cap and this would be a new brick
		}

		const FIntVector BrickCoord = FVoxelLightField::WorldToBrick(OriginUU);
		Field->VoxelizeChunk(BrickCoord, OriginUU, Comp->GetChunkQuads());
		BrickComponents.Add(BrickCoord, WeakComp);
		MarkBrickNeighbourhoodDirty(BrickCoord, EditRadius);
		bCoarseDirty = true;
		++Voxelized;
	}

	// 2) Coarse pyramid rebuild. Full rebuild rather than incremental because
	//    MAX aggregation is not invertible under removal, and O(NumBricks)
	//    map inserts a few times a second is far cheaper than getting
	//    decremental max bookkeeping wrong.
	++FramesSinceCoarseRebuild;
	if (bCoarseDirty && FramesSinceCoarseRebuild >= kCoarseRebuildIntervalFrames)
	{
		Field->RebuildCoarse();
		bCoarseDirty = false;
		FramesSinceCoarseRebuild = 0;
	}

	// 3) Solve a bounded slice of the dirty queue, plus a slow round-robin
	//    refresh so the progressive bounce converges and long-resident bricks
	//    do not go stale.
	FVoxelGISolveParams Params;
	Params.MaxConeDistanceUU = CVarGIConeDistanceUU.GetValueOnGameThread();
	Params.BounceAlbedo = CVarGIBounceAlbedo.GetValueOnGameThread();

	TArray<FIntVector> ToSolve;
	const int32 SolveBudget = FMath::Max(0, CVarGIMaxBrickSolvesPerFrame.GetValueOnGameThread());
	while (ToSolve.Num() < SolveBudget && DirtyQueue.Num() > 0)
	{
		const FIntVector Key = DirtyQueue[0];
		DirtyQueue.RemoveAt(0, 1, EAllowShrinking::No);
		DirtySet.Remove(Key);
		if (Field->HasBrick(Key))
		{
			ToSolve.Add(Key);
		}
	}

	const int32 RefreshBudget = FMath::Max(0, CVarGIRefreshBricksPerFrame.GetValueOnGameThread());
	if (RefreshBudget > 0)
	{
		if (RefreshCursor >= RefreshRotation.Num())
		{
			Field->GetResidentKeys(RefreshRotation);
			RefreshCursor = 0;
		}
		for (int32 I = 0; I < RefreshBudget && RefreshCursor < RefreshRotation.Num(); ++I, ++RefreshCursor)
		{
			const FIntVector Key = RefreshRotation[RefreshCursor];
			if (Field->HasBrick(Key) && !ToSolve.Contains(Key))
			{
				ToSolve.Add(Key);
			}
		}
	}

	int32 CellsSolved = 0;
	double SolveMs = 0.0;
	if (ToSolve.Num() > 0)
	{
		const double SolveStart = FPlatformTime::Seconds();
		CellsSolved = Field->SolveBricks(ToSolve, Params);
		SolveMs = (FPlatformTime::Seconds() - SolveStart) * 1000.0;

		for (const FIntVector& Key : ToSolve)
		{
			bool bAlready = false;
			RefreshSet.Add(Key, &bAlready);
			if (!bAlready)
			{
				RefreshQueue.Add(Key);
			}
		}
	}

	// 4) Re-shade chunks whose irradiance changed. MarkRenderStateDirty
	//    rebuilds the scene proxy, which re-samples the field per vertex --
	//    the same path a normal remesh takes, so this reuses machinery that is
	//    already on the streaming system's proven budget rather than inventing
	//    a second vertex-buffer update route.
	const int32 RefreshChunkBudget = FMath::Max(0, CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread());
	int32 Refreshed = 0;
	while (Refreshed < RefreshChunkBudget && RefreshQueue.Num() > 0)
	{
		const FIntVector Key = RefreshQueue[0];
		RefreshQueue.RemoveAt(0, 1, EAllowShrinking::No);
		RefreshSet.Remove(Key);
		if (TWeakObjectPtr<UVoxelChunkComponent>* Found = BrickComponents.Find(Key))
		{
			if (UVoxelChunkComponent* Comp = Found->Get())
			{
				Comp->MarkRenderStateDirty();
				++Refreshed;
			}
			else
			{
				BrickComponents.Remove(Key);
			}
		}
	}

	// 5) Eviction, at most twice a second. Distance-based rather than
	//    hooked to chunk unload: UVoxelChunkComponent has no unload callback
	//    this module is permitted to add, and level-0 chunks only ever unload
	//    because the camera left, which is exactly what this tests.
	if (Now - LastEvictSeconds > 0.5)
	{
		LastEvictSeconds = Now;
		TArray<FIntVector> Evicted;
		const int32 NumEvicted = Field->EvictFarBricks(FieldCentreUU, BuildRadiusUU * 1.25, 64, Evicted);
		for (const FIntVector& Key : Evicted)
		{
			BrickComponents.Remove(Key);
		}
		if (NumEvicted > 0)
		{
			bCoarseDirty = true;
			RefreshRotation.Reset();
			RefreshCursor = 0;
		}
	}

	// voxel.GI.Debug 2: probe a vertical column above the view origin with a
	// DOWNWARD normal and dump both the field's opacity and its solved
	// irradiance at each step. Added while chasing "the roof slab underside
	// stays fully lit while the enclosed wall darkens correctly" -- guessing at
	// that from screenshots cost two build/run cycles; this answers in one.
	if (VoxelGI::GetDebugLevel() >= 2 && Now - LastStatSeconds > 1.0)
	{
		const FVoxelLightField::FReadScope Read(*Field);
		FString Line;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const FVector P = FieldCentreUU + FVector(0, 0, double(Step) * VoxelLF::CellSizeUU);
			float Irr = -1.f;
			const bool bOk = Read.Sample(P, FVector3f(0, 0, -1), Irr);
			Line += FString::Printf(TEXT("[+%dcm %s%.2f] "), Step * VoxelLF::CellSizeUU / 10,
			                        bOk ? TEXT("") : TEXT("MISS "), Irr);
		}
		UE_LOG(LogVoxelGI, Log, TEXT("GI probe (normal -Z, column above view origin): %s"), *Line);
	}

	if (VoxelGI::GetDebugLevel() > 0 && Now - LastStatSeconds > 1.0)
	{
		LastStatSeconds = Now;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI: bricks=%d (%.1f MB) pendingVox=%d dirty=%d refresh=%d | solved %d bricks/%d cells in %.2fms, tick %.2fms"),
		       Field->NumBricks(), double(Field->EstimatedBytes()) / (1024.0 * 1024.0),
		       PendingVoxelize.Num(), DirtyQueue.Num(), RefreshQueue.Num(),
		       ToSolve.Num(), CellsSolved, SolveMs, (FPlatformTime::Seconds() - TickStart) * 1000.0);
	}
}

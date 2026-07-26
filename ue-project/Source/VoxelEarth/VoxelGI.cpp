#include "VoxelGI.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelLightField.h"
// VoxelEarth -> VoxelEarthShaders, never the reverse (see VoxelEarth.Build.cs).
// So the encoder and its driver live HERE, in the module that owns the light
// field, and call into the VOXELEARTHSHADERS_API volume rather than having the
// renderer module reach back for the field.
#include "VoxelGIVolume.h"
#include "VoxelGpuPoolComponent.h"
// voxel.GI.VolumeDigTest carves through the real edit path (CarveSphere) rather
// than poking the field, so the dig test exercises the same remesh -> ingest ->
// upload chain a player's dig does.
#include "VoxelWorldSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"

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
		TEXT("Chunks re-shaded per frame to pick up new GI vertex colours. Same order of ")
		TEXT("magnitude as the streaming system's own applies-per-frame budget. (Before the ")
		TEXT("in-place colour update this was a SCENE PROXY REBUILD per chunk -- see ")
		TEXT("voxel.GI.LegacyProxyRebuild.)"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGILegacyProxyRebuild(
		TEXT("voxel.GI.LegacyProxyRebuild"), 0,
		TEXT("1 = restore the pre-2026-07-22 re-shade path: push new GI vertex colours by calling ")
		TEXT("MarkRenderStateDirty(), i.e. destroy and rebuild the whole scene proxy, instead of ")
		TEXT("memcpying the colours into the existing colour vertex buffer. Kept solely so the ")
		TEXT("before/after cost can be measured from ONE binary in an interleaved A/B ")
		TEXT("(-VoxelGILegacyRefresh); it is strictly slower for identical output."),
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

	TAutoConsoleVariable<int32> CVarGILegacyConeBasis(
		TEXT("voxel.GI.LegacyConeBasis"), 0,
		TEXT("1 = restore the pre-2026-07-21 DEGENERATE cone basis: 6 axis-aligned cones, each ")
		TEXT("ambient-cube slot fed by its own axis alone. Because every normal this mesh produces ")
		TEXT("is axis-aligned, max(0,N.D) then selects exactly one cone and side light is weighted ")
		TEXT("to zero. Kept solely so the before/after of the fix can be captured from one build ")
		TEXT("(-VoxelGILegacyBasis); it is a bug, not a quality/perf tradeoff."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIDebug(
		TEXT("voxel.GI.Debug"), 0,
		TEXT("1 = log light field stats (bricks, queue depths, solve ms) once a second. ")
		TEXT("2 = + a downward-normal probe column and a per-chunk shading summary. ")
		TEXT("3 = + a PER-FACE-DIRECTION breakdown of every chunk proxy (which was what ")
		TEXT("finally isolated the roof-underside defect: the +X/-X/+Y/-Y/+Z/-Z buckets ")
		TEXT("report their own hit rate and mean shade, so 'this normal never finds data' ")
		TEXT("is one log line rather than a guess from a screenshot)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIDebugVis(
		TEXT("voxel.GI.DebugVis"), 0,
		TEXT("Diagnostic override of the vertex shade byte (read at proxy build, so use ")
		TEXT("-VoxelGIVis=<n> rather than -ExecCmds). 0 = off. 1 = raw sampled irradiance, ")
		TEXT("with a MISS forced to white -- distinguishes 'the field says lit' from 'the ")
		TEXT("field had no data and we fell back to plain AO'. 2 = pure hit/miss map ")
		TEXT("(hit = black, miss = white). 3 = |N.Z| ramp, to confirm which faces are ")
		TEXT("actually on screen. 4 = omit +Z faces. 5 = omit -Z faces. 6 = restore the ")
		TEXT("LEGACY inverted triangle winding (the roof-underside defect), for before/after ")
		TEXT("capture from a single build."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIMaxBrickUploadsPerFrame(
		TEXT("voxel.GI.MaxBrickUploadsPerFrame"), 64,
		TEXT("Light field bricks whose texels are pushed into the GPU GI volume per frame. This is ")
		TEXT("the pooled path's replacement for voxel.GI.MaxChunkRefreshesPerFrame -- a pooled chunk ")
		TEXT("has no component and no vertex colours to re-shade, so lighting updates are texel ")
		TEXT("uploads instead. Bricks are merged into X-runs before uploading, so this bounds ")
		TEXT("BYTES, not draw-call count."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGIVolumeCheck(
		TEXT("voxel.GI.VolumeCheck"), 0,
		TEXT("Numeric field-vs-volume equivalence harness. Non-zero arms it; the value is the number ")
		TEXT("of random solved cells to sample (1 = the default 4096). Runs ONCE, after the field has ")
		TEXT("settled, and logs VOLUMECHECK: cells=.. meanAbsErr=.. maxAbsErr=.. in irradiance bytes. ")
		TEXT("Set back to 0 to re-arm."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarGIVolumeCheckSettle(
		TEXT("voxel.GI.VolumeCheckSettleSeconds"), 20.f,
		TEXT("Seconds after the first texel upload before voxel.GI.VolumeCheck runs. Measuring a ")
		TEXT("field that has barely streamed in reports a tiny sample count and proves nothing."),
		ECVF_Default);

	constexpr int32 kMaxPendingVoxelize = 1024;
	constexpr int32 kCoarseRebuildIntervalFrames = 30;

	// Bricks of gap tolerated when coalescing an X-run. A run that skips two
	// clean bricks still beats a second UpdateTexture3D and its barrier, and a
	// dig dirties a contiguous 5x5x5 neighbourhood where the gaps are rare
	// anyway.
	constexpr int32 kVolumeRunGapBricks = 2;

	constexpr int32 kVolumeCheckDefaultSamples = 4096;

	// One brick-run staged for the render thread. Owns its bytes: the light
	// field is read on the GAME thread under FReadScope and the payload crosses
	// in the ENQUEUE_RENDER_COMMAND, never the other way round.
	struct FVoxelGIVolumeRun
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Size = FIntVector::ZeroValue;
		TArray<uint8> TexelsPos;
		TArray<uint8> TexelsNeg;
	};

	// The pool component's world location IS pool-primitive space's origin --
	// the component carries the world's big offset in its double-precision
	// transform and everything the shader sees is relative to it
	// (docs/gpu-gi-volume-design.md §6). Read off the live component rather than
	// recomputed from the streaming subsystem's rebase so there is exactly one
	// definition of the space.
	bool FindPoolWorldLocation(const UWorld* World, FVector& OutLocation)
	{
		if (!World)
		{
			return false;
		}
		for (TObjectIterator<UVoxelGpuPoolComponent> It; It; ++It)
		{
			UVoxelGpuPoolComponent* Pool = *It;
			if (Pool && !Pool->IsTemplate() && Pool->IsRegistered() && Pool->GetWorld() == World)
			{
				OutLocation = Pool->GetComponentLocation();
				return true;
			}
		}
		return false;
	}
}

namespace VoxelGI
{
	bool IsEnabled() { return CVarGIEnabled.GetValueOnAnyThread() != 0; }
	float GetStrength() { return FMath::Clamp(CVarGIStrength.GetValueOnAnyThread(), 0.f, 1.f); }
	float GetAmbientFloor() { return FMath::Clamp(CVarGIAmbientFloor.GetValueOnAnyThread(), 0.f, 1.f); }
	// -VoxelGIQuadSpan=<n> overrides voxel.GI.MaxQuadSpanVoxels from the COMMAND
	// LINE, latched on first use. This knob is read at proxy-build time, so an
	// -ExecCmds cvar only affects chunks meshed AFTER it lands -- chunks already
	// resident keep the old subdivision until something re-meshes them, which
	// silently mixes both settings into one measurement. Same class of trap as
	// the -ExecCmds voxel.GI.Enabled A/B that cost three runs.
	int32 GetMaxQuadSpanVoxels()
	{
		static const int32 CmdLineOverride = []
		{
			int32 Value = -1;
			return FParse::Value(FCommandLine::Get(), TEXT("VoxelGIQuadSpan="), Value) ? FMath::Clamp(Value, 0, 32) : -1;
		}();
		if (CmdLineOverride >= 0)
		{
			return CmdLineOverride;
		}
		return FMath::Clamp(CVarGIMaxQuadSpanVoxels.GetValueOnAnyThread(), 0, 32);
	}
	float GetFadeStartUU() { return CVarGIFadeStartUU.GetValueOnAnyThread(); }
	float GetFadeEndUU() { return CVarGIFadeEndUU.GetValueOnAnyThread(); }
	int32 GetDebugLevel() { return CVarGIDebug.GetValueOnAnyThread(); }
	int32 GetDebugVis() { return CVarGIDebugVis.GetValueOnAnyThread(); }
}

// --- subsystem lifetime ----------------------------------------------------

void UVoxelGISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Field = MakeUnique<FVoxelLightField>();

	// -VoxelGIConverge=<N> -- the energy-conservation proof harness. Read from
	// the command line at init, NOT via -ExecCmds: cvars set through ExecCmds
	// land after subsystems initialise, and the whole point of this harness is
	// to control what state the solve starts from (this module already learned
	// that lesson with -VoxelGIVis).
	//
	// Optional companions:
	//   -VoxelGIConvergeSettle=<s>  seconds to let streaming/voxelization fill
	//                               the field before measuring (default 40).
	//   -VoxelGIConvergeLegacy      gather the bounce from the live AvgIrr, i.e.
	//                               the pre-fix behaviour, so one build can
	//                               produce both the drift and the fix.
	// -VoxelGILegacyBasis: command line rather than -ExecCmds for the same
	// reason as -VoxelGIVis -- it has to be in force before the first solve,
	// and a cvar set after init leaves already-solved bricks on the new basis
	// and silently produces a mixed capture.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelGILegacyBasis")))
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.LegacyConeBasis")))
		{
			V->Set(1, ECVF_SetByCode);
			UE_LOG(LogVoxelGI, Log, TEXT("VoxelGILegacyBasis: voxel.GI.LegacyConeBasis=1 (degenerate pre-fix basis)"));
		}
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConverge="), ConvergePasses) && ConvergePasses > 0)
	{
		ConvergePasses = FMath::Clamp(ConvergePasses, 1, 64);
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConvergeSettle="), ConvergeSettleSeconds);
		bConvergeLegacy = FParse::Param(FCommandLine::Get(), TEXT("VoxelGIConvergeLegacy"));
		bConvergeSeed = FParse::Value(FCommandLine::Get(), TEXT("VoxelGIConvergeSeed="), ConvergeSeedValue);
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GI.Enabled")))
		{
			V->Set(1, ECVF_SetByCode); // the harness is meaningless with GI off
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGIConverge: %d passes after %.0fs settle (legacy in-place gather=%d). GI forced on."),
		       ConvergePasses, ConvergeSettleSeconds, bConvergeLegacy ? 1 : 0);
	}
	else
	{
		ConvergePasses = 0;
	}
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
	PendingPooledVoxelize.Reset();
	DirtyQueue.Reset();
	DirtySet.Reset();
	RefreshQueue.Reset();
	RefreshSet.Reset();
	BrickComponents.Reset();
	RefreshRotation.Reset();
	RefreshCursor = 0;
	VolumeUploadQueue.Reset();
	VolumeUploadSet.Reset();
	// An in-flight re-centre must not survive a toggle-off: its row buckets name
	// bricks the field is about to drop, and resuming it later would restage the
	// volume from a resident set that no longer exists.
	bVolumeRecentring = false;
	RecentreRowBricks.Reset();
	// The shadow and the origin deliberately SURVIVE a toggle-off: the GPU
	// texture still holds those bytes, and throwing the mirror away would leave
	// the harness comparing against nothing while the volume kept rendering.
	// Re-centring picks up from wherever the origin was left.
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

void UVoxelGISubsystem::NotifyPooledChunkMeshUpdated(const FVector& ChunkOriginUU, int32 ChunkLevel,
                                                     TArray<FVoxelChunkQuad>&& Quads)
{
	// First branch, before anything else and BEFORE the move: with GI off this
	// is one cvar read per pooled apply and nothing more -- no queue growth, no
	// quad copy, and the caller's array is left exactly as it was.
	if (!VoxelGI::IsEnabled())
	{
		return;
	}
	// Level-0 only, matching NotifyChunkMeshUpdated and the scene proxy's
	// bGIEnabled = VoxelGI::IsEnabled() && ChunkLevel == 0. Coarse rings have
	// no field coverage on either renderer.
	if (ChunkLevel != 0 || Quads.Num() == 0)
	{
		return;
	}
	if (PendingPooledVoxelize.Num() >= kMaxPendingVoxelize)
	{
		// Same overflow policy as the component queue: drop the oldest rather
		// than grow without bound. Costlier here because the entry owns its
		// quads, which is exactly why the bound matters.
		PendingPooledVoxelize.RemoveAt(0, 1, EAllowShrinking::No);
	}
	FPendingPooledChunk& Entry = PendingPooledVoxelize.AddDefaulted_GetRef();
	Entry.OriginUU = ChunkOriginUU;
	Entry.Quads = MoveTemp(Quads);
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

// --- GPU volume driver (docs/gpu-gi-volume-design.md §3) --------------------

void UVoxelGISubsystem::PushVolumeUpload(const FIntVector& Key)
{
	// Gated on the cvar rather than queued unconditionally: with the volume off
	// nothing drains, and the dedupe set is keyed by ABSOLUTE brick coord, so a
	// moving camera would grow it without bound. Toggling the volume on
	// mid-session starts from an empty queue and is refilled by the round-robin
	// re-solve (voxel.GI.RefreshBricksPerFrame) within a few seconds.
	if (!VoxelGIVolume::IsEnabled())
	{
		return;
	}
	bool bAlready = false;
	VolumeUploadSet.Add(Key, &bAlready);
	if (!bAlready)
	{
		VolumeUploadQueue.Add(Key);
	}
}

bool UVoxelGISubsystem::EnsureVolumeOrigin()
{
	if (bVolumeOriginSet)
	{
		return true;
	}

	// First-time establishment only. Camera-following re-centring afterwards is
	// BeginVolumeRecentre/StepVolumeRecentre (dead zone + staged re-upload + a
	// one-frame origin swap, docs/gpu-gi-volume-design.md §4).
	if (!FindPoolWorldLocation(GetWorld(), PoolWorldUU))
	{
		// No pooled primitive means nothing samples the volume: the per-chunk
		// renderer reads the field directly at proxy build. Not an error.
		if (!bLoggedNoPool)
		{
			bLoggedNoPool = true;
			UE_LOG(LogVoxelGI, Log,
			       TEXT("VoxelGI volume: no GPU pool component yet -- texel uploads deferred. ")
			       TEXT("(voxel.Stream.GPU 0 never creates one, and the volume has no consumer there.)"));
		}
		return false;
	}

	VolumeDim = VoxelGIVolume::GetDim();
	if (VolumeDim <= 0)
	{
		return false;
	}

	// Brick-snap, in DOUBLE (§4). Everything downstream leans on texel i being
	// world Origin + (i+0.5)*40 with the same lattice the field uses: get this
	// wrong and every sample is shifted half a cell, which reads as a soft
	// lighting offset rather than as a bug.
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	auto SnapDownToBrick = [](double V) -> double
	{
		return FMath::FloorToDouble(V / double(VoxelLF::BrickEdgeUU)) * double(VoxelLF::BrickEdgeUU);
	};
	VolumeOriginWorldUU = FVector(SnapDownToBrick(FieldCentreUU.X - HalfExtentUU),
	                              SnapDownToBrick(FieldCentreUU.Y - HalfExtentUU),
	                              SnapDownToBrick(FieldCentreUU.Z - HalfExtentUU));
	VolumeCellOrigin = FIntVector(int32(VolumeOriginWorldUU.X / VoxelLF::CellSizeUU),
	                              int32(VolumeOriginWorldUU.Y / VoxelLF::CellSizeUU),
	                              int32(VolumeOriginWorldUU.Z / VoxelLF::CellSizeUU));

	// POOL-PRIMITIVE SPACE, subtracted in double and narrowed exactly once
	// (§6). Passing the world-space origin as an FVector3f is explicitly
	// prohibited: at ~8.4M UU float32's ULP is 1.0 UU against a 40 UU cell.
	const FVector3f OriginPoolUU = FVector3f(VolumeOriginWorldUU - PoolWorldUU);
	CommittedOriginPoolUU = OriginPoolUU;

	VolumeShadow.Empty();
	VolumeShadow.SetNumZeroed(int64(VolumeDim) * VolumeDim * VolumeDim * 4);
	VolumeShadowNeg.Empty();
	VolumeShadowNeg.SetNumZeroed(int64(VolumeDim) * VolumeDim * VolumeDim * 4);
	bVolumeOriginSet = true;

	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI volume: origin set dim=%d fieldCentre=(%.0f,%.0f,%.0f) originWorld=(%.0f,%.0f,%.0f) ")
	       TEXT("poolWorld=(%.0f,%.0f,%.0f) originPool=(%.1f,%.1f,%.1f) coverage=+/-%.0f UU shadow=%.1f MB"),
	       VolumeDim, FieldCentreUU.X, FieldCentreUU.Y, FieldCentreUU.Z,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z,
	       PoolWorldUU.X, PoolWorldUU.Y, PoolWorldUU.Z,
	       OriginPoolUU.X, OriginPoolUU.Y, OriginPoolUU.Z,
	       HalfExtentUU, 2.0 * double(VolumeShadow.Num()) / (1024.0 * 1024.0));

	// The uniform buffer is published by PushVolumeParamsIfChanged, which runs
	// straight after this in TickVolume and now owns every input to it.
	bVolumeSettingsValid = false;
	return true;
}

// --- camera-following re-centring (docs/gpu-gi-volume-design.md §4) ---------
//
// WHY NOT THE OTHER TWO. Toroidal addressing only uploads the new slabs, but
// hardware trilinear bleeds across the wrap plane and that plane SWEEPS THROUGH
// THE INTERIOR as the origin scrolls -- a 40 UU artifact plane moving across the
// world, fixable only by replacing the free trilinear with a manual 8-tap
// Load(). Double-buffering is 2x VRAM, which at the shipping size is another
// 67 MB. Both were considered and rejected in the design; this is the third
// option, and it costs one texture, no seam, and Dim^3*4 bytes of upload spread
// over a handful of frames per re-centre.
//
// THE ONE HONEST CAVEAT, which the design does not state and the implementation
// cannot remove: while the staged upload is in flight the texture holds a MIX of
// old-addressed and new-addressed bytes, and the shader is still reading it with
// the old origin. So the restaged region is transiently wrong -- by exactly the
// re-centre shift -- until the origin swaps. Two things make that not a pop, and
// both are deliberate rather than lucky:
//
//   * rows are restaged from BOTH ENDS INWARD, so the wrongness lives furthest
//     from the camera (where the distance fade is already attenuating GI) and
//     reaches the camera's own neighbourhood only in the last frame or two;
//   * the swap is atomic -- one uniform-buffer publish on one frame -- so there
//     is never a frame where part of the image uses one origin and part another.

bool UVoxelGISubsystem::VolumeNeedsRecentre() const
{
	if (!bVolumeOriginSet || VolumeDim <= 0 || bVolumeRecentring)
	{
		return false;
	}
	// Nothing resident means nothing to re-address, and the first seconds of a
	// session are exactly when the camera can be a long way from wherever the
	// origin was first established. Re-centring an empty field would spend a
	// whole staged upload writing zeros over zeros.
	if (!Field || Field->NumBricks() == 0)
	{
		return false;
	}
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	const FVector Centre = VolumeOriginWorldUU + FVector(HalfExtentUU);

	// Clamped so the dead zone always leaves at least a two-brick margin of
	// volume beyond the camera. A dead zone wider than the half-extent would let
	// the camera leave coverage entirely without ever triggering a re-centre,
	// which presents as "GI stopped working over there" rather than as a bad
	// cvar value.
	const double MaxDeadZoneUU = FMath::Max(double(VoxelLF::BrickEdgeUU),
	                                        HalfExtentUU - 2.0 * double(VoxelLF::BrickEdgeUU));
	const double DeadZoneUU = FMath::Min(double(VoxelGIVolume::GetRecentreCells()) * double(VoxelLF::CellSizeUU),
	                                     MaxDeadZoneUU);

	return FMath::Abs(FieldCentreUU.X - Centre.X) > DeadZoneUU
	    || FMath::Abs(FieldCentreUU.Y - Centre.Y) > DeadZoneUU
	    || FMath::Abs(FieldCentreUU.Z - Centre.Z) > DeadZoneUU;
}

void UVoxelGISubsystem::BeginVolumeRecentre()
{
	if (!Field || VolumeDim <= 0 || VolumeShadow.Num() == 0)
	{
		return;
	}

	// Brick-snap in DOUBLE, exactly as EnsureVolumeOrigin does. The two must use
	// the same expression or the very first re-centre would shift the lattice by
	// a fraction of a cell and every sample after it would resample.
	const double HalfExtentUU = 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
	auto SnapDownToBrick = [](double V) -> double
	{
		return FMath::FloorToDouble(V / double(VoxelLF::BrickEdgeUU)) * double(VoxelLF::BrickEdgeUU);
	};
	const FVector NewOriginWorldUU(SnapDownToBrick(FieldCentreUU.X - HalfExtentUU),
	                               SnapDownToBrick(FieldCentreUU.Y - HalfExtentUU),
	                               SnapDownToBrick(FieldCentreUU.Z - HalfExtentUU));
	if (NewOriginWorldUU.Equals(VolumeOriginWorldUU, 0.5))
	{
		return; // dead zone tripped but the snap lands on the same brick
	}

	const FIntVector OldCellOrigin = VolumeCellOrigin;
	VolumeOriginWorldUU = NewOriginWorldUU;
	VolumeCellOrigin = FIntVector(int32(FMath::FloorToDouble(VolumeOriginWorldUU.X / VoxelLF::CellSizeUU)),
	                              int32(FMath::FloorToDouble(VolumeOriginWorldUU.Y / VoxelLF::CellSizeUU)),
	                              int32(FMath::FloorToDouble(VolumeOriginWorldUU.Z / VoxelLF::CellSizeUU)));

	// Bucket the resident bricks that land inside the NEW volume by brick row, so
	// restaging a row is a lookup rather than a rescan. Everything else in the
	// volume legitimately becomes A=0 -- "no data", which the shader turns into
	// plain AO -- because there is no resident brick there to say otherwise.
	const int32 E = VoxelLF::BrickEdgeCells;
	const int32 Rows = VolumeDim / E;
	RecentreRowBricks.Reset();
	RecentreRowBricks.SetNum(Rows);

	TArray<FIntVector> Keys;
	Field->GetResidentKeys(Keys);
	int32 Inside = 0;
	for (const FIntVector& Key : Keys)
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			continue;
		}
		RecentreRowBricks[Base.Z / E].Add(Key);
		++Inside;
	}

	// The per-brick queue is redundant now: every resident brick inside the new
	// volume is about to be re-encoded from the live field. Anything solved
	// DURING the re-centre re-queues itself and drains normally afterwards.
	VolumeUploadQueue.Reset();
	VolumeUploadSet.Reset();

	bVolumeRecentring = true;
	RecentreLoRow = 0;
	RecentreHiRow = Rows;
	RecentreFrames = 0;
	RecentreCount = Inside;
	RecentreStartSeconds = FPlatformTime::Seconds();
	RecentreStepOccupied = 0;
	RecentreOccupiedBeforeLast = 0;
	RecentreTotalOccupied = 0;
	RecentreStepNearestUU = TNumericLimits<double>::Max();
	RecentreNearestBeforeLastUU = TNumericLimits<double>::Max();

	UE_LOG(LogVoxelGI, Log,
	       TEXT("VoxelGI volume: re-centre BEGIN camera=(%.0f,%.0f,%.0f) originWorld (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) ")
	       TEXT("shift=(%d,%d,%d) cells | %d/%d resident bricks inside | staging %d rows"),
	       FieldCentreUU.X, FieldCentreUU.Y, FieldCentreUU.Z,
	       double(OldCellOrigin.X) * VoxelLF::CellSizeUU, double(OldCellOrigin.Y) * VoxelLF::CellSizeUU,
	       double(OldCellOrigin.Z) * VoxelLF::CellSizeUU,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z,
	       VolumeCellOrigin.X - OldCellOrigin.X, VolumeCellOrigin.Y - OldCellOrigin.Y,
	       VolumeCellOrigin.Z - OldCellOrigin.Z,
	       Inside, Keys.Num(), Rows);
}

void UVoxelGISubsystem::RestageVolumeZRange(int32 Z0, int32 Z1)
{
	if (!Field || Z1 <= Z0)
	{
		return;
	}
	const int32 E = VoxelLF::BrickEdgeCells;
	const int64 SliceBytes = int64(VolumeDim) * VolumeDim * 4;

	// 1) Zero the range. Everything not covered by a resident brick MUST read
	//    A=0: it is the "no data -> plain AO" case, and leaving the previous
	//    occupant's bytes there would light new geometry with old irradiance
	//    from somewhere else entirely.
	FMemory::Memzero(VolumeShadow.GetData() + int64(Z0) * SliceBytes,
	                 int64(Z1 - Z0) * SliceBytes);
	FMemory::Memzero(VolumeShadowNeg.GetData() + int64(Z0) * SliceBytes,
	                 int64(Z1 - Z0) * SliceBytes);

	// 2) Re-encode the resident bricks whose rows fall in this range, under ONE
	//    read scope on the GAME thread (§1: never read the field from the render
	//    thread).
	{
		const FVoxelLightField::FReadScope Read(*Field);
		uint8 BrickPos[FVoxelLightField::BrickTexelBytes];
		uint8 BrickNeg[FVoxelLightField::BrickTexelBytes];
		for (int32 Row = Z0 / E; Row < Z1 / E; ++Row)
		{
			if (!RecentreRowBricks.IsValidIndex(Row))
			{
				continue;
			}
			for (const FIntVector& Key : RecentreRowBricks[Row])
			{
				Read.EncodeBrick(Key, BrickPos, BrickNeg);
				const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
				                      Key.Y * E - VolumeCellOrigin.Y,
				                      Key.Z * E - VolumeCellOrigin.Z);
				for (int32 Z = 0; Z < E; ++Z)
				for (int32 Y = 0; Y < E; ++Y)
				{
					const int64 DstOffset = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + Base.X) * 4;
					const int32 SrcOffset = (Z * E + Y) * E * 4;
					FMemory::Memcpy(VolumeShadow.GetData() + DstOffset, BrickPos + SrcOffset, E * 4);
					FMemory::Memcpy(VolumeShadowNeg.GetData() + DstOffset, BrickNeg + SrcOffset, E * 4);
				}
			}
		}
	}

	// 3) One whole-XY-slab upload for the range. Full-width rows have zero
	//    staging waste (§0's 256-byte row pitch), and a contiguous Z range is
	//    contiguous in the shadow, so this is one UpdateTexture3D for the lot.
	TArray<uint8> PayloadPos;
	TArray<uint8> PayloadNeg;
	PayloadPos.SetNumUninitialized(int64(Z1 - Z0) * SliceBytes);
	PayloadNeg.SetNumUninitialized(int64(Z1 - Z0) * SliceBytes);
	FMemory::Memcpy(PayloadPos.GetData(), VolumeShadow.GetData() + int64(Z0) * SliceBytes,
	                int64(Z1 - Z0) * SliceBytes);
	FMemory::Memcpy(PayloadNeg.GetData(), VolumeShadowNeg.GetData() + int64(Z0) * SliceBytes,
	                int64(Z1 - Z0) * SliceBytes);

	// TRANSIENT ACCOUNTING (voxel.GI.Debug >= 2). This is what turns "no lighting
	// pop" from an assertion into a number: every row restaged BEFORE the origin
	// swap is, for the frames until that swap, being read with the old origin and
	// therefore returning irradiance from a point one re-centre shift away. How
	// bad that is depends on two things this measures -- how many of those texels
	// carry data at all (most of the volume is A=0 rock/sky/unloaded, and an
	// A=0 texel reads as plain AO both before and after, so it cannot pop), and
	// how far the nearest of them is from the camera.
	if (VoxelGI::GetDebugLevel() >= 2)
	{
		int64 Occupied = 0;
		const uint8* Base = VolumeShadow.GetData() + int64(Z0) * SliceBytes;
		const int64 Count = int64(Z1 - Z0) * int64(VolumeDim) * VolumeDim;
		for (int64 I = 0; I < Count; ++I)
		{
			if (Base[I * 4 + 3] != 0) { ++Occupied; }
		}
		RecentreStepOccupied = Occupied;
		RecentreOccupiedBeforeLast += Occupied;

		const double RowMinZ = VolumeOriginWorldUU.Z + double(Z0) * VoxelLF::CellSizeUU;
		const double RowMaxZ = VolumeOriginWorldUU.Z + double(Z1) * VoxelLF::CellSizeUU;
		const double DistZ = FMath::Max(0.0, FMath::Max(RowMinZ - FieldCentreUU.Z, FieldCentreUU.Z - RowMaxZ));
		RecentreStepNearestUU = FMath::Min(RecentreStepNearestUU, DistZ);
	}

	const FIntVector Min(0, 0, Z0);
	const FIntVector Size(VolumeDim, VolumeDim, Z1 - Z0);
	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeRestage)(
		[Min, Size, BytesPos = MoveTemp(PayloadPos), BytesNeg = MoveTemp(PayloadNeg)]
		(FRHICommandListImmediate& RHICmdList)
	{
		GVoxelGIVolume.UpdateTexels_RenderThread(RHICmdList, Min, Size,
		                                         BytesPos.GetData(), BytesNeg.GetData());
	});
	++VolumeRunsUploaded;
}

void UVoxelGISubsystem::StepVolumeRecentre()
{
	if (!bVolumeRecentring)
	{
		return;
	}
	const int32 E = VoxelLF::BrickEdgeCells;
	const int32 Rows = VolumeDim / E;
	// ~8 frames, the design's figure. Expressed as rows-per-frame so the wall
	// time of a re-centre does not scale with voxel.GI.VolumeDim.
	constexpr int32 kRecentreFrames = 8;
	const int32 RowsThisFrame = FMath::Max(2, FMath::DivideAndRoundUp(Rows, kRecentreFrames));

	// Snapshot what the transient looked like going INTO this frame. If this
	// frame turns out to be the committing one, these are the peak numbers --
	// the rows restaged in the committing frame are swapped in the same frame
	// they are written, so they are never read with the stale origin at all.
	RecentreNearestBeforeLastUU = RecentreStepNearestUU;
	const int64 OccupiedBeforeThisFrame = RecentreOccupiedBeforeLast;

	// FURTHEST FROM THE CAMERA FIRST, camera's own row LAST.
	//
	// This was "two rows off the low end, one off the high end" and that is a
	// measured defect, not a stylistic choice: taking an uneven number from each
	// end drifts the meeting point toward the high end instead of leaving it on
	// the camera, so the camera's own row was restaged in frame 7 of 8 and read
	// with the stale origin for a frame. Measured at
	// nearestStaleRowToCamera = 0 UU, which is exactly the artifact the ordering
	// exists to prevent.
	//
	// Picking the end by distance from the camera's row instead makes "the
	// camera's row is last" true by construction rather than by arithmetic
	// accident, and it stays true when the camera is off-centre in Z -- which it
	// usually is, because the dead zone lets it sit anywhere inside the box.
	const int32 CameraRow = FMath::Clamp(
		int32(FMath::FloorToDouble((FieldCentreUU.Z - VolumeOriginWorldUU.Z)
		                           / (double(VoxelLF::CellSizeUU) * double(E)))),
		0, Rows - 1);

	int32 Remaining = RowsThisFrame;
	while (Remaining > 0 && RecentreLoRow < RecentreHiRow)
	{
		// Whichever end is further from the camera goes now. The last row left
		// standing is therefore always the camera's own.
		const int32 LoDist = FMath::Abs(RecentreLoRow - CameraRow);
		const int32 HiDist = FMath::Abs((RecentreHiRow - 1) - CameraRow);
		if (LoDist >= HiDist)
		{
			RestageVolumeZRange(RecentreLoRow * E, (RecentreLoRow + 1) * E);
			++RecentreLoRow;
		}
		else
		{
			RestageVolumeZRange((RecentreHiRow - 1) * E, RecentreHiRow * E);
			--RecentreHiRow;
		}
		--Remaining;
	}
	++RecentreFrames;

	if (RecentreLoRow >= RecentreHiRow)
	{
		// THE ORIGIN SWAP. One publish, one frame, everything correct from here.
		bVolumeRecentring = false;
		RecentreRowBricks.Reset();
		CommittedOriginPoolUU = FVector3f(VolumeOriginWorldUU - PoolWorldUU);
		PushVolumeParamsIfChanged();
		RecentreTotalOccupied = RecentreOccupiedBeforeLast;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGI volume: re-centre COMMIT after %d frames (%.1f ms wall), %d bricks restaged, ")
		       TEXT("originPool=(%.1f,%.1f,%.1f)"),
		       RecentreFrames, (FPlatformTime::Seconds() - RecentreStartSeconds) * 1000.0, RecentreCount,
		       CommittedOriginPoolUU.X, CommittedOriginPoolUU.Y, CommittedOriginPoolUU.Z);
		if (VoxelGI::GetDebugLevel() >= 2)
		{
			// The two numbers that decide whether the staged re-upload can pop:
			// how many occupied texels were read with the stale origin at the
			// worst moment (the frame before the commit), and how close to the
			// camera the nearest of them got. Rows written in the committing
			// frame are excluded because their upload and the origin swap are
			// enqueued in that order into the same render frame.
			UE_LOG(LogVoxelGI, Log,
			       TEXT("VOLUMERECENTRE transient: peakStaleTexels=%lld of %lld occupied (%.1f%%), ")
			       TEXT("nearestStaleRowToCamera=%.0f UU, fadeEnd=%.0f UU"),
			       (long long)OccupiedBeforeThisFrame, (long long)RecentreTotalOccupied,
			       RecentreTotalOccupied > 0 ? 100.0 * double(OccupiedBeforeThisFrame) / double(RecentreTotalOccupied) : 0.0,
			       RecentreNearestBeforeLastUU == TNumericLimits<double>::Max() ? -1.0 : RecentreNearestBeforeLastUU,
			       double(LastVolumeSettings.FadeEndUU));
		}
	}
}

void UVoxelGISubsystem::FlushVolume()
{
	int32 Guard = 0;
	while (bVolumeRecentring && Guard++ < 4096)
	{
		StepVolumeRecentre();
	}
	DrainVolumeUploads(-1);
}

void UVoxelGISubsystem::PushVolumeParamsIfChanged()
{
	FVoxelGIVolumeSettings New;
	New.bEnabled = VoxelGIVolume::IsEnabled() && bVolumeOriginSet;
	New.DebugVis = VoxelGIVolume::GetDebugVis();
	New.OriginPoolUU = CommittedOriginPoolUU;
	// The CAMERA in pool space, not the volume centre: this is the quantity the
	// CPU shade measures its fade from (GICentreUU), and B4.3. Subtracted in
	// double and narrowed once, same rule as the origin.
	New.FadeCentrePoolUU = FVector3f(FieldCentreUU - PoolWorldUU);
	New.Strength = VoxelGI::GetStrength();
	New.AmbientFloor = VoxelGI::GetAmbientFloor();

	// Risk 8, enforced rather than documented. Beyond the volume face AM_Clamp
	// returns the edge texel, so a fade that has not finished by then does not
	// fade at all -- GI cuts off as a hard, plausible-looking lighting ring.
	// Clamping here means a fade that is too wide for the configured VolumeDim
	// degrades to "fades slightly early" instead.
	const float HalfExtentUU = 0.5f * float(VolumeDim > 0 ? VolumeDim : VoxelGIVolume::GetDim())
	                         * float(VoxelLF::CellSizeUU);
	//
	// The clamp SLIDES the band rather than truncating it. Clamping FadeEnd alone
	// would leave FadeStart where it was and collapse a 1600 UU fade into a
	// 40 UU one at VolumeDim 192 -- which is the hard ring this is meant to
	// prevent, arrived at from the other direction. Preserving the band width and
	// moving both ends down degrades to "GI fades out somewhat early", which is
	// the failure mode a player cannot see.
	const float MaxFadeEndUU = FMath::Max(2.0f * float(VoxelLF::BrickEdgeUU),
	                                      HalfExtentUU - float(VoxelLF::BrickEdgeUU));
	const float WantEndUU = VoxelGI::GetFadeEndUU();
	const float WantStartUU = FMath::Min(VoxelGI::GetFadeStartUU(), WantEndUU - float(VoxelLF::CellSizeUU));
	const float ShiftUU = FMath::Max(0.0f, WantEndUU - MaxFadeEndUU);
	New.FadeEndUU = WantEndUU - ShiftUU;
	New.FadeStartUU = FMath::Max(float(VoxelLF::BrickEdgeUU), WantStartUU - ShiftUU);
	New.FadeStartUU = FMath::Min(New.FadeStartUU, New.FadeEndUU - float(VoxelLF::CellSizeUU));

	if (bVolumeSettingsValid && New == LastVolumeSettings)
	{
		return;
	}
	// Announce the SHADING terms whenever they move, not the origin/fade-centre
	// (which move every frame the camera does and would drown the log). This is
	// the line that makes a fired clamp visible: risk 8's failure mode is a
	// plausible image, so "the fade you asked for is not the fade you got" has to
	// be greppable rather than inferable.
	if (!bVolumeSettingsValid
	    || New.bEnabled != LastVolumeSettings.bEnabled
	    || New.Strength != LastVolumeSettings.Strength
	    || New.AmbientFloor != LastVolumeSettings.AmbientFloor
	    || New.FadeStartUU != LastVolumeSettings.FadeStartUU
	    || New.FadeEndUU != LastVolumeSettings.FadeEndUU
	    || New.DebugVis != LastVolumeSettings.DebugVis)
	{
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VoxelGI volume params: enabled=%d strength=%.3f ambientFloor=%.3f fade=%.0f..%.0f UU ")
		       TEXT("(cvars asked %.0f..%.0f; volume half-extent %.0f) debugVis=%d"),
		       New.bEnabled ? 1 : 0, New.Strength, New.AmbientFloor, New.FadeStartUU, New.FadeEndUU,
		       VoxelGI::GetFadeStartUU(), VoxelGI::GetFadeEndUU(), HalfExtentUU, New.DebugVis);
	}
	LastVolumeSettings = New;
	bVolumeSettingsValid = true;

	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeParams)(
		[New](FRHICommandListImmediate& RHICmdList)
	{
		GVoxelGIVolume.UpdateParameters_RenderThread(New);
	});
}

void UVoxelGISubsystem::TickVolume()
{
	if (!VoxelGIVolume::IsEnabled())
	{
		// One publish on the way down, so a live toggle-off actually reaches the
		// shader instead of leaving the last Enabled=1 buffer bound forever.
		if (bVolumeSettingsValid && LastVolumeSettings.bEnabled)
		{
			FVoxelGIVolumeSettings Off = LastVolumeSettings;
			Off.bEnabled = false;
			LastVolumeSettings = Off;
			ENQUEUE_RENDER_COMMAND(VoxelGIVolumeOff)(
				[Off](FRHICommandListImmediate&) { GVoxelGIVolume.UpdateParameters_RenderThread(Off); });
		}
		return;
	}
	if (!EnsureVolumeOrigin())
	{
		return;
	}
	if (!bVolumeAllocated)
	{
		bVolumeAllocated = true;
		ENQUEUE_RENDER_COMMAND(VoxelGIVolumeAlloc)(
			[](FRHICommandListImmediate& RHICmdList) { GVoxelGIVolume.EnsureAllocated_RenderThread(RHICmdList); });
	}

	if (bVolumeRecentring)
	{
		// The per-brick drain is suspended for the duration: every resident brick
		// is being re-encoded anyway, and letting the two upload paths interleave
		// would make "the origin uniform changed on exactly one frame" harder to
		// state than it needs to be.
		StepVolumeRecentre();
	}
	else if (VolumeNeedsRecentre())
	{
		BeginVolumeRecentre();
		StepVolumeRecentre();
	}
	else
	{
		DrainVolumeUploads(FMath::Max(0, CVarGIMaxBrickUploadsPerFrame.GetValueOnGameThread()));
	}

	PushVolumeParamsIfChanged();
}

int32 UVoxelGISubsystem::DrainVolumeUploads(int32 Budget)
{
	if (!Field || !VoxelGIVolume::IsEnabled())
	{
		return 0;
	}
	if (VolumeUploadQueue.Num() == 0 || !EnsureVolumeOrigin())
	{
		return 0;
	}

	// 1) Pop the frame's work list. A brick outside the volume's extent is
	//    dropped here rather than left in the queue: the volume covers
	//    +/-Dim*20 UU and the field's build radius is larger, so most of the
	//    field is legitimately off-volume and re-queuing it forever would turn
	//    the queue into a treadmill.
	TArray<FIntVector> Batch;
	TArray<FIntVector> TexelBase; // per Batch entry, texel coords of the brick's (0,0,0) cell
	while ((Budget < 0 || Batch.Num() < Budget) && VolumeUploadQueue.Num() > 0)
	{
		const FIntVector Key = VolumeUploadQueue[0];
		VolumeUploadQueue.RemoveAt(0, 1, EAllowShrinking::No);
		VolumeUploadSet.Remove(Key);

		const FIntVector Base(Key.X * VoxelLF::BrickEdgeCells - VolumeCellOrigin.X,
		                      Key.Y * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Y,
		                      Key.Z * VoxelLF::BrickEdgeCells - VolumeCellOrigin.Z);
		const int32 E = VoxelLF::BrickEdgeCells;
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			continue;
		}
		Batch.Add(Key);
		TexelBase.Add(Base);
	}
	if (Batch.Num() == 0)
	{
		return 0;
	}

	// 2) Encode into the CPU shadow, under ONE read scope, on the GAME thread.
	//    §1: anything that reads the field to build texel data must be here, not
	//    on the render thread.
	{
		const FVoxelLightField::FReadScope Read(*Field);
		uint8 BrickPos[FVoxelLightField::BrickTexelBytes];
		uint8 BrickNeg[FVoxelLightField::BrickTexelBytes];
		for (int32 I = 0; I < Batch.Num(); ++I)
		{
			// Returns false (and all-zero bytes) for a brick that is absent
			// (evicted) or re-voxelized-but-not-yet-solved. Both of those MUST
			// reach the volume, or a dug tunnel keeps its pre-dig lighting --
			// which is exactly why this path does not skip on false.
			Read.EncodeBrick(Batch[I], BrickPos, BrickNeg);

			const FIntVector& Base = TexelBase[I];
			for (int32 Z = 0; Z < VoxelLF::BrickEdgeCells; ++Z)
			for (int32 Y = 0; Y < VoxelLF::BrickEdgeCells; ++Y)
			{
				const int64 DstOffset = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + Base.X) * 4;
				const int32 SrcOffset = (Z * VoxelLF::BrickEdgeCells + Y) * VoxelLF::BrickEdgeCells * 4;
				FMemory::Memcpy(VolumeShadow.GetData() + DstOffset, BrickPos + SrcOffset,
				                VoxelLF::BrickEdgeCells * 4);
				FMemory::Memcpy(VolumeShadowNeg.GetData() + DstOffset, BrickNeg + SrcOffset,
				                VoxelLF::BrickEdgeCells * 4);
			}
		}
	}

	// 3) Merge into X-runs. THE reason this exists: the D3D12 staging row pitch
	//    is Align(Width*4, 256), so an 8-texel-wide brick stages 16 KB to move
	//    2 KB. Grouping by (texelY, texelZ) and coalescing along X turns a dig's
	//    125 per-brick calls into 25 wide ones with no waste.
	TArray<int32> Order;
	Order.SetNumUninitialized(TexelBase.Num());
	for (int32 I = 0; I < Order.Num(); ++I) { Order[I] = I; }
	Order.Sort([&TexelBase](int32 A, int32 B)
	{
		const FIntVector& VA = TexelBase[A];
		const FIntVector& VB = TexelBase[B];
		if (VA.Z != VB.Z) { return VA.Z < VB.Z; }
		if (VA.Y != VB.Y) { return VA.Y < VB.Y; }
		return VA.X < VB.X;
	});

	TArray<FVoxelGIVolumeRun> Runs;
	const int32 E = VoxelLF::BrickEdgeCells;
	int32 I = 0;
	while (I < Order.Num())
	{
		const FIntVector& Start = TexelBase[Order[I]];
		int32 EndX = Start.X + E; // exclusive
		int32 J = I + 1;
		while (J < Order.Num())
		{
			const FIntVector& Next = TexelBase[Order[J]];
			if (Next.Y != Start.Y || Next.Z != Start.Z)
			{
				break;
			}
			if (Next.X > EndX + kVolumeRunGapBricks * E)
			{
				break;
			}
			EndX = FMath::Max(EndX, Next.X + E);
			++J;
		}

		FVoxelGIVolumeRun& Run = Runs.AddDefaulted_GetRef();
		Run.Min = FIntVector(Start.X, Start.Y, Start.Z);
		Run.Size = FIntVector(EndX - Start.X, E, E);
		Run.TexelsPos.SetNumUninitialized(int64(Run.Size.X) * Run.Size.Y * Run.Size.Z * 4);
		Run.TexelsNeg.SetNumUninitialized(int64(Run.Size.X) * Run.Size.Y * Run.Size.Z * 4);
		// Compact the run out of the shadow. The shadow is the source of truth
		// for what is on the GPU, so a run always carries the LATEST bytes for
		// every brick it spans -- including bricks that were not in this frame's
		// batch but happen to sit inside a coalesced gap.
		for (int32 Z = 0; Z < Run.Size.Z; ++Z)
		for (int32 Y = 0; Y < Run.Size.Y; ++Y)
		{
			const int64 Src = ((int64(Run.Min.Z + Z) * VolumeDim + (Run.Min.Y + Y)) * VolumeDim + Run.Min.X) * 4;
			const int64 Dst = (int64(Z) * Run.Size.Y + Y) * Run.Size.X * 4;
			FMemory::Memcpy(Run.TexelsPos.GetData() + Dst, VolumeShadow.GetData() + Src,
			                int64(Run.Size.X) * 4);
			FMemory::Memcpy(Run.TexelsNeg.GetData() + Dst, VolumeShadowNeg.GetData() + Src,
			                int64(Run.Size.X) * 4);
		}
		I = J;
	}

	VolumeBricksUploaded += Batch.Num();
	VolumeUploadedThisFrame += Batch.Num();
	VolumeRunsUploaded += Runs.Num();
	if (FirstVolumeUploadSeconds == 0.0)
	{
		FirstVolumeUploadSeconds = FPlatformTime::Seconds();
	}

	// 4) One render command for the whole frame's runs -- BeginUpdateTexture3D
	//    asserts IsInParallelRenderingThread(), and the payload is owned by the
	//    lambda so nothing on the game thread can mutate it mid-upload.
	ENQUEUE_RENDER_COMMAND(VoxelGIVolumeUpload)(
		[Payload = MoveTemp(Runs)](FRHICommandListImmediate& RHICmdList)
	{
		for (const FVoxelGIVolumeRun& Run : Payload)
		{
			GVoxelGIVolume.UpdateTexels_RenderThread(RHICmdList, Run.Min, Run.Size,
			                                         Run.TexelsPos.GetData(), Run.TexelsNeg.GetData());
		}
	});

	return Batch.Num();
}

// --- voxel.GI.VolumeCheck ---------------------------------------------------
//
// THE DELIVERABLE FOR STEP 2. "The volume matches the field" is otherwise only
// checkable by looking at two screenshots of a cave, which is slow, subjective,
// and cannot distinguish a half-cell addressing shift from a fade retune. This
// turns it into a grep.
//
// Method: pick random SOLVED cells, jitter the probe point off the cell centre
// (a cell centre lands exactly on a texel and would make trilinear degenerate
// to a single tap -- a test that cannot fail), then compute both sides at the
// SAME point:
//
//   * the shader side, evaluated in software from VolumeShadow -- the actual
//     staged bytes, so an addressing or run-merge bug shows up here and a
//     re-encode would have hidden it;
//   * FVoxelLightField::SampleIrradiance at the surface point that puts its
//     FIRST probe offset (0.6 cells, the one the vertex shader uses) exactly on
//     that probe point.
//
// Errors are reported in irradiance BYTES (0..255), the units the field is
// stored in. Under Scheme A every direction is stored exactly, so BOTH classes
// must come out at 0.000 and the pass bar covers both. (Under the Scheme B this
// replaced, the +-X/+-Y classes carried a mean of ~9.5 and an RMS of ~21,
// because four directions shared one averaged channel.)
void UVoxelGISubsystem::RunVolumeCheck(int32 NumSamples)
{
	bVolumeCheckDone = true;

	if (!Field || !bVolumeOriginSet || VolumeShadow.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMECHECK: nothing staged (originSet=%d shadow=%d) -- needs voxel.GI.Volume 1, ")
		       TEXT("voxel.Stream.GPU 1 and a settled field."),
		       bVolumeOriginSet ? 1 : 0, VolumeShadow.Num());
		return;
	}

	// Flush the whole upload queue first, ignoring the per-frame budget. A
	// diagnostic that measures a volume which is merely BEHIND the field
	// measures the queue, not the encoding.
	const int32 BeforeFlush = VolumeBricksUploaded;
	FlushVolume();
	const int32 Flushed = VolumeBricksUploaded - BeforeFlush;

	TArray<FIntVector> Keys;
	Field->GetResidentKeys(Keys);

	// Only bricks fully inside the volume can be compared: outside it the
	// sampler clamps to the edge texel, which is the sampler doing what it was
	// asked and not a mismatch.
	TArray<FIntVector> InVolume;
	InVolume.Reserve(Keys.Num());
	const int32 E = VoxelLF::BrickEdgeCells;
	for (const FIntVector& Key : Keys)
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		// One cell of margin on every side, so all 8 trilinear taps of any
		// probe inside the brick are real texels rather than clamped ones.
		if (Base.X >= 1 && Base.Y >= 1 && Base.Z >= 1 &&
		    Base.X + E + 1 <= VolumeDim && Base.Y + E + 1 <= VolumeDim && Base.Z + E + 1 <= VolumeDim)
		{
			InVolume.Add(Key);
		}
	}

	if (InVolume.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMECHECK: cells=0 -- %d resident bricks but none inside the volume ")
		       TEXT("(dim=%d covers +/-%.0f UU around (%.0f,%.0f,%.0f)). Raise voxel.GI.VolumeDim."),
		       Keys.Num(), VolumeDim, 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.X + 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.Y + 0.5 * VolumeDim * VoxelLF::CellSizeUU,
		       VolumeOriginWorldUU.Z + 0.5 * VolumeDim * VoxelLF::CellSizeUU);
		return;
	}

	// Software trilinear over the staged bytes, term for term what
	// Texture3DSampleLevel + the divide in VoxelQuadVertexFactory.ush does.
	// UVW = (P - Origin) * InvSize, so the texel coordinate is
	// UVW*Dim - 0.5 == (P - Origin)/40 - 0.5, which is the CPU sampler's own
	// convention -- no half-texel fixup, because the origin is brick-snapped.
	const uint8* ShadowPos = VolumeShadow.GetData();
	const uint8* ShadowNeg = VolumeShadowNeg.GetData();
	// Slot is now (volume, channel): DirTable index >> 1 is the AXIS and the low
	// bit is the SIGN, matching the +X -X +Y -Y +Z -Z storage order.
	auto SampleShadow = [this, ShadowPos, ShadowNeg](const FVector& P, int32 Dir, float& OutIrr) -> bool
	{
		const double TX = (P.X - VolumeOriginWorldUU.X) / double(VoxelLF::CellSizeUU) - 0.5;
		const double TY = (P.Y - VolumeOriginWorldUU.Y) / double(VoxelLF::CellSizeUU) - 0.5;
		const double TZ = (P.Z - VolumeOriginWorldUU.Z) / double(VoxelLF::CellSizeUU) - 0.5;
		const int32 BX = int32(FMath::FloorToDouble(TX));
		const int32 BY = int32(FMath::FloorToDouble(TY));
		const int32 BZ = int32(FMath::FloorToDouble(TZ));
		const float FX = float(TX - double(BX));
		const float FY = float(TY - double(BY));
		const float FZ = float(TZ - double(BZ));

		const uint8* Shadow = ((Dir & 1) == 0) ? ShadowPos : ShadowNeg;
		const int32 Channel = Dir >> 1;

		float AccumRGB = 0.f;
		float AccumA = 0.f;
		for (int32 T = 0; T < 8; ++T)
		{
			const int32 OX = T & 1, OY = (T >> 1) & 1, OZ = (T >> 2) & 1;
			const float W = (OX ? FX : 1.f - FX) * (OY ? FY : 1.f - FY) * (OZ ? FZ : 1.f - FZ);
			if (W <= 0.f)
			{
				continue;
			}
			// AM_Clamp on all three axes, matching the sampler state.
			const int32 CX = FMath::Clamp(BX + OX, 0, VolumeDim - 1);
			const int32 CY = FMath::Clamp(BY + OY, 0, VolumeDim - 1);
			const int32 CZ = FMath::Clamp(BZ + OZ, 0, VolumeDim - 1);
			const uint8* Texel = Shadow + ((int64(CZ) * VolumeDim + CY) * VolumeDim + CX) * 4;
			AccumRGB += W * float(Texel[Channel]) * (1.f / 255.f);
			AccumA += W * float(Texel[3]) * (1.f / 255.f);
		}
		if (AccumA <= 1.e-4f)
		{
			return false; // the shader's "W ~= 0 -> plain AO" branch
		}
		OutIrr = AccumRGB / AccumA;
		return true;
	};

	// Same offset the vertex shader pushes the probe out by (kGIProbeOffsetUU
	// = 0.6 * 40), which is also SampleIrradiance's FIRST probe offset. Feeding
	// the CPU side a surface point P - N*24 makes its first probe land exactly
	// on P, so both sides read the same 8 taps. Samples where that first probe
	// misses are counted, not silently averaged in: the CPU would have walked
	// on to 1.25 and 2.0 cells and the two sides would no longer be comparing
	// the same point.
	constexpr float kProbeOffsetUU = 0.6f * float(VoxelLF::CellSizeUU);

	struct FClassStats
	{
		int32 Count = 0;
		double SumAbsErr = 0.0;
		double SumSqErr = 0.0;
		double MaxAbsErr = 0.0;
		// Kept so the TAIL can be reported, not just the mean. Step 3's decision
		// turns on the horizontal class, whose mean passed its bar at ~6 bytes
		// while maxAbsErr sat at ~102 -- a mean that low with a max that high is
		// either a thin tail (fine, ship Scheme B) or a fat one (a visible defect
		// hiding behind an average), and only percentiles tell those apart.
		TArray<double> Samples;
		void Add(double ErrBytes)
		{
			++Count;
			SumAbsErr += ErrBytes;
			SumSqErr += ErrBytes * ErrBytes;
			MaxAbsErr = FMath::Max(MaxAbsErr, ErrBytes);
			Samples.Add(ErrBytes);
		}
		double Mean() const { return Count > 0 ? SumAbsErr / Count : 0.0; }
		// §2 asked for an RMS and the transcript reported a mean-abs. RMS is the
		// stricter statistic (it weights the tail), so reporting both is what
		// makes the two comparable rather than merely both present.
		double Rms() const { return Count > 0 ? FMath::Sqrt(SumSqErr / Count) : 0.0; }
		double Percentile(double P)
		{
			if (Samples.Num() == 0) { return 0.0; }
			Samples.Sort();
			const int32 Idx = FMath::Clamp(int32(P * double(Samples.Num() - 1) + 0.5), 0, Samples.Num() - 1);
			return Samples[Idx];
		}
		// Share of samples above the design doc's "free quality" bar of 8/255.
		double FractionAbove(double Bytes) const
		{
			if (Count == 0) { return 0.0; }
			int32 N = 0;
			for (double S : Samples) { if (S > Bytes) { ++N; } }
			return double(N) / double(Count);
		}
	};
	// Under Scheme A BOTH classes are exact by construction, so both must read
	// 0.000. They are still reported separately because that is what makes the
	// switch from Scheme B legible: the horizontal line is the one that used to
	// carry a mean of ~9.5 and an RMS of ~21, and seeing it go to zero is the
	// measurement that the encoding actually changed.
	FClassStats StatsZ;   // +-Z normals
	FClassStats StatsXY;  // +-X/+-Y normals
	// NEGATIVE CONTROL. Scheme B is exact on +-Z by construction, so a correct
	// implementation reports 0.000 -- and a harness that reports 0.000 because
	// it is comparing a thing against itself reports exactly the same number.
	// This runs the identical comparison with the volume probe displaced HALF A
	// CELL in X, i.e. the smallest addressing mistake the origin snap exists to
	// prevent. It must come out large. If it does not, the measurement above is
	// worthless and nothing else in this log line should be believed.
	FClassStats StatsControl;
	int32 CpuMiss = 0;    // volume had data, field's first probe did not
	int32 VolMiss = 0;    // field had data, volume did not (upload lag or a hole)
	int32 Attempts = 0;
	int32 Unsolved = 0;
	// How much dynamic range the +-Z measurement actually had. A field of
	// uniformly-1.0 irradiance -- which is what open sky looks like, since an
	// unoccluded cell solves to exactly 1.0 -- would make any encoding score
	// 0.000 regardless of whether it works. Reported so "meanAbsErr=0.000" can
	// be read together with the spread of the values it was computed over.
	double SignalSum = 0.0;
	double SignalSumSq = 0.0;
	double SignalMin = 1.0;

	// Deterministic stream, so two runs of the same build compare like for like.
	FRandomStream Rand(0x5EED0002);
	const int32 Target = FMath::Clamp(NumSamples, 1, 1 << 20);
	const int32 MaxAttempts = Target * 32;

	{
		const FVoxelLightField::FReadScope Read(*Field);
		while (StatsZ.Count + StatsXY.Count < Target && Attempts < MaxAttempts)
		{
			++Attempts;
			const FIntVector Key = InVolume[Rand.RandHelper(InVolume.Num())];
			const FVoxelLFBrick* Brick = Read.FindBrick(Key);
			if (!Brick || !Brick->bSolved)
			{
				continue;
			}
			const int32 CX = Rand.RandHelper(E);
			const int32 CY = Rand.RandHelper(E);
			const int32 CZ = Rand.RandHelper(E);
			if (!Brick->SolvedCells[VoxelLF::CellIndex(CX, CY, CZ)])
			{
				++Unsolved;
				continue;
			}

			// Cell centre, jittered inside the cell so trilinear is actually
			// exercised. On a centre exactly, the frac terms are 0 and the
			// filter collapses to one tap.
			const FVector Probe(
				(double(Key.X * E + CX) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU,
				(double(Key.Y * E + CY) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU,
				(double(Key.Z * E + CZ) + 0.5 + (Rand.GetFraction() - 0.5)) * VoxelLF::CellSizeUU);

			// 0..5 in DirTable order: +X -X +Y -Y +Z -Z. The shader's slot
			// selection is on the world normal's z, so +-Z map to R/G and the
			// four horizontals all map to B.
			const int32 Dir = Rand.RandHelper(VoxelLF::NumDirs);
			const FVector3f Normal = VoxelLF::DirTable[Dir];

			float VolIrr = 0.f;
			const bool bVolOk = SampleShadow(Probe, Dir, VolIrr);

			const FVector Surface = Probe - FVector(Normal) * double(kProbeOffsetUU);
			float FieldIrr = 0.f;
			const bool bFieldOk = Read.Sample(Surface, Normal, FieldIrr);

			if (!bVolOk && !bFieldOk)
			{
				continue; // both fall back to plain AO -- agreement, but no signal
			}
			if (!bVolOk)
			{
				++VolMiss;
				continue;
			}
			if (!bFieldOk)
			{
				++CpuMiss;
				continue;
			}

			const double ErrBytes = FMath::Abs(double(FieldIrr) - double(VolIrr)) * 255.0;
			if (Dir >= 4)
			{
				StatsZ.Add(ErrBytes);
				SignalSum += double(FieldIrr);
				SignalSumSq += double(FieldIrr) * double(FieldIrr);
				SignalMin = FMath::Min(SignalMin, double(FieldIrr));

				float ShiftedIrr = 0.f;
				if (SampleShadow(Probe + FVector(0.5 * VoxelLF::CellSizeUU, 0, 0), Dir, ShiftedIrr))
				{
					StatsControl.Add(FMath::Abs(double(FieldIrr) - double(ShiftedIrr)) * 255.0);
				}
			}
			else
			{
				StatsXY.Add(ErrBytes);
			}
		}
	}

	// THE grep line. Headline is the +-Z class, because that is the one Scheme B
	// claims to reproduce exactly and therefore the one that can falsify the
	// encoding, the addressing, the origin snap and the run merge in one number.
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK: cells=%d meanAbsErr=%.3f rms=%.3f maxAbsErr=%.3f"),
	       StatsZ.Count, StatsZ.Mean(), StatsZ.Rms(), StatsZ.MaxAbsErr);
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK horizontal (+-X/+-Y, Scheme A exact channel): cells=%d meanAbsErr=%.3f rms=%.3f ")
	       TEXT("p50=%.3f p90=%.3f p95=%.3f p99=%.3f maxAbsErr=%.3f frac>8/255=%.4f"),
	       StatsXY.Count, StatsXY.Mean(), StatsXY.Rms(),
	       StatsXY.Percentile(0.50), StatsXY.Percentile(0.90), StatsXY.Percentile(0.95),
	       StatsXY.Percentile(0.99), StatsXY.MaxAbsErr, StatsXY.FractionAbove(8.0));
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK control (+-Z, volume probe shifted half a cell in X -- MUST be large, else the ")
	       TEXT("line above is measuring nothing): cells=%d meanAbsErr=%.3f rms=%.3f maxAbsErr=%.3f"),
	       StatsControl.Count, StatsControl.Mean(), StatsControl.Rms(), StatsControl.MaxAbsErr);
	{
		const double N = FMath::Max(1, StatsZ.Count);
		const double SigMean = SignalSum / N;
		const double SigVar = FMath::Max(0.0, SignalSumSq / N - SigMean * SigMean);
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMECHECK signal (+-Z sampled irradiance, the range the error above was measured over): ")
		       TEXT("mean=%.1f sd=%.1f min=%.1f bytes"),
		       SigMean * 255.0, FMath::Sqrt(SigVar) * 255.0, SignalMin * 255.0);
	}
	// Pass bar, in irradiance bytes. Under Scheme A EVERY direction is stored
	// exactly, so the bar now covers both classes -- there is no longer a class
	// that is allowed to be approximate, and anything above a byte of mean error
	// on either is an addressing, origin or staging bug.
	//
	// The bar is a MEAN here and that is deliberate, unlike the design doc's
	// Scheme A-vs-B bar which asked for an RMS and was fed a mean-abs for three
	// transcripts running. This one is checking "is the pipeline exact", where a
	// mean of zero and an RMS of zero are the same statement; the RMS is logged
	// beside it anyway so the two can never drift apart unnoticed again.
	constexpr double kPassMeanBytes = 2.0;
	const bool bPass = StatsZ.Count > 0 && StatsZ.Mean() < kPassMeanBytes
	                && StatsXY.Count > 0 && StatsXY.Mean() < kPassMeanBytes;
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMECHECK detail: verdict=%s (bar: BOTH classes mean < %.1f bytes) | dim=%d bricksResident=%d bricksInVolume=%d ")
	       TEXT("flushedNow=%d uploadedTotal=%d runsTotal=%d queueLeft=%d | attempts=%d unsolvedCell=%d ")
	       TEXT("volumeMiss=%d fieldFirstProbeMiss=%d | originWorld=(%.0f,%.0f,%.0f)"),
	       bPass ? TEXT("PASS") : TEXT("FAIL"),
	       kPassMeanBytes, VolumeDim, Keys.Num(), InVolume.Num(),
	       Flushed, VolumeBricksUploaded, VolumeRunsUploaded, VolumeUploadQueue.Num(),
	       Attempts, Unsolved, VolMiss, CpuMiss,
	       VolumeOriginWorldUU.X, VolumeOriginWorldUU.Y, VolumeOriginWorldUU.Z);
}

// --- voxel.GI.VolumeDigTest -------------------------------------------------
//
// THE DELIVERABLE FOR TWO STEP-2 CLAIMS THAT WERE "CORRECT BY CONSTRUCTION ONLY".
//
//   * The X-run upload merge exists for the DIG case -- a contiguous 5x5x5 brick
//     neighbourhood -- and the only number ever measured for it was 1.4
//     bricks/run in STEADY STATE, where the round-robin re-solve delivers bricks
//     in TMap iteration order and X-adjacent pairs are rare by construction. That
//     number says nothing about the case the merge was built for.
//   * zero-on-revoxelize: a dug brick must reach the volume as A=0 (which the
//     shader reads as "no data" and turns into plain AO) BEFORE its re-solve
//     lands, or the tunnel keeps its pre-dig lighting for as long as the solve
//     queue is deep. Never observed, only argued.
//   * zero-on-evict: same, for a brick that leaves the resident set.
//
// Method: carve a real sphere through the real edit path at the camera, suppress
// the round-robin re-solve for the duration so the upload queue contains the
// dig's bricks and nothing else, then flush and read the answer out of
// VolumeShadow -- the actual staged bytes, not a re-encode.
void UVoxelGISubsystem::StepDigTest()
{
	if (DigTestPhase == 0)
	{
		return;
	}
	// A runtime toggle-off clears the field out from under an armed test.
	if (!Field || !bVolumeOriginSet || VolumeShadow.Num() == 0)
	{
		DigTestPhase = 0;
		DigTestBricks.Reset();
		return;
	}

	const int32 E = VoxelLF::BrickEdgeCells;
	auto BrickAllZero = [this, E](const FIntVector& Key, int32& OutInside) -> bool
	{
		const FIntVector Base(Key.X * E - VolumeCellOrigin.X,
		                      Key.Y * E - VolumeCellOrigin.Y,
		                      Key.Z * E - VolumeCellOrigin.Z);
		if (Base.X < 0 || Base.Y < 0 || Base.Z < 0 ||
		    Base.X + E > VolumeDim || Base.Y + E > VolumeDim || Base.Z + E > VolumeDim)
		{
			return true; // outside the volume; not counted
		}
		++OutInside;
		for (int32 Z = 0; Z < E; ++Z)
		for (int32 Y = 0; Y < E; ++Y)
		for (int32 X = 0; X < E; ++X)
		{
			const int64 Off = ((int64(Base.Z + Z) * VolumeDim + (Base.Y + Y)) * VolumeDim + (Base.X + X)) * 4;
			if (VolumeShadow[Off + 3] != 0 || VolumeShadowNeg[Off + 3] != 0) { return false; }
		}
		return true;
	};

	const double Now = FPlatformTime::Seconds();

	if (DigTestPhase == 1)
	{
		// Wait until the dig's remeshes have been ingested. The streaming system
		// budgets remeshes per frame, so this is several frames, not one.
		if ((PendingVoxelize.Num() > 0 || PendingPooledVoxelize.Num() > 0)
		    && Now - DigTestPhaseSeconds < 5.0)
		{
			return;
		}

		const int32 BricksBefore = VolumeBricksUploaded;
		const int32 RunsBefore = VolumeRunsUploaded;
		FlushVolume();
		const int32 Bricks = VolumeBricksUploaded - BricksBefore;
		const int32 Runs = VolumeRunsUploaded - RunsBefore;

		int32 Inside = 0;
		int32 Zeroed = 0;
		for (const FIntVector& Key : DigTestBricks)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			Inside += WasInside;
			if (WasInside > 0 && bZero) { ++Zeroed; }
		}

		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG revoxelize: dirtied=%d bricks (%d inside the volume) | uploaded %d bricks in %d runs ")
		       TEXT("= %.2f bricks/run | zeroed=%d/%d %s"),
		       DigTestBricks.Num(), Inside, Bricks, Runs,
		       Runs > 0 ? double(Bricks) / double(Runs) : 0.0,
		       Zeroed, Inside,
		       (Inside > 0 && Zeroed == Inside) ? TEXT("PASS") : TEXT("FAIL"));

		DigTestPhase = 2;
		DigTestPhaseSeconds = Now;
		return;
	}

	if (DigTestPhase == 2)
	{
		// Let the solve land, then confirm the same bricks come BACK -- otherwise
		// "zeroed" would also be the reading for "the upload path is broken".
		if (Now - DigTestPhaseSeconds < 6.0)
		{
			return;
		}
		FlushVolume();
		int32 Inside = 0;
		int32 Relit = 0;
		for (const FIntVector& Key : DigTestBricks)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			Inside += WasInside;
			if (WasInside > 0 && !bZero) { ++Relit; }
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG resolve: %d/%d bricks carry data again after the solve %s"),
		       Relit, Inside, (Inside > 0 && Relit > 0) ? TEXT("PASS") : TEXT("FAIL"));

		// --- zero-on-evict, through the real eviction path -------------------
		//
		// Driven rather than waited for: eviction is distance-based and at the
		// default radius nothing near the camera ever evicts, so waiting for one
		// would mean flying for a minute and hoping. This calls the same
		// EvictFarBricks the tick calls, with a radius chosen to catch a handful,
		// and pushes the result through the same PushVolumeUpload the tick uses.
		// It is destructive -- the evicted bricks come back only when their chunk
		// re-meshes -- which is why it lives behind a diagnostic command.
		TArray<FIntVector> Evicted;
		const double EvictRadiusUU = 0.35 * 0.5 * double(VolumeDim) * double(VoxelLF::CellSizeUU);
		const int32 NumEvicted = Field->EvictFarBricks(FieldCentreUU, EvictRadiusUU, 32, Evicted);
		for (const FIntVector& Key : Evicted)
		{
			BrickComponents.Remove(Key);
			PushVolumeUpload(Key);
		}
		FlushVolume();
		int32 EInside = 0;
		int32 EZeroed = 0;
		for (const FIntVector& Key : Evicted)
		{
			int32 WasInside = 0;
			const bool bZero = BrickAllZero(Key, WasInside);
			EInside += WasInside;
			if (WasInside > 0 && bZero) { ++EZeroed; }
		}
		UE_LOG(LogVoxelGI, Log,
		       TEXT("VOLUMEDIG evict: evicted=%d (%d inside the volume at radius %.0f UU) zeroed=%d %s"),
		       NumEvicted, EInside, EvictRadiusUU, EZeroed,
		       (EInside > 0 && EZeroed == EInside) ? TEXT("PASS") : TEXT("FAIL (0 inside = inconclusive)"));

		DigTestPhase = 0;
		DigTestBricks.Reset();
		bCoarseDirty = true;
		RefreshRotation.Reset();
		RefreshCursor = 0;
	}
}

void UVoxelGISubsystem::StartDigTest(double RadiusUU)
{
	if (!Field || !VoxelGIVolume::IsEnabled() || !bVolumeOriginSet)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMEDIG: needs voxel.GI.Enabled 1, voxel.GI.Volume 1, voxel.Stream.GPU 1 and a settled ")
		       TEXT("field (originSet=%d)."), bVolumeOriginSet ? 1 : 0);
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* WorldSub = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!WorldSub)
	{
		return;
	}

	// Straight down from the camera, which is where a dig actually goes and, more
	// usefully, is guaranteed to hit solid geometry on a surface spawn.
	const FVector CentreUU = FieldCentreUU - FVector(0, 0, 2.0 * double(VoxelLF::BrickEdgeUU));
	const int32 Removed = WorldSub->CarveSphere(CentreUU, RadiusUU, 0.0);

	// The bricks a dig of this size dirties: the touched neighbourhood plus the
	// voxel.GI.EditDirtyRadiusBricks halo, which is what MarkBrickNeighbourhoodDirty
	// will expand it to.
	const FIntVector Centre = FVoxelLightField::WorldToBrick(CentreUU);
	const int32 R = 1 + FMath::CeilToInt(RadiusUU / double(VoxelLF::BrickEdgeUU));
	DigTestBricks.Reset();
	for (int32 DZ = -R; DZ <= R; ++DZ)
	for (int32 DY = -R; DY <= R; ++DY)
	for (int32 DX = -R; DX <= R; ++DX)
	{
		DigTestBricks.Add(Centre + FIntVector(DX, DY, DZ));
	}

	DigTestPhase = 1;
	DigTestPhaseSeconds = FPlatformTime::Seconds();
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMEDIG: carved %d voxels at (%.0f,%.0f,%.0f) r=%.0f UU; watching %d bricks. ")
	       TEXT("Round-robin re-solve suppressed until the test completes so the run-merge number is the DIG's."),
	       Removed, CentreUU.X, CentreUU.Y, CentreUU.Z, RadiusUU, DigTestBricks.Num());
}

// Forces a re-centre by the given number of BRICKS, on whatever field is
// resident right now.
//
// WHY THIS EXISTS. The obvious verification -- fly the scripted flight and watch
// for a pop -- does not work, and the run that showed that is worth recording:
// under `-VoxelPerfFlight=surface` the light field holds **0-12 bricks** against
// 2,212 when settled, with `pendingVox=0(+0 pooled)` throughout. Re-centres fire
// correctly (7 of them, each exactly 8 frames, one-frame origin commit) but every
// one restages an EMPTY volume, so the transient accounting reads 0 of 0 occupied
// texels and proves nothing about a pop. A settled field that is then forced to
// re-centre is the only way to put real data through the staged path.
void UVoxelGISubsystem::ForceVolumeRecentre(int32 ShiftBricks)
{
	if (!Field || !bVolumeOriginSet || VolumeDim <= 0 || bVolumeRecentring)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("VOLUMERECENTRE: not ready (originSet=%d dim=%d inFlight=%d)"),
		       bVolumeOriginSet ? 1 : 0, VolumeDim, bVolumeRecentring ? 1 : 0);
		return;
	}
	// Pretend the camera sits ShiftBricks further along +X than it does. Nothing
	// else about the path is faked: BeginVolumeRecentre re-snaps, re-buckets and
	// restages exactly as a real camera move would.
	const FVector Saved = FieldCentreUU;
	FieldCentreUU.X += double(ShiftBricks) * double(VoxelLF::BrickEdgeUU);
	BeginVolumeRecentre();
	FieldCentreUU = Saved;
	UE_LOG(LogVoxelGI, Log,
	       TEXT("VOLUMERECENTRE: forced a %d-brick (%.0f UU) shift over a field of %d bricks"),
	       ShiftBricks, double(ShiftBricks) * VoxelLF::BrickEdgeUU, Field->NumBricks());
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GVoxelGIVolumeRecentreTestCmd(
		TEXT("voxel.GI.VolumeRecentreTest"),
		TEXT("Force a staged volume re-centre of N bricks (default 8 = 2560 UU, the default dead zone) ")
		TEXT("on the CURRENTLY RESIDENT field, and report the transient. Exists because the scripted ")
		TEXT("flight cannot verify this: under motion the light field holds single-digit bricks, so its ")
		TEXT("re-centres restage an empty volume and cannot show a pop. Needs voxel.GI.Debug 2 for the ")
		TEXT("transient line."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const int32 Bricks = Args.Num() > 0 ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 64) : 8;
			GI->ForceVolumeRecentre(Bricks);
		}
	}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelGIVolumeDigTestCmd(
		TEXT("voxel.GI.VolumeDigTest"),
		TEXT("Carve a sphere below the camera and measure the three GPU-volume behaviours that were ")
		TEXT("previously correct-by-construction only: the X-run upload merge on a dig's CONTIGUOUS brick ")
		TEXT("neighbourhood (the case it was built for -- the only number on record, 1.4 bricks/run, is ")
		TEXT("steady state), zero-on-revoxelize, and zero-on-evict. Optional argument: carve radius in UU ")
		TEXT("(default 300). Logs VOLUMEDIG lines. DESTRUCTIVE: it really digs, and it really evicts."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
			[](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) { return; }
		if (UVoxelGISubsystem* GI = World->GetSubsystem<UVoxelGISubsystem>())
		{
			const double RadiusUU = Args.Num() > 0 ? FMath::Clamp(FCString::Atod(*Args[0]), 40.0, 2000.0) : 300.0;
			GI->StartDigTest(RadiusUU);
		}
	}));
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
		// §3.3 row 2: re-voxelized and not yet solved has to ZERO the texels.
		// VoxelizeChunk has just cleared Vis/SolvedCells, so the encode produces
		// all-zero bytes; skipping this would leave a dug tunnel lit with its
		// pre-dig irradiance until the solve landed several frames later.
		PushVolumeUpload(BrickCoord);
		bCoarseDirty = true;
		++Voxelized;
	}

	// 1b) Pooled chunks, from the SAME budget. Identical admission rules
	//     (build radius, brick cap) applied to a payload the queue carries
	//     itself instead of reading back off a component -- see
	//     NotifyPooledChunkMeshUpdated. Draining both queues against one
	//     `Voxelized` counter is what keeps voxel.GI.MaxVoxelizePerFrame a
	//     bound on TOTAL work rather than per-renderer work; with
	//     voxel.Stream.GPUMaxLevel splitting the rings, both can be non-empty
	//     in the same frame.
	while (Voxelized < VoxelizeBudget && PendingPooledVoxelize.Num() > 0)
	{
		FPendingPooledChunk Pending = MoveTemp(PendingPooledVoxelize[0]);
		PendingPooledVoxelize.RemoveAt(0, 1, EAllowShrinking::No);

		if (FVector::Dist(Pending.OriginUU, FieldCentreUU) > BuildRadiusUU)
		{
			continue; // outside the GI ring; nothing to build
		}
		const FIntVector BrickCoord = FVoxelLightField::WorldToBrick(Pending.OriginUU);
		if (Field->NumBricks() >= MaxBricks && !Field->HasBrick(BrickCoord))
		{
			continue; // at the memory cap and this would be a new brick
		}

		Field->VoxelizeChunk(BrickCoord, Pending.OriginUU, Pending.Quads);
		// No BrickComponents entry: a pooled chunk has no component to
		// re-shade (see the member's comment). Drop any stale entry a
		// component-path residency of this same brick left behind, so the
		// re-shade phase cannot write colours from this brick's new
		// irradiance into a component that no longer owns it -- only reachable
		// via a mid-session voxel.Stream.GPU flip, but free to get right.
		BrickComponents.Remove(BrickCoord);
		MarkBrickNeighbourhoodDirty(BrickCoord, EditRadius);
		PushVolumeUpload(BrickCoord); // zero-on-revoxelize, as above
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
	Params.bLegacyConeBasis = CVarGILegacyConeBasis.GetValueOnGameThread() != 0;

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

	// Suppressed for the duration of voxel.GI.VolumeDigTest: the round-robin
	// delivers bricks in TMap iteration order, and mixing those into the upload
	// queue is exactly what makes the steady-state 1.4 bricks/run number
	// uninformative about the dig case.
	const int32 RefreshBudget = (DigTestPhase != 0)
		? 0 : FMath::Max(0, CVarGIRefreshBricksPerFrame.GetValueOnGameThread());
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
			// ONLY bricks a component actually owns go on the re-shade queue.
			//
			// Step 5's cost retirement, and it is smaller and more specific than
			// the roadmap claimed. Pooled bricks have no BrickComponents entry by
			// design (see the member's comment), so every pop of one was
			// guaranteed to miss: under voxel.Stream.GPU 1 the queue filled with
			// ~2,000 entries that could only ever be popped and discarded, which
			// is what forced the 8x pop cap below. Not enqueueing them at all
			// deletes the queue growth, the RemoveAt(0) memmove and the TSet
			// churn outright, and the pop cap stops being load-bearing.
			//
			// This is NOT "the volume replaces a re-shade": there was never a
			// re-shade to replace on the pooled path. What the volume replaces is
			// the ABSENCE of one.
			if (BrickComponents.Contains(Key))
			{
				bool bAlready = false;
				RefreshSet.Add(Key, &bAlready);
				if (!bAlready)
				{
					RefreshQueue.Add(Key);
				}
			}
			// §3.3 row 1: brick solved -> write Vis*v, v. Same work list as the
			// component path's re-shade, separate cursor (see the member decl).
			PushVolumeUpload(Key);
		}
	}

	// 4) Re-shade chunks whose irradiance changed.
	//
	//    THIS USED TO BE MarkRenderStateDirty(), i.e. a full scene-proxy
	//    rebuild per chunk: it regenerated positions, tangents, world-planar
	//    UVs and indices that had not changed, allocated five fresh RHI
	//    buffers, and pushed a primitive remove+add through the scene, all to
	//    alter ONE BYTE per vertex (VertexColor.G).
	//
	//    MEASURED WORTH, and it is less than the standing "~1.7 ms, the
	//    dominant cost" note claimed. Interleaved 4-round A/B from one binary
	//    (voxel.GI.LegacyProxyRebuild, 2026-07-22, contended box): the rebuild
	//    costs about 0.15 ms p50 and 0.72 ms p95 over the in-place update, and
	//    -- the more interesting part -- 15 post-warmup hitches against 3, plus
	//    a visible downward drift in chunks/s as a run progresses that the
	//    in-place path does not show. So the proxy churn was mostly a TAIL and
	//    throughput problem, not a median one. GI's median cost lives
	//    elsewhere: see the attribution note on
	//    voxel.GI.MaxQuadSpanVoxels.
	//
	//    UpdateGIVertexColors instead recomputes only the colour stream and
	//    memcpys it into the existing proxy's colour vertex buffer (see
	//    VoxelChunkComponent.cpp). Geometry is identical across a GI re-shade
	//    by construction: this queue only ever holds chunks whose IRRADIANCE
	//    changed -- a chunk whose geometry changed arrives through
	//    SetChunkQuads, which already rebuilds the proxy for its own reasons.
	//    MarkRenderStateDirty remains the fallback for the cases the fast path
	//    declines (no proxy yet), so behaviour is unchanged where it cannot
	//    apply.
	const int32 RefreshChunkBudget = FMath::Max(0, CVarGIMaxChunkRefreshesPerFrame.GetValueOnGameThread());
	// Read from the COMMAND LINE as well as the cvar: an -ExecCmds cvar lands
	// after streaming has already begun, so an -ExecCmds A/B silently measures
	// the same state twice (this cost three runs to discover on -VoxelGIOn).
	static const bool bLegacyRefreshCmdLine = FParse::Param(FCommandLine::Get(), TEXT("VoxelGILegacyRefresh"));
	const bool bLegacyProxyRebuild = bLegacyRefreshCmdLine || CVarGILegacyProxyRebuild.GetValueOnGameThread() != 0;

	// PER-COMPONENT dedupe, on top of the queue's existing per-BRICK dedupe.
	//
	// The queue is keyed by brick, but re-shading is per CHUNK and always
	// recomputes the chunk's WHOLE colour array. A chunk covers many bricks, so
	// a single edit -- which dirties a 5x5x5 brick neighbourhood by default
	// (voxel.GI.EditDirtyRadiusBricks) -- used to pop several bricks that all
	// resolve to the same component and pay for that chunk's full re-shade once
	// EACH. The second and subsequent recomputes of a frame read the same field
	// state and produce the same bytes, so they were pure waste.
	//
	// Skipped duplicates deliberately do NOT consume the frame's budget: the
	// budget exists to bound work done, and a dedupe hit does none.
	//
	// POPS are bounded as well as refreshes, which they were not before the
	// pooled path existed. A pooled brick has no BrickComponents entry, so
	// every pop of one misses and `Refreshed` never advances -- unbounded, this
	// loop would RemoveAt(0) its way through the whole ~2000-entry queue every
	// frame, an O(n^2) memmove producing no output at all. 8x the refresh
	// budget leaves the dedupe-skip case (the reason misses deliberately do not
	// consume the refresh budget) behaving exactly as before on the component
	// path, where the queue is short and the bound never binds.
	TSet<UVoxelChunkComponent*> RefreshedThisFrame;
	int32 Refreshed = 0;
	int32 DedupeSkips = 0;
	int32 Popped = 0;
	const int32 MaxPops = FMath::Max(8 * RefreshChunkBudget, 32);
	while (Refreshed < RefreshChunkBudget && Popped < MaxPops && RefreshQueue.Num() > 0)
	{
		++Popped;
		const FIntVector Key = RefreshQueue[0];
		RefreshQueue.RemoveAt(0, 1, EAllowShrinking::No);
		RefreshSet.Remove(Key);
		if (TWeakObjectPtr<UVoxelChunkComponent>* Found = BrickComponents.Find(Key))
		{
			if (UVoxelChunkComponent* Comp = Found->Get())
			{
				bool bAlreadyRefreshed = false;
				RefreshedThisFrame.Add(Comp, &bAlreadyRefreshed);
				if (bAlreadyRefreshed)
				{
					++DedupeSkips;
					continue;
				}
				if (bLegacyProxyRebuild || !Comp->UpdateGIVertexColors())
				{
					Comp->MarkRenderStateDirty();
				}
				++Refreshed;
			}
			else
			{
				BrickComponents.Remove(Key);
			}
		}
	}

	// 4b) GPU volume texel uploads -- the pooled path's replacement for (4).
	//
	//     A pooled chunk has no component and no per-chunk colour buffer, so
	//     baked per-vertex GI simply does not exist there; its lighting comes
	//     from this volume instead. The two drains run side by side rather than
	//     one replacing the other because voxel.Stream.GPUMaxLevel can put both
	//     renderers in one frame.
	//
	//     Costs nothing with voxel.GI.Volume 0: both PushVolumeUpload and this
	//     drain return on the cvar read before touching the queue.
	VolumeUploadedThisFrame = 0;
	TickVolume();
	StepDigTest();
	const int32 VolumeUploaded = VolumeUploadedThisFrame;

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
			// §3.3 row 3: evicted -> zero it. Safe precisely because of the
			// validity channel: A = 0 reads as "no data" and falls back to plain
			// AO, never to black.
			PushVolumeUpload(Key);
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

	// 6a) voxel.GI.VolumeCheck -- the field-vs-volume equivalence harness.
	//     Armed by a non-zero cvar, run ONCE, and re-armable by setting it back
	//     to 0. Deferred by VolumeCheckSettleSeconds after the first upload for
	//     the same reason the convergence harness has a settle: a field that has
	//     barely streamed in reports a handful of cells and proves nothing.
	{
		const int32 CheckArm = CVarGIVolumeCheck.GetValueOnGameThread();
		if (CheckArm == 0)
		{
			bVolumeCheckDone = false;
		}
		else if (!bVolumeCheckDone && FirstVolumeUploadSeconds > 0.0 &&
		         Now - FirstVolumeUploadSeconds >= double(CVarGIVolumeCheckSettle.GetValueOnGameThread()))
		{
			RunVolumeCheck(CheckArm > 1 ? CheckArm : kVolumeCheckDefaultSamples);
		}
	}

	// 6) Convergence harness. Last thing in the tick so the field is in its
	//    normal steady state when it runs.
	if (ConvergePasses > 0 && !bConvergeDone)
	{
		if (FirstTickSeconds == 0.0)
		{
			FirstTickSeconds = Now;
		}
		else if (Now - FirstTickSeconds >= double(ConvergeSettleSeconds))
		{
			RunConvergenceHarness();
		}
	}

	if (VoxelGI::GetDebugLevel() > 0 && Now - LastStatSeconds > 1.0)
	{
		LastStatSeconds = Now;
		UE_LOG(LogVoxelGI, Log,
		       TEXT("GI: bricks=%d (%.1f MB) pendingVox=%d(+%d pooled) dirty=%d refresh=%d | solved %d bricks/%d cells in %.2fms, ")
		       TEXT("reshaded %d chunks (%d dupes skipped), volumeUp %d bricks (queue %d, total %d bricks/%d runs), tick %.2fms"),
		       Field->NumBricks(), double(Field->EstimatedBytes()) / (1024.0 * 1024.0),
		       PendingVoxelize.Num(), PendingPooledVoxelize.Num(), DirtyQueue.Num(), RefreshQueue.Num(),
		       ToSolve.Num(), CellsSolved, SolveMs, Refreshed, DedupeSkips,
		       VolumeUploaded, VolumeUploadQueue.Num(), VolumeBricksUploaded, VolumeRunsUploaded,
		       (FPlatformTime::Seconds() - TickStart) * 1000.0);
	}
}

// --- convergence harness (-VoxelGIConverge=<N>) ------------------------------
//
// THE DELIVERABLE FOR THE ENERGY BUG. "Enclosed spaces creep brighter over
// successive re-solves" was previously only observable as a screenshot luma
// difference between runs (104.7 on one capture, 129.0 on a later one), which
// is both slow to measure and easy to mistake for a code regression -- two
// runs were burned bisecting exactly that. This measures the underlying
// quantity directly and synchronously.
//
// Method: let the field reach its normal streamed steady state, then re-solve
// EVERY resident brick N times back to back with nothing else changing --
// same scene, same geometry, same camera, no streaming in between. The only
// thing varying across passes is the bounce feedback, so this isolates it.
//
// What convergence looks like: the solve is a contraction with modulus
// BounceAlbedo (see VoxelLightField.h), so max|AvgIrr - PrevAvgIrr| must fall
// by roughly 2.5x per pass and the mean must go flat. Divergence -- the bug --
// shows up as a mean that keeps climbing and a delta that does not shrink.
void UVoxelGISubsystem::RunConvergenceHarness()
{
	bConvergeDone = true;

	TArray<FIntVector> AllKeys;
	Field->GetResidentKeys(AllKeys);
	if (AllKeys.Num() == 0)
	{
		UE_LOG(LogVoxelGI, Warning,
		       TEXT("GICONVERGE: no resident bricks after %.0fs settle -- nothing to measure. ")
		       TEXT("Raise -VoxelGIConvergeSettle."),
		       ConvergeSettleSeconds);
		FPlatformMisc::RequestExit(false);
		return;
	}

	FVoxelGISolveParams Params;
	Params.MaxConeDistanceUU = CVarGIConeDistanceUU.GetValueOnGameThread();
	Params.BounceAlbedo = CVarGIBounceAlbedo.GetValueOnGameThread();
	Params.bLegacyConeBasis = CVarGILegacyConeBasis.GetValueOnGameThread() != 0;

	Field->SetLegacyInPlaceGather(bConvergeLegacy);

	if (bConvergeSeed)
	{
		const uint8 SeedByte = uint8(FMath::Clamp(ConvergeSeedValue, 0, 255));
		Field->SeedIrradiance(SeedByte);
		UE_LOG(LogVoxelGI, Log, TEXT("GICONVERGE: seeded every solved cell to %d/255 (%s start)."),
		       int32(SeedByte), SeedByte >= 128 ? TEXT("HOT") : TEXT("COLD"));
	}

	UE_LOG(LogVoxelGI, Log,
	       TEXT("GICONVERGE: begin -- %d resident bricks, %d passes, albedo %.2f, seed=%s, mode=%s"),
	       AllKeys.Num(), ConvergePasses, Params.BounceAlbedo,
	       bConvergeSeed ? *FString::Printf(TEXT("%d"), ConvergeSeedValue) : TEXT("none"),
	       bConvergeLegacy ? TEXT("LEGACY in-place gather (pre-fix)") : TEXT("Jacobi published gather (fixed)"));

	double PrevMean = 0.0;
	double PrevDelta = 0.0;
	double FirstMean = 0.0;
	double LastMean = 0.0;
	double LastDelta = 0.0;
	double MaxMeanRise = 0.0;
	double LastMeanRise = 0.0;

	for (int32 Pass = 1; Pass <= ConvergePasses; ++Pass)
	{
		FVoxelGIPassStats Stats;
		const double PassStart = FPlatformTime::Seconds();
		Field->SolveBricks(AllKeys, Params, &Stats);
		const double PassMs = (FPlatformTime::Seconds() - PassStart) * 1000.0;

		const double MeanByte = Stats.MeanIrr * 255.0;
		const double DeltaByte = Stats.MaxAbsDelta * 255.0;
		const double MeanRise = Pass == 1 ? 0.0 : MeanByte - PrevMean;
		// Ratio of successive deltas: the contraction modulus, measured. The
		// theory says this should sit at or below BounceAlbedo.
		const double Ratio = (Pass > 1 && PrevDelta > 0.0) ? DeltaByte / PrevDelta : 0.0;

		UE_LOG(LogVoxelGI, Log,
		       TEXT("GICONVERGE pass %2d/%d: cells=%d meanIrr=%7.3f (d %+7.3f) maxDelta=%7.3f ratio=%5.3f  [%.0f ms]"),
		       Pass, ConvergePasses, Stats.CellsSolved, MeanByte, MeanRise, DeltaByte, Ratio, PassMs);

		if (Pass == 1)
		{
			FirstMean = MeanByte;
		}
		else
		{
			MaxMeanRise = FMath::Max(MaxMeanRise, MeanRise);
		}
		LastMeanRise = MeanRise;
		PrevMean = MeanByte;
		PrevDelta = DeltaByte;
		LastMean = MeanByte;
		LastDelta = DeltaByte;
	}

	// Verdict. Thresholds are in 0..255 irradiance bytes, the same units the
	// field is stored in, so "1.0" means the whole field has reached its
	// byte-quantized fixed point.
	//
	// Both conditions matter and they fail differently: a field that is still
	// climbing fails the drift test, while one that oscillates (the signature
	// of a feedback loop with gain >= 1) fails the delta test even though its
	// mean might look stable.
	constexpr double kDeltaSettled = 1.5; // bytes
	constexpr double kMeanDrift = 0.5;    // bytes per pass, late passes
	const bool bDeltaOk = LastDelta <= kDeltaSettled;
	const bool bDriftOk = FMath::Abs(LastMeanRise) <= kMeanDrift;
	const bool bPass = bDeltaOk && bDriftOk;

	UE_LOG(LogVoxelGI, Log,
	       TEXT("GICONVERGE RESULT: %s -- mode=%s bricks=%d passes=%d | meanIrr %.3f -> %.3f (total drift %+.3f, ")
	       TEXT("largest single-pass rise %+.3f) | final maxDelta %.3f (threshold %.1f)"),
	       bPass ? TEXT("CONVERGED") : TEXT("NOT CONVERGED"),
	       bConvergeLegacy ? TEXT("LEGACY") : TEXT("FIXED"),
	       AllKeys.Num(), ConvergePasses, FirstMean, LastMean, LastMean - FirstMean, MaxMeanRise,
	       LastDelta, kDeltaSettled);

	Field->SetLegacyInPlaceGather(false);
	FPlatformMisc::RequestExit(false);
}

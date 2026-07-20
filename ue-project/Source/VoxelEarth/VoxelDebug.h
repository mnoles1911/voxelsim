#pragma once
// Shared debug-tooling plumbing (docs/debug-tooling-plan.md P1): log
// categories, the `voxel.Debug*` cvars, the `stat VoxelEarth` group, and the
// plain FVoxelPerfSnapshot POD the perf HUD reads. Deliberately voxel-core-free
// (like every other UHT-parsed header in this module) even though it is
// pulled into VoxelWorldSubsystem.h -- FVoxelPerfSnapshot mirrors vxc::Counters
// totals as plain uint64 fields rather than holding a vxc::Counters directly.
//
// This header carries no UCLASS/USTRUCT (nothing here needs UHT reflection),
// so it is safe for UHT-parsed headers to include -- see doctrine comment atop
// VoxelWorldSubsystem.h.

#include "CoreMinimal.h"
#include "Stats/Stats.h"

// --- Log categories (P1 "Log split") ----------------------------------------
//
// LogVoxelEarth (VoxelEarth.h) stays for module-startup/general lines.
// Streaming lifecycle, edit-log rejections, and periodic perf/counter lines
// get their own categories so verbosity can be toggled independently at
// runtime (editor MCP LogsToolset, `log LogVoxelStream Verbose`, etc) without
// drowning the general log.
VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelStream, Log, All);
VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelEdit, Log, All);
VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelPerf, Log, All);

// --- stat VoxelEarth group (P1 "Stats group") -------------------------------

DECLARE_STATS_GROUP(TEXT("VoxelEarth"), STATGROUP_VoxelEarth, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Subsystem Tick"), STAT_VoxelSubsystemTick, STATGROUP_VoxelEarth, VOXELEARTH_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Worker Job"), STAT_VoxelWorkerJob, STATGROUP_VoxelEarth, VOXELEARTH_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Game-Thread Chunk Mesh"), STAT_VoxelGameThreadMesh, STATGROUP_VoxelEarth, VOXELEARTH_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Edit Apply"), STAT_VoxelEditApply, STATGROUP_VoxelEarth, VOXELEARTH_API);

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Chunks Loaded"), STAT_VoxelChunksLoaded, STATGROUP_VoxelEarth, VOXELEARTH_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Chunks In Flight"), STAT_VoxelChunksInFlight, STATGROUP_VoxelEarth, VOXELEARTH_API);

// --- voxel.Debug cvars + F3 (P1 "CVars + F3") -------------------------------

namespace VoxelDebug
{
	// voxel.Debug: 0=off, 1=perf HUD, 2=HUD+visualizations
	// (docs/debug-tooling-plan.md "Access model"). AVoxelEarthPlayerController's
	// F3 binding cycles 0->1->2->0 via CycleDebugMode.
	VOXELEARTH_API int32 GetDebugMode();
	VOXELEARTH_API void SetDebugMode(int32 NewMode);
	VOXELEARTH_API void CycleDebugMode();

	// Layer toggles (voxel.Debug.ChunkStates / voxel.Debug.Bounds): only
	// visually active when GetDebugMode() >= 2, matching the "all live under
	// mode 2" access-model row -- both helpers already fold that mode check
	// in, so call sites never need to check GetDebugMode() themselves.
	VOXELEARTH_API bool IsChunkStatesEnabled();
	VOXELEARTH_API bool IsBoundsEnabled();
}

// --- Perf HUD data (P1 "Perf HUD") ------------------------------------------
//
// Plain POD snapshot published once per second by UVoxelWorldSubsystem
// (VoxelWorldSubsystem.cpp, FVoxelWorldImpl::UpdatePerfSnapshot) and read every
// frame by AVoxelEarthHUD for its mode>=1 canvas rows. No voxel-core types
// leak in -- the BricksGenerated/CellsWritten/QuadsEmitted/EditsApplied/
// ColumnEvals fields are plain uint64 mirrors of vxc::Counters, which stays a
// private implementation detail of FVoxelWorldImpl (voxel-core stays engine-
// and UE-header-free by doctrine, but conversely the UE-visible header must
// itself stay voxel-core-free -- see VoxelWorldSubsystem.h).
struct FVoxelPerfSnapshot
{
	// --- Streaming --------------------------------------------------------
	int64 TotalChunksLoaded = 0;
	int64 TotalChunksUnloaded = 0;
	float ChunksLoadedPerSec = 0.f;
	float ChunksUnloadedPerSec = 0.f;
	int32 JobsInFlight = 0;
	int32 JobsInFlightCap = 0;
	int32 PendingJobQueueDepth = 0;
	int32 PendingGameThreadQueueDepth = 0;
	int32 PendingUnloadQueueDepth = 0;
	// Blended average of (applies/cap, re-meshes/cap, unloads/cap) over the
	// last refresh window, as a percentage -- "budget saturation" row.
	float BudgetSaturationPct = 0.f;
	int64 StaleResultsDiscarded = 0;

	// --- Worker timings -----------------------------------------------------
	// Rolling 256-sample window of per-chunk worker mesh-job milliseconds
	// (measured inside the worker, see VoxelStreaming::FJobResult::JobMs).
	float WorkerMsP50 = 0.f;
	float WorkerMsP95 = 0.f;
	float WorkerMsMax = 0.f;

	// --- Memory -------------------------------------------------------------
	int32 ResidentComponents = 0;
	int64 ResidentQuads = 0;
	int64 OverlayBrickCount = 0;
	int64 EditLogEntries = 0;

	// --- Frame ---------------------------------------------------------------
	float SubsystemTickMs = 0.f;

	// --- Raw counters (vxc::Counters totals, plain mirror) ------------------
	uint64 BricksGenerated = 0;
	uint64 CellsWritten = 0;
	uint64 QuadsEmitted = 0;
	uint64 EditsApplied = 0;
	uint64 ColumnEvals = 0;
};

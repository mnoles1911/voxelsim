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
#include "VoxelCoords.h" // VoxelCoords::kNumLevels (ring tint table + perf snapshot sizing) -- UE-only, voxel-core-free, safe here

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
// W2 (docs/debug-tooling-plan.md P3 "log split" extension): the pressure CA
// tick, breach/reservoir seeding, and replication plumbing get their own
// category so water verbosity can be toggled independently of terrain's.
VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelWater, Log, All);

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

	// voxel.Debug.Rings (docs/m2-plan.md first implementation wave item 4):
	// tints every loaded chunk component by its mip level (RingLevelTint)
	// instead of the chunk-state flash/overlay tints. Same mode>=2 gating as
	// the other layers; takes priority over ChunkStates if both are enabled
	// (see FVoxelWorldImpl::TickStreaming).
	VOXELEARTH_API bool IsRingsEnabled();

	// Programmatic setter (ECVF_SetByCode, mirrors SetDebugMode) -- used by
	// the -VoxelDebugRings command-line switch (AVoxelEarthGameMode::BeginPlay)
	// to force voxel.Debug=2 + voxel.Debug.Rings=1 for headless verification
	// runs without needing -ExecCmds plumbing.
	VOXELEARTH_API void SetRingsEnabled(bool bEnabled);

	// Ring level debug tint colors (m2-plan.md item 4): R0 green .. R4
	// magenta, indexed by VoxelCoords::kNumLevels. Clamped to a valid index.
	VOXELEARTH_API FLinearColor RingLevelTint(int32 Level);

	// Band 3 debug tint (m2-plan.md "Debug" row; docs/debug-tooling-plan.md
	// palette: "R4 magenta, heightmap band cyan") -- applied to every
	// AVoxelClipmapActor level's DebugTint parameter while voxel.Debug.Rings
	// is live under mode 2, same MID-lazy-create/clear doctrine every other
	// debug-tint call site in this module follows (see
	// UVoxelChunkComponent::SetDebugTint/ClearDebugTint).
	VOXELEARTH_API FLinearColor HeightmapBandTint();

	// voxel.MipCacheBudgetMB (M2 task "Mip cache eviction"): approximate byte
	// budget, in MB, for the shared cross-job mip cache (FSharedMipCache,
	// VoxelWorldSubsystem.cpp) -- default 512. Read via GetValueOnAnyThread:
	// SharedMipCache::Insert runs on worker job threads, same cross-thread
	// cvar-read pattern every other voxel.Debug* accessor in this namespace
	// uses. <= 0 disables eviction (unbounded, pre-eviction-wave behavior).
	VOXELEARTH_API int64 GetMipCacheBudgetBytes();

	// Programmatic setter (ECVF_SetByCode, mirrors SetRingsEnabled) -- used by
	// the -VoxelMipCacheBudgetMB=<N> command-line switch
	// (AVoxelEarthGameMode::BeginPlay) to force a small budget for headless
	// eviction-verification runs without needing -ExecCmds plumbing.
	VOXELEARTH_API void SetMipCacheBudgetMB(int32 NewBudgetMB);

	// --- voxel.Server.* (M3 wave 2 "Validation hardening", docs/m3-plan.md) ---
	//
	// Server-side edit-intent caps read by AVoxelEarthPlayerController's
	// ServerSubmit*Intent handlers (authority only -- these are meaningless on
	// a client, which never receives its own intent RPCs). Excess/oversized
	// intents are REJECTED (logged, no-op) rather than silently clamped or
	// disconnected -- see the handlers' doc comments.

	// voxel.Server.MaxIntentsPerSec: per-connection token-bucket cap (default
	// 10) on ServerSubmitDigIntent/ServerSubmitPlaceIntent/
	// ServerSubmitCarveIntent RPCs accepted per second.
	VOXELEARTH_API int32 GetServerMaxIntentsPerSec();

	// voxel.Server.MaxCarveRadiusUU: cap (UU, default 400) on
	// ServerSubmitCarveIntent's RadiusUU -- dig/place's equivalent cap is the
	// existing compile-time UVoxelWorldSubsystem::MaxCubeSizeVoxels constant
	// (shared with client-side prediction clamping, so it stays a constant
	// rather than a separately-tunable cvar that could drift from what the
	// client itself enforces).
	VOXELEARTH_API float GetServerMaxCarveRadiusUU();

	// --- voxel.Water.* (W2, docs/voxel-earth-implementation-plan.md SS3.7) ---

	// voxel.Water.MaxActiveBricks: advisory budget (default 4096) on the
	// number of active vxc::WaterCA bricks a single fixed-step tick may
	// process (WaterCA::steppedBrickCount() after step()). The CA's tick
	// contract (voxelcore/waterca.h) processes its whole active-set snapshot
	// atomically -- there is no mid-step cutoff that wouldn't break volume
	// conservation/determinism -- so this is a monitoring threshold, not a
	// hard clamp: UVoxelWaterSubsystem logs a throttled warning when exceeded
	// (task spec: "do not explode") rather than truncating the tick.
	VOXELEARTH_API int32 GetWaterMaxActiveBricks();
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
	// M2 wave 2 item 1 ("Cross-job mip caching"): the shared cross-job cache
	// of pure-generated level>=1 mip bricks (FSharedMipCache,
	// VoxelWorldSubsystem.cpp) -- brick count and an approximate byte total
	// (no eviction yet, so this only grows).
	int64 MipCacheBrickCount = 0;
	int64 MipCacheBytes = 0;
	// M2 task "Mip cache eviction": running count of approximate-LRU evictions
	// (FSharedMipCache::Insert, over voxel.MipCacheBudgetMB) since startup.
	int64 MipCacheEvictions = 0;

	// --- Ring levels (docs/m2-plan.md first implementation wave item 1) -----
	// Loaded (has a live component) and pending (queued across job/game-thread/
	// unload) chunk counts per mip level, indexed by VoxelCoords level.
	int32 LevelLoadedCount[VoxelCoords::kNumLevels] = {};
	int32 LevelPendingCount[VoxelCoords::kNumLevels] = {};

	// M2 wave 2 item 1: per-level worker mesh-job ms (same rolling-window
	// p50/p95 as WorkerMsP50/P95 above, split by ring level) -- the number
	// this wave's fix targets directly (wave 1 measured worker p95 ~296ms on
	// high-level jobs because every job rebuilt its whole level-0->L mip
	// chain from scratch; see FSharedMipCache).
	float LevelWorkerMsP50[VoxelCoords::kNumLevels] = {};
	float LevelWorkerMsP95[VoxelCoords::kNumLevels] = {};

	// --- Frame ---------------------------------------------------------------
	float SubsystemTickMs = 0.f;

	// --- Raw counters (vxc::Counters totals, plain mirror) ------------------
	uint64 BricksGenerated = 0;
	uint64 CellsWritten = 0;
	uint64 QuadsEmitted = 0;
	uint64 EditsApplied = 0;
	uint64 ColumnEvals = 0;
};

// W2 (docs/debug-tooling-plan.md P3 "Water (future)" row, now landed):
// published once per second by UVoxelWaterSubsystem (mirrors
// FVoxelPerfSnapshot's cadence/shape convention) and read by AVoxelEarthHUD's
// mode>=1 rows. Plain POD, no vxc:: types (same doctrine as FVoxelPerfSnapshot
// above) -- UVoxelWaterSubsystem.h is voxel-core-free by PImpl, exactly like
// UVoxelWorldSubsystem.h.
struct FVoxelWaterPerfSnapshot
{
	// vxc::WaterCA::activeBrickCount() / storedBrickCount() as of the last
	// fixed-step tick this second.
	int64 ActiveBricks = 0;
	int64 StoredBricks = 0;
	// vxc::WaterCA::totalVolume() (fill units; 255 = one full voxel).
	uint64 TotalVolume = 0;
	// Fixed-step CA ticks actually executed in the last 1s window (target
	// 10Hz; less if a frame hitch ate into the accumulator's iteration cap).
	float StepsPerSec = 0.f;
	// Most recent step()'s steppedBrickCount() -- the number the
	// voxel.Water.MaxActiveBricks budget check above compares against.
	int64 LastSteppedBrickCount = 0;
	// Replication plumbing (v1, authority only): bytes/sec actually pushed
	// through AVoxelEditRelay::MulticastWaterDiffs over the last window.
	float ReplicatedBytesPerSec = 0.f;
	// Reservoir v0 (docs/voxel-earth-implementation-plan.md SS3.7): number of
	// registered breach-boundary cells continuously topped up to 255/tick.
	int32 ReservoirCells = 0;

	// Wall-clock ms UVoxelWaterSubsystem::Tick() spent this frame (fixed-step
	// CA stepping + re-mesh + replication broadcast, whichever ran) -- the
	// number the task spec's perf budget ("<2ms/frame at v0 scale") is
	// measured against. Always fresh (not gated behind the 1Hz refresh),
	// same convention as FVoxelPerfSnapshot::SubsystemTickMs.
	float TickMs = 0.f;
};

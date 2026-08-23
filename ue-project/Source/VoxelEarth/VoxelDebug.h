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
	// voxel.Debug: 0=off, 1=perf HUD, 2=HUD+visualizations, 3=GPU streaming
	// panel (docs/debug-tooling-plan.md "Access model"; mode 3 added for the
	// GPU streaming programme). AVoxelEarthPlayerController's F3 binding
	// cycles 0->1->2->3->0 via CycleDebugMode.
	//
	// MODE 3 IS DELIBERATELY NOT "MODE 2 PLUS MORE". It is the panel the owner
	// reads WHILE FLYING at 30 m/s, judging whether the streaming pipeline
	// keeps up -- so it must not drag in the mode-2 3D layers (chunk-state
	// tints, bounds wireframes), whose per-chunk component walk is exactly the
	// kind of game-thread cost that would perturb the number being read. The
	// layer helpers below therefore gate on == 2, not >= 2.
	VOXELEARTH_API int32 GetDebugMode();
	VOXELEARTH_API void SetDebugMode(int32 NewMode);
	VOXELEARTH_API void CycleDebugMode();

	// voxel.GpuStream.Prototype: the one-switch arm for the GPU streaming
	// shakedown (see the cvar's declaration in VoxelDebug.cpp for the list of
	// cvar names it arms by name). Non-zero = armed. Read by the HUD so the
	// panel can say it was armed this way.
	VOXELEARTH_API int32 GetGpuStreamPrototype();

	// Layer toggles (voxel.Debug.ChunkStates / voxel.Debug.Bounds): only
	// visually active when GetDebugMode() == 2, matching the "all live under
	// mode 2" access-model row -- both helpers already fold that mode check
	// in, so call sites never need to check GetDebugMode() themselves.
	// (== 2, not >= 2: mode 3 is the flight-readable streaming panel and must
	// not silently arm the 3D layers -- see the mode comment above.)
	VOXELEARTH_API bool IsChunkStatesEnabled();
	VOXELEARTH_API bool IsBoundsEnabled();

	// voxel.Debug.Rings (docs/m2-plan.md first implementation wave item 4):
	// tints every loaded chunk component by its mip level (RingLevelTint)
	// instead of the chunk-state flash/overlay tints. Same mode>=2 gating as
	// the other layers; takes priority over ChunkStates if both are enabled
	// (see FVoxelWorldImpl::TickStreaming).
	VOXELEARTH_API bool IsRingsEnabled();

	// voxel.Debug.PlayerBox: the player's collision-volume wireframe,
	// eye-height marker and ground-probe voxel cells
	// (UVoxelCharacterMovementComponent::DebugDrawVolume), drawn in walk mode.
	//
	// THE ODD ONE OUT, deliberately: it does NOT fold in the mode>=2 gate the
	// three layers above do, and it defaults ON. It is not a diagnostic layer
	// but a scale reference for ordinary play-testing -- with 10 cm voxels and a
	// kilometres-wide world there is otherwise nothing in frame to judge size
	// against -- so requiring two F3 presses to see it would defeat it. The
	// cvar exists so a capture can turn it OFF; suppression for unattended
	// fixture runs is automatic (IsUnattendedFixtureRun, checked by the drawing
	// code) so this never lands in a verification screenshot.
	//
	// Because there is no gate to hide, this getter IS the raw cvar value, so
	// unlike Bounds/ChunkStates/Rings there is no separate Get*CVar companion
	// for the overlay to call.
	VOXELEARTH_API bool IsPlayerBoxEnabled();

	// voxel.Water.BucketFill -- how much the `1` key pours (fill units; 255 ==
	// one full voxel). A console knob rather than a constant because the whole
	// value of the bucket is trying different volumes on different terrain in
	// one session: 30,000 is a puddle, 200,000 a bathtub, 1,000,000 a pond, and
	// which one makes the flow legible depends entirely on the slope you are
	// standing on.
	VOXELEARTH_API int32 GetWaterBucketFill();
	VOXELEARTH_API void SetPlayerBoxEnabled(bool bEnabled);

	// Programmatic setter (ECVF_SetByCode, mirrors SetDebugMode) -- used by
	// the -VoxelDebugRings command-line switch (AVoxelEarthGameMode::BeginPlay)
	// to force voxel.Debug=2 + voxel.Debug.Rings=1 for headless verification
	// runs without needing -ExecCmds plumbing.
	VOXELEARTH_API void SetRingsEnabled(bool bEnabled);

	// --- In-game debug overlay support (AVoxelEarthHUD) ---------------------
	//
	// The overlay flips the SAME cvars the console does (nothing parallel):
	// these are the ECVF_SetByCode setters matching IsChunkStatesEnabled /
	// IsBoundsEnabled above, which SetRingsEnabled already had. Note the
	// getters fold in the mode>=2 gate but these setters do NOT touch
	// voxel.Debug -- the overlay shows and edits the mode as its own row.
	VOXELEARTH_API void SetChunkStatesEnabled(bool bEnabled);
	VOXELEARTH_API void SetBoundsEnabled(bool bEnabled);

	// Raw cvar values, ignoring the mode>=2 gate the Is*Enabled getters apply.
	// The overlay must show what the cvar is SET to (and let you flip it)
	// independently of whether mode 2 is currently making it visible --
	// otherwise every layer reads "off" at mode 1 and toggling looks broken.
	VOXELEARTH_API bool GetChunkStatesCVar();
	VOXELEARTH_API bool GetBoundsCVar();
	VOXELEARTH_API bool GetRingsCVar();

	// --- Tile-source status (overlay "real tiles vs synthetic sampler" row) --
	//
	// WHY A LOG SNIFFER. Which sampler the world actually booted on is decided
	// in MakeTileSampler (VoxelWorldSubsystem.cpp) and survives only as the
	// PRIVATE FVoxelWorldImpl::bUsingTileGrid -- UVoxelWorldSubsystem exposes
	// no accessor, and this task must not edit that file. The one durable,
	// already-shipped signal is the log line MakeTileSampler emits:
	//
	//   "Voxel tile grid: dir=%s loaded=%d rejected=%d seed=%llu scale=%d"
	//
	// so a tiny FOutputDevice registered at core init (VoxelDebug.cpp) parses
	// exactly that line (plus the bounding-box line) and caches the numbers.
	// This is deliberately a READ of an existing log line, not a second source
	// of truth: if the format ever changes, bKnown simply goes false and the
	// overlay says "unknown" rather than lying. Several hours were lost on this
	// project to not knowing which sampler was live, which is why it is worth
	// the ugliness.
	struct FTileSourceStatus
	{
		// False until MakeTileSampler has logged (i.e. before the world
		// subsystem initialises), or if the line was never seen at all.
		bool bKnown = false;
		// True only when the tile grid actually loaded >= 1 tile. A zero-loaded
		// -VoxelTileDir falls back to the synthetic sampler in MakeTileSampler,
		// and this mirrors that exact rule.
		bool bUsingRealTiles = false;
		int32 TilesLoaded = 0;
		int32 TilesRejected = 0;
		FString TileDir;
		// From the "loaded tile coords bounding box x=[a,b] y=[c,d]" line;
		// bBoxKnown is false when that line was not seen (e.g. zero tiles).
		bool bBoxKnown = false;
		int32 MinTileX = 0, MaxTileX = 0, MinTileY = 0, MaxTileY = 0;
	};
	VOXELEARTH_API FTileSourceStatus GetTileSourceStatus();

	// Tile index / tile-pixel coords for a world position, so the overlay can
	// answer "which diffusion tile am I standing in" -- the tiles on disk are
	// named "<x>_<y>.vxtl" and this is the same floorDiv mapping
	// vxc::TileGridSampler::findTile uses (tile = floorDiv(pixel, 512)), with
	// the pixel size from -VoxelTileScale (30 m/px at scale 1, 11.25 m/px at
	// scale 8 -- vxc::tilePixelSizeMm). Mirrored here as plain arithmetic
	// rather than including voxel-core: this header is UHT-adjacent and
	// voxel-core-free by doctrine (see the file header), and these are two
	// stable format constants, not worldgen behaviour.
	VOXELEARTH_API void WorldToTileCoords(double WorldXUU, double WorldYUU, int32& OutTileX, int32& OutTileY, int64& OutPixelX,
	                                       int64& OutPixelY);

	// -VoxelUndergroundVeil=0 (AVoxelClipmapActor::BeginPlay) disables the
	// underground veil for the whole run. That actor keeps its live
	// active/inactive state private, so the overlay reports this run-level
	// switch -- read the same way, from the command line, at call time.
	VOXELEARTH_API bool IsUndergroundVeilEnabledForRun();

	// True for the headless verification fixtures, every one of which launches
	// with -unattended (see ue-project/Tools/capture_terrain_shots.ps1 and the
	// -VoxelScreenshotAfter/-VoxelVistaShot/-VoxelPerfRun recipes in
	// docs/status.md). AVoxelEarthHUD suppresses its ALWAYS-ON additions (the
	// walk/fly mode line) under this so nothing new can land in a verification
	// screenshot; the overlay itself is default-off and key-driven, so it can
	// never appear in one regardless.
	VOXELEARTH_API bool IsUnattendedFixtureRun();

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

	// voxel.Water.GPU: route water surface geometry through its own ADR-0006
	// GPU pool (ONE primitive, ONE draw) rather than one UWaterChunkComponent
	// per vxc::WaterBrick8. Default false. Independent of voxel.Stream.GPU --
	// see the cvar's source comment for why the two are separate switches.
	VOXELEARTH_API bool GetWaterGpu();

	// voxel.Water.SWE (W4, docs/adr/0004-swe-fixed-point-coupling.md): arm the
	// shipped-but-inert shallow-water layer -- one vxc::SweGrid plus a
	// vxc::SweCaCoupler, stepped inside the existing fixed 10Hz water step.
	// Default 0.
	//
	// This getter is the RAW cvar, NOT permission to construct anything.
	// UVoxelWaterSubsystem refuses to arm on any net mode except NM_Standalone,
	// because ADR-0004's item 3 defers enablement to "M3 networked water" on the
	// grounds that the coupler is a second simulation with its own membership,
	// dwell and depth state that is "not yet wired into the replication path at
	// all". That reasoning is CLIENT-SCOPED -- in NM_Standalone there is no
	// mirror and no wire, so nothing can desync -- and gating on net mode
	// PRESERVES the ADR's deferral rather than overriding it. See
	// UVoxelWaterSubsystem.cpp's MaybeArmSwe for the refusal path.
	VOXELEARTH_API bool GetWaterSwe();

	// voxel.Water.Rivers -- W3 (plan S3.7 Layer R): arm the coarse river-network
	// graph + its coupling to the water CA. Default OFF. Gated on NM_Client
	// only (not NM_Standalone like GetWaterSwe): the river coupler's only
	// client-visible output is WaterCA fill, which already replicates, so a
	// server may run it without a desync surface. See MaybeArmRivers in
	// VoxelWaterSubsystem.cpp.
	VOXELEARTH_API bool GetWaterRivers();

	// --- voxel.Stream.* (M1/M2 "Perf-run hitches" isolation work, docs/status.md) ---
	//
	// Per-frame streaming budgets, previously compile-time constexpr in
	// VoxelWorldSubsystem.cpp's DrainResults/DrainGameThreadMesh/DrainUnloads.
	// Now live cvars so the streaming ramp's chunk-component apply/proxy-create
	// rate can be tuned (a SMOOTHING scheme -- fewer applies/frame spreads
	// proxy-creation cost more evenly) without a recompile. Defaults match the
	// pre-cvar constants (docs/m1-plan.md Stage 2 decisions table), so leaving
	// these untouched is a byte-identical no-op.

	// voxel.Stream.MaxAppliesPerFrame: HARD CEILING on worker-mesh-result
	// chunk-component applies (DrainResults) per frame. Default 64. As of the
	// 2026-07-24 streaming-speed pass this is a safety ceiling; the steady-state
	// throttle is the wall-clock GetStreamApplyBudgetMs below.
	VOXELEARTH_API int32 GetStreamMaxAppliesPerFrame();

	// voxel.Stream.ApplyBudgetMs: max wall-clock ms DrainResults may spend
	// applying finished chunks per frame (2026-07-24 pass). Default 6. The loop
	// applies a small floor for progress, then drains while under this budget up
	// to GetStreamMaxAppliesPerFrame.
	VOXELEARTH_API float GetStreamApplyBudgetMs();

	// voxel.Stream.LodRetentionMs: load-before-unload grace (2026-07-24 pass).
	// A visible chunk evicted by an LOD-ring transition is kept drawn as a
	// stand-in for this many ms before parking, so its replacement can stream in
	// without a hole. Default 1000; 0 disables.
	VOXELEARTH_API float GetStreamLodRetentionMs();

	// voxel.Stream.LogRetention (ring-gap wave): per-chunk drill-down on the
	// retention mechanism. The 5s `Voxel LOD retention` census counts releases;
	// this names them -- level, chunk key, radius from the anchor, and for a
	// covered-ABSENT release how many of the replacement columns consulted were
	// absent rather than settled. Default 0. Diagnostic only: it is per-chunk
	// output and has no place in a perf run.
	VOXELEARTH_API bool GetStreamLogRetention();

	// voxel.Stream.ApplyStageStats (Wave S0, docs/speculative-generation-plan.md
	// §4): split the per-apply cost into quad pack / SampleChunkParamsForPool /
	// pool add, reported per 5s window. Default 0.
	//
	// It gates the CLOCKS ONLY. The DrainResults exit-reason counters beside them
	// are unconditional, because they are what decide whether the apply loop
	// exits on an empty queue or on its 6 ms wall clock -- the question the
	// published "apply budget only 8.5% saturated, results are not ARRIVING"
	// reading answers by assumption. Pair with voxel.Stream.PoolPushStats
	// (VoxelEarthShaders), which splits the pool-add bucket across both threads.
	VOXELEARTH_API int32 GetStreamApplyStageStats();

	// voxel.Stream.CoverageVerify (ring-gap wave): once per periodic perf-log
	// window, count the XY footprints inside a ring's core annulus that NO level
	// is visibly covering -- i.e. the see-through holes, as a number rather than
	// a screenshot. Default 0; read-only (changes no streaming decision) and
	// entirely inside the enable check, so a default run pays one cvar read.
	// See the coverage block in FVoxelWorldImpl::MaybeLogCounters.
	VOXELEARTH_API bool GetStreamCoverageVerify();

	// voxel.Stream.LatencyStats (S0-3, docs/speculative-generation-plan.md Wave
	// S0 / docs/streaming-perf-implementation-plan.md T0-2): per-producer,
	// per-stage submit->apply latency windows (QueuedMs, DispatchToReadyMs,
	// ReadyToDeliverMs, SubmitToDeliverMs, DeliverToApplyMs for the GPU fork;
	// the worker-arm equivalent end-to-end figure and its own DeliverToApplyMs
	// for the CPU path) plus the per-level quad-count distribution. Default 0.
	// Gates the window bookkeeping and the new log lines only -- see the cvar's
	// source comment for why. Exists to tell a genuine per-job GPU cost apart
	// from Little's-law queue depth (deep-review-streaming-perf-2026-07-27.md
	// S1d) before believing either explanation.
	VOXELEARTH_API bool GetStreamLatencyStats();

	// voxel.Render.CastShadow: do terrain chunks render into the directional
	// light's shadow-depth pass (PR #95)? Default true. Exists to A/B the
	// render-thread cost of terrain shadows on ONE primitive flag -- see the
	// cvar's source comment for why r.ShadowQuality is not a valid substitute
	// (it desyncs the BeginPlay PSO precache and measures that instead).
	VOXELEARTH_API bool GetRenderCastShadow();

	// voxel.Stream.GPU: route chunk geometry through the ADR-0006 GPU pool
	// (ONE primitive, ONE draw) rather than one scene component per chunk.
	// Default false until G5. See the cvar's source comment -- only the
	// geometry handoff moves; every streaming decision upstream is shared.
	VOXELEARTH_API bool GetStreamGpu();

	// voxel.Stream.JobsInFlightPerCore: worker slots in flight as a multiple of
	// logical cores. Default 2 (the historical hardcoded 2xLogicalCores). See
	// the cvar's source comment for the dispatch-starvation measurement that
	// made this tunable.
	VOXELEARTH_API int32 GetStreamJobsInFlightPerCore();

	// voxel.Stream.DispatchAfterDrain: run a second DispatchJobs() pass after
	// DrainGameThreadMesh (i.e. once slots freed by this frame's DrainResults
	// are visible), instead of refilling only once per frame. Default 1. See
	// the cvar's source comment for the ~9% worker-utilisation measurement
	// this complements (voxel.Stream.JobsInFlightPerCore=8 widens the buffer;
	// this shortens the once-per-frame refill cadence that starves it).
	VOXELEARTH_API int32 GetStreamDispatchAfterDrain();

	// voxel.Stream.MaxRemeshesPerFrame: max game-thread overlay-aware edit
	// re-meshes (DrainGameThreadMesh -- first load of an edited chunk, or a
	// post-edit dirty re-mesh) applied per frame. Default 8 (2026-07-24 pass).
	// voxel.Stream.AdmissionRecordCap (S2-0): stop relaxing the per-level
	// admission cutoff once ChunkRecords reaches this many entries. 0 = off.
	// The existing relaxation gates on the pending JOB queue being short, which
	// was a proxy for spare downstream capacity that only held while the queue
	// was short from STARVATION -- Wave S1 made it permanently short by making
	// the consumer fast. See the cvar's source comment.
	VOXELEARTH_API int32 GetStreamAdmissionRecordCap();

	// voxel.Stream.PoolParkMax (S2-3): max chunks kept as hidden-but-allocated
	// pool ranges after eviction, so re-admission is a chunk-table write rather
	// than a re-mesh. 0 = off. Parked chunks still consume pool quads and
	// chunk-table entries, so this cap bounds real capacity -- see the cvar's
	// source comment.
	// T4-1 speculative generation. VelocityLeadSec 0 = off (the whole feature).
	// The lead feeds speculative enumeration ONLY -- admission, eviction and
	// retention stay on the true anchor. See the cvars' source comments.
	VOXELEARTH_API int32 GetStreamBatchRecompute();
	VOXELEARTH_API int32 GetStreamSpeculativeZTrim();
	VOXELEARTH_API int32 GetStreamFrameAttribution();
	VOXELEARTH_API float GetStreamVelocityLeadSec();
	VOXELEARTH_API float GetStreamVelocityLeadMaxUU();
	VOXELEARTH_API int32 GetStreamSpeculativeMaxParked();
	VOXELEARTH_API int32 GetStreamSpeculativeMaxInFlight();

	VOXELEARTH_API int32 GetStreamPoolParkMax();

	VOXELEARTH_API int32 GetStreamMaxRemeshesPerFrame();

	// voxel.Stream.MaxUnloadsPerFrame: max chunk-component unload events
	// (DrainUnloads -- pool-park, or DestroyComponent once the pool is at
	// cap) per frame. Default 24 (2026-07-24 pass -- keep pace with applies).
	VOXELEARTH_API int32 GetStreamMaxUnloadsPerFrame();

	// voxel.Stream.ComponentPoolMax (M1 hitch-gap wave, docs/status.md M1
	// gate row): max UVoxelChunkComponent instances parked (hidden, no scene
	// proxy, still registered/attached) in the reuse pool after leaving the
	// desired set, instead of DestroyComponent()'d immediately. A pending
	// first-load prefers a pooled component over NewObject+RegisterComponent
	// -- see FVoxelWorldImpl::AcquireChunkComponent -- avoiding the
	// render-thread AddPrimitive/RemovePrimitive churn the hitch-attribution
	// instrumentation pinned as the dominant cost at ring-boundary crossings.
	// Default 512; unloads past the cap fall back to DestroyComponent() (no
	// unbounded growth).
	VOXELEARTH_API int32 GetComponentPoolMax();

	// Shared hitch-frame threshold (ms), matching UVoxelPerfRunSubsystem's own
	// HitchThresholdMs (">30fps frame budget") -- used by
	// FVoxelWorldImpl::TickStreaming's per-frame attribution log so "which
	// frames count as a hitch" never disagrees between the two.
	inline constexpr float kHitchThresholdMs = 33.3f;
}

// --- Always-live streaming progress (front end / loading screen) ------------
//
// FVoxelPerfSnapshot BELOW IS NOT USABLE FOR THIS, and that is the entire
// reason this struct exists rather than a second accessor on that one.
// UpdatePerfSnapshot is called behind `if (VoxelDebug::GetDebugMode() >= 1)`
// (VoxelWorldSubsystem.cpp, a deliberate zero-cost-at-mode-0 gate), so in a
// normal session -- which is exactly when a player is watching a loading
// screen -- every field of it reads zero. A progress bar driven off it would
// sit at 0% forever and a readiness gate would never pass.
// UVoxelCharacterMovementComponent::IsTerrainReadyAt documents falling into
// this same trap and choosing a live query instead; this is that live query,
// aggregated.
//
// COST. Filling this walks ChunkRecords once, which is 39,020 entries at a
// settled 4 km cascade (VoxelWorldSubsystem.h's own measured figure). That is
// far too much to pay per frame and entirely affordable at the 0.4 s poll
// FVoxelWorldReadyProbe uses -- the header's contract is therefore "poll at
// 5 Hz or slower", and the probe logs its own per-poll milliseconds under
// -VoxelReadyProbeLog so the cost stays measured rather than assumed.
//
// PENDING MEANS SOMETHING DIFFERENT HERE than it does in FVoxelPerfSnapshot,
// and the difference is deliberate. That struct's LevelPendingCount folds in
// PendingUnloadKeys, because its job is to describe queue depth. This one
// counts only work that will PRODUCE geometry (job dispatch + game-thread
// mesh), because its job is to answer "is there anything left to wait for" --
// and a chunk queued for unload is the opposite of something to wait for.
// Counting unloads here would make the gate fail to pass during any camera
// move, which is precisely when a loading screen is up.
struct FVoxelStreamingProgress
{
	// Chunks currently holding drawn geometry, per ring level. Same predicate
	// as FVoxelPerfSnapshot::LevelLoadedCount (FChunkRecord::HoldsGeometry),
	// so the two agree wherever both are live.
	int32 LevelLoadedCount[VoxelCoords::kNumLevels] = {};
	// Queued and not yet drawn: PendingJobKeysByLevel + PendingGameThreadKeys.
	// Deliberately excludes PendingUnloadKeys -- see the note above.
	int32 LevelPendingCount[VoxelCoords::kNumLevels] = {};
	// Chunks with a worker job in flight right now (FChunkRecord::bJobInFlight),
	// per level. A ring is only settled when pending AND in-flight are both 0.
	int32 LevelJobsInFlight[VoxelCoords::kNumLevels] = {};

	int32 TotalJobsInFlight = 0;
	// ChunkRecords.Num(). Not a progress term -- it is the denominator of the
	// walk this struct costs, and having it in the log makes an unexpectedly
	// slow poll self-explaining.
	int32 TrackedChunks = 0;

	// False until UVoxelWorldSubsystem::StartWorldSession has run. Everything
	// above is zero in that state, and zero-because-nothing-started reads
	// identically to zero-because-nothing-loaded -- so a caller that does not
	// check this would compute 0/0 progress on the menu and call it ready.
	bool bSessionStarted = false;
};

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

	// --- Component pool (M1 hitch-gap wave, docs/status.md M1 gate row) -----
	// Chunk components parked (hidden, no scene proxy) instead of destroyed
	// when they leave the desired set; a first-load prefers one of these over
	// NewObject+RegisterComponent. See FVoxelWorldImpl::ComponentPool /
	// AcquireChunkComponent / ReturnChunkComponentToPool.
	int32 PooledComponents = 0;   // current pool size
	float PoolReusesPerSec = 0.f; // reuse events (pool hits) over the last refresh window
	int64 TotalPoolReuses = 0;    // cumulative reuse events since startup

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

	// --- Where the streaming anchor is, and how fast it is moving -----------
	//
	// The anchor is the pawn location TickStreaming was called with -- the same
	// point every ring radius, admission cutoff and retention decision is
	// measured from -- so this is the position that explains the rest of the
	// panel, not merely the camera's.
	//
	// SPEED IS FINITE-DIFFERENCED FROM THE ANCHOR, NOT READ FROM
	// Pawn->GetVelocity(). That matters specifically for the runs anyone wants
	// to watch: -VoxelPerfFlight=line drives the pawn with
	// SetActorLocationAndRotation(..., TeleportPhysics) every tick, bypassing
	// AddMovementInput and UFloatingPawnMovement entirely, so the movement
	// component's Velocity stays at zero for the whole flight. A speed row fed
	// from GetVelocity() would read 0.0 m/s across every perf leg while the
	// world visibly streams past. (Wave S3 needs this same signal for the
	// velocity-lead work -- see docs/speculative-generation-plan.md §2.4.)
	//
	// Metres, and metres/second: 1 voxel = 10 UU = 0.1 m, and every speed this
	// project quotes -- the 20 m/s flight, the movement tuning table -- is in
	// m/s. Converting once here keeps the HUD honest against those numbers.
	double AnchorXMeters = 0.0;
	double AnchorYMeters = 0.0;
	double AnchorZMeters = 0.0;
	// EMA-smoothed over ~0.25 s. Raw frame-to-frame delta is unreadable at
	// 60+ fps and jitters hard on any frame the pawn is repositioned.
	float AnchorSpeedMetersPerSec = 0.f;

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

// --- GPU streaming panel (voxel.Debug 3) ------------------------------------
//
// Published at 1 Hz by FVoxelWorldImpl::UpdatePerfSnapshot alongside
// FVoxelPerfSnapshot (same cadence, same doctrine: plain POD, no vxc:: types),
// read by AVoxelEarthHUD's mode-3 streaming panel. Everything in here is a
// read of counters that already exist -- the brick pool's add/eviction
// counters, the subsystem's job bookkeeping -- so filling it costs a handful
// of atomic loads once a second, not a walk.
//
// WHY THROUGHPUT IS POOL ADDS AND NOT THE CPU PACK COUNT. The 6,200/s floor
// was derived for chunks entering ring 0 (docs/marcher-handoff-2026-08-22.md:
// 30 m/s x 750 columns/s x 8.3 chunks/column), and a chunk only exists for the
// marcher when it becomes RESIDENT IN THE POOL. FVoxelBrickPool::
// GetChunksAdded() counts exactly that, from BOTH producer arms; the CPU pack
// counter (VoxelBrickGetCpuPackCount) counts only the CPU arm, so it reads
// ~equal today (adds 82,653 vs packs 82,749 on the measured leg -- the gap is
// refused/discarded packs) and would read ~ZERO once the GPU path carries the
// traffic, which is precisely when the owner will be looking at this panel.
// One counter family serves throughput AND the producer split, so the two rows
// cannot disagree about what a "chunk" is.
struct FVoxelStreamPanelSnapshot
{
	// False until the first 1 Hz publish after the panel's data source went
	// live. The HUD prints "collecting..." rather than zeros while false --
	// this project has retracted two findings in one session because an
	// instrument read zero and was believed to be "working, value zero".
	bool bValid = false;

	// --- Throughput (window = the ~1 s between publishes) -------------------
	float ChunksPerSec = 0.f;      // pool adds/s, both arms
	float ChunksPerSecPeak = 0.f;  // max 1 Hz window over the last ~30 s
	int64 ChunksAddedTotal = 0;    // cumulative pool adds since startup

	// --- Producer split -----------------------------------------------------
	// Window deltas and cumulative totals of the same GetChunksAdded family.
	// gpu + cpu == the window's adds, by construction.
	int64 WindowAddsGpu = 0;
	int64 WindowAddsCpu = 0;
	int64 TotalAddsGpu = 0;
	int64 TotalAddsCpu = 0;
	// VoxelGpuBrickPackEnabled() at publish time: false means the GPU arm is
	// switched OFF, so the HUD prints "arm off" instead of a 0% that would
	// read as a broken pipeline.
	bool bGpuArmEnabled = false;

	// --- Brick pool ---------------------------------------------------------
	int32 PoolResidentChunks = 0;
	uint32 PoolChunkCapacity = 0;   // FVoxelBrickPoolConfig::ChunkCapacity
	uint64 PoolResidentBytes = 0;   // format accounting, not VRAM commit
	int64 PoolEvictions = 0;        // the stated gate is that this STAYS 0
	int64 PoolAllocFailures = 0;    // adds refused even after eviction

	// --- Queue health -------------------------------------------------------
	int32 PendingJobs = 0;          // admission queue, all rings
	int32 CpuJobsInFlight = 0;      // worker jobs minus GPU-pending (the
	                                // dispatch loop's own CpuJobsOutstanding)
	int32 CpuJobsCap = 0;           // JobsInFlightPerCore x logical cores --
	                                // the REAL cap (the old snapshot printed a
	                                // stale 2 x cores against a real 8 x)
	int32 GpuDemandInFlight = 0;    // GpuJobsPending, non-speculative
	int32 GpuSpecInFlight = 0;      // GpuJobsPending, speculative
	int32 GpuInFlightCap = 0;       // VoxelStreamAdmission::GpuMeshInFlight()
	int32 SpecInFlightCap = 0;      // voxel.Stream.SpeculativeMaxInFlight

	// --- Dispatch-loop exit split (window counts) ---------------------------
	// Every streaming tick the dispatch loop ends ONE of two ways: the
	// in-flight cap filled (exitCap -- the workers are the limiter) or every
	// ring's queue drained (exitEmpty -- the dispatcher/admission side is the
	// limiter, or there is simply no work). The ratio is what says WHICH HALF
	// of the pipeline to blame when chunks/s is under the floor.
	int32 DispatchExitCap = 0;
	int32 DispatchExitEmpty = 0;
};

namespace VoxelDebug
{
	// The floor the throughput row is judged against: 6,200 level-0 chunks/s.
	// Derived, not chosen -- at the owner's 30 m/s flight speed, new ground
	// enters ring 0 at 2 x R x v = 7,680 m^2/s, which is 750 columns/s at
	// 10.24 m^2 per column, at 8.3 level-0 chunks per column
	// (docs/marcher-handoff-2026-08-22.md section 1). Measured 968-2,435/s the
	// day the floor was written down, i.e. a 3-6x shortfall; the panel's whole
	// job is to show where that number is now without anyone doing arithmetic.
	inline constexpr float kGpuStreamChunksPerSecFloor = 6200.f;
}

#include "VoxelDebug.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DelayedAutoRegister.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "VoxelCoords.h"

DEFINE_LOG_CATEGORY(LogVoxelStream);
DEFINE_LOG_CATEGORY(LogVoxelEdit);
DEFINE_LOG_CATEGORY(LogVoxelPerf);
DEFINE_LOG_CATEGORY(LogVoxelWater);

DEFINE_STAT(STAT_VoxelSubsystemTick);
DEFINE_STAT(STAT_VoxelWorkerJob);
DEFINE_STAT(STAT_VoxelGameThreadMesh);
DEFINE_STAT(STAT_VoxelEditApply);
DEFINE_STAT(STAT_VoxelChunksLoaded);
DEFINE_STAT(STAT_VoxelChunksInFlight);

namespace
{
TAutoConsoleVariable<int32> CVarVoxelDebug(
	TEXT("voxel.Debug"),
	0,
	TEXT("Voxel debug mode: 0=off, 1=perf HUD, 2=HUD+visualizations. F3 cycles in PIE/game."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugChunkStates(
	TEXT("voxel.Debug.ChunkStates"),
	true,
	TEXT("Chunk-state debug tints (just-loaded blue flash / edited orange / re-meshed purple flash). Live under voxel.Debug 2."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugBounds(
	TEXT("voxel.Debug.Bounds"),
	true,
	TEXT("Chunk AABB bounds wireframe for the nearest ~200 tracked chunks. Live under voxel.Debug 2."),
	ECVF_Default);

// NOTE: this is the ONE layer that is deliberately NOT gated behind
// voxel.Debug 2 -- see VoxelDebug.h. It is a play-testing scale reference, on in
// normal play by design; the cvar exists so screenshots can turn it off, not so
// it has to be turned on.
TAutoConsoleVariable<bool> CVarVoxelDebugPlayerBox(
	TEXT("voxel.Debug.PlayerBox"),
	true,
	TEXT("Player collision-volume wireframe, eye-height marker and ground-probe voxel cells, drawn in walk mode. ")
	TEXT("Unlike the other layers this is live at ANY voxel.Debug mode (default on); set 0 for a clean capture."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelWaterBucketFill(
	TEXT("voxel.Water.BucketFill"),
	200000,
	TEXT("How much water the `1` key pours at the player, in fill units (255 == one full voxel). ")
	TEXT("30000 is a puddle, 200000 a bathtub, 1000000 a small pond. Design aid; the pour goes through the same ")
	TEXT("SpawnWaterAt path as voxel.SpawnWater, so it is ledgered and conserved like any other injection."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelDebugRings(
	TEXT("voxel.Debug.Rings"),
	false,
	TEXT("Tint loaded chunks by mip ring level (R0 green .. R4 magenta) instead of chunk-state tints. Live under voxel.Debug 2."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelMipCacheBudgetMB(
	TEXT("voxel.MipCacheBudgetMB"),
	512,
	TEXT("Approximate byte budget (MB) for the shared cross-job mip cache (FSharedMipCache, VoxelWorldSubsystem.cpp). ")
	TEXT("Sharded approximate-LRU evicts on insert once over budget. <= 0 disables eviction (unbounded)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelServerMaxIntentsPerSec(
	TEXT("voxel.Server.MaxIntentsPerSec"),
	10,
	TEXT("M3 wave 2 validation hardening: per-connection token-bucket cap on dig/place/carve intent RPCs accepted per ")
	TEXT("second. Excess intents are rejected (logged), not disconnected."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelServerMaxCarveRadiusUU(
	TEXT("voxel.Server.MaxCarveRadiusUU"),
	400.0f,
	TEXT("M3 wave 2 validation hardening: server-side cap (UU) on ServerSubmitCarveIntent's RadiusUU. Requests above ")
	TEXT("this are rejected (logged)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelWaterMaxActiveBricks(
	TEXT("voxel.Water.MaxActiveBricks"),
	4096,
	TEXT("W2: advisory per-tick budget on vxc::WaterCA's active-set size. The CA tick is atomic (no mid-step cutoff ")
	TEXT("without breaking conservation/determinism), so exceeding this only logs a throttled warning -- see ")
	TEXT("UVoxelWaterSubsystem::TickWater."),
	ECVF_Default);

// The water half of ADR-0006. Deliberately a SEPARATE cvar from
// voxel.Stream.GPU rather than a rider on it: water and terrain are different
// primitives with different materials, and the water one is TRANSLUCENT. That
// makes their failure modes different enough that being able to run pooled
// terrain against per-brick water (or the reverse) is the bisection this needed
// -- exactly the argument voxel.Stream.GPUMaxLevel makes for mip rings.
//
// Read once per brick handoff, so like voxel.Stream.GPU it must be set before
// the water it should affect meshes; bricks already drawn keep the
// representation they were built with, and unload the way they loaded.
TAutoConsoleVariable<bool> CVarVoxelWaterGpu(
	TEXT("voxel.Water.GPU"),
	true,   // 2026-07-27: ON for the manual PIE evaluation. See voxel.Stream.GPU.
	TEXT("Route active + implicit water surface geometry through its own ADR-0006 GPU pool (ONE primitive, ONE ")
	TEXT("draw) instead of one UWaterChunkComponent per vxc::WaterBrick8. Default false. Independent of ")
	TEXT("voxel.Stream.GPU -- water is a separate pool with a separate, translucent material."),
	ECVF_Default);

// W4: arm the shallow-water layer (docs/adr/0004-swe-fixed-point-coupling.md).
//
// DEFAULT 0, AND THAT IS LOAD-BEARING, not caution. voxelcore/swe.h shipped
// INERT -- `SweCoupleConfig::enabled` defaults false and nothing constructs an
// `SweGrid` -- and the argument that let it merge without sign-off was
// precisely that an untouched world stays byte-identical: no golden moved, no
// version bumped, float-ban clean. Arming is opt-in per run so that argument
// survives intact; a world nobody typed this cvar into runs exactly the code it
// ran before W4 existed, down to the branch.
//
// NM_STANDALONE ONLY, ENFORCED AT THE POINT OF USE, NOT HERE. ADR-0004's item 3
// is ACCEPTED with enablement DEFERRED to "M3 networked water", because the
// coupler is a second simulation whose membership/dwell/sheet-depth state "must
// replicate or be derived identically on every client ... it is not yet wired
// into the replication path at all. Enabling it before that is a guaranteed
// desync." That is a statement about a world that has a client in it. In
// NM_Standalone there is no mirror to disagree with and no wire to be late on,
// so the deferral has nothing to bite on -- and refusing every other net mode is
// how the deferral is KEPT rather than quietly overridden. The refusal lives in
// UVoxelWaterSubsystem::MaybeArmSwe (which is where the net mode is known and
// where a clear one-shot log line citing the ADR can be emitted); this cvar is
// only the raw request.
//
// Read every fixed step, same "no relaunch needed" pattern as
// voxel.Water.SolidCacheEnabled: flipping to 0 flushes the sheet back into the
// CA through the ledgered demotion channel and tears the grid down, so the
// switch is genuinely reversible in both directions mid-session.
TAutoConsoleVariable<bool> CVarVoxelWaterSwe(
	TEXT("voxel.Water.SWE"),
	false,
	TEXT("W4 (ADR-0004): arm the shallow-water sheet + CA<->SWE coupler on the water subsystem's fixed 10Hz step, ")
	TEXT("giving open water actual momentum instead of Phase C's static equilibrium level. Default 0. REFUSED on ")
	TEXT("any net mode except NM_Standalone -- ADR-0004 item 3 defers enablement until the coupler's membership/dwell/")
	TEXT("depth state is replicated, and a standalone world is the one case that deferral does not cover. ")
	TEXT("NOTE: the renderer does not yet draw sheet depth (ADR-0004 'Renderer'), so promoted water is currently ")
	TEXT("SIMULATED BUT INVISIBLE."),
	ECVF_Default);

// W3 (plan S3.7 Layer R): the coarse river-network sim and its coupling to the
// water CA. Default 0 for the same reason every other new simulation layer in
// this file defaults off -- a layer does not get to start changing a live world
// because it was linked in.
//
// REFUSED on NM_Client only, NOT narrowed to NM_Standalone the way
// voxel.Water.SWE is, and the difference is worth stating because it looks like
// an inconsistency. ADR-0004 item 3 gates the SWE coupler because SWE holds
// SIMULATION STATE A CLIENT MUST SEE (sheet depth is water the renderer draws)
// and none of it replicates. The river coupler holds no such state: its only
// observable output is WaterCA fill, which already replicates through the
// existing water-diff channel, and the graph itself is server-side bookkeeping
// a client never reads. So a listen/dedicated server may run it and its clients
// mirror the resulting water exactly as they mirror a pour.
TAutoConsoleVariable<bool> CVarVoxelWaterRivers(
	TEXT("voxel.Water.Rivers"),
	false,
	TEXT("W3 (plan S3.7 Layer R): build the coarse river-network graph around the player and couple it to the water ")
	TEXT("CA -- segment discharge tops up real voxel water along each reach, the ocean (voxel z=0) is the sink, and ")
	TEXT("sustained CA flux down a fresh channel promotes it to a new segment. Default 0. Refused on NM_Client ")
	TEXT("(rivers tick server-side; clients mirror the resulting water diffs). The graph is built ONCE around the ")
	TEXT("arming anchor and is not yet persisted, so promotions do not survive a reload."),
	ECVF_Default);

// 64 -> 192 (2026-07-27, S1 close): 794 -> 1,040 chunks/s with converged holes
// 6-14 -> 0, two clean legs each, 1.4% spread. 384 measured 1,044 -- inside
// noise of 192 -- so 192 is the knee and there is nothing above it.
//
// This ceiling only started binding once batched publication made an apply
// cheap enough that DrainResults could hit 64 before spending its 6 ms
// ApplyBudgetMs. Before that it was unreachable and therefore invisible.
//
// AND IT WAS BRIEFLY RECORDED AS "REJECTED, MEASURED WORSE", WHICH WAS WRONG.
// That leg (749 chunks/s, 179 holes) shared the box with a second editor for 86
// seconds. Re-run alone on the same binary it gave 1,033. A contended leg looks
// exactly like a slow configuration, and a plausible mechanism had already been
// written to explain it -- which is how a 30% win nearly went into the
// do-not-re-litigate list permanently. tools/voxel-run-flight-leg.ps1 now
// refuses to start while another editor is alive.
// docs/measurements/s1-close-2026-07-27.txt.
TAutoConsoleVariable<int32> CVarVoxelStreamMaxAppliesPerFrame(
	TEXT("voxel.Stream.MaxAppliesPerFrame"),
	192,
	TEXT("Streaming throughput: HARD CEILING on worker-mesh-result chunk-component applies ")
	TEXT("(FVoxelWorldImpl::DrainResults) per frame. As of the 2026-07-24 streaming-speed pass this is a safety ")
	TEXT("ceiling, not the steady-state throttle -- the loop drains until voxel.Stream.ApplyBudgetMs of wall-clock ")
	TEXT("is spent (or the queue empties, or this ceiling is hit), whichever comes first. History: constant 8 ")
	TEXT("(m1-plan Stage 2) -> 3 (hitch isolation, 2026-07-20) starved fill to ~180 chunks/s, taking MINUTES to ")
	TEXT("fill the ring cascade and leaving 1-2 min bare-terrain lag on every LOD upgrade. Raised + time-budgeted ")
	TEXT("(Matt directive: prioritize silky/fast streaming, tolerate rare hitches during the initial load storm; ")
	TEXT("steady state stays smooth because the queue is small so neither cap binds)."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelStreamApplyBudgetMs(
	TEXT("voxel.Stream.ApplyBudgetMs"),
	6.0f,
	TEXT("Streaming throughput (2026-07-24 pass): max WALL-CLOCK milliseconds FVoxelWorldImpl::DrainResults may spend ")
	TEXT("applying finished chunk meshes to the scene per frame. The loop always applies at least a small floor for ")
	TEXT("forward progress, then keeps going while under this budget, up to voxel.Stream.MaxAppliesPerFrame. Because ")
	TEXT("the true proxy-create/GPU-upload cost partly surfaces on the render thread a frame or two later, a large ")
	TEXT("budget during a load storm can hitch -- that is the accepted trade for fast fill. Lower it to protect frame ")
	TEXT("pacing at the cost of slower fill; raise it to fill harder."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelStreamLodRetentionMs(
	TEXT("voxel.Stream.LodRetentionMs"),
	10000.0f,
	TEXT("Load-before-unload SAFETY CAP (2026-07-24 streaming-speed pass). When a VISIBLE chunk is evicted because a ")
	TEXT("different LOD ring took over its footprint (toward -> finer, away -> coarser), it is kept drawn as a stand-in ")
	TEXT("until its replacement LOD is actually on screen -- COVERAGE-based release (ReplacementCovered vs ChunkRecords), ")
	TEXT("not a fixed timer, so there is no rolling ring of holes where a timer would expire mid-transition. This value ")
	TEXT("is only the backstop: a footprint that never gets covered (e.g. a coastal quarter that is all ocean) is parked ")
	TEXT("after this many ms so resident chunks cannot grow unbounded. Cost: brief coarse+fine double-draw at the ")
	TEXT("boundary until coverage (minor shimmer, no hole). 0 disables retention entirely (immediate unload = holes). ")
	TEXT("RETUNED 5000 -> 20000 -> 10000 on 2026-07-27: instrumented 20 m/s line flights showed 5000 was too short for ")
	TEXT("the backstop to be a backstop -- stand-ins hit the cap and parked while their replacements were still ")
	TEXT("mid-pipeline, which is a hole. 20000 logged ZERO flight-phase cap releases but held +1,355 extra resident ")
	TEXT("chunks (+12.9% quads, pool 44.2% vs 39.4%), making every frame ~12% heavier on the render thread and tripling ")
	TEXT("the over-33.3ms frame count on the GPU-fork legs. 10000 is the balance point pending the fork's render-thread ")
	TEXT("overhead fix; allocFail=0 at every setting tried."),
	ECVF_Default);

// Terrain sun-shadow casting (PR #95, "the sun should not light a sealed cave").
// UVoxelChunkComponent sets CastShadow=true so terrain renders into the
// directional light's shadow-depth pass; this exists to A/B what that COSTS on
// the render thread, which is where the 2026-07-24 hitches live (renderMs=43.12
// of a 43.92 ms frame -- docs/status.md "M1 gate re-run").
//
// WHY NOT JUST r.ShadowQuality 0: measured 2026-07-24 and it is INVALID for this
// question. Turning it off produced 118 post-warmup hitches vs 2, with LOW
// renderMs -- because this module PSO-precaches the terrain material at BeginPlay
// (FPSOPrecacheParams), and changing render scalability at startup desyncs that
// precache from what is actually drawn, so every chunk hits a pipeline-state
// miss. That measures precache invalidation, not shadow cost. This cvar changes
// ONE primitive flag and leaves scalability alone.
//
// Read per chunk-component load, so an -ExecCmds value applies to the whole run.
// Flipping it mid-session affects only chunks loaded after the flip (already
// resident components keep the flag they loaded with) -- fine for a startup A/B,
// which is what it is for.
TAutoConsoleVariable<bool> CVarVoxelRenderCastShadow(
	TEXT("voxel.Render.CastShadow"),
	true,
	TEXT("Whether voxel terrain chunks cast sun/dynamic shadows (PR #95). Default true (shipping behaviour). Set 0 to ")
	TEXT("A/B the render-thread cost of the terrain shadow-depth pass WITHOUT touching render scalability -- see the ")
	TEXT("source comment for why r.ShadowQuality is not a valid substitute here. Turning this off makes the sun light ")
	TEXT("sealed caves again, so it is a measurement tool, not a shipping setting."),
	ECVF_Default);

// ADR-0006 G3: route chunk geometry through the GPU pool instead of one scene
// component per chunk.
//
// Default OFF. This is the flag the whole GPU streaming programme hides behind,
// and it stays off until G5 flips it, so the shipping renderer is exactly the
// one that has been flown and measured. On, every resident chunk becomes a
// range in one persistent GPU buffer drawn by ONE primitive in ONE draw call,
// and streaming a chunk in or out stops touching FScene entirely -- which is
// the funnel ADR-0006 measured as the frame-time ceiling.
//
// Only the geometry handoff moves. The desired set, admission, dispatch,
// retention and coverage logic upstream are shared and unaware of this flag,
// so an A/B here changes how chunks are DRAWN and nothing about which chunks
// are chosen. Toggling mid-flight is not supported: chunks already resident
// under the old path keep their old representation until they unload.
TAutoConsoleVariable<int32> CVarVoxelStreamGpuMaxLevel(
	TEXT("voxel.Stream.GPUMaxLevel"),
	-1,
	TEXT("Pool only ring levels <= this under voxel.Stream.GPU; coarser rings stay on the per-chunk component ")
	TEXT("path. -1 = all levels. The two renderers coexist per chunk, so this isolates 'the pooled path is wrong' ")
	TEXT("from 'the pooled path is wrong FOR MIP RINGS' -- and may be a shipping mode in its own right, since the ")
	TEXT("pooling win scales with chunk count and that is concentrated in the dense near rings."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamLogAdmission(
	TEXT("voxel.Stream.LogAdmission"),
	0,
	TEXT("Log the per-level admission loop state on every RecomputeDesiredSet call: did the ring re-enumerate, ")
	TEXT("how many candidates it turned away, its pending queue depth, and whether its refill trigger is still ")
	TEXT("armed. Those four decide whether a ring keeps filling, and no existing counter shows them together."),
	ECVF_Default);

// Wave S0 (docs/speculative-generation-plan.md §4, executing T0-1): split the
// per-apply cost into pack / params / pool-add so the deep review's §1a can be
// confirmed or killed in one leg instead of being built on.
//
// GATES THE CLOCKS, NOT THE COUNTS. The DrainResults exit-reason counters are
// unconditional -- four increments per frame, and they are what settle whether
// the apply loop exits on an empty queue or on its 6 ms wall clock, which the
// published "results are not ARRIVING" reading assumes without ever having
// measured. The FPlatformTime::Seconds pairs guarded by this are the part that
// could plausibly become what it measures, on a path that runs up to 64 times a
// frame. A leg with this OFF, showing avgChunks/s within noise of a leg with it
// ON, is part of closing the wave -- not optional.
TAutoConsoleVariable<int32> CVarVoxelStreamApplyStageStats(
	TEXT("voxel.Stream.ApplyStageStats"),
	0,
	TEXT("1 = time each apply's quad pack, SampleChunkParamsForPool and pool add (including the ")
	TEXT("PushUpdatesToProxy each ends in), reported per 5s window. Default 0. Pair it with ")
	TEXT("voxel.Stream.PoolPushStats, which splits the pool-add bucket across both threads."),
	ECVF_Default);

// Ring-gap wave (docs/status.md "ring gap"). The 5s retention census says HOW
// MANY stand-ins were released and why; it cannot say WHICH ones, and the
// hypothesis under test is positional -- coarse chunks released at the INNER
// ring edge, where the finer ring has not arrived. One line per release, with
// the chunk key and its radius from the anchor, is what makes a logged number
// comparable against a screenshot of the hole.
//
// Per-chunk and therefore OFF by default and never suitable for a perf run:
// the census line above is the always-on instrument, this is the drill-down.
TAutoConsoleVariable<int32> CVarVoxelStreamLogRetention(
	TEXT("voxel.Stream.LogRetention"),
	0,
	TEXT("Log one line per load-before-unload stand-in release that was NOT a clean settled cover: every ")
	TEXT("covered-ABSENT release (the covered verdict rested on a child/parent record that does not exist, which ")
	TEXT("is only sound if that record was genuinely undesired) and every safety-cap release. Level, chunk key, ")
	TEXT("radius from the anchor in metres, and the absent/settled split of the replacement columns consulted. ")
	TEXT("Per-chunk output -- diagnostic only, never a perf-run setting."),
	ECVF_Default);

// Ring-gap wave: turn the visual symptom into a number. See the coverage-verify
// block in MaybeLogCounters for what it scans and the two prior failure modes
// its coverage test is built to dodge. O(tracked) plus one annulus enumeration
// per level, ONCE per log window, and entirely inside the enable check -- a
// default run pays a single cvar read.
TAutoConsoleVariable<int32> CVarVoxelStreamCoverageVerify(
	TEXT("voxel.Stream.CoverageVerify"),
	0,
	TEXT("Once per periodic perf-log window, enumerate every XY footprint whose centre lies in a ring's core ")
	TEXT("annulus and count the ones no level is visibly covering -- the see-through holes, as a number, with a ")
	TEXT("few examples and their radius. READ-ONLY: it changes no streaming decision. Off by default because it ")
	TEXT("re-walks every ring's annulus."),
	ECVF_Default);

// S0-3 (docs/speculative-generation-plan.md Wave S0 / T0-2). Off by default:
// the window bookkeeping this gates adds new FPlatformTime::Seconds() calls
// (FJobResult::DeliverSeconds on both producer arms) on top of timestamps that
// already exist elsewhere, and the instrument must not be able to become part
// of what it measures. Flip on for one leg per arm when comparing QueuedMs
// against DispatchToReadyMs/ReadyToDeliverMs -- see MaybeLogCounters for the
// log line shape.
TAutoConsoleVariable<int32> CVarVoxelStreamLatencyStats(
	TEXT("voxel.Stream.LatencyStats"),
	0,
	TEXT("Per-producer, per-stage submit->apply latency windows (p50/p95/max over a 256-sample ring, same ")
	TEXT("idiom as the existing worker-ms window) plus the per-level quad-count distribution. GPU arm: ")
	TEXT("QueuedMs, DispatchToReadyMs, ReadyToDeliverMs, SubmitToDeliverMs, DeliverToApplyMs. CPU worker arm: ")
	TEXT("its existing end-to-end JobMs isolated from the GPU population, plus its own DeliverToApplyMs. ")
	TEXT("Default 0. Diagnostic only -- one standard leg per arm with this on is what Wave S0 closes on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamGpuMaxChunks(
	TEXT("voxel.Stream.GPUMaxChunks"),
	0,
	TEXT("Debug bisection: cap chunks admitted to the ADR-0006 GPU pool. 0 = unlimited. Exists so the streamed ")
	TEXT("path can be run at the same small scale voxel.GPU.SpawnPool is known-good at, separating 'CPU-meshed ")
	TEXT("quads through the pool are wrong' from 'the pool does not like a multi-million-quad draw'."),
	ECVF_Default);

// G5 was flipped to TRUE on 2026-07-25 and is flipped BACK to false the same
// day, on a measurement that finally worked.
//
// THE POOL HAS NO PER-CHUNK FRUSTUM CULLING, AND THAT COSTS MORE THAN THE
// PRIMITIVE COUNT SAVES -- at least once the world has settled.
//
// The component path hands the scene 9,822 separate primitives and lets it cull
// each one. The pool submits ONE draw covering its entire contents every frame,
// so it pays for all 8,813,242 quads no matter where the camera looks. Measured
// with -VoxelPerfFlight=static (position AND rotation pinned, pose logged),
// settled scene, identical geometry on both paths, three legs each:
//
//   camera at the horizon (pitch -20):
//     p50  component 15.12 ms [10.4-15.5]   pooled 18.58 ms [18.2-19.5]   +23%
//     p95  component 20.75 ms [17.0-22.5]   pooled 24.59 ms [24.4-25.5]   +19%
//
//   camera straight down (pitch -89), very little in frustum:
//     p50  component  5.39 ms [ 4.6- 6.2]   pooled 19.05 ms [18.6-19.5]  +253%
//
// Non-overlapping ranges in both configurations. The control is the second row:
// pointing the camera at almost nothing makes the component path 64% cheaper
// (15.12 -> 5.39 ms) and leaves the pool UNCHANGED (18.58 -> 19.05 ms, +3%).
// A renderer whose cost does not depend on what is on screen is a renderer that
// is not culling, and that is the whole finding.
//
// This does NOT refute ADR-0006, and the distinction matters. The ADR targets
// the per-chunk FScene::AddPrimitive cost paid while STREAMING; these runs have
// zero streaming in them by construction, so they measure the drawing side,
// where the pool traded culling away for one draw call. The two effects pull in
// opposite directions, which is exactly why every moving-flight measurement in
// this session was ambiguous -- a flight mixes both regimes.
//
// WHAT WOULD MAKE THE POOL WIN OUTRIGHT: GPU-driven culling. A compute pass over
// the chunk table emitting an indirect draw (or per-chunk draw ranges) restores
// per-chunk visibility rejection while keeping one primitive and one draw call.
// That is the natural completion of ADR-0006 rather than a departure from it,
// and until it exists the pool should not be the default.
//
// Still true, and still worth having: the pooled path is at visual parity
// (17.4% -> 4.3% of pixels differing, against a measured 1.1% noise floor), it
// draws 9,822 chunks as one primitive and one draw, and voxel.Stream.GPU 1 turns
// it on for anyone measuring the streaming side. See docs/streaming-handoff.md.
TAutoConsoleVariable<bool> CVarVoxelStreamGpu(
	TEXT("voxel.Stream.GPU"),
	true,   // 2026-07-27: ON for the manual PIE evaluation. See the note below.
	TEXT("Route chunk geometry through the ADR-0006 GPU pool (ONE primitive, ONE draw) instead of one scene ")
	TEXT("component per chunk. Default FALSE: the pool has no per-chunk frustum culling, so it pays for every ")
	TEXT("resident quad regardless of where the camera looks, and measures 23%% slower at p50 on a settled ")
	TEXT("scene (253%% when little is in frustum). Set 1 to enable it -- it is at visual parity and is the ")
	TEXT("right path for measuring the streaming side. Set before the world streams in; toggling mid-flight ")
	TEXT("leaves already-resident chunks on whichever path loaded them."),
	ECVF_Default);

// Worker slots in flight, as a multiple of logical cores (docs/m1-plan.md Stage
// 2 decisions table pinned this at "<=2xLogicalCores", which was a hardcoded
// 2 * NumberOfCoresIncludingHyperthreads() until 2026-07-24).
//
// WHY THIS IS NOW TUNABLE. Measured on the 20 m/s scripted flight: 14,540
// records ADDED to the desired set per 5 s but only 7,704 jobs DISPATCHED, with
// 12,306 evicted -- ~6,800 chunks per 5 s entered the desired set and were
// evicted before a worker ever touched them. That is the rolling-ring hole, and
// it is neither the apply funnel (budget only ~28% saturated) nor the workers
// (drained == dispatched, stale = 0).
//
// It is dispatch STARVATION, and the arithmetic is stark: DispatchJobs tops the
// queue up to the cap ONCE PER FRAME, an R0 job has p50 1.32 ms, and a frame is
// ~15 ms. So each of the 24 slots does roughly ONE job per frame and then idles
// ~13.7 ms. Measured dispatch was 1,540 jobs/s against a 24-slot x 1.32 ms
// capacity of ~18,000/s -- about 9% worker utilisation. Raising the multiplier
// keeps the task graph fed ACROSS frames instead of re-starving every frame.
//
// RAISED 2 -> 8 on 2026-07-27, measured on real terrain (docs/measurements/
// ring-gap-2026-07-27.txt): 128 m-cascade cold fill 50.5 s -> 38 s avg (~25%
// faster, wide spread 41/35 s so call it +/-10%), 64 m cold fill 4 s -> 2 s
// twice, and a 20 m/s motion leg at 8 loaded MORE chunks than any other leg
// that night (93,734) with the same 8 hitches, holes=0, p95 within noise.
//
// Costs to watch when raising it further: more in-flight jobs means more
// results landing per frame (bounded by voxel.Stream.MaxAppliesPerFrame /
// ApplyBudgetMs), more peak memory in flight, and more STALE results (a chunk
// evicted while its job runs is discarded -- watch the "job flow" census).
//
// 2026-08-23 SIZING NOTE -- the starvation above is back, one octave up. The
// index-upload fix made the CPU arm the majority producer (93.5% of packs)
// with jobs at p50 0.34-0.6 ms, and once per tick the dispatch loop fills to
// the cap and stops: measured 4,342 chunks/s mean = 84.1 CPU launches per
// 20.7 ms tick, which is exactly cap 96 minus the ~12 still in flight when
// the tick starts. The cap is therefore functioning as a PER-TICK BATCH
// QUOTA, and its ceiling is cap x tick rate: 96 x 48.3 = 4,637/s -- below
// the owner's 6,200 chunks/s floor by construction, no matter how idle the
// 8 workers are (they retire 96 x 0.5 ms / 8 = ~6 ms of work and then idle
// the remaining ~14 ms of every tick). To make 6,200 REACHABLE with the GPU
// arm contributing its measured ~280/s: (6,200 - 280) / 48.3 = 122.6 CPU
// launches/tick, i.e. this cvar at >= 11; 12 gives headroom (144/tick,
// ~6,950/s ceiling). THE CAVEAT THAT KEEPS THIS A NOTE AND NOT A NEW
// DEFAULT: the ceiling this lifts is the quota, not the game thread. The
// dispatch bucket already measures 42-86% of wall, and per-dispatched-chunk
// game-thread cost was last measured at ~0.21 ms -- at which 123/tick costs
// ~26 ms and blows the tick. Whether that per-chunk figure still holds
// post-merge is UNKNOWN (never re-measured); run the FrameAttribution leg
// and read the 'Voxel dispatch loop:' exitCap/exitEmpty counters before and
// after any raise. If chunks/s does not rise with the cap while exitCap
// still dominates, the game thread is the wall and the fix is per-dispatch
// cost, not a bigger quota.
TAutoConsoleVariable<int32> CVarVoxelStreamJobsInFlightPerCore(
	TEXT("voxel.Stream.JobsInFlightPerCore"),
	8,
	TEXT("Worker jobs allowed in flight, as a multiple of logical cores (96 at the default 8 on a 12-thread box; ")
	TEXT("raised from 2 on 2026-07-27, ~25% faster real-terrain cold fill, no hitch or coverage cost on record). ")
	TEXT("DispatchJobs refills to this cap once per frame, so with short jobs (R0 p50 ~1.3ms) and a ~15ms frame the ")
	TEXT("slots idle most of the frame -- measured ~9% worker utilisation and ~6,800 chunks per 5s evicted before ")
	TEXT("ever being dispatched. Raise to keep the task graph fed across frames. Watch stale%% in the job-flow census."),
	ECVF_Default);

// DispatchJobs tops the worker queue to MaxJobsInFlight once per TickStreaming
// call, at the top of the budget block; DrainResults then runs and frees
// slots as JobsInFlightCounter decrements. Those freed slots sit idle until
// NEXT frame's dispatch -- the same once-per-frame refill cadence that the
// JobsInFlightPerCore comment above measured at ~9% worker utilisation (24
// slots x 1.32ms R0 jobs against a ~15ms frame, ~1,540 jobs/s dispatched
// against an ~18,000/s slot capacity). Raising JobsInFlightPerCore 2->8
// widens the buffer so starvation takes longer to bite, but does not touch
// the cadence itself -- a wider queue still only gets topped up once a frame.
//
// This cvar adds a second DispatchJobs() call after DrainResults and
// DrainGameThreadMesh (see TickStreaming), so slots freed earlier in THIS
// frame get refilled in the SAME frame instead of sitting idle until the
// next one. Gated on queue pressure (PendingJobNum() > 0 at the point of the
// second call) so an idle frame -- nothing pending, workers caught up -- pays
// only the PendingJobNum() scan and nothing else.
//
// Default OFF, AND THAT IS A MEASUREMENT (2026-07-27 cadence A/B, real-terrain
// 20 m/s line flight, JobsInFlightPerCore already at 8): ON bought +0.2% chunks
// on the CPU arm (92,249 vs 92,080 -- inside the same-config noise floor) and
// nothing at all on the GPU arm (89,666 vs 89,588 the previous leg), while
// COSTING 22 vs 8 hitches on the CPU arm. With a 96-job in-flight buffer the
// once-per-frame top-up no longer starves anyone; the second call just lands
// extra dispatch work inside already-busy frames. Kept as a lever because the
// cold-fill case (queue perpetually saturated) was NOT part of that A/B and is
// where a second top-up could still pay -- measure there before flipping.
TAutoConsoleVariable<int32> CVarVoxelStreamDispatchAfterDrain(
	TEXT("voxel.Stream.DispatchAfterDrain"),
	0,
	TEXT("Run a second DispatchJobs() pass after DrainResults/DrainGameThreadMesh, so slots freed earlier this frame ")
	TEXT("are refilled same-frame instead of idling until next frame's dispatch. Default 0: with ")
	TEXT("JobsInFlightPerCore=8 the motion A/B measured no throughput gain and +14 hitches (2026-07-27). ")
	TEXT("Skipped cheaply when nothing is pending. A/B lever for cold-fill work."),
	ECVF_Default);

// 2026-08-23 RE-DERIVATION OF THE ABOVE, after the index-upload fix moved the
// pipeline ceiling. The 2026-07-27 "no throughput gain" verdict was taken when
// the in-flight cap never bound (the GPU fork carried most chunks and
// CpuJobsOutstanding read 0 all run -- see the phase-3 oversubscription
// audit). Today the producer mix is inverted -- the CPU arm packs 93.5% of
// chunks (brickFromGpu=49,665 of brickPacks=768,799) at p50 0.34-0.6 ms/job --
// and the arithmetic now says the cap DOES bind, as a per-tick batch quota:
//   measured 4,342 chunks/s mean, CPU arm ~4,060/s, frames p50 20.7 ms
//   (48.3 ticks/s) => 84.1 CPU launches per tick, against a cap of 96 with
//   ~12 still in flight at the log point -- 96 - 12 = 84. Exact agreement.
// A worker retires a 0.5 ms job long before the next tick, so "96 in flight"
// degenerates into "96 dispatched per tick, then the workers go idle until
// the next tick's single top-up" -- the same starvation shape the 2->8 raise
// fixed on 2026-07-27, reappearing one octave up. The owner floor is 6,200
// chunks/s; 96 x 48.3 = 4,637/s is below it BY CONSTRUCTION.
// The 'Voxel dispatch loop:' log line (exitCap= vs exitEmpty=) is the
// instrument that decides this: exitCap dominating a flight window proves the
// batch-quota reading; exitEmpty dominating refutes it and moves the blame to
// admission/refill cadence. Until that leg is read, the paragraph above is
// arithmetic on one flight's counters, not a measured verdict.

// 2026-08-23: test the ring slot floors against CPU-arm occupancy only, and
// make a floor-deficit pick dispatch on the CPU arm.
//
// THE EFFECT THIS EXISTS FOR: mid-flight the coarse rings starve while their
// queues sit full. Measured (phase-3 oversubscription audit, one 5 s window):
// R2-R5 dispatched 1-2 jobs each against 245-347 pending -- and those pending
// numbers are EXACTLY kRingCapShare x PendingJobCap (0.17*2048=348,
// 0.15*2048=307, 0.13*2048=266, 0.12*2048=245), i.e. the admission caps are
// pinned because dispatch never drains them. A repeating exact number is a
// clamp.
//
// THE MECHANISM, read from the code, not inferred: DispatchJobs increments
// LevelJobsInFlight[level] BEFORE the GPU fork decides who meshes the chunk,
// so a GPU job occupies its ring's floor slot for its whole round trip --
// measured p50 2,281-2,348 ms, p95 3,300-3,458 ms submit->deliver. The floor
// deficit test (Floors[L] - LevelJobsInFlight[L] > 0) therefore never fires
// for a ring whose ONE outstanding job is a GPU job, the ring is served only
// by the nearest-first pass -- which R0's 38.4 m-deep column always wins --
// and the cycle self-sustains: the GPU job completes, the deficit fires once,
// the pick forks to the GPU again, another ~2.3 s hold. One dispatch per GPU
// round trip per ring is precisely the measured 1-2 per 5 s.
//
// The floors were sized in worker-slot units ("R0 jobs are ~0.8 ms p50...a
// reserved slot is not a small loan" -- see kRingSlotFloorDefault), and a
// 2.3-second GPU round trip is not a worker slot. With this switch ON the
// floor test counts only CPU-arm jobs, and a pick that a floor deficit chose
// goes to the CPU worker directly instead of offering the fork -- the floor
// promised a WORKER slot, so it delivers one. Without that second half a
// deficit pick could fork to the GPU without ever raising CPU occupancy, and
// one ring could pump its whole queue into the fork in a single tick (the
// blended count self-limited exactly because the fork raised it).
//
// WHY THIS IS A SWITCH AND DEFAULT OFF: the recorded floor catastrophe
// (floors {0,2,3,4,4} reserved 13 of 24 slots; whole-run throughput collapsed
// 49,179 -> 558 chunks) was long jobs occupying the ~8-thread worker pool
// itself. This change does not resize the floors (still at most 5 slots of
// 96, 5.2%) and the jobs those floors now admit cost 0.34-0.44 ms p50
// (2026-08-23, all levels), so a floor slot is returned in under a
// millisecond -- the catastrophe's mechanism (3-second jobs on real threads)
// is not reachable from here at today's job costs. But the catastrophe is
// also the recorded reason floors and caps are never retuned in the same
// change, and this ships in the same window as other dispatch work -- so it
// defaults to today's behaviour and is flipped alone, in its own leg.
//
// PROVES IT WORKED: R2-R5 disp= on the 'Voxel ring dispatch' line rises out
// of the floor (>=50 per ring per 5 s against the measured 1-2) AND their
// pending= values fall off the exact admission-cap numbers above. PROVES IT
// DID NOT: R0 disp= or R0 residency falls >10%, or brickPacks/s falls -- either
// says the coarse rings bought their slots from the near field, which is the
// 49,179->558 failure in miniature. Revert on either.
TAutoConsoleVariable<int32> CVarVoxelStreamRingFloorCpuOnly(
	TEXT("voxel.Stream.RingFloorCpuOnly"),
	0,
	TEXT("Ring slot floors count CPU-arm jobs only, and a floor-deficit pick dispatches on the CPU worker ")
	TEXT("(a ~2.3 s GPU round trip no longer satisfies a floor sized for sub-millisecond worker slots, which ")
	TEXT("starved R2-R5 to 1-2 dispatches per 5 s against 245-347 pending). Default 0: floors test the blended ")
	TEXT("CPU+GPU in-flight count, exactly the pre-2026-08-23 behaviour."),
	ECVF_Default);

// S2-0 (2026-07-27). Bounded admission had a second failure mode that only
// appeared once the consumer got fast.
//
// RecomputeDesiredSet relaxes every LevelAdmissionCutoffDistSq to DBL_MAX when
// the pending job queue has drained below 3/4 of voxel.Stream.PendingJobCap.
// That test is a proxy for "there is spare capacity downstream" -- and it was
// only ever a correct proxy because the queue was short for exactly one reason:
// the consumer could not keep up. Wave S1 removed that reason. The queue is now
// short because applies are fast, so the cutoff relaxes on essentially every
// call and bounded admission is off in practice.
//
// Measured at the S1 config: 22,300 records/s admitted against ~800 chunks/s
// loading, ChunkRecords at 86,077 against a 39,020 settle, and
// RecomputeDesiredSet at 66% of the streaming tick paying an O(tracked) exit
// scan for records that will never be reached.
//
// This bounds what is actually growing. 0 = off (the pre-S2-0 behaviour). Size
// it above peak flight residency (~50,900 measured) and far below 86,077.
TAutoConsoleVariable<int32> CVarVoxelStreamAdmissionRecordCap(
	TEXT("voxel.Stream.AdmissionRecordCap"),
	0,
	TEXT("Stop relaxing the per-level admission cutoff once ChunkRecords reaches this many entries. 0 = off ")
	TEXT("(pre-2026-07-27 behaviour: the cutoff relaxes whenever the pending JOB queue is short, which stopped ")
	TEXT("meaning 'there is spare capacity' the moment the consumer got fast). Bounds the O(tracked) exit scan, ")
	TEXT("which measured 66% of the streaming tick with records at 86,077 against a 39,020 settle."),
	ECVF_Default);

// S2-3 (docs/speculative-generation-plan.md Wave S2): keep an evicted chunk's
// POOL RANGE, hidden, so re-admitting it is a chunk-table write instead of a
// full re-mesh round trip. Under motion the ring boundaries oscillate and the
// same ground is re-meshed seconds after it was evicted.
//
// It is also the mechanism T4-1 parks speculatively generated terrain in.
//
// THE CAP IS A CORRECTNESS BOUNDARY, NOT A TUNING KNOB. A parked chunk still
// owns its pool range, its Allocations slot and its chunk-table entry, so it
// counts against pool capacity and the chunk-table floor exactly as a drawn
// chunk does. S1 measured what happens when residency is free to expand into
// available capacity: it does, all of it, and the pool then starts refusing
// DEMAND allocations while reading 61% free
// (docs/measurements/s1-close-2026-07-27.txt). Parked geometry has weaker back
// pressure than resident geometry -- nothing is asking for it.
//
// DEFAULT 12,000 SINCE 2026-07-28, AND THAT IS A MEASUREMENT.
//
//   profile   parking off      parking 12,000        hit
//   circle       893.7            906.5  (+1.4%)     90%
//   line        1040.6           1064.9  (+2.3%)     78%
//
// holes 0 and allocFail 0 on every leg, and the streaming tick does ~6% LESS
// work with it on (tickMs 26,890 vs 28,517 summed over a circle leg). Roughly
// 78-90% of adoptions skip a GPU mesh dispatch entirely -- work that appears in
// none of those numbers.
//
// TWO THINGS HAD TO BE FIXED BEFORE IT PAID, and both are worth knowing:
//
//   1. Park/unpark published eagerly, so every adopt traded one meshing round
//      trip for one FULL POOL PUBLICATION. After S1-1 the publication is the
//      expensive thing. That alone cost ~17%. They now defer
//      (MarkChunkTableDirtyDeferred) and the streaming tick's own batch carries
//      it.
//   2. Parking EVERY eviction scored an 8% hit rate and cost allocFail
//      0 -> 45,633 and holes 0 -> 1,804. On a traverse everything evicted by the
//      outer edge is behind the camera and never returns. Restricting to
//      RetainDir_Finer -- inner-boundary oscillation, the only eviction cause
//      that comes back -- took the hit rate to 78-90% with no other change.
//      WHERE you cache matters more than HOW MUCH.
//
// And it read as a 21% REGRESSION until chunksPerSec was fixed to count adopted
// chunks as loaded, because the adopt path never incremented TotalChunksLoaded.
// A metric that cannot see half a feature's output will reject the feature.
// docs/measurements/s1-close-2026-07-27.txt.
//
// Sizing: peak flight residency is ~50,900 chunks, so 12,000 parked sits inside
// both the 64M-quad pool and the 81,920 chunk-table floor with room to spare --
// verified by allocFail 0 on every leg. Parked chunks consume BOTH, so raising
// this is bounded by what demand residency leaves free.
// --- T4-1 speculative generation (Wave S3/S4) -------------------------------
//
// How far AHEAD of the anchor speculation aims, in seconds of travel. 0 = the
// predicted anchor collapses onto the true anchor and speculation is off.
//
// The lead is applied to a SMOOTHED VELOCITY VECTOR, not a heading, so a
// stationary anchor produces zero lead with no special case and a turning one
// produces a shorter lead while the EMA settles -- which is the conservative
// direction, since speculation aimed at where the camera WAS is pure waste.
//
// It feeds ONLY speculative enumeration. Admission, eviction and retention all
// stay on the true anchor: evicting against a forward-shifted centre would
// delete ground behind the camera that is still on screen.
// TASK #17 REPRODUCER. 1 = wrap RecomputeDesiredSet in a pool FScopedBatch.
// Known to take parking to exactly zero; kept only so the park-refusal counters
// can say WHY. Never ship at 1.
TAutoConsoleVariable<int32> CVarVoxelStreamBatchRecompute(
	TEXT("voxel.Stream.BatchRecompute"),
	0,
	TEXT("1 = open a pool batch scope around RecomputeDesiredSet. EXPERIMENT ONLY: this is the "
	     "configuration that zeroed parking (task #17). Read the park census refusal counters with it on."),
	ECVF_Default);

// DEFAULT 2.0 SINCE 2026-07-28 (owner decision). T4-1 speculative generation is
// ON.
//
// Measured across a 10-leg alternated sweep at 2560x1440: median flight-phase
// transient holes ~822 with it off against ~57 with it on -- a 93% reduction --
// at identical throughput (chunks/s spans 1061.5-1070.9 across every arm
// INCLUDING the controls), identical pool pressure (86.2% -> 86.8%), and, after
// the batch scope was widened over GpuMeshJobs->Tick(), identical game-thread
// tick cost. holes(final)=0 and allocFail=0 on all ten.
//
// WHY 2.0 AND NOT MORE. The effect SATURATES at 1 s: 1/2/4/8 s all land inside a
// single arm's pass-to-pass spread. The plan swept lead precisely because it
// assumed more lead buys more coverage; it does not, and the binding constraint
// beyond ~1 s is SpeculativeMaxInFlight rather than how far ahead the cone
// reaches. So the cheapest setting is also the best one, and anything that
// scales with lead should be sized for ~2 s rather than 8.
// Detail: docs/measurements/t41-first-result-2026-07-28.txt
// Chunks trimmed from EACH END of the speculative surface band. 0 = off.
// See the call site: the empties cluster at both ends and never the middle, and
// an over-trim costs hit rate rather than holes because demand still loads
// anything speculation skips.
// DEFAULT 1, SET BY THE SWEEP OF 2026-07-28.
//
//   trim   dispatched  adopted   dropEmpty (top/mid/bot)   flightHoles med
//     0      19,724      9,133   183 (64/0/119)                 42
//     1      18,839     14,765     3 ( 0/3/  0)                 38
//     2           0          0     0                            46
//
// 1 removes 98% of the zero-quad dispatches and raises adopted-per-dispatch from
// 46% to 78%. 2 switches speculation OFF entirely -- the surface band is only
// 2-3 chunks tall, so trimming two from each end leaves nothing -- and its
// flight holes are the worst of the three, which is the control confirming
// speculation is doing something.
//
// WHAT IT DID NOT DO, stated because it was predicted to: frame time did not
// move (p50 15.47 -> 15.41). Dispatches fell only 4%, because speculation is
// bounded by SpeculativeMaxInFlight rather than by candidate supply -- the freed
// budget refilled with USEFUL work instead of disappearing. So this is a
// productivity change, not a saving. Converting it into GPU time means lowering
// the in-flight cap now that each dispatch is worth more.
TAutoConsoleVariable<int32> CVarVoxelStreamSpeculativeZTrim(
	TEXT("voxel.Stream.SpeculativeZTrim"),
	1,
	TEXT("Chunks trimmed from each end of the speculative Z band. Cuts zero-quad speculative dispatches, "
	     "which cost real GPU (meshing is 21-28% of the GPU frame). Cannot cause holes -- demand still "
	     "loads whatever speculation skips."),
	ECVF_Default);

// DEFERRED CONSOLE EXEC, because -ExecCmds fires before the world exists.
//
// This is the third time that ordering has cost a run. voxel.Stream.PoolClobberTest
// logs "no live pool component" and exits 0; voxel.Stream.PoolGpuHideProbe did the
// same until it grew its own settle; and ProfileGPU issued via -ExecCmds captures
// FRAME 1 -- an empty world -- and reports it as a successful capture. In every
// case the command ran, produced output, and said nothing about the thing being
// measured.
//
// One general fix rather than a settle timer per command:
//   voxel.DeferExec <seconds> <command with args>
static void VoxelDeferExec(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogVoxelPerf, Error, TEXT("voxel.DeferExec: usage: voxel.DeferExec <seconds> <command...>"));
		return;
	}
	const float Delay = FMath::Max(0.f, float(FCString::Atof(*Args[0])));
	TArray<FString> Rest = Args;
	Rest.RemoveAt(0);
	const FString Command = FString::Join(Rest, TEXT(" "));

	UE_LOG(LogVoxelPerf, Warning, TEXT("voxel.DeferExec: will run in %.0f s: %s"), Delay, *Command);

	double Elapsed = 0.0;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Delay, Command, Elapsed](float Dt) mutable -> bool
	{
		Elapsed += double(Dt);
		if (Elapsed < double(Delay))
		{
			return true;
		}
		UE_LOG(LogVoxelPerf, Warning, TEXT("voxel.DeferExec: running now: %s"), *Command);
		// GEngine->Exec against the first world -- the same route the console
		// takes, so a deferred command behaves exactly like a typed one.
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() != nullptr) { World = Ctx.World(); break; }
			}
			GEngine->Exec(World, *Command);
		}
		return false;
	}), 0.0f);
}

static FAutoConsoleCommand GVoxelDeferExecCmd(
	TEXT("voxel.DeferExec"),
	TEXT("Run a console command after N seconds. For anything that needs a streamed world -- ProfileGPU, "
	     "stat dumpframe -- because -ExecCmds fires at startup and captures an empty one. "
	     "Usage: voxel.DeferExec <seconds> <command...>"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&VoxelDeferExec));

// Per-frame timing capture + the fast-vs-slow attribution report. Off by
// default: it is a diagnostic, and it holds one 28-byte sample per frame.
TAutoConsoleVariable<int32> CVarVoxelStreamFrameAttribution(
	TEXT("voxel.Stream.FrameAttribution"),
	0,
	TEXT("1 = sample frame/thread timings EVERY frame and report the fast-vs-slow component breakdown in "
	     "the 5s census. Unlike the Hitch frame line this can describe a TYPICAL frame, which is what "
	     "naming the frame-time floor and the tail both require."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelStreamVelocityLeadSec(
	TEXT("voxel.Stream.VelocityLeadSec"),
	2.0f,
	TEXT("Seconds of travel ahead of the anchor that speculative generation aims at. 0 = off. Applied to a ")
	TEXT("0.25s-EMA velocity vector and clamped by voxel.Stream.VelocityLeadMaxUU. Feeds speculation ONLY -- ")
	TEXT("admission, eviction and retention stay on the true anchor by design."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarVoxelStreamVelocityLeadMaxUU(
	TEXT("voxel.Stream.VelocityLeadMaxUU"),
	6000.0f,
	TEXT("Hard clamp on the predicted-anchor lead distance (UU). 6000 = 60 m, ~3s at the 20 m/s flight ")
	TEXT("profile. Stops a speed spike throwing the speculative cone somewhere the camera will never reach."),
	ECVF_Default);

// Chunks speculation may hold parked-but-unasked-for. SEPARATE from
// PoolParkMax, and that separation is deliberate: demand parking caches geometry
// that WAS wanted, speculation caches geometry that MIGHT be. They have
// different hit rates and different justifications, and sharing a cap would let
// speculation starve the demand cache it depends on.
//
// A parked speculative chunk holds a pool range AND a chunk-table entry, exactly
// like a resident one. S1 measured what happens when residency is free to expand
// into spare capacity -- it does, all of it, and the pool then refuses DEMAND
// allocations while reading half empty. Speculative geometry has no natural back
// pressure at all, so this cap is a correctness boundary.
TAutoConsoleVariable<int32> CVarVoxelStreamSpeculativeMaxParked(
	TEXT("voxel.Stream.SpeculativeMaxParked"),
	4000,
	TEXT("Max chunks held parked from SPECULATIVE generation, separate from voxel.Stream.PoolParkMax. ")
	TEXT("Speculative geometry has no back pressure -- nothing asked for it -- so this bounds real pool and ")
	TEXT("chunk-table capacity. Only takes effect while voxel.Stream.VelocityLeadSec > 0."),
	ECVF_Default);

// Speculative jobs in flight, carved out of the fork's 256-slot budget.
//
// Kept small on purpose. Demand work is submitted FIRST every tick and
// speculation only gets what is left, but a large speculative depth would still
// sit in the manager's strict-FIFO queue ahead of the next tick's demand jobs.
// Small depth plus submit-last is what makes starvation structurally impossible
// rather than merely unlikely.
// 16 SINCE 2026-07-28, swept with SpeculativeZTrim 1:
//
//   cap   dispatched   adopted        p50      flightHoles med
//    32     18,834     14,948 (79%)  15.31          36
//    16     17,540     14,979 (85%)  15.34          38
//     8     10,136      9,501 (94%)  15.23          46
//
// 16 is free: identical adopted geometry to 32 for 7% fewer dispatches, because
// the trim made each dispatch more productive. 8 is too far -- 37% less adopted
// and holes visibly worse.
//
// AND THE FRAME DID NOT MOVE. Cutting dispatches 46% (32 -> 8) moved p50 by
// 0.08 ms. See the note at CVarVoxelStreamSpeculativeZTrim: reducing GPU MESHING
// work does not reduce frame time on this renderer, which is why 16 is taken for
// the free efficiency rather than as a performance fix.
TAutoConsoleVariable<int32> CVarVoxelStreamSpeculativeMaxInFlight(
	TEXT("voxel.Stream.SpeculativeMaxInFlight"),
	16,
	TEXT("Max speculative mesh jobs in flight, carved out of voxel.Stream.GpuMeshInFlight (256). Small by ")
	TEXT("design: the job manager's queue is strict FIFO, so speculative depth delays the NEXT tick's demand ")
	TEXT("work even though demand is submitted first."),
	ECVF_Default);

// WHY SPECULATION ADOPTED NOTHING UNDER THE MARCHER CONFIG, and what this
// switch does about it.
//
// Under voxel.Terrain.RetireQuads (the marcher shipping config,
// -VoxelRetireQuads=1) every GPU mesh job is brick-only: the manager sets
// bQuadMesh=false and bDirectToPool=false at Submit, so every delivery carries
// NumQuads == 0 and no GPU quad payload BY CONFIGURATION. ParkSpeculativeResult
// then asks its "did this job mesh anything" question of the QUAD product
// (!GpuQuads.IsValid() || NumQuads == 0) and classifies every result as
// dropEmpty. Measured on final-shipped-state.log (2026-08-22): cumulative
// dispatched=9,804 adopted=0, dropEmpty at 100%, with a MID-HEAVY band
// distribution (top=17 mid=190 bot=157 in one window) -- and the trim sweep of
// 2026-07-28 measured that GENUINE empties never sit mid-band (top/bot only,
// mid=0). Mid-band "empties" are surface chunks with real geometry whose quads
// were retired out from under the classifier.
//
// The waste is double: the bricks those jobs packed were ALREADY published
// resident into the global brick pool by the manager's Deliver (bBrickResident
// default 1) -- the march index sinks every pool write, so the terrain even
// RENDERS -- but nothing records it, so when admission later wants the chunk it
// dispatches a second full GPU job for ground the pool already holds.
//
// 1 = under quad retirement, park a brick-backed marker (no quad-pool slot)
// for each speculative delivery whose bricks the brick pool confirms resident,
// so admission ADOPTS the chunk (a table write) instead of re-meshing it.
// 0 (default) = today's behaviour: the result is dropped; only the census
// wording changes (dropBrickOnly, counted apart from dropEmpty, because the
// old label was false).
TAutoConsoleVariable<int32> CVarVoxelStreamSpeculativeParkBricks(
	TEXT("voxel.Stream.SpeculativeParkBricks"),
	0,
	TEXT("1 = under voxel.Terrain.RetireQuads, park speculative GPU results as brick-backed entries so ")
	TEXT("admission adopts them instead of re-dispatching a job for terrain the brick pool already holds. ")
	TEXT("0 (default) = drop them exactly as before (counted as dropBrickOnly, not dropEmpty). ")
	TEXT("No effect when quads are not retired."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamPoolParkMax(
	TEXT("voxel.Stream.PoolParkMax"),
	12000,
	TEXT("Max chunks kept as hidden-but-allocated pool ranges after eviction, so re-admission is a table ")
	TEXT("write rather than a re-mesh. DEFAULT 12,000 since 2026-07-28: +1.4% (circle) / +2.3% (line) ")
	TEXT("placed-chunks/s with holes 0, allocFail 0, a 78-90% adoption hit rate and ~6% less tick work. ")
	TEXT("0 = off (evict frees immediately) as the A/B control. Parked chunks consume pool quads AND ")
	TEXT("chunk-table entries, so raising this is bounded by what demand residency leaves free."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxRemeshesPerFrame(
	TEXT("voxel.Stream.MaxRemeshesPerFrame"),
	8,
	TEXT("Max game-thread overlay-aware edit re-meshes (FVoxelWorldImpl::DrainGameThreadMesh) per frame. History: ")
	TEXT("constant 4 -> 2 (hitch isolation) -> 8 (2026-07-24 streaming-speed pass, raised alongside MaxAppliesPerFrame ")
	TEXT("so edited-chunk loads keep pace with the faster clean-chunk apply path)."),
	ECVF_Default);

// 24 -> 256 (2026-07-27, S1). THE MOST EXPENSIVE-TO-DIAGNOSE NUMBER IN THIS FILE
// SO FAR, and the lesson generalises to every other budget here.
//
// Its own doc already said unloads must keep pace with the apply rate. When
// batched publication took the apply rate from ~260 to ~630 chunks/s, 24 stopped
// keeping pace -- ~684 unloads/s against ~630 loads/s is marginally under water,
// so the unload queue grew without bound (pendingUnload peaked at 75,925) and
// residency with it (80,716 live chunks against an 81,920 table floor).
//
// It did not present as an unload problem. It presented as ALLOCATOR
// FRAGMENTATION: 26,763-77,290 refused allocations, 16,903 free runs, and a 72x
// collapse in largest-free-run at equal total free -- textbook first-fit
// pathology, and the obvious response was to build a compacting defragmenter
// (T3-6). A chronically over-full pool fragments; the allocator was the
// messenger. Raising this took allocFail to 0 and converged holes 257 -> 0.
//
// SO: EVERY PER-FRAME BUDGET IN THIS SUBSYSTEM ENCODES AN ASSUMPTION ABOUT
// THROUGHPUT. MaxAppliesPerFrame, MaxRemeshesPerFrame, kMaxResultDrainsPerFrame,
// kMaxUnloadPopsPerFrame, GpuMeshInFlight, MeshBatchCap, MeshHarvestCap -- any
// change that moves throughput must re-sweep the ones downstream of it, and a
// saturated budget can surface as a bug in something else entirely.
// docs/measurements/s1-close-2026-07-27.txt.
TAutoConsoleVariable<int32> CVarVoxelStreamMaxUnloadsPerFrame(
	TEXT("voxel.Stream.MaxUnloadsPerFrame"),
	256,
	TEXT("Max chunk-component unload events (FVoxelWorldImpl::DrainUnloads -- pool-park, or DestroyComponent once the ")
	TEXT("pool is at voxel.Stream.ComponentPoolMax) per frame. History: constant 4 -> 2 (hitch isolation) -> 24 ")
	TEXT("(2026-07-24 streaming-speed pass) -> 256 (2026-07-27, S1): at the batched apply rate 24 could not keep pace, ")
	TEXT("so residency ballooned to 80,716 chunks and the pool began REFUSING allocations -- which looked exactly like ")
	TEXT("allocator fragmentation. 256 takes allocFail to 0 and converged holes to 0. See the source comment."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamComponentPoolMax(
	TEXT("voxel.Stream.ComponentPoolMax"),
	512,
	TEXT("M1 hitch-gap wave (docs/status.md M1 gate row): max UVoxelChunkComponent instances parked (hidden, quads ")
	TEXT("cleared, no scene proxy, still registered/attached) in the reuse pool after leaving the desired set, ")
	TEXT("instead of DestroyComponent()'d immediately. A pending first-load prefers a pooled component -- see ")
	TEXT("FVoxelWorldImpl::AcquireChunkComponent -- over NewObject+RegisterComponent, avoiding the render-thread ")
	TEXT("AddPrimitive/RemovePrimitive churn the hitch-attribution instrumentation pinned as the dominant cost at ")
	TEXT("ring-boundary crossings. Unloads past the cap fall back to DestroyComponent() (no unbounded growth)."),
	ECVF_Default);

// --- Tile-source log sniffer (see VoxelDebug.h FTileSourceStatus) -----------
//
// Parses the two lines MakeTileSampler (VoxelWorldSubsystem.cpp) already
// emits, because the sampler choice survives nowhere else this module can
// reach. Serialize() can be called from any thread and from inside a log
// call, so it does the strict minimum: two substring tests on the fast path,
// no logging of its own, and a plain FCriticalSection around the cached POD.
FCriticalSection GTileSourceLock;
VoxelDebug::FTileSourceStatus GTileSourceStatus;

// Reads "<Key>=<int>" out of Line, starting the search at/after the key.
// Returns false (Out untouched) if the key is absent.
bool ParseTaggedInt(const FString& Line, const TCHAR* Key, int32& Out)
{
	const int32 KeyIdx = Line.Find(Key, ESearchCase::CaseSensitive);
	if (KeyIdx == INDEX_NONE)
	{
		return false;
	}
	Out = FCString::Atoi(*Line + KeyIdx + FCString::Strlen(Key));
	return true;
}

class FVoxelTileSourceSniffer final : public FOutputDevice
{
public:
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const class FName&) override
	{
		if (!V || FCString::Strstr(V, TEXT("Voxel tile grid: ")) == nullptr)
		{
			return;
		}
		const FString Line(V);

		// "Voxel tile grid: dir=%s loaded=%d rejected=%d seed=%llu scale=%d"
		int32 Loaded = 0;
		if (ParseTaggedInt(Line, TEXT("loaded="), Loaded))
		{
			int32 Rejected = 0;
			ParseTaggedInt(Line, TEXT("rejected="), Rejected);

			FString Dir;
			const int32 DirIdx = Line.Find(TEXT("dir="), ESearchCase::CaseSensitive);
			if (DirIdx != INDEX_NONE)
			{
				const int32 DirStart = DirIdx + 4;
				const int32 LoadedIdx = Line.Find(TEXT(" loaded="), ESearchCase::CaseSensitive);
				Dir = (LoadedIdx > DirStart) ? Line.Mid(DirStart, LoadedIdx - DirStart) : Line.Mid(DirStart);
			}

			FScopeLock Lock(&GTileSourceLock);
			GTileSourceStatus.bKnown = true;
			GTileSourceStatus.TilesLoaded = Loaded;
			GTileSourceStatus.TilesRejected = Rejected;
			GTileSourceStatus.TileDir = Dir;
			// Mirrors MakeTileSampler's own rule exactly: Loaded == 0 falls
			// back to the synthetic sampler rather than booting an empty world.
			GTileSourceStatus.bUsingRealTiles = (Loaded > 0);
			return;
		}

		// "Voxel tile grid: loaded tile coords bounding box x=[%d,%d] y=[%d,%d]"
		int32 MinX = 0, MinY = 0;
		if (ParseTaggedInt(Line, TEXT("x=["), MinX) && ParseTaggedInt(Line, TEXT("y=["), MinY))
		{
			// The second number of each pair follows the comma.
			int32 MaxX = MinX, MaxY = MinY;
			const int32 XIdx = Line.Find(TEXT("x=["), ESearchCase::CaseSensitive);
			const int32 YIdx = Line.Find(TEXT("y=["), ESearchCase::CaseSensitive);
			const int32 XComma = Line.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, XIdx);
			const int32 YComma = Line.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, YIdx);
			if (XComma != INDEX_NONE) { MaxX = FCString::Atoi(*Line + XComma + 1); }
			if (YComma != INDEX_NONE) { MaxY = FCString::Atoi(*Line + YComma + 1); }

			FScopeLock Lock(&GTileSourceLock);
			GTileSourceStatus.bBoxKnown = true;
			GTileSourceStatus.MinTileX = MinX;
			GTileSourceStatus.MaxTileX = MaxX;
			GTileSourceStatus.MinTileY = MinY;
			GTileSourceStatus.MaxTileY = MaxY;
		}
	}
};

FVoxelTileSourceSniffer GTileSourceSniffer;

// Registered at FileSystemReady -- comfortably before any UWorld (and so
// before UVoxelWorldSubsystem::Initialize runs MakeTileSampler), and if this
// module's static init happens to run after that phase the helper simply
// invokes the lambda immediately. Never unregistered: GLog outlives the
// module in every configuration this project ships, and the device is a
// file-scope object with no destructor work.
FDelayedAutoRegisterHelper GTileSourceSnifferRegistrar(EDelayedRegisterRunPhase::FileSystemReady,
                                                        []()
                                                        {
	                                                        if (GLog)
	                                                        {
		                                                        GLog->AddOutputDevice(&GTileSourceSniffer);
	                                                        }
                                                        });
} // namespace

int32 VoxelDebug::GetDebugMode()
{
	return CVarVoxelDebug.GetValueOnAnyThread();
}

void VoxelDebug::SetDebugMode(int32 NewMode)
{
	CVarVoxelDebug->Set(FMath::Clamp(NewMode, 0, 2), ECVF_SetByCode);
}

void VoxelDebug::CycleDebugMode()
{
	SetDebugMode((GetDebugMode() + 1) % 3);
}

bool VoxelDebug::IsChunkStatesEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugChunkStates.GetValueOnAnyThread();
}

bool VoxelDebug::IsBoundsEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugBounds.GetValueOnAnyThread();
}

bool VoxelDebug::IsRingsEnabled()
{
	return GetDebugMode() >= 2 && CVarVoxelDebugRings.GetValueOnAnyThread();
}

int32 VoxelDebug::GetWaterBucketFill()
{
	return CVarVoxelWaterBucketFill.GetValueOnGameThread();
}

bool VoxelDebug::IsPlayerBoxEnabled()
{
	// NO mode>=2 gate, unlike every other Is*Enabled above. That is the whole
	// point: the player volume is a scale reference for ordinary play-testing,
	// so it must not require two F3 presses to appear. Because the gate is
	// absent, this doubles as the raw cvar read and there is no separate
	// Get*CVar companion for the overlay to call.
	return CVarVoxelDebugPlayerBox.GetValueOnAnyThread();
}

void VoxelDebug::SetPlayerBoxEnabled(bool bEnabled)
{
	CVarVoxelDebugPlayerBox->Set(bEnabled, ECVF_SetByCode);
}

void VoxelDebug::SetRingsEnabled(bool bEnabled)
{
	CVarVoxelDebugRings->Set(bEnabled, ECVF_SetByCode);
}

void VoxelDebug::SetChunkStatesEnabled(bool bEnabled)
{
	CVarVoxelDebugChunkStates->Set(bEnabled, ECVF_SetByCode);
}

void VoxelDebug::SetBoundsEnabled(bool bEnabled)
{
	CVarVoxelDebugBounds->Set(bEnabled, ECVF_SetByCode);
}

bool VoxelDebug::GetChunkStatesCVar()
{
	return CVarVoxelDebugChunkStates.GetValueOnAnyThread();
}

bool VoxelDebug::GetBoundsCVar()
{
	return CVarVoxelDebugBounds.GetValueOnAnyThread();
}

bool VoxelDebug::GetRingsCVar()
{
	return CVarVoxelDebugRings.GetValueOnAnyThread();
}

VoxelDebug::FTileSourceStatus VoxelDebug::GetTileSourceStatus()
{
	FScopeLock Lock(&GTileSourceLock);
	return GTileSourceStatus;
}

void VoxelDebug::WorldToTileCoords(double WorldXUU, double WorldYUU, int32& OutTileX, int32& OutTileY, int64& OutPixelX,
                                    int64& OutPixelY)
{
	// vxc::tilePixelSizeMm: 30000 mm/px at scale 1, 11250 at scale 8; anything
	// else is unsupported and MakeTileSampler rejects it, so fall back to
	// scale 1 here rather than dividing by zero.
	int32 TileScale = 1;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelTileScale="), TileScale);
	const double PixelSizeMm = (TileScale == 8) ? 11250.0 : 30000.0;

	// 1 UU == 1 cm == 10 mm (VoxelCoords.h).
	OutPixelX = (int64)FMath::FloorToDouble(WorldXUU * 10.0 / PixelSizeMm);
	OutPixelY = (int64)FMath::FloorToDouble(WorldYUU * 10.0 / PixelSizeMm);

	// vxc::TileData::kTileSize == 512 px per tile edge; tile = floorDiv(px, 512).
	constexpr double kTileSizePx = 512.0;
	OutTileX = (int32)FMath::FloorToDouble(double(OutPixelX) / kTileSizePx);
	OutTileY = (int32)FMath::FloorToDouble(double(OutPixelY) / kTileSizePx);
}

bool VoxelDebug::IsUndergroundVeilEnabledForRun()
{
	// Same parse AVoxelClipmapActor::BeginPlay does (-VoxelUndergroundVeil=0
	// disables); absent switch means enabled.
	int32 VeilFlag = 1;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelUndergroundVeil="), VeilFlag);
	return VeilFlag != 0;
}

bool VoxelDebug::IsUnattendedFixtureRun()
{
	return FApp::IsUnattended();
}

FLinearColor VoxelDebug::RingLevelTint(int32 Level)
{
	// m2-plan.md first implementation wave item 4: "R0 green, R1 yellow, R2
	// orange, R3 red, R4 magenta."
	static const FLinearColor kTints[VoxelCoords::kNumLevels] = {
		FLinearColor(0.1f, 0.9f, 0.15f, 1.0f),  // R0 green
		FLinearColor(0.95f, 0.9f, 0.1f, 1.0f),  // R1 yellow
		FLinearColor(1.0f, 0.55f, 0.05f, 1.0f), // R2 orange
		FLinearColor(0.9f, 0.1f, 0.1f, 1.0f),   // R3 red
		FLinearColor(0.85f, 0.1f, 0.85f, 1.0f), // R4 magenta
		FLinearColor(0.25f, 0.5f, 1.0f, 1.0f),  // R5 blue (2 km cascade edge)
	};
	static_assert(UE_ARRAY_COUNT(kTints) == VoxelCoords::kNumLevels,
	              "kTints must have one entry per level (a short list yields invisible transparent-black debug rings)");
	return kTints[FMath::Clamp(Level, 0, VoxelCoords::kNumLevels - 1)];
}

FLinearColor VoxelDebug::HeightmapBandTint()
{
	// m2-plan.md "Debug" row / debug-tooling-plan.md palette: "heightmap
	// band cyan".
	return FLinearColor(0.1f, 0.85f, 0.95f, 1.0f);
}

int64 VoxelDebug::GetMipCacheBudgetBytes()
{
	return int64(CVarVoxelMipCacheBudgetMB.GetValueOnAnyThread()) * 1024 * 1024;
}

void VoxelDebug::SetMipCacheBudgetMB(int32 NewBudgetMB)
{
	CVarVoxelMipCacheBudgetMB->Set(NewBudgetMB, ECVF_SetByCode);
}

int32 VoxelDebug::GetServerMaxIntentsPerSec()
{
	return CVarVoxelServerMaxIntentsPerSec.GetValueOnGameThread();
}

float VoxelDebug::GetServerMaxCarveRadiusUU()
{
	return CVarVoxelServerMaxCarveRadiusUU.GetValueOnGameThread();
}

int32 VoxelDebug::GetWaterMaxActiveBricks()
{
	return CVarVoxelWaterMaxActiveBricks.GetValueOnAnyThread();
}

int32 VoxelDebug::GetStreamMaxAppliesPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxAppliesPerFrame.GetValueOnGameThread());
}

float VoxelDebug::GetStreamApplyBudgetMs()
{
	return FMath::Max(0.f, CVarVoxelStreamApplyBudgetMs.GetValueOnGameThread());
}

float VoxelDebug::GetStreamLodRetentionMs()
{
	return FMath::Max(0.f, CVarVoxelStreamLodRetentionMs.GetValueOnGameThread());
}

bool VoxelDebug::GetStreamLogRetention()
{
	return CVarVoxelStreamLogRetention.GetValueOnGameThread() != 0;
}

int32 VoxelDebug::GetStreamApplyStageStats()
{
	return CVarVoxelStreamApplyStageStats.GetValueOnGameThread();
}

bool VoxelDebug::GetStreamCoverageVerify()
{
	return CVarVoxelStreamCoverageVerify.GetValueOnGameThread() != 0;
}

bool VoxelDebug::GetStreamLatencyStats()
{
	return CVarVoxelStreamLatencyStats.GetValueOnGameThread() != 0;
}

bool VoxelDebug::GetRenderCastShadow()
{
	return CVarVoxelRenderCastShadow.GetValueOnGameThread();
}

bool VoxelDebug::GetStreamGpu()
{
	return CVarVoxelStreamGpu.GetValueOnGameThread();
}

bool VoxelDebug::GetWaterGpu()
{
	return CVarVoxelWaterGpu.GetValueOnGameThread();
}

bool VoxelDebug::GetWaterSwe()
{
	return CVarVoxelWaterSwe.GetValueOnGameThread();
}

bool VoxelDebug::GetWaterRivers()
{
	return CVarVoxelWaterRivers.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamJobsInFlightPerCore()
{
	// -VoxelJobsInFlightPerCore=<n> wins over the cvar, resolved once at first
	// use. The cvar alone cannot sweep this honestly: it is only settable
	// through -ExecCmds, which lands AFTER streaming has begun, so the first
	// seconds of every run -- the cascade fill, where the pool is busiest and
	// contention is at its worst -- would run at the default whatever the sweep
	// asked for. Same reason -VoxelCoarseMinLevel and -VoxelPendingJobCap are
	// command-line switches (VoxelWorldSubsystem.cpp, namespace
	// VoxelStreamAdmission). 0/absent = use the cvar.
	static const int32 CommandLineOverride = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelJobsInFlightPerCore="), Value);
		return Value;
	}();
	const int32 Requested =
		CommandLineOverride > 0 ? CommandLineOverride : CVarVoxelStreamJobsInFlightPerCore.GetValueOnGameThread();
	return FMath::Clamp(Requested, 1, 64);
}

int32 VoxelDebug::GetStreamDispatchAfterDrain()
{
	return CVarVoxelStreamDispatchAfterDrain.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamRingFloorCpuOnly()
{
	// -VoxelRingFloorCpuOnly=<0|1> overrides the cvar and WINS, the
	// -VoxelAdmissionBandSkip idiom: an -ExecCmds cvar lands only after the
	// world has begun streaming, and the floor test steers dispatch order
	// from the very first cold-fill tick -- a leg that flips the cvar via
	// -ExecCmds would run its first seconds on the blended test and call the
	// contaminated result an arm. The cvar remains for interactive A/B in a
	// live session, where the first seconds are long gone anyway.
	static const int32 CmdLineOverride = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelRingFloorCpuOnly="), Value);
		return Value;
	}();
	return CmdLineOverride >= 0 ? CmdLineOverride : CVarVoxelStreamRingFloorCpuOnly.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamAdmissionRecordCap()
{
	return FMath::Max(0, CVarVoxelStreamAdmissionRecordCap.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamBatchRecompute()
{
	return CVarVoxelStreamBatchRecompute.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamSpeculativeZTrim()
{
	return FMath::Max(0, CVarVoxelStreamSpeculativeZTrim.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamFrameAttribution()
{
	return CVarVoxelStreamFrameAttribution.GetValueOnGameThread();
}

float VoxelDebug::GetStreamVelocityLeadSec()
{
	return FMath::Max(0.f, CVarVoxelStreamVelocityLeadSec.GetValueOnGameThread());
}

float VoxelDebug::GetStreamVelocityLeadMaxUU()
{
	return FMath::Max(0.f, CVarVoxelStreamVelocityLeadMaxUU.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamSpeculativeMaxParked()
{
	return FMath::Max(0, CVarVoxelStreamSpeculativeMaxParked.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamSpeculativeMaxInFlight()
{
	return FMath::Max(0, CVarVoxelStreamSpeculativeMaxInFlight.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamSpeculativeParkBricks()
{
	return CVarVoxelStreamSpeculativeParkBricks.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamPoolParkMax()
{
	return FMath::Max(0, CVarVoxelStreamPoolParkMax.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamMaxRemeshesPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxRemeshesPerFrame.GetValueOnGameThread());
}

int32 VoxelDebug::GetStreamMaxUnloadsPerFrame()
{
	return FMath::Max(1, CVarVoxelStreamMaxUnloadsPerFrame.GetValueOnGameThread());
}

int32 VoxelDebug::GetComponentPoolMax()
{
	return FMath::Max(0, CVarVoxelStreamComponentPoolMax.GetValueOnGameThread());
}

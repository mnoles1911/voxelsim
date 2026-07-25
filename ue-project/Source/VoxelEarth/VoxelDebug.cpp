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

TAutoConsoleVariable<int32> CVarVoxelStreamMaxAppliesPerFrame(
	TEXT("voxel.Stream.MaxAppliesPerFrame"),
	64,
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
	5000.0f,
	TEXT("Load-before-unload SAFETY CAP (2026-07-24 streaming-speed pass). When a VISIBLE chunk is evicted because a ")
	TEXT("different LOD ring took over its footprint (toward -> finer, away -> coarser), it is kept drawn as a stand-in ")
	TEXT("until its replacement LOD is actually on screen -- COVERAGE-based release (ReplacementCovered vs ChunkRecords), ")
	TEXT("not a fixed timer, so there is no rolling ring of holes where a timer would expire mid-transition. This value ")
	TEXT("is only the backstop: a footprint that never gets covered (e.g. a coastal quarter that is all ocean) is parked ")
	TEXT("after this many ms so resident chunks cannot grow unbounded. Cost: brief coarse+fine double-draw at the ")
	TEXT("boundary until coverage (minor shimmer, no hole). 0 disables retention entirely (immediate unload = holes)."),
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

TAutoConsoleVariable<int32> CVarVoxelStreamGpuMaxChunks(
	TEXT("voxel.Stream.GPUMaxChunks"),
	0,
	TEXT("Debug bisection: cap chunks admitted to the ADR-0006 GPU pool. 0 = unlimited. Exists so the streamed ")
	TEXT("path can be run at the same small scale voxel.GPU.SpawnPool is known-good at, separating 'CPU-meshed ")
	TEXT("quads through the pool are wrong' from 'the pool does not like a multi-million-quad draw'."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelStreamGpu(
	TEXT("voxel.Stream.GPU"),
	false,
	TEXT("Route chunk geometry through the ADR-0006 GPU pool (ONE primitive, ONE draw) instead of one scene ")
	TEXT("component per chunk. Default false. Set before the world streams in; toggling mid-flight leaves ")
	TEXT("already-resident chunks on whichever path loaded them."),
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
// Default 2 deliberately preserves the pre-change behaviour exactly, so this
// ships inert and the A/B runs on one binary.
//
// Costs to watch when raising it: more in-flight jobs means more results landing
// per frame (bounded by voxel.Stream.MaxAppliesPerFrame / ApplyBudgetMs), more
// peak memory in flight, and more STALE results (a chunk evicted while its job
// runs is discarded -- currently 0.0%, watch the "job flow" census).
TAutoConsoleVariable<int32> CVarVoxelStreamJobsInFlightPerCore(
	TEXT("voxel.Stream.JobsInFlightPerCore"),
	2,
	TEXT("Worker jobs allowed in flight, as a multiple of logical cores (24 at the default 2 on a 12-thread box). ")
	TEXT("DispatchJobs refills to this cap once per frame, so with short jobs (R0 p50 ~1.3ms) and a ~15ms frame the ")
	TEXT("slots idle most of the frame -- measured ~9% worker utilisation and ~6,800 chunks per 5s evicted before ")
	TEXT("ever being dispatched. Raise to keep the task graph fed across frames. Watch stale%% in the job-flow census."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxRemeshesPerFrame(
	TEXT("voxel.Stream.MaxRemeshesPerFrame"),
	8,
	TEXT("Max game-thread overlay-aware edit re-meshes (FVoxelWorldImpl::DrainGameThreadMesh) per frame. History: ")
	TEXT("constant 4 -> 2 (hitch isolation) -> 8 (2026-07-24 streaming-speed pass, raised alongside MaxAppliesPerFrame ")
	TEXT("so edited-chunk loads keep pace with the faster clean-chunk apply path)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelStreamMaxUnloadsPerFrame(
	TEXT("voxel.Stream.MaxUnloadsPerFrame"),
	24,
	TEXT("Max chunk-component unload events (FVoxelWorldImpl::DrainUnloads -- pool-park, or DestroyComponent once the ")
	TEXT("pool is at voxel.Stream.ComponentPoolMax) per frame. History: constant 4 -> 2 (hitch isolation) -> 24 ")
	TEXT("(2026-07-24 streaming-speed pass): unloads must keep pace with the raised apply rate or superseded coarse ")
	TEXT("chunks pile up resident (measured R0 bloating ~4x its desired size, which itself loads the render thread)."),
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

bool VoxelDebug::GetRenderCastShadow()
{
	return CVarVoxelRenderCastShadow.GetValueOnGameThread();
}

bool VoxelDebug::GetStreamGpu()
{
	return CVarVoxelStreamGpu.GetValueOnGameThread();
}

int32 VoxelDebug::GetStreamJobsInFlightPerCore()
{
	return FMath::Clamp(CVarVoxelStreamJobsInFlightPerCore.GetValueOnGameThread(), 1, 64);
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

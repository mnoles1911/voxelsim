// VoxelResidencyGpu.cpp -- T4-2 "GPU-resident residency": manager for the
// compute-pass desired-set walks. See VoxelResidencyGpu.h for the contract
// and VoxelResidencyScan.usf for the data structures; this file is plumbing
// plus the comparator.
//
// STRUCTURE OF THIS FILE
//   1. Constants + key packing/hash (must mirror the shader exactly).
//   2. The five global shader classes.
//   3. FImpl: staging, ledgers, in-flight ring, dispatch, poll, consume,
//      comparator, log.
//
// THREADING. Public API game-thread only. The render thread touches: the
// dispatch command (builds+executes one RDG graph), the poll command (locks
// ready readbacks into the seq's arrays, then publishes with a release
// store), and the release command (frees readbacks). Seq objects cross
// threads as TSharedPtr, the mesh-job manager's pattern.

#include "VoxelResidencyGpu.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
// THE TAIL-GROUP SCOPE, one line per ENQUEUE_RENDER_COMMAND body. See
// VoxelRenderFrame.h: these bodies run on the RENDER THREAD BETWEEN scene
// renders -- i.e. in the tail bucket -- and tailMs is where the parked->moving
// delta is expected to live. Costs one compare on a latched int unless the leg
// is run with -VoxelRenderFrame=2.
#include "VoxelRenderFrame.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"

#include <atomic>
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RHIBreadcrumbs.h"                         // RHI_BREADCRUMB_EVENT_STAT (5.8 spelling)

// ---------------------------------------------------------------------------
// STREAMING-SIDE GPU STATS -- the split for the unattributed +5.47 ms.
//
// THE SPELLING MATTERS AND THREE OF THEM ARE DEAD. In 5.8 SCOPED_GPU_STAT,
// RDG_GPU_STAT_SCOPE and RDG_RHI_GPU_STAT_SCOPE are UE_DEPRECATED_MACRO and
// expand to NOTHING -- they compile, they look armed, and they measure zero.
// RHI_BREADCRUMB_EVENT_STAT (RHIBreadcrumbs.h:1302) and RDG_EVENT_SCOPE_STAT
// (RenderGraphEvent.h:480) are the live spellings. These sites use the first;
// see the note at each one for why the RDG form cannot be used here.
//
// WHAT THE COLUMN IS. Every DECLARE_GPU_STAT_NAMED stat has its per-frame
// EXCLUSIVE (Busy + Wait) milliseconds written to the CSV profiler once per
// frame, end-of-pipe, as GPU/<StatName> (GPUProfiler.cpp:1065-1067). The
// engine also emits GPU/Unaccounted -- queue time inside NO stat scope
// (GPUProfiler.cpp:800, accumulated at :1556 only when the stat stack is
// empty). So the GPU/ columns of one row SUM to the frame's queue busy time
// and the decomposition checks itself.
//
// KEEP THESE SIBLINGS, NEVER NESTED. Exclusive time is charged to the
// innermost stat only; nesting would silently move a term and make "which
// number am I reading" a live question. Each scope below wraps one standalone
// FRDGBuilder in one ENQUEUE_RENDER_COMMAND, so they are siblings by
// construction.
//
// ARMING: -csvGpuStats on the command line (r.GPUCsvStatsEnabled defaults 0 --
// with it off the GPU/ columns are simply ABSENT, no error), plus
// `CsvProfile FRAMES=N`. A CSV with no GPU/ column measured nothing.
// ---------------------------------------------------------------------------
DECLARE_GPU_STAT_NAMED(VoxelStreamResidency, TEXT("VoxelStreamResidency"));

DEFINE_LOG_CATEGORY_STATIC(LogVoxelResidGpu, Log, All);

namespace VoxelResidency
{
	// ---- mirrors of the shader's constants (VoxelResidencyScan.usf) --------
	constexpr uint32 kDimXY = 128;
	constexpr uint32 kDimZ = 128;
	constexpr uint32 kMask = 127;
	constexpr uint32 kCellsPerLevel = kDimXY * kDimXY * kDimZ;
	constexpr uint32 kMaxLevels = 8;

	constexpr uint32 kAdmitCap = 65536;
	constexpr uint32 kEvictCap = 32768;
	constexpr uint32 kResurrectCap = 8192;
	constexpr uint32 kColdCap = 16384;
	constexpr uint32 kOrphanCap = 4096;

	constexpr uint32 kCtrAdmit = 0;
	constexpr uint32 kCtrEvict = 1;
	constexpr uint32 kCtrResurrect = 2;
	constexpr uint32 kCtrCold = 3;
	constexpr uint32 kCtrOrphan = 4;
	constexpr uint32 kCtrFbApplied = 5;
	constexpr uint32 kCtrFbDropped = 6;
	constexpr uint32 kCtrCollision = 7;
	constexpr uint32 kCtrAuditCount = 8;
	constexpr uint32 kCtrAuditXorLo = 9;
	constexpr uint32 kCtrAuditXorHi = 10;
	constexpr uint32 kCtrCandVisited = 11;
	constexpr uint32 kCtrCutoffRej = 12;
	constexpr uint32 kCtrZVisited = 13;
	constexpr uint32 kCtrOverflow = 14;
	constexpr uint32 kCtrExitVisited = 15;
	constexpr uint32 kCtrBudgetRej = 16;
	constexpr uint32 kCtrHistTotal = 17;
	constexpr uint32 kNumCounters = 18;

	// Distance-histogram resolution for the GPU admission budget. Bins are
	// equal-AREA (they partition SQUARED distance), so a uniform annulus fills
	// them evenly and the resolved threshold lands within one bin's occupancy
	// of the budget. 1024 bins is 4 KB per level -- 24 KB of transient buffer
	// for the whole cascade, cleared per dispatch.
	constexpr uint32 kHistBins = 1024;

	constexpr uint32 kOpInsert = 1;
	constexpr uint32 kOpRemove = 2;
	constexpr uint32 kOpMarkPending = 3;
	constexpr uint32 kOpClearPending = 4;

	// Audit cadence, in dispatches. Cheap (one reduce over the grid) but not
	// free; every 32nd recompute is ~every 3.5 s of fast flight.
	constexpr uint32 kAuditEvery = 32;
	// In-flight dispatch ring. A full ring skips the dispatch and counts it --
	// visible on the log line, never silent.
	constexpr int32 kMaxInFlight = 4;
	// Decision-ledger ring: sequences older than this are retired uncompared
	// (counted) when their delta still has not arrived.
	constexpr int32 kMaxLedger = 8;

	// Relative epsilon for the boundary classifier: a float/double mismatch
	// can only live within float rounding of a threshold; 1e-4 relative on
	// SQUARED quantities is ~500x float's worst case here, so anything past
	// it is a real disagreement, not precision.
	constexpr double kBoundaryRelEps = 1e-4;

	// ---- key packing: 4-bit level + three 20-bit two's-complement coords ---
	inline uint64 PackKey(int32 Level, const FIntVector& C)
	{
		return (uint64(uint32(Level) & 0xF) << 60) |
		       (uint64(uint32(C.X) & 0xFFFFF) << 40) |
		       (uint64(uint32(C.Y) & 0xFFFFF) << 20) |
		       uint64(uint32(C.Z) & 0xFFFFF);
	}
	inline int32 SignExt20(uint32 V) { return (int32(V << 12)) >> 12; }
	inline void UnpackKey(uint64 K, int32& Level, FIntVector& C)
	{
		Level = int32((K >> 60) & 0xF);
		C.X = SignExt20(uint32((K >> 40) & 0xFFFFF));
		C.Y = SignExt20(uint32((K >> 20) & 0xFFFFF));
		C.Z = SignExt20(uint32(K & 0xFFFFF));
	}
	inline uint64 PackFootprint(int32 Level, int32 X, int32 Y)
	{
		return PackKey(Level, FIntVector(X, Y, 0));
	}

	// The shader's HashKey, byte for byte. The audit gate exists to catch the
	// day these two drift, so a change here without the same change in
	// VoxelResidencyScan.usf reads as an audit FAIL, loudly.
	inline uint32 RotL(uint32 V, uint32 S) { return (V << S) | (V >> (32 - S)); }
	inline void HashKey(uint32 Level, const FIntVector& C, uint32& OutLo, uint32& OutHi)
	{
		const uint32 HX = uint32(C.X) * 0x9E3779B1u;
		const uint32 HY = uint32(C.Y) * 0x85EBCA77u;
		const uint32 HZ = uint32(C.Z) * 0xC2B2AE3Du;
		const uint32 HL = (Level + 1u) * 0x27D4EB2Fu;
		OutLo = HX ^ RotL(HY, 3) ^ RotL(HZ, 7) ^ HL;
		OutHi = RotL(HX, 11) ^ HY ^ RotL(HZ, 17) ^ RotL(HL, 5);
	}
} // namespace VoxelResidency

// ---------------------------------------------------------------------------
// Shader classes
// ---------------------------------------------------------------------------

#define VOXEL_RESIDENCY_USF "/VoxelEarth/VoxelResidencyScan.usf"

namespace
{
	class FVoxelResidencyFeedbackCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyFeedbackCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyFeedbackCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ShadowGrid)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OrphanList)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FeedbackOps)
			SHADER_PARAMETER(uint32, FeedbackOpCount)
			SHADER_PARAMETER(uint32, OrphanCap)
			SHADER_PARAMETER(uint32, MaxRingLevel)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelResidencyZRangeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyZRangeCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyZRangeCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, ZRangeGrid)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ZRangeOps)
			SHADER_PARAMETER(uint32, ZRangeOpCount)
			SHADER_PARAMETER(uint32, MaxRingLevel)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelResidencyExitScanCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyExitScanCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyExitScanCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ShadowGrid)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, EvictList)
			SHADER_PARAMETER(uint32, EvictCap)
			SHADER_PARAMETER(uint32, NumLevels)
			SHADER_PARAMETER(uint32, MaxRingLevel)
			SHADER_PARAMETER(uint32, HierarchicalCoverage)
			SHADER_PARAMETER_ARRAY(FVector4f, LevelRadiiA, [8])
			SHADER_PARAMETER_ARRAY(FVector4f, LevelRadiiB, [8])
			SHADER_PARAMETER_ARRAY(FVector4f, LevelAnchorFrac, [8])
			SHADER_PARAMETER_ARRAY(FIntVector4, LevelAnchorChunk, [8])
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelResidencyEntryScanCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyEntryScanCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyEntryScanCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ShadowGrid)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, ZRangeGrid)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AdmitList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ResurrectList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ColdList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AdmitHist)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, BudgetThresholdSq)
			SHADER_PARAMETER(uint32, AdmitBudget)
			SHADER_PARAMETER(uint32, EmitPhase)
			SHADER_PARAMETER(uint32, HistBins)
			SHADER_PARAMETER(uint32, AdmitCap)
			SHADER_PARAMETER(uint32, ResurrectCap)
			SHADER_PARAMETER(uint32, ColdCap)
			SHADER_PARAMETER(uint32, NumLevels)
			SHADER_PARAMETER(uint32, MaxRingLevel)
			SHADER_PARAMETER(uint32, LevelIndex)
			SHADER_PARAMETER(uint32, HierarchicalCoverage)
			SHADER_PARAMETER(uint32, RingOverlapChunks)
			SHADER_PARAMETER(uint32, UndergroundDisabledFlag)
			SHADER_PARAMETER(float, SkirtNearFrac)
			SHADER_PARAMETER(float, SkirtMidFrac)
			SHADER_PARAMETER(uint32, SkirtChunksNearMidFar)
			SHADER_PARAMETER_ARRAY(FVector4f, LevelRadiiA, [8])
			SHADER_PARAMETER_ARRAY(FVector4f, LevelRadiiB, [8])
			SHADER_PARAMETER_ARRAY(FVector4f, LevelAnchorFrac, [8])
			SHADER_PARAMETER_ARRAY(FIntVector4, LevelAnchorChunk, [8])
		END_SHADER_PARAMETER_STRUCT()
	};

	// The budget resolve. EVERY loose global its kernel reads must be declared
	// here: the .usf shares one global namespace across all six kernels, and a
	// global a kernel reads but its FParameters omits binds to nothing and
	// reads garbage silently. (MaxRingLevel was missing from four of five
	// structs until 2026-08-23.) tools/lint-residency-globals.py checks this
	// mechanically -- run it after touching either file.
	class FVoxelResidencyBudgetResolveCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyBudgetResolveCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyBudgetResolveCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AdmitHist)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, BudgetThresholdSq)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
			SHADER_PARAMETER(uint32, NumLevels)
			SHADER_PARAMETER(uint32, HistBins)
			SHADER_PARAMETER_ARRAY(FIntVector4, LevelAdmitBudget, [2])
			SHADER_PARAMETER_ARRAY(FVector4f, LevelRadiiA, [8])
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelResidencyAuditCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelResidencyAuditCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelResidencyAuditCS, FGlobalShader);
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
		{
			return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
		}
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ShadowGrid)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
			SHADER_PARAMETER(uint32, NumLevels)
			SHADER_PARAMETER(uint32, MaxRingLevel)
		END_SHADER_PARAMETER_STRUCT()
	};
} // namespace

IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyFeedbackCS, VOXEL_RESIDENCY_USF, "FeedbackCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyZRangeCS, VOXEL_RESIDENCY_USF, "ZRangeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyExitScanCS, VOXEL_RESIDENCY_USF, "ExitScanCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyEntryScanCS, VOXEL_RESIDENCY_USF, "EntryScanCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyBudgetResolveCS, VOXEL_RESIDENCY_USF, "BudgetResolveCS",
                        SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelResidencyAuditCS, VOXEL_RESIDENCY_USF, "AuditCS", SF_Compute);

// ---------------------------------------------------------------------------
// FImpl
// ---------------------------------------------------------------------------

using namespace VoxelResidency;

struct FVoxelResidencyGpu::FImpl
{
	// ---- one in-flight dispatch (crosses to the render thread) -------------
	struct FSeqInFlight
	{
		uint32 Seq = 0;
		bool bAudit = false;
		TUniquePtr<FRHIGPUBufferReadback> CountersRB;
		TUniquePtr<FRHIGPUBufferReadback> AdmitRB;
		TUniquePtr<FRHIGPUBufferReadback> EvictRB;
		TUniquePtr<FRHIGPUBufferReadback> ResurrectRB;
		TUniquePtr<FRHIGPUBufferReadback> ColdRB;
		TUniquePtr<FRHIGPUBufferReadback> OrphanRB;
		// Filled by the render-thread poll, published with State=2 (release).
		TArray<uint32> Counters;
		TArray<uint32> Admit;
		TArray<uint32> Evict;
		TArray<uint32> Resurrect;
		TArray<uint32> Cold;
		TArray<uint32> Orphan;
		std::atomic<int32> State{0}; // 0 dispatched, 1 poll in flight, 2 ready
	};
	using FSeqPtr = TSharedPtr<FSeqInFlight, ESPMode::ThreadSafe>;

	// ---- one recompute's CPU decision ledger -------------------------------
	struct FSeqLedger
	{
		uint32 Seq = 0;
		bool bDispatched = false;
		bool bUnderground = false;
		FVoxelResidencyDispatchParams Params;
		TArray<uint64> CpuAdmits;
		TArray<uint8> CpuAdmitDeep;
		TArray<uint64> CpuEvicts;
		TArray<uint8> CpuEvictDeep;
		TArray<uint64> CpuResurrects;
		// CPU ledger snapshot at dispatch, for the audit compare.
		int64 AuditCount = 0;
		uint32 AuditXorLo = 0;
		uint32 AuditXorHi = 0;
	};

	struct FTexelState
	{
		int16 ZMin = 0;
		int16 ZMax = 0;
		uint32 UploadedSeq = MAX_uint32; // first dispatch that carries it
	};

	// ---- game-thread state --------------------------------------------------
	int32 Mode = -1; // -1 = not latched yet
	// -VoxelResidencyAdmitBudget=N, latched with the mode. 0 = OFF and the
	// dispatch is byte-identical to the pre-budget one (no histogram pass, no
	// resolve pass, EmitPhase 1 only). See the .usf header for why it exists.
	int32 AdmitBudget = 0;
	bool bNeedsResync = true;
	bool bClearGridOnNextDispatch = true;
	bool bInBracket = false;
	uint32 NextSeq = 1;
	uint32 DispatchesSinceAudit = 0;

	TArray<uint32> StagedFeedback; // 4 dwords per op
	TSet<uint64> StagedTexels;
	TMap<uint64, FTexelState> TexelMirror;
	TSet<uint64> Residual; // keys orphaned out of the mirror (CPU-owned)

	// CPU running audit ledger (records currently represented in the mirror).
	int64 CpuCount = 0;
	uint32 CpuXorLo = 0;
	uint32 CpuXorHi = 0;

	TArray<FSeqPtr> InFlight;
	TArray<FSeqLedger> Ledgers;
	FSeqLedger* OpenLedger = nullptr; // points into Ledgers while bracketed

	// Mode 2: the newest arrived, unconsumed delta. AT MOST ONE is staged --
	// every scan is a complete statement over the (mirror, annulus) state at
	// its dispatch, so a newer delta supersedes an older one outright: any
	// proposal the older carried that is still valid is re-derived by the
	// newer scan by construction (the mirror only changes through confirmed
	// feedback, which the unconsumed older delta never generated). Superseded
	// deltas are counted, never merged.
	TUniquePtr<FVoxelResidencyLiveDelta> PendingLive;

	// Persistent GPU buffers (render-thread owned after creation).
	TRefCountPtr<FRDGPooledBuffer> ShadowPooled;
	TRefCountPtr<FRDGPooledBuffer> ZRangePooled;
	uint32 AllocatedLevels = 0;

	FTSTicker::FDelegateHandle TickerHandle;

	// ---- stats (game thread; the [gpu-resid] line) --------------------------
	struct FStats
	{
		uint64 Dispatches = 0, SkippedInFlight = 0, SkippedUnderground = 0, Uncompared = 0;
		uint64 GpuAdmit = 0, GpuEvict = 0, GpuResurrect = 0, GpuCold = 0, GpuOrphan = 0;
		uint64 Overflows = 0;
		// The kernels' own enumeration traffic. These were written by the
		// shader from the first commit and read by NOTHING until 2026-08-23 --
		// four InterlockedAdds on the hot path feeding an instrument that did
		// not exist. They are the GPU-side answer to "how many candidates does
		// the annulus produce, and where do they die".
		uint64 GpuCandVisited = 0;  // XY footprints past the inner/outer/seam gates
		uint64 GpuZVisited = 0;     // Z-column cells examined
		uint64 GpuCutoffRej = 0;    // cells the ring cutoff refused
		uint64 GpuBudgetRej = 0;    // cells the GPU admission budget refused
		uint64 GpuHistTotal = 0;    // histogram-pass total; == GpuAdmit + GpuBudgetRej
		uint64 GpuExitVisited = 0;  // tracked cells the exit kernel walked
		uint64 FbStaged = 0, FbApplied = 0, FbDropped = 0, FbCollisions = 0;
		uint64 EvictMatch = 0, EvictMissed = 0, EvictExtra = 0, EvictBoundary = 0;
		uint64 AdmitCovered = 0, MissedSurface = 0, MissedBoundary = 0, MissedDeep = 0;
		uint64 MissedEditHatch = 0, MissedCold = 0, MissedOverflow = 0, ProposedExtra = 0;
		uint64 ResurrectCovered = 0, ResurrectMissed = 0, ResurrectExtra = 0;
		uint64 AuditPass = 0, AuditFail = 0;
		int64 LastAuditGpuCount = -1, LastAuditCpuCount = -1;
		uint64 ColdLastWindow = 0;
		// ---- mode-2 (LIVE) lanes ----
		uint64 LiveConsumed = 0;     // deltas handed to the subsystem
		uint64 LiveSuperseded = 0;   // staged deltas replaced by a newer one before consumption
		uint64 LiveEmptyRetired = 0; // deltas with no proposals and no scanned level (nothing stampable)
		// The two mode-2 lanes that fall back to the FULL CPU walks without the
		// subsystem ever building a live outcome, so they used to report as an
		// all-zero live line -- indistinguishable from a dead GPU path. Counted
		// here, where the manager already knows both facts, so no subsystem
		// hook is needed. Either climbing means the leg's timings are measuring
		// the CPU arm.
		uint64 LiveUndergroundCalls = 0; // anchor underground: recompute is CPU-only by design
		uint64 LiveResyncCalls = 0;      // mirror replayed this call: CPU walks forced
		FVoxelResidencyLiveOutcome Live; // summed subsystem adjudication reports
	};
	FStats Since; // reset each log window
	FStats Total; // lifetime
	int32 AuditMismatchStreak = 0;
	double LastLogSeconds = 0.0;
	double LastWindowSeconds = 0.0; // the REAL span of the window just closed
	// THE WINDOW IS NOT 5 SECONDS (2026-08-23). This logger gated on a
	// hardcoded 5.0 while every flight leg passes -VoxelPerfLogInterval=2, so
	// its windows and the LogVoxelPerf windows were 2.5x apart and any
	// side-by-side reading of the two families -- "prop per window" against
	// "recompute per window" -- was wrong by that factor. This is the same
	// defect the lane holder found in MaybeLogCounters' `WindowMs = 5000.0`
	// divisor, in a second place, which is why the interval is now READ rather
	// than assumed and the real elapsed seconds are PRINTED on the line. A
	// window whose length is asserted rather than measured is a denominator
	// nobody checks.
	double LogIntervalSeconds = 0.0; // 0 = not latched yet
	// Anchor of the most recent OnRecomputeBegin, and its value at the last
	// log flush: their XY distance prints on the live line as move=, which is
	// what makes an all-zero window READABLE ON ITS OWN. Zero proposals with
	// move ~0 is a converged, parked world -- the healthy reading; zero
	// proposals with move >> a chunk edge is the dead-path reading. The
	// t42-live.log misdiagnosis was exactly this ambiguity: the final parked
	// windows (cons=427, prop=0) were read as a dead pass when the flight
	// windows just above them carried prop in the millions.
	FVector LastDispatchAnchor = FVector::ZeroVector;
	FVector LogWindowAnchor = FVector::ZeroVector;
	TArray<uint64> MissedSurfaceSamples; // up to 4 per window, logged then cleared

	// ------------------------------------------------------------------------
	int32 LatchMode()
	{
		if (Mode < 0)
		{
			int32 Requested = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuResidency="), Requested);
			Mode = FMath::Clamp(Requested, 0, 2);
			int32 Budget = 0;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelResidencyAdmitBudget="), Budget);
			AdmitBudget = FMath::Max(0, Budget);
			if (Mode != 0)
			{
				if (AdmitBudget > 0)
				{
					UE_LOG(LogVoxelResidGpu, Log,
					       TEXT("[gpu-resid] admission budget ON: the entry scan emits only the ")
					       TEXT("NEAREST ~%d candidates per level. Read budRej WITH adOK -- budRej ")
					       TEXT("climbing while adOK holds is the win; adOK falling with it is ")
					       TEXT("over-throttling. budRej=0 with histTot>0 means the threshold ")
					       TEXT("never bound."),
					       AdmitBudget);
				}
				if (Mode == 2)
				{
					UE_LOG(LogVoxelResidGpu, Log,
					       TEXT("[gpu-resid] mode 2 (LIVE): the GPU pass proposes the desired-set ")
					       TEXT("delta and the CPU walks are skipped on the steady path; the CPU ")
					       TEXT("adjudicates every proposal (edits, cold, underground, starvation ")
					       TEXT("all fall back CPU-side, counted on the live line)."));
				}
				else
				{
					UE_LOG(LogVoxelResidGpu, Log,
					       TEXT("[gpu-resid] mode 1 (shadow): scans dispatch and compare; streaming ")
					       TEXT("decisions unchanged."));
				}
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda([this](float) {
						PollAndConsume();
						return true;
					}),
					0.25f);
			}
		}
		return Mode;
	}

	void StageOp(uint32 Op, int32 Level, const FIntVector& C, bool bDeep)
	{
		StagedFeedback.Add(Op | (uint32(Level) << 3) | (bDeep ? 0x80u : 0u));
		StagedFeedback.Add(uint32(C.X));
		StagedFeedback.Add(uint32(C.Y));
		StagedFeedback.Add(uint32(C.Z));
		++Since.FbStaged;
		++Total.FbStaged;
	}

	void LedgerXor(int32 Level, const FIntVector& C, int32 Delta)
	{
		uint32 Lo, Hi;
		HashKey(uint32(Level), C, Lo, Hi);
		CpuXorLo ^= Lo;
		CpuXorHi ^= Hi;
		CpuCount += Delta;
	}

	// ---- notes -------------------------------------------------------------
	void NoteRecordAdded(int32 Level, const FIntVector& C, bool bDeep)
	{
		StageOp(kOpInsert, Level, C, bDeep);
		LedgerXor(Level, C, +1);
		// Decision-ledger recording is the COMPARATOR's input, so mode 1 only:
		// in mode 2 the "CPU decisions" are the consumption of the GPU's own
		// proposals, and comparing those would be a statistic that cannot come
		// out the other way. The ledger itself still opens in mode 2 -- the
		// audit snapshot and the dispatch params ride it.
		if (Mode == 1 && bInBracket && OpenLedger)
		{
			OpenLedger->CpuAdmits.Add(PackKey(Level, C));
			OpenLedger->CpuAdmitDeep.Add(bDeep ? 1 : 0);
		}
	}

	void NoteRecordRemoved(int32 Level, const FIntVector& C)
	{
		const uint64 K = PackKey(Level, C);
		if (Residual.Remove(K) > 0)
		{
			// Was orphaned out of the mirror: its hash already left the CPU
			// ledger when the orphan arrived, and its cell no longer names it,
			// so no feedback and no xor -- the removal is purely CPU-side.
			return;
		}
		StageOp(kOpRemove, Level, C, false);
		LedgerXor(Level, C, -1);
	}

	void NoteUnloadQueued(int32 Level, const FIntVector& C, bool bDeep)
	{
		if (!Residual.Contains(PackKey(Level, C)))
		{
			StageOp(kOpMarkPending, Level, C, false);
		}
		if (Mode == 1 && bInBracket && OpenLedger)
		{
			OpenLedger->CpuEvicts.Add(PackKey(Level, C));
			OpenLedger->CpuEvictDeep.Add(bDeep ? 1 : 0);
		}
	}

	void NoteUnloadCancelled(int32 Level, const FIntVector& C)
	{
		if (!Residual.Contains(PackKey(Level, C)))
		{
			StageOp(kOpClearPending, Level, C, false);
		}
		if (Mode == 1 && bInBracket && OpenLedger)
		{
			OpenLedger->CpuResurrects.Add(PackKey(Level, C));
		}
	}

	void NoteFootprintZRange(int32 Level, int32 X, int32 Y, int32 ZMin, int32 ZMax)
	{
		const uint64 K = PackFootprint(Level, X, Y);
		FTexelState* State = TexelMirror.Find(K);
		const int16 NZMin = int16(FMath::Clamp(ZMin, -32768, 32767));
		const int16 NZMax = int16(FMath::Clamp(ZMax, -32768, 32767));
		if (State != nullptr && State->ZMin == NZMin && State->ZMax == NZMax)
		{
			return; // unchanged -- the overwhelmingly common (memo-hit) case
		}
		FTexelState& S = State ? *State : TexelMirror.Add(K);
		S.ZMin = NZMin;
		S.ZMax = NZMax;
		S.UploadedSeq = MAX_uint32; // stamped by the dispatch that carries it
		StagedTexels.Add(K);
	}

	// ---- resync ------------------------------------------------------------
	void BeginResyncReplay()
	{
		// Mode 2: this recompute's CPU walks are forced (bResidResyncedThisCall
		// in the subsystem), and the replay itself is O(ChunkRecords). Counted
		// so a drift storm cannot read as a healthy live window.
		if (Mode == 2)
		{
			++Since.LiveResyncCalls;
			++Total.LiveResyncCalls;
		}
		StagedFeedback.Reset();
		StagedTexels.Reset();
		TexelMirror.Reset();
		Residual.Reset();
		CpuCount = 0;
		CpuXorLo = 0;
		CpuXorHi = 0;
		bClearGridOnNextDispatch = true;
		// In-flight deltas and open ledgers predate the resync; retire them.
		Since.Uncompared += uint64(Ledgers.Num());
		Total.Uncompared += uint64(Ledgers.Num());
		Ledgers.Reset();
		OpenLedger = nullptr;
		// A staged live delta was scanned against the pre-drift mirror; it
		// must not be consumed. Dropped proposals re-propose after the resync
		// by construction.
		if (PendingLive.IsValid())
		{
			PendingLive.Reset();
			++Since.LiveSuperseded;
			++Total.LiveSuperseded;
		}
	}

	void EndResyncReplay()
	{
		bNeedsResync = false;
	}

	// ---- dispatch ----------------------------------------------------------
	void OnRecomputeBegin(const FVoxelResidencyDispatchParams& P)
	{
		// Open the decision ledger first: the CPU walks that follow this call
		// are exactly what the dispatched scans are predicting.
		if (Ledgers.Num() >= kMaxLedger)
		{
			Ledgers.RemoveAt(0);
			++Since.Uncompared;
			++Total.Uncompared;
			OpenLedger = nullptr; // pointers into Ledgers are now stale
		}
		FSeqLedger& L = Ledgers.AddDefaulted_GetRef();
		L.Seq = NextSeq++;
		L.Params = P;
		LastDispatchAnchor = P.Anchor;
		L.bUnderground = P.bAnchorUnderground;
		L.AuditCount = CpuCount;
		L.AuditXorLo = CpuXorLo;
		L.AuditXorHi = CpuXorHi;
		OpenLedger = &L;
		bInBracket = true;

		if (P.bAnchorUnderground)
		{
			// The deep box's solid-skip proofs and anchor-Z candidates are
			// CPU-only by design (the owner's split: lopsided, at the producer
			// level). No dispatch; the recompute is CPU-owned and counted.
			++Since.SkippedUnderground;
			++Total.SkippedUnderground;
			if (Mode == 2)
			{
				// Mode 2's second silent CPU lane: no dispatch, no delta, and
				// the subsystem's live-outcome block is skipped entirely, so
				// this call reports as all-zero on the live line. Counted here.
				++Since.LiveUndergroundCalls;
				++Total.LiveUndergroundCalls;
			}
			return;
		}
		if (InFlight.Num() >= kMaxInFlight)
		{
			++Since.SkippedInFlight;
			++Total.SkippedInFlight;
			return;
		}

		L.bDispatched = true;
		++Since.Dispatches;
		++Total.Dispatches;
		++DispatchesSinceAudit;
		const bool bAudit = DispatchesSinceAudit >= kAuditEvery;
		if (bAudit)
		{
			DispatchesSinceAudit = 0;
		}

		FSeqPtr Seq = MakeShared<FSeqInFlight, ESPMode::ThreadSafe>();
		Seq->Seq = L.Seq;
		Seq->bAudit = bAudit;
		InFlight.Add(Seq);

		// Move the staged uploads out; stamp the texels with this seq so the
		// comparator knows which scans could see them.
		TArray<uint32> Feedback = MoveTemp(StagedFeedback);
		StagedFeedback.Reset();
		TArray<uint32> ZOps;
		ZOps.Reserve(StagedTexels.Num() * 4);
		for (uint64 K : StagedTexels)
		{
			FTexelState& S = TexelMirror.FindChecked(K);
			S.UploadedSeq = L.Seq;
			int32 Level;
			FIntVector C;
			UnpackKey(K, Level, C);
			ZOps.Add(uint32(Level));
			ZOps.Add(uint32(C.X));
			ZOps.Add(uint32(C.Y));
			ZOps.Add((uint32(uint16(S.ZMin))) | (uint32(uint16(S.ZMax)) << 16));
		}
		StagedTexels.Reset();

		const bool bClearGrid = bClearGridOnNextDispatch;
		bClearGridOnNextDispatch = false;

		// Everything the kernels need, captured by value (the cvar rule).
		struct FShaderConsts
		{
			FVector4f RadiiA[8];
			FVector4f RadiiB[8];
			FVector4f AnchorFrac[8];
			FIntVector4 AnchorChunk[8];
			uint32 NumLevels = 0;
			uint32 MaxRingLevel = 0;
			uint32 Hier = 0;
			uint32 Overlap = 0;
			uint32 UndergroundDisabled = 0;
			float SkirtNearFrac = 0.f;
			float SkirtMidFrac = 0.f;
			uint32 SkirtPacked = 0;
			uint32 ScanMask = 0;
			int32 AdmitBudget = 0;
			int32 LevelBudget[8] = {};
			int32 Spans[8] = {};
		};
		FShaderConsts K;
		K.AdmitBudget = AdmitBudget; // captured by value with everything else
		K.NumLevels = uint32(FMath::Clamp(P.NumLevels, 1, int32(kMaxLevels)));
		// Clamped below NumLevels so the kernel's seam-parent access at L+1
		// can never index past the populated params (the CPU accessor already
		// clamps to kNumLevels-1; this is belt for a garbage param).
		K.MaxRingLevel = uint32(FMath::Clamp(P.MaxRingLevel, 0, int32(K.NumLevels) - 1));
		K.Hier = P.bHierarchicalCoverage ? 1 : 0;
		K.Overlap = uint32(FMath::Max(0, P.RingOverlapChunks));
		K.UndergroundDisabled = P.bUndergroundDisabled ? 1 : 0;
		K.SkirtNearFrac = float(P.SkirtNearFrac);
		K.SkirtMidFrac = float(P.SkirtMidFrac);
		K.SkirtPacked = (uint32(P.SkirtChunksNear) & 0x3FF) |
		                ((uint32(P.SkirtChunksMid) & 0x3FF) << 10) |
		                ((uint32(P.SkirtChunksFar) & 0x3FF) << 20);
		for (int32 I = 0; I < int32(K.NumLevels); ++I)
		{
			const FVoxelResidencyLevelParams& LP = P.Levels[I];
			const double Edge = LP.ChunkEdgeUU;
			K.RadiiA[I] = FVector4f(float(Edge), float(LP.OuterUU * LP.OuterUU),
			                        float(LP.AdmitOuterUU * LP.AdmitOuterUU),
			                        float(LP.InnerAdmitUU * LP.InnerAdmitUU));
			K.RadiiB[I] = FVector4f(float(LP.InnerEvictUU * LP.InnerEvictUU),
			                        float(LP.UnloadOuterUU * LP.UnloadOuterUU),
			                        float(LP.VerticalKeepUU),
			                        LP.CutoffSortKeySq >= double(MAX_flt)
			                            ? MAX_flt
			                            : float(LP.CutoffSortKeySq));
			K.AnchorFrac[I] = FVector4f(float(P.Anchor.X - double(LP.AnchorChunk.X) * Edge),
			                            float(P.Anchor.Y - double(LP.AnchorChunk.Y) * Edge),
			                            float(P.Anchor.Z - double(LP.AnchorChunk.Z) * Edge),
			                            float(LP.InnerAdmitUU * LP.InnerAdmitUU)); // BiasClampSq
			K.AnchorChunk[I] =
				FIntVector4(LP.AnchorChunk.X, LP.AnchorChunk.Y, LP.AnchorChunk.Z, LP.ChunkSpan);
			K.Spans[I] = LP.ChunkSpan;
			// The ring's own allowance when the subsystem supplies one, else
			// the command-line number. Resolved HERE, once, by value -- the
			// shader must never have to choose between two sources for a knob.
			K.LevelBudget[I] = LP.AdmitBudget > 0 ? LP.AdmitBudget : AdmitBudget;
			if (LP.bScanThisDispatch && I <= P.MaxRingLevel)
			{
				K.ScanMask |= 1u << I;
			}
		}

		FImpl* Self = this; // leaked singleton; raw capture is safe
		ENQUEUE_RENDER_COMMAND(VoxelResidencyDispatch)(
			[Self, Seq, K, bAudit, bClearGrid, Feedback = MoveTemp(Feedback),
			 ZOps = MoveTemp(ZOps)](FRHICommandListImmediate& RHICmdList) mutable
		{
			// Builds and executes its OWN RDG graph on the render thread,
			// outside the scene renderer: pass construction, buffer creation and
			// upload, SRV/UAV creation. This is the shape the tail buckets exist
			// to name.
			VOXEL_RENDER_FRAME_SCOPE_TAIL(TailResidency);
			// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
			// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
			// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
			// Sentinel -- because these builders Execute inside the scope rather than
			// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
			// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
			// the same GPU/<name> CSV column, and outlives the graph by construction
			// (declared before it, destroyed after it).
			RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamResidency, "VoxelStreamResidency");
			FRDGBuilder GraphBuilder(RHICmdList);

			// Persistent buffers: create+clear on first use (or resync), else
			// register. Extraction re-targets the pooled handle every graph,
			// which is the march index's own pattern.
			FRDGBufferRef Shadow;
			FRDGBufferRef ZRange;
			const uint32 Levels = K.NumLevels;
			if (!Self->ShadowPooled.IsValid() || Self->AllocatedLevels < Levels)
			{
				Shadow = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Levels * kCellsPerLevel),
					TEXT("VoxelResidency.Shadow"));
				ZRange = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2, Levels * kDimXY * kDimXY),
					TEXT("VoxelResidency.ZRange"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Shadow), 0u);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ZRange), 0u);
				Self->AllocatedLevels = Levels;
			}
			else
			{
				Shadow = GraphBuilder.RegisterExternalBuffer(Self->ShadowPooled,
				                                             TEXT("VoxelResidency.Shadow"));
				ZRange = GraphBuilder.RegisterExternalBuffer(Self->ZRangePooled,
				                                             TEXT("VoxelResidency.ZRange"));
				if (bClearGrid)
				{
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Shadow), 0u);
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ZRange), 0u);
				}
			}

			// Counters, zeroed by upload; proposal lists, garbage-tolerated
			// past their counter (the CPU clamps).
			const uint32 Zeros[kNumCounters] = {};
			FRDGBufferRef Counters = CreateStructuredBuffer(
				GraphBuilder, TEXT("VoxelResidency.Counters"), sizeof(uint32), kNumCounters, Zeros,
				sizeof(Zeros));
			auto MakeList = [&](const TCHAR* Name, uint32 Cap) {
				return GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Cap * 4), Name);
			};
			FRDGBufferRef Admit = MakeList(TEXT("VoxelResidency.Admit"), kAdmitCap);
			FRDGBufferRef Evict = MakeList(TEXT("VoxelResidency.Evict"), kEvictCap);
			FRDGBufferRef Resurrect = MakeList(TEXT("VoxelResidency.Resurrect"), kResurrectCap);
			FRDGBufferRef Cold = MakeList(TEXT("VoxelResidency.Cold"), kColdCap);
			FRDGBufferRef Orphan = MakeList(TEXT("VoxelResidency.Orphan"), kOrphanCap);

			// The budget lane's two transient buffers. Always created and
			// always bound (an unbound UAV is a validation failure even on a
			// path that never reads it); only WRITTEN when the budget is on.
			// The histogram must start at zero every dispatch -- a stale
			// histogram would resolve a threshold for the PREVIOUS anchor,
			// which is the quiet way this feature could throttle the wrong
			// ring. BudgetThresholdSq is cleared to +inf as 0x7F7FFFFF so that
			// a resolve pass that never ran cannot throttle anything: the
			// failure direction is always "emit everything", never "emit
			// nothing".
			FRDGBufferRef AdmitHist = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Levels * kHistBins),
				TEXT("VoxelResidency.AdmitHist"));
			FRDGBufferRef BudgetThresh = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), FMath::Max(Levels, 1u)),
				TEXT("VoxelResidency.BudgetThresholdSq"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AdmitHist), 0u);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BudgetThresh), 0x7F7FFFFFu);

			const auto ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

			// 1. Feedback (ordered, single lane -- see the kernel's comment).
			if (Feedback.Num() > 0)
			{
				FRDGBufferRef Ops = CreateStructuredBuffer(
					GraphBuilder, TEXT("VoxelResidency.FeedbackOps"), sizeof(uint32), Feedback.Num(),
					Feedback.GetData(), Feedback.Num() * sizeof(uint32));
				TShaderMapRef<FVoxelResidencyFeedbackCS> CS(ShaderMap);
				auto* Params = GraphBuilder.AllocParameters<FVoxelResidencyFeedbackCS::FParameters>();
				Params->ShadowGrid = GraphBuilder.CreateUAV(Shadow);
				Params->Counters = GraphBuilder.CreateUAV(Counters);
				Params->OrphanList = GraphBuilder.CreateUAV(Orphan);
				Params->FeedbackOps = GraphBuilder.CreateSRV(Ops);
				Params->FeedbackOpCount = uint32(Feedback.Num() / 4);
				Params->OrphanCap = kOrphanCap;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelResidency::Feedback"),
				                             CS, Params, FIntVector(1, 1, 1));
			}

			// 2. Z-range texels (pre-dedup'd; parallel scatter).
			if (ZOps.Num() > 0)
			{
				FRDGBufferRef Ops = CreateStructuredBuffer(
					GraphBuilder, TEXT("VoxelResidency.ZRangeOps"), sizeof(uint32), ZOps.Num(),
					ZOps.GetData(), ZOps.Num() * sizeof(uint32));
				TShaderMapRef<FVoxelResidencyZRangeCS> CS(ShaderMap);
				auto* Params = GraphBuilder.AllocParameters<FVoxelResidencyZRangeCS::FParameters>();
				Params->ZRangeGrid = GraphBuilder.CreateUAV(ZRange);
				Params->ZRangeOps = GraphBuilder.CreateSRV(Ops);
				Params->ZRangeOpCount = uint32(ZOps.Num() / 4);
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelResidency::ZRange"), CS, Params,
					FIntVector(int32(FMath::DivideAndRoundUp(uint32(ZOps.Num() / 4), 64u)), 1, 1));
			}

			// 3. Exit scan: every level, every cell.
			{
				TShaderMapRef<FVoxelResidencyExitScanCS> CS(ShaderMap);
				auto* Params = GraphBuilder.AllocParameters<FVoxelResidencyExitScanCS::FParameters>();
				Params->ShadowGrid = GraphBuilder.CreateUAV(Shadow);
				Params->Counters = GraphBuilder.CreateUAV(Counters);
				Params->EvictList = GraphBuilder.CreateUAV(Evict);
				Params->EvictCap = kEvictCap;
				Params->NumLevels = Levels;
				Params->HierarchicalCoverage = K.Hier;
				for (int32 I = 0; I < 8; ++I)
				{
					Params->LevelRadiiA[I] = K.RadiiA[I];
					Params->LevelRadiiB[I] = K.RadiiB[I];
					Params->LevelAnchorFrac[I] = K.AnchorFrac[I];
					Params->LevelAnchorChunk[I] = K.AnchorChunk[I];
				}
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelResidency::ExitScan"), CS, Params,
					FIntVector(int32(kDimXY / 8), int32(kDimXY / 8), int32(Levels * kDimZ / 8)));
			}

			// 4. Entry scan. With the budget OFF this is exactly one emit pass
			// per gated level, as before. With it ON the chain is
			// histogram(all levels) -> resolve -> emit(all levels); RDG's UAV
			// dependencies order the three, and both enumeration phases MUST
			// see the same ShadowGrid (nothing writes it between them).
			const uint32 Budget = uint32(K.AdmitBudget);
			auto AddEntryPass = [&](uint32 LevelIdx, uint32 EmitPhase)
			{
				TShaderMapRef<FVoxelResidencyEntryScanCS> CS(ShaderMap);
				auto* Params = GraphBuilder.AllocParameters<FVoxelResidencyEntryScanCS::FParameters>();
				Params->AdmitHist = GraphBuilder.CreateUAV(AdmitHist);
				Params->BudgetThresholdSq = GraphBuilder.CreateUAV(BudgetThresh);
				Params->AdmitBudget = Budget;
				Params->EmitPhase = EmitPhase;
				Params->HistBins = kHistBins;
				Params->ShadowGrid = GraphBuilder.CreateUAV(Shadow);
				Params->ZRangeGrid = GraphBuilder.CreateUAV(ZRange);
				Params->Counters = GraphBuilder.CreateUAV(Counters);
				Params->AdmitList = GraphBuilder.CreateUAV(Admit);
				Params->ResurrectList = GraphBuilder.CreateUAV(Resurrect);
				Params->ColdList = GraphBuilder.CreateUAV(Cold);
				Params->AdmitCap = kAdmitCap;
				Params->ResurrectCap = kResurrectCap;
				Params->ColdCap = kColdCap;
				Params->NumLevels = Levels;
				Params->MaxRingLevel = K.MaxRingLevel;
				Params->LevelIndex = LevelIdx;
				Params->HierarchicalCoverage = K.Hier;
				Params->RingOverlapChunks = K.Overlap;
				Params->UndergroundDisabledFlag = K.UndergroundDisabled;
				Params->SkirtNearFrac = K.SkirtNearFrac;
				Params->SkirtMidFrac = K.SkirtMidFrac;
				Params->SkirtChunksNearMidFar = K.SkirtPacked;
				for (int32 I = 0; I < 8; ++I)
				{
					Params->LevelRadiiA[I] = K.RadiiA[I];
					Params->LevelRadiiB[I] = K.RadiiB[I];
					Params->LevelAnchorFrac[I] = K.AnchorFrac[I];
					Params->LevelAnchorChunk[I] = K.AnchorChunk[I];
				}
				const int32 BoxEdge = 2 * K.Spans[LevelIdx] + 1;
				const int32 Groups = FMath::DivideAndRoundUp(BoxEdge, 8);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("VoxelResidency::EntryScan L%u phase%u", LevelIdx, EmitPhase),
					CS, Params, FIntVector(Groups, Groups, 1));
			};

			if (Budget > 0)
			{
				for (uint32 LevelIdx = 0; LevelIdx < Levels; ++LevelIdx)
				{
					if ((K.ScanMask & (1u << LevelIdx)) != 0)
					{
						AddEntryPass(LevelIdx, 0u);
					}
				}
				TShaderMapRef<FVoxelResidencyBudgetResolveCS> CS(ShaderMap);
				auto* Params =
					GraphBuilder.AllocParameters<FVoxelResidencyBudgetResolveCS::FParameters>();
				Params->AdmitHist = GraphBuilder.CreateUAV(AdmitHist);
				Params->BudgetThresholdSq = GraphBuilder.CreateUAV(BudgetThresh);
				Params->Counters = GraphBuilder.CreateUAV(Counters);
				Params->NumLevels = Levels;
				Params->HistBins = kHistBins;
				for (int32 I = 0; I < 2; ++I)
				{
					Params->LevelAdmitBudget[I] =
						FIntVector4(K.LevelBudget[I * 4 + 0], K.LevelBudget[I * 4 + 1],
					                K.LevelBudget[I * 4 + 2], K.LevelBudget[I * 4 + 3]);
				}
				for (int32 I = 0; I < 8; ++I)
				{
					Params->LevelRadiiA[I] = K.RadiiA[I];
				}
				FComputeShaderUtils::AddPass(GraphBuilder,
				                             RDG_EVENT_NAME("VoxelResidency::BudgetResolve"), CS,
				                             Params, FIntVector(1, 1, 1));
			}
			for (uint32 LevelIdx = 0; LevelIdx < Levels; ++LevelIdx)
			{
				if ((K.ScanMask & (1u << LevelIdx)) != 0)
				{
					AddEntryPass(LevelIdx, 1u);
				}
			}

			// 5. Audit, on cadence.
			if (bAudit)
			{
				TShaderMapRef<FVoxelResidencyAuditCS> CS(ShaderMap);
				auto* Params = GraphBuilder.AllocParameters<FVoxelResidencyAuditCS::FParameters>();
				Params->ShadowGrid = GraphBuilder.CreateUAV(Shadow);
				Params->Counters = GraphBuilder.CreateUAV(Counters);
				Params->NumLevels = Levels;
				const uint32 TotalCells = Levels * kCellsPerLevel;
				const uint32 TotalGroups = FMath::DivideAndRoundUp(TotalCells, 64u);
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("VoxelResidency::Audit"), CS, Params,
					FIntVector(256, int32(FMath::DivideAndRoundUp(TotalGroups, 256u)), 1));
			}

			// 6. Readbacks: full capacity, clamped CPU-side by the counters.
			auto MakeRB = [&](const TCHAR* Name, FRDGBufferRef Buf, uint32 Bytes) {
				TUniquePtr<FRHIGPUBufferReadback> RB = MakeUnique<FRHIGPUBufferReadback>(Name);
				AddEnqueueCopyPass(GraphBuilder, RB.Get(), Buf, Bytes);
				return RB;
			};
			Seq->CountersRB = MakeRB(TEXT("VoxelResidency.CountersRB"), Counters,
			                         kNumCounters * sizeof(uint32));
			Seq->AdmitRB = MakeRB(TEXT("VoxelResidency.AdmitRB"), Admit, kAdmitCap * 16);
			Seq->EvictRB = MakeRB(TEXT("VoxelResidency.EvictRB"), Evict, kEvictCap * 16);
			Seq->ResurrectRB =
				MakeRB(TEXT("VoxelResidency.ResurrectRB"), Resurrect, kResurrectCap * 16);
			Seq->ColdRB = MakeRB(TEXT("VoxelResidency.ColdRB"), Cold, kColdCap * 16);
			Seq->OrphanRB = MakeRB(TEXT("VoxelResidency.OrphanRB"), Orphan, kOrphanCap * 16);

			// Keep (or re-establish) the persistent handles.
			GraphBuilder.QueueBufferExtraction(Shadow, &Self->ShadowPooled);
			GraphBuilder.QueueBufferExtraction(ZRange, &Self->ZRangePooled);

			GraphBuilder.Execute();
		});
	}

	void OnRecomputeEnd()
	{
		bInBracket = false;
		OpenLedger = nullptr;
		PollAndConsume();
	}

	// ---- mode-2 (LIVE) game-thread API -------------------------------------
	bool TakeLiveDelta(FVoxelResidencyLiveDelta& Out)
	{
		// Drain anything already copied back before answering -- the render
		// poll this enqueues completes asynchronously (a NEXT taker's gain);
		// nothing here waits.
		PollAndConsume();
		if (!PendingLive.IsValid())
		{
			return false;
		}
		Out = MoveTemp(*PendingLive);
		PendingLive.Reset();
		++Since.LiveConsumed;
		++Total.LiveConsumed;
		return true;
	}

	void SnapshotResidual(TArray<FVoxelResidencyProposal>& Out)
	{
		Out.Reset();
		Out.Reserve(Residual.Num());
		for (const uint64 Key : Residual)
		{
			int32 Level;
			FIntVector C;
			UnpackKey(Key, Level, C);
			Out.Add(FVoxelResidencyProposal{Level, C});
		}
	}

	static void FoldLiveOutcome(FVoxelResidencyLiveOutcome& A, const FVoxelResidencyLiveOutcome& B)
	{
		A.ConsumedDelta += B.ConsumedDelta;
		A.NoDeltaCalls += B.NoDeltaCalls;
		A.CpuFallbackCalls += B.CpuFallbackCalls;
		A.CpuFirstScans += B.CpuFirstScans;
		A.AdmitProposals += B.AdmitProposals;
		A.AdmitAdmitted += B.AdmitAdmitted;
		A.AdmitAdopted += B.AdmitAdopted;
		A.AdmitResurrected += B.AdmitResurrected;
		A.AdmitStale += B.AdmitStale;
		A.AdmitRejFine += B.AdmitRejFine;
		A.AdmitRejBudget += B.AdmitRejBudget;
		A.AdmitRejCutoff += B.AdmitRejCutoff;
		A.ResurrectProposals += B.ResurrectProposals;
		A.EvictProposals += B.EvictProposals;
		A.EvictQueued += B.EvictQueued;
		A.EvictVetoed += B.EvictVetoed;
		A.EvictStale += B.EvictStale;
		A.ColdProposals += B.ColdProposals;
		A.ColdEnumerated += B.ColdEnumerated;
		A.ColdDeferred += B.ColdDeferred;
		A.EditedEnumerated += B.EditedEnumerated;
		A.EditedFullSweeps += B.EditedFullSweeps;
		A.EditedDirtyPasses += B.EditedDirtyPasses;
		A.EditedSkipped += B.EditedSkipped;
		A.EditedDeferred += B.EditedDeferred;
		A.ResidualWalked += B.ResidualWalked;
		A.EvictMs += B.EvictMs;
		A.AdmitMs += B.AdmitMs;
	}

	void NoteLiveOutcome(const FVoxelResidencyLiveOutcome& Outcome)
	{
		FoldLiveOutcome(Since.Live, Outcome);
		FoldLiveOutcome(Total.Live, Outcome);
	}

	// ---- poll + consume ----------------------------------------------------
	void PollAndConsume()
	{
		// Kick a render-thread poll for any seq still waiting on its copies.
		bool bAnyWaiting = false;
		for (const FSeqPtr& Seq : InFlight)
		{
			if (Seq->State.load(std::memory_order_acquire) == 0)
			{
				bAnyWaiting = true;
			}
		}
		if (bAnyWaiting)
		{
			TArray<FSeqPtr> Snapshot = InFlight;
			ENQUEUE_RENDER_COMMAND(VoxelResidencyPoll)(
				[Batch = MoveTemp(Snapshot)](FRHICommandListImmediate&)
			{
				// READBACK HANDLING -- IsReady polls plus the Lock/copy/Unlock
				// of whichever samples completed. Render-thread work whose cost
				// depends on how many dispatches are in flight, i.e. on how hard
				// the leg is streaming.
				VOXEL_RENDER_FRAME_SCOPE_TAIL(TailResidency);
				for (const FSeqPtr& Seq : Batch)
				{
					if (Seq->State.load(std::memory_order_acquire) != 0)
					{
						continue;
					}
					if (!Seq->CountersRB->IsReady() || !Seq->AdmitRB->IsReady() ||
					    !Seq->EvictRB->IsReady() || !Seq->ResurrectRB->IsReady() ||
					    !Seq->ColdRB->IsReady() || !Seq->OrphanRB->IsReady())
					{
						continue;
					}
					auto Copy = [](FRHIGPUBufferReadback& RB, TArray<uint32>& Out, uint32 Bytes) {
						Out.SetNumUninitialized(Bytes / sizeof(uint32));
						const void* Src = RB.Lock(Bytes);
						if (Src != nullptr)
						{
							FMemory::Memcpy(Out.GetData(), Src, Bytes);
							RB.Unlock();
							return true;
						}
						return false;
					};
					bool bOk = Copy(*Seq->CountersRB, Seq->Counters, kNumCounters * sizeof(uint32));
					// Copy the lists only up to their counted lengths (clamped
					// to capacity) -- the rest of each buffer is garbage by
					// design and copying 2 MB per seq for a few KB of signal
					// would be pure waste.
					auto CopyList = [&](FRHIGPUBufferReadback& RB, TArray<uint32>& Out,
					                    uint32 Count, uint32 Cap) {
						const uint32 N = FMath::Min(Count, Cap);
						if (N == 0)
						{
							Out.Reset();
							return;
						}
						bOk = Copy(RB, Out, N * 16) && bOk;
					};
					if (bOk)
					{
						CopyList(*Seq->AdmitRB, Seq->Admit, Seq->Counters[kCtrAdmit], kAdmitCap);
						CopyList(*Seq->EvictRB, Seq->Evict, Seq->Counters[kCtrEvict], kEvictCap);
						CopyList(*Seq->ResurrectRB, Seq->Resurrect,
						         Seq->Counters[kCtrResurrect], kResurrectCap);
						CopyList(*Seq->ColdRB, Seq->Cold, Seq->Counters[kCtrCold], kColdCap);
						CopyList(*Seq->OrphanRB, Seq->Orphan, Seq->Counters[kCtrOrphan],
						         kOrphanCap);
					}
					// Readbacks die on the render thread, where they live.
					Seq->CountersRB.Reset();
					Seq->AdmitRB.Reset();
					Seq->EvictRB.Reset();
					Seq->ResurrectRB.Reset();
					Seq->ColdRB.Reset();
					Seq->OrphanRB.Reset();
					Seq->State.store(2, std::memory_order_release);
				}
			});
		}

		// Consume ready seqs on the game thread.
		for (int32 I = 0; I < InFlight.Num();)
		{
			if (InFlight[I]->State.load(std::memory_order_acquire) == 2)
			{
				FSeqPtr Seq = InFlight[I];
				InFlight.RemoveAt(I);
				ConsumeSeq(*Seq);
			}
			else
			{
				++I;
			}
		}
		MaybeLog();
	}

	// ---- the comparator ----------------------------------------------------
	// Mode-2 list parse: order-preserving, levels clamped to the packed field.
	// The subsystem re-validates level range against kNumLevels; a corrupt
	// entry can therefore be dropped there, never indexed with.
	static void ParseListArray(const TArray<uint32>& Raw, TArray<FVoxelResidencyProposal>& Out)
	{
		const int32 N = Raw.Num() / 4;
		Out.Reserve(N);
		for (int32 I = 0; I < N; ++I)
		{
			FVoxelResidencyProposal& P = Out.AddDefaulted_GetRef();
			P.Level = int32(Raw[I * 4 + 0] & 0xF);
			P.Coord = FIntVector(int32(Raw[I * 4 + 1]), int32(Raw[I * 4 + 2]),
			                     int32(Raw[I * 4 + 3]));
		}
	}

	static void ParseList(const TArray<uint32>& Raw, TSet<uint64>& Out)
	{
		const int32 N = Raw.Num() / 4;
		Out.Reserve(N);
		for (int32 I = 0; I < N; ++I)
		{
			const int32 Level = int32(Raw[I * 4 + 0] & 0xF);
			// NAMED LOCALS, NOT A BRACED TEMPORARY IN THE CONSTRUCTOR CALL.
			// FIntVector C(int32(Raw[..]), int32(Raw[..]), int32(Raw[..])) is
			// parsed by MSVC as a FUNCTION DECLARATION taking three `int32
			// Raw[]` parameters -- the most vexing parse -- and reports as
			// "error C2086: 'int32 Raw[]': redefinition", which points at the
			// argument rather than at the declaration that caused it.
			const int32 Cx = int32(Raw[I * 4 + 1]);
			const int32 Cy = int32(Raw[I * 4 + 2]);
			const int32 Cz = int32(Raw[I * 4 + 3]);
			Out.Add(PackKey(Level, FIntVector(Cx, Cy, Cz)));
		}
	}

	static bool NearRel(double A, double B)
	{
		const double Scale = FMath::Max3(FMath::Abs(A), FMath::Abs(B), 1.0);
		return FMath::Abs(A - B) <= kBoundaryRelEps * Scale;
	}

	// Double-precision re-evaluation of the admission XY+cutoff tests for one
	// key, reporting whether any decisive comparison sits within float
	// epsilon of its threshold. Only called for MISMATCHES, so O(mismatch).
	bool AdmitNearBoundary(const FSeqLedger& L, int32 Level, const FIntVector& C) const
	{
		const FVoxelResidencyLevelParams& LP = L.Params.Levels[Level];
		const double Edge = LP.ChunkEdgeUU;
		const double CX = (double(C.X) + 0.5) * Edge - L.Params.Anchor.X;
		const double CY = (double(C.Y) + 0.5) * Edge - L.Params.Anchor.Y;
		const double DistSq = CX * CX + CY * CY;
		const double InnerAdmitSq = LP.InnerAdmitUU * LP.InnerAdmitUU;
		const double OuterSq = LP.OuterUU * LP.OuterUU;
		const double AdmitOuterSq = LP.AdmitOuterUU * LP.AdmitOuterUU;
		if (Level > 0 && !L.Params.bHierarchicalCoverage && NearRel(DistSq, InnerAdmitSq))
		{
			return true;
		}
		if (NearRel(DistSq, OuterSq) || NearRel(DistSq, AdmitOuterSq))
		{
			return true;
		}
		if (DistSq >= OuterSq && L.Params.RingOverlapChunks == 0 &&
		    Level + 1 <= L.Params.MaxRingLevel)
		{
			// MaxRingLevel, not NumLevels-1: the CPU's seam test only runs the
			// parent-coverage exception below the outermost ACTIVE ring (its
			// own MaxRingLevel comment records why), so the boundary tolerance
			// must stop at the same edge or diagnostics near the cascade edge
			// are more forgiving than the rule they excuse.
			const double ParentEdge = Edge * 2.0;
			const double PX = (double(C.X >> 1) + 0.5) * ParentEdge - L.Params.Anchor.X;
			const double PY = (double(C.Y >> 1) + 0.5) * ParentEdge - L.Params.Anchor.Y;
			if (NearRel(PX * PX + PY * PY, OuterSq))
			{
				return true;
			}
		}
		const double CZ = (double(C.Z) + 0.5) * Edge - L.Params.Anchor.Z;
		double SortKeySq = DistSq + CZ * CZ;
		if (Level > 0 && L.Params.bHierarchicalCoverage)
		{
			SortKeySq = FMath::Max(SortKeySq, InnerAdmitSq);
		}
		if (LP.CutoffSortKeySq < double(MAX_flt) && NearRel(SortKeySq, LP.CutoffSortKeySq))
		{
			return true;
		}
		// Skirt band edges move ZMin, so a candidate near a band threshold can
		// be inside one arm's Z range and outside the other's.
		if (!L.Params.bUndergroundDisabled)
		{
			const double NearSq = OuterSq * L.Params.SkirtNearFrac * L.Params.SkirtNearFrac;
			const double MidSq = OuterSq * L.Params.SkirtMidFrac * L.Params.SkirtMidFrac;
			if (NearRel(DistSq, NearSq) || NearRel(DistSq, MidSq))
			{
				return true;
			}
		}
		return false;
	}

	bool EvictNearBoundary(const FSeqLedger& L, int32 Level, const FIntVector& C,
	                       bool bDeep) const
	{
		const FVoxelResidencyLevelParams& LP = L.Params.Levels[Level];
		const double Edge = LP.ChunkEdgeUU;
		const double CX = (double(C.X) + 0.5) * Edge - L.Params.Anchor.X;
		const double CY = (double(C.Y) + 0.5) * Edge - L.Params.Anchor.Y;
		const double DistSq = CX * CX + CY * CY;
		if (NearRel(DistSq, LP.UnloadOuterUU * LP.UnloadOuterUU))
		{
			return true;
		}
		if (Level > 0 && !L.Params.bHierarchicalCoverage &&
		    NearRel(DistSq, LP.InnerEvictUU * LP.InnerEvictUU))
		{
			return true;
		}
		if (bDeep)
		{
			const double CZ = (double(C.Z) + 0.5) * Edge - L.Params.Anchor.Z;
			if (NearRel(FMath::Abs(CZ), LP.VerticalKeepUU))
			{
				return true;
			}
		}
		return false;
	}

	void ConsumeSeq(const FSeqInFlight& Seq)
	{
		// Find and pop this seq's ledger.
		int32 LedgerIdx = INDEX_NONE;
		for (int32 I = 0; I < Ledgers.Num(); ++I)
		{
			if (Ledgers[I].Seq == Seq.Seq)
			{
				LedgerIdx = I;
				break;
			}
		}
		if (LedgerIdx == INDEX_NONE)
		{
			++Since.Uncompared;
			++Total.Uncompared;
			return;
		}
		FSeqLedger Ledger = MoveTemp(Ledgers[LedgerIdx]);
		Ledgers.RemoveAt(LedgerIdx);
		OpenLedger = nullptr; // indices shifted; the bracket re-establishes it

		if (Seq.Counters.Num() < int32(kNumCounters))
		{
			++Since.Uncompared;
			++Total.Uncompared;
			return;
		}

		// Traffic first: these move even when comparison is impossible, and
		// their staying at zero across a moving leg is itself a reading.
		const uint32 CAdmit = FMath::Min(Seq.Counters[kCtrAdmit], kAdmitCap);
		Since.GpuAdmit += CAdmit;
		Total.GpuAdmit += CAdmit;
		const uint32 CEvict = FMath::Min(Seq.Counters[kCtrEvict], kEvictCap);
		Since.GpuEvict += CEvict;
		Total.GpuEvict += CEvict;
		const uint32 CRes = FMath::Min(Seq.Counters[kCtrResurrect], kResurrectCap);
		Since.GpuResurrect += CRes;
		Total.GpuResurrect += CRes;
		const uint32 CCold = FMath::Min(Seq.Counters[kCtrCold], kColdCap);
		Since.GpuCold += CCold;
		Total.GpuCold += CCold;
		const uint32 COrphan = FMath::Min(Seq.Counters[kCtrOrphan], kOrphanCap);
		Since.GpuOrphan += COrphan;
		Total.GpuOrphan += COrphan;
		const uint32 Overflow = Seq.Counters[kCtrOverflow];
		if (Overflow != 0)
		{
			++Since.Overflows;
			++Total.Overflows;
		}
		Since.FbApplied += Seq.Counters[kCtrFbApplied];
		Total.FbApplied += Seq.Counters[kCtrFbApplied];
		Since.FbDropped += Seq.Counters[kCtrFbDropped];
		Total.FbDropped += Seq.Counters[kCtrFbDropped];
		Since.FbCollisions += Seq.Counters[kCtrCollision];
		Total.FbCollisions += Seq.Counters[kCtrCollision];
		// The enumeration traffic the kernels have always counted. candV/zV
		// are what the CPU sweep used to walk on the game thread; budRej is
		// what the GPU budget kept off it.
		Since.GpuCandVisited += Seq.Counters[kCtrCandVisited];
		Total.GpuCandVisited += Seq.Counters[kCtrCandVisited];
		Since.GpuZVisited += Seq.Counters[kCtrZVisited];
		Total.GpuZVisited += Seq.Counters[kCtrZVisited];
		Since.GpuCutoffRej += Seq.Counters[kCtrCutoffRej];
		Total.GpuCutoffRej += Seq.Counters[kCtrCutoffRej];
		Since.GpuBudgetRej += Seq.Counters[kCtrBudgetRej];
		Total.GpuBudgetRej += Seq.Counters[kCtrBudgetRej];
		Since.GpuHistTotal += Seq.Counters[kCtrHistTotal];
		Total.GpuHistTotal += Seq.Counters[kCtrHistTotal];
		Since.GpuExitVisited += Seq.Counters[kCtrExitVisited];
		Total.GpuExitVisited += Seq.Counters[kCtrExitVisited];

		// Orphans move to the CPU residual ledger: their hash leaves the CPU
		// xor (their cell no longer names them) and their eventual removal
		// note is absorbed by Residual instead of the feedback lane.
		{
			const int32 N = Seq.Orphan.Num() / 4;
			for (int32 I = 0; I < N; ++I)
			{
				const int32 Level = int32(Seq.Orphan[I * 4 + 0] & 0xF);
				const FIntVector C(int32(Seq.Orphan[I * 4 + 1]), int32(Seq.Orphan[I * 4 + 2]),
				                   int32(Seq.Orphan[I * 4 + 3]));
				const uint64 Key = PackKey(Level, C);
				if (!Residual.Contains(Key))
				{
					Residual.Add(Key);
					LedgerXor(Level, C, -1);
				}
			}
		}

		// Audit.
		if (Seq.bAudit)
		{
			const int64 GpuCount = int64(Seq.Counters[kCtrAuditCount]);
			const bool bMatch = GpuCount == Ledger.AuditCount &&
			                    Seq.Counters[kCtrAuditXorLo] == Ledger.AuditXorLo &&
			                    Seq.Counters[kCtrAuditXorHi] == Ledger.AuditXorHi;
			Since.LastAuditGpuCount = GpuCount;
			Since.LastAuditCpuCount = Ledger.AuditCount;
			Total.LastAuditGpuCount = GpuCount;
			Total.LastAuditCpuCount = Ledger.AuditCount;
			if (bMatch)
			{
				++Since.AuditPass;
				++Total.AuditPass;
				AuditMismatchStreak = 0;
			}
			else
			{
				++Since.AuditFail;
				++Total.AuditFail;
				++AuditMismatchStreak;
				UE_LOG(LogVoxelResidGpu, Warning,
				       TEXT("[gpu-resid] audit MISMATCH seq=%u: gpu count=%lld xor=%08x%08x vs ")
				       TEXT("cpu count=%lld xor=%08x%08x (streak %d)"),
				       Seq.Seq, (long long)GpuCount, Seq.Counters[kCtrAuditXorHi],
				       Seq.Counters[kCtrAuditXorLo], (long long)Ledger.AuditCount,
				       Ledger.AuditXorHi, Ledger.AuditXorLo, AuditMismatchStreak);
				if (AuditMismatchStreak >= 3)
				{
					UE_LOG(LogVoxelResidGpu, Error,
					       TEXT("[gpu-resid] audit FAIL: the mirror has drifted from ChunkRecords ")
					       TEXT("and will resync on the next recompute. This is the failing ")
					       TEXT("reading -- do not trust this run's compare numbers."));
					bNeedsResync = true;
					AuditMismatchStreak = 0;
				}
			}
		}

		if (!Ledger.bDispatched)
		{
			++Since.Uncompared;
			++Total.Uncompared;
			return;
		}

		// ---- mode 2 (LIVE): stage the delta for the subsystem --------------
		// No comparison happens here -- in live mode there are no independent
		// CPU walk decisions to hold the GPU against, and comparing the GPU to
		// its own consumed proposals would be a gate that cannot fail. The
		// gates that CAN fail in live mode are the audit above (mirror drift),
		// the subsystem's adjudication lanes (veto/stale/fallback counters),
		// and the leg-level uncovered metric.
		if (Mode == 2)
		{
			bool bAnyScannedLevel = false;
			for (int32 I = 0; I < Ledger.Params.NumLevels && I < 8; ++I)
			{
				bAnyScannedLevel |= Ledger.Params.Levels[I].bScanThisDispatch;
			}
			if (CAdmit + CEvict + CRes + CCold == 0 && !bAnyScannedLevel)
			{
				// Nothing to adjudicate and nothing to stamp: retiring it here
				// (instead of staging) is what lets a converged, stationary
				// world go quiet instead of forcing recomputes forever.
				++Since.LiveEmptyRetired;
				++Total.LiveEmptyRetired;
				return;
			}
			TUniquePtr<FVoxelResidencyLiveDelta> Delta = MakeUnique<FVoxelResidencyLiveDelta>();
			Delta->Seq = Seq.Seq;
			Delta->OverflowBits = Overflow;
			Delta->Params = Ledger.Params;
			ParseListArray(Seq.Admit, Delta->Admits);
			ParseListArray(Seq.Evict, Delta->Evicts);
			ParseListArray(Seq.Resurrect, Delta->Resurrects);
			ParseListArray(Seq.Cold, Delta->Colds);
			if (PendingLive.IsValid() && PendingLive->Seq > Delta->Seq)
			{
				// A newer delta is already staged; this one arrived out of
				// order and is superseded on arrival.
				++Since.LiveSuperseded;
				++Total.LiveSuperseded;
				return;
			}
			if (PendingLive.IsValid())
			{
				++Since.LiveSuperseded;
				++Total.LiveSuperseded;
			}
			PendingLive = MoveTemp(Delta);
			return;
		}

		// Parse proposal sets.
		TSet<uint64> GpuAdmit, GpuEvict, GpuResurrect, GpuColdFootprints;
		ParseList(Seq.Admit, GpuAdmit);
		ParseList(Seq.Evict, GpuEvict);
		ParseList(Seq.Resurrect, GpuResurrect);
		ParseList(Seq.Cold, GpuColdFootprints);

		// Evictions: exact two-way compare (pure math both sides).
		{
			TSet<uint64> CpuEvictSet;
			CpuEvictSet.Reserve(Ledger.CpuEvicts.Num());
			for (int32 I = 0; I < Ledger.CpuEvicts.Num(); ++I)
			{
				CpuEvictSet.Add(Ledger.CpuEvicts[I]);
			}
			for (int32 I = 0; I < Ledger.CpuEvicts.Num(); ++I)
			{
				const uint64 Key = Ledger.CpuEvicts[I];
				if (GpuEvict.Contains(Key))
				{
					++Since.EvictMatch;
					++Total.EvictMatch;
					continue;
				}
				int32 Level;
				FIntVector C;
				UnpackKey(Key, Level, C);
				if (EvictNearBoundary(Ledger, Level, C, Ledger.CpuEvictDeep[I] != 0))
				{
					++Since.EvictBoundary;
					++Total.EvictBoundary;
				}
				else
				{
					++Since.EvictMissed;
					++Total.EvictMissed;
				}
			}
			for (const uint64 Key : GpuEvict)
			{
				if (CpuEvictSet.Contains(Key))
				{
					continue;
				}
				int32 Level;
				FIntVector C;
				UnpackKey(Key, Level, C);
				// Deep flag unknown for a GPU-extra key; try both arms.
				if (EvictNearBoundary(Ledger, Level, C, true) ||
				    EvictNearBoundary(Ledger, Level, C, false))
				{
					++Since.EvictBoundary;
					++Total.EvictBoundary;
				}
				else
				{
					++Since.EvictExtra;
					++Total.EvictExtra;
				}
			}
		}

		// Admissions: the GPU proposal set must COVER the CPU's admissions
		// (the CPU's own gates reject some proposals, so extra is expected;
		// missing is the failure, classified before it is called one).
		{
			const bool bAdmitOverflow = (Overflow & 1u) != 0;
			for (int32 I = 0; I < Ledger.CpuAdmits.Num(); ++I)
			{
				const uint64 Key = Ledger.CpuAdmits[I];
				if (GpuAdmit.Contains(Key))
				{
					++Since.AdmitCovered;
					++Total.AdmitCovered;
					continue;
				}
				int32 Level;
				FIntVector C;
				UnpackKey(Key, Level, C);
				if (Ledger.CpuAdmitDeep[I] != 0)
				{
					++Since.MissedDeep;
					++Total.MissedDeep;
					continue;
				}
				if (bAdmitOverflow)
				{
					++Since.MissedOverflow;
					++Total.MissedOverflow;
					continue;
				}
				const uint64 FootKey = PackFootprint(Level, C.X, C.Y);
				if (GpuColdFootprints.Contains(FootKey))
				{
					++Since.MissedCold;
					++Total.MissedCold;
					continue;
				}
				const FTexelState* Tex = TexelMirror.Find(FootKey);
				if (Tex == nullptr || Tex->UploadedSeq >= Seq.Seq)
				{
					// The scan could not have seen this footprint's Z range.
					++Since.MissedCold;
					++Total.MissedCold;
					continue;
				}
				// Outside the texel's (skirted) Z range: an edit hatch (or a
				// hatch-shaped divergence) widened the CPU's Z loop.
				{
					const FVoxelResidencyLevelParams& LP = Ledger.Params.Levels[Level];
					const double Edge = LP.ChunkEdgeUU;
					const double CX = (double(C.X) + 0.5) * Edge - Ledger.Params.Anchor.X;
					const double CY = (double(C.Y) + 0.5) * Edge - Ledger.Params.Anchor.Y;
					const double DistSq = CX * CX + CY * CY;
					int32 Skirt = 0;
					if (!Ledger.Params.bUndergroundDisabled)
					{
						const double OuterSq = LP.OuterUU * LP.OuterUU;
						if (DistSq <
						    OuterSq * Ledger.Params.SkirtNearFrac * Ledger.Params.SkirtNearFrac)
						{
							Skirt = Ledger.Params.SkirtChunksNear;
						}
						else if (DistSq <
						         OuterSq * Ledger.Params.SkirtMidFrac * Ledger.Params.SkirtMidFrac)
						{
							Skirt = Ledger.Params.SkirtChunksMid;
						}
						else
						{
							Skirt = Ledger.Params.SkirtChunksFar;
						}
					}
					if (C.Z > int32(Tex->ZMax) || C.Z < int32(Tex->ZMin) - Skirt)
					{
						++Since.MissedEditHatch;
						++Total.MissedEditHatch;
						continue;
					}
				}
				if (AdmitNearBoundary(Ledger, Level, C))
				{
					++Since.MissedBoundary;
					++Total.MissedBoundary;
					continue;
				}
				// THE failing reading.
				++Since.MissedSurface;
				++Total.MissedSurface;
				if (MissedSurfaceSamples.Num() < 4)
				{
					MissedSurfaceSamples.Add(Key);
				}
			}
			// Proposed-extra is everything the CPU did not admit this seq --
			// expected nonzero (the CPU's own gates reject proposals) and NOT
			// a failure; its trend belongs on the line so cutoff drift is
			// visible.
			uint64 CoveredThisSeq = 0;
			for (int32 I = 0; I < Ledger.CpuAdmits.Num(); ++I)
			{
				if (GpuAdmit.Contains(Ledger.CpuAdmits[I]))
				{
					++CoveredThisSeq;
				}
			}
			const uint64 ExtraThisSeq = uint64(GpuAdmit.Num()) - CoveredThisSeq;
			Since.ProposedExtra += ExtraThisSeq;
			Total.ProposedExtra += ExtraThisSeq;
		}

		// Resurrections: same cover test, soft gate.
		{
			for (const uint64 Key : Ledger.CpuResurrects)
			{
				if (GpuResurrect.Contains(Key) || GpuAdmit.Contains(Key))
				{
					++Since.ResurrectCovered;
					++Total.ResurrectCovered;
				}
				else
				{
					++Since.ResurrectMissed;
					++Total.ResurrectMissed;
				}
			}
			TSet<uint64> CpuRes;
			for (const uint64 Key : Ledger.CpuResurrects)
			{
				CpuRes.Add(Key);
			}
			for (const uint64 Key : GpuResurrect)
			{
				if (!CpuRes.Contains(Key))
				{
					++Since.ResurrectExtra;
					++Total.ResurrectExtra;
				}
			}
		}
	}

	void MaybeLog()
	{
		const double Now = FPlatformTime::Seconds();
		if (LastLogSeconds == 0.0)
		{
			LastLogSeconds = Now;
			return;
		}
		if (LogIntervalSeconds <= 0.0)
		{
			// Same source the perf logger reads, latched once. Falls back to 5 s
			// only when the switch is absent, which is also what the perf lines
			// do, so the two families stay on the same window by construction
			// instead of by coincidence.
			float Interval = 0.f;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelPerfLogInterval="), Interval);
			LogIntervalSeconds = Interval > 0.f ? double(Interval) : 5.0;
		}
		if (Now - LastLogSeconds < LogIntervalSeconds)
		{
			return;
		}
		LastWindowSeconds = Now - LastLogSeconds;
		LastLogSeconds = Now;
		// Nothing moved and nothing pending: stay quiet (idle menus etc.).
		// SkippedUnderground and the two live CPU-fallback lanes join the quiet
		// test deliberately: a recompute that took one of them dispatches
		// nothing, so without them a fully underground flight -- or a drift
		// storm -- would print NO line at all and read as an idle menu.
		if (Since.Dispatches == 0 && Since.GpuAdmit == 0 && Since.FbStaged == 0 &&
		    Since.SkippedUnderground == 0 && Since.LiveUndergroundCalls == 0 &&
		    Since.LiveResyncCalls == 0 && InFlight.Num() == 0)
		{
			Since = FStats();
			Since.LastAuditGpuCount = Total.LastAuditGpuCount;
			Since.LastAuditCpuCount = Total.LastAuditCpuCount;
			return;
		}
		UE_LOG(LogVoxelResidGpu, Log,
		       TEXT("[gpu-resid] win=%.1fs disp=%llu skipFull=%llu skipUG=%llu uncmp=%llu inflight=%d | ")
		       TEXT("gpu: admit=%llu evict=%llu res=%llu cold=%llu orphan=%llu ovf=%llu | ")
		       TEXT("enum: candV=%llu zV=%llu cutRej=%llu budRej=%llu histTot=%llu ")
		       TEXT("exitV=%llu | ")
		       TEXT("cmp: evOK=%llu evMISS=%llu evEXTRA=%llu evBnd=%llu adOK=%llu ")
		       TEXT("adMISS-SURFACE=%llu adBnd=%llu adDeep=%llu adHatch=%llu adCold=%llu ")
		       TEXT("adOvf=%llu extra=%llu resOK=%llu resMiss=%llu | fb: staged=%llu ")
		       TEXT("applied=%llu dropped=%llu collide=%llu residual=%d | audit: pass=%llu ")
		       TEXT("fail=%llu cpu=%lld gpu=%lld | records~%lld"),
		       LastWindowSeconds, Since.Dispatches, Since.SkippedInFlight, Since.SkippedUnderground,
		       Since.Uncompared, InFlight.Num(), Since.GpuAdmit, Since.GpuEvict,
		       Since.GpuResurrect, Since.GpuCold, Since.GpuOrphan, Since.Overflows,
		       Since.GpuCandVisited, Since.GpuZVisited, Since.GpuCutoffRej, Since.GpuBudgetRej,
		       Since.GpuHistTotal, Since.GpuExitVisited,
		       Since.EvictMatch, Since.EvictMissed, Since.EvictExtra, Since.EvictBoundary,
		       Since.AdmitCovered, Since.MissedSurface, Since.MissedBoundary, Since.MissedDeep,
		       Since.MissedEditHatch, Since.MissedCold, Since.MissedOverflow,
		       Since.ProposedExtra, Since.ResurrectCovered, Since.ResurrectMissed,
		       Since.FbStaged, Since.FbApplied, Since.FbDropped, Since.FbCollisions,
		       Residual.Num(), Since.AuditPass, Since.AuditFail, (long long)Since.LastAuditCpuCount,
		       (long long)Since.LastAuditGpuCount, (long long)CpuCount);
		// The enum: block, and its failing readings. These are GPU-side traffic --
		// they move whether or not anything is consumed, which is why they go
		// first.
		//   candV/zV = 0 with disp > 0  -> the entry kernel is not running at
		//     all (dead permutation, empty ScanMask, span 0). No comparison and
		//     no timing on such a leg means anything.
		//   zV enormous against admit   -> the annulus is producing candidates
		//     the CPU cannot use. This is the GPU-side form of the streaming
		//     line's candidatesRejected, and it is the ratio the budget below
		//     exists to close.
		//   exitV = 0 while records~ > 0 -> the mirror is empty; the exit scan
		//     is walking nothing and evict proposals cannot appear at all.
		// The BUDGET (-VoxelResidencyAdmitBudget), and its pair rule -- never
		// read budRej alone:
		//   budRej = 0, histTot = 0     -> the switch is OFF, or never latched.
		//   budRej = 0, histTot > 0     -> the switch is on and the threshold
		//     NEVER BOUND: either every level fits inside the budget (legitimate
		//     once the world converges) or the resolve fell into its last-bin
		//     catch-all. "Fired but did nothing" -- a different fault from the
		//     line above, and the two are distinguishable only because histTot
		//     is printed next to it.
		//   admit + budRej != histTot   -> the histogram phase and the emit
		//     phase enumerated different candidate sets. This is an exact
		//     identity across the two passes, not a statistic, and it can fail
		//     in both directions.
		//   budRej climbing WITH adOK holding -> the win.
		//   budRej climbing WITH adOK falling -> OVER-THROTTLED: the budget is
		//     below what the level can absorb, and streaming will starve. This
		//     is the reading that makes the switch refutable instead of merely
		//     "the numbers got smaller".
		//
		// The mode-2 line. Failing readings, stated in advance (the counter
		// rule: every lane here can come out the other way):
		//   admit=0 (adOK+adopt+res all zero) WITH move >> a chunk edge and
		//     disp>0 -> the pass proposes nothing the CPU accepts: dead kernel,
		//        dead readback, or a gate rejecting everything. THE failure.
		//     THE SAME ZEROS WITH move~0 ARE HEALTH, not failure: a converged,
		//     parked world has nothing to propose, and a leg's final windows
		//     print exactly this shape after the flight ends. Read the flight
		//     windows, never the tail (t42-live.log, 2026-08-23: the tail's
		//     cons=427/prop=0 was misread as a dead pass while the flight
		//     windows above it carried prop in the millions and adOK=26-71k
		//     per window). move= exists so one line answers which case it is.
		//   cpuFallback>0 after warmup -> the live arm starved (no delta for
		//     kLiveStarvationCalls recomputes) and the run is silently CPU;
		//     the leg's timings then measure the CPU arm, not this feature.
		//   noDelta ~= one per consume-eligible recompute, sustained -> the
		//     readback lane is dead even if dispatches count up.
		//   evVETO a large, persistent fraction of evProp -> the exit kernel
		//     disagrees with the CPU recheck: kernel geometry wrong (would
		//     also have shown as evEXTRA in a mode-1 leg).
		//   adStale/evStale dominating their lanes -> the mirror is not being
		//     fed (audit would also be failing) or deltas are consistently
		//     ancient (superseded should be climbing with them).
		//   cold not decaying (coldEnum staying high, texels never warming)
		//     -> everything is coming back through the CPU-enumeration lane:
		//        the walk has been MOVED, not removed. Aokana's failure.
		//   firstScans climbing past kNumLevels -> live never holds a level.
		//   ug= or resync= climbing -> the recompute took a lane that runs the
		//     FULL CPU walks and never builds a live outcome, so every other
		//     number on this line stays 0 for that call. Before these two
		//     counters existed, an underground flight or a drift storm printed
		//     exactly the same all-zero live line as a dead GPU path.
		//   editEnum growing without edits happening -> the edited maps are
		//     leaking into the annulus test.
		// The -VoxelEditedLaneGate lanes (editFull/editDirty/editSkip/editDefer).
		// The lane they bound re-walked the WHOLE edit map every live call, so
		// these are read as a PAIR and neither alone is a verdict:
		//   editSkip=0 with edits present and move= large -> the gate NEVER
		//     SKIPS. Either a footprint is parked on a ring edge (slack is
		//     genuinely 0 -- legitimate, and the floor is exactly the old
		//     behaviour) or NoteEnumerated is not being called and the slack
		//     was never computed at all. Only distinguishable because the
		//     gate's own slack is printed beside it by the caller.
		//   editFull=0 across a window containing a teleport or a long flight
		//     -> the gate NEVER SWEEPS: the slack arithmetic is wrong in the
		//     UNSAFE direction and edited chunks are being missed. This is the
		//     dangerous half, and it is why editFull is a counter rather than
		//     an implied remainder.
		//   editFull+editDirty+editSkip != consume calls with edits present ->
		//     a call took none of the three paths. That is the silent-lane
		//     failure this file has now found in three places (underground,
		//     resync, and this one).
		//   editDefer climbing while editFull does not -> the per-call sweep
		//     budget is under the edit-map size and the tail is starving. Not
		//     a hole (the sweep resumes by construction, the gate refuses to
		//     publish a slack for a truncated sweep), but a dig then takes
		//     several recomputes to appear.
		if (Mode == 2)
		{
			const FVoxelResidencyLiveOutcome& L = Since.Live;
			const double MoveUU =
				FVector2D(LastDispatchAnchor.X - LogWindowAnchor.X,
			              LastDispatchAnchor.Y - LogWindowAnchor.Y).Size();
			UE_LOG(LogVoxelResidGpu, Log,
			       TEXT("[gpu-resid] live: win=%.1fs move=%.0fuu delta cons=%llu sup=%llu empty=%llu noDelta=%u | ")
			       TEXT("ad: prop=%u adOK=%u adopt=%u res=%u stale=%u rejFine=%u rejBud=%u ")
			       TEXT("rejCut=%u | ev: prop=%u q=%u VETO=%u stale=%u resid=%u | cold: prop=%u ")
			       TEXT("enum=%u defer=%u | edit: enum=%u full=%u dirty=%u skip=%u defer=%u | ")
			       TEXT("fallback: cpuCalls=%u firstScans=%u ")
			       TEXT("ug=%llu resync=%llu | ")
			       TEXT("ms ev=%.2f ad=%.2f"),
			       LastWindowSeconds, MoveUU, Since.LiveConsumed, Since.LiveSuperseded, Since.LiveEmptyRetired,
			       L.NoDeltaCalls, L.AdmitProposals, L.AdmitAdmitted, L.AdmitAdopted,
			       L.AdmitResurrected, L.AdmitStale, L.AdmitRejFine, L.AdmitRejBudget,
			       L.AdmitRejCutoff, L.EvictProposals, L.EvictQueued, L.EvictVetoed,
			       L.EvictStale, L.ResidualWalked, L.ColdProposals, L.ColdEnumerated,
			       L.ColdDeferred, L.EditedEnumerated, L.EditedFullSweeps, L.EditedDirtyPasses,
			       L.EditedSkipped, L.EditedDeferred, L.CpuFallbackCalls, L.CpuFirstScans,
			       Since.LiveUndergroundCalls, Since.LiveResyncCalls,
			       L.EvictMs, L.AdmitMs);
		}
		LogWindowAnchor = LastDispatchAnchor;
		if (MissedSurfaceSamples.Num() > 0)
		{
			FString Samples;
			for (const uint64 Key : MissedSurfaceSamples)
			{
				int32 Level;
				FIntVector C;
				UnpackKey(Key, Level, C);
				Samples += FString::Printf(TEXT(" L%d(%d,%d,%d)"), Level, C.X, C.Y, C.Z);
			}
			UE_LOG(LogVoxelResidGpu, Warning,
			       TEXT("[gpu-resid] missed-surface samples:%s -- these are chunks the CPU ")
			       TEXT("admitted that the pass failed to propose, with a warm texel, above ")
			       TEXT("ground, away from every boundary. The pass is WRONG for them."),
			       *Samples);
			MissedSurfaceSamples.Reset();
		}
		Since = FStats();
		Since.LastAuditGpuCount = Total.LastAuditGpuCount;
		Since.LastAuditCpuCount = Total.LastAuditCpuCount;
	}

	void Reset()
	{
		StagedFeedback.Reset();
		StagedTexels.Reset();
		TexelMirror.Reset();
		Residual.Reset();
		CpuCount = 0;
		CpuXorLo = 0;
		CpuXorHi = 0;
		InFlight.Reset();
		Ledgers.Reset();
		OpenLedger = nullptr;
		PendingLive.Reset();
		bInBracket = false;
		bNeedsResync = true;
		bClearGridOnNextDispatch = true;
		AuditMismatchStreak = 0;
		FImpl* Self = this;
		ENQUEUE_RENDER_COMMAND(VoxelResidencyRelease)([Self](FRHICommandListImmediate&) {
			VOXEL_RENDER_FRAME_SCOPE_TAIL(TailResidency);
			Self->ShadowPooled.SafeRelease();
			Self->ZRangePooled.SafeRelease();
			Self->AllocatedLevels = 0;
		});
	}
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

FVoxelResidencyGpu& FVoxelResidencyGpu::Get()
{
	// Leaked on purpose, the pattern every global in this module follows:
	// render commands capture the impl raw and must never outlive it.
	static FVoxelResidencyGpu* Singleton = new FVoxelResidencyGpu();
	return *Singleton;
}

FVoxelResidencyGpu::FImpl& FVoxelResidencyGpu::GetImpl()
{
	if (Impl == nullptr)
	{
		Impl = new FImpl();
	}
	return *Impl;
}

int32 FVoxelResidencyGpu::GetMode()
{
	return GetImpl().LatchMode();
}

bool FVoxelResidencyGpu::NeedsResync()
{
	return GetImpl().LatchMode() != 0 && GetImpl().bNeedsResync;
}

void FVoxelResidencyGpu::BeginResyncReplay()
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().BeginResyncReplay();
	}
}

void FVoxelResidencyGpu::EndResyncReplay()
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().EndResyncReplay();
	}
}

void FVoxelResidencyGpu::OnRecomputeBegin(const FVoxelResidencyDispatchParams& Params)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().OnRecomputeBegin(Params);
	}
}

void FVoxelResidencyGpu::OnRecomputeEnd()
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().OnRecomputeEnd();
	}
}

void FVoxelResidencyGpu::NoteRecordAdded(int32 Level, const FIntVector& Coord, bool bDeep)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().NoteRecordAdded(Level, Coord, bDeep);
	}
}

void FVoxelResidencyGpu::NoteRecordRemoved(int32 Level, const FIntVector& Coord)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().NoteRecordRemoved(Level, Coord);
	}
}

void FVoxelResidencyGpu::NoteUnloadQueued(int32 Level, const FIntVector& Coord, bool bDeep)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().NoteUnloadQueued(Level, Coord, bDeep);
	}
}

void FVoxelResidencyGpu::NoteUnloadCancelled(int32 Level, const FIntVector& Coord)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().NoteUnloadCancelled(Level, Coord);
	}
}

void FVoxelResidencyGpu::NoteFootprintZRange(int32 Level, int32 X, int32 Y, int32 ChunkZMin,
                                             int32 ChunkZMax)
{
	if (GetImpl().LatchMode() != 0)
	{
		GetImpl().NoteFootprintZRange(Level, X, Y, ChunkZMin, ChunkZMax);
	}
}

bool FVoxelResidencyGpu::HasActionableDelta()
{
	return GetImpl().LatchMode() == 2 && GetImpl().PendingLive.IsValid();
}

bool FVoxelResidencyGpu::TakeLiveDelta(FVoxelResidencyLiveDelta& Out)
{
	if (GetImpl().LatchMode() != 2)
	{
		return false;
	}
	return GetImpl().TakeLiveDelta(Out);
}

void FVoxelResidencyGpu::SnapshotResidual(TArray<FVoxelResidencyProposal>& Out)
{
	if (GetImpl().LatchMode() == 2)
	{
		GetImpl().SnapshotResidual(Out);
	}
	else
	{
		Out.Reset();
	}
}

void FVoxelResidencyGpu::NoteLiveOutcome(const FVoxelResidencyLiveOutcome& Outcome)
{
	if (GetImpl().LatchMode() == 2)
	{
		GetImpl().NoteLiveOutcome(Outcome);
	}
}

void FVoxelResidencyGpu::Reset()
{
	if (Impl != nullptr && Impl->Mode > 0)
	{
		Impl->Reset();
	}
}

// VoxelApplyBatch.cpp -- see VoxelApplyBatch.h for the measurement this exists
// to remove, the switch, and the failing readings for every counter below.

#include "VoxelApplyBatch.h"

#include "VoxelDebug.h" // LogVoxelPerf

#include "Components/SceneComponent.h"
#include "Engine/World.h"

namespace VoxelApplyFast
{
namespace
{

// --- sizing ------------------------------------------------------------------

int32 CacheSlots()
{
	static const int32 Latched = []
	{
		// 8192 slots x 40 B = 320 KB, and the working set it must hold is one
		// cascade's worth of FOOTPRINTS, not chunks: the measured settled
		// cascade is 43,328 chunks over roughly 1,400 footprints per level, so
		// a few thousand slots cover the live set with room for the ring the
		// camera is walking into. Direct-mapped, so oversizing costs only
		// memory -- and cacheEvict is the counter that says whether it is
		// undersized.
		// 8192 -> 131072 (2026-08-23). At 8192 the table THRASHED: cacheEvict
		// was 96% of cacheMiss, which is this counter's documented "undersized"
		// reading rather than its "wrong key" one (evict ~= 0 would be the
		// latter). Matched legs, one switch, mode 3, shipped caps:
		//
		//   slots     avoided    hit rate   evict/miss   sampler misses   settle
		//     8192      89.1%      81.5%          96%          204,243    23.4 s
		//   131072      91.9%      86.1%          43%          155,401    22.3 s
		//
		// 24% fewer calls into the fine-tier sampler lock, and avoided/calls
		// crosses the 90% the apply-fast brief set as its traffic gate. Cost is
		// 131,072 x 40 B = 5.2 MB of a direct-mapped table, allocated once.
		// evict/miss at 43% says the knee is at or above this; it is not below.
		int32 Value = 131072;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCache="), Value);
		Value = FMath::Clamp(Value, 64, 1 << 20);
		// Rounded UP to a power of two so the index is a mask, not a modulo:
		// this runs once per drained chunk, and a 64-bit division here would be
		// a measurable fraction of exactly what the change is trying to reclaim.
		return int32(FMath::RoundUpToPowerOfTwo(uint32(Value)));
	}();
	return Latched;
}

int32 AuditEveryN()
{
	static const int32 Latched = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelApplyColumnCacheAudit="), Value);
		return FMath::Max(0, Value);
	}();
	return Latched;
}

// --- the table ---------------------------------------------------------------

// PER (X, Y, LEVEL), NOT PER CHUNK, and that is the whole mechanism.
// SampleChunkParamsForPool's four corner heights, its climate bytes and its
// fitted gradients are functions of the chunk's XY footprint and its level
// alone. The chunk's Z enters exactly once, at the very end, as
//     SurfaceZRelUU = BaseZUU - ChunkWorldOrigin.Z
// so a whole vertical band shares one entry and differs only by a subtraction.
struct FSlot
{
	int32 Level = -1; // -1 = empty
	int32 X = 0;
	int32 Y = 0;
	float Temperature = 0.f;   // FVector4f.X exactly as the sampler produced it
	float Precipitation = 0.f; // FVector4f.Y
	float GradPacked = 0.f;    // FVector4f.W
	// THE FITTED PLANE'S ABSOLUTE WORLD Z, reconstructed once at fill time as
	// double(Params.Z) + ChunkWorldOrigin.Z.
	//
	// THE ROUND TRIP IS NOT EXACT AND ITS ERROR IS BOUNDED HERE RATHER THAN
	// LEFT TO BE DISCOVERED. Params.Z is float32 and already chunk-relative, so
	// reconstructing the absolute base costs one float32 rounding of a value
	// whose magnitude is at most a few thousand UU: 0.0005 UU at 8192 UU,
	// against a 10 UU voxel -- 5e-5 of a voxel. Every chunk that then HITS this
	// entry inherits that single rounding and nothing compounds, because the
	// stored double is written once. The ABSOLUTE height (~8.4e6 UU) is never
	// rounded to float, which is the precision trap SampleChunkParamsForPool
	// documents and deliberately avoids; this preserves that property.
	//
	// -VoxelApplyColumnCacheAudit reports maxMismatchUU, so the tail is a
	// measured number in the log rather than this comment's claim.
	double BaseZUU = 0.0;
};

TArray<FSlot>& Table()
{
	static TArray<FSlot> Slots;
	if (Slots.Num() == 0)
	{
		Slots.SetNum(CacheSlots());
	}
	return Slots;
}

// FNV-ish fold of the three key fields. Collisions are not a correctness
// question here: the slot stores X/Y/Level and they are compared EXACTLY, so a
// collision is a miss and an eviction, never a wrong answer.
FORCEINLINE uint32 SlotIndex(int32 Level, int32 X, int32 Y, uint32 Mask)
{
	uint64 H = 1469598103934665603ull;
	H = (H ^ uint64(uint32(X))) * 1099511628211ull;
	H = (H ^ uint64(uint32(Y))) * 1099511628211ull;
	H = (H ^ uint64(uint32(Level))) * 1099511628211ull;
	return uint32(H ^ (H >> 32)) & Mask;
}

// --- invalidation ------------------------------------------------------------
//
// The cached corner heights were sampled at
//     Root.GetComponentLocation() + ChunkOriginWorldForLevel(Key)
// so they are valid only for the Root transform they were taken under. The
// terrain root is static in every configuration this project ships -- but
// "static in practice" is precisely the kind of assumption this codebase has
// paid for repeatedly, so it is CHECKED once per call with an exact FVector
// compare and the whole table is dropped if it ever moves. rootFlush is the
// counter; a nonzero one voids the cache arm of a leg rather than corrupting
// it. The world pointer is compared for the same reason, across a PIE restart
// inside one process.
FVector CachedRootLoc = FVector::ZeroVector;
const UWorld* CachedWorld = nullptr;
bool bCacheAnchored = false;

// --- counters (one 5 s window) ----------------------------------------------
//
// Every one of these has its failing reading written down in the header. They
// are TRAFFIC, not timing: this project has shipped eleven features that were
// inert while every indicator read healthy, and a timing that improves is not
// evidence that a particular line of code ran.
int64 Calls = 0;            // ShadingForPublish calls that reached the slow path
int64 Sampled = 0;          // ...of which actually ran SampleChunkParamsForPool
int64 GuardSkipped = 0;     // ...of which returned without sampling (nothing consumes it)
int64 GuardSkipWithPack = 0; // ...of THOSE that DID hold a valid pack (see below)
int64 CacheHits = 0;
int64 CacheMisses = 0;
int64 CacheEvicts = 0;      // a miss that overwrote a DIFFERENT live entry
int64 SentinelUncached = 0; // "no surface gate" results, never cached (see below)
int64 RootFlushes = 0;
int64 OffThread = 0;        // HARD ZERO: this path is game-thread only
int64 AuditsRun = 0;
int64 AuditMismatches = 0;
double MaxMismatchUU = 0.0;
double SampleSeconds = 0.0; // wall time inside SampleChunkParamsForPool
int64 AuditTick = 0;
// Warm-ahead traffic (voxel.Stream.WarmShadingAhead; see the WARM-AHEAD HOOKS
// block in the header). SEPARATE from Calls/CacheMisses ON PURPOSE: the armed
// leg's verdict is the SUBMIT population's cacheMiss falling, and warm fills
// counted there would relocate every first-touch miss into the same counter
// and erase exactly the reading the A/B is decided on. WarmHits should read
// ~0: the warm loop probes IsCached before calling, and game-thread code has
// no one to race -- a nonzero here is a probe/entry disagreement worth a look,
// not a crash.
int64 WarmFills = 0;
int64 WarmHits = 0;

double LastLogSeconds = 0.0;

// 0.01 UU = 1/1000 of a voxel, and two orders of magnitude above the 0.0005 UU
// float32 reconstruction tail documented on FSlot::BaseZUU. A mismatch that
// clears this is a wrong GROUND height, not a rounding artifact -- the fine
// tile became resident between the column's first chunk and this one, and the
// entry is holding a pre-residency answer. maxMismatchUU is reported whether or
// not the tolerance is crossed, which is what separates the two cases without
// needing a rebuild.
constexpr double kAuditToleranceUU = 0.01;

// UVoxelGpuPoolComponent::kNoSurfaceGate is -1.0e30f: "no subsystem, no gate,
// always surface". It is a SENTINEL, not a height, and running it through the
// absolute-base reconstruction would turn it into a merely-very-negative number
// that no longer compares equal to the sentinel a consumer tests for. So a
// sentinel result is returned untouched and never enters the table.
constexpr float kSurfaceSentinelCeiling = -1.0e29f;

} // namespace

// Forward declaration: both public entry points below are thin wrappers over
// this, and it is DEFINED further down beside the cache it drives. Declared
// here rather than moved, so the cache body stays next to the slot layout and
// the audit it belongs to.
static FVoxelBrickChunkShading ShadingImpl(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                           const FVoxelBrickCpuPackRef& Pack,
                                           const USceneComponent& Root,
                                           FSampleParamsFn SampleParams,
                                           FShadingFromFn ShadingFrom,
                                           bool bAllowGuard,
                                           bool bWarmAhead = false);

FVoxelBrickChunkShading ShadingForPublishSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                              const FVoxelBrickCpuPackRef& Pack,
                                              const USceneComponent& Root,
                                              FSampleParamsFn SampleParams,
                                              FShadingFromFn ShadingFrom)
{
	return ShadingImpl(Key, Pack, Root, SampleParams, ShadingFrom, /*bAllowGuard*/ true);
}

// THE DIRECT CACHE-ENTRY API the dispatch site's own comment asks for.
//
// SubmitGpuMeshJob CONSUMES the shading it builds, so the publish guard must be
// unreachable here -- previously that was arranged by disengaging VoxelApplyFast
// entirely whenever the guard bit was set, which meant the shipping mode (3) ran
// the raw four-column expression at this site while caching it at the other one.
//
// WHY IT MATTERS MORE HERE THAN AT THE DRAIN SITE, measured 2026-08-23 on
// q-a1024.log (per-tick apply cap raised to 1024, so apply stopped being the
// bound and this site became it):
//
//   window   dispatch ms   submit ms   reqHdr ms   calls   perCallUs
//        2         412.4       339.6       299.5   15998        21.2
//        4         850.3       811.1       753.7    8756        92.6
//        6         854.3       825.0       658.3    4496       183.5
//
// reqHdr is footprint/seed/shading/skirt, and the shading is four
// GetSurfaceHeightUU calls that each take the FINE-TIER SAMPLER LOCK
// EXCLUSIVELY and run a full vxc::Amplifier::column on the game thread. As
// throughput rises, CPU worker jobs contend for that same lock and the per-call
// cost goes 21 us -> 183 us. Dispatch then consumed the entire game-thread tick
// (2,456 ms of a 2,000 ms window) and the tick rate collapsed from ~50 Hz to
// ~3 Hz. The apply-fast brief predicted this number ("under sampler-lock
// contention it read 328 us/call") and said to read reqHdr off a leg before
// applying this hook. This is that reading.
//
// FAILING READING: cacheHit ~= 0 at this site with cacheMiss ~= calls means the
// dispatch population has no column reuse -- one entry per chunk, not per
// column -- and this hook is pure loss. Read it on the 'Voxel apply fast' line
// with the drain site's traffic, since both feed the same table.
FVoxelBrickChunkShading ShadingForDispatchSlow(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                               const USceneComponent& Root,
                                               FSampleParamsFn SampleParams,
                                               FShadingFromFn ShadingFrom)
{
	return ShadingImpl(Key, FVoxelBrickCpuPackRef(), Root, SampleParams, ShadingFrom,
	                   /*bAllowGuard*/ false);
}

// --- the warm-ahead hooks (see the header's WARM-AHEAD HOOKS block) ----------

bool IsCached(int32 Level, int32 X, int32 Y)
{
	// GAME THREAD ONLY, like every read of Table(). Off-thread the honest
	// answer is "do not warm this footprint", which reads as cached -- the same
	// fail-toward-doing-nothing direction the sampler's offThread check takes.
	if (!IsInGameThread())
	{
		return true;
	}
	if ((Mode() & kModeCache) == 0 || !bCacheAnchored)
	{
		// No table to hit (cache arm off) or nothing anchored yet. "Not cached"
		// is literally true, but a caller must gate on CacheArmed() before
		// spending a sample on it -- with the cache off the sample cannot be
		// stored and warming is pure loss.
		return false;
	}
	const uint32 Mask = uint32(CacheSlots() - 1);
	const FSlot& Slot = Table()[SlotIndex(Level, X, Y, Mask)];
	return Slot.Level == Level && Slot.X == X && Slot.Y == Y;
}

// --- the cold-burst census probe (header: THE COLD-BURST CENSUS PROBE) -------
//
// A MIRROR OF ShadingImpl's DECISION CHAIN, DELIBERATELY IN THIS FILE. Every
// early exit below is a copy of a decision made a hundred lines down in
// ShadingImpl, and it is only defensible because it sits beside that function
// over the same statics and calls the same SlotIndex -- there is no second
// transcription of the slot key or the anchor test, which is the defect shape
// this module was written to avoid. If ShadingImpl's chain changes, THIS
// CHANGES WITH IT; the failing reading that would catch a drift is a census in
// which cold shadings stop tracking the 'Voxel apply fast' line's cacheMiss.
//
// Order matches ShadingImpl exactly:
//   Mode() == 0        -> the wrapper folded to the raw expression: always samples
//   off game thread    -> ShadingImpl's offThread arm samples unconditionally
//   cache bit off      -> no table exists: every call samples
//   anchor mismatch    -> the table is flushed and then sampled
//   slot miss          -> samples
//   slot hit           -> WARM (the audit re-sample is the stated inexactness)
//
// The publish guard is not in this list because it cannot fire at the dispatch
// site: ShadingForDispatchSlow passes bAllowGuard = false.
bool WouldSampleForDispatch(const VoxelCoords::FVoxelLevelChunkKey& Key, const USceneComponent& Root)
{
	const int32 M = Mode();
	if (M == 0)
	{
		return true;
	}
	if (!IsInGameThread())
	{
		return true;
	}
	if ((M & kModeCache) == 0)
	{
		return true;
	}
	if (!bCacheAnchored || Root.GetComponentLocation() != CachedRootLoc || Root.GetWorld() != CachedWorld)
	{
		// ShadingImpl clears every slot here and falls through to the sampler,
		// so this call is cold whatever the slot below happens to hold. Rare
		// (counted as rootFlushes on the window line) but not zero, and a
		// census that missed it would under-report a whole tick's burst.
		return true;
	}
	const uint32 Mask = uint32(CacheSlots() - 1);
	const FSlot& Slot = Table()[SlotIndex(Key.Level, Key.Key.X, Key.Key.Y, Mask)];
	return !(Slot.Level == Key.Level && Slot.X == Key.Key.X && Slot.Y == Key.Key.Y);
}

void WarmForDispatch(const VoxelCoords::FVoxelLevelChunkKey& Key,
                     const USceneComponent& Root,
                     FSampleParamsFn SampleParams,
                     FShadingFromFn ShadingFrom)
{
	if (!IsInGameThread())
	{
		// The hard zero, same rule as ShadingImpl's: the table is unsynchronised
		// and the fine-tier prefetch is game-thread-gated. Unlike ShadingImpl
		// there is no consumer to serve, so the right response is to do NOTHING,
		// loudly countable, rather than sample anyway.
		++OffThread;
		return;
	}
	if ((Mode() & kModeCache) == 0)
	{
		// Nothing can be persisted; the warm loop's window line reports
		// INERT-CACHE-OFF for this state so a leg cannot silently burn its
		// budget warming a cache that does not exist.
		return;
	}
	ShadingImpl(Key, FVoxelBrickCpuPackRef(), Root, SampleParams, ShadingFrom,
	            /*bAllowGuard*/ false, /*bWarmAhead*/ true);
}

void FlushStats(bool bForce)
{
	const double Now = FPlatformTime::Seconds();
	if (LastLogSeconds <= 0.0)
	{
		LastLogSeconds = Now; // first call: start the window, print nothing
		return;
	}
	if (!bForce && Now - LastLogSeconds < 5.0)
	{
		return;
	}
	if (Mode() == 0)
	{
		return; // control arm: this module prints nothing at all
	}

	const int32 M = Mode();
	const double WindowSec = FMath::Max(1e-6, Now - LastLogSeconds);
	const double SampleUsPerSample = Sampled > 0 ? (SampleSeconds * 1e6) / double(Sampled) : 0.0;
	const double SampleUsPerCall = Calls > 0 ? (SampleSeconds * 1e6) / double(Calls) : 0.0;
	const int64 Avoided = GuardSkipped + CacheHits;

	// READ `avoided` AND `apply=` TOGETHER, NEVER EITHER ALONE.
	//
	//   avoided ~= calls, apply= down     -> the change did what it claims.
	//   avoided ~= calls, apply= FLAT     -> the sampler was not apply's cost.
	//                                        The diagnosis is wrong: revert, do
	//                                        not tune. Take the -VoxelApplyFast=4
	//                                        leg first so this is known before
	//                                        any behaviour changes.
	//   avoided = 0                       -> nothing ran. FAIL, not a null
	//                                        result, whatever apply= says.
	//   avoided ~= calls, mismatch > 0    -> it ran and it is wrong.
	//
	// sampleUs/call is the DIRECT price of the four amplifier columns per
	// drained chunk. At mode=4 (measure only, behaviour unchanged) it is the
	// entire claim of this change, measured, before anything is altered.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel apply fast (window): mode=%d(%s%s%s) calls=%lld avoided=%lld (%.1f%%) ")
	       TEXT("| guardSkip=%lld (withPack=%lld) cacheHit=%lld cacheMiss=%lld cacheEvict=%lld ")
	       TEXT("sentinel=%lld | sampled=%lld sampleUs/sample=%.2f sampleUs/call=%.2f ")
	       TEXT("sampleMsWindow=%.1f | audit=%lld mismatch=%lld maxMismatchUU=%.4f ")
	       TEXT("| warmFill=%lld warmHit=%lld | rootFlush=%lld offThread=%lld slots=%d win=%.1fs"),
	       M,
	       (M & kModeGuard) ? TEXT("guard") : TEXT("-"),
	       (M & kModeCache) ? TEXT("+cache") : TEXT(""),
	       (M & kModeMeasure) ? TEXT("+measure") : TEXT(""),
	       (long long)Calls, (long long)Avoided,
	       Calls > 0 ? 100.0 * double(Avoided) / double(Calls) : 0.0,
	       (long long)GuardSkipped, (long long)GuardSkipWithPack,
	       (long long)CacheHits, (long long)CacheMisses, (long long)CacheEvicts,
	       (long long)SentinelUncached,
	       (long long)Sampled, SampleUsPerSample, SampleUsPerCall, SampleSeconds * 1000.0,
	       (long long)AuditsRun, (long long)AuditMismatches, MaxMismatchUU,
	       (long long)WarmFills, (long long)WarmHits,
	       (long long)RootFlushes, (long long)OffThread,
	       CacheSlots(), WindowSec);

	Calls = Sampled = GuardSkipped = GuardSkipWithPack = 0;
	CacheHits = CacheMisses = CacheEvicts = SentinelUncached = 0;
	WarmFills = WarmHits = 0;
	RootFlushes = OffThread = 0;
	AuditsRun = AuditMismatches = 0;
	MaxMismatchUU = 0.0;
	SampleSeconds = 0.0;
	LastLogSeconds = Now;
}

// The shared body. bAllowGuard is the ONLY difference between the two public
// entry points: at the drain site the pack decides whether anything consumes
// the result, and at the dispatch site the result is always consumed, so the
// guard must not be reachable there at all. Passing that as a parameter keeps
// ONE implementation of the cache -- a second transcription of the slot key or
// the Z rebase is the defect shape this module was written to avoid.
static FVoxelBrickChunkShading ShadingImpl(const VoxelCoords::FVoxelLevelChunkKey& Key,
                                           const FVoxelBrickCpuPackRef& Pack,
                                           const USceneComponent& Root,
                                           FSampleParamsFn SampleParams,
                                           FShadingFromFn ShadingFrom,
                                           bool bAllowGuard,
                                           bool bWarmAhead)
{
	const int32 M = Mode();
	const FVector OriginRelative = VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level);

	// GAME THREAD ONLY. The table is unsynchronised, and GetSurfaceHeightUU's
	// own fine-tier prefetch is game-thread-gated for the same reason (it takes
	// the sampler's lock exclusively and can do disk I/O). Both call sites are
	// game-thread, so offThread is a hard zero -- but a data race here would
	// present as wrong terrain rather than a crash, so it is checked instead of
	// asserted in a comment.
	if (!IsInGameThread())
	{
		++OffThread;
		return ShadingFrom(SampleParams(Root, OriginRelative, Key.Level));
	}

	// Warm-ahead traffic stays OUT of Calls (and out of the flush cadence,
	// which keys off it) -- see the WarmFills declaration for why.
	if (!bWarmAhead)
	{
		++Calls;
		// The window flush costs one clock read; amortised over 256 drained
		// chunks it is free, and at 12,700 drains per 5 s window the log still
		// lands within ~0.1 s of its nominal edge. Pass -- to FlushStats from
		// MaybeLogCounters (optional hook 3) if exact alignment with
		// `Voxel tick budget` is wanted.
		if ((Calls & 255) == 0)
		{
			FlushStats(/*bForce*/ false);
		}
	}

	auto Sample = [&]() -> FVector4f
	{
		++Sampled;
		if (M & kModeMeasure)
		{
			// Two FPlatformTime::Seconds() around a call claimed to cost ~10 us.
			// The pair costs tens of nanoseconds, so it is well under 1% of what
			// it measures -- and it is STILL gated, because "an instrument that
			// becomes what it measures" is a recorded failure on this exact path
			// (voxel.Stream.ApplyStageStats carries the same warning) and a
			// shipping default must not carry one.
			const double T0 = FPlatformTime::Seconds();
			const FVector4f P = SampleParams(Root, OriginRelative, Key.Level);
			SampleSeconds += FPlatformTime::Seconds() - T0;
			return P;
		}
		return SampleParams(Root, OriginRelative, Key.Level);
	};

	// --- ARM 1: THE PUBLISH GUARD --------------------------------------------
	//
	// Nothing consumes the result, so nothing is computed. This is the arm that
	// matters on the GPU-primary configuration, where Result.BrickPack is null
	// for EVERY drained result (the fork publishes its bricks at completion),
	// and the four amplifier columns were being computed and discarded 12,700
	// times per 5 s window.
	if (bAllowGuard && (M & kModeGuard) && !WillPublish(Pack))
	{
		++GuardSkipped;
		if (Pack.IsValid())
		{
			// The pack EXISTS and only the publication gate
			// (voxel.GPU.BrickPackResident) refused it. Returning neutral is
			// still correct -- Publish refuses on the same test -- but a nonzero
			// here means the CPU arm is packing chunks nobody stores, which is
			// worker time being spent for nothing and is worth knowing about
			// separately from "there was no producer".
			++GuardSkipWithPack;
		}
		// Neutral, not garbage: if this value were ever consumed despite the
		// guard, the failure is "mid-range climate, surface gate off" -- visible
		// and diagnosable -- rather than an uninitialised plane.
		return FVoxelBrickChunkShading::Neutral();
	}

	// --- ARM 2: THE PER-COLUMN CACHE -----------------------------------------
	//
	// Note the ordering: with the guard OFF (mode=2 or 4) everything is sampled
	// and published exactly as control does, so mode=2 exercises the CACHE
	// ALONE. That is the bisection if a mode=3 leg looks wrong in the world:
	// mode=2 right and mode=3 wrong isolates the guard, the reverse isolates the
	// cache, and neither needs a counter to be trusted.
	if ((M & kModeCache) == 0)
	{
		return ShadingFrom(Sample());
	}

	// Anchor check (see CachedRootLoc). Exact compare, no tolerance: a moved
	// root means every cached corner height was sampled at the wrong XY, and
	// "nearly the same place" is not a defensible basis for keeping them.
	const FVector RootLoc = Root.GetComponentLocation();
	const UWorld* World = Root.GetWorld();
	if (!bCacheAnchored || RootLoc != CachedRootLoc || World != CachedWorld)
	{
		if (bCacheAnchored)
		{
			++RootFlushes;
		}
		for (FSlot& S : Table())
		{
			S.Level = -1;
		}
		CachedRootLoc = RootLoc;
		CachedWorld = World;
		bCacheAnchored = true;
	}

	const double ChunkWorldZ = RootLoc.Z + OriginRelative.Z;
	const uint32 Mask = uint32(CacheSlots() - 1);
	FSlot& Slot = Table()[SlotIndex(Key.Level, Key.Key.X, Key.Key.Y, Mask)];

	if (Slot.Level == Key.Level && Slot.X == Key.Key.X && Slot.Y == Key.Key.Y)
	{
		if (bWarmAhead)
		{
			// The warm loop probes IsCached before calling, so this is ~0 by
			// construction -- counted rather than assumed, because silent
			// success is the house failure. No audit either: audits exist to
			// check what is PUBLISHED, and nothing consumes a warm result.
			++WarmHits;
			return FVoxelBrickChunkShading::Neutral();
		}
		++CacheHits;

		FVector4f Cached;
		Cached.X = Slot.Temperature;
		Cached.Y = Slot.Precipitation;
		Cached.Z = float(Slot.BaseZUU - ChunkWorldZ); // the ONLY per-chunk term
		Cached.W = Slot.GradPacked;

		const int32 AuditN = AuditEveryN();
		if (AuditN > 0 && (++AuditTick % int64(AuditN)) == 0)
		{
			++AuditsRun;
			const FVector4f Fresh = Sample();
			const double DZ = FMath::Abs(double(Fresh.Z) - double(Cached.Z));
			MaxMismatchUU = FMath::Max(MaxMismatchUU, DZ);
			const bool bOtherDiffers =
				(Fresh.X != Cached.X) || (Fresh.Y != Cached.Y) || (Fresh.W != Cached.W);
			if (bOtherDiffers || DZ > kAuditToleranceUU)
			{
				++AuditMismatches;
				UE_LOG(LogVoxelPerf, Error,
				       TEXT("Voxel apply fast AUDIT MISMATCH: L%d (%d,%d,%d) cached ")
				       TEXT("(T=%.5f P=%.5f Z=%.4f G=%.1f) fresh (T=%.5f P=%.5f Z=%.4f G=%.1f) ")
				       TEXT("dz=%.4f UU"),
				       Key.Level, Key.Key.X, Key.Key.Y, Key.Key.Z,
				       Cached.X, Cached.Y, Cached.Z, Cached.W,
				       Fresh.X, Fresh.Y, Fresh.Z, Fresh.W, DZ);
			}
			// THE FRESH VALUE WINS. Auditing must never be able to make a run
			// worse than control: an audited leg publishes exactly what the
			// uncached path would have published, and only the counters differ.
			return ShadingFrom(Fresh);
		}

		return ShadingFrom(Cached);
	}

	if (!bWarmAhead)
	{
		++CacheMisses;
		if (Slot.Level >= 0)
		{
			// A live entry for a DIFFERENT column is being overwritten. cacheEvict
			// ~= cacheMiss with a low hit rate is the table thrashing and wants
			// -VoxelApplyColumnCache raised; cacheEvict ~= 0 with a low hit rate
			// means the KEY is wrong, which is a completely different problem.
			++CacheEvicts;
		}
	}

	// A warm sample bypasses the Sample() lambda so its wall time lands in the
	// warm loop's own warmBudgetMs (the `Voxel warm-ahead` window line) and not
	// in SampleSeconds -- Sampled would not move with it and sampleUs/sample
	// would read inflated. A warm fill CAN overwrite a live entry for another
	// column; with 131,072 slots against a working set of a few thousand
	// footprints that collision is rare enough not to earn its own counter --
	// and if it ever mattered, the displaced column re-misses through the
	// counted path, so cacheEvict/cacheMiss still names it.
	const FVector4f Fresh = bWarmAhead ? SampleParams(Root, OriginRelative, Key.Level)
	                                   : Sample();

	if (Fresh.Z > kSurfaceSentinelCeiling)
	{
		Slot.Level = Key.Level;
		Slot.X = Key.Key.X;
		Slot.Y = Key.Key.Y;
		Slot.Temperature = Fresh.X;
		Slot.Precipitation = Fresh.Y;
		Slot.GradPacked = Fresh.W;
		Slot.BaseZUU = double(Fresh.Z) + ChunkWorldZ;
		if (bWarmAhead)
		{
			++WarmFills; // the slot write IS the product of a warm call
		}
	}
	else
	{
		// "No surface gate" sentinel -- see kSurfaceSentinelCeiling. Not stored,
		// and the slot is left as it was rather than half-filled.
		//
		// EXPECTED HARD ZERO in a normal run: it means SampleChunkParamsForPool
		// found no UWorld or no UVoxelWorldSubsystem, so every brick published
		// in that window carries a DISABLED surface gate -- cave floors tinted
		// as turf, with no error anywhere. If this counter moves, the problem is
		// upstream of this file.
		++SentinelUncached;
	}

	return ShadingFrom(Fresh);
}

} // namespace VoxelApplyFast
